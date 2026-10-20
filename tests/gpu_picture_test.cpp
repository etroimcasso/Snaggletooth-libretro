// The picture drawn on the GPU, the Picture Thread option's default: every frame the core hands
// over is the frame the machine draws on the processor, at every shape and under every interlace
// option; a state saved while drawing on the processor plays on under the GPU; and where no GPU
// device opens, the core says so and draws on the processor.
//
// What each frame should hold comes from a second machine run here on the processor, with an
// observer of the suite's own. A case that needs a device skips, saying why, on a computer where
// none opens: the core's message names SDL's reason.

#include <libretro.h>

#include "examples/common.h"
#include "loaded_core.h"
#include "snaggletooth/snes/snes.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

using snaggletooth_libretro_test::LoadedCore;

constexpr const char* kInterlaceKey = "snaggletooth_interlace";
constexpr const char* kPictureThreadKey = "snaggletooth_ppu_thread";

// What the core's message begins with where it cannot draw on the GPU.
constexpr const char* kNoGpu = "Snaggletooth cannot draw on the GPU here";

// Register values for the picture cartridge: BGMODE 0 draws BG1 at two bits a pixel and
// BGMODE 5 in half-pixels; SETINI bit 0 asks for an interlaced picture and bit 2 for the
// 239-line one.
constexpr std::uint8_t kModeZero = 0x00u;
constexpr std::uint8_t kModeFive = 0x05u;
constexpr std::uint8_t kProgressive = 0x00u;
constexpr std::uint8_t kInterlaced = 0x01u;
constexpr std::uint8_t kTaller = 0x04u;

// A cartridge that draws BG1 from one 2-bit tile whose columns run through colours 0, 1, 2, 3
// twice — the backdrop, red, green, blue — across the whole screen, in the screen mode and
// SETINI it is given. Its vertical-blank handler writes the frame count into colour 0, so every
// frame's backdrop differs from the last and two fields of an interlaced picture never match.
std::vector<std::uint8_t> pictureCartridge(std::uint8_t bgmode, std::uint8_t setini) {
  using snaggletooth::examples::put;
  std::vector<std::uint8_t> rom = snaggletooth::examples::imageWithNmi();
  put(rom, 0x0000u, {
      0xA9u, 0x80u,                // $8000 LDA #$80
      0x8Du, 0x00u, 0x21u,         // $8002 STA !$2100         forced blank
      0x8Du, 0x15u, 0x21u,         // $8005 STA !$2115         VMAIN: step after the high byte
      0x9Cu, 0x16u, 0x21u,         // $8008 STZ !$2116         VMADD = word $0000
      0x9Cu, 0x17u, 0x21u,         // $800B STZ !$2117
      0xA2u, 0x08u,                // $800E LDX #$08           eight rows of the tile
      0xA9u, 0x55u,                // $8010 LDA #$55
      0x8Du, 0x18u, 0x21u,         // $8012 STA !$2118         plane 0: columns 1, 3, 5, 7
      0xA9u, 0x33u,                // $8015 LDA #$33
      0x8Du, 0x19u, 0x21u,         // $8017 STA !$2119         plane 1: columns 2, 3, 6, 7
      0xCAu,                       // $801A DEX
      0xD0u, 0xF3u,                // $801B BNE $8010
      0x9Cu, 0x21u, 0x21u,         // $801D STZ !$2121         CGADD = 0
      0xA2u, 0x00u,                // $8020 LDX #$00
      0xBDu, 0x00u, 0x90u,         // $8022 LDA !$9000,X       the palette
      0x8Du, 0x22u, 0x21u,         // $8025 STA !$2122
      0xE8u,                       // $8028 INX
      0xE0u, 0x08u,                // $8029 CPX #$08
      0xD0u, 0xF5u,                // $802B BNE $8022
      0xA9u, 0x04u,                // $802D LDA #$04
      0x8Du, 0x07u, 0x21u,         // $802F STA !$2107         BG1SC: the screen at word $0400
      0x9Cu, 0x0Bu, 0x21u,         // $8032 STZ !$210B         BG12NBA: BG1's tiles at word $0000
      0xA9u, 0x01u,                // $8035 LDA #$01
      0x8Du, 0x2Cu, 0x21u,         // $8037 STA !$212C         TM: BG1 on the main screen
      0xA9u, bgmode,               // $803A LDA #bgmode
      0x8Du, 0x05u, 0x21u,         // $803C STA !$2105         BGMODE
      0xA9u, setini,               // $803F LDA #setini
      0x8Du, 0x33u, 0x21u,         // $8041 STA !$2133         SETINI
      0xA9u, 0x0Fu,                // $8044 LDA #$0F
      0x8Du, 0x00u, 0x21u,         // $8046 STA !$2100         screen on, full brightness
      0xA9u, 0x80u,                // $8049 LDA #$80
      0x8Du, 0x00u, 0x42u,         // $804B STA !$4200         NMI on
      0x80u, 0xFEu,                // $804E BRA $804E
  });
  put(rom, 0x1000u, {
      0x10u, 0x42u,                // $9000 colour 0: grey
      0x1Fu, 0x00u,                // $9002 colour 1: red
      0xE0u, 0x03u,                // $9004 colour 2: green
      0x00u, 0x7Cu,                // $9006 colour 3: blue
  });
  put(rom, 0x0310u, {
      0xADu, 0x10u, 0x42u,         // $8310 LDA !$4210         acknowledge the NMI
      0xEEu, 0x10u, 0x00u,         // $8313 INC !$0010         count the frame
      0x9Cu, 0x21u, 0x21u,         // $8316 STZ !$2121         CGADD = 0
      0xADu, 0x10u, 0x00u,         // $8319 LDA !$0010
      0x8Du, 0x22u, 0x21u,         // $831C STA !$2122         colour 0, low byte
      0x8Du, 0x22u, 0x21u,         // $831F STA !$2122         colour 0, high byte
      0x40u,                       // $8322 RTI
  });
  return rom;
}

