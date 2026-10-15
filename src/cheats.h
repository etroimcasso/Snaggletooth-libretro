// The player's cheat codes, as the frontend hands them over, and the bytes they change in the
// running console.
//
// A code is one of three forms. A Game Genie code, `XXYY-ZZZZ` in the device's own letters,
// patches a byte of the cartridge's ROM in the machine's copy of the image, and the byte it
// replaced is put back when the code is turned off. A Pro Action Replay code, `AAAAAAVV`, and a
// raw code, `AAAAAA:VV`, name a bus address and a byte that is written there before every
// frame. Every byte goes through the machine's own peek and poke.

#pragma once

#include "snaggletooth/snes/snes.h"

namespace snaggletooth_libretro {

// Puts `code` at `index` in the list of cheats, on or off, in place of what the index held; a
// ROM patch the index held is put back first. With a machine, a ROM patch turned on takes effect
// at once and an every-frame write from the next frame. With none, as before a cartridge loads,
// only the list changes and the code takes effect when one loads. A code in none of the three
// forms, or naming an address the cartridge leaves without memory (or, for Game Genie, outside
// its ROM), is refused with a message to the player and leaves the index empty; a null code
// empties it silently.
void setCheat(unsigned index, bool enabled, const char* code, snaggletooth::Snes* machine);

// Turns every cheat off: each ROM patch is put back, last index first, and the list emptied.
void resetCheats(snaggletooth::Snes* machine);

// Empties the list without touching any machine, for when the machine goes away.
void forgetCheats();

// Writes the byte of every every-frame code that is on. Called before each frame, after the
// frontend's own writes, so a code wins over a write to the same byte.
void applyCheats(snaggletooth::Snes& machine);

// Applies every ROM patch that is on to a machine holding the plain image, as a freshly loaded
// cartridge or a machine rebuilt on other threads does, reading each replaced byte from it. A
// code that names no memory on this cartridge is refused here, as setCheat refuses it.
void patchRom(snaggletooth::Snes& machine);

}  // namespace snaggletooth_libretro
