#include "cartridge.h"

#include <optional>

namespace snaggletooth_libretro {

std::vector<std::uint8_t> cartridgeImage(std::span<const std::uint8_t> content) {
  if (snaggletooth::readCopierHeader(content)) {
    content = content.subspan(snaggletooth::kCopierHeaderBytes);
  }
  return {content.begin(), content.end()};
}

snaggletooth::Region regionOf(std::span<const std::uint8_t> rom) {
  const std::optional<snaggletooth::CartridgeHeader> header = snaggletooth::parseCartridgeHeader(rom);
  if (header && header->video == snaggletooth::VideoStandard::Pal) return snaggletooth::Region::Pal;
  return snaggletooth::Region::Ntsc;
}

const char* chipName(snaggletooth::Coprocessor chip) {
  using snaggletooth::Coprocessor;
  switch (chip) {
    case Coprocessor::None: return "no coprocessor";
    case Coprocessor::Dsp: return "a DSP coprocessor (DSP-1 to DSP-4)";
    case Coprocessor::Gsu: return "the SuperFX coprocessor";
    case Coprocessor::Obc1: return "the OBC1 coprocessor";
    case Coprocessor::Sa1: return "the SA-1 coprocessor";
    case Coprocessor::Sdd1: return "the S-DD1 coprocessor";
    case Coprocessor::Srtc: return "the S-RTC real-time clock";
    case Coprocessor::Other: return "a Super Game Boy or Satellaview chip";
    case Coprocessor::Spc7110: return "the SPC7110 coprocessor";
    case Coprocessor::St010: return "the ST010 or ST011 coprocessor";
    case Coprocessor::St018: return "the ST018 coprocessor";
    case Coprocessor::Cx4: return "the CX4 coprocessor";
    case Coprocessor::Unknown: return "a chip its header does not identify";
  }
  return "a chip its header does not identify";
}

}  // namespace snaggletooth_libretro
