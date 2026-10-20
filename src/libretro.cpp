// The libretro face of Snaggletooth: the entry points a libretro frontend loads the core
// through. The library defines the console; the core is the application around it, owning
// what the frontend hands in and what it is handed back. Each entry point passes its work to
// the part of the core it concerns: the frontend's callbacks (frontend.h), the cartridge
// (cartridge.h), the picture (picture.h), the sound (sound.h), the controller ports
// (controllers.h), the cartridge's save (battery_save.h), the console's work RAM (work_ram.h)
// and the map placing both on the bus (memory_map.h), the player's cheat codes (cheats.h), the
// core options (options.h), the machine's saved states (save_state.h), the graphics card the
// picture is drawn on (gpu_device.h) and the running cartridge (session.h).
//
// A libretro core is one per process, reached through plain functions with no handle, so the
// running cartridge lives in this file's own state: the session a loaded cartridge runs in,
// from retro_load_game to retro_unload_game.

#include <libretro.h>

#include "cartridge.h"
#include "cheats.h"
#include "controllers.h"
#include "frontend.h"
#include "gpu_device.h"
#include "options.h"
#include "picture.h"
#include "save_state.h"
#include "session.h"
#include "sound.h"

#include "snaggletooth/snes/cartridge.h"
#include "snaggletooth/snes/snes.h"
#include "snaggletooth/version.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace snaggletooth_libretro;

namespace {

// The core's version followed by the version of the library it was built from, so a report
// from a frontend names both. Built once, on the first call that asks for it; the frontend
// keeps the pointer for the life of the process.
const char* libraryVersion() {
  static const std::string text = std::string(SNAGGLETOOTH_LIBRETRO_VERSION) + " (Snaggletooth " +
                                  std::string(snaggletooth::version()) + ")";
  return text.c_str();
}

std::unique_ptr<Session> session;

// The thread options as the player last set them, which the running machine follows unless its
// picture could not be drawn on the GPU.
MachineThreads askedThreads;

void cannotDrawOnGpu(const std::string& reason) {
  tellPlayer("Snaggletooth cannot draw on the GPU here (" + reason +
             "). The picture is drawn on the processor.");
}

// The device a machine on `threads` draws on: the process's GPU device where the picture is drawn
// on the GPU. Where no device opens, the player is told and the picture moves to the frontend's
// thread.
GpuDevice* deviceFor(MachineThreads& threads) {
  if (threads.picture != PictureDrawing::Gpu) return nullptr;
  std::string error;
  GpuDevice* device = sharedGpuDevice(error);
  if (device == nullptr) {
    cannotDrawOnGpu(error);
    threads.picture = PictureDrawing::CallingThread;
  }
  return device;
}

}  // namespace

RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }

RETRO_API void retro_get_system_info(struct retro_system_info* info) {
  *info = retro_system_info{};
  info->library_name = "Snaggletooth";
  info->library_version = libraryVersion();
  info->valid_extensions = "sfc|smc|swc|fig";
  info->need_fullpath = false;
  info->block_extract = false;
}

// The core runs a cartridge and nothing else, so a frontend never starts it without content.
// The core's options are listed here, before anything loads.
RETRO_API void retro_set_environment(retro_environment_t callback) {
  environment = callback;
  bool noGame = false;
  environment(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &noGame);
  registerOptions();
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t callback) { videoRefresh = callback; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t callback) { audioSample = callback; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t callback) {
  audioSampleBatch = callback;
}
RETRO_API void retro_set_input_poll(retro_input_poll_t callback) { inputPoll = callback; }
RETRO_API void retro_set_input_state(retro_input_state_t callback) { inputState = callback; }

// Both ports start with a pad in them, the frontend is asked whether it reports a pad's buttons
// as a bitmask, and it is told the save states have no serialization quirks.
RETRO_API void retro_init(void) {
  retro_log_callback log{};
  logPrintf = environment != nullptr && environment(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log)
                  ? log.log
                  : nullptr;
  startControllers();
  declareSerializationQuirks();
}

// The machine goes before the GPU device it draws on.
RETRO_API void retro_deinit(void) {
  session.reset();
  closeSharedGpuDevice();
  forgetCheats();
  logPrintf = nullptr;
}

