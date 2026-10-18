// The controllers as a frontend fills them: a RetroPad in each of the console's two ports until
// the frontend chooses otherwise, each of its twelve buttons reaching the console's button of
// the same name, a port set to None or to a device the core does not offer standing empty, and
// the menu entries and button names the frontend shows.
//
// What the machine sees is read back through the picture. The suite's cartridge reads both
// pads over the serial ports every vertical blank, seventeen bits from each — the sixteen a pad
// shifts out, then the one after them, which is high from a pad and low from an empty port —
// and paints each port's reading into a colour on screen.

#include <libretro.h>

#include "examples/common.h"
#include "loaded_core.h"
#include "snaggletooth/snes/snes.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using snaggletooth_libretro_test::LoadedCore;

// A cartridge that shows the two ports. BG1 is drawn from one 2-bit tile whose left four columns
// are colour 1 and right four colour 2, across the whole screen, so x = 0 shows colour 1 and
// x = 4 colour 2. Its vertical-blank handler strobes the ports, then reads each one seventeen
// times through $4016 or $4017: the first sixteen bits are the pad's word, B first, and the
// seventeenth is high only when a pad is in the port. It writes port 1's reading to colour 1 and
// port 2's to colour 2, as the word shifted down one bit with the seventeenth bit at the bottom:
// B lands in bit 14 and R in bit 3, and bit 0 says whether a pad is there. Auto-read stays off,
// so the handler's reads are the only ones.
std::vector<std::uint8_t> padCartridge() {
  using snaggletooth::examples::put;
  std::vector<std::uint8_t> rom = snaggletooth::examples::imageWithNmi();
  put(rom, 0x0000u, {
      0xA9u, 0x80u,                // $8000 LDA #$80
      0x8Du, 0x00u, 0x21u,         // $8002 STA !$2100         forced blank
      0x8Du, 0x15u, 0x21u,         // $8005 STA !$2115         VMAIN: step after the high byte
      0x9Cu, 0x16u, 0x21u,         // $8008 STZ !$2116         VMADD = word $0000
      0x9Cu, 0x17u, 0x21u,         // $800B STZ !$2117
      0xA2u, 0x08u,                // $800E LDX #$08           eight rows of the tile
      0xA9u, 0xF0u,                // $8010 LDA #$F0
      0x8Du, 0x18u, 0x21u,         // $8012 STA !$2118         plane 0: columns 0-3
      0xA9u, 0x0Fu,                // $8015 LDA #$0F
      0x8Du, 0x19u, 0x21u,         // $8017 STA !$2119         plane 1: columns 4-7
      0xCAu,                       // $801A DEX
      0xD0u, 0xF3u,                // $801B BNE $8010
      0xA9u, 0x04u,                // $801D LDA #$04
      0x8Du, 0x07u, 0x21u,         // $801F STA !$2107         BG1SC: the screen at word $0400
      0x9Cu, 0x0Bu, 0x21u,         // $8022 STZ !$210B         BG12NBA: BG1's tiles at word $0000
      0xA9u, 0x01u,                // $8025 LDA #$01
      0x8Du, 0x2Cu, 0x21u,         // $8027 STA !$212C         TM: BG1 on the main screen
      0x9Cu, 0x05u, 0x21u,         // $802A STZ !$2105         BGMODE 0
      0xA9u, 0x0Fu,                // $802D LDA #$0F
      0x8Du, 0x00u, 0x21u,         // $802F STA !$2100         screen on, full brightness
      0xA9u, 0x80u,                // $8032 LDA #$80
      0x8Du, 0x00u, 0x42u,         // $8034 STA !$4200         NMI on, auto-read off
      0x80u, 0xFEu,                // $8037 BRA $8037
  });
  put(rom, 0x0310u, {
      0xADu, 0x10u, 0x42u,         // $8310 LDA !$4210         acknowledge the NMI
      0xA9u, 0x01u,                // $8313 LDA #$01
      0x8Du, 0x16u, 0x40u,         // $8315 STA !$4016         strobe high
      0x9Cu, 0x16u, 0x40u,         // $8318 STZ !$4016         strobe low: both pads latch
      0xA0u, 0x00u,                // $831B LDY #$00           port 1
      0x20u, 0x00u, 0x84u,         // $831D JSR $8400
      0xA0u, 0x01u,                // $8320 LDY #$01           port 2
      0x20u, 0x00u, 0x84u,         // $8322 JSR $8400
      0x40u,                       // $8325 RTI
  });
  put(rom, 0x0400u, {
      0xA2u, 0x08u,                // $8400 LDX #$08           the word's high byte, B first
      0xB9u, 0x16u, 0x40u,         // $8402 LDA !$4016,Y
      0x4Au,                       // $8405 LSR A
      0x26u, 0x00u,                // $8406 ROL $00
      0xCAu,                       // $8408 DEX
      0xD0u, 0xF7u,                // $8409 BNE $8402
      0xA2u, 0x08u,                // $840B LDX #$08           the low byte, A first
      0xB9u, 0x16u, 0x40u,         // $840D LDA !$4016,Y
      0x4Au,                       // $8410 LSR A
      0x26u, 0x01u,                // $8411 ROL $01
      0xCAu,                       // $8413 DEX
      0xD0u, 0xF7u,                // $8414 BNE $840D
      0xB9u, 0x16u, 0x40u,         // $8416 LDA !$4016,Y       the seventeenth bit
      0x4Au,                       // $8419 LSR A
      0xA9u, 0x00u,                // $841A LDA #$00
      0x2Au,                       // $841C ROL A
      0x85u, 0x02u,                // $841D STA $02
      0x46u, 0x00u,                // $841F LSR $00            the word shifted down one bit
      0x66u, 0x01u,                // $8421 ROR $01
      0xA5u, 0x01u,                // $8423 LDA $01
      0x05u, 0x02u,                // $8425 ORA $02            the seventeenth bit at the bottom
      0x85u, 0x01u,                // $8427 STA $01
      0xC8u,                       // $8429 INY                colour = port + 1
      0x8Cu, 0x21u, 0x21u,         // $842A STY !$2121
      0x8Du, 0x22u, 0x21u,         // $842D STA !$2122         low byte
      0xA5u, 0x00u,                // $8430 LDA $00
      0x8Du, 0x22u, 0x21u,         // $8432 STA !$2122         high byte
      0x60u,                       // $8435 RTS
  });
  return rom;
}

