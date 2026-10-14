// The cartridge's save as a frontend keeps it: the RETRO_MEMORY_SAVE_RAM buffer, as large as the
// cartridge's header declares, holding the save as each frame leaves it, and taking what the
// frontend writes into it before the next frame.
//
// What the game sees is read back through the save itself. The suite's cartridge counts frames
// into its save, adding one to a 16-bit count at the save's first two bytes in every vertical
// blank, so the count after a run says which save the frame began from, and it writes whether
// B is held on the first pad to the third byte.

#include <libretro.h>

#include "examples/common.h"
#include "loaded_core.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace {

using snaggletooth_libretro_test::LoadedCore;

// Where a LoROM image's header sits, and its chipset and save-size bytes.
constexpr std::size_t kHeaderSite = 0x7FC0u;
constexpr std::size_t kChipsetByte = kHeaderSite + 0x16u;
constexpr std::size_t kSaveSizeByte = kHeaderSite + 0x18u;
constexpr std::uint8_t kRamAndBattery = 0x02u;

// The save sizes the cases use: the header's code is 1 KB shifted left by it.
constexpr std::uint8_t kEightKilobytes = 3u;
constexpr std::uint8_t kTwoKilobytes = 1u;
constexpr std::uint8_t kThirtyTwoKilobytes = 5u;

// Where the cartridge keeps what it writes in its save.
constexpr std::size_t kCountLow = 0u;
constexpr std::size_t kCountHigh = 1u;
constexpr std::size_t kButtonB = 2u;
constexpr std::size_t kFirstUntouched = 3u;

// A LoROM cartridge with a battery-backed save of 1 KB << `sizeCode`. It turns the NMI on and
// waits; its vertical-blank handler adds one to the 16-bit count at $70:0000-$70:0001, the first
// two bytes of the save, then strobes the pads and writes the first pad's B bit, read through
// $4016, to $70:0002.
std::vector<std::uint8_t> saveCartridge(std::uint8_t sizeCode) {
  using snaggletooth::examples::put;
  std::vector<std::uint8_t> rom = snaggletooth::examples::imageWithNmi();
  rom[kChipsetByte] = kRamAndBattery;
  rom[kSaveSizeByte] = sizeCode;
  put(rom, 0x0000u, {
      0xA9u, 0x80u,                // $8000 LDA #$80
      0x8Du, 0x00u, 0x42u,         // $8002 STA !$4200         NMI on, auto-read off
      0x80u, 0xFEu,                // $8005 BRA $8005
  });
  put(rom, 0x0310u, {
      0xADu, 0x10u, 0x42u,         // $8310 LDA !$4210         acknowledge the NMI
      0xAFu, 0x00u, 0x00u, 0x70u,  // $8313 LDA $700000        the count's low byte
      0x18u,                       // $8317 CLC
      0x69u, 0x01u,                // $8318 ADC #$01
      0x8Fu, 0x00u, 0x00u, 0x70u,  // $831A STA $700000
      0xAFu, 0x01u, 0x00u, 0x70u,  // $831E LDA $700001        the high byte, with the carry
      0x69u, 0x00u,                // $8322 ADC #$00
      0x8Fu, 0x01u, 0x00u, 0x70u,  // $8324 STA $700001
      0xA9u, 0x01u,                // $8328 LDA #$01
      0x8Du, 0x16u, 0x40u,         // $832A STA !$4016         strobe high
      0x9Cu, 0x16u, 0x40u,         // $832D STZ !$4016         strobe low: the pads latch
      0xADu, 0x16u, 0x40u,         // $8330 LDA !$4016         the first pad's B, in bit 0
      0x29u, 0x01u,                // $8333 AND #$01
      0x8Fu, 0x02u, 0x00u, 0x70u,  // $8335 STA $700002
      0x40u,                       // $8339 RTI
  });
  return rom;
}

// The same cartridge with a header that declares no save.
std::vector<std::uint8_t> cartridgeWithoutSave() {
  std::vector<std::uint8_t> rom = saveCartridge(kEightKilobytes);
  rom[kChipsetByte] = 0x00u;
  rom[kSaveSizeByte] = 0x00u;
  return rom;
}

// What the test frontend answers: whether B is held on the first pad.
bool holdingB = false;

bool environment(unsigned command, void* data) {
  (void)data;
  return command == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT;
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
  const bool b = port == 0u && device == RETRO_DEVICE_JOYPAD && index == 0u &&
                 id == RETRO_DEVICE_ID_JOYPAD_B && holdingB;
  return b ? std::int16_t{1} : std::int16_t{0};
}

