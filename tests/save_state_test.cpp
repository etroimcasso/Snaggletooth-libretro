// Save states as a frontend takes and loads them: the size a state takes, the bytes written into
// the frontend's buffer, and a loaded state putting the game back where it was saved, so the
// pictures and sound that follow a load are the ones that followed the save. A state is checked
// through what the game does after it, on two cartridges the suite builds: one that counts frames
// into its picture, its work RAM and its save, and one that plays a note.

#include <libretro.h>

#include "examples/common.h"
#include "loaded_core.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using snaggletooth_libretro_test::LoadedCore;

constexpr const char* kInterlaceKey = "snaggletooth_interlace";
constexpr const char* kAudioThreadKey = "snaggletooth_apu_thread";
constexpr const char* kPictureThreadKey = "snaggletooth_ppu_thread";

// The bytes of a state with no save; a state takes these and the save's size.
constexpr std::size_t kStateWithoutSave = 268858u;

// Where a LoROM image's header sits, and its chipset and save-size bytes.
constexpr std::size_t kHeaderSite = 0x7FC0u;
constexpr std::size_t kChipsetByte = kHeaderSite + 0x16u;
constexpr std::size_t kSaveSizeByte = kHeaderSite + 0x18u;
constexpr std::uint8_t kRamAndBattery = 0x02u;
constexpr std::uint8_t kTwoKilobytes = 1u;
constexpr std::uint8_t kEightKilobytes = 3u;

// SETINI bit 0 asks for an interlaced picture.
constexpr std::uint8_t kProgressive = 0x00u;
constexpr std::uint8_t kInterlaced = 0x01u;

// The count the counting cartridge keeps in work RAM ($7E0010), and the bus address of its step,
// the operand of the handler's ADC.
constexpr std::size_t kCount = 0x10u;
constexpr std::uint32_t kStepByte = 0x008318u;

// Where the bytes put the first controller port's presence byte, which is 0 or 1.
constexpr std::size_t kFirstPortByte = 198064u;

