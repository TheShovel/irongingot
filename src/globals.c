#include <stdio.h>
#include <stdint.h>
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <arpa/inet.h>
#endif
#include <unistd.h>

#include "globals.h"
#include "config.h"

#ifdef ESP_PLATFORM
  #include "esp_task_wdt.h"
  #include "esp_timer.h"

  // Time between vTaskDelay calls in microseconds
  #define TASK_YIELD_INTERVAL 1000 * 1000
  // How many ticks to delay for on each yield
  #define TASK_YIELD_TICKS 1

  int64_t last_yield = 0;
  void task_yield () {
    int64_t time_now = esp_timer_get_time();
    if (time_now - last_yield < TASK_YIELD_INTERVAL) return;
    vTaskDelay(TASK_YIELD_TICKS);
    last_yield = time_now;
  }
#endif

ClientState client_states[MAX_PLAYERS];

uint8_t packet_buffer[MAX_PACKET_LEN];
int packet_buffer_offset = 0;
int packet_mode = false;

uint8_t in_packet_buffer[MAX_PACKET_LEN];
int in_packet_buffer_offset = 0;
int in_packet_buffer_len = 0;

ssize_t recv_count;
uint8_t recv_buffer[MAX_RECV_BUF_LEN] = {0};

uint32_t world_seed = INITIAL_WORLD_SEED;
uint32_t rng_seed = INITIAL_RNG_SEED;

uint16_t world_time = 0;
uint32_t server_ticks = 0;

char motd[MOTD_MAX_LEN] = { "A bareiron server" };
uint8_t motd_len = sizeof("A bareiron server") - 1;

#ifdef SEND_BRAND
  char brand[BRAND_MAX_LEN] = { "bareiron" };
  uint8_t brand_len = sizeof("bareiron") - 1;
#endif

uint16_t client_count;

#ifdef INFINITE_BLOCK_CHANGES
BlockChange *block_changes = NULL;
int block_changes_capacity = 0;
#else
BlockChange block_changes[MAX_BLOCK_CHANGES];
#endif
int block_changes_count = 0;

PlayerData player_data[MAX_PLAYERS];
int player_data_count = 0;

MobData mob_data[MAX_MOBS];

// Global thread pool for parallel operations
static ThreadPool global_thread_pool;
static int thread_pool_initialized = 0;

void init_global_thread_pool(void) {
  if (!thread_pool_initialized) {
    int num_threads = get_cpu_count();
    if (num_threads < 2) num_threads = 2;
    
    // Cap at 8 threads to avoid excessive context switching
    if (num_threads > 8) num_threads = 8;
    
    if (thread_pool_init(&global_thread_pool, num_threads) == 0) {
      thread_pool_initialized = 1;
      printf("Initialized thread pool with %d worker threads\n", num_threads);
    } else {
      fprintf(stderr, "Warning: Failed to initialize thread pool, running single-threaded\n");
    }
  }
}

ThreadPool* get_global_thread_pool(void) {
  if (!thread_pool_initialized) {
    init_global_thread_pool();
  }
  return thread_pool_initialized ? &global_thread_pool : NULL;
}

void shutdown_global_thread_pool(void) {
  if (thread_pool_initialized) {
    thread_pool_destroy(&global_thread_pool);
    thread_pool_initialized = 0;
  }
}
