// The player's cheat codes: retro_cheat_set and retro_cheat_reset. A Game Genie code patches the
// cartridge's ROM in the running console and is put back when it is turned off; a Pro Action
// Replay code and a raw code write their byte before every frame; anything else is refused with
// a message.
//
// What a code does is read back through the work RAM. The suite's cartridge counts frames into
// it, adding an immediate step byte, one, to a 16-bit count at $7E:0000-0001 in every vertical
// blank; copies the byte at direct-page $10 to $7E:0002, the echo; then stores the count's low
// byte at direct-page $10. A Game Genie code on the step byte changes how far the count moves in
// a frame, one on the echo's operand changes which byte is echoed, and an every-frame code on
// $7E0010 is the echo after each run.
//
// A run ends as its frame's vertical blank begins, and the handler runs as the next run begins:
// the first run after a load or the reset button, from power-on to the first vertical blank,
// runs no handler, so the cases that count from a load or a reset make that run first.

#include <libretro.h>

#include "examples/common.h"
#include "loaded_core.h"

#include "snaggletooth/snes/snes.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using snaggletooth_libretro_test::LoadedCore;

// Where the cartridge keeps what it writes in work RAM, and the byte it echoes.
constexpr std::size_t kCountLow = 0u;
constexpr std::size_t kCountHigh = 1u;
constexpr std::size_t kEcho = 2u;
constexpr std::size_t kEchoed = 0x10u;

// Bus addresses: the step byte (the operand of the handler's ADC #$01), the echo's direct-page
// operand (the $10 of LDA $10), and the work-RAM byte the echo reads.
constexpr std::uint32_t kStepByte = 0x008319u;
constexpr std::uint32_t kEchoOperand = 0x008329u;
constexpr std::uint32_t kEchoedAddress = 0x7E0010u;

constexpr const char* kAspectKey = "snaggletooth_aspect";
constexpr const char* kAudioThreadKey = "snaggletooth_apu_thread";