// The core loaded and set up as a frontend sets one up: the callbacks handed over, then
// retro_init. Deinitialized when the case ends.
class Core {
 public:
  Core() {
    holdingB = false;
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

  // The save buffer as the frontend reaches it.
  [[nodiscard]] std::span<std::uint8_t> save() const {
    return {static_cast<std::uint8_t*>(memoryData(RETRO_MEMORY_SAVE_RAM)),
            memorySize(RETRO_MEMORY_SAVE_RAM)};
  }

 private:
  LoadedCore module_;
};

std::uint16_t countIn(std::span<const std::uint8_t> save) {
  return static_cast<std::uint16_t>(save[kCountLow] | (save[kCountHigh] << 8));
}

void setCount(std::span<std::uint8_t> save, std::uint16_t count) {
  save[kCountLow] = static_cast<std::uint8_t>(count & 0xFFu);
  save[kCountHigh] = static_cast<std::uint8_t>(count >> 8);
}

// Bytes no frame of the cartridge writes, each different from its neighbours.
std::uint8_t patternAt(std::size_t at) { return static_cast<std::uint8_t>(at * 37u + 11u); }

void fillWithPattern(std::span<std::uint8_t> save) {
  for (std::size_t at = 0; at < save.size(); ++at) save[at] = patternAt(at);
}

void expectPatternFrom(std::span<const std::uint8_t> save, std::size_t first) {
  std::size_t wrong = 0;
  for (std::size_t at = first; at < save.size(); ++at) wrong += save[at] != patternAt(at) ? 1u : 0u;
  EXPECT_EQ(wrong, 0u) << "bytes past the cartridge's own differ from what the frontend wrote";
}

// A cartridge whose header declares no save offers the frontend none, and neither does a core
// with no cartridge loaded.
TEST(BatterySave, CartridgeWithoutSaveOffersNone) {
  Core core;
  ASSERT_TRUE(core.loaded());
  EXPECT_EQ(core.memoryData(RETRO_MEMORY_SAVE_RAM), nullptr);
  EXPECT_EQ(core.memorySize(RETRO_MEMORY_SAVE_RAM), 0u);
  ASSERT_TRUE(core.load(cartridgeWithoutSave()));
  core.run();
  EXPECT_EQ(core.memoryData(RETRO_MEMORY_SAVE_RAM), nullptr);
  EXPECT_EQ(core.memorySize(RETRO_MEMORY_SAVE_RAM), 0u);
}

// The buffer is as large as the header declares and starts as the machine's new save, all zero.
TEST(BatterySave, SizeIsWhatTheHeaderDeclares) {
  Core core;
  ASSERT_TRUE(core.loaded());
  for (const auto& [code, bytes] : {std::pair{kTwoKilobytes, 2048u}, std::pair{kEightKilobytes, 8192u},
                                    std::pair{kThirtyTwoKilobytes, 32768u}}) {
    ASSERT_TRUE(core.load(saveCartridge(code)));
    const std::span<std::uint8_t> save = core.save();
    ASSERT_NE(save.data(), nullptr) << "code " << unsigned{code};
    EXPECT_EQ(save.size(), bytes) << "code " << unsigned{code};
    std::size_t nonzero = 0;
    for (const std::uint8_t byte : save) nonzero += byte != 0u ? 1u : 0u;
    EXPECT_EQ(nonzero, 0u) << "code " << unsigned{code};
    core.unload();
  }
}

// Each run's write is in the buffer when the run returns: the count after N runs is N.
TEST(BatterySave, EachFramesWriteIsInTheBufferAfterItsRun) {
  Core core;
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(saveCartridge(kEightKilobytes)));
  for (std::uint16_t runs = 1u; runs <= 300u; ++runs) {
    core.run();
    ASSERT_EQ(countIn(core.save()), runs);
  }
}

// The buffer stays where it is for as long as the cartridge is loaded.
TEST(BatterySave, BufferStaysPutWhileTheCartridgeIsLoaded) {
  Core core;
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(saveCartridge(kEightKilobytes)));
  void* const first = core.memoryData(RETRO_MEMORY_SAVE_RAM);
  core.run(5u);
  EXPECT_EQ(core.memoryData(RETRO_MEMORY_SAVE_RAM), first);
  setCount(core.save(), 0x0100u);
  core.run();
  EXPECT_EQ(core.memoryData(RETRO_MEMORY_SAVE_RAM), first);
  core.reset();
  core.run();
  EXPECT_EQ(core.memoryData(RETRO_MEMORY_SAVE_RAM), first);
}

