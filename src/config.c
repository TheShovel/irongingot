#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "config.h"
#include "globals.h"

ServerConfig config;

// Trim leading and trailing whitespace
static char* trim(char *str) {
  char *end;
  while (isspace((unsigned char)*str)) str++;
  if (*str == 0) return str;
  end = str + strlen(str) - 1;
  while (end > str && isspace((unsigned char)*end)) end--;
  end[1] = '\0';
  return str;
}

// Parse a boolean value (true/false, yes/no, on/off, 1/0)
static int parse_bool(const char *value) {
  if (strcmp(value, "true") == 0 || strcmp(value, "yes") == 0 ||
      strcmp(value, "on") == 0 || strcmp(value, "1") == 0) {
    return 1;
  }
  return 0;
}

// Initialize config with default values
void init_config_defaults(void) {
  // Network settings
  config.port = 25565;
  config.max_players = 16;
  config.compression_threshold = 256;
  config.network_timeout = 15000000;
  config.mojang_api_timeout_ms = 3000;

  // Game settings
  config.gamemode = 0;  // survival
  config.view_distance = 5;
  config.mob_despawn_distance = 256;
  config.mob_spawn_enabled = 1;
  config.mob_spawn_max_per_player = 5;
  config.mob_spawn_range = 24;
  config.mob_spawn_min_distance = 6;
  config.mob_spawn_interval = 10;  // spawn check every N server ticks
  config.world_seed = 0xA103DE6C;
  config.rng_seed = 0xE2B9419;

  // World generation settings
  config.chunk_size = 8;
  config.terrain_base_height = 55;
  config.cave_base_depth = 20;
  config.biome_size = 64;  // chunk_size * 8

  // Performance settings
  config.chunk_cache_size = 64;
  config.max_packet_len = 262144;
  config.max_block_changes = 20000;
  config.send_queue_limit = 6 * 1024 * 1024;
  config.chunk_queue_limit = 2 * 1024 * 1024;
  config.infinite_block_changes = 0;  // disabled by default
  config.tick_interval = 50000;  // 50ms = 20 TPS
  config.disk_sync_interval = 15000000;  // 15 seconds

  // Feature flags
  config.sync_world_to_disk = 1;
  config.sync_blocks_on_interval = 0;
  config.send_brand = 1;
  config.broadcast_all_movement = 1;
  config.scale_movement_updates = 1;
  config.do_fluid_flow = 1;
  config.allow_chests = 1;
  config.allow_doors = 1;
  config.enable_flight = 0;
  config.enable_pickup_animation = 1;
  config.enable_cactus_damage = 1;
  config.enable_commands = 1;
  config.fetch_skins_from_mojang = 1;
  config.safe_area_radius = 0;  // 0 = disabled by default

  // Debug flags
  config.log_unknown_packets = 0;
  config.log_length_discrepancy = 1;
  config.log_chunk_generation = 0;
  config.enable_beef_dumps = 0;

  // Strings
  strcpy(config.motd, "A bareiron server");
  strcpy(config.brand, "bareiron");

  // Runtime (calculated later)
  config.visited_history = 1024;
}

// Calculate derived config values
void calculate_derived_config(void) {
  // Calculate visited_history based on view_distance
  // Should cover entire view area plus extra for circular buffer wrapping
  int view_area = (2 * config.view_distance + 1) * (2 * config.view_distance + 1);
  config.visited_history = view_area + 512;  // extra buffer
  if (config.visited_history < 1024) config.visited_history = 1024;

  // Calculate biome_size from chunk_size
  config.biome_size = config.chunk_size * 8;
}

