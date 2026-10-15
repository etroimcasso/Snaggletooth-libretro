// The console's work RAM as a frontend keeps it: the RETRO_MEMORY_SYSTEM_RAM buffer, 128 KB,
// holding the work RAM as each frame leaves it and taking what the frontend writes into it
// before the next frame; and the memory map the frontend is told, placing that buffer and the
// save at the console's own bus addresses.
//
// What the game sees is read back through the work RAM itself. The suite's cartridge counts
// frames into it, adding one to a 16-bit count at $7E:0000-0001 in every vertical blank, and
// copies the byte at direct-page $10 — bank $00's mirror of $7E:0010 — to $7E:0002, so a byte
// the frontend writes shows up through the console's own mirror.
//
// The map is checked against the machine: every bus address resolved through the descriptors,
// as libretro.h describes resolving one, must land on the byte Snes::physical names for it, or
// on nothing where the machine names no memory the map carries.

#include <libretro.h>

#include "examples/common.h"
#include "loaded_core.h"

#include "snaggletooth/snes/snes.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using snaggletooth_libretro_test::LoadedCore;

constexpr std::size_t kWorkRamBytes = 131072u;
constexpr std::uint32_t kBusAddresses = 0x1000000u;

// The header's chipset and save-size bytes, at the same offsets from every map's site.
constexpr std::size_t kChipsetByte = 0x16u;
constexpr std::size_t kSaveSizeByte = 0x18u;
constexpr std::size_t kMapModeByte = 0x15u;
constexpr std::uint8_t kRamAndBattery = 0x02u;
constexpr std::uint8_t kEightKilobytes = 3u;  // the code: 1 KB shifted left by it
constexpr std::uint8_t kTwoKilobytes = 1u;
constexpr std::uint8_t kSixtyFourKilobytes = 6u;
constexpr std::size_t kLoRomSite = 0x7FC0u;
constexpr std::size_t kHiRomSite = 0xFFC0u;
constexpr std::size_t kExHiRomSite = 0x40FFC0u;
constexpr std::size_t kHeaderBytes = 0x40u;

// Where the cartridge keeps what it writes in work RAM, and the byte it echoes.
constexpr std::size_t kCountLow = 0u;
constexpr std::size_t kCountHigh = 1u;
constexpr std::size_t kEcho = 2u;
constexpr std::size_t kEchoed = 0x10u;

// The thread option a case switches to move the machine onto a new one.
constexpr const char* kAudioThreadKey = "snaggletooth_apu_thread";

// A LoROM cartridge that turns the NMI on and waits; its vertical-blank handler adds one to the
// 16-bit count at $7E:0000-0001, then copies direct-page $10 to $7E:0002.
std::vector<std::uint8_t> workRamCartridge() {
  using snaggletooth::examples::put;
  std::vector<std::uint8_t> rom = snaggletooth::examples::imageWithNmi();
  put(rom, 0x0000u, {
      0xA9u, 0x80u,                // $8000 LDA #$80
      0x8Du, 0x00u, 0x42u,         // $8002 STA !$4200         NMI on, auto-read off
      0x80u, 0xFEu,                // $8005 BRA $8005
  });
  put(rom, 0x0310u, {
      0xADu, 0x10u, 0x42u,         // $8310 LDA !$4210         acknowledge the NMI
      0xAFu, 0x00u, 0x00u, 0x7Eu,  // $8313 LDA $7E0000        the count's low byte
      0x18u,                       // $8317 CLC
      0x69u, 0x01u,                // $8318 ADC #$01
      0x8Fu, 0x00u, 0x00u, 0x7Eu,  // $831A STA $7E0000
      0xAFu, 0x01u, 0x00u, 0x7Eu,  // $831E LDA $7E0001        the high byte, with the carry
      0x69u, 0x00u,                // $8322 ADC #$00
      0x8Fu, 0x01u, 0x00u, 0x7Eu,  // $8324 STA $7E0001
      0xA5u, 0x10u,                // $8328 LDA $10            through bank $00's mirror
      0x8Fu, 0x02u, 0x00u, 0x7Eu,  // $832A STA $7E0002
      0x40u,                       // $832E RTI
  });
  return rom;
}