// A LoROM cartridge that turns the NMI on and waits; its vertical-blank handler adds the step
// byte to the count, echoes direct-page $10 to $7E:0002, then stores the count's low byte at $10.
std::vector<std::uint8_t> cheatCartridge() {
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
      0x69u, 0x01u,                // $8318 ADC #$01           the step byte at $8319
      0x8Fu, 0x00u, 0x00u, 0x7Eu,  // $831A STA $7E0000
      0xAFu, 0x01u, 0x00u, 0x7Eu,  // $831E LDA $7E0001        the high byte, with the carry
      0x69u, 0x00u,                // $8322 ADC #$00
      0x8Fu, 0x01u, 0x00u, 0x7Eu,  // $8324 STA $7E0001
      0xA5u, 0x10u,                // $8328 LDA $10            the operand at $8329
      0x8Fu, 0x02u, 0x00u, 0x7Eu,  // $832A STA $7E0002        the echo
      0xAFu, 0x00u, 0x00u, 0x7Eu,  // $832E LDA $7E0000
      0x85u, 0x10u,                // $8332 STA $10
      0x40u,                       // $8334 RTI
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

// A Pro Action Replay code, AAAAAAVV, and a raw code, AAAAAA:VV.
std::string actionReplay(std::uint32_t address, std::uint8_t value) {
  char text[16];
  std::snprintf(text, sizeof text, "%06X%02X", static_cast<unsigned>(address),
                static_cast<unsigned>(value));
  return text;
}

std::string raw(std::uint32_t address, std::uint8_t value) {
  char text[16];
  std::snprintf(text, sizeof text, "%06X:%02X", static_cast<unsigned>(address),
                static_cast<unsigned>(value));
  return text;
}

std::string refusal(const std::string& code) {
  return "Snaggletooth cannot read cheat code \"" + code + "\".";
}

// What the test frontend saw the core do, and what it answers the core's questions with.
struct Frontend {
  std::vector<std::string> messages;         // through SET_MESSAGE_EXT
  std::map<std::string, std::string> values;  // the options the player set
  bool optionsChanged = false;

  void set(const std::string& key, const std::string& value) {
    values[key] = value;
    optionsChanged = true;
  }
};

Frontend* frontend = nullptr;

// The message the core sends at load on a computer whose graphics card it cannot draw on. The
// GPU picture's cases hold it (gpu_picture_test.cpp); this file's cases count the messages the
// cartridge and the player's actions make, so the frontend sets it aside.
bool isGpuFallBack(std::string_view text) {
  return text.starts_with("Snaggletooth cannot draw on the GPU here");
}

bool environment(unsigned command, void* data) {
  switch (command) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
      return true;
    case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
      *static_cast<unsigned*>(data) = 1u;
      return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE_EXT:
      if (!isGpuFallBack(static_cast<const retro_message_ext*>(data)->msg)) {
        frontend->messages.emplace_back(static_cast<const retro_message_ext*>(data)->msg);
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

  void cheat(unsigned index, bool enabled, const char* code) {
    module_.entry<void(unsigned, bool, const char*)>("retro_cheat_set")(index, enabled, code);
  }
  void cheat(unsigned index, bool enabled, const std::string& code) {
    cheat(index, enabled, code.c_str());
  }
  void resetCheats() { module_.entry<void()>("retro_cheat_reset")(); }

  // The work RAM as the frontend reaches it.
  [[nodiscard]] std::span<std::uint8_t> workRam() const {
    return {static_cast<std::uint8_t*>(
                module_.entry<void*(unsigned)>("retro_get_memory_data")(RETRO_MEMORY_SYSTEM_RAM)),
            module_.entry<size_t(unsigned)>("retro_get_memory_size")(RETRO_MEMORY_SYSTEM_RAM)};
  }

  [[nodiscard]] std::uint16_t count() const {
    const std::span<const std::uint8_t> ram = workRam();
    return static_cast<std::uint16_t>(ram[kCountLow] | (ram[kCountHigh] << 8));
  }

  // How far one run moves the count.
  std::uint16_t step() {
    const std::uint16_t before = count();
    run();
    return static_cast<std::uint16_t>(count() - before);
  }

  // One run with $AA written to work RAM $10 and $BB to $11 before it: the step it made and the
  // byte it echoed, $AA while the handler reads $10 and $BB while a code points it at $11.
  struct Frame {
    std::uint16_t step;
    std::uint8_t echo;
    [[nodiscard]] bool operator==(const Frame&) const noexcept = default;
  };
  Frame markedRun() {
    workRam()[kEchoed] = 0xAAu;
    workRam()[kEchoed + 1u] = 0xBBu;
    const std::uint16_t moved = step();
    return Frame{.step = moved, .echo = workRam()[kEcho]};
  }

 private:
  LoadedCore module_;
};

// A Game Genie code on the step byte makes the count move by the code's value from the next run.
TEST(Cheats, GameGenieCodePatchesTheRom) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(cheatCartridge()));
  core.run();
  for (int run = 0; run < 3; ++run) EXPECT_EQ(core.step(), 1u);
  core.cheat(0u, true, gameGenie(kStepByte, 0x05u));
  for (int run = 0; run < 3; ++run) EXPECT_EQ(core.step(), 5u);
  EXPECT_TRUE(seen.messages.empty());
}

// Turning a Game Genie code off gives the ROM back its own byte, after an option change between.
TEST(Cheats, TurningTheCodeOffPutsTheByteBack) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(cheatCartridge()));
  core.run();
  const std::string code = gameGenie(kStepByte, 0x05u);
  core.cheat(0u, true, code);
  EXPECT_EQ(core.step(), 5u);
  EXPECT_EQ(core.step(), 5u);
  seen.set(kAspectKey, "4:3");
  EXPECT_EQ(core.step(), 5u);
  core.cheat(0u, false, code);
  EXPECT_EQ(core.step(), 1u);
  EXPECT_EQ(core.step(), 1u);
}

// retro_cheat_reset turns every code off and gives the ROM back every byte the codes replaced.
TEST(Cheats, ResetCheatsPutsEveryByteBack) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(cheatCartridge()));
  core.run();
  const Core::Frame plain{.step = 1u, .echo = 0xAAu};
  EXPECT_EQ(core.markedRun(), plain);
  core.cheat(3u, true, gameGenie(kStepByte, 0x05u));
  core.cheat(0u, true, gameGenie(kEchoOperand, 0x11u));
  const Core::Frame cheated{.step = 5u, .echo = 0xBBu};
  EXPECT_EQ(core.markedRun(), cheated);
  EXPECT_EQ(core.markedRun(), cheated);
  core.resetCheats();
  EXPECT_EQ(core.markedRun(), plain);
  EXPECT_EQ(core.markedRun(), plain);
}

