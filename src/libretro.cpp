// The libretro face of Snaggletooth: the entry points a libretro frontend loads the core
// through. The library defines the console; this file is the application around it, owning
// what the frontend hands in and what it is handed back.

#include <libretro.h>

#include "snaggletooth/version.h"

#include <string>

namespace {

// The core's version followed by the version of the library it was built from, so a report
// from a frontend names both. Built once, on the first call that asks for it; the frontend
// keeps the pointer for the life of the process.
const char* libraryVersion() {
  static const std::string text = std::string(SNAGGLETOOTH_LIBRETRO_VERSION) + " (Snaggletooth " +
                                  std::string(snaggletooth::version()) + ")";
  return text.c_str();
}

}  // namespace

RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }

RETRO_API void retro_get_system_info(struct retro_system_info* info) {
  *info = retro_system_info{};
  info->library_name = "Snaggletooth";
  info->library_version = libraryVersion();
}
