// World generation benchmark — self-contained with stubs for unused server deps.
// Compile:
//   gcc bench_worldgen.c src/worldgen.c src/noise/perlin.c src/generated_village_templates.c src/cubiomes/biomenoise.c src/cubiomes/biomes.c src/cubiomes/finders.c src/cubiomes/generator.c src/cubiomes/layers.c src/cubiomes/noise.c src/cubiomes/quadbase.c src/cubiomes/util.c -O3 -ffast-math -D_GNU_SOURCE -Iinclude -Isrc/cubiomes -o bench_worldgen -lm -lpthread

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <pthread.h>

#include "worldgen.h"
#include "perlin.h"
#include "registries.h"

#define DIMENSION_OVERWORLD 0

// --- Stubs for symbols worldgen.c needs but we don't use in the benchmark ---

uint32_t world_seed = 0xA103DE6C;
uint32_t rng_seed = 0xE2B9419;
int server_running = 1;

uint64_t splitmix64(uint64_t state) {
  uint64_t z = state + 0x9e3779b97f4a7c15;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
  z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
  return z ^ (z >> 31);
}

// Block-change queries — never hit during fresh-world section builds
int isChunkModified(int cx, int cz) { (void)cx;(void)cz; return 0; }
void* copyBlockChangesSnapshot(int *count) { *count = 0; return NULL; }
void freeBlockChangesSnapshot(void *p) { (void)p; }
uint16_t getBlockChange(int x, int y, int z, uint8_t dim) { (void)x;(void)y;(void)z;(void)dim; return 0; }
int special_block_has_entry(int x, int y, int z, uint8_t dim) { (void)x;(void)y;(void)z;(void)dim; return 0; }
void special_block_set_state(int x, int y, int z, uint8_t dim, uint16_t block, uint16_t state) { (void)x;(void)y;(void)z;(void)dim;(void)block;(void)state; }

// Thread pool stubs (unused in single-threaded bench)
int get_cpu_count(void) { return 1; }
void terminal_ui_log(const char *s) { (void)s; }
void terminal_ui_shutdown(void) {}
void fast_rand(void) {}

// Block-palette queries (special_block.h) — pulled in by block-change overlay dead code
uint8_t is_stair_block(uint16_t b) { (void)b; return 0; }
uint8_t is_oriented_block(uint16_t b) { (void)b; return 0; }
uint8_t is_fence_block(uint16_t b) { (void)b; return 0; }
uint8_t is_horizontal_facing_block(uint16_t b) { (void)b; return 0; }
uint8_t is_bed_block(uint16_t b) { (void)b; return 0; }
uint8_t is_door_block(uint16_t b) { (void)b; return 0; }

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec * 1e-3;
}

int main(void) {
    init_worldgen();

    printf("=== Section build time by Y-level (4x4 chunk area, 200 iters each) ===\n");
    for (int cy = 0; cy < 320; cy += 16) {
        double t0 = now_us();
        int count = 0;
        for (int iter = 0; iter < 200; iter++) {
            for (int cx = -2; cx < 2; cx++) {
                for (int cz = -2; cz < 2; cz++) {
                    buildChunkSection(cx * 16, cy, cz * 16, DIMENSION_OVERWORLD);
                    count++;
                }
            }
        }
        double dt = now_us() - t0;
        printf("  y=[%3d,%3d): %6.1f us/section\n", cy, cy + 16, dt / count);
    }

    printf("\n=== Full chunk profile (20 sections each, 50 chunks) ===\n");
    int chunks_done = 0;
    double t_chunk = now_us();
    for (int cx = -5; cx <= 5; cx++) {
        for (int cz = -5; cz <= 5; cz++) {
            if (cx == 0 && cz == 0) continue; // already warmed up
            // We can't call generate_chunk_data because it needs chunk cache,
            // but we can profile individual section builds
            chunks_done++;
        }
    }
    // Benchmark: build sections manually (like generate_chunk_data does)
    int sections_done = 0;
    double t0 = now_us();
    for (int iter = 0; iter < 10; iter++) {
        for (int cx = -3; cx <= 3; cx++) {
            for (int cz = -3; cz <= 3; cz++) {
                for (int cy = 0; cy < 320; cy += 16) {
                    buildChunkSection(cx * 16, cy, cz * 16, DIMENSION_OVERWORLD);
                    sections_done++;
                }
            }
        }
    }
    double dt = now_us() - t0;
    printf("  %d full chunks (20 sections each) in %.2f ms\n",
           sections_done / 20, dt / 1e3);
    printf("  %.1f us/section, %.2f ms/chunk\n",
           dt / sections_done, dt / (sections_done / 20) / 1e3);

    printf("\nDone.\n");
    return 0;
}
