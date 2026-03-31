#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "globals.h"
#include "worldgen.h"

// Simple chunk cache - stores generated chunk section data
#define CHUNK_CACHE_SIZE 512

typedef struct {
  int x;  // Chunk X
  int z;  // Chunk Z
  uint8_t sections[20][4096];  // Block data for 20 middle sections
  uint8_t biomes[20];  // Biome for each section
  uint8_t valid;
} CachedChunkData;

static CachedChunkData chunk_cache[CHUNK_CACHE_SIZE];
static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t gen_mutex = PTHREAD_MUTEX_INITIALIZER;

// Find cache slot for chunk
static CachedChunkData* get_cache(int x, int z) {
  // Check if already cached
  for (int i = 0; i < CHUNK_CACHE_SIZE; i++) {
    if (chunk_cache[i].valid && chunk_cache[i].x == x && chunk_cache[i].z == z) {
      return &chunk_cache[i];
    }
  }
  // Find empty slot
  for (int i = 0; i < CHUNK_CACHE_SIZE; i++) {
    if (!chunk_cache[i].valid) {
      chunk_cache[i].x = x;
      chunk_cache[i].z = z;
      return &chunk_cache[i];
    }
  }
  // Overwrite slot 0
  return &chunk_cache[0];
}

// Generate chunk data (called from worker or main thread)
void generate_chunk_data(int x, int z) {
  pthread_mutex_lock(&gen_mutex);  // Only one chunk generation at a time
  
  CachedChunkData* cache = get_cache(x, z);
  
  int world_x = x * 16;
  int world_z = z * 16;
  
  // Generate all 20 middle sections
  for (int i = 0; i < 20; i++) {
    int y = i * 16;
    cache->biomes[i] = buildChunkSection(world_x, y, world_z);
    memcpy(cache->sections[i], chunk_section, 4096);
  }
  
  cache->valid = 1;
  pthread_mutex_unlock(&gen_mutex);
}

// Get cached chunk data (returns NULL if not cached)
CachedChunkData* get_cached_chunk(int x, int z) {
  pthread_mutex_lock(&cache_mutex);
  
  for (int i = 0; i < CHUNK_CACHE_SIZE; i++) {
    if (chunk_cache[i].valid && chunk_cache[i].x == x && chunk_cache[i].z == z) {
      pthread_mutex_unlock(&cache_mutex);
      return &chunk_cache[i];
    }
  }
  
  pthread_mutex_unlock(&cache_mutex);
  return NULL;
}

// Worker thread - continuously generates chunks around players
static pthread_t worker_thread;
static uint8_t running = 0;

void* chunk_generator_worker(void* arg) {
  (void)arg;
  
  while (running) {
    // Generate chunks around all players
    for (int i = 0; i < MAX_PLAYERS; i++) {
      if (player_data[i].client_fd == -1) continue;
      if (player_data[i].flags & 0x20) continue;  // Not fully loaded
      
      int px = player_data[i].x / 16;
      int pz = player_data[i].z / 16;
      
      // Pre-generate chunks in view distance
      for (int dz = -2; dz <= 2; dz++) {
        for (int dx = -2; dx <= 2; dx++) {
          int cx = px + dx;
          int cz = pz + dz;
          
          // Check if already cached
          CachedChunkData* cached = get_cached_chunk(cx, cz);
          if (!cached) {
            generate_chunk_data(cx, cz);
          }
        }
      }
    }
    
    // Small sleep to prevent CPU spinning
    usleep(50000);  // 50ms
  }
  
  return NULL;
}

void init_chunk_generator() {
  running = 1;
  pthread_create(&worker_thread, NULL, chunk_generator_worker, NULL);
  printf("Chunk generator thread started\n");
}

void shutdown_chunk_generator() {
  running = 0;
  pthread_join(worker_thread, NULL);
  printf("Chunk generator thread stopped\n");
}
