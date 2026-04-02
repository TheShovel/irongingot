#ifndef H_WORLDGEN
#define H_WORLDGEN

#include <stdint.h>
#include "perlin.h"

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
  #define WORLDGEN_THREAD_LOCAL _Thread_local
#elif defined(__GNUC__)
  #define WORLDGEN_THREAD_LOCAL __thread
#else
  #define WORLDGEN_THREAD_LOCAL
#endif

typedef struct {
  short x;
  short z;
  uint32_t hash;
  uint8_t biome;
} ChunkAnchor;

typedef struct {
  short x;
  uint8_t y;
  short z;
  uint8_t variant;
} ChunkFeature;

uint32_t getChunkHash (short x, short z);
uint8_t getChunkBiome (short x, short z);
uint8_t getHeightAt (int x, int z);
uint8_t getTerrainAt (int x, int y, int z, ChunkAnchor anchor);
uint8_t getBlockAt (int x, int y, int z);

extern WORLDGEN_THREAD_LOCAL uint8_t chunk_section[4096];
uint8_t buildChunkSection (int cx, int cy, int cz);

// Noise samplers
extern OctavePerlinNoiseSampler surface_noise;
extern OctavePerlinNoiseSampler detail_noise;
extern OctavePerlinNoiseSampler mountain_noise;

#endif
