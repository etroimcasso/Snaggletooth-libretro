#pragma once

// The built module, loaded at run time the way a frontend loads it. Each lookup resolves an
// exported entry point by name, so a symbol the module fails to export fails the case.

#ifdef _WIN32
// Keeps <windows.h> from defining min and max as macros, which would break std::min and
// std::max in every file that includes this one.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace snaggletooth_libretro_test {

class LoadedCore {
 public:
  LoadedCore() {
#ifdef _WIN32
    handle_ = LoadLibraryA(SNAGGLETOOTH_LIBRETRO_PATH);
#else
    handle_ = dlopen(SNAGGLETOOTH_LIBRETRO_PATH, RTLD_NOW | RTLD_LOCAL);
#endif
  }
  LoadedCore(const LoadedCore&) = delete;
  LoadedCore& operator=(const LoadedCore&) = delete;
  ~LoadedCore() {
    if (handle_ == nullptr) {
      return;
    }
#ifdef _WIN32
    FreeLibrary(handle_);
#else
    dlclose(handle_);
#endif
  }

  [[nodiscard]] bool loaded() const noexcept { return handle_ != nullptr; }

  template <typename Function>
  [[nodiscard]] Function* entry(const char* name) const {
#ifdef _WIN32
    return reinterpret_cast<Function*>(reinterpret_cast<void*>(GetProcAddress(handle_, name)));
#else
    return reinterpret_cast<Function*>(dlsym(handle_, name));
#endif
  }

 private:
#ifdef _WIN32
  HMODULE handle_ = nullptr;
#else
  void* handle_ = nullptr;
#endif
};

}  // namespace snaggletooth_libretro_test
