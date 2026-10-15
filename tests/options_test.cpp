// The core options as a frontend lists and sets them: the whole listing in every form a frontend
// takes, the picture's aspect, the region the console runs in, and the threads the machine runs
// its sound and picture on.
//
// A thread switch is checked two ways. The process's own threads are counted, since the machine
// keeps each thread from its first call until it is destroyed; and a run that switches threads
// every few frames hands over the same pictures, sound and save as a run that never switches.

#include <libretro.h>

#include "examples/common.h"
#include "loaded_core.h"

#include <gtest/gtest.h>

#ifdef _WIN32
#include <tlhelp32.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#else
#include <filesystem>
#endif

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

using snaggletooth_libretro_test::LoadedCore;

constexpr const char* kAspectKey = "snaggletooth_aspect";
constexpr const char* kRegionKey = "snaggletooth_region";
constexpr const char* kAudioThreadKey = "snaggletooth_apu_thread";
constexpr const char* kPictureThreadKey = "snaggletooth_ppu_thread";

// Where a LoROM image's header sits, and its chipset, save-size and country bytes.
constexpr std::size_t kHeaderSite = 0x7FC0u;
constexpr std::size_t kChipsetByte = kHeaderSite + 0x16u;
constexpr std::size_t kSaveSizeByte = kHeaderSite + 0x18u;
constexpr std::size_t kCountryByte = kHeaderSite + 0x19u;
constexpr std::uint8_t kRamAndBattery = 0x02u;
constexpr std::uint8_t kTwoKilobytes = 1u;
constexpr std::uint8_t kEuropeCountry = 0x02u;

// Register values for the picture cartridge: BGMODE 5 draws in half-pixels; SETINI bit 0 asks
// for an interlaced picture and bit 2 for the 239-line one.
constexpr std::uint8_t kModeZero = 0x00u;
constexpr std::uint8_t kModeFive = 0x05u;
constexpr std::uint8_t kProgressive = 0x00u;
constexpr std::uint8_t kInterlaced = 0x01u;
constexpr std::uint8_t kTaller = 0x04u;

// The aspects of square pixels 256 across over 224 and over 239 lines, and the television's.
constexpr float kAspect224 = 256.0f / 224.0f;
constexpr float kAspect239 = 256.0f / 239.0f;
constexpr float kFourByThree = 4.0f / 3.0f;

// The console's frame rates: the master clock over the master cycles of one frame.
constexpr double kNtscFps = 236250000.0 / (11.0 * 357366.0);
constexpr double kPalFps = 21281370.0 / 425568.0;

// A cartridge with a 2 KB save that shows only its backdrop, in the screen mode and SETINI it is
// given. Its vertical-blank handler counts frames at $0010, writes the count into colour 0, so
// every frame's picture differs from the last, and writes it to the first byte of its save.
std::vector<std::uint8_t> pictureCartridge(std::uint8_t bgmode, std::uint8_t setini) {
  using snaggletooth::examples::put;
  std::vector<std::uint8_t> rom = snaggletooth::examples::imageWithNmi();
  rom[kChipsetByte] = kRamAndBattery;
  rom[kSaveSizeByte] = kTwoKilobytes;
  put(rom, 0x0000u, {
      0xA9u, bgmode,               // $8000 LDA #bgmode
      0x8Du, 0x05u, 0x21u,         // $8002 STA !$2105         BGMODE
      0xA9u, setini,               // $8005 LDA #setini
      0x8Du, 0x33u, 0x21u,         // $8007 STA !$2133         SETINI
      0xA9u, 0x0Fu,                // $800A LDA #$0F
      0x8Du, 0x00u, 0x21u,         // $800C STA !$2100         screen on, full brightness
      0xA9u, 0x80u,                // $800F LDA #$80
      0x8Du, 0x00u, 0x42u,         // $8011 STA !$4200         NMI on
      0x80u, 0xFEu,                // $8014 BRA $8014
  });
  put(rom, 0x0310u, {
      0xADu, 0x10u, 0x42u,         // $8310 LDA !$4210         acknowledge the NMI
      0xEEu, 0x10u, 0x00u,         // $8313 INC !$0010         count the frame
      0x9Cu, 0x21u, 0x21u,         // $8316 STZ !$2121         CGADD = 0
      0xADu, 0x10u, 0x00u,         // $8319 LDA !$0010
      0x8Du, 0x22u, 0x21u,         // $831C STA !$2122         colour 0, low byte
      0x8Du, 0x22u, 0x21u,         // $831F STA !$2122         colour 0, high byte
      0x8Fu, 0x00u, 0x00u, 0x70u,  // $8322 STA $700000        the save's first byte
      0x40u,                       // $8326 RTI
  });
  return rom;
}

