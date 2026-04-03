#!/usr/bin/env bash

set -e

VERSION="${VERSION:-1.0}"

# Check for registries before attempting to compile
if [ ! -f "include/registries.h" ]; then
  echo "Error: 'include/registries.h' is missing."
  echo "Please follow the 'Compilation' section of the README to generate it."
  exit 1
fi

# Source files
SRC_FILES=(
  src/*.c
  src/noise/*.c
  src/cubiomes/biomenoise.c
  src/cubiomes/biomes.c
  src/cubiomes/finders.c
  src/cubiomes/generator.c
  src/cubiomes/layers.c
  src/cubiomes/noise.c
  src/cubiomes/quadbase.c
  src/cubiomes/util.c
)

INCLUDES="-Iinclude -Isrc/cubiomes"
CFLAGS="-O3 -ffast-math"
LIBS="-lm -lz -pthread"

# Create build directory
mkdir -p build

# ─── Linux build ───
echo "=== Building Linux binary ==="
gcc ${SRC_FILES[@]} $CFLAGS $INCLUDES -o build/bareiron $LIBS -pthread
echo "Linux binary: build/bareiron"

# Package Linux release
echo "=== Packaging Linux release ==="
LINUX_DIR="build/irongingot-v${VERSION}-linux"
rm -rf "$LINUX_DIR"
mkdir -p "$LINUX_DIR"
cp build/bareiron "$LINUX_DIR/"
cp server.conf "$LINUX_DIR/"
(cd build && zip -rq "irongingot-v${VERSION}-linux.zip" "irongingot-v${VERSION}-linux")
rm -rf "$LINUX_DIR"
echo "Linux release: build/irongingot-v${VERSION}-linux.zip"

# ─── Windows build (cross-compile with mingw) ───
echo "=== Building Windows binary ==="

# Check for mingw cross-compiler
WIN_CC=""
if command -v x86_64-w64-mingw32-gcc &>/dev/null; then
  WIN_CC="x86_64-w64-mingw32-gcc"
elif command -v x86_64-w64-mingw32-gcc-posix &>/dev/null; then
  WIN_CC="x86_64-w64-mingw32-gcc-posix"
fi

if [ -n "$WIN_CC" ]; then
  # Bundle zlib sources (no system package needed)
  ZLIB_SRCS=(
    third_party/zlib/adler32.c
    third_party/zlib/compress.c
    third_party/zlib/crc32.c
    third_party/zlib/deflate.c
    third_party/zlib/gzclose.c
    third_party/zlib/gzlib.c
    third_party/zlib/gzread.c
    third_party/zlib/gzwrite.c
    third_party/zlib/infback.c
    third_party/zlib/inffast.c
    third_party/zlib/inflate.c
    third_party/zlib/inftrees.c
    third_party/zlib/trees.c
    third_party/zlib/uncompr.c
    third_party/zlib/zutil.c
  )

  WIN_INCLUDES="$INCLUDES -Ithird_party/zlib"

  $WIN_CC ${SRC_FILES[@]} ${ZLIB_SRCS[@]} $CFLAGS $WIN_INCLUDES -o build/bareiron.exe -static -lws2_32 -pthread -lm
  echo "Windows binary: build/bareiron.exe"

  # Package Windows release
  echo "=== Packaging Windows release ==="
  WINDOWS_DIR="build/irongingot-v${VERSION}-windows"
  rm -rf "$WINDOWS_DIR"
  mkdir -p "$WINDOWS_DIR"
  cp build/bareiron.exe "$WINDOWS_DIR/"
  cp server.conf "$WINDOWS_DIR/"
  (cd build && zip -r "irongingot-v${VERSION}-windows.zip" "irongingot-v${VERSION}-windows")
  rm -rf "$WINDOWS_DIR"
  echo "Windows release: build/irongingot-v${VERSION}-windows.zip"
else
  echo "SKIP: No mingw cross-compiler found."
  echo "  Install it with one of:"
  echo "    Debian/Ubuntu: sudo apt install mingw-w64"
  echo "    Arch:          sudo pacman -S mingw-w64-gcc"
  echo "    Fedora:        sudo dnf install mingw64-gcc"
fi

echo "=== Build complete ==="
echo "Releases are in build/"