// The same program with a header declaring a save of 1 KB << `sizeCode`.
std::vector<std::uint8_t> loRomWithSave(std::uint8_t sizeCode) {
  std::vector<std::uint8_t> rom = workRamCartridge();
  rom[kLoRomSite + kChipsetByte] = kRamAndBattery;
  rom[kLoRomSite + kSaveSizeByte] = sizeCode;
  return rom;
}

// A HiROM image of one bank with a save of 1 KB << `sizeCode`.
std::vector<std::uint8_t> hiRomWithSave(std::uint8_t sizeCode) {
  std::vector<std::uint8_t> rom = snaggletooth::examples::hiRomImage(1u);
  rom[kHiRomSite + kChipsetByte] = kRamAndBattery;
  rom[kHiRomSite + kSaveSizeByte] = sizeCode;
  return rom;
}

// An ExHiROM image: the HiROM header moved to $40FFC0, past the first 4 MB, with the map-mode
// byte's low nibble naming ExHiROM, and the HiROM site blanked so only one site reads as a
// header.
std::vector<std::uint8_t> exHiRomWithSave(std::uint8_t sizeCode) {
  std::vector<std::uint8_t> rom = snaggletooth::examples::hiRomImage(65u);
  std::copy_n(rom.begin() + kHiRomSite, kHeaderBytes, rom.begin() + kExHiRomSite);
  std::fill_n(rom.begin() + kHiRomSite, kHeaderBytes, std::uint8_t{0});
  rom[kExHiRomSite + kMapModeByte] = 0x25u;
  rom[kExHiRomSite + kChipsetByte] = kRamAndBattery;
  rom[kExHiRomSite + kSaveSizeByte] = sizeCode;
  return rom;
}

// What the test frontend saw the core do, and what it answers the core's questions with.
struct Frontend {
  bool takesMemoryMaps = true;
  std::vector<std::vector<retro_memory_descriptor>> maps;  // each SET_MEMORY_MAPS, copied
  std::map<std::string, std::string> values;               // the options the player set
  bool optionsChanged = false;

  void set(const std::string& key, const std::string& value) {
    values[key] = value;
    optionsChanged = true;
  }
};

Frontend* frontend = nullptr;

bool environment(unsigned command, void* data) {
  switch (command) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
      return true;
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS: {
      if (!frontend->takesMemoryMaps) return false;
      const auto* map = static_cast<const retro_memory_map*>(data);
      frontend->maps.emplace_back(map->descriptors, map->descriptors + map->num_descriptors);
      return true;
    }
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

  bool load(const std::vector<std::uint8_t>& image) {
    const retro_game_info game{.path = nullptr, .data = image.data(), .size = image.size(),
                               .meta = nullptr};
    return module_.entry<bool(const retro_game_info*)>("retro_load_game")(&game);
  }
  void unload() { module_.entry<void()>("retro_unload_game")(); }
  void reset() { module_.entry<void()>("retro_reset")(); }
  void run(std::size_t times = 1u) {
    for (std::size_t at = 0; at < times; ++at) module_.entry<void()>("retro_run")();
  }

  [[nodiscard]] void* memoryData(unsigned id) const {
    return module_.entry<void*(unsigned)>("retro_get_memory_data")(id);
  }
  [[nodiscard]] std::size_t memorySize(unsigned id) const {
    return module_.entry<size_t(unsigned)>("retro_get_memory_size")(id);
  }

  // The work RAM as the frontend reaches it.
  [[nodiscard]] std::span<std::uint8_t> workRam() const {
    return {static_cast<std::uint8_t*>(memoryData(RETRO_MEMORY_SYSTEM_RAM)),
            memorySize(RETRO_MEMORY_SYSTEM_RAM)};
  }

 private:
  LoadedCore module_;
};

std::uint16_t countIn(std::span<const std::uint8_t> ram) {
  return static_cast<std::uint16_t>(ram[kCountLow] | (ram[kCountHigh] << 8));
}

void setCount(std::span<std::uint8_t> ram, std::uint16_t count) {
  ram[kCountLow] = static_cast<std::uint8_t>(count & 0xFFu);
  ram[kCountHigh] = static_cast<std::uint8_t>(count >> 8);
}

