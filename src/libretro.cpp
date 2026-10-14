// The libretro face of Snaggletooth: the entry points a libretro frontend loads the core
// through. The library defines the console; the core is the application around it, owning
// what the frontend hands in and what it is handed back. Each entry point passes its work to
// the part of the core it concerns: the frontend's callbacks (frontend.h), the cartridge
// (cartridge.h), the picture (picture.h), the sound (sound.h), the controller ports
// (controllers.h), the cartridge's save (battery_save.h), the core options (options.h) and the
// running cartridge (session.h).
//
// A libretro core is one per process, reached through plain functions with no handle, so the
// running cartridge lives in this file's own state: the session a loaded cartridge runs in,
// from retro_load_game to retro_unload_game.

#include <libretro.h>

#include "cartridge.h"
#include "controllers.h"
#include "frontend.h"
#include "options.h"
#include "picture.h"
#include "session.h"
#include "sound.h"

#include "snaggletooth/snes/cartridge.h"
#include "snaggletooth/snes/snes.h"
#include "snaggletooth/version.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
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

// Both ports start with a pad in them, and the frontend is asked whether it reports a pad's
// buttons as a bitmask.
RETRO_API void retro_init(void) {
  retro_log_callback log{};
  logPrintf = environment != nullptr && environment(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log)
                  ? log.log
                  : nullptr;
  startControllers();
}

RETRO_API void retro_deinit(void) {
  session.reset();
  logPrintf = nullptr;
}

// The geometry is the picture's shape as the frontend was last told it. The timing is the
// console's own: its master clock over the master cycles of a frame, which is not a whole
// number of frames a second.
RETRO_API void retro_get_system_av_info(struct retro_system_av_info* info) {
  const snaggletooth::Region region =
      session ? session->machine.region() : snaggletooth::Region::Ntsc;
  const snaggletooth::ConsoleClock clock = snaggletooth::consoleClock(region);
  *info = retro_system_av_info{};
  info->geometry.base_width = session ? session->shownWidth : kBaseWidth;
  info->geometry.base_height = session ? session->shownHeight : kBaseHeight;
  info->geometry.max_width = kMaxWidth;
  info->geometry.max_height = kMaxHeight;
  info->geometry.aspect_ratio = aspectOf(session ? session->shownLines : kBaseHeight);
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
  if (session) session->machine.reset();
}

// One console frame. A save the frontend wrote since the last frame is in the machine as the
// frame begins, and each port holds what the frontend reports for it, so the frame's reads of
// the save and the pads see them. The frontend is handed the picture that frame makes, then the
// sound it made, and its save buffer holds the save as the frame left it. A change to the
// interlace option takes effect from this frame.
RETRO_API void retro_run(void) {
  if (inputPoll != nullptr) inputPoll();
  if (!session) return;
  if (optionsChanged()) session->screen.mode = interlaceOption();
  session->save.passToMachine(session->machine);
  session->machine.setJoypad(snaggletooth::JoypadPort::One, heldOn(0u));
  session->machine.setJoypad(snaggletooth::JoypadPort::Two, heldOn(1u));
  session->runFrame();
  session->showPicture();
  session->playSound();
}

RETRO_API size_t retro_serialize_size(void) { return 0; }

RETRO_API bool retro_serialize(void* data, size_t size) {
  (void)data;
  (void)size;
  return false;
}

RETRO_API bool retro_unserialize(const void* data, size_t size) {
  (void)data;
  (void)size;
  return false;
}

RETRO_API void retro_cheat_reset(void) {}

RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char* code) {
  (void)index;
  (void)enabled;
  (void)code;
}

// Loads a cartridge from the bytes the frontend read. A copier's header ahead of the image is
// dropped, the machine is built at the region the cartridge's header names, and a cartridge
// whose board carries a coprocessor is refused with a message naming the chip. The machine
// draws its picture on a thread of its own beside the one the frontend calls the core on: the
// pixels and the state are the same either way, and each thread carries less of the work. The
// frontend is told the devices each port takes and the name of every button on them.
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

  session = std::make_unique<Session>(
      snaggletooth::SnesConfig{.rom = rom, .region = regionOf(rom), .ppuThread = true});
  session->screen.mode = interlaceOption();
  return true;
}

RETRO_API bool retro_load_game_special(unsigned type, const struct retro_game_info* info,
                                       size_t count) {
  (void)type;
  (void)info;
  (void)count;
  return false;
}

RETRO_API void retro_unload_game(void) { session.reset(); }

RETRO_API unsigned retro_get_region(void) {
  return session && session->machine.region() == snaggletooth::Region::Pal ? RETRO_REGION_PAL
                                                                           : RETRO_REGION_NTSC;
}

// The cartridge's save, as RETRO_MEMORY_SAVE_RAM: the buffer the frontend loads its save file
// into after the cartridge loads and writes the file from. A cartridge without a save, or no
// cartridge, has none, and the core offers no other memory.
RETRO_API void* retro_get_memory_data(unsigned id) {
  if (id != RETRO_MEMORY_SAVE_RAM || !session) return nullptr;
  return session->save.data();
}

RETRO_API size_t retro_get_memory_size(unsigned id) {
  if (id != RETRO_MEMORY_SAVE_RAM || !session) return 0;
  return session->save.size();
}
