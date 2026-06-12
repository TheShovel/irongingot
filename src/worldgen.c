#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

#include "globals.h"
#include "tools.h"
#include "registries.h"
#include "procedures.h"
#include "worldgen.h"
#include "perlin.h"
#include "generator.h"
#include "special_block.h"
#include "generated_village_templates.h"

// build_registries.js now emits these stronghold-specific palette entries.
// If a developer has older generated registries locally, fall back to existing
// blocks so the source still compiles until the registries are regenerated.
#ifndef B_bookshelf
#define B_bookshelf B_oak_planks
#endif
#ifndef B_end_portal_frame_eye_north
#define B_end_portal_frame_eye_north B_end_portal_frame
#endif
#ifndef B_end_portal_frame_eye_south
#define B_end_portal_frame_eye_south B_end_portal_frame
#endif
#ifndef B_end_portal_frame_eye_west
#define B_end_portal_frame_eye_west B_end_portal_frame
#endif
#ifndef B_end_portal_frame_eye_east
#define B_end_portal_frame_eye_east B_end_portal_frame
#endif
#ifndef B_end_portal
#define B_end_portal B_air
#endif
#ifndef B_end_stone
#define B_end_stone B_stone
#endif
#ifndef B_mossy_cobblestone
#define B_mossy_cobblestone B_cobblestone
#endif
#ifndef B_spawner
#define B_spawner B_cobweb
#endif
#ifndef B_dirt_path
#define B_dirt_path B_coarse_dirt
#endif
#ifndef B_sandstone_slab
#define B_sandstone_slab B_cut_sandstone
#endif
#ifndef B_farmland
#define B_farmland B_dirt
#endif
#ifndef B_wheat
#define B_wheat B_short_grass
#endif
#ifndef B_wheat_7
#define B_wheat_7 B_wheat
#endif
#ifndef B_lectern
#define B_lectern B_bookshelf
#endif
#ifndef B_fletching_table
#define B_fletching_table B_crafting_table
#endif
#ifndef B_smithing_table
#define B_smithing_table B_crafting_table
#endif
#ifndef B_blast_furnace
#define B_blast_furnace B_furnace
#endif
#ifndef B_smoker
#define B_smoker B_furnace
#endif
#ifndef B_brewing_stand
#define B_brewing_stand B_crafting_table
#endif
#ifndef B_cartography_table
#define B_cartography_table B_crafting_table
#endif
#ifndef B_grindstone
#define B_grindstone B_stone
#endif
#ifndef B_loom
#define B_loom B_crafting_table
#endif
#ifndef B_stonecutter
#define B_stonecutter B_stone
#endif
#ifndef B_cauldron
#define B_cauldron B_iron_block
#endif
#ifndef B_hay_block
#define B_hay_block B_bamboo_block
#endif
#ifndef B_white_wool
#define B_white_wool B_snow_block
#endif
#ifndef W_the_end
#define W_the_end W_plains
#endif
#ifndef W_end_highlands
#define W_end_highlands W_the_end
#endif
#ifndef W_small_end_islands
#define W_small_end_islands W_the_end
#endif

static Generator g;
static SurfaceNoise surface_noise_biome;
OctavePerlinNoiseSampler surface_noise;
OctavePerlinNoiseSampler detail_noise;
static OctavePerlinNoiseSampler cave_noise;
OctavePerlinNoiseSampler mountain_noise;
// Ore clump noise for clustered ore generation
static OctavePerlinNoiseSampler ore_clump_noise;
static NetherNoise nether_noise;
static int nether_noise_init = 0;
static int gen_initialized = 0;
static pthread_mutex_t worldgen_init_mutex = PTHREAD_MUTEX_INITIALIZER;

// Thread-local storage for height cache state.
// getHeightAt() is used by both gameplay and async chunk generation threads.
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
  #define TLS_STORAGE _Thread_local
#elif defined(__GNUC__)
  #define TLS_STORAGE __thread
#else
  #define TLS_STORAGE
#endif

// Height cache for smooth chunk generation (covers 32x32 area for seamless borders)
static TLS_STORAGE int cache_cx = 0x7FFFFFFF, cache_cz = 0x7FFFFFFF;
static TLS_STORAGE uint8_t height_cache[36][36];  // 32x32 + 2 block border on each side

// Thread-local biome cache to avoid redundant expensive cubiomes lookups
#define BIOME_CACHE_SIZE 1024
#define BIOME_CACHE_MASK (BIOME_CACHE_SIZE - 1)
typedef struct {
    int x, z;
    uint8_t biome;
    uint8_t valid;
} BiomeCacheEntry;
static TLS_STORAGE BiomeCacheEntry biome_cache[BIOME_CACHE_SIZE];

uint8_t getChunkBiomeRaw(short x, short z);
void init_worldgen(void);

uint8_t getChunkBiome(short x, short z) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)z * 668265263u;
    uint32_t idx = h & BIOME_CACHE_MASK;
    if (biome_cache[idx].valid && biome_cache[idx].x == x && biome_cache[idx].z == z) {
        return biome_cache[idx].biome;
    }
    uint8_t b = getChunkBiomeRaw(x, z);
    biome_cache[idx].x = x;
    biome_cache[idx].z = z;
    biome_cache[idx].biome = b;
    biome_cache[idx].valid = 1;
    return b;
}

uint8_t getChunkBiomeRaw (short x, short z) {
  init_worldgen();
  
  // Sample biome at an estimated surface height instead of fixed Y=64
  // This prevents ocean biomes from being detected where terrain should be above sea level
  int surface_x = x * 16 + 8;
  int surface_z = z * 16 + 8;
  
  // Get approximate height at this location to sample biome at surface level
  float height;
  int biomeID;
  if (mapApproxHeight(&height, &biomeID, &g, &surface_noise_biome, surface_x >> 2, surface_z >> 2, 1, 1) == 0) {
      // Fallback: sample at Y=64 if mapApproxHeight fails
      biomeID = getBiomeAt(&g, 1, surface_x, 64, surface_z);
  }
  
  // Map cubiomes IDs to our registry IDs (W_ constants generated from build_registries.js)
  // Ocean biomes are remapped to land biomes for a fully terrestrial world
  switch (biomeID) {
    case ocean: return W_plains;  // Ocean → Plains
    case plains: return W_plains;
    case sunflower_plains: return W_sunflower_plains;
    case desert: return W_desert;
    case desert_lakes: return W_desert;
    case windswept_hills: return W_windswept_hills;
    case forest: return W_forest;
    case flower_forest: return W_flower_forest;
    case taiga: return W_taiga;
    case taiga_mountains: return W_windswept_hills;
    case swamp: return W_swamp;
    case swamp_hills: return W_swamp;
    case river: return W_plains;  // River → Plains
    case frozen_ocean: return W_snowy_plains;  // Frozen Ocean → Snowy Plains
    case frozen_river: return W_snowy_plains;  // Frozen River → Snowy Plains
    case ice_spikes: return W_snowy_plains;
    case beach: return W_plains;  // Beach → Plains
    case desertHills: return W_desert;
    case forestHills: return W_forest;
    case taigaHills: return W_taiga;
    case jungle: return W_jungle;

    case modified_jungle: return W_jungle;
    case deep_ocean: return W_plains;  // Deep Ocean → Plains
    case birch_forest: return W_birch_forest;
    case tall_birch_forest: return W_birch_forest;
    case dark_forest: return W_dark_forest;
    case snowy_taiga: return W_snowy_taiga;
    case savanna: return W_savanna;
    case shattered_savanna: return W_savanna;
    case badlands: return W_badlands;
    case eroded_badlands: return W_eroded_badlands;
    case mangrove_swamp: return W_swamp;  // Mangrove Swamp → Swamp
    case meadow: return W_meadow;

    default:
        // Use isSnowy helper from cubiomes
        if (isSnowy(biomeID)) return W_snowy_plains;
        if (isDeepOcean(biomeID)) return W_plains;  // Deep Ocean → Plains
        if (isOceanic(biomeID)) return W_plains;  // Ocean → Plains
        if (isMesa(biomeID)) return W_badlands;
        return W_plains;
  }
}

// Returns a human-readable biome name from server biome ID (W_* constants)
const char* getBiomeName(uint8_t biome_id) {
  switch (biome_id) {
    case W_plains: return "Plains";
    case W_desert: return "Desert";
    case W_windswept_hills: return "Windswept Hills";
    case W_forest: return "Forest";
    case W_taiga: return "Taiga";
    case W_swamp: return "Swamp";
    case W_snowy_plains: return "Snowy Plains";
    case W_jungle: return "Jungle";
    case W_birch_forest: return "Birch Forest";
    case W_dark_forest: return "Dark Forest";
    case W_snowy_taiga: return "Snowy Taiga";
    case W_savanna: return "Savanna";
    case W_badlands: return "Badlands";
    case W_meadow: return "Meadow";
    case W_windswept_forest: return "Windswept Forest";
    case W_windswept_savanna: return "Windswept Savanna";
    case W_snowy_slopes: return "Snowy Slopes";
    case W_sunflower_plains: return "Sunflower Plains";
    case W_flower_forest: return "Flower Forest";
    case W_eroded_badlands: return "Eroded Badlands";
    default: return "Unknown";
  }
}

// Returns true if the biome ID matches the given name (case-insensitive substring match)
// Handles underscores and spaces interchangeably (e.g. "dark_forest" matches "Dark Forest")
uint8_t biomeNameMatches(uint8_t biome_id, const char* name, uint8_t name_len) {
  const char* biome_name = getBiomeName(biome_id);
  // Normalize: convert biome_name to lowercase, replacing spaces with underscores
  char normalized[32];
  uint8_t j = 0;
  for (uint8_t i = 0; biome_name[i] != '\0' && j < 31; i++) {
    char c = biome_name[i];
    if (c == ' ') c = '_';
    if (c >= 'A' && c <= 'Z') c += 32;
    normalized[j++] = c;
  }
  normalized[j] = '\0';
  // Normalize the search query too
  char query[32];
  if (name_len > 31) name_len = 31;
  for (uint8_t i = 0; i < name_len; i++) {
    char c = name[i];
    if (c == ' ') c = '_';
    if (c >= 'A' && c <= 'Z') c += 32;
    query[i] = c;
  }
  query[name_len] = '\0';
  // Substring search: check if query appears anywhere in normalized biome name
  for (uint8_t i = 0; normalized[i] != '\0'; i++) {
    uint8_t k;
    for (k = 0; query[k] != '\0' && normalized[i + k] == query[k]; k++);
    if (query[k] == '\0') return 1; // Found match
  }
  return 0;
}

// Returns a bitmask of which biome name components match the given query
// This allows fast pre-filtering before doing expensive biome lookups
// Each bit corresponds to a biome keyword: plains, desert, forest, taiga, swamp,
// jungle, snowy, birch, dark, savanna, badlands, meadow, windswept, flower, eroded, sunflower, slopes
uint32_t getBiomeKeywordMask(uint8_t biome_id) {
  switch (biome_id) {
    case W_plains:            return 1u << 0;  // plains
    case W_desert:            return 1u << 1;  // desert
    case W_windswept_hills:   return 1u << 12; // windswept
    case W_forest:            return 1u << 2;  // forest
    case W_taiga:             return 1u << 3;  // taiga
    case W_swamp:             return 1u << 4;  // swamp
    case W_snowy_plains:      return (1u << 5) | (1u << 0);  // snowy, plains
    case W_jungle:            return 1u << 6;  // jungle
    case W_birch_forest:      return (1u << 7) | (1u << 2);  // birch, forest
    case W_dark_forest:       return (1u << 8) | (1u << 2);  // dark, forest
    case W_snowy_taiga:       return (1u << 5) | (1u << 3);  // snowy, taiga
    case W_savanna:           return 1u << 9;  // savanna
    case W_badlands:          return 1u << 10; // badlands
    case W_meadow:            return 1u << 11; // meadow
    case W_windswept_forest:  return (1u << 12) | (1u << 2); // windswept, forest
    case W_windswept_savanna: return (1u << 12) | (1u << 9); // windswept, savanna
    case W_snowy_slopes:      return (1u << 5) | (1u << 14); // snowy, slopes
    case W_sunflower_plains:  return (1u << 13) | (1u << 0); // sunflower, plains
    case W_flower_forest:     return (1u << 15) | (1u << 2); // flower, forest
    case W_eroded_badlands:   return (1u << 16) | (1u << 10); // eroded, badlands
    default: return 0;
  }
}

// Returns a keyword mask for a search query string
uint32_t getQueryKeywordMask(const char* name, uint8_t name_len) {
  char query[32];
  if (name_len > 31) name_len = 31;
  for (uint8_t i = 0; i < name_len; i++) {
    char c = name[i];
    if (c == ' ') c = '_';
    if (c >= 'A' && c <= 'Z') c += 32;
    query[i] = c;
  }
  query[name_len] = '\0';

  uint32_t mask = 0;
  // Check for each keyword in the query
  if (strstr(query, "plains"))    mask |= 1u << 0;
  if (strstr(query, "desert"))    mask |= 1u << 1;
  if (strstr(query, "forest"))    mask |= 1u << 2;
  if (strstr(query, "taiga"))     mask |= 1u << 3;
  if (strstr(query, "swamp"))     mask |= 1u << 4;
  if (strstr(query, "snowy"))     mask |= 1u << 5;
  if (strstr(query, "jungle"))    mask |= 1u << 6;
  if (strstr(query, "birch"))     mask |= 1u << 7;
  if (strstr(query, "dark"))      mask |= 1u << 8;
  if (strstr(query, "savanna"))   mask |= 1u << 9;
  if (strstr(query, "badlands"))  mask |= 1u << 10;
  if (strstr(query, "meadow"))    mask |= 1u << 11;
  if (strstr(query, "windswept")) mask |= 1u << 12;
  if (strstr(query, "sunflower")) mask |= 1u << 13;
  if (strstr(query, "slopes"))    mask |= 1u << 14;
  if (strstr(query, "flower"))    mask |= 1u << 15;
  if (strstr(query, "eroded"))    mask |= 1u << 16;
  if (strstr(query, "hills"))     mask |= 1u << 17;
  return mask;
}

// Gets biome at arbitrary block coordinates (not chunk coordinates)
// Returns the server biome ID (W_* constant)
uint8_t getBiomeAtBlockCoords(int x, int z) {
  init_worldgen();

  int biomeID;
  float height;
  if (mapApproxHeight(&height, &biomeID, &g, &surface_noise_biome, x >> 2, z >> 2, 1, 1) == 0) {
    biomeID = getBiomeAt(&g, 1, x, 64, z);
  }

  // Same remapping as getChunkBiomeRaw
  switch (biomeID) {
    case ocean: return W_plains;
    case plains: return W_plains;
    case sunflower_plains: return W_sunflower_plains;
    case desert: return W_desert;
    case desert_lakes: return W_desert;
    case windswept_hills: return W_windswept_hills;
    case forest: return W_forest;
    case flower_forest: return W_flower_forest;
    case taiga: return W_taiga;
    case taiga_mountains: return W_windswept_hills;
    case swamp: return W_swamp;
    case swamp_hills: return W_swamp;
    case river: return W_plains;
    case frozen_ocean: return W_snowy_plains;
    case frozen_river: return W_snowy_plains;
    case ice_spikes: return W_snowy_plains;
    case beach: return W_plains;
    case desertHills: return W_desert;
    case forestHills: return W_forest;
    case taigaHills: return W_taiga;
    case jungle: return W_jungle;
    case modified_jungle: return W_jungle;
    case deep_ocean: return W_plains;
    case birch_forest: return W_birch_forest;
    case tall_birch_forest: return W_birch_forest;
    case dark_forest: return W_dark_forest;
    case snowy_taiga: return W_snowy_taiga;
    case savanna: return W_savanna;
    case shattered_savanna: return W_savanna;
    case badlands: return W_badlands;
    case eroded_badlands: return W_eroded_badlands;
    case mangrove_swamp: return W_swamp;
    case meadow: return W_meadow;
    default:
      if (isSnowy(biomeID)) return W_snowy_plains;
      if (isDeepOcean(biomeID)) return W_plains;
      if (isOceanic(biomeID)) return W_plains;
      if (isMesa(biomeID)) return W_badlands;
      return W_plains;
  }
}

void init_worldgen() {
    if (gen_initialized) return;
    pthread_mutex_lock(&worldgen_init_mutex);
    if (gen_initialized) {
        pthread_mutex_unlock(&worldgen_init_mutex);
        return;
    }
    setupGenerator(&g, MC_1_20, 0);
    applySeed(&g, DIMENSION_OVERWORLD, (uint64_t)world_seed);
    initSurfaceNoise(&surface_noise_biome, DIMENSION_OVERWORLD, (uint64_t)world_seed);

    // Surface noise: increased octaves for more dramatic terrain
    octave_init(&surface_noise, (uint64_t)world_seed, 6);
    // Detail noise: more octaves for rugged surface detail
    octave_init(&detail_noise, (uint64_t)world_seed + 1, 4);
    // Cave noise uses same settings as surface noise but different seed and scale
    octave_init(&cave_noise, (uint64_t)world_seed + 0x5DEECE66D, 4);
    // Mountain noise for large mountain peaks (low frequency, very high amplitude)
    // Lower frequency (larger features) and more octaves for dramatic peaks
    octave_init(&mountain_noise, (uint64_t)world_seed + 0x12345678, 5);
    // Ore clump noise for small scattered ore veins (2 octaves for localized blobs)
    octave_init(&ore_clump_noise, (uint64_t)world_seed + 0xABCDEF12, 2);
    if (!nether_noise_init) {
        setNetherSeed(&nether_noise, (uint64_t)world_seed);
        nether_noise_init = 1;
    }
    gen_initialized = 1;
    pthread_mutex_unlock(&worldgen_init_mutex);
}

uint32_t getChunkHash (short x, short z) {

  uint8_t buf[8];
  memcpy(buf, &x, 2);
  memcpy(buf + 2, &z, 2);
  memcpy(buf + 4, &world_seed, 4);

  return splitmix64(*((uint64_t *)buf));

}

uint8_t getCornerHeight (uint32_t hash, uint8_t biome) {
  // This function is kept for legacy support if needed, but we'll use noise instead
  return TERRAIN_BASE_HEIGHT;
}

