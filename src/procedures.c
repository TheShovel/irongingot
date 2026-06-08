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

// World spawn point (block coordinates)
#define SPAWN_BLOCK_X 8
#define SPAWN_BLOCK_Z 8
// World spawn chunk (chunk coordinates)
#define SPAWN_CHUNK_X (SPAWN_BLOCK_X >> 4)
#define SPAWN_CHUNK_Z (SPAWN_BLOCK_Z >> 4)

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

static int getPlayerIndexByPointer(PlayerData *player) {
  if (!player) return -1;
  if (player < player_data || player >= player_data + MAX_PLAYERS) return -1;
  return (int)(player - player_data);
}

static uint8_t getConfiguredGameMode(void) {
  if (config.gamemode < 0 || config.gamemode > 3) return GAMEMODE;
  return (uint8_t)config.gamemode;
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

  snprintf(path, sizeof(path), "skins/%s.texture", base_name);
  len = readTokenFile(path, appearance->texture_value, sizeof(appearance->texture_value));
  if (len <= 0) {
    if (len == -2) terminal_ui_log("Skin: %s is too large, ignoring", path);
    return false;
  }

  appearance->texture_value_len = (uint16_t)len;
  appearance->has_texture = true;

  snprintf(path, sizeof(path), "skins/%s.signature", base_name);
  len = readTokenFile(path, appearance->texture_signature, sizeof(appearance->texture_signature));
  if (len > 0) {
    appearance->texture_signature_len = (uint16_t)len;
    appearance->has_signature = true;
  } else if (len == -2) {
    terminal_ui_log("Skin: %s is too large, ignoring signature", path);
  }

  return true;
}

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
  player->dimension = DIMENSION_OVERWORLD;
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

