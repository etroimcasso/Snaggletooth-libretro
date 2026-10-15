// The picture as a frontend receives it: every frame the machine draws, in XRGB8888, at the
// shape it was drawn (256 or 512 across, 224 or 239 lines, interlaced fields woven, bobbed or
// passed through as the interlace option asks), with the geometry sent as the shape changes and
// the aspect of square pixels. The interlace option is among the options listed in every form a
// frontend takes.
//
// What each frame should hold comes from a second machine run here, with an observer of the
// suite's own: its fields, converted and composed as the pixel format and the interlace modes
// define, are the pictures the core must hand over.

#include <libretro.h>

#include "examples/common.h"
#include "loaded_core.h"
#include "snaggletooth/snes/snes.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace {

using snaggletooth_libretro_test::LoadedCore;

constexpr const char* kInterlaceKey = "snaggletooth_interlace";

// Register values for the picture cartridge: BGMODE 0 draws BG1 at two bits a pixel and
// BGMODE 5 in half-pixels; SETINI bit 0 asks for an interlaced picture and bit 2 for the
// 239-line one.
constexpr std::uint8_t kModeZero = 0x00u;
constexpr std::uint8_t kModeFive = 0x05u;
constexpr std::uint8_t kProgressive = 0x00u;
constexpr std::uint8_t kInterlaced = 0x01u;
constexpr std::uint8_t kTaller = 0x04u;

// The aspects of square pixels 256 across over 224 and over 239 lines.
constexpr float kAspect224 = 256.0f / 224.0f;
constexpr float kAspect239 = 256.0f / 239.0f;

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

// The first `count` frames a cartridge draws, from a machine run here.
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

// What the test frontend saw the core do, and what it answers the core's questions with.
struct Frontend {
  unsigned optionsVersion = 2u;  // the answer to GET_CORE_OPTIONS_VERSION
  std::optional<std::string> interlace;  // the interlace option's value, when the player set one
  bool optionsChanged = false;           // the answer to GET_VARIABLE_UPDATE, cleared once read

  // The options as each form listed them.
  std::vector<std::string> v2Categories;
  std::vector<std::string> v2Options;  // "key|category|default|value,value,..."
  std::vector<std::string> v1Options;  // "key|default|value,value,..."
  std::vector<std::string> v0Options;  // "key|Description; value|value"

  std::vector<retro_game_geometry> geometries;  // every SET_GEOMETRY
  std::vector<Picture> pictures;
  std::vector<std::size_t> pitches;
};

Frontend* frontend = nullptr;

std::string valueList(const retro_core_option_value* values) {
  std::string text;
  for (const retro_core_option_value* value = values; value->value != nullptr; ++value) {
    if (!text.empty()) text += ',';
    text += value->value;
  }
  return text;
}

bool environment(unsigned command, void* data) {
  switch (command) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
      return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
      *static_cast<unsigned*>(data) = frontend->optionsVersion;
      return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2: {
      if (frontend->optionsVersion < 2u) return false;
      const auto* options = static_cast<const retro_core_options_v2*>(data);
      for (const auto* category = options->categories; category->key != nullptr; ++category) {
        frontend->v2Categories.push_back(std::string(category->key) + '|' + category->desc);
      }
      for (const auto* option = options->definitions; option->key != nullptr; ++option) {
        frontend->v2Options.push_back(std::string(option->key) + '|' + option->category_key + '|' +
                                      option->default_value + '|' + valueList(option->values));
      }
      return true;
    }
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS: {
      if (frontend->optionsVersion < 1u) return false;
      for (const auto* option = static_cast<const retro_core_option_definition*>(data);
           option->key != nullptr; ++option) {
        frontend->v1Options.push_back(std::string(option->key) + '|' + option->default_value + '|' +
                                      valueList(option->values));
      }
      return true;
    }
    case RETRO_ENVIRONMENT_SET_VARIABLES:
      for (const auto* variable = static_cast<const retro_variable*>(data);
           variable->key != nullptr; ++variable) {
        frontend->v0Options.push_back(std::string(variable->key) + '|' + variable->value);
      }
      return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
      auto* variable = static_cast<retro_variable*>(data);
      if (!frontend->interlace || std::string(variable->key) != kInterlaceKey) return false;
      variable->value = frontend->interlace->c_str();
      return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
      *static_cast<bool*>(data) = frontend->optionsChanged;
      frontend->optionsChanged = false;
      return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
      frontend->geometries.push_back(*static_cast<const retro_game_geometry*>(data));
      return true;
    default:
      return false;
  }
}

