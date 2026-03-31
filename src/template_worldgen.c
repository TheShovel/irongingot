#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>

#include "globals.h"
#include "worldgen.h"
#include "perlin.h"
#include "generator.h"
#include "registries.h"

// LRU template cache
static TemplateCacheEntry template_cache[TEMPLATE_CACHE_SIZE];
static uint8_t cache_initialized = 0;
static uint8_t lru_counter = 0;

// All templates loaded in memory (faster than individual files)
static uint8_t* all_templates_data = NULL;
static uint8_t templates_loaded = 0;

// External declarations from worldgen.c
extern uint32_t world_seed;
extern int block_changes_count;
extern uint8_t getBlockChange(int x, int y, int z);

// Get biome height parameters (matches worldgen.c exactly)
static void getBiomeHeightParams(uint8_t biome, double* base, double* scale) {
    *base = 64.0;
    *scale = 12.0;
    
    switch (biome) {
        case 2:  // W_desert
            *base = 66.0; *scale = 8.0; break;
        case 10: // W_snowy_plains
            *base = 70.0; *scale = 20.0; break;
        case 17: // W_savanna
            *base = 80.0; *scale = 40.0; break;
        case 29: // W_windswept_savanna
            *base = 85.0; *scale = 50.0; break;
        case 3:  // W_windswept_hills
        case 28: // W_windswept_forest
            *base = 95.0; *scale = 70.0; break;
        case 22: // W_jagged_peaks
            *base = 120.0; *scale = 110.0; break;
        case 23: // W_frozen_peaks
            *base = 110.0; *scale = 95.0; break;
        case 21: // W_stony_peaks
            *base = 115.0; *scale = 100.0; break;
        case 30: // W_snowy_slopes
            *base = 105.0; *scale = 90.0; break;
        case 31: // W_grove
            *base = 90.0; *scale = 60.0; break;
        case 25: // W_cherry_grove
            *base = 85.0; *scale = 35.0; break;
        case 0:  // W_ocean
        case 19: // W_deep_ocean
        case 8:  // W_frozen_ocean
            *base = 45.0; *scale = 12.0; break;
        case 7:  // W_river
        case 9:  // W_frozen_river
            *base = 58.0; *scale = 3.0; break;
        case 12: // W_beach
            *base = 60.0; *scale = 3.0; break;
        case 20: // W_mangrove_swamp
        case 6:  // W_swamp
            *base = 61.0; *scale = 4.0; break;
        case 24: // W_meadow
            *base = 90.0; *scale = 45.0; break;
        case 4:  // W_forest
        case 14: // W_birch_forest
        case 15: // W_dark_forest
            *base = 70.0; *scale = 18.0; break;
        case 33: // W_flower_forest
            *base = 75.0; *scale = 25.0; break;
        case 5:  // W_taiga
            *base = 72.0; *scale = 22.0; break;
        case 16: // W_snowy_taiga
            *base = 72.0; *scale = 22.0; break;
        case 26: // W_old_growth_pine_taiga
            *base = 72.0; *scale = 22.0; break;
        case 13: // W_jungle
        case 27: // W_bamboo_jungle
            *base = 75.0; *scale = 20.0; break;
        case 1:  // W_plains
        case 32: // W_sunflower_plains
            *base = 68.0; *scale = 12.0; break;
        case 18: // W_badlands
        case 35: // W_eroded_badlands
            *base = 80.0; *scale = 40.0; break;
        case 34: // W_ice_spikes
            *base = 65.0; *scale = 15.0; break;
        case 11: // W_mushroom_fields
            *base = 65.0; *scale = 10.0; break;
    }
}

// Check if biome is a mountain type
static int isMountainBiome(uint8_t biome) {
    return (biome == 3 || biome == 22 || biome == 23 || biome == 21 ||
            biome == 30 || biome == 31 || biome == 28 || biome == 29);
}