// Raw height calculation with smooth biome blending across chunk boundaries
static uint8_t getHeightAtRaw (int x, int z) {
  double noise = octave_sample(&surface_noise, x * 0.0125, 0, z * 0.0125);
  double detail = octave_sample(&detail_noise, x * 0.05, 0, z * 0.05);

  // Sample biomes on a 32-block grid for smoother blending
  // This aligns better with chunk boundaries and reduces visible seams
  int gx = div_floor(x, 32);  // Grid cell X (32-block spacing)
  int gz = div_floor(z, 32);  // Grid cell Z
  int lx = mod_abs(x, 32);    // Local position in cell (0-31)
  int lz = mod_abs(z, 32);

  float wx = lx / 32.0f;
  float wz = lz / 32.0f;

  // Sample biomes at 4 grid corners (32 blocks apart)
  uint8_t b00 = getChunkBiome(gx, gz);
  uint8_t b10 = getChunkBiome(gx + 1, gz);
  uint8_t b01 = getChunkBiome(gx, gz + 1);
  uint8_t b11 = getChunkBiome(gx + 1, gz + 1);

  // Get base height and scale for each biome
  double base00 = 64.0, scale00 = 12.0;
  double base10 = 64.0, scale10 = 12.0;
  double base01 = 64.0, scale01 = 12.0;
  double base11 = 64.0, scale11 = 12.0;

  #define SET_BIOME_HEIGHT(biome, base, scale) \
    if (biome == W_desert) { base = 64.0; scale = 4.0; } \
    else if (biome == W_snowy_plains || biome == W_snowy_taiga) { base = 70.0; scale = 20.0; } \
    else if (biome == W_savanna) { base = 80.0; scale = 40.0; } \
    else if (biome == W_windswept_savanna) { base = 85.0; scale = 50.0; } \
    else if (biome == W_windswept_hills || biome == W_windswept_forest) { base = 95.0; scale = 70.0; } \
    else if (biome == W_jagged_peaks) { base = 120.0; scale = 110.0; } \
    else if (biome == W_frozen_peaks) { base = 110.0; scale = 95.0; } \
    else if (biome == W_stony_peaks) { base = 115.0; scale = 100.0; } \
    else if (biome == W_snowy_slopes) { base = 105.0; scale = 90.0; } \
    else if (biome == W_grove) { base = 90.0; scale = 60.0; } \
    else if (biome == W_cherry_grove) { base = 85.0; scale = 35.0; } \
    else if (biome == W_swamp) { base = 61.0; scale = 4.0; } \
    else if (biome == W_meadow) { base = 90.0; scale = 45.0; } \
    else if (biome == W_forest || biome == W_birch_forest || biome == W_dark_forest) { base = 70.0; scale = 25.0; } \
    else if (biome == W_flower_forest) { base = 75.0; scale = 30.0; } \
    else if (biome == W_taiga || biome == W_snowy_taiga || biome == W_old_growth_pine_taiga) { base = 72.0; scale = 22.0; } \
    else if (biome == W_jungle || biome == W_bamboo_jungle) { base = 75.0; scale = 20.0; } \
    else if (biome == W_plains || biome == W_sunflower_plains) { base = 68.0; scale = 12.0; } \
    else if (biome == W_badlands || biome == W_eroded_badlands) { base = 80.0; scale = 40.0; } \
    else if (biome == W_ice_spikes) { base = 65.0; scale = 15.0; } \
    else if (biome == W_mushroom_fields) { base = 65.0; scale = 10.0; }

  SET_BIOME_HEIGHT(b00, base00, scale00);
  SET_BIOME_HEIGHT(b10, base10, scale10);
  SET_BIOME_HEIGHT(b01, base01, scale01);
  SET_BIOME_HEIGHT(b11, base11, scale11);

  #undef SET_BIOME_HEIGHT

  // Bilinear interpolation of base height and scale
  double base_height = base00 * (1-wx) * (1-wz) + base10 * wx * (1-wz) +
                       base01 * (1-wx) * wz + base11 * wx * wz;
  double scale = scale00 * (1-wx) * (1-wz) + scale10 * wx * (1-wz) +
                 scale01 * (1-wx) * wz + scale11 * wx * wz;

  // Check if this is a mountain biome
  uint8_t is_mountain = (b00 == W_windswept_hills || b00 == W_jagged_peaks ||
                         b00 == W_frozen_peaks || b00 == W_stony_peaks ||
                         b00 == W_snowy_slopes || b00 == W_grove ||
                         b00 == W_windswept_forest || b00 == W_windswept_savanna ||
                         b10 == W_windswept_hills || b10 == W_jagged_peaks ||
                         b10 == W_frozen_peaks || b10 == W_stony_peaks ||
                         b10 == W_snowy_slopes || b10 == W_grove ||
                         b10 == W_windswept_forest || b10 == W_windswept_savanna ||
                         b01 == W_windswept_hills || b01 == W_jagged_peaks ||
                         b01 == W_frozen_peaks || b01 == W_stony_peaks ||
                         b01 == W_snowy_slopes || b01 == W_grove ||
                         b01 == W_windswept_forest || b01 == W_windswept_savanna ||
                         b11 == W_windswept_hills || b11 == W_jagged_peaks ||
                         b11 == W_frozen_peaks || b11 == W_stony_peaks ||
                         b11 == W_snowy_slopes || b11 == W_grove ||
                         b11 == W_windswept_forest || b11 == W_windswept_savanna);

  // Check if this is a forest biome (for forest mountains)
  uint8_t is_forest = (b00 == W_forest || b00 == W_birch_forest || b00 == W_dark_forest || b00 == W_flower_forest ||
                       b10 == W_forest || b10 == W_birch_forest || b10 == W_dark_forest || b10 == W_flower_forest ||
                       b01 == W_forest || b01 == W_birch_forest || b01 == W_dark_forest || b01 == W_flower_forest ||
                       b11 == W_forest || b11 == W_birch_forest || b11 == W_dark_forest || b11 == W_flower_forest);

  if (is_mountain) {
    // Sample mountain noise for large peak shapes (low frequency)
    double mountain = octave_sample(&mountain_noise, x * 0.005, 0, z * 0.005);

    // Normalize mountain noise to [0, 1]
    double mountain_factor = (mountain + 1.0) / 2.0;

    // Mountain peaks: reduced from 180 to 100 for less extreme terrain
    // Quadratic scaling instead of cubic for smoother peaks
    double peak_bonus = mountain_factor * mountain_factor * 100.0;

    // Add detail variation scaled by mountain factor
    base_height += peak_bonus;
    scale += mountain_factor * 25.0;

    // Extra detail for rugged mountain surfaces (reduced)
    base_height += detail * (5.0 + mountain_factor * 10.0);
  }

  // Forest biomes: add mountains and hills for more dramatic terrain
  if (is_forest && !is_mountain) {
    // Sample forest mountain noise
    double forest_mountain = octave_sample(&mountain_noise, x * 0.006, 0, z * 0.006);

    // Normalize to [0, 1]
    double forest_mountain_factor = (forest_mountain + 1.0) / 2.0;

    // Forest mountains are smaller than proper mountain biomes but still significant
    // Only the highest areas (top 40%) get mountains
    if (forest_mountain_factor > 0.6) {
      double mountain_bonus = (forest_mountain_factor - 0.6) * 60.0;
      base_height += mountain_bonus;
      scale += forest_mountain_factor * 15.0;
    }

    // Extra detail for forest terrain
    base_height += detail * 4.0;
  }

  // Desert biomes: flatter terrain with small rolling hills
  uint8_t is_desert = (b00 == W_desert || b10 == W_desert || b01 == W_desert || b11 == W_desert);
  if (is_desert && !is_mountain) {
    // Reduce scale for flatter terrain
    scale *= 0.6;

    // Add very small dune-like hills using detail noise
    double dune_noise = octave_sample(&detail_noise, x * 0.02, 0, z * 0.02);
    base_height += dune_noise * 2.0;
  }

  return (uint8_t)(base_height + noise * scale + detail * 2.0);
}

// Build height cache for a 32x32 superchunk (including 2-block border)
// This ensures seamless transitions between adjacent 16x16 chunk sections
static void buildHeightCache (int cx, int cz) {
  // Use 32x32 superchunks to ensure adjacent 16x16 sections share cache
  int scx = cx >> 1;  // Superchunk X
  int scz = cz >> 1;

  if (cache_cx == scx && cache_cz == scz) return;  // Already cached

  cache_cx = scx;
  cache_cz = scz;

  int base_x = scx * 32;
  int base_z = scz * 32;

  // Generate raw heights for 37x37 area to allow averaging for 36x36 blocks
  // Sampling each point once is much faster than sampling 4 points per block.
  static TLS_STORAGE uint8_t raw_heights[37][37];
  for (int dz = 0; dz < 37; dz++) {
    for (int dx = 0; dx < 37; dx++) {
      raw_heights[dx][dz] = getHeightAtRaw(base_x + dx - 2, base_z + dz - 2);
    }
  }

  // Apply smoothing filter to reduce visible seams
  for (int dz = 0; dz < 36; dz++) {
    for (int dx = 0; dx < 36; dx++) {
      // Average the samples for smoother transitions
      height_cache[dx][dz] = (raw_heights[dx][dz] + raw_heights[dx+1][dz] +
                              raw_heights[dx][dz+1] + raw_heights[dx+1][dz+1]) / 4;
    }
  }
}

// Get terrain height at the given coordinates with caching
// Does *not* account for block changes
uint8_t getHeightAt (int x, int z) {
  init_worldgen();

  // Calculate which 16x16 section this coordinate belongs to
  int cx = div_floor(x, 16);
  int cz = div_floor(z, 16);
  int lx = mod_abs(x, 16);
  int lz = mod_abs(z, 16);

  // Build cache for the superchunk containing this section
  buildHeightCache(cx, cz);

  // Calculate position in the 36x36 cache
  // cx & 1 gives us which 16x16 section within the 32x32 superchunk
  int cache_x = 2 + (cx & 1) * 16 + lx;
  int cache_z = 2 + (cz & 1) * 16 + lz;

  return height_cache[cache_x][cache_z];
}

uint8_t getHeightAtFromAnchors (int rx, int rz, ChunkAnchor *anchor_ptr) {
  return getHeightAt(anchor_ptr[0].x * CHUNK_SIZE + rx, anchor_ptr[0].z * CHUNK_SIZE + rz);
}

uint8_t getHeightAtFromHash (int rx, int rz, int _x, int _z, uint32_t chunk_hash, uint8_t biome) {
  return getHeightAt(_x * CHUNK_SIZE + rx, _z * CHUNK_SIZE + rz);
}

// Check if this position is an ore "seed" (starting point of a vein)
// Returns the ore type or 0 if not a seed
static uint16_t getOreSeedAt(int x, int y, int z, uint8_t biome) {
  // Sample ore clump noise to find vein centers
  double ore_noise = octave_sample(&ore_clump_noise, x * 0.12, y * 0.12, z * 0.12);
  double ore_density = (ore_noise + 1.0) / 2.0;
  
  // Use position-based hash for additional randomness
  uint8_t ore_hash = (uint8_t)(x * 31 + y * 37 + z * 41);
  
  // Diamond: deep underground (Y < 16), rare
  if (y < 16) {
    if (ore_density > 0.75 && (ore_hash & 0x1F) == 0) return B_diamond_ore;
    if (ore_density > 0.70 && (ore_hash & 0x3F) == 0) return B_gold_ore;
  }

  // Emerald: rare cave-level ore, more common in mountain-like biomes.
  if (y >= 8 && y < 48) {
    uint8_t mountain_biome = (
      biome == W_windswept_hills ||
      biome == W_windswept_forest ||
      biome == W_stony_peaks ||
      biome == W_jagged_peaks ||
      biome == W_frozen_peaks ||
      biome == W_snowy_slopes ||
      biome == W_grove
    );
    if (mountain_biome) {
      if (ore_density > 0.60 && (ore_hash & 0x0F) == 0) return B_emerald_ore;
    } else if (ore_density > 0.74 && (ore_hash & 0x3F) == 0) return B_emerald_ore;
  }
  
  // Iron: mid-level (Y 16-48), very common
  if (y >= 16 && y < 48) {
    if (ore_density > 0.55 && (ore_hash & 0x07) == 0) return B_iron_ore;
    if (ore_density > 0.50 && (ore_hash & 0x0F) == 0) return B_copper_ore;
  }
  
  // Coal: widespread (Y < 64), very common
  if (y < 64) {
    if (ore_density > 0.55 && (ore_hash & 0x07) == 0) return B_coal_ore;
  }
  
  return 0;
}

// Generate ore at position, including spreading from nearby seeds
// Returns the ore block type or 0 if no ore
static uint16_t getOreClumpAt(int x, int y, int z, uint8_t biome) {
  // First, check if this block is an ore seed
  uint16_t seed_ore = getOreSeedAt(x, y, z, biome);
  if (seed_ore != 0) return seed_ore;
  
  // Not a seed - check if adjacent to a seed and spread with 1/4 chance
  // Each direction has its own hash for independent spread chance
  uint16_t adjacent_ore = 0;
  
  // Check +X neighbor (25% chance to spread)
  adjacent_ore = getOreSeedAt(x + 1, y, z, biome);
  if (adjacent_ore != 0 && ((x * 13 + y * 37 + z * 53) & 0x03) == 0) return adjacent_ore;
  
  // Check -X neighbor
  adjacent_ore = getOreSeedAt(x - 1, y, z, biome);
  if (adjacent_ore != 0 && ((x * 17 + y * 41 + z * 59) & 0x03) == 0) return adjacent_ore;
  
  // Check +Y neighbor
  adjacent_ore = getOreSeedAt(x, y + 1, z, biome);
  if (adjacent_ore != 0 && ((x * 19 + y * 43 + z * 61) & 0x03) == 0) return adjacent_ore;
  
  // Check -Y neighbor
  adjacent_ore = getOreSeedAt(x, y - 1, z, biome);
  if (adjacent_ore != 0 && ((x * 23 + y * 47 + z * 67) & 0x03) == 0) return adjacent_ore;
  
  // Check +Z neighbor
  adjacent_ore = getOreSeedAt(x, y, z + 1, biome);
  if (adjacent_ore != 0 && ((x * 29 + y * 51 + z * 71) & 0x03) == 0) return adjacent_ore;
  
  // Check -Z neighbor
  adjacent_ore = getOreSeedAt(x, y, z - 1, biome);
  if (adjacent_ore != 0 && ((x * 31 + y * 53 + z * 73) & 0x03) == 0) return adjacent_ore;
  
  return 0;
}

// Simple cave check using surface-like noise
// Reuses the same noise type as terrain but with different scaling
static inline uint8_t isCaveSimple(int x, int y, int z, uint8_t height, uint8_t biome) {
  // Don't generate caves too close to the surface
  if (y > height - 4) return 0;

  // Don't generate caves below bedrock level
  if (y < 5) return 0;

  // Use 3D noise with surface-like parameters for natural cave shapes
  double cave_scale = 0.04;  // Larger scale = bigger, smoother caves
  double noise = octave_sample(&cave_noise, x * cave_scale, y * cave_scale * 0.5, z * cave_scale);

  // Normalize from [-1, 1] to [0, 1]
  double cave_density = (noise + 1.0) / 2.0;

  // Cave threshold - higher = fewer caves
  double threshold = 0.65;

  // Mountain biomes have more caves
  if (biome == W_windswept_hills || biome == W_jagged_peaks ||
      biome == W_frozen_peaks || biome == W_stony_peaks) {
    threshold = 0.58;
  }

  return cave_density > threshold ? 1 : 0;
}

static uint16_t getMineshaftBlockAt(int x, int y, int z) {
  if (y < 16 || y >= 64) return 0xFFFF;

  int cx = div_floor(x, 16) * 16;
  int cy = div_floor(y, 16) * 16;
  int cz = div_floor(z, 16) * 16;

  uint64_t chunk_key = splitmix64(((uint64_t)(uint32_t)cx * 73856093 ^ (uint64_t)(uint32_t)cz * 83492791) ^ world_seed);
  if ((uint32_t)(chunk_key % 5) != 0) return 0xFFFF;

  int target_cy = ((chunk_key >> 16) % 3) * 16 + 16;
  if (cy != target_cy) return 0xFFFF;

  uint64_t seed = splitmix64((chunk_key >> 32) ^ (uint64_t)(uint32_t)cy);

  int dir = (seed >> 7) & 1;
  int pos = (seed >> 8) & 0xF;
  int y_off = ((seed >> 12) & 0xE) + 1;
  if (y_off > 11) y_off = 11;

  int lx = x - cx;
  int ly = y - cy;
  int lz = z - cz;

  int step, dw;
  if (dir == 0) { step = lx; dw = lz - pos; }
  else          { step = lz; dw = lx - pos; }

  if (step < 0 || step >= 16) return 0xFFFF;
  if (dw < -2 || dw > 2) return 0xFFFF;

  int dy = ly - y_off;
  if (dy < 0 || dy >= 5) return 0xFFFF;

  int support = (step % 5) == 2;
  uint32_t dec = (uint32_t)(seed >> 16);
  int has_torch = !support && ((dec >> (step * 2 + 1)) & 1);
  int has_cobweb = !support && ((dec >> (step * 3 + 16)) & 3) == 0;

  if (support && (dw == -2 || dw == 2)) {
    return B_oak_fence;
  }
  if (dy == 0) return B_oak_planks;
  if (dy == 1 && dw >= -1 && dw <= 1) {
    if (dw == 0) {
      if (has_torch) return B_torch;
      if (has_cobweb) return B_cobweb;
    }
    return B_air;
  }
  if (dw == -2 || dw == 2) return B_oak_planks;
  if (support && dy >= 4) return B_oak_planks;
  return B_air;
}

// ============================================================
// Dungeons
// ============================================================
// Small underground cobblestone rooms with a central spawner and loot chests.
// Cells are spaced widely enough that each dungeon fits entirely within its own
// cell, so lookup only needs to inspect the cell containing a queried block.
#define DUNGEON_SPACING 96
#define DUNGEON_MARGIN 24
#define DUNGEON_NONE 0xFFFF

static uint8_t getDungeonCell(int cell_x, int cell_z, int *center_x, int *center_z, int *floor_y, uint64_t *out_key) {
  uint64_t key = splitmix64(
    ((uint64_t)(uint32_t)cell_x * 0xD1B54A32D192ED03ULL) ^
    ((uint64_t)(uint32_t)cell_z * 0x94D049BB133111EBULL) ^
    ((uint64_t)(uint32_t)world_seed * 0x9E3779B97F4A7C15ULL) ^
    0xA0D6E0B51E5A7C3FULL
  );

  // Roughly one dungeon every ~135 blocks on average.
  if (key & 1ULL) return 0;

  int range = DUNGEON_SPACING - DUNGEON_MARGIN * 2;
  *center_x = cell_x * DUNGEON_SPACING + DUNGEON_MARGIN + (int)((key >> 8) % (uint64_t)range);
  *center_z = cell_z * DUNGEON_SPACING + DUNGEON_MARGIN + (int)((key >> 24) % (uint64_t)range);

  int surface = getHeightAt(*center_x, *center_z);
  int max_floor_y = surface - 12;
  if (max_floor_y < 8) return 0;

  int candidate_y = 10 + (int)((key >> 40) % 36); // Y 10..45 before surface clamping.
  if (candidate_y > max_floor_y) candidate_y = max_floor_y;
  if (candidate_y > 50) candidate_y = 50;
  if (candidate_y < 8) candidate_y = 8;

  *floor_y = candidate_y;
  if (out_key != NULL) *out_key = key;
  return 1;
}

static uint16_t getDungeonWallBlockAt(int x, int y, int z, uint64_t key) {
  uint64_t h = splitmix64(
    ((uint64_t)(uint32_t)x * 0x632BE59BD9B4E019ULL) ^
    ((uint64_t)(uint32_t)y * 0x85157AF5ULL) ^
    ((uint64_t)(uint32_t)z * 0x9E3779B1ULL) ^
    key
  );

  if ((h & 7ULL) == 0) return B_mossy_cobblestone;
  if ((h & 31ULL) == 1) return B_stone;
  return B_cobblestone;
}

static uint8_t getDungeonChestAtRelative(int rx, int ry, int rz, int half_x, int half_z, uint64_t key, uint8_t *direction, uint8_t *variant) {
  if (ry != 1) return 0;

  if (rx == -half_x + 1 && rz == -half_z + 1) {
    if (direction != NULL) *direction = 1; // south-facing in the chest state order used by chunk packing.
    if (variant != NULL) *variant = 0;
    return 1;
  }

  // Most dungeons get a second chest in the opposite corner.
  if (((key >> 50) & 3ULL) != 0 && rx == half_x - 1 && rz == half_z - 1) {
    if (direction != NULL) *direction = 0; // north-facing.
    if (variant != NULL) *variant = 1;
    return 1;
  }

  return 0;
}

static uint16_t getDungeonBlockAt(int x, int y, int z) {
  if (y < 8 || y > 58) return DUNGEON_NONE;

  int cell_x = div_floor(x, DUNGEON_SPACING);
  int cell_z = div_floor(z, DUNGEON_SPACING);
  int cx, cz, floor_y;
  uint64_t key;
  if (!getDungeonCell(cell_x, cell_z, &cx, &cz, &floor_y, &key)) return DUNGEON_NONE;

  int half_x = 4 + (int)((key >> 3) & 1ULL);
  int half_z = 4 + (int)((key >> 4) & 1ULL);
  int rx = x - cx;
  int ry = y - floor_y;
  int rz = z - cz;

  if (rx < -half_x || rx > half_x || rz < -half_z || rz > half_z || ry < 0 || ry > 5) return DUNGEON_NONE;

  uint8_t chest_dir = 0;
  uint8_t chest_variant = 0;
  if (getDungeonChestAtRelative(rx, ry, rz, half_x, half_z, key, &chest_dir, &chest_variant)) {
    (void)chest_variant;
    return 0x8000 | B_chest | ((uint16_t)chest_dir << 9);
  }

  if (ry == 1 && rx == 0 && rz == 0) return B_spawner;

  uint8_t wall = (ry == 0 || ry == 5 || rx == -half_x || rx == half_x || rz == -half_z || rz == half_z);
  if (wall) {
    // A couple of arched openings let caves/mineshafts naturally connect.
    if (ry >= 1 && ry <= 3) {
      if (rx == 0 && rz == -half_z && ((key >> 52) & 1ULL)) return B_air;
      if (rx == 0 && rz ==  half_z && ((key >> 53) & 1ULL)) return B_air;
      if (rz == 0 && rx == -half_x && ((key >> 54) & 1ULL)) return B_air;
      if (rz == 0 && rx ==  half_x && ((key >> 55) & 1ULL)) return B_air;
    }
    return getDungeonWallBlockAt(x, y, z, key);
  }

  if (ry >= 2 && ry <= 4) {
    uint64_t h = splitmix64(
      ((uint64_t)(uint32_t)x * 0x45D9F3BULL) ^
      ((uint64_t)(uint32_t)y * 0x27D4EB2DULL) ^
      ((uint64_t)(uint32_t)z * 0x119DE1F3ULL) ^
      key
    );
    if ((h & 0x7F) == 0x2A) return B_cobweb;
  }

  return B_air;
}

uint8_t getDungeonChestInfo(short x, uint8_t y, short z, uint8_t *direction, uint8_t *variant) {
  int cell_x = div_floor(x, DUNGEON_SPACING);
  int cell_z = div_floor(z, DUNGEON_SPACING);
  int cx, cz, floor_y;
  uint64_t key;
  if (!getDungeonCell(cell_x, cell_z, &cx, &cz, &floor_y, &key)) return 0;

  int half_x = 4 + (int)((key >> 3) & 1ULL);
  int half_z = 4 + (int)((key >> 4) & 1ULL);
  int rx = (int)x - cx;
  int ry = (int)y - floor_y;
  int rz = (int)z - cz;

  if (rx < -half_x || rx > half_x || rz < -half_z || rz > half_z || ry < 0 || ry > 5) return 0;
  return getDungeonChestAtRelative(rx, ry, rz, half_x, half_z, key, direction, variant);
}