void videoRefresh(const void* data, unsigned width, unsigned height, size_t pitch) {
  frontend->pitches.push_back(pitch);
  Picture picture{.width = width, .height = height,
                  .pixels = std::vector<std::uint32_t>(std::size_t{width} * height)};
  if (data != nullptr) {
    for (unsigned row = 0; row < height; ++row) {
      std::memcpy(picture.pixels.data() + std::size_t{row} * width,
                  static_cast<const std::uint8_t*>(data) + row * pitch, std::size_t{width} * 4u);
    }
  }
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
  void unload() { module_.entry<void()>("retro_unload_game")(); }
  void run(std::size_t times = 1u) {
    for (std::size_t at = 0; at < times; ++at) module_.entry<void()>("retro_run")();
  }
  [[nodiscard]] retro_system_av_info avInfo() const {
    retro_system_av_info info{};
    module_.entry<void(retro_system_av_info*)>("retro_get_system_av_info")(&info);
    return info;
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
      if ((seen[at].pixels[pixel] & kColourBits) != expected[at].pixels[pixel]) ++differing;
    }
    EXPECT_EQ(differing, 0u) << "frame " << at;
  }
}

// The pictures the core hands over for a cartridge under an interlace option (none set = the
// frontend has no value for it).
std::vector<Picture> picturesFromCore(const std::vector<std::uint8_t>& rom, std::size_t runs,
                                      std::optional<std::string> interlace = std::nullopt) {
  Frontend seen;
  seen.interlace = std::move(interlace);
  Core core(seen);
  if (!core.loaded() || !core.load(rom)) return {};
  core.run(runs);
  return seen.pictures;
}

TEST(Picture, EachFrameIsTheMachinesPictureInXrgb8888) {
  constexpr std::size_t kRuns = 8u;
  const std::vector<std::uint8_t> rom = pictureCartridge(kModeZero, kProgressive);
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(rom));
  core.run(kRuns);
  const std::vector<Picture> expected = expectedFor(rom, kRuns, Mode::Weave);
  // The picture holds red, green, blue and the backdrop: a picture of fewer colours could hide a
  // swapped channel.
  std::ptrdiff_t mostColours = 0;
  for (const Picture& picture : expected) {
    std::vector<std::uint32_t> colours(picture.pixels);
    std::sort(colours.begin(), colours.end());
    mostColours =
        std::max(mostColours, std::unique(colours.begin(), colours.end()) - colours.begin());
  }
  ASSERT_GE(mostColours, 4);
  ASSERT_NO_FATAL_FAILURE(expectPictures(seen.pictures, expected));
  for (std::size_t at = 0; at < seen.pitches.size(); ++at) {
    EXPECT_EQ(seen.pitches[at], std::size_t{seen.pictures[at].width} * 4u);
  }
}

TEST(Picture, TwoLoadsHandOverIdenticalFrames) {
  constexpr std::size_t kRuns = 120u;
  const std::vector<std::uint8_t> rom = pictureCartridge(kModeZero, kInterlaced);
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(rom));
  core.run(kRuns);
  const std::vector<Picture> first = std::move(seen.pictures);
  seen.pictures.clear();
  core.unload();
  ASSERT_TRUE(core.load(rom));
  core.run(kRuns);
  ASSERT_EQ(first.size(), kRuns);
  ASSERT_NO_FATAL_FAILURE(expectPictures(seen.pictures, first));
}