// The geometry is the picture's shape as the frontend was last told it, at the aspect the player
// chose. The timing is the console's own: its master clock over the master cycles of a frame,
// which is not a whole number of frames a second.
RETRO_API void retro_get_system_av_info(struct retro_system_av_info* info) {
  const snaggletooth::Region region =
      session ? session->machine->region() : snaggletooth::Region::Ntsc;
  const snaggletooth::ConsoleClock clock = snaggletooth::consoleClock(region);
  *info = retro_system_av_info{};
  info->geometry.base_width = session ? session->shownWidth : kBaseWidth;
  info->geometry.base_height = session ? session->shownHeight : kBaseHeight;
  info->geometry.max_width = kMaxWidth;
  info->geometry.max_height = kMaxHeight;
  info->geometry.aspect_ratio =
      session ? aspectOf(session->shownLines, session->shownAspect)
              : aspectOf(kBaseHeight, aspectOption());
  info->timing.fps = static_cast<double>(clock.hertzNumerator) /
                     (static_cast<double>(clock.hertzDenominator) *
                      static_cast<double>(clock.masterCyclesPerFrame));
  info->timing.sample_rate = kSampleRate;
}

// The machine is handed the port's new contents from the next frame.
RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device) {
  plugIn(port, device);
}

// The console's reset button.
RETRO_API void retro_reset(void) {
  if (session) session->machine->reset();
}

// One console frame, from the start of one vertical blank to the start of the next, where the
// machine reports the finished picture; the first after a load or the reset button runs from
// power-on to the first vertical blank. A save or work RAM the frontend wrote since the last
// frame is in the machine as the frame begins, then the byte of every every-frame cheat that is
// on, and each port holds what the frontend reports for it, so the frame's reads of the save,
// the work RAM and the pads — the vertical blank's controller read among them — see them. The
// frontend is handed the picture that frame makes, then the sound it made, and its save and
// work-RAM buffers hold both as the frame left them. A change to the interlace, aspect or thread
// options takes effect from this frame; a machine rebuilt on other threads is given the ROM
// patches of the cheats that are on. A change to the GPU on a computer where the core cannot draw
// on it is told to the player once, and the picture is drawn on the frontend's thread.
RETRO_API void retro_run(void) {
  if (inputPoll != nullptr) inputPoll();
  if (!session) return;
  if (optionsChanged()) {
    session->picture.mode = interlaceOption();
    session->aspect = aspectOption();
    MachineThreads threads = threadsOption();
    if (threads != askedThreads) {
      askedThreads = threads;
      GpuDevice* device = deviceFor(threads);
      const bool rebuilt = threads != session->threads || device != session->device;
      try {
        session->runOn(threads, device);
      } catch (const std::runtime_error& error) {
        cannotDrawOnGpu(error.what());
      }
      if (rebuilt) patchRom(*session->machine);
    }
  }
  session->save.passToMachine(*session->machine);
  session->workRam.passToMachine(*session->machine);
  applyCheats(*session->machine);
  session->machine->setJoypad(snaggletooth::JoypadPort::One, heldOn(0u));
  session->machine->setJoypad(snaggletooth::JoypadPort::Two, heldOn(1u));
  session->runFrame();
  session->showPicture();
  session->playSound();
}

// The bytes a save state of the loaded cartridge takes, the same from load to unload; none with
// no cartridge.
RETRO_API size_t retro_serialize_size(void) {
  return session ? SaveState::size(*session->machine) : 0u;
}

// The machine's state, written into the frontend's buffer. A buffer shorter than
// retro_serialize_size() is refused.
RETRO_API bool retro_serialize(void* data, size_t size) {
  if (!session || data == nullptr) return false;
  return SaveState::save(*session->machine,
                         std::span<std::uint8_t>(static_cast<std::uint8_t*>(data), size));
}

// A state retro_serialize wrote for this cartridge, put back into the machine: the game carries
// on from the cycle it was saved at, and the save and work-RAM buffers hold the state's. Bytes
// that are not such a state are refused, and the game carries on as it was.
RETRO_API bool retro_unserialize(const void* data, size_t size) {
  if (!session || data == nullptr) return false;
  return session->loadState(
      std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(data), size));
}

// Turns every cheat off: the cartridge's ROM gets back each byte a Game Genie code replaced.
RETRO_API void retro_cheat_reset(void) { resetCheats(session ? session->machine.get() : nullptr); }

