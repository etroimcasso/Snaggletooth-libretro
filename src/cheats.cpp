#include "cheats.h"

#include "frontend.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace snaggletooth_libretro {

namespace {

// A Game Genie code patches the cartridge's ROM; the other two forms write a byte before every
// frame.
enum class CheatKind { RomPatch, EveryFrame };

struct Cheat {
  std::string code;  // as the frontend handed it over, for the message that refuses it
  std::uint32_t address = 0;
  std::uint8_t value = 0;
  CheatKind kind = CheatKind::EveryFrame;
  std::optional<std::uint8_t> original;  // the ROM byte a patch in place replaced
  bool on = false;
};

// The frontend's list, by the index it gives each code. An index the frontend has not filled, or
// whose code was refused, is empty.
std::vector<std::optional<Cheat>> cheats;

// The Game Genie's sixteen letters, each standing for the hex digit of its place here.
constexpr std::string_view kGameGenieLetters = "DF4709156BC8A23E";

// Where each bit of a Game Genie code's address comes from, from the address's top bit down,
// among the 24 bits after the value: with those written abcd efgh ijkl mnop qrst uvwx from the
// top, the address is ijkl qrst opab cduv wxef ghmn.
constexpr std::array<unsigned, 24> kGameGenieAddressBits{
    15u, 14u, 13u, 12u, 7u, 6u, 5u, 4u, 9u, 8u, 23u, 22u,
    21u, 20u, 3u, 2u, 1u, 0u, 19u, 18u, 17u, 16u, 11u, 10u,
};

// The parts of each form: `AAAAAAVV` and `AAAAAA:VV` carry six hex digits of address and two of
// value; `XXYY-ZZZZ` carries its hyphen after the fourth letter.
constexpr std::size_t kAddressDigits = 6u;
constexpr std::size_t kValueDigits = 2u;
constexpr std::size_t kGameGenieHyphen = 4u;
constexpr std::size_t kGameGenieLength = 9u;
constexpr std::size_t kActionReplayLength = kAddressDigits + kValueDigits;
constexpr std::size_t kRawLength = kAddressDigits + 1u + kValueDigits;

constexpr std::uint32_t kBusMask = 0xFFFFFFu;

char upper(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; }

std::optional<std::uint32_t> hexDigit(char c) {
  if (c >= '0' && c <= '9') return static_cast<std::uint32_t>(c - '0');
  const char letter = upper(c);
  if (letter >= 'A' && letter <= 'F') return static_cast<std::uint32_t>(letter - 'A' + 10);
  return std::nullopt;
}

std::optional<std::uint32_t> gameGenieDigit(char c) {
  const std::size_t at = kGameGenieLetters.find(upper(c));
  if (at == std::string_view::npos) return std::nullopt;
  return static_cast<std::uint32_t>(at);
}

// The number `text` spells in hex, or nothing when a character is not a hex digit.
std::optional<std::uint32_t> readHex(std::string_view text) {
  std::uint32_t number = 0;
  for (const char c : text) {
    const std::optional<std::uint32_t> digit = hexDigit(c);
    if (!digit) return std::nullopt;
    number = number << 4 | *digit;
  }
  return number;
}

std::optional<Cheat> gameGenie(std::string_view text) {
  std::uint32_t word = 0;
  for (std::size_t at = 0; at < text.size(); ++at) {
    if (at == kGameGenieHyphen) continue;
    const std::optional<std::uint32_t> digit = gameGenieDigit(text[at]);
    if (!digit) return std::nullopt;
    word = word << 4 | *digit;
  }
  const std::uint32_t scrambled = word & kBusMask;
  std::uint32_t address = 0;
  for (const unsigned from : kGameGenieAddressBits) address = address << 1 | (scrambled >> from & 1u);
  return Cheat{.code = {},
               .address = address,
               .value = static_cast<std::uint8_t>(word >> 24),
               .kind = CheatKind::RomPatch,
               .original = std::nullopt,
               .on = false};
}

std::optional<Cheat> addressAndValue(std::string_view address, std::string_view value) {
  const std::optional<std::uint32_t> where = readHex(address);
  const std::optional<std::uint32_t> what = readHex(value);
  if (!where || !what) return std::nullopt;
  return Cheat{.code = {},
               .address = *where,
               .value = static_cast<std::uint8_t>(*what),
               .kind = CheatKind::EveryFrame,
               .original = std::nullopt,
               .on = false};
}

// The code `code` names, read without the whitespace at either end: `XXYY-ZZZZ` as Game Genie,
// `AAAAAAVV` as Pro Action Replay, `AAAAAA:VV` as raw, and nothing for any other string. Every
// Game Genie letter is a hex digit too, so eight characters with no hyphen are a Pro Action
// Replay code.
std::optional<Cheat> decode(std::string_view code) {
  constexpr std::string_view kWhitespace = " \t\r\n\f\v";
  const std::size_t first = code.find_first_not_of(kWhitespace);
  if (first == std::string_view::npos) return std::nullopt;
  const std::string_view text = code.substr(first, code.find_last_not_of(kWhitespace) - first + 1u);

  std::optional<Cheat> cheat;
  if (text.size() == kGameGenieLength && text[kGameGenieHyphen] == '-') {
    cheat = gameGenie(text);
  } else if (text.size() == kActionReplayLength) {
    cheat = addressAndValue(text.substr(0, kAddressDigits), text.substr(kAddressDigits));
  } else if (text.size() == kRawLength && text[kAddressDigits] == ':') {
    cheat = addressAndValue(text.substr(0, kAddressDigits), text.substr(kAddressDigits + 1u));
  }
  if (cheat) cheat->code = std::string(code);
  return cheat;
}

// Whether the cartridge in `machine` has the memory a code writes: ROM for a patch, and any
// memory the bus reaches for an every-frame write.
bool fits(const Cheat& cheat, const snaggletooth::Snes& machine) {
  if (cheat.kind == CheatKind::RomPatch) {
    return machine.physical(cheat.address).space == snaggletooth::Snes::Space::CartridgeRom;
  }
  return machine.addressable(cheat.address, 1u);
}

void refuse(std::string_view code) {
  tellPlayer("Snaggletooth cannot read cheat code \"" + std::string(code) + "\".");
}

// A ROM patch that is on goes into the image, keeping the byte it replaces.
void patch(Cheat& cheat, snaggletooth::Snes& machine) {
  if (!cheat.on || cheat.kind != CheatKind::RomPatch) return;
  cheat.original = machine.peek(cheat.address);
  machine.poke(cheat.address, cheat.value);
}

// A ROM patch in place gives the image back the byte it replaced.
void putBack(Cheat& cheat, snaggletooth::Snes& machine) {
  if (cheat.kind != CheatKind::RomPatch || !cheat.original) return;
  machine.poke(cheat.address, *cheat.original);
  cheat.original.reset();
}

}  // namespace