TEST(Picture, AvInfoReportsTheSquarePixelShape) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(pictureCartridge(kModeZero, kProgressive)));
  const retro_system_av_info info = core.avInfo();
  EXPECT_EQ(info.geometry.base_width, 256u);
  EXPECT_EQ(info.geometry.base_height, 224u);
  EXPECT_EQ(info.geometry.max_width, 512u);
  EXPECT_EQ(info.geometry.max_height, 478u);
  EXPECT_FLOAT_EQ(info.geometry.aspect_ratio, kAspect224);
}

TEST(Picture, AnUnchangingShapeSendsNoGeometry) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(pictureCartridge(kModeZero, kProgressive)));
  core.run(60u);
  EXPECT_TRUE(seen.geometries.empty());
}

TEST(Picture, HalfPixelFramesAre512AcrossAtTheSameAspect) {
  constexpr std::size_t kRuns = 6u;
  const std::vector<std::uint8_t> rom = pictureCartridge(kModeFive, kProgressive);
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(rom));
  core.run(kRuns);
  ASSERT_NO_FATAL_FAILURE(expectPictures(seen.pictures, expectedFor(rom, kRuns, Mode::Weave)));
  EXPECT_EQ(seen.pictures.back().width, 512u);
  EXPECT_EQ(seen.pictures.back().height, 224u);
  ASSERT_EQ(seen.geometries.size(), 1u);
  EXPECT_EQ(seen.geometries[0].base_width, 512u);
  EXPECT_EQ(seen.geometries[0].base_height, 224u);
  EXPECT_FLOAT_EQ(seen.geometries[0].aspect_ratio, kAspect224);
  EXPECT_EQ(core.avInfo().geometry.base_width, 512u);
}

TEST(Picture, TheTallerPictureIs239Lines) {
  constexpr std::size_t kRuns = 6u;
  const std::vector<std::uint8_t> rom = pictureCartridge(kModeZero, kTaller);
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(rom));
  core.run(kRuns);
  ASSERT_NO_FATAL_FAILURE(expectPictures(seen.pictures, expectedFor(rom, kRuns, Mode::Weave)));
  EXPECT_EQ(seen.pictures.back().width, 256u);
  EXPECT_EQ(seen.pictures.back().height, 239u);
  ASSERT_EQ(seen.geometries.size(), 1u);
  EXPECT_EQ(seen.geometries[0].base_height, 239u);
  EXPECT_FLOAT_EQ(seen.geometries[0].aspect_ratio, kAspect239);
}

TEST(Picture, WeaveInterleavesTheTwoFieldsByDefault) {
  constexpr std::size_t kRuns = 12u;
  const std::vector<std::uint8_t> rom = pictureCartridge(kModeZero, kInterlaced);
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(rom));
  core.run(kRuns);
  ASSERT_NO_FATAL_FAILURE(expectPictures(seen.pictures, expectedFor(rom, kRuns, Mode::Weave)));
  const Picture& last = seen.pictures.back();
  ASSERT_EQ(last.width, 256u);
  ASSERT_EQ(last.height, 448u);
  // Two fields of different colours: the rows alternate between them.
  EXPECT_NE(last.pixels[0], last.pixels[std::size_t{last.width}]);
  EXPECT_EQ(last.pixels[0], last.pixels[std::size_t{2u} * last.width]);
  ASSERT_EQ(seen.geometries.size(), 1u);
  EXPECT_EQ(seen.geometries[0].base_height, 448u);
  EXPECT_FLOAT_EQ(seen.geometries[0].aspect_ratio, kAspect224);
}

TEST(Picture, BobFillsEveryRowFromEachField) {
  constexpr std::size_t kRuns = 12u;
  const std::vector<std::uint8_t> rom = pictureCartridge(kModeZero, kInterlaced);
  const std::vector<Picture> seen = picturesFromCore(rom, kRuns, "bob");
  ASSERT_NO_FATAL_FAILURE(expectPictures(seen, expectedFor(rom, kRuns, Mode::Bob)));
  ASSERT_FALSE(seen.empty());
  const Picture& last = seen.back();
  ASSERT_EQ(last.height, 448u);
  EXPECT_EQ(last.pixels[0], last.pixels[std::size_t{last.width}]);
}

