# libretro.h, libretro_vulkan.h and libretro_d3d12.h

The libretro API header and its Vulkan and Direct3D 12 hardware-rendering headers, each vendored
unmodified as a single file, all from the same tag.

| | |
|---|---|
| Source | `libretro-common/include/libretro.h`, `libretro_vulkan.h` and `libretro_d3d12.h` in [libretro/RetroArch](https://github.com/libretro/RetroArch) |
| Tag | `v1.22.2` (commit `69a4f0ea1e8aaf442ae4858f2e7f2b31a1776576`) |
| SHA-256, `libretro.h` | `bd3398d29c3763d18617087020ff56b5450a48a63123403a4b674f8e79947acb` |
| SHA-256, `libretro_vulkan.h` | `c6c11e4c4e792c0466e494bcfc8c10fa3d83f9b1d70e0fbf6307720d9f79823e` |
| SHA-256, `libretro_d3d12.h` | `b52b861c811d58f7397cdd3ef9e1474dc1acc3f4a46fc68f759459a689e3c49a` |
| License | MIT, stated at the top of each header itself |

The core's own sources include them as system headers, so their warnings are their own.
`libretro_vulkan.h` includes `<vulkan/vulkan.h>`, which the core takes from the copy of the
Vulkan headers in SDL's tree on macOS and Linux; `libretro_d3d12.h` includes the Windows SDK's
`<d3d12.h>`, on Windows alone. To move the pin, replace the three files with the headers from a
newer tag and update this table.
