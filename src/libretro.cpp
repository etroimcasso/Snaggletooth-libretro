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

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
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

// The picture's shapes: 256 by 224 to begin with, and at its largest 512 across, where a frame
// is drawn in half-pixels, and 478 down, where two 239-line fields are woven into one picture.
constexpr unsigned kBaseWidth = 256u;
constexpr unsigned kBaseHeight = 224u;
constexpr unsigned kMaxWidth = 512u;
constexpr unsigned kMaxHeight = 478u;

// The picture's aspect is that of square pixels 256 across, over the lines the console draws
// (224, or 239 for the taller picture). A frame in half-pixels and a woven one keep that same
// aspect, so the picture holds one shape on screen whatever size the frame is.
float aspectOf(unsigned lines) {
  return static_cast<float>(kBaseWidth) / static_cast<float>(lines);
}

// How a game's interlaced fields are shown. The machine draws one field a frame, alternating
// parity: Weave interleaves the last two into the full-height picture the console sent, Bob
// shows each field at full height on its own, and Off shows each field as it is drawn.
enum class Interlace : std::uint8_t { Weave, Bob, Off };

// The core's options, as the frontend lists them under Core Options.
constexpr const char* kInterlaceKey = "snaggletooth_interlace";

retro_core_option_v2_category optionCategories[] = {
    {.key = "video", .desc = "Video", .info = "How the console's picture is shown."},
    {},
};

retro_core_option_v2_definition optionDefinitions[] = {
    {.key = kInterlaceKey,
     .desc = "Interlaced Picture",
     .desc_categorized = "Interlaced Picture",
     .info = "How a game that draws an interlaced picture is shown. Weave interleaves its two "
             "fields into the full-height picture the console sent; Bob shows each field at full "
             "height on its own; Off shows each field as it is drawn, at half height.",
     .info_categorized = nullptr,
     .category_key = "video",
     .values = {{"weave", "Weave"}, {"bob", "Bob"}, {"off", "Off"}, {nullptr, nullptr}},
     .default_value = "weave"},
    {},
};

retro_core_options_v2 coreOptions{.categories = optionCategories,
                                  .definitions = optionDefinitions};

// The rate of the console's sound: the audio machine runs 1,024,000 cycles to a second of the
// console's time, in both regions, and makes one stereo sample every 32 of them.
constexpr double kSampleRate = 32000.0;

// The frontend takes sound as 16-bit signed pairs, left then right, which is the layout of the
// machine's StereoFrame, so the machine's samples are handed over as they are.
static_assert(std::is_standard_layout_v<snaggletooth::StereoFrame>);
static_assert(sizeof(snaggletooth::StereoFrame) == 2u * sizeof(std::int16_t));
static_assert(offsetof(snaggletooth::StereoFrame, left) == 0u);
static_assert(offsetof(snaggletooth::StereoFrame, right) == sizeof(std::int16_t));

// Hands stereo samples to the frontend, calling again with whatever a call leaves untaken. A
// frontend that takes none of a call's samples is not asked for the rest.
void handOver(std::span<const snaggletooth::StereoFrame> frames) {
  if (audioSampleBatch == nullptr) return;
  const auto* samples = reinterpret_cast<const std::int16_t*>(frames.data());
  std::size_t untaken = frames.size();
  while (untaken > 0u) {
    const std::size_t taken = std::min(audioSampleBatch(samples, untaken), untaken);
    if (taken == 0u) return;
    samples += taken * 2u;
    untaken -= taken;
  }
}

// The console's two controller ports and the device the frontend put in each. libretro takes a
// RetroPad to be plugged into every port until the frontend chooses otherwise, so each starts
// as one. A choice holds across resets and cartridges until the frontend makes another or
// starts the core again.
constexpr unsigned kPortCount = 2u;
std::array<unsigned, kPortCount> portDevices{RETRO_DEVICE_JOYPAD, RETRO_DEVICE_JOYPAD};

// Whether the frontend reports all of a pad's buttons in one call, as a bitmask.
bool inputBitmasks = false;

// The RetroPad button each of the console's twelve stands on. The RetroPad is laid out as the
// console's pad, so each button is the one of the same name in the same place.
struct PadButton {
  unsigned id;
  snaggletooth::Button button;
  const char* name;
};

