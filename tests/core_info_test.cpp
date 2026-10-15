// The core's info file, snaggletooth_libretro.info: the description a frontend reads before it
// loads the core. Each value the core itself can show is checked against the built core — its
// system info, what it sends the frontend, what a loaded cartridge offers — so the file and the
// core say the same thing; the license and author against LICENSE; and the file's form against
// the keys libretro's template for info files defines.

#include <libretro.h>

#include "examples/waiting/waiting.h"
#include "loaded_core.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace {

using snaggletooth_libretro_test::LoadedCore;

// Every key libretro's template for core info files (00_example_libretro.info) defines, but the
// firmware entries: the core loads no file other than the cartridge.
const std::set<std::string> kTemplateKeys{
    "display_name",      "categories",         "authors",
    "corename",          "supported_extensions", "license",
    "permissions",       "display_version",    "manufacturer",
    "systemname",        "systemid",           "notes",
    "savestate",         "savestate_features", "cheats",
    "input_descriptors", "memory_descriptors", "libretro_saves",
    "core_options",      "core_options_version", "load_subsystem",
    "supports_no_game",  "single_purpose",     "database",
    "database_match_archive_member", "hw_render", "required_hw_api",
    "needs_fullpath",    "disk_control",       "is_experimental",
    "description",
};

constexpr std::size_t kDescriptionLimit = 512u;

// The header's chipset and save-size bytes in a LoROM image, for a cartridge with a save.
constexpr std::size_t kLoRomSite = 0x7FC0u;
constexpr std::size_t kChipsetByte = 0x16u;
constexpr std::size_t kSaveSizeByte = 0x18u;
constexpr std::uint8_t kRamAndBattery = 0x02u;
constexpr std::uint8_t kEightKilobytes = 3u;

// The file's text with its line ends as `\n`, whichever line ends the checkout gave it.
std::string readFile(const char* path) {
  std::ifstream file(path, std::ios::binary);
  std::ostringstream read;
  read << file.rdbuf();
  std::string text = read.str();
  std::erase(text, '\r');
  return text;
}

// The file as libretro writes one: `key = "value"` on each line, blank lines and `#` comments
// between them. A line in any other form is listed in `malformed`, a key met twice in `repeated`.
struct InfoFile {
  std::map<std::string, std::string> values;
  std::vector<std::string> malformed;
  std::vector<std::string> repeated;

  [[nodiscard]] std::string operator[](const std::string& key) const {
    const auto held = values.find(key);
    return held == values.end() ? std::string() : held->second;
  }
};

InfoFile readInfo() {
  InfoFile info;
  std::istringstream lines(readFile(SNAGGLETOOTH_LIBRETRO_INFO_PATH));
  std::string line;
  while (std::getline(lines, line)) {
    if (line.empty() || line.front() == '#') continue;
    const std::size_t equals = line.find(" = \"");
    const bool keyed = equals != std::string::npos && equals > 0u &&
                       line.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") == equals;
    const std::size_t open = equals + 4u;
    if (!keyed || line.size() < open + 1u || line.back() != '"' ||
        line.find('"', open) != line.size() - 1u) {
      info.malformed.push_back(line);
      continue;
    }
    const std::string key = line.substr(0, equals);
    if (info.values.contains(key)) info.repeated.push_back(key);
    info.values[key] = line.substr(open, line.size() - 1u - open);
  }
  return info;
}

// What the test frontend saw the core do, and what it answers the core's questions with.
struct Frontend {
  unsigned optionsVersion = 2u;
  std::optional<bool> supportsNoGame;
  std::set<unsigned> commands;  // every environment command the core sent
};

Frontend* frontend = nullptr;

bool environment(unsigned command, void* data) {
  frontend->commands.insert(command);
  switch (command) {
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
      frontend->supportsNoGame = *static_cast<const bool*>(data);
      return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
      *static_cast<unsigned*>(data) = frontend->optionsVersion;
      return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
      return true;
    default:
      return false;
  }
}

