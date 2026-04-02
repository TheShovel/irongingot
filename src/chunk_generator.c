#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "globals.h"
#include "worldgen.h"
#include "config.h"
#include "tools.h"
#include "chunk_generator.h"
#include "thread_pool.h"

// Simple chunk cache - stores generated chunk section data
// Size is determined by config.chunk_cache_size

typedef struct {
  int x;
  int z;
} ChunkCoord;

static CachedChunkData* chunk_cache = NULL;
static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t global_access_counter = 0;
static int cache_size = 0;

// FIFO queue for on-demand chunk generation requests
#define CHUNK_REQUEST_QUEUE_SIZE 1024
static ChunkCoord request_queue[CHUNK_REQUEST_QUEUE_SIZE];
static int request_head = 0;
static int request_tail = 0;
static pthread_mutex_t request_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t request_cond = PTHREAD_COND_INITIALIZER;

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
  // Check if already cached or in progress
  for (int i = 0; i < cache_size; i++) {
    if (chunk_cache[i].x == x && chunk_cache[i].z == z &&
        (chunk_cache[i].valid || chunk_cache[i].generating)) {
      chunk_cache[i].access_count = ++global_access_counter;
      return &chunk_cache[i];
    }
  }

  // Find empty slot using LRU strategy
  int lru_slot = -1;
  uint32_t lru_count = UINT32_MAX;
  for (int i = 0; i < cache_size; i++) {
    if (!chunk_cache[i].valid && !chunk_cache[i].generating) {
      chunk_cache[i].x = x;
      chunk_cache[i].z = z;
      chunk_cache[i].access_count = ++global_access_counter;
      return &chunk_cache[i];
    }
    if (chunk_cache[i].generating) continue;
    // Track least recently used
    if (chunk_cache[i].access_count < lru_count) {
      lru_count = chunk_cache[i].access_count;
      lru_slot = i;
    }
  }

  // All cache entries are currently generating
  if (lru_slot == -1) return NULL;

  // Overwrite LRU slot
  chunk_cache[lru_slot].x = x;
  chunk_cache[lru_slot].z = z;
  chunk_cache[lru_slot].access_count = ++global_access_counter;
  chunk_cache[lru_slot].valid = 0;  // Mark invalid until generation completes
  chunk_cache[lru_slot].generating = 0;
  return &chunk_cache[lru_slot];
}

// Check whether a chunk is already cached or being generated (cache_mutex held)
static int is_chunk_cached_or_generating_locked(int x, int z) {
  for (int i = 0; i < cache_size; i++) {
    if (chunk_cache[i].x == x && chunk_cache[i].z == z &&
        (chunk_cache[i].valid || chunk_cache[i].generating)) {
      chunk_cache[i].access_count = ++global_access_counter;
      return 1;
    }
  }
  return 0;
}

// Queue a chunk generation request if it's not already queued.
// If the queue is full, evict the oldest request to prioritize newest chunks.
static void enqueue_chunk_request(int x, int z) {
  pthread_mutex_lock(&request_mutex);

  for (int i = request_head; i != request_tail; i = (i + 1) % CHUNK_REQUEST_QUEUE_SIZE) {
    if (request_queue[i].x == x && request_queue[i].z == z) {
      pthread_mutex_unlock(&request_mutex);
      return;
    }
  }

  int next_tail = (request_tail + 1) % CHUNK_REQUEST_QUEUE_SIZE;
  if (next_tail == request_head) {
    // Queue is full: drop oldest so newest player-near requests are retained.
    request_head = (request_head + 1) % CHUNK_REQUEST_QUEUE_SIZE;
  }

  request_queue[request_tail].x = x;
  request_queue[request_tail].z = z;
  request_tail = next_tail;

  pthread_cond_signal(&request_cond);
  pthread_mutex_unlock(&request_mutex);
}

static int pop_chunk_request(int *x, int *z) {
  pthread_mutex_lock(&request_mutex);

  if (request_head == request_tail) {
    pthread_mutex_unlock(&request_mutex);
    return 0;
  }

  *x = request_queue[request_head].x;
  *z = request_queue[request_head].z;
  request_head = (request_head + 1) % CHUNK_REQUEST_QUEUE_SIZE;

  pthread_mutex_unlock(&request_mutex);
  return 1;
}

static int maybe_pregen_one_chunk(void) {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (player_data[i].client_fd == -1) continue;
    if (player_data[i].flags & 0x20) continue;  // Not fully loaded

    int px = div_floor(player_data[i].x, 16);
    int pz = div_floor(player_data[i].z, 16);

    // Keep a very small warm cache around each active player.
    for (int dz = -2; dz <= 2; dz++) {
      for (int dx = -2; dx <= 2; dx++) {
        int cx = px + dx;
        int cz = pz + dz;

        CachedChunkData* cached = get_cached_chunk(cx, cz);
        if (!cached) {
          generate_chunk_data(cx, cz);
          return 1;
        }
      }
    }
  }
  return 0;
}

