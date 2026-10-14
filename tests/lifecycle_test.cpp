// The core's life as a frontend drives it: every entry point a frontend resolves, a cartridge
// loaded from bytes, one console frame per retro_run, the console's timing and region, a
// copier's header dropped, a cartridge the library cannot run refused with a message, and the
// reset, unload, reload and deinit calls in sequence.
//
// The cartridges are Snaggletooth's own hand-built examples, with their header bytes set to
// name a region or a chip where a case needs one.

#include <libretro.h>

#include "examples/waiting/waiting.h"
#include "loaded_core.h"

#include <gtest/gtest.h>

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace {

using snaggletooth_libretro_test::LoadedCore;

// Where a LoROM cartridge's header sits in its image, and two of its bytes.
constexpr std::size_t kLoRomHeaderSite = 0x7FC0u;
constexpr std::size_t kChipsetByte = 0x16u;  // $FFD6: what sits beside the ROM
constexpr std::size_t kCountryByte = 0x19u;  // $FFD9: the region the cartridge was made for

// A chipset byte naming a SuperFX beside the ROM: the chip in the high nibble, "ROM and a
// coprocessor" in the low.
constexpr std::uint8_t kSuperFxChipset = 0x13u;
// A country byte for Europe, a 50 Hz region.
constexpr std::uint8_t kEuropeCountry = 0x02u;

// Every copier writes this many bytes ahead of the image.
constexpr std::size_t kCopierHeaderBytes = 512u;

// The console's frame rates: the master clock over the master cycles of one frame.
constexpr double kNtscFps = 236250000.0 / (11.0 * 357366.0);
constexpr double kPalFps = 21281370.0 / 425568.0;

std::vector<std::uint8_t> ntscCartridge() { return snaggletooth::examples::waitingImage(); }

std::vector<std::uint8_t> palCartridge() {
  std::vector<std::uint8_t> rom = snaggletooth::examples::waitingImage();
  rom[kLoRomHeaderSite + kCountryByte] = kEuropeCountry;
  return rom;
}

std::vector<std::uint8_t> superFxCartridge() {
  std::vector<std::uint8_t> rom = snaggletooth::examples::waitingImage();
  rom[kLoRomHeaderSite + kChipsetByte] = kSuperFxChipset;
  return rom;
}

std::vector<std::uint8_t> withCopierHeader(const std::vector<std::uint8_t>& rom) {
  std::vector<std::uint8_t> file(kCopierHeaderBytes, 0u);
  file.insert(file.end(), rom.begin(), rom.end());
  return file;
}

// What the test frontend saw the core do, and what it answers the core's questions with.
struct Frontend {
  unsigned messageInterface = 1u;  // the answer to GET_MESSAGE_INTERFACE_VERSION
  std::optional<retro_pixel_format> pixelFormat;
  std::optional<bool> supportsNoGame;
  std::vector<std::string> messages;        // through SET_MESSAGE_EXT
  std::vector<std::string> legacyMessages;  // through SET_MESSAGE
  std::vector<std::string> logged;
  unsigned videoCalls = 0;
  const void* lastData = nullptr;
  unsigned lastWidth = 0;
  unsigned lastHeight = 0;
  unsigned polls = 0;
};

Frontend* frontend = nullptr;

void logLine(retro_log_level level, const char* format, ...) {
  (void)level;
  char text[512];
  va_list arguments;
  va_start(arguments, format);
  std::vsnprintf(text, sizeof text, format, arguments);
  va_end(arguments);
  frontend->logged.emplace_back(text);
}

bool environment(unsigned command, void* data) {
  switch (command) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
      frontend->pixelFormat = *static_cast<const retro_pixel_format*>(data);
      return true;
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
      frontend->supportsNoGame = *static_cast<const bool*>(data);
      return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
      static_cast<retro_log_callback*>(data)->log = logLine;
      return true;
    case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
      *static_cast<unsigned*>(data) = frontend->messageInterface;
      return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE_EXT:
      if (frontend->messageInterface < 1u) return false;
      frontend->messages.emplace_back(static_cast<const retro_message_ext*>(data)->msg);
      return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE:
      frontend->legacyMessages.emplace_back(static_cast<const retro_message*>(data)->msg);
      return true;
    default:
      return false;
  }
}