// A cartridge with a save of the size it is given that shows only its backdrop. Its
// vertical-blank handler adds the step byte to the count at $0010, then writes the count into
// colour 0, so every frame's picture differs from the last, and into the first byte of its save.
std::vector<std::uint8_t> countingCartridge(std::uint8_t saveSize = kTwoKilobytes,
                                            std::uint8_t setini = kProgressive) {
  using snaggletooth::examples::put;
  std::vector<std::uint8_t> rom = snaggletooth::examples::imageWithNmi();
  rom[kChipsetByte] = kRamAndBattery;
  rom[kSaveSizeByte] = saveSize;
  put(rom, 0x0000u, {
      0xA9u, setini,               // $8000 LDA #setini
      0x8Du, 0x33u, 0x21u,         // $8002 STA !$2133         SETINI
      0xA9u, 0x0Fu,                // $8005 LDA #$0F
      0x8Du, 0x00u, 0x21u,         // $8007 STA !$2100         screen on, full brightness
      0xA9u, 0x80u,                // $800A LDA #$80
      0x8Du, 0x00u, 0x42u,         // $800C STA !$4200         NMI on
      0x80u, 0xFEu,                // $800F BRA $800F
  });
  put(rom, 0x0310u, {
      0xADu, 0x10u, 0x42u,         // $8310 LDA !$4210         acknowledge the NMI
      0xADu, 0x10u, 0x00u,         // $8313 LDA !$0010         the count
      0x18u,                       // $8316 CLC
      0x69u, 0x01u,                // $8317 ADC #$01           the step byte at $8318
      0x8Du, 0x10u, 0x00u,         // $8319 STA !$0010
      0x9Cu, 0x21u, 0x21u,         // $831C STZ !$2121         CGADD = 0
      0x8Du, 0x22u, 0x21u,         // $831F STA !$2122         colour 0, low byte
      0x8Du, 0x22u, 0x21u,         // $8322 STA !$2122         colour 0, high byte
      0x8Fu, 0x00u, 0x00u, 0x70u,  // $8325 STA $700000        the save's first byte
      0x40u,                       // $8329 RTI
  });
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

// A Game Genie code for `value` at `address`, as the device writes it: the value, then the 24
// address bits in the device's order, each four bits one of its letters, a hyphen after the
// fourth. The bits after the value are written abcd efgh ijkl mnop qrst uvwx from the top; the
// address, from its top bit, is ijkl qrst opab cduv wxef ghmn.
std::string gameGenie(std::uint32_t address, std::uint8_t value) {
  constexpr std::string_view kLetters = "DF4709156BC8A23E";
  constexpr std::string_view kWritten = "abcdefghijklmnopqrstuvwx";
  constexpr std::string_view kAddressOrder = "ijklqrstopabcduvwxefghmn";
  std::uint32_t bits = 0;
  for (std::size_t at = 0; at < kAddressOrder.size(); ++at) {
    if (((address >> (23u - at)) & 1u) == 0u) continue;
    bits |= 1u << (23u - kWritten.find(kAddressOrder[at]));
  }
  const std::uint32_t word = static_cast<std::uint32_t>(value) << 24 | bits;
  std::string code;
  for (int nibble = 7; nibble >= 0; --nibble) {
    code += kLetters[(word >> (nibble * 4)) & 0xFu];
    if (nibble == 4) code += '-';
  }
  return code;
}

// What a stretch of runs handed the frontend: each run's picture, its shape and a hash of its
// colours, and each run's samples, left then right. Two stretches are equal when every run
// handed over the same picture and the same samples, as many of them.
struct Runs {
  std::vector<std::string> pictures;
  std::vector<std::vector<std::int16_t>> sounds;

  [[nodiscard]] bool operator==(const Runs&) const = default;
};

// What the test frontend saw the core do, and what it answers the core's questions with.
struct Frontend {
  std::map<std::string, std::string> values;  // the options the player set
  bool optionsChanged = false;                // the answer to GET_VARIABLE_UPDATE, cleared once read
  std::optional<std::uint64_t> quirks;        // the serialization quirks the core declared
  Runs runs;

  // Sets an option the way a player does in the menu.
  void set(const std::string& key, const std::string& value) {
    values[key] = value;
    optionsChanged = true;
  }
};

Frontend* frontend = nullptr;

bool environment(unsigned command, void* data) {
  switch (command) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
      return true;
    case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
      frontend->quirks = *static_cast<const std::uint64_t*>(data);
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
  frontend->runs.pictures.push_back(std::to_string(width) + 'x' + std::to_string(height) + ' ' +
                                    std::to_string(hash));
}

void audioSample(std::int16_t left, std::int16_t right) {
  frontend->runs.sounds.back().push_back(left);
  frontend->runs.sounds.back().push_back(right);
}

size_t audioSampleBatch(const std::int16_t* data, size_t frames) {
  frontend->runs.sounds.back().insert(frontend->runs.sounds.back().end(), data,
                                      data + frames * 2u);
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
    for (std::size_t at = 0; at < times; ++at) {
      frontend->runs.sounds.emplace_back();
      module_.entry<void()>("retro_run")();
    }
  }
  // What the next `times` runs hand over.
  Runs record(std::size_t times) {
    frontend->runs = {};
    run(times);
    return std::exchange(frontend->runs, Runs{});
  }

  [[nodiscard]] std::size_t stateSize() const {
    return module_.entry<size_t()>("retro_serialize_size")();
  }
  bool serializeInto(std::vector<std::uint8_t>& buffer) {
    return module_.entry<bool(void*, size_t)>("retro_serialize")(buffer.data(), buffer.size());
  }
  // A state of the machine as it stands, in a buffer of exactly retro_serialize_size() bytes.
  std::vector<std::uint8_t> serialize() {
    std::vector<std::uint8_t> buffer(stateSize());
    if (!serializeInto(buffer)) buffer.clear();
    return buffer;
  }
  bool unserialize(const std::vector<std::uint8_t>& bytes) {
    return module_.entry<bool(const void*, size_t)>("retro_unserialize")(bytes.data(),
                                                                         bytes.size());
  }

  [[nodiscard]] const std::uint8_t* save() const { return memory(RETRO_MEMORY_SAVE_RAM); }
  [[nodiscard]] const std::uint8_t* workRam() const { return memory(RETRO_MEMORY_SYSTEM_RAM); }
  void cheat(unsigned index, bool enabled, const std::string& code) {
    module_.entry<void(unsigned, bool, const char*)>("retro_cheat_set")(index, enabled,
                                                                        code.c_str());
  }

 private:
  [[nodiscard]] const std::uint8_t* memory(unsigned id) const {
    return static_cast<const std::uint8_t*>(
        module_.entry<void*(unsigned)>("retro_get_memory_data")(id));
  }

  LoadedCore module_;
};