// One frame the reference machine drew, as its observer was handed it.
struct Field {
  std::vector<std::uint8_t> rgba;
  unsigned width = 0;
  unsigned height = 0;
  std::uint8_t parity = 0;
  bool interlaced = false;
};

class FieldRecorder final : public snaggletooth::FrameObserver {
 public:
  explicit FieldRecorder(const snaggletooth::Snes& machine) : machine_(machine) {}

  void frame(const snaggletooth::VideoFrame& frame) override {
    fields.push_back(Field{.rgba = {frame.pixels.begin(), frame.pixels.end()},
                           .width = frame.width,
                           .height = frame.height,
                           .parity = frame.field,
                           .interlaced = machine_.state().ppu.interlace()});
  }

  std::vector<Field> fields;

 private:
  const snaggletooth::Snes& machine_;
};

// The first `count` frames a cartridge draws, from a machine run here on the processor.
std::vector<Field> referenceFields(const std::vector<std::uint8_t>& rom, std::size_t count) {
  snaggletooth::Snes machine(snaggletooth::SnesConfig{.rom = rom});
  FieldRecorder recorder(machine);
  machine.setFrameObserver(&recorder);
  const std::uint64_t frameCycles =
      snaggletooth::consoleClock(machine.region()).masterCyclesPerFrame;
  while (recorder.fields.size() < count) machine.run(frameCycles);
  recorder.fields.resize(count);
  return recorder.fields;
}

// A picture as the frontend sees it: XRGB8888 words, `width` a row, rows packed.
struct Picture {
  unsigned width = 0;
  unsigned height = 0;
  std::vector<std::uint32_t> pixels;

  [[nodiscard]] bool operator==(const Picture&) const = default;
};

// XRGB8888 is one native 32-bit word a pixel, red in bits 16-23, green in 8-15, blue in 0-7;
// the top byte is not part of the colour.
constexpr std::uint32_t kColourBits = 0x00FFFFFFu;