void videoRefresh(const void* data, unsigned width, unsigned height, size_t pitch) {
  (void)pitch;
  ++frontend->videoCalls;
  frontend->lastData = data;
  frontend->lastWidth = width;
  frontend->lastHeight = height;
}

void audioSample(std::int16_t left, std::int16_t right) {
  (void)left;
  (void)right;
}

size_t audioSampleBatch(const std::int16_t* data, size_t frames) {
  (void)data;
  return frames;
}

void inputPoll() { ++frontend->polls; }

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
    init();
  }
  Core(const Core&) = delete;
  Core& operator=(const Core&) = delete;
  ~Core() {
    if (module_.loaded()) deinit();
    frontend = nullptr;
  }

  [[nodiscard]] bool loaded() const noexcept { return module_.loaded(); }

  void init() { module_.entry<void()>("retro_init")(); }
  void deinit() { module_.entry<void()>("retro_deinit")(); }

  bool load(const std::vector<std::uint8_t>& image) {
    const retro_game_info game{.path = nullptr, .data = image.data(), .size = image.size(),
                               .meta = nullptr};
    return module_.entry<bool(const retro_game_info*)>("retro_load_game")(&game);
  }
  bool loadNothing() {
    return module_.entry<bool(const retro_game_info*)>("retro_load_game")(nullptr);
  }
  void unload() { module_.entry<void()>("retro_unload_game")(); }
  void run() { module_.entry<void()>("retro_run")(); }
  void reset() { module_.entry<void()>("retro_reset")(); }
  [[nodiscard]] unsigned region() const { return module_.entry<unsigned()>("retro_get_region")(); }
  [[nodiscard]] retro_system_av_info avInfo() const {
    retro_system_av_info info{};
    module_.entry<void(retro_system_av_info*)>("retro_get_system_av_info")(&info);
    return info;
  }

 private:
  LoadedCore module_;
};

TEST(Lifecycle, EveryEntryPointIsExported) {
  const LoadedCore module;
  ASSERT_TRUE(module.loaded());
  for (const char* name : {
           "retro_set_environment", "retro_set_video_refresh", "retro_set_audio_sample",
           "retro_set_audio_sample_batch", "retro_set_input_poll", "retro_set_input_state",
           "retro_init", "retro_deinit", "retro_api_version", "retro_get_system_info",
           "retro_get_system_av_info", "retro_set_controller_port_device", "retro_reset",
           "retro_run", "retro_serialize_size", "retro_serialize", "retro_unserialize",
           "retro_cheat_reset", "retro_cheat_set", "retro_load_game", "retro_load_game_special",
           "retro_unload_game", "retro_get_region", "retro_get_memory_data",
           "retro_get_memory_size"}) {
    EXPECT_NE(module.entry<void()>(name), nullptr) << name;
  }
}

TEST(Lifecycle, SystemInfoAsksForTheBytesOfACartridge) {
  const LoadedCore module;
  ASSERT_TRUE(module.loaded());
  retro_system_info info{};
  module.entry<void(retro_system_info*)>("retro_get_system_info")(&info);
  ASSERT_NE(info.valid_extensions, nullptr);
  EXPECT_STREQ(info.valid_extensions, "sfc|smc|swc|fig");
  EXPECT_FALSE(info.need_fullpath);
}

TEST(Lifecycle, ContentIsRequired) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(seen.supportsNoGame.has_value());
  EXPECT_FALSE(*seen.supportsNoGame);
  EXPECT_FALSE(core.loadNothing());
}

TEST(Lifecycle, LoadsACartridgeAndAsksForXrgb8888) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  EXPECT_TRUE(core.load(ntscCartridge()));
  ASSERT_TRUE(seen.pixelFormat.has_value());
  EXPECT_EQ(*seen.pixelFormat, RETRO_PIXEL_FORMAT_XRGB8888);
  EXPECT_TRUE(seen.messages.empty());
}

TEST(Lifecycle, EachRunIsOneFrameAndOnePoll) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(ntscCartridge()));
  constexpr unsigned kRuns = 60u;
  for (unsigned at = 0; at < kRuns; ++at) core.run();
  EXPECT_EQ(seen.videoCalls, kRuns);
  EXPECT_EQ(seen.polls, kRuns);
  EXPECT_EQ(seen.lastData, nullptr);
  EXPECT_EQ(seen.lastWidth, 256u);
  EXPECT_EQ(seen.lastHeight, 224u);
}

