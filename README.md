# Snaggletooth for libretro

A [libretro](https://www.libretro.com/) core that runs [Snaggletooth](https://github.com/etroimcasso/Snaggletooth),
a clean-room, MIT-licensed implementation of the SNES, inside RetroArch and every other libretro
frontend.

Snaggletooth defines the console. This core is the application around it: it takes the cartridge,
the controllers and the timing from the frontend, and hands back the picture, the sound and the
save. The console's behavior is entirely the library's: what the core runs is exactly what the
library runs.

> [!IMPORTANT]
> **This is an early build.** A frontend loads the core and a cartridge and shows the picture the
> cartridge draws, one console frame at a time. Sound and controllers come next, so a game plays
> silently and takes no input yet. The table below shows what is done.

## Contents

- [Status](#status)
- [Building](#building)
- [Testing](#testing)
- [License](#license)

## Status

| Component | Status |
|---|---|
| Build, identity and test suite | Done |
| Loading a cartridge and running it | Done — `.sfc`, `.smc`, `.swc` and `.fig` images, with a copier's header dropped; 60 Hz or 50 Hz as the cartridge's header says; a cartridge that needs a coprocessor is refused with a message naming the chip |
| Picture | Done — every frame the console draws, 256 or 512 pixels across and 224 or 239 lines, in 32-bit color; interlaced games woven into the full-height picture, or shown field by field (Bob, Off) from the core options; square pixels, 8:7 at 224 lines, whatever the frame's size |
| Sound | Not started |
| Controllers | Not started |
| Battery saves and save states | Not started |
| Core options | In progress — Video › Interlaced Picture: Weave, Bob or Off |
| Releases | Not started |
| Official libretro core | Not started |

## Building

Snaggletooth is a submodule; clone with it.

```sh
git clone --recurse-submodules https://github.com/etroimcasso/Snaggletooth-libretro.git
cd Snaggletooth-libretro
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Requirements: CMake 3.24 or later and a C++20 compiler — GCC, Apple Clang or MSVC from Visual
Studio 2022. The core lands in `build/` (`build\Release\` under Visual Studio) as
`snaggletooth_libretro.dylib` on macOS, `snaggletooth_libretro.so` on Linux and
`snaggletooth_libretro.dll` on Windows.

Keep `--config Release`: the Visual Studio generator builds Debug without it, whatever the
configure step said. A Release build is optimized, drops code the core never reaches and exports
only the libretro entry points.

## Testing

```sh
ctest --test-dir build -C Release
```

The suite loads the built core the way a frontend does and calls the entry points it exports, so
every case runs against the binary that ships. GoogleTest 1.14.0 is fetched at configure time;
`-DSNAGGLETOOTH_LIBRETRO_BUILD_TESTS=OFF` leaves the suite out.

## License

MIT — see [LICENSE](LICENSE). Snaggletooth is MIT licensed as well. The vendored libretro API
header, [`third_party/libretro/libretro.h`](third_party/libretro/README.md), carries its own MIT
notice.