std::vector<std::uint8_t> palPictureCartridge() {
  std::vector<std::uint8_t> rom = pictureCartridge(kModeZero, kProgressive);
  rom[kCountryByte] = kEuropeCountry;
  return rom;
}

// The block the sound cartridge uploads to the audio machine's memory at $0200: a program, the
// DSP writes it makes, a sample directory entry and one BRR block.
constexpr std::size_t kBlockInImage = 0x0100u;  // $8100
constexpr std::uint8_t kBlockBytes = 0x8Du;     // $0200-$028C

// A cartridge that plays one held note. Its reset code sends the block above to the audio
// machine through the upload protocol, one acknowledged byte at a time, and starts the program
// at $0200. The program writes the DSP registers from its table and loops: voice 0 plays a
// sawtooth from a looping BRR block under a slow attack, at different volumes left and right, so
// no two stretches of its sound repeat.
std::vector<std::uint8_t> soundCartridge() {
  using snaggletooth::examples::put;
  std::vector<std::uint8_t> rom = snaggletooth::examples::loRomImage(1);
  put(rom, 0x0000u, {
      0x78u,                       // $8000 SEI
      0x18u,                       // $8001 CLC
      0xFBu,                       // $8002 XCE
      0xE2u, 0x30u,                // $8003 SEP #$30
      0xADu, 0x40u, 0x21u,         // $8005 LDA !$2140         ready?
      0xC9u, 0xAAu,                // $8008 CMP #$AA
      0xD0u, 0xF9u,                // $800A BNE $8005
      0xADu, 0x41u, 0x21u,         // $800C LDA !$2141
      0xC9u, 0xBBu,                // $800F CMP #$BB
      0xD0u, 0xF2u,                // $8011 BNE $8005
      0xA9u, 0x00u,                // $8013 LDA #$00           destination $0200
      0x8Du, 0x42u, 0x21u,         // $8015 STA !$2142
      0xA9u, 0x02u,                // $8018 LDA #$02
      0x8Du, 0x43u, 0x21u,         // $801A STA !$2143
      0xA9u, 0x01u,                // $801D LDA #$01           a transfer, not a start
      0x8Du, 0x41u, 0x21u,         // $801F STA !$2141
      0xA9u, 0xCCu,                // $8022 LDA #$CC           the kick
      0x8Du, 0x40u, 0x21u,         // $8024 STA !$2140
      0xCDu, 0x40u, 0x21u,         // $8027 CMP !$2140         acknowledged?
      0xD0u, 0xFBu,                // $802A BNE $8027
      0xA2u, 0x00u,                // $802C LDX #$00
      0xBDu, 0x00u, 0x81u,         // $802E LDA !$8100,X       the block's next byte
      0x8Du, 0x41u, 0x21u,         // $8031 STA !$2141
      0x8Au,                       // $8034 TXA                its index
      0x8Du, 0x40u, 0x21u,         // $8035 STA !$2140
      0xCDu, 0x40u, 0x21u,         // $8038 CMP !$2140         acknowledged?
      0xD0u, 0xFBu,                // $803B BNE $8038
      0xE8u,                       // $803D INX
      0xE0u, kBlockBytes,          // $803E CPX #$8D
      0xD0u, 0xECu,                // $8040 BNE $802E
      0xA9u, 0x00u,                // $8042 LDA #$00           start at $0200
      0x8Du, 0x42u, 0x21u,         // $8044 STA !$2142
      0xA9u, 0x02u,                // $8047 LDA #$02
      0x8Du, 0x43u, 0x21u,         // $8049 STA !$2143
      0xA9u, 0x00u,                // $804C LDA #$00           zero starts the program
      0x8Du, 0x41u, 0x21u,         // $804E STA !$2141
      0xA9u, kBlockBytes + 1u,     // $8051 LDA #$8E           two past the last index
      0x8Du, 0x40u, 0x21u,         // $8053 STA !$2140
      0xCDu, 0x40u, 0x21u,         // $8056 CMP !$2140         acknowledged?
      0xD0u, 0xFBu,                // $8059 BNE $8056
      0x80u, 0xFEu,                // $805B BRA $805B
  });
  put(rom, kBlockInImage, {
      0xCDu, 0x00u,                // $0200 MOV X,#$00
      0xF5u, 0x20u, 0x02u,         // $0202 MOV A,!$0220+X     a register
      0xC4u, 0xF2u,                // $0205 MOV $F2,A
      0x3Du,                       // $0207 INC X
      0xF5u, 0x20u, 0x02u,         // $0208 MOV A,!$0220+X     its value
      0xC4u, 0xF3u,                // $020B MOV $F3,A
      0x3Du,                       // $020D INC X
      0xC8u, 0x24u,                // $020E CMP X,#$24         eighteen writes
      0xD0u, 0xF0u,                // $0210 BNE $0202
      0x2Fu, 0xFEu,                // $0212 BRA $0212
  });
  put(rom, kBlockInImage + 0x20u, {
      0x6Cu, 0x20u,                // FLG: out of reset, sound on, echo writes off
      0x0Cu, 0x7Fu,                // MVOL left
      0x1Cu, 0x40u,                // MVOL right
      0x2Cu, 0x00u,                // EVOL left
      0x3Cu, 0x00u,                // EVOL right
      0x4Du, 0x00u,                // EON: no echo
      0x3Du, 0x00u,                // NON: no noise
      0x2Du, 0x00u,                // PMON: no pitch modulation
      0x5Du, 0x02u,                // DIR: the directory at $0200
      0x00u, 0x7Fu,                // voice 0 volume left
      0x01u, 0x30u,                // voice 0 volume right
      0x02u, 0x23u,                // voice 0 pitch $0123, low byte
      0x03u, 0x01u,                // voice 0 pitch, high byte
      0x04u, 0x20u,                // voice 0 source 32: the entry at $0280
      0x05u, 0x80u,                // voice 0 ADSR on, the slowest attack
      0x06u, 0xE0u,                // voice 0 sustain at full, held
      0x5Cu, 0x00u,                // KOF: no voice released
      0x4Cu, 0x01u,                // KON: voice 0
  });
  put(rom, kBlockInImage + 0x80u, {
      0x84u, 0x02u, 0x84u, 0x02u,  // $0280 source 32 starts and loops at $0284
  });
  put(rom, kBlockInImage + 0x84u, {
      0xB3u,                       // $0284 shift 11, filter 0, loop, end
      0x01u, 0x23u, 0x45u, 0x67u,  // a rising ramp, 0 to 7
      0x89u, 0xABu, 0xCDu, 0xEFu,  // then -8 to -1
  });
  return rom;
}

