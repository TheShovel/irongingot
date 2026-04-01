#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "globals.h"
#include "tools.h"
#include "registries.h"
#include "procedures.h"
#include "worldgen.h"
#include "perlin.h"
#include "generator.h"

static Generator g;
static SurfaceNoise surface_noise_biome;
OctavePerlinNoiseSampler surface_noise;
OctavePerlinNoiseSampler detail_noise;
static OctavePerlinNoiseSampler cave_noise;
OctavePerlinNoiseSampler mountain_noise;
// Ore clump noise for clustered ore generation
static OctavePerlinNoiseSampler ore_clump_noise;
static int gen_initialized = 0;

// Height cache for smooth chunk generation (covers 32x32 area for seamless borders)
static int cache_cx = 0x7FFFFFFF, cache_cz = 0x7FFFFFFF;
static uint8_t height_cache[36][36];  // 32x32 + 2 block border on each side

void init_worldgen() {
    if (gen_initialized) return;
    setupGenerator(&g, MC_1_20, 0);
    applySeed(&g, DIM_OVERWORLD, (uint64_t)world_seed);
    initSurfaceNoise(&surface_noise_biome, DIM_OVERWORLD, (uint64_t)world_seed);

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
    gen_initialized = 1;
}

uint32_t getChunkHash (short x, short z) {

  uint8_t buf[8];
  memcpy(buf, &x, 2);
  memcpy(buf + 2, &z, 2);
  memcpy(buf + 4, &world_seed, 4);

  return splitmix64(*((uint64_t *)buf));

}