uint8_t getNearbyDungeonSpawners(int px, int py, int pz, int radius, DungeonSpawnerInfo *out, uint8_t max_spawners) {
  if (out == NULL || max_spawners == 0 || radius <= 0) return 0;

  int min_cell_x = div_floor(px - radius - DUNGEON_MARGIN, DUNGEON_SPACING);
  int max_cell_x = div_floor(px + radius + DUNGEON_MARGIN, DUNGEON_SPACING);
  int min_cell_z = div_floor(pz - radius - DUNGEON_MARGIN, DUNGEON_SPACING);
  int max_cell_z = div_floor(pz + radius + DUNGEON_MARGIN, DUNGEON_SPACING);

  uint8_t count = 0;
  for (int cz_cell = min_cell_z; cz_cell <= max_cell_z && count < max_spawners; cz_cell++) {
    for (int cx_cell = min_cell_x; cx_cell <= max_cell_x && count < max_spawners; cx_cell++) {
      int cx, cz, floor_y;
      uint64_t key;
      if (!getDungeonCell(cx_cell, cz_cell, &cx, &cz, &floor_y, &key)) continue;
      (void)key;

      int sy = floor_y + 1;
      if (abs(cx - px) > radius || abs(cz - pz) > radius) continue;
      if (abs(sy - py) > 12) continue;

      out[count].x = (short)cx;
      out[count].y = (uint8_t)sy;
      out[count].z = (short)cz;
      out[count].mob_type = E_ZOMBIE;
      count++;
    }
  }

  return count;
}

// ============================================================
// Strongholds
// ============================================================
// Deterministic underground strongholds. Each stronghold fits fully inside its
// spawn cell, so a block only needs to inspect the cell containing its x/z.
#define STRONGHOLD_SPACING 256
#define STRONGHOLD_MARGIN 64
#define STRONGHOLD_NONE 0xFFFF

static uint8_t getStrongholdCell(int cell_x, int cell_z, int *center_x, int *center_z, int *base_y, uint64_t *out_key) {
  uint64_t key = splitmix64(
    ((uint64_t)(uint32_t)cell_x * 0x9E3779B97F4A7C15ULL) ^
    ((uint64_t)(uint32_t)cell_z * 0xBF58476D1CE4E5B9ULL) ^
    ((uint64_t)(uint32_t)world_seed * 0x94D049BB133111EBULL) ^
    0x51A7E5B91D3C4F2AULL
  );

  // Roughly one stronghold every ~440 blocks on average. This is frequent
  // enough to find by exploring, but sparse enough that they feel special.
  if ((uint32_t)(key % 3) != 0) return 0;

  int range = STRONGHOLD_SPACING - STRONGHOLD_MARGIN * 2;
  *center_x = cell_x * STRONGHOLD_SPACING + STRONGHOLD_MARGIN + (int)((key >> 8) % (uint64_t)range);
  *center_z = cell_z * STRONGHOLD_SPACING + STRONGHOLD_MARGIN + (int)((key >> 24) % (uint64_t)range);
  *base_y = 20 + (int)((key >> 40) % 18); // Y 20..37, safely underground in normal terrain.
  if (out_key != NULL) *out_key = key;
  return 1;
}

static uint16_t getStrongholdBrickAt(int x, int y, int z, uint64_t key) {
  uint64_t h = splitmix64(
    ((uint64_t)(uint32_t)x * 0x632BE59BD9B4E019ULL) ^
    ((uint64_t)(uint32_t)y * 0x85157AF5ULL) ^
    ((uint64_t)(uint32_t)z * 0x9E3779B1ULL) ^
    key
  );

  if ((h & 0x3F) == 0) return B_chiseled_stone_bricks;
  if ((h & 0x0F) == 0) return B_mossy_stone_bricks;
  if ((h & 0x1F) == 1) return B_cracked_stone_bricks;
  return B_stone_bricks;
}

static void strongholdMarkRoom(int rx, int ry, int rz,
                               int min_x, int max_x, int min_y, int max_y, int min_z, int max_z,
                               uint8_t *matched, uint8_t *carve_air, uint16_t *solid, uint64_t key) {
  if (rx < min_x || rx > max_x || ry < min_y || ry > max_y || rz < min_z || rz > max_z) return;
  *matched = 1;

  uint8_t wall = (
    rx == min_x || rx == max_x ||
    ry == min_y || ry == max_y ||
    rz == min_z || rz == max_z
  );

  if (wall) {
    if (!*carve_air) *solid = getStrongholdBrickAt(rx, ry, rz, key);
  } else {
    *carve_air = 1;
  }
}

static void strongholdMarkCorridorX(int rx, int ry, int rz,
                                    int min_x, int max_x, int min_y, int max_y, int min_z, int max_z,
                                    uint8_t *matched, uint8_t *carve_air, uint16_t *solid, uint64_t key) {
  if (rx < min_x || rx > max_x || ry < min_y || ry > max_y || rz < min_z || rz > max_z) return;
  *matched = 1;

  // X corridors have open ends; only their floors, ceilings, and side walls are solid.
  uint8_t wall = (ry == min_y || ry == max_y || rz == min_z || rz == max_z);
  if (wall) {
    if (!*carve_air) *solid = getStrongholdBrickAt(rx, ry, rz, key);
  } else {
    *carve_air = 1;
  }
}

static void strongholdMarkCorridorZ(int rx, int ry, int rz,
                                    int min_x, int max_x, int min_y, int max_y, int min_z, int max_z,
                                    uint8_t *matched, uint8_t *carve_air, uint16_t *solid, uint64_t key) {
  if (rx < min_x || rx > max_x || ry < min_y || ry > max_y || rz < min_z || rz > max_z) return;
  *matched = 1;

  // Z corridors have open ends; only their floors, ceilings, and side walls are solid.
  uint8_t wall = (ry == min_y || ry == max_y || rx == min_x || rx == max_x);
  if (wall) {
    if (!*carve_air) *solid = getStrongholdBrickAt(rx, ry, rz, key);
  } else {
    *carve_air = 1;
  }
}

static uint16_t getStrongholdPortalFrameBlock(int rx, int rz) {
  if (rz == -2 && rx >= -1 && rx <= 1) return B_end_portal_frame_eye_south;
  if (rz ==  2 && rx >= -1 && rx <= 1) return B_end_portal_frame_eye_north;
  if (rx == -2 && rz >= -1 && rz <= 1) return B_end_portal_frame_eye_east;
  if (rx ==  2 && rz >= -1 && rz <= 1) return B_end_portal_frame_eye_west;
  return STRONGHOLD_NONE;
}

static uint16_t getStrongholdDecorationAt(int rx, int ry, int rz, uint64_t key) {
  // Portal room: a complete visual-only 3x3 End portal with all 12 frame eyes filled.
  if (ry == 1) {
    if (rx >= -1 && rx <= 1 && rz >= -1 && rz <= 1) return B_end_portal;

    uint16_t frame = getStrongholdPortalFrameBlock(rx, rz);
    if (frame != STRONGHOLD_NONE) return frame;

    if ((rx == -6 || rx == 6) && (rz == -4 || rz == 4)) return B_torch;
  }

  if (ry == 0 && rx >= -1 && rx <= 1 && rz >= -1 && rz <= 1) return B_lava;

  // Portal-room barred alcoves.
  if (ry >= 2 && ry <= 4) {
    if ((rx == -7 || rx == 7) && (rz == -4 || rz == 4)) return B_iron_bars;
    if ((rz == -5 || rz == 5) && (rx == -5 || rx == 5)) return B_iron_bars;
  }

  // Library north of the main crossing: shelves along walls and two shelf rows.
  if (rx >= -46 && rx <= -22 && rz >= -42 && rz <= -25 && ry >= 1 && ry <= 5) {
    if (rx == -45 || rx == -23 || rz == -41 || rz == -26) return B_bookshelf;
    if ((rz == -34 || rz == -33) && rx >= -42 && rx <= -26 && (ry == 1 || ry == 2 || ry == 4)) return B_bookshelf;
    if (ry == 1 && (rx == -34 || rx == -33) && rz > -40 && rz < -27) return B_oak_planks;
  }

  // Small lava fountain room south of the crossing.
  if (rx >= -42 && rx <= -22 && rz >= 25 && rz <= 42) {
    if (ry == 1 && rx >= -34 && rx <= -30 && rz >= 32 && rz <= 36) return B_lava;
    if (ry == 1 && ((rx == -36 && rz == 34) || (rx == -28 && rz == 34) || (rx == -32 && rz == 30) || (rx == -32 && rz == 38))) return B_torch;
  }

  // Occasional cobwebs in hallways and rooms.
  if (ry >= 2 && ry <= 3) {
    uint64_t h = splitmix64(((uint64_t)(uint32_t)rx * 0x45D9F3BULL) ^ ((uint64_t)(uint32_t)rz * 0x119DE1F3ULL) ^ key);
    if ((h & 0xFF) == 0x3A) return B_cobweb;
  }

  return STRONGHOLD_NONE;
}

static uint16_t getStrongholdBlockAt(int x, int y, int z) {
  if (y < 16 || y > 48) return STRONGHOLD_NONE;

  int cell_x = div_floor(x, STRONGHOLD_SPACING);
  int cell_z = div_floor(z, STRONGHOLD_SPACING);
  int cx, cz, base_y;
  uint64_t key;
  if (!getStrongholdCell(cell_x, cell_z, &cx, &cz, &base_y, &key)) return STRONGHOLD_NONE;

  int rx = x - cx;
  int ry = y - base_y;
  int rz = z - cz;
  if (rx < -48 || rx > 16 || rz < -44 || rz > 44 || ry < 0 || ry > 8) return STRONGHOLD_NONE;

  uint8_t matched = 0;
  uint8_t carve_air = 0;
  uint16_t solid = B_stone_bricks;

  // Central portal room.
  strongholdMarkRoom(rx, ry, rz, -8, 8, 0, 6, -6, 6, &matched, &carve_air, &solid, key);

  // Main hall and four-way crossing.
  strongholdMarkCorridorX(rx, ry, rz, -32, -8, 0, 4, -2, 2, &matched, &carve_air, &solid, key);
  strongholdMarkRoom(rx, ry, rz, -38, -24, 0, 5, -6, 6, &matched, &carve_air, &solid, key);
  strongholdMarkCorridorZ(rx, ry, rz, -34, -30, 0, 4, -28, 28, &matched, &carve_air, &solid, key);

  // Side rooms branching from the crossing.
  strongholdMarkRoom(rx, ry, rz, -46, -22, 0, 7, -42, -25, &matched, &carve_air, &solid, key);
  strongholdMarkRoom(rx, ry, rz, -42, -22, 0, 5, 25, 42, &matched, &carve_air, &solid, key);

  if (!matched) return STRONGHOLD_NONE;

  uint16_t decoration = getStrongholdDecorationAt(rx, ry, rz, key);
  if (decoration != STRONGHOLD_NONE) return decoration;

  return carve_air ? B_air : solid;
}

static uint8_t isVillageHouseBiome(uint8_t biome) {
  switch (biome) {
    case W_plains: case W_forest: case W_sunflower_plains:
    case W_desert: case W_savanna:
    case W_taiga: case W_snowy_taiga: case W_old_growth_pine_taiga:
    case W_birch_forest: case W_flower_forest:
      return 1;
    default:
      return 0;
  }
}

#define VILLAGE_SPACING 128
#define VILLAGE_PROFESSION_COUNT 13

#define VILLAGE_PROF_FARMER 0
#define VILLAGE_PROF_LIBRARIAN 1
#define VILLAGE_PROF_CLERIC 2
#define VILLAGE_PROF_ARMORER 3
#define VILLAGE_PROF_BUTCHER 4
#define VILLAGE_PROF_CARTOGRAPHER 5
#define VILLAGE_PROF_FISHERMAN 6
#define VILLAGE_PROF_FLETCHER 7
#define VILLAGE_PROF_LEATHERWORKER 8
#define VILLAGE_PROF_MASON 9
#define VILLAGE_PROF_SHEPHERD 10
#define VILLAGE_PROF_TOOLSMITH 11
#define VILLAGE_PROF_WEAPONSMITH 12

#define VILLAGE_STYLE_PLAINS 0
#define VILLAGE_STYLE_DESERT 1
#define VILLAGE_STYLE_SAVANNA 2
#define VILLAGE_STYLE_TAIGA 3
#define VILLAGE_STYLE_SNOWY 4


typedef struct {
  int cx;
  int cz;
  uint64_t key;
  uint8_t biome;
  uint8_t houses;
  int center_y;
  int hx[16];
  int hz[16];
  int hy[16];
  uint8_t variant[16];
  uint8_t profession[16];
} VillageLayout;

static uint8_t buildVillageLayoutForCell(int vx, int vz, VillageLayout *layout) {
  uint64_t key = splitmix64(((uint64_t)(uint32_t)vx * 1234567 ^ (uint64_t)(uint32_t)vz * 7654321) ^ world_seed);
  if ((uint32_t)(key % 4) != 0) return 0;

  int cx = vx * VILLAGE_SPACING + 56 + (int)((key >> 8) % 17);
  int cz = vz * VILLAGE_SPACING + 56 + (int)((key >> 16) % 17);
  uint8_t biome = getChunkBiome(div_floor(cx, 32), div_floor(cz, 32));
  if (!isVillageHouseBiome(biome)) return 0;

  layout->cx = cx;
  layout->cz = cz;
  layout->key = key;
  layout->biome = biome;
  layout->houses = 0;
  layout->center_y = getHeightAt(cx, cz);
  if (layout->center_y < 63) return 0;

  static const int8_t off_x[VILLAGE_PROFESSION_COUNT] = { 18, -18,   0,  34, -34,  34, -34,  18, -18,  42, -42,  18, -18 };
  static const int8_t off_z[VILLAGE_PROFESSION_COUNT] = { -8,  -8, -30,  -8,  -8,  16,  16,  36,  36, -32, -32, -42, -42 };

  for (int i = 0; i < VILLAGE_PROFESSION_COUNT && layout->houses < 16; i++) {
    int hx = cx + off_x[i];
    int hz = cz + off_z[i];
    int hy = getHeightAt(hx, hz);
    if (hy < 63) continue;

    uint8_t idx = layout->houses++;
    layout->hx[idx] = hx;
    layout->hz[idx] = hz;
    layout->hy[idx] = hy;
    layout->variant[idx] = (uint8_t)((key >> (i + 28)) & 1ULL);
    layout->profession[idx] = (uint8_t)i;
  }

  return layout->houses > 0;
}

static uint8_t getVillageLayout(int x, int z, VillageLayout *layout) {
  int vx = div_floor(x, VILLAGE_SPACING);
  int vz = div_floor(z, VILLAGE_SPACING);

  static TLS_STORAGE int cached_vx = 0x7FFFFFFF;
  static TLS_STORAGE int cached_vz = 0x7FFFFFFF;
  static TLS_STORAGE uint8_t cached_exists = 0;
  static TLS_STORAGE VillageLayout cached_layout;

  if (cached_vx != vx || cached_vz != vz) {
    cached_vx = vx;
    cached_vz = vz;
    cached_exists = buildVillageLayoutForCell(vx, vz, &cached_layout);
  }

  if (!cached_exists) return 0;
  if (layout != NULL) memcpy(layout, &cached_layout, sizeof(VillageLayout));
  return 1;
}

static void getVillagePalette(uint8_t biome, uint8_t variant, uint16_t *wall, uint16_t *roof, uint16_t *pillar, uint16_t *roof_slab, uint16_t *door, uint16_t *fence) {
  *wall = (variant == 1) ? B_cobblestone : B_oak_planks;
  *roof = B_oak_planks;
  *pillar = B_oak_log;
  *roof_slab = B_oak_slab;
  *door = B_oak_door;
  *fence = B_oak_fence;

  switch (biome) {
    case W_desert:
      *wall = (variant == 1) ? B_cut_sandstone : B_sandstone;
      *roof = B_sandstone;
      *pillar = B_sandstone;
      *roof_slab = B_sandstone_slab;
      *fence = B_cobblestone;
      break;
    case W_savanna:
      *wall = (variant == 1) ? B_cobblestone : B_acacia_planks;
      *roof = B_acacia_planks;
      *pillar = B_acacia_log;
      *roof_slab = B_acacia_slab;
      *door = B_acacia_door;
      *fence = B_acacia_fence;
      break;
    case W_taiga: case W_old_growth_pine_taiga: case W_snowy_taiga:
      *wall = (variant == 1) ? B_cobblestone : B_spruce_planks;
      *roof = (biome == W_snowy_taiga) ? B_snow_block : B_spruce_planks;
      *pillar = B_spruce_log;
      *roof_slab = B_spruce_slab;
      *door = B_spruce_door;
      *fence = B_spruce_fence;
      break;
    case W_birch_forest:
      *wall = (variant == 1) ? B_cobblestone : B_birch_planks;
      *roof = B_birch_planks;
      *pillar = B_birch_log;
      *roof_slab = B_birch_slab;
      *door = B_birch_door;
      *fence = B_birch_fence;
      break;
  }
}

static uint16_t getVillagePathBlockAt(int x, int y, int z, uint8_t surface, VillageLayout *v) {
  int dx = x - v->cx;
  int dz = z - v->cz;
  int adx = dx < 0 ? -dx : dx;
  int adz = dz < 0 ? -dz : dz;

  if (adx <= 5 && adz <= 5) {
    int rel_y = y - v->center_y;

    // Meeting point: well at center, path-covered plaza.
    if (rel_y == 0) {
      if (adx == 0 && adz == 0) return B_water;
      if (adx <= 1 && adz <= 1) return B_cobblestone;
      return B_dirt_path;
    }
  }

  uint8_t on_road = (adx <= 2 && adz <= 50) || (adz <= 2 && adx <= 50);
  for (int i = 0; i < v->houses && !on_road; i++) {
    int hx = v->hx[i];
    int hz = v->hz[i];
    if (((x >= hx - 2 && x <= hx + 2) && ((z >= hz && z <= v->cz) || (z <= hz && z >= v->cz))) ||
        ((z >= hz - 2 && z <= hz + 2) && ((x >= hx && x <= v->cx) || (x <= hx && x >= v->cx)))) {
      on_road = 1;
    }
  }

  if (on_road) {
    if (y == surface) return (v->biome == W_desert) ? B_sandstone : B_dirt_path;
  }

  return 0xFFFF;
}

static uint8_t getVillageTemplateStyleForBiome(uint8_t biome) {
  switch (biome) {
    case W_desert:
      return VILLAGE_STYLE_DESERT;
    case W_savanna:
    case W_windswept_savanna:
      return VILLAGE_STYLE_SAVANNA;
    case W_taiga:
    case W_old_growth_pine_taiga:
      return VILLAGE_STYLE_TAIGA;
    case W_snowy_taiga:
    case W_snowy_plains:
      return VILLAGE_STYLE_SNOWY;
    default:
      return VILLAGE_STYLE_PLAINS;
  }
}

static uint16_t getVillageProfessionJobBlock(uint8_t profession) {
  switch (profession) {
    case VILLAGE_PROF_FARMER: return B_composter;
    case VILLAGE_PROF_LIBRARIAN: return B_lectern;
    case VILLAGE_PROF_CLERIC: return B_brewing_stand;
    case VILLAGE_PROF_ARMORER: return B_blast_furnace;
    case VILLAGE_PROF_BUTCHER: return B_smoker;
    case VILLAGE_PROF_CARTOGRAPHER: return B_cartography_table;
    case VILLAGE_PROF_FISHERMAN: return B_barrel;
    case VILLAGE_PROF_FLETCHER: return B_fletching_table;
    case VILLAGE_PROF_LEATHERWORKER: return B_cauldron;
    case VILLAGE_PROF_MASON: return B_stonecutter;
    case VILLAGE_PROF_SHEPHERD: return B_loom;
    case VILLAGE_PROF_TOOLSMITH: return B_smithing_table;
    case VILLAGE_PROF_WEAPONSMITH: return B_grindstone;
    default: return B_crafting_table;
  }
}

static uint8_t villageProfessionUsesStone(uint8_t profession) {
  return profession == VILLAGE_PROF_CLERIC ||
         profession == VILLAGE_PROF_ARMORER ||
         profession == VILLAGE_PROF_MASON ||
         profession == VILLAGE_PROF_TOOLSMITH ||
         profession == VILLAGE_PROF_WEAPONSMITH;
}

