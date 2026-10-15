// The console's bus as the frontend is told it: where the work RAM and the save the frontend is
// handed sit at the console's own addresses, for achievements and the frontend's cheat search.

#pragma once

#include <libretro.h>

#include "snaggletooth/snes/snes.h"

#include <vector>

namespace snaggletooth_libretro {

// The descriptors of the console's bus a frontend takes through SET_MEMORY_MAPS: the work RAM
// at $7E0000 with its low 8 KB repeated at $0000-$1FFF of every system bank, and the save in
// the windows the cartridge's map puts it in, each window pointing into the buffer the frontend
// is handed for that memory. The windows are read off the machine itself, page by page, so the
// map names what the machine reaches. A cartridge with no save has no save window. The
// descriptors stay where they are for as long as the cartridge is loaded.
class MemoryMap {
 public:
  MemoryMap(const snaggletooth::Snes& machine, void* workRam, void* save);
  MemoryMap(const MemoryMap&) = delete;
  MemoryMap& operator=(const MemoryMap&) = delete;

  // Hands the frontend the map. A frontend that does not take one is left as it is.
  void tell() const;

 private:
  std::vector<retro_memory_descriptor> descriptors_;
  retro_memory_map map_{};
};

}  // namespace snaggletooth_libretro
