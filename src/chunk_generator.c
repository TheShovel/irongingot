#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "globals.h"
#include "worldgen.h"
#include "config.h"
#include "chunk_generator.h"

// Simple chunk cache - stores generated chunk section data
// Size is determined by config.chunk_cache_size

static CachedChunkData* chunk_cache = NULL;
static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t gen_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t global_access_counter = 0;
static int cache_size = 0;

// Find cache slot for chunk (must be called with cache_mutex held)
static CachedChunkData* get_cache_locked(int x, int z) {
  // Check if already cached
  for (int i = 0; i < cache_size; i++) {
    if (chunk_cache[i].valid && chunk_cache[i].x == x && chunk_cache[i].z == z) {
      chunk_cache[i].access_count = ++global_access_counter;
      return &chunk_cache[i];
    }
  }
  // Find empty slot using LRU strategy
  int lru_slot = 0;
  uint32_t lru_count = UINT32_MAX;
  for (int i = 0; i < cache_size; i++) {
    if (!chunk_cache[i].valid) {
      chunk_cache[i].x = x;
      chunk_cache[i].z = z;
      chunk_cache[i].access_count = ++global_access_counter;
      return &chunk_cache[i];
    }
    // Track least recently used
    if (chunk_cache[i].access_count < lru_count) {
      lru_count = chunk_cache[i].access_count;
      lru_slot = i;
    }
  }
  // Overwrite LRU slot
  chunk_cache[lru_slot].x = x;
  chunk_cache[lru_slot].z = z;
  chunk_cache[lru_slot].access_count = ++global_access_counter;
  chunk_cache[lru_slot].valid = 0;  // Mark invalid until generation completes
  return &chunk_cache[lru_slot];
}

// Generate chunk data (called from worker or main thread)
void generate_chunk_data(int x, int z) {
  pthread_mutex_lock(&gen_mutex);  // Only one chunk generation at a time
  pthread_mutex_lock(&cache_mutex);

  CachedChunkData* cache = get_cache_locked(x, z);
  
  // Mark as being generated to prevent partial reads
  cache->generating = 1;

  pthread_mutex_unlock(&cache_mutex);

  int world_x = x * 16;
  int world_z = z * 16;

  // Temporary buffer to build complete chunk data
  uint8_t temp_sections[20][4096];
  uint8_t temp_biomes[20];

  // Generate all 20 middle sections into temporary buffer
  for (int i = 0; i < 20; i++) {
    int y = i * 16;
    temp_biomes[i] = buildChunkSection(world_x, y, world_z);
    memcpy(temp_sections[i], chunk_section, 4096);
  }

  // Copy complete data to cache atomically
  pthread_mutex_lock(&cache_mutex);
  memcpy(cache->sections, temp_sections, sizeof(temp_sections));
  memcpy(cache->biomes, temp_biomes, sizeof(temp_biomes));
  cache->valid = 1;
  cache->generating = 0;  // Mark as complete
  pthread_mutex_unlock(&cache_mutex);

  pthread_mutex_unlock(&gen_mutex);
}

// Get cached chunk data (returns NULL if not cached or being generated)
CachedChunkData* get_cached_chunk(int x, int z) {
  pthread_mutex_lock(&cache_mutex);

  for (int i = 0; i < cache_size; i++) {
    if (chunk_cache[i].valid && !chunk_cache[i].generating &&
        chunk_cache[i].x == x && chunk_cache[i].z == z) {
      chunk_cache[i].access_count = ++global_access_counter;
      pthread_mutex_unlock(&cache_mutex);
      return &chunk_cache[i];
    }
  }

  pthread_mutex_unlock(&cache_mutex);
  return NULL;
}

// Invalidate cached chunk (called when block changes are made)
void invalidate_cached_chunk(int x, int z) {
  pthread_mutex_lock(&cache_mutex);

  for (int i = 0; i < cache_size; i++) {
    if (chunk_cache[i].valid &&
        chunk_cache[i].x == x && chunk_cache[i].z == z) {
      chunk_cache[i].valid = 0;  // Mark as invalid, will regenerate on next access
      break;
    }
  }

  pthread_mutex_unlock(&cache_mutex);
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
  // Allocate chunk cache based on config
  cache_size = config.chunk_cache_size;
  chunk_cache = (CachedChunkData*)malloc(cache_size * sizeof(CachedChunkData));
  if (!chunk_cache) {
    fprintf(stderr, "ERROR: Failed to allocate chunk cache (%d entries)\n", cache_size);
    exit(1);
  }
  // Initialize cache entries
  memset(chunk_cache, 0, cache_size * sizeof(CachedChunkData));
  
  running = 1;
  pthread_create(&worker_thread, NULL, chunk_generator_worker, NULL);
  printf("Chunk generator thread started (cache size: %d chunks)\n", cache_size);
}

void shutdown_chunk_generator() {
  running = 0;
  pthread_join(worker_thread, NULL);
  if (chunk_cache) {
    free(chunk_cache);
    chunk_cache = NULL;
  }
  printf("Chunk generator thread stopped\n");
}