static uint16_t getVillageProfessionStructureBlockAt(int dx, int dz, int rel_y, uint8_t profession, uint8_t biome, uint8_t variant) {
  int adx = dx < 0 ? -dx : dx;
  int adz = dz < 0 ? -dz : dz;

  if (profession == VILLAGE_PROF_FARMER) {
    if (adx > 5 || adz > 5 || rel_y < 0 || rel_y > 2) return 0xFFFF;
    if (rel_y == 0) {
      if (adx == 5 || adz == 5) return (biome == W_desert) ? B_sandstone : B_oak_log;
      if (dx == 0) return B_water;
      return B_farmland;
    }
    if (rel_y == 1) {
      if (dx == 5 && dz == 0) return B_composter;
      if ((dx == -5 && dz == -5) || (dx == -5 && dz == 5)) return B_hay_block;
      if (adx < 5 && adz < 5 && dx != 0) return B_wheat_7;
      return B_air;
    }
    return B_air;
  }

  uint16_t wall, roof, pillar, roof_slab, door, fence;
  getVillagePalette(biome, variant, &wall, &roof, &pillar, &roof_slab, &door, &fence);
  (void)fence;

  if (villageProfessionUsesStone(profession)) {
    wall = (biome == W_desert) ? B_sandstone : B_cobblestone;
    roof = (biome == W_desert) ? B_cut_sandstone : B_cobblestone;
    roof_slab = (biome == W_desert) ? B_sandstone_slab : B_cobblestone_slab;
    pillar = (biome == W_desert) ? B_sandstone : B_oak_log;
  }

  int half_x = (profession == VILLAGE_PROF_LIBRARIAN || profession == VILLAGE_PROF_CLERIC) ? 4 : 3;
  int half_z = (profession == VILLAGE_PROF_LIBRARIAN || profession == VILLAGE_PROF_CLERIC) ? 4 : 3;
  if (adx > half_x + 1 || adz > half_z + 1 || rel_y < 0 || rel_y > 5) return 0xFFFF;

  if (rel_y == 5) {
    if ((profession == VILLAGE_PROF_CLERIC || profession == VILLAGE_PROF_LIBRARIAN) && adx <= half_x - 1 && adz <= half_z - 1) return roof_slab;
    return 0xFFFF;
  }

  if (rel_y == 4) {
    if (adx == half_x + 1 || adz == half_z + 1) return roof_slab;
    if (adx <= half_x && adz <= half_z) return roof;
    return 0xFFFF;
  }

  if (adx > half_x || adz > half_z) return 0xFFFF;
  if (rel_y == 0) return (biome == W_desert) ? B_sandstone : B_cobblestone;

  if (dx == half_x && dz == 0 && rel_y >= 1 && rel_y <= 2) {
    uint16_t pd = 0x8000 | door;
    pd |= (1 << 9);
    if (rel_y == 2) pd |= (1 << 14);
    return pd;
  }

  if (rel_y == 1) {
    uint16_t job = getVillageProfessionJobBlock(profession);
    if (dx == -half_x + 1 && dz == 0) return job;
    if (profession == VILLAGE_PROF_LIBRARIAN && dx == -half_x + 1 && (dz == -1 || dz == 1)) return B_bookshelf;
    if (profession == VILLAGE_PROF_SHEPHERD && dx == -half_x + 1 && (dz == -1 || dz == 1)) return B_white_wool;
    if ((profession == VILLAGE_PROF_ARMORER || profession == VILLAGE_PROF_TOOLSMITH || profession == VILLAGE_PROF_WEAPONSMITH) && dx == -half_x + 1 && dz == 1) return B_iron_block;
    if (profession == VILLAGE_PROF_BUTCHER && dx == -half_x + 1 && dz == 1) return B_hay_block;
    if (profession == VILLAGE_PROF_FISHERMAN && dx == -half_x + 1 && dz == 1) return B_water;
  }

  if (rel_y == 2) {
    if (profession == VILLAGE_PROF_LIBRARIAN && dx == -half_x + 1 && (dz == -1 || dz == 1)) return B_bookshelf;
    if (dx == -half_x && dz == 0) return B_glass;
    if (dx == 0 && (dz == -half_z || dz == half_z)) return B_glass;
    if (dx == half_x && (dz == -1 || dz == 1)) return B_glass;
  }

  if ((adx == half_x && adz == half_z) && rel_y >= 1 && rel_y <= 3) return pillar;
  if (dx == -half_x || dx == half_x || dz == -half_z || dz == half_z) return wall;

  return B_air;
}

static uint16_t getVillageBlockAt(int x, int y, int z, uint8_t height) {
  VillageLayout v;
  if (!getVillageLayout(x, z, &v)) return 0xFFFF;

  int dx_center = x - v.cx;
  int dz_center = z - v.cz;
  int adx_center = dx_center < 0 ? -dx_center : dx_center;
  int adz_center = dz_center < 0 ? -dz_center : dz_center;
  uint8_t near_village_part = (adx_center <= 50 && adz_center <= 4) || (adz_center <= 50 && adx_center <= 4) || (adx_center <= 5 && adz_center <= 5);

  for (int i = 0; i < v.houses && !near_village_part; i++) {
    int hdx = x - v.hx[i]; if (hdx < 0) hdx = -hdx;
    int hdz = z - v.hz[i]; if (hdz < 0) hdz = -hdz;
    if (hdx <= 10 && hdz <= 10) near_village_part = 1;
    if (!near_village_part && ((x >= v.hx[i] - 2 && x <= v.hx[i] + 2) || (z >= v.hz[i] - 2 && z <= v.hz[i] + 2))) {
      if ((x >= v.hx[i] && x <= v.cx) || (x <= v.hx[i] && x >= v.cx) ||
          (z >= v.hz[i] && z <= v.cz) || (z <= v.hz[i] && z >= v.cz)) near_village_part = 1;
    }
  }
  if (!near_village_part) return 0xFFFF;

  for (int i = 0; i < v.houses; i++) {
    int hx = v.hx[i];
    int hz = v.hz[i];
    int dx = x - hx;
    int dz = z - hz;
    int rel_y = y - v.hy[i];
    uint8_t house_biome = getChunkBiome(div_floor(hx, 32), div_floor(hz, 32));
    if (!isVillageHouseBiome(house_biome)) continue;

    uint8_t style = getVillageTemplateStyleForBiome(house_biome);

    // Check if this position falls within the template's horizontal footprint.
    // We need the template's size and origin to determine the bounds.
    const VillageTemplate *vt = &village_templates[style][v.profession[i]];
    int in_footprint = 0;
    if (vt->block_count > 0) {
      int lx = dx + (int)vt->origin_x;
      int lz = dz + (int)vt->origin_z;
      if (lx >= 0 && lx < (int)vt->size_x && lz >= 0 && lz < (int)vt->size_z) {
        in_footprint = 1;
      }
    }

    // Terrain integration: fill gaps below building floors and clear terrain
    // that would protrude into the structure.
    uint16_t template_block = getVillageTemplateBlockAt(style, v.profession[i], dx, rel_y, dz);
    if (template_block != VILLAGE_TEMPLATE_NONE) return template_block;

    if (in_footprint) {
      if (rel_y < 0) {
        // Below the floor — fill with natural terrain blocks so the
        // ground under buildings blends with the surrounding terrain.
        // Use grass at the surface level, dirt below, sand for desert.
        uint16_t fill_block;
        if (house_biome == W_desert) {
          fill_block = (rel_y >= -2) ? B_sand : B_sandstone;
        } else {
          fill_block = (rel_y == -1) ? B_grass_block : B_dirt;
        }
        // Only fill if this column has template blocks above that need support.
        int has_block_above = 0;
        for (int check_y = 0; check_y < (int)vt->size_y && !has_block_above; check_y++) {
          if (getVillageTemplateBlockAt(style, v.profession[i], dx, check_y, dz) != VILLAGE_TEMPLATE_NONE) {
            has_block_above = 1;
          }
        }
        if (has_block_above) {
          return fill_block;
        }
      } else if (rel_y < (int)vt->size_y) {
        // Within the building's vertical extent but no template block here.
        // Clear any natural terrain that would protrude (handles hillsides).
        return B_air;
      }
      // Above the template — let terrain generate normally.
    }

    if (villageTemplateExists(style, v.profession[i])) continue;

    uint16_t block = getVillageProfessionStructureBlockAt(dx, dz, rel_y, v.profession[i], house_biome, v.variant[i]);
    if (block != 0xFFFF) return block;
  }

  uint16_t path = getVillagePathBlockAt(x, y, z, height, &v);
  if (path != 0xFFFF) return path;

  return 0xFFFF;
}

uint8_t getVillageHousePositions(int x, int z, short *house_x, short *house_z, uint8_t max_houses) {

  if (house_x == NULL || house_z == NULL || max_houses == 0) return 0;

  VillageLayout v;
  if (!getVillageLayout(x, z, &v)) return 0;

  uint8_t count = 0;
  for (int i = 0; i < v.houses && count < max_houses; i++) {
    int hx = v.hx[i];
    int hz = v.hz[i];
    uint8_t house_biome = getChunkBiome(div_floor(hx, 32), div_floor(hz, 32));
    if (!isVillageHouseBiome(house_biome)) continue;

    house_x[count] = (short)hx;
    house_z[count] = (short)hz;
    count++;
  }

  return count;
}