// The runs before a save, and the runs compared after it.
constexpr std::size_t kRunsBefore = 30u;
constexpr std::size_t kRunsAfter = 60u;

// The two cartridges a round trip is checked on, with what makes each one's runs differ: the
// counting cartridge's every picture, and the sound cartridge's note.
void expectTheRunsChange(const Runs& runs) {
  const std::set<std::string> pictures(runs.pictures.begin(), runs.pictures.end());
  std::size_t sounding = 0;
  std::size_t samples = 0;
  for (const auto& sound : runs.sounds) {
    samples += sound.size();
    sounding += static_cast<std::size_t>(
        std::count_if(sound.begin(), sound.end(), [](std::int16_t s) { return s != 0; }));
  }
  EXPECT_TRUE(pictures.size() == runs.pictures.size() || sounding > samples / 2u)
      << "neither the pictures nor the sound change from run to run";
}

const std::vector<std::vector<std::uint8_t>>& cartridges() {
  static const std::vector<std::vector<std::uint8_t>> each{countingCartridge(), soundCartridge()};
  return each;
}

// No cartridge, no state; with one, the library's figure for its save.
TEST(SaveState, TheSizeIsTheLibrarysForTheCartridgesSave) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  EXPECT_EQ(core.stateSize(), 0u);
  std::vector<std::uint8_t> withoutSave = countingCartridge();
  withoutSave[kChipsetByte] = 0u;
  withoutSave[kSaveSizeByte] = 0u;
  ASSERT_TRUE(core.load(withoutSave));
  EXPECT_EQ(core.stateSize(), kStateWithoutSave);
  for (const std::uint8_t code : {kTwoKilobytes, kEightKilobytes, std::uint8_t{5u}}) {
    ASSERT_TRUE(core.load(countingCartridge(code)));
    EXPECT_EQ(core.stateSize(), kStateWithoutSave + (std::size_t{1024u} << code))
        << "save size code " << int{code};
  }
  core.unload();
  EXPECT_EQ(core.stateSize(), 0u);
}

// The size a frontend sizes its buffers by holds through runs, the reset button and a move onto
// other threads.
TEST(SaveState, TheSizeHoldsFromLoadToUnload) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(countingCartridge()));
  const std::size_t size = core.stateSize();
  core.run(kRunsBefore);
  EXPECT_EQ(core.stateSize(), size);
  core.reset();
  core.run(3u);
  EXPECT_EQ(core.stateSize(), size);
  seen.set(kAudioThreadKey, "enabled");
  seen.set(kPictureThreadKey, "disabled");
  core.run(3u);
  EXPECT_EQ(core.stateSize(), size);
  EXPECT_EQ(core.serialize().size(), size);
}

// A buffer shorter than the size is refused and left as it was; a longer one takes the state in
// its first bytes and keeps the rest.
TEST(SaveState, AShortBufferIsRefusedAndALongerOneTakesTheStateFirst) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(countingCartridge()));
  core.run(kRunsBefore);
  const std::vector<std::uint8_t> state = core.serialize();
  ASSERT_EQ(state.size(), core.stateSize());
  std::vector<std::uint8_t> shorter(state.size() - 1u, 0xA5u);
  EXPECT_FALSE(core.serializeInto(shorter));
  EXPECT_TRUE(std::all_of(shorter.begin(), shorter.end(), [](std::uint8_t b) { return b == 0xA5u; }));
  std::vector<std::uint8_t> longer(state.size() + 16u, 0xA5u);
  ASSERT_TRUE(core.serializeInto(longer));
  EXPECT_TRUE(std::equal(state.begin(), state.end(), longer.begin()));
  EXPECT_TRUE(std::all_of(longer.begin() + static_cast<std::ptrdiff_t>(state.size()), longer.end(),
                          [](std::uint8_t b) { return b == 0xA5u; }));
  ASSERT_TRUE(core.unserialize(longer));
}