void setCheat(unsigned index, bool enabled, const char* code, snaggletooth::Snes* machine) {
  if (index >= cheats.size()) cheats.resize(static_cast<std::size_t>(index) + 1u);
  std::optional<Cheat>& held = cheats[index];
  if (held && machine != nullptr) putBack(*held, *machine);
  held.reset();
  if (code == nullptr) return;
  std::optional<Cheat> cheat = decode(code);
  if (!cheat || (machine != nullptr && !fits(*cheat, *machine))) {
    refuse(code);
    return;
  }
  cheat->on = enabled;
  if (machine != nullptr) patch(*cheat, *machine);
  held = std::move(cheat);
}

void resetCheats(snaggletooth::Snes* machine) {
  if (machine != nullptr) {
    for (auto held = cheats.rbegin(); held != cheats.rend(); ++held) {
      if (*held) putBack(**held, *machine);
    }
  }
  cheats.clear();
}

void forgetCheats() { cheats.clear(); }

void applyCheats(snaggletooth::Snes& machine) {
  for (const std::optional<Cheat>& held : cheats) {
    if (held && held->on && held->kind == CheatKind::EveryFrame) {
      machine.poke(held->address, held->value);
    }
  }
}

void patchRom(snaggletooth::Snes& machine) {
  for (std::optional<Cheat>& held : cheats) {
    if (!held) continue;
    if (!fits(*held, machine)) {
      refuse(held->code);
      held.reset();
      continue;
    }
    patch(*held, machine);
  }
}

}  // namespace snaggletooth_libretro
