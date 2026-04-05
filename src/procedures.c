#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
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
#include "procedures.h"
#include "special_block.h"
#include "thread_pool.h"

// Forward declaration for parallel player updates
static void handlePlayerUpdatesParallel(int64_t time_since_last_tick, ThreadPool* pool);

void setClientState (int client_fd, int new_state) {
  // Look for a client state with a matching file descriptor
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (client_states[i].client_fd != client_fd) continue;
    client_states[i].state = new_state;
    return;
  }
  // If the above failed, look for an unused client state slot
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (client_states[i].client_fd != -1) continue;
    client_states[i].client_fd = client_fd;
    client_states[i].state = new_state;
    client_states[i].compression_threshold = 0; // Disabled by default
    client_states[i].connection_generation++;
    return;
  }
}

int getClientState (int client_fd) {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (client_states[i].client_fd != client_fd) continue;
    return client_states[i].state;
  }
  return STATE_NONE;
}

int getClientIndex (int client_fd) {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (client_states[i].client_fd != client_fd) continue;
    return i;
  }
  return -1;
}

void setCompressionThreshold (int client_fd, int threshold) {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (client_states[i].client_fd != client_fd) continue;
    client_states[i].compression_threshold = threshold;
    return;
  }
}

int getCompressionThreshold (int client_fd) {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (client_states[i].client_fd != client_fd) continue;
    return client_states[i].compression_threshold;
  }
  return 0;
}

// Restores player data to initial state (fresh spawn)
void resetPlayerData (PlayerData *player) {
  player->health = 20;
  player->hunger = 20;
  player->saturation = 2500;
  player->x = 8;
  player->z = 8;
  player->y = 80;
  player->flags |= 0x02;
  player->grounded_y = 0;
  for (int i = 0; i < 41; i ++) {
    player->inventory_items[i] = 0;
    player->inventory_count[i] = 0;
  }
  for (int i = 0; i < 9; i ++) {
    player->craft_items[i] = 0;
    player->craft_count[i] = 0;
  }
  player->flags &= ~0x80;
}

// Assigns the given data to a player_data entry
int reservePlayerData (int client_fd, uint8_t *uuid, char *name) {

  for (int i = 0; i < MAX_PLAYERS; i ++) {
    // Found existing player entry (UUID match)
    if (memcmp(player_data[i].uuid, uuid, 16) == 0) {
      // Set network file descriptor and username
      player_data[i].client_fd = client_fd;
      memcpy(player_data[i].name, name, 16);
      // Flag player as loading
      player_data[i].flags |= 0x20;
      player_data[i].flagval_16 = 0;
      // Reset their recently visited chunk list
      for (int j = 0; j < VISITED_HISTORY; j ++) {
        player_data[i].visited_x[j] = 32767;
        player_data[i].visited_z[j] = 32767;
      }
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
      player_data[i].flags |= 0x20;
      player_data[i].flagval_16 = 0;
      memcpy(player_data[i].uuid, uuid, 16);
      memcpy(player_data[i].name, name, 16);
      resetPlayerData(&player_data[i]);
      player_data_count ++;
      return 0;
    }
  }

  return 1;

}

int getPlayerData (int client_fd, PlayerData **output) {
  for (int i = 0; i < MAX_PLAYERS; i ++) {
    if (player_data[i].client_fd == client_fd) {
      *output = &player_data[i];
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
    // Mark the player as being offline
    player_data[i].client_fd = -1;
    // Prepare leave message for broadcast
    uint8_t player_name_len = strlen(player_data[i].name);
    strcpy((char *)recv_buffer, player_data[i].name);
    strcpy((char *)recv_buffer + player_name_len, " left the game");
    // Broadcast this player's leave to all other connected clients
    for (int j = 0; j < MAX_PLAYERS; j ++) {
      if (player_data[j].client_fd == client_fd) continue;
      if (player_data[j].flags & 0x20) continue;
      // Send chat message
      sc_systemChat(player_data[j].client_fd, (char *)recv_buffer, 14 + player_name_len);
      // Remove leaving player's entity
      sc_removeEntity(player_data[j].client_fd, client_fd);
    }
    break;
  }
  // Find the client state entry and reset it
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (client_states[i].client_fd == client_fd) {
      clear_client_send_queue(client_fd);
      client_states[i].client_fd = -1;
      client_states[i].state = STATE_NONE;
      client_states[i].compression_threshold = 0;
      client_states[i].connection_generation++;
      return;
    }
  }
}