// The threads this process runs, the core's among them.
std::size_t threadsRunning() {
#ifdef _WIN32
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return 0;
  std::size_t count = 0;
  THREADENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
    if (entry.th32OwnerProcessID == GetCurrentProcessId()) ++count;
  }
  CloseHandle(snapshot);
  return count;
#elif defined(__APPLE__)
  thread_act_array_t threads = nullptr;
  mach_msg_type_number_t count = 0;
  if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) return 0;
  for (mach_msg_type_number_t at = 0; at < count; ++at) {
    mach_port_deallocate(mach_task_self(), threads[at]);
  }
  vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads),
                sizeof(thread_act_t) * count);
  return count;
#else
  std::size_t count = 0;
  for ([[maybe_unused]] const auto& entry :
       std::filesystem::directory_iterator("/proc/self/task")) {
    ++count;
  }
  return count;
#endif
}

// What the test frontend saw the core do, and what it answers the core's questions with.
struct Frontend {
  unsigned optionsVersion = 2u;              // the answer to GET_CORE_OPTIONS_VERSION
  std::map<std::string, std::string> values;  // the options the player set
  bool optionsChanged = false;               // the answer to GET_VARIABLE_UPDATE, cleared once read

  // The options as each form listed them.
  std::vector<std::string> v2Categories;
  std::vector<std::string> v2Options;  // "key|category|default|value,value,..."
  std::vector<std::string> v1Options;  // "key|default|value,value,..."
  std::vector<std::string> v0Options;  // "key|Description; value|value"

  std::vector<retro_game_geometry> geometries;  // every SET_GEOMETRY
  // Each picture handed over: its shape and a hash of its colours.
  std::vector<std::string> pictures;
  std::vector<std::int16_t> samples;  // every sample, left then right