constexpr std::array<PadButton, snaggletooth::kButtonCount> kPadButtons{{
    {.id = RETRO_DEVICE_ID_JOYPAD_B, .button = snaggletooth::Button::B, .name = "B"},
    {.id = RETRO_DEVICE_ID_JOYPAD_Y, .button = snaggletooth::Button::Y, .name = "Y"},
    {.id = RETRO_DEVICE_ID_JOYPAD_SELECT, .button = snaggletooth::Button::Select, .name = "Select"},
    {.id = RETRO_DEVICE_ID_JOYPAD_START, .button = snaggletooth::Button::Start, .name = "Start"},
    {.id = RETRO_DEVICE_ID_JOYPAD_UP, .button = snaggletooth::Button::Up, .name = "D-Pad Up"},
    {.id = RETRO_DEVICE_ID_JOYPAD_DOWN, .button = snaggletooth::Button::Down, .name = "D-Pad Down"},
    {.id = RETRO_DEVICE_ID_JOYPAD_LEFT, .button = snaggletooth::Button::Left, .name = "D-Pad Left"},
    {.id = RETRO_DEVICE_ID_JOYPAD_RIGHT, .button = snaggletooth::Button::Right,
     .name = "D-Pad Right"},
    {.id = RETRO_DEVICE_ID_JOYPAD_A, .button = snaggletooth::Button::A, .name = "A"},
    {.id = RETRO_DEVICE_ID_JOYPAD_X, .button = snaggletooth::Button::X, .name = "X"},
    {.id = RETRO_DEVICE_ID_JOYPAD_L, .button = snaggletooth::Button::L, .name = "L"},
    {.id = RETRO_DEVICE_ID_JOYPAD_R, .button = snaggletooth::Button::R, .name = "R"},
}};

// What each port offers in the frontend's controller menu: the console's pad, or nothing.
constexpr retro_controller_description kPortTypes[] = {
    {.desc = "SNES Joypad", .id = RETRO_DEVICE_JOYPAD},
    {.desc = "None", .id = RETRO_DEVICE_NONE},
};

constexpr retro_controller_info kControllerInfo[] = {
    {.types = kPortTypes, .num_types = std::size(kPortTypes)},
    {.types = kPortTypes, .num_types = std::size(kPortTypes)},
    {.types = nullptr, .num_types = 0u},
};

// The name of every button on both ports, as the frontend shows them when the player binds
// their controls.
const std::array<retro_input_descriptor, kPortCount * snaggletooth::kButtonCount + 1u>
    kInputDescriptors = [] {
      std::array<retro_input_descriptor, kPortCount * snaggletooth::kButtonCount + 1u> list{};
      std::size_t at = 0;
      for (unsigned port = 0; port < kPortCount; ++port) {
        for (const PadButton& pad : kPadButtons) {
          list[at++] = retro_input_descriptor{.port = port,
                                              .device = RETRO_DEVICE_JOYPAD,
                                              .index = 0u,
                                              .id = pad.id,
                                              .description = pad.name};
        }
      }
      return list;
    }();

// What the frontend says is plugged into a port and held on it: the twelve buttons as the
// frontend reports them, or nothing for a port whose device is not a pad. The buttons are
// passed through as reported, so the machine sees every combination the frontend sends.
std::optional<snaggletooth::Joypad> heldOn(unsigned port) {
  if ((portDevices[port] & RETRO_DEVICE_MASK) != RETRO_DEVICE_JOYPAD) return std::nullopt;
  snaggletooth::Joypad pad;
  if (inputState == nullptr) return pad;
  if (inputBitmasks) {
    const auto held = static_cast<std::uint16_t>(
        inputState(port, RETRO_DEVICE_JOYPAD, 0u, RETRO_DEVICE_ID_JOYPAD_MASK));
    for (const PadButton& button : kPadButtons) {
      pad.hold(button.button, ((held >> button.id) & 1u) != 0u);
    }
  } else {
    for (const PadButton& button : kPadButtons) {
      pad.hold(button.button, inputState(port, RETRO_DEVICE_JOYPAD, 0u, button.id) != 0);
    }
  }
  return pad;
}

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

