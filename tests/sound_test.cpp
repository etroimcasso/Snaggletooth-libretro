// The sound as a frontend receives it: 32,000 stereo samples a second of the console's time, in
// 16-bit pairs, left then right, through the batch callback, each run handing over the samples
// of the frame it ran.
//
// What the samples should be comes from a second machine run here: its sound, drained as it
// runs, is the stream the core's runs must hand over in order, and the master cycle each of its
// frames ends at says how many samples the console's clock has made by then.

#include <libretro.h>

#include "examples/common.h"
#include "loaded_core.h"
#include "snaggletooth/snes/snes.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <vector>

namespace {

using snaggletooth::StereoFrame;
using snaggletooth_libretro_test::LoadedCore;

// The rate the console makes sound at.
constexpr double kSampleRate = 32000.0;
constexpr std::int64_t kSamplesPerSecond = 32000;

// Where a LoROM cartridge's header sits in its image, its country byte, and a country byte for
// Europe, a 50 Hz region.
constexpr std::size_t kLoRomHeaderSite = 0x7FC0u;
constexpr std::size_t kCountryByte = 0x19u;
constexpr std::uint8_t kEuropeCountry = 0x02u;

// The block the sound cartridge uploads to the audio machine's memory at $0200: a program, the
// DSP writes it makes, a sample directory entry and one BRR block.
constexpr std::size_t kBlockInImage = 0x0100u;  // $8100
constexpr std::uint8_t kBlockBytes = 0x8Du;     // $0200-$028C

// A cartridge that plays one held note. Its reset code sends the block above to the audio
// machine through the upload protocol, one acknowledged byte at a time, and starts the program
// at $0200. The program writes the DSP registers from its table and loops: voice 0 plays a
// sawtooth from a looping BRR block, at a pitch that is not a whole fraction of the sample rate,
// under a slow attack, at different volumes left and right, so no two stretches of its sound
// repeat and a swapped channel shows.
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

std::vector<std::uint8_t> palSoundCartridge() {
  std::vector<std::uint8_t> rom = soundCartridge();
  rom[kLoRomHeaderSite + kCountryByte] = kEuropeCountry;
  return rom;
}

// Records the master cycle each frame ends at.
class FrameEnds final : public snaggletooth::FrameObserver {
 public:
  explicit FrameEnds(const snaggletooth::Snes& machine) : machine_(machine) {}

  void frame(const snaggletooth::VideoFrame& frame) override {
    (void)frame;
    ends.push_back(machine_.state().master);
  }

  std::vector<std::uint64_t> ends;

 private:
  const snaggletooth::Snes& machine_;
};

// A cartridge's first `frames` frames, run on a machine here: the master cycle each ends at,
// and the sound the machine made, drained a quarter frame at a time, through the last of them
// and a little past it.
struct Reference {
  std::vector<std::uint64_t> frameEnds;
  std::vector<StereoFrame> samples;
};

Reference referenceRun(const std::vector<std::uint8_t>& rom, snaggletooth::Region region,
                       std::size_t frames) {
  snaggletooth::Snes machine(snaggletooth::SnesConfig{.rom = rom, .region = region});
  FrameEnds ends(machine);
  machine.setFrameObserver(&ends);
  const std::uint64_t chunk = snaggletooth::consoleClock(region).masterCyclesPerFrame / 4u;
  Reference reference;
  while (ends.ends.size() < frames) {
    machine.run(chunk);
    const std::vector<StereoFrame> made = machine.takeFrames();
    reference.samples.insert(reference.samples.end(), made.begin(), made.end());
  }
  reference.frameEnds = std::move(ends.ends);
  reference.frameEnds.resize(frames);
  return reference;
}

// What the test frontend saw the core do.
struct Frontend {
  std::size_t takeAtMost = std::numeric_limits<std::size_t>::max();  // the most one batch takes
  std::vector<StereoFrame> samples;     // every sample taken, in order
  std::vector<std::size_t> afterRun;    // samples.size() after each retro_run
  std::size_t batchCalls = 0;
  std::size_t singleCalls = 0;
};

Frontend* frontend = nullptr;

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
  ++frontend->singleCalls;
  frontend->samples.push_back(StereoFrame{.left = left, .right = right});
}