// Where each port's colour shows, and the row it is read from.
constexpr unsigned kPortOneX = 0u;
constexpr unsigned kPortTwoX = 4u;
constexpr unsigned kReadRow = 100u;

// The cartridge reads a port in a vertical blank and the picture drawn after it shows it. A run
// begins as a vertical blank does, so a change made as a run begins is on screen at its end.
constexpr std::size_t kRunsToShow = 1u;

// The RetroPad ids and the console's buttons they stand for, as libretro.h lays them out.
struct PadButton {
  unsigned id;
  snaggletooth::Button button;
};

constexpr std::array<PadButton, 12> kPadButtons{{
    {RETRO_DEVICE_ID_JOYPAD_B, snaggletooth::Button::B},
    {RETRO_DEVICE_ID_JOYPAD_Y, snaggletooth::Button::Y},
    {RETRO_DEVICE_ID_JOYPAD_SELECT, snaggletooth::Button::Select},
    {RETRO_DEVICE_ID_JOYPAD_START, snaggletooth::Button::Start},
    {RETRO_DEVICE_ID_JOYPAD_UP, snaggletooth::Button::Up},
    {RETRO_DEVICE_ID_JOYPAD_DOWN, snaggletooth::Button::Down},
    {RETRO_DEVICE_ID_JOYPAD_LEFT, snaggletooth::Button::Left},
    {RETRO_DEVICE_ID_JOYPAD_RIGHT, snaggletooth::Button::Right},
    {RETRO_DEVICE_ID_JOYPAD_A, snaggletooth::Button::A},
    {RETRO_DEVICE_ID_JOYPAD_X, snaggletooth::Button::X},
    {RETRO_DEVICE_ID_JOYPAD_L, snaggletooth::Button::L},
    {RETRO_DEVICE_ID_JOYPAD_R, snaggletooth::Button::R},
}};