void videoRefresh(const void* data, unsigned width, unsigned height, size_t pitch) {
  (void)data;
  (void)width;
  (void)height;
  (void)pitch;
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
  [[nodiscard]] const LoadedCore& module() const noexcept { return module_; }

  [[nodiscard]] retro_system_info systemInfo() const {
    retro_system_info info{};
    module_.entry<void(retro_system_info*)>("retro_get_system_info")(&info);
    return info;
  }

  bool load(const std::vector<std::uint8_t>& image) {
    const retro_game_info game = gameOf(image);
    return module_.entry<bool(const retro_game_info*)>("retro_load_game")(&game);
  }
  bool loadSpecial(const std::vector<std::uint8_t>& image) {
    const retro_game_info game = gameOf(image);
    return module_.entry<bool(unsigned, const retro_game_info*, size_t)>(
        "retro_load_game_special")(0u, &game, 1u);
  }
  void run() { module_.entry<void()>("retro_run")(); }

  [[nodiscard]] std::size_t serializeSize() const {
    return module_.entry<size_t()>("retro_serialize_size")();
  }
  [[nodiscard]] std::size_t memorySize(unsigned id) const {
    return module_.entry<size_t(unsigned)>("retro_get_memory_size")(id);
  }
  [[nodiscard]] std::span<const std::uint8_t> workRam() const {
    return {static_cast<const std::uint8_t*>(
                module_.entry<void*(unsigned)>("retro_get_memory_data")(RETRO_MEMORY_SYSTEM_RAM)),
            memorySize(RETRO_MEMORY_SYSTEM_RAM)};
  }
  void cheat(unsigned index, bool enabled, const char* code) {
    module_.entry<void(unsigned, bool, const char*)>("retro_cheat_set")(index, enabled, code);
  }

 private:
  static retro_game_info gameOf(const std::vector<std::uint8_t>& image) {
    return retro_game_info{.path = nullptr, .data = image.data(), .size = image.size(),
                           .meta = nullptr};
  }

  LoadedCore module_;
};

// "true" or "false", as the file writes a flag.
std::string flag(bool value) { return value ? "true" : "false"; }

// The file reads as libretro writes one: every line a quoted `key = "value"`, each key once and
// one the template defines; the display name opens with the manufacturer and closes with the
// core's name, and the description is one line of at most 512 characters.
TEST(CoreInfo, FileIsInLibretrosForm) {
  const InfoFile info = readInfo();
  EXPECT_TRUE(info.malformed.empty()) << "first: " << info.malformed.front();
  EXPECT_TRUE(info.repeated.empty()) << "first: " << info.repeated.front();
  for (const auto& [key, value] : info.values) {
    EXPECT_TRUE(kTemplateKeys.contains(key)) << key;
  }
  const std::string& shown = info["display_name"];
  EXPECT_EQ(shown.rfind(info["manufacturer"] + " - ", 0), 0u) << shown;
  const std::string core = " (" + info["corename"] + ")";
  EXPECT_TRUE(shown.size() > core.size() &&
              shown.compare(shown.size() - core.size(), core.size(), core) == 0)
      << shown;
  EXPECT_FALSE(info["description"].empty());
  EXPECT_LE(info["description"].size(), kDescriptionLimit);
}

// The name, the extensions, the full-path flag and the version are the ones the core reports.
TEST(CoreInfo, NamesMatchTheCoresSystemInfo) {
  const InfoFile info = readInfo();
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  const retro_system_info system = core.systemInfo();
  EXPECT_EQ(info["corename"], system.library_name);
  EXPECT_EQ(info["supported_extensions"], system.valid_extensions);
  EXPECT_EQ(info["needs_fullpath"], flag(system.need_fullpath));
  EXPECT_EQ(info["display_version"], SNAGGLETOOTH_LIBRETRO_VERSION);
  EXPECT_EQ(std::string(system.library_version).rfind(info["display_version"] + " (", 0), 0u)
      << system.library_version;
}

