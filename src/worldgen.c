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
static OctavePerlinNoiseSampler surface_noise;
static OctavePerlinNoiseSampler detail_noise;
static int gen_initialized = 0;

// Height cache for smooth chunk generation (covers 32x32 area for seamless borders)
static int cache_cx = 0x7FFFFFFF, cache_cz = 0x7FFFFFFF;
static uint8_t height_cache[36][36];  // 32x32 + 2 block border on each side

void init_worldgen() {
    if (gen_initialized) return;
    setupGenerator(&g, MC_1_20, 0);
    applySeed(&g, DIM_OVERWORLD, (uint64_t)world_seed);
    initSurfaceNoise(&surface_noise_biome, DIM_OVERWORLD, (uint64_t)world_seed);

    octave_init(&surface_noise, (uint64_t)world_seed, 4);
    octave_init(&detail_noise, (uint64_t)world_seed + 1, 2);
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
  switch (biomeID) {
    case ocean: return W_ocean;
    case plains: return W_plains;
    case sunflower_plains: return W_plains;
    case desert: return W_desert;
    case desert_lakes: return W_desert;
    case windswept_hills: return W_windswept_hills;
    case forest: return W_forest;
    case flower_forest: return W_forest;
    case taiga: return W_taiga;
    case taiga_mountains: return W_taiga;
    case swamp: return W_swamp;
    case swamp_hills: return W_swamp;
    case river: return W_river;
    case frozen_ocean: return W_frozen_ocean;
    case frozen_river: return W_frozen_river;
    case ice_spikes: return W_snowy_plains;
    case beach: return W_beach;
    case desertHills: return W_desert;
    case forestHills: return W_forest;
    case taigaHills: return W_taiga;
    case jungle: return W_jungle;

    case modified_jungle: return W_jungle;
    case deep_ocean: return W_deep_ocean;
    case birch_forest: return W_birch_forest;
    case tall_birch_forest: return W_birch_forest;
    case dark_forest: return W_dark_forest;
    case snowy_taiga: return W_snowy_taiga;
    case savanna: return W_savanna;
    case shattered_savanna: return W_savanna;
    case badlands: return W_badlands;
    case eroded_badlands: return W_badlands;
    case mangrove_swamp: return W_mangrove_swamp;
    
    default:
        // Use isSnowy helper from cubiomes
        if (isSnowy(biomeID)) return W_snowy_plains;
        if (isDeepOcean(biomeID)) return W_deep_ocean;
        if (isOceanic(biomeID)) return W_ocean;
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

  // Sample biomes on a fixed 16-block grid for consistent blending
  int gx = div_floor(x, 16);  // Grid cell X
  int gz = div_floor(z, 16);  // Grid cell Z
  int lx = mod_abs(x, 16);    // Local position in cell (0-15)
  int lz = mod_abs(z, 16);
  
  float wx = lx / 16.0f;
  float wz = lz / 16.0f;
  
  // Sample biomes at 4 grid corners
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
    if (biome == W_desert) { base = 66.0; scale = 8.0; } \
    else if (biome == W_snowy_plains || biome == W_snowy_taiga) { base = 68.0; scale = 15.0; } \
    else if (biome == W_savanna || biome == W_badlands || biome == W_windswept_hills) { base = 75.0; scale = 25.0; } \
    else if (biome == W_stony_peaks || biome == W_jagged_peaks || biome == W_frozen_peaks) { base = 100.0; scale = 40.0; } \
    else if (biome == W_cherry_grove) { base = 80.0; scale = 15.0; } \
    else if (biome == W_ocean || biome == W_deep_ocean || biome == W_frozen_ocean) { base = 50.0; scale = 10.0; } \
    else if (biome == W_river || biome == W_frozen_river) { base = 60.0; scale = 2.0; } \
    else if (biome == W_beach) { base = 62.0; scale = 2.0; } \
    else if (biome == W_mangrove_swamp || biome == W_swamp) { base = 62.0; scale = 3.0; }
  
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
  for (int dz = 0; dz < 36; dz++) {
    for (int dx = 0; dx < 36; dx++) {
      height_cache[dx][dz] = getHeightAtRaw(base_x + dx - 2, base_z + dz - 2);
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


uint8_t getTerrainAtFromCache (int x, int y, int z, int rx, int rz, ChunkAnchor anchor, ChunkFeature feature, uint8_t height) {

  if (y >= 64 && y >= height && feature.y != 255) switch (anchor.biome) {
    case W_plains: { // Generate trees in the plains biome

      // Don't generate trees underwater
      if (feature.y < 64) break;

      // Handle tree stem and the dirt under it
      if (x == feature.x && z == feature.z) {
        if (y == feature.y - 1) return B_dirt;
        if (y >= feature.y && y < feature.y - feature.variant + 6) return B_oak_log;
      }

      // Get X/Z distance from center of tree
      uint8_t dx = x > feature.x ? x - feature.x : feature.x - x;
      uint8_t dz = z > feature.z ? z - feature.z : feature.z - z;

      // Generate leaf clusters
      if (dx < 3 && dz < 3 && y > feature.y - feature.variant + 2 && y < feature.y - feature.variant + 5) {
        if (y == feature.y - feature.variant + 4 && dx == 2 && dz == 2) break;
        return B_oak_leaves;
      }
      if (dx < 2 && dz < 2 && y >= feature.y - feature.variant + 5 && y <= feature.y - feature.variant + 6) {
        if (y == feature.y - feature.variant + 6 && dx == 1 && dz == 1) break;
        return B_oak_leaves;
      }

      // Since we're sure that we're above sea level and in a plains biome,
      // there's no need to drop down to decide the surrounding blocks.
      if (y == height) return B_grass_block;
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

    case W_snowy_plains: { // Generate grass stubs in snowy plains

      if (x == feature.x && z == feature.z && y == height + 1 && height >= 64) {
        return B_short_grass;
      }

      break;
    }

    default: break;
  }

  // Handle surface-level terrain (the very topmost blocks)
  if (height >= 63) {
    if (y == height) {
      if (anchor.biome == W_mangrove_swamp) return B_mud;
      if (anchor.biome == W_snowy_plains) return B_snowy_grass_block;
      if (anchor.biome == W_desert) return B_sand;
      if (anchor.biome == W_beach) return B_sand;
      return B_grass_block;
    }
    if (anchor.biome == W_snowy_plains && y == height + 1) {
      return B_snow;
    }
  }
  // Starting at 4 blocks below terrain level, generate minerals and caves
  if (y <= height - 4) {
    // Caves use the same shape as surface terrain, just mirrored
    int8_t gap = height - TERRAIN_BASE_HEIGHT;
    if (y < CAVE_BASE_DEPTH + gap && y > CAVE_BASE_DEPTH - gap) return B_air;

    // The chunk-relative X and Z coordinates are used as the seed for an
    // xorshift RNG/hash function to generate the Y coordinate of the ore
    // in this column. This way, each column is guaranteed to have exactly
    // one ore candidate, as there will always be a Y value to reference.
    uint8_t ore_y = ((rx & 15) << 4) + (rz & 15);
    ore_y ^= ore_y << 4;
    ore_y ^= ore_y >> 5;
    ore_y ^= ore_y << 1;
    ore_y &= 63;

    if (y == ore_y) {
      // Since the ore Y coordinate is effectely a random number in range [0;64),
      // we use it in a bit shift with the chunk's anchor hash to get another
      // pseudo-random number for the ore's rarity.
      uint8_t ore_probability = (anchor.hash >> (ore_y % 24)) & 255;
      // Ore placement is determined by Y level and "probability"
      if (y < 15) {
        if (ore_probability < 10) return B_diamond_ore;
        if (ore_probability < 12) return B_gold_ore;
        if (ore_probability < 15) return B_redstone_ore;
      }
      if (y < 30) {
        if (ore_probability < 3) return B_gold_ore;
        if (ore_probability < 8) return B_redstone_ore;
      }
      if (y < 54) {
        if (ore_probability < 30) return B_iron_ore;
        if (ore_probability < 40) return B_copper_ore;
      }
      if (ore_probability < 60) return B_coal_ore;
      if (y < 5) return B_lava;
      return B_cobblestone;
    }

    // For everything else, fall back to stone
    return B_stone;
  }
  // Handle the space between stone and grass
  if (y <= height) {
    if (anchor.biome == W_desert) return B_sandstone;
    if (anchor.biome == W_mangrove_swamp) return B_mud;
    if (anchor.biome == W_beach && height > 64) return B_sandstone;
    return B_dirt;
  }
  // If all else failed, but we're below sea level, generate water (or ice)
  if (y == 63 && anchor.biome == W_snowy_plains) return B_ice;
  if (y < 64) return B_water;

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
  if (anchor.biome != W_mangrove_swamp) {
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

  if (y > 150) return B_air;

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

  // Generate 4096 blocks in one buffer to reduce overhead
  for (int j = 0; j < 4096; j += 8) {
    // These values don't change in the lower array,
    // since all of the operations are on multiples of 8
    int y = j / 256 + cy;
    int rz = j / 16 % 16;
    int rz_mod = rz % CHUNK_SIZE;
    feature_index = (j % 16) / CHUNK_SIZE + (j / 16 % 16) / CHUNK_SIZE * (16 / CHUNK_SIZE);
    anchor_index = (j % 16) / CHUNK_SIZE + (j / 16 % 16) / CHUNK_SIZE * (16 / CHUNK_SIZE + 1);
    // The client expects "big-endian longs", which in our
    // case means reversing the order in which we store/send
    // each 8 block sequence.
    for (int offset = 7; offset >= 0; offset--) {
      int k = j + offset;
      int rx = k % 16;
      // Combine all of the cached data to retrieve the block
      chunk_section[j + 7 - offset] = getTerrainAtFromCache(
        rx + cx, y, rz + cz,
        rx % CHUNK_SIZE, rz_mod,
        chunk_anchors[anchor_index],
        chunk_features[feature_index],
        chunk_section_height[rx][rz]
      );
    }
  }

  // Apply block changes on top of terrain
  // This does mean that we're generating some terrain only to replace it,
  // but it's better to apply changes in one run rather than in individual
  // runs per block, as this is more expensive than terrain generation.
  for (int i = 0; i < block_changes_count; i ++) {
    if (block_changes[i].block == 0xFF) continue;
    // Skip blocks that behave better when sent using a block update
    if (block_changes[i].block == B_torch) continue;
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