// The console's pad holding the RetroPad buttons set in `held`, bit N for id N.
snaggletooth::Joypad padHolding(std::uint16_t held) {
  snaggletooth::Joypad pad;
  for (const PadButton& button : kPadButtons) {
    pad.hold(button.button, ((held >> button.id) & 1u) != 0u);
  }
  return pad;
}

// A picture as the frontend sees it: XRGB8888 words, `width` a row, rows packed.
struct Picture {
  unsigned width = 0;
  unsigned height = 0;
  std::vector<std::uint32_t> pixels;
};

// What the cartridge read from one port: the sixteen-bit word the pad shifted out, and whether
// a pad was there.
struct Reading {
  std::uint16_t word = 0;
  bool pad = false;

  bool operator==(const Reading&) const = default;
};

void PrintTo(const Reading& reading, std::ostream* out) {
  *out << (reading.pad ? "pad " : "empty ") << std::hex << std::showbase << reading.word;
}

// The reading a port's colour carries. At full brightness the machine shows a five-bit channel
// as the byte nearest value * 255 / 31, so the nearest value to byte * 31 / 255 is the one the
// cartridge wrote.
Reading readingAt(const Picture& picture, unsigned x) {
  const std::uint32_t pixel = picture.pixels[std::size_t{kReadRow} * picture.width + x];
  const auto five = [](std::uint32_t byte) { return (byte * 31u + 127u) / 255u; };
  const std::uint32_t colour = five((pixel >> 16) & 0xFFu) | (five((pixel >> 8) & 0xFFu) << 5) |
                               (five(pixel & 0xFFu) << 10);
  return Reading{.word = static_cast<std::uint16_t>((colour & ~1u) << 1),
                 .pad = (colour & 1u) != 0u};
}

Reading padReading(std::uint16_t held) {
  return Reading{.word = padHolding(held).bits(), .pad = true};
}
constexpr Reading kEmptyPort{.word = 0u, .pad = false};

// One call the core made to read input.
struct Query {
  unsigned port;
  unsigned device;
  unsigned index;
  unsigned id;
};

// What the test frontend saw the core do, and what it answers the core's questions with.
struct Frontend {
  bool bitmasks = false;                 // the answer to GET_INPUT_BITMASKS
  std::array<std::uint16_t, 2> held{};   // the RetroPad buttons held on each port, bit N for id N

  std::vector<Query> queries;
  std::vector<Picture> pictures;
  std::vector<std::vector<std::pair<std::string, unsigned>>> controllerInfo;  // per port
  std::vector<retro_input_descriptor> descriptors;
  std::vector<std::string> descriptorNames;
};

Frontend* frontend = nullptr;

bool environment(unsigned command, void* data) {
  switch (command) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
      return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
      return frontend->bitmasks;
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
      frontend->controllerInfo.clear();
      for (const auto* port = static_cast<const retro_controller_info*>(data);
           port->types != nullptr; ++port) {
        std::vector<std::pair<std::string, unsigned>> types;
        for (unsigned at = 0; at < port->num_types; ++at) {
          types.emplace_back(port->types[at].desc, port->types[at].id);
        }
        frontend->controllerInfo.push_back(std::move(types));
      }
      return true;
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
      frontend->descriptors.clear();
      frontend->descriptorNames.clear();
      for (const auto* descriptor = static_cast<const retro_input_descriptor*>(data);
           descriptor->description != nullptr; ++descriptor) {
        frontend->descriptors.push_back(*descriptor);
        frontend->descriptorNames.emplace_back(descriptor->description);
      }
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
  frontend->queries.push_back(Query{.port = port, .device = device, .index = index, .id = id});
  if (device != RETRO_DEVICE_JOYPAD || index != 0u || port >= frontend->held.size()) return 0;
  const std::uint16_t held = frontend->held[port];
  if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return static_cast<std::int16_t>(held);
  return id < 16u ? static_cast<std::int16_t>((held >> id) & 1u) : std::int16_t{0};
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
  void restart() {
    module_.entry<void()>("retro_deinit")();
    module_.entry<void()>("retro_init")();
  }
  void plug(unsigned port, unsigned device) {
    module_.entry<void(unsigned, unsigned)>("retro_set_controller_port_device")(port, device);
  }
  void run(std::size_t times = 1u) {
    for (std::size_t at = 0; at < times; ++at) module_.entry<void()>("retro_run")();
  }

 private:
  LoadedCore module_;
};

