#ifndef H_CONFIG
#define H_CONFIG

#include <stdint.h>

// Server configuration structure
typedef struct {
  // Network settings
  int port;
  int max_players;
  int compression_threshold;
  int network_timeout;
  int mojang_api_timeout_ms;

  // Game settings
  int gamemode;              // 0 - survival; 1 - creative; 2 - adventure; 3 - spectator
  int view_distance;
  int mob_despawn_distance;
  int mob_spawn_enabled;
  int mob_spawn_max_per_player;
  int mob_spawn_range;
  int mob_spawn_min_distance;
  int mob_spawn_interval;
  uint32_t world_seed;
  uint32_t rng_seed;

  // World generation settings
  int chunk_size;
  int terrain_base_height;
  int cave_base_depth;
  int biome_size;

  // Performance settings
  int chunk_cache_size;
  int max_packet_len;
  int max_block_changes;
  int send_queue_limit;
  int chunk_queue_limit;
  int infinite_block_changes;  // 0 = fixed size, 1 = dynamic allocation
  int tick_interval;           // microseconds between server ticks
  int disk_sync_interval;      // microseconds between disk syncs

  // Feature flags
  int sync_world_to_disk;
  int sync_blocks_on_interval;
  int send_brand;
  int broadcast_all_movement;
  int scale_movement_updates;
  int do_fluid_flow;
  int allow_chests;
  int allow_doors;
  int enable_flight;
  int enable_pickup_animation;
  int enable_cactus_damage;
  int enable_commands;
  int fetch_skins_from_mojang;
  int safe_area_radius;  // chunks from spawn (8,8) protected from modification

  // Debug flags
  int log_unknown_packets;
  int log_length_discrepancy;
  int log_chunk_generation;
  int enable_beef_dumps;

  // Strings
  char motd[256];
  char brand[64];

  // Runtime
  int visited_history;       // calculated from view_distance
} ServerConfig;

// Global config instance
extern ServerConfig config;

// Config functions
void init_config_defaults(void);
int load_config(const char *filename);
int save_config(const char *filename);
void calculate_derived_config(void);

#endif
