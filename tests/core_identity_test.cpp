// The core as a frontend first meets it: the module loads, answers the libretro API version
// it implements, and names itself and its version.

#include <libretro.h>

#include "loaded_core.h"

#include <gtest/gtest.h>

namespace {

using snaggletooth_libretro_test::LoadedCore;

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

TEST(CoreIdentity, SystemInfoNamesTheCoreAndItsVersion) {
  const LoadedCore core;
  ASSERT_TRUE(core.loaded());
  auto* systemInfo = core.entry<void(retro_system_info*)>("retro_get_system_info");
  ASSERT_NE(systemInfo, nullptr);

  retro_system_info info{};
  systemInfo(&info);
  ASSERT_NE(info.library_name, nullptr);
  ASSERT_NE(info.library_version, nullptr);
  EXPECT_STREQ(info.library_name, "Snaggletooth");
  EXPECT_STREQ(info.library_version, SNAGGLETOOTH_LIBRETRO_VERSION);
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