uint16_t getTerrainAtFromCache (int x, int y, int z, int rx, int rz, ChunkAnchor anchor, ChunkFeature feature, uint8_t height) {

  if (y >= 55 && y <= 140) {
    uint16_t vb = getVillageBlockAt(x, y, z, height);
    if (vb != 0xFFFF) return vb;
  }

  uint16_t stronghold_block = getStrongholdBlockAt(x, y, z);
  if (stronghold_block != STRONGHOLD_NONE) return stronghold_block;

  uint16_t dungeon_block = getDungeonBlockAt(x, y, z);
  if (dungeon_block != DUNGEON_NONE) return dungeon_block;

  if (y >= 64 && y >= height) switch (anchor.biome) {
    case W_plains: { // Generate oak trees and grass in plains

      // Tree generation - check if feature is valid (not skipped)
      if (feature.y == 255) goto plains_vegetation;  // Feature was skipped
      
      // Don't generate trees underwater or on mountains
      if (feature.y < 64 || feature.y > 120) goto plains_vegetation;

      // Increased tree spawn rate: 85% chance (was 75%)
      if ((anchor.hash & 0x0F) <= 12) {
        // Add random height variation: ±1 block
        int height_var = (anchor.hash >> (x + z)) & 3;  // 0-3
        int height_adjust = (height_var == 0) ? -1 : (height_var == 3) ? 1 : 0;

        // Handle tree trunk and dirt
        if (x == feature.x && z == feature.z) {
          if (y == feature.y - 1) return B_dirt;
          if (y >= feature.y && y < feature.y + 4 + feature.variant + height_adjust) return B_oak_log;
        }

        // Get X/Z distance from center of tree
        uint8_t dx = x > feature.x ? x - feature.x : feature.x - x;
        uint8_t dz = z > feature.z ? z - feature.z : feature.z - z;
        uint8_t dist = dx + dz;

        // Oak canopy - wider leaves (radius 3 instead of 2) for fuller appearance
        uint8_t trunk_top = feature.y + 4 + feature.variant + height_adjust;
        // Leaves start 2 blocks below trunk top, wider canopy
        if (y == trunk_top - 2 && dist <= 3) {
          if (dist == 3 && (dx == 3 || dz == 3)) break;
          return B_oak_leaves;
        }
        // Middle layers - wider
        if (y == trunk_top - 1 && dist <= 3) {
          if (dist == 3 && (dx == 3 || dz == 3)) break;
          return B_oak_leaves;
        }
        if (y == trunk_top && dist <= 3) {
          if (dist == 3 && (dx == 3 || dz == 3)) break;
          return B_oak_leaves;
        }
        // Top layer - slightly smaller
        if (y == trunk_top + 1 && dist <= 2) return B_oak_leaves;
      }

      plains_vegetation:
      // Random vegetation and flowers (spawns everywhere, not just near trees)
      if (y == height) return B_grass_block;

      if (y == height + 1) {
        // Use position-based hash for consistent random decoration
        uint8_t decor_hash = (anchor.hash >> (x + z)) & 255;
        uint8_t flower_hash = (anchor.hash >> ((x * z) & 7)) & 255;

        // 15% chance for short grass
        if (decor_hash < 38) return B_short_grass;

        // 2% chance for flowers (scattered randomly)
        if (decor_hash >= 230 && decor_hash < 235) {
          uint8_t flower_type = flower_hash % 6;
          if (flower_type == 0) return B_dandelion;
          if (flower_type == 1) return B_poppy;
          if (flower_type == 2) return B_azure_bluet;
          if (flower_type == 3) return B_oxeye_daisy;
          if (flower_type == 4) return B_cornflower;
          if (flower_type == 5) return B_allium;
        }
      }

      return B_air;
    }
    
    case W_forest: { // Generate oak and birch trees in forests

      // Tree generation - check if feature is valid
      if (feature.y == 255) goto forest_vegetation;
      
      // Don't generate trees underwater or on mountains
      if (feature.y < 64 || feature.y > 120) goto forest_vegetation;

      // Increased tree spawn rate: 90% chance (was 50%)
      if ((anchor.hash & 0x07) <= 6) {
        // Use hash to decide between oak and birch
        uint8_t is_birch = (anchor.hash >> 3) & 1;

        // Add random height variation: ±1 block
        int height_var = (anchor.hash >> (x + z)) & 3;
        int height_adjust = (height_var == 0) ? -1 : (height_var == 3) ? 1 : 0;

        if (x == feature.x && z == feature.z) {
          if (y == feature.y - 1) return B_dirt;
          // Birch trees are taller with distinctive white bark
          if (is_birch) {
            if (y >= feature.y && y < feature.y + 5 + feature.variant + height_adjust) return B_birch_log;
          } else {
            if (y >= feature.y && y < feature.y + 4 + feature.variant + height_adjust) return B_oak_log;
          }
        }

        // Get X/Z distance from center of tree
        uint8_t dx = x > feature.x ? x - feature.x : feature.x - x;
        uint8_t dz = z > feature.z ? z - feature.z : feature.z - z;
        uint8_t dist = dx + dz;

        // Generate leaf clusters
        if (is_birch) {
          // Birch leaves - wider canopy (radius 3), sparse and spread out
          uint8_t trunk_top = feature.y + 5 + feature.variant + height_adjust;
          // Leaves start 2 blocks below trunk top, wider canopy
          if (y == trunk_top - 2 && dist <= 3) {
            if (dist == 3 && (dx == 3 || dz == 3)) break;
            return B_birch_leaves;
          }
          // Main canopy layers - wider
          if (y == trunk_top - 1 && dist <= 3) {
            if (dist == 3 && (dx == 3 || dz == 3)) break;
            return B_birch_leaves;
          }
          if (y == trunk_top && dist <= 3) {
            if (dist == 3 && (dx == 3 || dz == 3)) break;
            return B_birch_leaves;
          }
          // Top layer - slightly smaller
          if (y == trunk_top + 1 && dist <= 2) return B_birch_leaves;
          // Occasional lower leaves - wider
          if (y == trunk_top - 3 && dist == 3 && (dx == 3 || dz == 3)) {
            if ((anchor.hash >> (y + x + z)) & 1) return B_birch_leaves;
          }
        } else {
          // Oak leaves - wider rounded canopy (radius 3)
          uint8_t trunk_top = feature.y + 4 + feature.variant + height_adjust;
          // Leaves start 2 blocks below trunk top, wider canopy
          if (y == trunk_top - 2 && dist <= 3) {
            if (dist == 3 && (dx == 3 || dz == 3)) break;
            return B_oak_leaves;
          }
          // Middle layers - wider
          if (y == trunk_top - 1 && dist <= 3) {
            if (dist == 3 && (dx == 3 || dz == 3)) break;
            return B_oak_leaves;
          }
          if (y == trunk_top && dist <= 3) {
            if (dist == 3 && (dx == 3 || dz == 3)) break;
            return B_oak_leaves;
          }
          // Top layer - slightly smaller
          if (y == trunk_top + 1 && dist <= 2) return B_oak_leaves;
        }
      }

      forest_vegetation:
      // Random vegetation and flowers (spawns everywhere)
      if (y == height) return B_grass_block;
      
      if (y == height + 1) {
        uint8_t decor_hash = (anchor.hash >> (x + z)) & 255;
        uint8_t flower_hash = (anchor.hash >> ((x * z) & 7)) & 255;

        // 20% chance for short grass
        if (decor_hash < 51) return B_short_grass;

        // 3% chance for flowers (forests have more flowers)
        if (decor_hash >= 220 && decor_hash < 228) {
          uint8_t flower_type = flower_hash % 8;
          if (flower_type == 0) return B_dandelion;
          if (flower_type == 1) return B_poppy;
          if (flower_type == 2) return B_azure_bluet;
          if (flower_type == 3) return B_oxeye_daisy;
          if (flower_type == 4) return B_allium;
          if (flower_type == 5) return B_red_tulip;
          if (flower_type == 6) return B_orange_tulip;
          if (flower_type == 7) return B_lily_of_the_valley;
        }
      }

      return B_air;
    }

    case W_taiga: // Generate spruce trees in taiga
    case W_old_growth_pine_taiga: {

      // Tree generation - check if feature is valid
      if (feature.y == 255) goto taiga_vegetation;
      
      // Don't generate trees underwater or on mountains
      if (feature.y < 64 || feature.y > 120) goto taiga_vegetation;

      // Increased tree spawn rate: 85% chance (was ~50%)
      if ((anchor.hash & 0x07) <= 6) {
        // Add random height variation: ±1 block
        int height_var = (anchor.hash >> (x + z)) & 3;
        int height_adjust = (height_var == 0) ? -1 : (height_var == 3) ? 1 : 0;

        // Spruce trees are tall and conical
        if (x == feature.x && z == feature.z) {
          if (y == feature.y - 1) return B_dirt;
          if (y >= feature.y && y < feature.y + 6 + feature.variant * 2 + height_adjust) return B_spruce_log;
        }

        // Get X/Z distance from center of tree
        uint8_t dx = x > feature.x ? x - feature.x : feature.x - x;
        uint8_t dz = z > feature.z ? z - feature.z : feature.z - z;
        uint8_t dist = dx + dz;

        // Conical spruce leaves - moved down by 1 block for fuller appearance
        uint8_t leaf_base = feature.y + 2;
        uint8_t leaf_top = feature.y + 8 + feature.variant * 2 + height_adjust;

        // Spruce tree leaves form a cone shape
        if (y >= leaf_base && y <= leaf_top) {
          int dist_from_top = leaf_top - y;
          int max_radius = (dist_from_top / 2) + 1;
          if (dist <= max_radius && dist > 0) {
            return B_spruce_leaves;
          }
        }
        // Top of the tree
        if (y == leaf_top + 1 && dist == 0) return B_spruce_leaves;
      }

      taiga_vegetation:
      // Random vegetation (spawns everywhere)
      if (y == height) return B_grass_block;
      
      if (y == height + 1) {
        uint8_t decor_hash = (anchor.hash >> (x + z)) & 255;

        // 25% chance for ferns (taiga has lots of ferns)
        if (decor_hash < 64) return B_fern;

        // 10% chance for short grass
        if (decor_hash >= 64 && decor_hash < 90) return B_short_grass;

        // 2% chance for berries (bush)
        if (decor_hash >= 240 && decor_hash < 245) return B_bush;
      }

      return B_air;
    }

    case W_snowy_taiga: { // Generate spruce trees with snow in cold taiga

      // Tree generation - check if feature is valid
      if (feature.y == 255) goto snowy_taiga_vegetation;
      
      // Don't generate trees underwater or on mountains
      if (feature.y < 64 || feature.y > 120) goto snowy_taiga_vegetation;

      // Increased tree spawn rate: 85% chance
      if ((anchor.hash & 0x07) <= 6) {
        // Add random height variation: ±1 block
        int height_var = (anchor.hash >> (x + z)) & 3;
        int height_adjust = (height_var == 0) ? -1 : (height_var == 3) ? 1 : 0;

        if (x == feature.x && z == feature.z) {
          if (y == feature.y - 1) return B_dirt;
          if (y >= feature.y && y < feature.y + 5 + feature.variant * 2 + height_adjust) return B_spruce_log;
        }

        // Get X/Z distance from center of tree
        uint8_t dx = x > feature.x ? x - feature.x : feature.x - x;
        uint8_t dz = z > feature.z ? z - feature.z : feature.z - z;
        uint8_t dist = dx + dz;

        // Conical spruce leaves with snow - moved down by 1 block
        uint8_t leaf_base = feature.y + 1;
        uint8_t leaf_top = feature.y + 7 + feature.variant * 2 + height_adjust;

        if (y >= leaf_base && y <= leaf_top) {
          int dist_from_top = leaf_top - y;
          int max_radius = (dist_from_top / 2) + 1;
          if (dist <= max_radius && dist > 0) {
            return B_spruce_leaves;
          }
        }
        if (y == leaf_top + 1 && dist == 0) return B_spruce_leaves;
      }

      snowy_taiga_vegetation:
      // Random vegetation (spawns everywhere)
      if (y == height) return B_grass_block;
      
      if (y == height + 1) {
        uint8_t decor_hash = (anchor.hash >> (x + z)) & 255;

        // 15% chance for ferns
        if (decor_hash < 38) return B_fern;

        // 8% chance for short grass (appears under snow layer)
        if (decor_hash >= 38 && decor_hash < 58) return B_short_grass;
      }

      return B_air;
    }

    case W_jungle: // Generate jungle trees with vines
    case W_bamboo_jungle: {

      // Tree generation - check if feature is valid
      if (feature.y == 255) goto jungle_vegetation;
      
      // Don't generate trees underwater or on mountains
      if (feature.y < 64 || feature.y > 120) goto jungle_vegetation;

      // Increased tree spawn rate: 80% chance
      if ((anchor.hash & 0x07) <= 5) {
        // Add random height variation: ±2 blocks for jungle trees (they vary more)
        int height_var = (anchor.hash >> (x + z)) & 3;
        int height_adjust = (height_var == 0) ? -2 : (height_var == 3) ? 2 : ((height_var == 1) ? -1 : 1);

        // Jungle trees are very tall with 2x2 trunks
        int base_x = feature.x & ~1;
        int base_z = feature.z & ~1;
        int trunk_height = 10 + feature.variant * 4 + height_adjust;

        // 2x2 trunk
        if (x >= base_x && x < base_x + 2 && z >= base_z && z < base_z + 2) {
          if (y == feature.y - 1) return B_dirt;
          if (y >= feature.y && y < feature.y + trunk_height) return B_jungle_log;
        }

        // Get Manhattan distance from tree center (use feature position as center
        // to keep canopy within minichunk bounds)
        int cx = feature.x;
        int cz = feature.z;
        int dx = x > cx ? x - cx : cx - x;
        int dz = z > cz ? z - cz : cz - z;
        int dist = dx + dz;

        // Large jungle leaf canopy - tall, wide, organic shape (Manhattan distance)
        int trunk_top = feature.y + trunk_height;
        int leaf_base = trunk_top - 5;
        int leaf_top = trunk_top + 4;

        // Main canopy: tall layered structure with bulging middle
        if (y >= leaf_base && y <= leaf_top) {
          int layer = y - leaf_base;
          int total_layers = leaf_top - leaf_base + 1; // 10 layers
          // Taper at bottom and top, bulge in middle; max radius 3
          // to stay within minichunk bounds (center +/- 3 fits in 0-7)
          int max_radius;
          if (layer <= 1) max_radius = 2;
          else if (layer <= 7) max_radius = 3;
          else max_radius = 2;

          if (dist <= max_radius) {
            // Cut corners on outer layers for rounder shape
            if ((y == leaf_base || y == leaf_top) && dist == max_radius) {
              if (dx == max_radius || dz == max_radius) break;
            }
            return B_jungle_leaves;
          }
        }
        // Dome on top
        if (y == leaf_top + 1 && dist <= 2) return B_jungle_leaves;
        if (y == leaf_top + 2 && dist <= 1) return B_jungle_leaves;
      }

      jungle_vegetation:
      // Random vegetation (spawns everywhere)
      if (y == height) return B_grass_block;
      
      if (y == height + 1) {
        uint8_t decor_hash = (anchor.hash >> (x + z)) & 255;

        // 30% chance for ferns (jungles have lots of vegetation)
        if (decor_hash < 77) return B_fern;

        // 10% chance for tall grass
        if (decor_hash >= 77 && decor_hash < 102) return B_short_grass;

        // 2% chance for melon stems (bush as placeholder)
        if (decor_hash >= 240 && decor_hash < 245) return B_bush;
      }

      return B_air;
    }

    case W_savanna: { // Generate acacia trees in savanna

      // Tree generation - check if feature is valid
      if (feature.y == 255) goto savanna_vegetation;
      
      // Don't generate trees underwater or on mountains
      if (feature.y < 64 || feature.y > 120) goto savanna_vegetation;

      // Increased tree spawn rate: 85% chance
      if ((anchor.hash & 0x07) <= 6) {
        // Add random height variation: ±1 block
        int height_var = (anchor.hash >> (x + z)) & 3;
        int height_adjust = (height_var == 0) ? -1 : (height_var == 3) ? 1 : 0;

        // Acacia trees have a distinctive curved shape
        int trunk_height = 4 + feature.variant + height_adjust;

        // Main trunk (straight part)
        if (x == feature.x && z == feature.z) {
          if (y == feature.y - 1) return B_dirt;
          if (y >= feature.y && y < feature.y + trunk_height) return B_acacia_log;
        }

        // Curved trunk section (extends in +X direction)
        for (int i = 1; i <= 3; i++) {
          if (x == feature.x + i && z == feature.z) {
            if (y >= feature.y + trunk_height && y < feature.y + trunk_height + 2) {
              return B_acacia_log;
            }
          }
        }

        // Get distance from canopy center (offset from trunk)
        uint8_t canopy_cx = feature.x + 2;
        uint8_t dx = x > canopy_cx ? x - canopy_cx : canopy_cx - x;
        uint8_t dz = z > feature.z ? z - feature.z : feature.z - z;
        uint8_t dist = dx + dz;

        // Acacia leaf canopy (wide, flat umbrella shape) - moved down by 1 block
        uint8_t canopy_y = feature.y + trunk_height + 1;
        if (y >= canopy_y && y <= canopy_y + 1) {
          if (dist <= 3) {
            if (y == canopy_y && dist == 3) break;
            return B_acacia_leaves;
          }
        }
        // Top layer
        if (y == canopy_y + 2 && dist <= 2) return B_acacia_leaves;
      }

      savanna_vegetation:
      // Random vegetation (spawns everywhere)
      if (y == height) return B_grass_block;
      
      if (y == height + 1) {
        uint8_t decor_hash = (anchor.hash >> (x + z)) & 255;

        // 20% chance for tall dry grass
        if (decor_hash < 51) return B_tall_dry_grass;

        // 8% chance for short dry grass
        if (decor_hash >= 51 && decor_hash < 71) return B_short_dry_grass;

        // 1% chance for dandelions (hardy flowers)
        if (decor_hash >= 250 && decor_hash < 253) return B_dandelion;
      }

      return B_air;
    }

    case W_dark_forest: { // Generate dark oak trees (2x2 trunks)

      // Tree generation - check if feature is valid
      if (feature.y == 255) goto dark_forest_vegetation;
      
      // Don't generate trees underwater
      if (feature.y < 64) goto dark_forest_vegetation;

      // Increased tree spawn rate: 90% chance
      if ((anchor.hash & 0x07) <= 6) {
        // Add random height variation: ±1 block
        int height_var = (anchor.hash >> (x + z)) & 3;
        int height_adjust = (height_var == 0) ? -1 : (height_var == 3) ? 1 : 0;

        // Dark oak trees have 2x2 trunks
        int base_x = feature.x & ~1;
        int base_z = feature.z & ~1;
        int trunk_height = 6 + feature.variant + height_adjust;

        if (x >= base_x && x < base_x + 2 && z >= base_z && z < base_z + 2) {
          if (y == feature.y - 1) return B_dirt;
          if (y >= feature.y && y < feature.y + trunk_height) return B_dark_oak_log;
        }

        // Get Manhattan distance from tree center (use feature position as center
        // to keep canopy within minichunk bounds)
        int cx = feature.x;
        int cz = feature.z;
        int dx = x > cx ? x - cx : cx - x;
        int dz = z > cz ? z - cz : cz - z;
        int dist = dx + dz;

        // Dark oak canopy - dome shape (Manhattan distance)
        int canopy_base = feature.y + trunk_height - 1;
        if (y >= canopy_base && y <= canopy_base + 3) {
          int layer = y - canopy_base;
          // Taper from radius 3 at bottom to 1 at top
          int max_radius = (layer == 0) ? 3 : (layer == 1) ? 2 : 1;
          if (dist <= max_radius && dist > 0) {
            return B_dark_oak_leaves;
          }
        }
        // Top of canopy
        if (y == canopy_base + 4 && dist <= 1) return B_dark_oak_leaves;
      }

      dark_forest_vegetation:
      // Random vegetation (spawns everywhere)
      if (y == height) return B_grass_block;
      
      if (y == height + 1) {
        uint8_t decor_hash = (anchor.hash >> (x + z)) & 255;

        // 15% chance for short grass (dark forest has dense ground cover)
        if (decor_hash < 38) return B_short_grass;
      }

      return B_air;
    }

    case W_birch_forest: { // Generate only birch trees

      // Tree generation - check if feature is valid
      if (feature.y == 255) goto birch_forest_vegetation;
      
      // Don't generate trees underwater
      if (feature.y < 64) goto birch_forest_vegetation;

      // Increased tree spawn rate: 90% chance
      if ((anchor.hash & 0x07) <= 6) {
        // Add random height variation: ±1 block
        int height_var = (anchor.hash >> (x + z)) & 3;
        int height_adjust = (height_var == 0) ? -1 : (height_var == 3) ? 1 : 0;

        if (x == feature.x && z == feature.z) {
          if (y == feature.y - 1) return B_dirt;
          if (y >= feature.y && y < feature.y + 5 + feature.variant + height_adjust) return B_birch_log;
        }

        // Get X/Z distance from center of tree
        uint8_t dx = x > feature.x ? x - feature.x : feature.x - x;
        uint8_t dz = z > feature.z ? z - feature.z : feature.z - z;
        uint8_t dist = dx + dz;

        // Birch leaves - wider canopy (radius 3), sparse and spread out
        uint8_t trunk_top = feature.y + 5 + feature.variant + height_adjust;
        // Leaves start 2 blocks below trunk top, wider canopy
        if (y == trunk_top - 2 && dist <= 3) {
          if (dist == 3 && (dx == 3 || dz == 3)) break;
          return B_birch_leaves;
        }
        // Main canopy layers - wider
        if (y == trunk_top - 1 && dist <= 3) {
          if (dist == 3 && (dx == 3 || dz == 3)) break;
          return B_birch_leaves;
        }
        if (y == trunk_top && dist <= 3) {
          if (dist == 3 && (dx == 3 || dz == 3)) break;
          return B_birch_leaves;
        }
        // Top layer - slightly smaller
        if (y == trunk_top + 1 && dist <= 2) return B_birch_leaves;
        // Occasional lower leaves - wider
        if (y == trunk_top - 3 && dist == 3 && (dx == 3 || dz == 3)) {
          if ((anchor.hash >> (y + x + z)) & 1) return B_birch_leaves;
        }
      }

      birch_forest_vegetation:
      // Random vegetation (spawns everywhere)
      if (y == height) return B_grass_block;
      
      if (y == height + 1) {
        uint8_t decor_hash = (anchor.hash >> (x + z)) & 255;

        // 15% chance for short grass
        if (decor_hash < 38) return B_short_grass;
      }

      return B_air;
    }

    case W_cherry_grove: { // Generate cherry blossom trees

      // Tree generation - check if feature is valid
      if (feature.y == 255) goto cherry_grove_vegetation;
      
      // Don't generate trees underwater
      if (feature.y < 64) goto cherry_grove_vegetation;

      // Increased tree spawn rate: 85% chance
      if ((anchor.hash & 0x07) <= 6) {
        // Add random height variation: ±1 block
        int height_var = (anchor.hash >> (x + z)) & 3;
        int height_adjust = (height_var == 0) ? -1 : (height_var == 3) ? 1 : 0;

        if (x == feature.x && z == feature.z) {
          if (y == feature.y - 1) return B_dirt;
          if (y >= feature.y && y < feature.y + 3 + feature.variant + height_adjust) return B_cherry_log;
        }

        // Get X/Z distance from center of tree
        uint8_t dx = x > feature.x ? x - feature.x : feature.x - x;
        uint8_t dz = z > feature.z ? z - feature.z : feature.z - z;
        uint8_t dist = dx + dz;

        // Cherry blossom canopy (wide, flat, with hanging edges) - moved down by 1 block
        uint8_t trunk_top = feature.y + 3 + feature.variant + height_adjust;
        // Leaves start 2 blocks below trunk top (was 1)
        if (y == trunk_top - 2 && dist <= 3) {
          if (dist == 3 && (dx == 3 || dz == 3)) break;
          return B_cherry_leaves;
        }
        // Main canopy - wide and flat
        if (y == trunk_top - 1 && dist <= 3) {
          if (dist == 3) break;
          return B_cherry_leaves;
        }
        if (y == trunk_top && dist <= 2) return B_cherry_leaves;
        // Top layer
        if (y == trunk_top + 1 && dist <= 2) return B_cherry_leaves;
        // Hanging edges (petals falling)
        if (y == trunk_top - 3 && dist == 3) {
          if ((anchor.hash >> (x + z)) & 3) return B_cherry_leaves;
        }
      }

      cherry_grove_vegetation:
      // Random vegetation and flowers (spawns everywhere)
      if (y == height) return B_grass_block;
      
      if (y == height + 1) {
        uint8_t decor_hash = (anchor.hash >> (x + z)) & 255;
        uint8_t flower_hash = (anchor.hash >> ((x * z) & 7)) & 255;

        // 15% chance for short grass
        if (decor_hash < 38) return B_short_grass;

        // 3% chance for pink flowers
        if (decor_hash >= 230 && decor_hash < 235) {
          uint8_t flower_type = flower_hash % 3;
          if (flower_type == 0) return B_pink_tulip;
          if (flower_type == 1) return B_red_tulip;
          if (flower_type == 2) return B_poppy;
        }
      }

      return B_air;
    }

    case W_desert: { // Generate dead bushes and cacti in deserts

      if (x != feature.x || z != feature.z) break;

      if (feature.variant == 0) {
        if (y == height + 1) return B_dead_bush;
      } else if (y > height) {
        // The size of the cactus is determined based on whether the terrain
        // height is even or odd at the target location
        if (height & 1 && y <= height + 3) return B_cactus;
        if (y <= height + 2) return B_cactus;
      }

      break;

    }

    case W_mangrove_swamp: { // Generate lilypads and moss carpets in swamps

      if (x == feature.x && z == feature.z && y == 64 && height < 63) {
        return B_lily_pad;
      }

      if (y == height + 1) {
        uint8_t dx = x > feature.x ? x - feature.x : feature.x - x;
        uint8_t dz = z > feature.z ? z - feature.z : feature.z - z;
        if (dx + dz < 4) return B_moss_carpet;
      }

      break;
    }

    case W_snowy_plains: { // Generate grass stubs and snow in snowy plains

      if (x == feature.x && z == feature.z && y == height + 1 && height >= 64) {
        return B_short_grass;
      }

      break;
    }

    case W_meadow: { // Generate dense flowers and tall grass in meadows

      if (y == height) return B_grass_block;
      
      // Meadow decorations - very dense flowers and tall grass
      if (y == height + 1) {
        uint8_t decor_hash = (anchor.hash >> (x + z)) & 255;
        
        // 30% chance for short grass
        if (decor_hash < 77) return B_short_grass;
        
        // 15% chance for dandelions (common in meadows)
        if (decor_hash >= 77 && decor_hash < 115) return B_dandelion;
        
        // 10% chance for various flowers
        if (decor_hash >= 115 && decor_hash < 141) {
          uint8_t flower_type = (decor_hash >> 3) % 6;
          if (flower_type == 0) return B_poppy;
          if (flower_type == 1) return B_blue_orchid;
          if (flower_type == 2) return B_allium;
          if (flower_type == 3) return B_azure_bluet;
          if (flower_type == 4) return B_red_tulip;
          if (flower_type == 5) return B_oxeye_daisy;
        }
        
        // 5% chance for cornflowers
        if (decor_hash >= 141 && decor_hash < 154) return B_cornflower;
        
        // 3% chance for lilac (using bush as placeholder for tall flowers)
        if (decor_hash >= 154 && decor_hash < 162) return B_bush;
      }
      
      break;
    }

    case W_swamp: { // Generate swamp vegetation with oak trees and vines
      
      if (x == feature.x && z == feature.z && y == 64 && height < 63) {
        return B_lily_pad;
      }
      
      // Occasional oak trees with vines
      if (feature.y >= 64 && x == feature.x && z == feature.z) {
        if (y == feature.y - 1) return B_dirt;
        if (y >= feature.y && y < feature.y + 4 + feature.variant) return B_oak_log;
        // Vines on trunk
        if (y > feature.y + 1 && y < feature.y + 4 + feature.variant) {
          uint8_t vine_dx = x > feature.x ? x - feature.x : feature.x - x;
          uint8_t vine_dz = z > feature.z ? z - feature.z : feature.z - z;
          if (vine_dx == 1 && vine_dz == 0 && (anchor.hash >> y) & 1) return B_oak_leaves;
          if (vine_dx == 0 && vine_dz == 1 && (anchor.hash >> y) & 1) return B_oak_leaves;
        }
      }
      
      // Oak canopy for swamp trees
      if (feature.y >= 64) {
        uint8_t dx = x > feature.x ? x - feature.x : feature.x - x;
        uint8_t dz = z > feature.z ? z - feature.z : feature.z - z;
        uint8_t dist = dx + dz;
        uint8_t trunk_top = feature.y + 4 + feature.variant;
        
        // Leaves start partway up trunk
        if (y == trunk_top - 1 && dist <= 2) {
          if (dist == 2 && (dx == 2 || dz == 2)) break;
          return B_oak_leaves;
        }
        // Middle layers
        if (y == trunk_top && dist <= 2) {
          if (dx == 2 && dz == 2) break;
          return B_oak_leaves;
        }
        if (y == trunk_top + 1 && dist <= 2) {
          if (dx == 2 && dz == 2) break;
          return B_oak_leaves;
        }
        // Top layer
        if (y == trunk_top + 2 && dist <= 1) return B_oak_leaves;
      }
      
      break;
    }

    case W_windswept_forest: { // Generate spruce trees on windswept hills

      if (feature.y < 64) break;

      // Tall spruce trees adapted for windy conditions
      if (x == feature.x && z == feature.z) {
        if (y == feature.y - 1) return B_dirt;
        if (y >= feature.y && y < feature.y + 7 + feature.variant) return B_spruce_log;
      }

      // Conical spruce leaves
      uint8_t dx = x > feature.x ? x - feature.x : feature.x - x;
      uint8_t dz = z > feature.z ? z - feature.z : feature.z - z;
      uint8_t dist = dx + dz;
      uint8_t leaf_top = feature.y + 10 + feature.variant;

      if (y >= feature.y + 3 && y <= leaf_top) {
        int dist_from_top = leaf_top - y;
        int max_radius = (dist_from_top / 2) + 1;
        if (dist <= max_radius && dist > 0) {
          return B_spruce_leaves;
        }
      }

      if (y == height) return B_grass_block;
      break;
    }

    case W_windswept_savanna: { // Generate acacia trees in windswept savanna

      if (feature.y < 64) break;

      // Acacia trees with curved trunks
      int trunk_height = 4 + feature.variant;

      if (x == feature.x && z == feature.z) {
        if (y == feature.y - 1) return B_dirt;
        if (y >= feature.y && y < feature.y + trunk_height) return B_acacia_log;
      }

      // Curved trunk
      for (int i = 1; i <= 3; i++) {
        if (x == feature.x + i && z == feature.z) {
          if (y >= feature.y + trunk_height && y < feature.y + trunk_height + 2) {
            return B_acacia_log;
          }
        }
      }

      // Leaf canopy
      uint8_t canopy_cx = feature.x + 2;
      uint8_t dx = x > canopy_cx ? x - canopy_cx : canopy_cx - x;
      uint8_t dz = z > feature.z ? z - feature.z : feature.z - z;
      uint8_t dist = dx + dz;
      uint8_t canopy_y = feature.y + trunk_height + 2;

      if (y >= canopy_y && y <= canopy_y + 1 && dist <= 3) {
        if (y == canopy_y && dist == 3) break;
        return B_acacia_leaves;
      }
      if (y == canopy_y + 2 && dist <= 2) return B_acacia_leaves;

      if (y == height) return B_grass_block;
      break;
    }

    case W_snowy_slopes: { // Generate sparse spruce trees on snowy slopes

      if (feature.y < 64) break;

      // Small spruce trees adapted for snow
      if (x == feature.x && z == feature.z) {
        if (y == feature.y - 1) return B_dirt;
        if (y >= feature.y && y < feature.y + 4 + feature.variant) return B_spruce_log;
      }

      // Compact leaf clusters
      uint8_t dx = x > feature.x ? x - feature.x : feature.x - x;
      uint8_t dz = z > feature.z ? z - feature.z : feature.z - z;
      uint8_t dist = dx + dz;
      uint8_t leaf_top = feature.y + 6 + feature.variant;

      if (y >= feature.y + 2 && y <= leaf_top) {
        int dist_from_top = leaf_top - y;
        int max_radius = (dist_from_top / 2) + 1;
        if (dist <= max_radius && dist > 0) {
          return B_spruce_leaves;
        }
      }

      if (y == height) return B_snow_block;
      break;
    }

    case W_grove: { // Generate dense spruce forest in groves

      if (feature.y < 64) break;

      // Tall spruce trees
      if (x == feature.x && z == feature.z) {
        if (y == feature.y - 1) return B_dirt;
        if (y >= feature.y && y < feature.y + 8 + feature.variant * 2) return B_spruce_log;
      }

      // Full spruce canopy
      uint8_t dx = x > feature.x ? x - feature.x : feature.x - x;
      uint8_t dz = z > feature.z ? z - feature.z : feature.z - z;
      uint8_t dist = dx + dz;
      uint8_t leaf_top = feature.y + 12 + feature.variant * 2;

      if (y >= feature.y + 4 && y <= leaf_top) {
        int dist_from_top = leaf_top - y;
        int max_radius = (dist_from_top / 2) + 1;
        if (dist <= max_radius && dist > 0) {
          return B_spruce_leaves;
        }
      }

      if (y == height) return B_grass_block;
      break;
    }

    case W_flower_forest: { // Generate oak trees with abundant flowers

      if (feature.y < 64) break;

      // Oak trees
      if (x == feature.x && z == feature.z) {
        if (y == feature.y - 1) return B_dirt;
        if (y >= feature.y && y < feature.y + 5 + feature.variant) return B_oak_log;
      }

      // Oak canopy
      uint8_t dx = x > feature.x ? x - feature.x : feature.x - x;
      uint8_t dz = z > feature.z ? z - feature.z : feature.z - z;
      uint8_t dist = dx + dz;
      uint8_t trunk_top = feature.y + 5 + feature.variant;

      if (y == trunk_top - 1 && dist <= 2) {
        if (dist == 2 && (dx == 2 || dz == 2)) break;
        return B_oak_leaves;
      }
      if (y >= trunk_top && y <= trunk_top + 2 && dist <= 2) {
        if (y == trunk_top + 2 && dist > 1) break;
        return B_oak_leaves;
      }

      if (y == height) return B_grass_block;

      // Dense flower coverage
      if (y == height + 1) {
        uint8_t decor_hash = (anchor.hash >> (x + z)) & 255;
        if (decor_hash < 50) return B_short_grass;
        if (decor_hash < 200) {
          uint8_t flower = decor_hash % 12;
          if (flower == 0) return B_dandelion;
          if (flower == 1) return B_poppy;
          if (flower == 2) return B_blue_orchid;
          if (flower == 3) return B_allium;
          if (flower == 4) return B_azure_bluet;
          if (flower == 5) return B_red_tulip;
          if (flower == 6) return B_orange_tulip;
          if (flower == 7) return B_white_tulip;
          if (flower == 8) return B_pink_tulip;
          if (flower == 9) return B_oxeye_daisy;
          if (flower == 10) return B_cornflower;
          if (flower == 11) return B_lily_of_the_valley;
        }
      }
      break;
    }

    case W_sunflower_plains: { // Generate sunflowers in plains

      if (y == height) return B_grass_block;

      if (y == height + 1) {
        uint8_t decor_hash = (anchor.hash >> (x + z)) & 255;
        if (decor_hash < 80) return B_short_grass;
        // 8% chance for sunflower (using bush as placeholder)
        if (decor_hash >= 100 && decor_hash < 120) return B_bush;
      }
      break;
    }

    case W_eroded_badlands: { // Generate cacti and dead bushes in eroded badlands

      if (y == height) return B_terracotta;

      if (y == height + 1) {
        uint8_t decor_hash = (anchor.hash >> (x + z)) & 255;
        if (decor_hash < 30) return B_cactus;
        if (decor_hash < 50) return B_dead_bush;
      }
      break;
    }

    case W_ice_spikes: { // Generate ice spikes and packed ice

      if (x == feature.x && z == feature.z && feature.y >= 64) {
        // Ice spike
        int spike_height = 10 + feature.variant * 5;
        if (y >= height && y < height + spike_height) {
          uint8_t spike_width = (spike_height - (y - height)) / 3;
          uint8_t dx = x > feature.x ? x - feature.x : feature.x - x;
          uint8_t dz = z > feature.z ? z - feature.z : feature.z - z;
          if (dx <= spike_width && dz <= spike_width) {
            return (y - height) < spike_height / 2 ? B_packed_ice : B_ice;
          }
        }
      }

      if (y == height) return B_snow_block;
      break;
    }

    default: break;
  }

  // Handle surface-level terrain (the very topmost blocks)
  if (height >= 63) {
    if (y == height) {
      if (anchor.biome == W_swamp) return B_mud;
      if (anchor.biome == W_desert) return B_sand;
      return B_grass_block;
    }
    // Snow layer on top of grass in snowy biomes
    if (anchor.biome == W_snowy_plains || anchor.biome == W_snowy_taiga) {
      if (y == height + 1) return B_snow;
    }
  }

  // Mountain terrain is generated through height map - no additional cliff structures needed
  // The mountain noise already creates natural steep slopes and peaks

  // Starting at 4 blocks below terrain level, generate minerals and caves
  if (y <= height - 4) {
    // Deep bedrock layer - now a 5-block thick floor at the bottom of the world
    if (y >= 0 && y < 5) return B_bedrock;

    // Check for caves using surface-like noise for natural shapes
    if (isCaveSimple(x, y, z, height, anchor.biome)) {
      // Fill caves with lava below the lava level (Y=7)
      if (y <= 7) return B_lava;
      return B_air;
    }

    // Generate ore clumps using 3D noise for clustered veins
    uint16_t ore = getOreClumpAt(x, y, z, anchor.biome);
    if (ore != 0) return ore;

    // Check for mineshaft blocks
    uint16_t ms_block = getMineshaftBlockAt(x, y, z);
    if (ms_block != 0xFFFF) return ms_block;

    // For everything else, fall back to stone
    return B_stone;
  }
  // Handle the space between stone and surface
  if (y <= height) {
    if (anchor.biome == W_desert) return B_sandstone;
    if (anchor.biome == W_swamp) return B_mud;
    return B_dirt;
  }
  // Below sea level - generate water with sand/gravel floor
  if (y < 64) {
    // Ice on top in snowy biomes
    if (y == 63 && anchor.biome == W_snowy_plains) return B_ice;
    
    // Sand/gravel floor at the bottom (1-3 blocks thick)
    if (y <= height + 2) {
      uint8_t floor_hash = (anchor.hash >> (x + y + z)) & 255;
      if (y == height + 1) {
        // Top floor layer - mostly sand
        if (floor_hash < 200) return B_sand;
        return B_gravel;
      } else if (y == height + 2) {
        // Middle floor layer - mix
        if (floor_hash < 100) return B_sand;
        if (floor_hash < 180) return B_gravel;
        return B_dirt;
      } else {
        // Bottom floor layer - mostly gravel/dirt
        if (floor_hash < 50) return B_sand;
        if (floor_hash < 150) return B_gravel;
        return B_dirt;
      }
    } else {
      // Water above the floor - with underwater vegetation
      uint8_t water_depth = 63 - y;
      uint8_t veg_hash = (anchor.hash >> (x + z)) & 255;
      
      // Seagrass on the sea/lake floor (first water layer above sand/gravel)
      if (y == height + 3 && veg_hash < 40 && water_depth >= 1) {
        return B_seagrass;
      }
      
      return B_water;
    }
  }

  // Global lava level (like sea level) - fills caves and deep air pockets
  if (y <= 7) {
    // Occasional magma blocks at the bedrock floor
    if (y == 5 && ((x ^ z) & 3) == 0) return B_magma_block;
    return B_lava;
  }

  // For everything else, fall back to air
  return B_air;
}