// Fast batch height generation - matches getHeightAtRaw exactly
static void generateHeightsBatch(uint8_t* heights, uint8_t* biomes, int base_x, int base_z, int width, int height) {
    for (int z = 0; z < height; z++) {
        int world_z = base_z + z;
        for (int x = 0; x < width; x++) {
            int world_x = base_x + x;
            
            // Sample surface noise (matches worldgen.c)
            double noise = octave_sample(&surface_noise, world_x * 0.0125, 0, world_z * 0.0125);
            double detail = octave_sample(&detail_noise, world_x * 0.05, 0, world_z * 0.05);
            
            // Get biome at this position
            int chunk_x = world_x / 16;
            int chunk_z = world_z / 16;
            uint8_t biome = getChunkBiome(chunk_x, chunk_z);
            if (biomes) biomes[z * width + x] = biome;
            
            // Get base height and scale for this biome
            double base_height, scale;
            getBiomeHeightParams(biome, &base_height, &scale);
            
            // Check if this is a mountain biome
            if (isMountainBiome(biome)) {
                // Sample mountain noise for large peak shapes
                double mountain = octave_sample(&mountain_noise, world_x * 0.005, 0, world_z * 0.005);
                
                // Normalize mountain noise to [0, 1]
                double mountain_factor = (mountain + 1.0) / 2.0;
                
                // Mountain peaks with quadratic scaling
                double peak_bonus = mountain_factor * mountain_factor * 100.0;
                
                // Add detail variation scaled by mountain factor
                base_height += peak_bonus;
                scale += mountain_factor * 25.0;
                
                // Extra detail for rugged mountain surfaces
                base_height += detail * (5.0 + mountain_factor * 10.0);
            }
            
            // Calculate final height
            double h = base_height + noise * scale + detail * 2.0;
            heights[z * width + x] = (uint8_t)h;
        }
    }
}
static uint8_t getHeightAtRaw(int x, int z);
static void generateTemplateHeightMap(BiomeTemplate* tmpl, int base_seed_x, int base_seed_z);

// Initialize the template system
void initTemplateSystem() {
    if (cache_initialized) return;
    
    memset(template_cache, 0, sizeof(template_cache));
    cache_initialized = 1;
    lru_counter = 0;
    all_templates_data = NULL;
    templates_loaded = 0;
}

// Generate height map for a template using the existing noise functions
static void generateTemplateHeightMap(BiomeTemplate* tmpl, int base_seed_x, int base_seed_z) {
    // Template covers area from (base_seed_x - TEMPLATE_RADIUS_CHUNKS*16) to
    // (base_seed_x + TEMPLATE_RADIUS_CHUNKS*16) in both X and Z

    int base_x = base_seed_x - TEMPLATE_RADIUS_CHUNKS * 16;
    int base_z = base_seed_z - TEMPLATE_RADIUS_CHUNKS * 16;
    int size = TEMPLATE_DIAMETER_CHUNKS * 16;  // 80 blocks for 5 chunks

    // Use fast batch generation that matches original terrain
    generateHeightsBatch(tmpl->height_map, NULL, base_x, base_z, size, size);
}

// Template file header - stored at beginning of template archive
typedef struct {
    uint8_t magic[4];                    // "BTPL" magic bytes
    uint16_t total_templates;            // Total number of templates
    uint16_t template_size;              // Size of each template data in bytes
} TemplateArchiveHeader;

// Get template data pointer from archive
static uint8_t* getTemplateData(uint8_t biome_id, uint8_t template_index) {
    if (!templates_loaded || !all_templates_data) return NULL;
    
    int idx = biome_id * TEMPLATES_PER_BIOME + template_index;
    if (idx >= 49 * TEMPLATES_PER_BIOME) return NULL;
    
    // Skip archive header, then calculate offset
    size_t header_size = sizeof(TemplateArchiveHeader);
    size_t template_data_size = TEMPLATE_DIAMETER_CHUNKS * 16 * TEMPLATE_DIAMETER_CHUNKS * 16;
    
    return all_templates_data + header_size + (idx * template_data_size);
}