// What the last picture shows each port read.
Reading portOne(const Frontend& seen) { return readingAt(seen.pictures.back(), kPortOneX); }
Reading portTwo(const Frontend& seen) { return readingAt(seen.pictures.back(), kPortTwoX); }

// Holds each of the twelve buttons alone on one port and checks that the machine read exactly
// that button there, with the other port's pad holding nothing.
void expectEachButtonOn(unsigned port) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(padCartridge()));
  core.run(kRunsToShow);
  for (const PadButton& button : kPadButtons) {
    seen.held = {};
    seen.held[port] = static_cast<std::uint16_t>(1u << button.id);
    core.run(kRunsToShow);
    snaggletooth::Joypad alone;
    alone.hold(button.button, true);
    const Reading expected{.word = alone.bits(), .pad = true};
    const Reading idle = padReading(0u);
    EXPECT_EQ(portOne(seen), port == 0u ? expected : idle) << "id " << button.id;
    EXPECT_EQ(portTwo(seen), port == 1u ? expected : idle) << "id " << button.id;
  }
}

TEST(Controllers, EachButtonReachesTheMachineOnPortOne) { expectEachButtonOn(0u); }

TEST(Controllers, EachButtonReachesTheMachineOnPortTwo) { expectEachButtonOn(1u); }

TEST(Controllers, BothPortsHoldAPadBeforeTheFrontendChooses) {
  Frontend seen;
  seen.held = {0x0005u, 0x0300u};  // B + Select on port 1, A + X on port 2
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(padCartridge()));
  core.run(kRunsToShow + 1u);
  EXPECT_EQ(portOne(seen), padReading(0x0005u));
  EXPECT_EQ(portTwo(seen), padReading(0x0300u));
  // Every button the machine read was asked of the frontend's RetroPad.
  for (const Query& query : seen.queries) {
    EXPECT_EQ(query.device, unsigned{RETRO_DEVICE_JOYPAD});
    EXPECT_EQ(query.index, 0u);
    EXPECT_LT(query.port, 2u);
  }
}

TEST(Controllers, ANonePortIsEmptyAndIsNotRead) {
  Frontend seen;
  seen.held = {0x0800u, 0x0FFFu};  // R on port 1, every button on port 2
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(padCartridge()));
  core.plug(1u, RETRO_DEVICE_NONE);
  core.run(kRunsToShow + 1u);
  EXPECT_EQ(portOne(seen), padReading(0x0800u));
  EXPECT_EQ(portTwo(seen), kEmptyPort);
  for (const Query& query : seen.queries) EXPECT_EQ(query.port, 0u);
}

TEST(Controllers, AJoypadPutBackInAPortIsReadAgain) {
  Frontend seen;
  seen.held = {0x0000u, 0x0010u};  // Up on port 2
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(padCartridge()));
  core.plug(0u, RETRO_DEVICE_NONE);
  core.plug(1u, RETRO_DEVICE_NONE);
  core.run(kRunsToShow + 1u);
  EXPECT_EQ(portOne(seen), kEmptyPort);
  EXPECT_EQ(portTwo(seen), kEmptyPort);
  core.plug(1u, RETRO_DEVICE_JOYPAD);
  core.run(kRunsToShow);
  EXPECT_EQ(portOne(seen), kEmptyPort);
  EXPECT_EQ(portTwo(seen), padReading(0x0010u));
}

TEST(Controllers, AnUnofferedDeviceLeavesThePortEmpty) {
  Frontend seen;
  seen.held = {0x0001u, 0x0002u};
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(padCartridge()));
  core.plug(1u, RETRO_DEVICE_MOUSE);
  core.plug(2u, RETRO_DEVICE_NONE);  // the console has no third port: nothing changes
  core.run(kRunsToShow + 1u);
  EXPECT_EQ(portOne(seen), padReading(0x0001u));
  EXPECT_EQ(portTwo(seen), kEmptyPort);
}