ChunkFeature getNetherFeatureFromAnchor (ChunkAnchor anchor) {
  ChunkFeature feature;
  uint8_t feature_position = anchor.hash % (CHUNK_SIZE * CHUNK_SIZE);

  feature.x = feature_position % CHUNK_SIZE;
  feature.z = feature_position / CHUNK_SIZE;
  
  uint8_t skip_feature = false;
  // Density check: 50% chance per 8x8 minichunk
  if ((anchor.hash & 0x01) != 0) skip_feature = true;
  
  // Relaxed boundary check: 2x2 trunk only needs 1 block margin to avoid cutoff
  if (feature.x < 1 || feature.x > CHUNK_SIZE - 2) skip_feature = true;
  if (feature.z < 1 || feature.z > CHUNK_SIZE - 2) skip_feature = true;

  if (skip_feature) {
    feature.y = 0xFF;
    return feature;
  }

  feature.x += anchor.x * CHUNK_SIZE;
  feature.z += anchor.z * CHUNK_SIZE;
  
  // Unified floor height formula
  double fn = octave_sample(&surface_noise, (double)feature.x * 0.015, 0, (double)feature.z * 0.015);
  int fh = 60 + (int)(fn * 35.0);
  if (fh < 15) fh = 15;
  
  feature.y = fh + 1;
  feature.variant = (anchor.hash >> (feature.x + feature.z)) & 1;
  
  return feature;
}

ChunkFeature getFeatureFromAnchor (ChunkAnchor anchor) {

  ChunkFeature feature;
  uint8_t feature_position = anchor.hash % (CHUNK_SIZE * CHUNK_SIZE);

  feature.x = feature_position % CHUNK_SIZE;
  feature.z = feature_position / CHUNK_SIZE;
  uint8_t skip_feature = false;

  // The following check does two things:
  //  firstly, it ensures that trees don't cross chunk boundaries;
  //  secondly, it reduces overall feature count. This is favorable
  //  everywhere except for swamps, which are otherwise very boring.
  //  Margin of 4 ensures 3-block-radius canopies (including 2x2 trees)
  //  stay within the 8-block minichunk (positions 0-7).
  if (anchor.biome != W_swamp) {
    if (feature.x < 3 || feature.x > CHUNK_SIZE - 4) skip_feature = true;
    else if (feature.z < 3 || feature.z > CHUNK_SIZE - 4) skip_feature = true;
  }



  if (skip_feature) {
    // Skipped features are indicated by a Y coordinate of 0xFF (255)
    feature.y = 0xFF;
  } else {
    feature.x += anchor.x * CHUNK_SIZE;
    feature.z += anchor.z * CHUNK_SIZE;
    feature.y = getHeightAtFromHash(
      mod_abs(feature.x, CHUNK_SIZE), mod_abs(feature.z, CHUNK_SIZE),
      anchor.x, anchor.z, anchor.hash, anchor.biome
    ) + 1;
    feature.variant = (anchor.hash >> (feature.x + feature.z)) & 1;
  }

  return feature;

}

uint16_t getTerrainAt (int x, int y, int z, ChunkAnchor anchor) {

  if (y > 320) return B_air;

  int rx = x % CHUNK_SIZE;
  int rz = z % CHUNK_SIZE;
  if (rx < 0) rx += CHUNK_SIZE;
  if (rz < 0) rz += CHUNK_SIZE;

  ChunkFeature feature = getFeatureFromAnchor(anchor);
  uint8_t height = getHeightAt(x, z);

  return getTerrainAtFromCache(x, y, z, rx, rz, anchor, feature, height);

}

// ============================================================
// Ruined Portal
// ============================================================
// Broken obsidian portal frame on the ground with blackstone rubble.
static uint16_t getRuinedPortalBlockAt(int x, int y, int z) {
    double fn = octave_sample(&surface_noise, (double)x * 0.015, 0, (double)z * 0.015);
    int floor_h = 60 + (int)(fn * 35.0);
    if (floor_h < 15) floor_h = 15;
    int dy = y - floor_h;
    if (dy < 0 || dy > 4) return 0xFFFF;
    if (y <= 63) return 0xFFFF; // don't place in lava sea

    int px = div_floor(x, 40);
    int pz = div_floor(z, 40);
    uint64_t key = splitmix64(((uint64_t)(uint32_t)px * 12345987 ^ (uint64_t)(uint32_t)pz * 76543921) ^ world_seed);
    if ((uint32_t)(key % 3) != 0) return 0xFFFF;

    int cx = px * 40 + 10 + (int)((key >> 8) % 10);
    int cz = pz * 40 + 10 + (int)((key >> 16) % 10);

    int dx = x - cx;
    int dz = z - cz;
    if (dz < -2 || dz > 2) return 0xFFFF;
    if (dx < -3 || dx > 4) return 0xFFFF;

    uint32_t ph = (uint32_t)(x * 1013 ^ z * 7027);
    int missing = (ph >> 4) & 3;

    // Rubble on the ground around the frame
    if (dy == 0) {
        if (dz == -2 || dz == 2 || dx == -3 || dx == 4) {
            if ((ph & 7) != 0) return B_blackstone;
            return B_air;
        }
    }

    // Portal frame at dz == 0
    if (dz != 0) return 0xFFFF;

    // Bottom rail (dy == 0)
    if (dy == 0 && dx >= -1 && dx <= 2) {
        if ((dx == -1 && missing > 2) || (dx == 2 && missing > 1)) return B_air;
        return (ph & 0x80) ? B_gold_block : B_obsidian;
    }
    // Left column (dx == -1)
    if (dx == -1 && dy >= 1 && dy <= 2) {
        if ((dy == 1 && missing > 0) || (dy == 2 && missing > 2)) return B_air;
        return B_obsidian;
    }
    // Right column (dx == 2)
    if (dx == 2 && dy >= 1 && dy <= 2) {
        if ((dy == 1 && missing > 1) || (dy == 2 && missing > 0)) return B_air;
        return B_obsidian;
    }
    // Top rail (dy == 3)
    if (dy == 3 && dx >= -1 && dx <= 2) {
        if (dx == 0 && missing > 0) return B_air;
        return B_obsidian;
    }

    return 0xFFFF;
}

// ============================================================
// Glowstone Stalactite
// ============================================================
// Tapering cone of glowstone and netherrack hanging from the ceiling.
static uint16_t getGlowstoneStalactiteBlockAt(int x, int y, int z) {
    double cn = octave_sample(&surface_noise, (double)x * 0.015 + 100.0, 0, (double)z * 0.015 + 100.0);
    int ceil_h = 130 + (int)(cn * 35.0);
    int dy = y - ceil_h; // negative = below ceiling
    if (dy >= 0 || dy < -12) return 0xFFFF;

    int sx = div_floor(x, 32);
    int sz = div_floor(z, 32);
    uint64_t key = splitmix64(((uint64_t)(uint32_t)sx * 87654321 ^ (uint64_t)(uint32_t)sz * 23456789) ^ world_seed);
    if ((uint32_t)(key % 3) != 0) return 0xFFFF;

    int cx = sx * 32 + 8 + (int)((key >> 8) % 8);
    int cz = sz * 32 + 8 + (int)((key >> 16) % 8);

    int dx = x - cx;
    int dz = z - cz;
    int adx = (dx < 0) ? -dx : dx;
    int adz = (dz < 0) ? -dz : dz;
    int dist = (adx > adz) ? adx : adz;

    // Cone: radius shrinks as we go down
    int idy = -dy; // 1 = just below ceiling, 12 = furthest down
    int radius;
    if (idy <= 2) radius = 2;
    else if (idy <= 4) radius = 1;
    else if (idy <= 7) radius = 1;
    else radius = 0;
    // Taper more: at idy=3-4 radius=1, at idy=5-7 radius=0 (point)
    if (idy >= 5 && idy <= 7) radius = (dist == 0) ? 0 : -1;
    if (idy >= 8) radius = (dist == 0 && idy <= 10) ? 0 : -1;

    if (dist > radius) return 0xFFFF;

    // Random chance for shape variation
    uint32_t sh = (uint32_t)(x * 57431 ^ y * 23417 ^ z * 92831);
    // Make some blocks air to create branching look
    if (radius > 0 && (sh & 0x1F) == 0 && dist == radius) return B_air;

    // Core: glowstone, edges: netherrack
    if (dist <= radius - 1 || (sh & 3) == 0) return B_glowstone;
    return B_netherrack;
}

// ============================================================
// Nether Fortress
// ============================================================
// Grid-based nether brick fortress — a solid rectangular keep
// with pillar-supported interior, upper gallery, and treasure vault.
static uint16_t getNetherFortressBlockAt(int x, int y, int z) {
    if (y < 5 || y > 130) return 0xFFFF;

    int fx = div_floor(x, 64);
    int fz = div_floor(z, 64);

    uint64_t key = splitmix64(((uint64_t)(uint32_t)fx * 3456789 ^ (uint64_t)(uint32_t)fz * 9876543) ^ world_seed);
    if ((uint32_t)(key % 5) != 0) return 0xFFFF;

    int cx = fx * 64 + 16 + (int)((key >> 8) % 16);
    int cz = fz * 64 + 16 + (int)((key >> 16) % 16);
    int fortress_y = 75 + (int)((key >> 24) % 25);

    int len = 15 + ((key >> 30) & 3) * 4;  // 15, 19, 23, 27
    int half = len / 2;

    int dx = x - cx;
    int dz = z - cz;
    int dy = y - fortress_y;
    int adx = (dx < 0) ? -dx : dx;
    int adz = (dz < 0) ? -dz : dz;

    // Outer bounding box
    if (adx > half || adz > half) return 0xFFFF;

    // ===== Support pillars below the structure =====
    if (dy < 0) {
        // Only place pillars at wall and interior grid positions
        int is_pillar = (adx == half - 1 || adz == half - 1)
                     || ((adx & 3) == 0 && (adz & 3) == 0 && adx < half - 1 && adz < half - 1);
        if (!is_pillar) return 0xFFFF;
        double fn = octave_sample(&surface_noise, (double)x * 0.015, 0, (double)z * 0.015);
        int floor_h = 60 + (int)(fn * 35.0);
        if (floor_h < 15) floor_h = 15;
        if (y > floor_h) return B_nether_bricks;
        return 0xFFFF;
    }

    if (dy >= 6) return 0xFFFF;

    // Balcony overhang at outer edge
    if (adx == half || adz == half) {
        if (dy == 2 || dy == 3) {
            if ((adx + adz) & 1) return B_nether_bricks;
        }
        return B_air;
    }

    uint16_t block = B_air;

    // Outer walls
    if (adx == half - 1 || adz == half - 1) {
        if (dy == 0 || dy == 5) block = B_nether_bricks;
        else if (dy >= 1 && dy <= 4) {
            block = B_nether_bricks;
        }
    }
    // Interior floor and ceiling
    else if (dy == 0 || dy == 5) {
        block = B_nether_bricks;
    }
    // Interior pillars and features (every 4 blocks in both directions)
    else if (dy >= 1 && dy <= 4) {
        int px = adx;
        int pz = adz;
        if (px % 4 == 0 && pz % 4 == 0) {
            block = B_nether_bricks;
        }
        // Upper gallery: partial floor at dy=3
        if (dy == 3 && (px <= 2 || pz <= 2)) {
            block = B_nether_bricks;
        }
        // Treasure vault at center base
        if (dy == 1 && adx <= 2 && adz <= 2) {
            uint32_t thr = (uint32_t)(x * 31337 + y * 13337 + z * 74747);
            if ((thr & 7) == 0) block = B_gold_block;
            else if ((thr & 3) == 0) block = B_cracked_nether_bricks;
            else block = B_nether_bricks;
        }
        // Windows / arrow slits in outer walls
        if ((adx == half - 1 || adz == half - 1) && dy >= 1 && dy <= 3) {
            uint32_t wh = (uint32_t)(x * 1013 ^ z * 7027);
            if ((wh & 7) == 0) block = B_air;
        }
    }

    return block;
}