  // Sets an option the way a player does in the menu.
  void set(const std::string& key, const std::string& value) {
    values[key] = value;
    optionsChanged = true;
  }
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
      const auto held = frontend->values.find(variable->key);
      if (held == frontend->values.end()) return false;
      variable->value = held->second.c_str();
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

// XRGB8888's top byte is not part of the colour, so the hash leaves it out.
void videoRefresh(const void* data, unsigned width, unsigned height, size_t pitch) {
  std::uint64_t hash = 14695981039346656037ull;
  if (data != nullptr) {
    for (unsigned row = 0; row < height; ++row) {
      const auto* pixel = reinterpret_cast<const std::uint32_t*>(
          static_cast<const std::uint8_t*>(data) + row * pitch);
      for (unsigned x = 0; x < width; ++x) {
        hash = (hash ^ (pixel[x] & 0x00FFFFFFu)) * 1099511628211ull;
      }
    }
  }
  frontend->pictures.push_back(std::to_string(width) + 'x' + std::to_string(height) + ' ' +
                               std::to_string(hash));
}

void audioSample(std::int16_t left, std::int16_t right) {
  frontend->samples.push_back(left);
  frontend->samples.push_back(right);
}

size_t audioSampleBatch(const std::int16_t* data, size_t frames) {
  frontend->samples.insert(frontend->samples.end(), data, data + frames * 2u);
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
  [[nodiscard]] unsigned region() const {
    return module_.entry<unsigned()>("retro_get_region")();
  }
  [[nodiscard]] std::uint8_t* save() const {
    return static_cast<std::uint8_t*>(
        module_.entry<void*(unsigned)>("retro_get_memory_data")(RETRO_MEMORY_SAVE_RAM));
  }

 private:
  LoadedCore module_;
};

// The thread options a run of the switching cases moves through, a set every kSwitchEvery runs,
// visiting each of the four combinations and coming back to the default.
constexpr std::size_t kSwitchEvery = 7u;
const std::vector<std::pair<const char*, const char*>> kThreadChoices = {
    {"enabled", "enabled"},   {"enabled", "disabled"}, {"disabled", "disabled"},
    {"disabled", "enabled"},  {"enabled", "enabled"},  {"disabled", "disabled"},
    {"enabled", "disabled"},  {"disabled", "enabled"},
};

// A cartridge's runs, with the thread options moved through kThreadChoices when `switching`.
Frontend runsOf(const std::vector<std::uint8_t>& rom, bool switching) {
  Frontend seen;
  Core core(seen);
  if (!core.loaded() || !core.load(rom)) return seen;
  for (const auto& [audio, picture] : kThreadChoices) {
    if (switching) {
      seen.set(kAudioThreadKey, audio);
      seen.set(kPictureThreadKey, picture);
    }
    core.run(kSwitchEvery);
  }
  return seen;
}

TEST(Options, EveryOptionIsListedInCategories) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  EXPECT_EQ(seen.v2Categories, (std::vector<std::string>{"video|Video", "system|System"}));
  EXPECT_EQ(seen.v2Options, (std::vector<std::string>{
                                "snaggletooth_interlace|video|weave|weave,bob,off",
                                "snaggletooth_aspect|video|square|square,4:3",
                                "snaggletooth_region|system|auto|auto,ntsc,pal",
                                "snaggletooth_apu_thread|system|disabled|disabled,enabled",
                                "snaggletooth_ppu_thread|system|enabled|enabled,disabled",
                            }));
  EXPECT_TRUE(seen.v1Options.empty());
  EXPECT_TRUE(seen.v0Options.empty());
}

TEST(Options, AFirstVersionFrontendIsListedEveryPlainOption) {
  Frontend seen;
  seen.optionsVersion = 1u;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  EXPECT_EQ(seen.v1Options, (std::vector<std::string>{
                                "snaggletooth_interlace|weave|weave,bob,off",
                                "snaggletooth_aspect|square|square,4:3",
                                "snaggletooth_region|auto|auto,ntsc,pal",
                                "snaggletooth_apu_thread|disabled|disabled,enabled",
                                "snaggletooth_ppu_thread|enabled|enabled,disabled",
                            }));
  EXPECT_TRUE(seen.v2Options.empty());
}

TEST(Options, AFrontendWithoutOptionVersionsIsListedEveryVariable) {
  Frontend seen;
  seen.optionsVersion = 0u;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  EXPECT_EQ(seen.v0Options, (std::vector<std::string>{
                                "snaggletooth_interlace|Interlaced Picture; weave|bob|off",
                                "snaggletooth_aspect|Aspect Ratio; square|4:3",
                                "snaggletooth_region|Region; auto|ntsc|pal",
                                "snaggletooth_apu_thread|Audio Thread; disabled|enabled",
                                "snaggletooth_ppu_thread|Picture Thread; enabled|disabled",
                            }));
  EXPECT_TRUE(seen.v2Options.empty());
}