TEST(Picture, OffHandsOverEachFieldAsDrawn) {
  constexpr std::size_t kRuns = 12u;
  const std::vector<std::uint8_t> rom = pictureCartridge(kModeZero, kInterlaced);
  const std::vector<Picture> seen = picturesFromCore(rom, kRuns, "off");
  ASSERT_NO_FATAL_FAILURE(expectPictures(seen, expectedFor(rom, kRuns, Mode::Off)));
  ASSERT_FALSE(seen.empty());
  EXPECT_EQ(seen.back().height, 224u);
}

TEST(Picture, TheLargestPictureIs512By478) {
  constexpr std::size_t kRuns = 12u;
  const std::vector<std::uint8_t> rom = pictureCartridge(kModeFive, kInterlaced | kTaller);
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(rom));
  core.run(kRuns);
  ASSERT_NO_FATAL_FAILURE(expectPictures(seen.pictures, expectedFor(rom, kRuns, Mode::Weave)));
  ASSERT_FALSE(seen.pictures.empty());
  EXPECT_EQ(seen.pictures.back().width, 512u);
  EXPECT_EQ(seen.pictures.back().height, 478u);
  ASSERT_FALSE(seen.geometries.empty());
  EXPECT_FLOAT_EQ(seen.geometries.back().aspect_ratio, kAspect239);
}

TEST(Picture, AnOptionChangeTakesEffectOnTheNextRun) {
  const std::vector<std::uint8_t> rom = pictureCartridge(kModeZero, kInterlaced);
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(rom));
  core.run(8u);
  ASSERT_EQ(seen.pictures.back().height, 448u);
  seen.interlace = "off";
  seen.optionsChanged = true;
  core.run();
  EXPECT_EQ(seen.pictures.back().height, 224u);
  ASSERT_FALSE(seen.geometries.empty());
  EXPECT_EQ(seen.geometries.back().base_height, 224u);
  EXPECT_FLOAT_EQ(seen.geometries.back().aspect_ratio, kAspect224);

  // Back to Weave: the full-height picture starts black, so the first field after the change
  // fills its own parity's rows and leaves the other parity's black.
  seen.interlace = "weave";
  seen.optionsChanged = true;
  core.run();
  const Picture& woven = seen.pictures.back();
  ASSERT_EQ(woven.height, 448u);
  const auto isBlack = [](std::uint32_t pixel) { return (pixel & kColourBits) == 0u; };
  unsigned blackRows = 0;
  for (unsigned row = 0; row < woven.height; ++row) {
    const auto start = woven.pixels.begin() + std::ptrdiff_t{row} * woven.width;
    if (std::all_of(start, start + woven.width, isBlack)) ++blackRows;
  }
  EXPECT_EQ(blackRows, 224u);
}

// Whether a listing holds an entry. The interlace option is one row of the core's options; the
// whole listing is pinned in options_test.cpp.
bool lists(const std::vector<std::string>& listing, const std::string& entry) {
  return std::find(listing.begin(), listing.end(), entry) != listing.end();
}

TEST(Picture, TheInterlaceOptionIsListedInCategories) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  EXPECT_TRUE(lists(seen.v2Categories, "video|Video"));
  EXPECT_TRUE(lists(seen.v2Options, "snaggletooth_interlace|video|weave|weave,bob,off"));
  EXPECT_TRUE(seen.v1Options.empty());
  EXPECT_TRUE(seen.v0Options.empty());
}

TEST(Picture, AFirstVersionFrontendIsListedThePlainOption) {
  Frontend seen;
  seen.optionsVersion = 1u;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  EXPECT_TRUE(lists(seen.v1Options, "snaggletooth_interlace|weave|weave,bob,off"));
  EXPECT_TRUE(seen.v2Options.empty());
  EXPECT_TRUE(seen.v0Options.empty());
}

TEST(Picture, AFrontendWithoutOptionVersionsIsListedTheVariable) {
  Frontend seen;
  seen.optionsVersion = 0u;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  EXPECT_TRUE(lists(seen.v0Options, "snaggletooth_interlace|Interlaced Picture; weave|bob|off"));
  EXPECT_TRUE(seen.v2Options.empty());
  EXPECT_TRUE(seen.v1Options.empty());
}

}  // namespace