// Load config from file
int load_config(const char *filename) {
  FILE *f = fopen(filename, "r");
  if (!f) {
    printf("Config file '%s' not found, using defaults\n", filename);
    return -1;
  }

  char line[512];
  int line_num = 0;

  while (fgets(line, sizeof(line), f)) {
    line_num++;
    char *trimmed = trim(line);

    // Skip empty lines and comments
    if (trimmed[0] == '\0' || trimmed[0] == '#') continue;

    // Find '=' separator
    char *eq = strchr(trimmed, '=');
    if (!eq) {
      fprintf(stderr, "Warning: Invalid config syntax at line %d: %s\n", line_num, trimmed);
      continue;
    }

    *eq = '\0';
    char *key = trim(trimmed);
    char *value = trim(eq + 1);

    // Parse known keys
    if (strcmp(key, "port") == 0) {
      config.port = atoi(value);
    } else if (strcmp(key, "max_players") == 0) {
      config.max_players = atoi(value);
    } else if (strcmp(key, "compression_threshold") == 0) {
      config.compression_threshold = atoi(value);
    } else if (strcmp(key, "network_timeout") == 0) {
      config.network_timeout = atoi(value);
    } else if (strcmp(key, "mojang_api_timeout_ms") == 0) {
      config.mojang_api_timeout_ms = atoi(value);
      if (config.mojang_api_timeout_ms < 250) config.mojang_api_timeout_ms = 250;
      if (config.mojang_api_timeout_ms > 30000) config.mojang_api_timeout_ms = 30000;
    } else if (strcmp(key, "gamemode") == 0) {
      config.gamemode = atoi(value);
      if (config.gamemode < 0) config.gamemode = 0;
      if (config.gamemode > 3) config.gamemode = 3;
    } else if (strcmp(key, "view_distance") == 0) {
      config.view_distance = atoi(value);
      if (config.view_distance < 2) config.view_distance = 2;
      if (config.view_distance > 32) config.view_distance = 32;
    } else if (strcmp(key, "mob_despawn_distance") == 0) {
      config.mob_despawn_distance = atoi(value);
    } else if (strcmp(key, "mob_spawn_enabled") == 0) {
      config.mob_spawn_enabled = parse_bool(value);
    } else if (strcmp(key, "mob_spawn_max_per_player") == 0) {
      config.mob_spawn_max_per_player = atoi(value);
    } else if (strcmp(key, "mob_spawn_range") == 0) {
      config.mob_spawn_range = atoi(value);
    } else if (strcmp(key, "mob_spawn_min_distance") == 0) {
      config.mob_spawn_min_distance = atoi(value);
    } else if (strcmp(key, "mob_spawn_interval") == 0) {
      config.mob_spawn_interval = atoi(value);
    } else if (strcmp(key, "world_seed") == 0) {
      config.world_seed = (uint32_t)strtoul(value, NULL, 0);
    } else if (strcmp(key, "rng_seed") == 0) {
      config.rng_seed = (uint32_t)strtoul(value, NULL, 0);
    } else if (strcmp(key, "chunk_size") == 0) {
      config.chunk_size = atoi(value);
    } else if (strcmp(key, "terrain_base_height") == 0) {
      config.terrain_base_height = atoi(value);
    } else if (strcmp(key, "cave_base_depth") == 0) {
      config.cave_base_depth = atoi(value);
    } else if (strcmp(key, "biome_size") == 0) {
      config.biome_size = atoi(value);
    } else if (strcmp(key, "chunk_cache_size") == 0) {
      config.chunk_cache_size = atoi(value);
    } else if (strcmp(key, "max_packet_len") == 0) {
      config.max_packet_len = atoi(value);
    } else if (strcmp(key, "send_queue_limit") == 0) {
      config.send_queue_limit = atoi(value);
    } else if (strcmp(key, "chunk_queue_limit") == 0) {
      config.chunk_queue_limit = atoi(value);
    } else if (strcmp(key, "max_block_changes") == 0) {
      config.max_block_changes = atoi(value);
    } else if (strcmp(key, "infinite_block_changes") == 0) {
      config.infinite_block_changes = parse_bool(value);
    } else if (strcmp(key, "tick_interval") == 0) {
      config.tick_interval = atoi(value);
    } else if (strcmp(key, "disk_sync_interval") == 0) {
      config.disk_sync_interval = atoi(value);
    } else if (strcmp(key, "sync_world_to_disk") == 0) {
      config.sync_world_to_disk = parse_bool(value);
    } else if (strcmp(key, "sync_blocks_on_interval") == 0) {
      config.sync_blocks_on_interval = parse_bool(value);
    } else if (strcmp(key, "send_brand") == 0) {
      config.send_brand = parse_bool(value);
    } else if (strcmp(key, "broadcast_all_movement") == 0) {
      config.broadcast_all_movement = parse_bool(value);
    } else if (strcmp(key, "scale_movement_updates") == 0) {
      config.scale_movement_updates = parse_bool(value);
    } else if (strcmp(key, "do_fluid_flow") == 0) {
      config.do_fluid_flow = parse_bool(value);
    } else if (strcmp(key, "allow_chests") == 0) {
      config.allow_chests = parse_bool(value);
    } else if (strcmp(key, "allow_doors") == 0) {
      config.allow_doors = parse_bool(value);
    } else if (strcmp(key, "enable_flight") == 0) {
      config.enable_flight = parse_bool(value);
    } else if (strcmp(key, "enable_pickup_animation") == 0) {
      config.enable_pickup_animation = parse_bool(value);
    } else if (strcmp(key, "enable_cactus_damage") == 0) {
      config.enable_cactus_damage = parse_bool(value);
    } else if (strcmp(key, "enable_commands") == 0) {
      config.enable_commands = parse_bool(value);
    } else if (strcmp(key, "safe_area_radius") == 0) {
      config.safe_area_radius = atoi(value);
      if (config.safe_area_radius < 0) config.safe_area_radius = 0;
    } else if (strcmp(key, "fetch_skins_from_mojang") == 0) {
      config.fetch_skins_from_mojang = parse_bool(value);
    } else if (strcmp(key, "log_unknown_packets") == 0) {
      config.log_unknown_packets = parse_bool(value);
    } else if (strcmp(key, "log_length_discrepancy") == 0) {
      config.log_length_discrepancy = parse_bool(value);
    } else if (strcmp(key, "log_chunk_generation") == 0) {
      config.log_chunk_generation = parse_bool(value);
    } else if (strcmp(key, "enable_beef_dumps") == 0) {
      config.enable_beef_dumps = parse_bool(value);
    } else if (strcmp(key, "motd") == 0) {
      strncpy(config.motd, value, sizeof(config.motd) - 1);
      config.motd[sizeof(config.motd) - 1] = '\0';
    } else if (strcmp(key, "brand") == 0) {
      strncpy(config.brand, value, sizeof(config.brand) - 1);
      config.brand[sizeof(config.brand) - 1] = '\0';
    } else {
      fprintf(stderr, "Warning: Unknown config key at line %d: %s\n", line_num, key);
    }
  }

  fclose(f);
  calculate_derived_config();
  printf("Configuration loaded from '%s'\n", filename);
  return 0;
}