// One cheat code, as the player entered it: a Game Genie code, `XXYY-ZZZZ`, patching the
// cartridge's ROM at once and put back when it is turned off; or a Pro Action Replay code,
// `AAAAAAVV`, or a raw one, `AAAAAA:VV`, writing its byte before every frame from the next. A
// code set before a cartridge loads takes effect when it loads. A code in no such form, or
// naming an address with no memory behind it, or a Game Genie code outside the cartridge's ROM,
// is refused with a message.
RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char* code) {
  setCheat(index, enabled, code, session ? session->machine.get() : nullptr);
}

// Loads a cartridge from the bytes the frontend read. A copier's header ahead of the image is
// dropped, the machine is built at the region the Region option names, or under Auto the one
// the cartridge's header names, and a cartridge whose board carries a coprocessor is refused
// with a message naming the chip. The machine runs its sound and draws its picture where the
// thread options choose; where the picture cannot be drawn on the GPU, the player is told and it
// is drawn on the frontend's thread. The frontend is told the devices each port takes and the
// name of every button on them, and where the work RAM and the save sit on the console's bus.
// The cheats set so far take effect on the new machine.
RETRO_API bool retro_load_game(const struct retro_game_info* game) {
  session.reset();
  if (game == nullptr || game->data == nullptr || game->size == 0u) {
    tellPlayer("Snaggletooth cannot run this content: it is empty.");
    return false;
  }
  const std::vector<std::uint8_t> rom = cartridgeImage(
      std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(game->data), game->size));

  const snaggletooth::Coprocessor chip = snaggletooth::cartridgeBoard(rom).coprocessor;
  if (chip != snaggletooth::Coprocessor::None) {
    tellPlayer(std::string("Snaggletooth cannot run this cartridge: it needs ") + chipName(chip) +
               ".");
    return false;
  }

  retro_pixel_format format = RETRO_PIXEL_FORMAT_XRGB8888;
  if (environment == nullptr || !environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format)) {
    tellPlayer("Snaggletooth needs a frontend that shows 32-bit XRGB8888 pictures.");
    return false;
  }

  describeControllers();

  const snaggletooth::Region region = regionOption().value_or(regionOf(rom));
  MachineThreads threads = threadsOption();
  askedThreads = threads;
  GpuDevice* device = deviceFor(threads);
  try {
    session = std::make_unique<Session>(rom, region, threads, device);
  } catch (const std::runtime_error& error) {
    if (device == nullptr) throw;
    cannotDrawOnGpu(error.what());
    threads.picture = PictureDrawing::CallingThread;
    session = std::make_unique<Session>(rom, region, threads, nullptr);
  }
  session->picture.mode = interlaceOption();
  session->aspect = aspectOption();
  session->shownAspect = session->aspect;
  session->memoryMap.tell();
  patchRom(*session->machine);
  return true;
}

RETRO_API bool retro_load_game_special(unsigned type, const struct retro_game_info* info,
                                       size_t count) {
  (void)type;
  (void)info;
  (void)count;
  return false;
}

// The cartridge goes, and the cheats set on it with it.
RETRO_API void retro_unload_game(void) {
  session.reset();
  forgetCheats();
}

RETRO_API unsigned retro_get_region(void) {
  return session && session->machine->region() == snaggletooth::Region::Pal ? RETRO_REGION_PAL
                                                                           : RETRO_REGION_NTSC;
}

// The cartridge's save, as RETRO_MEMORY_SAVE_RAM: the buffer the frontend loads its save file
// into after the cartridge loads and writes the file from. The console's work RAM, as
// RETRO_MEMORY_SYSTEM_RAM: the 128 KB the frontend reads for achievements and its cheat search.
// A cartridge without a save has none; with no cartridge there is no memory; and the core
// offers no other.
RETRO_API void* retro_get_memory_data(unsigned id) {
  if (!session) return nullptr;
  switch (id) {
    case RETRO_MEMORY_SAVE_RAM: return session->save.data();
    case RETRO_MEMORY_SYSTEM_RAM: return session->workRam.data();
    default: return nullptr;
  }
}

RETRO_API size_t retro_get_memory_size(unsigned id) {
  if (!session) return 0;
  switch (id) {
    case RETRO_MEMORY_SAVE_RAM: return session->save.size();
    case RETRO_MEMORY_SYSTEM_RAM: return session->workRam.size();
    default: return 0;
  }
}