TEST(Options, TheAspectIsSquarePixelsByDefault) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  EXPECT_FLOAT_EQ(core.avInfo().geometry.aspect_ratio, kAspect224);
  ASSERT_TRUE(core.load(pictureCartridge(kModeZero, kTaller)));
  EXPECT_FLOAT_EQ(core.avInfo().geometry.aspect_ratio, kAspect224);
  core.run(4u);
  ASSERT_EQ(seen.geometries.size(), 1u);
  EXPECT_FLOAT_EQ(seen.geometries[0].aspect_ratio, kAspect239);
}

TEST(Options, FourByThreeIsReportedFromTheStart) {
  Frontend seen;
  seen.values[kAspectKey] = "4:3";
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  EXPECT_FLOAT_EQ(core.avInfo().geometry.aspect_ratio, kFourByThree);
  ASSERT_TRUE(core.load(pictureCartridge(kModeZero, kProgressive)));
  EXPECT_FLOAT_EQ(core.avInfo().geometry.aspect_ratio, kFourByThree);
  core.run(4u);
  EXPECT_TRUE(seen.geometries.empty());
}

TEST(Options, FourByThreeHoldsForEveryShape) {
  for (const std::uint8_t setini :
       {kTaller, kInterlaced, static_cast<std::uint8_t>(kInterlaced | kTaller)}) {
    Frontend seen;
    seen.values[kAspectKey] = "4:3";
    Core core(seen);
    ASSERT_TRUE(core.loaded());
    ASSERT_TRUE(core.load(pictureCartridge(kModeFive, setini)));
    core.run(4u);
    ASSERT_FALSE(seen.geometries.empty()) << "SETINI " << int{setini};
    EXPECT_EQ(seen.geometries.back().base_width, 512u) << "SETINI " << int{setini};
    EXPECT_FLOAT_EQ(seen.geometries.back().aspect_ratio, kFourByThree) << "SETINI " << int{setini};
    EXPECT_FLOAT_EQ(core.avInfo().geometry.aspect_ratio, kFourByThree) << "SETINI " << int{setini};
  }
}

TEST(Options, AnAspectChangeIsSentOnTheNextRun) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(pictureCartridge(kModeZero, kProgressive)));
  core.run(2u);
  ASSERT_TRUE(seen.geometries.empty());
  seen.set(kAspectKey, "4:3");
  core.run();
  ASSERT_EQ(seen.geometries.size(), 1u);
  EXPECT_EQ(seen.geometries[0].base_width, 256u);
  EXPECT_EQ(seen.geometries[0].base_height, 224u);
  EXPECT_FLOAT_EQ(seen.geometries[0].aspect_ratio, kFourByThree);
  EXPECT_FLOAT_EQ(core.avInfo().geometry.aspect_ratio, kFourByThree);
  seen.set(kAspectKey, "square");
  core.run();
  ASSERT_EQ(seen.geometries.size(), 2u);
  EXPECT_FLOAT_EQ(seen.geometries[1].aspect_ratio, kAspect224);
  EXPECT_FLOAT_EQ(core.avInfo().geometry.aspect_ratio, kAspect224);
}

TEST(Options, AutoRunsTheRegionTheHeaderNames) {
  Frontend seen;
  seen.values[kRegionKey] = "auto";
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(pictureCartridge(kModeZero, kProgressive)));
  EXPECT_EQ(core.region(), static_cast<unsigned>(RETRO_REGION_NTSC));
  EXPECT_DOUBLE_EQ(core.avInfo().timing.fps, kNtscFps);
  core.unload();
  ASSERT_TRUE(core.load(palPictureCartridge()));
  EXPECT_EQ(core.region(), static_cast<unsigned>(RETRO_REGION_PAL));
  EXPECT_DOUBLE_EQ(core.avInfo().timing.fps, kPalFps);
}

TEST(Options, PalRunsAnNtscCartridgeAt50Hz) {
  Frontend seen;
  seen.values[kRegionKey] = "pal";
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(pictureCartridge(kModeZero, kProgressive)));
  EXPECT_EQ(core.region(), static_cast<unsigned>(RETRO_REGION_PAL));
  EXPECT_DOUBLE_EQ(core.avInfo().timing.fps, kPalFps);
}

