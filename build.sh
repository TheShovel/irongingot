#!/usr/bin/env bash

# Check for registries before attempting to compile, prevents confusion
if [ ! -f "include/registries.h" ]; then
  echo "Error: 'include/registries.h' is missing."
  echo "Please follow the 'Compilation' section of the README to generate it."
  exit 1
fi

# Figure out executable suffix (for MSYS compilation)
case "$OSTYPE" in
  msys*|cygwin*|win32*) exe=".exe" ;;
  *) exe="" ;;
esac

# mingw64-specific linker options
windows_linker=""
unameOut="$(uname -s)"
case "$unameOut" in
  MINGW64_NT*)
    windows_linker="-static -lws2_32 -pthread"
    ;;
esac

# Default compiler
compiler="gcc"
musl=0

# Handle arguments
for arg in "$@"; do
  case $arg in
    --9x)
      if [[ "$unameOut" == MINGW64_NT* ]]; then
        compiler="/opt/bin/i686-w64-mingw32-gcc"
        windows_linker="$windows_linker -Wl,--subsystem,console:4"
      else
        echo "Error: Compiling for Windows 9x is only supported when running under the MinGW64 shell."
        exit 1
      fi
      ;;
    --musl)
      musl=1
      if command -v musl-gcc &>/dev/null; then
        compiler="musl-gcc"
      else
        echo "Error: musl-gcc not found. Install it with:"
        echo "  Debian/Ubuntu: sudo apt install musl-tools"
        echo "  Arch:          sudo pacman -S musl"
        echo "  Fedora:        sudo dnf install musl-gcc"
        exit 1
      fi
      ;;
  esac
done

rm -f "bareiron$exe"

# Source files
SRC_LIST="src/*.c src/noise/*.c src/cubiomes/biomenoise.c src/cubiomes/biomes.c src/cubiomes/finders.c src/cubiomes/generator.c src/cubiomes/layers.c src/cubiomes/noise.c src/cubiomes/quadbase.c src/cubiomes/util.c"
ZLIB_SRCS="third_party/zlib/adler32.c third_party/zlib/compress.c third_party/zlib/crc32.c third_party/zlib/deflate.c third_party/zlib/gzclose.c third_party/zlib/gzlib.c third_party/zlib/gzread.c third_party/zlib/gzwrite.c third_party/zlib/infback.c third_party/zlib/inffast.c third_party/zlib/inflate.c third_party/zlib/inftrees.c third_party/zlib/trees.c third_party/zlib/uncompr.c third_party/zlib/zutil.c"
ZLIB_INCLUDE="-Ithird_party/zlib"

# musl requires static linking and bundled zlib (to avoid glibc headers conflict)
if [ "$musl" -eq 1 ]; then
  eval $compiler $SRC_LIST $ZLIB_SRCS -O3 -ffast-math -D_GNU_SOURCE -Iinclude -Isrc/cubiomes $ZLIB_INCLUDE -o "bareiron$exe" $windows_linker -lm -static -lpthread
else
  eval $compiler $SRC_LIST -O3 -ffast-math -Iinclude -Isrc/cubiomes -o "bareiron$exe" $windows_linker -lm -lz -pthread
fi

# Only run if not cross-compiling and not musl build
if [[ "$musl" -eq 0 ]] && [[ "$OSTYPE" != msys* && "$OSTYPE" != cygwin* && "$OSTYPE" != win32* ]]; then
  "./bareiron$exe"
fi
