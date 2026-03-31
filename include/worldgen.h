#ifndef H_WORLDGEN
#define H_WORLDGEN

#include <stdint.h>
#include "perlin.h"

// Template-based world generation constants
#define TEMPLATE_RADIUS_CHUNKS 2       // 2 chunk radius = 5x5 chunk templates (fast generation)
#define TEMPLATE_DIAMETER_CHUNKS 5     // 2 * TEMPLATE_RADIUS + 1
#define TEMPLATES_PER_BIOME 3          // 3 template variants per biome
#define TEMPLATE_CACHE_SIZE 8          // Number of templates to keep in memory (LRU cache)
#define TEMPLATE_FILE_PATH "world_templates.dat"  // Single file for all templates

// Template enable toggle - set to 1 to enable, 0 to disable
// Templates provide faster runtime chunk generation but have ~2 min startup time
// For development, keep disabled. Enable for production after pre-generating templates.
#define USE_TEMPLATES 1

// Define macro for conditional compilation in other files
#if USE_TEMPLATES
#define USE_TEMPLATES_ENABLED
#endif

// Template file header - stored at beginning of each template file
typedef struct {
  uint8_t magic[4];                    // "BTPL" magic bytes
  uint8_t biome_id;
  uint8_t template_index;
  uint32_t seed_offset;
  uint16_t width_chunks;               // Always TEMPLATE_DIAMETER_CHUNKS
  uint16_t height_chunks;              // Always TEMPLATE_DIAMETER_CHUNKS
} TemplateFileHeader;

// In-memory template (loaded from file)
typedef struct {
  // Height map: one byte per block column (656x656 = 430KB)
  uint8_t* height_map;                 // Dynamically allocated
  uint8_t biome_id;
  uint8_t template_index;
  uint32_t seed_offset;
} BiomeTemplate;

// LRU cache entry for loaded templates
typedef struct {
  uint8_t biome_id;
  uint8_t template_index;
  uint8_t valid;
  uint8_t used;                        // LRU counter
  BiomeTemplate template;
} TemplateCacheEntry;

// Feature cache from template - stores pre-computed features for a chunk
typedef struct {
  uint8_t has_tree;
  uint8_t tree_type;
  uint8_t tree_x, tree_z;
  uint8_t tree_height;
} TemplateFeatures;

// Transition smoothing region
#define TRANSITION_SMOOTH_WIDTH 4      // Number of chunks to blend at template boundaries

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
uint8_t getHeightAtFromHash (int rx, int rz, int _x, int _z, uint32_t chunk_hash, uint8_t biome);
uint8_t getHeightAt (int x, int z);
uint8_t getTerrainAt (int x, int y, int z, ChunkAnchor anchor);
uint8_t getBlockAt (int x, int y, int z);

extern uint8_t chunk_section[4096];
uint8_t buildChunkSection (int cx, int cy, int cz);

// Template-based generation functions
void initTemplateSystem();
void generateAllBiomeTemplates();
void loadAllTemplates();
uint8_t buildChunkSectionFromTemplate(int cx, int cy, int cz);
BiomeTemplate* loadTemplate(uint8_t biome_id, uint8_t template_index);
void unloadTemplate(BiomeTemplate* tmpl);
uint8_t getHeightFromTemplate(int x, int z);
void getFeaturesFromTemplate(int cx, int cz, TemplateFeatures* features);

// Noise sampler externs for template generation
extern OctavePerlinNoiseSampler surface_noise;
extern OctavePerlinNoiseSampler detail_noise;
extern OctavePerlinNoiseSampler mountain_noise;

#endif