// Lists the core's options with the frontend in the newest form it takes: definitions grouped
// into categories (version 2), plain definitions (version 1), or, for any frontend, one
// "Description; value|value" line for each option, its default value first.
void registerOptions() {
  unsigned version = 0;
  if (!environment(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version)) version = 0;
  if (version >= 2u) {
    environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &coreOptions);
    return;
  }
  if (version >= 1u) {
    static std::vector<retro_core_option_definition> plain = [] {
      std::vector<retro_core_option_definition> definitions;
      for (const auto* option = optionDefinitions; option->key != nullptr; ++option) {
        retro_core_option_definition definition{.key = option->key,
                                                .desc = option->desc,
                                                .info = option->info,
                                                .values = {},
                                                .default_value = option->default_value};
        std::copy(std::begin(option->values), std::end(option->values),
                  std::begin(definition.values));
        definitions.push_back(definition);
      }
      definitions.push_back(retro_core_option_definition{});
      return definitions;
    }();
    environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS, plain.data());
    return;
  }
  static const std::vector<std::string> lines = [] {
    std::vector<std::string> text;
    for (const auto* option = optionDefinitions; option->key != nullptr; ++option) {
      std::string line = std::string(option->desc) + "; " + option->default_value;
      for (const auto* value = option->values; value->value != nullptr; ++value) {
        if (std::string_view(value->value) == option->default_value) continue;
        line += std::string("|") + value->value;
      }
      text.push_back(std::move(line));
    }
    return text;
  }();
  static std::vector<retro_variable> variables = [] {
    std::vector<retro_variable> list;
    for (std::size_t at = 0; at < lines.size(); ++at) {
      list.push_back(retro_variable{.key = optionDefinitions[at].key, .value = lines[at].c_str()});
    }
    list.push_back(retro_variable{.key = nullptr, .value = nullptr});
    return list;
  }();
  environment(RETRO_ENVIRONMENT_SET_VARIABLES, variables.data());
}