TEST(Options, NtscRunsAPalCartridgeAt60Hz) {
  Frontend seen;
  seen.values[kRegionKey] = "ntsc";
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(palPictureCartridge()));
  EXPECT_EQ(core.region(), static_cast<unsigned>(RETRO_REGION_NTSC));
  EXPECT_DOUBLE_EQ(core.avInfo().timing.fps, kNtscFps);
}

TEST(Options, ARegionChangeTakesEffectAtTheNextLoad) {
  const std::vector<std::uint8_t> rom = pictureCartridge(kModeZero, kProgressive);
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(rom));
  core.run(2u);
  seen.set(kRegionKey, "pal");
  core.run(2u);
  EXPECT_EQ(core.region(), static_cast<unsigned>(RETRO_REGION_NTSC));
  EXPECT_DOUBLE_EQ(core.avInfo().timing.fps, kNtscFps);
  core.unload();
  ASSERT_TRUE(core.load(rom));
  EXPECT_EQ(core.region(), static_cast<unsigned>(RETRO_REGION_PAL));
  EXPECT_DOUBLE_EQ(core.avInfo().timing.fps, kPalFps);
}

TEST(Options, TheMachineRunsOnTheThreadsTheOptionsChoose) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  const std::size_t before = threadsRunning();
  ASSERT_GT(before, 0u);
  ASSERT_TRUE(core.load(soundCartridge()));
  core.run(2u);
  // The picture's thread by default, and the sound's on the frontend's.
  EXPECT_EQ(threadsRunning(), before + 1u);
  seen.set(kAudioThreadKey, "enabled");
  core.run(2u);
  EXPECT_EQ(threadsRunning(), before + 2u);
  seen.set(kPictureThreadKey, "disabled");
  core.run(2u);
  EXPECT_EQ(threadsRunning(), before + 1u);
  seen.set(kAudioThreadKey, "disabled");
  core.run(2u);
  EXPECT_EQ(threadsRunning(), before);
  seen.set(kPictureThreadKey, "enabled");
  core.run(2u);
  EXPECT_EQ(threadsRunning(), before + 1u);
}

TEST(Options, ThreadOptionsChosenBeforeLoadBuildTheMachineOnThem) {
  Frontend seen;
  seen.values[kAudioThreadKey] = "enabled";
  seen.values[kPictureThreadKey] = "enabled";
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  const std::size_t before = threadsRunning();
  ASSERT_TRUE(core.load(soundCartridge()));
  core.run(2u);
  EXPECT_EQ(threadsRunning(), before + 2u);
  core.unload();
  seen.values[kAudioThreadKey] = "disabled";
  seen.values[kPictureThreadKey] = "disabled";
  ASSERT_TRUE(core.load(soundCartridge()));
  core.run(2u);
  EXPECT_EQ(threadsRunning(), before);
}

TEST(Options, SwitchingThreadsKeepsThePicture) {
  // Half-pixel, interlaced and 239-line, so every part of a frame's shape crosses the switches.
  const std::vector<std::uint8_t> rom = pictureCartridge(kModeFive, kInterlaced | kTaller);
  const Frontend steady = runsOf(rom, false);
  const Frontend switching = runsOf(rom, true);
  ASSERT_EQ(steady.pictures.size(), kThreadChoices.size() * kSwitchEvery);
  EXPECT_EQ(switching.pictures, steady.pictures);
  EXPECT_EQ(switching.samples, steady.samples);
}

TEST(Options, SwitchingThreadsKeepsTheSound) {
  const std::vector<std::uint8_t> rom = soundCartridge();
  const Frontend steady = runsOf(rom, false);
  const Frontend switching = runsOf(rom, true);
  // The note is playing, so a lost or repeated stretch of it would show.
  std::size_t sounding = 0;
  for (const std::int16_t sample : steady.samples) sounding += sample != 0 ? 1u : 0u;
  ASSERT_GT(sounding, steady.samples.size() / 2u);
  EXPECT_EQ(switching.samples, steady.samples);
  EXPECT_EQ(switching.pictures, steady.pictures);
}

TEST(Options, TheSaveBufferStaysPutAcrossASwitch) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(pictureCartridge(kModeZero, kProgressive)));
  core.run(10u);
  std::uint8_t* const save = core.save();
  ASSERT_NE(save, nullptr);
  const std::uint8_t count = save[0];
  ASSERT_NE(count, 0u);
  seen.set(kAudioThreadKey, "enabled");
  seen.set(kPictureThreadKey, "disabled");
  core.run();
  EXPECT_EQ(core.save(), save);
  EXPECT_EQ(save[0], static_cast<std::uint8_t>(count + 1u));
}

}  // namespace