// Generate and save all templates to a single archive file
static void generateAndSaveAllTemplates() {
    // Check if archive already exists
    FILE* f = fopen(TEMPLATE_FILE_PATH, "rb");
    if (f) {
        fclose(f);
        return;  // Already generated
    }
    
    int total = 49 * TEMPLATES_PER_BIOME;
    size_t template_data_size = TEMPLATE_DIAMETER_CHUNKS * 16 * TEMPLATE_DIAMETER_CHUNKS * 16;
    size_t total_data_size = total * template_data_size;
    
    // Allocate buffer for all templates
    uint8_t* all_data = (uint8_t*)malloc(total_data_size);
    if (!all_data) {
        fprintf(stderr, "Failed to allocate memory for templates\n");
        return;
    }
    
    fprintf(stderr, "Pre-generating biome templates...\n");
    fprintf(stderr, "Templates: %d biomes × %d variants = %d total\n", 
           49, TEMPLATES_PER_BIOME, total);
    
    // Generate all templates
    for (uint8_t biome_id = 0; biome_id < 49; biome_id++) {
        for (uint8_t template_idx = 0; template_idx < TEMPLATES_PER_BIOME; template_idx++) {
            uint32_t seed_offset = (biome_id * 1000) + (template_idx * 173);
            int center_x = (seed_offset & 0xFF) * 16;
            int center_z = ((seed_offset >> 8) & 0xFF) * 16;
            
            uint8_t* tmpl_data = getTemplateData(biome_id, template_idx);
            if (tmpl_data) {
                generateHeightsBatch(tmpl_data, NULL, center_x, center_z,
                                    TEMPLATE_DIAMETER_CHUNKS * 16,
                                    TEMPLATE_DIAMETER_CHUNKS * 16);
            }
            
            int generated = biome_id * TEMPLATES_PER_BIOME + template_idx + 1;
            if (generated % 30 == 0) {
                fprintf(stderr, "Progress: %d/%d templates (%d%%)\n", 
                       generated, total, generated * 100 / total);
            }
        }
    }
    
    // Write archive file
    f = fopen(TEMPLATE_FILE_PATH, "wb");
    if (!f) {
        fprintf(stderr, "Failed to create template archive\n");
        free(all_data);
        return;
    }
    
    TemplateArchiveHeader header;
    memcpy(header.magic, "BTPL", 4);
    header.total_templates = total;
    header.template_size = template_data_size;
    
    fwrite(&header, sizeof(header), 1, f);
    fwrite(all_data, 1, total_data_size, f);
    fclose(f);
    free(all_data);
    
    fprintf(stderr, "Template generation complete! (%d templates)\n", total);
}

// Generate all biome templates (called once at server startup)
void generateAllBiomeTemplates() {
    initTemplateSystem();
    generateAndSaveAllTemplates();
    loadAllTemplates();
}

// Load all templates from archive into memory
void loadAllTemplates() {
    if (templates_loaded) return;
    
    FILE* f = fopen(TEMPLATE_FILE_PATH, "rb");
    if (!f) {
        fprintf(stderr, "Warning: Template archive not found\n");
        return;
    }
    
    // Read and verify header
    TemplateArchiveHeader header;
    if (fread(&header, sizeof(header), 1, f) != 1) {
        fclose(f);
        return;
    }
    
    if (memcmp(header.magic, "BTPL", 4) != 0) {
        fprintf(stderr, "Invalid template archive\n");
        fclose(f);
        return;
    }
    
    // Load all template data
    size_t total_size = header.total_templates * header.template_size;
    all_templates_data = (uint8_t*)malloc(total_size);
    
    if (!all_templates_data) {
        fclose(f);
        return;
    }
    
    if (fread(all_templates_data, 1, total_size, f) != total_size) {
        free(all_templates_data);
        all_templates_data = NULL;
        fclose(f);
        return;
    }
    
    fclose(f);
    templates_loaded = 1;
    
    fprintf(stderr, "Loaded %d templates into memory\n", header.total_templates);
}