// The gate: after a load, every run hands over the picture and the samples the same run handed
// over after the save, each run's sample count included, which the budget a frame leaves the
// machine partway through decides.
TEST(SaveState, ALoadHandsOverThePicturesAndSoundThatFollowedTheSave) {
  for (const auto& rom : cartridges()) {
    Frontend seen;
    Core core(seen);
    ASSERT_TRUE(core.loaded());
    ASSERT_TRUE(core.load(rom));
    core.run(kRunsBefore);
    const std::vector<std::uint8_t> state = core.serialize();
    ASSERT_FALSE(state.empty());
    const Runs saved = core.record(kRunsAfter);
    expectTheRunsChange(saved);
    ASSERT_TRUE(core.unserialize(state));
    EXPECT_EQ(core.record(kRunsAfter), saved);
  }
}

// A state outlives the session it was saved in: loaded after the cartridge is loaded again and
// has run on elsewhere, it puts the game back where it was saved.
TEST(SaveState, AStateLoadsAfterTheCartridgeIsLoadedAgain) {
  for (const auto& rom : cartridges()) {
    Frontend seen;
    Core core(seen);
    ASSERT_TRUE(core.loaded());
    ASSERT_TRUE(core.load(rom));
    core.run(kRunsBefore);
    const std::vector<std::uint8_t> state = core.serialize();
    const Runs saved = core.record(kRunsAfter);
    core.unload();
    ASSERT_TRUE(core.load(rom));
    core.run(7u);
    ASSERT_TRUE(core.unserialize(state));
    EXPECT_EQ(core.record(kRunsAfter), saved);
  }
}

// A state saved with the machine on the default threads loads into a machine on any others and
// runs on the same.
TEST(SaveState, AStateLoadsOnOtherThreads) {
  const std::vector<std::pair<const char*, const char*>> others = {
      {"disabled", "disabled"}, {"enabled", "disabled"}, {"enabled", "enabled"}};
  for (const auto& rom : cartridges()) {
    for (const auto& [audio, picture] : others) {
      Frontend seen;
      Core core(seen);
      ASSERT_TRUE(core.loaded());
      ASSERT_TRUE(core.load(rom));
      core.run(kRunsBefore);
      const std::vector<std::uint8_t> state = core.serialize();
      const Runs saved = core.record(kRunsAfter);
      seen.set(kAudioThreadKey, audio);
      seen.set(kPictureThreadKey, picture);
      core.run(3u);
      ASSERT_TRUE(core.unserialize(state));
      EXPECT_EQ(core.record(kRunsAfter), saved) << "audio " << audio << ", picture " << picture;
    }
  }
}

// A state taken before the first run, and one taken just after the reset button, each play on
// from there.
TEST(SaveState, AStateTakenAtLoadOrAfterResetPlaysOnFromThere) {
  for (const auto& rom : cartridges()) {
    Frontend seen;
    Core core(seen);
    ASSERT_TRUE(core.loaded());
    ASSERT_TRUE(core.load(rom));
    const std::vector<std::uint8_t> atLoad = core.serialize();
    const Runs fromLoad = core.record(kRunsAfter);
    ASSERT_TRUE(core.unserialize(atLoad));
    EXPECT_EQ(core.record(kRunsAfter), fromLoad);
    core.reset();
    const std::vector<std::uint8_t> atReset = core.serialize();
    const Runs fromReset = core.record(kRunsAfter);
    core.run(5u);
    ASSERT_TRUE(core.unserialize(atReset));
    EXPECT_EQ(core.record(kRunsAfter), fromReset);
  }
}

// Bytes that are not a state of the cartridge are refused, and the game runs on exactly as it
// would have: a state of another version, one cut short, one with a byte no state holds, and the
// states of the same program on cartridges with a larger save and with none.
TEST(SaveState, BytesThatAreNotAStateOfTheCartridgeAreRefused) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(countingCartridge(kEightKilobytes)));
  core.run(5u);
  const std::vector<std::uint8_t> largerSave = core.serialize();
  std::vector<std::uint8_t> withoutSave = countingCartridge();
  withoutSave[kChipsetByte] = 0u;
  withoutSave[kSaveSizeByte] = 0u;
  ASSERT_TRUE(core.load(withoutSave));
  core.run(5u);
  const std::vector<std::uint8_t> noSave = core.serialize();

  const std::vector<std::uint8_t> rom = countingCartridge();
  ASSERT_TRUE(core.load(rom));
  core.run(kRunsBefore);
  const Runs untouched = core.record(kRunsAfter);

  ASSERT_TRUE(core.load(rom));
  core.run(kRunsBefore);
  const std::vector<std::uint8_t> state = core.serialize();
  std::vector<std::uint8_t> otherVersion = state;
  otherVersion[0] ^= 0xFFu;
  const std::vector<std::uint8_t> cutShort(state.begin(), state.end() - 1);
  std::vector<std::uint8_t> badByte = state;
  badByte[kFirstPortByte] = 2u;
  for (const std::vector<std::uint8_t>* bytes :
       {&std::as_const(otherVersion), &cutShort, &std::as_const(badByte), &largerSave, &noSave}) {
    EXPECT_FALSE(core.unserialize(*bytes)) << bytes->size() << " bytes";
  }
  EXPECT_EQ(core.record(kRunsAfter), untouched);
}

