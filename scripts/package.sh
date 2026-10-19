#!/bin/sh
# Package a built core as the archive a person downloads, and prove the archive before it goes
# anywhere.
#
#     scripts/package.sh <core module> <work dir> <output.zip> <asset name>
#
# The archive holds ONE folder, named for the asset, with four files inside it: the core module,
# its info file, the core's LICENSE and Snaggletooth's as LICENSE.Snaggletooth (the library is
# linked into the module). Unzipping it never scatters files into the download directory, and
# everything a frontend installs, and everything the module's licenses require, arrives together.
#
# The info file is read from beside the module, where every build puts it; the licenses from this
# repository, found from this script's own location. zip(1) and unzip(1) are required: the archive
# records the Unix permission bits, and the extraction is what proves the archive.
#
# After packing, the archive is extracted again and the extracted files, not the ones packed, are
# checked: exactly the four, the module and the info file byte for byte what went in. On Linux
# the module is also checked to carry its own C++ runtime, which the build links statically: a
# module that looked for the machine's libstdc++ would fail to load on a system whose libstdc++
# is older than the builder's.

set -eu

if [ "$#" -ne 4 ]; then
    echo "usage: package.sh <core module> <work dir> <output.zip> <asset name>" >&2
    exit 2
fi

MODULE="$1"
WORK="$2"
OUTPUT="$3"
ASSET="$4"

# The asset is named for the module and the platform, one of the five the core is built on.
case "$ASSET" in
    snaggletooth_libretro-linux-x64|snaggletooth_libretro-linux-arm64|snaggletooth_libretro-macos-arm64|\
    snaggletooth_libretro-windows-x64|snaggletooth_libretro-windows-arm64) ;;
    *)
        echo "error: $ASSET is not a release asset name (snaggletooth_libretro-<os>-<arch>)" >&2
        exit 1
        ;;
esac

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$script_dir/.." && pwd)

MODULE_NAME=$(basename "$MODULE")
INFO="$(dirname "$MODULE")/snaggletooth_libretro.info"
LICENSE="$root/LICENSE"
LIBRARY_LICENSE="$root/third_party/snaggletooth/LICENSE"

for file in "$MODULE" "$INFO" "$LICENSE" "$LIBRARY_LICENSE"; do
    if [ ! -f "$file" ]; then
        echo "error: no file at $file" >&2
        exit 1
    fi
done

for tool in zip unzip cmp; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "error: $tool is not installed on this machine; the archive is made with zip and" >&2
        echo "       proved with unzip and cmp. Install it and run this again." >&2
        exit 1
    fi
done

# The module carries its own C++ runtime on Linux. ldd lists what the module would load from the
# machine; libstdc++ or libgcc_s there means the link left the runtime out.
if [ "$(uname -s)" = "Linux" ]; then
    runtime=$(ldd "$MODULE" | grep -E 'libstdc\+\+|libgcc_s' || true)
    if [ -n "$runtime" ]; then
        echo "error: $MODULE_NAME needs the machine's C++ runtime:" >&2
        echo "$runtime" | sed 's/^/       /' >&2
        echo "       the build links libstdc++ and libgcc into the module; this one did not." >&2
        exit 1
    fi
fi

rm -rf "$WORK"
mkdir -p "$WORK/$ASSET"
cp -p "$MODULE" "$WORK/$ASSET/$MODULE_NAME"
cp -p "$INFO" "$WORK/$ASSET/snaggletooth_libretro.info"
cp -p "$LICENSE" "$WORK/$ASSET/LICENSE"
cp -p "$LIBRARY_LICENSE" "$WORK/$ASSET/LICENSE.Snaggletooth"

mkdir -p "$(dirname "$OUTPUT")"
rm -f "$OUTPUT"
# -r from inside the work directory, so the stored paths begin at the asset folder rather than
# carrying the path of the machine that built it.
(cd "$WORK" && zip -r -q "$OUTPUT" "$ASSET")

if [ ! -f "$OUTPUT" ]; then
    echo "error: zip reported success but produced no archive at $OUTPUT" >&2
    exit 1
fi

# Extract what a downloader would get, and check those files.
rm -rf "$WORK/verify"
mkdir -p "$WORK/verify"
(cd "$WORK/verify" && unzip -q "$OUTPUT")

status=0

top=$(cd "$WORK/verify" && find . -mindepth 1 -maxdepth 1 | sed 's|^\./||')
if [ "$top" != "$ASSET" ]; then
    echo "error: the archive's top level is not the one folder $ASSET; it holds:" >&2
    echo "$top" | sed 's/^/       /' >&2
    status=1
fi

for name in "$MODULE_NAME" snaggletooth_libretro.info LICENSE LICENSE.Snaggletooth; do
    if [ ! -f "$WORK/verify/$ASSET/$name" ]; then
        echo "error: the archive does not contain $ASSET/$name" >&2
        status=1
    fi
done

count=$(find "$WORK/verify/$ASSET" -type f | wc -l | tr -d ' ')
if [ "$count" != "4" ]; then
    echo "error: the archive holds $count files, not the four it ships:" >&2
    (cd "$WORK/verify" && find . -type f) | sed 's/^/       /' >&2
    status=1
fi

if ! cmp -s "$MODULE" "$WORK/verify/$ASSET/$MODULE_NAME"; then
    echo "error: the extracted $MODULE_NAME differs from the built module" >&2
    status=1
fi
if ! cmp -s "$INFO" "$WORK/verify/$ASSET/snaggletooth_libretro.info"; then
    echo "error: the extracted info file differs from the build's" >&2
    status=1
fi

if [ "$status" -ne 0 ]; then
    echo "" >&2
    echo "Refusing to call $OUTPUT a release archive." >&2
    exit "$status"
fi

rm -rf "$WORK/verify"
echo "packaged $OUTPUT ($ASSET: $MODULE_NAME $(wc -c < "$MODULE" | tr -d ' ') bytes, snaggletooth_libretro.info, LICENSE, LICENSE.Snaggletooth)"