// The interlace presentation the player chose, or Weave where the frontend has no choice for it.
Interlace interlaceOption() {
  retro_variable variable{.key = kInterlaceKey, .value = nullptr};
  if (environment == nullptr || !environment(RETRO_ENVIRONMENT_GET_VARIABLE, &variable) ||
      variable.value == nullptr) {
    return Interlace::Weave;
  }
  const std::string_view value = variable.value;
  if (value == "bob") return Interlace::Bob;
  if (value == "off") return Interlace::Off;
  return Interlace::Weave;
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

// The machine's frame observer. It turns each finished frame into the picture the frontend is
// handed, then stops the run there. The picture is XRGB8888, one 32-bit 0x00RRGGBB word a
// pixel, row-major from the top line, in a buffer sized once for the largest picture, so no
// frame allocates. A frame the cartridge did not interlace, or any frame under Off, is the
// picture as drawn. A field of an interlaced run is placed in the full-height picture at the
// rows of its parity, line i at row 2i + parity: under Weave the other parity's rows keep the
// last field of that parity, and under Bob this field fills them too. A picture of a new shape
// starts black.
class Screen final : public snaggletooth::FrameObserver {
 public:
  void frame(const snaggletooth::VideoFrame& field) override {
    const bool interlaced = machine != nullptr && machine->state().ppu.interlace();
    lines = field.height;
    if (!interlaced || mode == Interlace::Off) {
      width = field.width;
      height = field.height;
      for (unsigned line = 0; line < field.height; ++line) convertLine(field, line, line);
    } else {
      const unsigned tall = field.height * 2u;
      if (field.width != width || tall != height) {
        width = field.width;
        height = tall;
        std::fill_n(pixels.begin(), static_cast<std::size_t>(width) * height, 0u);
      }
      const unsigned parity = field.field & 1u;
      for (unsigned line = 0; line < field.height; ++line) {
        convertLine(field, line, 2u * line + parity);
        if (mode == Interlace::Bob) {
          std::copy_n(rowAt(2u * line + parity), width, rowAt(2u * line + (parity ^ 1u)));
        }
      }
    }
    throw FrameFinished{};
  }

  // The machine whose PPU says whether the run is interlaced.
  const snaggletooth::Snes* machine = nullptr;
  Interlace mode = Interlace::Weave;

  std::vector<std::uint32_t> pixels = std::vector<std::uint32_t>(
      static_cast<std::size_t>(kMaxWidth) * kMaxHeight, 0u);
  unsigned width = kBaseWidth;
  unsigned height = kBaseHeight;
  unsigned lines = kBaseHeight;  // the lines the console drew the last frame with

 private:
  [[nodiscard]] std::uint32_t* rowAt(unsigned row) {
    return pixels.data() + static_cast<std::size_t>(row) * width;
  }

  void convertLine(const snaggletooth::VideoFrame& field, unsigned line, unsigned row) {
    const std::uint8_t* from =
        field.pixels.data() + static_cast<std::size_t>(line) * field.width * 4u;
    std::uint32_t* to = rowAt(row);
    for (unsigned x = 0; x < field.width; ++x, from += 4) {
      to[x] = (static_cast<std::uint32_t>(from[0]) << 16) |
              (static_cast<std::uint32_t>(from[1]) << 8) | static_cast<std::uint32_t>(from[2]);
    }
  }
};

// A loaded cartridge and everything that runs it. The machine keeps a pointer to the
// observer, so a session is built in place and never moved.
struct Session {
  explicit Session(const snaggletooth::SnesConfig& config) : machine(config) {
    screen.machine = &machine;
    machine.setFrameObserver(&screen);
  }
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  Screen screen;
  snaggletooth::Snes machine;
  // Where the machine's sound is drained to each frame, in as many passes as it takes. A frame
  // makes about 533 samples at 60 Hz and 640 at 50 Hz, so one pass is the usual case.
  std::array<snaggletooth::StereoFrame, 1024> sound{};
  // The geometry the frontend was last told.
  unsigned shownWidth = kBaseWidth;
  unsigned shownHeight = kBaseHeight;
  unsigned shownLines = kBaseHeight;
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
  portDevices.fill(RETRO_DEVICE_JOYPAD);
  inputBitmasks =
      environment != nullptr && environment(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, nullptr);
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

// Puts a device in one of the console's two ports: the console's pad for RETRO_DEVICE_JOYPAD,
// and an empty port for RETRO_DEVICE_NONE or any device the core does not offer. A port past
// the second is ignored. The machine is handed the port's new contents from the next frame.
RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device) {
  if (port < kPortCount) portDevices[port] = device;
}

// The console's reset button.
RETRO_API void retro_reset(void) {
  if (session) session->machine.reset();
}

// One console frame: the machine runs until the beam reaches the next frame's first line, so
// a frame drawn a line longer, as an interlaced field is, is run whole. Each port holds what
// the frontend reports for it as the frame begins, so the frame's reads of the pads see it. The
// frontend is handed the picture that frame makes, and told the new geometry first when the
// picture's shape has changed, then the sound the frame made, every sample from the console's
// time up to the frame's end. A change to the interlace option takes effect from this frame.
RETRO_API void retro_run(void) {
  if (inputPoll != nullptr) inputPoll();
  if (!session) return;
  bool optionsChanged = false;
  if (environment != nullptr &&
      environment(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &optionsChanged) && optionsChanged) {
    session->screen.mode = interlaceOption();
  }
  session->machine.setJoypad(snaggletooth::JoypadPort::One, heldOn(0u));
  session->machine.setJoypad(snaggletooth::JoypadPort::Two, heldOn(1u));

  const std::uint64_t frameCycles =
      snaggletooth::consoleClock(session->machine.region()).masterCyclesPerFrame;
  try {
    for (;;) session->machine.run(frameCycles);
  } catch (const FrameFinished&) {
  }

  const Screen& screen = session->screen;
  if (screen.width != session->shownWidth || screen.height != session->shownHeight ||
      screen.lines != session->shownLines) {
    session->shownWidth = screen.width;
    session->shownHeight = screen.height;
    session->shownLines = screen.lines;
    retro_game_geometry geometry{.base_width = screen.width,
                                 .base_height = screen.height,
                                 .max_width = kMaxWidth,
                                 .max_height = kMaxHeight,
                                 .aspect_ratio = aspectOf(screen.lines)};
    if (environment != nullptr) environment(RETRO_ENVIRONMENT_SET_GEOMETRY, &geometry);
  }
  if (videoRefresh != nullptr) {
    videoRefresh(screen.pixels.data(), screen.width, screen.height,
                 static_cast<std::size_t>(screen.width) * sizeof(std::uint32_t));
  }

  for (;;) {
    const std::size_t made = session->machine.takeFrames(session->sound);
    handOver(std::span<const snaggletooth::StereoFrame>(session->sound).first(made));
    if (made < session->sound.size()) break;
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
// whose board carries a coprocessor is refused with a message naming the chip. The frontend is
// told the devices each port takes and the name of every button on them.
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

  environment(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO,
              const_cast<retro_controller_info*>(kControllerInfo));
  environment(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS,
              const_cast<retro_input_descriptor*>(kInputDescriptors.data()));

  session = std::make_unique<Session>(
      snaggletooth::SnesConfig{.rom = rom, .region = regionOf(rom)});
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

RETRO_API void* retro_get_memory_data(unsigned id) {
  (void)id;
  return nullptr;
}

RETRO_API size_t retro_get_memory_size(unsigned id) {
  (void)id;
  return 0;
}
