// The libretro face of Snaggletooth: the entry points a libretro frontend loads the core
// through. The library defines the console; this file is the application around it, owning
// what the frontend hands in and what it is handed back.
//
// A libretro core is one per process, reached through plain functions with no handle, so the
// running cartridge lives in this file's own state: the frontend's callbacks, set before
// anything loads, and the session a loaded cartridge runs in, from retro_load_game to
// retro_unload_game.

#include <libretro.h>

#include "snaggletooth/snes/cartridge.h"
#include "snaggletooth/snes/snes.h"
#include "snaggletooth/version.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

// The core's version followed by the version of the library it was built from, so a report
// from a frontend names both. Built once, on the first call that asks for it; the frontend
// keeps the pointer for the life of the process.
const char* libraryVersion() {
  static const std::string text = std::string(SNAGGLETOOTH_LIBRETRO_VERSION) + " (Snaggletooth " +
                                  std::string(snaggletooth::version()) + ")";
  return text.c_str();
}

// The frontend's callbacks, as it hands them over. Each is absent until the frontend sets it.
retro_environment_t environment = nullptr;
retro_video_refresh_t videoRefresh = nullptr;
retro_audio_sample_t audioSample = nullptr;
retro_audio_sample_batch_t audioSampleBatch = nullptr;
retro_input_poll_t inputPoll = nullptr;
retro_input_state_t inputState = nullptr;
retro_log_printf_t logPrintf = nullptr;

// The picture's largest shape: a frame drawn in half-pixels is 512 wide, and an interlaced
// picture woven from two 239-line fields is 478 tall.
constexpr unsigned kBaseWidth = 256u;
constexpr unsigned kBaseHeight = 224u;
constexpr unsigned kMaxWidth = 512u;
constexpr unsigned kMaxHeight = 478u;

// The rate the audio machine hands its stereo frames over at.
constexpr double kSampleRate = 32000.0;

// How long a message the frontend shows on screen stays there.
constexpr unsigned kMessageMilliseconds = 6000u;
constexpr unsigned kMessageFrames = 360u;