// Where a bus address lands in the host's memory under a list of descriptors, resolved as
// libretro.h describes: the first descriptor whose selected bits match wins; the address's
// offset into it has its high bits dropped until it is within the length; the descriptor's own
// offset is added. A descriptor with no pointer, or no descriptor at all, is nothing.
struct HostByte {
  const void* buffer;
  std::size_t offset;
  [[nodiscard]] bool operator==(const HostByte&) const noexcept = default;
};

std::optional<HostByte> resolve(std::span<const retro_memory_descriptor> descriptors,
                                std::uint32_t address) {
  for (const retro_memory_descriptor& d : descriptors) {
    const bool hit = d.select != 0u ? (address & d.select) == d.start
                                    : address >= d.start && address < d.start + d.len;
    if (!hit) continue;
    if (d.ptr == nullptr) return std::nullopt;
    std::size_t offset = (address & ~d.disconnect) - d.start;
    while (d.len != 0u && offset >= d.len) offset &= ~std::bit_floor(offset);
    return HostByte{.buffer = d.ptr, .offset = d.offset + offset};
  }
  return std::nullopt;
}

// Where the machine says the address's byte is, in terms of the buffers the frontend holds.
std::optional<HostByte> machineByte(const snaggletooth::Snes& machine, std::uint32_t address,
                                    const void* workRam, const void* save) {
  const snaggletooth::Snes::Physical byte = machine.physical(address);
  switch (byte.space) {
    case snaggletooth::Snes::Space::WorkRam: return HostByte{.buffer = workRam, .offset = byte.index};
    case snaggletooth::Snes::Space::SaveRam: return HostByte{.buffer = save, .offset = byte.index};
    default: return std::nullopt;
  }
}

// Loads `rom`, then checks every bus address of the map the core sent against a machine built
// on the same cartridge. `windows` is how many save windows the map should carry.
void expectMapMatchesTheMachine(const std::vector<std::uint8_t>& rom, std::size_t windows,
                                const char* name) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(rom)) << name;
  ASSERT_EQ(seen.maps.size(), 1u) << name;
  const std::span<const retro_memory_descriptor> map = seen.maps.front();
  const void* const workRam = core.memoryData(RETRO_MEMORY_SYSTEM_RAM);
  const void* const save = core.memoryData(RETRO_MEMORY_SAVE_RAM);
  ASSERT_EQ(map.size(), 3u + windows + 1u) << name;

  const snaggletooth::Snes machine(snaggletooth::SnesConfig{.rom = rom});
  std::size_t wrong = 0;
  std::uint32_t firstWrong = 0;
  for (std::uint32_t address = 0; address < kBusAddresses; ++address) {
    if (resolve(map, address) != machineByte(machine, address, workRam, save)) {
      if (wrong == 0) firstWrong = address;
      ++wrong;
    }
  }
  EXPECT_EQ(wrong, 0u) << name << ": first at $" << std::hex << firstWrong;
}

// A core with no cartridge loaded offers no work RAM.
TEST(WorkRam, NoCartridgeOffersNoWorkRam) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  EXPECT_EQ(core.memoryData(RETRO_MEMORY_SYSTEM_RAM), nullptr);
  EXPECT_EQ(core.memorySize(RETRO_MEMORY_SYSTEM_RAM), 0u);
  ASSERT_TRUE(core.load(workRamCartridge()));
  core.unload();
  EXPECT_EQ(core.memoryData(RETRO_MEMORY_SYSTEM_RAM), nullptr);
  EXPECT_EQ(core.memorySize(RETRO_MEMORY_SYSTEM_RAM), 0u);
}

// The buffer is the console's 128 KB and starts as the machine's work RAM, all zero.
TEST(WorkRam, WorkRamIsTheConsoles128Kilobytes) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(workRamCartridge()));
  const std::span<std::uint8_t> ram = core.workRam();
  ASSERT_NE(ram.data(), nullptr);
  EXPECT_EQ(ram.size(), kWorkRamBytes);
  std::size_t nonzero = 0;
  for (const std::uint8_t byte : ram) nonzero += byte != 0u ? 1u : 0u;
  EXPECT_EQ(nonzero, 0u);
}