// After a load the frontend's save and work-RAM buffers hold the state's, and the next frame
// carries on from them rather than from what the buffers held before the load.
TEST(SaveState, TheSaveAndWorkRamBuffersHoldTheLoadedState) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(countingCartridge()));
  core.run(10u);
  const std::uint8_t* const save = core.save();
  const std::uint8_t* const workRam = core.workRam();
  ASSERT_NE(save, nullptr);
  ASSERT_NE(workRam, nullptr);
  const std::uint8_t count = save[0];
  ASSERT_EQ(workRam[kCount], count);
  const std::vector<std::uint8_t> state = core.serialize();
  core.run(20u);
  ASSERT_NE(save[0], count);
  ASSERT_TRUE(core.unserialize(state));
  EXPECT_EQ(core.save(), save);
  EXPECT_EQ(core.workRam(), workRam);
  EXPECT_EQ(save[0], count);
  EXPECT_EQ(workRam[kCount], count);
  core.run();
  EXPECT_EQ(save[0], static_cast<std::uint8_t>(count + 1u));
  EXPECT_EQ(workRam[kCount], static_cast<std::uint8_t>(count + 1u));
}

// A Game Genie code patches the cartridge's ROM, which a state does not carry, so the patch is
// still in force after a load: the count goes on stepping by the patched byte.
TEST(SaveState, ACheatsRomPatchHoldsAcrossALoad) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(countingCartridge()));
  core.cheat(0u, true, gameGenie(kStepByte, 0x05u));
  core.run(10u);
  const std::uint8_t count = core.workRam()[kCount];
  const std::vector<std::uint8_t> state = core.serialize();
  core.run(10u);
  ASSERT_TRUE(core.unserialize(state));
  ASSERT_EQ(core.workRam()[kCount], count);
  core.run();
  EXPECT_EQ(core.workRam()[kCount], static_cast<std::uint8_t>(count + 5u));
}

// An interlaced picture after a load, in each way of showing it. Under Off and Bob every picture
// is the field the machine drew, so each is the one that followed the save. Under Weave a picture
// also holds the other field's rows from the picture before, which for the first picture after a
// load is the last one shown; from the second on, every picture is the one that followed the save.
TEST(SaveState, AnInterlacedPictureAfterALoadInEachMode) {
  const std::vector<std::uint8_t> rom = countingCartridge(kTwoKilobytes, kInterlaced);
  for (const char* mode : {"off", "bob", "weave"}) {
    Frontend seen;
    seen.values[kInterlaceKey] = mode;
    Core core(seen);
    ASSERT_TRUE(core.loaded());
    ASSERT_TRUE(core.load(rom));
    core.run(kRunsBefore);
    const std::vector<std::uint8_t> state = core.serialize();
    const Runs saved = core.record(kRunsAfter);
    ASSERT_TRUE(core.unserialize(state));
    const Runs loaded = core.record(kRunsAfter);
    ASSERT_EQ(loaded.pictures.size(), saved.pictures.size());
    const std::size_t first = std::string_view(mode) == "weave" ? 1u : 0u;
    EXPECT_TRUE(std::equal(loaded.pictures.begin() + static_cast<std::ptrdiff_t>(first),
                           loaded.pictures.end(),
                           saved.pictures.begin() + static_cast<std::ptrdiff_t>(first)))
        << mode;
    EXPECT_EQ(loaded.sounds, saved.sounds) << mode;
  }
}

// The frontend is told, as the core starts, that its states have none of libretro's
// serialization quirks.
TEST(SaveState, NoSerializationQuirkIsDeclared) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(seen.quirks.has_value());
  EXPECT_EQ(*seen.quirks, 0u);
}

}  // namespace