std::uint32_t xrgb(const std::uint8_t* rgba) {
  return (static_cast<std::uint32_t>(rgba[0]) << 16) | (static_cast<std::uint32_t>(rgba[1]) << 8) |
         static_cast<std::uint32_t>(rgba[2]);
}

enum class Mode { Weave, Bob, Off };

// The pictures a run of these fields shows under `mode`. A frame the cartridge did not
// interlace, or any under Off, is shown as drawn. A field of an interlaced run lands in the
// full-height picture at line i -> row 2i + parity; Weave leaves the other parity's rows as the
// last field of that parity left them (black from the moment the shape began), Bob fills them
// from this field.
std::vector<Picture> expectedPictures(const std::vector<Field>& fields, Mode mode) {
  std::vector<Picture> shown;
  Picture held;
  for (const Field& field : fields) {
    const auto lineAt = [&](unsigned line, std::uint32_t* to) {
      for (unsigned x = 0; x < field.width; ++x) {
        to[x] = xrgb(field.rgba.data() + (static_cast<std::size_t>(line) * field.width + x) * 4u);
      }
    };
    if (!field.interlaced || mode == Mode::Off) {
      Picture picture{
          .width = field.width,
          .height = field.height,
          .pixels = std::vector<std::uint32_t>(std::size_t{field.width} * field.height)};
      for (unsigned line = 0; line < field.height; ++line) {
        lineAt(line, picture.pixels.data() + std::size_t{line} * field.width);
      }
      held = picture;
      shown.push_back(picture);
      continue;
    }
    const unsigned tall = field.height * 2u;
    if (held.width != field.width || held.height != tall) {
      held = Picture{.width = field.width, .height = tall,
                     .pixels = std::vector<std::uint32_t>(std::size_t{field.width} * tall, 0u)};
    }
    const unsigned parity = field.parity & 1u;
    for (unsigned line = 0; line < field.height; ++line) {
      lineAt(line, held.pixels.data() + std::size_t{2u * line + parity} * field.width);
      if (mode == Mode::Bob) {
        lineAt(line, held.pixels.data() + std::size_t{2u * line + (parity ^ 1u)} * field.width);
      }
    }
    shown.push_back(held);
  }
  return shown;
}

// The pictures the first `runs` frames of a cartridge show under `mode`.
std::vector<Picture> expectedFor(const std::vector<std::uint8_t>& rom, std::size_t runs,
                                 Mode mode) {
  return expectedPictures(referenceFields(rom, runs), mode);
}

// What the test frontend saw the core do, and the option values it answers the core with.
struct Frontend {
  std::map<std::string, std::string> values;  // the options the player set
  std::vector<std::string> messages;          // every SET_MESSAGE and SET_MESSAGE_EXT
  std::vector<Picture> pictures;

  // The core's message that it cannot draw on the GPU, or an empty string.
  [[nodiscard]] std::string noGpu() const {
    for (const std::string& message : messages) {
      if (message.rfind(kNoGpu, 0) == 0) return message;
    }
    return {};
  }
};

Frontend* frontend = nullptr;

bool environment(unsigned command, void* data) {
  switch (command) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
      return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
      auto* variable = static_cast<retro_variable*>(data);
      const auto value = frontend->values.find(variable->key);
      if (value == frontend->values.end()) return false;
      variable->value = value->second.c_str();
      return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
      *static_cast<bool*>(data) = false;
      return true;
    case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
      *static_cast<unsigned*>(data) = 1u;
      return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE:
      frontend->messages.emplace_back(static_cast<const retro_message*>(data)->msg);
      return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE_EXT:
      frontend->messages.emplace_back(static_cast<const retro_message_ext*>(data)->msg);
      return true;
    default:
      return false;
  }
}

void videoRefresh(const void* data, unsigned width, unsigned height, size_t pitch) {
  Picture picture{.width = width, .height = height,
                  .pixels = std::vector<std::uint32_t>(std::size_t{width} * height)};
  if (data != nullptr) {
    for (unsigned row = 0; row < height; ++row) {
      std::memcpy(picture.pixels.data() + std::size_t{row} * width,
                  static_cast<const std::uint8_t*>(data) + row * pitch, std::size_t{width} * 4u);
    }
  }
  for (std::uint32_t& pixel : picture.pixels) pixel &= kColourBits;
  frontend->pictures.push_back(std::move(picture));
}

