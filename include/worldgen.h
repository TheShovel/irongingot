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
uint16_t getTerrainAt (int x, int y, int z, ChunkAnchor anchor);
uint16_t getBlockAt (int x, int y, int z);
uint16_t getBlockAt2 (int x, int y, int z, uint8_t dimension);
uint16_t getNetherBlockAt (int x, int y, int z);
uint16_t getEndBlockAt (int x, int y, int z);
const char* getBiomeName(uint8_t biome_id);
uint8_t biomeNameMatches(uint8_t biome_id, const char* name, uint8_t name_len);
uint32_t getBiomeKeywordMask(uint8_t biome_id);
uint32_t getQueryKeywordMask(const char* name, uint8_t name_len);
uint8_t getBiomeAtBlockCoords(int x, int z);
uint8_t getVillageHousePositions(int x, int z, short *house_x, short *house_z, uint8_t max_houses);

extern WORLDGEN_THREAD_LOCAL uint16_t chunk_section[4096];
uint16_t buildChunkSection (int cx, int cy, int cz, uint8_t dimension);
uint16_t buildNetherChunkSection(int cx, int cy, int cz);
uint16_t buildEndChunkSection(int cx, int cy, int cz);
uint8_t getChunkNetherBiome(short x, short z);
uint8_t getChunkEndBiome(short x, short z);

// Searches for nearest nether structure (fortress or bastion) from (px, pz).
// Returns 1 if found and fills out_x/out_y/out_z/out_name. Returns 0 if none found within radius.
uint8_t findNearestNetherStructure(int px, int pz, int radius, int *out_x, int *out_y, int *out_z, const char **out_name);

void init_worldgen(void);

// Noise samplers
extern OctavePerlinNoiseSampler surface_noise;
extern OctavePerlinNoiseSampler detail_noise;
extern OctavePerlinNoiseSampler mountain_noise;

#endif
