#ifndef H_CHUNK_GENERATOR
#define H_CHUNK_GENERATOR

#define CHUNK_CACHE_SIZE 512

typedef struct {
  int x;  // Chunk X
  int z;  // Chunk Z
  uint8_t sections[20][4096];  // Block data for 20 middle sections
  uint8_t biomes[20];  // Biome for each section
  uint8_t valid;
  uint8_t generating;  // Lock-free generation flag
  uint32_t access_count;  // For LRU eviction
} CachedChunkData;

void init_chunk_generator();
void shutdown_chunk_generator();
void generate_chunk_data(int x, int z);
CachedChunkData* get_cached_chunk(int x, int z);

#endif
