#include "memory_map.h"

#include "frontend.h"
#include "work_ram.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace snaggletooth_libretro {

namespace {

// The grain the save windows and the work-RAM mirror are laid out on: 8 KB pages, eight to a
// bank, 256 banks on the 24-bit bus.
constexpr std::uint32_t kPageBytes = 0x2000u;
constexpr std::uint32_t kPagesPerBank = 8u;
constexpr std::uint32_t kBanks = 256u;
constexpr std::uint32_t kBusMask = 0xFFFFFFu;

// A run of pages in one bank that read the save one after another.
struct SaveRun {
  std::uint32_t firstPage;  // the bank's page the run starts at
  std::uint32_t pages;
  std::size_t saveOffset;  // where in the save the run's first page begins
};

// The descriptor for `pages` pages from `start`, aligned to their own size, reading the save
// from `saveOffset`. A window larger than the save reads it again and again, which the
// descriptor says by a length of the save's size: the frontend drops an address's high bits
// until it is within the length.
retro_memory_descriptor saveWindow(std::uint32_t start, std::uint32_t pages,
                                   std::size_t saveOffset, std::size_t saveBytes, void* save) {
  const std::uint32_t bytes = pages * kPageBytes;
  return retro_memory_descriptor{.flags = RETRO_MEMDESC_SAVE_RAM,
                                 .ptr = save,
                                 .offset = saveOffset,
                                 .start = start,
                                 .select = kBusMask & ~(bytes - 1u),
                                 .disconnect = 0u,
                                 .len = std::min(saveBytes, static_cast<std::size_t>(bytes)),
                                 .addrspace = "SRAM"};
}

}  // namespace

MemoryMap::MemoryMap(const snaggletooth::Snes& machine, void* workRam, void* save) {
  descriptors_.push_back(retro_memory_descriptor{.flags = RETRO_MEMDESC_SYSTEM_RAM,
                                                 .ptr = workRam,
                                                 .offset = 0u,
                                                 .start = kWorkRamStart,
                                                 .select = 0u,
                                                 .disconnect = 0u,
                                                 .len = kWorkRamBytes,
                                                 .addrspace = "WRAM"});
  // The low 8 KB again at $0000-$1FFF of banks $00-$3F and $80-$BF.
  for (const std::uint32_t start : {0x000000u, 0x800000u}) {
    descriptors_.push_back(retro_memory_descriptor{.flags = RETRO_MEMDESC_SYSTEM_RAM,
                                                   .ptr = workRam,
                                                   .offset = 0u,
                                                   .start = start,
                                                   .select = 0xC0E000u,
                                                   .disconnect = 0u,
                                                   .len = kPageBytes,
                                                   .addrspace = "WRAM"});
  }

  // Each bank's pages that reach the save, merged into runs, then cut into windows aligned to
  // their own size so one start and one select mask name each.
  const std::size_t saveBytes = machine.state().sram.size();
  for (std::uint32_t bank = 0; bank < kBanks; ++bank) {
    SaveRun run{.firstPage = 0u, .pages = 0u, .saveOffset = 0u};
    const auto close = [&] {
      std::uint32_t page = run.firstPage;
      std::uint32_t left = run.pages;
      while (left != 0u) {
        std::uint32_t pages = std::bit_floor(left);
        while (page % pages != 0u) pages /= 2u;
        const std::size_t offset =
            (run.saveOffset + static_cast<std::size_t>(page - run.firstPage) * kPageBytes) %
            saveBytes;
        descriptors_.push_back(
            saveWindow((bank << 16) | (page * kPageBytes), pages, offset, saveBytes, save));
        page += pages;
        left -= pages;
      }
      run.pages = 0u;
    };
    for (std::uint32_t page = 0; page < kPagesPerBank; ++page) {
      const snaggletooth::Snes::Physical byte = machine.physical((bank << 16) | (page * kPageBytes));
      const bool continues =
          run.pages != 0u && page == run.firstPage + run.pages &&
          byte.index == (run.saveOffset + static_cast<std::size_t>(run.pages) * kPageBytes) % saveBytes;
      if (byte.space != snaggletooth::Snes::Space::SaveRam) {
        if (run.pages != 0u) close();
        continue;
      }
      if (continues) {
        ++run.pages;
        continue;
      }
      if (run.pages != 0u) close();
      run = SaveRun{.firstPage = page, .pages = 1u, .saveOffset = byte.index};
    }
    if (run.pages != 0u) close();
  }

  // The last descriptor tells the frontend how large the bus is.
  descriptors_.push_back(retro_memory_descriptor{.flags = 0u,
                                                 .ptr = nullptr,
                                                 .offset = 0u,
                                                 .start = 0u,
                                                 .select = kBusMask,
                                                 .disconnect = 0u,
                                                 .len = 0u,
                                                 .addrspace = nullptr});
  map_ = retro_memory_map{.descriptors = descriptors_.data(),
                          .num_descriptors = static_cast<unsigned>(descriptors_.size())};
}

void MemoryMap::tell() const {
  if (environment == nullptr) return;
  environment(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, const_cast<retro_memory_map*>(&map_));
}

}  // namespace snaggletooth_libretro
