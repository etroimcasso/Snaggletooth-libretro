cmake_minimum_required(VERSION 3.24)

project(SnaggletoothLibretro VERSION 0.1.0 LANGUAGES CXX)

# A build is optimized by default: an empty CMAKE_BUILD_TYPE (CMake's default when nothing
# supplies one) would produce an unoptimized, unstripped core. Multi-config generators
# ignore this and select the configuration at build time.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Route compilation through ccache when it is installed. A no-op when absent.
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
  set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

option(SNAGGLETOOTH_LIBRETRO_BUILD_TESTS "Build the test suite" ON)

# The platform a release archive is named for: <os>-<arch>, one of linux-x64, linux-arm64,
# macos-arm64, windows-x64 and windows-arm64. Under Visual Studio the architecture is the
# generator's platform, which the configure line selects.
if(WIN32)
  set(_os windows)
elseif(APPLE)
  set(_os macos)
else()
  set(_os linux)
endif()
if(CMAKE_GENERATOR_PLATFORM)
  string(TOLOWER "${CMAKE_GENERATOR_PLATFORM}" _arch)
else()
  string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _arch)
endif()
if(_arch MATCHES "^(x64|amd64|x86_64)$")
  set(_arch x64)
elseif(_arch MATCHES "^(arm64|aarch64)$")
  set(_arch arm64)
endif()
set(SNAGGLETOOTH_LIBRETRO_PLATFORM "${_os}-${_arch}")
unset(_os)
unset(_arch)

# The module carries its C and C++ runtimes with it. On Windows the runtime is linked statically,
# so the DLL loads on a machine without the Visual C++ redistributable; set before the library is
# added so its objects and GoogleTest's agree with the module's. On Linux the module links
# libstdc++ and libgcc statically (below), so it loads on a system whose libstdc++ is older than
# the one it was built against.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# The core is a shared module the frontend loads, and the library is a static archive linked
# into it, so the library's objects are built position-independent. Every symbol is hidden
# unless it is declared otherwise; the libretro entry points are declared RETRO_API, which
# exports them, and they are the module's only exports. Each function and datum gets its own
# section so the final link can drop whatever the core never reaches. These are set before
# the library and SDL are added so they reach their objects too, SDL's C and Objective-C among
# them; they change code generation only, never either one's warnings.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_OBJC_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
if(NOT MSVC)
  add_compile_options(-ffunction-sections -fdata-sections)
endif()

# The core draws the picture on a GPU through SDL's GPU API: Metal on macOS, Direct3D 12 on
# Windows, Vulkan on Linux. SDL is built from the copy Snaggletooth pins, as a static library
# linked into the module, with only the video and GPU subsystems: the core opens no window,
# plays no sound and reads no device, and the offscreen video driver is the only one it needs.
# It is added before the library so SDL3::SDL3 exists when the library looks for it.
set(SDL_SHARED       OFF CACHE BOOL "" FORCE)
set(SDL_STATIC       ON  CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_AUDIO        OFF CACHE BOOL "" FORCE)
set(SDL_VIDEO        ON  CACHE BOOL "" FORCE)
set(SDL_GPU          ON  CACHE BOOL "" FORCE)
set(SDL_RENDER       OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA       OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC       OFF CACHE BOOL "" FORCE)
set(SDL_JOYSTICK     OFF CACHE BOOL "" FORCE)
set(SDL_HIDAPI       OFF CACHE BOOL "" FORCE)
set(SDL_POWER        OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR       OFF CACHE BOOL "" FORCE)
set(SDL_DIALOG       OFF CACHE BOOL "" FORCE)
set(SDL_OPENGL       OFF CACHE BOOL "" FORCE)
set(SDL_OPENGLES     OFF CACHE BOOL "" FORCE)
set(SDL_OFFSCREEN    ON  CACHE BOOL "" FORCE)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(SDL_X11        OFF CACHE BOOL "" FORCE)
  set(SDL_WAYLAND    OFF CACHE BOOL "" FORCE)
  set(SDL_KMSDRM     OFF CACHE BOOL "" FORCE)