// A run of presses that changes every frame on both ports.
std::array<std::uint16_t, 2> pressesAt(std::size_t frame) {
  return {static_cast<std::uint16_t>((frame * 0x9E5u + 0x123u) & 0x0FFFu),
          static_cast<std::uint16_t>((frame * 0x3C7u + 0x0A5u) & 0x0FFFu)};
}

// The readings a run of the pad cartridge shows under the scripted presses.
std::vector<std::array<Reading, 2>> scriptedReadings(bool bitmasks, std::vector<Query>& queries) {
  constexpr std::size_t kRuns = 40u;
  Frontend seen;
  seen.bitmasks = bitmasks;
  Core core(seen);
  std::vector<std::array<Reading, 2>> readings;
  if (!core.loaded() || !core.load(padCartridge())) return readings;
  for (std::size_t frame = 0; frame < kRuns; ++frame) {
    seen.held = pressesAt(frame);
    core.run();
    readings.push_back({portOne(seen), portTwo(seen)});
  }
  queries = std::move(seen.queries);
  return readings;
}

TEST(Controllers, BitmaskAndSingleButtonReportsAgree) {
  std::vector<Query> masked;
  std::vector<Query> single;
  const std::vector<std::array<Reading, 2>> withMasks = scriptedReadings(true, masked);
  const std::vector<std::array<Reading, 2>> without = scriptedReadings(false, single);
  ASSERT_EQ(withMasks.size(), 40u);
  EXPECT_EQ(withMasks, without);
  // Each run's presses are on screen as it ends, from the second run: the first, from power-on to
  // the first vertical blank, reads no pad.
  for (std::size_t frame = 1; frame + kRunsToShow <= withMasks.size(); ++frame) {
    const std::array<std::uint16_t, 2> held = pressesAt(frame);
    EXPECT_EQ(withMasks[frame + kRunsToShow - 1u][0], padReading(held[0])) << "frame " << frame;
    EXPECT_EQ(withMasks[frame + kRunsToShow - 1u][1], padReading(held[1])) << "frame " << frame;
  }
  // A frontend that reports bitmasks is asked once a port each run; one that does not is asked
  // for each of the twelve buttons.
  ASSERT_EQ(masked.size(), 2u * 40u);
  for (const Query& query : masked) EXPECT_EQ(query.id, unsigned{RETRO_DEVICE_ID_JOYPAD_MASK});
  ASSERT_EQ(single.size(), 2u * 12u * 40u);
  for (const Query& query : single) EXPECT_LT(query.id, 12u);
}

TEST(Controllers, FramesMatchAReferenceMachineGivenTheSamePads) {
  constexpr std::size_t kRuns = 30u;
  const std::vector<std::uint8_t> rom = padCartridge();

  // The reference: a machine run here, handed each frame's pads as that frame begins, its
  // frame ending where the machine reports it.
  struct Done {};
  struct Recorder final : snaggletooth::FrameObserver {
    void frame(const snaggletooth::VideoFrame& frame) override {
      Picture picture{
          .width = frame.width,
          .height = frame.height,
          .pixels = std::vector<std::uint32_t>(std::size_t{frame.width} * frame.height)};
      for (std::size_t at = 0; at < picture.pixels.size(); ++at) {
        const std::uint8_t* rgba = frame.pixels.data() + at * 4u;
        picture.pixels[at] = (static_cast<std::uint32_t>(rgba[0]) << 16) |
                             (static_cast<std::uint32_t>(rgba[1]) << 8) | rgba[2];
      }
      pictures.push_back(std::move(picture));
      throw Done{};
    }
    std::vector<Picture> pictures;
  };
  snaggletooth::Snes machine(snaggletooth::SnesConfig{.rom = rom});
  Recorder recorder;
  machine.setFrameObserver(&recorder);
  const std::uint64_t frameCycles =
      snaggletooth::consoleClock(machine.region()).masterCyclesPerFrame;
  for (std::size_t frame = 0; frame < kRuns; ++frame) {
    const std::array<std::uint16_t, 2> held = pressesAt(frame);
    machine.setJoypad(snaggletooth::JoypadPort::One, padHolding(held[0]));
    machine.setJoypad(snaggletooth::JoypadPort::Two, padHolding(held[1]));
    try {
      for (;;) machine.run(frameCycles);
    } catch (const Done&) {
    }
  }

  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(rom));
  for (std::size_t frame = 0; frame < kRuns; ++frame) {
    seen.held = pressesAt(frame);
    core.run();
  }
  ASSERT_EQ(seen.pictures.size(), kRuns);
  ASSERT_EQ(recorder.pictures.size(), kRuns);
  for (std::size_t frame = 0; frame < kRuns; ++frame) {
    ASSERT_EQ(seen.pictures[frame].width, recorder.pictures[frame].width) << "frame " << frame;
    ASSERT_EQ(seen.pictures[frame].height, recorder.pictures[frame].height) << "frame " << frame;
    std::size_t differing = 0;
    for (std::size_t at = 0; at < seen.pictures[frame].pixels.size(); ++at) {
      if ((seen.pictures[frame].pixels[at] & 0x00FFFFFFu) != recorder.pictures[frame].pixels[at]) {
        ++differing;
      }
    }
    EXPECT_EQ(differing, 0u) << "frame " << frame;
  }
}