TEST(Lifecycle, NtscCartridgeRunsAtTheNtscClock) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(ntscCartridge()));
  EXPECT_EQ(core.region(), static_cast<unsigned>(RETRO_REGION_NTSC));
  const retro_system_av_info info = core.avInfo();
  EXPECT_DOUBLE_EQ(info.timing.fps, kNtscFps);
  EXPECT_EQ(info.geometry.base_width, 256u);
  EXPECT_EQ(info.geometry.base_height, 224u);
  EXPECT_EQ(info.geometry.max_width, 512u);
  EXPECT_EQ(info.geometry.max_height, 478u);
}

TEST(Lifecycle, PalCartridgeRunsAtThePalClock) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(palCartridge()));
  EXPECT_EQ(core.region(), static_cast<unsigned>(RETRO_REGION_PAL));
  EXPECT_DOUBLE_EQ(core.avInfo().timing.fps, kPalFps);
  constexpr unsigned kRuns = 10u;
  for (unsigned at = 0; at < kRuns; ++at) core.run();
  EXPECT_EQ(seen.videoCalls, kRuns);
}

// The image behind a copier's header is the cartridge: read in place, the header would be
// read 512 bytes early and the cartridge's region lost.
TEST(Lifecycle, CopierHeaderIsDropped) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(withCopierHeader(palCartridge())));
  EXPECT_EQ(core.region(), static_cast<unsigned>(RETRO_REGION_PAL));
  EXPECT_DOUBLE_EQ(core.avInfo().timing.fps, kPalFps);
}

TEST(Lifecycle, ChipBoardIsRefusedWithTheChipNamed) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  EXPECT_FALSE(core.load(superFxCartridge()));
  ASSERT_EQ(seen.messages.size(), 1u);
  EXPECT_NE(seen.messages[0].find("SuperFX"), std::string::npos) << seen.messages[0];
  ASSERT_EQ(seen.logged.size(), 1u);
  EXPECT_NE(seen.logged[0].find("SuperFX"), std::string::npos) << seen.logged[0];

  // Nothing of the refused cartridge stays loaded: a run shows nothing, and the next
  // cartridge loads.
  core.run();
  EXPECT_EQ(seen.videoCalls, 0u);
  EXPECT_TRUE(core.load(ntscCartridge()));
}

TEST(Lifecycle, RefusalReachesAFrontendWithTheFirstMessageInterface) {
  Frontend seen;
  seen.messageInterface = 0u;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  EXPECT_FALSE(core.load(superFxCartridge()));
  EXPECT_TRUE(seen.messages.empty());
  ASSERT_EQ(seen.legacyMessages.size(), 1u);
  EXPECT_NE(seen.legacyMessages[0].find("SuperFX"), std::string::npos) << seen.legacyMessages[0];
}

TEST(Lifecycle, EmptyContentIsRefused) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  EXPECT_FALSE(core.load({}));
  EXPECT_EQ(seen.messages.size(), 1u);
}

TEST(Lifecycle, ResetKeepsOneFramePerRun) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(ntscCartridge()));
  for (unsigned at = 0; at < 5u; ++at) core.run();
  core.reset();
  for (unsigned at = 0; at < 5u; ++at) core.run();
  EXPECT_EQ(seen.videoCalls, 10u);
  EXPECT_EQ(seen.polls, 10u);
}

TEST(Lifecycle, UnloadReloadAndDeinitInOneProcess) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(ntscCartridge()));
  core.run();
  core.unload();
  core.run();
  EXPECT_EQ(seen.videoCalls, 1u);

  ASSERT_TRUE(core.load(palCartridge()));
  EXPECT_EQ(core.region(), static_cast<unsigned>(RETRO_REGION_PAL));
  core.run();
  EXPECT_EQ(seen.videoCalls, 2u);

  core.deinit();
  core.init();
  ASSERT_TRUE(core.load(ntscCartridge()));
  EXPECT_EQ(core.region(), static_cast<unsigned>(RETRO_REGION_NTSC));
  EXPECT_DOUBLE_EQ(core.avInfo().timing.fps, kNtscFps);
  core.run();
  EXPECT_EQ(seen.videoCalls, 3u);
}

}  // namespace
