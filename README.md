# Snaggletooth for libretro

A [libretro](https://www.libretro.com/) core that runs [Snaggletooth](https://github.com/etroimcasso/Snaggletooth),
a clean-room, MIT-licensed implementation of the SNES, inside RetroArch and every other libretro
frontend.

Snaggletooth defines the console. This core is the application around it: it takes the cartridge,
the controllers and the timing from the frontend, and hands back the picture, the sound and the
save. The console's behavior is entirely the library's: what the core runs is exactly what the
library runs.

> [!IMPORTANT]
> **This is an early build.** A frontend loads the core and a cartridge, shows the picture the
> cartridge draws, plays its sound and passes it the two players' controllers, one console frame
> at a time; a game's battery-backed save is kept in the frontend's save file, and the frontend's
> save states, rewind, run-ahead and netplay take the console's whole state at any frame.
> The table below shows what is done.

## Contents

- [Status](#status)
- [Download](#download)
- [Installing](#installing)
- [Building](#building)
- [Testing](#testing)
- [License](#license)

## Status

| Component | Status |
|---|---|
| Build, identity and test suite | Done |
| Loading a cartridge and running it | Done — `.sfc`, `.smc`, `.swc` and `.fig` images, with a copier's header dropped; 60 Hz or 50 Hz as the cartridge's header says, or as the Region core option sets it; a cartridge that needs a coprocessor is refused with a message naming the chip |
| Picture | Done — every frame the console draws, 256 or 512 pixels across and 224 or 239 lines, in 32-bit color, drawn on the GPU by default and on the processor where the core finds no graphics card it can use; interlaced games woven into the full-height picture, or shown field by field (Bob, Off) from the core options; square pixels, 8:7 at 224 lines, whatever the frame's size, or 4:3 from the core options |
| Sound | Done — the console's stereo sound at its own rate, 32,000 samples a second, handed to the frontend with every frame; the frontend resamples it for the output device |
| Controllers | Done — two ports, each holding the console's standard pad (SNES Joypad) or nothing (None), chosen from the frontend's controller menu; all twelve buttons named for binding, and passed to the console as the frontend reports them |
| Battery saves | Done — a cartridge's save RAM, as large as its header declares, handed to the frontend to keep in its save file and loaded from it when the cartridge starts; the frontend's copy holds the save as each frame leaves it |
| Work RAM and memory map | Done — the console's 128 KB of work RAM handed to the frontend as each frame leaves it, taking what the frontend writes into it before the next; a memory map placing the work RAM, its mirror in the system banks and the cartridge's save at the console's own bus addresses, for achievements and the frontend's cheat search |
| Cheats | Done — Game Genie codes patch the cartridge's bytes in the running console and are put back when turned off; Pro Action Replay (`AAAAAAVV`) and raw (`AAAAAA:VV`) codes write their byte before every frame |
| Core information | Done — `snaggletooth_libretro.info`: the core's name, system, extensions and the libretro features it offers, as RetroArch lists them before the core loads |
| Save states | Done — the console's whole state in Snaggletooth's own byte form, the same size from the moment a cartridge loads and the same bytes on every platform, so a state saved on one computer loads on another; a loaded state puts the game back on the cycle it was saved at, and the pictures and sound that follow are the ones that followed the save, which the frontend's rewind, run-ahead and netplay rely on. Under Weave, the first picture of an interlaced game after a load keeps the other field's lines from the picture shown before it, until the next field replaces them |
| Core options | Done — Video › Interlaced Picture (Weave, Bob or Off) and Aspect Ratio (Square Pixels or 4:3); System › Region (Auto, NTSC or PAL, applied when a cartridge loads), Picture Thread (GPU, the default: the picture drawn on the graphics card with Snaggletooth's own shader; Enabled: on a thread of its own; Disabled: on the core's thread) and Audio Thread (the console's sound on a thread of its own; off by default), each switched while a game runs |
| Releases | Done — an archive per platform on the [Releases](https://github.com/etroimcasso/Snaggletooth-libretro/releases) page, holding the core, its info file and the licenses |
| Official libretro core | Not started |

## Download

The [latest release](https://github.com/etroimcasso/Snaggletooth-libretro/releases/latest) has an
archive for each platform the core is built on:

| Platform | Archive |
|---|---|
| Linux x64 | `snaggletooth_libretro-linux-x64.zip` |
| Linux ARM64 | `snaggletooth_libretro-linux-arm64.zip` |
| macOS (Apple silicon) | `snaggletooth_libretro-macos-arm64.zip` |
| Windows x64 | `snaggletooth_libretro-windows-x64.zip` |
| Windows ARM64 | `snaggletooth_libretro-windows-arm64.zip` |

Each archive unpacks to one folder holding the core (`snaggletooth_libretro.so`, `.dylib` or
`.dll`), `snaggletooth_libretro.info`, the core's `LICENSE` and Snaggletooth's as
`LICENSE.Snaggletooth`. The core carries its own C and C++ runtimes, so it needs nothing
installed beside the frontend. The picture is drawn on the GPU through Metal on macOS, Direct3D 12
on Windows and Vulkan on Linux; on a computer without one, the core says so and draws the picture
on the processor. The release notes name the Snaggletooth commit the core is built on; the core's
own version is the release's.

## Installing

Copy the core into RetroArch's cores directory and `snaggletooth_libretro.info` into its core info
directory, which is a directory of its own; RetroArch's Settings › Directory names both (Cores,
Core Info). The info file is what RetroArch shows under Information › Core Information and what
it lists the core by when choosing one for a cartridge: Nintendo - SNES / SFC (Snaggletooth). A
build leaves the [info file](src/snaggletooth_libretro.info) beside the core, so a build installs
the same way as a release.

On macOS, a dylib downloaded in a browser carries the quarantine attribute, and the system refuses
to load it into RetroArch until the attribute is cleared:

```sh
xattr -d com.apple.quarantine snaggletooth_libretro.dylib
```

## Building

Snaggletooth is a submodule, and the SDL the core draws the picture with is a submodule of
Snaggletooth's; clone with both.

```sh
git clone --recurse-submodules https://github.com/etroimcasso/Snaggletooth-libretro.git
cd Snaggletooth-libretro
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Requirements: CMake 3.24 or later; a C++20 compiler — GCC, Apple Clang or MSVC from Visual
Studio 2022; and the shader tools Snaggletooth compiles its GPU shader with: glslang on Linux
(`apt install glslang-tools`); glslang, spirv-cross (`brew install glslang spirv-cross`) and the
Metal toolchain (`xcodebuild -downloadComponent MetalToolchain`) on macOS; dxc from the Windows
SDK on Windows. The configure stops and names a tool it cannot find. The core lands in `build/` (`build\Release\` under Visual Studio) as
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
every case runs against the binary that ships. Its last case packages the built core with
[`scripts/package.sh`](scripts/package.sh) (`package.ps1` on Windows), the script that makes a
release archive, and checks the archive it made. GoogleTest 1.14.0 is fetched at configure time;
`-DSNAGGLETOOTH_LIBRETRO_BUILD_TESTS=OFF` leaves the suite out.

## License

MIT — see [LICENSE](LICENSE). Snaggletooth is MIT licensed as well. The core is built with
[SDL 3](https://www.libsdl.org/), under the zlib license, for the GPU picture. The vendored libretro API
header, [`third_party/libretro/libretro.h`](third_party/libretro/README.md), carries its own MIT
notice.
