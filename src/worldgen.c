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
    case meadow: return W_meadow;

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
    case W_plains: { // Generate oak trees and grass in plains
      
      // Don't generate trees underwater
      if (feature.y < 64) break;
      
      // Handle tree trunk and dirt
      if (x == feature.x && z == feature.z) {
        if (y == feature.y - 1) return B_dirt;
        if (y >= feature.y && y < feature.y + 4 + feature.variant) return B_oak_log;
      }
      
      // Get X/Z distance from center of tree
      uint8_t dx = x > feature.x ? x - feature.x : feature.x - x;
      uint8_t dz = z > feature.z ? z - feature.z : feature.z - z;
      uint8_t dist = dx + dz;
      
      // Oak canopy - starts partway up trunk, rounded shape
      uint8_t trunk_top = feature.y + 4 + feature.variant;
      // Leaves start 1 block below trunk top
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
      
      // Since we're sure that we're above sea level and in a plains biome,
      // there's no need to drop down to decide the surrounding blocks.
      if (y == height) return B_grass_block;
      
      // Flower and grass decorations
      if (y == height + 1) {
        uint8_t decor_hash = (anchor.hash >> (x + z)) & 255;
        
        // 20% chance for short grass
        if (decor_hash < 51) return B_short_grass;
        
        // 3% chance for flowers
        if (decor_hash >= 51 && decor_hash < 59) {
          uint8_t flower_type = (decor_hash >> 3) % 6;
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
      
      // Don't generate trees underwater
      if (feature.y < 64) break;
      
      // Use hash to decide between oak and birch
      uint8_t is_birch = (anchor.hash >> 3) & 1;
      
      if (x == feature.x && z == feature.z) {
        if (y == feature.y - 1) return B_dirt;
        // Birch trees are taller with distinctive white bark
        if (is_birch) {
          if (y >= feature.y && y < feature.y + 5 + feature.variant) return B_birch_log;
        } else {
          if (y >= feature.y && y < feature.y + 4 + feature.variant) return B_oak_log;
        }
      }
      
      // Get X/Z distance from center of tree
      uint8_t dx = x > feature.x ? x - feature.x : feature.x - x;
      uint8_t dz = z > feature.z ? z - feature.z : feature.z - z;
      uint8_t dist = dx + dz;
      
      // Generate leaf clusters
      if (is_birch) {
        // Birch leaves - sparse and spread out, typical birch shape
        uint8_t trunk_top = feature.y + 5 + feature.variant;
        // Leaves start partway up trunk
        if (y == trunk_top - 1 && dist <= 2) {
          if (dist == 2 && (dx == 2 || dz == 2)) break;
          return B_birch_leaves;
        }
        // Main canopy layers
        if (y == trunk_top && dist <= 2) {
          if (dx == 2 && dz == 2) break;
          return B_birch_leaves;
        }
        if (y == trunk_top + 1 && dist <= 2) {
          if (dx == 2 && dz == 2) break;
          return B_birch_leaves;
        }
        // Top layer
        if (y == trunk_top + 2 && dist <= 1) return B_birch_leaves;
        // Occasional lower leaves
        if (y == trunk_top - 2 && dist == 2 && (dx == 2 || dz == 2)) {
          if ((anchor.hash >> (y + x + z)) & 1) return B_birch_leaves;
        }
      } else {
        // Oak leaves - rounded canopy
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

      if (y == height) return B_grass_block;
      
      // Flower and grass decorations - forests have more vegetation
      if (y == height + 1) {
        uint8_t decor_hash = (anchor.hash >> (x + z)) & 255;
        
        // 25% chance for short grass
        if (decor_hash < 64) return B_short_grass;
        
        // 5% chance for flowers (forests have more flowers)
        if (decor_hash >= 64 && decor_hash < 77) {
          uint8_t flower_type = (decor_hash >> 3) % 8;
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
      
      // Don't generate trees underwater
      if (feature.y < 64) break;
      
      // Spruce trees are tall and conical
      if (x == feature.x && z == feature.z) {
        if (y == feature.y - 1) return B_dirt;
        if (y >= feature.y && y < feature.y + 6 + feature.variant * 2) return B_spruce_log;
      }
      
      // Get X/Z distance from center of tree
      uint8_t dx = x > feature.x ? x - feature.x : feature.x - x;
      uint8_t dz = z > feature.z ? z - feature.z : feature.z - z;
      uint8_t dist = dx + dz;
      
      // Conical spruce leaves - starts partway up the trunk
      uint8_t leaf_base = feature.y + 3;
      uint8_t leaf_top = feature.y + 9 + feature.variant * 2;
      
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

      if (y == height) return B_grass_block;
      
      // Taiga decorations - ferns and grass
      if (y == height + 1) {
        uint8_t decor_hash = (anchor.hash >> (x + z)) & 255;
        
        // 30% chance for ferns (taiga has lots of ferns)
        if (decor_hash < 77) return B_fern;
        
        // 15% chance for short grass
        if (decor_hash >= 77 && decor_hash < 115) return B_short_grass;
        
        // 2% chance for berries (bush)
        if (decor_hash >= 115 && decor_hash < 120) return B_bush;
      }
      
      return B_air;
    }

    case W_snowy_taiga: { // Generate spruce trees with snow in cold taiga
      
      // Don't generate trees underwater
      if (feature.y < 64) break;
      
      if (x == feature.x && z == feature.z) {
        if (y == feature.y - 1) return B_dirt;
        if (y >= feature.y && y < feature.y + 5 + feature.variant * 2) return B_spruce_log;
      }
      
      // Get X/Z distance from center of tree
      uint8_t dx = x > feature.x ? x - feature.x : feature.x - x;
      uint8_t dz = z > feature.z ? z - feature.z : feature.z - z;
      uint8_t dist = dx + dz;
      
      // Conical spruce leaves with snow
      uint8_t leaf_base = feature.y + 2;
      uint8_t leaf_top = feature.y + 8 + feature.variant * 2;
      
      for (int ly = leaf_base; ly <= leaf_top; ly++) {
        int dist_from_top = leaf_top - ly;
        int max_radius = (dist_from_top / 2) + 1;
        if (y == ly && dist <= max_radius && dist > 0) {
          return B_spruce_leaves;
        }
      }
      if (y == leaf_top + 1 && dist == 0) return B_spruce_leaves;

      if (y == height) return B_grass_block;
      
      // Snowy taiga decorations - ferns and grass stubs under snow
      if (y == height + 1) {
        uint8_t decor_hash = (anchor.hash >> (x + z)) & 255;
        
        // 20% chance for ferns
        if (decor_hash < 51) return B_fern;
        
        // 10% chance for short grass (appears under snow layer)
        if (decor_hash >= 51 && decor_hash < 77) return B_short_grass;
      }
      
      return B_air;
    }

    case W_jungle: // Generate jungle trees with vines
    case W_bamboo_jungle: {
      
      // Don't generate trees underwater
      if (feature.y < 64) break;
      
      // Jungle trees are very tall with 2x2 trunks
      int base_x = feature.x & ~1;
      int base_z = feature.z & ~1;
      int trunk_height = 10 + feature.variant * 4;
      
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
      uint8_t dist = (dx > dz ? dx : dz);  // Chebyshev distance for square shape
      
      // Large jungle leaf canopy - wide and flat
      uint8_t canopy_base = feature.y + trunk_height - 2;
      uint8_t canopy_top = feature.y + trunk_height + 2;
      
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

      if (y == height) return B_grass_block;
      
      // Jungle decorations - dense ferns and tall grass
      if (y == height + 1) {
        uint8_t decor_hash = (anchor.hash >> (x + z)) & 255;
        
        // 35% chance for ferns (jungles have lots of vegetation)
        if (decor_hash < 90) return B_fern;
        
        // 15% chance for tall grass
        if (decor_hash >= 90 && decor_hash < 128) return B_short_grass;
        
        // 3% chance for melon stems (bush as placeholder)
        if (decor_hash >= 128 && decor_hash < 136) return B_bush;
      }
      
      return B_air;
    }

    case W_savanna: { // Generate acacia trees in savanna
      
      // Don't generate trees underwater
      if (feature.y < 64) break;
      
      // Acacia trees have a distinctive curved shape
      int trunk_height = 4 + feature.variant;
      
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
      
      // Acacia leaf canopy (wide, flat umbrella shape)
      uint8_t canopy_y = feature.y + trunk_height + 2;
      if (y >= canopy_y && y <= canopy_y + 1) {
        if (dist <= 3) {
          if (y == canopy_y && dist == 3) break;
          return B_acacia_leaves;
        }
      }
      // Top layer
      if (y == canopy_y + 2 && dist <= 2) return B_acacia_leaves;

      if (y == height) return B_grass_block;
      
      // Savanna decorations - dry grass and sparse flowers
      if (y == height + 1) {
        uint8_t decor_hash = (anchor.hash >> (x + z)) & 255;
        
        // 25% chance for tall dry grass
        if (decor_hash < 64) return B_tall_dry_grass;
        
        // 10% chance for short dry grass
        if (decor_hash >= 64 && decor_hash < 90) return B_short_dry_grass;
        
        // 2% chance for dandelions (hardy flowers)
        if (decor_hash >= 90 && decor_hash < 95) return B_dandelion;
      }
      
      return B_air;
    }

    case W_dark_forest: { // Generate dark oak trees (2x2 trunks)
      
      // Don't generate trees underwater
      if (feature.y < 64) break;
      
      // Dark oak trees have 2x2 trunks
      int base_x = feature.x & ~1;  // Round down to even
      int base_z = feature.z & ~1;
      int trunk_height = 6 + feature.variant;
      
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
      
      // Large dark oak canopy - dome shape
      uint8_t canopy_base = feature.y + trunk_height;
      if (y >= canopy_base && y <= canopy_base + 3) {
        int layer = y - canopy_base;
        int max_dist = 3 - layer;
        if (dist <= max_dist + 1 && dist > 0) {
          return B_dark_oak_leaves;
        }
      }
      // Top of canopy
      if (y == canopy_base + 4 && dist <= 1) return B_dark_oak_leaves;
      
      if (y == height) return B_grass_block;
      return B_air;
    }
    
    case W_birch_forest: { // Generate only birch trees
      
      // Don't generate trees underwater
      if (feature.y < 64) break;
      
      if (x == feature.x && z == feature.z) {
        if (y == feature.y - 1) return B_dirt;
        if (y >= feature.y && y < feature.y + 5 + feature.variant) return B_birch_log;
      }
      
      // Get X/Z distance from center of tree
      uint8_t dx = x > feature.x ? x - feature.x : feature.x - x;
      uint8_t dz = z > feature.z ? z - feature.z : feature.z - z;
      uint8_t dist = dx + dz;
      
      // Birch leaves - sparse and spread out
      uint8_t trunk_top = feature.y + 5 + feature.variant;
      // Leaves start partway up trunk
      if (y == trunk_top - 1 && dist <= 2) {
        if (dist == 2 && (dx == 2 || dz == 2)) break;
        return B_birch_leaves;
      }
      // Main canopy layers
      if (y == trunk_top && dist <= 2) {
        if (dx == 2 && dz == 2) break;
        return B_birch_leaves;
      }
      if (y == trunk_top + 1 && dist <= 2) {
        if (dx == 2 && dz == 2) break;
        return B_birch_leaves;
      }
      // Top layer
      if (y == trunk_top + 2 && dist <= 1) return B_birch_leaves;
      // Occasional lower leaves
      if (y == trunk_top - 2 && dist == 2 && (dx == 2 || dz == 2)) {
        if ((anchor.hash >> (y + x + z)) & 1) return B_birch_leaves;
      }
      
      if (y == height) return B_grass_block;
      return B_air;
    }
    
    case W_cherry_grove: { // Generate cherry blossom trees
      
      // Don't generate trees underwater
      if (feature.y < 64) break;
      
      if (x == feature.x && z == feature.z) {
        if (y == feature.y - 1) return B_dirt;
        if (y >= feature.y && y < feature.y + 3 + feature.variant) return B_cherry_log;
      }
      
      // Get X/Z distance from center of tree
      uint8_t dx = x > feature.x ? x - feature.x : feature.x - x;
      uint8_t dz = z > feature.z ? z - feature.z : feature.z - z;
      uint8_t dist = dx + dz;
      
      // Cherry blossom canopy (wide, flat, with hanging edges)
      uint8_t trunk_top = feature.y + 3 + feature.variant;
      // Leaves start partway up trunk
      if (y == trunk_top - 1 && dist <= 3) {
        if (dist == 3 && (dx == 3 || dz == 3)) break;
        return B_cherry_leaves;
      }
      // Main canopy - wide and flat
      if (y == trunk_top && dist <= 3) {
        if (dist == 3) break;
        return B_cherry_leaves;
      }
      if (y == trunk_top + 1 && dist <= 2) return B_cherry_leaves;
      // Top layer
      if (y == trunk_top + 2 && dist <= 2) return B_cherry_leaves;
      // Hanging edges (petals falling)
      if (y == trunk_top - 2 && dist == 3) {
        if ((anchor.hash >> (x + z)) & 3) return B_cherry_leaves;
      }

      if (y == height) return B_grass_block;
      
      // Cherry grove decorations - grass and pink flowers
      if (y == height + 1) {
        uint8_t decor_hash = (anchor.hash >> (x + z)) & 255;
        
        // 25% chance for short grass
        if (decor_hash < 64) return B_short_grass;
        
        // 8% chance for pink flowers (tulips)
        if (decor_hash >= 64 && decor_hash < 84) {
          uint8_t flower_type = (decor_hash >> 3) % 3;
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

    default: break;
  }

  // Handle surface-level terrain (the very topmost blocks)
  if (height >= 63) {
    if (y == height) {
      if (anchor.biome == W_mangrove_swamp) return B_mud;
      if (anchor.biome == W_desert) return B_sand;
      if (anchor.biome == W_beach) return B_sand;
      return B_grass_block;
    }
    // Snow layer on top of grass in snowy biomes
    if (anchor.biome == W_snowy_plains || anchor.biome == W_snowy_taiga) {
      if (y == height + 1) return B_snow;
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
