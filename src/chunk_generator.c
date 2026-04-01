#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "globals.h"
#include "worldgen.h"
#include "config.h"
#include "chunk_generator.h"
#include "thread_pool.h"

// Simple chunk cache - stores generated chunk section data
// Size is determined by config.chunk_cache_size

static CachedChunkData* chunk_cache = NULL;
static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t global_access_counter = 0;
static int cache_size = 0;

// Per-chunk locks for parallel generation
// Using a fixed-size hash table of mutexes to limit memory usage
#define CHUNK_LOCK_BUCKETS 256
static pthread_mutex_t chunk_locks[CHUNK_LOCK_BUCKETS];
static int chunk_locks_initialized = 0;

// Hash function for chunk coordinates
static inline uint32_t chunk_hash(int x, int z) {
  uint32_t h = (uint32_t)x * 374761393u + (uint32_t)z * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}

// Get lock for specific chunk
static inline pthread_mutex_t* get_chunk_lock(int x, int z) {
  return &chunk_locks[chunk_hash(x, z) % CHUNK_LOCK_BUCKETS];
}

// Initialize per-chunk locks
static void init_chunk_locks(void) {
  if (!chunk_locks_initialized) {
    for (int i = 0; i < CHUNK_LOCK_BUCKETS; i++) {
      pthread_mutex_init(&chunk_locks[i], NULL);
    }
    chunk_locks_initialized = 1;
  }
}

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

// Generate chunk data (can be called from worker thread or main thread)
// This function now supports parallel generation of non-adjacent chunks
void generate_chunk_data(int x, int z) {
  pthread_mutex_t* chunk_lock = get_chunk_lock(x, z);
  pthread_mutex_lock(chunk_lock);  // Lock only this chunk's bucket
  pthread_mutex_lock(&cache_mutex);

  CachedChunkData* cache = get_cache_locked(x, z);

  // Check if another thread already generated this chunk while we were waiting
  if (cache->valid && !cache->generating) {
    pthread_mutex_unlock(&cache_mutex);
    pthread_mutex_unlock(chunk_lock);
    return;
  }

  // Mark as being generated to prevent duplicate work
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

  pthread_mutex_unlock(chunk_lock);
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

  // Initialize per-chunk locks for parallel generation
  init_chunk_locks();

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

// Task argument for parallel chunk generation
typedef struct {
  int x;
  int z;
} ChunkCoord;

// Task function for chunk generation
static void chunk_gen_task(void* arg) {
  ChunkCoord* coord = (ChunkCoord*)arg;
  generate_chunk_data(coord->x, coord->z);
  free(coord);
}

// Generate multiple chunks in parallel using thread pool
void generate_chunks_parallel(int* chunks, int chunk_count, ThreadPool* pool) {
  if (pool == NULL || chunks == NULL || chunk_count <= 0) {
    return;
  }

  // Submit chunk generation tasks
  int submitted = 0;
  for (int i = 0; i < chunk_count; i += 2) {
    int x = chunks[i];
    int z = chunks[i + 1];

    // Check if already cached before submitting
    if (get_cached_chunk(x, z) != NULL) {
      continue;
    }

    ChunkCoord* coord = (ChunkCoord*)malloc(sizeof(ChunkCoord));
    if (coord == NULL) {
      continue;
    }
    coord->x = x;
    coord->z = z;

    if (thread_pool_submit(pool, chunk_gen_task, coord) == 0) {
      submitted++;
    } else {
      free(coord);
    }
  }

  // Wait for all submitted tasks to complete
  if (submitted > 0) {
    thread_pool_wait(pool);
  }
}