size_t audioSampleBatch(const std::int16_t* data, size_t frames) {
  ++frontend->batchCalls;
  const std::size_t taken = std::min(frames, frontend->takeAtMost);
  for (std::size_t at = 0; at < taken; ++at) {
    frontend->samples.push_back(StereoFrame{.left = data[2u * at], .right = data[2u * at + 1u]});
  }
  return taken;
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
      module_.entry<void()>("retro_run")();
      frontend->afterRun.push_back(frontend->samples.size());
    }
  }
  [[nodiscard]] retro_system_av_info avInfo() const {
    retro_system_av_info info{};
    module_.entry<void(retro_system_av_info*)>("retro_get_system_av_info")(&info);
    return info;
  }

 private:
  LoadedCore module_;
};

// By the end of each run the frontend holds the samples of the console's time up to that frame's
// end, to within one: `taken` samples after a frame ending at master cycle M, against the
// 32,000 a second the clock makes, M x 32,000 / (the master clock's rate).
void expectOnTheClock(const std::vector<std::size_t>& taken,
                      const std::vector<std::uint64_t>& frameEnds, snaggletooth::Region region) {
  const snaggletooth::ConsoleClock clock = snaggletooth::consoleClock(region);
  ASSERT_EQ(taken.size(), frameEnds.size());
  for (std::size_t at = 0; at < taken.size(); ++at) {
    // Scaled by the master clock's numerator, so the comparison stays in whole numbers.
    const auto held = static_cast<std::int64_t>(taken[at]) *
                      static_cast<std::int64_t>(clock.hertzNumerator);
    const auto owed = static_cast<std::int64_t>(frameEnds[at]) * kSamplesPerSecond *
                      static_cast<std::int64_t>(clock.hertzDenominator);
    ASSERT_LE(std::llabs(held - owed), static_cast<std::int64_t>(clock.hertzNumerator))
        << "run " << at << ": " << taken[at] << " samples by master cycle " << frameEnds[at];
  }
}

// The core's samples are the first of the reference machine's, in order.
void expectTheMachinesSamples(const std::vector<StereoFrame>& seen,
                              const std::vector<StereoFrame>& made) {
  ASSERT_LE(seen.size(), made.size());
  std::size_t differing = 0;
  std::size_t first = seen.size();
  for (std::size_t at = 0; at < seen.size(); ++at) {
    if (seen[at] == made[at]) continue;
    if (differing++ == 0u) first = at;
  }
  EXPECT_EQ(differing, 0u) << "first difference at sample " << first;
}

TEST(Sound, TheRateIs32000InBothRegions) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(soundCartridge()));
  EXPECT_EQ(core.avInfo().timing.sample_rate, kSampleRate);
  core.unload();
  ASSERT_TRUE(core.load(palSoundCartridge()));
  EXPECT_EQ(core.avInfo().timing.sample_rate, kSampleRate);
}

TEST(Sound, EachRunHandsOverTheSoundOfItsFrame) {
  constexpr std::size_t kRuns = 600u;
  const std::vector<std::uint8_t> rom = soundCartridge();
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(rom));
  core.run(kRuns);
  const Reference reference = referenceRun(rom, snaggletooth::Region::Ntsc, kRuns);
  ASSERT_NO_FATAL_FAILURE(
      expectOnTheClock(seen.afterRun, reference.frameEnds, snaggletooth::Region::Ntsc));
  EXPECT_EQ(seen.singleCalls, 0u);
  EXPECT_GE(seen.batchCalls, kRuns);
}

