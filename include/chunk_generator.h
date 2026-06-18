#ifndef H_CHUNK_GENERATOR
#define H_CHUNK_GENERATOR

#include "config.h"
#include "thread_pool.h"

typedef struct {
  int x;  // Chunk X
  int z;  // Chunk Z
  uint16_t *sections[20];  // Block data for 20 middle sections, NULL if uniform
  uint16_t uniform_blocks[20];  // Block state if section is uniform
  uint8_t biomes[20];  // Biome for each section
  uint8_t valid;
  uint8_t generating;  // Lock-free generation flag
  uint8_t dimension;
  uint32_t access_count;  // For LRU eviction
} CachedChunkData;

void init_chunk_generator();
void shutdown_chunk_generator();
void generate_chunk_data(int x, int z, uint8_t dimension);
void request_chunk_generation(int x, int z, uint8_t dimension);
CachedChunkData* get_cached_chunk(int x, int z, uint8_t dimension);
int get_cached_chunk_copy(int x, int z, uint8_t dimension, uint16_t out_sections[20][4096], uint8_t out_biomes[20]);
void invalidate_cached_chunk(int x, int z, uint8_t dimension);

// Parallel chunk generation using thread pool
void generate_chunks_parallel(int* chunks, int chunk_count, uint8_t dimension, ThreadPool* pool);

#endif