// The console's reset button leaves a ROM patch in place, and the count where it stood.
TEST(Cheats, RomPatchHoldsAcrossReset) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(cheatCartridge()));
  core.cheat(0u, true, gameGenie(kStepByte, 0x05u));
  core.run(4u);
  ASSERT_EQ(core.count(), 15u);
  core.reset();
  EXPECT_EQ(core.count(), 15u);
  core.run();
  EXPECT_EQ(core.count(), 15u);
  EXPECT_EQ(core.step(), 5u);
  EXPECT_EQ(core.count(), 20u);
}

// A machine rebuilt on other threads carries the ROM patch, and turning the code off then gives
// back the cartridge's own byte.
TEST(Cheats, RomPatchHoldsAcrossAThreadSwitch) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(cheatCartridge()));
  core.run();
  const std::string code = gameGenie(kStepByte, 0x05u);
  core.cheat(0u, true, code);
  EXPECT_EQ(core.step(), 5u);
  seen.set(kAudioThreadKey, "enabled");
  EXPECT_EQ(core.step(), 5u);
  EXPECT_EQ(core.step(), 5u);
  core.cheat(0u, false, code);
  EXPECT_EQ(core.step(), 1u);
}

// A Pro Action Replay code writes its byte before every frame: the game echoes it each run,
// though it writes its own count over the byte each frame, and the count still moves.
TEST(Cheats, ActionReplayCodeWritesEveryFrame) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(cheatCartridge()));
  core.run();
  core.cheat(0u, true, actionReplay(kEchoedAddress, 0x5Au));
  for (int run = 0; run < 10; ++run) {
    EXPECT_EQ(core.step(), 1u);
    EXPECT_EQ(core.workRam()[kEcho], 0x5Au);
  }
  EXPECT_TRUE(seen.messages.empty());
}

// Turned off, the code writes nothing more: the game echoes the byte it stored itself.
TEST(Cheats, ActionReplayCodeOffStopsWriting) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(cheatCartridge()));
  const std::string code = actionReplay(kEchoedAddress, 0x5Au);
  core.cheat(0u, true, code);
  core.run(3u);
  ASSERT_EQ(core.workRam()[kEcho], 0x5Au);
  core.cheat(0u, false, code);
  const std::uint8_t stored = core.workRam()[kCountLow];
  core.run();
  EXPECT_EQ(core.workRam()[kEcho], stored);
}

// A raw code, AAAAAA:VV, writes its byte before every frame as a Pro Action Replay code does.
TEST(Cheats, RawCodeWritesEveryFrame) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(cheatCartridge()));
  core.run();
  core.cheat(0u, true, raw(kEchoedAddress, 0xC3u));
  for (int run = 0; run < 10; ++run) {
    EXPECT_EQ(core.step(), 1u);
    EXPECT_EQ(core.workRam()[kEcho], 0xC3u);
  }
  EXPECT_TRUE(seen.messages.empty());
}

// A frontend write and a code on the same byte before one frame: the code's byte is the one the
// game reads.
TEST(Cheats, CheatWinsOverAFrontendWrite) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(cheatCartridge()));
  core.cheat(0u, true, actionReplay(kEchoedAddress, 0x55u));
  core.run(2u);
  core.workRam()[kEchoed] = 0xAAu;
  core.run();
  EXPECT_EQ(core.workRam()[kEcho], 0x55u);
}

// A code set at an index already holding one replaces it, and turning the new one off leaves the
// cartridge's own byte.
TEST(Cheats, CodeSetAgainReplacesTheOld) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(cheatCartridge()));
  core.run();
  core.cheat(0u, true, gameGenie(kStepByte, 0x05u));
  EXPECT_EQ(core.step(), 5u);
  const std::string three = gameGenie(kStepByte, 0x03u);
  core.cheat(0u, true, three);
  EXPECT_EQ(core.step(), 3u);
  core.cheat(0u, false, three);
  EXPECT_EQ(core.step(), 1u);
}