// Generate chunk data (can be called from worker thread or main thread)
// This function now supports parallel generation of non-adjacent chunks
void generate_chunk_data(int x, int z) {
  pthread_mutex_t* chunk_lock = get_chunk_lock(x, z);
  pthread_mutex_lock(chunk_lock);  // Lock only this chunk's bucket
  pthread_mutex_lock(&cache_mutex);

  CachedChunkData* cache = get_cache_locked(x, z);
  if (cache == NULL) {
    pthread_mutex_unlock(&cache_mutex);
    pthread_mutex_unlock(chunk_lock);
    return;
  }

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

// Queue background generation for a chunk if needed.
// This is intentionally non-blocking to keep packet handling fast.
void request_chunk_generation(int x, int z) {
  pthread_mutex_lock(&cache_mutex);
  int already_ready_or_generating = is_chunk_cached_or_generating_locked(x, z);
  pthread_mutex_unlock(&cache_mutex);

  if (already_ready_or_generating) return;
  enqueue_chunk_request(x, z);
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

int get_cached_chunk_copy(int x, int z, uint8_t out_sections[20][4096], uint8_t out_biomes[20]) {
  if (out_sections == NULL || out_biomes == NULL) return 0;

  pthread_mutex_lock(&cache_mutex);

  for (int i = 0; i < cache_size; i++) {
    if (chunk_cache[i].valid && !chunk_cache[i].generating &&
        chunk_cache[i].x == x && chunk_cache[i].z == z) {
      chunk_cache[i].access_count = ++global_access_counter;
      memcpy(out_sections, chunk_cache[i].sections, sizeof(chunk_cache[i].sections));
      memcpy(out_biomes, chunk_cache[i].biomes, sizeof(chunk_cache[i].biomes));
      pthread_mutex_unlock(&cache_mutex);
      return 1;
    }
  }

  pthread_mutex_unlock(&cache_mutex);
  return 0;
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

// Worker threads - continuously generate queued chunks
#define MAX_CHUNK_GENERATOR_WORKERS 4
static pthread_t worker_threads[MAX_CHUNK_GENERATOR_WORKERS];
static int worker_thread_count = 1;
static uint8_t running = 0;

void* chunk_generator_worker(void* arg) {
  intptr_t worker_id = (intptr_t)arg;

  while (running) {
    int request_x, request_z;

    // Always prioritize explicit chunk requests from networking.
    if (pop_chunk_request(&request_x, &request_z)) {
      generate_chunk_data(request_x, request_z);
      continue;
    }

    // Only one worker does opportunistic pre-generation while the request queue is idle.
    if (worker_id == 0 && maybe_pregen_one_chunk()) {
      continue;
    }

    // Queue idle: wait until new work arrives.
    pthread_mutex_lock(&request_mutex);
    if (running && request_head == request_tail) {
      pthread_cond_wait(&request_cond, &request_mutex);
    }
    pthread_mutex_unlock(&request_mutex);
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
  request_head = 0;
  request_tail = 0;

  int cpu_count = get_cpu_count();
  if (cpu_count < 1) cpu_count = 1;
  // Keep chunk generation conservative so it cannot starve gameplay.
  // One worker for up to 4 cores, two workers for larger hosts.
  if (cpu_count <= 4) worker_thread_count = 1;
  else worker_thread_count = 2;
  if (worker_thread_count > MAX_CHUNK_GENERATOR_WORKERS) worker_thread_count = MAX_CHUNK_GENERATOR_WORKERS;

  for (intptr_t i = 0; i < worker_thread_count; i++) {
    pthread_create(&worker_threads[i], NULL, chunk_generator_worker, (void *)i);
  }
  printf(
    "Chunk generator threads started (%d workers, cache size: %d chunks)\n",
    worker_thread_count,
    cache_size
  );
}

void shutdown_chunk_generator() {
  running = 0;
  pthread_mutex_lock(&request_mutex);
  pthread_cond_broadcast(&request_cond);
  pthread_mutex_unlock(&request_mutex);

  for (int i = 0; i < worker_thread_count; i++) {
    pthread_join(worker_threads[i], NULL);
  }
  worker_thread_count = 1;

  if (chunk_cache) {
    free(chunk_cache);
    chunk_cache = NULL;
  }
  printf("Chunk generator threads stopped\n");
}

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
