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
#include "thread_utils.h"
#include "terminal_ui.h"
#include "registries.h"
#include "special_block.h"

// Simple chunk cache - stores generated chunk section data
// Size is determined by config.chunk_cache_size

typedef struct {
  int x;
  int z;
  uint8_t dimension;
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
static inline uint32_t chunk_hash(int x, int z, uint8_t dimension) {
  uint32_t h = (uint32_t)x * 374761393u + (uint32_t)z * 668265263u + (uint32_t)dimension * 1640531513u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}

// Get lock for specific chunk
static inline pthread_mutex_t* get_chunk_lock(int x, int z, uint8_t dimension) {
  return &chunk_locks[chunk_hash(x, z, dimension) % CHUNK_LOCK_BUCKETS];
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
static CachedChunkData* get_cache_locked(int x, int z, uint8_t dimension) {
  // Check if already cached or in progress
  for (int i = 0; i < cache_size; i++) {
    if (chunk_cache[i].x == x && chunk_cache[i].z == z && chunk_cache[i].dimension == dimension &&
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
      chunk_cache[i].dimension = dimension;
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
  chunk_cache[lru_slot].dimension = dimension;
  chunk_cache[lru_slot].access_count = ++global_access_counter;
  chunk_cache[lru_slot].valid = 0;  // Mark invalid until generation completes
  chunk_cache[lru_slot].generating = 0;
  return &chunk_cache[lru_slot];
}

// Check whether a chunk is already cached or being generated (cache_mutex held)
static int is_chunk_cached_or_generating_locked(int x, int z, uint8_t dimension) {
  for (int i = 0; i < cache_size; i++) {
    if (chunk_cache[i].x == x && chunk_cache[i].z == z && chunk_cache[i].dimension == dimension &&
        (chunk_cache[i].valid || chunk_cache[i].generating)) {
      chunk_cache[i].access_count = ++global_access_counter;
      return 1;
    }
  }
  return 0;
}

// Queue a chunk generation request if it's not already queued.
// If the queue is full, evict the oldest request to prioritize newest chunks.
static void enqueue_chunk_request(int x, int z, uint8_t dimension) {
  pthread_mutex_lock(&request_mutex);

  for (int i = request_head; i != request_tail; i = (i + 1) % CHUNK_REQUEST_QUEUE_SIZE) {
    if (request_queue[i].x == x && request_queue[i].z == z && request_queue[i].dimension == dimension) {
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
  request_queue[request_tail].dimension = dimension;
  request_tail = next_tail;

  pthread_cond_signal(&request_cond);
  pthread_mutex_unlock(&request_mutex);
}

static int pop_chunk_request(int *x, int *z, uint8_t *dimension) {
  pthread_mutex_lock(&request_mutex);

  if (request_head == request_tail) {
    pthread_mutex_unlock(&request_mutex);
    return 0;
  }

  *x = request_queue[request_head].x;
  *z = request_queue[request_head].z;
  *dimension = request_queue[request_head].dimension;
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

    // Tiny warm cache around each active player.
    // With cache_size=4 and fast gen (~1.7ms/chunk), pregen only needs
    // to keep a few chunks ready for the next streamer cycle.
    for (int dz = -1; dz <= 1; dz++) {
      for (int dx = -1; dx <= 1; dx++) {
        int cx = px + dx;
        int cz = pz + dz;

        CachedChunkData* cached = get_cached_chunk(cx, cz, 0);
        if (!cached) {
          generate_chunk_data(cx, cz, 0);
          return 1;
        }
      }
    }
  }
  return 0;
}

// Generate chunk data (can be called from worker thread or main thread)
// This function now supports parallel generation of non-adjacent chunks
void generate_chunk_data(int x, int z, uint8_t dimension) {
  pthread_mutex_t* chunk_lock = get_chunk_lock(x, z, dimension);
  pthread_mutex_lock(chunk_lock);  // Lock only this chunk's bucket
  pthread_mutex_lock(&cache_mutex);

  CachedChunkData* cache = get_cache_locked(x, z, dimension);
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
  cache->dimension = dimension;

  pthread_mutex_unlock(&cache_mutex);

  int world_x = x * 16;
  int world_z = z * 16;

  // Temporary buffer to build complete chunk data (heap allocated to avoid 160KB stack usage)
  uint16_t (*temp_sections)[4096] = (uint16_t (*)[4096])malloc(20 * 4096 * sizeof(uint16_t));
  uint8_t *temp_biomes = (uint8_t *)malloc(20);
  if (!temp_sections || !temp_biomes) {
    free(temp_sections);
    free(temp_biomes);
    pthread_mutex_unlock(chunk_lock);
    return;
  }

  // Generate all 20 middle sections into temporary buffer
  for (int i = 0; i < 20; i++) {
    int y = i * 16;
    temp_biomes[i] = (uint8_t)buildChunkSection(world_x, y, world_z, dimension);
    memcpy(temp_sections[i], chunk_section, 4096 * sizeof(uint16_t));

    // Register generated wheat (B_wheat_1 through B_wheat_6) in the special
    // blocks table so the growth tick handler will find and grow them.
    // Fully-grown wheat (B_wheat_7) and player-planted wheat (B_wheat) are
    // already handled elsewhere.
    for (int j = 0; j < 4096; j++) {
      uint16_t raw = chunk_section[j];
      if (raw >= B_wheat && raw < B_wheat_7) {
        // Convert section index to world coordinates (bit-reversed addressing)
        int _addr = (j & ~7) | (7 - (j & 7));
        int _wx = world_x + (_addr & 15);
        int _wz = world_z + ((_addr >> 4) & 15);
        int _wy = y + (_addr >> 8);
        if (!special_block_has_entry(_wx, _wy, _wz, dimension)) {
          special_block_set_state(_wx, _wy, _wz, dimension, B_wheat, (uint16_t)(raw - B_wheat));
        }
      }
    }
  }

  // Copy complete data to cache atomically
  pthread_mutex_lock(&cache_mutex);
  memcpy(cache->sections, temp_sections, 20 * 4096 * sizeof(uint16_t));
  memcpy(cache->biomes, temp_biomes, 20);
  cache->valid = 1;
  cache->generating = 0;  // Mark as complete
  pthread_mutex_unlock(&cache_mutex);

  pthread_mutex_unlock(chunk_lock);

  free(temp_sections);
  free(temp_biomes);
}

// Queue background generation for a chunk if needed.
// This is intentionally non-blocking to keep packet handling fast.
void request_chunk_generation(int x, int z, uint8_t dimension) {
  pthread_mutex_lock(&cache_mutex);
  int already_ready_or_generating = is_chunk_cached_or_generating_locked(x, z, dimension);
  pthread_mutex_unlock(&cache_mutex);

  if (already_ready_or_generating) return;
  enqueue_chunk_request(x, z, dimension);
}

// Get cached chunk data (returns NULL if not cached or being generated)
CachedChunkData* get_cached_chunk(int x, int z, uint8_t dimension) {
  pthread_mutex_lock(&cache_mutex);

  for (int i = 0; i < cache_size; i++) {
    if (chunk_cache[i].valid && !chunk_cache[i].generating &&
        chunk_cache[i].x == x && chunk_cache[i].z == z && chunk_cache[i].dimension == dimension) {
      chunk_cache[i].access_count = ++global_access_counter;
      pthread_mutex_unlock(&cache_mutex);
      return &chunk_cache[i];
    }
  }

  pthread_mutex_unlock(&cache_mutex);
  return NULL;
}

int get_cached_chunk_copy(int x, int z, uint8_t dimension, uint16_t out_sections[20][4096], uint8_t out_biomes[20]) {
  if (out_sections == NULL || out_biomes == NULL) return 0;

  pthread_mutex_lock(&cache_mutex);

  for (int i = 0; i < cache_size; i++) {
    if (chunk_cache[i].valid && !chunk_cache[i].generating &&
        chunk_cache[i].x == x && chunk_cache[i].z == z && chunk_cache[i].dimension == dimension) {
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

// Scan a single chunk section for unregistered wheat and register it.
// Called during chunk copy to pick up any generated wheat that wasn't
// registered during initial chunk generation.
static void scan_chunk_for_wheat(int world_x, int section_y, int world_z, uint16_t* section, uint8_t dimension) {
  for (int j = 0; j < 4096; j++) {
    uint16_t raw = section[j];
    if (raw >= B_wheat && raw < B_wheat_7) {
      int _addr = (j & ~7) | (7 - (j & 7));
      int _wx = world_x + (_addr & 15);
      int _wz = world_z + ((_addr >> 4) & 15);
      int _wy = section_y + (_addr >> 8);
      if (!special_block_has_entry(_wx, _wy, _wz, dimension)) {
        special_block_set_state(_wx, _wy, _wz, dimension, B_wheat, (uint16_t)(raw - B_wheat));
      }
    }
  }
}

// Invalidate cached chunk (called when block changes are made)
void invalidate_cached_chunk(int x, int z, uint8_t dimension) {
  pthread_mutex_lock(&cache_mutex);

  for (int i = 0; i < cache_size; i++) {
    if (chunk_cache[i].valid &&
        chunk_cache[i].x == x && chunk_cache[i].z == z && chunk_cache[i].dimension == dimension) {
      chunk_cache[i].valid = 0;  // Mark as invalid, will regenerate on next access
      break;
    }
  }

  pthread_mutex_unlock(&cache_mutex);
}

// Worker threads - continuously generate queued chunks
#define MAX_CHUNK_GENERATOR_WORKERS 8
static pthread_t worker_threads[MAX_CHUNK_GENERATOR_WORKERS];
static int worker_thread_count = 1;
static uint8_t running = 0;

void* chunk_generator_worker(void* arg) {
  intptr_t worker_id = (intptr_t)arg;

  while (running) {
    int request_x, request_z;
    uint8_t request_dimension;

    // Always prioritize explicit chunk requests from networking.
    if (pop_chunk_request(&request_x, &request_z, &request_dimension)) {
      generate_chunk_data(request_x, request_z, request_dimension);
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
    char message[160];
    snprintf(message, sizeof(message), "ERROR: Failed to allocate chunk cache (%d entries)", cache_size);
    terminal_ui_shutdown(message);
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
  // More workers for faster chunk generation, scaled to CPU count.
  if (cpu_count <= 2) worker_thread_count = 1;
  else if (cpu_count <= 4) worker_thread_count = 2;
  else if (cpu_count <= 8) worker_thread_count = 4;
  else worker_thread_count = 6;
  if (worker_thread_count > MAX_CHUNK_GENERATOR_WORKERS) worker_thread_count = MAX_CHUNK_GENERATOR_WORKERS;

  for (intptr_t i = 0; i < worker_thread_count; i++) {
    int ret = create_server_thread_with_stack(&worker_threads[i], IRONGINGOT_CHUNK_THREAD_STACK_SIZE, chunk_generator_worker, (void *)i);
    if (ret != 0) {
      char message[192];
      snprintf(message, sizeof(message), "ERROR: Failed to start chunk generator thread %ld: %s", (long)i, strerror(ret));
      running = 0;
      pthread_mutex_lock(&request_mutex);
      pthread_cond_broadcast(&request_cond);
      pthread_mutex_unlock(&request_mutex);
      for (intptr_t j = 0; j < i; j++) {
        pthread_join(worker_threads[j], NULL);
      }
      terminal_ui_shutdown(message);
      exit(1);
    }
  }
  terminal_ui_log(
    "Chunk generator threads started (%d workers, cache size: %d chunks)",
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
  terminal_ui_log("Chunk generator threads stopped");
}

// Task function for chunk generation
static void chunk_gen_task(void* arg) {
  ChunkCoord* coord = (ChunkCoord*)arg;
  generate_chunk_data(coord->x, coord->z, coord->dimension);
  free(coord);
}

// Generate multiple chunks in parallel using thread pool
void generate_chunks_parallel(int* chunks, int chunk_count, uint8_t dimension, ThreadPool* pool) {
  if (pool == NULL || chunks == NULL || chunk_count <= 0) {
    return;
  }

  // Submit chunk generation tasks
  int submitted = 0;
  for (int i = 0; i < chunk_count; i += 2) {
    int x = chunks[i];
    int z = chunks[i + 1];

    // Check if already cached before submitting
    if (get_cached_chunk(x, z, dimension) != NULL) {
      continue;
    }

    ChunkCoord* coord = (ChunkCoord*)malloc(sizeof(ChunkCoord));
    if (coord == NULL) {
      continue;
    }
    coord->x = x;
    coord->z = z;
    coord->dimension = dimension;

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