// A string in none of the three forms is refused with a message naming it, and changes nothing;
// a null code changes nothing and says nothing.
TEST(Cheats, UnreadableCodeIsRefusedWithAMessage) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(cheatCartridge()));
  core.run();
  const std::vector<std::string> unreadable{
      "",
      "XYZ",
      "7E00105",
      "DDGD-DDDD",
      "7E0010:V",
      "7E00 1055",
      actionReplay(kEchoedAddress, 0x5Au) + "+" + actionReplay(kEchoedAddress, 0x66u),
  };
  unsigned index = 0;
  for (const std::string& code : unreadable) {
    core.cheat(index++, true, code);
    ASSERT_EQ(seen.messages.size(), index) << code;
    EXPECT_EQ(seen.messages.back(), refusal(code));
  }
  core.cheat(index, true, nullptr);
  EXPECT_EQ(seen.messages.size(), unreadable.size());
  const std::uint8_t stored = core.workRam()[kCountLow];
  EXPECT_EQ(core.step(), 1u);
  EXPECT_EQ(core.workRam()[kEcho], stored);
}

// A code naming an address with no memory behind it, or a Game Genie code outside the ROM, is
// refused with a message and changes nothing.
TEST(Cheats, CodeAtNoMemoryIsRefused) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(cheatCartridge()));
  const std::vector<std::uint8_t> rom = cheatCartridge();
  const snaggletooth::Snes machine(snaggletooth::SnesConfig{.rom = rom});
  std::optional<std::uint32_t> openBus;
  for (std::uint32_t address = 0x2000u; address < 0x8000u && !openBus; ++address) {
    if (machine.physical(address).space == snaggletooth::Snes::Space::OpenBus) openBus = address;
  }
  ASSERT_TRUE(openBus.has_value());
  ASSERT_EQ(machine.physical(0x002100u).space, snaggletooth::Snes::Space::Register);
  ASSERT_EQ(machine.physical(kEchoedAddress).space, snaggletooth::Snes::Space::WorkRam);

  core.run();
  const std::vector<std::string> refused{
      actionReplay(0x002100u, 0x80u),
      actionReplay(*openBus, 0x80u),
      gameGenie(kEchoedAddress, 0x55u),
  };
  unsigned index = 0;
  for (const std::string& code : refused) {
    core.cheat(index++, true, code);
    ASSERT_EQ(seen.messages.size(), index) << code;
    EXPECT_EQ(seen.messages.back(), refusal(code));
  }
  const std::uint8_t stored = core.workRam()[kCountLow];
  EXPECT_EQ(core.step(), 1u);
  EXPECT_EQ(core.workRam()[kEcho], stored);
}

// A code set before a cartridge loads takes effect when it does; one the cartridge has no memory
// for is refused then, not before.
TEST(Cheats, CodeSetBeforeLoadTakesEffectAtLoad) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  core.cheat(0u, true, gameGenie(kStepByte, 0x05u));
  const std::string outsideRom = gameGenie(kEchoedAddress, 0x55u);
  core.cheat(1u, true, outsideRom);
  EXPECT_TRUE(seen.messages.empty());
  ASSERT_TRUE(core.load(cheatCartridge()));
  ASSERT_EQ(seen.messages.size(), 1u);
  EXPECT_EQ(seen.messages.back(), refusal(outsideRom));
  core.run();
  EXPECT_EQ(core.step(), 5u);
  EXPECT_EQ(core.step(), 5u);
}

// Unloading the cartridge forgets its codes: the same cartridge loaded again runs as it is.
TEST(Cheats, UnloadForgetsTheCodes) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(cheatCartridge()));
  core.cheat(0u, true, gameGenie(kStepByte, 0x05u));
  core.cheat(1u, true, actionReplay(kEchoedAddress, 0x5Au));
  core.run(2u);
  ASSERT_EQ(core.count(), 5u);
  ASSERT_EQ(core.workRam()[kEcho], 0x5Au);
  core.unload();
  ASSERT_TRUE(core.load(cheatCartridge()));
  core.run();
  EXPECT_EQ(core.step(), 1u);
  EXPECT_EQ(core.step(), 1u);
  EXPECT_EQ(core.workRam()[kEcho], static_cast<std::uint8_t>(core.workRam()[kCountLow] - 1u));
}

}  // namespace