// Load a template from the in-memory archive
BiomeTemplate* loadTemplate(uint8_t biome_id, uint8_t template_index) {
    initTemplateSystem();
    
    // Check cache first
    for (int i = 0; i < TEMPLATE_CACHE_SIZE; i++) {
        if (template_cache[i].valid && 
            template_cache[i].biome_id == biome_id && 
            template_cache[i].template_index == template_index) {
            template_cache[i].used = lru_counter++;
            return &template_cache[i].template;
        }
    }
    
    // Get data from in-memory archive
    uint8_t* tmpl_data = getTemplateData(biome_id, template_index);
    if (!tmpl_data) {
        return NULL;
    }
    
    // Find LRU cache slot
    int lru_index = 0;
    uint8_t min_used = template_cache[0].used;
    for (int i = 1; i < TEMPLATE_CACHE_SIZE; i++) {
        if (!template_cache[i].valid || template_cache[i].used < min_used) {
            min_used = template_cache[i].used;
            lru_index = i;
        }
    }
    
    // Unload existing template if present (just mark as invalid, data stays in archive)
    template_cache[lru_index].valid = 0;
    
    // Set up cache entry pointing to archive data
    BiomeTemplate* tmpl = &template_cache[lru_index].template;
    tmpl->height_map = tmpl_data;  // Point to archive data, don't allocate
    tmpl->biome_id = biome_id;
    tmpl->template_index = template_index;
    tmpl->seed_offset = 0;
    
    template_cache[lru_index].biome_id = biome_id;
    template_cache[lru_index].template_index = template_index;
    template_cache[lru_index].valid = 1;
    template_cache[lru_index].used = lru_counter++;
    
    return tmpl;
}

// Unload a template (free memory)
void unloadTemplate(BiomeTemplate* tmpl) {
    if (tmpl && tmpl->height_map) {
        free(tmpl->height_map);
        tmpl->height_map = NULL;
    }
}

// Get height from template at world coordinates
uint8_t getHeightFromTemplate(int x, int z) {
    // Determine which template covers this position
    // Templates are centered at multiples of TEMPLATE_DIAMETER_CHUNKS * 16
    int template_size = TEMPLATE_DIAMETER_CHUNKS * 16;  // 272 blocks for 17 chunks
    int half_size = template_size / 2;  // 136 blocks
    
    // Find the template center that would cover this position
    // Round to nearest template center
    int template_cx = ((x + half_size) / template_size) * template_size;
    int template_cz = ((z + half_size) / template_size) * template_size;
    int template_base_x = template_cx - half_size;
    int template_base_z = template_cz - half_size;
    
    // Get the biome at this location to select the right template
    int chunk_x = x / 16;
    int chunk_z = z / 16;
    uint8_t biome = getChunkBiome(chunk_x, chunk_z);
    
    // Use template index based on position hash for consistency
    uint32_t hash = getChunkHash(chunk_x, chunk_z);
    uint8_t template_idx = hash % TEMPLATES_PER_BIOME;
    
    BiomeTemplate* tmpl = loadTemplate(biome, template_idx);
    if (!tmpl || !tmpl->height_map) {
        // Fallback to procedural generation - return 0 to signal caller
        return 0;
    }
    
    // Calculate position within template
    int local_x = x - template_base_x;
    int local_z = z - template_base_z;
    
    if (local_x < 0 || local_x >= template_size || local_z < 0 || local_z >= template_size) {
        // Position outside template bounds
        return 0;
    }
    
    uint8_t height = tmpl->height_map[local_z * template_size + local_x];
    
    // Sanity check - height should be reasonable
    if (height < 1 || height > 250) {
        return 0;
    }
    
    return height;
}

// Get features for a chunk from template
void getFeaturesFromTemplate(int cx, int cz, TemplateFeatures* features) {
    // Features are still generated procedurally based on chunk hash
    // This maintains variety while using template height maps
    uint32_t hash = getChunkHash(cx, cz);
    uint8_t biome = getChunkBiome(cx, cz);
    
    features->has_tree = 0;
    features->tree_type = 0;
    features->tree_x = 0;
    features->tree_z = 0;
    features->tree_height = 0;
    
    // Simple tree placement logic based on biome and hash
    if (biome == W_forest || biome == W_plains || biome == W_taiga) {
        if ((hash & 0x0F) > 3) {  // 75% chance for tree in suitable biomes
            features->has_tree = 1;
            features->tree_x = hash % 12 + 2;  // Position 2-13 within chunk
            features->tree_z = (hash >> 8) % 12 + 2;
            
            if (biome == W_taiga) {
                features->tree_type = 2;  // Spruce
                features->tree_height = 6 + (hash >> 4) % 4;
            } else if (biome == W_forest && (hash & 1)) {
                features->tree_type = 1;  // Birch
                features->tree_height = 5 + (hash >> 4) % 3;
            } else {
                features->tree_type = 0;  // Oak
                features->tree_height = 4 + (hash >> 4) % 2;
            }
        }
    }
}

