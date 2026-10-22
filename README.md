# Snaggletooth for libretro

Play SNES games in RetroArch with [Snaggletooth](https://github.com/etroimcasso/Snaggletooth),
a clean-room, MIT-licensed SNES emulator. This core works in RetroArch and any other libretro
frontend.

> [!NOTE]
> **Early build.** Everything below works today, but games that use an extra chip in the
> cartridge (Super FX, for example) aren't supported yet. The core tells you which chip when you
> load one.

## What you get

- **The full picture.** Every resolution the console draws, including hi-res and interlaced
  games, in 32-bit color. Choose Weave, Bob or Off for interlaced games, and square pixels or 4:3.
- **GPU-powered PPU**. Snaggletooth renders the SNES picture on the graphics card rather
  than the CPU, which keeps even demanding games running smoothly. It's on by default on Mac,
  Windows and Linux.
- **Sound at the console's native rate**, 32 kHz stereo.
- **Two controllers**, with every button named in RetroArch's bind menu.
- **Battery saves** that live in your RetroArch save folder like any other core's.
- **Save states, rewind, run-ahead and netplay.** States are the same size and format on every
  platform, so one saved on a Mac loads on a PC.
- **Cheats.** Game Genie, Pro Action Replay (`AAAAAAVV`) and raw (`AAAAAA:VV`) codes, plus
  RetroArch's cheat search.
- **RetroAchievements** support through a full memory map.
- **Core options** for region (Auto, NTSC or PAL), aspect ratio, interlacing, and where the
  picture and sound are processed.

## Download

Grab the archive for your platform from the
[latest release](https://github.com/etroimcasso/Snaggletooth-libretro/releases/latest):

| Platform | Archive |
|---|---|
| Windows x64 | `snaggletooth_libretro-windows-x64.zip` |
| Windows ARM64 | `snaggletooth_libretro-windows-arm64.zip` |
| macOS (Apple silicon) | `snaggletooth_libretro-macos-arm64.zip` |
| Linux x64 | `snaggletooth_libretro-linux-x64.zip` |
| Linux ARM64 | `snaggletooth_libretro-linux-arm64.zip` |

Each archive holds the core, its info file and the licenses. The core is self-contained, so
there's nothing else to install. GPU rendering uses Metal on macOS, Direct3D 12 on Windows and
Vulkan on Linux; if your machine has no usable graphics card, the core says so and falls back to
the CPU.

## Installing

1. Copy `snaggletooth_libretro.dylib` (`.so` on Linux, `.dll` on Windows) from the archive's
   folder into RetroArch's **Cores** directory.
2. Copy `snaggletooth_libretro.info` from the same folder into RetroArch's **Core Info**
   directory.

Both directories are listed under Settings › Directory in RetroArch. The core shows up as
**Nintendo - SNES / SFC (Snaggletooth)**. It loads `.sfc`, `.smc`, `.swc` and `.fig` files.

On macOS, a file downloaded in a browser is quarantined and RetroArch won't load it until you
clear the flag:

```sh
xattr -d com.apple.quarantine snaggletooth_libretro.dylib
```

## Building from source

```sh
git clone --recurse-submodules https://github.com/etroimcasso/Snaggletooth-libretro.git
cd Snaggletooth-libretro
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

You'll need CMake 3.24 or later, a C++20 compiler (GCC, Apple Clang or Visual Studio 2022) and
the shader tools Snaggletooth uses for its GPU renderer:

- **Linux:** `apt install glslang-tools`
- **macOS:** `brew install glslang spirv-cross` and `xcodebuild -downloadComponent MetalToolchain`
- **Windows:** dxc, which ships with the Windows SDK

If a tool is missing, the configure step tells you which one. The built core lands in `build/`
(`build\Release\` with Visual Studio), with the info file beside it, so a build installs the same
way as a release. Keep `--config Release` on Visual Studio, which builds Debug otherwise.

To run the test suite:

```sh
ctest --test-dir build -C Release
```

The suite loads the built core the way a frontend does, so every test runs against the binary
that ships. Pass `-DSNAGGLETOOTH_LIBRETRO_BUILD_TESTS=OFF` to skip building it.

## License

MIT, see [LICENSE](LICENSE). Snaggletooth is MIT licensed too. The core is built with
[SDL 3](https://www.libsdl.org/) (zlib license), and the bundled
[libretro API header](third_party/libretro/README.md) carries its own MIT notice.