TEST(Controllers, AChoiceHoldsAcrossResetAndReloadUntilTheCoreRestarts) {
  Frontend seen;
  seen.held = {0x0040u, 0x0080u};  // Left on port 1, Right on port 2
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  const std::vector<std::uint8_t> rom = padCartridge();
  ASSERT_TRUE(core.load(rom));
  core.plug(1u, RETRO_DEVICE_NONE);
  core.run(kRunsToShow + 1u);
  ASSERT_EQ(portTwo(seen), kEmptyPort);

  core.reset();
  core.run(kRunsToShow + 1u);
  EXPECT_EQ(portOne(seen), padReading(0x0040u));
  EXPECT_EQ(portTwo(seen), kEmptyPort);

  core.unload();
  ASSERT_TRUE(core.load(rom));
  core.run(kRunsToShow + 1u);
  EXPECT_EQ(portOne(seen), padReading(0x0040u));
  EXPECT_EQ(portTwo(seen), kEmptyPort);

  core.unload();
  core.restart();
  ASSERT_TRUE(core.load(rom));
  core.run(kRunsToShow + 1u);
  EXPECT_EQ(portOne(seen), padReading(0x0040u));
  EXPECT_EQ(portTwo(seen), padReading(0x0080u));
}

TEST(Controllers, EachPortOffersTheJoypadAndNone) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(padCartridge()));
  const std::vector<std::pair<std::string, unsigned>> offered{
      {"SNES Joypad", RETRO_DEVICE_JOYPAD}, {"None", RETRO_DEVICE_NONE}};
  ASSERT_EQ(seen.controllerInfo.size(), 2u);
  EXPECT_EQ(seen.controllerInfo[0], offered);
  EXPECT_EQ(seen.controllerInfo[1], offered);
}

TEST(Controllers, EveryButtonIsNamedOnBothPorts) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(padCartridge()));
  const std::array<const char*, 12> names{"B", "Y", "Select", "Start", "D-Pad Up", "D-Pad Down",
                                          "D-Pad Left", "D-Pad Right", "A", "X", "L", "R"};
  ASSERT_EQ(seen.descriptors.size(), 24u);
  for (unsigned port = 0; port < 2u; ++port) {
    for (std::size_t at = 0; at < kPadButtons.size(); ++at) {
      const std::size_t entry = port * kPadButtons.size() + at;
      const retro_input_descriptor& descriptor = seen.descriptors[entry];
      EXPECT_EQ(descriptor.port, port);
      EXPECT_EQ(descriptor.device, unsigned{RETRO_DEVICE_JOYPAD});
      EXPECT_EQ(descriptor.index, 0u);
      EXPECT_EQ(descriptor.id, kPadButtons[at].id);
      EXPECT_EQ(seen.descriptorNames[entry], names[at]);
    }
  }
}

}  // namespace
