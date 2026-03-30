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
static OctavePerlinNoiseSampler surface_noise;
static OctavePerlinNoiseSampler detail_noise;
static int gen_initialized = 0;

void init_worldgen() {
    if (gen_initialized) return;
    setupGenerator(&g, MC_1_20, 0);
    applySeed(&g, DIM_OVERWORLD, (uint64_t)world_seed);
    
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
  // Cubiomes uses block coordinates for getBiomeAt if scale is 1
  int biomeID = getBiomeAt(&g, 1, x * 16 + 8, 64, z * 16 + 8);
  
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

// Get terrain height at the given coordinates
// Does *not* account for block changes
uint8_t getHeightAt (int x, int z) {
  init_worldgen();
  
  double noise = octave_sample(&surface_noise, x * 0.0125, 0, z * 0.0125);
  double detail = octave_sample(&detail_noise, x * 0.05, 0, z * 0.05);
  
  uint8_t biome = getChunkBiome(div_floor(x, 16), div_floor(z, 16));
  
  double base_height = 64.0;
  double scale = 12.0;
  
  // Simple height heuristics per biome group
  if (biome == W_desert) {
      scale = 8.0;
  } else if (biome == W_snowy_plains || biome == W_snowy_taiga) {
      scale = 15.0;
  } else if (biome == W_savanna || biome == W_badlands || biome == W_windswept_hills) {
      base_height = 75.0;
      scale = 25.0;
  } else if (biome == W_stony_peaks || biome == W_jagged_peaks || biome == W_frozen_peaks) {
      base_height = 100.0;
      scale = 40.0;
  } else if (biome == W_cherry_grove) {
      base_height = 80.0;
      scale = 15.0;
  } else if (biome == W_ocean || biome == W_deep_ocean || biome == W_frozen_ocean) {
      base_height = 50.0;
      scale = 10.0;
  } else if (biome == W_river || biome == W_frozen_river) {
      base_height = 60.0;
      scale = 2.0;
  } else if (biome == W_beach) {
      base_height = 62.0;
      scale = 2.0;
  } else if (biome == W_mangrove_swamp || biome == W_swamp) {
      base_height = 62.0;
      scale = 3.0;
  }
  
  return (uint8_t)(base_height + noise * scale + detail * 2.0);
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