// Smooth transition between two height values
static uint8_t smoothHeight(uint8_t h1, uint8_t h2, float t) {
    // Smoothstep interpolation for smoother transitions
    t = t * t * (3.0f - 2.0f * t);
    return (uint8_t)(h1 * (1.0f - t) + h2 * t);
}

// Build a chunk section using template data - uses original terrain generation
uint8_t buildChunkSectionFromTemplate(int cx, int cy, int cz) {
    // Just call the original buildChunkSection logic
    // The template heights are used via getHeightFromTemplate which is called
    // by getHeightAt when USE_TEMPLATES is enabled
    
    // Precompute hashes, anchors and features for each relevant minichunk
    int anchor_index = 0, feature_index = 0;
    
    // These need to match the original buildChunkSection
    extern ChunkAnchor chunk_anchors[];
    extern ChunkFeature chunk_features[];
    extern uint8_t chunk_section_height[][16];
    extern ChunkFeature getFeatureFromAnchor(ChunkAnchor anchor);
    extern uint8_t getHeightAtFromAnchors(int rx, int rz, ChunkAnchor *anchor_ptr);
    
    for (int i = cz; i < cz + 16 + CHUNK_SIZE; i += CHUNK_SIZE) {
        for (int j = cx; j < cx + 16 + CHUNK_SIZE; j += CHUNK_SIZE) {
            ChunkAnchor *anchor = chunk_anchors + anchor_index;
            
            anchor->x = j / CHUNK_SIZE;
            anchor->z = i / CHUNK_SIZE;
            anchor->hash = getChunkHash(anchor->x, anchor->z);
            anchor->biome = getChunkBiome(anchor->x, anchor->z);
            
            if (i != cz + 16 && j != cx + 16) {
                chunk_features[feature_index] = getFeatureFromAnchor(*anchor);
                feature_index++;
            }
            
            anchor_index++;
        }
    }
    
    // Precompute terrain height for entire chunk section
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 16; j++) {
            anchor_index = (j / CHUNK_SIZE) + (i / CHUNK_SIZE) * (16 / CHUNK_SIZE + 1);
            ChunkAnchor *anchor_ptr = chunk_anchors + anchor_index;
            chunk_section_height[j][i] = getHeightAtFromAnchors(j % CHUNK_SIZE, i % CHUNK_SIZE, anchor_ptr);
        }
    }
    
    // Generate 4096 blocks using optimized tri-nested loop
    extern uint8_t getTerrainAtFromCache(int x, int y, int z, int rx, int rz,
                                         ChunkAnchor anchor, ChunkFeature feature, uint8_t height);
    
    for (int dy = 0; dy < 16; dy++) {
        int y = cy + dy;
        for (int dz = 0; dz < 16; dz++) {
            int z = cz + dz;
            int rz_mod = dz % CHUNK_SIZE;
            int rz_idx = dz / CHUNK_SIZE * (16 / CHUNK_SIZE);
            int anchor_base = dz / CHUNK_SIZE * (16 / CHUNK_SIZE + 1);

            for (int dx = 0; dx < 16; dx++) {
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
    extern int block_changes_count;
    extern BlockChange block_changes[];
    
    for (int i = 0; i < block_changes_count; i++) {
        if (block_changes[i].block == 0xFF) continue;
        if (block_changes[i].block == B_torch) continue;
        #ifdef ALLOW_CHESTS
            if (block_changes[i].block == B_chest) continue;
        #endif
        if (block_changes[i].x >= cx && block_changes[i].x < cx + 16 &&
            block_changes[i].y >= cy && block_changes[i].y < cy + 16 &&
            block_changes[i].z >= cz && block_changes[i].z < cz + 16) {
            int dx = block_changes[i].x - cx;
            int dy = block_changes[i].y - cy;
            int dz = block_changes[i].z - cz;
            unsigned address = (unsigned)(dx + (dz << 4) + (dy << 8));
            unsigned index = (address & ~7u) | (7u - (address & 7u));
            chunk_section[index] = block_changes[i].block;
        }
    }
    
    return chunk_anchors[0].biome;
}