// A save the frontend writes after the cartridge loads is the one the game runs on: the count
// carries on from it, and every byte the game does not write stays as the frontend wrote it.
TEST(BatterySave, SaveWrittenAfterLoadIsTheGamesSave) {
  Core core;
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(saveCartridge(kEightKilobytes)));
  fillWithPattern(core.save());
  setCount(core.save(), 0x12FFu);
  core.run();
  EXPECT_EQ(countIn(core.save()), 0x1300u);
  EXPECT_EQ(core.save()[kButtonB], 0u);
  expectPatternFrom(core.save(), kFirstUntouched);
  core.run(2u);
  EXPECT_EQ(countIn(core.save()), 0x1302u);
  expectPatternFrom(core.save(), kFirstUntouched);
}

// The save a run leaves, kept by the frontend and handed back after the cartridge is loaded
// again, is the save the game carries on from.
TEST(BatterySave, SaveSurvivesUnloadAndReload) {
  Core core;
  ASSERT_TRUE(core.loaded());
  const std::vector<std::uint8_t> cartridge = saveCartridge(kEightKilobytes);
  ASSERT_TRUE(core.load(cartridge));
  fillWithPattern(core.save());
  setCount(core.save(), 0u);
  core.run(10u);
  const std::vector<std::uint8_t> kept(core.save().begin(), core.save().end());
  ASSERT_EQ(countIn(kept), 10u);
  core.unload();
  EXPECT_EQ(core.memoryData(RETRO_MEMORY_SAVE_RAM), nullptr);
  EXPECT_EQ(core.memorySize(RETRO_MEMORY_SAVE_RAM), 0u);

  ASSERT_TRUE(core.load(cartridge));
  ASSERT_EQ(core.save().size(), kept.size());
  EXPECT_EQ(countIn(core.save()), 0u);
  std::ranges::copy(kept, core.save().begin());
  core.run();
  EXPECT_EQ(countIn(core.save()), 11u);
  expectPatternFrom(core.save(), kFirstUntouched);
}

// A write the frontend makes between two frames of a running game reaches the next frame.
TEST(BatterySave, WriteBetweenFramesReachesTheNextFrame) {
  Core core;
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(saveCartridge(kEightKilobytes)));
  core.run(5u);
  ASSERT_EQ(countIn(core.save()), 5u);
  setCount(core.save(), 0x0300u);
  core.run();
  EXPECT_EQ(countIn(core.save()), 0x0301u);
  core.run();
  EXPECT_EQ(countIn(core.save()), 0x0302u);
}

// A frame that begins with a save from the frontend still reads the pads the frontend reports
// for it.
TEST(BatterySave, WriteBeforeAFrameKeepsThatFramesPads) {
  Core core;
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(saveCartridge(kEightKilobytes)));
  core.run(3u);
  ASSERT_EQ(core.save()[kButtonB], 0u);
  holdingB = true;
  setCount(core.save(), 0x0040u);
  core.run();
  EXPECT_EQ(countIn(core.save()), 0x0041u);
  EXPECT_EQ(core.save()[kButtonB], 1u);
  holdingB = false;
  core.run();
  EXPECT_EQ(core.save()[kButtonB], 0u);
}

// The console's reset button keeps the save: the game starts again and counts on from it.
TEST(BatterySave, ResetKeepsTheSave) {
  Core core;
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(saveCartridge(kEightKilobytes)));
  core.run(5u);
  core.reset();
  EXPECT_EQ(countIn(core.save()), 5u);
  core.run();
  EXPECT_EQ(countIn(core.save()), 6u);
}

// A cartridge loaded in place of another gets a buffer of its own save's size, starting empty.
TEST(BatterySave, NextCartridgeGetsItsOwnSave) {
  Core core;
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(saveCartridge(kEightKilobytes)));
  core.run(4u);
  ASSERT_TRUE(core.load(saveCartridge(kTwoKilobytes)));
  ASSERT_EQ(core.save().size(), 2048u);
  EXPECT_EQ(countIn(core.save()), 0u);
  core.run();
  EXPECT_EQ(countIn(core.save()), 1u);
}

// The save is the only memory the core offers.
TEST(BatterySave, NoOtherMemoryIsOffered) {
  Core core;
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(saveCartridge(kEightKilobytes)));
  core.run();
  for (const unsigned id : {RETRO_MEMORY_RTC, RETRO_MEMORY_SYSTEM_RAM, RETRO_MEMORY_VIDEO_RAM}) {
    EXPECT_EQ(core.memoryData(id), nullptr) << "id " << id;
    EXPECT_EQ(core.memorySize(id), 0u) << "id " << id;
  }
}

}  // namespace