endif()
add_subdirectory(third_party/snaggletooth/third_party/sdl EXCLUDE_FROM_ALL SYSTEM)
# SDL marks its functions for export; an empty SDL_DECLSPEC leaves them hidden like the rest of
# the module, so the libretro entry points stay its only exports.
target_compile_definitions(SDL3-static PRIVATE "SDL_DECLSPEC=")

add_subdirectory(third_party/snaggletooth EXCLUDE_FROM_ALL)

add_library(snaggletooth_libretro MODULE
  src/battery_save.cpp
  src/cartridge.cpp
  src/cheats.cpp
  src/controllers.cpp
  src/frontend.cpp
  src/gpu_device.cpp
  src/libretro.cpp
  src/memory_map.cpp
  src/options.cpp
  src/picture.cpp
  src/save_state.cpp
  src/session.cpp
  src/sound.cpp
  src/work_ram.cpp
)
target_compile_features(snaggletooth_libretro PRIVATE cxx_std_20)
# SDL_MAIN_HANDLED keeps SDL's headers from renaming main: the core is a module in the
# frontend's process, and it calls SDL_SetMainReady before starting SDL.
target_compile_definitions(snaggletooth_libretro PRIVATE
  SNAGGLETOOTH_LIBRETRO_VERSION="${PROJECT_VERSION}"
  SDL_MAIN_HANDLED
  "SDL_DECLSPEC="
)
target_include_directories(snaggletooth_libretro SYSTEM PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libretro
)
target_link_libraries(snaggletooth_libretro PRIVATE snaggletooth::snaggletooth SDL3::SDL3)
# libretro names a core <name>_libretro.<ext> with no "lib" prefix on any platform, and the
# extension is the platform's shared-library one: .dylib on Apple, where CMake would give a
# module .so.
set_target_properties(snaggletooth_libretro PROPERTIES
  PREFIX ""
  CXX_EXTENSIONS OFF
)
if(APPLE)
  set_target_properties(snaggletooth_libretro PROPERTIES SUFFIX ".dylib")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(snaggletooth_libretro PRIVATE -static-libstdc++ -static-libgcc)
endif()

# The core's info file sits beside the module in the build tree, so the directory a build
# produces holds both files a frontend installs. It is copied on every build.
add_custom_target(snaggletooth_libretro_info ALL
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_CURRENT_SOURCE_DIR}/src/snaggletooth_libretro.info
    $<TARGET_FILE_DIR:snaggletooth_libretro>/snaggletooth_libretro.info
  DEPENDS snaggletooth_libretro
)

# Warnings are errors on this project's own targets only.
if(MSVC)
  target_compile_options(snaggletooth_libretro PRIVATE /W4 /WX)
else()
  target_compile_options(snaggletooth_libretro PRIVATE -Wall -Wextra -Werror)
endif()

# Every configuration but Debug ships lean: unreferenced code is dropped at the final link
# and the local symbol table is left out of the module. Under MSVC the Release configuration
# compiles without debug information and links with /OPT:REF, which drops unreferenced functions.
set(SNAGGLETOOTH_LIBRETRO_LEAN $<NOT:$<CONFIG:Debug>>)
if(APPLE)
  target_link_options(snaggletooth_libretro PRIVATE
    $<${SNAGGLETOOTH_LIBRETRO_LEAN}:LINKER:-dead_strip>
    $<${SNAGGLETOOTH_LIBRETRO_LEAN}:LINKER:-x>
  )
elseif(NOT MSVC)
  target_link_options(snaggletooth_libretro PRIVATE
    $<${SNAGGLETOOTH_LIBRETRO_LEAN}:LINKER:--gc-sections>
    $<${SNAGGLETOOTH_LIBRETRO_LEAN}:LINKER:--strip-all>
  )
