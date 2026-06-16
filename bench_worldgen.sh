#!/bin/sh
# World generation benchmark runner.
# Compiles the benchmark and runs it, comparing with original.
set -e
cd "$(dirname "$0")"

echo "=== Building benchmark ==="
gcc bench_worldgen.c src/worldgen.c src/noise/perlin.c src/generated_village_templates.c \
    src/cubiomes/biomenoise.c src/cubiomes/biomes.c src/cubiomes/finders.c \
    src/cubiomes/generator.c src/cubiomes/layers.c src/cubiomes/noise.c \
    src/cubiomes/quadbase.c src/cubiomes/util.c \
    -O3 -ffast-math -D_GNU_SOURCE -Iinclude -Isrc/cubiomes \
    -o bench_worldgen -lm -lpthread

echo "=== Running benchmark ==="
./bench_worldgen