void audioSample(std::int16_t left, std::int16_t right) {
  (void)left;
  (void)right;
}

size_t audioSampleBatch(const std::int16_t* data, size_t frames) {
  (void)data;
  return frames;
}

void inputPoll() {}

std::int16_t inputState(unsigned port, unsigned device, unsigned index, unsigned id) {
  (void)port;
  (void)device;
  (void)index;
  (void)id;
  return 0;
}

// The core loaded and set up as a frontend sets one up: the callbacks handed over, then
// retro_init. Deinitialized when the case ends.
class Core {
 public:
  explicit Core(Frontend& seen) {
    frontend = &seen;
    if (!module_.loaded()) return;
    module_.entry<void(retro_environment_t)>("retro_set_environment")(environment);
    module_.entry<void(retro_video_refresh_t)>("retro_set_video_refresh")(videoRefresh);
    module_.entry<void(retro_audio_sample_t)>("retro_set_audio_sample")(audioSample);
    module_.entry<void(retro_audio_sample_batch_t)>("retro_set_audio_sample_batch")(
        audioSampleBatch);
    module_.entry<void(retro_input_poll_t)>("retro_set_input_poll")(inputPoll);
    module_.entry<void(retro_input_state_t)>("retro_set_input_state")(inputState);
    module_.entry<void()>("retro_init")();
  }
  Core(const Core&) = delete;
  Core& operator=(const Core&) = delete;
  ~Core() {
    if (module_.loaded()) module_.entry<void()>("retro_deinit")();
    frontend = nullptr;
  }

  [[nodiscard]] bool loaded() const noexcept { return module_.loaded(); }

  bool load(const std::vector<std::uint8_t>& image) {
    const retro_game_info game{.path = nullptr, .data = image.data(), .size = image.size(),
                               .meta = nullptr};
    return module_.entry<bool(const retro_game_info*)>("retro_load_game")(&game);
  }
  void run(std::size_t times = 1u) {
    for (std::size_t at = 0; at < times; ++at) module_.entry<void()>("retro_run")();
  }
  [[nodiscard]] std::vector<std::uint8_t> serialize() const {
    std::vector<std::uint8_t> state(module_.entry<size_t()>("retro_serialize_size")());
    if (!module_.entry<bool(void*, size_t)>("retro_serialize")(state.data(), state.size())) {
      state.clear();
    }
    return state;
  }
  bool unserialize(const std::vector<std::uint8_t>& state) {
    return module_.entry<bool(const void*, size_t)>("retro_unserialize")(state.data(),
                                                                         state.size());
  }

 private:
  LoadedCore module_;
};

// Every picture the core handed over matches the expected one: the same shape, and the same
// colour at every pixel.
void expectPictures(const std::vector<Picture>& seen, const std::vector<Picture>& expected) {
  ASSERT_EQ(seen.size(), expected.size());
  for (std::size_t at = 0; at < seen.size(); ++at) {
    ASSERT_EQ(seen[at].width, expected[at].width) << "frame " << at;
    ASSERT_EQ(seen[at].height, expected[at].height) << "frame " << at;
    std::size_t differing = 0;
    for (std::size_t pixel = 0; pixel < seen[at].pixels.size(); ++pixel) {
      if (seen[at].pixels[pixel] != expected[at].pixels[pixel]) ++differing;
    }
    EXPECT_EQ(differing, 0u) << "frame " << at;
  }
}