endif()

if(SNAGGLETOOTH_LIBRETRO_BUILD_TESTS)
  enable_testing()

  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  # GoogleTest takes the runtime library the project sets above, the same static one as the core.
  set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  # The suite drives the built core the way a frontend does: it loads the module at run time
  # and calls the entry points it exports, so every case runs against the shipped binary.
  # The cartridges the cases load are Snaggletooth's hand-built examples, header-only images
  # under its tools tree, and images the suite builds on the same helpers; only the suite
  # includes them.
  add_executable(snaggletooth_libretro_tests
    tests/core_identity_test.cpp
    tests/lifecycle_test.cpp
    tests/picture_test.cpp
    tests/sound_test.cpp
    tests/controllers_test.cpp
    tests/battery_save_test.cpp
    tests/options_test.cpp
    tests/work_ram_test.cpp
    tests/cheats_test.cpp
    tests/core_info_test.cpp
    tests/save_state_test.cpp
    tests/gpu_picture_test.cpp
  )
  target_compile_features(snaggletooth_libretro_tests PRIVATE cxx_std_20)
  set_target_properties(snaggletooth_libretro_tests PROPERTIES CXX_EXTENSIONS OFF)
  target_include_directories(snaggletooth_libretro_tests SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libretro
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/snaggletooth/tools
  )
  target_compile_definitions(snaggletooth_libretro_tests PRIVATE
    RETRO_IMPORT_SYMBOLS
    SNAGGLETOOTH_LIBRETRO_VERSION="${PROJECT_VERSION}"
    SNAGGLETOOTH_LIBRETRO_PATH="$<TARGET_FILE:snaggletooth_libretro>"
    SNAGGLETOOTH_LIBRETRO_INFO_PATH="$<TARGET_FILE_DIR:snaggletooth_libretro>/snaggletooth_libretro.info"
    SNAGGLETOOTH_LIBRETRO_LICENSE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
  )
  target_link_libraries(snaggletooth_libretro_tests PRIVATE
    snaggletooth::snaggletooth
    GTest::gtest_main
    ${CMAKE_DL_LIBS}
  )
  add_dependencies(snaggletooth_libretro_tests snaggletooth_libretro snaggletooth_libretro_info)
  # On macOS a case messages NSCursor after the core is closed, as a frontend's video setup does.
  if(APPLE)
    target_link_libraries(snaggletooth_libretro_tests PRIVATE "-framework AppKit")
  endif()
  if(MSVC)
    target_compile_options(snaggletooth_libretro_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(snaggletooth_libretro_tests PRIVATE -Wall -Wextra -Werror)
  endif()

  include(GoogleTest)
  gtest_discover_tests(snaggletooth_libretro_tests DISCOVERY_TIMEOUT 60)

  # The release archive, made from the built module by the script the release workflow runs and
  # proved by it: one folder named for the asset, the four files it ships, the module and the info
  # file byte for byte the build's, and the module carrying its own runtime.
  set(_asset snaggletooth_libretro-${SNAGGLETOOTH_LIBRETRO_PLATFORM})
  set(_package_dir ${CMAKE_CURRENT_BINARY_DIR}/package)
  if(WIN32)
    add_test(NAME Package
      COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File ${CMAKE_CURRENT_SOURCE_DIR}/scripts/package.ps1
        -Module $<TARGET_FILE:snaggletooth_libretro>
        -WorkDir ${_package_dir}
        -Output ${_package_dir}/${_asset}.zip
        -Asset ${_asset}
    )
  else()
    add_test(NAME Package
      COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/package.sh
        $<TARGET_FILE:snaggletooth_libretro>
        ${_package_dir}
        ${_package_dir}/${_asset}.zip
        ${_asset}
    )
  endif()
  unset(_asset)
  unset(_package_dir)
endif()
