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

// One code of a cheat: the byte it changes and how.
struct Part {
  std::uint32_t address = 0;
  std::uint8_t value = 0;
  CheatKind kind = CheatKind::EveryFrame;
  std::optional<std::uint8_t> original;  // the ROM byte a patch in place replaced
};

// A cheat as the frontend hands it over: one code, or several joined with `+`, on or off as one.
struct Cheat {
  std::string code;  // as the frontend handed it over, for the message that refuses it
  std::vector<Part> parts;
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

std::optional<Part> gameGenie(std::string_view text) {
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
  return Part{.address = address,
              .value = static_cast<std::uint8_t>(word >> 24),
              .kind = CheatKind::RomPatch,
              .original = std::nullopt};
}

std::optional<Part> addressAndValue(std::string_view address, std::string_view value) {
  const std::optional<std::uint32_t> where = readHex(address);
  const std::optional<std::uint32_t> what = readHex(value);
  if (!where || !what) return std::nullopt;
  return Part{.address = *where,
              .value = static_cast<std::uint8_t>(*what),
              .kind = CheatKind::EveryFrame,
              .original = std::nullopt};
}

constexpr std::string_view kWhitespace = " \t\r\n\f\v";

// `text` without the whitespace at either end.
std::string_view trimmed(std::string_view text) {
  const std::size_t first = text.find_first_not_of(kWhitespace);
  if (first == std::string_view::npos) return {};
  return text.substr(first, text.find_last_not_of(kWhitespace) - first + 1u);
}

// The code `text` names: `XXYY-ZZZZ` as Game Genie, `AAAAAAVV` as Pro Action Replay,
// `AAAAAA:VV` as raw, and nothing for any other string. Every Game Genie letter is a hex digit
// too, so eight characters with no hyphen are a Pro Action Replay code.
std::optional<Part> decodeOne(std::string_view text) {
  if (text.size() == kGameGenieLength && text[kGameGenieHyphen] == '-') return gameGenie(text);
  if (text.size() == kActionReplayLength) {
    return addressAndValue(text.substr(0, kAddressDigits), text.substr(kAddressDigits));
  }
  if (text.size() == kRawLength && text[kAddressDigits] == ':') {
    return addressAndValue(text.substr(0, kAddressDigits), text.substr(kAddressDigits + 1u));
  }
  return std::nullopt;
}

// The cheat `code` names: one code, or several joined with `+`, each read without the
// whitespace around it. A string with a part in none of the three forms names nothing.
std::optional<Cheat> decode(std::string_view code) {
  Cheat cheat{.code = std::string(code), .parts = {}, .on = false};
  std::string_view rest = code;
  for (;;) {
    const std::size_t plus = rest.find('+');
    const std::optional<Part> part = decodeOne(trimmed(rest.substr(0, plus)));
    if (!part) return std::nullopt;
    cheat.parts.push_back(*part);
    if (plus == std::string_view::npos) return cheat;
    rest = rest.substr(plus + 1u);
  }
}

// Whether the cartridge in `machine` has the memory every part writes: ROM for a patch, and any
// memory the bus reaches for an every-frame write.
bool fits(const Cheat& cheat, const snaggletooth::Snes& machine) {
  for (const Part& part : cheat.parts) {
    if (part.kind == CheatKind::RomPatch) {
      if (machine.physical(part.address).space != snaggletooth::Snes::Space::CartridgeRom) {
        return false;
      }
    } else if (!machine.addressable(part.address, 1u)) {
      return false;
    }
  }
  return true;
}

void refuse(std::string_view code) {
  tellPlayer("Snaggletooth cannot read cheat code \"" + std::string(code) + "\".");
}

// The ROM patches of a cheat that is on go into the image, each keeping the byte it replaces.
void patch(Cheat& cheat, snaggletooth::Snes& machine) {
  if (!cheat.on) return;
  for (Part& part : cheat.parts) {
    if (part.kind != CheatKind::RomPatch) continue;
    part.original = machine.peek(part.address);
    machine.poke(part.address, part.value);
  }
}

// The ROM patches in place give the image back the bytes they replaced, last patch first.
void putBack(Cheat& cheat, snaggletooth::Snes& machine) {
  for (auto part = cheat.parts.rbegin(); part != cheat.parts.rend(); ++part) {
    if (part->kind != CheatKind::RomPatch || !part->original) continue;
    machine.poke(part->address, *part->original);
    part->original.reset();
  }
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
    if (!held || !held->on) continue;
    for (const Part& part : held->parts) {
      if (part.kind == CheatKind::EveryFrame) machine.poke(part.address, part.value);
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
