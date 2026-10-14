cmake_minimum_required(VERSION 3.24)

project(SnaggletoothLibretro VERSION 0.0.1 LANGUAGES CXX)

# A build is optimized by default: an empty CMAKE_BUILD_TYPE (CMake's default when nothing
# supplies one) would produce an unoptimized, unstripped core. Multi-config generators
# ignore this and select the configuration at build time.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Route compilation through ccache when it is installed. A no-op when absent.
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

option(SNAGGLETOOTH_LIBRETRO_BUILD_TESTS "Build the test suite" ON)

# The core is a shared module the frontend loads, and the library is a static archive linked
# into it, so the library's objects are built position-independent. Every symbol is hidden
# unless it is declared otherwise; the libretro entry points are declared RETRO_API, which
# exports them, and they are the module's only exports. Each function and datum gets its own
# section so the final link can drop whatever the core never reaches. These are set before
# the library is added so they reach its objects too; they change code generation only, never
# the library's warnings.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
if(NOT MSVC)
  add_compile_options(-ffunction-sections -fdata-sections)
endif()

add_subdirectory(third_party/snaggletooth EXCLUDE_FROM_ALL)

add_library(snaggletooth_libretro MODULE
  src/libretro.cpp
)
target_compile_features(snaggletooth_libretro PRIVATE cxx_std_20)
target_compile_definitions(snaggletooth_libretro PRIVATE
  SNAGGLETOOTH_LIBRETRO_VERSION="${PROJECT_VERSION}"
)
target_include_directories(snaggletooth_libretro SYSTEM PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libretro
)
target_link_libraries(snaggletooth_libretro PRIVATE snaggletooth::snaggletooth)
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
  # Keep GoogleTest on the same CRT as the core on Windows.
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
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
  )
  target_link_libraries(snaggletooth_libretro_tests PRIVATE
    snaggletooth::snaggletooth
    GTest::gtest_main
    ${CMAKE_DL_LIBS}
  )
  add_dependencies(snaggletooth_libretro_tests snaggletooth_libretro)
  if(MSVC)
    target_compile_options(snaggletooth_libretro_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(snaggletooth_libretro_tests PRIVATE -Wall -Wextra -Werror)
  endif()

  include(GoogleTest)
  gtest_discover_tests(snaggletooth_libretro_tests DISCOVERY_TIMEOUT 60)
endif()
