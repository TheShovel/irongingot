#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <ctype.h>
#ifdef _WIN32
#include <winsock2.h>
#endif

#include "globals.h"
#include "tools.h"
#include "varnum.h"
#include "packets.h"
#include "registries.h"
#include "worldgen.h"
#include "structures.h"
#include "serialize.h"
#include "chunk_generator.h"
#include "mojang.h"
#include "procedures.h"
#include "special_block.h"
#include "thread_pool.h"
#include "config.h"
#include "creative_mode.h"
#include "terminal_ui.h"

#ifndef B_spawner
#define B_spawner B_cobweb
#endif
#ifndef B_wheat_7
#define B_wheat_7 B_wheat
#endif

// Precomputed angle conversion constants for mob AI
// 256.0 / (2.0 * M_PI) - converts radians to mob yaw units (0-255)
#define RAD_TO_MOBROT 40.743665431525205f
// 128.0 / M_PI - converts radians to enderman stare angle units
#define RAD_TO_ENDERNESS 40.743665431525205f

static void addXpToPlayer(PlayerData *player, uint16_t amount);

/* ── FD-to-index lookup tables ───────────────────────────────────────── */
/* These replace linear MAX_PLAYERS scans in hot paths. */
/* fd_to_player_index maps client_fd to player_data index (or -1 if none) */
/* fd_to_client_index maps client_fd to client_states index (or -1 if none) */

/* The maximum fd we track. Most FDs are small integers. */
#define FD_LOOKUP_SIZE 4096
static int fd_to_player_index[FD_LOOKUP_SIZE];
static int fd_to_client_index[FD_LOOKUP_SIZE];
static int fd_tables_initialized = 0;

static void ensure_fd_tables_init(void) {
    if (!fd_tables_initialized) {
        for (int i = 0; i < FD_LOOKUP_SIZE; i++) {
            fd_to_player_index[i] = -1;
            fd_to_client_index[i] = -1;
        }
        fd_tables_initialized = 1;
    }
}

static inline int fd_to_player(int client_fd) {
    if (client_fd >= 0 && client_fd < FD_LOOKUP_SIZE) return fd_to_player_index[client_fd];
    return -1;
}

static inline int fd_to_client(int client_fd) {
    if (client_fd >= 0 && client_fd < FD_LOOKUP_SIZE) return fd_to_client_index[client_fd];
    return -1;
}

static inline void fd_set_player(int client_fd, int player_index) {
    if (client_fd >= 0 && client_fd < FD_LOOKUP_SIZE) fd_to_player_index[client_fd] = player_index;
}

static inline void fd_set_client(int client_fd, int client_index) {
    if (client_fd >= 0 && client_fd < FD_LOOKUP_SIZE) fd_to_client_index[client_fd] = client_index;
}

static uint8_t isWheatBlock(uint16_t block) {
  return block >= B_wheat && block <= B_wheat_7;
}

// Helper: check if a block is solid enough for a fence/glass_pane to connect to.
// Fences connect to other fences, full solid blocks, and opaque cubes.
static uint8_t isFenceSolidBlock(uint16_t block) {
  // Fences connect to other fences and glass panes.
  if (is_fence_block(block)) return 1;
  // Everything else: connect only if it's a full solid cube.
  return is_full_block(block);
}

static uint8_t wheatBlockAge(uint16_t block) {
  if (block >= B_wheat && block <= B_wheat_7) return (uint8_t)(block - B_wheat);
  return 0;
}

// World spawn point (block coordinates)
#define SPAWN_BLOCK_X 8
#define SPAWN_BLOCK_Z 8
// World spawn chunk (chunk coordinates)
#define SPAWN_CHUNK_X (SPAWN_BLOCK_X >> 4)
#define SPAWN_CHUNK_Z (SPAWN_BLOCK_Z >> 4)

/* Forward declaration for water block check used by water surface cache */
static uint8_t isWaterBlock(uint16_t block);

/* ── Water surface cache for fish ───────────────────────────────────── */
/* Maps block column → water surface Y. Invalidated on block changes. */
#define WATER_SURFACE_CACHE_SIZE 256
#define WATER_SURFACE_CACHE_MASK (WATER_SURFACE_CACHE_SIZE - 1)
typedef struct {
    int x;
    int z;
    uint8_t dimension;
    uint8_t surface_y;
    uint8_t valid;
} WaterSurfaceEntry;
static WaterSurfaceEntry water_surface_cache[WATER_SURFACE_CACHE_SIZE];
static volatile uint32_t water_surface_cache_epoch = 1;
static uint32_t water_surface_cache_ages[WATER_SURFACE_CACHE_SIZE];

static inline uint32_t water_surface_hash(int x, int z, uint8_t dim) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)z * 668265263u + (uint32_t)dim * 1640531513u;
    return h ^ (h >> 16);
}

/* Look up or compute the water surface Y at block column (x, z). Returns 0 if no water found. */
static int get_water_surface_y(int block_x, int block_z, uint8_t dimension) {
    uint32_t h = water_surface_hash(block_x, block_z, dimension);
    uint32_t idx = h & WATER_SURFACE_CACHE_MASK;

    // Check cache
    if (water_surface_cache[idx].valid &&
        water_surface_cache[idx].x == block_x &&
        water_surface_cache[idx].z == block_z &&
        water_surface_cache[idx].dimension == dimension &&
        water_surface_cache_ages[idx] == water_surface_cache_epoch) {
        return water_surface_cache[idx].surface_y ? (int)water_surface_cache[idx].surface_y : 0;
    }

    // Cache miss: scan from top down for water surface
    int surface_y = 0;
    for (int y = 319; y >= 0; y--) {
        uint16_t block = getBlockAt2(block_x, y, block_z, dimension);
        uint16_t block_above = getBlockAt2(block_x, y + 1, block_z, dimension);
        if (isWaterBlock(block) && !isWaterBlock(block_above)) {
            surface_y = y;
            break;
        }
    }

    // Store in cache
    water_surface_cache[idx].x = block_x;
    water_surface_cache[idx].z = block_z;
    water_surface_cache[idx].dimension = dimension;
    water_surface_cache[idx].surface_y = (uint8_t)surface_y;
    water_surface_cache[idx].valid = 1;
    water_surface_cache_ages[idx] = water_surface_cache_epoch;

    return surface_y;
}

/* Invalidate the water surface cache (call when block changes occur) */
static void invalidate_water_surface_cache(void) {
    water_surface_cache_epoch++;
    if (water_surface_cache_epoch == 0) water_surface_cache_epoch = 1;
    for (int i = 0; i < WATER_SURFACE_CACHE_SIZE; i++) {
        water_surface_cache[i].valid = 0;
    }
}

/* ── Mob spatial hash for O(n) collision detection ─────────────────── */
#define MOB_GRID_CELL_SIZE 2  // 2x2 block cells
#define MOB_GRID_SIZE 32       // 32 cells wide = 64 blocks coverage
#define MOB_GRID_MASK (MOB_GRID_SIZE - 1)

// Each grid cell is a linked list of mob indices
typedef struct MobGridCell {
    int indices[16];  // Max 16 mobs per cell
    int count;
} MobGridCell;

// Static grid for the current tick's collision checks
static MobGridCell mob_grid[MOB_GRID_SIZE][MOB_GRID_SIZE];

/* Convert world coordinate to grid cell coordinate */
static inline int mob_world_to_grid(double coord) {
    return (int)(floor(coord / MOB_GRID_CELL_SIZE)) & MOB_GRID_MASK;
}

/* Clear the spatial hash grid */
static void mob_grid_clear(void) {
    for (int gx = 0; gx < MOB_GRID_SIZE; gx++) {
        for (int gz = 0; gz < MOB_GRID_SIZE; gz++) {
            mob_grid[gx][gz].count = 0;
        }
    }
}

/* Populate the spatial hash grid with ALL live mobs across all dimensions.
   Call once before processing a block of mobs to get O(1) collision lookups. */
static void mob_grid_build_all(void) {
    mob_grid_clear();
    for (int j = 0; j < MAX_MOBS; j++) {
        if (mob_data[j].type == 0) continue;
        if ((mob_data[j].data & 31) == 0) continue;

        int gx = mob_world_to_grid(mob_data[j].x);
        int gz = mob_world_to_grid(mob_data[j].z);
        MobGridCell *cell = &mob_grid[gx][gz];

        if (cell->count < 16) {
            cell->indices[cell->count++] = j;
        }
    }
}

/* Check if a position collides with any mob in the grid.
   Also checks dimension to avoid cross-dimension false collisions. */
static int mob_grid_check_collision(double x, double y, double z, int self_i, uint8_t dimension) {
    int gx = mob_world_to_grid(x);
    int gz = mob_world_to_grid(z);

    // Check this cell and 8 adjacent cells
    for (int dx = -1; dx <= 1; dx++) {
        for (int dz = -1; dz <= 1; dz++) {
            int cx = (gx + dx) & MOB_GRID_MASK;
            int cz = (gz + dz) & MOB_GRID_MASK;
            MobGridCell *cell = &mob_grid[cx][cz];

            for (int k = 0; k < cell->count; k++) {
                int j = cell->indices[k];
                if (j == self_i) continue;
                if (mob_data[j].dimension != dimension) continue;

                double mx = mob_data[j].x - x;
                double mz = mob_data[j].z - z;
                double my = fabs(mob_data[j].y - y);
                if (mx * mx + mz * mz < 1.0 && my < 2.0) {
                    return 1;  // Collision!
                }
            }
        }
    }
    return 0;
}

// Check if a block position is within the safe area (spawn protection)
static int is_in_safe_area(short bx, short bz, uint8_t dimension) {
  if (dimension != DIMENSION_OVERWORLD) return 0;
  if (config.safe_area_radius <= 0) return 0;
  // Spawn is at block (8, 8) which is in chunk (0, 0)
  int block_cx = div_floor(bx, 16);
  int block_cz = div_floor(bz, 16);
  // Use Chebyshev distance (square protection area)
  int dx = block_cx < 0 ? -block_cx : block_cx;
  int dz = block_cz < 0 ? -block_cz : block_cz;
  int dist = dx > dz ? dx : dz;
  return dist < config.safe_area_radius;
}

// Forward declaration for parallel player updates
static void handlePlayerUpdatesParallel(int64_t time_since_last_tick, ThreadPool* pool);
static void setPlayerActiveHand(PlayerData *player, uint8_t active);
static uint8_t shootBowArrow(PlayerData *player);

uint16_t getItemMaxDamage (uint16_t item) {
  switch (item) {
    case I_wooden_sword:
    case I_wooden_pickaxe:
    case I_wooden_axe:
    case I_wooden_shovel:
    case I_wooden_hoe:
      return 59;
    case I_stone_sword:
    case I_stone_pickaxe:
    case I_stone_axe:
    case I_stone_shovel:
    case I_stone_hoe:
      return 131;
    case I_iron_sword:
    case I_iron_pickaxe:
    case I_iron_axe:
    case I_iron_shovel:
    case I_iron_hoe:
      return 250;
    case I_diamond_sword:
    case I_diamond_pickaxe:
    case I_diamond_axe:
    case I_diamond_shovel:
    case I_diamond_hoe:
      return 1561;
    case I_golden_sword:
    case I_golden_pickaxe:
    case I_golden_axe:
    case I_golden_shovel:
    case I_golden_hoe:
      return 32;
    case I_netherite_sword:
    case I_netherite_pickaxe:
    case I_netherite_axe:
    case I_netherite_shovel:
    case I_netherite_hoe:
      return 2031;
    case I_shears: return 238;
    case I_fishing_rod: return 64;
    case I_bow: return 384;
    case I_crossbow: return 465;
    case I_shield: return 336;
    case I_flint_and_steel: return 64;
    case I_trident: return 250;
    case I_mace: return 500;
    case I_brush: return 64;
    case I_carrot_on_a_stick: return 25;
    case I_warped_fungus_on_a_stick: return 100;
    case I_elytra: return 432;
    default: return 0;
  }
}

uint8_t isDamageableItem (uint16_t item) {
  return getItemMaxDamage(item) != 0;
}

static uint8_t getBlockBreakDurabilityCost(uint16_t item) {
  switch (item) {
    case I_wooden_sword:
    case I_stone_sword:
    case I_iron_sword:
    case I_golden_sword:
    case I_diamond_sword:
    case I_netherite_sword:
      return 2;
    default:
      return isDamageableItem(item) ? 1 : 0;
  }
}

static void damageHeldItem(PlayerData *player, uint8_t amount) {
  uint8_t slot = player->hotbar;
  uint16_t item = player->inventory_items[slot];
  uint16_t max_damage = getItemMaxDamage(item);
  if (amount == 0 || max_damage == 0 || player->inventory_count[slot] == 0) return;

  uint32_t new_damage = (uint32_t)player->inventory_damage[slot] + amount;
  if (new_damage >= max_damage) {
    player->inventory_items[slot] = 0;
    player->inventory_count[slot] = 0;
    player->inventory_damage[slot] = 0;
  } else {
    player->inventory_damage[slot] = (uint16_t)new_damage;
  }

  sc_setContainerSlot(
    player->client_fd, 0,
    serverSlotToClientSlot(0, slot),
    player->inventory_count[slot],
    player->inventory_items[slot]
  );
  broadcastPlayerEquipment(player);
  sc_updateEntityAttributes(player->client_fd, player->client_fd, player->inventory_items[slot]);
}

static void damageAttackerItem(int attacker_id, uint8_t damage_type) {
  if (attacker_id <= 0 || damage_type != D_player_attack) return;
  if (getConfiguredGameMode() == 1) return;

  PlayerData *attacker;
  if (getPlayerData(attacker_id, &attacker)) return;
  damageHeldItem(attacker, 1);
}

// ---- Vanilla Java Combat Stats ----

// Returns the base attack damage for a held item (vanilla Java 1.9+ values).
static uint8_t getWeaponBaseDamage(uint16_t item) {
  switch (item) {
    case I_wooden_sword: return 4;
    case I_golden_sword: return 4;
    case I_stone_sword:  return 5;
    case I_iron_sword:   return 6;
    case I_diamond_sword:    return 7;
    case I_netherite_sword:  return 8;
    case I_wooden_axe:   return 7;
    case I_stone_axe:    return 9;
    case I_iron_axe:     return 9;
    case I_golden_axe:   return 7;
    case I_diamond_axe:      return 9;
    case I_netherite_axe:    return 10;
    case I_wooden_pickaxe:   return 2;
    case I_stone_pickaxe:    return 3;
    case I_iron_pickaxe:     return 3;
    case I_golden_pickaxe:   return 2;
    case I_diamond_pickaxe:  return 3;
    case I_netherite_pickaxe: return 4;
    case I_wooden_shovel:    return 3;
    case I_stone_shovel:     return 4;
    case I_iron_shovel:      return 5;
    case I_golden_shovel:    return 3;
    case I_diamond_shovel:   return 6;
    case I_netherite_shovel: return 7;
    case I_wooden_hoe:   return 1;
    case I_stone_hoe:    return 1;
    case I_iron_hoe:     return 1;
    case I_golden_hoe:   return 1;
    case I_diamond_hoe:       return 1;
    case I_netherite_hoe:     return 1;
    case I_trident:      return 8;
    case I_mace:         return 6;
    default: return 1; // Fist / unarmed
  }
}

// Returns the attack cooldown duration in microseconds for a held item.
// Derived from vanilla attackSpeed attribute (1 second / attackSpeed).
static int64_t getWeaponCooldownUs(uint16_t item) {
  // Vanilla attack speed values (attacks per second)
  float speed;
  switch (item) {
    case I_wooden_sword:
    case I_stone_sword:
    case I_iron_sword:
    case I_golden_sword:
    case I_diamond_sword:
    case I_netherite_sword:
      speed = 1.6f; break;
    case I_wooden_axe:
    case I_stone_axe:
      speed = 0.8f; break;
    case I_iron_axe:
      speed = 0.9f; break;
    case I_golden_axe:
    case I_diamond_axe:
    case I_netherite_axe:
      speed = 1.0f; break;
    case I_wooden_pickaxe:
    case I_stone_pickaxe:
    case I_iron_pickaxe:
    case I_golden_pickaxe:
    case I_diamond_pickaxe:
    case I_netherite_pickaxe:
      speed = 1.2f; break;
    case I_wooden_shovel:
    case I_stone_shovel:
    case I_iron_shovel:
    case I_golden_shovel:
    case I_diamond_shovel:
    case I_netherite_shovel:
      speed = 1.0f; break;
    case I_wooden_hoe:
    case I_golden_hoe:
      speed = 1.0f; break;
    case I_stone_hoe:
      speed = 2.0f; break;
    case I_iron_hoe:
      speed = 3.0f; break;
    case I_diamond_hoe:
    case I_netherite_hoe:
      speed = 4.0f; break;
    case I_trident:
      speed = 1.1f; break;
    case I_mace:
      speed = 0.4f; break;
    default:
      speed = 4.0f; break;  // Fist
  }
  // Convert speed to cooldown in microseconds (1 second / speed * 1,000,000)
  return (int64_t)(1000000.0 / (double)speed + 0.5);
}

// Returns the knockback strength for a held item (vanilla knockback attribute).
// The returned value is scaled by 100 for integer precision.
static uint8_t getWeaponKnockback(uint16_t item) {
  switch (item) {
    case I_wooden_sword:
    case I_stone_sword:
    case I_iron_sword:
    case I_golden_sword:
    case I_diamond_sword:
    case I_netherite_sword:
      return 40;  // 0.4 knockback
    case I_mace:
      return 40;
    case I_wooden_axe:
    case I_stone_axe:
    case I_iron_axe:
    case I_golden_axe:
    case I_diamond_axe:
    case I_netherite_axe:
      return 60;  // 0.6 knockback
    default:
      return 0;   // No knockback
  }
}

static uint32_t player_bow_draw_start[MAX_PLAYERS];
static uint32_t player_bow_last_use_tick[MAX_PLAYERS];

static int getPlayerIndexByPointer(PlayerData *player) {
  if (!player) return -1;
  if (player < player_data || player >= player_data + MAX_PLAYERS) return -1;
  return (int)(player - player_data);
}

uint8_t getConfiguredGameMode(void) {
  if (config.gamemode < 0 || config.gamemode > 3) return GAMEMODE;
  return (uint8_t)config.gamemode;
}

uint8_t getConfiguredDifficulty(void) {
  if (config.difficulty < 0 || config.difficulty > 3) return 2;
  return (uint8_t)config.difficulty;
}

static uint8_t getAbilityFlagsForGameMode(uint8_t gamemode) {
  switch (gamemode) {
    case 1: return 0x0D; // invulnerable, may fly, instant build
    case 3: return 0x07; // invulnerable, flying, may fly
    default: return config.enable_flight ? 0x04 : 0x00;
  }
}

uint8_t isPlayerNoclipEnabled(PlayerData *player) {
  int player_index = getPlayerIndexByPointer(player);
  if (player_index < 0) return 0;
  return player_noclip[player_index] != 0;
}

void syncPlayerNoclipState(PlayerData *player) {
  int player_index = getPlayerIndexByPointer(player);
  if (player_index < 0 || player->client_fd == -1) return;

  uint8_t gamemode = player_noclip[player_index] ? 3 : getConfiguredGameMode();
  sc_gameEvent(player->client_fd, 3, (float)gamemode);
  sc_playerAbilities(player->client_fd, getAbilityFlagsForGameMode(gamemode));
  sc_playerInfoUpdateUpdateGamemode(player->client_fd, *player, gamemode);

  // Synchronize position to force the client to refresh its state/collision
  sc_synchronizePlayerPosition(player->client_fd,
    (double)player->x + 0.5, (double)player->y, (double)player->z + 0.5,
    (float)player->yaw * 180.0f / 127.0f, (float)player->pitch * 90.0f / 127.0f);

  player->grounded_y = player->y;
}

void setPlayerNoclip(PlayerData *player, uint8_t enabled) {
  int player_index = getPlayerIndexByPointer(player);
  if (player_index < 0) return;

  player_noclip[player_index] = enabled ? 1 : 0;
  syncPlayerNoclipState(player);
}

static uint8_t isSafeSkinName(const char *name) {
  if (!name || !name[0]) return false;
  for (int i = 0; name[i] != '\0'; i++) {
    if (!(isalnum((unsigned char)name[i]) || name[i] == '_')) return false;
  }
  return true;
}

static void uuidToHex(const uint8_t *uuid, char *out) {
  static const char hex[] = "0123456789abcdef";
  for (int i = 0; i < 16; i++) {
    out[i * 2] = hex[(uuid[i] >> 4) & 0x0F];
    out[i * 2 + 1] = hex[uuid[i] & 0x0F];
  }
  out[32] = '\0';
}

static int readTokenFile(const char *path, char *out, size_t capacity) {
  FILE *file = fopen(path, "rb");
  if (!file) return -1;

  size_t len = 0;
  int ch;
  while ((ch = fgetc(file)) != EOF) {
    if (isspace((unsigned char)ch)) continue;
    if (len + 1 >= capacity) {
      fclose(file);
      if (capacity > 0) out[0] = '\0';
      return -2;
    }
    out[len++] = (char)ch;
  }

  fclose(file);
  out[len] = '\0';
  return (int)len;
}

static uint8_t loadAppearanceFilesForBase(PlayerAppearance *appearance, const char *base_name) {
  char path[128];
  int len;

  // Free any previously allocated buffers
  free(appearance->texture_value);
  appearance->texture_value = NULL;
  free(appearance->texture_signature);
  appearance->texture_signature = NULL;

  snprintf(path, sizeof(path), "skins/%s.texture", base_name);
  // Read file content into a temp buffer first
  FILE *file = fopen(path, "rb");
  if (!file) return false;

  char temp_buf[PLAYER_TEXTURE_VALUE_MAX];
  size_t read_len = fread(temp_buf, 1, sizeof(temp_buf) - 1, file);
  fclose(file);
  if (read_len <= 0) return false;
  temp_buf[read_len] = '\0';

  appearance->texture_value = (char *)malloc(read_len + 1);
  if (!appearance->texture_value) return false;
  memcpy(appearance->texture_value, temp_buf, read_len + 1);
  appearance->texture_value_len = (uint16_t)read_len;
  appearance->has_texture = true;

  snprintf(path, sizeof(path), "skins/%s.signature", base_name);
  file = fopen(path, "rb");
  if (file) {
    char sig_buf[PLAYER_TEXTURE_SIGNATURE_MAX];
    read_len = fread(sig_buf, 1, sizeof(sig_buf) - 1, file);
    fclose(file);
    if (read_len > 0) {
      sig_buf[read_len] = '\0';
      appearance->texture_signature = (char *)malloc(read_len + 1);
      if (appearance->texture_signature) {
        memcpy(appearance->texture_signature, sig_buf, read_len + 1);
        appearance->texture_signature_len = (uint16_t)read_len;
        appearance->has_signature = true;
      }
    }
  }

  return true;
}

void setClientState (int client_fd, int new_state) {
  ensure_fd_tables_init();
  // Fast path: check lookup table
  int ci = fd_to_client(client_fd);
  if (ci >= 0 && client_states[ci].client_fd == client_fd) {
    client_states[ci].state = new_state;
    return;
  }
  // Look for a client state with a matching file descriptor
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (client_states[i].client_fd != client_fd) continue;
    client_states[i].state = new_state;
    fd_set_client(client_fd, i);
    return;
  }
  // If the above failed, look for an unused client state slot
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (client_states[i].client_fd != -1) continue;
    client_states[i].client_fd = client_fd;
    client_states[i].state = new_state;
    client_states[i].compression_threshold = 0; // Disabled by default
    client_states[i].connection_generation++;
    fd_set_client(client_fd, i);
    return;
  }
}

// Fast inline check: is this client_fd in play state?
static inline uint8_t isClientInPlay(int client_fd) {
  int ci = fd_to_client(client_fd);
  if (ci >= 0 && client_states[ci].client_fd == client_fd) {
    return client_states[ci].state == STATE_PLAY;
  }
  // Fallback linear scan for unusual fd values
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (client_states[i].client_fd == client_fd) {
      fd_set_client(client_fd, i);
      return client_states[i].state == STATE_PLAY;
    }
  }
  return 0;
}

int getClientState (int client_fd) {
  int ci = fd_to_client(client_fd);
  if (ci >= 0 && client_states[ci].client_fd == client_fd) {
    return client_states[ci].state;
  }
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (client_states[i].client_fd != client_fd) continue;
    fd_set_client(client_fd, i);
    return client_states[i].state;
  }
  return STATE_NONE;
}

int getClientIndex (int client_fd) {
  int ci = fd_to_client(client_fd);
  if (ci >= 0 && client_states[ci].client_fd == client_fd) return ci;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (client_states[i].client_fd != client_fd) continue;
    fd_set_client(client_fd, i);
    return i;
  }
  return -1;
}

void setCompressionThreshold (int client_fd, int threshold) {
  int ci = fd_to_client(client_fd);
  if (ci >= 0 && client_states[ci].client_fd == client_fd) {
    client_states[ci].compression_threshold = threshold;
    return;
  }
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (client_states[i].client_fd != client_fd) continue;
    client_states[i].compression_threshold = threshold;
    fd_set_client(client_fd, i);
    return;
  }
}

int getCompressionThreshold (int client_fd) {
  int ci = fd_to_client(client_fd);
  if (ci >= 0 && client_states[ci].client_fd == client_fd) return client_states[ci].compression_threshold;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (client_states[i].client_fd != client_fd) continue;
    fd_set_client(client_fd, i);
    return client_states[i].compression_threshold;
  }
  return 0;
}

// Restores player data to initial state (fresh spawn)
void resetPlayerData (PlayerData *player) {
  uint8_t use_bed_spawn = false;
  short respawn_x = player->spawn_x;
  int16_t respawn_y = player->spawn_y;
  short respawn_z = player->spawn_z;
  uint8_t respawn_dimension = player->spawn_dimension;

  if (player->spawn_set && respawn_y > 0) {
    uint16_t spawn_block = getBlockAt2(respawn_x, respawn_y - 1, respawn_z, respawn_dimension);
    if (is_bed_block(spawn_block)) {
      use_bed_spawn = true;
    } else {
      player->spawn_set = 0;
    }
  }

  player->health = 20;
  player->hunger = 20;
  player->saturation = 2500;
  player->air = 300;
  if (use_bed_spawn) {
    player->x = respawn_x;
    player->z = respawn_z;
    player->y = respawn_y;
    player->dimension = respawn_dimension;
    player->flags &= ~0x02;
  } else {
    player->x = 8;
    player->z = 8;
    player->y = 80;
    player->dimension = DIMENSION_OVERWORLD;
    player->flags |= 0x02;
  }
  player->grounded_y = player->y;
  player->position_lock_ticks = 0;
  player->locked_x = player->x;
  player->locked_y = player->y;
  player->locked_z = player->z;
  if (!config.keep_inventory) {
    for (int i = 0; i < 41; i ++) {
      player->inventory_items[i] = 0;
      player->inventory_count[i] = 0;
    }
    for (int i = 0; i < 9; i ++) {
      player->craft_items[i] = 0;
      player->craft_count[i] = 0;
    }
    for (int i = 0; i < 27; i ++) {
      player->ender_chest_items[i] = 0;
      player->ender_chest_count[i] = 0;
    }
  }
  player->flags &= ~0x80;
  player->last_attack_time = 0;
  // Initialize XP
  player->xp_total = 0;
  player->xp_level = 0;
  player->xp_progress = 0.0f;
}

void resetPlayerAppearance (int player_index) {
  if (player_index < 0 || player_index >= MAX_PLAYERS) return;

  PlayerAppearance *appearance = &player_appearance[player_index];
  // Free dynamically allocated texture buffers
  free(appearance->texture_value);
  appearance->texture_value = NULL;
  free(appearance->texture_signature);
  appearance->texture_signature = NULL;
  appearance->texture_value_len = 0;
  appearance->texture_signature_len = 0;
  appearance->has_texture = false;
  appearance->has_signature = false;
  appearance->skin_parts = 0x7F;
  appearance->main_hand = 1;
}

void loadPlayerAppearance (int player_index, const uint8_t *uuid, const char *name) {
  if (player_index < 0 || player_index >= MAX_PLAYERS) return;

  resetPlayerAppearance(player_index);

  PlayerAppearance *appearance = &player_appearance[player_index];
  if (fetchMojangPlayerAppearance(uuid, name, appearance)) {
    terminal_ui_log("Skin: loaded texture for %s from Mojang", name);
    return;
  }

  char uuid_hex[33];
  uuidToHex(uuid, uuid_hex);

  if (loadAppearanceFilesForBase(appearance, uuid_hex)) {
    terminal_ui_log("Skin: loaded texture for %s from skins/%s.*", name, uuid_hex);
    return;
  }

  if (!isSafeSkinName(name)) return;
  if (loadAppearanceFilesForBase(appearance, name)) {
    terminal_ui_log("Skin: loaded texture for %s from skins/%s.*", name, name);
  }
}

void updatePlayerAppearanceClientSettings (int client_fd, uint8_t skin_parts, uint8_t main_hand) {
  int pi = fd_to_player(client_fd);
  if (pi >= 0 && player_data[pi].client_fd == client_fd) {
    player_appearance[pi].skin_parts = skin_parts;
    player_appearance[pi].main_hand = main_hand ? 1 : 0;
    return;
  }
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (player_data[i].client_fd != client_fd) continue;
    player_appearance[i].skin_parts = skin_parts;
    player_appearance[i].main_hand = main_hand ? 1 : 0;
    fd_set_player(client_fd, i);
    return;
  }
}

// Assigns the given data to a player_data entry
int reservePlayerData (int client_fd, uint8_t *uuid, char *name) {
  ensure_fd_tables_init();

  for (int i = 0; i < MAX_PLAYERS; i ++) {
    // Found existing player entry (UUID match)
    if (memcmp(player_data[i].uuid, uuid, 16) == 0) {
      // Set network file descriptor and username
      player_data[i].client_fd = client_fd;
      fd_set_player(client_fd, i);
      memcpy(player_data[i].name, name, 16);
      // Flag player as loading
      player_data[i].flags |= 0x20;
      player_data[i].flagval_16 = 0;
      // Reset their recently visited chunk list
      for (int j = 0; j < VISITED_HISTORY; j ++) {
        player_data[i].visited_x[j] = 32767;
        player_data[i].visited_z[j] = 32767;
      }
      player_bow_draw_start[i] = 0;
      player_bow_last_use_tick[i] = 0;
      player_noclip[i] = 0;
      loadPlayerAppearance(i, uuid, name);
      initCreativeModeUI(i);  // Initialize creative mode UI for this player
      return 0;
    }
    // Search for unallocated player slots
    uint8_t empty = true;
    for (uint8_t j = 0; j < 16; j ++) {
      if (player_data[i].uuid[j] != 0) {
        empty = false;
        break;
      }
    }
    // Found free space for a player, initialize default parameters
    if (empty) {
      if (player_data_count >= MAX_PLAYERS) return 1;
      player_data[i].client_fd = client_fd;
      fd_set_player(client_fd, i);
      player_data[i].flags |= 0x20;
      player_data[i].flagval_16 = 0;
      memcpy(player_data[i].uuid, uuid, 16);
      memcpy(player_data[i].name, name, 16);
      resetPlayerData(&player_data[i]);
      player_bow_draw_start[i] = 0;
      player_bow_last_use_tick[i] = 0;
      player_noclip[i] = 0;
      loadPlayerAppearance(i, uuid, name);
      initCreativeModeUI(i);  // Initialize creative mode UI for this player
      player_data_count ++;
      return 0;
    }
  }

  return 1;

}

int getPlayerData (int client_fd, PlayerData **output) {
  int pi = fd_to_player(client_fd);
  if (pi >= 0 && player_data[pi].client_fd == client_fd) {
    *output = &player_data[pi];
    return 0;
  }
  for (int i = 0; i < MAX_PLAYERS; i ++) {
    if (player_data[i].client_fd == client_fd) {
      *output = &player_data[i];
      fd_set_player(client_fd, i);
      return 0;
    }
  }
  return 1;
}

// Returns the player with the given name, or NULL if not found
PlayerData *getPlayerByName (int start_offset, int end_offset, uint8_t *buffer) {
  for (int i = 0; i < MAX_PLAYERS; i ++) {
    if (player_data[i].client_fd == -1) continue;
    int j;
    for (j = start_offset; j < end_offset && j < 256 && buffer[j] != ' '; j++) {
      if (player_data[i].name[j - start_offset] != buffer[j]) break;
    }
    if ((j == end_offset || buffer[j] == ' ') && j < 256) {
      return &player_data[i];
    }
  }
  return NULL;
}


// Marks a client as disconnected and cleans up player data
void handlePlayerDisconnect (int client_fd) {
  // Search for a corresponding player in the player data array
  for (int i = 0; i < MAX_PLAYERS; i ++) {
    if (player_data[i].client_fd != client_fd) continue;
    PlayerData leaving_player = player_data[i];
    // Mark the player as being offline
    player_data[i].client_fd = -1;
    fd_set_player(client_fd, -1);
    player_bow_draw_start[i] = 0;
    player_bow_last_use_tick[i] = 0;
    player_noclip[i] = 0;
    // Prepare leave message for broadcast
    uint8_t player_name_len = strlen(player_data[i].name);
    strcpy((char *)recv_buffer, player_data[i].name);
    strcpy((char *)recv_buffer + player_name_len, " left the game");
    // Broadcast this player's leave to all other connected clients
    for (int j = 0; j < MAX_PLAYERS; j ++) {
      if (player_data[j].client_fd == -1) continue;
      if (player_data[j].client_fd == client_fd) continue;
      if (player_data[j].flags & 0x20) continue;
      // Send chat message
      sc_systemChat(player_data[j].client_fd, (char *)recv_buffer, 14 + player_name_len);
      // Remove leaving player from tab list and from the world entity list
      sc_playerInfoRemovePlayer(player_data[j].client_fd, leaving_player);
      sc_removeEntity(player_data[j].client_fd, client_fd);
    }
    break;
  }
  // Save player data to disk when a player disconnects
  writePlayerDataToDisk();
  // Find the client state entry and reset it
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (client_states[i].client_fd == client_fd) {
      clear_client_send_queue(client_fd);
      client_states[i].client_fd = -1;
      fd_set_client(client_fd, -1);
      client_states[i].state = STATE_NONE;
      client_states[i].compression_threshold = 0;
      client_states[i].connection_generation++;
      // Clean up per-client inflate state and compressed buffer
      if (client_states[i].inflate_initialized) {
        inflateEnd(&client_states[i].inflate_stream);
        client_states[i].inflate_initialized = 0;
      }
      free(client_states[i].compressed_buf);
      client_states[i].compressed_buf = NULL;
      client_states[i].compressed_buf_cap = 0;
      return;
    }
  }
}

// Marks a client as connected and broadcasts their data to other players
void handlePlayerJoin (PlayerData* player) {

  // Send recipe book data to the joining player
  sc_declareRecipes(player->client_fd);
  sc_unlockRecipes(player->client_fd);

  // Prepare join message for broadcast
  uint8_t player_name_len = strlen(player->name);
  strcpy((char *)recv_buffer, player->name);
  strcpy((char *)recv_buffer + player_name_len, " joined the game");

  // Inform other clients (and the joining client) of the player's name and entity
  for (int i = 0; i < MAX_PLAYERS; i ++) {
    if (player_data[i].client_fd == -1) continue;
    if (!isClientInPlay(player_data[i].client_fd)) continue;  // Skip clients not yet in play state
    sc_systemChat(player_data[i].client_fd, (char *)recv_buffer, 16 + player_name_len);
    sc_playerInfoUpdateAddPlayer(player_data[i].client_fd, *player);
    sendPlayerMetadata(player_data[i].client_fd, player);
    if (player_data[i].client_fd != player->client_fd) {
      sc_spawnEntityPlayer(player_data[i].client_fd, *player);
      sendPlayerEquipment(player_data[i].client_fd, player);
    }
  }

  // Clear "client loading" flag and fallback timer
  player->flags &= ~0x20;
  player->flagval_16 = 0;

}

void disconnectClient (int *client_fd, int cause) {
  if (*client_fd == -1) return;

  // Clear any pending send queue data
  clear_client_send_queue(*client_fd);

  client_count --;
  setClientState(*client_fd, STATE_NONE);
  handlePlayerDisconnect(*client_fd);
  int disconnected_fd = *client_fd;
  terminal_ui_record_client_disconnect(cause);
  #ifdef _WIN32
  int disconnect_errno = WSAGetLastError();
  closesocket(disconnected_fd);
  #else
  int disconnect_errno = errno;
  close(disconnected_fd);
  #endif
  terminal_ui_log("Disconnected client fd=%d cause=%d errno=%d", disconnected_fd, cause, disconnect_errno);
  *client_fd = -1;
}

uint8_t serverSlotToClientSlot (int window_id, uint8_t slot) {

  if (window_id == 0) { // player inventory

    if (slot < 9) return slot + 36;
    if (slot >= 9 && slot <= 35) return slot;
    if (slot == 40) return 45;
    if (slot >= 36 && slot <= 39) return 44 - slot;
    if (slot == 41) return 1;
    if (slot == 42) return 2;
    if (slot == 44) return 3;
    if (slot == 45) return 4;

  } else if (window_id == 12) { // crafting table

    if (slot >= 41 && slot <= 49) return slot - 40;
    if (slot >= 9 && slot <= 35) return slot + 1;
    if (slot <= 8) return slot + 37;

  } else if (window_id == 14) { // furnace

    if (slot >= 41 && slot <= 43) return slot - 41;
    if (slot >= 9 && slot <= 35) return slot - 6;
    if (slot <= 8) return slot + 30;

  } else if (window_id == 19) { // merchant

    if (slot >= 41 && slot <= 43) return slot - 41;
    if (slot >= 9 && slot <= 35) return slot - 6;   // main inv at client 3-29
    if (slot <= 8) return slot + 30;                // hotbar at client 30-38

  }

  return 255;
}

uint8_t clientSlotToServerSlot (int window_id, uint8_t slot) {

  if (window_id == 0) { // player inventory

    if (slot >= 36 && slot <= 44) return slot - 36;
    if (slot >= 9 && slot <= 35) return slot;
    if (slot == 45) return 40;
    if (slot >= 5 && slot <= 8) return 44 - slot;

    // map inventory crafting slots to player data crafting grid (semi-hack)
    // this abuses the fact that the buffers are adjacent in player data
    if (slot == 1) return 41;
    if (slot == 2) return 42;
    if (slot == 3) return 44;
    if (slot == 4) return 45;

  } else if (window_id == 12) { // crafting table

    // same crafting offset overflow hack as above
    if (slot >= 1 && slot <= 9) return 40 + slot;
    // the rest of the slots are identical, just shifted by one
    if (slot >= 10 && slot <= 45) return clientSlotToServerSlot(0, slot - 1);

  } else if (window_id == 14) { // furnace

    // move furnace items to the player's crafting grid
    // this lets us put them back in the inventory once the window closes
    if (slot <= 2) return 41 + slot;
    // the rest of the slots are identical, just shifted by 6
    if (slot >= 3 && slot <= 38) return clientSlotToServerSlot(0, slot + 6);

  } else if (window_id == 19) { // merchant

    if (slot <= 2) return 41 + slot;
    if (slot >= 3 && slot <= 29) return slot + 6;   // main inv
    if (slot >= 30 && slot <= 38) return slot - 30; // hotbar

  }
  #ifdef ALLOW_CHESTS
  else if (window_id == 2) { // chest

    // overflow chest slots into crafting grid
    // technically invalid, expected to be handled on a per-case basis
    if (slot <= 26) return 41 + slot;
    // the rest of the slots are identical, just shifted by 18
    if (slot >= 27 && slot <= 62) return clientSlotToServerSlot(0, slot - 18);

  }
  #endif

  return 255;
}

int givePlayerItem (PlayerData *player, uint16_t item, uint8_t count) {

  if (item == 0 || count == 0) return 0;

  uint8_t slot = 255;
  uint8_t stack_size = getItemStackSize(item);

  for (int i = 0; i < 41; i ++) {
    if (player->inventory_items[i] == item && player->inventory_count[i] <= stack_size - count) {
      slot = i;
      break;
    }
  }

  if (slot == 255) {
    for (int i = 0; i < 41; i ++) {
      if (player->inventory_count[i] == 0) {
        slot = i;
        break;
      }
    }
  }

  // Fail to assign item if slot is outside of main inventory
  if (slot >= 36) return 1;

  player->inventory_items[slot] = item;
  player->inventory_count[slot] += count;
  if (!isDamageableItem(item)) player->inventory_damage[slot] = 0;
  sc_setContainerSlot(player->client_fd, 0, serverSlotToClientSlot(0, slot), player->inventory_count[slot], item);
  if (slot == player->hotbar) {
    broadcastPlayerEquipment(player);
    sc_updateEntityAttributes(player->client_fd, player->client_fd, player->inventory_items[player->hotbar]);
  }

  return 0;

}

static uint8_t canGivePlayerItem (PlayerData *player, uint16_t item, uint8_t count) {

  if (item == 0 || count == 0) return true;

  uint8_t stack_size = getItemStackSize(item);

  for (int i = 0; i < 36; i ++) {
    if (player->inventory_items[i] == item && player->inventory_count[i] <= stack_size - count) return true;
  }

  for (int i = 0; i < 36; i ++) {
    if (player->inventory_count[i] == 0) return true;
  }

  return false;

}

// Sends the full sequence for spawning the player to the client
void spawnPlayer (PlayerData *player) {

  // Player spawn coordinates, initialized to placeholders
  float spawn_x = 8.5f, spawn_y = 80.0f, spawn_z = 8.5f;
  float spawn_yaw = 0.0f, spawn_pitch = 0.0f;

  if (player->flags & 0x02) { // Is this a new player?
    // Determine spawning Y coordinate based on terrain height
    spawn_y = getHeightAt(8, 8) + 1;
    player->y = spawn_y;
    player->grounded_y = (int16_t)spawn_y;
    player->flags &= ~0x02;
  } else { // Not a new player
    // Calculate spawn position from player data
    spawn_x = (float)player->x + 0.5;
    spawn_y = player->y;
    spawn_z = (float)player->z + 0.5;
    // If Y is 0 or impossibly low, lift above surface
    if (spawn_y <= 0 || spawn_y > 319 || spawn_y < 5) {
      if (player->dimension == DIMENSION_OVERWORLD) {
        spawn_y = getHeightAt(player->x, player->z) + 1;
      } else {
        init_worldgen();
        double fn = octave_sample(&surface_noise, (double)player->x * 0.015, 0, (double)player->z * 0.015);
        int fh = 80 + (int)(fn * 15.0);
        if (fh < 65) fh = 65;
        if (fh > 100) fh = 100;
        spawn_y = (float)(fh + 2);
      }
      player->y = (int16_t)spawn_y;
      player->grounded_y = (int16_t)spawn_y;
    }
    spawn_yaw = player->yaw * 180 / 127;
    spawn_pitch = player->pitch * 90 / 127;
  }

  // Final safety clamp: ensure player never spawns below Y=5 or above build limit
  if (spawn_y < 5.0f) spawn_y = getHeightAt(8, 8) + 1;
  if (spawn_y > 319.0f) spawn_y = 319.0f;
  if (spawn_y < 5.0f) spawn_y = 80.0f; // Absolute fallback if getHeightAt also fails

  // Teleport player to spawn coordinates (first pass)
  sc_synchronizePlayerPosition(player->client_fd, spawn_x, spawn_y, spawn_z, spawn_yaw, spawn_pitch);

  // Clear crafting grid residue, unlock craft_items
  for (int i = 0; i < 9; i++) {
    player->craft_items[i] = 0;
    player->craft_count[i] = 0;
  }
  player->flags &= ~0x80;

  // Sync client inventory and hotbar
  for (uint8_t i = 0; i < 41; i ++) {
    sc_setContainerSlot(player->client_fd, 0, serverSlotToClientSlot(0, i), player->inventory_count[i], player->inventory_items[i]);
  }
  sc_setHeldItem(player->client_fd, player->hotbar);
  // Sync weapon attributes for attack cooldown
  sc_updateEntityAttributes(player->client_fd, player->client_fd, player->inventory_items[player->hotbar]);
  // Sync client health and hunger
  sc_setHealth(player->client_fd, player->health, player->hunger, player->saturation);
  // Sync player entity metadata (air, skin parts, etc.) to the joining player
  sendPlayerMetadata(player->client_fd, player);
  // Sync client XP
  sc_setExperience(player->client_fd, player->xp_total, player->xp_level, player->xp_progress);
  // Sync client clock time
  sc_updateTime(player->client_fd, world_time);

  // Sync respawn screen state (event 11 = enable respawn screen)
  sc_gameEvent(player->client_fd, 11, config.do_immediate_respawn ? 0.0f : 1.0f);

  // Sync client weather (only Overworld has weather)
  if (player->dimension == DIMENSION_OVERWORLD) {
    if (world_weather_clear) {
      sc_gameEvent(player->client_fd, 2, 0.0f);
    } else {
      sc_gameEvent(player->client_fd, 1, 0.0f);
    }
    sc_gameEvent(player->client_fd, 7, world_rain_level);
    sc_gameEvent(player->client_fd, 8, world_thunder_level);
  } else {
    sc_gameEvent(player->client_fd, 2, 0.0f);
    sc_gameEvent(player->client_fd, 7, 0.0f);
    sc_gameEvent(player->client_fd, 8, 0.0f);
  }

  // Sync client difficulty
  sc_changeDifficulty(player->client_fd, (uint8_t)config.difficulty, 0);

  #ifdef ENABLE_PLAYER_FLIGHT
  uint8_t configured_gamemode = getConfiguredGameMode();
  if (configured_gamemode != 1 && configured_gamemode != 3) {
    // Give the player flight (for testing)
    sc_playerAbilities(player->client_fd, 0x04);
  }
  #endif

  // Calculate player's chunk coordinates
  short _x = div_floor(player->x, 16), _z = div_floor(player->z, 16);

  // Indicate that we're about to send chunk data
  if (player->spawn_set) {
    sc_setDefaultSpawnPosition(player->client_fd, player->spawn_x, player->spawn_y, player->spawn_z);
  } else {
    sc_setDefaultSpawnPosition(player->client_fd, 8, 80, 8);
  }
  sc_startWaitingForChunks(player->client_fd);
  sc_setCenterChunk(player->client_fd, _x, _z);

  sc_chunkBatchStart(player->client_fd);
  // For non-overworld dimensions, send a 3×3 area so the portal (and any
  // placed blocks in adjacent chunks) are visible immediately. Otherwise
  // the client only sees the center chunk and can't place blocks nearby.
  int r = (player->dimension == DIMENSION_OVERWORLD) ? 0 : 1;
  for (int dz = -r; dz <= r; dz++) {
    for (int dx = -r; dx <= r; dx++) {
      generate_chunk_data(_x + dx, _z + dz, player->dimension);
      sc_chunkDataAndUpdateLight(player->client_fd, _x + dx, _z + dz, player->dimension);
    }
  }
  sc_chunkBatchFinished(player->client_fd, 1);

  // Initialize visited chunks tracking
  player->visited_next = 0;
  // Initialize all slots as invalid
  for (int i = 0; i < VISITED_HISTORY; i++) {
    player->visited_x[i] = 32767;
    player->visited_z[i] = 32767;
  }
  // Mark center chunk as visited
  player->visited_x[player->visited_next] = _x;
  player->visited_z[player->visited_next] = _z;
  player->visited_next = (player->visited_next + 1) % VISITED_HISTORY;

  // Re-teleport player after chunks have been sent
  sc_synchronizePlayerPosition(player->client_fd, spawn_x, spawn_y, spawn_z, spawn_yaw, spawn_pitch);

}

void sendPlayerMetadata (int client_fd, PlayerData *player) {
  int player_index = getPlayerIndexByPointer(player);
  if (client_fd == -1 || player_index == -1) return;

  uint8_t sneaking = (player->flags & 0x04) != 0;
  uint8_t sprinting = (player->flags & 0x08) != 0;
  PlayerAppearance *appearance = &player_appearance[player_index];

  uint8_t entity_bit_mask = 0;
  if (sneaking) entity_bit_mask |= 0x02;
  if (sprinting) entity_bit_mask |= 0x08;

  int pose = 0;
  if (sneaking) pose = 5;

  uint8_t hand_state = 0;
  if ((player->flags & 0x10) || (player->flags & 0x0100)) {
    hand_state |= 0x01;
  }

  EntityData metadata[] = {
    {
      0,                   // Index (Entity Bit Mask)
      0,                   // Type (Byte)
      { entity_bit_mask }, // Value
    },
    {
      1,                   // Index (Air)
      1,                   // Type (VarInt)
      { .varint = player->air },
    },
    {
      6,        // Index (Pose),
      21,       // Type (Pose),
      { pose }, // Value (Standing)
    },
    {
      8,                   // Living Entity State
      0,                   // Type (Byte)
      { hand_state },      // Value (Is hand active)
    },
    {
      17,                        // Displayed skin parts
      0,                         // Type (Byte)
      { appearance->skin_parts },
    },
    {
      18,                       // Main hand
      0,                        // Type (Byte)
      { appearance->main_hand },
    }
  };

  sc_setEntityMetadata(client_fd, player->client_fd, metadata, 6);
}

void sendPlayerEquipment (int client_fd, PlayerData *player) {
  if (client_fd == -1 || player == NULL) return;
  sc_setEquipment(client_fd, player->client_fd, player);
}

// Broadcasts a player's entity metadata (sneak/sprint state) to other players
void broadcastPlayerMetadata (PlayerData *player) {

  for (int i = 0; i < MAX_PLAYERS; i ++) {
    PlayerData* other_player = &player_data[i];
    int client_fd = other_player->client_fd;

    if (client_fd == -1) continue;
    if (client_fd == player->client_fd) continue;
    if (other_player->flags & 0x20) continue;
    if (!isClientInPlay(client_fd)) continue;

    sendPlayerMetadata(client_fd, player);
  }
}

void broadcastPlayerEquipment (PlayerData *player) {

  for (int i = 0; i < MAX_PLAYERS; i ++) {
    PlayerData* other_player = &player_data[i];
    int client_fd = other_player->client_fd;

    if (client_fd == -1) continue;
    if (client_fd == player->client_fd) continue;
    if (other_player->flags & 0x20) continue;
    if (!isClientInPlay(client_fd)) continue;

    sendPlayerEquipment(client_fd, player);
  }
}

// Sends a mob's entity metadata to the given player.
// If client_fd is -1, broadcasts to all player
// Broadcasts a sound effect from a mob entity to all players in the same dimension.
// Volume 1.0 = normal, pitch 1.0 = normal.
void broadcastMobSound (int entity_id, int sound_id, int category, float volume, float pitch) {
  int mob_index = -entity_id - 2;
  if (mob_index < 0 || mob_index >= MAX_MOBS) return;
  MobData *mob = &mob_data[mob_index];

  for (int i = 0; i < MAX_PLAYERS; i ++) {
    int client_fd = player_data[i].client_fd;
    if (client_fd == -1) continue;
    if (player_data[i].dimension != mob->dimension) continue;
    if (!isClientInPlay(client_fd)) continue;

    sc_soundEntity(client_fd, sound_id, category, entity_id, volume, pitch);
  }
}

uint32_t getVillagerTradeExperience (int mob_index) {
  if (mob_index < 0 || mob_index >= MAX_MOBS) return 0;
  if (mob_data[mob_index].type != E_VILLAGER) return 0;

  uint32_t total = 0;
  for (uint8_t i = 0; i < 5; i++) total += mob_trade_uses[mob_index][i];
  return total;
}

uint8_t getVillagerTradeLevel (int mob_index) {
  uint32_t xp = getVillagerTradeExperience(mob_index);
  if (xp >= 12) return 5; // Master
  if (xp >= 10) return 4; // Expert
  if (xp >= 5) return 3;  // Journeyman
  if (xp >= 2) return 2;  // Apprentice
  return 1;               // Novice
}

uint32_t getVillagerTradeDisplayExperience (int mob_index) {
  uint32_t raw_xp = getVillagerTradeExperience(mob_index);
  uint8_t level = getVillagerTradeLevel(mob_index);

  static const uint32_t raw_level_xp[5] = {0, 2, 5, 10, 12};
  static const uint32_t vanilla_level_xp[5] = {0, 10, 70, 150, 250};

  if (level >= 5) return vanilla_level_xp[4];

  uint32_t raw_min = raw_level_xp[level - 1];
  uint32_t raw_next = raw_level_xp[level];
  uint32_t vanilla_min = vanilla_level_xp[level - 1];
  uint32_t vanilla_next = vanilla_level_xp[level];

  if (raw_xp < raw_min) raw_xp = raw_min;
  if (raw_xp > raw_next) raw_xp = raw_next;

  uint32_t raw_span = raw_next - raw_min;
  uint32_t vanilla_span = vanilla_next - vanilla_min;
  if (raw_span == 0) return vanilla_min;

  return vanilla_min + ((raw_xp - raw_min) * vanilla_span) / raw_span;
}

void broadcastMobMetadata (int client_fd, int entity_id) {

  int mob_index = -entity_id - 2;
  if (mob_index < 0 || mob_index >= MAX_MOBS) return;
  MobData *mob = &mob_data[mob_index];

  EntityData *metadata;
  size_t length;

  switch (mob->type) {
    case 106: // Sheep
      if (!((mob->data >> 5) & 1)) // Don't send metadata if sheep isn't sheared
        return;

      metadata = malloc(sizeof *metadata);
      metadata[0] = (EntityData){
        17,                // Index (Sheep Bit Mask),
        0,                 // Type (Byte),
        { (uint8_t)0x10 }, // Value
      };
      length = 1;

      break;

    case E_ENDERMAN:
      metadata = malloc(sizeof *metadata);
      metadata[0] = (EntityData){
        17,                    // Enderman screaming flag
        8,                     // Type (Boolean)
        { (uint8_t)(mob->anger_timer > 0 ? 0x01 : 0x00) },
      };
      length = 1;
      break;

    case E_CREEPER:
      metadata = malloc(sizeof *metadata);
      metadata[0] = (EntityData){
        16,                    // Creeper state (ignited flag)
        1,                     // Type (VarInt)
        { .varint = mob->move_timer > 0 ? 1 : -1 }, // 1 = ignited, -1 = idle
      };
      length = 1;
      break;

    case E_VILLAGER:
    {
      // Map codebase profession ID to Minecraft villager profession ID
      static const uint8_t prof_map[] = {5, 9, 4, 1, 2, 3, 6, 7, 8, 10, 12, 13, 14};
      uint8_t mc_prof = (mob->profession < sizeof(prof_map)) ? prof_map[mob->profession] : 0;

      // Determine villager biome type from the chunk biome
      uint8_t biome_type = 2; // default: plains
      uint8_t chunk_biome = getChunkBiome(div_floor((int)mob->x, 32), div_floor((int)mob->z, 32));
      if (chunk_biome == W_desert) biome_type = 0;
      else if (chunk_biome == W_jungle) biome_type = 1;
      else if (chunk_biome == W_savanna) biome_type = 3;
      else if (chunk_biome == W_snowy_plains || chunk_biome == W_snowy_taiga) biome_type = 4;
      else if (chunk_biome == W_swamp) biome_type = 5;
      else if (chunk_biome == W_taiga) biome_type = 6;

      // Build villager metadata entry: index(18), type(19=villager_data),
      // then three VarInts: biomeType, profession, level, terminator(0xFF)
      uint8_t raw[16];
      int pos = 0;
      raw[pos++] = 18;  // index (18 = villager_data)
      // Manually encode VarInts into the buffer
      uint32_t vals[] = {19, biome_type, mc_prof, getVillagerTradeLevel(mob_index)};
      for (int vi = 0; vi < 4; vi++) {
        uint32_t v = vals[vi];
        while (1) {
          if ((v & ~0x7F) == 0) { raw[pos++] = (uint8_t)v; break; }
          raw[pos++] = (uint8_t)((v & 0x7F) | 0x80);
          v >>= 7;
        }
      }
      raw[pos++] = 0xFF;

      // Send the packet to the right player(s)
      int broadcast_fd = -2;
      while (1) {
        int target_fd;
        if (client_fd == -1) {
          // Select next player
          broadcast_fd++;
          if (broadcast_fd >= MAX_PLAYERS) break;
          PlayerData *p = &player_data[broadcast_fd];
          target_fd = p->client_fd;
          if (target_fd == -1) continue;
          if (p->flags & 0x20) continue;
          if (p->dimension != mob->dimension) continue;
          if (!isClientInPlay(target_fd)) continue;
        } else {
          target_fd = client_fd;
        }
        startPacket(target_fd, 0x5C);
        writeVarInt(target_fd, entity_id);
        send_all(target_fd, raw, pos);
        endPacket(target_fd);
        if (client_fd != -1) break;
      }
      return;
    }

    default: return;
  }

  if (client_fd == -1) {
    for (int i = 0; i < MAX_PLAYERS; i ++) {
      PlayerData* player = &player_data[i];
      client_fd = player->client_fd;

      if (client_fd == -1) continue;
      if (player->flags & 0x20) continue;
      if (player->dimension != mob->dimension) continue;
      if (!isClientInPlay(client_fd)) continue;

      sc_setEntityMetadata(client_fd, entity_id, metadata, length);
    }
  } else {
    sc_setEntityMetadata(client_fd, entity_id, metadata, length);
  }

  free(metadata);
}

#define BLOCK_LOOKUP_CACHE_SIZE 4096

typedef struct {
  short x;
  short z;
  int16_t y;
  uint8_t dimension;
  uint16_t block;
  uint32_t epoch;
} BlockLookupCacheEntry;

static THREAD_LOCAL BlockLookupCacheEntry block_lookup_cache[BLOCK_LOOKUP_CACHE_SIZE];
static volatile uint32_t block_lookup_cache_epoch = 1;

// Fast hash table for direct block change lookups (avoids O(n) scan).
// Global (not thread-local) — races only cause occasional cache misses,
// never corruption, because the linear scan fallback is the source of truth.
// Epoch-based invalidation: every block mutation bumps the epoch, so stale
// entries from other threads are automatically ignored.
#define BLOCKCHANGE_HASH_SIZE 4096
#define BLOCKCHANGE_HASH_MASK (BLOCKCHANGE_HASH_SIZE - 1)
static int16_t blockchange_hash_x[BLOCKCHANGE_HASH_SIZE];
static int16_t blockchange_hash_z[BLOCKCHANGE_HASH_SIZE];
static int16_t blockchange_hash_y[BLOCKCHANGE_HASH_SIZE];
static uint8_t blockchange_hash_dim[BLOCKCHANGE_HASH_SIZE];
static uint16_t blockchange_hash_block[BLOCKCHANGE_HASH_SIZE];
static volatile uint32_t blockchange_hash_epoch = 1;
static uint32_t blockchange_hash_ages[BLOCKCHANGE_HASH_SIZE];

#define MODIFIED_CHUNK_TABLE_SIZE 16384
typedef struct {
  int x;
  int z;
} ModifiedChunkEntry;

static ModifiedChunkEntry modified_chunk_table[MODIFIED_CHUNK_TABLE_SIZE];
static uint8_t modified_chunk_table_initialized = 0;
static pthread_mutex_t block_changes_mutex = PTHREAD_MUTEX_INITIALIZER;

static inline uint32_t coord_hash(int x, int z) {
  uint32_t h = (uint32_t)x * 374761393u + (uint32_t)z * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}

static inline uint32_t block_lookup_hash(short x, int16_t y, short z, uint8_t dimension) {
  uint32_t h = coord_hash((int)x, (int)z);
  h ^= (uint32_t)(uint16_t)y * 2246822519u;
  h ^= (uint32_t)dimension * 3266489917u;
  return h;
}

static void clear_modified_chunk_table(void) {
  for (int i = 0; i < MODIFIED_CHUNK_TABLE_SIZE; i++) {
    modified_chunk_table[i].x = INT32_MIN;
    modified_chunk_table[i].z = INT32_MIN;
  }
}

static inline void ensure_modified_chunk_table_initialized(void) {
  if (modified_chunk_table_initialized) return;
  clear_modified_chunk_table();
  modified_chunk_table_initialized = 1;
}

static inline void invalidate_block_lookup_cache(void) {
  // Epoch bump invalidates all thread-local lookup entries lazily.
  block_lookup_cache_epoch++;
  if (block_lookup_cache_epoch == 0) block_lookup_cache_epoch = 1;
  // Also bump the hash table epoch to force re-hash on next lookups.
  blockchange_hash_epoch++;
  if (blockchange_hash_epoch == 0) blockchange_hash_epoch = 1;
}

static void mark_modified_chunk(int chunk_x, int chunk_z) {
  ensure_modified_chunk_table_initialized();

  uint32_t mask = MODIFIED_CHUNK_TABLE_SIZE - 1;
  uint32_t base = coord_hash(chunk_x, chunk_z) & mask;

  for (uint32_t probe = 0; probe < MODIFIED_CHUNK_TABLE_SIZE; probe++) {
    uint32_t idx = (base + probe) & mask;
    ModifiedChunkEntry *entry = &modified_chunk_table[idx];

    if (entry->x == chunk_x && entry->z == chunk_z) return;
    if (entry->x == INT32_MIN) {
      entry->x = chunk_x;
      entry->z = chunk_z;
      return;
    }
  }

  // Table saturated: overwrite home slot as a bounded fallback.
  modified_chunk_table[base].x = chunk_x;
  modified_chunk_table[base].z = chunk_z;
}

uint8_t isChunkModified (int chunk_x, int chunk_z) {
  ensure_modified_chunk_table_initialized();

  uint32_t mask = MODIFIED_CHUNK_TABLE_SIZE - 1;
  uint32_t base = coord_hash(chunk_x, chunk_z) & mask;

  for (uint32_t probe = 0; probe < MODIFIED_CHUNK_TABLE_SIZE; probe++) {
    uint32_t idx = (base + probe) & mask;
    ModifiedChunkEntry *entry = &modified_chunk_table[idx];

    if (entry->x == chunk_x && entry->z == chunk_z) return true;
    if (entry->x == INT32_MIN) return false;
  }

  return false;
}

void rebuildBlockChangeIndexes (void) {
  ensure_modified_chunk_table_initialized();
  clear_modified_chunk_table();

  for (int i = 0; i < block_changes_count; i++) {
    if (block_changes[i].block == 0xFF) continue;
    int chunk_x = div_floor(block_changes[i].x, 16);
    int chunk_z = div_floor(block_changes[i].z, 16);
    mark_modified_chunk(chunk_x, chunk_z);
  }

  invalidate_block_lookup_cache();
}

BlockChange *copyBlockChangesSnapshot (int *count) {
  if (count == NULL) return NULL;

  pthread_mutex_lock(&block_changes_mutex);
  *count = block_changes_count;

  if (*count <= 0) {
    pthread_mutex_unlock(&block_changes_mutex);
    *count = 0;
    return NULL;
  }

  BlockChange *snapshot = (BlockChange *)malloc((size_t)(*count) * sizeof(BlockChange));
  if (snapshot != NULL) {
    memcpy(snapshot, block_changes, (size_t)(*count) * sizeof(BlockChange));
  }
  pthread_mutex_unlock(&block_changes_mutex);

  if (snapshot == NULL) {
    fprintf(stderr, "ERROR: Failed to allocate block change snapshot\n");
    *count = 0;
  }

  return snapshot;
}

void freeBlockChangesSnapshot (BlockChange *snapshot) {
  free(snapshot);
}

// Insert or update an entry in the block change hash table.
static void blockchange_hash_set(short x, int16_t y, short z, uint8_t dimension, uint16_t block) {
  uint32_t h = block_lookup_hash(x, y, z, dimension);
  uint32_t idx = h & BLOCKCHANGE_HASH_MASK;
  blockchange_hash_x[idx] = x;
  blockchange_hash_z[idx] = z;
  blockchange_hash_y[idx] = y;
  blockchange_hash_dim[idx] = dimension;
  blockchange_hash_block[idx] = block;
  blockchange_hash_ages[idx] = blockchange_hash_epoch;
}

// Remove an entry from the block change hash table.
static void blockchange_hash_remove(short x, int16_t y, short z, uint8_t dimension) {
  uint32_t h = block_lookup_hash(x, y, z, dimension);
  uint32_t idx = h & BLOCKCHANGE_HASH_MASK;
  if (blockchange_hash_x[idx] == x && blockchange_hash_z[idx] == z &&
      blockchange_hash_y[idx] == y && blockchange_hash_dim[idx] == dimension &&
      blockchange_hash_ages[idx] == blockchange_hash_epoch) {
    blockchange_hash_block[idx] = 0xFF;
  }
}

// Look up a block in the hash table. Returns 0xFF if not found.
static inline uint16_t blockchange_hash_get(short x, int16_t y, short z, uint8_t dimension) {
  uint32_t h = block_lookup_hash(x, y, z, dimension);
  uint32_t idx = h & BLOCKCHANGE_HASH_MASK;
  if (blockchange_hash_ages[idx] == blockchange_hash_epoch &&
      blockchange_hash_x[idx] == x && blockchange_hash_z[idx] == z &&
      blockchange_hash_y[idx] == y && blockchange_hash_dim[idx] == dimension) {
    return blockchange_hash_block[idx];
  }
  return 0xFF;
}

static inline void notify_block_change_mutation(short x, short z, uint8_t dimension) {
  int chunk_x = div_floor(x, 16);
  int chunk_z = div_floor(z, 16);
  mark_modified_chunk(chunk_x, chunk_z);
  invalidate_cached_chunk(chunk_x, chunk_z, dimension);
  invalidate_block_lookup_cache();
  invalidate_water_surface_cache();
}

uint16_t getBlockChange (short x, int16_t y, short z, uint8_t dimension) {
  uint32_t epoch = block_lookup_cache_epoch;
  uint32_t cache_idx = block_lookup_hash(x, y, z, dimension) & (BLOCK_LOOKUP_CACHE_SIZE - 1);
  BlockLookupCacheEntry *cached = &block_lookup_cache[cache_idx];
  if (
    cached->epoch == epoch &&
    cached->x == x &&
    cached->y == y &&
    cached->z == z &&
    cached->dimension == dimension
  ) {
    return cached->block;
  }

  // Fast path: thread-local hash table for O(1) block change lookup
  {
    uint16_t fast_block = blockchange_hash_get(x, y, z, dimension);
    if (fast_block != 0xFF) {
      cached->x = x;
      cached->y = y;
      cached->z = z;
      cached->dimension = dimension;
      cached->block = fast_block;
      cached->epoch = epoch;
      return fast_block;
    }
  }

  // Fallback: linear scan (only when hash table misses)
  uint16_t block = 0xFF;
  for (int i = 0; i < block_changes_count; i ++) {
    if (block_changes[i].block == 0xFF) continue;
    if (
      block_changes[i].x == x &&
      block_changes[i].y == y &&
      block_changes[i].z == z &&
      block_changes[i].dimension == dimension
    ) {
      block = block_changes[i].block;
      break;
    }
    #ifdef ALLOW_DOORS
    // Skip door upper half and state data
    if (is_door_block(block_changes[i].block)) {
      // Check if upper half matches before skipping
      if (i + 1 < block_changes_count &&
          block_changes[i + 1].x == x && block_changes[i + 1].y == y && block_changes[i + 1].z == z &&
          block_changes[i + 1].dimension == dimension) {
        block = block_changes[i + 1].block;
        break;
      }
      i += 2;
      continue;
    }
    #endif
    // Skip stair, furnace, ender_chest, fence, horizontal facing, and lantern state data
    if (is_stair_block(block_changes[i].block) || block_changes[i].block == B_furnace || block_changes[i].block == B_ender_chest || is_fence_block(block_changes[i].block) || is_horizontal_facing_block(block_changes[i].block) || block_changes[i].block == B_lantern) {
      i += 1;
    }
    // Skip chest/barrel inventory entries
    if (block_changes[i].block == B_chest || block_changes[i].block == B_barrel) {
      if (i + 14 >= block_changes_count) break;
      i += 14;
    }
  }

  // Cache the result in the hash table for future lookups
  if (block != 0xFF) {
    blockchange_hash_set(x, y, z, dimension, block);
  }

  cached->x = x;
  cached->y = y;
  cached->z = z;
  cached->dimension = dimension;
  cached->block = block;
  cached->epoch = epoch;

  return block;
}

// Handle running out of memory for new block changes
void failBlockChange (short x, int16_t y, short z, uint16_t block) {

  // Get previous block at this location
  uint16_t before = getBlockAt(x, y, z);

  // Broadcast a new update to all players
  for (int i = 0; i < MAX_PLAYERS; i ++) {
    if (player_data[i].client_fd == -1) continue;
    if (player_data[i].flags & 0x20) continue;
    // Reset the block they tried to change
    sc_blockUpdate(player_data[i].client_fd, x, y, z, before);
    // Broadcast a chat message warning about the limit
    sc_systemChat(player_data[i].client_fd, "Block changes limit exceeded. Restore original terrain to continue.", 67);
  }

}

uint8_t makeBlockChange (short x, int16_t y, short z, uint16_t block, uint8_t dimension) {

  // Transmit block update to all in-game clients
  for (int i = 0; i < MAX_PLAYERS; i ++) {
    if (player_data[i].client_fd == -1) continue;
    if (player_data[i].flags & 0x20) continue;
    sc_blockUpdate(player_data[i].client_fd, x, y, z, block);
  }

  // Calculate terrain at these coordinates and compare it to the input block.
  // Since block changes get overlayed on top of terrain, we don't want to
  // store blocks that don't differ from the base terrain.
  ChunkAnchor anchor = {
    x / CHUNK_SIZE,
    z / CHUNK_SIZE
  };
  if (x % CHUNK_SIZE < 0) anchor.x --;
  if (z % CHUNK_SIZE < 0) anchor.z --;
  anchor.hash = getChunkHash(anchor.x, anchor.z);
  anchor.biome = getChunkBiome(anchor.x, anchor.z);

  // base-terrain comparison is meaningless for the nether (getTerrainAt returns overworld)
  uint8_t is_base_block = (dimension != DIMENSION_NETHER) && (block == getTerrainAt(x, y, z, anchor));
  int write_from = -1;
  int write_to = -1;

  // Helper macro to clear old special block entries at a position
  #define CLEAR_OLD_SPECIAL_ENTRIES(idx) \
    do { \
      if (block_changes[idx].block == B_chest || block_changes[idx].block == B_barrel) { \
        for (int j = 1; j < 15 && (idx) + j < block_changes_count; j++) block_changes[(idx) + j].block = 0xFF; \
      } else if (is_door_block(block_changes[idx].block)) { \
        if ((idx) + 1 < block_changes_count) block_changes[(idx) + 1].block = 0xFF; \
        if ((idx) + 2 < block_changes_count) block_changes[(idx) + 2].block = 0xFF; \
      } else if (is_stair_block(block_changes[idx].block) || block_changes[idx].block == B_furnace || block_changes[idx].block == B_ender_chest || is_fence_block(block_changes[idx].block) || is_horizontal_facing_block(block_changes[idx].block) || block_changes[idx].block == B_lantern) { \
        if ((idx) + 1 < block_changes_count) block_changes[(idx) + 1].block = 0xFF; \
      } \
    } while (0)

  pthread_mutex_lock(&block_changes_mutex);

  // In the block_changes array, 0xFF indicates a missing/restored entry.
  // We track the position of the first such "gap" for when the operation
  // isn't replacing an existing block change.
  int first_gap = block_changes_count;

  // Prioritize replacing entries with matching coordinates
  // This prevents having conflicting entries for one set of coordinates
  for (int i = 0; i < block_changes_count; i ++) {
    if (block_changes[i].block == 0xFF) {
      if (first_gap == block_changes_count) first_gap = i;
      continue;
    }
    if (
      block_changes[i].x == x &&
      block_changes[i].y == y &&
      block_changes[i].z == z &&
      block_changes[i].dimension == dimension
    ) {
      // Clear any old special block state entries
      CLEAR_OLD_SPECIAL_ENTRIES(i);

      if ((block == B_chest || block == B_barrel) && block_changes[i].block != block) {
        block_changes[i].block = 0xFF;
        if (is_base_block) special_block_clear(x, y, z, dimension);
        continue;
      }

      if (is_base_block) {
        block_changes[i].block = 0xFF;
        special_block_clear(x, y, z, dimension);
      } else {
        block_changes[i].block = block;
        block_changes[i].dimension = dimension;
        // Initialize default state for special blocks
        if (is_door_block(block)) {
          special_block_set_state(x, y, z, dimension, block, door_encode_state(0, 0, 0));
        } else if (is_stair_block(block)) {
          special_block_set_state(x, y, z, dimension, block, stair_encode_state(0, 0));
        } else if (block == B_furnace) {
          special_block_set_state(x, y, z, dimension, block, furnace_encode_state(0, 0));
        } else if (block == B_chest || block == B_barrel) {
          memset(&block_changes[i + 1], 0, 14 * sizeof(BlockChange));
          if (block == B_barrel) {
            special_block_set_state(x, y, z, dimension, block, barrel_encode_state(0, 0));
          } else {
            special_block_set_state(x, y, z, dimension, block, oriented_encode_state(0));
          }
        } else if (block == B_ender_chest) {
          // ender chest uses one state entry (no inventory storage here)
          block_changes[i + 1].x = 0;
          block_changes[i + 1].y = 0;
          block_changes[i + 1].z = z;
          block_changes[i + 1].block = 0;
          special_block_set_state(x, y, z, dimension, block, ender_chest_encode_state(0, 0));
        } else if (is_fence_block(block)) {
          // Compute fence connections from neighbors
          uint8_t _fn = 0, _fe = 0, _fs = 0, _fw = 0;
          uint16_t _fb;
          _fb = getBlockAt2(x - 1, y, z, dimension); if (isFenceSolidBlock(_fb)) _fw = 1;
          _fb = getBlockAt2(x + 1, y, z, dimension); if (isFenceSolidBlock(_fb)) _fe = 1;
          _fb = getBlockAt2(x, y, z - 1, dimension); if (isFenceSolidBlock(_fb)) _fn = 1;
          _fb = getBlockAt2(x, y, z + 1, dimension); if (isFenceSolidBlock(_fb)) _fs = 1;
          special_block_set_state(x, y, z, dimension, block, fence_encode_state(_fn, _fe, _fs, _fw));
        } else if (is_horizontal_facing_block(block)) {
          special_block_set_state(x, y, z, dimension, block, horizontal_facing_encode_state(0));
        } else if (block == B_wheat) {
          // B_wheat uses 1 slot; state is tracked only in special_blocks table.
        }
      }
      write_from = i;
      write_to = i;
      pthread_mutex_unlock(&block_changes_mutex);
      #ifndef DISK_SYNC_BLOCKS_ON_INTERVAL
      writeBlockChangesToDisk(write_from, write_to);
      #endif
      notify_block_change_mutation(x, z, dimension);
      return 0;
    }
    // Skip extra entries for special blocks during search
    if (block_changes[i].block == B_chest || block_changes[i].block == B_barrel) { if (i + 14 >= block_changes_count) break; i += 14; continue; }
    #ifdef ALLOW_DOORS
    if (is_door_block(block_changes[i].block)) { if (i + 2 >= block_changes_count) break; i += 2; continue; }
    #endif
    if (is_stair_block(block_changes[i].block) || block_changes[i].block == B_furnace || block_changes[i].block == B_ender_chest || is_fence_block(block_changes[i].block) || is_horizontal_facing_block(block_changes[i].block) || block_changes[i].block == B_lantern) { i += 1; }
  }

  // Don't create a new entry if it contains the base terrain block
  if (is_base_block) {
    pthread_mutex_unlock(&block_changes_mutex);
    return 0;
  }

  // Determine how many block_changes entries this block needs
  int slots_needed = 1;
  if (block == B_chest || block == B_barrel) slots_needed = 15;
  else if (block == B_ender_chest) slots_needed = 2;
  #ifdef ALLOW_DOORS
  else if (is_door_block(block)) slots_needed = 3;
  #endif
  else if (is_stair_block(block) || block == B_furnace || is_fence_block(block) || is_horizontal_facing_block(block) || block == B_lantern) slots_needed = 2;

  // Find or create a gap of sufficient size
  int last_real_entry = first_gap - 1;
  for (int i = first_gap; i <= block_changes_count + slots_needed; i ++) {
    #ifdef INFINITE_BLOCK_CHANGES
    if (i >= block_changes_capacity - slots_needed) {
      if (!config.infinite_block_changes) break;
      int new_capacity = block_changes_capacity * 2;
      if (new_capacity <= block_changes_capacity) {
        new_capacity = block_changes_capacity + slots_needed + 1;
      }
      BlockChange *new_block_changes = (BlockChange *)realloc(block_changes, new_capacity * sizeof(BlockChange));
      if (!new_block_changes) {
        pthread_mutex_unlock(&block_changes_mutex);
        failBlockChange(x, y, z, block);
        return 1;
      }
      block_changes = new_block_changes;
      for (int j = block_changes_capacity; j < new_capacity; j ++) block_changes[j].block = 0xFF;
      terminal_ui_log("Block changes expanded: %d -> %d", block_changes_capacity, new_capacity);
      block_changes_capacity = new_capacity;
    }
    #else
    if (i >= MAX_BLOCK_CHANGES) break;
    #endif

    if (block_changes[i].block != 0xFF) {
      last_real_entry = i;
      continue;
    }
    if (i - last_real_entry != slots_needed) continue;

    // Found a sufficient gap -- place the block
    int base = last_real_entry + 1;
    block_changes[base].x = x;
    block_changes[base].y = y;
    block_changes[base].z = z;
    block_changes[base].block = block;
    block_changes[base].dimension = dimension;

    if (block == B_chest || block == B_barrel) {
      memset(&block_changes[base + 1], 0, 14 * sizeof(BlockChange));
      if (block == B_barrel) {
        special_block_set_state(x, y, z, dimension, block, barrel_encode_state(0, 0));
      } else {
        special_block_set_state(x, y, z, dimension, block, oriented_encode_state(0));
      }
    } else if (block == B_ender_chest) {
      block_changes[base + 1].x = 0;
      block_changes[base + 1].y = 0;
      block_changes[base + 1].z = z;
      block_changes[base + 1].block = 0;
      special_block_set_state(x, y, z, dimension, block, ender_chest_encode_state(0, 0));
    }
    #ifdef ALLOW_DOORS
    else if (is_door_block(block)) {
      // Upper half
      block_changes[base + 1].x = x;
      block_changes[base + 1].y = y + 1;
      block_changes[base + 1].z = z;
      block_changes[base + 1].block = block;
      block_changes[base + 1].dimension = dimension;
      // State entry
      block_changes[base + 2].x = 0;
      block_changes[base + 2].y = 0;
      block_changes[base + 2].z = z;
      block_changes[base + 2].block = 0;
      special_block_set_state(x, y, z, dimension, block, door_encode_state(0, 0, 0));
    }
    #endif
    else if (is_stair_block(block) || block == B_furnace || is_fence_block(block) || is_horizontal_facing_block(block)) {
      // State entry
      block_changes[base + 1].x = 0;
      block_changes[base + 1].y = 0;
      block_changes[base + 1].z = z;
      block_changes[base + 1].block = 0;
      if (is_stair_block(block)) {
        special_block_set_state(x, y, z, dimension, block, stair_encode_state(0, 0));
      } else if (block == B_furnace) {
        special_block_set_state(x, y, z, dimension, block, furnace_encode_state(0, 0));
      } else if (is_fence_block(block)) {
        // Compute fence connections from neighbors
        uint8_t _fn = 0, _fe = 0, _fs = 0, _fw = 0;
        uint16_t _fb;
        _fb = getBlockAt2(x - 1, y, z, dimension); if (isFenceSolidBlock(_fb)) _fw = 1;
        _fb = getBlockAt2(x + 1, y, z, dimension); if (isFenceSolidBlock(_fb)) _fe = 1;
        _fb = getBlockAt2(x, y, z - 1, dimension); if (isFenceSolidBlock(_fb)) _fn = 1;
        _fb = getBlockAt2(x, y, z + 1, dimension); if (isFenceSolidBlock(_fb)) _fs = 1;
        special_block_set_state(x, y, z, dimension, block, fence_encode_state(_fn, _fe, _fs, _fw));
      } else if (is_horizontal_facing_block(block)) {
        special_block_set_state(x, y, z, dimension, block, horizontal_facing_encode_state(0));
      } else if (block == B_wheat) {
        if (!special_block_has_entry(x, (uint8_t)y, z, dimension)) {
          special_block_set_state(x, y, z, dimension, block, 0);
        }
      } else if (block == B_lantern) {
        block_changes[base + 1].block = 0;
        if (!special_block_has_entry(x, (uint8_t)y, z, dimension)) {
          special_block_set_state(x, y, z, dimension, block, 0);
        }
      }
    }

    if (i >= block_changes_count) block_changes_count = i + 1;
    write_from = base;
    write_to = base + slots_needed - 1;
    pthread_mutex_unlock(&block_changes_mutex);
    #ifndef DISK_SYNC_BLOCKS_ON_INTERVAL
    writeBlockChangesToDisk(write_from, write_to);
    #endif
    notify_block_change_mutation(x, z, dimension);
    return 0;
  }

  pthread_mutex_unlock(&block_changes_mutex);

// If we're here, no changes were made
  failBlockChange(x, y, z, block);
  return 1;
}

// Forward declarations for breakConnectedLeaves
static void breakConnectedLeaves(short x, int16_t y, short z, uint16_t leaf_type, PlayerData *player, int *broken_count);
static void breakFloatingLeaves(short bx, int16_t by, short bz, uint16_t leaf_type, PlayerData *player, int *broken_count);
void playPickupAnimation(PlayerData *player, uint16_t item, double x, double y, double z);
int givePlayerItem(PlayerData *player, uint16_t item, uint8_t count);
uint16_t getMiningResult(uint16_t held_item, uint16_t block);

uint8_t isLeafBlock (uint16_t block) {
  return (
    block == B_oak_leaves ||
    block == B_spruce_leaves ||
    block == B_birch_leaves ||
    block == B_jungle_leaves ||
    block == B_acacia_leaves ||
    block == B_dark_oak_leaves ||
    block == B_cherry_leaves ||
    block == B_mangrove_leaves ||
    block == B_pale_oak_leaves
  );
}

// Recursively break connected leaf blocks of the same type (cap at 6 total)
static void breakConnectedLeaves (short x, int16_t y, short z, uint16_t leaf_type, PlayerData *player, int *broken_count) {
  if (*broken_count >= 6) return;

  // Check all 6 adjacent blocks
  short dx[6] = {1, -1, 0, 0, 0, 0};
  short dz[6] = {0, 0, 1, -1, 0, 0};
  int dy[6] = {0, 0, 0, 0, 1, -1};

  for (int i = 0; i < 6; i++) {
    if (*broken_count >= 6) break;

    short nx = x + dx[i];
    int16_t ny = y + dy[i];
    if (ny < 0 || ny > 319) continue;
    short nz = z + dz[i];

    uint16_t adj = getBlockAt2(nx, ny, nz, player->dimension);
    if (adj == leaf_type) {
      makeBlockChange(nx, ny, nz, 0, player->dimension);
      (*broken_count)++;
      // Drop item from breaking this leaf
      uint16_t held_item = player->inventory_items[player->hotbar];
      uint16_t drop = getMiningResult(held_item, leaf_type);
      if (drop) {
        spawnItemEntity(nx + 0.5, ny + 0.5, nz + 0.5, drop, 1, player->dimension, 0, 0, 0);
      }
      // Recurse into this leaf's neighbors
      breakConnectedLeaves(nx, ny, nz, leaf_type, player, broken_count);
    }
  }
}

// Check if a block is any kind of log
static uint8_t isLogBlock(uint16_t block) {
  return (
    block == B_oak_log ||
    block == B_spruce_log ||
    block == B_birch_log ||
    block == B_jungle_log ||
    block == B_acacia_log ||
    block == B_dark_oak_log ||
    block == B_cherry_log ||
    block == B_mangrove_log ||
    block == B_pale_oak_log
  );
}

// Break floating leaf blocks within a 2-block radius that have no log support
static void breakFloatingLeaves (short bx, int16_t by, short bz, uint16_t leaf_type, PlayerData *player, int *broken_count) {
  // Scan a 5x5x5 area centered on the broken block (radius 2)
  for (short dx = -2; dx <= 2; dx++) {
    for (short dz = -2; dz <= 2; dz++) {
      for (int dy = -2; dy <= 2; dy++) {
        if (*broken_count >= 6) return;

        short nx = bx + dx;
        int16_t ny = by + dy;
        if (ny < 0 || ny > 319) continue;
        short nz = bz + dz;

        // Skip the block that was already broken
        if (dx == 0 && dy == 0 && dz == 0) continue;

        uint16_t adj = getBlockAt2(nx, ny, nz, player->dimension);
        if (adj != leaf_type) continue;

        // Check if this leaf has a log within 2 blocks
        uint8_t has_support = 0;
        for (short lx = -2; lx <= 2 && !has_support; lx++) {
          for (short lz = -2; lz <= 2 && !has_support; lz++) {
            for (int ly = -2; ly <= 2 && !has_support; ly++) {
              if (lx == 0 && ly == 0 && lz == 0) continue;
              if (isLogBlock(getBlockAt2(nx + lx, ny + ly, nz + lz, player->dimension))) {
                has_support = 1;
              }
            }
          }
        }

        // Break the leaf if it has no log support
        if (!has_support) {
          makeBlockChange(nx, ny, nz, 0, player->dimension);
          (*broken_count)++;
          uint16_t held_item = player->inventory_items[player->hotbar];
          uint16_t drop = getMiningResult(held_item, leaf_type);
          if (drop) {
            spawnItemEntity(nx + 0.5, ny + 0.5, nz + 0.5, drop, 1, player->dimension, 0, 0, 0);
          }
        }
      }
    }
  }
}

uint8_t isSaplingBlock (uint16_t block) {
  return (
    block == B_oak_sapling ||
    block == B_spruce_sapling ||
    block == B_birch_sapling ||
    block == B_jungle_sapling ||
    block == B_acacia_sapling ||
    block == B_cherry_sapling ||
    block == B_dark_oak_sapling ||
    block == B_pale_oak_sapling ||
    block == B_mangrove_propagule
  );
}

static uint16_t getLeafItemFromBlock(uint16_t block) {
  switch (block) {
    case B_oak_leaves: return I_oak_leaves;
    case B_spruce_leaves: return I_spruce_leaves;
    case B_birch_leaves: return I_birch_leaves;
    case B_jungle_leaves: return I_jungle_leaves;
    case B_acacia_leaves: return I_acacia_leaves;
    case B_dark_oak_leaves: return I_dark_oak_leaves;
    case B_cherry_leaves: return I_cherry_leaves;
    case B_mangrove_leaves: return I_mangrove_leaves;
    case B_pale_oak_leaves: return I_pale_oak_leaves;
    default: return 0;
  }
}

static uint16_t getSaplingItemFromLeafBlock(uint16_t block) {
  switch (block) {
    case B_oak_leaves: return I_oak_sapling;
    case B_spruce_leaves: return I_spruce_sapling;
    case B_birch_leaves: return I_birch_sapling;
    case B_jungle_leaves: return I_jungle_sapling;
    case B_acacia_leaves: return I_acacia_sapling;
    case B_dark_oak_leaves: return I_dark_oak_sapling;
    case B_cherry_leaves: return I_cherry_sapling;
    case B_mangrove_leaves: return I_mangrove_propagule;
    case B_pale_oak_leaves: return I_pale_oak_sapling;
    default: return 0;
  }
}

static uint8_t leafDropsApple(uint16_t block) {
  return (
    block == B_oak_leaves ||
    block == B_dark_oak_leaves ||
    block == B_pale_oak_leaves
  );
}

static uint8_t isLeafItem(uint16_t item) {
  return (
    item == I_oak_leaves ||
    item == I_spruce_leaves ||
    item == I_birch_leaves ||
    item == I_jungle_leaves ||
    item == I_acacia_leaves ||
    item == I_dark_oak_leaves ||
    item == I_cherry_leaves ||
    item == I_mangrove_leaves ||
    item == I_pale_oak_leaves
  );
}

static uint8_t isSaplingItem(uint16_t item) {
  return (
    item == I_oak_sapling ||
    item == I_spruce_sapling ||
    item == I_birch_sapling ||
    item == I_jungle_sapling ||
    item == I_acacia_sapling ||
    item == I_cherry_sapling ||
    item == I_dark_oak_sapling ||
    item == I_pale_oak_sapling ||
    item == I_mangrove_propagule
  );
}

static uint8_t isPickaxeItem(uint16_t item) {
  return (
    item == I_wooden_pickaxe ||
    item == I_stone_pickaxe ||
    item == I_iron_pickaxe ||
    item == I_golden_pickaxe ||
    item == I_diamond_pickaxe ||
    item == I_netherite_pickaxe
  );
}

static uint8_t isIronTierPickaxe(uint16_t item) {
  // Keep golden pickaxe compatible with this server's prior ore-drop behavior.
  return (
    item == I_iron_pickaxe ||
    item == I_golden_pickaxe ||
    item == I_diamond_pickaxe ||
    item == I_netherite_pickaxe
  );
}

static uint8_t isDiamondTierPickaxe(uint16_t item) {
  return item == I_diamond_pickaxe || item == I_netherite_pickaxe;
}

static uint8_t isShovelItem(uint16_t item) {
  return (
    item == I_wooden_shovel ||
    item == I_stone_shovel ||
    item == I_iron_shovel ||
    item == I_golden_shovel ||
    item == I_diamond_shovel ||
    item == I_netherite_shovel
  );
}

static uint8_t isShearsFastBlock(uint16_t block) {
  return isLeafBlock(block) || (block >= B_white_wool && block <= B_blue_wool) || block == B_cobweb;
}

static uint8_t isIronTierPickaxeDropBlock(uint16_t block) {
  switch (block) {
    case B_gold_ore:
    case B_deepslate_gold_ore:
    case B_redstone_ore:
    case B_diamond_ore:
    case B_emerald_ore:
      return true;
    default:
      return false;
  }
}

static uint8_t isPickaxeDropBlock(uint16_t block) {
  switch (block) {
    case B_stone:
    case B_granite:
    case B_polished_granite:
    case B_diorite:
    case B_polished_diorite:
    case B_andesite:
    case B_polished_andesite:
    case B_cobblestone:
    case B_cobblestone_slab:
    case B_cobblestone_stairs:
    case B_sandstone:
    case B_chiseled_sandstone:
    case B_cut_sandstone:
    case B_calcite:
    case B_terracotta:
    case B_white_terracotta:
    case B_orange_terracotta:
    case B_yellow_terracotta:
    case B_brown_terracotta:
    case B_red_terracotta:
    case B_light_gray_terracotta:
    case B_magma_block:
    case B_furnace:
    case B_dispenser:
    case B_piston:
    case B_sticky_piston:
    case B_powered_rail:
    case B_detector_rail:
    case B_coal_ore:
    case B_deepslate_coal_ore:
    case B_iron_ore:
    case B_deepslate_iron_ore:
    case B_copper_ore:
    case B_lapis_ore:
    case B_deepslate_lapis_ore:
    case B_nether_gold_ore:
    case B_nether_quartz_ore:
    case B_coal_block:
    case B_copper_block:
    case B_iron_block:
    case B_gold_block:
    case B_diamond_block:
    case B_redstone_block:
    case B_lapis_block:
    case B_basalt:
    case B_blackstone:
    case B_gilded_blackstone:
    case B_nether_bricks:
    case B_cracked_nether_bricks:
      return true;
    default:
      return false;
  }
}

// Returns the result of mining a block, taking into account the block type and tools
// Probability numbers obtained with this formula: N = floor(P * (2 ^ 32))
uint16_t getMiningResult (uint16_t held_item, uint16_t block) {
  if (isLeafBlock(block)) {
    uint16_t leaf_item = getLeafItemFromBlock(block);
    uint16_t sapling_item = getSaplingItemFromLeafBlock(block);

    if (held_item == I_shears) return leaf_item;
    // Each drop is rolled independently, matching vanilla behavior
    if (leafDropsApple(block) && fast_rand() < 214748364) return I_apple; // 5%
    if (fast_rand() < 85899345) return I_stick; // 2%
    if (sapling_item != 0 && fast_rand() < 214748364) return sapling_item; // 5%
    return 0;
  }

  if (block == B_obsidian || block == B_ancient_debris) {
    if (!isDiamondTierPickaxe(held_item)) return 0;
  } else if (isIronTierPickaxeDropBlock(block)) {
    if (!isIronTierPickaxe(held_item)) return 0;
  } else if (isPickaxeDropBlock(block)) {
    if (!isPickaxeItem(held_item)) return 0;
  }

  if ((block == B_snow || block == B_snow_block) && !isShovelItem(held_item)) return 0;

  if (isWheatBlock(block)) return 0; // Handled in handlePlayerAction with age check

  switch (block) {
    case B_gravel:
    case B_suspicious_gravel:
      // 50% chance to drop flint instead of gravel
      if (fast_rand() % 2 == 0) return I_flint;
      return I_gravel;

    case B_soul_sand: return I_soul_sand;
    case B_soul_soil: return I_soul_soil;

    case B_short_grass:
    case B_fern:
      if (fast_rand() % 8 == 0) return I_wheat_seeds; // 12.5% chance
      return B_to_I[block];

    default: break;
  }

  return B_to_I[block];

}


// Checks whether the given block would be mined instantly with the held tool
uint8_t isInstantlyMined (PlayerData *player, uint16_t block) {

  uint16_t held_item = player->inventory_items[player->hotbar];

  if (block == B_snow || block == B_snow_block) return isShovelItem(held_item);

  if (block == B_soul_sand || block == B_soul_soil) return isShovelItem(held_item);

  if (isShearsFastBlock(block)) return held_item == I_shears;

  return (
    block == B_dead_bush ||
    block == B_bush ||
    block == B_fern ||
    block == B_short_grass ||
    block == B_short_dry_grass ||
    block == B_tall_dry_grass ||
    isWheatBlock(block) ||
    isSaplingBlock(block) ||
    block == B_dandelion ||
    block == B_torchflower ||
    block == B_poppy ||
    block == B_blue_orchid ||
    block == B_allium ||
    block == B_azure_bluet ||
    block == B_red_tulip ||
    block == B_orange_tulip ||
    block == B_white_tulip ||
    block == B_pink_tulip ||
    block == B_oxeye_daisy ||
    block == B_cornflower ||
    block == B_wither_rose ||
    block == B_lily_of_the_valley ||
    block == B_lily_pad ||
    block == B_wall_torch ||
    block == B_torch
  );

}

// Checks whether the given block has to have something beneath it
uint8_t isColumnBlock (uint16_t block) {
  return (
    block == B_snow ||
    block == B_moss_carpet ||
    block == B_cactus ||
    block == B_short_grass ||
    block == B_short_dry_grass ||
    block == B_tall_dry_grass ||
    block == B_fern ||
    isWheatBlock(block) ||
    block == B_dead_bush ||
    block == B_bush ||
    block == B_wall_torch ||
    block == B_torch ||
    isSaplingBlock(block) ||
    block == B_dandelion ||
    block == B_torchflower ||
    block == B_poppy ||
    block == B_blue_orchid ||
    block == B_allium ||
    block == B_azure_bluet ||
    block == B_red_tulip ||
    block == B_orange_tulip ||
    block == B_white_tulip ||
    block == B_pink_tulip ||
    block == B_oxeye_daisy ||
    block == B_cornflower ||
    block == B_wither_rose ||
    block == B_lily_of_the_valley
  );
}

// Checks whether the given block is non-solid
// Note: This function doesn't take coordinates, so for doors we check the block type only
// For coordinate-aware passability, use isDoorOpen() separately
uint8_t isPassableBlock (uint16_t block) {
  return (
    block == B_air ||
    (block >= B_water && block < B_water + 8) ||
    (block >= B_lava && block < B_lava + 4) ||
    block == B_snow ||
    block == B_moss_carpet ||
    block == B_short_grass ||
    block == B_short_dry_grass ||
    block == B_tall_dry_grass ||
    block == B_fern ||
    isWheatBlock(block) ||
    block == B_dead_bush ||
    block == B_bush ||
    block == B_seagrass ||
    block == B_wall_torch ||
    block == B_torch ||
    isSaplingBlock(block) ||
    block == B_dandelion ||
    block == B_torchflower ||
    block == B_poppy ||
    block == B_blue_orchid ||
    block == B_allium ||
    block == B_azure_bluet ||
    block == B_red_tulip ||
    block == B_orange_tulip ||
    block == B_white_tulip ||
    block == B_pink_tulip ||
    block == B_oxeye_daisy ||
    block == B_cornflower ||
    block == B_wither_rose ||
    block == B_lily_of_the_valley ||
    block == B_campfire ||
    block == B_ladder
  );
}

// Checks whether the given block is non-solid and spawnable
uint8_t isPassableSpawnBlock (uint16_t block) {
    if ((block >= B_water && block < B_water + 8) ||
        (block >= B_lava && block < B_lava + 4))
    {
        return 0;
    }
    return isPassableBlock(block);
}

// Checks if a block is water
uint8_t isWaterBlock(uint16_t block) {
    return (block >= B_water && block < B_water + 8);
}

// Checks whether the given block can be replaced by another block
uint8_t isReplaceableBlock (uint16_t block) {
  return (
    block == B_air ||
    (block >= B_water && block < B_water + 8) ||
    (block >= B_lava && block < B_lava + 4) ||
    block == B_short_grass ||
    block == B_short_dry_grass ||
    block == B_tall_dry_grass ||
    block == B_snow
  );
}

uint8_t isReplaceableFluid (uint16_t block, uint8_t level, uint16_t fluid) {
  if (block >= fluid && block - fluid < 8) {
    return block - fluid > level;
  }
  return isReplaceableBlock(block);
}

// Checks whether the given item can be used in a composter
// Returns the probability (out of 2^32) to return bone meal
uint32_t isCompostItem (uint16_t item) {

  // Output values calculated using the following formula:
  // P = 2^32 / (7 / compost_chance)

  if ( // Compost chance: 30%
    isLeafItem(item) ||
    item == I_short_grass ||
    item == I_wheat_seeds ||
    isSaplingItem(item) ||
    item == I_moss_carpet
  ) return 184070026;

  if ( // Compost chance: 50%
    item == I_cactus ||
    item == I_sugar_cane
  ) return 306783378;

  if ( // Compost chance: 65%
    item == I_apple ||
    item == I_lily_pad
  ) return 398818392;

  return 0;
}

// Maps raw food items to their campfire-cooked counterpart.
// Returns 0 if the item is not cookable.
static uint16_t getCampfireCookedItem (uint16_t item) {
  switch (item) {
    case I_chicken: return I_cooked_chicken;
    case I_beef: return I_cooked_beef;
    case I_porkchop: return I_cooked_porkchop;
    case I_mutton: return I_cooked_mutton;
    case I_cod: return I_cooked_cod;
    case I_salmon: return I_cooked_salmon;
    case I_rabbit: return I_cooked_rabbit;
    case I_potato: return I_baked_potato;
    case I_kelp: return I_dried_kelp;
    default: return 0;
  }
}

// Returns the maximum stack size of an item
uint8_t getItemStackSize (uint16_t item) {

  if (
    // Pickaxes
    item == I_wooden_pickaxe ||
    item == I_stone_pickaxe ||
    item == I_iron_pickaxe ||
    item == I_golden_pickaxe ||
    item == I_diamond_pickaxe ||
    item == I_netherite_pickaxe ||
    // Axes
    item == I_wooden_axe ||
    item == I_stone_axe ||
    item == I_iron_axe ||
    item == I_golden_axe ||
    item == I_diamond_axe ||
    item == I_netherite_axe ||
    // Shovels
    item == I_wooden_shovel ||
    item == I_stone_shovel ||
    item == I_iron_shovel ||
    item == I_golden_shovel ||
    item == I_diamond_shovel ||
    item == I_netherite_shovel ||
    // Swords
    item == I_wooden_sword ||
    item == I_stone_sword ||
    item == I_iron_sword ||
    item == I_golden_sword ||
    item == I_diamond_sword ||
    item == I_netherite_sword ||
    // Hoes
    item == I_wooden_hoe ||
    item == I_stone_hoe ||
    item == I_iron_hoe ||
    item == I_golden_hoe ||
    item == I_diamond_hoe ||
    item == I_netherite_hoe ||
    // Utility tools and weapons
    item == I_shears ||
    item == I_fishing_rod ||
    item == I_bow ||
    item == I_crossbow ||
    item == I_shield ||
    item == I_flint_and_steel ||
    item == I_trident ||
    item == I_mace ||
    item == I_brush ||
    item == I_carrot_on_a_stick ||
    item == I_warped_fungus_on_a_stick ||
    item == I_elytra ||
    // Filled buckets
    item == I_water_bucket ||
    item == I_lava_bucket ||
    // Beds
    (item >= I_white_bed && item <= I_black_bed)
  ) return 1;

  if (
    item == I_snowball ||
    item == I_bucket
  ) return 16;

  return 64;
}

#ifdef ALLOW_DOORS
/* Legacy wrappers for compatibility with existing code that calls these.
 * All actual logic is in special_block.c */
uint8_t isDoorBlock (uint16_t block) { return is_door_block(block); }
uint8_t isStairBlock (uint16_t block) { return is_stair_block(block); }
uint8_t isTrapdoorBlock (uint16_t block) { return is_trapdoor_block(block); }
uint8_t isOrientedBlock (uint16_t block) { return is_oriented_block(block); }
uint8_t isFenceBlock (uint16_t block) { return is_fence_block(block); }
uint8_t isHorizontalFacingBlock (uint16_t block) { return is_horizontal_facing_block(block); }
uint8_t isDoorItem (uint16_t item) {
  return (
    item == I_oak_door ||
    item == I_spruce_door ||
    item == I_birch_door ||
    item == I_jungle_door ||
    item == I_acacia_door ||
    item == I_cherry_door ||
    item == I_dark_oak_door ||
    item == I_pale_oak_door ||
    item == I_mangrove_door
  );
}
uint16_t getDoorItemFromBlock (uint16_t block) {
  switch (block) {
    case B_oak_door: return I_oak_door;
    case B_spruce_door: return I_spruce_door;
    case B_birch_door: return I_birch_door;
    case B_jungle_door: return I_jungle_door;
    case B_acacia_door: return I_acacia_door;
    case B_cherry_door: return I_cherry_door;
    case B_dark_oak_door: return I_dark_oak_door;
    case B_pale_oak_door: return I_pale_oak_door;
    case B_mangrove_door: return I_mangrove_door;
    default: return 0;
  }
}
uint8_t getDoorBlockFromItem (uint16_t item) {
  switch (item) {
    case I_oak_door: return B_oak_door;
    case I_spruce_door: return B_spruce_door;
    case I_birch_door: return B_birch_door;
    case I_jungle_door: return B_jungle_door;
    case I_acacia_door: return B_acacia_door;
    case I_cherry_door: return B_cherry_door;
    case I_dark_oak_door: return B_dark_oak_door;
    case I_pale_oak_door: return B_pale_oak_door;
    case I_mangrove_door: return B_mangrove_door;
    default: return 0;
  }
}

/* Checks if a door at the given coordinates is open */
uint8_t isDoorOpen (short x, uint8_t y, short z, uint8_t dimension) {
  return is_door_open_at(x, y, z, dimension);
}

/* Legacy wrappers for network state ID computation */
uint16_t getDoorStateId (uint16_t block, uint8_t is_upper, uint8_t open, uint8_t direction, uint8_t hinge) {
  return get_door_state_id(block, is_upper, open, direction, hinge);
}

// Send door block update with proper state
void sendDoorUpdate (int client_fd, short x, uint8_t y, short z, uint16_t block, uint8_t is_upper, uint8_t open, uint8_t direction, uint8_t hinge) {
  startPacket(client_fd, 0x08);
  writeUint64(client_fd, (((int64_t)x & 0x3FFFFFF) << 38) | (((int64_t)z & 0x3FFFFFF) << 12) | (y & 0xFFF));
  writeVarInt(client_fd, get_door_state_id(block, is_upper, open, direction, hinge));
  endPacket(client_fd);
}

// Get trapdoor state ID for network packet
uint16_t getTrapdoorStateId (uint16_t block, uint8_t open, uint8_t direction, uint8_t half) {
  return get_trapdoor_state_id(block, open, direction, half);
}

// Send trapdoor block update with proper state
void sendTrapdoorUpdate (int client_fd, short x, uint8_t y, short z, uint16_t block, uint8_t open, uint8_t direction, uint8_t half) {
  startPacket(client_fd, 0x08);
  writeUint64(client_fd, (((int64_t)x & 0x3FFFFFF) << 38) | (((int64_t)z & 0x3FFFFFF) << 12) | (y & 0xFFF));
  writeVarInt(client_fd, get_trapdoor_state_id(block, open, direction, half));
  endPacket(client_fd);
}

// Get oriented block state ID for network packet
uint16_t getOrientedStateId (uint16_t block, uint8_t direction) {
  return get_oriented_state_id(block, direction);
}

// Send oriented block update with proper state
void sendOrientedUpdate (int client_fd, short x, uint8_t y, short z, uint16_t block, uint8_t direction) {
  startPacket(client_fd, 0x08);
  writeUint64(client_fd, (((int64_t)x & 0x3FFFFFF) << 38) | (((int64_t)z & 0x3FFFFFF) << 12) | (y & 0xFFF));
  writeVarInt(client_fd, get_oriented_state_id(block, direction));
  endPacket(client_fd);
}

// Get stair state ID for network packet
uint16_t getStairStateId (uint16_t block, uint8_t half, uint8_t direction) {
  return get_stair_state_id(block, half, direction);
}

// Send stair block update with proper state
void sendStairUpdate (int client_fd, short x, uint8_t y, short z, uint16_t block, uint8_t half, uint8_t direction) {
  startPacket(client_fd, 0x08);
  writeUint64(client_fd, (((int64_t)x & 0x3FFFFFF) << 38) | (((int64_t)z & 0x3FFFFFF) << 12) | (y & 0xFFF));
  writeVarInt(client_fd, get_stair_state_id(block, half, direction));
  endPacket(client_fd);
}
#endif

// Returns defense points for the given piece of armor
// If the input item is not armor, returns 0
uint8_t getItemDefensePoints (uint16_t item) {

  switch (item) {
    case I_leather_helmet: return 1;
    case I_golden_helmet: return 2;
    case I_iron_helmet: return 2;
    case I_diamond_helmet: // Same as netherite
    case I_netherite_helmet: return 3;
    case I_leather_chestplate: return 3;
    case I_golden_chestplate: return 5;
    case I_iron_chestplate: return 6;
    case I_diamond_chestplate: // Same as netherite
    case I_netherite_chestplate: return 8;
    case I_leather_leggings: return 2;
    case I_golden_leggings: return 3;
    case I_iron_leggings: return 5;
    case I_diamond_leggings: // Same as netherite
    case I_netherite_leggings: return 6;
    case I_leather_boots: return 1;
    case I_golden_boots: return 1;
    case I_iron_boots: return 2;
    case I_diamond_boots: // Same as netherite
    case I_netherite_boots: return 3;
    default: break;
  }

  return 0;
}

// Calculates total defense points for the player's equipped armor
uint8_t getPlayerDefensePoints (PlayerData *player) {
  return (
    // Helmet
    getItemDefensePoints(player->inventory_items[39]) +
    // Chestplate
    getItemDefensePoints(player->inventory_items[38]) +
    // Leggings
    getItemDefensePoints(player->inventory_items[37]) +
    // Boots
    getItemDefensePoints(player->inventory_items[36])
  );
}

// Returns the designated server slot for the given piece of armor
// If input item is not armor, returns 255
uint8_t getArmorItemSlot (uint16_t item) {

    switch (item) {
    case I_leather_helmet:
    case I_golden_helmet:
    case I_iron_helmet:
    case I_diamond_helmet:
    case I_netherite_helmet:
      return 39;
    case I_leather_chestplate:
    case I_golden_chestplate:
    case I_iron_chestplate:
    case I_diamond_chestplate:
    case I_netherite_chestplate:
      return 38;
    case I_leather_leggings:
    case I_golden_leggings:
    case I_iron_leggings:
    case I_diamond_leggings:
    case I_netherite_leggings:
      return 37;
    case I_leather_boots:
    case I_golden_boots:
    case I_iron_boots:
    case I_diamond_boots:
    case I_netherite_boots:
      return 36;
    default: break;
  }

  return 255;
}

// Handles the player eating their currently held item
// Returns whether the operation was succesful (item was consumed)
// If `just_check` is set to true, the item doesn't get consumed
uint8_t handlePlayerEating (PlayerData *player, uint8_t just_check) {

  // Exit early if player is unable to eat
  if (player->hunger >= 20) return false;

  uint16_t *held_item = &player->inventory_items[player->hotbar];
  uint8_t *held_count = &player->inventory_count[player->hotbar];

  // Exit early if player isn't holding anything
  if (*held_item == 0 || *held_count == 0) return false;

  uint8_t food = 0;
  uint16_t saturation = 0;

  // The saturation ratio from vanilla to here is about 1:500
  switch (*held_item) {
    case I_chicken: food = 2; saturation = 600; break;
    case I_beef: food = 3; saturation = 900; break;
    case I_porkchop: food = 3; saturation = 300; break;
    case I_mutton: food = 2; saturation = 600; break;
    case I_cooked_chicken: food = 6; saturation = 3600; break;
    case I_cooked_beef: food = 8; saturation = 6400; break;
    case I_cooked_porkchop: food = 8; saturation = 6400; break;
    case I_cooked_mutton: food = 6; saturation = 4800; break;
    case I_rotten_flesh: food = 4; saturation = 0; break;
    case I_apple: food = 4; saturation = 1200; break;
    case I_bread: food = 5; saturation = 3000; break;
    case I_baked_potato: food = 5; saturation = 3000; break;
    case I_carrot: food = 3; saturation = 1800; break;
    case I_potato: food = 1; saturation = 300; break;
    case I_poisonous_potato: food = 2; saturation = 600; break;
    case I_golden_carrot: food = 6; saturation = 7200; break;
    case I_cooked_cod: food = 5; saturation = 3000; break;
    case I_cooked_salmon: food = 6; saturation = 4800; break;
    case I_cooked_rabbit: food = 5; saturation = 3000; break;
    case I_rabbit: food = 3; saturation = 900; break;
    case I_cod: food = 2; saturation = 200; break;
    case I_salmon: food = 2; saturation = 200; break;
    case I_tropical_fish: food = 1; saturation = 100; break;
    case I_pufferfish: food = 1; saturation = 100; break;
    case I_beetroot: food = 1; saturation = 600; break;
    case I_melon_slice: food = 2; saturation = 600; break;
    case I_pumpkin_pie: food = 8; saturation = 2400; break;
    case I_cookie: food = 2; saturation = 200; break;
    case I_mushroom_stew: food = 6; saturation = 3600; break;
    case I_beetroot_soup: food = 6; saturation = 3600; break;
    case I_rabbit_stew: food = 10; saturation = 6000; break;
    default: break;
  }

  // If just checking the item, return before making any changes
  if (just_check) return food != 0;

  // Apply saturation and food boost
  player->saturation += saturation;
  player->hunger += food;
  if (player->hunger > 20) player->hunger = 20;

  // Consume held item
  *held_count -= 1;
  if (*held_count == 0) *held_item = 0;

  // Update the client of these changes
  sc_entityEvent(player->client_fd, player->client_fd, 9);
  sc_setHealth(player->client_fd, player->health, player->hunger, player->saturation);
  sc_setContainerSlot(
    player->client_fd, 0,
    serverSlotToClientSlot(0, player->hotbar),
    *held_count, *held_item
  );
  broadcastPlayerEquipment(player);

  return true;
}

static void enqueueFluidUpdate (short x, int16_t y, short z, uint16_t fluid, uint16_t block);

void handleFluidMovement (short x, int16_t y, short z, uint16_t fluid, uint16_t block) {

  // Skip stale entries — the block has already been changed by a prior update
  if (getBlockAt(x, y, z) != block) return;

  uint8_t level = block - fluid;

  uint16_t adjacent[4] = {
    getBlockAt(x + 1, y, z),
    getBlockAt(x - 1, y, z),
    getBlockAt(x, y, z + 1),
    getBlockAt(x, y, z - 1)
  };

  // Handle maintaining connections to a fluid source
  if (level != 0) {
    uint8_t connected = false;
    for (int i = 0; i < 4; i ++) {
      if (adjacent[i] == block - 1) {
        connected = true;
        break;
      }
    }
    if (!connected) {
      makeBlockChange(x, y, z, B_air, DIMENSION_OVERWORLD);
      checkFluidUpdate(x + 1, y, z, adjacent[0]);
      checkFluidUpdate(x - 1, y, z, adjacent[1]);
      checkFluidUpdate(x, y, z + 1, adjacent[2]);
      checkFluidUpdate(x, y, z - 1, adjacent[3]);
      return;
    }
  }

  // Infinite water source: flowing water with 2+ adjacent source blocks becomes a source
  if (fluid == B_water && level != 0) {
    uint8_t source_count = 0;
    for (int i = 0; i < 4; i++) {
      if (adjacent[i] == fluid) source_count++;
    }
    if (source_count >= 2) {
      makeBlockChange(x, y, z, fluid, DIMENSION_OVERWORLD);
      level = 0;
      block = fluid;
    }
  }

  // Check if water should flow down, prioritize that over lateral flow
  uint16_t block_below = getBlockAt(x, y - 1, z);
  if (isReplaceableBlock(block_below)) {
    makeBlockChange(x, y - 1, z, fluid, DIMENSION_OVERWORLD);
    enqueueFluidUpdate(x, y - 1, z, fluid, fluid);
    return;
  }

  // Stop flowing laterally at the maximum level
  if (level == 3 && fluid == B_lava) return;
  if (level == 7) return;

  // Handle lateral water flow, increasing level by 1
  if (isReplaceableFluid(adjacent[0], level, fluid)) {
    makeBlockChange(x + 1, y, z, block + 1, DIMENSION_OVERWORLD);
    enqueueFluidUpdate(x + 1, y, z, fluid, block + 1);
  }
  if (isReplaceableFluid(adjacent[1], level, fluid)) {
    makeBlockChange(x - 1, y, z, block + 1, DIMENSION_OVERWORLD);
    enqueueFluidUpdate(x - 1, y, z, fluid, block + 1);
  }
  if (isReplaceableFluid(adjacent[2], level, fluid)) {
    makeBlockChange(x, y, z + 1, block + 1, DIMENSION_OVERWORLD);
    enqueueFluidUpdate(x, y, z + 1, fluid, block + 1);
  }
  if (isReplaceableFluid(adjacent[3], level, fluid)) {
    makeBlockChange(x, y, z - 1, block + 1, DIMENSION_OVERWORLD);
    enqueueFluidUpdate(x, y, z - 1, fluid, block + 1);
  }

}

static void enqueueFluidUpdate (short x, int16_t y, short z, uint16_t fluid, uint16_t block) {
  // De-dup: skip if the last enqueued entry is for the same position
  int tail = fluid_queue_tail;
  if (tail != fluid_queue_head) {
    int prev = (tail - 1 + FLUID_QUEUE_SIZE) % FLUID_QUEUE_SIZE;
    if (fluid_queue[prev].x == x && fluid_queue[prev].y == y && fluid_queue[prev].z == z) return;
  }
  int next = (tail + 1) % FLUID_QUEUE_SIZE;
  if (next == fluid_queue_head) return;
  fluid_queue[tail].x = x;
  fluid_queue[tail].y = y;
  fluid_queue[tail].z = z;
  fluid_queue[tail].fluid = fluid;
  fluid_queue[tail].block = block;
  fluid_queue_tail = next;
}

void processFluidQueue (void) {
  int processed = 0;
  while (processed < FLUID_UPDATES_PER_TICK && fluid_queue_head != fluid_queue_tail) {
    FluidUpdateEntry entry = fluid_queue[fluid_queue_head];
    fluid_queue_head = (fluid_queue_head + 1) % FLUID_QUEUE_SIZE;
    handleFluidMovement(entry.x, entry.y, entry.z, entry.fluid, entry.block);
    processed++;
  }
}

void checkFluidUpdate (short x, int16_t y, short z, uint16_t block) {

  uint16_t fluid;
  if (block >= B_water && block < B_water + 8) fluid = B_water;
  else if (block >= B_lava && block < B_lava + 4) fluid = B_lava;
  else return;

  enqueueFluidUpdate(x, y, z, fluid, block);

}

#ifdef ENABLE_PICKUP_ANIMATION
// Plays the item pickup animation with the given item at the given coordinates
void playPickupAnimation (PlayerData *player, uint16_t item, double x, double y, double z) {

  // Spawn a new item entity at the input coordinates
  // ID -1 is safe, as elsewhere it's reserved as a placeholder
  // The player's name is used as the UUID as it's cheap and unique enough
  sc_spawnEntity(player->client_fd, -1, (uint8_t *)player->name, 69, x + 0.5, y + 0.5, z + 0.5, 0, 0, 0, 0, 0);

  // Write a Set Entity Metadata packet for the item
  // Using startPacket/endPacket to properly handle compression
  startPacket(player->client_fd, 0x5C);
  writeVarInt(player->client_fd, -1);  // Entity ID

  // Describe slot data array entry
  writeByte(player->client_fd, 8);  // String NBT tag type
  writeByte(player->client_fd, 7);  // String length (item as string? Actually this is metadata index)
  // Send slot data
  writeByte(player->client_fd, 1);
  writeVarInt(player->client_fd, item);
  writeByte(player->client_fd, 0);
  writeByte(player->client_fd, 0);
  // Terminate entity metadata array
  writeByte(player->client_fd, 0xFF);
  endPacket(player->client_fd);

  // Send the Pickup Item packet targeting this entity
  sc_pickupItem(player->client_fd, -1, player->client_fd, 1);

  // Remove the item entity from the client right away
  sc_removeEntity(player->client_fd, -1);

}
#endif

static void clearConnectedNetherPortal(short start_x, int16_t start_y, short start_z, uint8_t dimension) {

  if (getBlockAt2(start_x, start_y, start_z, dimension) != B_nether_portal) return;

  typedef struct {
    short x;
    int16_t y;
    short z;
  } PortalNode;

  PortalNode queue[128];
  uint8_t head = 0;
  uint8_t tail = 0;

  queue[tail++] = (PortalNode){start_x, start_y, start_z};
  makeBlockChange(start_x, start_y, start_z, 0, dimension);

  while (head < tail) {
    PortalNode node = queue[head++];

    static const int8_t offsets[6][3] = {
      { 1, 0, 0 }, { -1, 0, 0 },
      { 0, 1, 0 }, { 0, -1, 0 },
      { 0, 0, 1 }, { 0, 0, -1 }
    };

    for (uint8_t i = 0; i < 6; i ++) {
      int ny = (int)node.y + offsets[i][1];
      if (ny < 0 || ny > 319) continue;

      short nx = node.x + offsets[i][0];
      short nz = node.z + offsets[i][2];
      if (getBlockAt2(nx, (int16_t)ny, nz, dimension) != B_nether_portal) continue;

      makeBlockChange(nx, (int16_t)ny, nz, 0, dimension);
      if (tail < sizeof(queue) / sizeof(queue[0])) {
        queue[tail++] = (PortalNode){nx, (int16_t)ny, nz};
      }
    }
  }

}

static void clearNearbyNetherPortal(short x, short y, short z, uint8_t dimension) {

  for (short dx = -2; dx <= 2; dx ++) {
    for (short dz = -2; dz <= 2; dz ++) {
      for (short dy = -4; dy <= 4; dy ++) {
        int ny = (int)y + dy;
        if (ny < 0 || ny > 319) continue;
        if (getBlockAt2(x + dx, (int16_t)ny, z + dz, dimension) == B_nether_portal) {
          clearConnectedNetherPortal(x + dx, (int16_t)ny, z + dz, dimension);
        }
      }
    }
  }

}

static void getBedDirectionOffset(uint8_t direction, short *dx, short *dz) {
  *dx = 0;
  *dz = 0;
  switch (direction & 3) {
    case 0: *dz = -1; break;  // north
    case 1: *dx = 1; break;   // east
    case 2: *dz = 1; break;   // south
    case 3: *dx = -1; break;  // west
    default: break;
  }
}

static uint8_t getPlayerFacingDirection(PlayerData *player) {
  if (player->yaw >= -96 && player->yaw < -32) return 1;  // East
  if (player->yaw >= -32 && player->yaw < 32) return 2;   // South
  if (player->yaw >= 32 && player->yaw < 96) return 3;    // West
  return 0;                                                // North
}

static void broadcastBedUpdate(short x, int16_t y, short z, uint8_t dimension, uint16_t block) {
  uint16_t state = special_block_get_state(x, y, z, dimension);
  uint16_t state_id = get_bed_state_id(block, bed_get_head(state), bed_get_occupied(state), bed_get_direction(state));

  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (player_data[i].client_fd == -1) continue;
    if (player_data[i].flags & 0x20) continue;
    if (player_data[i].dimension != dimension) continue;
    sc_blockUpdateState(player_data[i].client_fd, x, y, z, state_id);
  }
}

static uint8_t findOtherBedHalf(short x, int16_t y, short z, uint8_t dimension, uint16_t block, short *other_x, int16_t *other_y, short *other_z) {
  if (special_block_has_entry(x, y, z, dimension)) {
    uint16_t state = special_block_get_state(x, y, z, dimension);
    short dx, dz;
    getBedDirectionOffset(bed_get_direction(state), &dx, &dz);
    if (bed_get_head(state)) {
      dx = -dx;
      dz = -dz;
    }

    *other_x = x + dx;
    *other_y = y;
    *other_z = z + dz;
    if (getBlockAt2(*other_x, *other_y, *other_z, dimension) == block) return true;
  }

  static const int8_t adjacent[4][2] = {
    {0, -1}, {1, 0}, {0, 1}, {-1, 0}
  };
  for (uint8_t i = 0; i < 4; i++) {
    short nx = x + adjacent[i][0];
    short nz = z + adjacent[i][1];
    if (getBlockAt2(nx, y, nz, dimension) != block) continue;
    *other_x = nx;
    *other_y = y;
    *other_z = nz;
    return true;
  }

  return false;
}

static void setPlayerBedSpawn(PlayerData *player, short bed_x, int16_t bed_y, short bed_z) {
  player->spawn_set = 1;
  player->spawn_x = bed_x;
  player->spawn_y = bed_y + 1;
  player->spawn_z = bed_z;
  player->spawn_dimension = player->dimension;
  writePlayerDataToDisk();
}

static void broadcastSleepTimeAndWeather(void) {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (player_data[i].client_fd == -1) continue;
    if (player_data[i].flags & 0x20) continue;
    sc_updateTime(player_data[i].client_fd, world_time);
    if (player_data[i].dimension == DIMENSION_OVERWORLD) {
      sc_gameEvent(player_data[i].client_fd, 2, 0.0f);
      sc_gameEvent(player_data[i].client_fd, 7, 0.0f);
      sc_gameEvent(player_data[i].client_fd, 8, 0.0f);
    }
  }
}

static void explodeUnsafeBed(PlayerData *player, short x, int16_t y, short z, uint16_t block) {
  short other_x;
  int16_t other_y;
  short other_z;
  if (findOtherBedHalf(x, y, z, player->dimension, block, &other_x, &other_y, &other_z)) {
    special_block_clear(other_x, other_y, other_z, player->dimension);
    makeBlockChange(other_x, other_y, other_z, 0, player->dimension);
  }
  special_block_clear(x, y, z, player->dimension);
  makeBlockChange(x, y, z, 0, player->dimension);

  double ex = (double)x + 0.5;
  double ey = (double)y + 0.5;
  double ez = (double)z + 0.5;

  doExplosion(ex, ey, ez, 5.0f, player->dimension);

  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (player_data[i].client_fd == -1) continue;
    if (player_data[i].flags & 0x20) continue;
    if (player_data[i].dimension != player->dimension) continue;
    sc_soundEffect(player_data[i].client_fd, S_GENERIC_EXPLODE, SOUND_CATEGORY_BLOCKS, ex, ey, ez, 4.0f, 1.0f);
  }

  const double blast_radius = 6.0;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (player_data[i].client_fd == -1) continue;
    if (player_data[i].flags & 0x20) continue;
    if (player_data[i].dimension != player->dimension) continue;

    double dx = ((double)player_data[i].x + 0.5) - ex;
    double dy = ((double)player_data[i].y + 0.5) - ey;
    double dz = ((double)player_data[i].z + 0.5) - ez;
    double horizontal_dist = sqrt(dx * dx + dz * dz);
    if (horizontal_dist >= blast_radius || fabs(dy) >= blast_radius) continue;

    uint8_t damage = (uint8_t)((blast_radius - horizontal_dist) * 6.0);
    if (damage < 1) damage = 1;
    hurtEntity(player_data[i].client_fd, -1, D_bad_respawn_point, damage);

    if (horizontal_dist > 0.01) {
      double force = (blast_radius - horizontal_dist) / blast_radius;
      double vx = (dx / horizontal_dist) * force * 3.0;
      double vz = (dz / horizontal_dist) * force * 3.0;
      double vy = force * 0.8;
      sc_setEntityVelocity(
        player_data[i].client_fd, player_data[i].client_fd,
        (int16_t)(vx * 8000.0), (int16_t)(vy * 8000.0), (int16_t)(vz * 8000.0)
      );
    }
  }

  for (int i = 0; i < MAX_MOBS; i++) {
    if (mob_data[i].type == 0) continue;
    if ((mob_data[i].data & 31) == 0) continue;
    if (mob_data[i].dimension != player->dimension) continue;

    double dx = mob_data[i].x - ex;
    double dz = mob_data[i].z - ez;
    double dist = sqrt(dx * dx + dz * dz);
    if (dist >= 5.0 || fabs(mob_data[i].y - ey) >= 5.0) continue;

    uint8_t damage = (uint8_t)((5.0 - dist) * 6.0);
    if (damage < 1) damage = 1;
    hurtEntity(-2 - i, -1, D_explosion, damage);
  }
}

static void handleBedInteraction(PlayerData *player, short x, int16_t y, short z, uint16_t block) {
  if (player->dimension != DIMENSION_OVERWORLD) {
    explodeUnsafeBed(player, x, y, z, block);
    return;
  }

  setPlayerBedSpawn(player, x, y, z);

  uint8_t can_sleep = (world_time >= 12542 && world_time <= 23459) || !world_weather_clear;
  if (!can_sleep) {
    const char msg[] = "§aRespawn point set.";
    sc_systemChat(player->client_fd, (char *)msg, (uint16_t)sizeof(msg) - 1);
    return;
  }

  world_time = 1000;
  world_weather_clear = 1;
  world_rain_level = 0.0f;
  world_thunder_level = 0.0f;
  world_weather_rain_time = 0;
  world_weather_thunder_time = 0;
  world_weather_clear_time = 12000 + (int32_t)(fast_rand() % 168000);
  broadcastSleepTimeAndWeather();

  char msg[96];
  int len = snprintf(msg, sizeof(msg), "§e%s slept and skipped to morning.", player->name);
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (player_data[i].client_fd == -1) continue;
    if (player_data[i].flags & 0x20) continue;
    sc_systemChat(player_data[i].client_fd, msg, (uint16_t)len);
  }
}

void handlePlayerAction (PlayerData *player, int action, short x, short y, short z) {

  // Check spawn protection for block-breaking actions
  if ((action == 0 || action == 2) && is_in_safe_area(x, z, player->dimension)) {
    sc_systemChat(player->client_fd, "§cCannot break blocks in protected spawn area", 46);
    return;
  }

  // Special case: punch fire to extinguish it
  if (action == 0) {
    uint16_t target_block = getBlockAt2(x, y, z, player->dimension);
    if (target_block == B_fire) {
      // Extinguish the fire
      makeBlockChange(x, y, z, 0, player->dimension);
      return;
    }
  }

  // Drop item when player presses Q in gameplay (no GUI open)
  if (action == 3 || action == 4) {
    uint8_t slot = player->hotbar;
    if (player->inventory_items[slot] != 0 && player->inventory_count[slot] > 0) {
      uint8_t count = (action == 3) ? 1 : player->inventory_count[slot];
      uint16_t item = player->inventory_items[slot];
      player->inventory_count[slot] -= count;
      if (player->inventory_count[slot] == 0) player->inventory_items[slot] = 0;
      double yaw_rad = player->yaw * M_PI / 127.0;
      double pitch_rad = player->pitch * M_PI / 254.0;
      double speed = 0.4;
      sc_setContainerSlot(player->client_fd, 0, serverSlotToClientSlot(0, slot),
        player->inventory_count[slot], player->inventory_items[slot]);
      spawnItemEntity(player->x + 0.5, player->y + 0.5, player->z + 0.5, item, count, player->dimension,
        -sin(yaw_rad) * cos(pitch_rad) * speed,
        -sin(pitch_rad) * speed + 0.3,
        cos(yaw_rad) * cos(pitch_rad) * speed);
      broadcastPlayerEquipment(player);
    }
    return;
  }

  // "Release use item" action, sent when eating stops or a bow is released
  if (action == 5) {
    int player_idx = getPlayerIndexByPointer(player);
    if (player_idx >= 0 && player_bow_draw_start[player_idx] != 0) {
      if (player->inventory_items[player->hotbar] == I_bow) {
        shootBowArrow(player);
      } else {
        player_bow_draw_start[player_idx] = 0;
        player_bow_last_use_tick[player_idx] = 0;
        setPlayerActiveHand(player, 0);
      }
    }
    // Reset eating timer and clear eating/blocking flags
    player->flagval_16 = 0;
    player->flags &= ~0x10;
    if (player->flags & 0x0100) {
      player->flags &= ~0x0100;
      broadcastPlayerMetadata(player);
    }
  }

  // Ignore further actions not pertaining to mining blocks
  if (action != 0 && action != 2) return;

  // In creative, only the "start mining" action is sent
  // No additional verification is performed, the block is simply removed
  if (action == 0 && getConfiguredGameMode() == 1) {
    uint16_t block = getBlockAt2(x, y, z, player->dimension);
    makeBlockChange(x, y, z, 0, player->dimension);
    if (block == B_obsidian || block == B_nether_portal) {
      clearNearbyNetherPortal(x, y, z, player->dimension);
    }
    return;
  }

  uint16_t block = getBlockAt2(x, y, z, player->dimension);

  // If this is a "start mining" packet, the block must be instamine
  if (action == 0 && !isInstantlyMined(player, block)) return;

  // Read wheat age BEFORE makeBlockChange clears the special block entry.
  // Generated farms use palette entries B_wheat..B_wheat_7, while planted
  // crops store age in the special-block table.
  uint16_t wheat_age = 0;
  if (isWheatBlock(block)) {
    wheat_age = wheatBlockAge(block);
    if (block == B_wheat) wheat_age = special_block_get_state(x, y, z, player->dimension);
  }

  // Don't continue if the block change failed
  if (makeBlockChange(x, y, z, 0, player->dimension)) return;

  // Update neighbor fences when a block is broken (they may have been connected)
  int _adj[4][2] = {{x, z - 1}, {x, z + 1}, {x - 1, z}, {x + 1, z}};
  for (int _ai = 0; _ai < 4; _ai++) {
    int _ax = _adj[_ai][0], _az = _adj[_ai][1];
    uint16_t _ab = getBlockAt2(_ax, y, _az, player->dimension);
    if (!is_fence_block(_ab)) continue;
    uint16_t _a1 = getBlockAt2(_ax, y, _az - 1, player->dimension);
    uint16_t _a2 = getBlockAt2(_ax + 1, y, _az, player->dimension);
    uint16_t _a3 = getBlockAt2(_ax, y, _az + 1, player->dimension);
    uint16_t _a4 = getBlockAt2(_ax - 1, y, _az, player->dimension);
    uint8_t _bn = isFenceSolidBlock(_a1) ? 1 : 0;
    uint8_t _be = isFenceSolidBlock(_a2) ? 1 : 0;
    uint8_t _bs = isFenceSolidBlock(_a3) ? 1 : 0;
    uint8_t _bw = isFenceSolidBlock(_a4) ? 1 : 0;
    special_block_set_state(_ax, y, _az, player->dimension, _ab, fence_encode_state(_bn, _be, _bs, _bw));
    uint8_t _conn = _bn | (_be << 1) | (_bs << 2) | (_bw << 3);
    uint16_t _fsid = get_fence_state_id(_ab, _conn);
    for (int _j = 0; _j < MAX_PLAYERS; _j++) {
      if (player_data[_j].client_fd == -1) continue;
      if (player_data[_j].flags & 0x20) continue;
      sc_blockUpdateState(player_data[_j].client_fd, _ax, y, _az, _fsid);
    }
  }

  if (block == B_obsidian || block == B_nether_portal) {
    clearNearbyNetherPortal(x, y, z, player->dimension);
  }

  uint16_t held_item = player->inventory_items[player->hotbar];
  uint16_t item = getMiningResult(held_item, block);
  uint8_t durability_cost = getBlockBreakDurabilityCost(held_item);

  if (is_bed_block(block)) {
    short other_x;
    int16_t other_y;
    short other_z;
    if (findOtherBedHalf(x, y, z, player->dimension, block, &other_x, &other_y, &other_z)) {
      special_block_clear(other_x, other_y, other_z, player->dimension);
      makeBlockChange(other_x, other_y, other_z, 0, player->dimension);
    }
    special_block_clear(x, y, z, player->dimension);
    if (item) {
      spawnItemEntity(x + 0.5, y + 0.5, z + 0.5, item, 1, player->dimension, 0, 0, 0);
    }
    damageHeldItem(player, durability_cost);
    return;
  }

  #ifdef ALLOW_DOORS
  // If mining a door, also break the other half and clear state data
  if (isDoorBlock(block)) {
    // Check if this is the upper half of a door
    uint8_t is_upper_half = 0;
    short door_x = x;
    uint8_t door_y = y;
    short door_z = z;

    // Search for a door lower half at y-1
    for (int i = 0; i < block_changes_count; i ++) {
      if (!isDoorBlock(block_changes[i].block)) continue;
      if (block_changes[i].x == x && block_changes[i].y == y - 1 && block_changes[i].z == z) {
        // This is the upper half, use lower half's y
        is_upper_half = 1;
        door_y = y - 1;
        break;
      }
    }

    // Break the other half
    if (is_upper_half) {
      makeBlockChange(x, y - 1, z, 0, player->dimension);  // Break lower half
    } else {
      makeBlockChange(x, y + 1, z, 0, player->dimension);  // Break upper half
    }

    // Clear door state from the unified special block table
    special_block_clear(door_x, door_y, door_z, player->dimension);
    // Give door item (only once for the whole door)
    if (item) {
      spawnItemEntity(x + 0.5, y + 0.5, z + 0.5, item, 1, player->dimension, 0, 0, 0);
    }
    special_block_clear(x, y, z, player->dimension);
    damageHeldItem(player, durability_cost);
    return;
  }
  #endif

  if (item) {
    spawnItemEntity(x + 0.5, y + 0.5, z + 0.5, item, 1, player->dimension, 0, 0, 0);
  }

  // Special handling for wheat drops based on maturity
  if (isWheatBlock(block)) {
    if (wheat_age >= 7) {
      spawnItemEntity(x + 0.5, y + 0.5, z + 0.5, I_wheat, 1, player->dimension, 0, 0, 0);
      spawnItemEntity(x + 0.5, y + 0.5, z + 0.5, I_wheat_seeds, 1 + (fast_rand() % 3), player->dimension, 0, 0, 0);
    } else {
      spawnItemEntity(x + 0.5, y + 0.5, z + 0.5, I_wheat_seeds, 1, player->dimension, 0, 0, 0);
    }
    special_block_clear(x, y, z, player->dimension);
  }

  // Clear special block state for any block type that might have one.
  // This prevents the hash table from filling up with stale entries when
  // blocks like stairs, fences, trapdoors, furnaces, chests, barrels,
  // ender_chests, wall_torches, lanterns, or ladders are broken.
  // (special_block_clear is a no-op if no entry exists.)
  if (is_stair_block(block) || block == B_furnace || block == B_ender_chest ||
      is_fence_block(block) || is_horizontal_facing_block(block) ||
      block == B_lantern || block == B_chest || block == B_barrel ||
      block == B_torch || is_trapdoor_block(block)) {
    special_block_clear(x, y, z, player->dimension);
  }

  // Cascade-break connected leaves and floating leaves (cap at 6 additional broken)
  if (isLeafBlock(block)) {
    int broken_count = 0;
    breakConnectedLeaves(x, y, z, block, player, &broken_count);
    breakFloatingLeaves(x, y, z, block, player, &broken_count);
  }

  damageHeldItem(player, durability_cost);

  // Update nearby fluids
  uint16_t block_above = getBlockAt2(x, y + 1, z, player->dimension);
  #ifdef DO_FLUID_FLOW
  if (config.do_fluid_flow) {
    checkFluidUpdate(x, y + 1, z, block_above);
    checkFluidUpdate(x - 1, y, z, getBlockAt2(x - 1, y, z, player->dimension));
    checkFluidUpdate(x + 1, y, z, getBlockAt2(x + 1, y, z, player->dimension));
    checkFluidUpdate(x, y, z - 1, getBlockAt2(x, y, z - 1, player->dimension));
    checkFluidUpdate(x, y, z + 1, getBlockAt2(x, y, z + 1, player->dimension));
  }
  #endif

  // Check if any blocks above this should break, and if so,
  // iterate upward over all blocks in the column and break them
  int16_t y_offset = 1;
  while (isColumnBlock(block_above)) {
    // Destroy the next block
    makeBlockChange(x, y + y_offset, z, 0, player->dimension);
    // Check for item drops *without a tool*
    uint16_t item = getMiningResult(0, block_above);
    if (item) spawnItemEntity(x + 0.5, y + y_offset + 0.5, z + 0.5, item, 1, player->dimension, 0, 0, 0);
    // Select the next block in the column
    y_offset ++;
    block_above = getBlockAt2(x, y + y_offset, z, player->dimension);
  }
}

static uint8_t getFaceOffsetBlock(short x, int16_t y, short z, uint8_t face, short *out_x, int16_t *out_y, short *out_z) {
  int fy = y;
  *out_x = x;
  *out_z = z;

  switch (face) {
    case 0: fy = (int)y - 1; break;
    case 1: fy = (int)y + 1; break;
    case 2: *out_z = z - 1; break;
    case 3: *out_z = z + 1; break;
    case 4: *out_x = x - 1; break;
    case 5: *out_x = x + 1; break;
    default: return 0;
  }

  if (fy < 0 || fy > 319) return 0;
  *out_y = (int16_t)fy;
  return 1;
}

static uint8_t canReplaceHeldItemWithOverflow(PlayerData *player, uint8_t count, uint16_t replacement) {
  if (count == 1) return true;
  return canGivePlayerItem(player, replacement, 1);
}

static void replaceHeldItemWithOverflow(PlayerData *player, uint8_t *count, uint16_t *item, uint16_t replacement) {
  if (*count == 1) {
    *item = replacement;
    return;
  }

  *count -= 1;
  givePlayerItem(player, replacement, 1);
}

static void syncHeldItem(PlayerData *player, uint8_t count, uint16_t item) {
  sc_setContainerSlot(player->client_fd, 0, serverSlotToClientSlot(0, player->hotbar), count, item);
  broadcastPlayerEquipment(player);
  writePlayerDataToDisk();
}

static uint8_t handleBucketUse(PlayerData *player, short x, short y, short z, uint8_t face, uint8_t *count, uint16_t *item) {
  if (face == 255) {
    // For some reason, UseItemOn and UseItem are both sent when looking at a block.
    // Skip the untargeted variant if the targeted one already ran this tick.
    if (player->last_bucket_tick == server_ticks) return true;
    if (*item != I_bucket && *item != I_water_bucket && *item != I_lava_bucket) return false;

    const double yaw_rad = ((double)player->yaw * 180.0 / 127.0) * (3.14159265358979323846 / 180.0);
    const double pitch_rad = ((double)player->pitch * 90.0 / 127.0) * (3.14159265358979323846 / 180.0);
    const double cos_pitch = cos(pitch_rad);
    const double ray_dx = -sin(yaw_rad) * cos_pitch;
    const double ray_dy = -sin(pitch_rad);
    const double ray_dz = cos(yaw_rad) * cos_pitch;
    const double eye_x = (double)player->x + 0.5;
    const double eye_y = (double)player->y + 1.62;
    const double eye_z = (double)player->z + 0.5;

    short prev_x = player->x;
    int16_t prev_y = player->y;
    short prev_z = player->z;
    uint8_t have_prev = false;

    for (double distance = 0.0; distance <= 5.0; distance += 0.1) {
      int bx_i = (int)floor(eye_x + ray_dx * distance);
      int by_i = (int)floor(eye_y + ray_dy * distance);
      int bz_i = (int)floor(eye_z + ray_dz * distance);

      if (by_i < 0 || by_i > 319 || bx_i < -32768 || bx_i > 32767 || bz_i < -32768 || bz_i > 32767) {
        continue;
      }

      short bx = (short)bx_i;
      int16_t by = (int16_t)by_i;
      short bz = (short)bz_i;
      uint16_t ray_block = getBlockAt2(bx, by, bz, player->dimension);

      if (*item == I_bucket && (ray_block == B_water || ray_block == B_lava)) {
        return handleBucketUse(player, bx, by, bz, 1, count, item);
      }

      if (*item != I_bucket && !isReplaceableBlock(ray_block)) {
        if (!have_prev) return true;
        uint16_t fluid = *item == I_water_bucket ? B_water : B_lava;

        if (is_in_safe_area(prev_x, prev_z, player->dimension)) {
          sc_systemChat(player->client_fd, "§cCannot interact with blocks in protected spawn area", 54);
          return true;
        }
        if (!canReplaceHeldItemWithOverflow(player, *count, I_bucket)) return true;
        if (makeBlockChange(prev_x, prev_y, prev_z, fluid, player->dimension)) return true;
        #ifdef DO_FLUID_FLOW
        if (config.do_fluid_flow) {
          checkFluidUpdate(prev_x, prev_y + 1, prev_z, getBlockAt2(prev_x, prev_y + 1, prev_z, player->dimension));
          checkFluidUpdate(prev_x - 1, prev_y, prev_z, getBlockAt2(prev_x - 1, prev_y, prev_z, player->dimension));
          checkFluidUpdate(prev_x + 1, prev_y, prev_z, getBlockAt2(prev_x + 1, prev_y, prev_z, player->dimension));
          checkFluidUpdate(prev_x, prev_y, prev_z - 1, getBlockAt2(prev_x, prev_y, prev_z - 1, player->dimension));
          checkFluidUpdate(prev_x, prev_y, prev_z + 1, getBlockAt2(prev_x, prev_y, prev_z + 1, player->dimension));
          checkFluidUpdate(prev_x, prev_y - 1, prev_z, getBlockAt2(prev_x, prev_y - 1, prev_z, player->dimension));
          checkFluidUpdate(prev_x, prev_y, prev_z, fluid);
        }
        #endif
        replaceHeldItemWithOverflow(player, count, item, I_bucket);
        syncHeldItem(player, *count, *item);
        player->last_bucket_tick = server_ticks;
        return true;
      }

      if (isReplaceableBlock(ray_block)) {
        prev_x = bx;
        prev_y = by;
        prev_z = bz;
        have_prev = true;
      }
    }

    return true;
  }

  uint16_t target = getBlockAt2(x, y, z, player->dimension);

  if (*item == I_bucket) {
    uint16_t filled_bucket = 0;
    if (target == B_water) filled_bucket = I_water_bucket;
    else if (target == B_lava) filled_bucket = I_lava_bucket;
    else return false;

    if (is_in_safe_area(x, z, player->dimension)) {
      sc_systemChat(player->client_fd, "§cCannot interact with blocks in protected spawn area", 54);
      return true;
    }
    if (!canReplaceHeldItemWithOverflow(player, *count, filled_bucket)) return true;
    if (makeBlockChange(x, y, z, B_air, player->dimension)) return true;
    #ifdef DO_FLUID_FLOW
    if (config.do_fluid_flow) {
      checkFluidUpdate(x, y + 1, z, getBlockAt2(x, y + 1, z, player->dimension));
      checkFluidUpdate(x - 1, y, z, getBlockAt2(x - 1, y, z, player->dimension));
      checkFluidUpdate(x + 1, y, z, getBlockAt2(x + 1, y, z, player->dimension));
      checkFluidUpdate(x, y, z - 1, getBlockAt2(x, y, z - 1, player->dimension));
      checkFluidUpdate(x, y, z + 1, getBlockAt2(x, y, z + 1, player->dimension));
      checkFluidUpdate(x, y - 1, z, getBlockAt2(x, y - 1, z, player->dimension));
    }
    #endif
    replaceHeldItemWithOverflow(player, count, item, filled_bucket);
    syncHeldItem(player, *count, *item);
    player->last_bucket_tick = server_ticks;
    return true;
  }

  uint16_t fluid = 0;
  if (*item == I_water_bucket) fluid = B_water;
  else if (*item == I_lava_bucket) fluid = B_lava;
  else return false;

  short fx = x, fz = z;
  int16_t fy = y;
  if (!getFaceOffsetBlock(x, y, z, face, &fx, &fy, &fz)) return true;

  uint16_t existing = getBlockAt2(fx, fy, fz, player->dimension);
  // Water + lava = obsidian
  if (fluid == B_water && existing >= B_lava && existing < B_lava + 4) {
    fluid = B_obsidian;
  } else if (fluid == B_lava && existing >= B_water && existing < B_water + 8) {
    fluid = B_obsidian;
  }

  if (!isReplaceableBlock(existing)) return true;
  if (is_in_safe_area(fx, fz, player->dimension)) {
    sc_systemChat(player->client_fd, "§cCannot interact with blocks in protected spawn area", 54);
    return true;
  }
  if (!canReplaceHeldItemWithOverflow(player, *count, I_bucket)) return true;
  if (makeBlockChange(fx, fy, fz, fluid, player->dimension)) return true;
  #ifdef DO_FLUID_FLOW
  if (config.do_fluid_flow) {
    checkFluidUpdate(fx, fy + 1, fz, getBlockAt2(fx, fy + 1, fz, player->dimension));
    checkFluidUpdate(fx - 1, fy, fz, getBlockAt2(fx - 1, fy, fz, player->dimension));
    checkFluidUpdate(fx + 1, fy, fz, getBlockAt2(fx + 1, fy, fz, player->dimension));
    checkFluidUpdate(fx, fy, fz - 1, getBlockAt2(fx, fy, fz - 1, player->dimension));
    checkFluidUpdate(fx, fy, fz + 1, getBlockAt2(fx, fy, fz + 1, player->dimension));
    checkFluidUpdate(fx, fy - 1, fz, getBlockAt2(fx, fy - 1, fz, player->dimension));
    checkFluidUpdate(fx, fy, fz, fluid);
  }
  #endif
  replaceHeldItemWithOverflow(player, count, item, I_bucket);
  syncHeldItem(player, *count, *item);
  player->last_bucket_tick = server_ticks;
  return true;
}

static uint8_t isPortalInteriorBlock(uint16_t block) {
  return isReplaceableBlock(block) || block == B_fire || block == B_nether_portal;
}

static uint8_t tryCreatePortalInPlane(short x, uint8_t y, short z, int8_t dx, int8_t dz, uint8_t dimension) {
  for (int offset_w = 0; offset_w < 2; offset_w++) {
    for (int offset_h = 0; offset_h < 3; offset_h++) {
      int origin_x = x - dx * offset_w;
      int origin_y = (int)y - offset_h;
      int origin_z = z - dz * offset_w;

      if (origin_y < 1 || origin_y + 3 > 255) continue;

      uint8_t valid = 1;

      for (int w = -1; w <= 2 && valid; w++) {
        if (getBlockAt2(origin_x + dx * w, origin_y - 1, origin_z + dz * w, dimension) != B_obsidian) valid = 0;
        if (getBlockAt2(origin_x + dx * w, origin_y + 3, origin_z + dz * w, dimension) != B_obsidian) valid = 0;
      }

      for (int h = 0; h < 3 && valid; h++) {
        if (getBlockAt2(origin_x - dx, origin_y + h, origin_z - dz, dimension) != B_obsidian) valid = 0;
        if (getBlockAt2(origin_x + dx * 2, origin_y + h, origin_z + dz * 2, dimension) != B_obsidian) valid = 0;
      }

      for (int w = 0; w < 2 && valid; w++) {
        for (int h = 0; h < 3; h++) {
          if (!isPortalInteriorBlock(getBlockAt2(origin_x + dx * w, origin_y + h, origin_z + dz * w, dimension))) {
            valid = 0;
            break;
          }
        }
      }

      if (!valid) continue;

      for (int w = 0; w < 2; w++) {
        for (int h = 0; h < 3; h++) {
          makeBlockChange(
            (short)(origin_x + dx * w),
            (uint8_t)(origin_y + h),
            (short)(origin_z + dz * w),
            B_nether_portal,
            dimension
          );
        }
      }

      return 1;
    }
  }

  return 0;
}

static uint8_t tryCreatePortal(short x, uint8_t y, short z, uint8_t dimension) {
  return tryCreatePortalInPlane(x, y, z, 1, 0, dimension) ||
         tryCreatePortalInPlane(x, y, z, 0, 1, dimension);
}

static void syncDimensionEntities(PlayerData *player, uint8_t old_dim) {
  // 1. Tell other players in the OLD dimension to remove this player
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (player_data[i].client_fd == -1 || player_data[i].client_fd == player->client_fd) continue;
    if (player_data[i].dimension == old_dim) {
      sc_removeEntity(player_data[i].client_fd, player->client_fd);
    }
  }

  // 2. Tell other players in the NEW dimension to spawn this player
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (player_data[i].client_fd == -1 || player_data[i].client_fd == player->client_fd) continue;
    if (player_data[i].dimension == player->dimension) {
      sc_spawnEntityPlayer(player_data[i].client_fd, *player);
      sendPlayerMetadata(player_data[i].client_fd, player);
      sendPlayerEquipment(player_data[i].client_fd, player);
    }
  }

  // 3. Send all players and mobs in the NEW dimension to the player
  // (Existing players)
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (player_data[i].client_fd == -1 || player_data[i].client_fd == player->client_fd) continue;
    if (player_data[i].flags & 0x20) continue;
    if (player_data[i].dimension == player->dimension) {
      sc_spawnEntityPlayer(player->client_fd, player_data[i]);
      sendPlayerMetadata(player->client_fd, &player_data[i]);
      sendPlayerEquipment(player->client_fd, &player_data[i]);
    }
  }

  // (Mobs)
  uint8_t uuid[16];
  uint32_t r = fast_rand();
  memcpy(uuid, &r, 4);
  memset(uuid + 4, 0, 12);
  for (int i = 0; i < MAX_MOBS; i++) {
    if (mob_data[i].type == 0) continue;
    if ((mob_data[i].data & 31) == 0) continue;
    if (mob_data[i].dimension != player->dimension) continue;
    memcpy(uuid + 4, &i, 4);
    sc_spawnEntity(
      player->client_fd, -2 - i, uuid,
      mob_data[i].type, mob_data[i].x, mob_data[i].y, mob_data[i].z,
      0, 0, 0, 0, 0
    );
    broadcastMobMetadata(player->client_fd, -2 - i);
  }
}

static uint8_t isNetherSpawnFloorBlock(uint16_t block) {
  return block != B_bedrock && block != B_magma_block && !isPassableBlock(block);
}

static uint8_t isSafeNetherSpawnColumn(int x, int floor_y, int z) {
  return floor_y >= 72 && floor_y <= 118 &&
         isNetherSpawnFloorBlock(getBlockAt2(x, floor_y, z, DIMENSION_NETHER)) &&
         isPassableSpawnBlock(getBlockAt2(x, floor_y + 1, z, DIMENSION_NETHER)) &&
         isPassableSpawnBlock(getBlockAt2(x, floor_y + 2, z, DIMENSION_NETHER)) &&
         isPassableSpawnBlock(getBlockAt2(x, floor_y + 3, z, DIMENSION_NETHER));
}

static uint8_t findSafeNetherSpawn(int origin_x, int origin_z, int preferred_floor_y, int *out_x, int *out_z, int *out_floor_y) {
  if (preferred_floor_y < 72 || preferred_floor_y > 118) preferred_floor_y = 80;

  for (int radius = 0; radius <= 8; radius++) {
    for (int dz = -radius; dz <= radius; dz++) {
      for (int dx = -radius; dx <= radius; dx++) {
        if (radius != 0 && dx != -radius && dx != radius && dz != -radius && dz != radius) continue;

        int x = origin_x + dx;
        int z = origin_z + dz;
        for (int delta_y = 0; delta_y <= 46; delta_y++) {
          int y = preferred_floor_y + delta_y;
          if (y <= 118 && isSafeNetherSpawnColumn(x, y, z)) {
            *out_x = x;
            *out_z = z;
            *out_floor_y = y;
            return 1;
          }

          y = preferred_floor_y - delta_y;
          if (delta_y != 0 && y >= 72 && isSafeNetherSpawnColumn(x, y, z)) {
            *out_x = x;
            *out_z = z;
            *out_floor_y = y;
            return 1;
          }
        }
      }
    }
  }

  return 0;
}

static void switchPlayerToDimension(PlayerData *player, uint8_t new_dim) {
  uint8_t old_dim = player->dimension;
  if (old_dim == new_dim) return;

  // Drop stale chunk-stream packets from the old dimension so they cannot
  // arrive between Respawn and the destination chunks.
  clear_client_send_queue(player->client_fd);

  int had_saved_portal = 0;
  int preferred_nether_floor_y = (int)player->y - 1;

  if (new_dim == DIMENSION_NETHER) {
    if (old_dim == DIMENSION_OVERWORLD) {
      // Save overworld entry coords for return trip.
      player->portal_ow_x = player->x;
      player->portal_ow_y = player->y;
      player->portal_ow_z = player->z;
      player->portal_valid = 1;
      player->x = player->x / 8;
      player->z = player->z / 8;
    }
  } else if (new_dim == DIMENSION_OVERWORLD) {
    if (old_dim == DIMENSION_NETHER) {
      if (player->portal_valid) {
        // Use saved overworld coords for return, with 3-block offset to avoid re-entering portal.
        short ox = player->portal_ow_x;
        short oz = player->portal_ow_z;
        player->x = ox + 3;
        player->z = oz - 3;
        player->portal_valid = 0;
        had_saved_portal = 1;
      } else {
        player->x = player->x * 8;
        player->z = player->z * 8;
      }
    } else if (old_dim == DIMENSION_END) {
      player->x = 8;
      player->z = 8;
    }
  } else if (new_dim == DIMENSION_END) {
    // Visual-only stronghold End portals now lead to the End. Spawn at the
    // center island; no exit gateway/dragon fight mechanics are implemented yet.
    player->x = 0;
    player->z = 0;
    player->y = 76;
  }

  if (player->x < -16384) player->x = -16384;
  if (player->x > 16384) player->x = 16384;
  if (player->z < -16384) player->z = -16384;
  if (player->z > 16384) player->z = 16384;

  player->dimension = new_dim;

  // Compute nether Y and save portal position before respawn.
  int np_x = 0, np_z = 0, np_h = 80;
  if (new_dim == DIMENSION_NETHER) {
    init_worldgen();
    np_x = (int)player->x;
    np_z = (int)player->z;
    if (preferred_nether_floor_y < 72 || preferred_nether_floor_y > 118) preferred_nether_floor_y = 80;

    int safe_x = np_x, safe_z = np_z, safe_floor_y = preferred_nether_floor_y;
    uint8_t found_natural_floor = findSafeNetherSpawn(np_x, np_z, preferred_nether_floor_y, &safe_x, &safe_z, &safe_floor_y);
    np_x = safe_x;
    np_z = safe_z;
    np_h = found_natural_floor ? safe_floor_y : preferred_nether_floor_y;
    player->y = (int16_t)(np_h + 1);

    terminal_ui_log("Nether spawn: bx=%d bz=%d preferred_floor=%d np_h=%d natural=%d player_y=%d",
      np_x, np_z, preferred_nether_floor_y, np_h, found_natural_floor, (int)player->y);
  }

  sc_respawn(player->client_fd, new_dim);

  sc_setDefaultSpawnPosition(player->client_fd, 8, 80, 8);
  sc_startWaitingForChunks(player->client_fd);

  if (new_dim == DIMENSION_NETHER) {
    player->x = np_x;
    player->z = np_z + 1;
  }

  short cx = div_floor(player->x, 16);
  short cz = div_floor(player->z, 16);
  sc_setCenterChunk(player->client_fd, cx, cz);

  // Pre-generate and send a 3x3 area of chunks around the portal before
  // teleporting the player. Without this, the player can be dropped into the
  // nether with only the center chunk loaded, leaving the surrounding floor
  // invisible to the client and causing them to fall into the void and die
  // while the streamer catches up. Generating a small area synchronously here
  // keeps the spawn experience safe and instant.
  sc_chunkBatchStart(player->client_fd);
  for (int dz = -1; dz <= 1; dz++) {
    for (int dx = -1; dx <= 1; dx++) {
      generate_chunk_data(cx + dx, cz + dz, new_dim);
      sc_chunkDataAndUpdateLight(player->client_fd, cx + dx, cz + dz, new_dim);
    }
  }
  sc_chunkBatchFinished(player->client_fd, 1);

  // Create portal blocks AFTER respawn + chunk send so client doesn't discard them
  if (new_dim == DIMENSION_NETHER) {
    // Place an obsidian platform under the portal so the player doesn't
    // fall into lava when stepping through. The platform covers a 5x5 area
    // centered on the portal at floor height.
    for (int px = np_x - 2; px <= np_x + 2; px++) {
      for (int pz = np_z - 2; pz <= np_z + 2; pz++) {
        makeBlockChange(px, np_h, pz, B_obsidian, DIMENSION_NETHER);
      }
    }
    // Clear a compact arrival room around the platform. The fallback spawn can
    // be inside netherrack, so clearing only the player's body is not enough.
    for (int py = np_h + 1; py <= np_h + 4; py++) {
      for (int px = np_x - 2; px <= np_x + 2; px++) {
        for (int pz = np_z - 2; pz <= np_z + 2; pz++) {
          makeBlockChange(px, py, pz, B_air, DIMENSION_NETHER);
        }
      }
    }
    for (int dy = 1; dy <= 3; dy++) {
      makeBlockChange(np_x, np_h + dy, np_z, B_nether_portal, DIMENSION_NETHER);
      makeBlockChange(np_x + 1, np_h + dy, np_z, B_nether_portal, DIMENSION_NETHER);
    }
    // Build an obsidian frame around the portal so it matches what a player
    // would get from a hand-built portal: 2x3 portal interior framed by
    // obsidian on all four sides, top, and bottom. The frame blocks are
    // placed after the portal so any existing air/terrain underneath them
    // is replaced cleanly.
    // Bottom and top rails
    for (int dx = 0; dx < 2; dx++) {
      makeBlockChange(np_x + dx, np_h, np_z, B_obsidian, DIMENSION_NETHER);
      makeBlockChange(np_x + dx, np_h + 4, np_z, B_obsidian, DIMENSION_NETHER);
    }
    // Left and right columns
    for (int dy = 1; dy <= 3; dy++) {
      makeBlockChange(np_x - 1, np_h + dy, np_z, B_obsidian, DIMENSION_NETHER);
      makeBlockChange(np_x + 2, np_h + dy, np_z, B_obsidian, DIMENSION_NETHER);
    }
    // Place the player in front of the portal on the obsidian platform
    // so they step through the portal rather than spawning on top of it.
    player->x = np_x;
    player->z = np_z + 1;
    player->y = (int16_t)(np_h + 1);
    // Recalculate center chunk from new position
    cx = div_floor(player->x, 16);
    cz = div_floor(player->z, 16);
    sc_setCenterChunk(player->client_fd, cx, cz);
  }

  if (new_dim == DIMENSION_OVERWORLD) {
    // Clear the spawn area and, if needed, build a new portal. No obsidian
    // platform in the overworld — the ground is already solid.
    int ow_np_x, ow_np_z;
    int16_t ow_np_h;

    if (had_saved_portal) {
      // Returning through the original portal — the !portal command builds
      // the portal at (portal_ow_x, portal_ow_y, portal_ow_z - 3) with the
      // interior center at (portal_ow_x + 1, portal_ow_z - 3). Only clear
      // the spawn area — no platform needed on solid overworld ground.
      ow_np_x = (int)player->portal_ow_x + 1;
      ow_np_z = (int)player->portal_ow_z - 3;
      ow_np_h = (int16_t)player->portal_ow_y;

      // Clear the blocks where the player spawns (in front of the portal)
      makeBlockChange(ow_np_x, ow_np_h + 1, ow_np_z + 1, B_air, DIMENSION_OVERWORLD);
      makeBlockChange(ow_np_x, ow_np_h + 2, ow_np_z + 1, B_air, DIMENSION_OVERWORLD);
    } else {
      // No existing portal — build a new one at the arrival position.
      uint8_t surface_y = getHeightAt(player->x, player->z);
      ow_np_h = (surface_y > 0) ? (int16_t)surface_y : 60;
      ow_np_x = (int)player->x;
      ow_np_z = (int)player->z - 1;  // portal 1 block north, player stands in front

      // Clear the blocks where the player spawns (in front of the portal)
      makeBlockChange(ow_np_x, ow_np_h + 1, ow_np_z + 1, B_air, DIMENSION_OVERWORLD);
      makeBlockChange(ow_np_x, ow_np_h + 2, ow_np_z + 1, B_air, DIMENSION_OVERWORLD);

      // Portal blocks (2x3 interior)
      for (int dy = 1; dy <= 3; dy++) {
        makeBlockChange(ow_np_x, ow_np_h + dy, ow_np_z, B_nether_portal, DIMENSION_OVERWORLD);
        makeBlockChange(ow_np_x + 1, ow_np_h + dy, ow_np_z, B_nether_portal, DIMENSION_OVERWORLD);
      }
      // Obsidian frame
      for (int dx = 0; dx < 2; dx++) {
        makeBlockChange(ow_np_x + dx, ow_np_h, ow_np_z, B_obsidian, DIMENSION_OVERWORLD);
        makeBlockChange(ow_np_x + dx, ow_np_h + 4, ow_np_z, B_obsidian, DIMENSION_OVERWORLD);
      }
      for (int dy = 1; dy <= 3; dy++) {
        makeBlockChange(ow_np_x - 1, ow_np_h + dy, ow_np_z, B_obsidian, DIMENSION_OVERWORLD);
        makeBlockChange(ow_np_x + 2, ow_np_h + dy, ow_np_z, B_obsidian, DIMENSION_OVERWORLD);
      }
    }

    // Place the player in front of the portal
    player->x = ow_np_x;
    player->z = ow_np_z + 1;
    player->y = ow_np_h + 1;

    // Recalculate center chunk from new position
    cx = div_floor(player->x, 16);
    cz = div_floor(player->z, 16);
    sc_setCenterChunk(player->client_fd, cx, cz);
  } else if (new_dim == DIMENSION_END) {
    // Spawn offset from the exit portal to avoid immediate teleportation back
    // The exit portal is at the center (0,0) from X/Z -2 to 2 at Y=64
    // Spawn at (5, 5) on the island surface, well away from the portal
    player->x = 5;
    player->z = 5;
    player->y = 65;  // On top of the island surface
  }

  // Fall damage is calculated from the last grounded Y. A dimension transfer is
  // a teleport, so reset the baseline to avoid charging the height delta
  // between the Nether portal platform and the Overworld exit position.
  player->grounded_y = player->y;
  player->locked_x = player->x;
  player->locked_y = player->y;
  player->locked_z = player->z;
  player->position_lock_ticks = 2 * TICKS_PER_SECOND;

  player->visited_next = 0;
  for (int j = 0; j < VISITED_HISTORY; j++) {
    player->visited_x[j] = 32767;
    player->visited_z[j] = 32767;
  }
  player->visited_x[0] = cx;
  player->visited_z[0] = cz;
  player->visited_next = 1;

  // The sc_respawn packet above is sent with "data kept" = 0, which tells the
  // client to wipe its local copy of the player's inventory. We have to push
  // the full inventory back down, otherwise the player sees an empty inventory
  // in the new dimension until they pick up an item or rejoin.
  for (uint8_t i = 0; i < 41; i++) {
    sc_setContainerSlot(
      player->client_fd, 0,
      serverSlotToClientSlot(0, i),
      player->inventory_count[i],
      player->inventory_items[i]
    );
  }
  sc_setHeldItem(player->client_fd, player->hotbar);
  sc_setHealth(player->client_fd, player->health, player->hunger, player->saturation);
  if (isPlayerNoclipEnabled(player)) syncPlayerNoclipState(player);

  sc_synchronizePlayerPosition(player->client_fd,
    (float)player->x + 0.5f, (float)player->y, (float)player->z + 0.5f,
    player->yaw * 180.0f / 127.0f, player->pitch * 90.0f / 127.0f);

  syncDimensionEntities(player, old_dim);
}

void switchPlayerDimension(PlayerData *player) {
  uint8_t new_dim = (player->dimension == DIMENSION_OVERWORLD) ? DIMENSION_NETHER : DIMENSION_OVERWORLD;
  switchPlayerToDimension(player, new_dim);
}

void handlePortalTravel(PlayerData *player) {
  if (player->client_fd == -1) return;
  if (player->flags & 0x20) return;

  uint16_t block_feet = getBlockAt2(player->x, player->y, player->z, player->dimension);
  uint16_t block_waist = getBlockAt2(player->x, player->y + 1, player->z, player->dimension);

  if (block_feet == B_nether_portal || block_waist == B_nether_portal) {
    switchPlayerDimension(player);
  } else if (player->dimension == DIMENSION_OVERWORLD && (block_feet == B_end_portal || block_waist == B_end_portal)) {
    switchPlayerToDimension(player, DIMENSION_END);
  } else if (player->dimension == DIMENSION_END && (block_feet == B_end_portal || block_waist == B_end_portal)) {
    // Exit portal in the End takes player back to overworld
    switchPlayerToDimension(player, DIMENSION_OVERWORLD);
  }
}

typedef struct {
  uint16_t item;
  uint8_t min_count;
  uint8_t max_count;
  uint8_t weight;
} ChestLootEntry;

static uint8_t chestLootSlotEmpty(uint8_t *slots, uint8_t slot) {
  uint16_t item = 0;
  uint8_t count = 0;
  memcpy(&item, slots + slot * 3, sizeof(item));
  memcpy(&count, slots + slot * 3 + 2, sizeof(count));
  return item == 0 || count == 0;
}

static void writeChestLootSlot(uint8_t *slots, uint8_t slot, uint16_t item, uint8_t count) {
  memcpy(slots + slot * 3, &item, sizeof(item));
  memcpy(slots + slot * 3 + 2, &count, sizeof(count));
}

static const ChestLootEntry *pickChestLootEntry(const ChestLootEntry *entries, size_t entry_count, uint64_t *seed) {
  uint16_t total_weight = 0;
  for (size_t i = 0; i < entry_count; i++) total_weight += entries[i].weight;
  if (total_weight == 0) return NULL;

  *seed = splitmix64(*seed);
  uint16_t roll = (uint16_t)(*seed % total_weight);
  for (size_t i = 0; i < entry_count; i++) {
    if (roll < entries[i].weight) return &entries[i];
    roll -= entries[i].weight;
  }
  return &entries[entry_count - 1];
}

static void addChestLootStack(uint8_t *slots, const ChestLootEntry *entry, uint64_t *seed) {
  if (entry == NULL || entry->item == 0) return;

  *seed = splitmix64(*seed);
  uint8_t count_range = (entry->max_count >= entry->min_count) ? (entry->max_count - entry->min_count + 1) : 1;
  uint8_t count = entry->min_count + (uint8_t)(*seed % count_range);

  *seed = splitmix64(*seed);
  uint8_t start_slot = (uint8_t)(*seed % 27);
  for (uint8_t attempt = 0; attempt < 27; attempt++) {
    uint8_t slot = (uint8_t)((start_slot + attempt) % 27);
    if (!chestLootSlotEmpty(slots, slot)) continue;
    writeChestLootSlot(slots, slot, entry->item, count);
    return;
  }
}

static void populateDungeonChestLoot(uint8_t *slots, short x, int16_t y, short z, uint8_t variant) {
  uint64_t seed = splitmix64(
    ((uint64_t)(uint16_t)x << 48) ^
    ((uint64_t)(uint16_t)z << 32) ^
    ((uint64_t)(uint16_t)y << 16) ^
    ((uint64_t)variant << 8) ^
    (uint64_t)world_seed ^
    0xD06E0BADC0FFEEULL
  );

  static const ChestLootEntry guaranteed[] = {
    { I_diamond, 1, 2, 5 },
    { I_emerald, 2, 5, 5 },
  };

  static const ChestLootEntry loot_table[] = {
    { I_bread, 2, 6, 10 },
    { I_wheat, 3, 8, 8 },
    { I_string, 2, 7, 8 },
    { I_bone, 2, 8, 8 },
    { I_arrow, 4, 12, 8 },
    { I_bow, 1, 1, 4 },
    { I_coal, 3, 10, 7 },
    { I_redstone, 3, 9, 6 },
    { I_lapis_lazuli, 2, 6, 5 },
    { I_iron_ingot, 1, 5, 7 },
    { I_gold_ingot, 1, 4, 5 },
    { I_bucket, 1, 1, 3 },
  };

  addChestLootStack(slots, pickChestLootEntry(guaranteed, sizeof(guaranteed) / sizeof(guaranteed[0]), &seed), &seed);

  seed = splitmix64(seed);
  uint8_t rolls = 5 + (uint8_t)(seed % 5); // 5..9 additional stacks.
  for (uint8_t i = 0; i < rolls; i++) {
    addChestLootStack(slots, pickChestLootEntry(loot_table, sizeof(loot_table) / sizeof(loot_table[0]), &seed), &seed);
  }
}

// Loot tables for each village profession building.
// Each table has items appropriate to the building's trade.
static void populateVillageChestLoot(uint8_t *slots, short x, int16_t y, short z, uint8_t profession) {
  uint64_t seed = splitmix64(
    ((uint64_t)(uint16_t)x << 48) ^
    ((uint64_t)(uint16_t)z << 32) ^
    ((uint64_t)(uint16_t)y << 16) ^
    ((uint64_t)profession << 8) ^
    (uint64_t)world_seed ^
    0xC0FFEEBEEFULL
  );

  static const ChestLootEntry farmer[] = {
    { I_wheat, 3, 8, 12 },
    { I_carrot, 2, 6, 10 },
    { I_potato, 2, 6, 10 },
    { I_bread, 2, 5, 10 },
    { I_pumpkin_seeds, 1, 4, 8 },
    { I_melon_seeds, 1, 4, 8 },
    { I_beetroot_seeds, 1, 4, 8 },
    { I_bone_meal, 2, 5, 7 },
    { I_hay_block, 1, 3, 6 },
    { I_apple, 1, 3, 6 },
    { I_cookie, 3, 7, 5 },
  };

  static const ChestLootEntry librarian[] = {
    { I_book, 1, 4, 12 },
    { I_paper, 3, 8, 12 },
    { I_feather, 1, 3, 10 },
    { I_ink_sac, 1, 3, 10 },
    { I_writable_book, 1, 2, 8 },
    { I_compass, 1, 1, 5 },
    { I_clock, 1, 1, 4 },
    { I_glass_pane, 2, 6, 8 },
    { I_lantern, 1, 2, 6 },
    { I_candle, 1, 3, 5 },
  };

  static const ChestLootEntry cleric[] = {
    { I_rotten_flesh, 2, 6, 12 },
    { I_gold_ingot, 1, 3, 8 },
    { I_redstone, 2, 6, 10 },
    { I_lapis_lazuli, 2, 6, 10 },
    { I_glowstone_dust, 1, 4, 8 },
    { I_gunpowder, 1, 4, 8 },
    { I_nether_wart, 1, 3, 7 },
    { I_glass_bottle, 1, 3, 7 },
    { I_bone, 2, 5, 6 },
    { I_amethyst_shard, 1, 2, 4 },
  };

  static const ChestLootEntry armorer[] = {
    { I_iron_ingot, 1, 4, 12 },
    { I_coal, 3, 8, 10 },
    { I_iron_nugget, 4, 12, 10 },
    { I_iron_boots, 1, 1, 6 },
    { I_iron_helmet, 1, 1, 5 },
    { I_chainmail_boots, 1, 1, 5 },
    { I_chainmail_helmet, 1, 1, 4 },
    { I_flint_and_steel, 1, 1, 6 },
    { I_bell, 1, 1, 3 },
    { I_iron_block, 1, 2, 5 },
  };

  static const ChestLootEntry butcher[] = {
    { I_porkchop, 2, 6, 10 },
    { I_beef, 2, 6, 10 },
    { I_chicken, 2, 5, 10 },
    { I_cooked_porkchop, 1, 3, 8 },
    { I_cooked_beef, 1, 3, 8 },
    { I_cooked_chicken, 1, 3, 8 },
    { I_wheat, 2, 5, 8 },
    { I_carrot, 1, 4, 6 },
    { I_potato, 1, 4, 6 },
    { I_mutton, 2, 5, 6 },
  };

  static const ChestLootEntry cartographer[] = {
    { I_paper, 4, 12, 12 },
    { I_compass, 1, 2, 8 },
    { I_map, 1, 1, 8 },
    { I_filled_map, 1, 1, 6 },
    { I_glass_pane, 3, 8, 8 },
    { I_book, 1, 3, 6 },
    { I_ink_sac, 1, 2, 6 },
    { I_feather, 1, 3, 6 },
    { I_emerald, 1, 2, 5 },
    { I_cyan_bed, 1, 1, 3 },
  };

  static const ChestLootEntry fisherman[] = {
    { I_cod, 2, 6, 12 },
    { I_salmon, 2, 5, 10 },
    { I_cooked_cod, 1, 3, 8 },
    { I_cooked_salmon, 1, 3, 8 },
    { I_water_bucket, 1, 1, 5 },
    { I_lily_pad, 1, 4, 8 },
    { I_string, 1, 4, 7 },
    { I_bone, 1, 3, 7 },
    { I_tropical_fish, 1, 1, 4 },
    { I_pufferfish, 1, 1, 3 },
  };

  static const ChestLootEntry fletcher[] = {
    { I_stick, 4, 12, 12 },
    { I_feather, 3, 8, 12 },
    { I_flint, 2, 6, 10 },
    { I_arrow, 4, 12, 12 },
    { I_string, 2, 5, 8 },
    { I_gravel, 3, 8, 8 },
    { I_bow, 1, 1, 5 },
    { I_crossbow, 1, 1, 3 },
    { I_tripwire_hook, 1, 3, 6 },
  };

  static const ChestLootEntry leatherworker[] = {
    { I_leather, 2, 6, 12 },
    { I_leather_boots, 1, 1, 8 },
    { I_leather_leggings, 1, 1, 6 },
    { I_leather_chestplate, 1, 1, 5 },
    { I_leather_helmet, 1, 1, 7 },
    { I_rabbit_hide, 1, 4, 8 },
    { I_saddle, 1, 1, 3 },
    { I_rabbit_foot, 1, 1, 3 },
    { I_leather_horse_armor, 1, 1, 2 },
  };

  static const ChestLootEntry mason[] = {
    { I_clay_ball, 2, 6, 10 },
    { I_brick, 2, 6, 10 },
    { I_stone, 2, 6, 8 },
    { I_stone_bricks, 2, 5, 8 },
    { I_granite, 2, 5, 6 },
    { I_diorite, 2, 5, 6 },
    { I_andesite, 2, 5, 6 },
    { I_dripstone_block, 1, 3, 5 },
    { I_bricks, 1, 3, 6 },
    { I_chiseled_stone_bricks, 1, 2, 4 },
  };

  static const ChestLootEntry shepherd[] = {
    { I_white_wool, 2, 6, 12 },
    { I_black_wool, 1, 3, 6 },
    { I_gray_wool, 1, 3, 6 },
    { I_light_gray_wool, 1, 3, 6 },
    { I_brown_wool, 1, 3, 6 },
    { I_shears, 1, 1, 8 },
    { I_string, 2, 6, 8 },
    { I_white_carpet, 2, 5, 6 },
    { I_emerald, 1, 2, 4 },
    { I_painting, 1, 1, 4 },
  };

  static const ChestLootEntry toolsmith[] = {
    { I_iron_ingot, 1, 4, 12 },
    { I_coal, 3, 8, 10 },
    { I_stick, 4, 10, 8 },
    { I_stone_pickaxe, 1, 1, 8 },
    { I_stone_axe, 1, 1, 8 },
    { I_stone_shovel, 1, 1, 8 },
    { I_iron_pickaxe, 1, 1, 5 },
    { I_iron_axe, 1, 1, 4 },
    { I_iron_shovel, 1, 1, 5 },
    { I_iron_hoe, 1, 1, 4 },
  };

  static const ChestLootEntry weaponsmith[] = {
    { I_iron_ingot, 1, 4, 12 },
    { I_coal, 3, 8, 10 },
    { I_flint, 2, 5, 8 },
    { I_obsidian, 1, 3, 5 },
    { I_stone_sword, 1, 1, 8 },
    { I_iron_sword, 1, 1, 6 },
    { I_iron_axe, 1, 1, 5 },
    { I_diamond, 1, 1, 3 },
    { I_arrow, 4, 10, 6 },
    { I_shield, 1, 1, 4 },
  };

  // Map profession to loot table
  static const struct {
    const ChestLootEntry *entries;
    size_t count;
  } loot_tables[] = {
    { farmer,         sizeof(farmer) / sizeof(farmer[0]) },
    { librarian,      sizeof(librarian) / sizeof(librarian[0]) },
    { cleric,         sizeof(cleric) / sizeof(cleric[0]) },
    { armorer,        sizeof(armorer) / sizeof(armorer[0]) },
    { butcher,        sizeof(butcher) / sizeof(butcher[0]) },
    { cartographer,   sizeof(cartographer) / sizeof(cartographer[0]) },
    { fisherman,      sizeof(fisherman) / sizeof(fisherman[0]) },
    { fletcher,       sizeof(fletcher) / sizeof(fletcher[0]) },
    { leatherworker,  sizeof(leatherworker) / sizeof(leatherworker[0]) },
    { mason,          sizeof(mason) / sizeof(mason[0]) },
    { shepherd,       sizeof(shepherd) / sizeof(shepherd[0]) },
    { toolsmith,      sizeof(toolsmith) / sizeof(toolsmith[0]) },
    { weaponsmith,    sizeof(weaponsmith) / sizeof(weaponsmith[0]) },
  };

  if (profession >= 13) {
    // Fallback: generic loot for unknown professions
    static const ChestLootEntry fallback[] = {
      { I_bread, 2, 5, 10 },
      { I_iron_ingot, 1, 3, 8 },
      { I_stick, 3, 8, 8 },
      { I_emerald, 1, 2, 4 },
    };
    for (uint8_t i = 0; i < 3; i++) {
      addChestLootStack(slots, pickChestLootEntry(fallback, sizeof(fallback) / sizeof(fallback[0]), &seed), &seed);
      seed = splitmix64(seed);
    }
    return;
  }

  seed = splitmix64(seed);
  uint8_t rolls = 3 + (uint8_t)(seed % 3); // 3..5 stacks
  for (uint8_t i = 0; i < rolls; i++) {
    addChestLootStack(slots, pickChestLootEntry(loot_tables[profession].entries, loot_tables[profession].count, &seed), &seed);
    seed = splitmix64(seed);
  }
}

static int projectileEntityId(int slot) {
  return -200 - slot;
}

static void removeProjectileFromClients(int slot, uint8_t dimension) {
  int entity_id = projectileEntityId(slot);
  for (int j = 0; j < MAX_PLAYERS; j++) {
    if (player_data[j].client_fd == -1) continue;
    if (player_data[j].dimension != dimension) continue;
    sc_removeEntity(player_data[j].client_fd, entity_id);
  }
}

static int findPlayerArrowSlot(PlayerData *player) {
  for (int i = 0; i < 41; i++) {
    if (player->inventory_items[i] == I_arrow && player->inventory_count[i] > 0) return i;
  }
  return -1;
}

static uint8_t playerHasArrowAvailable(PlayerData *player) {
  return getConfiguredGameMode() == 1 || findPlayerArrowSlot(player) >= 0;
}

static void makeProjectileUuid(ProjectileData *p, int slot) {
  uint32_t r = fast_rand();
  memcpy(p->uuid, &r, 4);
  memcpy(p->uuid + 4, &server_ticks, 4);
  memcpy(p->uuid + 8, &slot, 4);
  memset(p->uuid + 12, 0, 4);
}

static void projectileAngles(double vx, double vy, double vz, float *yaw_deg, float *pitch_deg, uint8_t *yaw_byte, uint8_t *pitch_byte) {
  double horizontal = sqrt(vx * vx + vz * vz);
  double yaw = atan2(vx, vz) * 180.0 / M_PI;
  double pitch = atan2(vy, horizontal) * 180.0 / M_PI;

  if (yaw_deg) *yaw_deg = (float)yaw;
  if (pitch_deg) *pitch_deg = (float)pitch;
  if (yaw_byte) *yaw_byte = (uint8_t)((int)(yaw * 256.0 / 360.0) & 255);
  if (pitch_byte) *pitch_byte = (uint8_t)((int)(pitch * 256.0 / 360.0) & 255);
}

static void syncPlayerInventorySlot(PlayerData *player, int slot) {
  if (slot < 0 || slot > 40) return;
  uint8_t client_slot = serverSlotToClientSlot(0, (uint8_t)slot);
  if (client_slot == 255) return;
  sc_setContainerSlot(player->client_fd, 0, client_slot, player->inventory_count[slot], player->inventory_items[slot]);
  sc_setContainerSlot(player->client_fd, -2, client_slot, player->inventory_count[slot], player->inventory_items[slot]);
}

static void setPlayerActiveHand(PlayerData *player, uint8_t active) {
  EntityData metadata[] = {{
    8,                 // Living Entity active hand flags
    0,                 // Byte
    { active ? 0x01 : 0x00 },
  }};

  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (player_data[i].client_fd == -1) continue;
    if (player_data[i].flags & 0x20) continue;
    if (player_data[i].dimension != player->dimension) continue;
    sc_setEntityMetadata(player_data[i].client_fd, player->client_fd, metadata, 1);
  }
}

static uint8_t shootBowArrow(PlayerData *player) {
  int owner_idx = getPlayerIndexByPointer(player);
  if (owner_idx < 0) return 0;

  uint32_t draw_start = player_bow_draw_start[owner_idx];
  player_bow_draw_start[owner_idx] = 0;
  player_bow_last_use_tick[owner_idx] = 0;
  setPlayerActiveHand(player, 0);

  uint32_t charge_ticks = draw_start == 0 ? (uint32_t)TICKS_PER_SECOND : server_ticks - draw_start;
  if (charge_ticks < 3) charge_ticks = 3;

  int creative = getConfiguredGameMode() == 1;
  int arrow_slot = findPlayerArrowSlot(player);
  if (!creative && arrow_slot < 0) return 0;

  int slot = -1;
  for (int i = 0; i < MAX_PROJECTILES; i++) {
    if (!projectile_data[i].active) { slot = i; break; }
  }
  if (slot < 0) return 0;

  double charge = (double)charge_ticks / (double)TICKS_PER_SECOND;
  double power = (charge * charge + 2.0 * charge) / 3.0;
  if (power > 1.0) power = 1.0;
  if (power < 0.2) power = 0.2;

  ProjectileData *p = &projectile_data[slot];
  p->active = 1;
  p->type = E_ARROW;
  p->owner_index = owner_idx;
  p->dimension = player->dimension;
  p->x = (double)player->x + 0.5;
  p->y = (double)player->y + 1.62;
  p->z = (double)player->z + 0.5;

  double speed = power * 3.0;
  double pitch_rad = player->pitch * M_PI / 254.0;
  double yaw_rad = player->yaw * M_PI / 127.0;
  p->vx = -sin(yaw_rad) * cos(pitch_rad) * speed;
  p->vy = -sin(pitch_rad) * speed;
  p->vz = cos(yaw_rad) * cos(pitch_rad) * speed;
  p->spawn_tick = server_ticks;
  p->damage = (uint8_t)(3 + power * 5.0);
  p->stuck = 0;
  makeProjectileUuid(p, slot);

  if (arrow_slot >= 0) {
    player->inventory_count[arrow_slot]--;
    if (player->inventory_count[arrow_slot] == 0) player->inventory_items[arrow_slot] = 0;
    syncPlayerInventorySlot(player, arrow_slot);
    if (arrow_slot == player->hotbar || arrow_slot == 40) broadcastPlayerEquipment(player);
  }
  if (!creative) damageHeldItem(player, 1);

  int16_t nvx = (int16_t)(p->vx * 8000);
  int16_t nvy = (int16_t)(p->vy * 8000);
  int16_t nvz = (int16_t)(p->vz * 8000);
  uint8_t yaw_byte = 0;
  uint8_t pitch_byte = 0;
  projectileAngles(p->vx, p->vy, p->vz, NULL, NULL, &yaw_byte, &pitch_byte);

  for (int j = 0; j < MAX_PLAYERS; j++) {
    if (player_data[j].client_fd == -1) continue;
    if (player_data[j].dimension != player->dimension) continue;
    sc_spawnEntity(
      player_data[j].client_fd,
      projectileEntityId(slot), p->uuid,
      E_ARROW, p->x, p->y, p->z,
      yaw_byte, pitch_byte, nvx, nvy, nvz
    );
    sc_entityAnimation(player_data[j].client_fd, player->client_fd, 0);
  }

  return 1;
}

static void shootSkeletonArrow(int mob_index, double target_x, double target_y, double target_z) {
  int slot = -1;
  for (int i = 0; i < MAX_PROJECTILES; i++) {
    if (!projectile_data[i].active) { slot = i; break; }
  }
  if (slot < 0) return;

  MobData *mob = &mob_data[mob_index];
  ProjectileData *p = &projectile_data[slot];
  p->active = 1;
  p->type = E_ARROW;
  p->owner_index = -1;
  p->dimension = mob->dimension;
  p->x = mob->x;
  p->y = mob->y + 1.6;
  p->z = mob->z;

  double dx = target_x - p->x;
  double dy = target_y - p->y;
  double dz = target_z - p->z;
  double dist = sqrt(dx * dx + dy * dy + dz * dz);
  if (dist < 0.001) { p->active = 0; return; }

  // Add random spread to make it fair — offset scales with distance
  double spread = 0.6 + dist * 0.06;
  target_x += ((double)(fast_rand() % 201) - 100.0) / 100.0 * spread;
  target_y += ((double)(fast_rand() % 201) - 100.0) / 100.0 * spread;
  target_z += ((double)(fast_rand() % 201) - 100.0) / 100.0 * spread;

  dx = target_x - p->x;
  dy = target_y - p->y;
  dz = target_z - p->z;
  dist = sqrt(dx * dx + dy * dy + dz * dz);
  if (dist < 0.001) { p->active = 0; return; }

  double speed = 2.0;
  p->vx = (dx / dist) * speed;
  p->vy = (dy / dist) * speed;
  p->vz = (dz / dist) * speed;
  p->spawn_tick = server_ticks;
  p->damage = 2;
  p->stuck = 0;
  makeProjectileUuid(p, slot);

  int16_t nvx = (int16_t)(p->vx * 8000);
  int16_t nvy = (int16_t)(p->vy * 8000);
  int16_t nvz = (int16_t)(p->vz * 8000);
  uint8_t yaw_byte = 0, pitch_byte = 0;
  projectileAngles(p->vx, p->vy, p->vz, NULL, NULL, &yaw_byte, &pitch_byte);

  for (int j = 0; j < MAX_PLAYERS; j++) {
    if (player_data[j].client_fd == -1) continue;
    if (player_data[j].dimension != mob->dimension) continue;
    sc_spawnEntity(
      player_data[j].client_fd,
      projectileEntityId(slot), p->uuid,
      E_ARROW, p->x, p->y, p->z,
      yaw_byte, pitch_byte, nvx, nvy, nvz
    );
  }
}

static uint8_t projectileHitsPlayer(ProjectileData *p, double x, double y, double z, PlayerData *player) {
  if (player->client_fd == -1) return 0;
  if (player->flags & 0x20) return 0;
  if (player->dimension != p->dimension) return 0;

  double px = (double)player->x + 0.5;
  double pz = (double)player->z + 0.5;
  double dx = x - px;
  double dz = z - pz;
  if (dx * dx + dz * dz > 0.45) return 0;
  return y >= (double)player->y && y <= (double)player->y + 1.9;
}

static uint8_t projectileHitsMob(ProjectileData *p, double x, double y, double z, MobData *mob) {
  if (mob->type == 0) return 0;
  if ((mob->data & 31) == 0) return 0;
  if (mob->dimension != p->dimension) return 0;

  double dx = x - mob->x;
  double dz = z - mob->z;
  if (dx * dx + dz * dz > 0.55) return 0;
  return y >= mob->y && y <= mob->y + 1.9;
}

static void syncStoppedArrow(int slot, ProjectileData *p) {
  float yaw = 0;
  float pitch = 0;
  projectileAngles(p->vx, p->vy, p->vz, &yaw, &pitch, NULL, NULL);

  for (int j = 0; j < MAX_PLAYERS; j++) {
    if (player_data[j].client_fd == -1) continue;
    if (player_data[j].dimension != p->dimension) continue;
    sc_teleportEntity(player_data[j].client_fd, projectileEntityId(slot), p->x, p->y, p->z, yaw, pitch);
    sc_setEntityVelocity(player_data[j].client_fd, projectileEntityId(slot), 0, 0, 0);
  }
}

static uint8_t tryPickupStuckArrow(int slot, ProjectileData *p) {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    PlayerData *player = &player_data[i];
    if (player->client_fd == -1) continue;
    if (player->flags & 0x20) continue;
    if (player->dimension != p->dimension) continue;

    double px = (double)player->x + 0.5;
    double py = (double)player->y + 1.0;
    double pz = (double)player->z + 0.5;
    double dx = p->x - px;
    double dy = p->y - py;
    double dz = p->z - pz;
    if (dx * dx + dy * dy + dz * dz > 2.25) continue;

    if (givePlayerItem(player, I_arrow, 1) != 0) continue;

    for (int j = 0; j < MAX_PLAYERS; j++) {
      if (player_data[j].client_fd == -1) continue;
      if (player_data[j].dimension != p->dimension) continue;
      sc_pickupItem(player_data[j].client_fd, projectileEntityId(slot), player->client_fd, 1);
    }
    removeProjectileFromClients(slot, p->dimension);
    p->active = 0;
    return 1;
  }

  return 0;
}

static int findArrowHitEntity(ProjectileData *p, double x, double y, double z) {
  for (int i = 0; i < MAX_MOBS; i++) {
    if (projectileHitsMob(p, x, y, z, &mob_data[i])) return -2 - i;
  }

  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (i == p->owner_index) continue;
    if (projectileHitsPlayer(p, x, y, z, &player_data[i])) return player_data[i].client_fd;
  }

  return 0;
}

void handlePlayerUseItem (PlayerData *player, short x, short y, short z, uint8_t face, uint8_t hand) {

  // Check spawn protection for block interactions
  if (face != 255 && is_in_safe_area(x, z, player->dimension)) {
    sc_systemChat(player->client_fd, "§cCannot interact with blocks in protected spawn area", 54);
    return;
  }

  // Get targeted block (if coordinates are provided)
  uint16_t target = face == 255 ? 0 : getBlockAt2(x, y, z, player->dimension);
  // Get held item properties
  uint8_t *count;
  uint16_t *item;
  uint8_t slot;

  if (hand == 0) { // Main hand
    count = &player->inventory_count[player->hotbar];
    item = &player->inventory_items[player->hotbar];
    slot = player->hotbar;
  } else { // Off hand
    count = &player->inventory_count[40];
    item = &player->inventory_items[40];
    slot = 40;
  }

  // Check interaction with containers when not sneaking
  if (!(player->flags & 0x04) && face != 255) {
    if (target == B_crafting_table) {
      sc_openScreen(player->client_fd, 12, "Crafting", 8);
      return;
    } else if (target == B_furnace) {
      sc_openScreen(player->client_fd, 14, "Furnace", 7);
      return;
    } else if (target == B_composter) {
      // Check if the player is holding anything
      if (*count == 0) return;
      // Check if the item is a valid compost item
      uint32_t compost_chance = isCompostItem(*item);
      if (compost_chance != 0) {
        // Take away composted item
        if ((*count -= 1) == 0) *item = 0;
        sc_setContainerSlot(player->client_fd, 0, serverSlotToClientSlot(0, player->hotbar), *count, *item);
        // Test compost chance and give bone meal on success
        if (fast_rand() < compost_chance) {
          givePlayerItem(player, I_bone_meal, 1);
        }
        broadcastPlayerEquipment(player);
        return;
      }
    } else if (target == B_campfire) {
      // Campfire cooking - right-click with raw food to cook it instantly
      if (*count == 0) return;
      uint16_t cooked = getCampfireCookedItem(*item);
      if (cooked != 0) {
        *item = cooked;
        sc_setContainerSlot(player->client_fd, 0, serverSlotToClientSlot(0, player->hotbar), *count, *item);
        broadcastPlayerEquipment(player);
        return;
      }
    } else if (is_bed_block(target)) {
      handleBedInteraction(player, x, y, z, target);
      return;
    }
    #ifdef ALLOW_CHESTS
    else if (target == B_chest && config.allow_chests) {
      int chest_idx = -1;
      for (int i = 0; i < block_changes_count; i++) {
        if (block_changes[i].block != B_chest) continue;
        if (block_changes[i].x != x || block_changes[i].y != y || block_changes[i].z != z) continue;
        chest_idx = i;
        break;
      }
      if (chest_idx < 0) {
        // Generated chest — create block_changes entry on first interaction.
        uint8_t dungeon_direction = 0;
        uint8_t dungeon_variant = 0;
        uint8_t is_dungeon_chest = (
          player->dimension == DIMENSION_OVERWORLD &&
          y >= 0 && y < 256 &&
          getDungeonChestInfo(x, (uint8_t)y, z, &dungeon_direction, &dungeon_variant)
        );

        pthread_mutex_lock(&block_changes_mutex);
        int slots_needed = 15;
        int last_real = -1;
        for (int i = 0; i <= block_changes_count + slots_needed; i++) {
          #ifdef INFINITE_BLOCK_CHANGES
          if (i >= block_changes_capacity - slots_needed) {
            if (!config.infinite_block_changes) break;
            int new_cap = block_changes_capacity * 2;
            if (new_cap <= block_changes_capacity) new_cap = block_changes_capacity + slots_needed + 1;
            BlockChange *nb = (BlockChange *)realloc(block_changes, new_cap * sizeof(BlockChange));
            if (!nb) break;
            block_changes = nb;
            for (int j = block_changes_capacity; j < new_cap; j++) block_changes[j].block = 0xFF;
            block_changes_capacity = new_cap;
          }
          #else
          if (i >= MAX_BLOCK_CHANGES) break;
          #endif
          if (i < block_changes_count && block_changes[i].block != 0xFF) { last_real = i; continue; }
          if (i - last_real != slots_needed) continue;
          int base = last_real + 1;
          block_changes[base].x = x; block_changes[base].y = y; block_changes[base].z = z;
          block_changes[base].block = B_chest; block_changes[base].dimension = player->dimension;
          memset(&block_changes[base + 1], 0, 14 * sizeof(BlockChange));
          uint8_t *_slot = (uint8_t *)&block_changes[base + 1];
          if (is_dungeon_chest) {
            populateDungeonChestLoot(_slot, x, y, z, dungeon_variant);
            special_block_set_state(x, y, z, player->dimension, B_chest, oriented_encode_state(dungeon_direction));
          } else {
            // Populate village chests with building-specific loot
            uint8_t prof = getVillageProfessionAt(x, z);
            if (prof != 0xFF) {
              populateVillageChestLoot(_slot, x, y, z, prof);
            } else {
              // Fallback for surface chests not in a village building
              uint32_t _loot_seed = (uint32_t)(x * 2743 ^ y * 7451 ^ z * 5659);
              int _set = _loot_seed % 4;
              static const uint16_t _items[4][3] = {
                {134, 882, 912},   // oak_log, stone_pickaxe, bread
                {36,  883, 857},   // oak_planks, stone_axe, apple
                {35,  881, 939},   // cobblestone, stone_shovel, cooked_porkchop
                {310, 905, 1066},  // torch, stick, cooked_beef
              };
              static const uint8_t _counts[4][3] = {
                {4, 1, 3}, {6, 1, 2}, {5, 1, 2}, {3, 4, 3}
              };
              for (int _s = 0; _s < 3; _s++) {
                uint16_t _id = _items[_set][_s];
                uint8_t _cnt = _counts[_set][_s] + (_loot_seed >> (_s * 5)) % 3;
                _slot[_s * 3 + 0] = _id & 0xFF;
                _slot[_s * 3 + 1] = (_id >> 8) & 0xFF;
                _slot[_s * 3 + 2] = _cnt;
              }
            }
          }
          if (i >= block_changes_count) block_changes_count = i + 1;
          chest_idx = base;
          uint16_t st = special_block_get_state(x, y, z, player->dimension);
          block_changes[chest_idx + 14].y = oriented_get_direction(st);
          break;
        }
        pthread_mutex_unlock(&block_changes_mutex);
        if (chest_idx < 0) return;
      }
      memcpy(player->craft_items, &chest_idx, sizeof(chest_idx));
      player->flags |= 0x80;
      sc_blockEvent(player->client_fd, x, y, z, 1, 1, B_chest);
      sc_openScreen(player->client_fd, 2, "Chest", 5);
      uint8_t *base = (uint8_t *)&block_changes[chest_idx + 1];
      for (int i = 0; i < 27; i++) {
        uint16_t item;
        uint8_t count;
        memcpy(&item, base + i * 3, 2);
        memcpy(&count, base + i * 3 + 2, 1);
        sc_setContainerSlot(player->client_fd, 2, i, count, item);
      }
      return;
    }
    else if (target == B_barrel && config.allow_chests) {
      // Barrel works like chest (27 slots of storage)
      int barrel_idx = -1;
      for (int i = 0; i < block_changes_count; i++) {
        if (block_changes[i].block != B_barrel) continue;
        if (block_changes[i].x != x || block_changes[i].y != y || block_changes[i].z != z) continue;
        barrel_idx = i;
        break;
      }
      if (barrel_idx < 0) {
        // First interaction with this barrel: create entry
        pthread_mutex_lock(&block_changes_mutex);
        int slots_needed = 15;
        int last_real = -1;
        for (int i = 0; i <= block_changes_count + slots_needed; i++) {
          #ifdef INFINITE_BLOCK_CHANGES
          if (i >= block_changes_capacity - slots_needed) {
            if (!config.infinite_block_changes) break;
            int new_cap = block_changes_capacity * 2;
            if (new_cap <= block_changes_capacity) new_cap = block_changes_capacity + slots_needed + 1;
            BlockChange *nb = (BlockChange *)realloc(block_changes, new_cap * sizeof(BlockChange));
            if (!nb) break;
            block_changes = nb;
            for (int j = block_changes_capacity; j < new_cap; j++) block_changes[j].block = 0xFF;
            block_changes_capacity = new_cap;
          }
          #else
          if (i >= MAX_BLOCK_CHANGES) break;
          #endif
          if (i < block_changes_count && block_changes[i].block != 0xFF) { last_real = i; continue; }
          if (i - last_real != slots_needed) continue;
          int base = last_real + 1;
          block_changes[base].x = x; block_changes[base].y = y; block_changes[base].z = z;
          block_changes[base].block = B_barrel; block_changes[base].dimension = player->dimension;
          memset(&block_changes[base + 1], 0, 14 * sizeof(BlockChange));
          special_block_set_state(x, y, z, player->dimension, B_barrel, barrel_encode_state(0, 0));
          if (i >= block_changes_count) block_changes_count = i + 1;
          barrel_idx = base;
          break;
        }
        pthread_mutex_unlock(&block_changes_mutex);
        if (barrel_idx < 0) return;
      }
      memcpy(player->craft_items, &barrel_idx, sizeof(barrel_idx));
      player->flags |= 0x80;
      // Open barrel animation (open=true)
      uint16_t st = special_block_get_state(x, y, z, player->dimension);
      uint8_t dir = barrel_get_direction(st);
      special_block_set_state(x, y, z, player->dimension, B_barrel, barrel_encode_state(dir, 1));
      sc_blockEvent(player->client_fd, x, y, z, 1, 1, B_barrel);
      sc_openScreen(player->client_fd, 2, "Barrel", 6);
      uint8_t *base = (uint8_t *)&block_changes[barrel_idx + 1];
      for (int i = 0; i < 27; i++) {
        uint16_t item;
        uint8_t count;
        memcpy(&item, base + i * 3, 2);
        memcpy(&count, base + i * 3 + 2, 1);
        sc_setContainerSlot(player->client_fd, 2, i, count, item);
      }
      return;
    }
    else if (target == B_ender_chest && config.allow_chests) {
      // Ender chest uses per-player inventory
      player->flags |= 0x80;
      // Store ender chest position in craft_items for close animation
      player->craft_items[0] = 0xFFFF; // marker: not a block_changes index
      player->craft_items[1] = (uint16_t)(int16_t)x;
      player->craft_items[2] = (uint16_t)y;
      player->craft_items[3] = (uint16_t)(int16_t)z;
      sc_blockEvent(player->client_fd, x, y, z, 1, 1, B_ender_chest);
      sc_openScreen(player->client_fd, 2, "Ender Chest", 11);
      for (int i = 0; i < 27; i++) {
        sc_setContainerSlot(player->client_fd, 2, i, player->ender_chest_count[i], player->ender_chest_items[i]);
      }
      return;
    }
    #endif
    #ifdef ALLOW_DOORS
    else if (config.allow_doors && isDoorBlock(target)) {
      // Use the unified special block system to toggle the door
      // Door state is always stored at the lower half position
      short door_x = x;
      uint8_t door_y = y;
      short door_z = z;

      // Check if this is the upper half by looking for a door at y-1
      // Use getBlockAt2 so village doors (not in block_changes) are found
      uint16_t block_below = getBlockAt2(x, y - 1, z, player->dimension);
      if (isDoorBlock(block_below)) {
        // This is the upper half, use lower half's position for state
        door_y = y - 1;
      }

      uint16_t state = special_block_get_state(door_x, door_y, door_z, player->dimension);
      uint8_t open = door_get_open(state);
      uint8_t hinge = door_get_hinge(state);
      uint8_t direction = door_get_direction(state);

      // Toggle open/closed
      open ^= 1;
      state = door_encode_state(open, hinge, direction);
      special_block_set_state(door_x, door_y, door_z, player->dimension, target, state);

      // Also update upper half's state in the special block table
      // Use getBlockAt2 so village door upper halves are detected
      uint16_t above = getBlockAt2(door_x, door_y + 1, door_z, player->dimension);
      if (isDoorBlock(above)) {
        special_block_set_state(door_x, door_y + 1, door_z, player->dimension, above, state);
      }

      // Broadcast door update to all players
      for (int j = 0; j < MAX_PLAYERS; j++) {
        if (player_data[j].client_fd == -1) continue;
        if (player_data[j].flags & 0x20) continue;
        sendDoorUpdate(player_data[j].client_fd, door_x, door_y, door_z, target, 0, open, direction, hinge);
        if (isDoorBlock(above)) {
          sendDoorUpdate(player_data[j].client_fd, door_x, door_y + 1, door_z, above, 1, open, direction, hinge);
        }
      }
      return;
    }
    #endif
    // Trapdoor toggle on right-click
    else if (config.allow_doors && isTrapdoorBlock(target)) {
      uint16_t state = special_block_get_state(x, y, z, player->dimension);
      uint8_t open = trapdoor_get_open(state);
      uint8_t half = trapdoor_get_half(state);
      uint8_t direction = trapdoor_get_direction(state);

      // Toggle open/closed
      open ^= 1;
      state = trapdoor_encode_state(open, half, direction);
      special_block_set_state(x, y, z, player->dimension, target, state);

      // Broadcast trapdoor update to all players
      for (int j = 0; j < MAX_PLAYERS; j++) {
        if (player_data[j].client_fd == -1) continue;
        if (player_data[j].flags & 0x20) continue;
        sendTrapdoorUpdate(player_data[j].client_fd, x, y, z, target, open, direction, half);
      }
      return;
    }
  }

  // If the selected slot doesn't hold any items, exit
  if (*count == 0) return;

  // Check special item handling
  if (*item == I_bone_meal) {
      uint16_t target_below = getBlockAt2(x, y - 1, z, player->dimension);
    if (isSaplingBlock(target)) {
      // Consume the bone meal (yes, even before checks)
      // Wasting bone meal on misplanted saplings is vanilla behavior
      if ((*count -= 1) == 0) *item = 0;
      sc_setContainerSlot(player->client_fd, 0, serverSlotToClientSlot(0, player->hotbar), *count, *item);
      if ( // Saplings can only grow when placed on these blocks
        target_below == B_dirt ||
        target_below == B_grass_block ||
        target_below == B_mud
      ) {
        // Bone meal has a 25% chance of growing a tree from a sapling
        if ((fast_rand() & 3) == 0) placeSaplingStructure(x, y, z, target);
      }
      broadcastPlayerEquipment(player);
    }
  } else if (handlePlayerEating(player, true)) {
    // Reset eating timer and set eating flag
    player->flagval_16 = 0;
    player->flags |= 0x10;
  } else if (*item == I_shield) {
    player->flags |= 0x0100;
    broadcastPlayerMetadata(player);
    return;
  } else if (getItemDefensePoints(*item) != 0) {
    // For some reason, this action is sent twice when looking at a block
    // Ignore the variant that has coordinates
    if (face != 255) return;
    // Swap to held piece of armor
    uint8_t armor_slot = getArmorItemSlot(*item);
    uint16_t prev_item = player->inventory_items[armor_slot];
    uint8_t prev_count = player->inventory_count[armor_slot];
    player->inventory_items[armor_slot] = *item;
    player->inventory_count[armor_slot] = 1;
    player->inventory_items[slot] = prev_item;
    player->inventory_count[slot] = prev_count;
    // Update client inventory
    sc_setContainerSlot(player->client_fd, -2, serverSlotToClientSlot(0, armor_slot), 1, *item);
    sc_setContainerSlot(player->client_fd, -2, serverSlotToClientSlot(0, slot), prev_count, prev_item);
    broadcastPlayerEquipment(player);
    // Sync weapon attributes since the held item may have changed
    sc_updateEntityAttributes(player->client_fd, player->client_fd, player->inventory_items[player->hotbar]);
    return;
  }

  if (handleBucketUse(player, x, y, z, face, count, item)) return;

  if (*item == I_bow && *count > 0) {
    int player_idx = getPlayerIndexByPointer(player);
    if (player_idx < 0) return;
    if (!playerHasArrowAvailable(player)) return;
    if (player_bow_draw_start[player_idx] == 0) {
      player_bow_draw_start[player_idx] = server_ticks == 0 ? 1 : server_ticks;
      setPlayerActiveHand(player, 1);
    }
    player_bow_last_use_tick[player_idx] = server_ticks == 0 ? 1 : server_ticks;
    return;
  }

  // Handle throwable items when no block is targeted
  if (face == 255 && *item == I_ender_pearl && *count > 0) {
    int owner_idx = getPlayerIndexByPointer(player);
    // Don't allow throwing if player already has an active ender pearl
    int has_active = 0;
    for (int i = 0; i < MAX_PROJECTILES; i++) {
      if (projectile_data[i].active &&
          projectile_data[i].type == E_ENDER_PEARL &&
          projectile_data[i].owner_index == owner_idx) {
        has_active = 1;
        break;
      }
    }
    if (has_active) return;
    // Find a free projectile slot
    int slot = -1;
    for (int i = 0; i < MAX_PROJECTILES; i++) {
      if (!projectile_data[i].active) { slot = i; break; }
    }
    if (slot >= 0) {
      ProjectileData *p = &projectile_data[slot];
      p->active = 1;
      p->type = E_ENDER_PEARL;
      p->owner_index = owner_idx;
      p->dimension = player->dimension;
      p->x = (double)player->x + 0.5;
      p->y = (double)player->y + 1.3;
      p->z = (double)player->z + 0.5;

      double speed = 1.5;
      double pitch_rad = player->pitch * M_PI / 254.0;
      double yaw_rad = player->yaw * M_PI / 127.0;
      p->vx = -sin(yaw_rad) * cos(pitch_rad) * speed;
      p->vy = -sin(pitch_rad) * speed;
      p->vz = cos(yaw_rad) * cos(pitch_rad) * speed;

      p->spawn_tick = server_ticks;
      p->damage = 0;
      p->stuck = 0;
      makeProjectileUuid(p, slot);

      int16_t nvx = (int16_t)(p->vx * 8000);
      int16_t nvy = (int16_t)(p->vy * 8000);
      int16_t nvz = (int16_t)(p->vz * 8000);

      for (int j = 0; j < MAX_PLAYERS; j++) {
        if (player_data[j].client_fd == -1) continue;
        if (player_data[j].dimension != player->dimension) continue;
        sc_spawnEntity(
          player_data[j].client_fd,
          projectileEntityId(slot), p->uuid,
          E_ENDER_PEARL, p->x, p->y, p->z,
          0, 0, nvx, nvy, nvz
        );
      }

      *count -= 1;
      if (*count == 0) *item = 0;
      sc_setContainerSlot(player->client_fd, 0, serverSlotToClientSlot(0, player->hotbar), *count, *item);
      broadcastPlayerEquipment(player);
    }
    return;
  }

  // Bottle of Enchantment: consume the bottle and grant XP
  if (face == 255 && *item == I_experience_bottle && *count > 0) {
    // Consume one bottle
    *count -= 1;
    if (*count == 0) *item = 0;
    sc_setContainerSlot(player->client_fd, 0, serverSlotToClientSlot(0, slot), *count, *item);
    broadcastPlayerEquipment(player);

    // Grant 3-11 XP (matching vanilla bottle o' enchanting range)
    uint16_t xp_amount = 3 + (fast_rand() % 9);
    addXpToPlayer(player, xp_amount);
    return;
  }

  // Don't proceed with block placement if no coordinates were provided
  if (face == 255) return;

  // Handle flint and steel - portal creation or fire placement
  if (*item == I_flint_and_steel) {
    short fx = x, fz = z;
    int16_t fy = y;
    if (!getFaceOffsetBlock(x, y, z, face, &fx, &fy, &fz)) return;
    if (target == B_obsidian && tryCreatePortal(fx, fy, fz, player->dimension)) {
      damageHeldItem(player, 1);
      return;
    }
    if (isReplaceableBlock(getBlockAt2(fx, fy, fz, player->dimension))) {
      makeBlockChange(fx, fy, fz, B_fire, player->dimension);
      damageHeldItem(player, 1);
    }
    return;
  }

  // Hoe tilling: right-click dirt or grass_block with any hoe → farmland
  if (*item == I_wooden_hoe || *item == I_stone_hoe || *item == I_iron_hoe ||
      *item == I_golden_hoe || *item == I_diamond_hoe || *item == I_netherite_hoe) {
    if (target == B_dirt || target == B_grass_block) {
      if (!makeBlockChange(x, y, z, B_farmland, player->dimension)) damageHeldItem(player, 1);
    }
    return;
  }

  // Wheat seed planting: right-click farmland with seeds → wheat crop
  if (*item == I_wheat_seeds && target == B_farmland) {
    short above_x = x, above_z = z;
    uint8_t above_y = y + 1;
    uint16_t above_block = getBlockAt2(above_x, above_y, above_z, player->dimension);
    if (above_block == B_air || isReplaceableBlock(above_block)) {
      if (makeBlockChange(above_x, above_y, above_z, B_wheat, player->dimension)) return;
      special_block_set_state(above_x, above_y, above_z, player->dimension, B_wheat, 0);
      *count -= 1;
      if (*count == 0) *item = 0;
      sc_setContainerSlot(player->client_fd, 0, serverSlotToClientSlot(0, player->hotbar), *count, *item);
      broadcastPlayerEquipment(player);
    }
    return;
  }

  // If the selected item doesn't correspond to a block, exit
  uint16_t block = I_to_B(*item);
  if (block == 0) return;

  // Special handling for bed placement (two horizontal halves)
  if (is_bed_block(block)) {
    short foot_x = x;
    int16_t foot_y = y;
    short foot_z = z;
    if (!getFaceOffsetBlock(x, y, z, face, &foot_x, &foot_y, &foot_z)) return;
    if (foot_y <= 0 || foot_y >= 319) return;

    uint8_t direction = getPlayerFacingDirection(player);
    short dx, dz;
    getBedDirectionOffset(direction, &dx, &dz);
    short head_x = foot_x + dx;
    int16_t head_y = foot_y;
    short head_z = foot_z + dz;

    if (is_in_safe_area(foot_x, foot_z, player->dimension) || is_in_safe_area(head_x, head_z, player->dimension)) {
      sc_systemChat(player->client_fd, "§cCannot interact with blocks in protected spawn area", 54);
      return;
    }

    if (!isReplaceableBlock(getBlockAt2(foot_x, foot_y, foot_z, player->dimension))) return;
    if (!isReplaceableBlock(getBlockAt2(head_x, head_y, head_z, player->dimension))) return;
    if (isReplaceableBlock(getBlockAt2(foot_x, foot_y - 1, foot_z, player->dimension))) return;
    if (isReplaceableBlock(getBlockAt2(head_x, head_y - 1, head_z, player->dimension))) return;

    if (
      (foot_x == player->x && (foot_y == player->y || foot_y == player->y + 1) && foot_z == player->z) ||
      (head_x == player->x && (head_y == player->y || head_y == player->y + 1) && head_z == player->z)
    ) return;

    if (makeBlockChange(foot_x, foot_y, foot_z, block, player->dimension)) return;
    if (makeBlockChange(head_x, head_y, head_z, block, player->dimension)) {
      makeBlockChange(foot_x, foot_y, foot_z, 0, player->dimension);
      return;
    }

    special_block_set_state(foot_x, foot_y, foot_z, player->dimension, block, bed_encode_state(0, 0, direction));
    special_block_set_state(head_x, head_y, head_z, player->dimension, block, bed_encode_state(1, 0, direction));
    broadcastBedUpdate(foot_x, foot_y, foot_z, player->dimension, block);
    broadcastBedUpdate(head_x, head_y, head_z, player->dimension, block);

    *count -= 1;
    if (*count == 0) *item = 0;
    sc_setContainerSlot(player->client_fd, 0, serverSlotToClientSlot(0, slot), *count, *item);
    broadcastPlayerEquipment(player);
    return;
  }

  // Special handling for door placement
  #ifdef ALLOW_DOORS
  if (isDoorBlock(block)) {
    if (!config.allow_doors) return;
    switch (face) {
      case 0: y -= 1; break;
      case 1: y += 1; break;
      case 2: z -= 1; break;
      case 3: z += 1; break;
      case 4: x -= 1; break;
      case 5: x += 1; break;
      default: break;
    }

    uint16_t existing = getBlockAt2(x, y, z, player->dimension);
    uint16_t block_above = getBlockAt2(x, y + 1, z, player->dimension);
    uint16_t block_below = getBlockAt2(x, y - 1, z, player->dimension);

    if (!isReplaceableBlock(existing) || isDoorBlock(existing)) return;
    if (!isReplaceableBlock(block_above) || isDoorBlock(block_above)) return;
    if (isReplaceableBlock(block_below)) return;

    // Calculate door direction based on placement context
    // Direction: 0=north, 1=east, 2=south, 3=west
    // This represents which way the door "faces" (the side the hinge is NOT on)
    uint8_t direction;

    if (face == 0 || face == 1) {
      // Clicked on bottom or top face - use player facing
      // Player yaw: -128 to 127, where 0 is South, 64 is West, -64 is East, 128/-128 is North
      if (player->yaw >= -32 && player->yaw < 32) direction = 0;      // Facing South (centered around 0)
      else if (player->yaw >= 32 && player->yaw < 96) direction = 1;  // Facing West (centered around 64)
      else if (player->yaw >= 96 || player->yaw < -96) direction = 2; // Facing North (centered around 128/-128)
      else direction = 3;                                             // Facing East (centered around -64)
    } else {
      // Clicked on side face - door faces perpendicular to the face
      // Face: 2=north(-z), 3=south(+z), 4=west(-x), 5=east(+x)
      switch (face) {
        case 2: direction = 2; break;  // Clicked north face, door faces south
        case 3: direction = 0; break;  // Clicked south face, door faces north
        case 4: direction = 1; break;  // Clicked west face, door faces east
        case 5: direction = 3; break;  // Clicked east face, door faces west
        default: direction = 0; break;
      }
    }

    // Check if the block's placement conditions are met
    if (
      !( // Is player in the way?
        x == player->x &&
        (y == player->y || y == player->y + 1) &&
        z == player->z
      ) &&
      isReplaceableBlock(getBlockAt2(x, y, z, player->dimension))
    ) {
      // Apply server-side block change for door - makeBlockChange handles both halves
      if (makeBlockChange(x, y, z, block, player->dimension)) return;

      // Update the door state with the correct direction in the unified special block table
      // makeBlockChange already set state for lower half, but we need to update it with direction
      // and set state for upper half
      uint16_t state = door_encode_state(0, 0, direction);  // closed, hinge left, direction
      special_block_set_state(x, y, z, player->dimension, block, state);
      special_block_set_state(x, y + 1, z, player->dimension, block, state);

      // Send door updates with proper state to all players
      for (int j = 0; j < MAX_PLAYERS; j++) {
        if (player_data[j].client_fd == -1) continue;
        if (player_data[j].flags & 0x20) continue;
        sendDoorUpdate(player_data[j].client_fd, x, y, z, block, 0, 0, direction, 0);
        sendDoorUpdate(player_data[j].client_fd, x, y + 1, z, block, 1, 0, direction, 0);
      }

      // Decrease item amount in selected slot
      *count -= 1;
      // Clear item id in slot if amount is zero
      if (*count == 0) *item = 0;

      // Sync hotbar contents to player
      sc_setContainerSlot(player->client_fd, 0, serverSlotToClientSlot(0, player->hotbar), *count, *item);
      broadcastPlayerEquipment(player);
      return;
    }
  }
  #endif

  // Special handling for trapdoor placement
  if (isTrapdoorBlock(block)) {
    if (!config.allow_doors) return;
    switch (face) {
      case 0: y -= 1; break;
      case 1: y += 1; break;
      case 2: z -= 1; break;
      case 3: z += 1; break;
      case 4: x -= 1; break;
      case 5: x += 1; break;
      default: break;
    }

    // Calculate trapdoor direction based on placement face
    uint8_t direction;
    uint8_t half; // 0 = bottom half, 1 = top half

    if (face == 0) {
      // Clicked bottom face — place on top half
      half = 1;
      if (player->yaw >= -32 && player->yaw < 32) direction = 0;
      else if (player->yaw >= 32 && player->yaw < 96) direction = 1;
      else if (player->yaw >= 96 || player->yaw < -96) direction = 2;
      else direction = 3;
    } else if (face == 1) {
      // Clicked top face — place on bottom half
      half = 0;
      if (player->yaw >= -32 && player->yaw < 32) direction = 0;
      else if (player->yaw >= 32 && player->yaw < 96) direction = 1;
      else if (player->yaw >= 96 || player->yaw < -96) direction = 2;
      else direction = 3;
    } else {
      // Clicked side face — trapdoor goes on side, bottom half
      half = 0;
      switch (face) {
        case 2: direction = 0; break;  // North face
        case 3: direction = 2; break;  // South face
        case 4: direction = 3; break;  // West face
        case 5: direction = 1; break;  // East face
        default: direction = 0; break;
      }
    }

    // Check placement validity
    if (
      !(x == player->x && y == player->y && z == player->z) &&
      isReplaceableBlock(getBlockAt2(x, y, z, player->dimension))
    ) {
      if (makeBlockChange(x, y, z, block, player->dimension)) return;

      // Store trapdoor state (open=0 by default)
      uint16_t state = trapdoor_encode_state(0, half, direction);
      special_block_set_state(x, y, z, player->dimension, block, state);

      // Broadcast trapdoor update
      for (int j = 0; j < MAX_PLAYERS; j++) {
        if (player_data[j].client_fd == -1) continue;
        if (player_data[j].flags & 0x20) continue;
        sendTrapdoorUpdate(player_data[j].client_fd, x, y, z, block, 0, direction, half);
      }

      *count -= 1;
      if (*count == 0) *item = 0;
      sc_setContainerSlot(player->client_fd, 0, serverSlotToClientSlot(0, player->hotbar), *count, *item);
      broadcastPlayerEquipment(player);
      return;
    }
  }

  // Special handling for oriented block placement (chests, furnaces, barrels, ender chests, lecterns)
  if (isOrientedBlock(block) || block == B_lectern) {
    if ((block == B_chest || block == B_ender_chest) && !config.allow_chests) return;
    // Determine placement position
    switch (face) {
      case 0: y -= 1; break;
      case 1: y += 1; break;
      case 2: z -= 1; break;
      case 3: z += 1; break;
      case 4: x -= 1; break;
      case 5: x += 1; break;
      default: break;
    }

    uint8_t direction;
    if (block == B_barrel) {
      // Barrels can face in 6 directions (lid faces toward the player)
      // The clicked block face tells us which side the player is on:
      // the barrel is placed on the side of that face, so the lid
      // should face the player — which is the direction of the face.
      // face: 0=bottom,1=top,2=north,3=south,4=west,5=east
      // barrel direction: 4=up,5=down,2=south,0=north,1=east,3=west
      static const uint8_t face_to_barrel[6] = {5, 4, 0, 2, 3, 1};
      direction = face_to_barrel[face & 7];
    } else {
      // Chests, furnaces, ender chests: horizontal only (4 directions)
      if (player->yaw >= -96 && player->yaw < -32) direction = 3;   // East
      else if (player->yaw >= -32 && player->yaw < 32) direction = 1; // South
      else if (player->yaw >= 32 && player->yaw < 96) direction = 2;  // West
      else direction = 0;                                             // North
      // Chests and furnaces face TOWARDS the player
      direction ^= 1;
    }

    // Check if the block's placement conditions are met
    if (
      !( // Is player in the way?
        x == player->x &&
        (y == player->y || y == player->y + 1) &&
        z == player->z
      ) &&
      isReplaceableBlock(getBlockAt2(x, y, z, player->dimension))
    ) {
      // Apply server-side block change (persists to block_changes and disk)
      if (makeBlockChange(x, y, z, block, player->dimension)) return;

      // Update state with direction in the unified special block table
      uint16_t state;
      if (block == B_barrel) {
        state = barrel_encode_state(direction, 0);
      } else if (block == B_ender_chest) {
        state = ender_chest_encode_state(direction, 0);
      } else if (block == B_lectern) {
        // Lectern uses separate direction mapping (horizontal_state_rows order: north=0, south=1, west=2, east=3)
        static const uint8_t horiz_dir[4] = {0, 1, 2, 3}; // internal(north=0, east=1, south=2, west=3) -> table(north=0, south=1, west=2, east=3)
        static const uint8_t yaw_to_horiz[4] = {0, 3, 1, 2}; // yaw(north=0, east=1, south=2, west=3) -> table(north=0, south=1, west=2, east=3)
        // Player yaw: -128 to 127, 0=South, -64=East, 64=West
        uint8_t yaw_dir;
        if (player->yaw >= -96 && player->yaw < -32) yaw_dir = 1;   // East
        else if (player->yaw >= -32 && player->yaw < 32) yaw_dir = 2; // South
        else if (player->yaw >= 32 && player->yaw < 96) yaw_dir = 3;  // West
        else yaw_dir = 0;                                              // North
        state = horizontal_facing_encode_state(yaw_to_horiz[yaw_dir]);
      } else {
        state = oriented_encode_state(direction);
      }
      special_block_set_state(x, y, z, player->dimension, block, state);

      // Store direction in legacy field for persistence across restarts
      if (block == B_chest) {
        for (int i = 0; i < block_changes_count; i++) {
          if (block_changes[i].block == B_chest &&
              block_changes[i].x == x && block_changes[i].y == y && block_changes[i].z == z) {
            block_changes[i + 14].y = direction;
            break;
          }
        }
      }

      // Send state updates to all players
      for (int j = 0; j < MAX_PLAYERS; j++) {
        if (player_data[j].client_fd == -1) continue;
        if (player_data[j].flags & 0x20) continue;
        if (block == B_lectern) {
          uint16_t st = special_block_get_state(x, y, z, player->dimension);
          uint8_t horiz_dir = st & 3;
          uint16_t sid = get_horizontal_state_id(block, horiz_dir);
          sc_blockUpdateState(player_data[j].client_fd, x, y, z, sid);
        } else {
          sendOrientedUpdate(player_data[j].client_fd, x, y, z, block, direction);
        }
      }

      // Decrease item amount in selected slot
      *count -= 1;
      if (*count == 0) *item = 0;
      sc_setContainerSlot(player->client_fd, 0, serverSlotToClientSlot(0, player->hotbar), *count, *item);
      broadcastPlayerEquipment(player);
      return;
    }
    return;
  }

  // Special handling for stair placement
  if (isStairBlock(block)) {
    // Determine placement position
    switch (face) {
      case 0: y -= 1; break;
      case 1: y += 1; break;
      case 2: z -= 1; break;
      case 3: z += 1; break;
      case 4: x -= 1; break;
      case 5: x += 1; break;
      default: break;
    }

    // Determine direction based on player facing
    // Player yaw: -128 to 127, where 0 is South, -64 is East, 64 is West, 128/-128 is North
    uint8_t direction;
    if (player->yaw >= -32 && player->yaw < 32) direction = 2;       // South
    else if (player->yaw >= -96 && player->yaw < -32) direction = 1; // East
    else if (player->yaw >= 32 && player->yaw < 96) direction = 3;   // West
    else direction = 0;                                               // North

    // Determine if upside down based on where on the block the player clicked
    uint8_t half = 0; // 0 = bottom, 1 = top
    if (face == 0) half = 1;

    // Check if the block's placement conditions are met
    if (
      !( // Is player in the way?
        x == player->x &&
        (y == player->y || y == player->y + 1) &&
        z == player->z
      ) &&
      isReplaceableBlock(getBlockAt2(x, y, z, player->dimension))
    ) {
      // Apply server-side block change
      if (makeBlockChange(x, y, z, block, player->dimension)) return;

      // Update stair state in the unified special block table
      uint16_t state = stair_encode_state(half, direction);
      special_block_set_state(x, y, z, player->dimension, block, state);

      // Send stair updates to all players
      for (int j = 0; j < MAX_PLAYERS; j++) {
        if (player_data[j].client_fd == -1) continue;
        if (player_data[j].flags & 0x20) continue;
        sendStairUpdate(player_data[j].client_fd, x, y, z, block, half, direction);
      }

      // Decrease item amount
      *count -= 1;
      if (*count == 0) *item = 0;
      sc_setContainerSlot(player->client_fd, 0, serverSlotToClientSlot(0, player->hotbar), *count, *item);
      broadcastPlayerEquipment(player);
      return;
    }
    return;
  }

  // Special handling for torch/wall_torch placement
  if (block == B_torch) {
    // Compute placement position
    int32_t tx = x, tz = z;
    int16_t ty = y;
    switch (face) {
      case 0: ty -= 1; break;
      case 1: ty += 1; break;
      case 2: tz -= 1; break;
      case 3: tz += 1; break;
      case 4: tx -= 1; break;
      case 5: tx += 1; break;
      default: break;
    }

    // Check if placement position is valid
    if (!isReplaceableBlock(getBlockAt2(tx, ty, tz, player->dimension))) return;

    if (face == 0 || face == 1) {
      // Placing on floor or ceiling - use standing torch
      if (makeBlockChange(tx, ty, tz, B_torch, player->dimension)) return;
    } else {
      // Placing on wall - use wall torch with facing
      // Determine facing based on which face was clicked
      // Face 2=north, 3=south, 4=west, 5=east
      // wall_torch facing property: the wall it attaches to
      // horizontal_state_rows order: north=0, south=1, west=2, east=3
      uint8_t direction;
      switch (face) {
        case 2: direction = 0; break;  // Attached to north wall
        case 3: direction = 1; break;  // Attached to south wall
        case 4: direction = 2; break;  // Attached to west wall
        case 5: direction = 3; break;  // Attached to east wall
        default: direction = 0; break;
      }
      if (makeBlockChange(tx, ty, tz, B_wall_torch, player->dimension)) return;
      special_block_set_state(tx, ty, tz, player->dimension, B_wall_torch, horizontal_facing_encode_state(direction));
      // Also store the direction as a backup in the state entry so it survives
      // world.json save/load even if the special_blocks table entry is lost.
      // The state entry is always at block_changes[k+1] for horizontal blocks.
      for (int k = 0; k < block_changes_count; k++) {
        if (block_changes[k].block == B_wall_torch &&
            block_changes[k].x == tx &&
            block_changes[k].y == ty &&
            block_changes[k].z == tz &&
            k + 1 < block_changes_count) {
          block_changes[k + 1].block = (uint16_t)(direction + 1); // store 1-4, 0 means not set
          break;
        }
      }
      // Broadcast the correct state ID to all players so the torch faces the right wall
      uint16_t sid = get_horizontal_state_id(B_wall_torch, direction);
      for (int j = 0; j < MAX_PLAYERS; j++) {
        if (player_data[j].client_fd == -1) continue;
        if (player_data[j].flags & 0x20) continue;
        sc_blockUpdateState(player_data[j].client_fd, tx, ty, tz, sid);
      }
    }

    // Decrease item amount
    *count -= 1;
    if (*count == 0) *item = 0;
    sc_setContainerSlot(player->client_fd, 0, serverSlotToClientSlot(0, player->hotbar), *count, *item);
    broadcastPlayerEquipment(player);
    return;
  }

  // Special handling for lantern placement (hanging from ceiling vs standing)
  if (block == B_lantern) {
    int32_t lx = x, lz = z;
    int16_t ly = y;
    switch (face) {
      case 0: ly -= 1; break;
      case 1: ly += 1; break;
      case 2: lz -= 1; break;
      case 3: lz += 1; break;
      case 4: lx -= 1; break;
      case 5: lx += 1; break;
      default: break;
    }

    if (!isReplaceableBlock(getBlockAt2(lx, ly, lz, player->dimension))) return;

    uint8_t hanging = (face == 0) ? 1 : 0;
    if (makeBlockChange(lx, ly, lz, B_lantern, player->dimension)) return;
    special_block_set_state(lx, ly, lz, player->dimension, B_lantern, horizontal_facing_encode_state(hanging));
    // Store hanging+1 as backup in the state entry
    for (int _k = 0; _k < block_changes_count; _k++) {
      if (block_changes[_k].block == B_lantern &&
          block_changes[_k].x == lx && block_changes[_k].y == ly && block_changes[_k].z == lz &&
          _k + 1 < block_changes_count) {
        block_changes[_k + 1].block = (uint16_t)(hanging + 1);
        break;
      }
    }

    // Broadcast the correct state ID
    uint16_t state_id = block_palette[B_lantern] - (hanging ? 2 : 0);
    for (int j = 0; j < MAX_PLAYERS; j++) {
      if (player_data[j].client_fd == -1) continue;
      if (player_data[j].flags & 0x20) continue;
      sc_blockUpdateState(player_data[j].client_fd, lx, ly, lz, state_id);
    }

    *count -= 1;
    if (*count == 0) *item = 0;
    sc_setContainerSlot(player->client_fd, 0, serverSlotToClientSlot(0, player->hotbar), *count, *item);
    broadcastPlayerEquipment(player);
    return;
  }

  // Broadcast correct fence state immediately after placement
  if (is_fence_block(block)) {
    switch (face) {
      case 0: y -= 1; break;
      case 1: y += 1; break;
      case 2: z -= 1; break;
      case 3: z += 1; break;
      case 4: x -= 1; break;
      case 5: x += 1; break;
      default: break;
    }
    if (!isReplaceableBlock(getBlockAt2(x, y, z, player->dimension))) return;
    if (makeBlockChange(x, y, z, block, player->dimension)) return;

    // Recompute and broadcast state for the placed fence and all neighbor fences
    int _nxf[4] = {x, x, x - 1, x + 1};
    int _nzf[4] = {z - 1, z + 1, z, z};
    for (int _ni = -1; _ni < 4; _ni++) {
      int _rx = (_ni < 0) ? x : _nxf[_ni];
      int _rz = (_ni < 0) ? z : _nzf[_ni];
      int _ry = y;
      uint16_t _nb = getBlockAt2(_rx, _ry, _rz, player->dimension);
      // For neighbors (not the placed block itself), only update if it's a fence
      if (_ni >= 0 && !is_fence_block(_nb)) continue;
      // For the placed block itself, also continue if it's somehow not a fence
      if (_ni < 0 && !is_fence_block(_nb)) continue;
      uint16_t _nn = getBlockAt2(_rx, _ry, _rz - 1, player->dimension);
      uint16_t _ne = getBlockAt2(_rx + 1, _ry, _rz, player->dimension);
      uint16_t _ns = getBlockAt2(_rx, _ry, _rz + 1, player->dimension);
      uint16_t _nw = getBlockAt2(_rx - 1, _ry, _rz, player->dimension);
      uint8_t _rn = isFenceSolidBlock(_nn) ? 1 : 0;
      uint8_t _re = isFenceSolidBlock(_ne) ? 1 : 0;
      uint8_t _rs = isFenceSolidBlock(_ns) ? 1 : 0;
      uint8_t _rw = isFenceSolidBlock(_nw) ? 1 : 0;
      special_block_set_state(_rx, _ry, _rz, player->dimension, _nb, fence_encode_state(_rn, _re, _rs, _rw));
      uint8_t _conn = _rn | (_re << 1) | (_rs << 2) | (_rw << 3);
      uint16_t _fsid = get_fence_state_id(_nb, _conn);
      for (int _j = 0; _j < MAX_PLAYERS; _j++) {
        if (player_data[_j].client_fd == -1) continue;
        if (player_data[_j].flags & 0x20) continue;
        sc_blockUpdateState(player_data[_j].client_fd, _rx, _ry, _rz, _fsid);
      }
    }
    *count -= 1;
    if (*count == 0) *item = 0;
    sc_setContainerSlot(player->client_fd, 0, serverSlotToClientSlot(0, player->hotbar), *count, *item);
    broadcastPlayerEquipment(player);
    return;
  }

  // Special handling for ladder placement (must attach to a wall)
  if (block == B_ladder) {
    // Ladders can only be placed on walls (faces 2-5), not floor/ceiling
    if (face < 2 || face > 5) return;

    // Compute placement position based on clicked face
    short lx = x, lz = z;
    int16_t ly = y;
    switch (face) {
      case 2: lz -= 1; break;  // north face
      case 3: lz += 1; break;  // south face
      case 4: lx -= 1; break;  // west face
      case 5: lx += 1; break;  // east face
      default: return;
    }

    // Check the placement position is replaceable (air, water, etc.)
    if (!isReplaceableBlock(getBlockAt2(lx, ly, lz, player->dimension))) return;

    // The block we clicked on (x, y, z) is the wall — must be solid
    // (not passable, not replaceable)
    uint16_t wall_block = getBlockAt2(x, y, z, player->dimension);
    if (isReplaceableBlock(wall_block) || isPassableBlock(wall_block)) return;

    // Determine facing direction based on which face was clicked.
    // Face 2=north, 3=south, 4=west, 5=east
    // horizontal_state_rows order: north=0, south=1, west=2, east=3
    uint8_t direction;
    switch (face) {
      case 2: direction = 0; break;  // Attached to north wall, faces north
      case 3: direction = 1; break;  // Attached to south wall, faces south
      case 4: direction = 2; break;  // Attached to west wall, faces west
      case 5: direction = 3; break;  // Attached to east wall, faces east
      default: direction = 0; break;
    }

    // Apply server-side block change
    if (makeBlockChange(lx, ly, lz, B_ladder, player->dimension)) return;

    // Store ladder facing direction
    special_block_set_state(lx, ly, lz, player->dimension, B_ladder, horizontal_facing_encode_state(direction));

    // Broadcast the correct state ID to all players so the ladder faces the right wall
    uint16_t sid = get_horizontal_state_id(B_ladder, direction);
    for (int j = 0; j < MAX_PLAYERS; j++) {
      if (player_data[j].client_fd == -1) continue;
      if (player_data[j].flags & 0x20) continue;
      sc_blockUpdateState(player_data[j].client_fd, lx, ly, lz, sid);
    }

    // Decrease item amount
    *count -= 1;
    if (*count == 0) *item = 0;
    sc_setContainerSlot(player->client_fd, 0, serverSlotToClientSlot(0, player->hotbar), *count, *item);
    broadcastPlayerEquipment(player);
    return;
  }

  switch (face) {
    case 0: y -= 1; break;
    case 1: y += 1; break;
    case 2: z -= 1; break;
    case 3: z += 1; break;
    case 4: x -= 1; break;
    case 5: x += 1; break;
    default: break;
  }

  // Check if the block's placement conditions are met
  if (
    !( // Is player in the way?
      !isPassableBlock(block) &&
      x == player->x &&
      (y == player->y || y == player->y + 1) &&
      z == player->z
    ) &&
    isReplaceableBlock(getBlockAt2(x, y, z, player->dimension)) &&
    (!isColumnBlock(block) || getBlockAt2(x, y - 1, z, player->dimension) != B_air)
  ) {
    // Apply server-side block change
    if (makeBlockChange(x, y, z, block, player->dimension)) return;
    // Instant tree growth for saplings
    if (isSaplingBlock(block)) {
    uint16_t target_below = getBlockAt2(x, y - 1, z, player->dimension);
      if (
        target_below == B_dirt ||
        target_below == B_grass_block ||
        target_below == B_mud
      ) {
        placeSaplingStructure(x, y, z, block);
      }
    }
    // Decrease item amount in selected slot
    *count -= 1;
    // Clear item id in slot if amount is zero
    if (*count == 0) *item = 0;
    // Calculate fluid flow
    #ifdef DO_FLUID_FLOW
    if (config.do_fluid_flow) {
      checkFluidUpdate(x, y + 1, z, getBlockAt2(x, y + 1, z, player->dimension));
      checkFluidUpdate(x - 1, y, z, getBlockAt2(x - 1, y, z, player->dimension));
      checkFluidUpdate(x + 1, y, z, getBlockAt2(x + 1, y, z, player->dimension));
      checkFluidUpdate(x, y, z - 1, getBlockAt2(x, y, z - 1, player->dimension));
      checkFluidUpdate(x, y, z + 1, getBlockAt2(x, y, z + 1, player->dimension));
    }
    #endif
  }

  // Sync hotbar contents to player
  sc_setContainerSlot(player->client_fd, 0, serverSlotToClientSlot(0, player->hotbar), *count, *item);
  broadcastPlayerEquipment(player);

}

void spawnMob (uint8_t type, short x, uint8_t y, short z, uint8_t health, uint8_t dimension, uint8_t profession) {

  for (int i = 0; i < MAX_MOBS; i ++) {
    // Look for type 0 (unallocated)
    if (mob_data[i].type != 0) continue;

    // Assign it the input parameters with sub-block centering
    mob_data[i].type = type;
    mob_data[i].x = (double)x + 0.5;
    mob_data[i].y = (double)y;  // Land mobs at integer Y, fish handled separately
    mob_data[i].z = (double)z + 0.5;
    mob_data[i].data = health & 31;
    mob_data[i].anger_timer = 0;
    mob_data[i].dimension = dimension;
    // Initialize movement to 0
    mob_data[i].move_dx = 0;
    mob_data[i].move_dz = 0;
    mob_data[i].move_dy = 0;
    mob_data[i].move_timer = 0;
    mob_data[i].yaw_store = 0;
    mob_data[i].look_timer = 0;
    mob_data[i].look_yaw = 0;
    mob_data[i].look_pitch = 0;
    mob_data[i].profession = profession;
    // Reset trade usage tracking
    for (int t = 0; t < 5; t++) mob_trade_uses[i][t] = 0;

    // Forge a UUID from a random number and the mob's index
    uint8_t uuid[16];
    uint32_t r = fast_rand();
    memcpy(uuid, &r, 4);
    memcpy(uuid + 4, &i, 4);
    // Zero out the remaining bytes to ensure valid UUIDs
    memset(uuid + 8, 0, 8);

    // Broadcast entity creation to all players in the same dimension
    for (int j = 0; j < MAX_PLAYERS; j ++) {
      if (player_data[j].client_fd == -1) continue;
      if (player_data[j].dimension != dimension) continue;
      sc_spawnEntity(
        player_data[j].client_fd,
        -2 - i, // Use negative IDs to avoid conflicts with player IDs
        uuid, // Use the UUID generated above
        type, mob_data[i].x, mob_data[i].y, mob_data[i].z,
        // Face opposite of the player, as if looking at them when spawning
        (player_data[j].yaw + 127) & 255, 0,
        0, 0, 0
      );
    }

    // Give skeletons a bow
    if (type == E_SKELETON) {
      for (int j = 0; j < MAX_PLAYERS; j ++) {
        if (player_data[j].client_fd == -1) continue;
        if (player_data[j].dimension != dimension) continue;
        sc_setMobEquipment(player_data[j].client_fd, -2 - i, I_bow);
      }
    }

    // Send entity metadata (villager profession, sheep sheared, etc.)
    broadcastMobMetadata(-1, -2 - i);

    break;
  }

}

// Calculate the total XP needed to reach the next level
static uint16_t xpNeededForNextLevel(uint16_t level) {
  if (level <= 15) {
    return 2 * level + 7;
  } else if (level <= 30) {
    return 5 * level - 38;
  } else {
    return 9 * level - 158;
  }
}

// Give XP to a player and handle leveling up
static void addXpToPlayer(PlayerData *player, uint16_t amount) {
  if (player->client_fd == -1) return;
  if (amount == 0) return;

  player->xp_total += amount;
  float remaining = (float)amount + player->xp_progress * (float)xpNeededForNextLevel(player->xp_level);

  while (remaining > 0) {
    uint16_t needed = xpNeededForNextLevel(player->xp_level);
    if (remaining >= (float)needed) {
      remaining -= (float)needed;
      player->xp_level++;
      player->xp_progress = 0.0f;
    } else {
      player->xp_progress = remaining / (float)needed;
      remaining = 0;
    }
  }

  sc_setExperience(player->client_fd, player->xp_total, player->xp_level, player->xp_progress);
}

// Get the XP count value (1-3) for an XP orb based on its value
static uint8_t xpOrbCountFromValue(uint8_t value) {
  if (value <= 2) return 1;
  if (value <= 6) return 2;
  return 3;
}

// Next-slot hint for XP orb allocation
static int next_xp_orb_slot = 0;

void spawnXpOrb (double x, double y, double z, uint8_t value, uint8_t dimension) {
  if (value == 0) return;

  for (int i = 0; i < MAX_XP_ORBS; i++) {
    int idx = (next_xp_orb_slot + i) % MAX_XP_ORBS;
    if (xp_orb_data[idx].active) continue;

    xp_orb_data[idx].active = 1;
    xp_orb_data[idx].x = x;
    xp_orb_data[idx].y = y;
    xp_orb_data[idx].z = z;
    xp_orb_data[idx].value = value;
    xp_orb_data[idx].count = xpOrbCountFromValue(value);
    xp_orb_data[idx].dimension = dimension;
    xp_orb_data[idx].age = 0;

    // Generate a UUID for this orb
    uint8_t uuid[16];
    uint32_t r = fast_rand();
    memcpy(uuid, &r, 4);
    memcpy(uuid + 4, &idx, 4);
    memset(uuid + 8, 0, 8);

    int entity_id = XP_ORB_ENTITY_ID_BASE - idx;

    // Broadcast the orb to all players in the same dimension
    for (int j = 0; j < MAX_PLAYERS; j++) {
      if (player_data[j].client_fd == -1) continue;
      if (player_data[j].dimension != dimension) continue;
      sc_spawnEntity(
        player_data[j].client_fd,
        entity_id, uuid, E_XP_ORB,
        x, y, z,
        0, 0, xp_orb_data[idx].count, 0, 0
      );
    }

    next_xp_orb_slot = (idx + 1) % MAX_XP_ORBS;
    break;
  }
}

// Next-slot hint for item entity allocation (avoids scanning from 0 every time)
static int next_item_entity_slot = 0;

// Spawn an item entity on the ground
void spawnItemEntity (double x, double y, double z, uint16_t item, uint8_t count, uint8_t dimension,
    double vx, double vy, double vz) {
  if (item == 0 || count == 0) return;

  // Server simulates physics — client just renders where we tell it
  int16_t mvx = 0;
  int16_t mvy = 0;
  int16_t mvz = 0;

  // Scan for free slot starting from the last known free position
  for (int i = 0; i < MAX_ITEM_ENTITIES; i++) {
    int idx = (next_item_entity_slot + i) % MAX_ITEM_ENTITIES;
    if (item_entity_data[idx].active) continue;

    item_entity_data[idx].active = 1;
    item_entity_data[idx].x = x;
    item_entity_data[idx].y = y;
    item_entity_data[idx].z = z;
    item_entity_data[idx].vx = vx;
    item_entity_data[idx].vy = vy;
    item_entity_data[idx].vz = vz;
    item_entity_data[idx].on_ground = 0;
    item_entity_data[idx].item = item;
    item_entity_data[idx].count = count;
    item_entity_data[idx].dimension = dimension;
    item_entity_data[idx].age = 0;

    int entity_id = ITEM_ENTITY_ID_BASE - idx;
    uint8_t uuid[16];
    uint32_t r = fast_rand();
    memcpy(uuid, &r, 4);
    memcpy(uuid + 4, &idx, 4);
    memset(uuid + 8, 0, 8);

    for (int j = 0; j < MAX_PLAYERS; j++) {
      if (player_data[j].client_fd == -1) continue;
      if (player_data[j].dimension != dimension) continue;
      sc_spawnEntity(player_data[j].client_fd, entity_id, uuid, 69, x, y, z, 0, 0, mvx, mvy, mvz);
      // Send item slot metadata (index 8 = item, type 7 = slot)
      startPacket(player_data[j].client_fd, 0x5C);
      writeVarInt(player_data[j].client_fd, entity_id);
      writeByte(player_data[j].client_fd, 8);
      writeVarInt(player_data[j].client_fd, 7);
      writeItemSlot(player_data[j].client_fd, count, item);
      writeByte(player_data[j].client_fd, 0xFF);
      endPacket(player_data[j].client_fd);
    }

    // Update next-slot hint to resume near where we found a free slot
    next_item_entity_slot = (idx + 1) % MAX_ITEM_ENTITIES;
    break;
  }
}

// Tick item entities - check for pickups and despawn
void tickItemEntities (void) {
  for (int i = 0; i < MAX_ITEM_ENTITIES; i++) {
    if (!item_entity_data[i].active) continue;

    int entity_id = ITEM_ENTITY_ID_BASE - i;
    item_entity_data[i].age++;

    // Simulate physics: gravity and drag. Grounded items can lose support
    // when leaves/blocks below them are broken, so let them fall again.
    if (item_entity_data[i].on_ground) {
      int bx = (int)floor(item_entity_data[i].x);
      int by = (int)floor(item_entity_data[i].y);
      int bz = (int)floor(item_entity_data[i].z);
      uint16_t block_below = getBlockAt2(bx, by - 1, bz, item_entity_data[i].dimension);
      if (isPassableBlock(block_below) && item_entity_data[i].y > -64) {
        item_entity_data[i].on_ground = 0;
      }
    }

    if (!item_entity_data[i].on_ground) {
      item_entity_data[i].vy -= 0.04;
      item_entity_data[i].vx *= 0.98;
      item_entity_data[i].vy *= 0.98;
      item_entity_data[i].vz *= 0.98;
      item_entity_data[i].x += item_entity_data[i].vx;
      item_entity_data[i].y += item_entity_data[i].vy;
      item_entity_data[i].z += item_entity_data[i].vz;
      // Check if item hit the ground
      if (item_entity_data[i].vy < 0) {
        int bx = (int)floor(item_entity_data[i].x);
        int by = (int)floor(item_entity_data[i].y);
        int bz = (int)floor(item_entity_data[i].z);
        uint16_t block_below = getBlockAt2(bx, by - 1, bz, item_entity_data[i].dimension);
        if (!isPassableBlock(block_below) || item_entity_data[i].y <= -64) {
          item_entity_data[i].y = by;
          item_entity_data[i].vx = 0;
          item_entity_data[i].vy = 0;
          item_entity_data[i].vz = 0;
          item_entity_data[i].on_ground = 1;
        }
      }
    }

    // Teleport while flying — grounded items don't move
    // Throttle: only broadcast every 3 ticks when near the ground or moving slowly.
    if (!item_entity_data[i].on_ground) {
      uint8_t should_broadcast = 1;
      double speed_sq = item_entity_data[i].vx * item_entity_data[i].vx +
                        item_entity_data[i].vy * item_entity_data[i].vy +
                        item_entity_data[i].vz * item_entity_data[i].vz;
      if (speed_sq < 0.0001) {
        // Almost stopped — throttle to every 3 ticks
        should_broadcast = (item_entity_data[i].age % 3 == 0);
      } else if (speed_sq < 0.01 && item_entity_data[i].age > 60) {
        // Very slow and been around a while — throttle to every 2 ticks
        should_broadcast = (item_entity_data[i].age % 2 == 0);
      }

      if (should_broadcast) {
        for (int j = 0; j < MAX_PLAYERS; j++) {
          if (player_data[j].client_fd == -1) continue;
          if (player_data[j].flags & 0x20) continue;
          if (player_data[j].dimension != item_entity_data[i].dimension) continue;
          sc_teleportEntity(player_data[j].client_fd, entity_id,
            item_entity_data[i].x, item_entity_data[i].y, item_entity_data[i].z, 0, 0);
        }
      }
    }

    // Despawn after 5 minutes (6000 ticks)
    if (item_entity_data[i].age > 6000) {
      item_entity_data[i].active = 0;
      for (int j = 0; j < MAX_PLAYERS; j++) {
        if (player_data[j].client_fd == -1) continue;
        if (player_data[j].dimension != item_entity_data[i].dimension) continue;
        sc_removeEntity(player_data[j].client_fd, entity_id);
      }
      continue;
    }

    // Allow pickup immediately (use same logic as XP orbs)
    if (item_entity_data[i].age < 10) continue;

    // Check for nearby players to pick up
    for (int j = 0; j < MAX_PLAYERS; j++) {
      if (player_data[j].client_fd == -1) continue;
      if (player_data[j].flags & 0x20) continue;
      if (player_data[j].dimension != item_entity_data[i].dimension) continue;

      double dx = item_entity_data[i].x - player_data[j].x;
      double dy = item_entity_data[i].y - player_data[j].y;
      double dz = item_entity_data[i].z - player_data[j].z;
      if (dx * dx + dy * dy + dz * dz <= 2.5) { // ~1.6 block radius, matches vanilla pickup feel
        if (givePlayerItem(&player_data[j], item_entity_data[i].item, item_entity_data[i].count) == 0) {
          for (int k = 0; k < MAX_PLAYERS; k++) {
            if (player_data[k].client_fd == -1) continue;
            if (player_data[k].dimension != item_entity_data[i].dimension) continue;
            sc_pickupItem(player_data[k].client_fd, entity_id, player_data[j].client_fd, item_entity_data[i].count);
            sc_removeEntity(player_data[k].client_fd, entity_id);
          }
          item_entity_data[i].active = 0;
        }
        break;
      }
    }
  }
}

// Count how many mobs are near a player (within configured spawn range)
static uint8_t countMobsNearPlayer (PlayerData *player) {
  uint8_t count = 0;
  int range = config.mob_spawn_range;
  for (int i = 0; i < MAX_MOBS; i ++) {
    if (mob_data[i].type == 0) continue;
    if ((mob_data[i].data & 31) == 0) continue; // Skip dead mobs
    if (mob_data[i].dimension != player->dimension) continue;
    double dx = fabs(mob_data[i].x - player->x);
    double dz = fabs(mob_data[i].z - player->z);
    uint16_t dist = (uint16_t)(dx + dz);
    if (dist <= (unsigned int)range) count ++;
  }
  return count;
}

static int mobBlockCoord (double v) {
  return (int)floor(v);
}

static uint8_t canMobOccupyPosition (double x, double y, double z, uint8_t dimension) {
  const double radius = 0.3;
  int min_x = mobBlockCoord(x - radius);
  int max_x = mobBlockCoord(x + radius);
  int min_z = mobBlockCoord(z - radius);
  int max_z = mobBlockCoord(z + radius);
  int min_y = mobBlockCoord(y);
  int max_y = mobBlockCoord(y + 1.8);

  for (int bx = min_x; bx <= max_x; bx ++) {
    for (int bz = min_z; bz <= max_z; bz ++) {
      for (int by = min_y; by <= max_y; by ++) {
        if (!isPassableBlock(getBlockAt2(bx, by, bz, dimension))) return false;
      }
    }
  }

  return true;
}

static uint8_t hasMobFloorBelow (double x, double y, double z, uint8_t dimension) {
  const double radius = 0.3;
  int min_x = mobBlockCoord(x - radius);
  int max_x = mobBlockCoord(x + radius);
  int min_z = mobBlockCoord(z - radius);
  int max_z = mobBlockCoord(z + radius);
  int below_y = mobBlockCoord(y - 0.05);

  for (int bx = min_x; bx <= max_x; bx ++) {
    for (int bz = min_z; bz <= max_z; bz ++) {
      if (!isPassableBlock(getBlockAt2(bx, below_y, bz, dimension))) return true;
    }
  }

  return false;
}

static uint8_t canMobStepTo (double x, double y, double z, uint8_t dimension) {
  return canMobOccupyPosition(x, y, z, dimension) && hasMobFloorBelow(x, y, z, dimension);
}

static uint8_t countMobsNearPositionOfType(double x, double y, double z, uint8_t dimension, double radius, uint8_t type) {
  uint8_t count = 0;
  double radius_sq = radius * radius;

  for (int i = 0; i < MAX_MOBS; i++) {
    if (mob_data[i].type == 0) continue;
    if (type != 0 && mob_data[i].type != type) continue;
    if ((mob_data[i].data & 31) == 0) continue;
    if (mob_data[i].dimension != dimension) continue;

    double dx = mob_data[i].x - x;
    double dy = mob_data[i].y - y;
    double dz = mob_data[i].z - z;
    if (dx * dx + dy * dy + dz * dz <= radius_sq) count++;
  }

  return count;
}

static uint8_t getNetherSpawnSurfaceY (short x, short z) {
  for (int y = 120; y >= 5; y --) {
    uint16_t surface_block = getBlockAt2(x, y, z, DIMENSION_NETHER);
    if (surface_block == B_air || surface_block == B_lava || surface_block == B_fire) continue;
    if (!isPassableSpawnBlock(getBlockAt2(x, y + 1, z, DIMENSION_NETHER))) continue;
    if (!isPassableSpawnBlock(getBlockAt2(x, y + 2, z, DIMENSION_NETHER))) continue;
    return (uint8_t)y;
  }
  return 0;
}

// Find a suitable Y position for fish spawning (in water, with at least 1 water block above)
static uint8_t getWaterSpawnY(short x, short z, uint8_t dimension) {
  for (int y = TERRAIN_BASE_HEIGHT + 10; y >= 30; y--) {
    uint16_t block = getBlockAt2(x, y, z, dimension);

    // We want the fish to spawn in water with headroom above
    if (isWaterBlock(block)) {
      uint16_t block_above = getBlockAt2(x, y + 1, z, dimension);
      uint16_t block_above2 = getBlockAt2(x, y + 2, z, dimension);
      // The block above must be passable (water or air) for swim room
      if ((isWaterBlock(block_above) || block_above == B_air) &&
          (isWaterBlock(block_above2) || block_above2 == B_air)) {
        return (uint8_t)y;
      }
    }
  }
  // DEBUG: check a few specific y-levels to see what blocks exist
  if (verbose_mode) {
    static short _dbg_x = -9999, _dbg_z = -9999;
    if (_dbg_x != x || _dbg_z != z) {
      _dbg_x = x; _dbg_z = z;
      uint16_t b62 = getBlockAt2(x, 62, z, dimension);
      uint16_t b63 = getBlockAt2(x, 63, z, dimension);
      uint16_t b64 = getBlockAt2(x, 64, z, dimension);
      terminal_ui_log("[DEBUG] getWaterSpawnY(%d,%d): y=62 block=%d y=63 block=%d y=64 block=%d", x, z, b62, b63, b64);
    }
  }
  return 0;
}

static void getRandomSpawnOffset(int min_dist, int spawn_range, int16_t *out_dx, int16_t *out_dz) {

  int dist_range = spawn_range - min_dist;
  if (dist_range <= 0) dist_range = 1;

  int dist = min_dist + (fast_rand() % dist_range);
  if (dist < 1) dist = 1;

  // Pick one of eight directions, with a small perpendicular spread.
  static const int8_t dir_x[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
  static const int8_t dir_z[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };
  uint8_t dir = fast_rand() & 7;
  int8_t side_offset = (int8_t)(fast_rand() % 11) - 5;
  int8_t side_x = -dir_z[dir];
  int8_t side_z = dir_x[dir];

  *out_dx = dir_x[dir] * dist + side_x * side_offset;
  *out_dz = dir_z[dir] * dist + side_z * side_offset;

}

static void spawnDungeonMobs(PlayerData *player) {
  if (player->dimension != DIMENSION_OVERWORLD) return;
  if (config.difficulty == 0) return; // No hostile spawns on peaceful

  uint32_t interval = (uint32_t)(4.0f * TICKS_PER_SECOND);
  if (interval == 0) interval = 1;
  if (server_ticks % interval != 0) return;

  DungeonSpawnerInfo spawners[4];
  uint8_t spawner_count = getNearbyDungeonSpawners(player->x, player->y, player->z, 16, spawners, 4);
  for (uint8_t i = 0; i < spawner_count; i++) {
    DungeonSpawnerInfo *spawner = &spawners[i];

    // If the spawner block was mined, stop spawning from this generated room.
    if (getBlockAt2(spawner->x, spawner->y, spawner->z, DIMENSION_OVERWORLD) != B_spawner) continue;
    if (countMobsNearPositionOfType((double)spawner->x + 0.5, spawner->y, (double)spawner->z + 0.5, DIMENSION_OVERWORLD, 9.0, spawner->mob_type) >= 4) continue;

    uint8_t spawned = 0;
    for (uint8_t attempt = 0; attempt < 16 && !spawned; attempt++) {
      int dx = (int)(fast_rand() % 9) - 4;
      int dz = (int)(fast_rand() % 9) - 4;
      if (dx == 0 && dz == 0) continue;

      short spawn_x = spawner->x + dx;
      short spawn_z = spawner->z + dz;
      uint8_t spawn_y = spawner->y;
      if (!canMobStepTo((double)spawn_x + 0.5, spawn_y, (double)spawn_z + 0.5, DIMENSION_OVERWORLD)) continue;

      uint8_t mob_health = 20;  // Default: 20 HP (zombies/skeletons in dungeons)
      if (spawner->mob_type == E_SPIDER) mob_health = 16;
      spawnMob(spawner->mob_type, spawn_x, spawn_y, spawn_z, mob_health, DIMENSION_OVERWORLD, 0);
    }

    if (spawned) continue;

    // Fallback: scan the room interior so unlucky random choices or wall/chest
    // positions don't make a valid spawner appear to do nothing.
    for (int dz = -3; dz <= 3 && !spawned; dz++) {
      for (int dx = -3; dx <= 3 && !spawned; dx++) {
        if (dx == 0 && dz == 0) continue;

        short spawn_x = spawner->x + dx;
        short spawn_z = spawner->z + dz;
        uint8_t spawn_y = spawner->y;
        if (!canMobStepTo((double)spawn_x + 0.5, spawn_y, (double)spawn_z + 0.5, DIMENSION_OVERWORLD)) continue;

        uint8_t mob_health = 20;  // Default: 20 HP (zombies/skeletons in dungeons)
        if (spawner->mob_type == E_SPIDER) mob_health = 16;
        spawnMob(spawner->mob_type, spawn_x, spawn_y, spawn_z, mob_health, DIMENSION_OVERWORLD, 0);
        spawned = 1;
        }
      }
    }
}

static void spawnVillageVillagers(PlayerData *player) {

  if (player->dimension != DIMENSION_OVERWORLD) return;

  short house_x[72], house_z[72];
  uint8_t house_count = 0;

  // Villages are keyed by 48x48 cells, while houses can sit near a cell edge.
  // Scan adjacent cells so standing in a visible village still finds its houses.
  for (int dz = -1; dz <= 1; dz ++) {
    for (int dx = -1; dx <= 1; dx ++) {
      short tmp_x[8], tmp_z[8];
      uint8_t found = getVillageHousePositions(player->x + dx * 48, player->z + dz * 48, tmp_x, tmp_z, 8);
      for (uint8_t i = 0; i < found && house_count < 72; i ++) {
        double px = (double)player->x + 0.5;
        double pz = (double)player->z + 0.5;
        double hx = (double)tmp_x[i] + 0.5;
        double hz = (double)tmp_z[i] + 0.5;
        double dist_x = hx - px;
        double dist_z = hz - pz;
        if (dist_x * dist_x + dist_z * dist_z > 80.0 * 80.0) continue;
        house_x[house_count] = tmp_x[i];
        house_z[house_count] = tmp_z[i];
        house_count++;
      }
    }
  }

  if (house_count == 0) return;

  uint8_t existing = 0;
  for (int i = 0; i < MAX_MOBS; i ++) {
    if (mob_data[i].type != E_VILLAGER) continue;
    if ((mob_data[i].data & 31) == 0) continue;
    if (mob_data[i].dimension != DIMENSION_OVERWORLD) continue;

    for (uint8_t h = 0; h < house_count; h ++) {
      double dx = mob_data[i].x - ((double)house_x[h] + 0.5);
      double dz = mob_data[i].z - ((double)house_z[h] + 0.5);
      if (dx * dx + dz * dz <= 24.0 * 24.0) {
        existing++;
        break;
      }
    }
  }

  uint8_t target = house_count > 12 ? 12 : house_count;
  if (existing >= target) return;

  uint8_t start = fast_rand() % house_count;
  for (uint8_t n = 0; n < house_count && existing < target; n ++) {
    uint8_t h = (start + n) % house_count;
    uint8_t occupied = false;

    for (int i = 0; i < MAX_MOBS; i ++) {
      if (mob_data[i].type != E_VILLAGER) continue;
      if ((mob_data[i].data & 31) == 0) continue;
      if (mob_data[i].dimension != DIMENSION_OVERWORLD) continue;
      double dx = mob_data[i].x - ((double)house_x[h] + 0.5);
      double dz = mob_data[i].z - ((double)house_z[h] + 0.5);
      double dy = fabs(mob_data[i].y - (double)(getHeightAt(house_x[h], house_z[h]) + 1));
      if (dx * dx + dz * dz < 9.0 && dy < 4.0) {
        occupied = true;
        break;
      }
    }
    if (occupied) continue;

    uint8_t y = getHeightAt(house_x[h], house_z[h]) + 1;
    if (!isPassableSpawnBlock(getBlockAt2(house_x[h], y, house_z[h], DIMENSION_OVERWORLD))) continue;
    if (!isPassableSpawnBlock(getBlockAt2(house_x[h], y + 1, house_z[h], DIMENSION_OVERWORLD))) continue;

    uint8_t profession = getVillageProfession(house_x[h], house_z[h]);
    spawnMob(E_VILLAGER, house_x[h], y, house_z[h], 20, DIMENSION_OVERWORLD, profession);
    existing++;
  }

}

// Spawn mobs around the player. Overworld gets passive mobs and nighttime zombies;
// Nether gets zombified piglins at any time of day.
static void spawnFishInWater(PlayerData *player) {
  if (player->dimension != DIMENSION_OVERWORLD) return;

  int max_mobs = config.mob_spawn_max_per_player;
  int spawn_range = config.mob_spawn_range;
  int min_dist = config.mob_spawn_min_distance;

  // Count existing fish near this player
  uint8_t existing_fish = 0;
  for (int i = 0; i < MAX_MOBS; i++) {
    if (mob_data[i].type == 0) continue;
    if ((mob_data[i].data & 31) == 0) continue; // Skip dead mobs
    if (mob_data[i].dimension != player->dimension) continue;
    // Check if this is a fish
    if (mob_data[i].type != E_COD && mob_data[i].type != E_SALMON &&
        mob_data[i].type != E_PUFFERFISH && mob_data[i].type != E_TROPICAL_FISH) continue;
    double dx = fabs(mob_data[i].x - player->x);
    double dz = fabs(mob_data[i].z - player->z);
    uint16_t dist = (uint16_t)(dx + dz);
    if (dist <= (unsigned int)spawn_range) existing_fish++;
  }

  // Limit fish count (use up to half of mob slots for fish)
  uint8_t max_fish = max_mobs / 2;
  if (verbose_mode)
    terminal_ui_log("[DEBUG] spawnFishInWater: existing_fish=%d max_fish=%d spawn_range=%d min_dist=%d", existing_fish, max_fish, spawn_range, min_dist);
  if (existing_fish >= max_fish) {
    if (verbose_mode) terminal_ui_log("[DEBUG]  -> Fish cap reached");
    return;
  }

  // Fish types
  static const uint8_t fish_types[] = { E_COD, E_SALMON, E_TROPICAL_FISH };
  static const uint8_t num_fish_types = 3;

  uint8_t fish_to_spawn = 1 + (fast_rand() % 2); // Spawn 1-2 fish
  if (fish_to_spawn > max_fish - existing_fish)
    fish_to_spawn = max_fish - existing_fish;

  // Since the world is fully terrestrial (oceans/rivers map to plains),
  // water only appears in small noise-driven depressions. Random offsets
  // often miss these, so we try multiple positions and do a small spiral
  // search around each one to find nearby water patches.
  #define MAX_WATER_SPOTS 16
  short water_spots_x[MAX_WATER_SPOTS];
  short water_spots_z[MAX_WATER_SPOTS];
  uint8_t water_spots_y[MAX_WATER_SPOTS];
  uint8_t water_spot_count = 0;

  // Try up to 10 random offsets and search a 7x7 diamond area around each
  if (verbose_mode) terminal_ui_log("[DEBUG]  -> Scanning for water positions...");
  for (int attempt = 0; attempt < 10 && water_spot_count < MAX_WATER_SPOTS; attempt++) {
    int16_t offset_x, offset_z;
    getRandomSpawnOffset(min_dist, spawn_range, &offset_x, &offset_z);
    short base_x = (short)(player->x + offset_x);
    short base_z = (short)(player->z + offset_z);

    // Spiral search around the base position (up to 3 blocks out)
    for (int r = 0; r <= 3 && water_spot_count < MAX_WATER_SPOTS; r++) {
      for (int dx = -r; dx <= r && water_spot_count < MAX_WATER_SPOTS; dx++) {
        for (int dz_sign = -1; dz_sign <= 1 && water_spot_count < MAX_WATER_SPOTS; dz_sign += 2) {
          int adz = r - (dx < 0 ? -dx : dx);  // Manhattan ring offset
          if (adz == 0 && dz_sign == -1) continue; // Skip duplicate center
          int dz = adz * dz_sign;

          short wx = (short)(base_x + dx);
          short wz = (short)(base_z + dz);

          uint8_t sy = getWaterSpawnY(wx, wz, player->dimension);
          if (sy != 0) {
            water_spots_x[water_spot_count] = wx;
            water_spots_z[water_spot_count] = wz;
            water_spots_y[water_spot_count] = sy;
            water_spot_count++;
          }
        }
      }
    }
  }

  if (verbose_mode) terminal_ui_log("[DEBUG]  -> Found %d water spots", water_spot_count);
  if (water_spot_count == 0) return; // No water found anywhere near this player

  for (uint8_t s = 0; s < fish_to_spawn; s++) {
    // Pick a random water spot
    uint8_t idx = fast_rand() % water_spot_count;
    short spawn_x = water_spots_x[idx];
    short spawn_z = water_spots_z[idx];
    uint8_t spawn_y = water_spots_y[idx];

    uint8_t type = fish_types[fast_rand() % num_fish_types];
    if (verbose_mode)
      terminal_ui_log("[DEBUG]  -> Spawning fish type=%d at (%d, %d, %d)", type, spawn_x, spawn_y, spawn_z);
    // Fish spawn with 3 HP (they're small)
    // spawn_y is already a water block, so spawn the fish directly in it
    spawnMob(type, spawn_x, spawn_y, spawn_z, 3, player->dimension, 0);

    // Find the fish we just spawned and adjust its Y to float in water
    uint8_t found_adjust = 0;
    for (int j = 0; j < MAX_MOBS; j++) {
      if (mob_data[j].type == type &&
          mob_data[j].x == (double)spawn_x + 0.5 &&
          mob_data[j].z == (double)spawn_z + 0.5 &&
          mob_data[j].y == (double)(spawn_y)) {
        mob_data[j].y = (double)(spawn_y) + 0.5; // Float at center of water block
        if (verbose_mode)
          terminal_ui_log("[DEBUG]  -> Adjusted fish[%d] y from %d to %.1f", j, spawn_y, (double)(spawn_y) + 0.5);
        found_adjust = 1;
        break;
      }
    }
    if (!found_adjust) {
      if (verbose_mode) {
        terminal_ui_log("[DEBUG]  -> WARNING: Could not find spawned fish to adjust Y!");
        // Check what mobs exist at this position
        for (int j = 0; j < MAX_MOBS; j++) {
          if (mob_data[j].type == type &&
              mob_data[j].x == (double)spawn_x + 0.5 &&
              mob_data[j].z == (double)spawn_z + 0.5) {
            terminal_ui_log("[DEBUG]     -> Found fish[%d] at y=%.1f (expected %d)", j, mob_data[j].y, spawn_y);
          }
        }
      }
    }
  }
  #undef MAX_WATER_SPOTS
}

static uint8_t getEndSpawnSurfaceY (short x, short z) {
  for (int y = 255; y >= 5; y --) {
    uint16_t surface_block = getBlockAt2(x, y, z, DIMENSION_END);
    if (surface_block == B_air) continue;
    if (!isPassableSpawnBlock(getBlockAt2(x, y + 1, z, DIMENSION_END))) continue;
    if (!isPassableSpawnBlock(getBlockAt2(x, y + 2, z, DIMENSION_END))) continue;
    return (uint8_t)y;
  }
  return 0;
}

static void spawnMobsAroundPlayer (PlayerData *player) {
  int max_mobs = config.mob_spawn_max_per_player;
  int spawn_range = config.mob_spawn_range;
  int min_dist = config.mob_spawn_min_distance;

  // Dungeon spawners are block-driven and should keep working even when the
  // normal ambient mob cap/range has already been reached.
  spawnDungeonMobs(player);

  // Fish have their own independent cap, so they're not blocked by the
  // general mob cap. This is important because the world may have many
  // pre-existing mobs loaded from disk.
  if (player->dimension == DIMENSION_OVERWORLD) {
    spawnFishInWater(player);
  }

  // Count existing mobs near this player
  uint8_t existing = countMobsNearPlayer(player);
  if (verbose_mode)
    terminal_ui_log("[DEBUG] spawnMobsAroundPlayer: dim=%d existing=%d max_mobs=%d", player->dimension, existing, max_mobs);
  if ((int)existing >= max_mobs) {
    if (verbose_mode) terminal_ui_log("[DEBUG]  -> Mob cap full, skipping");
    return;
  }

  uint8_t slots_left = (uint8_t)(max_mobs - existing);

  // Endermen spawn in the End
  if (player->dimension == DIMENSION_END) {
    if (verbose_mode) terminal_ui_log("[DEBUG]  -> In End dimension");
    uint8_t endermen_to_spawn = (fast_rand() % slots_left) + 1;
    if (endermen_to_spawn > slots_left / 2) endermen_to_spawn = slots_left / 2;
    if (endermen_to_spawn < 1) endermen_to_spawn = 1;

    for (uint8_t s = 0; s < endermen_to_spawn; s ++) {
      int16_t offset_x, offset_z;
      getRandomSpawnOffset(min_dist, spawn_range, &offset_x, &offset_z);
      short spawn_x = player->x + offset_x;
      short spawn_z = player->z + offset_z;

      uint8_t surface_y = getEndSpawnSurfaceY(spawn_x, spawn_z);
      if (surface_y == 0) continue;

      // Enderman (147) spawns in the End — vanilla health is 40
      spawnMob(E_ENDERMAN, spawn_x, surface_y + 1, spawn_z, 40, player->dimension, 0);
    }

    return;
  }

  spawnVillageVillagers(player);

  // Passive mob types: Chicken(25), Cow(28), Pig(95), Sheep(106)
  static const uint8_t passive_types[] = { 25, 28, 95, 106 };
  static const uint8_t num_passive_types = 4;

  // Hostile mob types: Zombie(145), Skeleton(110), Spider(119), Creeper(30) - spawn only at night.
  // Enderman(39) is weighted rare (~1/11 chance per hostile spawn).
  static const uint8_t hostile_types[] = { 145, 145, 145, 145, E_SKELETON, E_SKELETON, E_SKELETON, E_SPIDER, E_SPIDER, E_CREEPER, E_ENDERMAN };
  static const uint8_t num_hostile_types = 11;

  // Determine if it's night (world_time >= 12000)
  uint8_t is_night = (world_time >= 12000);



  if (player->dimension == DIMENSION_NETHER) {
    if (verbose_mode) terminal_ui_log("[DEBUG]  -> In Nether dimension");
    // Spawn piglins
    uint8_t piglins_to_spawn = (fast_rand() % slots_left) + 1;
    if (piglins_to_spawn > slots_left / 2) piglins_to_spawn = slots_left / 2;
    if (piglins_to_spawn < 1) piglins_to_spawn = 1;

    for (uint8_t s = 0; s < piglins_to_spawn; s ++) {
      int16_t offset_x, offset_z;
      getRandomSpawnOffset(min_dist, spawn_range, &offset_x, &offset_z);
      short spawn_x = player->x + offset_x;
      short spawn_z = player->z + offset_z;

      uint8_t surface_y = getNetherSpawnSurfaceY(spawn_x, spawn_z);
      if (surface_y == 0) continue;

      // Regular Piglin (96) spawns in the Nether
      spawnMob(E_PIGLIN, spawn_x, surface_y + 1, spawn_z, 16, player->dimension, 0);
    }

    // Also spawn zombified piglins
    uint8_t zombie_piglins_to_spawn = (fast_rand() % slots_left) + 1;
    if (zombie_piglins_to_spawn > slots_left / 4) zombie_piglins_to_spawn = slots_left / 4;
    if (zombie_piglins_to_spawn < 1) zombie_piglins_to_spawn = 1;

    for (uint8_t s = 0; s < zombie_piglins_to_spawn; s ++) {
      int16_t offset_x, offset_z;
      getRandomSpawnOffset(min_dist, spawn_range, &offset_x, &offset_z);
      short spawn_x = player->x + offset_x;
      short spawn_z = player->z + offset_z;

      uint8_t surface_y = getNetherSpawnSurfaceY(spawn_x, spawn_z);
      if (surface_y == 0) continue;

      // Zombified Piglin(148) spawns in the Nether regardless of time of day — vanilla health is 20
      spawnMob(148, spawn_x, surface_y + 1, spawn_z, 20, player->dimension, 0);
    }

    return;
  }

  // Try to spawn mobs in random positions behind the player
  uint8_t to_spawn = (fast_rand() % slots_left) + 1; // Spawn 1 to slots_left
  for (uint8_t s = 0; s < to_spawn; s ++) {

    int16_t offset_x, offset_z;
    getRandomSpawnOffset(min_dist, spawn_range, &offset_x, &offset_z);
    short spawn_x = player->x + offset_x;
    short spawn_z = player->z + offset_z;


    // Get the surface height at this position
    uint8_t surface_y = getHeightAt(spawn_x, spawn_z);
    if (surface_y == 0) continue; // Invalid height

    // Check that the block at surface is grass or snow
      uint16_t surface_block = getBlockAt(spawn_x, surface_y, spawn_z);
    if (surface_block != B_grass_block &&
        surface_block != B_snowy_grass_block &&
        surface_block != B_snow_block) continue;

    // Check that the two blocks above are passable (enough headroom)
    if (!isPassableSpawnBlock(getBlockAt(spawn_x, surface_y + 1, spawn_z))) continue;
    if (!isPassableSpawnBlock(getBlockAt(spawn_x, surface_y + 2, spawn_z))) continue;

    // Pick a random passive mob type
    uint8_t type = passive_types[fast_rand() % num_passive_types];

    // Vanilla health values for passive mobs
    uint8_t health = 10;
    if (type == 25) health = 4;   // Chicken: vanilla 4 HP
    else if (type == 106) health = 8;  // Sheep: vanilla 8 HP
    spawnMob(type, spawn_x, surface_y + 1, spawn_z, health, player->dimension, 0);
  }

  // Spawn hostile mobs (zombies) only at night, and never on peaceful
  if (is_night && num_hostile_types > 0 && config.difficulty != 0) {
    uint8_t hostile_to_spawn = (fast_rand() % slots_left) + 1;
    if (hostile_to_spawn > slots_left / 2) hostile_to_spawn = slots_left / 2;
    if (hostile_to_spawn < 1) hostile_to_spawn = 1;

    int hostile_range = 2; // Zombies spawn within 2 blocks

    for (uint8_t s = 0; s < hostile_to_spawn; s ++) {
      int16_t offset_x, offset_z;
      getRandomSpawnOffset(min_dist, hostile_range, &offset_x, &offset_z);
      short spawn_x = player->x + offset_x;
      short spawn_z = player->z + offset_z;


      uint8_t surface_y = getHeightAt(spawn_x, spawn_z);
      if (surface_y == 0) continue;

    uint16_t surface_block = getBlockAt(spawn_x, surface_y, spawn_z);
      if (surface_block != B_grass_block &&
          surface_block != B_snowy_grass_block &&
          surface_block != B_snow_block) continue;

      if (!isPassableSpawnBlock(getBlockAt(spawn_x, surface_y + 1, spawn_z))) continue;
      if (!isPassableSpawnBlock(getBlockAt(spawn_x, surface_y + 2, spawn_z))) continue;

      uint8_t type = hostile_types[fast_rand() % num_hostile_types];
      uint8_t health = 20;  // Zombies(145) and Skeletons(E_SKELETON): vanilla 20 HP
      if (type == E_ENDERMAN) health = 40;
      else if (type == E_SPIDER) health = 16;
      else if (type == E_CREEPER) health = 20;

      spawnMob(type, spawn_x, surface_y + 1, spawn_z, health, player->dimension, 0);
    }
  }

}

static int getMerchantMobIndexForPlayer (PlayerData *player) {
  if (!player || player->merchant_villager_eid == 0) return -1;

  int mob_idx = -player->merchant_villager_eid - 2;
  if (mob_idx < 0 || mob_idx >= MAX_MOBS) return -1;
  if (mob_data[mob_idx].type != E_VILLAGER) return -1;
  return mob_idx;
}

static uint8_t getMerchantProfessionForPlayer (PlayerData *player) {
  int mob_idx = getMerchantMobIndexForPlayer(player);
  if (mob_idx >= 0) return mob_data[mob_idx].profession;
  return 0;
}

static uint8_t getMerchantTradeInput (PlayerData *player, uint8_t trade_index, uint16_t *in_item, uint8_t *in_count) {
  static const uint8_t input_counts[][5] = {
    {1, 1, 1, 2, 3}, // Farmer
    {1, 2, 4, 5, 1}, // Librarian
    {1, 2, 3, 4, 5}, // Cleric
    {3, 4, 5, 7, 6}, // Armorer
    {1, 1, 1, 1, 2}, // Butcher
    {1, 2, 3, 4, 5}, // Cartographer
    {1, 1, 1, 2, 3}, // Fisherman
    {1, 1, 2, 3, 4}, // Fletcher
    {1, 2, 3, 3, 4}, // Leatherworker
    {1, 1, 2, 2, 1}, // Mason
    {1, 1, 2, 2, 1}, // Shepherd
    {2, 2, 3, 3, 4}, // Toolsmith
    {2, 3, 4, 4, 3}, // Weaponsmith
  };
  static const uint8_t default_counts[5] = {1, 1, 2, 3, 1};

  if (trade_index >= 5) return 1;

  int mob_idx = getMerchantMobIndexForPlayer(player);
  if (mob_idx < 0 || trade_index >= getVillagerTradeLevel(mob_idx)) return 1;

  uint8_t profession = getMerchantProfessionForPlayer(player);
  const uint8_t *counts = default_counts;
  if (profession < sizeof(input_counts) / sizeof(input_counts[0])) {
    counts = input_counts[profession];
  }

  *in_item = I_emerald;
  *in_count = counts[trade_index];
  return 0;
}

static void syncMerchantInventorySlots (PlayerData *player) {
  for (uint8_t i = 0; i < 41; i++) {
    uint8_t client_slot = serverSlotToClientSlot(19, i);
    if (client_slot != 255) {
      sc_setContainerSlot(player->client_fd, 19, client_slot,
        player->inventory_count[i], player->inventory_items[i]);
    }
  }
}

static uint8_t returnMerchantInputItems (PlayerData *player, uint8_t count) {
  if (count == 0 || player->craft_count[0] == 0 || player->craft_items[0] == 0) return 0;
  if (count > player->craft_count[0]) count = player->craft_count[0];

  uint16_t item = player->craft_items[0];
  if (givePlayerItem(player, item, count)) return 1;

  player->craft_count[0] -= count;
  if (player->craft_count[0] == 0) player->craft_items[0] = 0;
  return 0;
}

void selectMerchantTrade (PlayerData *player, uint8_t trade_index) {
  if (!player || !player->merchant_open) return;

  uint16_t in_item = 0;
  uint8_t in_count = 0;
  if (getMerchantTradeInput(player, trade_index, &in_item, &in_count)) return;

  player->selected_trade = trade_index;

  if (player->craft_count[0] > 0 && player->craft_items[0] != in_item) {
    if (returnMerchantInputItems(player, player->craft_count[0])) {
      sc_setContainerSlot(player->client_fd, 19, 0, player->craft_count[0], player->craft_items[0]);
      syncMerchantInventorySlots(player);
      return;
    }
  }

  if (player->craft_count[0] > in_count) {
    if (returnMerchantInputItems(player, player->craft_count[0] - in_count)) {
      sc_setContainerSlot(player->client_fd, 19, 0, player->craft_count[0], player->craft_items[0]);
      syncMerchantInventorySlots(player);
      return;
    }
  }

  uint8_t equipment_dirty = false;
  if (player->craft_count[0] < in_count) {
    uint8_t needed = in_count - player->craft_count[0];
    for (uint8_t i = 0; i < 36 && needed > 0; i++) {
      if (player->inventory_items[i] != in_item || player->inventory_count[i] == 0) continue;

      uint8_t take = needed < player->inventory_count[i] ? needed : player->inventory_count[i];
      player->inventory_count[i] -= take;
      needed -= take;
      if (player->inventory_count[i] == 0) player->inventory_items[i] = 0;

      player->craft_items[0] = in_item;
      player->craft_count[0] += take;
      if (i == player->hotbar) equipment_dirty = true;
    }
  }

  if (player->craft_count[0] == 0) player->craft_items[0] = 0;

  sc_setContainerSlot(player->client_fd, 19, 0, player->craft_count[0], player->craft_items[0]);
  sc_setContainerSlot(player->client_fd, 19, 1, 0, 0);
  syncMerchantInventorySlots(player);

  if (equipment_dirty) broadcastPlayerEquipment(player);
}

void executeMerchantTrade (PlayerData *player, uint8_t trade_index, uint8_t to_inventory) {
  // Trade entries: {input_item, input_count, output_item, output_count, max_uses}
  struct TradeEntry {
    uint16_t in_item, in_cnt;
    uint16_t out_item, out_cnt;
    uint8_t max_uses;
  };

  uint8_t profession = getMerchantProfessionForPlayer(player);

  // Same trade data as in sc_setTradeOffers (packets.c)
  static const struct TradeEntry farmer[] = {
    {I_emerald, 1, I_bread, 3, 16},
    {I_emerald, 1, I_apple, 2, 8},
    {I_emerald, 1, I_cookie, 4, 12},
    {I_emerald, 2, I_cake, 1, 4},
    {I_emerald, 3, I_golden_carrot, 1, 3},
  };
  static const struct TradeEntry librarian[] = {
    {I_emerald, 1, I_paper, 4, 12},
    {I_emerald, 2, I_book, 1, 8},
    {I_emerald, 4, I_bookshelf, 1, 4},
    {I_emerald, 5, I_golden_apple, 1, 2},
    {I_emerald, 1, I_glass, 4, 12},
  };
  static const struct TradeEntry cleric[] = {
    {I_emerald, 1, I_redstone, 2, 12},
    {I_emerald, 2, I_lapis_lazuli, 1, 8},
    {I_emerald, 3, I_glowstone, 1, 6},
    {I_emerald, 4, I_ender_pearl, 1, 3},
    {I_emerald, 5, I_experience_bottle, 1, 3},
  };
  static const struct TradeEntry armorer[] = {
    {I_emerald, 3, I_iron_boots, 1, 4},
    {I_emerald, 4, I_iron_leggings, 1, 4},
    {I_emerald, 5, I_iron_chestplate, 1, 4},
    {I_emerald, 7, I_diamond_boots, 1, 2},
    {I_emerald, 6, I_iron_helmet, 1, 4},
  };
  static const struct TradeEntry butcher[] = {
    {I_emerald, 1, I_cooked_beef, 3, 12},
    {I_emerald, 1, I_cooked_porkchop, 3, 12},
    {I_emerald, 1, I_cooked_mutton, 3, 12},
    {I_emerald, 1, I_leather, 2, 8},
    {I_emerald, 2, I_rabbit_stew, 1, 4},
  };
  static const struct TradeEntry cartographer[] = {
    {I_emerald, 1, I_paper, 6, 12},
    {I_emerald, 2, I_compass, 1, 6},
    {I_emerald, 3, I_map, 1, 4},
    {I_emerald, 4, I_filled_map, 1, 3},
    {I_emerald, 5, I_globe_banner_pattern, 1, 2},
  };
  static const struct TradeEntry fisherman[] = {
    {I_emerald, 1, I_cooked_cod, 3, 12},
    {I_emerald, 1, I_cooked_salmon, 3, 12},
    {I_emerald, 1, I_cod, 3, 12},
    {I_emerald, 2, I_salmon, 3, 8},
    {I_emerald, 3, I_fishing_rod, 1, 4},
  };
  static const struct TradeEntry fletcher[] = {
    {I_emerald, 1, I_arrow, 6, 12},
    {I_emerald, 1, I_flint, 4, 12},
    {I_emerald, 2, I_bow, 1, 6},
    {I_emerald, 3, I_crossbow, 1, 4},
    {I_emerald, 4, I_arrow, 12, 3},
  };
  static const struct TradeEntry leatherworker[] = {
    {I_emerald, 1, I_leather, 3, 12},
    {I_emerald, 2, I_leather_helmet, 1, 6},
    {I_emerald, 3, I_leather_chestplate, 1, 6},
    {I_emerald, 3, I_leather_leggings, 1, 6},
    {I_emerald, 4, I_saddle, 1, 3},
  };
  static const struct TradeEntry mason[] = {
    {I_emerald, 1, I_stone, 6, 12},
    {I_emerald, 1, I_stone_bricks, 4, 12},
    {I_emerald, 2, I_chiseled_stone_bricks, 1, 6},
    {I_emerald, 2, I_polished_andesite, 4, 6},
    {I_emerald, 1, I_bricks, 4, 12},
  };
  static const struct TradeEntry shepherd[] = {
    {I_emerald, 1, I_white_wool, 3, 12},
    {I_emerald, 1, I_white_carpet, 6, 12},
    {I_emerald, 2, I_shears, 1, 6},
    {I_emerald, 2, I_white_bed, 1, 4},
    {I_emerald, 1, I_string, 4, 12},
  };
  static const struct TradeEntry toolsmith[] = {
    {I_emerald, 2, I_stone_axe, 1, 6},
    {I_emerald, 2, I_stone_pickaxe, 1, 6},
    {I_emerald, 3, I_iron_axe, 1, 4},
    {I_emerald, 3, I_iron_pickaxe, 1, 4},
    {I_emerald, 4, I_diamond_hoe, 1, 2},
  };
  static const struct TradeEntry weaponsmith[] = {
    {I_emerald, 2, I_iron_sword, 1, 6},
    {I_emerald, 3, I_iron_axe, 1, 4},
    {I_emerald, 4, I_diamond_sword, 1, 2},
    {I_emerald, 4, I_diamond_axe, 1, 2},
    {I_emerald, 3, I_iron_ingot, 2, 6},
  };
  static const struct TradeEntry default_trades[] = {
    {I_emerald, 1, I_bread, 3, 16},
    {I_emerald, 1, I_oak_planks, 8, 12},
    {I_emerald, 2, I_iron_ingot, 1, 6},
    {I_emerald, 3, I_book, 1, 4},
    {I_emerald, 1, I_stick, 4, 12},
  };

  const struct TradeEntry *trades;
  int trade_count;
  switch (profession) {
    case 0:  trades = farmer;         trade_count = sizeof(farmer) / sizeof(farmer[0]); break;
    case 1:  trades = librarian;      trade_count = sizeof(librarian) / sizeof(librarian[0]); break;
    case 2:  trades = cleric;         trade_count = sizeof(cleric) / sizeof(cleric[0]); break;
    case 3:  trades = armorer;        trade_count = sizeof(armorer) / sizeof(armorer[0]); break;
    case 4:  trades = butcher;        trade_count = sizeof(butcher) / sizeof(butcher[0]); break;
    case 5:  trades = cartographer;   trade_count = sizeof(cartographer) / sizeof(cartographer[0]); break;
    case 6:  trades = fisherman;      trade_count = sizeof(fisherman) / sizeof(fisherman[0]); break;
    case 7:  trades = fletcher;       trade_count = sizeof(fletcher) / sizeof(fletcher[0]); break;
    case 8:  trades = leatherworker;  trade_count = sizeof(leatherworker) / sizeof(leatherworker[0]); break;
    case 9:  trades = mason;          trade_count = sizeof(mason) / sizeof(mason[0]); break;
    case 10: trades = shepherd;       trade_count = sizeof(shepherd) / sizeof(shepherd[0]); break;
    case 11: trades = toolsmith;      trade_count = sizeof(toolsmith) / sizeof(toolsmith[0]); break;
    case 12: trades = weaponsmith;    trade_count = sizeof(weaponsmith) / sizeof(weaponsmith[0]); break;
    default: trades = default_trades; trade_count = sizeof(default_trades) / sizeof(default_trades[0]); break;
  }

  if (trade_index >= trade_count) return;

  int mob_idx = getMerchantMobIndexForPlayer(player);
  if (mob_idx < 0) return;
  if (trade_index >= getVillagerTradeLevel(mob_idx)) {
    if (player->merchant_open) sc_setTradeOffers(player->client_fd, 19, profession, mob_idx);
    return;
  }
  if (trade_index < 5 && mob_trade_uses[mob_idx][trade_index] >= trades[trade_index].max_uses) {
    if (player->merchant_open) sc_setTradeOffers(player->client_fd, 19, profession, mob_idx);
    return;
  }

  const uint16_t in1 = trades[trade_index].in_item;
  const uint8_t  in1c = trades[trade_index].in_cnt;
  const uint16_t out = trades[trade_index].out_item;
  const uint8_t  outc = trades[trade_index].out_cnt;

  if (to_inventory && !canGivePlayerItem(player, out, outc)) return;

  // Check if the player has the required input items
  int total_found = 0;
  for (uint8_t i = 0; i < 41; i++) {
    if (player->inventory_items[i] == in1) {
      total_found += player->inventory_count[i];
      if (total_found >= in1c) break;
    }
  }
  // Also check craft_items[0] (merchant input slot via slot changes)
  if (total_found < in1c && player->craft_items[0] == in1) {
    total_found += player->craft_count[0];
  }
  if (total_found < in1c) return;

  // Consume from craft_items[0] FIRST (auto-populated items end up here),
  // then from inventory for any remainder.
  int remaining = in1c;
  if (player->craft_items[0] == in1 && player->craft_count[0] > 0) {
    uint8_t take = (remaining < player->craft_count[0]) ? (uint8_t)remaining : player->craft_count[0];
    player->craft_count[0] -= take;
    remaining -= take;
    if (player->craft_count[0] == 0) player->craft_items[0] = 0;
    sc_setContainerSlot(player->client_fd, 19, 0, player->craft_count[0], player->craft_items[0]);
  }
  // Then consume remaining from inventory
  for (uint8_t i = 0; i < 41 && remaining > 0; i++) {
    if (player->inventory_items[i] == in1) {
      uint8_t take = (remaining < player->inventory_count[i]) ? (uint8_t)remaining : player->inventory_count[i];
      player->inventory_count[i] -= take;
      remaining -= take;
      if (player->inventory_count[i] == 0) player->inventory_items[i] = 0;
      syncPlayerInventorySlot(player, i);
    }
  }

  // Increment trade usage counter
  if (trade_index < 5 && mob_trade_uses[mob_idx][trade_index] < UINT8_MAX) {
    mob_trade_uses[mob_idx][trade_index]++;
  }

  if (to_inventory) {
    givePlayerItem(player, out, outc);
    player->flagval_16 = 0;
    player->flagval_8 = 0;
    sc_setCursorItem(player->client_fd, 0, 0);
  } else {
    // Give the output item — the client already carries it as cursor,
    // so only set the cursor item rather than adding to inventory again.
    player->flagval_16 = out;
    player->flagval_8 = outc;
    sc_setCursorItem(player->client_fd, out, outc);
  }

  // Update merchant slots and player inventory in the GUI
  if (player->merchant_open) {
    // Clear merchant input and output slots
    sc_setContainerSlot(player->client_fd, 19, 0, 0, 0);  // input 1
    sc_setContainerSlot(player->client_fd, 19, 1, 0, 0);  // input 2
    sc_setContainerSlot(player->client_fd, 19, 2, 0, 0);  // output
    // Refresh offers so the client sees updated uses, sold-out stock, and level.
    sc_setTradeOffers(player->client_fd, 19, profession, mob_idx);
    broadcastMobMetadata(-1, player->merchant_villager_eid);

    // Sync all inventory slots so the client sees the consumed emeralds
    for (uint8_t i = 0; i < 41; i++) {
      uint8_t cs = serverSlotToClientSlot(19, i);
      if (cs != 255) {
        sc_setContainerSlot(player->client_fd, 19, cs,
          player->inventory_count[i], player->inventory_items[i]);
      }
    }
  }

  writePlayerDataToDisk();
}

void interactEntity (int entity_id, int interactor_id) {

  PlayerData *player;
  if (getPlayerData(interactor_id, &player)) return;

  int mob_index = -entity_id - 2;
  if (mob_index < 0 || mob_index >= MAX_MOBS) return;
  MobData *mob = &mob_data[mob_index];

  switch (mob->type) {
    case 106: // Sheep
      if (player->inventory_items[player->hotbar] != I_shears)
        return;

      if ((mob->data >> 5) & 1) // Check if sheep has already been sheared
        return;

      mob->data |= 1 << 5; // Set sheared to true

      uint8_t item_count = 1 + (fast_rand() % 3); // 1-3
      spawnItemEntity(mob->x, mob->y + 0.75, mob->z, I_white_wool, item_count, mob->dimension,
        ((int)(fast_rand() & 0xFF) - 128) / 4096.0,
        0.15,
        ((int)(fast_rand() & 0xFF) - 128) / 4096.0);
      if (getConfiguredGameMode() != 1) damageHeldItem(player, 1);

      for (int i = 0; i < MAX_PLAYERS; i ++) {
        PlayerData* player = &player_data[i];
        int client_fd = player->client_fd;

        if (client_fd == -1) continue;
        if (player->flags & 0x20) continue;

        sc_entityAnimation(client_fd, interactor_id, 0);
      }

      broadcastMobMetadata(-1, entity_id);
      broadcastMobSound(entity_id, S_SHEEP_SHEAR, SOUND_CATEGORY_NEUTRAL, 1.0f, 1.0f);

      break;

    case E_VILLAGER: // Villager
      // Open merchant trading GUI
      player->merchant_open = 1;
      player->merchant_villager_eid = entity_id;
      player->selected_trade = 0;

      sc_openScreen(player->client_fd, 19, "Villager", 8);
      sc_setTradeOffers(player->client_fd, 19, mob->profession, mob_index);

      // Send empty merchant slots
      sc_setContainerSlot(player->client_fd, 19, 0, 0, 0);  // input 1
      sc_setContainerSlot(player->client_fd, 19, 1, 0, 0);  // input 2
      sc_setContainerSlot(player->client_fd, 19, 2, 0, 0);  // output
      // Sync player inventory into the merchant window
      for (uint8_t i = 0; i < 41; i++) {
        uint8_t client_slot = serverSlotToClientSlot(19, i);
        if (client_slot != 255) {
          sc_setContainerSlot(player->client_fd, 19, client_slot,
            player->inventory_count[i], player->inventory_items[i]);
        }
      }

      broadcastMobSound(entity_id, S_VILLAGER_TRADE, SOUND_CATEGORY_NEUTRAL, 1.0f, 1.0f);
      break;

    case E_PIGLIN: // Piglin
      // Check if piglin is angry (uses anger_timer)
      if (mob->anger_timer > 0) {
        // Angry piglins refuse trades
        break;
      }

      // Piglins trade gold ingots for random items (same as villagers)
      if (player->inventory_items[player->hotbar] == I_gold_ingot) {
        // Take the gold ingot
        player->inventory_count[player->hotbar]--;
        if (player->inventory_count[player->hotbar] == 0) {
          player->inventory_items[player->hotbar] = 0;
        }
        syncHeldItem(player, player->inventory_count[player->hotbar], player->inventory_items[player->hotbar]);

        // Give a random creative item (same as villagers)
        uint16_t random_item = getRandomCreativeItem();
        givePlayerItem(player, random_item, 1);

        // Swing arm animation for all nearby players
        for (int i = 0; i < MAX_PLAYERS; i ++) {
          if (player_data[i].client_fd == -1) continue;
          if (player_data[i].flags & 0x20) continue;
          sc_entityAnimation(player_data[i].client_fd, interactor_id, 0);
        }

        broadcastMobSound(entity_id, S_PIGLIN_ADMIRE, SOUND_CATEGORY_NEUTRAL, 1.0f, 1.0f);
      }
      break;
  }
}

void hurtEntity (int entity_id, int attacker_id, uint8_t damage_type, uint8_t damage) {

  // Whether this attack was blocked by a shield
  uint8_t damage_blocked = false;

  if (attacker_id > 0 && damage_type != D_arrow) { // Attacker is a player

    PlayerData *player;
    if (getPlayerData(attacker_id, &player)) return;

    uint16_t held_item = player->inventory_items[player->hotbar];
    uint8_t base_damage = getWeaponBaseDamage(held_item);
    int64_t cooldown_us = getWeaponCooldownUs(held_item);

    // Calculate base progress (0.0 to 1.0) from real time since last attack
    int64_t now_us = get_program_time();
    int64_t time_since = now_us - player->last_attack_time;
    float base_progress = (float)time_since / (float)cooldown_us;
    if (base_progress > 1.0f) base_progress = 1.0f;
    if (base_progress < 0.0f) base_progress = 0.0f;

    // Apply vanilla Java's getProgress(0.5F) lerp curve:
    // result = base + (base - 0.5*base^2 - 0.5) * lerp, where lerp=0.5
    // This keeps damage very low early in the cooldown
    float charge = 1.5f * base_progress - 0.25f * base_progress * base_progress - 0.25f;
    if (charge > 1.0f) charge = 1.0f;
    if (charge < 0.0f) charge = 0.0f;

    // Vanilla damage formula: damage = baseDamage * (0.2 + charge^2 * 0.8)
    float damage_f = (float)base_damage * (0.2f + charge * charge * 0.8f);
    damage = (uint8_t)(damage_f + 0.5f);
    if (damage < 1) damage = 1;

    // ---- Critical Hit ----
    // A critical hit occurs when a player attacks while falling:
    // - Player is below their last grounded Y (falling at least 1 block)
    // - Player is not in water
    // - Player is not on a ladder or vine
    {
      uint16_t feet_block = getBlockAt2(player->x, player->y, player->z, player->dimension);
      if (
        player->y < player->grounded_y &&           // Falling
        !(feet_block >= B_water && feet_block < B_water + 8) &&  // Not in water
        feet_block != B_ladder                       // Not on ladder
      ) {
        // Vanilla critical hit: 1.5x damage (add half)
        damage = (uint8_t)((float)damage * 1.5f + 0.5f);
        if (damage < 1) damage = 1;
      }
    }

    // Update last attack time
    player->last_attack_time = now_us;

    // ---- Knockback ----
    uint8_t kb = getWeaponKnockback(held_item);
    if (kb > 0 && charge > 0.0f) {
      // Get target entity position
      double tx, ty, tz;
      if (entity_id > 0) { // Target is a player
        PlayerData *target;
        if (!getPlayerData(entity_id, &target)) {
          tx = target->x; ty = target->y; tz = target->z;
        } else { tx = 0; ty = 0; tz = 0; }
      } else { // Target is a mob
        int mob_idx = -entity_id - 2;
        if (mob_idx >= 0 && mob_idx < MAX_MOBS) {
          tx = mob_data[mob_idx].x;
          ty = mob_data[mob_idx].y;
          tz = mob_data[mob_idx].z;
        } else { tx = 0; ty = 0; tz = 0; }
      }

      // Direction from attacker to target (horizontal only)
      double dx = tx - player->x;
      double dz = tz - player->z;
      double dist = sqrt(dx * dx + dz * dz);

      if (dist > 0.001) {
        // Vanilla knockback: sprint adds 0.5 bonus, weapon adds kbStrength * charge * 0.5
        double kb_str = (double)kb / 100.0;
        double sprint_bonus = (player->flags & 0x08) ? 0.5 : 0.0;  // Sprinting bonus
        double kb_factor = kb_str * (double)charge * 0.5 + sprint_bonus;
        double vx = (dx / dist) * kb_factor;
        double vz = (dz / dist) * kb_factor;
        // Vertical: base 0.2 + weapon contribution + sprint bonus
        double vy = 0.2 + kb_str * (double)charge * 0.1 + sprint_bonus * 0.1;

        int16_t nvx = (int16_t)(vx * 8000.0);
        int16_t nvy = (int16_t)(vy * 8000.0);
        int16_t nvz = (int16_t)(vz * 8000.0);

        // Send velocity to all players tracking this entity
        for (int i = 0; i < MAX_PLAYERS; i++) {
          if (player_data[i].client_fd == -1) continue;
          if (player_data[i].flags & 0x20) continue;
          sc_setEntityVelocity(player_data[i].client_fd, entity_id, nvx, nvy, nvz);
        }
      }
    }

  }

  // Whether this attack caused the target entity to die
  uint8_t entity_died = false;

  if (entity_id > 0) { // The attacked entity is a player

    PlayerData *player;
    if (getPlayerData(entity_id, &player)) return;

    // Don't continue if the player is already dead
    if (player->health == 0) return;

    // Difficulty scaling for mob attacks (attacker_id < -1 means mob)
    if (attacker_id < -1) {
      uint8_t diff = getConfiguredDifficulty();
      if (diff == 0) {
        damage = 0; // Peaceful: no mob damage
      } else if (diff == 1) {
        // Easy: 0.5x, round down, min 1
        damage = damage / 2;
        if (damage < 1) damage = 1;
      } else if (diff == 3) {
        // Hard: 1.5x, round up
        damage = (damage * 3 + 1) / 2;
        if (damage < 1) damage = 1;
      }
    }

    damageAttackerItem(attacker_id, damage_type);

    // Calculate damage reduction from player's armor
    uint8_t defense = getPlayerDefensePoints(player);
    // This uses the old (pre-1.9) protection calculation. Factors are
    // scaled up 256 times to avoid floating point math. Due to lost
    // precision, the 4% reduction factor drops to ~3.9%, although the
    // the resulting effective damage is then also rounded down.
    uint8_t effective_damage = damage * (256 - defense * 10) / 256;

    // Calculate damage reduction from player's shield
    if (player->flags & 0x0100) {
      if (
        damage_type == D_arrow ||
        damage_type == D_mob_attack ||
        damage_type == D_player_attack ||
        damage_type == D_mob_projectile ||
        damage_type == D_thrown ||
        damage_type == D_trident ||
        damage_type == D_explosion ||
        damage_type == D_player_explosion ||
        attacker_id != 0 // Any attack from a player or mob
      ) {
        effective_damage = 0;
        damage_blocked = true;

        // Play shield block sound for all nearby players
        for (int i = 0; i < MAX_PLAYERS; i++) {
          if (player_data[i].client_fd == -1) continue;
          if (player_data[i].flags & 0x20) continue;
          sc_soundEntity(player_data[i].client_fd, S_ITEM_SHIELD_BLOCK, SOUND_CATEGORY_PLAYERS, entity_id, 1.0f, 1.0f);
        }
      }
    }

    // Process health change on the server
    if (player->health <= effective_damage) {

      player->health = 0;
      entity_died = true;

      // Auto-respawn if doImmediateRespawn is enabled
      if (config.do_immediate_respawn) {
        sc_respawn(player->client_fd, player->dimension);
        resetPlayerData(player);
        spawnPlayer(player);
      }

      // Prepare death message in recv_buffer
      uint8_t player_name_len = strlen(player->name);
      strcpy((char *)recv_buffer, player->name);

      if (damage_type == D_fall && damage > 8) {
        // Killed by a greater than 5 block fall
        strcpy((char *)recv_buffer + player_name_len, " fell from a high place");
        recv_buffer[player_name_len + 23] = '\0';
      } else if (damage_type == D_fall) {
        // Killed by a less than 5 block fall
        strcpy((char *)recv_buffer + player_name_len, " hit the ground too hard");
        recv_buffer[player_name_len + 24] = '\0';
      } else if (damage_type == D_lava) {
        // Killed by being in lava
        strcpy((char *)recv_buffer + player_name_len, " tried to swim in lava");
        recv_buffer[player_name_len + 22] = '\0';
      } else if (damage_type == D_hot_floor) {
        // Killed by standing on magma
        strcpy((char *)recv_buffer + player_name_len, " discovered the floor was lava");
        recv_buffer[player_name_len + 31] = '\0';
      } else if (damage_type == D_campfire) {
        // Killed by standing in a campfire
        strcpy((char *)recv_buffer + player_name_len, " went up in flames");
        recv_buffer[player_name_len + 17] = '\0';
      } else if (attacker_id < -1) {
        // Killed by a mob
        strcpy((char *)recv_buffer + player_name_len, " was slain by a mob");
        recv_buffer[player_name_len + 19] = '\0';
      } else if (attacker_id > 0) {
        // Killed by a player
        PlayerData *attacker;
        if (getPlayerData(attacker_id, &attacker)) return;
        strcpy((char *)recv_buffer + player_name_len, " was slain by ");
        strcpy((char *)recv_buffer + player_name_len + 14, attacker->name);
        recv_buffer[player_name_len + 14 + strlen(attacker->name)] = '\0';
      } else if (damage_type == D_drown) {
        // Killed by drowning
        strcpy((char *)recv_buffer + player_name_len, " drowned");
        recv_buffer[player_name_len + 8] = '\0';
      } else if (damage_type == D_cactus) {
        // Killed by being near a cactus
        strcpy((char *)recv_buffer + player_name_len, " was pricked to death");
        recv_buffer[player_name_len + 21] = '\0';
      } else if (damage_type == D_out_of_world) {
        // Killed by falling into the void
        strcpy((char *)recv_buffer + player_name_len, " fell from the world");
        recv_buffer[player_name_len + 20] = '\0';
      } else {
        // Unknown death reason
        strcpy((char *)recv_buffer + player_name_len, " died");
        recv_buffer[player_name_len + 5] = '\0';
      }

    } else player->health -= effective_damage;

    // Update health on the client
    sc_setHealth(player->client_fd, player->health, player->hunger, player->saturation);

  } else { // The attacked entity is a mob

    int mob_index = -entity_id - 2;
    if (mob_index < 0 || mob_index >= MAX_MOBS) return;
    MobData *mob = &mob_data[mob_index];

    uint8_t mob_health = mob->data & 31;

    // Don't continue if the mob is already dead
    if (mob_health == 0) return;

    damageAttackerItem(attacker_id, damage_type);

    // Set the mob's panic timer
    mob->data |= (3 << 6);
    // Make passive mobs run instantly when hit
    if (mob->type == 25 || mob->type == 28 || mob->type == 95 ||
        mob->type == 106 || mob->type == E_VILLAGER ||
        mob->type == E_COD || mob->type == E_SALMON ||
        mob->type == E_PUFFERFISH || mob->type == E_TROPICAL_FISH) {
      mob->move_timer = 0; // Force immediate direction change
      mob->move_dx = 0;   // Clear current movement
      mob->move_dz = 0;
    }

    // Hurt sound for passive mobs
    if (mob->type == 25 || mob->type == 28 || mob->type == 95 ||
        mob->type == 106 || mob->type == E_VILLAGER ||
        mob->type == E_COD || mob->type == E_SALMON ||
        mob->type == E_PUFFERFISH || mob->type == E_TROPICAL_FISH) {
      int hurt_sound = -1;
      switch (mob->type) {
        case 25: hurt_sound = S_CHICKEN_HURT; break;
        case 28: hurt_sound = S_COW_HURT; break;
        case 95: hurt_sound = S_PIG_HURT; break;
        case 106: hurt_sound = S_SHEEP_HURT; break;
        case E_VILLAGER: hurt_sound = S_VILLAGER_HURT; break;
        case E_COD: hurt_sound = S_COD_HURT; break;
        case E_SALMON: hurt_sound = S_SALMON_HURT; break;
        case E_PUFFERFISH: hurt_sound = S_PUFFERFISH_HURT; break;
        case E_TROPICAL_FISH: hurt_sound = S_TROPICAL_FISH_HURT; break;
      }
      if (hurt_sound > 0) {
        broadcastMobSound(entity_id, hurt_sound, SOUND_CATEGORY_NEUTRAL, 1.0f, 1.0f);
      }
    }

    // Hitting one zombified piglin angers nearby zombified piglins, but keep
    // the alert radius modest so one hit does not pull mobs from far away.
    if (attacker_id > 0 && mob->type == 148) {
      const double piglin_alert_range = 12.0;
      for (int i = 0; i < MAX_MOBS; i ++) {
        if (mob_data[i].type != 148) continue;
        if ((mob_data[i].data & 31) == 0) continue;
        if (mob_data[i].dimension != mob->dimension) continue;
        double dx = mob_data[i].x - mob->x;
        double dz = mob_data[i].z - mob->z;
        if (dx * dx + dz * dz > piglin_alert_range * piglin_alert_range) continue;
        mob_data[i].anger_timer = 30 * TICKS_PER_SECOND;
        mob_data[i].move_timer = 0;
      }
    }

    // Knockback is now handled by hurtEntity's velocity-based knockback system

    // Piglins get angry when hit by a player
    if (attacker_id > 0 && mob->type == E_PIGLIN) {
      mob->anger_timer = 40; // 2 seconds of anger (40 ticks at 20 TPS)
      broadcastMobSound(entity_id, S_PIGLIN_ANGRY, SOUND_CATEGORY_NEUTRAL, 1.0f, 1.0f);
    }

    // Endermen get angry when hit by a player
    if (attacker_id > 0 && mob->type == E_ENDERMAN) {
      mob->anger_timer = 40 * TICKS_PER_SECOND; // 40 seconds of anger
      mob->move_timer = 0;
      broadcastMobMetadata(-1, entity_id);
      broadcastMobSound(entity_id, S_ENDERMAN_SCREAM, SOUND_CATEGORY_HOSTILE, 1.0f, 1.0f);
    }

    // Hurt sound for hostile/neutral mobs when hit
    if (attacker_id > 0) {
      int hurt_sound = -1;
      int hurt_cat = SOUND_CATEGORY_HOSTILE;
      switch (mob->type) {
        case 145: hurt_sound = S_ZOMBIE_HURT; break;
        case E_SKELETON: hurt_sound = S_SKELETON_HURT; break;
        case E_SPIDER: hurt_sound = S_SPIDER_HURT; break;
        case E_CREEPER: hurt_sound = S_CREEPER_HURT; break;
        case E_ENDERMAN: hurt_sound = S_ENDERMAN_HURT; break;
        case E_PIGLIN: hurt_sound = S_PIGLIN_HURT; hurt_cat = SOUND_CATEGORY_NEUTRAL; break;
        case 148: hurt_sound = S_ZOMBIFIED_PIGLIN_HURT; break;
      }
      if (hurt_sound > 0) {
        broadcastMobSound(entity_id, hurt_sound, hurt_cat, 1.0f, (float)(fast_rand() % 20 + 90) / 100.0f);
      }
    }

    // Process health change on the server
    if (mob_health <= damage) {

      double mob_death_y = mob->y;
      mob->data -= mob_health;
      mob->y = 0;
      entity_died = true;

      // Handle mob drops
      if (attacker_id > 0) {
        PlayerData *player;
        if (getPlayerData(attacker_id, &player)) return;
        switch (mob->type) {
          case 25: spawnItemEntity(mob->x, mob_death_y + 0.5, mob->z, I_chicken, 1, mob->dimension, 0, 0, 0); break;
          case 28: spawnItemEntity(mob->x, mob_death_y + 0.5, mob->z, I_beef, 1 + (fast_rand() % 3), mob->dimension, 0, 0, 0); break;
          case 95: spawnItemEntity(mob->x, mob_death_y + 0.5, mob->z, I_porkchop, 1 + (fast_rand() % 3), mob->dimension, 0, 0, 0); break;
          case 106: spawnItemEntity(mob->x, mob_death_y + 0.5, mob->z, I_mutton, 1 + (fast_rand() & 1), mob->dimension, 0, 0, 0); break;
          case E_COD: spawnItemEntity(mob->x, mob_death_y + 0.5, mob->z, I_cod, 1, mob->dimension, 0, 0, 0); break;
          case E_SALMON: spawnItemEntity(mob->x, mob_death_y + 0.5, mob->z, I_salmon, 1, mob->dimension, 0, 0, 0); break;
          case E_PUFFERFISH: spawnItemEntity(mob->x, mob_death_y + 0.5, mob->z, I_pufferfish, 1, mob->dimension, 0, 0, 0); break;
          case E_TROPICAL_FISH: spawnItemEntity(mob->x, mob_death_y + 0.5, mob->z, I_tropical_fish, 1, mob->dimension, 0, 0, 0); break;
          case E_PIGLIN:
            spawnItemEntity(mob->x, mob_death_y + 0.5, mob->z, getRandomCreativeItem(), 1, mob->dimension, 0, 0, 0);
            break;
          case E_CREEPER:
            spawnItemEntity(mob->x, mob_death_y + 0.5, mob->z, I_gunpowder, 1 + (fast_rand() % 2), mob->dimension, 0, 0, 0);
            break;
          case E_ENDERMAN:
            if ((fast_rand() & 3) == 0) spawnItemEntity(mob->x, mob_death_y + 0.5, mob->z, I_ender_pearl, 1, mob->dimension, 0, 0, 0);
            break;
          case E_SPIDER:
            spawnItemEntity(mob->x, mob_death_y + 0.5, mob->z, I_string, fast_rand() % 3, mob->dimension, 0, 0, 0);
            if ((fast_rand() & 3) == 0) spawnItemEntity(mob->x, mob_death_y + 0.5, mob->z, I_spider_eye, 1, mob->dimension, 0, 0, 0);
            break;
          case 145: spawnItemEntity(mob->x, mob_death_y + 0.5, mob->z, I_rotten_flesh, (fast_rand() % 3), mob->dimension, 0, 0, 0); break;
          case E_SKELETON:
            if ((fast_rand() & 1) == 0) spawnItemEntity(mob->x, mob_death_y + 0.5, mob->z, I_bone, 1, mob->dimension, 0, 0, 0);
            if ((fast_rand() % 3) == 0) spawnItemEntity(mob->x, mob_death_y + 0.5, mob->z, I_arrow, 1 + (fast_rand() % 2), mob->dimension, 0, 0, 0);
            break;
          case 148:
            spawnItemEntity(mob->x, mob_death_y + 0.5, mob->z, I_rotten_flesh, fast_rand() % 3, mob->dimension, 0, 0, 0);
            if ((fast_rand() & 3) == 0) spawnItemEntity(mob->x, mob_death_y + 0.5, mob->z, I_gold_ingot, 1, mob->dimension, 0, 0, 0);
            break;
          default: break;
        }
      }

      // Spawn XP orbs on mob death
      uint8_t xp_value = 0;
      switch (mob->type) {
        case 25: xp_value = 1 + (fast_rand() & 1); break;   // Chicken: 1-2 XP
        case 28: xp_value = 1 + (fast_rand() % 3); break;    // Cow: 1-3 XP
        case 95: xp_value = 1 + (fast_rand() % 3); break;    // Pig: 1-3 XP
        case 106: xp_value = 1 + (fast_rand() & 1); break;   // Sheep: 1-2 XP
        case E_VILLAGER: xp_value = 3 + (fast_rand() & 1); break; // Villager: 3-4 XP
        case E_COD:
        case E_SALMON:
        case E_PUFFERFISH:
        case E_TROPICAL_FISH: xp_value = 1; break;           // Fish: 1 XP
        case 145: xp_value = 5; break;                         // Zombie: 5 XP
        case E_SKELETON: xp_value = 5; break;                  // Skeleton: 5 XP
        case E_SPIDER: xp_value = 5; break;                    // Spider: 5 XP
        case E_CREEPER: xp_value = 5; break;                   // Creeper: 5 XP
        case E_ENDERMAN: xp_value = 5; break;                  // Enderman: 5 XP
        case E_PIGLIN: xp_value = 5; break;                    // Piglin: 5 XP
        case 148: xp_value = 5; break;                         // Zombified Piglin: 5 XP
        default: xp_value = 2; break;                          // Other: 2 XP
      }
      if (xp_value > 0) {
        spawnXpOrb(mob->x, mob_death_y + 0.5, mob->z, xp_value, mob->dimension);
      }

    } else mob->data -= damage;

  }

  // Broadcast damage event to all players
  for (int i = 0; i < MAX_PLAYERS; i ++) {
    int client_fd = player_data[i].client_fd;
    if (client_fd == -1) continue;
    if (!damage_blocked) sc_damageEvent(client_fd, entity_id, damage_type);
    // Below this, handle death events
    if (!entity_died) continue;
    sc_entityEvent(client_fd, entity_id, 3);
    if (entity_id >= 0) {
      // If a player died, broadcast their death message
      sc_systemChat(client_fd, (char *)recv_buffer, strlen((char *)recv_buffer));
    }
  }

}

// Creates an explosion that destroys blocks in a spherical radius
void doExplosion (double x, double y, double z, float power, uint8_t dimension) {
  int radius = (int)ceilf(power);
  int rad_sq = (int)(power * power);

  for (int bx = (int)x - radius; bx <= (int)x + radius; bx++) {
    for (int by = (int)y - radius; by <= (int)y + radius; by++) {
      for (int bz = (int)z - radius; bz <= (int)z + radius; bz++) {
        double dx = bx + 0.5 - x;
        double dy = by + 0.5 - y;
        double dz = bz + 0.5 - z;
        int dist_sq = (int)(dx * dx + dy * dy + dz * dz);
        if (dist_sq > rad_sq) continue;
        if (is_in_safe_area((short)bx, (short)bz, dimension)) continue;

        uint16_t block = getBlockAt2(bx, by, bz, dimension);
        if (block != 0 && block != 7) { // skip air and bedrock
          makeBlockChange((short)bx, (int16_t)by, (short)bz, 0, dimension);
        }
      }
    }
  }
}

// Checks if there is a clear line of sight between two points (no solid blocks in the way)
static uint8_t hasLineOfSight (double x1, double y1, double z1, double x2, double y2, double z2, uint8_t dimension) {
  double dx = x2 - x1;
  double dy = y2 - y1;
  double dz = z2 - z1;
  double dist = sqrt(dx * dx + dy * dy + dz * dz);
  int steps = (int)(dist * 2.0) + 1;
  if (steps < 1) steps = 1;
  if (steps > 16) steps = 16; // cap for performance (only used at close range)

  for (int i = 0; i <= steps; i++) {
    double t = (double)i / (double)steps;
    int bx = (int)floor(x1 + dx * t);
    int by = (int)floor(y1 + dy * t);
    int bz = (int)floor(z1 + dz * t);
    uint16_t block = getBlockAt2(bx, by, bz, dimension);
    if (block != 0 && !isPassableBlock(block)) return 0;
  }
  return 1;
}

// Simulates events scheduled for regular intervals
// Takes the time since the last tick in microseconds as the only arguemnt
void handleServerTick (int64_t time_since_last_tick) {

  // Update world time
  if (config.do_daylight_cycle) {
    world_time = (world_time + time_since_last_tick / 50000) % 24000;
  }
  // Increment server tick counter
  server_ticks ++;

  // Get thread pool for parallel operations
  ThreadPool* pool = get_global_thread_pool();

  // Update player events (parallelized if thread pool is available)
  if (pool != NULL) {
    handlePlayerUpdatesParallel(time_since_last_tick, pool);
  } else {
    // Fallback to sequential processing
    for (int i = 0; i < MAX_PLAYERS; i ++) {
      PlayerData *player = &player_data[i];
      if (player->client_fd == -1) continue;
      if (player->flags & 0x20) {
        player->flagval_16 ++;
        if (player->flagval_16 > (uint16_t)(3 * TICKS_PER_SECOND)) {
          handlePlayerJoin(player);
        } else continue;
      }
      if (player->position_lock_ticks > 0) {
        player->position_lock_ticks--;
        player->x = player->locked_x;
        player->y = player->locked_y;
        player->z = player->locked_z;
        player->grounded_y = player->locked_y;
      }
      if (player->flags & 0x01) {
        if (player->flagval_8 >= (uint8_t)(0.6f * TICKS_PER_SECOND)) {
          player->flags &= ~0x01;
          player->flagval_8 = 0;
        } else player->flagval_8 ++;
      }
      if (player->flags & 0x10) {
        if (player->flagval_16 >= (uint16_t)(0.8f * TICKS_PER_SECOND)) {
          handlePlayerEating(&player_data[i], false);
          player->flags &= ~0x10;
          player->flagval_16 = 0;
        } else player->flagval_16 ++;
      }
      #ifndef BROADCAST_ALL_MOVEMENT
        player->flags &= ~0x40;
      #endif

      // ---- Air supply & drowning (runs every tick) ----
      {
        uint8_t pdim = player->dimension;
        // Check if the player's head is in water (block at y+1)
        uint16_t head_block = getBlockAt2(player->x, player->y + 1, player->z, pdim);
        uint8_t head_in_water = (head_block >= B_water && head_block < B_water + 8);

        if (head_in_water) {
          // Submerged: deplete air
          if (player->air > 0) player->air--;
        } else {
          // Not submerged: replenish air
          if (player->air < 300) {
            player->air += 4;
            if (player->air > 300) player->air = 300;
          }
        }
      }

      if (server_ticks % (uint32_t)TICKS_PER_SECOND != 0) continue;
      sc_keepAlive(player->client_fd);
      sc_updateTime(player->client_fd, world_time);
      uint8_t pdim = player->dimension;
      uint16_t block = getBlockAt2(player->x, player->y, player->z, pdim);
      if (block >= B_lava && block < B_lava + 4) {
        hurtEntity(player->client_fd, -1, D_lava, 8);
      }
      if (!(player->flags & 0x04) && getBlockAt2(player->x, player->y - 1, player->z, pdim) == B_magma_block) {
        hurtEntity(player->client_fd, -1, D_hot_floor, 1);
      }
      // Campfire damage - standing inside a campfire
      uint16_t block_at_feet = getBlockAt2(player->x, player->y, player->z, pdim);
      if (block_at_feet == B_campfire || getBlockAt2(player->x, player->y - 1, player->z, pdim) == B_campfire) {
        hurtEntity(player->client_fd, -1, D_campfire, 1);
      }
      #ifdef ENABLE_CACTUS_DAMAGE
      if (block == B_cactus ||
        getBlockAt2(player->x + 1, player->y, player->z, pdim) == B_cactus ||
        getBlockAt2(player->x - 1, player->y, player->z, pdim) == B_cactus ||
        getBlockAt2(player->x, player->y, player->z + 1, pdim) == B_cactus ||
        getBlockAt2(player->x, player->y, player->z - 1, pdim) == B_cactus
      ) hurtEntity(player->client_fd, -1, D_cactus, 4);
      #endif
      // Drowning damage - deals 2 damage (1 heart) per second when air is depleted
      if (player->air == 0) {
        hurtEntity(player->client_fd, -1, D_drown, 2);
      }
      // Starvation damage when hunger is 0
      if (player->hunger == 0 && player->health > 0) {
        uint8_t diff = getConfiguredDifficulty();
        if (diff == 1 && player->health > 10) {
          hurtEntity(player->client_fd, -1, D_generic, 1); // Easy: stops at 5 hearts
        } else if (diff == 2 && player->health > 1) {
          hurtEntity(player->client_fd, -1, D_generic, 1); // Normal: stops at 0.5 heart
        } else if (diff == 3) {
          hurtEntity(player->client_fd, -1, D_generic, 1); // Hard: can kill
        }
        // Peaceful: no starvation damage
      }
      // Natural regeneration
      if (!config.natural_regeneration) continue;
      if (player->health >= 20 || player->health == 0) continue;
      if (player->hunger < 18) continue;
      uint8_t diff = getConfiguredDifficulty();
      if (diff == 0) {
        // Peaceful: regen 1 HP per second for free
        player->health++;
      } else {
        if (player->saturation >= 600) {
          player->saturation -= 600;
          player->health ++;
        } else {
          player->hunger --;
          player->health ++;
        }
      }
      sc_setHealth(player->client_fd, player->health, player->hunger, player->saturation);
    }
  }

  // Weather progression - every ~2 seconds (40 ticks)
  if (config.do_weather_cycle && server_ticks % 40 == 0) {
    uint8_t weather_changed = 0;

    if (world_weather_thunder_time > 0) {
      world_weather_thunder_time -= 40;
      if (world_weather_thunder_time <= 0) {
        world_weather_thunder_time = 0;
        world_thunder_level = 0.0f;
        weather_changed = 1;
      }
    }

    if (world_weather_rain_time > 0) {
      world_weather_rain_time -= 40;
      if (world_weather_rain_time <= 0) {
        world_weather_rain_time = 0;
        world_weather_clear = 1;
        world_rain_level = 0.0f;
        world_thunder_level = 0.0f;
        world_weather_thunder_time = 0;
        world_weather_clear_time = 12000 + (int32_t)(fast_rand() % 168000);
        weather_changed = 1;
      }
    }

    if (world_weather_clear_time > 0) {
      world_weather_clear_time -= 40;
      if (world_weather_clear_time <= 0) {
        world_weather_clear_time = 0;
        world_weather_clear = 0;
        world_rain_level = 1.0f;
        world_weather_rain_time = 12000 + (int32_t)(fast_rand() % 12000);
        if ((fast_rand() % 4) == 0) {
          world_thunder_level = 1.0f;
          world_weather_thunder_time = 3600 + (int32_t)(fast_rand() % 12000);
          if (world_weather_thunder_time > world_weather_rain_time)
            world_weather_thunder_time = world_weather_rain_time;
        }
        weather_changed = 1;
      }
    }

    if (weather_changed) {
      for (int i = 0; i < MAX_PLAYERS; i++) {
        if (player_data[i].client_fd == -1) continue;
        if (player_data[i].flags & 0x20) continue;
        if (player_data[i].dimension != DIMENSION_OVERWORLD) {
          sc_gameEvent(player_data[i].client_fd, 2, 0.0f);
          sc_gameEvent(player_data[i].client_fd, 7, 0.0f);
          sc_gameEvent(player_data[i].client_fd, 8, 0.0f);
        } else if (world_weather_clear) {
          sc_gameEvent(player_data[i].client_fd, 2, 0.0f);
          sc_gameEvent(player_data[i].client_fd, 7, 0.0f);
          sc_gameEvent(player_data[i].client_fd, 8, 0.0f);
        } else {
          sc_gameEvent(player_data[i].client_fd, 1, 0.0f);
          sc_gameEvent(player_data[i].client_fd, 7, world_rain_level);
          sc_gameEvent(player_data[i].client_fd, 8, world_thunder_level);
        }
      }
    }
  }

  // Check for portal travel
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (player_data[i].client_fd == -1) continue;
    if (player_data[i].flags & 0x20) continue;
    handlePortalTravel(&player_data[i]);
  }

  // Some clients repeatedly send Use Item while a bow is held instead of only
  // sending one Use Item followed by Player Action/release. Treat a short gap
  // in those packets as releasing the bow so holding charges and releasing fires once.
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (player_bow_draw_start[i] == 0) continue;
    PlayerData *player = &player_data[i];
    if (player->client_fd == -1 || player->flags & 0x20 || player->inventory_items[player->hotbar] != I_bow) {
      player_bow_draw_start[i] = 0;
      player_bow_last_use_tick[i] = 0;
      if (player->client_fd != -1) setPlayerActiveHand(player, 0);
      continue;
    }
    if (
      player_bow_last_use_tick[i] != player_bow_draw_start[i] &&
      server_ticks - player_bow_last_use_tick[i] > 4
    ) shootBowArrow(player);
  }

  // Process queued fluid updates (spread across ticks for gradual flow)
  #ifdef DO_FLUID_FLOW
  if (config.do_fluid_flow) {
    processFluidQueue();
  }
  #endif

  // Sync all player inventories and weapon attributes every 2 seconds (40 ticks)
  if (server_ticks % 40 == 0) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
      PlayerData *player = &player_data[i];
      if (player->client_fd == -1) continue;
      if (player->flags & 0x20) continue;
      for (int j = 0; j < 41; j++) {
        sc_setContainerSlot(player->client_fd, 0, serverSlotToClientSlot(0, j),
          player->inventory_count[j], player->inventory_items[j]);
      }
      // Keep weapon attributes in sync (ensures correct cooldown indicator after item changes)
      sc_updateEntityAttributes(player->client_fd, player->client_fd, player->inventory_items[player->hotbar]);
    }
  }

  // Perform regular checks for if it's time to write to disk
  writeDataToDiskOnInterval();

  // Wheat growth random tick every ~2 seconds (40 ticks)
  if (server_ticks % 40 == 0) {
    uint32_t seed = fast_rand();

    // Process wheat using dedicated tracking list, avoiding a full scan of all
    // 8192 special block entries.
    for (int wi = 0; wi < wheat_count; wi++) {
      WheatCoord *wc = &wheat_coords[wi];
      uint16_t state = special_block_get_state(wc->x, wc->y, wc->z, wc->dimension);
      uint16_t age = state & 7;
      if (age >= 7) continue;
      uint16_t current = getBlockAt2(wc->x, wc->y, wc->z, wc->dimension);
      // Accept both player-planted wheat (B_wheat) and generated wheat (B_wheat_1..B_wheat_6)
      if (current != B_wheat && (current <= B_wheat || current >= B_wheat_7)) continue;
      if ((seed >> (wi & 31)) & 1) {
        age++;
        // Update state through the hash table API - will re-add to wheat list
        // but wheat_list_add checks for duplicates, so this is harmless.
        special_block_set_state(wc->x, wc->y, wc->z, wc->dimension, B_wheat, age);
        // For generated wheat (current > B_wheat), convert to planted form so
        // the chunk section's default palette state doesn't override our updates.
        if (current > B_wheat) {
          makeBlockChange(wc->x, wc->y, wc->z, B_wheat, wc->dimension);
        }
        // Broadcast the new visual state to all players
        uint16_t state_id = block_palette[B_wheat] + age;
        for (int j = 0; j < MAX_PLAYERS; j++) {
          if (player_data[j].client_fd == -1) continue;
          if (player_data[j].flags & 0x20) continue;
          sc_blockUpdateState(player_data[j].client_fd,
            wc->x, wc->y, wc->z, state_id);
        }
      }
    }

    // Fallback: scan the hash table for any B_wheat entries that weren't added
    // to the tracking list (e.g. when the hash table was full at planting time).
    // Register any untracked wheat so it starts growing on the next cycle.
    int sb_cap = special_blocks_capacity;
    if (wheat_count < sb_cap) {
      for (int si = 0; si < sb_cap; si++) {
        if (special_blocks[si].block != B_wheat) continue;
        short sx = special_blocks[si].x;
        uint8_t sy = special_blocks[si].y;
        short sz = special_blocks[si].z;
        uint8_t sdim = special_blocks[si].dimension;
        // Check if already in tracking list
        uint8_t found = 0;
        for (int wi = 0; wi < wheat_count; wi++) {
          if (wheat_coords[wi].x == sx && wheat_coords[wi].y == sy &&
              wheat_coords[wi].z == sz && wheat_coords[wi].dimension == sdim) {
            found = 1;
            break;
          }
        }
        if (!found) {
          // Ensure capacity (the list grows dynamically, but we're outside
          // the special_blocks mutex here; just skip if we'd exceed current cap)
          if (wheat_count >= wheat_capacity) break;
          wheat_coords[wheat_count].x = sx;
          wheat_coords[wheat_count].y = sy;
          wheat_coords[wheat_count].z = sz;
          wheat_coords[wheat_count].dimension = sdim;
          wheat_count++;
        }
      }
    }
  }

  /**
   * If the RNG seed ever hits 0, it'll never generate anything
   * else. This is because the fast_rand function uses a simple
   * XORshift. This isn't a common concern, so we only check for
   * this periodically. If it does become zero, we reset it to
   * the world seed as a good-enough fallback.
   */
  if (rng_seed == 0) rng_seed = world_seed;

  // Tick mob behavior
  // Build spatial hash ONCE before the mob loop for O(n) collision detection
  // (mobs move < 0.1 blocks/tick, so the grid is stable for a full tick)
  mob_grid_build_all();
  for (int i = 0; i < MAX_MOBS; i ++) {
    if (mob_data[i].type == 0) continue;
    int entity_id = -2 - i;

    // Handle deallocation on mob death
    if ((mob_data[i].data & 31) == 0) {
      if (mob_data[i].y < (unsigned int)TICKS_PER_SECOND) {
        mob_data[i].y ++;
        continue;
      }
      mob_data[i].type = 0;
      for (int j = 0; j < MAX_PLAYERS; j ++) {
        if (player_data[j].client_fd == -1) continue;
        // Spawn death smoke particles
        sc_entityEvent(player_data[j].client_fd, entity_id, 60);
        // Remove the entity from the client
        sc_removeEntity(player_data[j].client_fd, entity_id);
      }
      continue;
    }

    uint8_t is_fish = (
      mob_data[i].type == E_COD ||
      mob_data[i].type == E_SALMON ||
      mob_data[i].type == E_PUFFERFISH ||
      mob_data[i].type == E_TROPICAL_FISH
    );
    uint8_t passive = (
      mob_data[i].type == 25 || // Chicken
      mob_data[i].type == 28 || // Cow
      mob_data[i].type == 95 || // Pig
      mob_data[i].type == 106 || // Sheep
      mob_data[i].type == E_VILLAGER || // Villager
      is_fish
    );
    // Mob "panic" timer, set to 3 after being hit
    // Currently has no effect on hostile mobs
    uint8_t panic = (mob_data[i].data >> 6) & 3;

    // Burn overworld zombies and skeletons if above ground during sunlight.
    // Nether mobs should not burn based on overworld time of day.
    if (mob_data[i].dimension == DIMENSION_OVERWORLD &&
        (mob_data[i].type == 145 || mob_data[i].type == E_SKELETON) &&
        (world_time < 13000 || world_time > 23460) && mob_data[i].y > 48) {
      hurtEntity(entity_id, -1, D_on_fire, 2);
    }

    uint32_t r = fast_rand();

    uint8_t enderman_was_angry = (mob_data[i].type == E_ENDERMAN && mob_data[i].anger_timer > 0);

    if (mob_data[i].anger_timer > 0) mob_data[i].anger_timer --;

    uint8_t neutral_roaming = (
      (mob_data[i].type == 148 && mob_data[i].anger_timer <= 0) ||
      (mob_data[i].type == E_PIGLIN && mob_data[i].anger_timer <= 0) ||
      (mob_data[i].type == E_ENDERMAN && mob_data[i].anger_timer <= 0)
    );
    uint8_t roaming = passive || neutral_roaming;

    // Process panic timer (no early continue - mobs move every tick now)
    // Decrement every 2 seconds for longer panic duration
    if (roaming && panic && server_ticks % (2 * (uint32_t)TICKS_PER_SECOND) == 0) {
      // Properly decrement panic timer (2 bits at positions 6-7)
      mob_data[i].data = (mob_data[i].data & ~(3 << 6)) | ((panic - 1) << 6);
    }

    // Find the player closest to this mob
    PlayerData* closest_player = NULL;
    double closest_dist_double = 2147483647.0;
    for (int j = 0; j < MAX_PLAYERS; j ++) {
      if (player_data[j].client_fd == -1) continue;
      if (player_data[j].dimension != mob_data[i].dimension) continue;
      double dx = mob_data[i].x - player_data[j].x;
      double dz = mob_data[i].z - player_data[j].z;
      double curr_dist = dx * dx + dz * dz; // Squared distance
      if (curr_dist < closest_dist_double) {
        closest_dist_double = curr_dist;
        closest_player = &player_data[j];
      }
    }

    // Proximity gating: skip expensive AI for mobs far from players
    uint8_t is_close_to_player = (closest_player != NULL && closest_dist_double < 2500.0); // 50 blocks squared

    // Random ambient sounds - passive mobs vocalize more often
    // Only process every 4 ticks and for mobs reasonably close to players
    if (is_close_to_player && server_ticks % 4 == 0) {
      int ambient_chance = (passive || is_fish) ? 500 : 1500;
      if (r % ambient_chance == 0) {
        int ambient_sound = -1;
        int ambient_cat = SOUND_CATEGORY_NEUTRAL;
        switch (mob_data[i].type) {
          case E_ZOMBIE: ambient_sound = S_ZOMBIE_AMBIENT; ambient_cat = SOUND_CATEGORY_HOSTILE; break;
          case E_SKELETON: ambient_sound = S_SKELETON_AMBIENT; ambient_cat = SOUND_CATEGORY_HOSTILE; break;
          case E_SPIDER: ambient_sound = S_SPIDER_AMBIENT; ambient_cat = SOUND_CATEGORY_HOSTILE; break;
          case E_CREEPER: break; // No ambient sound for creepers
          case E_ENDERMAN:
            if (mob_data[i].anger_timer <= 0) { ambient_sound = S_ENDERMAN_AMBIENT; ambient_cat = SOUND_CATEGORY_HOSTILE; }
            break;
          case E_PIGLIN:
            ambient_sound = mob_data[i].anger_timer > 0 ? S_PIGLIN_ANGRY : S_PIGLIN_AMBIENT;
            ambient_cat = SOUND_CATEGORY_NEUTRAL;
            break;
          case 148:
            ambient_sound = mob_data[i].anger_timer > 0 ? S_ZOMBIFIED_PIGLIN_ANGRY : S_ZOMBIFIED_PIGLIN_AMBIENT;
            ambient_cat = SOUND_CATEGORY_HOSTILE;
            break;
          case 25: ambient_sound = S_CHICKEN_AMBIENT; break;  // Chicken
          case 28: ambient_sound = S_COW_AMBIENT; break;      // Cow
          case 95: ambient_sound = S_PIG_AMBIENT; break;      // Pig
          case 106: ambient_sound = S_SHEEP_AMBIENT; break;   // Sheep
          case E_VILLAGER: ambient_sound = S_VILLAGER_AMBIENT; break;
          case E_COD: ambient_sound = S_COD_AMBIENT; break;
          case E_SALMON: ambient_sound = S_SALMON_AMBIENT; break;
          case E_TROPICAL_FISH: ambient_sound = S_TROPICAL_FISH_AMBIENT; break;
          default: break;
        }
        if (ambient_sound > 0) {
          broadcastMobSound(entity_id, ambient_sound, ambient_cat, 1.0f, (float)(fast_rand() % 20 + 90) / 100.0f);
        }
      }
    }

    // Enderman stare mechanic: they agro when you look at them
    if (mob_data[i].type == E_ENDERMAN && closest_player != NULL) {
      double dx = mob_data[i].x - closest_player->x;
      double dz = mob_data[i].z - closest_player->z;
      double dist_sq = dx * dx + dz * dz;
      if (dist_sq < 256.0) {
        double angle_rad = atan2(dz, dx);
        int16_t target_yaw = (int16_t)(angle_rad * RAD_TO_MOBROT);
        target_yaw = (uint8_t)((target_yaw - 64) & 255);
        uint8_t player_yaw = (uint8_t)(int8_t)closest_player->yaw;
        int16_t diff = (int16_t)((int16_t)player_yaw - target_yaw);
        if (diff > 127) diff -= 256;
        if (diff < -128) diff += 256;
        uint8_t staring = 0;
        if (abs(diff) <= 30) {
          double dy = mob_data[i].y - (closest_player->y + 1.6);
          double horiz_dist = sqrt(dist_sq);
          if (horiz_dist > 0.5) {
            double pitch_rad = atan2(dy, horiz_dist);
            int8_t target_pitch = (int8_t)(pitch_rad * RAD_TO_MOBROT);
            int16_t pitch_diff = (int16_t)((int16_t)(int8_t)closest_player->pitch - (int16_t)target_pitch);
            if (abs((int)pitch_diff) <= 20) staring = 1;
          }
        }
        if (staring) {
          if (mob_data[i].anger_timer <= 0) {
            mob_data[i].anger_timer--;
            if (mob_data[i].anger_timer <= -(int)(1.0f * TICKS_PER_SECOND)) {
              mob_data[i].anger_timer = 30 * TICKS_PER_SECOND;
              mob_data[i].move_timer = 0;
              broadcastMobSound(entity_id, S_ENDERMAN_STARE, SOUND_CATEGORY_HOSTILE, 1.0f, 1.0f);
            }
          }
        } else if (mob_data[i].anger_timer < 0) {
          mob_data[i].anger_timer = 0;
        }
      }
    }

    // Broadcast enderman anger metadata on state change
    if (mob_data[i].type == E_ENDERMAN) {
      uint8_t is_angry = (mob_data[i].anger_timer > 0);
      if (is_angry != enderman_was_angry) {
        broadcastMobMetadata(-1, entity_id);
        if (is_angry) {
          broadcastMobSound(entity_id, S_ENDERMAN_SCREAM, SOUND_CATEGORY_HOSTILE, 1.0f, 1.0f);
        }
      }
    }

    // If no players are online, skip AI updates (mobs stay idle)
    if (closest_player == NULL) {
      continue;
    }

    // Despawn non-villagers / skip AI for all mobs past despawn distance.
    // Villagers persist (no despawn) but their AI is frozen to prevent CPU
    // waste from distant villages the player has visited.
    if (closest_dist_double > (double)MOB_DESPAWN_DISTANCE * MOB_DESPAWN_DISTANCE) {
      if (mob_data[i].type != E_VILLAGER) {
        mob_data[i].type = 0;  // Despawn non-villagers
      }
      continue;  // Skip AI for any mob beyond range (including villagers)
    }

    // Random look-around behavior - all mobs occasionally look in random directions
    if (mob_data[i].look_timer > 0) {
      mob_data[i].look_timer--;
    } else if ((fast_rand() & 0xFF) == 0) {
      // ~0.4% chance per tick to start looking around (~once per 12.8 seconds on average)
      mob_data[i].look_timer = 15 + (fast_rand() % 25); // 15-40 ticks (0.75-2 seconds)
      mob_data[i].look_yaw = (uint8_t)(fast_rand() & 255);
      mob_data[i].look_pitch = (int8_t)((fast_rand() % 21) - 10); // -10 to +10
    }

    double old_x = mob_data[i].x, old_z = mob_data[i].z;
    double old_y = mob_data[i].y;

    double new_x = old_x, new_z = old_z;
    double new_y = old_y;
    uint8_t yaw = mob_data[i].yaw_store;  // Keep current yaw from stored direction

    // Movement increment per tick (sub-block movement for smoothness)
    double move_amount = 0.05;  // Move 0.05 blocks per tick for smooth walking

    // Fish-specific swimming behavior
    if (is_fish) {
      // Fish swim faster and can move vertically in water
      move_amount = 0.08;  // Fish swim faster

      // Check if fish is in water
      int fish_block_x = mobBlockCoord(old_x);
      int fish_block_y = mobBlockCoord(old_y);
      int fish_block_z = mobBlockCoord(old_z);
      uint16_t fish_block = getBlockAt2(fish_block_x, fish_block_y, fish_block_z, mob_data[i].dimension);
      uint8_t in_water = isWaterBlock(fish_block);

      // If fish is out of water, try to jump back in
      if (!in_water) {
        // Look for water in a 3x3x3 area around the fish
        uint8_t found_water = 0;
        for (int dx = -1; dx <= 1 && !found_water; dx++) {
          for (int dy = -1; dy <= 1 && !found_water; dy++) {
            for (int dz = -1; dz <= 1 && !found_water; dz++) {
              uint16_t nearby = getBlockAt2(fish_block_x + dx, fish_block_y + dy, fish_block_z + dz, mob_data[i].dimension);
              if (isWaterBlock(nearby)) {
                found_water = 1;
                // Move towards water
                new_x += dx * move_amount * 2;
                new_y += dy * move_amount * 2;
                new_z += dz * move_amount * 2;
                break;
              }
            }
          }
        }

        // Fish out of water take damage and flop
        if (!found_water && server_ticks % 10 == 0) {
          hurtEntity(entity_id, -1, D_generic, 1);
          int flop_sound = -1;
          switch (mob_data[i].type) {
            case E_COD: flop_sound = S_COD_FLOP; break;
            case E_SALMON: flop_sound = S_SALMON_FLOP; break;
            case E_PUFFERFISH: flop_sound = S_PUFFERFISH_FLOP; break;
            case E_TROPICAL_FISH: flop_sound = S_TROPICAL_FISH_FLOP; break;
          }
          if (flop_sound > 0) broadcastMobSound(entity_id, flop_sound, SOUND_CATEGORY_NEUTRAL, 1.0f, 1.0f);
        }
      }

      // Fish movement - more frequent direction changes and vertical movement
      if (mob_data[i].move_timer <= 0) {
        // Pick a random 3D direction
        uint32_t dir_rand = fast_rand();

        // Reset previous movement
        mob_data[i].move_dx = 0;
        mob_data[i].move_dz = 0;

        // Horizontal movement (X or Z axis)
        if ((dir_rand >> 2) & 1) {
          if ((dir_rand >> 1) & 1) {
            mob_data[i].move_dx = move_amount;
            mob_data[i].yaw_store = 192;
          } else {
            mob_data[i].move_dx = -move_amount;
            mob_data[i].yaw_store = 64;
          }
        } else {
          if ((dir_rand >> 1) & 1) {
            mob_data[i].move_dz = move_amount;
            mob_data[i].yaw_store = 0;
          } else {
            mob_data[i].move_dz = -move_amount;
            mob_data[i].yaw_store = 128;
          }
        }

        // Vertical movement - fish can swim up and down (30% chance)
        if ((dir_rand & 7) < 3) { // 3/8 chance for vertical movement
          if (dir_rand & 1) {
            mob_data[i].move_dy = move_amount * 0.5; // Swim up
          } else {
            mob_data[i].move_dy = -move_amount * 0.5; // Swim down
          }
        } else {
          mob_data[i].move_dy = 0;
        }

        // Fish change direction more often (20-60 ticks)
        int min_ticks = (int)(1.0f * TICKS_PER_SECOND);
        int max_ticks = (int)(3.0f * TICKS_PER_SECOND);
        mob_data[i].move_timer = min_ticks + (fast_rand() % (max_ticks - min_ticks + 1));
      }

      // Apply movement
      new_x += mob_data[i].move_dx;
      new_z += mob_data[i].move_dz;
      new_y += mob_data[i].move_dy;

      // Decrement movement timer
      if (mob_data[i].move_timer > 0) mob_data[i].move_timer--;

      // Use cached water surface height (avoids scanning up to 23 blocks per tick per fish)
      int ws_fish_block_x = mobBlockCoord(new_x);
      int ws_fish_block_z = mobBlockCoord(new_z);
      int fish_surface = get_water_surface_y(ws_fish_block_x, ws_fish_block_z, mob_data[i].dimension);

      // Fish must stay at least 1 block below the water surface
      if (fish_surface > 0) {
        double max_y = (double)fish_surface - 1.0; // At least 1 block under surface
        if (new_y > max_y) new_y = max_y;
      }
      // Also clamp to prevent swimming too far from spawn depth
      if (new_y < old_y - 2.0) new_y = old_y - 2.0;
    } else if (roaming) { // Passive/neutral roaming movement handling

      // Check if we need to pick a new direction or start resting
      if (mob_data[i].move_timer <= 0) {
        // Check if we were just moving (had non-zero velocity)
        if (mob_data[i].move_dx != 0 || mob_data[i].move_dz != 0) {
          // Just finished walking, start resting for 1-2 seconds
          int rest_ticks = (int)(1.0f * TICKS_PER_SECOND) + (fast_rand() % ((int)TICKS_PER_SECOND + 1));
          mob_data[i].move_timer = rest_ticks;
          mob_data[i].move_dx = 0;
          mob_data[i].move_dz = 0;
        } else {
          // Was resting, now pick a new direction to walk in
          uint32_t dir_rand = fast_rand();

          if (panic && closest_player != NULL) {
            // Panicking: RUN from the closest player
            double dx = mob_data[i].x - closest_player->x;
            double dz = mob_data[i].z - closest_player->z;
            double len = sqrt(dx * dx + dz * dz);
            if (len > 0.001) {
              // Normalize and set direction away from player - RUN at 0.2 blocks/tick
              mob_data[i].move_dx = (dx / len) * 0.2;
              mob_data[i].move_dz = (dz / len) * 0.2;
              // Calculate yaw from flee direction
              double angle = atan2(dz, dx) * RAD_TO_MOBROT;
              mob_data[i].yaw_store = (uint8_t)(((int)(angle + 0.5) - 64) & 255);
            } else {
              // Player is very close, pick random direction to RUN
              if ((dir_rand >> 2) & 1) {
                if ((dir_rand >> 1) & 1) {
                  mob_data[i].move_dx = 0.2;
                  mob_data[i].move_dz = 0;
                  mob_data[i].yaw_store = 192;
                } else {
                  mob_data[i].move_dx = -0.2;
                  mob_data[i].move_dz = 0;
                  mob_data[i].yaw_store = 64;
                }
              } else {
                if ((dir_rand >> 1) & 1) {
                  mob_data[i].move_dx = 0;
                  mob_data[i].move_dz = 0.2;
                  mob_data[i].yaw_store = 0;
                } else {
                  mob_data[i].move_dx = 0;
                  mob_data[i].move_dz = -0.2;
                  mob_data[i].yaw_store = 128;
                }
              }
            }
          } else {
            // Normal random movement
            // Move by a fraction of a block on the X or Z axis
            // Yaw is set to face in the direction of motion
            if ((dir_rand >> 2) & 1) {
              if ((dir_rand >> 1) & 1) {
                mob_data[i].move_dx = move_amount;
                mob_data[i].move_dz = 0;
                mob_data[i].yaw_store = 192;
              } else {
                mob_data[i].move_dx = -move_amount;
                mob_data[i].move_dz = 0;
                mob_data[i].yaw_store = 64;
              }
            } else {
              if ((dir_rand >> 1) & 1) {
                mob_data[i].move_dx = 0;
                mob_data[i].move_dz = move_amount;
                mob_data[i].yaw_store = 0;
              } else {
                mob_data[i].move_dx = 0;
                mob_data[i].move_dz = -move_amount;
                mob_data[i].yaw_store = 128;
              }
            }
          }

          // Walk in this direction for 2-4 seconds (40-80 ticks at 20 TPS)
          int min_ticks = (int)(2.0f * TICKS_PER_SECOND);
          int max_ticks = (int)(4.0f * TICKS_PER_SECOND);
          mob_data[i].move_timer = min_ticks + (fast_rand() % (max_ticks - min_ticks + 1));

          // If panicking, change direction extremely frequently for crazy running
          if (panic) {
            mob_data[i].move_timer = 5 + (fast_rand() % 11); // 5-15 ticks (~0.25-0.75 seconds)
          }
        }
      }

      // Continue moving in the current direction
      new_x += mob_data[i].move_dx;
      new_z += mob_data[i].move_dz;
      yaw = mob_data[i].yaw_store;  // Keep consistent yaw while walking

      // Decrement movement timer
      if (mob_data[i].move_timer > 0) mob_data[i].move_timer--;

    } else { // Hostile mob movement handling

      double move_speed = move_amount;
      double vision_range = 16.0;
      double attack_range = 3.0;

      // Zombies move 2x faster
      if (mob_data[i].type == 145) move_speed = move_amount * 2.0;

      // Zombified piglins have shorter range
      if (mob_data[i].type == 148) {
        vision_range = 10.0;
        attack_range = 2.0;
      }

      double dist_to_player = sqrt(closest_dist_double);
      double y_diff = fabs(old_y - closest_player->y);
      uint8_t has_los = 0;
      // Throttle line-of-sight checks: only every 8 ticks for mobs > 6 blocks,
      // every 4 ticks otherwise — LOS is expensive and rarely changes per tick.
      if (dist_to_player <= 16.0) {
        uint8_t los_interval = (dist_to_player > 6.0) ? 8 : 4;
        if ((server_ticks + (uint32_t)i) % los_interval == 0) {
          has_los = hasLineOfSight(
            old_x, old_y + 0.5, old_z,
            closest_player->x, closest_player->y + 1.6, closest_player->z,
            mob_data[i].dimension
          );
        } else {
          // Reuse last known LOS state.  Mob and player rarely move fast
          // enough to meaningfully change LOS in 4-8 ticks.
          has_los = (mob_data[i].data >> 5) & 1;
        }
      }

      // Store the LOS result in a data bit for reuse next tick
      if (dist_to_player <= 16.0) {
        if (has_los) mob_data[i].data |= (1 << 5);
        else mob_data[i].data &= ~(1 << 5);
      }

      // Pre-compute normalized player direction shared by all hostile AI types.
      // Every branch recomputes the same dx/dz/len/atan2 — do it once here.
      double to_player_nx = 0.0, to_player_nz = 0.0, yaw_to_player = 0.0;
      uint8_t has_player_dir = 0;
      if (dist_to_player > 0.001) {
        double dx = closest_player->x - old_x;
        double dz = closest_player->z - old_z;
        to_player_nx = dx / dist_to_player;
        to_player_nz = dz / dist_to_player;
        yaw_to_player = atan2(dz, dx) * RAD_TO_MOBROT;
        has_player_dir = 1;
      }

      // Skeleton AI: maintain distance and shoot arrows
      if (mob_data[i].type == E_SKELETON) {
        // Skeleton melee attack if cornered (very close)
        if (dist_to_player < 2.0 && y_diff < 2.0 && has_los) {
          if (mob_data[i].move_timer <= 0) {
            if (closest_player->client_fd != -1) {
              hurtEntity(closest_player->client_fd, entity_id, D_generic, 1);
            }
            mob_data[i].move_timer = 20;
          }
        }

        // Shoot arrows when player is 2-15 blocks away
        if (dist_to_player > 2.0 && dist_to_player <= 15.0 && y_diff < 4.0 && has_los) {
          if (mob_data[i].move_timer <= 0) {
            if (closest_player->client_fd != -1) {
              shootSkeletonArrow(i, closest_player->x, closest_player->y + 1.6, closest_player->z);
              broadcastMobSound(entity_id, S_SKELETON_SHOOT, SOUND_CATEGORY_HOSTILE, 1.0f, 1.0f);
            }
            mob_data[i].move_timer = 20;
          }
        }

        // Decrement attack cooldown
        if (mob_data[i].move_timer > 0) mob_data[i].move_timer--;

        // Skeletons maintain ~10 block distance from player
        if (dist_to_player <= vision_range && has_player_dir) {
          double move_dir = 1.0;
          // Back away if too close
          if (dist_to_player < 6.0) move_dir = -1.0;
          // Strafe if at good range
          if (dist_to_player >= 6.0 && dist_to_player <= 12.0) {
            // Strafe perpendicular to the player
            new_x += -to_player_nz * move_amount * 2.0;
            new_z += to_player_nx * move_amount * 2.0;
            yaw = (uint8_t)(((int)(yaw_to_player + 0.5) - 64) & 255);
          } else {
            new_x += to_player_nx * move_amount * 2.0 * move_dir;
            new_z += to_player_nz * move_amount * 2.0 * move_dir;
            yaw = (uint8_t)(((int)(yaw_to_player + 0.5) - 64) & 255);
          }
        }
      } else if (mob_data[i].type == E_CREEPER) {
        // Creeper AI: chase, fuse, explode

        // If fusing, count down the fuse
        if (mob_data[i].move_timer > 0) {
          mob_data[i].move_timer--;

          // If player moved away, cancel fuse
          if (dist_to_player > 7.0 || y_diff > 3.0) {
            mob_data[i].move_timer = 0;
            broadcastMobMetadata(-1, entity_id); // stop flashing
          } else if (mob_data[i].move_timer <= 0) {
            // Fuse complete — explode!
            // Broadcast metadata (stop flashing)
            broadcastMobMetadata(-1, entity_id);

            // Destroy blocks in a 3-block radius
            doExplosion(mob_data[i].x, mob_data[i].y, mob_data[i].z, 3.0f, mob_data[i].dimension);

            // Explosion sound
            broadcastMobSound(entity_id, S_GENERIC_EXPLODE, SOUND_CATEGORY_HOSTILE, 4.0f, 1.0f);

            // Damage all players within blast radius
            for (int p = 0; p < MAX_PLAYERS; p++) {
              if (player_data[p].client_fd == -1) continue;
              if (player_data[p].dimension != mob_data[i].dimension) continue;
              double pdx = player_data[p].x - mob_data[i].x;
              double pdz = player_data[p].z - mob_data[i].z;
              double pdist = sqrt(pdx * pdx + pdz * pdz);
              double pdy = fabs(mob_data[i].y - player_data[p].y);
              if (pdist < 4.0 && pdy < 3.0) {
                uint8_t dmg = (uint8_t)((4.0 - pdist) * 7.0);
                if (dmg < 1) dmg = 1;
                hurtEntity(player_data[p].client_fd, entity_id, D_explosion, dmg);

                // Knockback: push player away from explosion
                if (pdist > 0.01) {
                  double force = (4.0 - pdist) / 4.0;
                  double vx = (pdx / pdist) * force * 2.5;
                  double vz = (pdz / pdist) * force * 2.5;
                  double vy = force * 0.6;
                  sc_setEntityVelocity(
                    player_data[p].client_fd, player_data[p].client_fd,
                    (int16_t)(vx * 8000.0), (int16_t)(vy * 8000.0), (int16_t)(vz * 8000.0)
                  );
                }
              }
            }

            // Damage nearby mobs
            for (int mi = 0; mi < MAX_MOBS; mi++) {
              if (mi == i || mob_data[mi].type == 0) continue;
              if ((mob_data[mi].data & 31) == 0) continue;
              if (mob_data[mi].dimension != mob_data[i].dimension) continue;
              double mdx = mob_data[i].x - mob_data[mi].x;
              double mdz = mob_data[i].z - mob_data[mi].z;
              double mdist = sqrt(mdx * mdx + mdz * mdz);
              if (mdist < 3.0 && fabs(mob_data[i].y - mob_data[mi].y) < 3.0) {
                uint8_t dmg = (uint8_t)((3.0 - mdist) * 5.0);
                if (dmg < 1) dmg = 1;
                hurtEntity(-2 - mi, entity_id, D_explosion, dmg);
              }
            }

            // Spawn smoke particles
            for (int p = 0; p < MAX_PLAYERS; p++) {
              if (player_data[p].client_fd == -1) continue;
              if (player_data[p].dimension != mob_data[i].dimension) continue;
              sc_entityEvent(player_data[p].client_fd, entity_id, 60);
            }

            // Kill the creeper with explosion damage for drops
            hurtEntity(entity_id, -1, D_explosion, 99);
          }
        } else {
          // Not fusing — chase player
          if (dist_to_player <= vision_range && has_player_dir) {
            // Start fusing when close enough
            if (dist_to_player < 3.0 && y_diff < 2.0 && has_los) {
              mob_data[i].move_timer = 30;
              broadcastMobMetadata(-1, entity_id);
              broadcastMobSound(entity_id, S_CREEPER_PRIMED, SOUND_CATEGORY_HOSTILE, 1.0f, 1.0f);
            } else {
              // Chase the player
              new_x += to_player_nx * move_amount * 2.0;
              new_z += to_player_nz * move_amount * 2.0;
              yaw = (uint8_t)(((int)(yaw_to_player + 0.5) - 64) & 255);
            }
          }
        }
      } else if (mob_data[i].type == E_ENDERMAN) {
        // Enderman AI
        double enderman_speed = move_amount * 6.0;
        double enderman_vision = 64.0;

        // If we're within attack range, hurt the player
        if (dist_to_player < 3.0 && y_diff < 2.0 && has_los) {
          if (mob_data[i].move_timer <= 0) {
            if (closest_player->client_fd != -1) {
              hurtEntity(closest_player->client_fd, entity_id, D_generic, 1);
            }
            mob_data[i].move_timer = 20;
          }
        }

        // Decrement attack cooldown timer
        if (mob_data[i].move_timer > 0) mob_data[i].move_timer--;

        // Move towards the closest player if within vision range
        if (dist_to_player <= enderman_vision && has_player_dir) {
          new_x += to_player_nx * enderman_speed;
          new_z += to_player_nz * enderman_speed;
          yaw = (uint8_t)(((int)(yaw_to_player + 0.5) - 64) & 255);
        }
      } else if (mob_data[i].type == E_SPIDER) {
        // Spider AI: chase and melee attack
        double spider_speed = move_amount * 2.0;

        // If we're within attack range, hurt the player
        if (dist_to_player < 2.0 && y_diff < 2.0 && has_los) {
          if (mob_data[i].move_timer <= 0) {
            if (closest_player->client_fd != -1) {
              hurtEntity(closest_player->client_fd, entity_id, D_generic, 1);
            }
            mob_data[i].move_timer = 20;
          }
        }

        // Decrement attack cooldown timer
        if (mob_data[i].move_timer > 0) mob_data[i].move_timer--;

        // Move towards the closest player if within vision range
        if (dist_to_player <= vision_range && has_player_dir) {
          new_x += to_player_nx * spider_speed;
          new_z += to_player_nz * spider_speed;
          yaw = (uint8_t)(((int)(yaw_to_player + 0.5) - 64) & 255);
        }
      } else {
        // Standard hostile melee AI (zombies, piglins)

        // If we're within attack range, hurt the player
        if (dist_to_player < attack_range && y_diff < 2.0 && has_los) {
          if (mob_data[i].move_timer <= 0) {
            if (closest_player->client_fd != -1) {
              hurtEntity(closest_player->client_fd, entity_id, D_generic, 1);
            }
            mob_data[i].move_timer = 20;
          }
        }

        // Decrement attack cooldown timer
        if (mob_data[i].move_timer > 0) mob_data[i].move_timer--;

        // Move towards the closest player if within vision range
        if (dist_to_player <= vision_range && has_player_dir) {
          new_x += to_player_nx * move_speed;
          new_z += to_player_nz * move_speed;
          yaw = (uint8_t)(((int)(yaw_to_player + 0.5) - 64) & 255);
        }
      }

    }

    // Collision + one-block step-up.  Most ticks the mob hasn't crossed
    // a block boundary (moves <0.1 blocks/tick), so we skip the expensive
    // bounding-box cascade when staying within the same horizontal block.
    if (!is_fish) {
      new_y = old_y;
    }
    uint8_t stepped_up = false;

    // Fish can move through water blocks
    if (is_fish) {
      // Simplified collision for fish - check if the new position has water nearby
      int fish_block_x = mobBlockCoord(new_x);
      int fish_block_y = mobBlockCoord(new_y);
      int fish_block_z = mobBlockCoord(new_z);

      // Check a 3x3x2 area around the fish for water
      uint8_t has_water = 0;
      for (int dx = -1; dx <= 1; dx++) {
        for (int dy = 0; dy <= 1; dy++) {
          for (int dz = -1; dz <= 1; dz++) {
            uint16_t block = getBlockAt2(fish_block_x + dx, fish_block_y + dy, fish_block_z + dz, mob_data[i].dimension);
            if (isWaterBlock(block)) {
              has_water = 1;
              break;
            }
          }
          if (has_water) break;
        }
        if (has_water) break;
      }

      // Also check that the position isn't inside a solid block
      uint16_t fish_block = getBlockAt2(fish_block_x, fish_block_y, fish_block_z, mob_data[i].dimension);
      uint16_t fish_block_above = getBlockAt2(fish_block_x, fish_block_y + 1, fish_block_z, mob_data[i].dimension);

      if (!has_water || (!isPassableBlock(fish_block) && !isWaterBlock(fish_block))) {
        // Can't move to position without water or inside solid block
        new_x = old_x;
        new_y = old_y;
        new_z = old_z;
        if (roaming) mob_data[i].move_timer = 0;
      }
    } else {
      // Land mob: detect whether we crossed a block boundary this tick.
      // Roaming mobs move 0.05 blocks/tick so ~95% of ticks they stay in
      // the same block — skip the full bounding-box cascade when possible.
      int obx = mobBlockCoord(old_x);
      int obz = mobBlockCoord(old_z);
      int nbx = mobBlockCoord(new_x);
      int nbz = mobBlockCoord(new_z);
      uint8_t same_block = (obx == nbx && obz == nbz);

      if (!same_block ||
          !canMobOccupyPosition(new_x, old_y, new_z, mob_data[i].dimension)) {
        if (canMobStepTo(new_x, old_y + 1.0, new_z, mob_data[i].dimension)) {
          new_y = old_y + 1.0;
          stepped_up = true;
        } else {
          if (!canMobOccupyPosition(new_x, old_y, old_z, mob_data[i].dimension)) {
            if (canMobStepTo(new_x, old_y + 1.0, old_z, mob_data[i].dimension)) {
              new_y = old_y + 1.0;
              new_z = old_z;
              stepped_up = true;
            } else {
              new_x = old_x;
              if (roaming) mob_data[i].move_timer = 0;
            }
          }

          double test_y = stepped_up ? new_y : old_y;
          if (!canMobOccupyPosition(new_x, test_y, new_z, mob_data[i].dimension)) {
            if (!stepped_up && canMobStepTo(new_x, old_y + 1.0, new_z, mob_data[i].dimension)) {
              new_y = old_y + 1.0;
              stepped_up = true;
            } else {
              new_z = old_z;
              if (roaming) mob_data[i].move_timer = 0;
            }
          }
        }
      }
    }

    // Fish don't fall - they float in water
    if (!is_fish && !stepped_up &&
        !hasMobFloorBelow(new_x, new_y, new_z, mob_data[i].dimension) &&
        canMobOccupyPosition(new_x, old_y - 1.0, new_z, mob_data[i].dimension)) {
      new_y = old_y - 1.0;
    }

    int block_x = mobBlockCoord(new_x);
    int block_y = mobBlockCoord(new_y);
    int block_z = mobBlockCoord(new_z);
    uint16_t block = getBlockAt2(block_x, block_y, block_z, mob_data[i].dimension);
    uint16_t block_above = getBlockAt2(block_x, block_y + 1, block_z, mob_data[i].dimension);

    // Exit early if all movement was cancelled and not looking around
    if (mob_data[i].look_timer == 0 &&
        fabs(new_x - mob_data[i].x) < 0.001 &&
        fabs(new_z - mob_data[i].z) < 0.001 &&
        fabs(new_y - mob_data[i].y) < 0.001) continue;

    // Prevent collisions with other mobs (using spatial hash).
    // Skip if the mob hasn't moved at all (common for resting roaming mobs).
    uint8_t colliding = false;
    if (is_close_to_player) {
      if (fabs(new_x - old_x) > 0.0001 || fabs(new_z - old_z) > 0.0001 || fabs(new_y - old_y) > 0.0001) {
        colliding = mob_grid_check_collision(new_x, new_y, new_z, i, mob_data[i].dimension);
      }
    }
    if (colliding) continue;

    if ( // Hurt mobs that stumble into lava
      (block >= B_lava && block < B_lava + 4) ||
      (block_above >= B_lava && block_above < B_lava + 4)
    ) hurtEntity(entity_id, -1, D_lava, 8);

    // Footstep sounds when actually moving horizontally
    if (is_close_to_player && !is_fish &&
        mob_data[i].type != E_CREEPER &&
        mob_data[i].type != E_ENDERMAN &&
        (fabs(new_x - old_x) > 0.0001 || fabs(new_z - old_z) > 0.0001) &&
        server_ticks % 12 == 0) {
      int step_sound = -1;
      int step_cat = SOUND_CATEGORY_NEUTRAL;
      switch (mob_data[i].type) {
        case 25: step_sound = S_CHICKEN_STEP; break;
        case 28: step_sound = S_COW_STEP; break;
        case 95: step_sound = S_PIG_STEP; break;
        case 106: step_sound = S_SHEEP_STEP; break;
        case E_VILLAGER: step_sound = S_WOOD_STEP; break;
        case 145: step_sound = S_ZOMBIE_STEP; step_cat = SOUND_CATEGORY_HOSTILE; break;
        case E_SKELETON: step_sound = S_SKELETON_STEP; step_cat = SOUND_CATEGORY_HOSTILE; break;
        case E_SPIDER: step_sound = S_SPIDER_STEP; step_cat = SOUND_CATEGORY_HOSTILE; break;
        case E_PIGLIN: step_sound = S_PIGLIN_STEP; break;
        case 148: step_sound = S_ZOMBIE_STEP; step_cat = SOUND_CATEGORY_HOSTILE; break;
        default: break;
      }
      if (step_sound > 0) {
        broadcastMobSound(entity_id, step_sound, step_cat, 1.0f, (float)(fast_rand() % 10 + 95) / 100.0f);
      }
    }

    // Store new mob position
    mob_data[i].x = new_x;
    mob_data[i].y = new_y;
    mob_data[i].z = new_z;

    // Broadcast entity movement/rotation packets
    uint8_t mob_is_looking = (mob_data[i].look_timer > 0);
    if (mob_is_looking ||
        fabs(new_x - old_x) > 0.0001 ||
        fabs(new_z - old_z) > 0.0001 ||
        fabs(new_y - old_y) > 0.0001) {
      uint8_t head_yaw = mob_is_looking ? mob_data[i].look_yaw : yaw;
      for (int j = 0; j < MAX_PLAYERS; j ++) {
        if (player_data[j].client_fd == -1) continue;
        if (player_data[j].dimension != mob_data[i].dimension) continue;
        if (!isClientInPlay(player_data[j].client_fd)) continue;
        sc_teleportEntity (
          player_data[j].client_fd, entity_id,
          new_x, new_y, new_z,
          yaw * 360 / 256, 0
        );
        sc_setHeadRotation(player_data[j].client_fd, entity_id, head_yaw);
      }
    }

  }

  // Tick projectiles (arrows, ender pearls, etc.)
  for (int i = 0; i < MAX_PROJECTILES; i++) {
    ProjectileData *p = &projectile_data[i];
    if (!p->active) continue;

    uint8_t is_arrow = p->type == E_ARROW;

    if (is_arrow && p->stuck) {
      if (tryPickupStuckArrow(i, p)) continue;
      if (server_ticks - p->spawn_tick > (uint32_t)(15 * TICKS_PER_SECOND)) {
        removeProjectileFromClients(i, p->dimension);
        p->active = 0;
      }
      continue;
    }

    // Apply gravity
    p->vy -= is_arrow ? 0.05 : 0.03;

    double start_x = p->x;
    double start_y = p->y;
    double start_z = p->z;
    double target_x = p->x + p->vx;
    double target_y = p->y + p->vy;
    double target_z = p->z + p->vz;
    double speed = sqrt(p->vx * p->vx + p->vy * p->vy + p->vz * p->vz);
    int steps = (int)ceil(speed * 2.0);
    if (steps < 1) steps = 1;
    if (steps > 8) steps = 8;

    uint8_t hit_block = 0;
    int hit_entity = 0;
    double new_x = target_x;
    double new_y = target_y;
    double new_z = target_z;
    double last_passable_x = start_x;
    double last_passable_y = start_y;
    double last_passable_z = start_z;

    for (int step = 1; step <= steps; step++) {
      double t = (double)step / (double)steps;
      double sx = start_x + (target_x - start_x) * t;
      double sy = start_y + (target_y - start_y) * t;
      double sz = start_z + (target_z - start_z) * t;

      int bx = (int)floor(sx);
      int by = (int)floor(sy);
      int bz = (int)floor(sz);
      uint16_t block = getBlockAt2(bx, by, bz, p->dimension);
      if (!isPassableBlock(block)) {
        hit_block = 1;
        new_x = last_passable_x;
        new_y = last_passable_y;
        new_z = last_passable_z;
        break;
      }

      last_passable_x = sx;
      last_passable_y = sy;
      last_passable_z = sz;

      if (is_arrow && server_ticks - p->spawn_tick > 2) {
        hit_entity = findArrowHitEntity(p, sx, sy, sz);
        if (hit_entity != 0) {
          new_x = sx;
          new_y = sy;
          new_z = sz;
          break;
        }
      }
    }

    if (is_arrow && hit_entity != 0) {
      p->x = new_x;
      p->y = new_y;
      p->z = new_z;

      int attacker_id = -1;
      if (p->owner_index >= 0 && p->owner_index < MAX_PLAYERS) {
        PlayerData *owner = &player_data[p->owner_index];
        if (owner->client_fd != -1 && owner->dimension == p->dimension) attacker_id = owner->client_fd;
      }
      hurtEntity(hit_entity, attacker_id, D_arrow, p->damage == 0 ? 4 : p->damage);

      removeProjectileFromClients(i, p->dimension);
      p->active = 0;
      continue;
    }

    if (is_arrow && hit_block) {
      p->x = new_x;
      p->y = new_y;
      p->z = new_z;
      p->vx = target_x - start_x;
      p->vy = target_y - start_y;
      p->vz = target_z - start_z;
      p->stuck = 1;
      p->spawn_tick = server_ticks;
      syncStoppedArrow(i, p);
      continue;
    }

    if (hit_block) {
      // Ender pearls teleport their owner to the last safe projectile position.
      PlayerData *owner = NULL;
      if (p->owner_index >= 0 && p->owner_index < MAX_PLAYERS)
        owner = &player_data[p->owner_index];
      if (owner && owner->client_fd != -1 && owner->dimension == p->dimension) {
        owner->x = (short)p->x;
        owner->y = (int16_t)p->y;
        owner->z = (short)p->z;

        // Update center chunk so the client loads the correct area
        short _cx = div_floor(owner->x, 16);
        short _cz = div_floor(owner->z, 16);
        sc_setCenterChunk(owner->client_fd, _cx, _cz);

        // Send Synchronize Player Position to the owner (requires client confirmation)
        sc_synchronizePlayerPosition(
          owner->client_fd,
          p->x, p->y, p->z,
          owner->yaw * 360.0f / 256.0f,
          owner->pitch * 360.0f / 256.0f
        );
        // Send Teleport Entity to other players to show the owner moving
        for (int j = 0; j < MAX_PLAYERS; j++) {
          if (player_data[j].client_fd == -1) continue;
          if (player_data[j].client_fd == owner->client_fd) continue;
          sc_teleportEntity(
            player_data[j].client_fd, owner->client_fd,
            p->x, p->y, p->z,
            owner->yaw * 360.0f / 256.0f,
            owner->pitch * 360.0f / 256.0f
          );
        }
        // Apply fall damage (ender pearl damage bypasses armor)
        if (owner->health > 0) {
          hurtEntity(owner->client_fd, -1, D_ender_pearl, 2);
        }
      }

      removeProjectileFromClients(i, p->dimension);
      p->active = 0;
      continue;
    }

    // Update position
    p->x = target_x;
    p->y = target_y;
    p->z = target_z;

    // Apply drag (air resistance)
    p->vx *= 0.99;
    p->vy *= 0.99;
    p->vz *= 0.99;

    // Despawn old projectiles
    uint32_t max_age = is_arrow ? 1200 : 6000;
    if (server_ticks - p->spawn_tick > max_age) {
      removeProjectileFromClients(i, p->dimension);
      p->active = 0;
      continue;
    }

    // Broadcast position update
    float yaw = 0;
    float pitch = 0;
    if (is_arrow) projectileAngles(p->vx, p->vy, p->vz, &yaw, &pitch, NULL, NULL);

    for (int j = 0; j < MAX_PLAYERS; j++) {
      if (player_data[j].client_fd == -1) continue;
      if (player_data[j].dimension != p->dimension) continue;
      sc_teleportEntity(
        player_data[j].client_fd,
        projectileEntityId(i),
        p->x, p->y, p->z,
        yaw, pitch
      );
    }
  }

  // Tick XP orbs - movement towards players and collection
  tickXpOrbs();

  // Tick item entities - check for pickups and despawn
  tickItemEntities();

  // Spawn friendly mobs around players at configurable interval
  if (config.mob_spawn_enabled && config.mob_spawn_interval > 0 &&
      server_ticks % (uint32_t)config.mob_spawn_interval == 0) {
    if (verbose_mode)
      terminal_ui_log("[DEBUG] Mob spawn tick at server_ticks=%u, dim=%d", server_ticks, player_data[0].dimension);
    for (int i = 0; i < MAX_PLAYERS; i ++) {
      PlayerData *player = &player_data[i];
      if (player->client_fd == -1) continue;
      if (player->flags & 0x20) { if (verbose_mode) terminal_ui_log("[DEBUG]  Player[%d] still loading (flags=0x%04X)", i, player->flags); continue; }
      if (verbose_mode)
        terminal_ui_log("[DEBUG]  Calling spawnMobsAroundPlayer for player[%d] at (%.0f, %.0f, %.0f) dim=%d existing=%d",
          i, (double)player->x, (double)player->y, (double)player->z, player->dimension, countMobsNearPlayer(player));
      spawnMobsAroundPlayer(player);
    }
  }

}

void tickXpOrbs (void) {
  for (int i = 0; i < MAX_XP_ORBS; i++) {
    if (!xp_orb_data[i].active) continue;

    int entity_id = XP_ORB_ENTITY_ID_BASE - i;

    // Age the orb - despawn after 5 minutes (6000 ticks)
    xp_orb_data[i].age++;
    if (xp_orb_data[i].age > 6000) {
      xp_orb_data[i].active = 0;
      for (int j = 0; j < MAX_PLAYERS; j++) {
        if (player_data[j].client_fd == -1) continue;
        if (player_data[j].dimension != xp_orb_data[i].dimension) continue;
        sc_removeEntity(player_data[j].client_fd, entity_id);
      }
      continue;
    }

    // Find the closest player in the same dimension
    PlayerData *closest_player = NULL;
    double closest_dist_sq = 36.0; // Max pickup range (6 blocks)
    for (int j = 0; j < MAX_PLAYERS; j++) {
      if (player_data[j].client_fd == -1) continue;
      if (player_data[j].flags & 0x20) continue;
      if (player_data[j].dimension != xp_orb_data[i].dimension) continue;
      double dx = xp_orb_data[i].x - player_data[j].x;
      double dz = xp_orb_data[i].z - player_data[j].z;
      double dy = xp_orb_data[i].y - player_data[j].y;
      double dist_sq = dx * dx + dy * dy + dz * dz;
      if (dist_sq < closest_dist_sq) {
        closest_dist_sq = dist_sq;
        closest_player = &player_data[j];
      }
    }

    if (closest_player == NULL) continue;

    double dist = sqrt(closest_dist_sq);

    // If within 1.5 blocks, collect the orb
    if (dist <= 1.5) {
      // Send pickup animation to all players in range
      for (int j = 0; j < MAX_PLAYERS; j++) {
        if (player_data[j].client_fd == -1) continue;
        if (player_data[j].dimension != xp_orb_data[i].dimension) continue;
        sc_pickupItem(player_data[j].client_fd, entity_id, closest_player->client_fd, 1);
        sc_removeEntity(player_data[j].client_fd, entity_id);
      }

      // Add XP to the player
      addXpToPlayer(closest_player, xp_orb_data[i].value);

      xp_orb_data[i].active = 0;
    } else if (dist <= 6.0) {
      // Attract towards player (simulate magnetic pull)
      // Move at speed inversely proportional to distance
      double speed = 0.1 * (1.0 - dist / 6.0);
      if (speed < 0.02) speed = 0.02;
      double dx = closest_player->x - xp_orb_data[i].x;
      double dz = closest_player->z - xp_orb_data[i].z;
      double dy = (closest_player->y + 1.0) - xp_orb_data[i].y;
      double mag = sqrt(dx * dx + dy * dy + dz * dz);
      if (mag > 0.001) {
        xp_orb_data[i].x += (dx / mag) * speed;
        xp_orb_data[i].y += (dy / mag) * speed;
        xp_orb_data[i].z += (dz / mag) * speed;

        // Broadcast position update every few ticks to reduce packet spam
        if (xp_orb_data[i].age % 3 == 0) {
          for (int j = 0; j < MAX_PLAYERS; j++) {
            if (player_data[j].client_fd == -1) continue;
            if (player_data[j].dimension != xp_orb_data[i].dimension) continue;
            sc_teleportEntity(
              player_data[j].client_fd, entity_id,
              xp_orb_data[i].x, xp_orb_data[i].y, xp_orb_data[i].z,
              0, 0
            );
          }
        }
      }
    }
  }
}

// Task argument for parallel player state updates
typedef struct {
  int player_index;
  int64_t time_since_last_tick;
} PlayerUpdateTask;

// Update a single player's state ONLY (called from thread pool)
// This function does NOT send any packets - only updates player data
static void updatePlayerStateTask(void* arg) {
  PlayerUpdateTask* task = (PlayerUpdateTask*)arg;
  int i = task->player_index;
  PlayerData* player = &player_data[i];

  if (player->client_fd == -1) {
    free(task);
    return;
  }

  // Handle client loading state (no packet - will be sent in main thread)
  if (player->flags & 0x20) {
    player->flagval_16++;
    free(task);
    return;
  }

  if (player->position_lock_ticks > 0) {
    player->position_lock_ticks--;
    player->x = player->locked_x;
    player->y = player->locked_y;
    player->z = player->locked_z;
    player->grounded_y = player->locked_y;
  }

  // Handle eating animation timer (no packet - will be sent in main thread)
  if (player->flags & 0x10) {
    if (player->flagval_16 >= (uint16_t)(0.8f * TICKS_PER_SECOND)) {
      // Complete eating - update state only, packets sent separately
      uint16_t *held_item = &player->inventory_items[player->hotbar];
      uint8_t *held_count = &player->inventory_count[player->hotbar];

      uint16_t saturation = 0;
      uint8_t food = 0;
      switch (*held_item) {
        case I_chicken: food = 2; saturation = 600; break;
        case I_beef: food = 3; saturation = 900; break;
        case I_porkchop: food = 3; saturation = 300; break;
        case I_mutton: food = 2; saturation = 600; break;
        case I_cooked_chicken: food = 6; saturation = 3600; break;
        case I_cooked_beef: food = 8; saturation = 6400; break;
        case I_cooked_porkchop: food = 8; saturation = 6400; break;
        case I_cooked_mutton: food = 6; saturation = 4800; break;
        case I_rotten_flesh: food = 4; saturation = 0; break;
        case I_apple: food = 4; saturation = 1200; break;
        case I_bread: food = 5; saturation = 3000; break;
        case I_baked_potato: food = 5; saturation = 3000; break;
        case I_carrot: food = 3; saturation = 1800; break;
        case I_potato: food = 1; saturation = 300; break;
        case I_poisonous_potato: food = 2; saturation = 600; break;
        case I_golden_carrot: food = 6; saturation = 7200; break;
        case I_cooked_cod: food = 5; saturation = 3000; break;
        case I_cooked_salmon: food = 6; saturation = 4800; break;
        case I_cooked_rabbit: food = 5; saturation = 3000; break;
        case I_rabbit: food = 3; saturation = 900; break;
        case I_cod: food = 2; saturation = 200; break;
        case I_salmon: food = 2; saturation = 200; break;
        case I_tropical_fish: food = 1; saturation = 100; break;
        case I_pufferfish: food = 1; saturation = 100; break;
        case I_beetroot: food = 1; saturation = 600; break;
        case I_melon_slice: food = 2; saturation = 600; break;
        case I_pumpkin_pie: food = 8; saturation = 2400; break;
        case I_cookie: food = 2; saturation = 200; break;
        case I_mushroom_stew: food = 6; saturation = 3600; break;
        case I_beetroot_soup: food = 6; saturation = 3600; break;
        case I_rabbit_stew: food = 10; saturation = 6000; break;
        default: break;
      }

      if (food > 0 && *held_count > 0) {
        player->saturation += saturation;
        player->hunger += food;
        if (player->hunger > 20) player->hunger = 20;
        *held_count -= 1;
        if (*held_count == 0) *held_item = 0;
        // Mark that eating just completed (for packet sending in main thread)
        player->flagval_16 = 0xFFFF;
      } else {
        player->flags &= ~0x10;
        player->flagval_16 = 0;
      }
    } else {
      player->flagval_16++;
    }
  }

  // Reset movement update cooldown if not broadcasting every update
  #ifndef BROADCAST_ALL_MOVEMENT
    player->flags &= ~0x40;
  #endif

  // ---- Air supply & drowning (runs every tick) ----
  {
    uint8_t pdim = player->dimension;
    // Check if the player's head is in water (block at y+1)
    uint16_t head_block = getBlockAt2(player->x, player->y + 1, player->z, pdim);
    uint8_t head_in_water = (head_block >= B_water && head_block < B_water + 8);

    if (head_in_water) {
      // Submerged: deplete air
      if (player->air > 0) player->air--;
    } else {
      // Not submerged: replenish air
      if (player->air < 300) {
        player->air += 4;
        if (player->air > 300) player->air = 300;
      }
    }
  }

  // Process once-per-second state updates (no packet sending here)
  if (server_ticks % (uint32_t)TICKS_PER_SECOND == 0) {
    // Calculate lava damage (apply health change, packet sent separately)
    uint8_t pdim = player->dimension;
    uint16_t block = getBlockAt2(player->x, player->y, player->z, pdim);
    if (block >= B_lava && block < B_lava + 4) {
      if (player->health > 8) player->health -= 8;
      else player->health = 0;
    }
    if (!(player->flags & 0x04) && getBlockAt2(player->x, player->y - 1, player->z, pdim) == B_magma_block) {
      if (player->health > 1) player->health -= 1;
      else player->health = 0;
    }
    // Campfire damage - standing inside a campfire
    uint16_t cf_block = getBlockAt2(player->x, player->y, player->z, pdim);
    if (cf_block == B_campfire || getBlockAt2(player->x, player->y - 1, player->z, pdim) == B_campfire) {
      if (player->health > 1) player->health -= 1;
      else player->health = 0;
    }
    #ifdef ENABLE_CACTUS_DAMAGE
    // Calculate fire damage (apply health change, packet sent separately)
    if (block == B_fire) {
      if (player->health > 1) player->health -= 1;
      else player->health = 0;
    }

    // Calculate cactus damage (apply health change, packet sent separately)
    if (block == B_cactus ||
        getBlockAt2(player->x + 1, player->y, player->z, pdim) == B_cactus ||
        getBlockAt2(player->x - 1, player->y, player->z, pdim) == B_cactus ||
        getBlockAt2(player->x, player->y, player->z + 1, pdim) == B_cactus ||
        getBlockAt2(player->x, player->y, player->z - 1, pdim) == B_cactus) {
      if (player->health > 4) player->health -= 4;
      else player->health = 0;
    }
    #endif

    // Drowning damage - deals 2 damage (1 heart) per second when air is depleted
    if (player->air == 0) {
      if (player->health > 2) player->health -= 2;
      else player->health = 0;
    }

    // Starvation damage when hunger is 0 (update state only, packet sent separately)
    if (player->hunger == 0 && player->health > 0) {
      uint8_t diff = getConfiguredDifficulty();
      if (diff == 1 && player->health > 10) {
        player->health--;
      } else if (diff == 2 && player->health > 1) {
        player->health--;
      } else if (diff == 3) {
        player->health--;
      }
    }
    // Natural regeneration (update state only, packet sent separately)
  if (player->health < 20 && player->health != 0) {
    uint8_t diff = getConfiguredDifficulty();
    if (diff == 0) {
      // Peaceful: regen 1 HP per second for free
      player->health++;
    } else if (player->hunger >= 18) {
        if (player->saturation >= 600) {
          player->saturation -= 600;
          player->health++;
        } else {
          player->hunger--;
          player->health++;
        }
      }
    }
  }

  free(task);
}

// Send packets for player updates (called from main thread after parallel state updates)
static void sendPlayerUpdatePackets(int64_t time_since_last_tick) {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    PlayerData* player = &player_data[i];
    if (player->client_fd == -1) continue;
    if (player->flags & 0x20) continue;  // Skip loading players

    // Send once-per-second packets
    if (server_ticks % (uint32_t)TICKS_PER_SECOND == 0) {
      sc_keepAlive(player->client_fd);
      sc_updateTime(player->client_fd, world_time);
      sc_setHealth(player->client_fd, player->health, player->hunger, player->saturation);
      // Sync air value to client for oxygen bar display
      // Only send the air metadata entry (index 1) to avoid re-sending pose/sneak
      // state to the player themselves, which can cause a flicker on the client.
      EntityData air_data = { 1, 1, { .varint = player->air } };
      sc_setEntityMetadata(player->client_fd, player->client_fd, &air_data, 1);
    }

    // Send eating completion packets if flag is set
    if (player->flagval_16 == 0xFFFF) {
      sc_entityEvent(player->client_fd, player->client_fd, 9);
      sc_setHealth(player->client_fd, player->health, player->hunger, player->saturation);
      sc_setContainerSlot(
        player->client_fd, 0,
        serverSlotToClientSlot(0, player->hotbar),
        player->inventory_count[player->hotbar],
        player->inventory_items[player->hotbar]
      );
      broadcastPlayerEquipment(player);
      player->flags &= ~0x10;
      player->flagval_16 = 0;
    }

    // If blocking, check if the shield is still held in either hand
    if (player->flags & 0x0100) {
      if (player->inventory_items[player->hotbar] != I_shield && player->inventory_items[40] != I_shield) {
        player->flags &= ~0x0100;
        broadcastPlayerMetadata(player);
      }
    }
  }

  // Handle player join packets (after loading state completes)
  for (int i = 0; i < MAX_PLAYERS; i++) {
    PlayerData* player = &player_data[i];
    if (player->client_fd == -1) continue;
    if (!(player->flags & 0x20)) continue;  // Only process loading players

    if (player->flagval_16 > (uint16_t)(3 * TICKS_PER_SECOND)) {
      handlePlayerJoin(player);
    }
  }
}

// Parallel version of player update loop from handleServerTick
static void handlePlayerUpdatesParallel(int64_t time_since_last_tick, ThreadPool* pool) {
  if (pool == NULL) return;

  // Phase 1: Submit parallel state update tasks (no packet sending)
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (player_data[i].client_fd == -1) continue;

    PlayerUpdateTask* task = (PlayerUpdateTask*)malloc(sizeof(PlayerUpdateTask));
    if (task == NULL) continue;

    task->player_index = i;
    task->time_since_last_tick = time_since_last_tick;

    thread_pool_submit(pool, updatePlayerStateTask, task);
  }

  // Wait for all state updates to complete
  thread_pool_wait(pool);

  // Phase 2: Send packets sequentially on main thread
  sendPlayerUpdatePackets(time_since_last_tick);
}

#ifdef ALLOW_CHESTS
// Broadcasts a chest slot update to all clients who have that chest open,
// except for the client who initiated the update.
void broadcastChestUpdate (int origin_fd, int chest_idx, uint16_t item, uint8_t count, uint8_t slot) {

  for (int i = 0; i < MAX_PLAYERS; i ++) {
    if (player_data[i].client_fd == -1) continue;
    if (player_data[i].flags & 0x20) continue;
    int other_idx;
    memcpy(&other_idx, player_data[i].craft_items, sizeof(other_idx));
    if (other_idx != chest_idx) continue;
    sc_setContainerSlot(player_data[i].client_fd, 2, slot, count, item);
  }

  #ifndef DISK_SYNC_BLOCKS_ON_INTERVAL
  writeChestChangesToDisk(chest_idx, slot);
  #endif

}
#endif

ssize_t writeEntityData (int client_fd, EntityData *data) {
  writeByte(client_fd, data->index);
  writeVarInt(client_fd, data->type);

  switch (data->type) {
    case 0: // Byte
    case 8: // Boolean (written as a single byte)
      return writeByte(client_fd, data->value.byte);
    case 1: // VarInt
      writeVarInt(client_fd, data->value.varint);
      return 0;
    case 21: // Pose
      writeVarInt(client_fd, data->value.pose);
      return 0;

    default: return -1;
  }
}

// Returns the networked size of an EntityData entry
int sizeEntityData (EntityData *data) {
  int value_size;

  switch (data->type) {
    case 0: // Byte
      value_size = 1;
      break;
    case 1: // VarInt
      value_size = sizeVarInt(data->value.varint);
      break;
    case 21: // Pose
      value_size = sizeVarInt(data->value.pose);
      break;

    default: return -1;
  }

  return 1 + sizeVarInt(data->type) + value_size;
}

// Returns the networked size of an array of EntityData entries
int sizeEntityMetadata (EntityData *metadata, size_t length) {
  int total_size = 0;
  for (size_t i = 0; i < length; i ++) {
    int size = sizeEntityData(&metadata[i]);
    if (size == -1) return -1;
    total_size += size;
  }
  return total_size;
}