// ============================================================
// Bastion Remnant
// ============================================================
// Grid-based blackstone/basalt bastion with gilded treasure.
// Square keep with corner towers, inner courtyard, and treasure vault.
static uint16_t getBastionBlockAt(int x, int y, int z) {
    if (y < 5 || y > 130) return 0xFFFF;

    int bx = div_floor(x, 56);
    int bz = div_floor(z, 56);

    uint64_t key = splitmix64(((uint64_t)(uint32_t)bx * 56789123 ^ (uint64_t)(uint32_t)bz * 32948137) ^ world_seed);
    if ((uint32_t)(key % 6) != 0) return 0xFFFF;

    int cx = bx * 56 + 14 + (int)((key >> 8) % 14);
    int cz = bz * 56 + 14 + (int)((key >> 16) % 14);
    int bastion_y = 70 + (int)((key >> 24) % 25);

    int dx = x - cx;
    int dz = z - cz;
    int dy = y - bastion_y;
    int adx = (dx < 0) ? -dx : dx;
    int adz = (dz < 0) ? -dz : dz;

    if (adx > 8 || adz > 8) return 0xFFFF;

    // ===== Support pillars below the structure =====
    if (dy < 0) {
        int is_pillar = (adx >= 5 && adz >= 5)  // under outer wall
                     || (adx >= 7 && adz >= 7)   // under corner towers
                     || (adx <= 4 && adz <= 4 && (adx & 3) == 0 && (adz & 3) == 0); // courtyard grid
        if (!is_pillar) return 0xFFFF;
        double fn = octave_sample(&surface_noise, (double)x * 0.015, 0, (double)z * 0.015);
        int floor_h = 60 + (int)(fn * 35.0);
        if (floor_h < 15) floor_h = 15;
        if (y > floor_h) return B_blackstone;
        return 0xFFFF;
    }

    if (dy >= 6) return 0xFFFF;

    uint16_t block = B_air;

    // Corner towers: 5x5 pillars at each corner (radius 7-8)
    if (adx >= 7 && adz >= 7) {
        if (adx <= 8 && adz <= 8) {
            if (dy < 5) block = B_polished_andesite;
        }
        return block;
    }

    // Outer walls: radius 6-7 (3 blocks thick ring)
    int in_outer = (adx >= 5 && adz >= 5);

    if (in_outer) {
        if (dy == 0) block = B_blackstone;
        else if (dy >= 1 && dy <= 3) block = B_blackstone;
        else if (dy == 4) block = B_basalt; // wall cap
        else if (dy == 5) block = B_air;
        // Battlements: every other block on top
        if (dy == 4 && ((adx + adz) & 1)) block = B_blackstone;
        return block;
    }

    // Inner courtyard (radius <= 4)
    if (adx <= 4 && adz <= 4) {
        // Floor: basalt
        if (dy == 0) block = B_basalt;
        // Center treasure platform
        if (dy == 1 && adx <= 1 && adz <= 1) {
            block = B_gilded_blackstone;
        }
        if (dy == 2 && adx <= 1 && adz <= 1) {
            uint32_t th = (uint32_t)(x * 31337 + y * 13337 + z * 74747);
            block = ((th & 3) == 0) ? B_gold_block : B_gilded_blackstone;
        }
        // Low walls forming inner enclosure
        if (dy == 1 && (adx == 4 || adz == 4)) {
            if (adx < 3 || adz < 3) block = B_basalt;
        }
        // Pillars at inner wall corners
        if (dy >= 1 && dy <= 3 && adx == 4 && adz == 4) {
            block = B_blackstone;
        }
    }

    // Entrance bridge from outer wall to courtyard
    if (!in_outer && (adx > 4 || adz > 4)) {
        if (dy == 0) block = B_blackstone;
        if (dy == 1 && ((adx + adz) & 1)) block = B_blackstone;
    }

    return block;
}

// Searches for nearest nether structure (fortress or bastion) from (px, pz).
// Returns 1 if found and fills out_x/out_y/out_z/out_name.
uint8_t findNearestNetherStructure(int px, int pz, int radius, int *out_x, int *out_y, int *out_z, const char **out_name) {
    int best_dist = radius + 1;
    int best_x = 0, best_y = 0, best_z = 0;
    const char *best_name = NULL;

    // Nether Fortress: 64-block grid, 1 in 5 cells
    #define FORTRESS_SPACING 64
    int f_ax = div_floor(px, FORTRESS_SPACING);
    int f_az = div_floor(pz, FORTRESS_SPACING);
    int f_rad = radius / FORTRESS_SPACING + 1;

    for (int ring = 0; ring <= f_rad; ring++) {
        int start_fx, start_fz, end_fx, end_fz;
        if (ring == 0) {
            start_fx = f_ax; end_fx = f_ax;
            start_fz = f_az; end_fz = f_az;
        } else {
            start_fx = f_ax - ring; end_fx = f_ax + ring;
            start_fz = f_az - ring; end_fz = f_az + ring;
        }
        for (int fx = start_fx; fx <= end_fx; fx++) {
            for (int fz = start_fz; fz <= end_fz; fz++) {
                if (ring > 0 && fx > f_ax - ring && fx < f_ax + ring && fz > f_az - ring && fz < f_az + ring) continue;
                uint64_t key = splitmix64(((uint64_t)(uint32_t)fx * 3456789 ^ (uint64_t)(uint32_t)fz * 9876543) ^ world_seed);
                if ((uint32_t)(key % 5) != 0) continue;
                int cx = fx * FORTRESS_SPACING + 16 + (int)((key >> 8) % 16);
                int cz = fz * FORTRESS_SPACING + 16 + (int)((key >> 16) % 16);
                int cy = 75 + (int)((key >> 24) % 25);
                int dx = cx - px;
                int dz = cz - pz;
                int dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
                if (dist < best_dist) {
                    best_dist = dist;
                    best_x = cx; best_y = cy; best_z = cz;
                    best_name = "Fortress";
                }
            }
        }
    }

    // Bastion Remnant: 56-block grid, 1 in 6 cells
    #define BASTION_SPACING 56
    int b_ax = div_floor(px, BASTION_SPACING);
    int b_az = div_floor(pz, BASTION_SPACING);
    int b_rad = radius / BASTION_SPACING + 1;

    for (int ring = 0; ring <= b_rad; ring++) {
        int start_bx, start_bz, end_bx, end_bz;
        if (ring == 0) {
            start_bx = b_ax; end_bx = b_ax;
            start_bz = b_az; end_bz = b_az;
        } else {
            start_bx = b_ax - ring; end_bx = b_ax + ring;
            start_bz = b_az - ring; end_bz = b_az + ring;
        }
        for (int bx = start_bx; bx <= end_bx; bx++) {
            for (int bz = start_bz; bz <= end_bz; bz++) {
                if (ring > 0 && bx > b_ax - ring && bx < b_ax + ring && bz > b_az - ring && bz < b_az + ring) continue;
                uint64_t key = splitmix64(((uint64_t)(uint32_t)bx * 56789123 ^ (uint64_t)(uint32_t)bz * 32948137) ^ world_seed);
                if ((uint32_t)(key % 6) != 0) continue;
                int cx = bx * BASTION_SPACING + 14 + (int)((key >> 8) % 14);
                int cz = bz * BASTION_SPACING + 14 + (int)((key >> 16) % 14);
                int cy = 70 + (int)((key >> 24) % 25);
                int dx = cx - px;
                int dz = cz - pz;
                int dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
                if (dist < best_dist) {
                    best_dist = dist;
                    best_x = cx; best_y = cy; best_z = cz;
                    best_name = "Bastion";
                }
            }
        }
    }

    if (best_name != NULL) {
        *out_x = best_x;
        *out_y = best_y;
        *out_z = best_z;
        *out_name = best_name;
        return 1;
    }
    return 0;
}

// Nether block lookup used by gameplay systems. Keep this in sync with
// buildNetherChunkSection() for blocks players can interact with.
uint16_t getNetherBlockAt (int x, int y, int z) {
  if (y < 0) return B_bedrock;
  if (y > 319) return B_air;

  uint8_t biome = getChunkNetherBiome(div_floor(x, 16), div_floor(z, 16));

  // Get feature for this minichunk
  ChunkAnchor a;
  a.x = div_floor(x, 8);
  a.z = div_floor(z, 8);
  a.hash = getChunkHash(a.x, a.z);
  a.biome = biome;
  ChunkFeature feature = getNetherFeatureFromAnchor(a);

  double fn = octave_sample(&surface_noise, (double)x * 0.015, 0, (double)z * 0.015);
  int floor_h = 60 + (int)(fn * 35.0);
  if (floor_h < 15) floor_h = 15;

  double cn = octave_sample(&surface_noise, (double)x * 0.015 + 100.0, 0, (double)z * 0.015 + 100.0);
  int ceil_h = 130 + (int)(cn * 35.0);

  // Flat lake logic
  double pool_n = octave_sample(&detail_noise, (double)x * 0.01, 0, (double)z * 0.01);
  double pool_threshold = 0.5;
  int lake_y = 90;
  int is_lake = (pool_n > pool_threshold && floor_h > lake_y);
  uint32_t lh = (uint32_t)(x * 31 + z * 37 + y);

  uint16_t block = B_air;

  // === Bedrock Bottom (Y 0-4) ===
  if (y < 5) {
    block = B_bedrock;
  }
  // === Lava Sea (Y 5-63) ===
  else if (y <= 63) {
    if (y <= floor_h) {
      if (biome == W_basalt_deltas) block = (lh & 1) ? B_basalt : B_blackstone;
      else block = B_netherrack;
    } else {
      block = B_lava;
    }
  }
  // === Main Terrain Body ===
  else if (y <= floor_h) {
    if (is_lake && y >= lake_y - 4) {
      if (y > lake_y) block = B_air;
      else if (y == lake_y - 4) block = (lh & 1) ? B_magma_block : B_gravel;
      else block = B_lava;
    } else if (biome == W_basalt_deltas) {
      uint32_t bh = (uint32_t)(x * 27281 ^ y * 47297 ^ z * 32423);
      block = (bh & 1) ? B_basalt : B_blackstone;
    } else {
      block = B_netherrack;
      double cave = octave_sample(&cave_noise, x * 0.04, y * 0.04, z * 0.04);
      if (cave > 0.6) block = B_air;

      if (block == B_netherrack && y < 28) {
        uint32_t bh = (uint32_t)(x * 27281 ^ y * 47297 ^ z * 32423);
        if ((bh & 0x1F) == 0) block = B_blackstone;
        else if ((bh & 0xFF) == 0xDE) block = B_gilded_blackstone;
      }
    }
  }
  // === Ceiling Terrain ===
  else if (y >= ceil_h) {
    block = B_netherrack;
    if (biome == W_basalt_deltas) {
      uint32_t bh = (uint32_t)(x * 31337 ^ y * 13337 ^ z * 74747);
      if ((bh & 3) == 0) block = B_basalt;
    }

    // Keep this in sync with buildNetherChunkSection(): ceiling caves are
    // carved after the base ceiling material is selected.
    double cave = octave_sample(&cave_noise, x * 0.04 + 500.0, y * 0.04, z * 0.04 + 500.0);
    if (cave > 0.6) block = B_air;
  }
  // === Open Void with Bridges ===
  else if (y > floor_h && y < ceil_h) {
    double bridge_freq = 0.025;
    double bridge_threshold = 0.75;
    if (biome == W_crimson_forest || biome == W_warped_forest) bridge_threshold = 0.65;
    else if (biome == W_basalt_deltas) bridge_threshold = 0.60;
    double bridge = octave_sample(&surface_noise, x * bridge_freq, y * 0.02, z * bridge_freq);
    if (bridge > bridge_threshold) {
      double cave = octave_sample(&cave_noise, x * 0.04, y * 0.04, z * 0.04);
      if (cave < 0.5) {
        if (biome == W_basalt_deltas) {
          uint32_t bh = (uint32_t)(x * 27281 ^ y * 47297 ^ z * 32423);
          block = (bh & 1) ? B_basalt : B_blackstone;
        } else {
          block = B_netherrack;
        }
      }
    }
  }

  // Trees (Features) - 1-width trunk, tall
  if (block == B_air && (biome == W_crimson_forest || biome == W_warped_forest)) {
    if (feature.y != 0xFF && feature.y >= 32 && feature.y <= 120) {
      uint16_t stem = (biome == W_crimson_forest) ? B_crimson_stem : B_warped_stem;
      uint16_t wart = (biome == W_crimson_forest) ? B_nether_wart_block : B_warped_wart_block;
      int trunk_h = 5 + (feature.variant * 3);

      if (x == feature.x && z == feature.z) {
        if (y >= feature.y && y < feature.y + trunk_h) block = stem;
      }

      int dx = x - feature.x;
      int dz = z - feature.z;
      int adx = (dx < 0) ? -dx : dx;
      int adz = (dz < 0) ? -dz : dz;
      int dist = adx + adz;
      int cap_y = feature.y + trunk_h;
      if (y >= cap_y - 2 && y <= cap_y + 1) {
        int radius = (y == cap_y + 1) ? 1 : (y >= cap_y) ? 3 : 2;
        if (dist <= radius) {
          uint32_t ch = (uint32_t)(x * 7211 ^ z * 5237);
          block = ((ch & 0x1F) == 0) ? B_shroomlight : wart;
        }
      }
    }
  }

  // General Decorations
  if (block == B_netherrack || block == B_basalt || block == B_blackstone) {
    uint32_t h = (uint32_t)(x * 31337 + y * 13337 + z * 74747);
    if (block == B_netherrack) {
      if ((h & 0x3F) == 0) block = B_nether_quartz_ore;
      else if ((h & 0x7F) == 0x20 && y < 80) block = B_nether_gold_ore;
      else if ((h & 0xFFF) == 0xDEB && y < 40) block = B_ancient_debris;
    }
    if (y >= ceil_h && y < ceil_h + 6) {
      double glow = octave_sample(&detail_noise, x * 0.08 + 2000.0, y * 0.1, z * 0.08 + 2000.0);
      if (glow > 0.65) block = B_glowstone;
    }
    if (y == floor_h || y == ceil_h) {
      if (biome == W_crimson_forest) block = B_crimson_nylium;
      else if (biome == W_warped_forest) block = B_warped_nylium;
    }
  }

  // Ruined Portals at ground level
  if (block == B_air && y == floor_h + 1 && !is_lake) {
    uint16_t rp = getRuinedPortalBlockAt(x, y, z);
    if (rp != 0xFFFF) block = rp;
  }

  // Surface Decorations
  if (block == B_air && y == floor_h + 1 && !is_lake) {
    double soul_threshold = 0.5;
    if (biome == W_soul_sand_valley) soul_threshold = 0.2;
    double soul = octave_sample(&detail_noise, x * 0.02 + 3000.0, 0, z * 0.02 + 3000.0);
    if (soul > soul_threshold) {
      uint32_t sh = (uint32_t)(x * 31337 + z * 74747);
      block = (sh & 1) ? B_soul_sand : B_soul_soil;
    }
    if (biome == W_soul_sand_valley || biome == W_basalt_deltas) {
      double magma = octave_sample(&detail_noise, x * 0.04 + 7000.0, 0, z * 0.04 + 7000.0);
      if (magma > 0.65) block = B_magma_block;
    }
    if (biome == W_nether_wastes) {
      double gravel = octave_sample(&detail_noise, x * 0.03 + 11000.0, 0, z * 0.03 + 11000.0);
      if (gravel > 0.75) block = B_gravel;
    }
  }

  // Basalt columns
  if (block == B_air && y > floor_h + 1 && y < floor_h + 40 && !is_lake) {
    double col_threshold = 0.75;
    if (biome == W_basalt_deltas) col_threshold = 0.55;
    else if (biome == W_soul_sand_valley) col_threshold = 0.65;
    double col = octave_sample(&detail_noise, x * 0.05 + 5000.0, 0, z * 0.05 + 5000.0);
    if (col > col_threshold) {
      double col_h_n = octave_sample(&surface_noise, x * 0.015 + 6000.0, 0, z * 0.015 + 6000.0);
      int col_h = 5 + (int)((col_h_n + 1.0) * 15.0);
      if (y - floor_h <= col_h) block = B_basalt;
    }
  }

  // Glowstone stalactites hanging from ceiling
  if (block == B_air && y < ceil_h && y >= ceil_h - 12 && !is_lake) {
    uint16_t gs = getGlowstoneStalactiteBlockAt(x, y, z);
    if (gs != 0xFFFF) block = gs;
  }

  // Nether structures (including support pillars below them)
  if (block == B_air && y >= 5 && y <= 130) {
    uint16_t fb = getNetherFortressBlockAt(x, y, z);
    if (fb != 0xFFFF) block = fb;
    if (block == B_air) {
      uint16_t bb = getBastionBlockAt(x, y, z);
      if (bb != 0xFFFF) block = bb;
    }
  }

  return block;
}

static int getEndIslandBoundsAt(int x, int z, int *bottom, int *top, uint8_t *biome) {
  double dx = (double)x;
  double dz = (double)z;
  double r2 = dx * dx + dz * dz;
  double main_radius = 104.0;

  if (r2 <= main_radius * main_radius) {
    double r = sqrt(r2);
    double noise = octave_sample(&surface_noise, x * 0.025, 0, z * 0.025);
    int t = 64 - (int)((r * r) / 190.0) + (int)(noise * 5.0);
    int thickness = 10 + (int)((main_radius - r) / 5.0);
    if (thickness < 6) thickness = 6;
    *top = t;
    *bottom = t - thickness;
    *biome = W_the_end;
    return 1;
  }

  // Sparse outer End islands. This is intentionally simple: deterministic island
  // centers per 256×256 region, skipped near the main island to preserve the void.
  int region_x = div_floor(x, 256);
  int region_z = div_floor(z, 256);
  uint64_t key = splitmix64(((uint64_t)(uint32_t)region_x << 32) ^ (uint32_t)region_z ^ world_seed ^ 0x454e44ULL);
  int center_x = region_x * 256 + 64 + (int)((key >> 8) & 127);
  int center_z = region_z * 256 + 64 + (int)((key >> 16) & 127);
  int cdx = x - center_x;
  int cdz = z - center_z;
  double cr2 = (double)cdx * cdx + (double)cdz * cdz;
  int far_x = center_x < 0 ? -center_x : center_x;
  int far_z = center_z < 0 ? -center_z : center_z;
  if (far_x < 640 && far_z < 640) return 0;

  double radius = 22.0 + (double)((key >> 24) & 31);
  if (cr2 > radius * radius) return 0;

  double cr = sqrt(cr2);
  double noise = octave_sample(&detail_noise, x * 0.035 + 9000.0, 0, z * 0.035 + 9000.0);
  int t = 70 + (int)(noise * 7.0) - (int)((cr * cr) / (radius * 2.2));
  int thickness = 5 + (int)((radius - cr) / 3.0);
  if (thickness < 3) thickness = 3;
  *top = t;
  *bottom = t - thickness;
  *biome = (radius > 40.0) ? W_end_highlands : W_small_end_islands;
  return 1;
}

uint8_t getChunkEndBiome(short x, short z) {
  int bottom, top;
  uint8_t biome = W_the_end;
  return getEndIslandBoundsAt(x * 16 + 8, z * 16 + 8, &bottom, &top, &biome) ? biome : W_the_end;
}

uint16_t getEndBlockAt(int x, int y, int z) {
  if (y < 0 || y > 319) return B_air;

  int bottom, top;
  uint8_t biome;
  if (getEndIslandBoundsAt(x, z, &bottom, &top, &biome) && y >= bottom && y <= top) {
    // Place exit portal on the surface of the main End island center
    // Check if we're within the center platform area (a 5x5 square from -2 to 2)
    if (x >= -2 && x <= 2 && z >= -2 && z <= 2) {
      // Create a flat exit portal platform at Y=64
      // The natural surface may have noise, but we override for the portal platform
      int portal_top = 64;  // Fixed height for the exit portal platform
      int platform_bottom = 54;  // 10 blocks down from portal top
      
      // Portal blocks at surface level
      if (y == portal_top) {
        return B_end_portal;
      }
      // Fill the platform area from portal top down to platform_bottom
      if (y >= platform_bottom && y < portal_top) {
        return B_end_stone;
      }
      // Fall through to natural island generation for deeper parts
    }
    return B_end_stone;
  }
  return B_air;
}

uint16_t buildEndChunkSection(int cx, int cy, int cz) {
  uint8_t biome = getChunkEndBiome(div_floor(cx, 16), div_floor(cz, 16));

  for (int dy = 0; dy < 16; dy++) {
    int y = cy + dy;
    for (int dz = 0; dz < 16; dz++) {
      int z = cz + dz;
      for (int dx = 0; dx < 16; dx++) {
        int x = cx + dx;
        unsigned address = (unsigned)(dx + (dz << 4) + (dy << 8));
        unsigned index = (address & ~7u) | (7u - (address & 7u));
        chunk_section[index] = getEndBlockAt(x, y, z);
      }
    }
  }

  if (!isChunkModified(div_floor(cx, 16), div_floor(cz, 16))) return biome;

  int block_changes_snapshot_count = 0;
  BlockChange *block_changes_snapshot = copyBlockChangesSnapshot(&block_changes_snapshot_count);
  if (block_changes_snapshot_count > 0 && block_changes_snapshot != NULL) {
    for (int i = 0; i < block_changes_snapshot_count; i++) {
      if (block_changes_snapshot[i].block == 0xFF) continue;
      if (is_stair_block(block_changes_snapshot[i].block) || is_oriented_block(block_changes_snapshot[i].block)) {
        if (block_changes_snapshot[i].block == B_chest || block_changes_snapshot[i].block == B_barrel) i += 14;
        else if (is_stair_block(block_changes_snapshot[i].block) || block_changes_snapshot[i].block == B_furnace || block_changes_snapshot[i].block == B_ender_chest) i += 1;
        continue;
      }
      if (block_changes_snapshot[i].dimension != DIMENSION_END ||
          block_changes_snapshot[i].x < cx || block_changes_snapshot[i].x >= cx + 16 ||
          block_changes_snapshot[i].y < cy || block_changes_snapshot[i].y >= cy + 16 ||
          block_changes_snapshot[i].z < cz || block_changes_snapshot[i].z >= cz + 16) continue;
      int dx = block_changes_snapshot[i].x - cx;
      int dy = block_changes_snapshot[i].y - cy;
      int dz = block_changes_snapshot[i].z - cz;
      unsigned address = (unsigned)(dx + (dz << 4) + (dy << 8));
      unsigned index = (address & ~7u) | (7u - (address & 7u));
      chunk_section[index] = block_changes_snapshot[i].block;
    }
    freeBlockChangesSnapshot(block_changes_snapshot);
  }

  return biome;
}