TEST(Sound, APalRunHandsOverTheSoundOfItsFrame) {
  constexpr std::size_t kRuns = 500u;
  const std::vector<std::uint8_t> rom = palSoundCartridge();
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(rom));
  core.run(kRuns);
  const Reference reference = referenceRun(rom, snaggletooth::Region::Pal, kRuns);
  ASSERT_NO_FATAL_FAILURE(
      expectOnTheClock(seen.afterRun, reference.frameEnds, snaggletooth::Region::Pal));
  ASSERT_NO_FATAL_FAILURE(expectTheMachinesSamples(seen.samples, reference.samples));
}

TEST(Sound, TheSamplesAreTheMachinesLeftThenRight) {
  constexpr std::size_t kRuns = 240u;
  const std::vector<std::uint8_t> rom = soundCartridge();
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(rom));
  core.run(kRuns);
  const Reference reference = referenceRun(rom, snaggletooth::Region::Ntsc, kRuns);
  // The cartridge sounds within the stretch compared, louder on the left than on the right: a
  // silent stretch, or one alike on both sides, could hide a lost or swapped channel.
  ASSERT_LE(seen.samples.size(), reference.samples.size());
  const auto louderLeft = std::count_if(
      reference.samples.begin(), reference.samples.begin() + std::ptrdiff_t(seen.samples.size()),
      [](const StereoFrame& sample) {
        return sample.right != 0 && std::abs(sample.left) > std::abs(sample.right);
      });
  ASSERT_GE(louderLeft, 1000);
  ASSERT_NO_FATAL_FAILURE(expectTheMachinesSamples(seen.samples, reference.samples));
}

TEST(Sound, TwoLoadsHandOverIdenticalSamples) {
  constexpr std::size_t kRuns = 240u;
  const std::vector<std::uint8_t> rom = soundCartridge();
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(rom));
  core.run(kRuns);
  // Four seconds of sound, so two loads that both handed over nothing do not pass as alike.
  ASSERT_GE(seen.samples.size(), kRuns * 531u);
  const std::vector<StereoFrame> first = std::move(seen.samples);
  const std::vector<std::size_t> firstCounts = std::move(seen.afterRun);
  seen.samples.clear();
  seen.afterRun.clear();
  core.unload();
  ASSERT_TRUE(core.load(rom));
  core.run(kRuns);
  EXPECT_EQ(seen.afterRun, firstCounts);
  EXPECT_TRUE(seen.samples == first);
}

TEST(Sound, AFrontendTakingPartOfABatchIsHandedTheRest) {
  constexpr std::size_t kRuns = 120u;
  const std::vector<std::uint8_t> rom = soundCartridge();
  Frontend seen;
  seen.takeAtMost = 100u;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(rom));
  core.run(kRuns);
  const Reference reference = referenceRun(rom, snaggletooth::Region::Ntsc, kRuns);
  ASSERT_NO_FATAL_FAILURE(
      expectOnTheClock(seen.afterRun, reference.frameEnds, snaggletooth::Region::Ntsc));
  ASSERT_NO_FATAL_FAILURE(expectTheMachinesSamples(seen.samples, reference.samples));
  EXPECT_GE(seen.batchCalls, kRuns * 5u);
}

TEST(Sound, AResetKeepsTheSoundGoing) {
  constexpr std::size_t kRuns = 240u;
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(soundCartridge()));
  core.run(kRuns);
  core.reset();
  const std::size_t beforeReset = seen.samples.size();
  core.run(kRuns);
  // Every run after the reset hands over a frame's sound — 32,000 a second at about 60.1
  // frames a second is 532.5 a frame — and the cartridge, started again, sounds again.
  for (std::size_t at = kRuns; at < seen.afterRun.size(); ++at) {
    const std::size_t made = seen.afterRun[at] - seen.afterRun[at - 1u];
    EXPECT_GE(made, 531u) << "run " << at;
    EXPECT_LE(made, 534u) << "run " << at;
  }
  const auto sounding =
      std::count_if(seen.samples.begin() + std::ptrdiff_t(beforeReset), seen.samples.end(),
                    [](const StereoFrame& sample) { return sample.left != 0; });
  EXPECT_GE(sounding, 1000);
}

}  // namespace