// Tells the player something they need to know, the way the frontend shows messages, and
// writes it to the frontend's log as well. A frontend that offers only the first message
// interface is handed the message that way.
void tellPlayer(const std::string& text) {
  if (logPrintf != nullptr) logPrintf(RETRO_LOG_ERROR, "%s\n", text.c_str());
  if (environment == nullptr) return;
  unsigned version = 0;
  if (!environment(RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION, &version)) version = 0;
  if (version >= 1u) {
    retro_message_ext message{.msg = text.c_str(),
                              .duration = kMessageMilliseconds,
                              .priority = 3u,
                              .level = RETRO_LOG_ERROR,
                              .target = RETRO_MESSAGE_TARGET_ALL,
                              .type = RETRO_MESSAGE_TYPE_NOTIFICATION,
                              .progress = -1};
    environment(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &message);
  } else {
    retro_message message{.msg = text.c_str(), .frames = kMessageFrames};
    environment(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
  }
}

// The chip a cartridge's header names, as a player would know it.
const char* chipName(snaggletooth::Coprocessor chip) {
  using snaggletooth::Coprocessor;
  switch (chip) {
    case Coprocessor::None: return "no coprocessor";
    case Coprocessor::Dsp: return "a DSP coprocessor (DSP-1 to DSP-4)";
    case Coprocessor::Gsu: return "the SuperFX coprocessor";
    case Coprocessor::Obc1: return "the OBC1 coprocessor";
    case Coprocessor::Sa1: return "the SA-1 coprocessor";
    case Coprocessor::Sdd1: return "the S-DD1 coprocessor";
    case Coprocessor::Srtc: return "the S-RTC real-time clock";
    case Coprocessor::Other: return "a Super Game Boy or Satellaview chip";
    case Coprocessor::Spc7110: return "the SPC7110 coprocessor";
    case Coprocessor::St010: return "the ST010 or ST011 coprocessor";
    case Coprocessor::St018: return "the ST018 coprocessor";
    case Coprocessor::Cx4: return "the CX4 coprocessor";
    case Coprocessor::Unknown: return "a chip its header does not identify";
  }
  return "a chip its header does not identify";
}

// Thrown by the frame observer as the machine finishes a frame. The machine returns from
// run() through it with the beam at the next frame's first line, which is where a frontend's
// frame ends; the next run() spends the rest of the interrupted budget before its own.
struct FrameFinished {};

// The machine's frame observer: it notes the finished frame's shape and stops the run there.
class FrameEnd final : public snaggletooth::FrameObserver {
 public:
  void frame(const snaggletooth::VideoFrame& picture) override {
    width = picture.width;
    height = picture.height;
    throw FrameFinished{};
  }

  unsigned width = kBaseWidth;
  unsigned height = kBaseHeight;
};

// A loaded cartridge and everything that runs it. The machine keeps a pointer to the
// observer, so a session is built in place and never moved.
struct Session {
  explicit Session(const snaggletooth::SnesConfig& config) : machine(config) {
    machine.setFrameObserver(&frameEnd);
  }
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  FrameEnd frameEnd;
  snaggletooth::Snes machine;
  // Where the machine's sound is drained to each frame.
  std::array<snaggletooth::StereoFrame, 1024> sound{};
};

std::unique_ptr<Session> session;

// The region a cartridge was made for, read from its header's country byte. A cartridge whose
// header names no region runs at 60 Hz.
snaggletooth::Region regionOf(std::span<const std::uint8_t> rom) {
  const std::optional<snaggletooth::CartridgeHeader> header = snaggletooth::parseCartridgeHeader(rom);
  if (header && header->video == snaggletooth::VideoStandard::Pal) return snaggletooth::Region::Pal;
  return snaggletooth::Region::Ntsc;
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
RETRO_API void retro_set_environment(retro_environment_t callback) {
  environment = callback;
  bool noGame = false;
  environment(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &noGame);
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t callback) { videoRefresh = callback; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t callback) { audioSample = callback; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t callback) {
  audioSampleBatch = callback;
}
RETRO_API void retro_set_input_poll(retro_input_poll_t callback) { inputPoll = callback; }
RETRO_API void retro_set_input_state(retro_input_state_t callback) { inputState = callback; }

RETRO_API void retro_init(void) {
  retro_log_callback log{};
  logPrintf = environment != nullptr && environment(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log)
                  ? log.log
                  : nullptr;
}

RETRO_API void retro_deinit(void) {
  session.reset();
  logPrintf = nullptr;
}

// The timing is the console's own: its master clock over the master cycles of a frame, which
// is not a whole number of frames a second.
RETRO_API void retro_get_system_av_info(struct retro_system_av_info* info) {
  const snaggletooth::Region region =
      session ? session->machine.region() : snaggletooth::Region::Ntsc;
  const snaggletooth::ConsoleClock clock = snaggletooth::consoleClock(region);
  *info = retro_system_av_info{};
  info->geometry.base_width = kBaseWidth;
  info->geometry.base_height = kBaseHeight;
  info->geometry.max_width = kMaxWidth;
  info->geometry.max_height = kMaxHeight;
  info->geometry.aspect_ratio = 0.0f;
  info->timing.fps = static_cast<double>(clock.hertzNumerator) /
                     (static_cast<double>(clock.hertzDenominator) *
                      static_cast<double>(clock.masterCyclesPerFrame));
  info->timing.sample_rate = kSampleRate;
}

// Every port starts empty, with no controller in it.
RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device) {
  (void)port;
  (void)device;
}

// The console's reset button.
RETRO_API void retro_reset(void) {
  if (session) session->machine.reset();
}

// One console frame: the machine runs until the beam reaches the next frame's first line, so
// a frame drawn a line longer, as an interlaced field is, is run whole. The frame is reported
// to the frontend as a repeat of the last one shown, at the shape the machine drew it.
RETRO_API void retro_run(void) {
  if (inputPoll != nullptr) inputPoll();
  if (!session) return;
  const std::uint64_t frameCycles =
      snaggletooth::consoleClock(session->machine.region()).masterCyclesPerFrame;
  try {
    for (;;) session->machine.run(frameCycles);
  } catch (const FrameFinished&) {
  }
  while (session->machine.takeFrames(session->sound) == session->sound.size()) {
  }
  if (videoRefresh != nullptr) {
    videoRefresh(nullptr, session->frameEnd.width, session->frameEnd.height, 0);
  }
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
// whose board carries a coprocessor is refused with a message naming the chip.
RETRO_API bool retro_load_game(const struct retro_game_info* game) {
  session.reset();
  if (game == nullptr || game->data == nullptr || game->size == 0u) {
    tellPlayer("Snaggletooth cannot run this content: it is empty.");
    return false;
  }
  const auto* bytes = static_cast<const std::uint8_t*>(game->data);
  std::vector<std::uint8_t> rom(bytes, bytes + game->size);
  if (snaggletooth::readCopierHeader(rom)) {
    rom.erase(rom.begin(), rom.begin() + static_cast<std::ptrdiff_t>(snaggletooth::kCopierHeaderBytes));
  }

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

  session = std::make_unique<Session>(
      snaggletooth::SnesConfig{.rom = rom, .region = regionOf(rom)});
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

RETRO_API void* retro_get_memory_data(unsigned id) {
  (void)id;
  return nullptr;
}

RETRO_API size_t retro_get_memory_size(unsigned id) {
  (void)id;
  return 0;
}
