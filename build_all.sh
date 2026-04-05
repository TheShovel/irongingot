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
MOJANG_SKIN_CFLAGS=""
MOJANG_SKIN_LIBS=""

detect_mojang_skin_support() {
  if command -v curl-config >/dev/null 2>&1; then
    local cflags libs
    cflags="$(curl-config --cflags 2>/dev/null)"
    libs="$(curl-config --libs 2>/dev/null)"
    if [ -n "$libs" ]; then
      MOJANG_SKIN_CFLAGS="-DMOJANG_SKIN_LOOKUP_AVAILABLE $cflags"
      MOJANG_SKIN_LIBS="$libs"
      return 0
    fi
  fi

  if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libcurl; then
    MOJANG_SKIN_CFLAGS="-DMOJANG_SKIN_LOOKUP_AVAILABLE $(pkg-config --cflags libcurl)"
    MOJANG_SKIN_LIBS="$(pkg-config --libs libcurl)"
    return 0
  fi

  return 1
}

if ! detect_mojang_skin_support; then
  echo "SKIP: libcurl not found. Mojang skin lookup will be disabled in the glibc build."
fi

# Create build directory
mkdir -p build

# ─── Linux build (glibc) ───
echo "=== Building Linux binary (glibc) ==="
gcc ${SRC_FILES[@]} $CFLAGS $MOJANG_SKIN_CFLAGS $INCLUDES -o build/bareiron $LIBS -pthread $MOJANG_SKIN_LIBS
echo "Linux binary: build/bareiron"

# ─── Linux build (musl) ───
MUSL_BUILT=0
if command -v musl-gcc &>/dev/null; then
  echo "=== Building Linux binary (musl) ==="
  # Bundle zlib sources to avoid glibc headers conflict
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
  MUSL_INCLUDES="$INCLUDES -Ithird_party/zlib"
  musl-gcc ${SRC_FILES[@]} ${ZLIB_SRCS[@]} $CFLAGS -D_GNU_SOURCE $MUSL_INCLUDES -o build/bareiron-musl -static -lpthread -lm
  echo "Linux musl binary: build/bareiron-musl"
  MUSL_BUILT=1
else
  echo "SKIP: musl-gcc not found. Install with:"
  echo "  Debian/Ubuntu: sudo apt install musl-tools"
  echo "  Arch:          sudo pacman -S musl"
  echo "  Fedora:        sudo dnf install musl-gcc"
fi

# Package Linux release (glibc)
echo "=== Packaging Linux release (glibc) ==="
GLIBC_DIR="build/irongingot-v${VERSION}-linux-glibc"
rm -rf "$GLIBC_DIR"
mkdir -p "$GLIBC_DIR"
cp build/bareiron "$GLIBC_DIR/"
cp server.conf "$GLIBC_DIR/"
cp serverIcon.png "$GLIBC_DIR/"
(cd build && zip -rq "irongingot-v${VERSION}-linux-glibc.zip" "irongingot-v${VERSION}-linux-glibc")
rm -rf "$GLIBC_DIR"
echo "Linux glibc release: build/irongingot-v${VERSION}-linux-glibc.zip"

# Package Linux release (musl)
if [ "$MUSL_BUILT" -eq 1 ]; then
  echo "=== Packaging Linux release (musl) ==="
  MUSL_DIR="build/irongingot-v${VERSION}-linux-musl"
  rm -rf "$MUSL_DIR"
  mkdir -p "$MUSL_DIR"
  cp build/bareiron-musl "$MUSL_DIR/"
  cp server.conf "$MUSL_DIR/"
  cp serverIcon.png "$MUSL_DIR/"
  (cd build && zip -rq "irongingot-v${VERSION}-linux-musl.zip" "irongingot-v${VERSION}-linux-musl")
  rm -rf "$MUSL_DIR"
  echo "Linux musl release: build/irongingot-v${VERSION}-linux-musl.zip"
fi

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
  cp serverIcon.png "$WINDOWS_DIR/"
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