// Each run's write is in the buffer when the run returns: the count after N runs is N.
TEST(WorkRam, EachFramesWriteIsInTheBufferAfterItsRun) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(workRamCartridge()));
  for (std::uint16_t runs = 1u; runs <= 300u; ++runs) {
    core.run();
    ASSERT_EQ(countIn(core.workRam()), runs);
  }
}

// A byte the frontend writes is the game's from the next frame, read through the console's own
// mirror of the work RAM in bank $00, and every byte the game does not write stays as written.
TEST(WorkRam, WriteReachesTheGameThroughTheMirror) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(workRamCartridge()));
  core.run(3u);
  ASSERT_EQ(core.workRam()[kEcho], 0u);
  core.workRam()[kEchoed] = 0x5Au;
  core.workRam()[0x1FFFFu] = 0xA5u;
  core.workRam()[0x12345u] = 0x3Cu;
  core.run();
  EXPECT_EQ(core.workRam()[kEcho], 0x5Au);
  EXPECT_EQ(core.workRam()[kEchoed], 0x5Au);
  EXPECT_EQ(core.workRam()[0x1FFFFu], 0xA5u);
  EXPECT_EQ(core.workRam()[0x12345u], 0x3Cu);
  core.workRam()[kEchoed] = 0x00u;
  core.run();
  EXPECT_EQ(core.workRam()[kEcho], 0x00u);
}

// A write the frontend makes to the count between two frames is what the game counts on from.
TEST(WorkRam, WriteBetweenFramesContinuesTheCount) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(workRamCartridge()));
  core.run(5u);
  ASSERT_EQ(countIn(core.workRam()), 5u);
  setCount(core.workRam(), 0x12FFu);
  core.run();
  EXPECT_EQ(countIn(core.workRam()), 0x1300u);
  core.run();
  EXPECT_EQ(countIn(core.workRam()), 0x1301u);
}

// The buffer stays where it is for as long as the cartridge is loaded, and the work RAM is kept
// through the console's reset button and a move of the machine onto another thread.
TEST(WorkRam, BufferStaysPutThroughResetAndThreadSwitch) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(workRamCartridge()));
  void* const first = core.memoryData(RETRO_MEMORY_SYSTEM_RAM);
  core.run(5u);
  EXPECT_EQ(core.memoryData(RETRO_MEMORY_SYSTEM_RAM), first);
  setCount(core.workRam(), 0x0100u);
  core.run();
  EXPECT_EQ(core.memoryData(RETRO_MEMORY_SYSTEM_RAM), first);
  EXPECT_EQ(countIn(core.workRam()), 0x0101u);
  core.reset();
  EXPECT_EQ(countIn(core.workRam()), 0x0101u);
  core.run();
  EXPECT_EQ(core.memoryData(RETRO_MEMORY_SYSTEM_RAM), first);
  EXPECT_EQ(countIn(core.workRam()), 0x0102u);
  seen.set(kAudioThreadKey, "enabled");
  core.run();
  EXPECT_EQ(core.memoryData(RETRO_MEMORY_SYSTEM_RAM), first);
  EXPECT_EQ(countIn(core.workRam()), 0x0103u);
  seen.set(kAudioThreadKey, "disabled");
  core.workRam()[kEchoed] = 0x77u;
  core.run();
  EXPECT_EQ(core.memoryData(RETRO_MEMORY_SYSTEM_RAM), first);
  EXPECT_EQ(countIn(core.workRam()), 0x0104u);
  EXPECT_EQ(core.workRam()[kEcho], 0x77u);
}

// A cartridge loaded in place of another starts with work RAM of its own, all zero.
TEST(WorkRam, NextCartridgeGetsItsOwnWorkRam) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(workRamCartridge()));
  core.run(4u);
  ASSERT_EQ(countIn(core.workRam()), 4u);
  ASSERT_TRUE(core.load(workRamCartridge()));
  EXPECT_EQ(countIn(core.workRam()), 0u);
  core.run();
  EXPECT_EQ(countIn(core.workRam()), 1u);
}