uint8_t getChunkBiome (short x, short z) {
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

  // Generate raw heights for 36x36 area (32x32 superchunk + 2 block border)
  // Apply smoothing filter to reduce visible seams
  for (int dz = 0; dz < 36; dz++) {
    for (int dx = 0; dx < 36; dx++) {
      int raw_x = base_x + dx - 2;
      int raw_z = base_z + dz - 2;
      
      // Sample multiple points and average for smoother terrain
      uint8_t h0 = getHeightAtRaw(raw_x, raw_z);
      uint8_t h1 = getHeightAtRaw(raw_x + 1, raw_z);
      uint8_t h2 = getHeightAtRaw(raw_x, raw_z + 1);
      uint8_t h3 = getHeightAtRaw(raw_x + 1, raw_z + 1);
      
      // Average the samples for smoother transitions
      height_cache[dx][dz] = (h0 + h1 + h2 + h3) / 4;
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
static uint8_t getOreSeedAt(int x, int y, int z, uint8_t biome) {
  // Sample ore clump noise to find vein centers
  double ore_noise = octave_sample(&ore_clump_noise, x * 0.12, y * 0.12, z * 0.12);
  double ore_density = (ore_noise + 1.0) / 2.0;
  
  // Use position-based hash for additional randomness
  uint8_t ore_hash = (uint8_t)(x * 31 + y * 37 + z * 41);
  
  // Diamond: deep underground (Y < 16), rare
  if (y < 16) {
    if (ore_density > 0.75 && (ore_hash & 0x1F) == 0) return B_diamond_ore;
    if (ore_density > 0.70 && (ore_hash & 0x3F) == 0) return B_gold_ore;
    if (ore_density > 0.68 && (ore_hash & 0x1F) == 0) return B_redstone_ore;
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
static uint8_t getOreClumpAt(int x, int y, int z, uint8_t biome) {
  // First, check if this block is an ore seed
  uint8_t seed_ore = getOreSeedAt(x, y, z, biome);
  if (seed_ore != 0) return seed_ore;
  
  // Not a seed - check if adjacent to a seed and spread with 1/4 chance
  // Each direction has its own hash for independent spread chance
  uint8_t adjacent_ore = 0;
  
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
  if (y < 6) return 0;

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


uint8_t getTerrainAtFromCache (int x, int y, int z, int rx, int rz, ChunkAnchor anchor, ChunkFeature feature, uint8_t height) {

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
        for (int ly = leaf_base; ly <= leaf_top; ly++) {
          int dist_from_top = leaf_top - ly;
          int max_radius = (dist_from_top / 2) + 1;
          if (y == ly && dist <= max_radius && dist > 0) {
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

        for (int ly = leaf_base; ly <= leaf_top; ly++) {
          int dist_from_top = leaf_top - ly;
          int max_radius = (dist_from_top / 2) + 1;
          if (y == ly && dist <= max_radius && dist > 0) {
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

        // Get distance from tree center
        uint8_t cx = base_x + 1;
        uint8_t cz = base_z + 1;
        uint8_t dx = x > cx ? x - cx : cx - x;
        uint8_t dz = z > cz ? z - cz : cz - z;
        uint8_t dist = (dx > dz ? dx : dz);

        // Large jungle leaf canopy - wide and flat, moved down by 1 block
        uint8_t canopy_base = feature.y + trunk_height - 3;
        uint8_t canopy_top = feature.y + trunk_height + 1;

        // Main canopy layers
        if (y >= canopy_base && y <= canopy_top) {
          if (dist <= 3) {
            if (y == canopy_base && dist == 3) break;
            return B_jungle_leaves;
          }
        }
        // Upper canopy
        if (y == canopy_top + 1 && dist <= 2) return B_jungle_leaves;
        if (y == canopy_top + 2 && dist <= 1) return B_jungle_leaves;
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

        // Get distance from tree center
        uint8_t cx = base_x + 1;
        uint8_t cz = base_z + 1;
        uint8_t dx = x > cx ? x - cx : cx - x;
        uint8_t dz = z > cz ? z - cz : cz - z;
        uint8_t dist = dx + dz;

        // Large dark oak canopy - dome shape, moved down by 1 block
        uint8_t canopy_base = feature.y + trunk_height - 1;
        if (y >= canopy_base && y <= canopy_base + 3) {
          int layer = y - canopy_base;
          int max_dist = 3 - layer;
          if (dist <= max_dist + 1 && dist > 0) {
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

      for (int ly = feature.y + 3; ly <= leaf_top; ly++) {
        int dist_from_top = leaf_top - ly;
        int max_radius = (dist_from_top / 2) + 1;
        if (y == ly && dist <= max_radius && dist > 0) {
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

      for (int ly = feature.y + 2; ly <= leaf_top; ly++) {
        int dist_from_top = leaf_top - ly;
        int max_radius = (dist_from_top / 2) + 1;
        if (y == ly && dist <= max_radius && dist > 0) {
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

      for (int ly = feature.y + 4; ly <= leaf_top; ly++) {
        int dist_from_top = leaf_top - ly;
        int max_radius = (dist_from_top / 2) + 1;
        if (y == ly && dist <= max_radius && dist > 0) {
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
    // Check for caves using surface-like noise for natural shapes
    if (isCaveSimple(x, y, z, height, anchor.biome)) {
      // Cave found - return air
      return B_air;
    }

    // Generate ore clumps using 3D noise for clustered veins
    uint8_t ore = getOreClumpAt(x, y, z, anchor.biome);
    if (ore != 0) return ore;
    
    // Deep bedrock layer
    if (y < 5) return B_bedrock;

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
    }
    
    // Water above the floor
    return B_water;
  }

  // For everything else, fall back to air
  return B_air;
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
  if (anchor.biome != W_swamp) {
    if (feature.x < 3 || feature.x > CHUNK_SIZE - 3) skip_feature = true;
    else if (feature.z < 3 || feature.z > CHUNK_SIZE - 3) skip_feature = true;
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

uint8_t getTerrainAt (int x, int y, int z, ChunkAnchor anchor) {

  if (y > 320) return B_air;

  int rx = x % CHUNK_SIZE;
  int rz = z % CHUNK_SIZE;
  if (rx < 0) rx += CHUNK_SIZE;
  if (rz < 0) rz += CHUNK_SIZE;

  ChunkFeature feature = getFeatureFromAnchor(anchor);
  uint8_t height = getHeightAt(x, z);

  return getTerrainAtFromCache(x, y, z, rx, rz, anchor, feature, height);

}

uint8_t getBlockAt (int x, int y, int z) {

  if (y < 0) return B_bedrock;

  uint8_t block_change = getBlockChange(x, y, z);
  if (block_change != 0xFF) return block_change;

  short anchor_x = div_floor(x, CHUNK_SIZE);
  short anchor_z = div_floor(z, CHUNK_SIZE);
  ChunkAnchor anchor = {
    .x = anchor_x,
    .z = anchor_z,
    .hash = getChunkHash(anchor_x, anchor_z),
    .biome = getChunkBiome(anchor_x, anchor_z)
  };

  return getTerrainAt(x, y, z, anchor);

}

uint8_t chunk_section[4096];
ChunkAnchor chunk_anchors[(16 / CHUNK_SIZE + 1) * (16 / CHUNK_SIZE + 1)];
ChunkFeature chunk_features[256 / (CHUNK_SIZE * CHUNK_SIZE)];
uint8_t chunk_section_height[16][16];

// Builds a 16x16x16 chunk of blocks and writes it to `chunk_section`
// Returns the biome at the origin corner of the chunk
uint8_t buildChunkSection (int cx, int cy, int cz) {
  // Precompute hashes, anchors and features for each relevant minichunk
  int anchor_index = 0, feature_index = 0;
  for (int i = cz; i < cz + 16 + CHUNK_SIZE; i += CHUNK_SIZE) {
    for (int j = cx; j < cx + 16 + CHUNK_SIZE; j += CHUNK_SIZE) {

      ChunkAnchor *anchor = chunk_anchors + anchor_index;

      anchor->x = j / CHUNK_SIZE;
      anchor->z = i / CHUNK_SIZE;
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

  // Apply block changes on top of terrain
  // This does mean that we're generating some terrain only to replace it,
  // but it's better to apply changes in one run rather than in individual
  // runs per block, as this is more expensive than terrain generation.
  for (int i = 0; i < block_changes_count; i ++) {
    if (block_changes[i].block == 0xFF) continue;
    // Skip blocks that behave better when sent using a block update
    if (block_changes[i].block == B_torch || isStairBlock(block_changes[i].block)) continue;
    #ifdef ALLOW_CHESTS
      if (block_changes[i].block == B_chest) continue;
    #endif
    if ( // Check if block is within this chunk section
      block_changes[i].x >= cx && block_changes[i].x < cx + 16 &&
      block_changes[i].y >= cy && block_changes[i].y < cy + 16 &&
      block_changes[i].z >= cz && block_changes[i].z < cz + 16
    ) {
      int dx = block_changes[i].x - cx;
      int dy = block_changes[i].y - cy;
      int dz = block_changes[i].z - cz;
      // Same 8-block sequence reversal as before, this time 10x dirtier
      // because we're working with specific indexes.
      unsigned address = (unsigned)(dx + (dz << 4) + (dy << 8));
      unsigned index = (address & ~7u) | (7u - (address & 7u));
      chunk_section[index] = block_changes[i].block;
    }
  }

  return chunk_anchors[0].biome;

}
