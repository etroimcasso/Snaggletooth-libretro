// The core as a frontend first meets it: the module loads, answers the libretro API version
// it implements, and names itself and the library it was built from.

#include <libretro.h>

#include "snaggletooth/version.h"

#include <gtest/gtest.h>

#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

// The built module, loaded at run time the way a frontend loads it. Each lookup resolves an
// exported entry point by name, so a symbol the module fails to export fails the case.
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

TEST(CoreIdentity, ModuleLoads) {
  const LoadedCore core;
  EXPECT_TRUE(core.loaded()) << SNAGGLETOOTH_LIBRETRO_PATH;
}

TEST(CoreIdentity, ApiVersionIsTheHeaders) {
  const LoadedCore core;
  ASSERT_TRUE(core.loaded());
  auto* apiVersion = core.entry<unsigned()>("retro_api_version");
  ASSERT_NE(apiVersion, nullptr);
  EXPECT_EQ(apiVersion(), 1u);
}

TEST(CoreIdentity, SystemInfoNamesTheCoreAndTheLibrary) {
  const LoadedCore core;
  ASSERT_TRUE(core.loaded());
  auto* systemInfo = core.entry<void(retro_system_info*)>("retro_get_system_info");
  ASSERT_NE(systemInfo, nullptr);

  retro_system_info info{};
  systemInfo(&info);
  ASSERT_NE(info.library_name, nullptr);
  ASSERT_NE(info.library_version, nullptr);
  EXPECT_STREQ(info.library_name, "Snaggletooth");
  const std::string expected = std::string(SNAGGLETOOTH_LIBRETRO_VERSION) + " (Snaggletooth " +
                               std::string(snaggletooth::version()) + ")";
  EXPECT_EQ(std::string(info.library_version), expected);
}

TEST(CoreIdentity, SystemInfoIsStableAcrossCalls) {
  const LoadedCore core;
  ASSERT_TRUE(core.loaded());
  auto* systemInfo = core.entry<void(retro_system_info*)>("retro_get_system_info");
  ASSERT_NE(systemInfo, nullptr);

  retro_system_info first{};
  retro_system_info second{};
  systemInfo(&first);
  systemInfo(&second);
  EXPECT_EQ(first.library_name, second.library_name);
  EXPECT_EQ(first.library_version, second.library_version);
}

}  // namespace