// What the core tells the frontend before a cartridge loads: whether it runs with no content,
// and its options, listed in the version the file names to a frontend that takes it.
TEST(CoreInfo, FlagsMatchWhatTheCoreSendsAtStart) {
  const InfoFile info = readInfo();
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(seen.supportsNoGame.has_value());
  EXPECT_EQ(info["supports_no_game"], flag(*seen.supportsNoGame));
  const bool listsOptionsV2 = seen.commands.contains(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2);
  EXPECT_EQ(info["core_options"], flag(listsOptionsV2));
  EXPECT_EQ(info["core_options_version"], listsOptionsV2 ? "2.0" : "");
}

// What a loaded cartridge offers: names for every button, a map of the bus, the save through
// libretro's save memory, pictures drawn in software, and no disk control.
TEST(CoreInfo, FlagsMatchWhatALoadSends) {
  const InfoFile info = readInfo();
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  std::vector<std::uint8_t> rom = snaggletooth::examples::waitingImage();
  rom[kLoRomSite + kChipsetByte] = kRamAndBattery;
  rom[kLoRomSite + kSaveSizeByte] = kEightKilobytes;
  ASSERT_TRUE(core.load(rom));
  EXPECT_EQ(info["input_descriptors"],
            flag(seen.commands.contains(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS)));
  EXPECT_EQ(info["memory_descriptors"],
            flag(seen.commands.contains(RETRO_ENVIRONMENT_SET_MEMORY_MAPS)));
  EXPECT_EQ(info["libretro_saves"], flag(core.memorySize(RETRO_MEMORY_SAVE_RAM) > 0u));
  EXPECT_EQ(info["hw_render"], flag(seen.commands.contains(RETRO_ENVIRONMENT_SET_HW_RENDER)));
  EXPECT_EQ(info["disk_control"],
            flag(seen.commands.contains(RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE) ||
                 seen.commands.contains(RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE)));
}

// The save-state flag is whether a loaded cartridge has state bytes to save.
TEST(CoreInfo, SaveStateFlagMatchesTheStateSize) {
  const InfoFile info = readInfo();
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(snaggletooth::examples::waitingImage()));
  EXPECT_EQ(info["savestate"], flag(core.serializeSize() > 0u));
}

// The cheat flag is whether a code the frontend sets reaches the console: a raw code on a
// work-RAM byte the cartridge leaves alone is in the work RAM after a frame.
TEST(CoreInfo, CheatFlagMatchesACode) {
  const InfoFile info = readInfo();
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(snaggletooth::examples::waitingImage()));
  core.cheat(0u, true, "7F8000:5A");
  core.run();
  constexpr std::size_t kCheatedByte = 0x18000u;  // $7F8000
  EXPECT_EQ(info["cheats"], flag(core.workRam()[kCheatedByte] == 0x5Au));
}

// The subsystem flag is whether the core loads content through retro_load_game_special.
TEST(CoreInfo, SubsystemFlagMatchesTheSpecialLoad) {
  const InfoFile info = readInfo();
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  EXPECT_EQ(info["load_subsystem"], flag(core.loadSpecial(snaggletooth::examples::waitingImage())));
}

// The license and the author are the ones LICENSE names.
TEST(CoreInfo, LicenseMatchesTheLicenseFile) {
  const InfoFile info = readInfo();
  const std::string license = readFile(SNAGGLETOOTH_LIBRETRO_LICENSE_PATH);
  EXPECT_EQ(license.rfind(info["license"] + " License\n", 0), 0u);
  EXPECT_FALSE(info["authors"].empty());
  EXPECT_NE(license.find("Copyright (c) 2026 " + info["authors"] + "\n"), std::string::npos);
}

}  // namespace