uint16_t getBlockAt2 (int x, int y, int z, uint8_t dimension) {
  uint16_t block_change = getBlockChange(x, y, z, dimension);
  if (block_change != 0xFF) return block_change;
  if (dimension == DIMENSION_NETHER) {
    return getNetherBlockAt(x, y, z);
  }
  if (dimension == DIMENSION_END) {
    return getEndBlockAt(x, y, z);
  }
  short anchor_x = div_floor(x, CHUNK_SIZE);
  short anchor_z = div_floor(z, CHUNK_SIZE);
  ChunkAnchor anchor = {
    .x = anchor_x,
    .z = anchor_z,
    .hash = getChunkHash(anchor_x, anchor_z),
    .biome = getChunkBiome(anchor_x, anchor_z)
  };
  uint16_t block = getTerrainAt(x, y, z, anchor);
  if (block & 0x8000) return block & 0x1FF;
  return block;
}

uint16_t getBlockAt (int x, int y, int z) {
  return getBlockAt2(x, y, z, DIMENSION_OVERWORLD);
}

WORLDGEN_THREAD_LOCAL uint16_t chunk_section[4096];
static WORLDGEN_THREAD_LOCAL ChunkAnchor chunk_anchors[(16 / CHUNK_SIZE + 1) * (16 / CHUNK_SIZE + 1)];
static WORLDGEN_THREAD_LOCAL ChunkFeature chunk_features[256 / (CHUNK_SIZE * CHUNK_SIZE)];
static WORLDGEN_THREAD_LOCAL uint8_t chunk_section_height[16][16];

uint8_t getChunkNetherBiome(short x, short z) {
    init_worldgen();
    int bx = (int)x * 16 + 8;
    int bz = (int)z * 16 + 8;
    
    // Sample floor height at chunk center to check if submerged
    double floor_n = octave_sample(&surface_noise, bx * 0.015, 0, bz * 0.015);
    int floor_h = 60 + (int)(floor_n * 35.0);
    if (floor_h < 15) floor_h = 15;

    float ndel;
    // Sample biome at sea level (Y=16 * 4 = 64)
    int cid = getNetherBiome(&nether_noise, bx >> 2, 16, bz >> 2, &ndel);
    uint8_t b;
    switch (cid) {
        case nether_wastes:      b = W_nether_wastes; break;
        case soul_sand_valley:   b = W_soul_sand_valley; break;
        case crimson_forest:     b = W_crimson_forest; break;
        case warped_forest:      b = W_warped_forest; break;
        case basalt_deltas:      b = W_basalt_deltas; break;
        default:                 b = W_nether_wastes; break;
    }
    
    // If the floor is submerged under the lava sea (Y=63), 
    // force it to Nether Wastes to avoid forests under lava.
    if (floor_h <= 63 && (b == W_warped_forest || b == W_crimson_forest)) {
        return W_nether_wastes;
    }
    return b;
}

// Builds a 16x16x16 nether chunk section
// Fills chunk_section with nether terrain blocks
// Returns the biome for this chunk column
uint16_t buildNetherChunkSection(int cx, int cy, int cz) {
    init_worldgen();

    short anchor_x = (short)div_floor(cx, 16);
    short anchor_z = (short)div_floor(cz, 16);
    uint8_t biome = getChunkNetherBiome(anchor_x, anchor_z);

    // Precompute 4 minichunk features
    ChunkFeature features[4];
    for (int i = 0; i < 4; i++) {
        ChunkAnchor a;
        a.x = anchor_x * 2 + (i & 1);
        a.z = anchor_z * 2 + (i >> 1);
        a.hash = getChunkHash(a.x, a.z);
        a.biome = biome;
        features[i] = getNetherFeatureFromAnchor(a);
    }

    for (int dz = 0; dz < 16; dz++) {
        int z = cz + dz;
        int rz_idx = (dz / 8) * 2;
        for (int dx = 0; dx < 16; dx++) {
            int x = cx + dx;
            int feature_idx = rz_idx + (dx / 8);
            ChunkFeature feature = features[feature_idx];

            // Sample column-based noise
            double floor_n = octave_sample(&surface_noise, x * 0.015, 0, z * 0.015);
            int floor_h = 60 + (int)(floor_n * 35.0); // Range ~25 to 95
            if (floor_h < 15) floor_h = 15;

            double ceil_n = octave_sample(&surface_noise, x * 0.015 + 100.0, 0, z * 0.015 + 100.0);
            int ceil_h = 130 + (int)(ceil_n * 35.0); // Range ~95 to 165
            
            // Flat lake logic (like Overworld Y=64)
            double pool_n = octave_sample(&detail_noise, x * 0.01, 0, z * 0.01);
            double pool_threshold = 0.5;
            int lake_y = 90;
            // Only form a lake if the ground is high enough to contain it
            int is_lake = (pool_n > pool_threshold && floor_h > lake_y);

            for (int dy = 0; dy < 16; dy++) {
                int y = cy + dy;
                int address = dx + (dz << 4) + (dy << 8);
                int index = (address & ~7) | (7 - (address & 7));
                uint32_t lh = (uint32_t)(x * 31 + z * 37 + y);

                uint16_t block = B_air;

                // === Bedrock Bottom (Y 0-4) ===
                if (y < 5) {
                    block = B_bedrock;
                }
                // === Lava Sea (Y 5-31) ===
                else if (y <= 63) {
                    if (y <= floor_h) {
                        // Terrain below sea level
                        if (biome == W_basalt_deltas) block = (lh & 1) ? B_basalt : B_blackstone;
                        else block = B_netherrack;
                    } else {
                        // Open lava sea
                        block = B_lava;
                    }
                }
                // === Main Terrain Body (Y 32 to floor_h) ===
                else if (y <= floor_h) {
                    // Carve Flat Lakes into the terrain
                    if (is_lake && y >= lake_y - 4) {
                        if (y > lake_y) block = B_air;
                        else if (y == lake_y - 4) block = (lh & 1) ? B_magma_block : B_gravel;
                        else block = B_lava;
                    } else {
                        // Regular Netherrack/Basalt body with caves
                        if (biome == W_basalt_deltas) {
                            uint32_t bh = (uint32_t)(x * 27281 ^ y * 47297 ^ z * 32423);
                            block = (bh & 1) ? B_basalt : B_blackstone;
                        } else {
                            block = B_netherrack;
                            // Add 3D caves to Zone 2
                            double cave = octave_sample(&cave_noise, x * 0.04, y * 0.04, z * 0.04);
                            if (cave > 0.6) block = B_air;
                            
                            // Rare blackstone/gilded blackstone patches
                            if (block == B_netherrack && y < 28) {
                                uint32_t bh = (uint32_t)(x * 27281 ^ y * 47297 ^ z * 32423);
                                if ((bh & 0x1F) == 0) block = B_blackstone;
                                else if ((bh & 0xFF) == 0xDE) block = B_gilded_blackstone;
                            }
                        }
                    }
                }
                // === Ceiling Terrain (ceil_h to 255) ===
                else if (y >= ceil_h) {
                    block = B_netherrack;
                    if (biome == W_basalt_deltas) {
                         uint32_t bh = (uint32_t)(x * 31337 ^ y * 13337 ^ z * 74747);
                         if ((bh & 3) == 0) block = B_basalt;
                    }
                    // Caves in ceiling
                    double cave = octave_sample(&cave_noise, x * 0.04 + 500.0, y * 0.04, z * 0.04 + 500.0);
                    if (cave > 0.6) block = B_air;
                }
                // === Open Void with Bridges ===
                else if (y > floor_h && y < ceil_h) {
                    double bridge_freq = 0.025;
                    double bridge_threshold = 0.75;
                    if (biome == W_crimson_forest || biome == W_warped_forest) bridge_threshold = 0.65;
                    else if (biome == W_basalt_deltas) bridge_threshold = 0.60;
                    double bridge = octave_sample(&surface_noise, x * bridge_freq, y * 0.02, z * bridge_freq);
                    if (bridge > bridge_threshold) {
                        double cave = octave_sample(&cave_noise, x * 0.04, y * 0.04, z * 0.04);
                        if (cave < 0.5) {
                            if (biome == W_basalt_deltas) {
                                uint32_t bh = (uint32_t)(x * 27281 ^ y * 47297 ^ z * 32423);
                                block = (bh & 1) ? B_basalt : B_blackstone;
                            } else block = B_netherrack;
                        }
                    }
                }

                // Trees (Features) - 1-width trunk, tall
                if (block == B_air && (biome == W_crimson_forest || biome == W_warped_forest)) {
                    if (feature.y != 0xFF && feature.y >= 32 && feature.y <= 120) {
                        uint16_t stem = (biome == W_crimson_forest) ? B_crimson_stem : B_warped_stem;
                        uint16_t wart = (biome == W_crimson_forest) ? B_nether_wart_block : B_warped_wart_block;
                        int trunk_h = 5 + (feature.variant * 3);
                        
                        // 1x1 Trunk
                        if (x == feature.x && z == feature.z) {
                            if (y >= feature.y && y < feature.y + trunk_h) block = stem;
                        }
                        
                        // Organic Cap
                        int dx = x - feature.x;
                        int dz = z - feature.z;
                        int adx = (dx < 0) ? -dx : dx;
                        int adz = (dz < 0) ? -dz : dz;
                        int dist = adx + adz;
                        int cap_y = feature.y + trunk_h;
                        if (y >= cap_y - 2 && y <= cap_y + 1) {
                            int radius = (y == cap_y + 1) ? 1 : (y >= cap_y) ? 3 : 2;
                            if (dist <= radius) {
                                uint32_t ch = (uint32_t)(x * 7211 ^ z * 5237);
                                block = ((ch & 0x1F) == 0) ? B_shroomlight : wart;
                            }
                        }
                    }
                }

                // General Decorations
                if (block == B_netherrack || block == B_basalt || block == B_blackstone) {
                    uint32_t h = (uint32_t)(x * 31337 + y * 13337 + z * 74747);
                    if (block == B_netherrack) {
                        if ((h & 0x3F) == 0) block = B_nether_quartz_ore;
                        else if ((h & 0x7F) == 0x20 && y < 80) block = B_nether_gold_ore;
                        else if ((h & 0xFFF) == 0xDEB && y < 40) block = B_ancient_debris;
                    }
                    if (y >= ceil_h && y < ceil_h + 6) {
                        double glow = octave_sample(&detail_noise, x * 0.08 + 2000.0, y * 0.1, z * 0.08 + 2000.0);
                        if (glow > 0.65) block = B_glowstone;
                    }
                    if (y == floor_h || y == ceil_h) {
                        if (biome == W_crimson_forest) block = B_crimson_nylium;
                        else if (biome == W_warped_forest) block = B_warped_nylium;
                    }
                }

                // Ruined Portals at ground level
                if (block == B_air && y == floor_h + 1 && !is_lake) {
                    uint16_t rp = getRuinedPortalBlockAt(x, y, z);
                    if (rp != 0xFFFF) block = rp;
                }

                // Surface Decorations
                if (block == B_air && y == floor_h + 1 && !is_lake) {
                    double soul_threshold = 0.5;
                    if (biome == W_soul_sand_valley) soul_threshold = 0.2;
                    double soul = octave_sample(&detail_noise, x * 0.02 + 3000.0, 0, z * 0.02 + 3000.0);
                    if (soul > soul_threshold) {
                        uint32_t sh = (uint32_t)(x * 31337 + z * 74747);
                        block = (sh & 1) ? B_soul_sand : B_soul_soil;
                    }
                    if (biome == W_soul_sand_valley || biome == W_basalt_deltas) {
                        double magma = octave_sample(&detail_noise, x * 0.04 + 7000.0, 0, z * 0.04 + 7000.0);
                        if (magma > 0.65) block = B_magma_block;
                    }
                    if (biome == W_nether_wastes) {
                        double gravel = octave_sample(&detail_noise, x * 0.03 + 11000.0, 0, z * 0.03 + 11000.0);
                        if (gravel > 0.75) block = B_gravel;
                    }
                }

                // Basalt columns
                if (block == B_air && y > floor_h + 1 && y < floor_h + 40 && !is_lake) {
                    double col_threshold = 0.75;
                    if (biome == W_basalt_deltas) col_threshold = 0.55;
                    else if (biome == W_soul_sand_valley) col_threshold = 0.65;
                    double col = octave_sample(&detail_noise, x * 0.05 + 5000.0, 0, z * 0.05 + 5000.0);
                    if (col > col_threshold) {
                        double col_h_n = octave_sample(&surface_noise, x * 0.015 + 6000.0, 0, z * 0.015 + 6000.0);
                        int col_h = 5 + (int)((col_h_n + 1.0) * 15.0);
                        if (y - floor_h <= col_h) block = B_basalt;
                    }
                }

                // Glowstone stalactites hanging from ceiling
                if (block == B_air && y < ceil_h && y >= ceil_h - 12 && !is_lake) {
                    uint16_t gs = getGlowstoneStalactiteBlockAt(x, y, z);
                    if (gs != 0xFFFF) block = gs;
                }

                // Nether structures (including support pillars below them)
                if (block == B_air && y >= 5 && y <= 130) {
                    uint16_t fb = getNetherFortressBlockAt(x, y, z);
                    if (fb != 0xFFFF) block = fb;
                    if (block == B_air) {
                        uint16_t bb = getBastionBlockAt(x, y, z);
                        if (bb != 0xFFFF) block = bb;
                    }
                }

                chunk_section[index] = block;
            }
        }
    }

    if (!isChunkModified(div_floor(cx, 16), div_floor(cz, 16))) return biome;

    int block_changes_snapshot_count = 0;
    BlockChange *block_changes_snapshot = copyBlockChangesSnapshot(&block_changes_snapshot_count);
    if (block_changes_snapshot_count > 0 && block_changes_snapshot != NULL) {
        for (int i = 0; i < block_changes_snapshot_count; i ++) {
            if (block_changes_snapshot[i].block == 0xFF) continue;
            if (is_stair_block(block_changes_snapshot[i].block) || is_oriented_block(block_changes_snapshot[i].block)) {
                if (block_changes_snapshot[i].block == B_chest) i += 14;
                else if (is_stair_block(block_changes_snapshot[i].block) || block_changes_snapshot[i].block == B_furnace) i += 1;
                continue;
            }
            if (block_changes_snapshot[i].dimension != DIMENSION_NETHER ||
                block_changes_snapshot[i].x < cx || block_changes_snapshot[i].x >= cx + 16 ||
                block_changes_snapshot[i].y < cy || block_changes_snapshot[i].y >= cy + 16 ||
                block_changes_snapshot[i].z < cz || block_changes_snapshot[i].z >= cz + 16) continue;
            int dx = block_changes_snapshot[i].x - cx;
            int dy = block_changes_snapshot[i].y - cy;
            int dz = block_changes_snapshot[i].z - cz;
            unsigned address = (unsigned)(dx + (dz << 4) + (dy << 8));
            unsigned index = (address & ~7u) | (7u - (address & 7u));
            chunk_section[index] = block_changes_snapshot[i].block;
        }
        freeBlockChangesSnapshot(block_changes_snapshot);
    }

    return biome;
}

// Builds a 16x16x16 chunk of blocks and writes it to `chunk_section`
// Returns the biome at the origin corner of the chunk
uint16_t buildChunkSection (int cx, int cy, int cz, uint8_t dimension) {
  if (dimension == DIMENSION_NETHER) {
    return buildNetherChunkSection(cx, cy, cz);
  }
  if (dimension == DIMENSION_END) {
    return buildEndChunkSection(cx, cy, cz);
  }
  // Precompute hashes, anchors and features for each relevant minichunk
  int anchor_index = 0, feature_index = 0;
  for (int i = cz; i < cz + 16 + CHUNK_SIZE; i += CHUNK_SIZE) {
    for (int j = cx; j < cx + 16 + CHUNK_SIZE; j += CHUNK_SIZE) {

      ChunkAnchor *anchor = chunk_anchors + anchor_index;

      anchor->x = div_floor(j, CHUNK_SIZE);
      anchor->z = div_floor(i, CHUNK_SIZE);
      anchor->hash = getChunkHash(anchor->x, anchor->z);
      anchor->biome = getChunkBiome(anchor->x, anchor->z);

      // Compute chunk features for the minichunks within this section
      if (i != cz + 16 && j != cx + 16) {
        chunk_features[feature_index] = getFeatureFromAnchor(*anchor);
        feature_index ++;
      }

      anchor_index ++;
    }
  }

  // Precompute terrain height for entire chunk section
  for (int i = 0; i < 16; i ++) {
    for (int j = 0; j < 16; j ++) {
      anchor_index = (j / CHUNK_SIZE) + (i / CHUNK_SIZE) * (16 / CHUNK_SIZE + 1);
      ChunkAnchor *anchor_ptr = chunk_anchors + anchor_index;
      chunk_section_height[j][i] = getHeightAtFromAnchors(j % CHUNK_SIZE, i % CHUNK_SIZE, anchor_ptr);
    }
  }

  // Generate 4096 blocks using tri-nested loop for better performance
  for (int dy = 0; dy < 16; dy++) {
    int y = cy + dy;
    for (int dz = 0; dz < 16; dz++) {
      int z = cz + dz;
      int rz_mod = dz % CHUNK_SIZE;
      int rz_idx = dz / CHUNK_SIZE * (16 / CHUNK_SIZE);
      int anchor_base = dz / CHUNK_SIZE * (16 / CHUNK_SIZE + 1);

      for (int dx = 0; dz < 16 && dx < 16; dx++) {
        // The protocol expects 8-block sequence reversal for big-endian longs
        int address = dx + (dz << 4) + (dy << 8);
        int index = (address & ~7) | (7 - (address & 7));

        int feature_idx = dx / CHUNK_SIZE + rz_idx;
        int anchor_idx = dx / CHUNK_SIZE + anchor_base;

        chunk_section[index] = getTerrainAtFromCache(
          cx + dx, y, z,
          dx % CHUNK_SIZE, rz_mod,
          chunk_anchors[anchor_idx],
          chunk_features[feature_idx],
          chunk_section_height[dx][dz]
        );
      }
    }
  }

  // Skip the expensive block-change overlay scan when this chunk has no
  // player-made edits recorded.
  if (!isChunkModified(div_floor(cx, 16), div_floor(cz, 16))) {
    return chunk_anchors[0].biome;
  }

  int block_changes_snapshot_count = 0;
  BlockChange *block_changes_snapshot = copyBlockChangesSnapshot(&block_changes_snapshot_count);
  if (block_changes_snapshot_count <= 0 || block_changes_snapshot == NULL) {
    return chunk_anchors[0].biome;
  }

  // Apply block changes on top of terrain
  // Special blocks (stairs, doors, chests, furnaces) are SKIPPED here because
  // their state IDs don't fit in the uint16_t chunk_section[] / fixed 256-entry
  // global palette. They are sent via individual block update packets after the
  // chunk bulk (see sc_chunkDataAndUpdateLight).
  for (int i = 0; i < block_changes_snapshot_count; i ++) {
    if (block_changes_snapshot[i].block == 0xFF) continue;
    // Skip special blocks — they use block updates, not chunk data
    if (is_stair_block(block_changes_snapshot[i].block) || is_oriented_block(block_changes_snapshot[i].block) || is_fence_block(block_changes_snapshot[i].block) || is_horizontal_facing_block(block_changes_snapshot[i].block) || block_changes_snapshot[i].block == B_lantern) {
      if (block_changes_snapshot[i].block == B_chest || block_changes_snapshot[i].block == B_barrel) i += 14;
      else if (is_stair_block(block_changes_snapshot[i].block) || block_changes_snapshot[i].block == B_furnace || block_changes_snapshot[i].block == B_ender_chest || is_fence_block(block_changes_snapshot[i].block) || is_horizontal_facing_block(block_changes_snapshot[i].block) || block_changes_snapshot[i].block == B_lantern) i += 1;
      continue;
    }
    #ifdef ALLOW_DOORS
    if (is_door_block(block_changes_snapshot[i].block)) {
      // Still write the raw door block ID into chunk_section so the client
      // sees *something*. The correct state comes via block update packets.
    } else
    #endif
    // Check if block is within this chunk section and matches dimension
    if (
      block_changes_snapshot[i].x >= cx && block_changes_snapshot[i].x < cx + 16 &&
      block_changes_snapshot[i].y >= cy && block_changes_snapshot[i].y < cy + 16 &&
      block_changes_snapshot[i].z >= cz && block_changes_snapshot[i].z < cz + 16 &&
      block_changes_snapshot[i].dimension == dimension
    ) {
      int dx = block_changes_snapshot[i].x - cx;
      int dy = block_changes_snapshot[i].y - cy;
      int dz = block_changes_snapshot[i].z - cz;
      unsigned address = (unsigned)(dx + (dz << 4) + (dy << 8));
      unsigned index = (address & ~7u) | (7u - (address & 7u));
      chunk_section[index] = block_changes_snapshot[i].block;
    }
  }

  freeBlockChangesSnapshot(block_changes_snapshot);
  return chunk_anchors[0].biome;

}