// Runs a cartridge `runs` frames under the default Picture Thread, the GPU, with the interlace
// option set where `interlace` names a value, and holds every picture to the processor's.
// Skips, saying why, where the core reports that it cannot draw on the GPU.
void expectGpuPictures(const std::vector<std::uint8_t>& rom, std::size_t runs, Mode mode,
                       const char* interlace = nullptr) {
  Frontend seen;
  if (interlace != nullptr) seen.values[kInterlaceKey] = interlace;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(rom));
  if (const std::string reason = seen.noGpu(); !reason.empty()) {
    GTEST_SKIP() << "no GPU device here: " << reason;
  }
  core.run(runs);
  ASSERT_TRUE(seen.noGpu().empty());
  expectPictures(seen.pictures, expectedFor(rom, runs, mode));
}

TEST(GpuPicture, GpuFramesMatchTheProcessorsFrames) {
  expectGpuPictures(pictureCartridge(kModeZero, kProgressive), 8u, Mode::Weave);
}

TEST(GpuPicture, GpuHalfPixelFramesMatch) {
  expectGpuPictures(pictureCartridge(kModeFive, kProgressive), 6u, Mode::Weave);
}

TEST(GpuPicture, GpuTallerPictureMatches) {
  expectGpuPictures(pictureCartridge(kModeZero, kTaller), 6u, Mode::Weave);
}

TEST(GpuPicture, GpuWeaveMatches) {
  expectGpuPictures(pictureCartridge(kModeFive, kInterlaced), 12u, Mode::Weave, "weave");
}

TEST(GpuPicture, GpuBobMatches) {
  expectGpuPictures(pictureCartridge(kModeFive, kInterlaced), 12u, Mode::Bob, "bob");
}

TEST(GpuPicture, GpuOffMatches) {
  expectGpuPictures(pictureCartridge(kModeFive, kInterlaced), 12u, Mode::Off, "off");
}

TEST(GpuPicture, AStateSavedOnTheProcessorLoadsUnderTheGpu) {
  const std::vector<std::uint8_t> rom = pictureCartridge(kModeZero, kInterlaced);
  std::vector<std::uint8_t> state;
  std::vector<Picture> followed;
  {
    Frontend seen;
    seen.values[kPictureThreadKey] = "disabled";
    Core core(seen);
    ASSERT_TRUE(core.loaded());
    ASSERT_TRUE(core.load(rom));
    core.run(4u);
    state = core.serialize();
    ASSERT_FALSE(state.empty());
    seen.pictures.clear();
    core.run(8u);
    followed = seen.pictures;
  }
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(rom));
  if (const std::string reason = seen.noGpu(); !reason.empty()) {
    GTEST_SKIP() << "no GPU device here: " << reason;
  }
  // A frame is shown before the state loads. The first picture after the load keeps that frame's
  // field in its other rows, as a woven picture does on the processor too; every later picture
  // is two fields drawn after the load.
  core.run();
  ASSERT_TRUE(core.unserialize(state));
  seen.pictures.clear();
  core.run(8u);
  ASSERT_EQ(seen.pictures.size(), followed.size());
  for (std::size_t at = 1; at < followed.size(); ++at) {
    EXPECT_TRUE(seen.pictures[at] == followed[at]) << "frame " << at << " after the load";
  }
}

TEST(GpuPicture, NoGpuDeviceDrawsOnTheProcessorAndSaysSo) {
  // SDL reads its hints from the environment, and no GPU driver is named "none", so no device
  // opens whatever the computer has.
#ifdef _WIN32
  _putenv_s("SDL_GPU_DRIVER", "none");
#else
  setenv("SDL_GPU_DRIVER", "none", 1);
#endif
  const std::vector<std::uint8_t> rom = pictureCartridge(kModeZero, kProgressive);
  {
    Frontend seen;
    Core core(seen);
    ASSERT_TRUE(core.loaded());
    ASSERT_TRUE(core.load(rom));
    core.run(8u);
    EXPECT_EQ(seen.messages.size(), 1u);
    EXPECT_FALSE(seen.noGpu().empty());
    expectPictures(seen.pictures, expectedFor(rom, 8u, Mode::Weave));
  }
#ifdef _WIN32
  _putenv_s("SDL_GPU_DRIVER", "");
#else
  unsetenv("SDL_GPU_DRIVER");
#endif
}

}  // namespace
