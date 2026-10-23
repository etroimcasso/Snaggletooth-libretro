#pragma once

// What a frontend should be shown: a cartridge that draws a known picture in the screen mode and
// SETINI it is given, the frames a second machine draws of it on the processor, and the pictures
// those frames make under each interlace option. The cases that hold the GPU's pictures to the
// processor's share them.

#include "examples/common.h"
#include "snaggletooth/snes/snes.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace snaggletooth_libretro_test {

// Register values for the picture cartridge: BGMODE 0 draws BG1 at two bits a pixel and
// BGMODE 5 in half-pixels; SETINI bit 0 asks for an interlaced picture and bit 2 for the
// 239-line one.
constexpr std::uint8_t kModeZero = 0x00u;
constexpr std::uint8_t kModeFive = 0x05u;
constexpr std::uint8_t kProgressive = 0x00u;
constexpr std::uint8_t kInterlaced = 0x01u;
constexpr std::uint8_t kTaller = 0x04u;

// A cartridge that draws BG1 from one 2-bit tile whose columns run through colours 0, 1, 2, 3
// twice — the backdrop, red, green, blue — across the whole screen, in the screen mode and
// SETINI it is given. Its vertical-blank handler writes the frame count into colour 0, so every
// frame's backdrop differs from the last and two fields of an interlaced picture never match.
inline std::vector<std::uint8_t> pictureCartridge(std::uint8_t bgmode, std::uint8_t setini) {
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

// The first `count` frames a cartridge draws, from a machine run here on the processor.
inline std::vector<Field> referenceFields(const std::vector<std::uint8_t>& rom, std::size_t count) {
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

  [[nodiscard]] bool operator==(const Picture&) const = default;
};

// XRGB8888 is one native 32-bit word a pixel, red in bits 16-23, green in 8-15, blue in 0-7;
// the top byte is not part of the colour.
constexpr std::uint32_t kColourBits = 0x00FFFFFFu;

inline std::uint32_t xrgb(const std::uint8_t* rgba) {
  return (static_cast<std::uint32_t>(rgba[0]) << 16) | (static_cast<std::uint32_t>(rgba[1]) << 8) |
         static_cast<std::uint32_t>(rgba[2]);
}

enum class Mode { Weave, Bob, Off };

// The pictures a run of these fields shows under `mode`. A frame the cartridge did not
// interlace, or any under Off, is shown as drawn. A field of an interlaced run lands in the
// full-height picture at line i -> row 2i + parity; Weave leaves the other parity's rows as the
// last field of that parity left them (black from the moment the shape began), Bob fills them
// from this field.
inline std::vector<Picture> expectedPictures(const std::vector<Field>& fields, Mode mode) {
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
inline std::vector<Picture> expectedFor(const std::vector<std::uint8_t>& rom, std::size_t runs,
                                        Mode mode) {
  return expectedPictures(referenceFields(rom, runs), mode);
}

// Every picture the core handed over matches the expected one: the same shape, and the same
// colour at every pixel.
inline void expectPictures(const std::vector<Picture>& seen, const std::vector<Picture>& expected) {
  ASSERT_EQ(seen.size(), expected.size());
  for (std::size_t at = 0; at < seen.size(); ++at) {
    ASSERT_EQ(seen[at].width, expected[at].width) << "frame " << at;
    ASSERT_EQ(seen[at].height, expected[at].height) << "frame " << at;
    std::size_t differing = 0;
    for (std::size_t pixel = 0; pixel < seen[at].pixels.size(); ++pixel) {
      if (seen[at].pixels[pixel] != expected[at].pixels[pixel]) ++differing;
    }
    EXPECT_EQ(differing, 0u) << "frame " << at;
  }
}

}  // namespace snaggletooth_libretro_test
