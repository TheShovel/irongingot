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
#include "terminal_ui.h"

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

THREAD_LOCAL uint8_t *packet_buffer = NULL;
THREAD_LOCAL int packet_buffer_offset = 0;
THREAD_LOCAL int packet_mode = false;

uint8_t in_packet_buffer[MAX_PACKET_LEN];
int in_packet_buffer_offset = 0;
int in_packet_buffer_len = 0;

ssize_t recv_count;
uint8_t recv_buffer[MAX_RECV_BUF_LEN] = {0};

uint32_t world_seed = INITIAL_WORLD_SEED;
uint32_t rng_seed = INITIAL_RNG_SEED;

uint16_t world_time = 0;
uint32_t server_ticks = 0;

float world_rain_level = 0.0f;
float world_thunder_level = 0.0f;
uint8_t world_weather_clear = 1;
int32_t world_weather_clear_time = 0;
int32_t world_weather_rain_time = 0;
int32_t world_weather_thunder_time = 0;

char motd[MOTD_MAX_LEN] = { "A irongingot server" };
uint8_t motd_len = sizeof("A irongingot server") - 1;

char favicon[FAVICON_MAX_LEN] = { 0 };
uint16_t favicon_len = 0;

#ifdef SEND_BRAND
  char brand[BRAND_MAX_LEN] = { "irongingot" };
  uint8_t brand_len = sizeof("irongingot") - 1;
#endif

uint16_t client_count;
uint8_t player_noclip[MAX_PLAYERS];

#ifdef INFINITE_BLOCK_CHANGES
BlockChange *block_changes = NULL;
int block_changes_capacity = 0;
#else
BlockChange block_changes[MAX_BLOCK_CHANGES];
#endif
int block_changes_count = 0;

// Verbose debug logging
int verbose_mode = 0;

PlayerData player_data[MAX_PLAYERS];
int player_data_count = 0;
PlayerAppearance *player_appearance = NULL;

FluidUpdateEntry fluid_queue[FLUID_QUEUE_SIZE];
volatile int fluid_queue_head = 0;
volatile int fluid_queue_tail = 0;

MobData mob_data[MAX_MOBS];
// Per-mob trade usage tracking [mob_index][trade_index]
uint8_t mob_trade_uses[MAX_MOBS][5];
XpOrbData xp_orb_data[MAX_XP_ORBS];
ItemEntityData item_entity_data[MAX_ITEM_ENTITIES];
ProjectileData projectile_data[MAX_PROJECTILES];

// Global thread pool for parallel operations
static ThreadPool global_thread_pool;
static int thread_pool_initialized = 0;

void init_global_thread_pool(void) {
  if (!thread_pool_initialized) {
    int cpu_count = get_cpu_count();
    if (cpu_count < 1) cpu_count = 1;
    // Reserve one core for the main/gameplay loop.
    int num_threads = cpu_count - 1;
    if (num_threads < 1) num_threads = 1;
    // Cap thread pool to reasonable max. On small hosts (ESP32, etc.) use 1.
    // On large hosts, more threads can help with parallel chunk generation.
    // The hard cap prevents scheduler overload from hundreds of threads.
    int max_threads = config.max_thread_pool;
    if (max_threads < 1) max_threads = 4; // default for desktop
    if (num_threads > max_threads) num_threads = max_threads;
    
    if (thread_pool_init(&global_thread_pool, num_threads) == 0) {
      thread_pool_initialized = 1;
      terminal_ui_log("Initialized thread pool with %d worker threads", num_threads);
    } else {
      terminal_ui_log("Warning: Failed to initialize thread pool, running single-threaded");
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