// Marks a client as connected and broadcasts their data to other players
void handlePlayerJoin (PlayerData* player) {

  // Prepare join message for broadcast
  uint8_t player_name_len = strlen(player->name);
  strcpy((char *)recv_buffer, player->name);
  strcpy((char *)recv_buffer + player_name_len, " joined the game");

  // Inform other clients (and the joining client) of the player's name and entity
  for (int i = 0; i < MAX_PLAYERS; i ++) {
    if (player_data[i].client_fd == -1) continue;
    // Find the client state for this player
    uint8_t target_in_play = 0;
    for (int k = 0; k < MAX_PLAYERS; k ++) {
      if (client_states[k].client_fd == player_data[i].client_fd) {
        target_in_play = (client_states[k].state == STATE_PLAY);
        break;
      }
    }
    if (!target_in_play) continue;  // Skip clients not yet in play state
    sc_systemChat(player_data[i].client_fd, (char *)recv_buffer, 16 + player_name_len);
    sc_playerInfoUpdateAddPlayer(player_data[i].client_fd, *player);
    if (player_data[i].client_fd != player->client_fd) {
      sc_spawnEntityPlayer(player_data[i].client_fd, *player);
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
  #ifdef _WIN32
  closesocket(*client_fd);
  printf("Disconnected client %d, cause: %d, errno: %d\n", *client_fd, cause, WSAGetLastError());
  #else
  close(*client_fd);
  printf("Disconnected client %d, cause: %d, errno: %d\n\n", *client_fd, cause, errno);
  #endif
  *client_fd = -1;
}

uint8_t serverSlotToClientSlot (int window_id, uint8_t slot) {

  if (window_id == 0) { // player inventory

    if (slot < 9) return slot + 36;
    if (slot >= 9 && slot <= 35) return slot;
    if (slot == 40) return 45;
    if (slot >= 36 && slot <= 39) return 44 - slot;
    if (slot >= 41 && slot <= 44) return slot - 40;

  } else if (window_id == 12) { // crafting table

    if (slot >= 41 && slot <= 49) return slot - 40;
    return serverSlotToClientSlot(0, slot - 1);

  } else if (window_id == 14) { // furnace

    if (slot >= 41 && slot <= 43) return slot - 41;
    return serverSlotToClientSlot(0, slot + 6);

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
  sc_setContainerSlot(player->client_fd, 0, serverSlotToClientSlot(0, slot), player->inventory_count[slot], item);

  return 0;

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
    player->flags &= ~0x02;
  } else { // Not a new player
    // Calculate spawn position from player data
    spawn_x = (float)player->x + 0.5;
    spawn_y = player->y;
    spawn_z = (float)player->z + 0.5;
    spawn_yaw = player->yaw * 180 / 127;
    spawn_pitch = player->pitch * 90 / 127;
  }

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
  // Sync client health and hunger
  sc_setHealth(player->client_fd, player->health, player->hunger, player->saturation);
  // Sync client clock time
  sc_updateTime(player->client_fd, world_time);

  #ifdef ENABLE_PLAYER_FLIGHT
  if (GAMEMODE != 1 && GAMEMODE != 3) {
    // Give the player flight (for testing)
    sc_playerAbilities(player->client_fd, 0x04);
  }
  #endif

  // Calculate player's chunk coordinates
  short _x = div_floor(player->x, 16), _z = div_floor(player->z, 16);

  // Indicate that we're about to send chunk data
  sc_setDefaultSpawnPosition(player->client_fd, 8, 80, 8);
  sc_startWaitingForChunks(player->client_fd);
  sc_setCenterChunk(player->client_fd, _x, _z);

  // Ensure the spawn chunk is available right away for initial login.
  // Movement-driven chunk loading uses non-blocking background generation.
  generate_chunk_data(_x, _z);
  sc_chunkDataAndUpdateLight(player->client_fd, _x, _z);

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

// Broadcasts a player's entity metadata (sneak/sprint state) to other players
void broadcastPlayerMetadata (PlayerData *player) {
  uint8_t sneaking = (player->flags & 0x04) != 0;
  uint8_t sprinting = (player->flags & 0x08) != 0;

  uint8_t entity_bit_mask = 0;
  if (sneaking) entity_bit_mask |= 0x02;
  if (sprinting) entity_bit_mask |= 0x08;

  int pose = 0;
  if (sneaking) pose = 5;

  EntityData metadata[] = {
    {
      0,                   // Index (Entity Bit Mask)
      0,                   // Type (Byte)
      { entity_bit_mask }, // Value
    },
    {
      6,        // Index (Pose),
      21,       // Type (Pose),
      { pose }, // Value (Standing)
    }
  };

  for (int i = 0; i < MAX_PLAYERS; i ++) {
    PlayerData* other_player = &player_data[i];
    int client_fd = other_player->client_fd;

    if (client_fd == -1) continue;
    if (client_fd == player->client_fd) continue;
    if (other_player->flags & 0x20) continue;
    // Check client state
    uint8_t target_in_play = 0;
    for (int k = 0; k < MAX_PLAYERS; k ++) {
      if (client_states[k].client_fd == client_fd) {
        target_in_play = (client_states[k].state == STATE_PLAY);
        break;
      }
    }
    if (!target_in_play) continue;

    sc_setEntityMetadata(client_fd, player->client_fd, metadata, 2);
  }
}

// Sends a mob's entity metadata to the given player.
// If client_fd is -1, broadcasts to all player
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

    default: return;
  }

  if (client_fd == -1) {
    for (int i = 0; i < MAX_PLAYERS; i ++) {
      PlayerData* player = &player_data[i];
      client_fd = player->client_fd;

      if (client_fd == -1) continue;
      if (player->flags & 0x20) continue;
      // Check client state
      uint8_t target_in_play = 0;
      for (int k = 0; k < MAX_PLAYERS; k ++) {
        if (client_states[k].client_fd == client_fd) {
          target_in_play = (client_states[k].state == STATE_PLAY);
          break;
        }
      }
      if (!target_in_play) continue;

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
  uint8_t y;
  uint8_t block;
  uint32_t epoch;
} BlockLookupCacheEntry;

static THREAD_LOCAL BlockLookupCacheEntry block_lookup_cache[BLOCK_LOOKUP_CACHE_SIZE];
static volatile uint32_t block_lookup_cache_epoch = 1;

#define MODIFIED_CHUNK_TABLE_SIZE 16384
typedef struct {
  int x;
  int z;
} ModifiedChunkEntry;

static ModifiedChunkEntry modified_chunk_table[MODIFIED_CHUNK_TABLE_SIZE];
static uint8_t modified_chunk_table_initialized = 0;

static inline uint32_t coord_hash(int x, int z) {
  uint32_t h = (uint32_t)x * 374761393u + (uint32_t)z * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}

static inline uint32_t block_lookup_hash(short x, uint8_t y, short z) {
  uint32_t h = coord_hash((int)x, (int)z);
  h ^= (uint32_t)y * 2246822519u;
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

static inline void notify_block_change_mutation(short x, short z) {
  int chunk_x = div_floor(x, 16);
  int chunk_z = div_floor(z, 16);
  mark_modified_chunk(chunk_x, chunk_z);
  invalidate_cached_chunk(chunk_x, chunk_z);
  invalidate_block_lookup_cache();
}

uint8_t getBlockChange (short x, uint8_t y, short z) {
  uint32_t epoch = block_lookup_cache_epoch;
  uint32_t cache_idx = block_lookup_hash(x, y, z) & (BLOCK_LOOKUP_CACHE_SIZE - 1);
  BlockLookupCacheEntry *cached = &block_lookup_cache[cache_idx];
  if (
    cached->epoch == epoch &&
    cached->x == x &&
    cached->y == y &&
    cached->z == z
  ) {
    return cached->block;
  }

  uint8_t block = 0xFF;
  for (int i = 0; i < block_changes_count; i ++) {
    if (block_changes[i].block == 0xFF) continue;
    if (
      block_changes[i].x == x &&
      block_changes[i].y == y &&
      block_changes[i].z == z
    ) {
      block = block_changes[i].block;
      break;
    }
    #ifdef ALLOW_DOORS
    // Skip door upper half and state data
    if (is_door_block(block_changes[i].block)) {
      // Check if upper half matches before skipping
      if (i + 1 < block_changes_count &&
          block_changes[i + 1].x == x && block_changes[i + 1].y == y && block_changes[i + 1].z == z) {
        block = block_changes[i + 1].block;
        break;
      }
      i += 2;
      continue;
    }
    #endif
    // Skip stair and furnace state data
    if (is_stair_block(block_changes[i].block) || block_changes[i].block == B_furnace) {
      i += 1;
    }
    // Skip chest inventory entries
    if (block_changes[i].block == B_chest) {
      i += 14;
    }
  }

  cached->x = x;
  cached->y = y;
  cached->z = z;
  cached->block = block;
  cached->epoch = epoch;

  return block;
}

// Handle running out of memory for new block changes
void failBlockChange (short x, uint8_t y, short z, uint8_t block) {

  // Get previous block at this location
  uint8_t before = getBlockAt(x, y, z);

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

uint8_t makeBlockChange (short x, uint8_t y, short z, uint8_t block) {

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

  uint8_t is_base_block = block == getTerrainAt(x, y, z, anchor);

  // In the block_changes array, 0xFF indicates a missing/restored entry.
  // We track the position of the first such "gap" for when the operation
  // isn't replacing an existing block change.
  int first_gap = block_changes_count;

  // Helper macro to clear old special block entries at a position
  #define CLEAR_OLD_SPECIAL_ENTRIES(idx) \
    do { \
      if (block_changes[idx].block == B_chest) { \
        for (int j = 1; j < 15; j++) block_changes[(idx) + j].block = 0xFF; \
      } else if (is_door_block(block_changes[idx].block)) { \
        if ((idx) + 1 < block_changes_count) block_changes[(idx) + 1].block = 0xFF; \
        if ((idx) + 2 < block_changes_count) block_changes[(idx) + 2].block = 0xFF; \
      } else if (is_stair_block(block_changes[idx].block) || block_changes[idx].block == B_furnace) { \
        if ((idx) + 1 < block_changes_count) block_changes[(idx) + 1].block = 0xFF; \
      } \
    } while (0)

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
      block_changes[i].z == z
    ) {
      // Clear any old special block state entries
      CLEAR_OLD_SPECIAL_ENTRIES(i);

      if (is_base_block) {
        block_changes[i].block = 0xFF;
        special_block_clear(x, y, z);
      } else {
        block_changes[i].block = block;
        // Initialize default state for special blocks
        if (is_door_block(block)) {
          special_block_set_state(x, y, z, block, door_encode_state(0, 0, 0));
        } else if (is_stair_block(block)) {
          special_block_set_state(x, y, z, block, stair_encode_state(0, 0));
        } else if (block == B_furnace) {
          special_block_set_state(x, y, z, block, furnace_encode_state(0, 0));
        } else if (block == B_chest) {
          special_block_set_state(x, y, z, block, oriented_encode_state(0));
        }
      }
      #ifndef DISK_SYNC_BLOCKS_ON_INTERVAL
      writeBlockChangesToDisk(i, i);
      #endif
      notify_block_change_mutation(x, z);
      return 0;
    }
    // Skip extra entries for special blocks during search
    if (block_changes[i].block == B_chest) { i += 14; continue; }
    #ifdef ALLOW_DOORS
    if (is_door_block(block_changes[i].block)) { i += 2; continue; }
    #endif
    if (is_stair_block(block_changes[i].block) || block_changes[i].block == B_furnace) { i += 1; }
  }

  // Don't create a new entry if it contains the base terrain block
  if (is_base_block) return 0;

  // Determine how many block_changes entries this block needs
  int slots_needed = 1;
  if (block == B_chest) slots_needed = 15;
  #ifdef ALLOW_DOORS
  else if (is_door_block(block)) slots_needed = 3;
  #endif
  else if (is_stair_block(block) || block == B_furnace) slots_needed = 2;

  // Find or create a gap of sufficient size
  int last_real_entry = first_gap - 1;
  for (int i = first_gap; i <= block_changes_count + slots_needed; i ++) {
    #ifdef INFINITE_BLOCK_CHANGES
    if (i >= block_changes_capacity - slots_needed) {
      int new_capacity = block_changes_capacity * 2;
      BlockChange *new_block_changes = (BlockChange *)realloc(block_changes, new_capacity * sizeof(BlockChange));
      if (!new_block_changes) { failBlockChange(x, y, z, block); return 1; }
      block_changes = new_block_changes;
      for (int j = block_changes_capacity; j < new_capacity; j ++) block_changes[j].block = 0xFF;
      printf("Block changes expanded: %d -> %d\n", block_changes_capacity, new_capacity);
      fflush(stdout);
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

    if (block == B_chest) {
      // Zero out the following 14 entries for item data
      for (int j = 1; j < 15; j ++) {
        block_changes[base + j].x = 0;
        block_changes[base + j].y = 0;
        block_changes[base + j].z = 0;
        block_changes[base + j].block = 0;
      }
      special_block_set_state(x, y, z, block, oriented_encode_state(0));
    }
    #ifdef ALLOW_DOORS
    else if (is_door_block(block)) {
      // Upper half
      block_changes[base + 1].x = x;
      block_changes[base + 1].y = y + 1;
      block_changes[base + 1].z = z;
      block_changes[base + 1].block = block;
      // State entry
      block_changes[base + 2].x = 0;
      block_changes[base + 2].y = 0;
      block_changes[base + 2].z = z;
      block_changes[base + 2].block = 0;
      special_block_set_state(x, y, z, block, door_encode_state(0, 0, 0));
    }
    #endif
    else if (is_stair_block(block) || block == B_furnace) {
      // State entry
      block_changes[base + 1].x = 0;
      block_changes[base + 1].y = 0;
      block_changes[base + 1].z = z;
      block_changes[base + 1].block = 0;
      if (is_stair_block(block)) {
        special_block_set_state(x, y, z, block, stair_encode_state(0, 0));
      } else {
        special_block_set_state(x, y, z, block, furnace_encode_state(0, 0));
      }
    }

    if (i >= block_changes_count) block_changes_count = i + 1;
    #ifndef DISK_SYNC_BLOCKS_ON_INTERVAL
    writeBlockChangesToDisk(base, base + slots_needed - 1);
    #endif
    notify_block_change_mutation(x, z);
    return 0;
  }

  // If we're here, no changes were made
  failBlockChange(x, y, z, block);
  return 1;
}

// Returns the result of mining a block, taking into account the block type and tools
// Probability numbers obtained with this formula: N = floor(P * (2 ^ 32))
uint16_t getMiningResult (uint16_t held_item, uint8_t block) {

  switch (block) {

    case B_oak_leaves:
      if (held_item == I_shears) return I_oak_leaves;
      uint32_t r = fast_rand();
      if (r < 21474836) return I_apple; // 0.5%
      if (r < 85899345) return I_stick; // 2%
      if (r < 214748364) return I_oak_sapling; // 5%
      return 0;
      break;

    case B_stone:
    case B_cobblestone:
    case B_sandstone:
    case B_furnace:
    case B_coal_ore:
    case B_iron_ore:
    case B_iron_block:
    case B_gold_block:
    case B_diamond_block:
    case B_redstone_block:
    case B_coal_block:
      // Check if player is holding (any) pickaxe
      if (
        held_item != I_wooden_pickaxe &&
        held_item != I_stone_pickaxe &&
        held_item != I_iron_pickaxe &&
        held_item != I_golden_pickaxe &&
        held_item != I_diamond_pickaxe &&
        held_item != I_netherite_pickaxe
      ) return 0;
      break;

    case B_gold_ore:
    case B_redstone_ore:
    case B_diamond_ore:
      // Check if player is holding an iron (or better) pickaxe
      if (
        held_item != I_iron_pickaxe &&
        held_item != I_golden_pickaxe &&
        held_item != I_diamond_pickaxe &&
        held_item != I_netherite_pickaxe
      ) return 0;
      break;

    case B_snow:
      // Check if player is holding (any) shovel
      if (
        held_item != I_wooden_shovel &&
        held_item != I_stone_shovel &&
        held_item != I_iron_shovel &&
        held_item != I_golden_shovel &&
        held_item != I_diamond_shovel &&
        held_item != I_netherite_shovel
      ) return 0;
      break;

    default: break;
  }

  return B_to_I[block];

}


// Checks whether the given block would be mined instantly with the held tool
uint8_t isInstantlyMined (PlayerData *player, uint8_t block) {

  uint16_t held_item = player->inventory_items[player->hotbar];

  if (
    block == B_snow ||
    block == B_snow_block
  ) return (
    held_item == I_stone_shovel ||
    held_item == I_iron_shovel ||
    held_item == I_diamond_shovel ||
    held_item == I_netherite_shovel ||
    held_item == I_golden_shovel
  );

  if (block == B_oak_leaves)
    return held_item == I_shears;

  return (
    block == B_dead_bush ||
    block == B_bush ||
    block == B_fern ||
    block == B_short_grass ||
    block == B_short_dry_grass ||
    block == B_tall_dry_grass ||
    block == B_oak_sapling ||
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

// Checks whether the given block has to have something beneath it
uint8_t isColumnBlock (uint8_t block) {
  return (
    block == B_snow ||
    block == B_moss_carpet ||
    block == B_cactus ||
    block == B_short_grass ||
    block == B_short_dry_grass ||
    block == B_tall_dry_grass ||
    block == B_fern ||
    block == B_dead_bush ||
    block == B_bush ||
    block == B_torch ||
    block == B_oak_sapling ||
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
uint8_t isPassableBlock (uint8_t block) {
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
    block == B_dead_bush ||
    block == B_bush ||
    block == B_torch ||
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
// Checks whether the given block is non-solid and spawnable
uint8_t isPassableSpawnBlock (uint8_t block) {
    if ((block >= B_water && block < B_water + 8) ||
        (block >= B_lava && block < B_lava + 4))
    {
        return 0;
    }
    return isPassableBlock(block);
}

// Checks whether the given block can be replaced by another block
uint8_t isReplaceableBlock (uint8_t block) {
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

uint8_t isReplaceableFluid (uint8_t block, uint8_t level, uint8_t fluid) {
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
    item == I_oak_leaves ||
    item == I_short_grass ||
    item == I_wheat_seeds ||
    item == I_oak_sapling ||
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
    // Shears
    item == I_shears
  ) return 1;

  if (
    item == I_snowball
  ) return 16;

  return 64;
}

#ifdef ALLOW_DOORS
/* Legacy wrappers for compatibility with existing code that calls these.
 * All actual logic is in special_block.c */
uint8_t isDoorBlock (uint8_t block) { return is_door_block(block); }
uint8_t isStairBlock (uint8_t block) { return is_stair_block(block); }
uint8_t isOrientedBlock (uint8_t block) { return is_oriented_block(block); }
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
uint16_t getDoorItemFromBlock (uint8_t block) {
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
uint8_t isDoorOpen (short x, uint8_t y, short z) {
  return is_door_open_at(x, y, z);
}

/* Legacy wrappers for network state ID computation */
uint16_t getDoorStateId (uint8_t block, uint8_t is_upper, uint8_t open, uint8_t direction, uint8_t hinge) {
  return get_door_state_id(block, is_upper, open, direction, hinge);
}

// Send door block update with proper state
void sendDoorUpdate (int client_fd, short x, uint8_t y, short z, uint8_t block, uint8_t is_upper, uint8_t open, uint8_t direction, uint8_t hinge) {
  startPacket(client_fd, 0x08);
  writeUint64(client_fd, (((int64_t)x & 0x3FFFFFF) << 38) | (((int64_t)z & 0x3FFFFFF) << 12) | (y & 0xFFF));
  writeVarInt(client_fd, get_door_state_id(block, is_upper, open, direction, hinge));
  endPacket(client_fd);
}

// Get oriented block state ID for network packet
uint16_t getOrientedStateId (uint8_t block, uint8_t direction) {
  return get_oriented_state_id(block, direction);
}

// Send oriented block update with proper state
void sendOrientedUpdate (int client_fd, short x, uint8_t y, short z, uint8_t block, uint8_t direction) {
  startPacket(client_fd, 0x08);
  writeUint64(client_fd, (((int64_t)x & 0x3FFFFFF) << 38) | (((int64_t)z & 0x3FFFFFF) << 12) | (y & 0xFFF));
  writeVarInt(client_fd, get_oriented_state_id(block, direction));
  endPacket(client_fd);
}

// Get stair state ID for network packet
uint16_t getStairStateId (uint8_t block, uint8_t half, uint8_t direction) {
  return get_stair_state_id(block, half, direction);
}

// Send stair block update with proper state
void sendStairUpdate (int client_fd, short x, uint8_t y, short z, uint8_t block, uint8_t half, uint8_t direction) {
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

  return true;
}

void handleFluidMovement (short x, uint8_t y, short z, uint8_t fluid, uint8_t block) {

  // Get fluid level (0-7)
  // The terminology here is a bit different from vanilla:
  // a higher fluid "level" means the fluid has traveled farther
  uint8_t level = block - fluid;

  // Query blocks adjacent to this fluid stream
  uint8_t adjacent[4] = {
    getBlockAt(x + 1, y, z),
    getBlockAt(x - 1, y, z),
    getBlockAt(x, y, z + 1),
    getBlockAt(x, y, z - 1)
  };

  // Handle maintaining connections to a fluid source
  if (level != 0) {
    // Check if this fluid is connected to a block exactly one level lower
    uint8_t connected = false;
    for (int i = 0; i < 4; i ++) {
      if (adjacent[i] == block - 1) {
        connected = true;
        break;
      }
    }
    // If not connected, clear this block and recalculate surrounding flow
    if (!connected) {
      makeBlockChange(x, y, z, B_air);
      checkFluidUpdate(x + 1, y, z, adjacent[0]);
      checkFluidUpdate(x - 1, y, z, adjacent[1]);
      checkFluidUpdate(x, y, z + 1, adjacent[2]);
      checkFluidUpdate(x, y, z - 1, adjacent[3]);
      return;
    }
  }

  // Check if water should flow down, prioritize that over lateral flow
  uint8_t block_below = getBlockAt(x, y - 1, z);
  if (isReplaceableBlock(block_below)) {
    makeBlockChange(x, y - 1, z, fluid);
    return handleFluidMovement(x, y - 1, z, fluid, fluid);
  }

  // Stop flowing laterally at the maximum level
  if (level == 3 && fluid == B_lava) return;
  if (level == 7) return;

  // Handle lateral water flow, increasing level by 1
  if (isReplaceableFluid(adjacent[0], level, fluid)) {
    makeBlockChange(x + 1, y, z, block + 1);
    handleFluidMovement(x + 1, y, z, fluid, block + 1);
  }
  if (isReplaceableFluid(adjacent[1], level, fluid)) {
    makeBlockChange(x - 1, y, z, block + 1);
    handleFluidMovement(x - 1, y, z, fluid, block + 1);
  }
  if (isReplaceableFluid(adjacent[2], level, fluid)) {
    makeBlockChange(x, y, z + 1, block + 1);
    handleFluidMovement(x, y, z + 1, fluid, block + 1);
  }
  if (isReplaceableFluid(adjacent[3], level, fluid)) {
    makeBlockChange(x, y, z - 1, block + 1);
    handleFluidMovement(x, y, z - 1, fluid, block + 1);
  }

}

void checkFluidUpdate (short x, uint8_t y, short z, uint8_t block) {

  uint8_t fluid;
  if (block >= B_water && block < B_water + 8) fluid = B_water;
  else if (block >= B_lava && block < B_lava + 4) fluid = B_lava;
  else return;

  handleFluidMovement(x, y, z, fluid, block);

}

#ifdef ENABLE_PICKUP_ANIMATION
// Plays the item pickup animation with the given item at the given coordinates
void playPickupAnimation (PlayerData *player, uint16_t item, double x, double y, double z) {

  // Spawn a new item entity at the input coordinates
  // ID -1 is safe, as elsewhere it's reserved as a placeholder
  // The player's name is used as the UUID as it's cheap and unique enough
  sc_spawnEntity(player->client_fd, -1, (uint8_t *)player->name, 69, x + 0.5, y + 0.5, z + 0.5, 0, 0);

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

void handlePlayerAction (PlayerData *player, int action, short x, short y, short z) {

  // Re-sync slot when player drops an item
  if (action == 3 || action == 4) {
    sc_setContainerSlot(
      player->client_fd, 0,
      serverSlotToClientSlot(0, player->hotbar),
      player->inventory_count[player->hotbar],
      player->inventory_items[player->hotbar]
    );
    return;
  }

  // "Finish eating" action, called any time eating stops
  if (action == 5) {
    // Reset eating timer and clear eating flag
    player->flagval_16 = 0;
    player->flags &= ~0x10;
  }

  // Ignore further actions not pertaining to mining blocks
  if (action != 0 && action != 2) return;

  // In creative, only the "start mining" action is sent
  // No additional verification is performed, the block is simply removed
  if (action == 0 && GAMEMODE == 1) {
    makeBlockChange(x, y, z, 0);
    return;
  }

  uint8_t block = getBlockAt(x, y, z);

  // If this is a "start mining" packet, the block must be instamine
  if (action == 0 && !isInstantlyMined(player, block)) return;

  // Don't continue if the block change failed
  if (makeBlockChange(x, y, z, 0)) return;

  uint16_t held_item = player->inventory_items[player->hotbar];
  uint16_t item = getMiningResult(held_item, block);

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
      makeBlockChange(x, y - 1, z, 0);  // Break lower half
    } else {
      makeBlockChange(x, y + 1, z, 0);  // Break upper half
    }

    // Clear door state from the unified special block table
    special_block_clear(door_x, door_y, door_z);
    // Give door item (only once for the whole door)
    if (item) {
      #ifdef ENABLE_PICKUP_ANIMATION
      playPickupAnimation(player, item, x, y, z);
      #endif
      givePlayerItem(player, item, 1);
    }
    return;
  }
  #endif

  if (item) {
    #ifdef ENABLE_PICKUP_ANIMATION
    playPickupAnimation(player, item, x, y, z);
    #endif
    givePlayerItem(player, item, 1);
  }

  // Update nearby fluids
  uint8_t block_above = getBlockAt(x, y + 1, z);
  #ifdef DO_FLUID_FLOW
    checkFluidUpdate(x, y + 1, z, block_above);
    checkFluidUpdate(x - 1, y, z, getBlockAt(x - 1, y, z));
    checkFluidUpdate(x + 1, y, z, getBlockAt(x + 1, y, z));
    checkFluidUpdate(x, y, z - 1, getBlockAt(x, y, z - 1));
    checkFluidUpdate(x, y, z + 1, getBlockAt(x, y, z + 1));
  #endif

  // Check if any blocks above this should break, and if so,
  // iterate upward over all blocks in the column and break them
  uint8_t y_offset = 1;
  while (isColumnBlock(block_above)) {
    // Destroy the next block
    makeBlockChange(x, y + y_offset, z, 0);
    // Check for item drops *without a tool*
    uint16_t item = getMiningResult(0, block_above);
    if (item) givePlayerItem(player, item, 1);
    // Select the next block in the column
    y_offset ++;
    block_above = getBlockAt(x, y + y_offset, z);
  }
}

void handlePlayerUseItem (PlayerData *player, short x, short y, short z, uint8_t face) {

  // Get targeted block (if coordinates are provided)
  uint8_t target = face == 255 ? 0 : getBlockAt(x, y, z);
  // Get held item properties
  uint8_t *count = &player->inventory_count[player->hotbar];
  uint16_t *item = &player->inventory_items[player->hotbar];

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
        return;
      }
    }
    #ifdef ALLOW_CHESTS
    else if (target == B_chest) {
      // Get a pointer to the entry following this chest in block_changes
      uint8_t *storage_ptr = NULL;
      for (int i = 0; i < block_changes_count; i ++) {
        if (block_changes[i].block != B_chest) continue;
        if (block_changes[i].x != x || block_changes[i].y != y || block_changes[i].z != z) continue;
        storage_ptr = (uint8_t *)(&block_changes[i + 1]);
        break;
      }
      if (storage_ptr == NULL) return;
      // Terrible memory hack!!
      // Copy the pointer into the player's crafting table item array.
      // This allows us to save some memory by repurposing a feature that
      // is mutually exclusive with chests, though it is otherwise a
      // terrible idea for obvious reasons.
      memcpy(player->craft_items, &storage_ptr, sizeof(storage_ptr));
      // Flag craft_items as locked due to holding a pointer
      player->flags |= 0x80;
      // Show the player the chest UI
      sc_openScreen(player->client_fd, 2, "Chest", 5);
      // Load the slots of the chest from the block_changes array.
      // This is a similarly dubious memcpy hack, but at least we're not
      // mixing data types? Kind of?
      for (int i = 0; i < 27; i ++) {
        uint16_t item;
        uint8_t count;
        memcpy(&item, storage_ptr + i * 3, 2);
        memcpy(&count, storage_ptr + i * 3 + 2, 1);
        sc_setContainerSlot(player->client_fd, 2, i, count, item);
      }
      return;
    }
    #endif
    #ifdef ALLOW_DOORS
    else if (isDoorBlock(target)) {
      // Use the unified special block system to toggle the door
      // Door state is always stored at the lower half position
      short door_x = x;
      uint8_t door_y = y;
      short door_z = z;

      // Check if this is the upper half by looking for a door at y-1
      uint8_t block_below = getBlockChange(x, y - 1, z);
      if (isDoorBlock(block_below)) {
        // This is the upper half, use lower half's position for state
        door_y = y - 1;
      }

      uint16_t state = special_block_get_state(door_x, door_y, door_z);
      uint8_t open = door_get_open(state);
      uint8_t hinge = door_get_hinge(state);
      uint8_t direction = door_get_direction(state);

      // Toggle open/closed
      open ^= 1;
      state = door_encode_state(open, hinge, direction);
      special_block_set_state(door_x, door_y, door_z, target, state);

      // Broadcast door update to all players
      for (int j = 0; j < MAX_PLAYERS; j++) {
        if (player_data[j].client_fd == -1) continue;
        if (player_data[j].flags & 0x20) continue;
        sendDoorUpdate(player_data[j].client_fd, door_x, door_y, door_z, target, 0, open, direction, hinge);
        uint8_t above = getBlockChange(door_x, door_y + 1, door_z);
        if (isDoorBlock(above)) {
          sendDoorUpdate(player_data[j].client_fd, door_x, door_y + 1, door_z, above, 1, open, direction, hinge);
        }
      }
      return;
    }
    #endif
  }

  // If the selected slot doesn't hold any items, exit
  if (*count == 0) return;

  // Check special item handling
  if (*item == I_bone_meal) {
    uint8_t target_below = getBlockAt(x, y - 1, z);
    if (target == B_oak_sapling) {
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
        if ((fast_rand() & 3) == 0) placeTreeStructure(x, y, z);
      }
    }
  } else if (handlePlayerEating(player, true)) {
    // Reset eating timer and set eating flag
    player->flagval_16 = 0;
    player->flags |= 0x10;
  } else if (getItemDefensePoints(*item) != 0) {
    // For some reason, this action is sent twice when looking at a block
    // Ignore the variant that has coordinates
    if (face != 255) return;
    // Swap to held piece of armor
    uint8_t slot = getArmorItemSlot(*item);
    uint16_t prev_item = player->inventory_items[slot];
    player->inventory_items[slot] = *item;
    player->inventory_count[slot] = 1;
    player->inventory_items[player->hotbar] = prev_item;
    player->inventory_count[player->hotbar] = 1;
    // Update client inventory
    sc_setContainerSlot(player->client_fd, -2, serverSlotToClientSlot(0, slot), 1, *item);
    sc_setContainerSlot(player->client_fd, -2, serverSlotToClientSlot(0, player->hotbar), 1, prev_item);
    return;
  }

  // Don't proceed with block placement if no coordinates were provided
  if (face == 255) return;

  // If the selected item doesn't correspond to a block, exit
  uint8_t block = I_to_B(*item);
  if (block == 0) return;

  // Special handling for door placement
  #ifdef ALLOW_DOORS
  if (isDoorBlock(block)) {
    // Check if there's space above for the upper half (need 2 blocks of clearance)
    uint8_t block_above = getBlockAt(x, y + 1, z);
    uint8_t block_above_2 = getBlockAt(x, y + 2, z);

    // Need both y+1 and y+2 to be replaceable (and not doors)
    if (!isReplaceableBlock(block_above) || isDoorBlock(block_above)) {
      return;  // Can't place door if upper half position is blocked
    }
    if (!isReplaceableBlock(block_above_2) || isDoorBlock(block_above_2)) {
      return;  // Can't place door if there's a block above the upper half
    }
    // Check that we're not replacing an existing door
    uint8_t existing = getBlockAt(x, y, z);
    if (isDoorBlock(existing)) {
      return;
    }
    // Check that there's no door above (at y+2) - prevents placing under doors
    if (isDoorBlock(block_above_2)) {
      return;  // Can't place door underneath another door
    }
    // Doors need a solid block below
    uint8_t block_below = getBlockAt(x, y - 1, z);
    if (isReplaceableBlock(block_below) && block_below != block) {
      // Adjust placement position
      y -= 1;
      block_below = getBlockAt(x, y - 1, z);
      if (isReplaceableBlock(block_below)) {
        return;  // Can't place door in mid-air
      }
      // Re-check all conditions at adjusted position
      block_above = getBlockAt(x, y + 1, z);
      block_above_2 = getBlockAt(x, y + 2, z);
      if (!isReplaceableBlock(block_above) || isDoorBlock(block_above)) {
        return;
      }
      if (!isReplaceableBlock(block_above_2) || isDoorBlock(block_above_2)) {
        return;
      }
      existing = getBlockAt(x, y, z);
      if (isDoorBlock(existing)) {
        return;
      }
      if (isDoorBlock(block_above_2)) {
        return;
      }
    }

    // Calculate door direction based on placement context
    // Direction: 0=north, 1=east, 2=south, 3=west
    // This represents which way the door "faces" (the side the hinge is NOT on)
    uint8_t direction;

    if (face == 0 || face == 1) {
      // Clicked on bottom or top face - use player facing
      // Player yaw: -128 to 127, where 0 is South, -64 is East, 64 is West, 128/-128 is North
      if (player->yaw >= -64 && player->yaw < 0) direction = 0;      // Facing North
      else if (player->yaw >= 0 && player->yaw < 64) direction = 1;  // Facing East
      else if (player->yaw >= 64 || player->yaw < -96) direction = 2; // Facing South
      else direction = 3;                                             // Facing West
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
      isReplaceableBlock(getBlockAt(x, y, z))
    ) {
      // Apply server-side block change (this will place both halves and set default state)
      if (makeBlockChange(x, y, z, block)) return;

      // Update the door state with the correct direction in the unified special block table
      uint16_t state = door_encode_state(0, 0, direction);  // closed, hinge left, direction
      special_block_set_state(x, y, z, block, state);

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
      return;
    }
  }
  #endif

  // Special handling for oriented block placement (chests, furnaces)
  if (isOrientedBlock(block)) {
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
    uint8_t direction;
    if (player->yaw >= -96 && player->yaw < -32) direction = 3;   // East
    else if (player->yaw >= -32 && player->yaw < 32) direction = 1; // South
    else if (player->yaw >= 32 && player->yaw < 96) direction = 2;  // West
    else direction = 0;                                             // North

    // Chests and furnaces face TOWARDS the player
    direction ^= 1;

    // Check if the block's placement conditions are met
    if (
      !( // Is player in the way?
        x == player->x &&
        (y == player->y || y == player->y + 1) &&
        z == player->z
      ) &&
      isReplaceableBlock(getBlockAt(x, y, z))
    ) {
      // Apply server-side block change
      if (makeBlockChange(x, y, z, block)) return;

      // Update state with direction in the unified special block table
      uint16_t state = oriented_encode_state(direction);
      special_block_set_state(x, y, z, block, state);

      // Send oriented updates to all players
      for (int j = 0; j < MAX_PLAYERS; j++) {
        if (player_data[j].client_fd == -1) continue;
        if (player_data[j].flags & 0x20) continue;
        sendOrientedUpdate(player_data[j].client_fd, x, y, z, block, direction);
      }

      // Decrease item amount in selected slot
      *count -= 1;
      if (*count == 0) *item = 0;
      sc_setContainerSlot(player->client_fd, 0, serverSlotToClientSlot(0, player->hotbar), *count, *item);
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
      isReplaceableBlock(getBlockAt(x, y, z))
    ) {
      // Apply server-side block change
      if (makeBlockChange(x, y, z, block)) return;

      // Update stair state in the unified special block table
      uint16_t state = stair_encode_state(half, direction);
      special_block_set_state(x, y, z, block, state);

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
      return;
    }
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
    isReplaceableBlock(getBlockAt(x, y, z)) &&
    (!isColumnBlock(block) || getBlockAt(x, y - 1, z) != B_air)
  ) {
    // Apply server-side block change
    if (makeBlockChange(x, y, z, block)) return;
    // Decrease item amount in selected slot
    *count -= 1;
    // Clear item id in slot if amount is zero
    if (*count == 0) *item = 0;
    // Calculate fluid flow
    #ifdef DO_FLUID_FLOW
      checkFluidUpdate(x, y + 1, z, getBlockAt(x, y + 1, z));
      checkFluidUpdate(x - 1, y, z, getBlockAt(x - 1, y, z));
      checkFluidUpdate(x + 1, y, z, getBlockAt(x + 1, y, z));
      checkFluidUpdate(x, y, z - 1, getBlockAt(x, y, z - 1));
      checkFluidUpdate(x, y, z + 1, getBlockAt(x, y, z + 1));
    #endif
  }

  // Sync hotbar contents to player
  sc_setContainerSlot(player->client_fd, 0, serverSlotToClientSlot(0, player->hotbar), *count, *item);

}

void spawnMob (uint8_t type, short x, uint8_t y, short z, uint8_t health) {

  for (int i = 0; i < MAX_MOBS; i ++) {
    // Look for type 0 (unallocated)
    if (mob_data[i].type != 0) continue;

    // Assign it the input parameters with sub-block centering
    mob_data[i].type = type;
    mob_data[i].x = (double)x + 0.5;
    mob_data[i].y = (double)y;
    mob_data[i].z = (double)z + 0.5;
    mob_data[i].data = health & 31;

    // Forge a UUID from a random number and the mob's index
    uint8_t uuid[16];
    uint32_t r = fast_rand();
    memcpy(uuid, &r, 4);
    memcpy(uuid + 4, &i, 4);

    // Broadcast entity creation to all players
    for (int j = 0; j < MAX_PLAYERS; j ++) {
      if (player_data[j].client_fd == -1) continue;
      sc_spawnEntity(
        player_data[j].client_fd,
        -2 - i, // Use negative IDs to avoid conflicts with player IDs
        uuid, // Use the UUID generated above
        type, mob_data[i].x, mob_data[i].y, mob_data[i].z,
        // Face opposite of the player, as if looking at them when spawning
        (player_data[j].yaw + 127) & 255, 0
      );
    }

    // Freshly spawned mobs currently don't need metadata updates.
    // If this changes, uncomment this line.
    // broadcastMobMetadata(-1, i);

    break;
  }

}

// Count how many mobs are near a player (within configured spawn range)
static uint8_t countMobsNearPlayer (PlayerData *player) {
  uint8_t count = 0;
  int range = config.mob_spawn_range;
  for (int i = 0; i < MAX_MOBS; i ++) {
    if (mob_data[i].type == 0) continue;
    if ((mob_data[i].data & 31) == 0) continue; // Skip dead mobs
    double dx = fabs(mob_data[i].x - player->x);
    double dz = fabs(mob_data[i].z - player->z);
    uint16_t dist = (uint16_t)(dx + dz);
    if (dist <= (unsigned int)range) count ++;
  }
  return count;
}

// Spawn friendly mobs behind the player on grass/snow blocks
static void spawnMobsAroundPlayer (PlayerData *player) {

  // Passive mob types: Chicken(25), Cow(28), Pig(95), Sheep(106)
  static const uint8_t passive_types[] = { 25, 28, 95, 106 };
  static const uint8_t num_passive_types = 4;

  int max_mobs = config.mob_spawn_max_per_player;
  int spawn_range = config.mob_spawn_range;
  int min_dist = config.mob_spawn_min_distance;

  // Count existing mobs near this player
  uint8_t existing = countMobsNearPlayer(player);
  if ((int)existing >= max_mobs) return;

  uint8_t slots_left = (uint8_t)(max_mobs - existing);

  // Determine the direction "behind" the player using yaw.
  // yaw is -128..127 mapping to -180..180 degrees.
  // We compute the forward direction vector, then negate it for "behind".
  // Instead of trigonometry, use the integer yaw-to-direction mapping
  // that the Minecraft protocol uses for movement packets:
  //   yaw values map to 8 directions on the XZ plane.
  // Forward direction (where player is facing):
  //   yaw  -128..-96  => dx= 0, dz=-1  (North, -Z)
  //   yaw   -96..-64  => dx= 1, dz=-1  (North-East)
  //   yaw   -64..-32  => dx= 1, dz= 0  (East, +X)
  //   yaw   -32..  0  => dx= 1, dz= 1  (South-East)
  //   yaw     0.. 32  => dx= 0, dz= 1  (South, +Z)
  //   yaw    32.. 64  => dx=-1, dz= 1  (South-West)
  //   yaw    64.. 96  => dx=-1, dz= 0  (West, -X)
  //   yaw    96..127  => dx=-1, dz=-1  (North-West)
  int8_t fwd_dx, fwd_dz;
  int8_t yaw = player->yaw;
  if (yaw < -96)      { fwd_dx =  0; fwd_dz = -1; }
  else if (yaw < -64) { fwd_dx =  1; fwd_dz = -1; }
  else if (yaw < -32) { fwd_dx =  1; fwd_dz =  0; }
  else if (yaw <   0) { fwd_dx =  1; fwd_dz =  1; }
  else if (yaw <  32) { fwd_dx =  0; fwd_dz =  1; }
  else if (yaw <  64) { fwd_dx = -1; fwd_dz =  1; }
  else if (yaw <  96) { fwd_dx = -1; fwd_dz =  0; }
  else                { fwd_dx = -1; fwd_dz = -1; }

  // Behind = opposite of forward
  int8_t behind_dx = -fwd_dx;
  int8_t behind_dz = -fwd_dz;

  // Perpendicular direction for lateral offset
  int8_t side_dx = -behind_dz;
  int8_t side_dz = behind_dx;

  // Try to spawn mobs in random positions behind the player
  uint8_t to_spawn = (fast_rand() % slots_left) + 1; // Spawn 1 to slots_left
  for (uint8_t s = 0; s < to_spawn; s ++) {

    // Pick a random distance behind the player
    int dist_range = spawn_range - min_dist;
    if (dist_range <= 0) dist_range = 1;
    uint8_t dist = (uint8_t)min_dist + (fast_rand() % dist_range);

    // Random offset perpendicular to behind direction
    int8_t side_offset = (int8_t)(fast_rand() % 11) - 5; // -5 to +5

    short spawn_x = player->x + behind_dx * dist + side_dx * side_offset;
    short spawn_z = player->z + behind_dz * dist + side_dz * side_offset;

    // Verify this position is actually BEHIND the player using dot product.
    // Vector from player to spawn point:
    int16_t vec_x = spawn_x - player->x;
    int16_t vec_z = spawn_z - player->z;
    // Dot product with forward direction — if positive, it's in front:
    int16_t dot = vec_x * fwd_dx + vec_z * fwd_dz;
    if (dot >= 0) continue; // Spawn point is in front or perpendicular, skip

    // Get the surface height at this position
    uint8_t surface_y = getHeightAt(spawn_x, spawn_z);
    if (surface_y == 0) continue; // Invalid height

    // Check that the block at surface is grass or snow
    uint8_t surface_block = getBlockAt(spawn_x, surface_y, spawn_z);
    if (surface_block != B_grass_block &&
        surface_block != B_snowy_grass_block &&
        surface_block != B_snow_block) continue;

    // Check that the two blocks above are passable (enough headroom)
    if (!isPassableSpawnBlock(getBlockAt(spawn_x, surface_y + 1, spawn_z))) continue;
    if (!isPassableSpawnBlock(getBlockAt(spawn_x, surface_y + 2, spawn_z))) continue;

    // Pick a random passive mob type
    uint8_t type = passive_types[fast_rand() % num_passive_types];

    // Spawn at full health (20)
    spawnMob(type, spawn_x, surface_y + 1, spawn_z, 20);
  }

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

      #ifdef ENABLE_PICKUP_ANIMATION
      playPickupAnimation(player, I_white_wool, mob->x, mob->y, mob->z);
      #endif

      uint8_t item_count = 1 + (fast_rand() & 1); // 1-2
      givePlayerItem(player, I_white_wool, item_count);

      for (int i = 0; i < MAX_PLAYERS; i ++) {
        PlayerData* player = &player_data[i];
        int client_fd = player->client_fd;

        if (client_fd == -1) continue;
        if (player->flags & 0x20) continue;

        sc_entityAnimation(client_fd, interactor_id, 0);
      }

      broadcastMobMetadata(-1, entity_id);

      break;
  }
}

void hurtEntity (int entity_id, int attacker_id, uint8_t damage_type, uint8_t damage) {

  if (attacker_id > 0) { // Attacker is a player

    PlayerData *player;
    if (getPlayerData(attacker_id, &player)) return;

    // Check if attack cooldown flag is set
    if (player->flags & 0x01) return;

    // Scale damage based on held item
    uint16_t held_item = player->inventory_items[player->hotbar];
    if (held_item == I_wooden_sword) damage *= 4;
    else if (held_item == I_golden_sword) damage *= 4;
    else if (held_item == I_stone_sword) damage *= 5;
    else if (held_item == I_iron_sword) damage *= 6;
    else if (held_item == I_diamond_sword) damage *= 7;
    else if (held_item == I_netherite_sword) damage *= 8;

    // Enable attack cooldown
    player->flags |= 0x01;
    player->flagval_8 = 0;

  }

  // Whether this attack caused the target entity to die
  uint8_t entity_died = false;

  if (entity_id > 0) { // The attacked entity is a player

    PlayerData *player;
    if (getPlayerData(entity_id, &player)) return;

    // Don't continue if the player is already dead
    if (player->health == 0) return;

    // Calculate damage reduction from player's armor
    uint8_t defense = getPlayerDefensePoints(player);
    // This uses the old (pre-1.9) protection calculation. Factors are
    // scaled up 256 times to avoid floating point math. Due to lost
    // precision, the 4% reduction factor drops to ~3.9%, although the
    // the resulting effective damage is then also rounded down.
    uint8_t effective_damage = damage * (256 - defense * 10) / 256;

    // Process health change on the server
    if (player->health <= effective_damage) {

      player->health = 0;
      entity_died = true;

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
      } else if (damage_type == D_cactus) {
        // Killed by being near a cactus
        strcpy((char *)recv_buffer + player_name_len, " was pricked to death");
        recv_buffer[player_name_len + 21] = '\0';
      } else {
        // Unknown death reason
        strcpy((char *)recv_buffer + player_name_len, " died");
        recv_buffer[player_name_len + 5] = '\0';
      }

    } else player->health -= effective_damage;

    // Update health on the client
    sc_setHealth(entity_id, player->health, player->hunger, player->saturation);

  } else { // The attacked entity is a mob

    int mob_index = -entity_id - 2;
    if (mob_index < 0 || mob_index >= MAX_MOBS) return;
    MobData *mob = &mob_data[mob_index];

    uint8_t mob_health = mob->data & 31;

    // Don't continue if the mob is already dead
    if (mob_health == 0) return;

    // Set the mob's panic timer
    mob->data |= (3 << 6);

    // Process health change on the server
    if (mob_health <= damage) {

      mob->data -= mob_health;
      mob->y = 0;
      entity_died = true;

      // Handle mob drops
      if (attacker_id > 0) {
        PlayerData *player;
        if (getPlayerData(attacker_id, &player)) return;
        switch (mob->type) {
          case 25: givePlayerItem(player, I_chicken, 1); break;
          case 28: givePlayerItem(player, I_beef, 1 + (fast_rand() % 3)); break;
          case 95: givePlayerItem(player, I_porkchop, 1 + (fast_rand() % 3)); break;
          case 106: givePlayerItem(player, I_mutton, 1 + (fast_rand() & 1)); break;
          case 145: givePlayerItem(player, I_rotten_flesh, (fast_rand() % 3)); break;
          default: break;
        }
      }

    } else mob->data -= damage;

  }

  // Broadcast damage event to all players
  for (int i = 0; i < MAX_PLAYERS; i ++) {
    int client_fd = player_data[i].client_fd;
    if (client_fd == -1) continue;
    sc_damageEvent(client_fd, entity_id, damage_type);
    // Below this, handle death events
    if (!entity_died) continue;
    sc_entityEvent(client_fd, entity_id, 3);
    if (entity_id >= 0) {
      // If a player died, broadcast their death message
      sc_systemChat(client_fd, (char *)recv_buffer, strlen((char *)recv_buffer));
    }
  }

}

// Simulates events scheduled for regular intervals
// Takes the time since the last tick in microseconds as the only arguemnt
void handleServerTick (int64_t time_since_last_tick) {

  // Update world time
  world_time = (world_time + time_since_last_tick / 50000) % 24000;
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
      if (player->flags & 0x01) {
        if (player->flagval_8 >= (uint8_t)(0.6f * TICKS_PER_SECOND)) {
          player->flags &= ~0x01;
          player->flagval_8 = 0;
        } else player->flagval_8 ++;
      }
      if (player->flags & 0x10) {
        if (player->flagval_16 >= (uint16_t)(1.6f * TICKS_PER_SECOND)) {
          handlePlayerEating(&player_data[i], false);
          player->flags &= ~0x10;
          player->flagval_16 = 0;
        } else player->flagval_16 ++;
      }
      #ifndef BROADCAST_ALL_MOVEMENT
        player->flags &= ~0x40;
      #endif
      if (server_ticks % (uint32_t)TICKS_PER_SECOND != 0) continue;
      sc_keepAlive(player->client_fd);
      sc_updateTime(player->client_fd, world_time);
      uint8_t block = getBlockAt(player->x, player->y, player->z);
      if (block >= B_lava && block < B_lava + 4) {
        hurtEntity(player->client_fd, -1, D_lava, 8);
      }
      #ifdef ENABLE_CACTUS_DAMAGE
      if (block == B_cactus ||
        getBlockAt(player->x + 1, player->y, player->z) == B_cactus ||
        getBlockAt(player->x - 1, player->y, player->z) == B_cactus ||
        getBlockAt(player->x, player->y, player->z + 1) == B_cactus ||
        getBlockAt(player->x, player->y, player->z - 1) == B_cactus
      ) hurtEntity(player->client_fd, -1, D_cactus, 4);
      #endif
      if (player->health >= 20 || player->health == 0) continue;
      if (player->hunger < 18) continue;
      if (player->saturation >= 600) {
        player->saturation -= 600;
        player->health ++;
      } else {
        player->hunger --;
        player->health ++;
      }
      sc_setHealth(player->client_fd, player->health, player->hunger, player->saturation);
    }
  }

  // Perform regular checks for if it's time to write to disk
  writeDataToDiskOnInterval();

  /**
   * If the RNG seed ever hits 0, it'll never generate anything
   * else. This is because the fast_rand function uses a simple
   * XORshift. This isn't a common concern, so we only check for
   * this periodically. If it does become zero, we reset it to
   * the world seed as a good-enough fallback.
   */
  if (rng_seed == 0) rng_seed = world_seed;

  // Tick mob behavior
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

    uint8_t passive = (
      mob_data[i].type == 25 || // Chicken
      mob_data[i].type == 28 || // Cow
      mob_data[i].type == 95 || // Pig
      mob_data[i].type == 106 // Sheep
    );
    // Mob "panic" timer, set to 3 after being hit
    // Currently has no effect on hostile mobs
    uint8_t panic = (mob_data[i].data >> 6) & 3;

    // Burn hostile mobs if above ground during sunlight
    if (!passive && (world_time < 13000 || world_time > 23460) && mob_data[i].y > 48) {
      hurtEntity(entity_id, -1, D_on_fire, 2);
    }

    uint32_t r = fast_rand();

    // Process panic timer (no early continue - mobs move every tick now)
    if (passive && panic && server_ticks % (uint32_t)TICKS_PER_SECOND == 0) {
      mob_data[i].data -= (1 << 6);
    }

    // Find the player closest to this mob
    PlayerData* closest_player = &player_data[0];
    double closest_dist_double = 2147483647.0;
    for (int j = 0; j < MAX_PLAYERS; j ++) {
      if (player_data[j].client_fd == -1) continue;
      double dx = mob_data[i].x - player_data[j].x;
      double dz = mob_data[i].z - player_data[j].z;
      double curr_dist = dx * dx + dz * dz; // Squared distance
      if (curr_dist < closest_dist_double) {
        closest_dist_double = curr_dist;
        closest_player = &player_data[j];
      }
    }

    // Despawn mobs past a certain distance from nearest player
    // Use squared distance to avoid sqrt (256^2 = 65536)
    if (closest_dist_double > (double)MOB_DESPAWN_DISTANCE * MOB_DESPAWN_DISTANCE) {
      mob_data[i].type = 0;
      continue;
    }

    double old_x = mob_data[i].x, old_z = mob_data[i].z;
    double old_y = mob_data[i].y;

    double new_x = old_x, new_z = old_z;
    double new_y = old_y;
    uint8_t yaw = mob_data[i].yaw_store;  // Keep current yaw from stored direction

    // Movement increment per tick (sub-block movement for smoothness)
    double move_amount = 0.05;  // Move 0.05 blocks per tick for smooth walking

    if (passive) { // Passive mob movement handling

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

          // Walk in this direction for 2-4 seconds (40-80 ticks at 20 TPS)
          int min_ticks = (int)(2.0f * TICKS_PER_SECOND);
          int max_ticks = (int)(4.0f * TICKS_PER_SECOND);
          mob_data[i].move_timer = min_ticks + (fast_rand() % (max_ticks - min_ticks + 1));

          // If panicking, shorter rest periods and keep moving
          if (panic) {
            mob_data[i].move_timer /= 2;
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

      // If we're already next to the player, hurt them and skip movement
      double dist_to_player = sqrt(closest_dist_double);
      double y_diff = fabs(old_y - closest_player->y);
      if (dist_to_player < 3.0 && y_diff < 2.0) {
        hurtEntity(closest_player->client_fd, entity_id, D_generic, 6);
        continue;
      }

      // Move towards the closest player with persistent direction
      double dx = closest_player->x - old_x;
      double dz = closest_player->z - old_z;
      double len = sqrt(dx * dx + dz * dz);

      if (len > 0.001) {
        // Normalize and scale movement for smooth pursuit
        double move_x = (dx / len) * move_amount;
        double move_z = (dz / len) * move_amount;

        new_x += move_x;
        new_z += move_z;

        // Calculate yaw from direction
        double angle = atan2(dz, dx) * 256.0 / (2.0 * 3.14159265358979);
        yaw = (uint8_t)((int)(angle + 0.5) & 255);
      }

    }

    // Get block coordinates for collision detection
    int block_x = (int)new_x;
    int block_y = (int)new_y;
    int block_z = (int)new_z;

    // Holds the block that the mob is moving into
    uint8_t block = getBlockAt(block_x, block_y, block_z);
    // Holds the block above the target block, i.e. the "head" block
    uint8_t block_above = getBlockAt(block_x, block_y + 1, block_z);

    // Validate movement on X axis
    int old_block_x = (int)old_x;
    int old_block_z = (int)old_z;

    if (block_x != old_block_x && (
      !isPassableBlock(getBlockAt(block_x, block_y + 1, old_block_z)) ||
      (
        !isPassableBlock(getBlockAt(block_x, block_y, old_block_z)) &&
        !isPassableBlock(getBlockAt(block_x, block_y + 2, old_block_z))
      )
    )) {
      new_x = old_x;
      block_x = old_block_x;
      block = getBlockAt(block_x, block_y, block_z);
      block_above = getBlockAt(block_x, block_y + 1, block_z);
      // Reset timer when blocked so passive mobs pick a new direction
      if (passive) mob_data[i].move_timer = 0;
    }
    // Validate movement on Z axis
    if (block_z != old_block_z && (
      !isPassableBlock(getBlockAt(old_block_x, block_y + 1, block_z)) ||
      (
        !isPassableBlock(getBlockAt(old_block_x, block_y, block_z)) &&
        !isPassableBlock(getBlockAt(old_block_x, block_y + 2, block_z))
      )
    )) {
      new_z = old_z;
      block_z = old_block_z;
      block = getBlockAt(block_x, block_y, block_z);
      block_above = getBlockAt(block_x, block_y + 1, block_z);
      // Reset timer when blocked so passive mobs pick a new direction
      if (passive) mob_data[i].move_timer = 0;
    }
    // Validate diagonal movement
    if (block_x != old_block_x && block_z != old_block_z && (
      !isPassableBlock(block_above) ||
      (
        !isPassableBlock(block) &&
        !isPassableBlock(getBlockAt(block_x, block_y + 2, block_z))
      )
    )) {
      // We know that movement along just one axis is fine thanks to the
      // checks above, pick one based on proximity.
      double dist_x = fabs(old_x - closest_player->x);
      double dist_z = fabs(old_z - closest_player->z);
      if (dist_x < dist_z) {
        new_z = old_z;
        block_z = old_block_z;
      } else {
        new_x = old_x;
        block_x = old_block_x;
      }
      block = getBlockAt(block_x, block_y, block_z);
      // Reset timer when blocked so passive mobs pick a new direction
      if (passive) mob_data[i].move_timer = 0;
    }

    // Check if we're supposed to climb/drop one block
    // The checks above already ensure that there's enough space to climb
    if (!isPassableBlock(block)) new_y += 1.0;
    else if (isPassableBlock(getBlockAt(block_x, block_y - 1, block_z))) new_y -= 1.0;

    // Exit early if all movement was cancelled
    if (fabs(new_x - mob_data[i].x) < 0.001 &&
        fabs(new_z - mob_data[i].z) < 0.001 &&
        fabs(new_y - mob_data[i].y) < 0.001) continue;

    // Prevent collisions with other mobs
    uint8_t colliding = false;
    for (int j = 0; j < MAX_MOBS; j ++) {
      if (j == i) continue;
      if (mob_data[j].type == 0) continue;
      if ((mob_data[j].data & 31) == 0) continue; // Skip dead mobs
      double dx = mob_data[j].x - new_x;
      double dz = mob_data[j].z - new_z;
      double dy = fabs(mob_data[j].y - new_y);
      if (dx * dx + dz * dz < 1.0 && dy < 2.0) {
        colliding = true;
        break;
      }
    }
    if (colliding) continue;

    if ( // Hurt mobs that stumble into lava
      (block >= B_lava && block < B_lava + 4) ||
      (block_above >= B_lava && block_above < B_lava + 4)
    ) hurtEntity(entity_id, -1, D_lava, 8);

    // Store new mob position
    mob_data[i].x = new_x;
    mob_data[i].y = new_y;
    mob_data[i].z = new_z;

    // Broadcast entity movement packets (only if we actually moved)
    if (fabs(new_x - old_x) > 0.0001 ||
        fabs(new_z - old_z) > 0.0001 ||
        fabs(new_y - old_y) > 0.0001) {
      for (int j = 0; j < MAX_PLAYERS; j ++) {
        if (player_data[j].client_fd == -1) continue;
        // Find the client state for this player
        uint8_t target_in_play = 0;
        for (int k = 0; k < MAX_PLAYERS; k ++) {
          if (client_states[k].client_fd == player_data[j].client_fd) {
            target_in_play = (client_states[k].state == STATE_PLAY);
            break;
          }
        }
        if (!target_in_play) continue;
        sc_teleportEntity (
          player_data[j].client_fd, entity_id,
          new_x, new_y, new_z,
          yaw * 360 / 256, 0
        );
        sc_setHeadRotation(player_data[j].client_fd, entity_id, yaw);
      }
    }

  }

  // Spawn friendly mobs around players at configurable interval
  if (config.mob_spawn_enabled && config.mob_spawn_interval > 0 &&
      server_ticks % (uint32_t)config.mob_spawn_interval == 0) {
    for (int i = 0; i < MAX_PLAYERS; i ++) {
      PlayerData *player = &player_data[i];
      if (player->client_fd == -1) continue;
      if (player->flags & 0x20) continue; // Skip players still loading
      spawnMobsAroundPlayer(player);
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

  // Reset player attack cooldown
  if (player->flags & 0x01) {
    if (player->flagval_8 >= (uint8_t)(0.6f * TICKS_PER_SECOND)) {
      player->flags &= ~0x01;
      player->flagval_8 = 0;
    } else {
      player->flagval_8++;
    }
  }

  // Handle eating animation timer (no packet - will be sent in main thread)
  if (player->flags & 0x10) {
    if (player->flagval_16 >= (uint16_t)(1.6f * TICKS_PER_SECOND)) {
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

  // Process once-per-second state updates (no packet sending here)
  if (server_ticks % (uint32_t)TICKS_PER_SECOND == 0) {
    // Calculate lava damage (apply health change, packet sent separately)
    uint8_t block = getBlockAt(player->x, player->y, player->z);
    if (block >= B_lava && block < B_lava + 4) {
      if (player->health > 8) player->health -= 8;
      else player->health = 0;
    }
    #ifdef ENABLE_CACTUS_DAMAGE
    // Calculate cactus damage (apply health change, packet sent separately)
    if (block == B_cactus ||
        getBlockAt(player->x + 1, player->y, player->z) == B_cactus ||
        getBlockAt(player->x - 1, player->y, player->z) == B_cactus ||
        getBlockAt(player->x, player->y, player->z + 1) == B_cactus ||
        getBlockAt(player->x, player->y, player->z - 1) == B_cactus) {
      if (player->health > 4) player->health -= 4;
      else player->health = 0;
    }
    #endif

    // Heal from saturation (update state only, packet sent separately)
    if (player->health < 20 && player->health != 0 && player->hunger >= 18) {
      if (player->saturation >= 600) {
        player->saturation -= 600;
        player->health++;
      } else {
        player->hunger--;
        player->health++;
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
      player->flags &= ~0x10;
      player->flagval_16 = 0;
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
void broadcastChestUpdate (int origin_fd, uint8_t *storage_ptr, uint16_t item, uint8_t count, uint8_t slot) {

  for (int i = 0; i < MAX_PLAYERS; i ++) {
    if (player_data[i].client_fd == -1) continue;
    if (player_data[i].flags & 0x20) continue;
    // Filter for players that have this chest open
    if (memcmp(player_data[i].craft_items, &storage_ptr, sizeof(storage_ptr)) != 0) continue;
    // Send slot update packet
    sc_setContainerSlot(player_data[i].client_fd, 2, slot, count, item);
  }

  #ifndef DISK_SYNC_BLOCKS_ON_INTERVAL
  writeChestChangesToDisk(storage_ptr, slot);
  #endif

}
#endif

ssize_t writeEntityData (int client_fd, EntityData *data) {
  writeByte(client_fd, data->index);
  writeVarInt(client_fd, data->type);

  switch (data->type) {
    case 0: // Byte
      return writeByte(client_fd, data->value.byte);
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