// Every bus address resolves through the map to the byte the machine reaches: for LoROM with
// and without a save, HiROM and ExHiROM, with saves smaller than a window, which the window
// repeats, and larger, which the windows hold piece by piece. LoROM's save sits in the lower
// halves of 30 banks, HiROM's and ExHiROM's in an 8 KB window of 64 banks each.
TEST(WorkRam, MapResolvesEveryAddressAsTheMachineDoes) {
  expectMapMatchesTheMachine(workRamCartridge(), 0u, "LoROM without a save");
  expectMapMatchesTheMachine(loRomWithSave(kEightKilobytes), 30u, "LoROM, 8 KB save");
  expectMapMatchesTheMachine(loRomWithSave(kTwoKilobytes), 30u, "LoROM, 2 KB save");
  expectMapMatchesTheMachine(loRomWithSave(kSixtyFourKilobytes), 30u, "LoROM, 64 KB save");
  expectMapMatchesTheMachine(hiRomWithSave(kEightKilobytes), 64u, "HiROM, 8 KB save");
  expectMapMatchesTheMachine(hiRomWithSave(kTwoKilobytes), 64u, "HiROM, 2 KB save");
  expectMapMatchesTheMachine(hiRomWithSave(kSixtyFourKilobytes), 64u, "HiROM, 64 KB save");
  expectMapMatchesTheMachine(exHiRomWithSave(kEightKilobytes), 64u, "ExHiROM, 8 KB save");
}

// The descriptors point at the buffers the frontend is handed, flagged for what they hold, and
// the list ends by telling the frontend how large the bus is.
TEST(WorkRam, MapPointsAtTheBuffersTheFrontendIsHanded) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(loRomWithSave(kEightKilobytes)));
  ASSERT_EQ(seen.maps.size(), 1u);
  const std::vector<retro_memory_descriptor>& map = seen.maps.front();
  void* const workRam = core.memoryData(RETRO_MEMORY_SYSTEM_RAM);
  void* const save = core.memoryData(RETRO_MEMORY_SAVE_RAM);
  ASSERT_NE(workRam, nullptr);
  ASSERT_NE(save, nullptr);
  std::size_t workRamWindows = 0;
  std::size_t saveWindows = 0;
  for (std::size_t at = 0; at + 1u < map.size(); ++at) {
    const retro_memory_descriptor& d = map[at];
    if (d.ptr == workRam) {
      ++workRamWindows;
      EXPECT_EQ(d.flags, std::uint64_t{RETRO_MEMDESC_SYSTEM_RAM}) << "descriptor " << at;
    } else if (d.ptr == save) {
      ++saveWindows;
      EXPECT_EQ(d.flags, std::uint64_t{RETRO_MEMDESC_SAVE_RAM}) << "descriptor " << at;
      EXPECT_LE(d.offset + d.len, core.memorySize(RETRO_MEMORY_SAVE_RAM)) << "descriptor " << at;
    } else {
      ADD_FAILURE() << "descriptor " << at << " points at neither buffer";
    }
  }
  EXPECT_EQ(workRamWindows, 3u);
  EXPECT_EQ(saveWindows, 30u);
  EXPECT_EQ(map.front().start, 0x7E0000u);
  EXPECT_EQ(map.front().len, kWorkRamBytes);
  EXPECT_EQ(map.back().ptr, nullptr);
  EXPECT_EQ(map.back().select, 0xFFFFFFu);
  EXPECT_EQ(map.back().flags, 0u);
}

// The map is sent once for every cartridge loaded, and a frontend that does not take one still
// runs the game.
TEST(WorkRam, MapIsSentOnEveryLoad) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(workRamCartridge()));
  ASSERT_TRUE(core.load(hiRomWithSave(kEightKilobytes)));
  core.unload();
  ASSERT_TRUE(core.load(workRamCartridge()));
  ASSERT_EQ(seen.maps.size(), 3u);
  EXPECT_EQ(seen.maps[0].size(), 4u);
  EXPECT_EQ(seen.maps[1].size(), 68u);
  EXPECT_EQ(seen.maps[2].size(), 4u);

  seen.takesMemoryMaps = false;
  ASSERT_TRUE(core.load(workRamCartridge()));
  EXPECT_EQ(seen.maps.size(), 3u);
  core.run(2u);
  EXPECT_EQ(countIn(core.workRam()), 2u);
}

}  // namespace