// Save config to file
int save_config(const char *filename) {
  FILE *f = fopen(filename, "w");
  if (!f) {
    perror("Failed to create config file");
    return -1;
  }

  fprintf(f, "# Bareiron Server Configuration\n");
  fprintf(f, "# Generated automatically - edit as needed\n\n");

  fprintf(f, "# ============================================\n");
  fprintf(f, "# Network Settings\n");
  fprintf(f, "# ============================================\n");
  fprintf(f, "port = %d\n", config.port);
  fprintf(f, "max_players = %d\n", config.max_players);
  fprintf(f, "compression_threshold = %d\n", config.compression_threshold);
  fprintf(f, "network_timeout = %d\n", config.network_timeout);
  fprintf(f, "mojang_api_timeout_ms = %d\n", config.mojang_api_timeout_ms);
  fprintf(f, "\n");

  fprintf(f, "# ============================================\n");
  fprintf(f, "# Game Settings\n");
  fprintf(f, "# ============================================\n");
  fprintf(f, "# Gamemode: 0 = survival, 1 = creative, 2 = adventure, 3 = spectator\n");
  fprintf(f, "gamemode = %d\n", config.gamemode);
  fprintf(f, "view_distance = %d\n", config.view_distance);
  fprintf(f, "mob_despawn_distance = %d\n", config.mob_despawn_distance);
  fprintf(f, "mob_spawn_enabled = %s\n", config.mob_spawn_enabled ? "true" : "false");
  fprintf(f, "mob_spawn_max_per_player = %d\n", config.mob_spawn_max_per_player);
  fprintf(f, "mob_spawn_range = %d\n", config.mob_spawn_range);
  fprintf(f, "mob_spawn_min_distance = %d\n", config.mob_spawn_min_distance);
  fprintf(f, "mob_spawn_interval = %d\n", config.mob_spawn_interval);
  fprintf(f, "world_seed = 0x%X\n", config.world_seed);
  fprintf(f, "rng_seed = 0x%X\n", config.rng_seed);
  fprintf(f, "\n");

  fprintf(f, "# ============================================\n");
  fprintf(f, "# World Generation Settings\n");
  fprintf(f, "# ============================================\n");
  fprintf(f, "chunk_size = %d\n", config.chunk_size);
  fprintf(f, "terrain_base_height = %d\n", config.terrain_base_height);
  fprintf(f, "cave_base_depth = %d\n", config.cave_base_depth);
  fprintf(f, "biome_size = %d\n", config.biome_size);
  fprintf(f, "\n");

  fprintf(f, "# ============================================\n");
  fprintf(f, "# Performance Settings\n");
  fprintf(f, "# ============================================\n");
  fprintf(f, "chunk_cache_size = %d\n", config.chunk_cache_size);
  fprintf(f, "max_packet_len = %d\n", config.max_packet_len);
  fprintf(f, "send_queue_limit = %d\n", config.send_queue_limit);
  fprintf(f, "chunk_queue_limit = %d\n", config.chunk_queue_limit);
  fprintf(f, "max_block_changes = %d\n", config.max_block_changes);
  fprintf(f, "infinite_block_changes = %s\n", config.infinite_block_changes ? "true" : "false");
  fprintf(f, "tick_interval = %d\n", config.tick_interval);
  fprintf(f, "disk_sync_interval = %d\n", config.disk_sync_interval);
  fprintf(f, "\n");

  fprintf(f, "# ============================================\n");
  fprintf(f, "# Feature Flags (true/false)\n");
  fprintf(f, "# ============================================\n");
  fprintf(f, "sync_world_to_disk = %s\n", config.sync_world_to_disk ? "true" : "false");
  fprintf(f, "sync_blocks_on_interval = %s\n", config.sync_blocks_on_interval ? "true" : "false");
  fprintf(f, "send_brand = %s\n", config.send_brand ? "true" : "false");
  fprintf(f, "broadcast_all_movement = %s\n", config.broadcast_all_movement ? "true" : "false");
  fprintf(f, "scale_movement_updates = %s\n", config.scale_movement_updates ? "true" : "false");
  fprintf(f, "do_fluid_flow = %s\n", config.do_fluid_flow ? "true" : "false");
  fprintf(f, "allow_chests = %s\n", config.allow_chests ? "true" : "false");
  fprintf(f, "allow_doors = %s\n", config.allow_doors ? "true" : "false");
  fprintf(f, "enable_flight = %s\n", config.enable_flight ? "true" : "false");
  fprintf(f, "enable_pickup_animation = %s\n", config.enable_pickup_animation ? "true" : "false");
  fprintf(f, "enable_cactus_damage = %s\n", config.enable_cactus_damage ? "true" : "false");
  fprintf(f, "enable_commands = %s\n", config.enable_commands ? "true" : "false");
  fprintf(f, "safe_area_radius = %d\n", config.safe_area_radius);
  fprintf(f, "fetch_skins_from_mojang = %s\n", config.fetch_skins_from_mojang ? "true" : "false");
  fprintf(f, "\n");

  fprintf(f, "# ============================================\n");
  fprintf(f, "# Debug Options (true/false)\n");
  fprintf(f, "# ============================================\n");
  fprintf(f, "log_unknown_packets = %s\n", config.log_unknown_packets ? "true" : "false");
  fprintf(f, "log_length_discrepancy = %s\n", config.log_length_discrepancy ? "true" : "false");
  fprintf(f, "log_chunk_generation = %s\n", config.log_chunk_generation ? "true" : "false");
  fprintf(f, "enable_beef_dumps = %s\n", config.enable_beef_dumps ? "true" : "false");
  fprintf(f, "\n");

  fprintf(f, "# ============================================\n");
  fprintf(f, "# Server Information\n");
  fprintf(f, "# ============================================\n");
  fprintf(f, "motd = %s\n", config.motd);
  fprintf(f, "brand = %s\n", config.brand);

  fclose(f);
  printf("Configuration saved to '%s'\n", filename);
  return 0;
}