void resetPlayerAppearance (int player_index) {
  if (player_index < 0 || player_index >= MAX_PLAYERS) return;

  PlayerAppearance *appearance = &player_appearance[player_index];
  appearance->texture_value[0] = '\0';
  appearance->texture_signature[0] = '\0';
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
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (player_data[i].client_fd != client_fd) continue;
    player_appearance[i].skin_parts = skin_parts;
    player_appearance[i].main_hand = main_hand ? 1 : 0;
    return;
  }
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
      player_data[i].flags |= 0x20;
      player_data[i].flagval_16 = 0;
      memcpy(player_data[i].uuid, uuid, 16);
      memcpy(player_data[i].name, name, 16);
      resetPlayerData(&player_data[i]);
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
    PlayerData leaving_player = player_data[i];
    // Mark the player as being offline
    player_data[i].client_fd = -1;
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
  if (slot == player->hotbar) broadcastPlayerEquipment(player);

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
    player->flags &= ~0x02;
  } else { // Not a new player
    // Calculate spawn position from player data
    spawn_x = (float)player->x + 0.5;
    spawn_y = player->y;
    spawn_z = (float)player->z + 0.5;
    // If Y is 0 or impossibly low, lift above surface
    if (spawn_y <= 0 || spawn_y > 319) {
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
    }
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

  sc_setEntityMetadata(client_fd, player->client_fd, metadata, 4);
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
    // Check client state
    uint8_t target_in_play = 0;
    for (int k = 0; k < MAX_PLAYERS; k ++) {
      if (client_states[k].client_fd == client_fd) {
        target_in_play = (client_states[k].state == STATE_PLAY);
        break;
      }
    }
    if (!target_in_play) continue;

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
    // Check client state
    uint8_t target_in_play = 0;
    for (int k = 0; k < MAX_PLAYERS; k ++) {
      if (client_states[k].client_fd == client_fd) {
        target_in_play = (client_states[k].state == STATE_PLAY);
        break;
      }
    }
    if (!target_in_play) continue;

    sendPlayerEquipment(client_fd, player);
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
      if (player->dimension != mob->dimension) continue;
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
  int16_t y;
  uint8_t dimension;
  uint16_t block;
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

static inline void notify_block_change_mutation(short x, short z, uint8_t dimension) {
  int chunk_x = div_floor(x, 16);
  int chunk_z = div_floor(z, 16);
  mark_modified_chunk(chunk_x, chunk_z);
  invalidate_cached_chunk(chunk_x, chunk_z, dimension);
  invalidate_block_lookup_cache();
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
    // Skip stair and furnace state data
    if (is_stair_block(block_changes[i].block) || block_changes[i].block == B_furnace) {
      i += 1;
    }
    // Skip chest inventory entries
    if (block_changes[i].block == B_chest) {
      if (i + 14 >= block_changes_count) break;
      i += 14;
    }
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
      if (block_changes[idx].block == B_chest) { \
        for (int j = 1; j < 15 && (idx) + j < block_changes_count; j++) block_changes[(idx) + j].block = 0xFF; \
      } else if (is_door_block(block_changes[idx].block)) { \
        if ((idx) + 1 < block_changes_count) block_changes[(idx) + 1].block = 0xFF; \
        if ((idx) + 2 < block_changes_count) block_changes[(idx) + 2].block = 0xFF; \
      } else if (is_stair_block(block_changes[idx].block) || block_changes[idx].block == B_furnace) { \
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

      if (block == B_chest && block_changes[i].block != B_chest) {
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
        } else if (block == B_chest) {
          memset(&block_changes[i + 1], 0, 14 * sizeof(BlockChange));
          special_block_set_state(x, y, z, dimension, block, oriented_encode_state(0));
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
    if (block_changes[i].block == B_chest) { if (i + 14 >= block_changes_count) break; i += 14; continue; }
    #ifdef ALLOW_DOORS
    if (is_door_block(block_changes[i].block)) { if (i + 2 >= block_changes_count) break; i += 2; continue; }
    #endif
    if (is_stair_block(block_changes[i].block) || block_changes[i].block == B_furnace) { i += 1; }
  }

  // Don't create a new entry if it contains the base terrain block
  if (is_base_block) {
    pthread_mutex_unlock(&block_changes_mutex);
    return 0;
  }

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

    if (block == B_chest) {
      memset(&block_changes[base + 1], 0, 14 * sizeof(BlockChange));
      special_block_set_state(x, y, z, dimension, block, oriented_encode_state(0));
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
    else if (is_stair_block(block) || block == B_furnace) {
      // State entry
      block_changes[base + 1].x = 0;
      block_changes[base + 1].y = 0;
      block_changes[base + 1].z = z;
      block_changes[base + 1].block = 0;
      if (is_stair_block(block)) {
        special_block_set_state(x, y, z, dimension, block, stair_encode_state(0, 0));
      } else {
        special_block_set_state(x, y, z, dimension, block, furnace_encode_state(0, 0));
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
        #ifdef ENABLE_PICKUP_ANIMATION
        playPickupAnimation(player, drop, nx, ny, nz);
        #endif
        givePlayerItem(player, drop, 1);
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
            #ifdef ENABLE_PICKUP_ANIMATION
            playPickupAnimation(player, drop, nx, ny, nz);
            #endif
            givePlayerItem(player, drop, 1);
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
    uint32_t r = fast_rand();

    if (held_item == I_shears) return leaf_item;
    if (leafDropsApple(block) && r < 214748364) return I_apple; // 5%
    if (r < 85899345) return I_stick; // 2%
    if (sapling_item != 0 && r < 214748364) return sapling_item; // 5%
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

    case B_wheat:
      if (held_item == 0) return I_wheat_seeds; // Cascaded break (support removed)
      return 0; // Handled in handlePlayerAction with age check

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
    block == B_wheat ||
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
    block == B_wheat ||
    block == B_dead_bush ||
    block == B_bush ||
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
    block == B_wheat ||
    block == B_dead_bush ||
    block == B_bush ||
    block == B_seagrass ||
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
    item == I_shears ||
    // Filled buckets
    item == I_water_bucket ||
    item == I_lava_bucket
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

  // Read wheat age BEFORE makeBlockChange clears the special block entry
  uint16_t wheat_age = 0;
  if (block == B_wheat) {
    wheat_age = special_block_get_state(x, y, z, player->dimension);
  }

  // Don't continue if the block change failed
  if (makeBlockChange(x, y, z, 0, player->dimension)) return;

  if (block == B_obsidian || block == B_nether_portal) {
    clearNearbyNetherPortal(x, y, z, player->dimension);
  }

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
      makeBlockChange(x, y - 1, z, 0, player->dimension);  // Break lower half
    } else {
      makeBlockChange(x, y + 1, z, 0, player->dimension);  // Break upper half
    }

    // Clear door state from the unified special block table
    special_block_clear(door_x, door_y, door_z, player->dimension);
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

  // Special handling for wheat drops based on maturity
  if (block == B_wheat) {
    if (wheat_age >= 7) {
      givePlayerItem(player, I_wheat, 1);
      givePlayerItem(player, I_wheat_seeds, 1 + (fast_rand() % 3));
    } else {
      givePlayerItem(player, I_wheat_seeds, 1);
    }
    special_block_clear(x, y, z, player->dimension);
  }

  // Cascade-break connected leaves and floating leaves (cap at 6 additional broken)
  if (isLeafBlock(block)) {
    int broken_count = 0;
    breakConnectedLeaves(x, y, z, block, player, &broken_count);
    breakFloatingLeaves(x, y, z, block, player, &broken_count);
  }

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
    if (item) givePlayerItem(player, item, 1);
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

static void switchPlayerToDimension(PlayerData *player, uint8_t new_dim) {
  uint8_t old_dim = player->dimension;
  if (old_dim == new_dim) return;

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
  int np_x = 0, np_z = 0, np_h = 65;
  if (new_dim == DIMENSION_NETHER) {
    init_worldgen();
    np_x = (int)player->x;
    np_z = (int)player->z;
    // Sample floor height instead of ceiling height.
    double floor_n = octave_sample(&surface_noise, (double)np_x * 0.015, 0, (double)np_z * 0.015);
    int floor_h = 60 + (int)(floor_n * 35.0);
    if (floor_h < 35) floor_h = 35;
    if (floor_h > 95) floor_h = 95;

    // Ensure we are above the lava sea (Y=50).
    if (floor_h < 64) floor_h = 64;

    np_h = floor_h;
    player->y = (float)(floor_h + 1);

    terminal_ui_log("Nether spawn: bx=%d bz=%d floor_h=%d np_h=%d player_y=%d",
      np_x, np_z, floor_h, np_h, (int)player->y);
  }

  sc_respawn(player->client_fd, new_dim);

  sc_setDefaultSpawnPosition(player->client_fd, 8, 80, 8);
  sc_startWaitingForChunks(player->client_fd);

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
    // Stand the player on top of the obsidian frame so they have a solid
    // platform that is guaranteed to be inside the pre-generated center
    // chunk. (Previously the player was placed at (np_x + 2, ofh + 2, np_z),
    // which is now occupied by the right column of the frame and sits in
    // floor that may not be loaded yet.)
    player->x = np_x;
    player->z = np_z;
    player->y = (int16_t)(np_h + 5);
    // Recalculate center chunk from new position
    cx = div_floor(player->x, 16);
    cz = div_floor(player->z, 16);
    sc_setCenterChunk(player->client_fd, cx, cz);
  }

  if (new_dim == DIMENSION_OVERWORLD) {
    uint8_t surface_y = getHeightAt(player->x, player->z);
    if (surface_y > 0) player->y = surface_y + 1;
    else player->y = 80;
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
}

void switchPlayerDimension(PlayerData *player) {
  uint8_t new_dim = (player->dimension == DIMENSION_OVERWORLD) ? DIMENSION_NETHER : DIMENSION_OVERWORLD;
  switchPlayerToDimension(player, new_dim);
}

void handlePortalTravel(PlayerData *player) {
  if (player->client_fd == -1) return;
  if (player->flags & 0x20) return;
  if (server_ticks % 5 != 0) return;

  uint16_t block = getBlockAt2(player->x, player->y, player->z, player->dimension);
  if (block == B_nether_portal) {
    switchPlayerDimension(player);
  } else if (player->dimension == DIMENSION_OVERWORLD && block == B_end_portal) {
    switchPlayerToDimension(player, DIMENSION_END);
  } else if (player->dimension == DIMENSION_END && block == B_end_portal) {
    // Exit portal in the End takes player back to overworld
    switchPlayerToDimension(player, DIMENSION_OVERWORLD);
  }
}

void handlePlayerUseItem (PlayerData *player, short x, short y, short z, uint8_t face) {

  // Check spawn protection for block interactions
  if (face != 255 && is_in_safe_area(x, z, player->dimension)) {
    sc_systemChat(player->client_fd, "§cCannot interact with blocks in protected spawn area", 54);
    return;
  }

  // Get targeted block (if coordinates are provided)
  uint16_t target = face == 255 ? 0 : getBlockAt2(x, y, z, player->dimension);
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
        broadcastPlayerEquipment(player);
        return;
      }
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
        // Village chest — create block_changes entry on first interaction
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
          // Populate with 3 random common items
          uint8_t *_slot = (uint8_t *)&block_changes[base + 1];
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
  } else if (getItemDefensePoints(*item) != 0) {
    // For some reason, this action is sent twice when looking at a block
    // Ignore the variant that has coordinates
    if (face != 255) return;
    // Swap to held piece of armor
    uint8_t slot = getArmorItemSlot(*item);
    uint16_t prev_item = player->inventory_items[slot];
    uint8_t prev_count = player->inventory_count[slot];
    player->inventory_items[slot] = *item;
    player->inventory_count[slot] = 1;
    player->inventory_items[player->hotbar] = prev_item;
    player->inventory_count[player->hotbar] = prev_count;
    // Update client inventory
    sc_setContainerSlot(player->client_fd, -2, serverSlotToClientSlot(0, slot), 1, *item);
    sc_setContainerSlot(player->client_fd, -2, serverSlotToClientSlot(0, player->hotbar), prev_count, prev_item);
    broadcastPlayerEquipment(player);
    return;
  }

  if (handleBucketUse(player, x, y, z, face, count, item)) return;

  // Don't proceed with block placement if no coordinates were provided
  if (face == 255) return;

  // Handle flint and steel - portal creation or fire placement
  if (*item == I_flint_and_steel) {
    short fx = x, fz = z;
    int16_t fy = y;
    if (!getFaceOffsetBlock(x, y, z, face, &fx, &fy, &fz)) return;
    if (target == B_obsidian && tryCreatePortal(fx, fy, fz, player->dimension)) {
      if (*count > 0) {
        *count -= 1;
        if (*count == 0) *item = 0;
      }
      sc_setContainerSlot(player->client_fd, 0, serverSlotToClientSlot(0, player->hotbar), *count, *item);
      broadcastPlayerEquipment(player);
      return;
    }
    if (isReplaceableBlock(getBlockAt2(fx, fy, fz, player->dimension))) {
      makeBlockChange(fx, fy, fz, B_fire, player->dimension);
      if (*count > 0) {
        *count -= 1;
        if (*count == 0) *item = 0;
      }
    }
    sc_setContainerSlot(player->client_fd, 0, serverSlotToClientSlot(0, player->hotbar), *count, *item);
    broadcastPlayerEquipment(player);
    return;
  }

  // Hoe tilling: right-click dirt or grass_block with any hoe → farmland
  if (*item == I_wooden_hoe || *item == I_stone_hoe || *item == I_iron_hoe ||
      *item == I_golden_hoe || *item == I_diamond_hoe || *item == I_netherite_hoe) {
    if (target == B_dirt || target == B_grass_block) {
      makeBlockChange(x, y, z, B_farmland, player->dimension);
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

  // Special handling for oriented block placement (chests, furnaces)
  if (isOrientedBlock(block)) {
    if (block == B_chest && !config.allow_chests) return;
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
      isReplaceableBlock(getBlockAt2(x, y, z, player->dimension))
    ) {
      // Apply server-side block change (persists to block_changes and disk)
      if (makeBlockChange(x, y, z, block, player->dimension)) return;

      // Update state with direction in the unified special block table
      uint16_t state = oriented_encode_state(direction);
      special_block_set_state(x, y, z, player->dimension, block, state);

      // Store direction in legacy field for persistence across restarts
      for (int i = 0; i < block_changes_count; i++) {
        if (block_changes[i].block == B_chest &&
            block_changes[i].x == x && block_changes[i].y == y && block_changes[i].z == z) {
          block_changes[i + 14].y = direction;
          break;
        }
      }

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

void spawnMob (uint8_t type, short x, uint8_t y, short z, uint8_t health, uint8_t dimension) {

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
    uint16_t block_above = getBlockAt2(x, y + 1, z, dimension);
    
    // We want the fish to spawn in water with at least 1 water block above
    if (isWaterBlock(block) && isWaterBlock(block_above)) {
      // Make sure there's at least one more water or air block above for swimming room
      uint16_t block_above2 = getBlockAt2(x, y + 2, z, dimension);
      if (isWaterBlock(block_above2) || block_above2 == B_air) {
        return (uint8_t)y;
      }
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

    spawnMob(E_VILLAGER, house_x[h], y, house_z[h], 20, DIMENSION_OVERWORLD);
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
  if (existing_fish >= max_fish) return;

  // Fish types
  static const uint8_t fish_types[] = { E_COD, E_SALMON, E_TROPICAL_FISH };
  static const uint8_t num_fish_types = 3;

  uint8_t fish_to_spawn = 1 + (fast_rand() % 2); // Spawn 1-2 fish
  if (fish_to_spawn > max_fish - existing_fish) 
    fish_to_spawn = max_fish - existing_fish;

  for (uint8_t s = 0; s < fish_to_spawn; s++) {
    int16_t offset_x, offset_z;
    getRandomSpawnOffset(min_dist, spawn_range, &offset_x, &offset_z);
    short spawn_x = player->x + offset_x;
    short spawn_z = player->z + offset_z;

    uint8_t spawn_y = getWaterSpawnY(spawn_x, spawn_z, player->dimension);
    if (spawn_y == 0) continue; // No valid water position found

    uint8_t type = fish_types[fast_rand() % num_fish_types];
    // Fish spawn with 3 HP (they're small)
    // Fish need to be higher in water, so spawn at spawn_y + 1
    spawnMob(type, spawn_x, spawn_y + 1, spawn_z, 3, player->dimension);
    
    // Find the fish we just spawned and adjust its Y to float in water
    for (int j = 0; j < MAX_MOBS; j++) {
      if (mob_data[j].type == type && 
          mob_data[j].x == (double)spawn_x + 0.5 && 
          mob_data[j].z == (double)spawn_z + 0.5 &&
          mob_data[j].y == (double)(spawn_y + 1)) {
        mob_data[j].y = (double)(spawn_y + 1) + 0.5; // Float at center of water block
        break;
      }
    }
  }
}

static void spawnMobsAroundPlayer (PlayerData *player) {

  // Passive mobs and villagers don't spawn in the End
  if (player->dimension == DIMENSION_END) return;

  spawnVillageVillagers(player);
  spawnFishInWater(player);

  // Passive mob types: Chicken(25), Cow(28), Pig(95), Sheep(106)
  static const uint8_t passive_types[] = { 25, 28, 95, 106 };
  static const uint8_t num_passive_types = 4;

  // Hostile mob types: Zombie(145) - spawns only at night
  static const uint8_t hostile_types[] = { 145 };
  static const uint8_t num_hostile_types = 1;

  int max_mobs = config.mob_spawn_max_per_player;
  int spawn_range = config.mob_spawn_range;
  int min_dist = config.mob_spawn_min_distance;

  // Count existing mobs near this player
  uint8_t existing = countMobsNearPlayer(player);
  if ((int)existing >= max_mobs) return;

  uint8_t slots_left = (uint8_t)(max_mobs - existing);

  // Determine if it's night (world_time >= 12000)
  uint8_t is_night = (world_time >= 12000);



  if (player->dimension == DIMENSION_NETHER) {
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
      spawnMob(E_PIGLIN, spawn_x, surface_y + 1, spawn_z, 16, player->dimension);
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

      // Zombified Piglin(148) spawns in the Nether regardless of time of day.
      spawnMob(148, spawn_x, surface_y + 1, spawn_z, 10, player->dimension);
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

    // Spawn mobs with 10 HP.
    spawnMob(type, spawn_x, surface_y + 1, spawn_z, 10, player->dimension);
  }

  // Spawn hostile mobs (zombies) only at night
  if (is_night && num_hostile_types > 0) {
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

      // Zombies spawn with 10 HP
      spawnMob(type, spawn_x, surface_y + 1, spawn_z, 10, player->dimension);
    }
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

    case E_VILLAGER: // Villager
      if (player->inventory_items[player->hotbar] == I_emerald) {
        // Take the emerald
        player->inventory_count[player->hotbar]--;
        if (player->inventory_count[player->hotbar] == 0) {
          player->inventory_items[player->hotbar] = 0;
        }
        syncHeldItem(player, player->inventory_count[player->hotbar], player->inventory_items[player->hotbar]);

        // Give a random item
        uint16_t random_item = getRandomCreativeItem();
        givePlayerItem(player, random_item, 1);

        // Swing arm animation for all nearby players
        for (int i = 0; i < MAX_PLAYERS; i ++) {
          if (player_data[i].client_fd == -1) continue;
          if (player_data[i].flags & 0x20) continue;
          sc_entityAnimation(player_data[i].client_fd, interactor_id, 0);
        }
      }
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
      }
      break;
  }
}

void hurtEntity (int entity_id, int attacker_id, uint8_t damage_type, uint8_t damage) {

  if (attacker_id > 0) { // Attacker is a player

    PlayerData *player;
    if (getPlayerData(attacker_id, &player)) return;

    // Scale damage based on held item
    uint16_t held_item = player->inventory_items[player->hotbar];
    if (held_item == I_wooden_sword) damage *= 4;
    else if (held_item == I_golden_sword) damage *= 4;
    else if (held_item == I_stone_sword) damage *= 5;
    else if (held_item == I_iron_sword) damage *= 6;
    else if (held_item == I_diamond_sword) damage *= 7;
    else if (held_item == I_netherite_sword) damage *= 8;

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
      } else if (damage_type == D_hot_floor) {
        // Killed by standing on magma
        strcpy((char *)recv_buffer + player_name_len, " discovered the floor was lava");
        recv_buffer[player_name_len + 31] = '\0';
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
    // Make passive mobs run instantly when hit
    if (mob->type == 25 || mob->type == 28 || mob->type == 95 || 
        mob->type == 106 || mob->type == E_VILLAGER ||
        mob->type == E_COD || mob->type == E_SALMON ||
        mob->type == E_PUFFERFISH || mob->type == E_TROPICAL_FISH) {
      mob->move_timer = 0; // Force immediate direction change
      mob->move_dx = 0;   // Clear current movement
      mob->move_dz = 0;
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

    // Push zombies back 2 blocks when hit by a player
    if (attacker_id > 0 && mob->type == 145) {
      PlayerData *attacker;
      if (!getPlayerData(attacker_id, &attacker)) {
        double dx = mob->x - attacker->x;
        double dz = mob->z - attacker->z;
        double len = sqrt(dx * dx + dz * dz);
        if (len > 0.001) {
          mob->x += (dx / len) * 2.0;
          mob->z += (dz / len) * 2.0;
        }
      }
    }
    
    // Piglins get angry when hit by a player
    if (attacker_id > 0 && mob->type == E_PIGLIN) {
      mob->anger_timer = 40; // 2 seconds of anger (40 ticks at 20 TPS)
    }

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
          case E_COD: givePlayerItem(player, I_cod, 1); break;
          case E_SALMON: givePlayerItem(player, I_salmon, 1); break;
          case E_PUFFERFISH: givePlayerItem(player, I_pufferfish, 1); break;
          case E_TROPICAL_FISH: givePlayerItem(player, I_tropical_fish, 1); break;
          case E_PIGLIN: 
            givePlayerItem(player, getRandomCreativeItem(), 1);
            break;
          case 145: givePlayerItem(player, I_rotten_flesh, (fast_rand() % 3)); break;
          case 148:
            givePlayerItem(player, I_rotten_flesh, fast_rand() % 3);
            if ((fast_rand() & 3) == 0) givePlayerItem(player, I_gold_ingot, 1);
            break;
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
      uint8_t pdim = player->dimension;
      uint16_t block = getBlockAt2(player->x, player->y, player->z, pdim);
      if (block >= B_lava && block < B_lava + 4) {
        hurtEntity(player->client_fd, -1, D_lava, 8);
      }
      if (!(player->flags & 0x04) && getBlockAt2(player->x, player->y - 1, player->z, pdim) == B_magma_block) {
        hurtEntity(player->client_fd, -1, D_hot_floor, 1);
      }
      #ifdef ENABLE_CACTUS_DAMAGE
      if (block == B_cactus ||
        getBlockAt2(player->x + 1, player->y, player->z, pdim) == B_cactus ||
        getBlockAt2(player->x - 1, player->y, player->z, pdim) == B_cactus ||
        getBlockAt2(player->x, player->y, player->z + 1, pdim) == B_cactus ||
        getBlockAt2(player->x, player->y, player->z - 1, pdim) == B_cactus
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

  // Weather progression - every ~2 seconds (40 ticks)
  if (server_ticks % 40 == 0) {
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

  // Check for portal travel every 5 ticks
  if (server_ticks % 5 == 0) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
      if (player_data[i].client_fd == -1) continue;
      if (player_data[i].flags & 0x20) continue;
      handlePortalTravel(&player_data[i]);
    }
  }

  // Process queued fluid updates (spread across ticks for gradual flow)
  #ifdef DO_FLUID_FLOW
  if (config.do_fluid_flow) {
    processFluidQueue();
  }
  #endif

  // Sync all player inventories every 2 seconds (40 ticks)
  if (server_ticks % 40 == 0) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
      PlayerData *player = &player_data[i];
      if (player->client_fd == -1) continue;
      if (player->flags & 0x20) continue;
      for (int j = 0; j < 41; j++) {
        sc_setContainerSlot(player->client_fd, 0, serverSlotToClientSlot(0, j),
          player->inventory_count[j], player->inventory_items[j]);
      }
    }
  }

  // Perform regular checks for if it's time to write to disk
  writeDataToDiskOnInterval();

  // Wheat growth random tick every ~2 seconds (40 ticks)
  if (server_ticks % 40 == 0) {
    uint32_t seed = fast_rand();
    for (int i = 0; i < MAX_SPECIAL_BLOCKS; i++) {
      if (special_blocks[i].block != B_wheat) continue;
      uint16_t age = special_blocks[i].state & 7;
      if (age >= 7) continue;
      uint16_t current = getBlockAt2(
        special_blocks[i].x, special_blocks[i].y, special_blocks[i].z,
        special_blocks[i].dimension);
      if (current != B_wheat) continue;
      if ((seed >> (i & 31)) & 1) {
        age++;
        special_blocks[i].state = age;
        // Broadcast the new visual state to all players
        uint16_t state_id = block_palette[B_wheat] + age;
        for (int j = 0; j < MAX_PLAYERS; j++) {
          if (player_data[j].client_fd == -1) continue;
          if (player_data[j].flags & 0x20) continue;
          sc_blockUpdateState(player_data[j].client_fd,
            special_blocks[i].x,
            special_blocks[i].y,
            special_blocks[i].z,
            state_id);
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

    // Burn overworld zombies if above ground during sunlight.
    // Nether mobs should not burn based on overworld time of day.
    if (mob_data[i].dimension == DIMENSION_OVERWORLD && mob_data[i].type == 145 &&
        (world_time < 13000 || world_time > 23460) && mob_data[i].y > 48) {
      hurtEntity(entity_id, -1, D_on_fire, 2);
    }

    uint32_t r = fast_rand();

    if (mob_data[i].anger_timer > 0) mob_data[i].anger_timer --;

    uint8_t neutral_roaming = (
      (mob_data[i].type == 148 && mob_data[i].anger_timer <= 0) ||
      (mob_data[i].type == E_PIGLIN && mob_data[i].anger_timer <= 0)
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

    // If no players are online, skip AI updates (mobs stay idle)
    if (closest_player == NULL) {
      continue;
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
        
        // Fish out of water take damage
        if (!found_water && server_ticks % 10 == 0) {
          hurtEntity(entity_id, -1, D_generic, 1);
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
      
      // Find nearby water surface level. Scanning from world top every tick is
      // expensive; fish only need local surface avoidance around their current
      // swim depth.
      int ws_fish_block_x = mobBlockCoord(new_x);
      int ws_fish_block_z = mobBlockCoord(new_z);
      int fish_scan_y = mobBlockCoord(new_y);
      int water_surface = 0;
      int scan_top = fish_scan_y + 12;
      int scan_bottom = fish_scan_y - 10;
      if (scan_top > 319) scan_top = 319;
      if (scan_bottom < 0) scan_bottom = 0;
      for (int y = scan_top; y >= scan_bottom; y--) {
        uint16_t block = getBlockAt2(ws_fish_block_x, y, ws_fish_block_z, mob_data[i].dimension);
        uint16_t block_above = getBlockAt2(ws_fish_block_x, y + 1, ws_fish_block_z, mob_data[i].dimension);
        // Water surface is a water block with non-water (or air) above it
        if (isWaterBlock(block) && !isWaterBlock(block_above)) {
          water_surface = y;
          break;
        }
      }
      
      // Fish must stay at least 1 block below the water surface
      if (water_surface > 0) {
        double max_y = (double)water_surface - 1.0; // At least 1 block under surface
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
              double angle = atan2(dz, dx) * 256.0 / (2.0 * 3.14159265358979);
              mob_data[i].yaw_store = (uint8_t)(((int)(angle + 0.5) - 75) & 255);
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

      // Zombies move 2x faster and have attack cooldown
      double zombie_move = (mob_data[i].type == 145) ? move_amount * 2.0 : move_amount;

      double dist_to_player = sqrt(closest_dist_double);
      double y_diff = fabs(old_y - closest_player->y);

      // Zombified piglins should not acquire/chase from as far away as zombies.
      double vision_range = mob_data[i].type == 148 ? 10.0 : 16.0;
      double attack_range = mob_data[i].type == 148 ? 2.0 : 3.0;

      // If we're within attack range, hurt the player
      if (dist_to_player < attack_range && y_diff < 2.0) {
        // Attack cooldown check - zombies can't attack more than once per second
        if (mob_data[i].move_timer <= 0) {
          // Only attack if the player is still connected
          if (closest_player->client_fd != -1) {
            hurtEntity(closest_player->client_fd, entity_id, D_generic, 1);
          }
          mob_data[i].move_timer = 20; // 1 second cooldown (20 ticks)
        }
      }

      // Decrement attack cooldown timer
      if (mob_data[i].move_timer > 0) mob_data[i].move_timer--;

      // Only move towards player if within vision range
      if (dist_to_player > vision_range) {
        continue;
      }

      // Move towards the closest player with persistent direction
      double dx = closest_player->x - old_x;
      double dz = closest_player->z - old_z;
      double len = sqrt(dx * dx + dz * dz);

      if (len > 0.001) {
        // Normalize and scale movement for smooth pursuit
        double move_x = (dx / len) * zombie_move;
        double move_z = (dz / len) * zombie_move;

        new_x += move_x;
        new_z += move_z;

        // Calculate yaw from direction
        double angle = atan2(dz, dx) * 256.0 / (2.0 * 3.14159265358979);
        yaw = (uint8_t)(((int)(angle + 0.5) - 75) & 255);
      }

    }

    // Collision + one-block step-up. Check the intended horizontal move before
    // cancelling individual axes; otherwise mobs never get a chance to climb.
    // Fish keep their vertical movement, land mobs start at old_y
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
    }

    if (!is_fish && !canMobOccupyPosition(new_x, old_y, new_z, mob_data[i].dimension)) {
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
      if (mob_data[j].dimension != mob_data[i].dimension) continue;
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
        if (player_data[j].dimension != mob_data[i].dimension) continue;
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
      broadcastPlayerEquipment(player);
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
