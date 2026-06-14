#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#ifdef ESP_PLATFORM
  #include "lwip/sockets.h"
  #include "lwip/netdb.h"
  #include "esp_task_wdt.h"
#else
  #ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
  #else
    #include <arpa/inet.h>
  #endif
  #include <unistd.h>
#endif

#include "globals.h"
#include "tools.h"
#include "varnum.h"
#include "registries.h"
#include "worldgen.h"
#include "crafting.h"
#include "procedures.h"
#include "packets.h"
#include "chunk_generator.h"
#include "special_block.h"
#include "creative_mode.h"
#include "config.h"

#ifdef B_end_portal
#define HAVE_B_END_PORTAL 1
#else
#define HAVE_B_END_PORTAL 0
#define B_end_portal B_air
#endif

#define BLOCK_ENTITY_TYPE_CHEST 1
#define BLOCK_ENTITY_TYPE_END_PORTAL 14

static const char *dimension_name(uint8_t dimension) {
  switch (dimension) {
    case DIMENSION_NETHER: return "minecraft:the_nether";
    case DIMENSION_END: return "minecraft:the_end";
    case DIMENSION_OVERWORLD:
    default: return "minecraft:overworld";
  }
}

static void write_dimension_name(int client_fd, uint8_t dimension) {
  const char *name = dimension_name(dimension);
  writeVarInt(client_fd, strlen(name));
  send_all(client_fd, name, strlen(name));
}

// Case-insensitive string comparison for item lookup
static int creative_strcasecmp(const char *s1, const char *s2) {
  if (!s1 || !s2) return -1;
  for (; *s1 && *s2; s1++, s2++) {
    int c1 = tolower((unsigned char)*s1);
    int c2 = tolower((unsigned char)*s2);
    if (c1 != c2) return c1 - c2;
  }
  return *s1 - *s2;
}

static PlayerAppearance *findPlayerAppearanceByUuid(const uint8_t *uuid) {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (memcmp(player_data[i].uuid, uuid, 16) != 0) continue;
    return &player_appearance[i];
  }
  return NULL;
}

static void writePlayerProfileProperties(int client_fd, PlayerAppearance *appearance) {
  int property_count = (appearance && appearance->has_texture) ? 1 : 0;
  writeVarInt(client_fd, property_count);
  if (!property_count) return;

  static const char property_name[] = "textures";

  writeVarInt(client_fd, sizeof(property_name) - 1);
  send_all(client_fd, property_name, sizeof(property_name) - 1);

  writeVarInt(client_fd, appearance->texture_value_len);
  send_all(client_fd, appearance->texture_value, appearance->texture_value_len);

  writeByte(client_fd, appearance->has_signature);
  if (!appearance->has_signature) return;

  writeVarInt(client_fd, appearance->texture_signature_len);
  send_all(client_fd, appearance->texture_signature, appearance->texture_signature_len);
}

static void writeItemSlot(int client_fd, uint8_t count, uint16_t item) {
  if (count == 0 || item == 0) {
    writeVarInt(client_fd, 0);
    return;
  }

  writeVarInt(client_fd, count);
  writeVarInt(client_fd, item);
  writeVarInt(client_fd, 0);
  writeVarInt(client_fd, 0);
}

static uint8_t isIntegerToken(const uint8_t *buffer, int start, int end) {
  if (start >= end) return false;

  int i = start;
  if (buffer[i] == '+' || buffer[i] == '-') i++;
  if (i >= end) return false;

  for (; i < end; i++) {
    if (buffer[i] < '0' || buffer[i] > '9') return false;
  }

  return true;
}

static uint8_t isCommandBoundary(const uint8_t *buffer, int offset) {
  return buffer[offset] == '\0' || buffer[offset] == ' ';
}

static const char *gameModeName(uint8_t gamemode) {
  switch (gamemode) {
    case 0: return "survival";
    case 1: return "creative";
    case 2: return "adventure";
    case 3: return "spectator";
    default: return "unknown";
  }
}

static uint8_t parseGameModeToken(const char *token, uint8_t *gamemode) {
  if (!token || token[0] == '\0') return false;

  if (!strcmp(token, "0") || !creative_strcasecmp(token, "survival") || !creative_strcasecmp(token, "s")) {
    *gamemode = 0;
    return true;
  }
  if (!strcmp(token, "1") || !creative_strcasecmp(token, "creative") || !creative_strcasecmp(token, "c")) {
    *gamemode = 1;
    return true;
  }
  if (!strcmp(token, "2") || !creative_strcasecmp(token, "adventure") || !creative_strcasecmp(token, "a")) {
    *gamemode = 2;
    return true;
  }
  if (!strcmp(token, "3") || !creative_strcasecmp(token, "spectator") || !creative_strcasecmp(token, "sp")) {
    *gamemode = 3;
    return true;
  }

  return false;
}

static uint8_t parseTimeToken(const char *token, int *ticks) {
  if (!token || token[0] == '\0') return false;

  if (!creative_strcasecmp(token, "day")) {
    *ticks = 1000;
    return true;
  }
  if (!creative_strcasecmp(token, "noon")) {
    *ticks = 6000;
    return true;
  }
  if (!creative_strcasecmp(token, "night")) {
    *ticks = 13000;
    return true;
  }
  if (!creative_strcasecmp(token, "midnight")) {
    *ticks = 18000;
    return true;
  }

  char *end = NULL;
  long parsed = strtol(token, &end, 10);
  if (end == token || *end != '\0') return false;
  *ticks = (int)parsed;
  return true;
}

static uint16_t normalizeWorldTime(int ticks) {
  ticks %= 24000;
  if (ticks < 0) ticks += 24000;
  return (uint16_t)ticks;
}

static void broadcastTimeUpdate(void) {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (player_data[i].client_fd == -1) continue;
    if (player_data[i].flags & 0x20) continue;
    sc_updateTime(player_data[i].client_fd, world_time);
  }
}

static void syncConfiguredGameModeToPlayers(void) {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (player_data[i].client_fd == -1) continue;
    if (player_data[i].flags & 0x20) continue;

    syncPlayerNoclipState(&player_data[i]);

    uint8_t gamemode = isPlayerNoclipEnabled(&player_data[i]) ? 3 : getConfiguredGameMode();
    for (int viewer = 0; viewer < MAX_PLAYERS; viewer++) {
      if (viewer == i) continue;
      if (player_data[viewer].client_fd == -1) continue;
      if (player_data[viewer].flags & 0x20) continue;
      sc_playerInfoUpdateUpdateGamemode(player_data[viewer].client_fd, player_data[i], gamemode);
    }
  }
}

// S->C Status Response (server list ping)
int sc_statusResponse (int client_fd) {

  char max_players_str[16];
  snprintf(max_players_str, sizeof(max_players_str), "%d", config.max_players);
  char online_players_str[16];
  // Subtract 1 because the pinging client is not yet logged in
  snprintf(online_players_str, sizeof(online_players_str), "%u", client_count > 0 ? client_count - 1 : 0);

  // Build the full JSON for debugging
  // Buffer large enough for MOTD + favicon data URI
  char json[FAVICON_MAX_LEN + 1024];
  if (favicon_len > 0) {
    snprintf(json, sizeof(json),
      "{\"version\":{\"name\":\"1.21.8\",\"protocol\":772},"
      "\"players\":{\"max\":%s,\"online\":%s},"
      "\"description\":{\"text\":\"%.*s\"},"
      "\"favicon\":\"%.*s\"}",
      max_players_str, online_players_str, motd_len, motd, favicon_len, favicon);
  } else {
    snprintf(json, sizeof(json),
      "{\"version\":{\"name\":\"1.21.8\",\"protocol\":772},"
      "\"players\":{\"max\":%s,\"online\":%s},"
      "\"description\":{\"text\":\"%.*s\"}}",
      max_players_str, online_players_str, motd_len, motd);
  }

  printf("Sending Status Response: %s\n", json);

  uint16_t string_len = strlen(json);

  startPacket(client_fd, 0x00);

  writeVarInt(client_fd, string_len);
  send_all(client_fd, json, string_len);

  endPacket(client_fd);

  return 0;
}

// C->S Handshake
int cs_handshake (int client_fd) {
  printf("Received Handshake:\n");

  printf("  Protocol version: %d\n", (int)readVarInt(client_fd));
  readString(client_fd);
  if (recv_count == -1) return 1;
  printf("  Server address: %s\n", recv_buffer);
  printf("  Server port: %u\n", readUint16(client_fd));
  int intent = readVarInt(client_fd);
  if (intent == VARNUM_ERROR) return 1;
  printf("  Intent: %d\n\n", intent);
  setClientState(client_fd, intent);

  return 0;
}

// C->S Login Start
int cs_loginStart (int client_fd, uint8_t *uuid, char *name) {
  printf("Received Login Start:\n");

  readString(client_fd);
  if (recv_count == -1) return 1;
  strncpy(name, (char *)recv_buffer, 16 - 1);
  name[16 - 1] = '\0';
  printf("  Player name: %s\n", name);
  recv_count = recv_all(client_fd, recv_buffer, 16, false);
  if (recv_count == -1) return 1;
  memcpy(uuid, recv_buffer, 16);
  printf("  Player UUID: ");
  for (int i = 0; i < 16; i ++) printf("%x", uuid[i]);
  printf("\n\n");

  return 0;
}

// S->C Set Compression
int sc_setCompression (int client_fd, int threshold) {
  printf("Sending Set Compression (threshold: %d)...\n\n", threshold);

  startPacket(client_fd, 0x03);
  writeVarInt(-1, threshold);
  endPacket(client_fd);

  setCompressionThreshold(client_fd, threshold);

  return 0;
}

// S->C Login Success
int sc_loginSuccess (int client_fd, uint8_t *uuid, char *name) {
  printf("Sending Login Success...\n\n");

  PlayerAppearance *appearance = findPlayerAppearanceByUuid(uuid);
  uint8_t name_length = strlen(name);
  startPacket(client_fd, 0x02);
  send_all(client_fd, uuid, 16);
  writeVarInt(client_fd, name_length);
  send_all(client_fd, name, name_length);
  writePlayerProfileProperties(client_fd, appearance);

  endPacket(client_fd);

  return 0;
}

int cs_clientInformation (int client_fd) {
  int tmp;
  uint8_t skin_parts;
  uint8_t main_hand;
  printf("Received Client Information:\n");
  readString(client_fd);
  if (recv_count == -1) return 1;
  printf("  Locale: %s\n", recv_buffer);
  tmp = readByte(client_fd);
  if (recv_count == -1) return 1;
  printf("  View distance: %d\n", tmp);
  tmp = readVarInt(client_fd);
  if (recv_count == -1) return 1;
  printf("  Chat mode: %d\n", tmp);
  tmp = readByte(client_fd);
  if (recv_count == -1) return 1;
  if (tmp) printf("  Chat colors: on\n");
  else printf("  Chat colors: off\n");
  tmp = readByte(client_fd);
  if (recv_count == -1) return 1;
  skin_parts = (uint8_t)tmp;
  printf("  Skin parts: %d\n", skin_parts);
  tmp = readVarInt(client_fd);
  if (recv_count == -1) return 1;
  main_hand = tmp ? 1 : 0;
  if (main_hand) printf("  Main hand: right\n");
  else printf("  Main hand: left\n");
  tmp = readByte(client_fd);
  if (recv_count == -1) return 1;
  if (tmp) printf("  Text filtering: on\n");
  else printf("  Text filtering: off\n");
  tmp = readByte(client_fd);
  if (recv_count == -1) return 1;
  if (tmp) printf("  Allow listing: on\n");
  else printf("  Allow listing: off\n");
  tmp = readVarInt(client_fd);
  if (recv_count == -1) return 1;
  printf("  Particles: %d\n\n", tmp);

  updatePlayerAppearanceClientSettings(client_fd, skin_parts, main_hand);
  return 0;
}

// S->C Clientbound Known Packs
int sc_knownPacks (int client_fd) {
  printf("Sending Server's Known Packs\n\n");
  char known_packs[] = {
    0x0e, 0x01, 0x09, 0x6d, 0x69, 0x6e,
    0x65, 0x63, 0x72, 0x61, 0x66, 0x74, 0x04, 0x63,
    0x6f, 0x72, 0x65, 0x06, 0x31, 0x2e, 0x32, 0x31,
    0x2e, 0x38
  };
  startPacket(client_fd, 0x0E);
  send_all(client_fd, known_packs + 1, 23);
  endPacket(client_fd);
  return 0;
}

// C->S Serverbound Plugin Message
int cs_pluginMessage (int client_fd) {
  printf("Received Plugin Message:\n");
  readString(client_fd);
  if (recv_count == -1) return 1;
  printf("  Channel: \"%s\"\n", recv_buffer);
  if (strcmp((char *)recv_buffer, "minecraft:brand") == 0) {
    readString(client_fd);
    if (recv_count == -1) return 1;
    printf("  Brand: \"%s\"\n", recv_buffer);
  }
  printf("\n");
  return 0;
}

// S->C Clientbound Plugin Message
int sc_sendPluginMessage (int client_fd, const char *channel, const uint8_t *data, size_t data_len) {
  printf("Sending Plugin Message\n\n");
  int channel_len = (int)strlen(channel);

  startPacket(client_fd, 0x01);

  writeVarInt(client_fd, channel_len);
  send_all(client_fd, channel, channel_len);

  writeVarInt(client_fd, data_len);
  send_all(client_fd, data, data_len);

  endPacket(client_fd);

  return 0;
}

// S->C Finish Configuration
int sc_finishConfiguration (int client_fd) {
  startPacket(client_fd, 0x03);
  endPacket(client_fd);
  return 0;
}

// S->C Login (play)
int sc_loginPlay (int client_fd, uint8_t dimension) {

  startPacket(client_fd, 0x2B);
  // entity id
  writeUint32(client_fd, client_fd);
  // hardcore
  writeByte(client_fd, false);
  // dimensions
  writeVarInt(client_fd, 3);
  write_dimension_name(client_fd, DIMENSION_OVERWORLD);
  write_dimension_name(client_fd, DIMENSION_NETHER);
  write_dimension_name(client_fd, DIMENSION_END);
  // maxplayers
  writeVarInt(client_fd, MAX_PLAYERS);
  // view distance
  writeVarInt(client_fd, VIEW_DISTANCE);
  // sim distance
  writeVarInt(client_fd, VIEW_DISTANCE);
  // reduced debug info
  writeByte(client_fd, 0);
  // respawn screen
  writeByte(client_fd, true);
  // limited crafting
  writeByte(client_fd, false);
  // dimension id (from server-sent registries)
  writeVarInt(client_fd, dimension);
  // dimension name
  write_dimension_name(client_fd, dimension);
  // hashed seed
  writeUint64(client_fd, 0x0123456789ABCDEF);
  // gamemode
  writeByte(client_fd, getConfiguredGameMode());
  // previous gamemode
  writeByte(client_fd, 0xFF);
  // is debug
  writeByte(client_fd, 0);
  // is flat
  writeByte(client_fd, 0);
  // has death location
  writeByte(client_fd, 0);
  // portal cooldown
  writeVarInt(client_fd, 10);
  // sea level
  writeVarInt(client_fd, 63);
  // secure chat
  writeByte(client_fd, 0);

  endPacket(client_fd);

  return 0;

}

// S->C Synchronize Player Position
int sc_synchronizePlayerPosition (int client_fd, double x, double y, double z, float yaw, float pitch) {

  startPacket(client_fd, 0x41);

  // Teleport ID
  writeVarInt(client_fd, -1);

  // Position
  writeDouble(client_fd, x);
  writeDouble(client_fd, y);
  writeDouble(client_fd, z);

  // Velocity
  writeDouble(client_fd, 0);
  writeDouble(client_fd, 0);
  writeDouble(client_fd, 0);

  // Angles (Yaw/Pitch)
  writeFloat(client_fd, yaw);
  writeFloat(client_fd, pitch);

  // Flags
  writeUint32(client_fd, 0);

  endPacket(client_fd);

  return 0;

}

// S->C Set Default Spawn Position
int sc_setDefaultSpawnPosition (int client_fd, int64_t x, int64_t y, int64_t z) {

  startPacket(client_fd, 0x5A);

  writeUint64(client_fd, ((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF));
  writeFloat(client_fd, 0);

  endPacket(client_fd);

  return 0;
}

// S->C Player Abilities (clientbound)
int sc_playerAbilities (int client_fd, uint8_t flags) {

  startPacket(client_fd, 0x39);

  writeByte(client_fd, flags);
  writeFloat(client_fd, 0.05f);
  writeFloat(client_fd, 0.1f);

  endPacket(client_fd);

  return 0;
}

// S->C Update Time
int sc_updateTime (int client_fd, uint64_t ticks) {

  startPacket(client_fd, 0x6A);

  uint64_t world_age = get_program_time() / 50000;
  writeUint64(client_fd, world_age);
  writeUint64(client_fd, ticks);
  writeByte(client_fd, true);

  endPacket(client_fd);

  return 0;
}

// S->C Game Event
int sc_gameEvent (int client_fd, uint8_t event, float value) {
  startPacket(client_fd, 0x22);
  writeByte(client_fd, event);
  writeFloat(client_fd, value);
  endPacket(client_fd);
  return 0;
}

// S->C Game Event 13 (Start waiting for level chunks)
int sc_startWaitingForChunks (int client_fd) {
  return sc_gameEvent(client_fd, 13, 0.0f);
}

// S->C Chunk Batch Start (1.21.5+)
int sc_chunkBatchStart (int client_fd) {
  startPacket(client_fd, 0x0C);
  endPacket(client_fd);
  return 0;
}

// S->C Chunk Batch Finished (1.21.5+)
int sc_chunkBatchFinished (int client_fd, int batchSize) {
  startPacket(client_fd, 0x0B);
  writeVarInt(client_fd, batchSize);
  endPacket(client_fd);
  return 0;
}

// S->C Set Center Chunk
int sc_setCenterChunk (int client_fd, int x, int y) {
  startPacket(client_fd, 0x57);
  writeVarInt(client_fd, x);
  writeVarInt(client_fd, y);
  endPacket(client_fd);
  return 0;
}

// Compute block light for a single chunk section from natural light-emitting blocks
void compute_section_block_light(const uint16_t section[4096], uint8_t light_out[2048]) {
  memset(light_out, 0, 2048);
  for (int i = 0; i < 4096; i++) {
    uint16_t block = section[i];
    if (block == B_shroomlight || block == B_glowstone || block == B_lava) goto full;
    if (block == B_fire) goto full;
    if (block == B_magma_block) goto full;
    if (block == B_torch) goto full;
  }
  return;
full:
  memset(light_out, 0xFF, 2048);
}

// S->C Chunk Data and Update Light
int sc_chunkDataAndUpdateLight (int client_fd, int _x, int _z, uint8_t dimension) {
  chunk_debug_record_stream_request(client_fd, _x, _z);

  // Backpressure: if low-priority chunk backlog is already large, delay chunk packets
  // so gameplay packets (block updates, interactions) stay responsive.
  size_t chunk_queue_bytes = get_client_send_queue_bytes(client_fd);
  if (chunk_queue_bytes > (1024 * 1024)) {
    chunk_debug_record_backpressure_skip(client_fd, chunk_queue_bytes);
    request_chunk_generation(_x, _z, dimension);
    return 1;
  }

  // Use a dynamic buffer to build the chunk data part
  // Grows as needed to handle large chunks
  static THREAD_LOCAL uint8_t *data_buf = NULL;
  static THREAD_LOCAL size_t data_buf_capacity = 0;
  static THREAD_LOCAL uint16_t (*cached_sections)[4096] = NULL;
  static THREAD_LOCAL uint8_t *cached_biomes = NULL;
  int data_offset = 0;

  if (cached_sections == NULL) {
    cached_sections = malloc(20 * sizeof(*cached_sections));
    if (cached_sections == NULL) return 1;
  }
  if (cached_biomes == NULL) {
    cached_biomes = malloc(20 * sizeof(*cached_biomes));
    if (cached_biomes == NULL) return 1;
  }

  // Ensure buffer is large enough
  if (data_buf == NULL || data_buf_capacity < 32768) {
    void *new_buf = realloc(data_buf, 32768);
    if (new_buf) {
      data_buf = (uint8_t *)new_buf;
      data_buf_capacity = 32768;
    } else {
      return 1;
    }
  }

  // Helper to grow buffer if needed
  #define ENSURE_SPACE(needed) do { \
    if (data_offset + (needed) > (int)data_buf_capacity) { \
      size_t new_cap = data_buf_capacity * 2; \
      while (new_cap < (size_t)(data_offset + (needed))) new_cap *= 2; \
      void *nb = realloc(data_buf, new_cap); \
      if (!nb) return 1; \
      data_buf = (uint8_t *)nb; \
      data_buf_capacity = new_cap; \
    } \
  } while(0)

  int x = _x * 16, z = _z * 16;

  // Determine section count per dimension
  // Overworld:  min_y=-64, height=384 -> 24 sections
  // Nether/End: min_y=0,   height=256 -> 16 sections
  int zero_min_y_dimension = (dimension == DIMENSION_NETHER || dimension == DIMENSION_END);
  int terrain_sections = zero_min_y_dimension ? 16 : 20;
  int bedrock_sections = zero_min_y_dimension ? 0  : 4;
  int total_sections   = bedrock_sections + terrain_sections;
  int light_bits       = total_sections + 2;  // +2 for border sections

  // Try to get cached chunk data
  if (!get_cached_chunk_copy(_x, _z, dimension, cached_sections, cached_biomes)) {
    // Not cached yet: queue generation and skip sending this chunk for now.
    // This keeps movement packet handling non-blocking.
    chunk_debug_record_cache_miss_skip(client_fd);
    request_chunk_generation(_x, _z, dimension);
    return 1;
  }

  // 1. Send chunk sections
  // send bedrock sections at the bottom (overworld only, Y=-64 to -1)
  uint32_t val;
  for (int i = 0; i < bedrock_sections; i ++) {
    if (data_offset + 32 > (int)data_buf_capacity) {
      fprintf(stderr, "ERROR: Chunk data buffer overflow at bedrock sections\n");
      return 1;
    }
    // block count
    data_buf[data_offset++] = 4096 >> 8;
    data_buf[data_offset++] = 4096 & 0xFF;
    // block bits
    data_buf[data_offset++] = 0;
    // block palette (bedrock = 85)
    val = 85;
    while (true) {
      if (data_offset + 32 > (int)data_buf_capacity) {
        fprintf(stderr, "ERROR: Chunk data buffer overflow at bedrock palette\n");
        return 1;
      }
      if ((val & ~SEGMENT_BITS) == 0) { data_buf[data_offset++] = val; break; }
      data_buf[data_offset++] = (val & SEGMENT_BITS) | CONTINUE_BIT;
      val >>= 7;
    }
    // biome bits
    data_buf[data_offset++] = 0;
    // biome palette (0)
    data_buf[data_offset++] = 0;
  }

  // A helper to encode a single section into the data buffer
  // (captures data_offset and data_buf by scope)
  #define ENCODE_SECTION(section_data, biome) do { \
    uint16_t* _sd = (section_data); \
    uint8_t _biome = (biome); \
    uint16_t _first = _sd[0]; \
    uint8_t _uniform = 1; \
    for (int _j = 1; _j < 4096; _j++) { if (_sd[_j] != _first) { _uniform = 0; break; } } \
    if (_uniform && !(_first & 0x8000)) { \
      uint16_t _bc = (_first == 0) ? 0 : 4096; \
      data_buf[data_offset++] = _bc >> 8; \
      data_buf[data_offset++] = _bc & 0xFF; \
      data_buf[data_offset++] = 0; \
      val = block_palette[_first]; \
      while (1) { \
        if ((val & ~SEGMENT_BITS) == 0) { data_buf[data_offset++] = val; break; } \
        data_buf[data_offset++] = (val & SEGMENT_BITS) | CONTINUE_BIT; \
        val >>= 7; \
      } \
    } else { \
      uint16_t _pal[320]; \
      uint16_t _palidx[sizeof(B_to_I) / sizeof(B_to_I[0])]; \
      int _psize = 0; \
      memset(_palidx, 0xFF, sizeof(_palidx)); \
      for (int _j = 0; _j < 4096; _j++) { \
        uint16_t _b = _sd[_j]; \
        if (_b & 0x8000) { \
          int _found = 0; \
          for (int _k = 0; _k < _psize; _k++) { if (_pal[_k] == _b) { _found = 1; break; } } \
          if (!_found) _pal[_psize++] = _b; \
        } else { \
          if (_b < (sizeof(_palidx) / sizeof(_palidx[0])) && _palidx[_b] == 0xFFFF) { _palidx[_b] = _psize; _pal[_psize++] = _b; } \
        } \
      } \
      uint16_t _bc = 0; \
      for (int _j = 0; _j < 4096; _j++) { \
        uint16_t _b = _sd[_j]; \
        if ((_b & 0x8000) ? (_b & 0x1FF) : _b) _bc++; \
      } \
      data_buf[data_offset++] = _bc >> 8; \
      data_buf[data_offset++] = _bc & 0xFF; \
      data_buf[data_offset++] = 8; \
      val = _psize; \
      while (1) { \
        if ((val & ~SEGMENT_BITS) == 0) { data_buf[data_offset++] = val; break; } \
        data_buf[data_offset++] = (val & SEGMENT_BITS) | CONTINUE_BIT; \
        val >>= 7; \
      } \
      ENSURE_SPACE(_psize * 3 + 4096); \
      for (int _j = 0; _j < _psize; _j++) { \
        uint16_t _pv = _pal[_j]; \
        if (_pv & 0x8000) { \
          uint8_t _t = _pv & 0x1FF; \
          if (_t == B_chest) { \
            uint8_t _d = (_pv >> 9) & 3; \
            val = get_oriented_state_id(_t, _d); \
          } else if (is_stair_block(_t)) { \
            uint8_t _d = (_pv >> 9) & 3; \
            uint8_t _h = (_pv >> 11) & 1; \
            val = get_stair_state_id(_t, _h, _d); \
          } else if (is_trapdoor_block(_t)) { \
            uint8_t _d = (_pv >> 9) & 3; \
            uint8_t _h = (_pv >> 11) & 1; \
            uint8_t _o = (_pv >> 12) & 1; \
            val = get_trapdoor_state_id(_t, _o, _d, _h); \
          } else if (is_fence_block(_t)) { \
            uint8_t _n = (_pv >> 9) & 1; \
            uint8_t _e = (_pv >> 10) & 1; \
            uint8_t _s = (_pv >> 11) & 1; \
            uint8_t _w = (_pv >> 12) & 1; \
            uint8_t _conn = _n | (_e << 1) | (_s << 2) | (_w << 3); \
            val = get_fence_state_id(_t, _conn); \
          } else if (is_horizontal_facing_block(_t)) { \
            uint8_t _d = (_pv >> 9) & 3; \
            val = get_horizontal_state_id(_t, _d); \
          } else if (is_bed_block(_t)) { \
            uint8_t _d = (_pv >> 9) & 3; \
            uint8_t _h = (_pv >> 11) & 1; \
            uint8_t _o = (_pv >> 12) & 1; \
            val = get_bed_state_id(_t, _h, _o, _d); \
          } else if (_t == B_barrel) { \
            uint8_t _d = (_pv >> 9) & 7; \
            val = get_oriented_state_id(_t, _d); \
          } else if (_t == B_lantern) { \
            uint8_t _h = (_pv >> 9) & 1; \
            val = block_palette[_t] - (_h ? 2 : 0); \
          } else { \
            uint8_t _d = (_pv >> 9) & 3; \
            uint8_t _h = (_pv >> 12) & 1; \
            uint8_t _o = (_pv >> 13) & 1; \
            uint8_t _u = (_pv >> 14) & 1; \
            val = get_door_state_id(_t, _u, _o, _d, _h); \
          } \
        } else { \
          val = block_palette[_pv]; \
        } \
        while (1) { \
          if ((val & ~SEGMENT_BITS) == 0) { data_buf[data_offset++] = val; break; } \
          data_buf[data_offset++] = (val & SEGMENT_BITS) | CONTINUE_BIT; \
          val >>= 7; \
        } \
      } \
      ENSURE_SPACE(4096); \
      for (int _j = 0; _j < 4096; _j++) { \
        uint16_t _b = _sd[_j]; \
        if (_b & 0x8000) { \
          int _idx; \
          for (_idx = 0; _idx < _psize; _idx++) { if (_pal[_idx] == _b) break; } \
          data_buf[data_offset++] = (uint8_t)_idx; \
        } else { \
          data_buf[data_offset++] = _palidx[_b]; \
        } \
      } \
    } \
    data_buf[data_offset++] = 0; \
    data_buf[data_offset++] = _biome; \
  } while(0)

  // send terrain sections using cached data
  for (int i = 0; i < terrain_sections; i ++) {
    if (data_offset + 32 > (int)data_buf_capacity) {
      fprintf(stderr, "ERROR: Chunk data buffer overflow at terrain sections\n");
      return 1;
    }
    ENCODE_SECTION(cached_sections[i], cached_biomes[i]);
  }

  // Scan terrain sections for blocks that need block entities in the chunk data
  // packet to render correctly. Chests are invisible without one, and End portal
  // blocks need one for the animated portal surface renderer.
  int chest_count = 0;
  int chest_wx[64], chest_wy[64], chest_wz[64];
  int end_portal_count = 0;
  int end_portal_wx[64], end_portal_wy[64], end_portal_wz[64];
  for (int _s = 0; _s < terrain_sections; _s++) {
    int section_y = (dimension == DIMENSION_NETHER) ? _s * 16 : _s * 16;
    for (int _j = 0; _j < 4096; _j++) {
      uint16_t raw = cached_sections[_s][_j];
      uint16_t block_type = (raw & 0x8000) ? (raw & 0x1FF) : raw;
      if (block_type != B_chest && (!HAVE_B_END_PORTAL || block_type != B_end_portal)) continue;

      int addr = (_j & ~7) | (7 - (_j & 7));
      int wx = x + (addr & 15);
      int wz = z + ((addr >> 4) & 15);
      int wy = section_y + (addr >> 8);

      if (block_type == B_chest && chest_count < 64) {
        chest_wx[chest_count] = wx;
        chest_wz[chest_count] = wz;
        chest_wy[chest_count] = wy;
        chest_count++;
      } else if (HAVE_B_END_PORTAL && block_type == B_end_portal && end_portal_count < 64) {
        end_portal_wx[end_portal_count] = wx;
        end_portal_wz[end_portal_count] = wz;
        end_portal_wy[end_portal_count] = wy;
        end_portal_count++;
      }
    }
  }

  int chunk_data_size = data_offset;

  // 2. Build the full packet
  startPacket(client_fd, 0x27);

  writeUint32(client_fd, _x);
  writeUint32(client_fd, _z);

  writeVarInt(client_fd, 0); // omit heightmaps

  writeVarInt(client_fd, chunk_data_size);
  send_all(client_fd, data_buf, chunk_data_size);

  // Block entities section — needed for some blocks to render correctly.
  writeVarInt(client_fd, chest_count + end_portal_count);
  for (int _be = 0; _be < chest_count; _be++) {
    writeByte(client_fd, ((chest_wx[_be] & 15) << 4) | (chest_wz[_be] & 15));
    writeByte(client_fd, (chest_wy[_be] >> 8) & 0xFF);
    writeByte(client_fd, chest_wy[_be] & 0xFF);
    writeVarInt(client_fd, BLOCK_ENTITY_TYPE_CHEST);
    // Anonymous network NBT: TAG_Compound type byte, then compound data (no name),
    // terminated by TAG_End.
    writeByte(client_fd, 0x0A); // TAG_Compound type
    writeByte(client_fd, 0x00); // TAG_End — empty compound
  }
  for (int _be = 0; _be < end_portal_count; _be++) {
    writeByte(client_fd, ((end_portal_wx[_be] & 15) << 4) | (end_portal_wz[_be] & 15));
    writeByte(client_fd, (end_portal_wy[_be] >> 8) & 0xFF);
    writeByte(client_fd, end_portal_wy[_be] & 0xFF);
    writeVarInt(client_fd, BLOCK_ENTITY_TYPE_END_PORTAL);
    writeByte(client_fd, 0x0A); // TAG_Compound type
    writeByte(client_fd, 0x00); // TAG_End — empty compound
  }

  // Light data
  // Sky light mask: all sections + 2 border sections
  uint64_t light_mask = ((uint64_t)1 << light_bits) - 1;
  writeVarInt(client_fd, 1);
  writeUint64(client_fd, light_mask);
  // Block light mask: all sections for nether (for worldgen emitters like shroomlight/lava)
  if (dimension == DIMENSION_NETHER) {
    writeVarInt(client_fd, 1);
    writeUint64(client_fd, light_mask);
  } else {
    writeVarInt(client_fd, 0);
  }
  // Empty sky light mask: empty
  writeVarInt(client_fd, 0);
  // Empty block light mask: empty
  writeVarInt(client_fd, 0);

  // Sky light arrays
  static THREAD_LOCAL uint8_t *light_sky = NULL;
  static THREAD_LOCAL uint8_t *light_dark = NULL;
  if (light_sky == NULL) {
    light_sky = malloc(2048);
    if (light_sky == NULL) return 1;
  }
  if (light_dark == NULL) {
    light_dark = malloc(2048);
    if (light_dark == NULL) return 1;
  }
  if (dimension == DIMENSION_NETHER) {
    memset(light_sky, 0x44, 2048);
    memset(light_dark, 0, 2048);
  } else {
    memset(light_sky, 0xFF, 2048);
    memset(light_dark, 0, 2048);
  }

  writeVarInt(client_fd, light_bits);
  // One section below world: dark
  writeVarInt(client_fd, 2048);
  send_all(client_fd, light_dark, 2048);
  // In-world sections: sky lit for overworld, dim ambient for nether
  for (int i = 0; i < total_sections; i ++) {
    writeVarInt(client_fd, 2048);
    send_all(client_fd, light_sky, 2048);
  }
  // One section above world: sky lit
  writeVarInt(client_fd, 2048);
  send_all(client_fd, light_sky, 2048);
  // Block light: for nether, compute spherical falloff from light-emitting blocks
  if (dimension == DIMENSION_NETHER) {
    writeVarInt(client_fd, light_bits);
    // One section below world: dark
    writeVarInt(client_fd, 2048);
    send_all(client_fd, light_dark, 2048);
    // In-world sections
    for (int s = 0; s < total_sections; s++) {
      uint8_t block_light[2048];
      compute_section_block_light(cached_sections[s], block_light);
      writeVarInt(client_fd, 2048);
      send_all(client_fd, block_light, 2048);
    }
    // One section above world: dark
    writeVarInt(client_fd, 2048);
    send_all(client_fd, light_dark, 2048);
  } else {
    writeVarInt(client_fd, 0);
  }

  if (endPacket(client_fd) != 0) {
    // Queue full or client state changed; retry this chunk later.
    chunk_debug_record_enqueue_result(client_fd, _x, _z, 0);
    return 1;
  }
  chunk_debug_record_enqueue_result(client_fd, _x, _z, 1);

  // Sending block updates for special blocks and torches.
  // Doors, trapdoors, stairs, chests, and furnaces need explicit state IDs
  // from the unified special block table, so we send precise per-block updates here.
  int block_changes_snapshot_count = 0;
  BlockChange *block_changes_snapshot = copyBlockChangesSnapshot(&block_changes_snapshot_count);

  for (int i = 0; i < block_changes_snapshot_count; i ++) {
    if (block_changes_snapshot[i].block == 0xFF) continue;
    // Only send block updates for blocks in the same dimension as this chunk
    if (block_changes_snapshot[i].dimension != dimension) continue;
    if (
      block_changes_snapshot[i].block != B_torch &&
      block_changes_snapshot[i].block != B_wheat &&
      block_changes_snapshot[i].block != B_lantern &&
      !isOrientedBlock(block_changes_snapshot[i].block) &&
      !isStairBlock(block_changes_snapshot[i].block) &&
      !is_fence_block(block_changes_snapshot[i].block) &&
      !is_horizontal_facing_block(block_changes_snapshot[i].block) &&
      !is_bed_block(block_changes_snapshot[i].block)
      #ifdef ALLOW_DOORS
      && !isDoorBlock(block_changes_snapshot[i].block)
      && !isTrapdoorBlock(block_changes_snapshot[i].block)
      #endif
    ) continue;

    if (block_changes_snapshot[i].x < x || block_changes_snapshot[i].x >= x + 16) {
      if (block_changes_snapshot[i].block == B_chest) {
        if (i + 14 >= block_changes_snapshot_count) continue;
        i += 14;
      } else if (isStairBlock(block_changes_snapshot[i].block) || block_changes_snapshot[i].block == B_furnace || block_changes_snapshot[i].block == B_lantern) i += 1;
      else if (isDoorBlock(block_changes_snapshot[i].block)) i += 2;
      continue;
    }
    if (block_changes_snapshot[i].z < z || block_changes_snapshot[i].z >= z + 16) {
      if (block_changes_snapshot[i].block == B_chest) {
        if (i + 14 >= block_changes_snapshot_count) continue;
        i += 14;
      } else if (isStairBlock(block_changes_snapshot[i].block) || block_changes_snapshot[i].block == B_furnace || block_changes_snapshot[i].block == B_lantern) i += 1;
      else if (isDoorBlock(block_changes_snapshot[i].block)) i += 2;
      continue;
    }

    #ifdef ALLOW_DOORS
    if (isDoorBlock(block_changes_snapshot[i].block)) {
      // Skip upper half of doors - they are sent together with lower half
      uint16_t lower_state = special_block_get_state(block_changes_snapshot[i].x, block_changes_snapshot[i].y - 1, block_changes_snapshot[i].z, block_changes_snapshot[i].dimension);
      if (lower_state != 0) {
        // This is an upper half (lower half exists below), skip it
        // Still need to skip state entries
        if (i + 2 < block_changes_snapshot_count) i += 2;
        continue;
      }

      // Bounds check: ensure i+1 and i+2 are valid indices
      if (i + 2 >= block_changes_snapshot_count) continue;

      // Read state from the unified special block table
      uint16_t state = special_block_get_state(block_changes_snapshot[i].x, block_changes_snapshot[i].y, block_changes_snapshot[i].z, block_changes_snapshot[i].dimension);
      uint8_t open = door_get_open(state);
      uint8_t hinge = door_get_hinge(state);
      uint8_t direction = door_get_direction(state);

      // Send door with proper state (both halves)
      sendDoorUpdate(client_fd, block_changes_snapshot[i].x, block_changes_snapshot[i].y, block_changes_snapshot[i].z, block_changes_snapshot[i].block, 0, open, direction, hinge);
      // Check if upper half exists in special block table
      uint16_t upper_state = special_block_get_state(block_changes_snapshot[i].x, block_changes_snapshot[i].y + 1, block_changes_snapshot[i].z, block_changes_snapshot[i].dimension);
      if (upper_state != 0) {
        // Upper half exists, send it
        sendDoorUpdate(client_fd, block_changes_snapshot[i].x, block_changes_snapshot[i].y + 1, block_changes_snapshot[i].z, block_changes_snapshot[i].block, 1, open, direction, hinge);
      }
      // Skip the next two entries (state data for this door half)
      i += 2;
      continue;
    }
    #endif

    #ifdef ALLOW_DOORS
    if (isTrapdoorBlock(block_changes_snapshot[i].block)) {
      uint16_t state = special_block_get_state(block_changes_snapshot[i].x, block_changes_snapshot[i].y, block_changes_snapshot[i].z, block_changes_snapshot[i].dimension);
      uint8_t open = trapdoor_get_open(state);
      uint8_t half = trapdoor_get_half(state);
      uint8_t direction = trapdoor_get_direction(state);

      sendTrapdoorUpdate(client_fd, block_changes_snapshot[i].x, block_changes_snapshot[i].y, block_changes_snapshot[i].z, block_changes_snapshot[i].block, open, direction, half);
      continue;
    }
    #endif

    if (is_bed_block(block_changes_snapshot[i].block)) {
      uint16_t state = special_block_get_state(block_changes_snapshot[i].x, block_changes_snapshot[i].y, block_changes_snapshot[i].z, block_changes_snapshot[i].dimension);
      uint16_t state_id = get_bed_state_id(
        block_changes_snapshot[i].block,
        bed_get_head(state),
        bed_get_occupied(state),
        bed_get_direction(state)
      );
      sc_blockUpdateState(client_fd, block_changes_snapshot[i].x, block_changes_snapshot[i].y, block_changes_snapshot[i].z, state_id);
      continue;
    }

    if (isOrientedBlock(block_changes_snapshot[i].block)) {
      // Read direction from the unified special block table
      uint16_t state = special_block_get_state(block_changes_snapshot[i].x, block_changes_snapshot[i].y, block_changes_snapshot[i].z, block_changes_snapshot[i].dimension);
      // Barrels use 3 bits for 6-directional facing (0-5); other oriented blocks use 2 bits (0-3)
      uint8_t direction = (block_changes_snapshot[i].block == B_barrel)
        ? barrel_get_direction(state)
        : oriented_get_direction(state);

      if (block_changes_snapshot[i].block == B_chest) {
        sendOrientedUpdate(client_fd, block_changes_snapshot[i].x, block_changes_snapshot[i].y, block_changes_snapshot[i].z, block_changes_snapshot[i].block, direction);
        if (i + 14 >= block_changes_snapshot_count) continue;
        i += 14;
      } else {
        sendOrientedUpdate(client_fd, block_changes_snapshot[i].x, block_changes_snapshot[i].y, block_changes_snapshot[i].z, block_changes_snapshot[i].block, direction);
        i += 1;
      }
      continue;
    }

    if (isStairBlock(block_changes_snapshot[i].block)) {
      // Bounds check: ensure i+1 is a valid index
      if (i + 1 >= block_changes_snapshot_count) continue;

      // Read state from the unified special block table
      uint16_t state = special_block_get_state(block_changes_snapshot[i].x, block_changes_snapshot[i].y, block_changes_snapshot[i].z, block_changes_snapshot[i].dimension);
      uint8_t half = stair_get_half(state);
      uint8_t direction = stair_get_direction(state);

      sendStairUpdate(client_fd, block_changes_snapshot[i].x, block_changes_snapshot[i].y, block_changes_snapshot[i].z, block_changes_snapshot[i].block, half, direction);
      // Skip the next entry (state data)
      i += 1;
      continue;
    }

    if (block_changes_snapshot[i].block == B_lantern) {
      uint16_t state = special_block_get_state(block_changes_snapshot[i].x, block_changes_snapshot[i].y, block_changes_snapshot[i].z, block_changes_snapshot[i].dimension);
      uint8_t hanging = state & 1;
      // If state is 0, try to recover from the state entry backup (hanging+1 stored in block_changes[i+1].block)
      if (state == 0 && i + 1 < block_changes_snapshot_count) {
        uint16_t backup = block_changes_snapshot[i + 1].block;
        if (backup >= 1 && backup <= 2) hanging = (uint8_t)(backup - 1);
      }
      uint16_t state_id = block_palette[B_lantern] - (hanging ? 2 : 0);
      sc_blockUpdateState(client_fd, block_changes_snapshot[i].x, block_changes_snapshot[i].y, block_changes_snapshot[i].z, state_id);
      continue;
    }

    if (block_changes_snapshot[i].block == B_wheat) {
      uint16_t state = special_block_get_state(block_changes_snapshot[i].x, block_changes_snapshot[i].y, block_changes_snapshot[i].z, block_changes_snapshot[i].dimension);
      uint16_t age = state & 7;
      sc_blockUpdateState(client_fd,
        block_changes_snapshot[i].x, block_changes_snapshot[i].y,
        block_changes_snapshot[i].z,
        block_palette[B_wheat] + age);
      continue;
    }

    if (is_fence_block(block_changes_snapshot[i].block)) {
      uint16_t state = special_block_get_state(block_changes_snapshot[i].x, block_changes_snapshot[i].y, block_changes_snapshot[i].z, block_changes_snapshot[i].dimension);
      uint8_t north = fence_get_north(state);
      uint8_t east  = fence_get_east(state);
      uint8_t south = fence_get_south(state);
      uint8_t west  = fence_get_west(state);
      uint8_t connections = north | (east << 1) | (south << 2) | (west << 3);
      uint16_t state_id = get_fence_state_id(block_changes_snapshot[i].block, connections);
      sc_blockUpdateState(client_fd, block_changes_snapshot[i].x, block_changes_snapshot[i].y, block_changes_snapshot[i].z, state_id);
      continue;
    }

    if (is_horizontal_facing_block(block_changes_snapshot[i].block)) {
      uint16_t state = special_block_get_state(block_changes_snapshot[i].x, block_changes_snapshot[i].y, block_changes_snapshot[i].z, block_changes_snapshot[i].dimension);
      uint8_t direction = state & 3;
      // If state is 0 (missing entry), try to recover from the state entry backup.
      // The torch handler stores direction+1 in block_changes[i+1].block.
      if (state == 0 && i + 1 < block_changes_snapshot_count) {
        uint16_t backup = block_changes_snapshot[i + 1].block;
        if (backup >= 1 && backup <= 4) direction = (uint8_t)(backup - 1);
      }
      uint16_t state_id = get_horizontal_state_id(block_changes_snapshot[i].block, direction);
      sc_blockUpdateState(client_fd, block_changes_snapshot[i].x, block_changes_snapshot[i].y, block_changes_snapshot[i].z, state_id);
      continue;
    }

    sc_blockUpdate(client_fd, block_changes_snapshot[i].x, block_changes_snapshot[i].y, block_changes_snapshot[i].z, block_changes_snapshot[i].block);
  }

  // Scan chunk sections for generated wheat not yet registered in the special
  // blocks table, and register it so the growth tick handler will find it.
  for (int i = 0; i < terrain_sections; i++) {
    int scan_y = (dimension == DIMENSION_NETHER) ? i * 16 : i * 16;
    uint16_t *scan_sec = cached_sections[i];
    for (int j = 0; j < 4096; j++) {
      uint16_t raw = scan_sec[j];
      if (raw >= B_wheat && raw < B_wheat_7 && !(raw & 0x8000)) {
        int s_addr = (j & ~7) | (7 - (j & 7));
        int s_wx = x + (s_addr & 15);
        int s_wz = z + ((s_addr >> 4) & 15);
        int s_wy = scan_y + (s_addr >> 8);
        if (!special_block_has_entry(s_wx, s_wy, s_wz, dimension)) {
          special_block_set_state(s_wx, s_wy, s_wz, dimension, B_wheat, (uint16_t)(raw - B_wheat));
        }
      }
    }
  }

  // Send updates for generated doors, chests, stairs, trapdoors, fences, and
  // horizontal-facing blocks (not in block changes).
  // Chunk data has packed values; correct state and special_block entries
  // are applied here. This covers village houses as well as underground dungeons.
  #ifdef ALLOW_DOORS
  int processed_generated[64][3];
  int num_generated = 0;
  for (int i = 0; i < terrain_sections; i++) {
    int section_y = (dimension == DIMENSION_NETHER) ? i * 16 : i * 16;
    uint16_t *section = cached_sections[i];
    for (int j = 0; j < 4096; j++) {
      uint16_t raw = section[j];
      uint16_t block_type = (raw & 0x8000) ? (raw & 0x1FF) : raw;
      if (!isDoorBlock(block_type) && block_type != B_chest && block_type != B_barrel && block_type != B_lantern &&
          !isStairBlock(block_type) && !isTrapdoorBlock(block_type) &&
          !is_fence_block(block_type) && !is_horizontal_facing_block(block_type) &&
          !is_bed_block(block_type)) continue;

      int address = (j & ~7) | (7 - (j & 7));
      int wx = x + (address & 15);
      int wz = z + ((address >> 4) & 15);
      int wy = section_y + (address >> 8);

      int skip = 0;
      for (int p = 0; p < num_generated; p++)
        if (processed_generated[p][0] == wx &&
            processed_generated[p][1] == wy &&
            processed_generated[p][2] == wz) { skip = 1; break; }
      if (skip) continue;

      for (int k = 0; k < block_changes_snapshot_count && !skip; k++) {
        if (block_changes_snapshot[k].block == 0xFF) continue;
        if ((isDoorBlock(block_changes_snapshot[k].block) || block_changes_snapshot[k].block == B_chest || is_bed_block(block_changes_snapshot[k].block)) &&
            block_changes_snapshot[k].x == wx &&
            block_changes_snapshot[k].y == wy &&
            block_changes_snapshot[k].z == wz) skip = 1;
      }
      if (skip) continue;

      if (block_type == B_chest) {
        uint8_t direction = (raw >> 9) & 3;
        if (!special_block_has_entry(wx, wy, wz, dimension))
          special_block_set_state(wx, wy, wz, dimension, block_type, oriented_encode_state(direction));
        uint16_t st = special_block_get_state(wx, wy, wz, dimension);
        sendOrientedUpdate(client_fd, wx, wy, wz, block_type, oriented_get_direction(st));
        if (num_generated < 62) {
          processed_generated[num_generated][0] = wx;
          processed_generated[num_generated][1] = wy;
          processed_generated[num_generated][2] = wz;
          num_generated++;
        }
      } else if (block_type == B_barrel) {
        // Decode direction (3 bits, bits 9-11) from raw; barrel with closed state
        uint8_t direction = (raw >> 9) & 7;
        if (!special_block_has_entry(wx, wy, wz, dimension))
          special_block_set_state(wx, wy, wz, dimension, block_type, barrel_encode_state(direction, 0));
        uint16_t state_id = get_oriented_state_id(block_type, direction);
        sc_blockUpdateState(client_fd, wx, wy, wz, state_id);
        if (num_generated < 62) {
          processed_generated[num_generated][0] = wx;
          processed_generated[num_generated][1] = wy;
          processed_generated[num_generated][2] = wz;
          num_generated++;
        }
      } else if (block_type == B_lantern) {
        // Decode hanging (bit 9) from raw; waterlogged=false
        uint8_t hanging = (raw >> 9) & 1;
        // Lantern states: hanging=false(0)=default, hanging=true(2)=default-2
        uint16_t state_id = block_palette[B_lantern] - (hanging ? 2 : 0);
        sc_blockUpdateState(client_fd, wx, wy, wz, state_id);
        if (num_generated < 62) {
          processed_generated[num_generated][0] = wx;
          processed_generated[num_generated][1] = wy;
          processed_generated[num_generated][2] = wz;
          num_generated++;
        }
      } else if (isDoorBlock(block_type)) {
        // Read existing state or init default
        if (!special_block_has_entry(wx, wy, wz, dimension)) {
          uint16_t vs = door_encode_state(0, 0, 1);
          special_block_set_state(wx, wy, wz, dimension, block_type, vs);
          if (!special_block_has_entry(wx, wy + 1, wz, dimension))
            special_block_set_state(wx, wy + 1, wz, dimension, block_type, vs);
        }
        // Send correct state to client (chunk data may have stale default)
        uint16_t st = special_block_get_state(wx, wy, wz, dimension);
        uint8_t open = door_get_open(st);
        uint8_t hinge = door_get_hinge(st);
        uint8_t dir = door_get_direction(st);
        sendDoorUpdate(client_fd, wx, wy, wz, block_type, 0, open, dir, hinge);
        sendDoorUpdate(client_fd, wx, wy + 1, wz, block_type, 1, open, dir, hinge);

        if (num_generated < 62) {
          processed_generated[num_generated][0] = wx;
          processed_generated[num_generated][1] = wy;
          processed_generated[num_generated][2] = wz;
          num_generated++;
        }
        if (num_generated < 62) {
          processed_generated[num_generated][0] = wx;
          processed_generated[num_generated][1] = wy + 1;
          processed_generated[num_generated][2] = wz;
          num_generated++;
        }
      } else if (isStairBlock(block_type)) {
        // Decode facing (bits 9-10) and half (bit 11) from raw
        uint8_t direction = (raw >> 9) & 3;
        uint8_t half = (raw >> 11) & 1;
        if (!special_block_has_entry(wx, wy, wz, dimension))
          special_block_set_state(wx, wy, wz, dimension, block_type, stair_encode_state(half, direction));
        uint16_t state_id = get_stair_state_id(block_type, half, direction);
        sc_blockUpdateState(client_fd, wx, wy, wz, state_id);
        if (num_generated < 62) {
          processed_generated[num_generated][0] = wx;
          processed_generated[num_generated][1] = wy;
          processed_generated[num_generated][2] = wz;
          num_generated++;
        }
      } else if (isTrapdoorBlock(block_type)) {
        // Decode facing (bits 9-10), half (bit 11), open (bit 12) from raw
        uint8_t direction = (raw >> 9) & 3;
        uint8_t half = (raw >> 11) & 1;
        uint8_t open = (raw >> 12) & 1;
        if (!special_block_has_entry(wx, wy, wz, dimension))
          special_block_set_state(wx, wy, wz, dimension, block_type, trapdoor_encode_state(open, half, direction));
        uint16_t state_id = get_trapdoor_state_id(block_type, open, direction, half);
        sc_blockUpdateState(client_fd, wx, wy, wz, state_id);
        if (num_generated < 62) {
          processed_generated[num_generated][0] = wx;
          processed_generated[num_generated][1] = wy;
          processed_generated[num_generated][2] = wz;
          num_generated++;
        }
      } else if (is_bed_block(block_type)) {
        // Decode bed state from raw: bit9-10=facing, bit11=head, bit12=occupied
        uint8_t direction = (raw >> 9) & 3;
        uint8_t head = (raw >> 11) & 1;
        uint8_t occupied = (raw >> 12) & 1;
        if (!special_block_has_entry(wx, wy, wz, dimension))
          special_block_set_state(wx, wy, wz, dimension, block_type, bed_encode_state(head, occupied, direction));
        uint16_t state_id = get_bed_state_id(block_type, head, occupied, direction);
        sc_blockUpdateState(client_fd, wx, wy, wz, state_id);
        if (num_generated < 62) {
          processed_generated[num_generated][0] = wx;
          processed_generated[num_generated][1] = wy;
          processed_generated[num_generated][2] = wz;
          num_generated++;
        }
      } else if (is_fence_block(block_type)) {
        // Decode connections from raw: bit9=north, bit10=east, bit11=south, bit12=west
        uint8_t north = (raw >> 9) & 1;
        uint8_t east  = (raw >> 10) & 1;
        uint8_t south = (raw >> 11) & 1;
        uint8_t west  = (raw >> 12) & 1;
        uint8_t connections = north | (east << 1) | (south << 2) | (west << 3);
        if (!special_block_has_entry(wx, wy, wz, dimension))
          special_block_set_state(wx, wy, wz, dimension, block_type, fence_encode_state(north, east, south, west));
        uint16_t state_id = get_fence_state_id(block_type, connections);
        sc_blockUpdateState(client_fd, wx, wy, wz, state_id);
        if (num_generated < 62) {
          processed_generated[num_generated][0] = wx;
          processed_generated[num_generated][1] = wy;
          processed_generated[num_generated][2] = wz;
          num_generated++;
        }
      } else if (is_horizontal_facing_block(block_type)) {
        // Decode facing direction (bits 9-10) from raw
        uint8_t direction = (raw >> 9) & 3;
        if (!special_block_has_entry(wx, wy, wz, dimension))
          special_block_set_state(wx, wy, wz, dimension, block_type, horizontal_facing_encode_state(direction));
        uint16_t state_id = get_horizontal_state_id(block_type, direction);
        sc_blockUpdateState(client_fd, wx, wy, wz, state_id);
        if (num_generated < 62) {
          processed_generated[num_generated][0] = wx;
          processed_generated[num_generated][1] = wy;
          processed_generated[num_generated][2] = wz;
          num_generated++;
        }
      }
    }
  }
  #endif

  freeBlockChangesSnapshot(block_changes_snapshot);

  return 0;

}

// S->C Clientbound Keep Alive (play)
int sc_keepAlive (int client_fd) {

  startPacket(client_fd, 0x26);
  writeUint64(client_fd, 0);
  endPacket(client_fd);

  return 0;
}

// S->C Set Container Slot
int sc_setContainerSlot (int client_fd, int window_id, uint16_t slot, uint8_t count, uint16_t item) {

  startPacket(client_fd, 0x14);

  writeVarInt(client_fd, window_id);
  writeVarInt(client_fd, 0);
  writeUint16(client_fd, slot);

  writeVarInt(client_fd, count);
  if (count > 0) {
    writeVarInt(client_fd, item);
    writeVarInt(client_fd, 0);
    writeVarInt(client_fd, 0);
  }

  endPacket(client_fd);

  return 0;

}

// S->C Block Update
int sc_blockUpdate (int client_fd, int64_t x, int64_t y, int64_t z, uint16_t block) {
  startPacket(client_fd, 0x08);
  writeUint64(client_fd, ((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF));
  writeVarInt(client_fd, block_palette[block]);
  endPacket(client_fd);
  return 0;
}

// S->C Block Update with raw state ID (for blocks with dynamic states like wheat)
int sc_blockUpdateState (int client_fd, int64_t x, int64_t y, int64_t z, uint16_t state_id) {
  startPacket(client_fd, 0x08);
  writeUint64(client_fd, ((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF));
  writeVarInt(client_fd, state_id);
  endPacket(client_fd);
  return 0;
}

// S->C Block Event (chest open/close, etc.)
int sc_blockEvent (int client_fd, int64_t x, int64_t y, int64_t z, int action, int data, uint16_t block) {
  startPacket(client_fd, 0x07);
  writeUint64(client_fd, ((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF));
  writeVarInt(client_fd, action);
  writeVarInt(client_fd, data);
  writeVarInt(client_fd, block_palette[block]);
  endPacket(client_fd);
  return 0;
}

#ifdef ALLOW_DOORS
// S->C Block Update with door state support
int sc_blockUpdateDoor (int client_fd, int64_t x, int64_t y, int64_t z, uint8_t block, uint8_t is_upper, uint8_t open, uint8_t direction, uint8_t hinge) {
  startPacket(client_fd, 0x08);
  writeUint64(client_fd, ((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF));
  writeVarInt(client_fd, get_door_state_id(block, is_upper, open, direction, hinge));
  endPacket(client_fd);
  return 0;
}
#endif

// S->C Acknowledge Block Change
int sc_acknowledgeBlockChange (int client_fd, int sequence) {
  startPacket(client_fd, 0x04);
  writeVarInt(client_fd, sequence);
  endPacket(client_fd);
  return 0;
}

// C->S Player Action
int cs_playerAction (int client_fd) {

  uint8_t action = readByte(client_fd);

  int64_t pos = readInt64(client_fd);
  int x = pos >> 38;
  int y = pos << 52 >> 52;
  int z = pos << 26 >> 38;

  readByte(client_fd); // ignore face

  int sequence = readVarInt(client_fd);
  sc_acknowledgeBlockChange(client_fd, sequence);

  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return 1;

  handlePlayerAction(player, action, x, y, z);

  return 0;

}

// S->C Open Screen
int sc_openScreen (int client_fd, uint8_t window, const char *title, uint16_t length) {

  startPacket(client_fd, 0x34);

  writeVarInt(client_fd, window);
  writeVarInt(client_fd, window);

  writeByte(client_fd, 8); // string nbt tag
  writeUint16(client_fd, length); // string length
  send_all(client_fd, title, length);

  endPacket(client_fd);

  return 0;
}

// C->S Use Item
int cs_useItem (int client_fd) {

  uint8_t hand = readByte(client_fd);
  int sequence = readVarInt(client_fd);

  float yaw = readFloat(client_fd);
  float pitch = readFloat(client_fd);

  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return 1;

  player->yaw = ((short)(yaw + 540) % 360 - 180) * 127 / 180;
  player->pitch = pitch / 90.0f * 127.0f;
  sc_acknowledgeBlockChange(client_fd, sequence);

  handlePlayerUseItem(player, 0, 0, 0, 255, hand);

  return 0;
}

// C->S Use Item On
int cs_useItemOn (int client_fd) {

  uint8_t hand = readByte(client_fd);

  int64_t pos = readInt64(client_fd);
  int x = pos >> 38;
  int y = pos << 52 >> 52;
  int z = pos << 26 >> 38;

  uint8_t face = readByte(client_fd);

  // ignore cursor position
  readUint32(client_fd);
  readUint32(client_fd);
  readUint32(client_fd);

  // ignore "inside block" and "world border hit"
  readByte(client_fd);
  readByte(client_fd);

  int sequence = readVarInt(client_fd);
  sc_acknowledgeBlockChange(client_fd, sequence);

  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return 1;

  handlePlayerUseItem(player, x, y, z, face, hand);

  return 0;
}

static void skipHashedSlotComponents (int client_fd) {
  int added_components = readVarInt(client_fd);
  for (int i = 0; i < added_components; i++) {
    readVarInt(client_fd);  // component type
    readUint32(client_fd);  // component hash
  }

  int removed_components = readVarInt(client_fd);
  for (int i = 0; i < removed_components; i++) {
    readVarInt(client_fd);  // component type
  }
}

static void readChangedSlotPayload (int client_fd, uint8_t *present, uint16_t *item, uint8_t *count) {

  *present = readByte(client_fd);
  *item = 0;
  *count = 0;

  if (!*present) return;

  *item = readVarInt(client_fd);
  *count = (uint8_t)readVarInt(client_fd);

  skipHashedSlotComponents(client_fd);

}

static uint8_t countCraftOperationsFromInventoryDelta (PlayerData *player, uint16_t item, uint8_t result_count, uint16_t *before_items, uint8_t *before_counts) {

  if (item == 0 || result_count == 0) return 1;

  uint16_t added = 0;
  for (uint8_t i = 0; i < 41; i ++) {
    if (player->inventory_items[i] != item) continue;
    uint8_t before = before_items[i] == item ? before_counts[i] : 0;
    if (player->inventory_count[i] > before) added += player->inventory_count[i] - before;
  }

  uint8_t operations = added / result_count;
  return operations == 0 ? 1 : operations;

}

static void consumeCraftingIngredients (PlayerData *player, int window_id, uint8_t operations, uint8_t *before_counts, uint16_t *before_items) {

  uint8_t inventory_craft_slots[] = {0, 1, 3, 4};
  uint8_t max_slots = window_id == 12 ? 9 : 4;

  for (uint8_t n = 0; n < max_slots; n ++) {
    uint8_t i = window_id == 12 ? n : inventory_craft_slots[n];
    if (before_items[i] == 0 || before_counts[i] == 0) continue;

    uint8_t expected = before_counts[i] > operations ? before_counts[i] - operations : 0;

    // If the client already sent the ingredient decrement, leave it alone.
    // Otherwise reduce the still-unchanged slot server-side to keep crafting authoritative.
    if (player->craft_items[i] == before_items[i] && player->craft_count[i] > expected) {
      player->craft_count[i] = expected;
      if (expected == 0) player->craft_items[i] = 0;
    }
  }

}

static void syncCraftingSlots (PlayerData *player, int window_id) {

  uint8_t inventory_craft_slots[] = {0, 1, 3, 4};
  uint8_t max_slots = window_id == 12 ? 9 : 4;

  for (uint8_t n = 0; n < max_slots; n ++) {
    uint8_t i = window_id == 12 ? n : inventory_craft_slots[n];
    uint8_t client_slot = serverSlotToClientSlot(window_id, 41 + i);
    if (client_slot != 255) {
      sc_setContainerSlot(player->client_fd, window_id, client_slot, player->craft_count[i], player->craft_items[i]);
    }
  }

}

// C->S Click Container
int cs_clickContainer (int client_fd) {

  int window_id = readVarInt(client_fd);

  readVarInt(client_fd); // ignore state id

  int16_t clicked_slot = readInt16(client_fd);
  uint8_t button = readByte(client_fd);
  uint8_t mode = readVarInt(client_fd);

  int changes_count = readVarInt(client_fd);

  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return 1;

  uint16_t before_inventory_items[41];
  uint8_t before_inventory_count[41];
  uint16_t before_craft_items[9];
  uint8_t before_craft_count[9];
  memcpy(before_inventory_items, player->inventory_items, sizeof(before_inventory_items));
  memcpy(before_inventory_count, player->inventory_count, sizeof(before_inventory_count));
  memcpy(before_craft_items, player->craft_items, sizeof(before_craft_items));
  memcpy(before_craft_count, player->craft_count, sizeof(before_craft_count));

  uint8_t output_click = false;
  uint8_t output_count = 0;
  uint16_t output_item = 0;
  if ((window_id == 0 || window_id == 12) && clicked_slot == 0) {
    getCraftingOutput(player, &output_count, &output_item);
    output_click = output_item != 0 && output_count != 0;
  }

  // Handle merchant trade output clicks before applying client slot changes,
  // while the payment is still server-side in the input slot or inventory.
  // All result-slot clicks are server-authoritative so stock/level counters
  // cannot be bypassed by shift-clicking or other client-authored deltas.
  if (window_id == 19 && player->merchant_open && clicked_slot == 2) {
    if (mode == 0 || mode == 1) {
      executeMerchantTrade(player, player->selected_trade, mode == 1);
    } else {
      int mob_idx = -player->merchant_villager_eid - 2;
      if (mob_idx >= 0 && mob_idx < MAX_MOBS && mob_data[mob_idx].type == E_VILLAGER) {
        sc_setTradeOffers(player->client_fd, 19, mob_data[mob_idx].profession, mob_idx);
      }
      sc_setContainerSlot(player->client_fd, 19, 0, player->craft_count[0], player->craft_items[0]);
      sc_setContainerSlot(player->client_fd, 19, 1, 0, 0);
      sc_setContainerSlot(player->client_fd, 19, 2, 0, 0);
      for (uint8_t i = 0; i < 41; i++) {
        uint8_t cs = serverSlotToClientSlot(19, i);
        if (cs != 255) {
          sc_setContainerSlot(player->client_fd, 19, cs,
            player->inventory_count[i], player->inventory_items[i]);
        }
      }
    }

    // Discard unread slot change data from the packet
    for (int i = 0; i < changes_count; i++) {
      uint8_t p, c;
      uint16_t it;
      readUint16(client_fd); // slot index
      readChangedSlotPayload(client_fd, &p, &it, &c);
    }
    // Discard cursor item from packet (executeMerchantTrade already set it)
    if (readByte(client_fd)) {
      readVarInt(client_fd); // item id
      readVarInt(client_fd); // count
      skipHashedSlotComponents(client_fd);
    }
    return 0;
  }

  uint8_t apply_changes = true;
  uint8_t equipment_dirty = false;
  // prevent dropping items
  if (mode == 4 && clicked_slot != -999) {
    // when using drop button, re-sync the respective slot
    uint8_t slot = clientSlotToServerSlot(window_id, clicked_slot);
    if (slot <= 40) {
      sc_setContainerSlot(client_fd, window_id, clicked_slot, player->inventory_count[slot], player->inventory_items[slot]);
    }
    apply_changes = false;
  } else if (mode == 0 && clicked_slot == -999) {
    // when clicking outside inventory, return the dropped item to the player
    if (button == 0) {
      givePlayerItem(player, player->flagval_16, player->flagval_8);
      player->flagval_16 = 0;
      player->flagval_8 = 0;
    } else {
      givePlayerItem(player, player->flagval_16, 1);
      player->flagval_8 -= 1;
      if (player->flagval_8 == 0) player->flagval_16 = 0;
    }
    apply_changes = false;
  }

  uint8_t slot, count, present, craft = false;
  uint16_t item;
  int tmp;

  uint16_t *p_item;
  uint8_t *p_count;

	  #ifdef ALLOW_CHESTS
	  int chest_idx = -1;
	  memcpy(&chest_idx, player->craft_items, sizeof(chest_idx));
	  uint8_t is_ender_chest = 0;
	  if (window_id == 2) {
	    // Check if this is a container with block_changes storage or ender chest
	    if (chest_idx >= 0 && chest_idx < block_changes_count &&
	        (block_changes[chest_idx].block == B_chest || block_changes[chest_idx].block == B_barrel)) {
	      is_ender_chest = 0;
	    } else {
	      is_ender_chest = 1;
	    }
	  }
	  #endif

	  for (int i = 0; i < changes_count; i ++) {

	    uint16_t changed_client_slot = readUint16(client_fd);
	    slot = clientSlotToServerSlot(window_id, changed_client_slot);
	    readChangedSlotPayload(client_fd, &present, &item, &count);
	    if (window_id == 19 && changed_client_slot == 2) continue; // merchant output is server-authoritative
	    if (slot > 67) continue; // skip out-of-bounds slots after consuming their payload
	    // slots outside of the inventory overflow into the crafting buffer
	    if (slot > 40 && apply_changes) craft = true;

	    #ifdef ALLOW_CHESTS
	    if (window_id == 2 && slot > 40 && !is_ender_chest) {
	      uint8_t *base = (uint8_t *)&block_changes[chest_idx + 1];
	      p_item = (uint16_t *)(base + (slot - 41) * 3);
	      p_count = base + (slot - 41) * 3 + 2;
	    } else if (window_id == 2 && slot > 40 && is_ender_chest) {
	      p_item = &player->ender_chest_items[slot - 41];
	      p_count = &player->ender_chest_count[slot - 41];
	    } else
	    #endif
	    {
	      // Prevent accessing crafting-related slots when craft_items is locked
	      if (slot > 40 && player->flags & 0x80) return 1;
	      p_item = &player->inventory_items[slot];
	      p_count = &player->inventory_count[slot];
	    }

    if (!present) { // no item?
      if (slot != 255 && apply_changes) {
        *p_item = 0;
        *p_count = 0;
        if (
          slot == player->hotbar ||
          slot == 40 ||
          (slot >= 36 && slot <= 39)
        ) equipment_dirty = true;
	        #ifdef ALLOW_CHESTS
	        if (window_id == 2 && slot > 40 && !is_ender_chest) {
	          broadcastChestUpdate(client_fd, chest_idx, 0, 0, slot - 41);
	        }
	        #endif
      }
      continue;
    }

    if (count > 0 && apply_changes) {
      *p_item = item;
      *p_count = count;
      if (
        slot == player->hotbar ||
        slot == 40 ||
        (slot >= 36 && slot <= 39)
      ) equipment_dirty = true;
	      #ifdef ALLOW_CHESTS
	      if (window_id == 2 && slot > 40 && !is_ender_chest) {
	        broadcastChestUpdate(client_fd, chest_idx, item, count, slot - 41);
	      }
	      #endif
    }

  }

  // window 0 is player inventory, window 12 is crafting table
  if (output_click && apply_changes) {
    uint8_t operations = mode == 1
      ? countCraftOperationsFromInventoryDelta(player, output_item, output_count, before_inventory_items, before_inventory_count)
      : 1;
    consumeCraftingIngredients(player, window_id, operations, before_craft_count, before_craft_items);
    syncCraftingSlots(player, window_id);
    craft = true;
  }

  if (craft && (window_id == 0 || window_id == 12)) {
    getCraftingOutput(player, &count, &item);
    sc_setContainerSlot(client_fd, window_id, 0, count, item);
  } else if (window_id == 14) { // furnace
    getSmeltingOutput(player);
    for (int i = 0; i < 3; i ++) {
      sc_setContainerSlot(client_fd, window_id, i, player->craft_count[i], player->craft_items[i]);
    }
  } else if (window_id == 19 && player->merchant_open && craft) {
    // Confirm slot changes to the client so auto-populated items don't revert
    sc_setContainerSlot(player->client_fd, 19, 0, player->craft_count[0], player->craft_items[0]);
    sc_setContainerSlot(player->client_fd, 19, 2, 0, 0);
    for (uint8_t i = 0; i < 41; i++) {
      uint8_t cs = serverSlotToClientSlot(19, i);
      if (cs != 255) {
        sc_setContainerSlot(player->client_fd, 19, cs,
          player->inventory_count[i], player->inventory_items[i]);
      }
    }
  }

skip_normal_processing:
  // assign cursor-carried item slot
  if (readByte(client_fd)) {
    player->flagval_16 = readVarInt(client_fd);
    player->flagval_8 = readVarInt(client_fd);
    skipHashedSlotComponents(client_fd);
  } else {
    player->flagval_16 = 0;
    player->flagval_8 = 0;
  }

  if (apply_changes && equipment_dirty) broadcastPlayerEquipment(player);

  return 0;

}

// S->C Set Cursor Item
int sc_setCursorItem (int client_fd, uint16_t item, uint8_t count) {

  startPacket(client_fd, 0x59);

  writeVarInt(client_fd, count);
  if (count == 0) {
    endPacket(client_fd);
    return 0;
  }

  writeVarInt(client_fd, item);

  // Skip components
  writeByte(client_fd, 0);
  writeByte(client_fd, 0);

  endPacket(client_fd);

  return 0;
}

// C->S Set Player Position And Rotation
int cs_setPlayerPositionAndRotation (int client_fd, double *x, double *y, double *z, float *yaw, float *pitch, uint8_t *on_ground) {

  *x = readDouble(client_fd);
  *y = readDouble(client_fd);
  *z = readDouble(client_fd);

  *yaw = readFloat(client_fd);
  *pitch = readFloat(client_fd);

  *on_ground = readByte(client_fd) & 0x01;

  return 0;
}

// C->S Set Player Position
int cs_setPlayerPosition (int client_fd, double *x, double *y, double *z, uint8_t *on_ground) {

  *x = readDouble(client_fd);
  *y = readDouble(client_fd);
  *z = readDouble(client_fd);

  *on_ground = readByte(client_fd) & 0x01;

  return 0;
}

// C->S Set Player Rotation
int cs_setPlayerRotation (int client_fd, float *yaw, float *pitch, uint8_t *on_ground) {

  *yaw = readFloat(client_fd);
  *pitch = readFloat(client_fd);

  *on_ground = readByte(client_fd) & 0x01;

  return 0;
}

int cs_setPlayerMovementFlags (int client_fd, uint8_t *on_ground) {

  *on_ground = readByte(client_fd) & 0x01;

  PlayerData *player;
  if (!getPlayerData(client_fd, &player))
    broadcastPlayerMetadata(player);

  return 0;
}

// C->S Swing Arm (serverbound)
int cs_swingArm (int client_fd) {

  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return 1;

  uint8_t hand = readVarInt(client_fd);

  uint8_t animation = 255;
  switch (hand) {
    case 0: {
      animation = 0;
      break;
    }
    case 1: {
      animation = 2;
      break;
    }
  }

  if (animation == 255)
    return 1;

  // Forward animation to all connected players
  for (int i = 0; i < MAX_PLAYERS; i ++) {
    PlayerData* other_player = &player_data[i];

    if (other_player->client_fd == -1) continue;
    if (other_player->client_fd == player->client_fd) continue;
    if (other_player->flags & 0x20) continue;

    sc_entityAnimation(other_player->client_fd, player->client_fd, animation);
  }

  return 0;
}

// C->S Set Held Item (serverbound)
int cs_setHeldItem (int client_fd) {

  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return 1;

  uint8_t slot = readUint16(client_fd);
  if (slot >= 9) return 1;

  player->hotbar = slot;
  broadcastPlayerEquipment(player);
  sc_updateEntityAttributes(player->client_fd, player->client_fd, player->inventory_items[slot]);

  return 0;
}

// S->C Set Held Item (clientbound)
int sc_setHeldItem (int client_fd, uint8_t slot) {

  startPacket(client_fd, 0x62);

  writeByte(client_fd, slot);

  endPacket(client_fd);

  return 0;
}

// C->S Close Container (serverbound)
int cs_closeContainer (int client_fd) {

  uint8_t window_id = readVarInt(client_fd);

  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return 1;

	  #ifdef ALLOW_CHESTS
	  int chest_idx = -1;
	  uint8_t is_barrel = 0;
	  // Ender chest close info (saved before craft_items is cleared)
	  uint8_t is_ender_chest = 0;
	  short ec_x = 0, ec_y = 0, ec_z = 0;
	  if (window_id == 2) {
	    memcpy(&chest_idx, player->craft_items, sizeof(chest_idx));
	    if (chest_idx >= 0 && chest_idx < block_changes_count && block_changes[chest_idx].block == B_barrel) {
	      is_barrel = 1;
	    }
	    // Check for ender chest marker (0xFFFF in craft_items[0])
	    if (player->craft_items[0] == 0xFFFF) {
	      is_ender_chest = 1;
	      ec_x = (int16_t)player->craft_items[1];
	      ec_y = (int16_t)player->craft_items[2];
	      ec_z = (int16_t)player->craft_items[3];
	    }
	  }
	  #endif

	  // return all items in crafting slots to the player
	  // or, in the case of chests, simply clear the storage pointer
	  for (uint8_t i = 0; i < 9; i ++) {
	    if (window_id != 2) {
	      givePlayerItem(player, player->craft_items[i], player->craft_count[i]);
	      uint8_t client_slot = serverSlotToClientSlot(window_id, 41 + i);
	      if (client_slot != 255) sc_setContainerSlot(player->client_fd, window_id, client_slot, 0, 0);
	    }
	    player->craft_items[i] = 0;
	    player->craft_count[i] = 0;
	    // Unlock craft_items
	    player->flags &= ~0x80;
	  }

	  #ifdef ALLOW_CHESTS
	  if (chest_idx >= 0 && chest_idx < block_changes_count && block_changes[chest_idx].block == B_chest) {
	    sc_blockEvent(client_fd,
	      block_changes[chest_idx].x,
	      block_changes[chest_idx].y,
	      block_changes[chest_idx].z,
	      1, 0, B_chest);
	  }
	  if (is_barrel) {
	    // Restore barrel closed state
	    short bx = block_changes[chest_idx].x;
	    uint8_t by = (uint8_t)block_changes[chest_idx].y;
	    short bz = block_changes[chest_idx].z;
	    uint8_t dim = block_changes[chest_idx].dimension;
	    uint16_t st = special_block_get_state(bx, by, bz, dim);
	    uint8_t dir = barrel_get_direction(st);
	    special_block_set_state(bx, by, bz, dim, B_barrel, barrel_encode_state(dir, 0));
	    sc_blockEvent(client_fd, bx, by, bz, 1, 0, B_barrel);
	  }
	  if (is_ender_chest) {
	    sc_blockEvent(client_fd, ec_x, ec_y, ec_z, 1, 0, B_ender_chest);
	  }
	  #endif

	  givePlayerItem(player, player->flagval_16, player->flagval_8);
  sc_setCursorItem(client_fd, 0, 0);
  player->flagval_16 = 0;
  player->flagval_8 = 0;

  // Clean up merchant state when closing the merchant GUI
  if (window_id == 19) {
    player->merchant_open = 0;
    player->merchant_villager_eid = 0;
    player->selected_trade = 0;
  }

  return 0;
}

// S->C Player Info Update: Add Player + listed/gamemode/latency.
int sc_playerInfoUpdateAddPlayer (int client_fd, PlayerData player) {
  PlayerAppearance *appearance = findPlayerAppearanceByUuid(player.uuid);

  startPacket(client_fd, 0x3F); // Player Info Update

  // Action mask: Add Player | Update Game Mode | Update Listed | Update Latency.
  // Modern clients do not show entries in the tab list until Update Listed=true
  // has been sent for that profile.
  writeByte(client_fd, 0x1D);
  writeVarInt(client_fd, 1); // Player count (1 per packet)

  // Player UUID
  send_all(client_fd, player.uuid, 16);

  // Add Player fields
  int player_name_len = (int)strlen(player.name);
  writeVarInt(client_fd, player_name_len);
  send_all(client_fd, player.name, player_name_len);
  writePlayerProfileProperties(client_fd, appearance);

  // Update Game Mode fields
  writeVarInt(client_fd, getConfiguredGameMode());

  // Update Listed fields
  writeByte(client_fd, true);

  // Update Latency fields
  writeVarInt(client_fd, 0);

  endPacket(client_fd);

  return 0;
}

// S->C Player Info Remove
int sc_playerInfoRemovePlayer (int client_fd, PlayerData player) {
  startPacket(client_fd, 0x3E); // Player Info Remove
  writeVarInt(client_fd, 1);
  send_all(client_fd, player.uuid, 16);
  endPacket(client_fd);
  return 0;
}

// S->C Player Info Update, "Update Game Mode" action
int sc_playerInfoUpdateUpdateGamemode (int client_fd, PlayerData player, uint8_t gamemode) {

  startPacket(client_fd, 0x3F); // Packet ID

  writeByte(client_fd, 0x04); // EnumSet: Update Game Mode
  writeVarInt(client_fd, 1); // Player count

  // Player UUID
  send_all(client_fd, player.uuid, 16);
  // Game Mode
  writeVarInt(client_fd, gamemode);

  endPacket(client_fd);

  return 0;
}

// S->C Spawn Entity
int sc_spawnEntity (
  int client_fd,
  int id, uint8_t *uuid, int type,
  double x, double y, double z,
  uint8_t yaw, uint8_t pitch,
  int16_t vx, int16_t vy, int16_t vz
) {

  startPacket(client_fd, 0x01);

  writeVarInt(client_fd, id); // Entity ID
  send_all(client_fd, uuid, 16); // Entity UUID
  writeVarInt(client_fd, type); // Entity type

  // Position
  writeDouble(client_fd, x);
  writeDouble(client_fd, y);
  writeDouble(client_fd, z);

  // Angles
  writeByte(client_fd, pitch);
  writeByte(client_fd, yaw);
  writeByte(client_fd, yaw);

  // Data - mostly unused
  writeByte(client_fd, 0);

  // Velocity (units of 1/8000 blocks/tick)
  writeInt16(client_fd, vx);
  writeInt16(client_fd, vy);
  writeInt16(client_fd, vz);

  endPacket(client_fd);

  return 0;
}

// S->C Set Entity Velocity
int sc_setEntityVelocity (int client_fd, int id, int16_t vx, int16_t vy, int16_t vz) {
  startPacket(client_fd, 0x5E);

  writeVarInt(client_fd, id); // Entity ID
  writeInt16(client_fd, vx);
  writeInt16(client_fd, vy);
  writeInt16(client_fd, vz);

  endPacket(client_fd);

  return 0;
}

// S->C Update Attributes (0x7C) — sends entity attributes like attack speed to the client
int sc_updateEntityAttributes (int client_fd, int entity_id, uint16_t held_item) {
  startPacket(client_fd, 0x7C);

  writeVarInt(client_fd, entity_id); // Entity ID

  // Calculate attack speed for the held item
  float speed;
  uint8_t has_speed_modifier;
  switch (held_item) {
    case I_wooden_sword:
    case I_stone_sword:
    case I_iron_sword:
    case I_golden_sword:
    case I_diamond_sword:
    case I_netherite_sword:
      speed = 1.6f; has_speed_modifier = 1; break;
    case I_wooden_axe:
    case I_stone_axe:
      speed = 0.8f; has_speed_modifier = 1; break;
    case I_iron_axe:
      speed = 0.9f; has_speed_modifier = 1; break;
    case I_golden_axe:
    case I_diamond_axe:
    case I_netherite_axe:
      speed = 1.0f; has_speed_modifier = 1; break;
    case I_wooden_pickaxe:
    case I_stone_pickaxe:
    case I_iron_pickaxe:
    case I_golden_pickaxe:
    case I_diamond_pickaxe:
    case I_netherite_pickaxe:
      speed = 1.2f; has_speed_modifier = 1; break;
    case I_wooden_shovel:
    case I_stone_shovel:
    case I_iron_shovel:
    case I_golden_shovel:
    case I_diamond_shovel:
    case I_netherite_shovel:
      speed = 1.0f; has_speed_modifier = 1; break;
    case I_wooden_hoe:
    case I_golden_hoe:
      speed = 1.0f; has_speed_modifier = 1; break;
    case I_stone_hoe:
      speed = 2.0f; has_speed_modifier = 1; break;
    case I_iron_hoe:
      speed = 3.0f; has_speed_modifier = 1; break;
    case I_diamond_hoe:
    case I_netherite_hoe:
      speed = 4.0f; has_speed_modifier = 1; break;
    case I_trident:
      speed = 1.1f; has_speed_modifier = 1; break;
    case I_mace:
      speed = 0.4f; has_speed_modifier = 1; break;
    default:
      speed = 4.0f; has_speed_modifier = 0; break;
  }

  // Send 2 attributes: attack_speed (registry ID 4) + attack_damage (registry ID 2)
  writeVarInt(client_fd, 2);

  // --- Attribute 1: attack_speed (minecraft:attribute registry ID = 4) ---
  writeVarInt(client_fd, 4); // Attribute registry ID for minecraft:attack_speed
  writeDouble(client_fd, 4.0); // Base value
  writeVarInt(client_fd, has_speed_modifier ? 1 : 0);
  if (has_speed_modifier) {
    // Modifier Id: Identifier string (1.21+ uses Identifiers instead of UUIDs)
    const char mod_id[] = "minecraft:base_attack_speed";
    writeVarInt(client_fd, sizeof(mod_id) - 1);
    send_all(client_fd, mod_id, sizeof(mod_id) - 1);
    writeDouble(client_fd, speed - 4.0); // Modifier amount (weapon speed - base 4.0)
    writeByte(client_fd, 0); // Operation: ADD_VALUE (0)
  }

  // --- Attribute 2: attack_damage (minecraft:attribute registry ID = 2) ---
  writeVarInt(client_fd, 2); // Attribute registry ID for minecraft:attack_damage
  writeDouble(client_fd, 1.0); // Base value
  writeVarInt(client_fd, 0); // No modifiers for now

  endPacket(client_fd);
  return 0;
}

// S->C Set Entity Metadata
int sc_setEntityMetadata (int client_fd, int id, EntityData *metadata, size_t length) {
  startPacket(client_fd, 0x5C);

  writeVarInt(client_fd, id); // Entity ID

  for (size_t i = 0; i < length; i ++) {
    EntityData *data = &metadata[i];
    writeEntityData(client_fd, data);
  }

  writeByte(client_fd, 0xFF); // End

  endPacket(client_fd);

  return 0;
}

// S->C Set Equipment
int sc_setEquipment (int client_fd, int entity_id, PlayerData *player) {
  static const uint8_t equipment_slot_ids[] = {
    0, // main hand
    1, // off hand
    2, // boots
    3, // leggings
    4, // chestplate
    5  // helmet
  };
  static const uint8_t inventory_slots[] = {
    0,  // selected hotbar slot, resolved below
    40,
    36,
    37,
    38,
    39
  };

  if (player == NULL) return 1;

  startPacket(client_fd, 0x5F);

  writeVarInt(client_fd, entity_id);

  for (size_t i = 0; i < sizeof(equipment_slot_ids); i++) {
    uint8_t equipment_slot = equipment_slot_ids[i];
    uint8_t inventory_slot = inventory_slots[i];
    if (i == 0) inventory_slot = player->hotbar;
    if (i + 1 < sizeof(equipment_slot_ids)) equipment_slot |= 0x80;

    writeByte(client_fd, equipment_slot);
    writeItemSlot(
      client_fd,
      player->inventory_count[inventory_slot],
      player->inventory_items[inventory_slot]
    );
  }

  endPacket(client_fd);

  return 0;
}

// S->C Set Mob Equipment (main hand only)
int sc_setMobEquipment (int client_fd, int entity_id, uint16_t item) {
  startPacket(client_fd, 0x5F);

  writeVarInt(client_fd, entity_id);
  writeByte(client_fd, 0x00);
  writeItemSlot(client_fd, 1, item);

  endPacket(client_fd);
  return 0;
}

// S->C Spawn Entity (from PlayerData)
int sc_spawnEntityPlayer (int client_fd, PlayerData player) {
  return sc_spawnEntity(
    client_fd,
    player.client_fd, player.uuid, 149,
    player.x > 0 ? (double)player.x + 0.5 : (double)player.x - 0.5,
    player.y,
    player.z > 0 ? (double)player.z + 0.5 : (float)player.z - 0.5,
    player.yaw, player.pitch,
    0, 0, 0
  );
}

// S->C Entity Animation
int sc_entityAnimation (int client_fd, int id, uint8_t animation) {
  startPacket(client_fd, 0x02);

  writeVarInt(client_fd, id); // Entity ID
  writeByte(client_fd, animation); // Animation

  endPacket(client_fd);

  return 0;
}

// S->C Teleport Entity
int sc_teleportEntity (
  int client_fd, int id,
  double x, double y, double z,
  float yaw, float pitch
) {

  startPacket(client_fd, 0x1F);

  // Entity ID
  writeVarInt(client_fd, id);
  // Position
  writeDouble(client_fd, x);
  writeDouble(client_fd, y);
  writeDouble(client_fd, z);
  // Velocity
  writeUint64(client_fd, 0);
  writeUint64(client_fd, 0);
  writeUint64(client_fd, 0);
  // Angles
  writeFloat(client_fd, yaw);
  writeFloat(client_fd, pitch);
  // On ground flag
  writeByte(client_fd, 1);

  endPacket(client_fd);

  return 0;
}

// S->C Set Head Rotation
int sc_setHeadRotation (int client_fd, int id, uint8_t yaw) {

  startPacket(client_fd, 0x4C);
  // Entity ID
  writeVarInt(client_fd, id);
  // Head yaw
  writeByte(client_fd, yaw);

  endPacket(client_fd);

  return 0;
}

// S->C Set Head Rotation
int sc_updateEntityRotation (int client_fd, int id, uint8_t yaw, uint8_t pitch) {

  startPacket(client_fd, 0x31);
  // Entity ID
  writeVarInt(client_fd, id);
  // Angles
  writeByte(client_fd, yaw);
  writeByte(client_fd, pitch);
  // "On ground" flag
  writeByte(client_fd, 1);

  endPacket(client_fd);

  return 0;
}

// S->C Damage Event
int sc_damageEvent (int client_fd, int entity_id, int type) {

  startPacket(client_fd, 0x19);

  writeVarInt(client_fd, entity_id);
  writeVarInt(client_fd, type);
  writeByte(client_fd, 0);
  writeByte(client_fd, 0);
  writeByte(client_fd, false);

  endPacket(client_fd);

  return 0;
}

// S->C Set Health
int sc_setExperience (int client_fd, uint16_t xp_total, uint16_t xp_level, float xp_progress) {

  // S->C Set Experience (0x60)
  startPacket(client_fd, 0x60);

  writeFloat(client_fd, xp_progress);     // Progress to next level
  writeVarInt(client_fd, xp_level);       // Current XP level
  writeVarInt(client_fd, xp_total);       // Total XP accumulated

  endPacket(client_fd);

  return 0;
}

int sc_setHealth (int client_fd, uint8_t health, uint8_t food, uint16_t saturation) {

  startPacket(client_fd, 0x61);

  writeFloat(client_fd, (float)health);
  writeVarInt(client_fd, food);
  writeFloat(client_fd, (float)(saturation - 200) / 500.0f);

  endPacket(client_fd);

  return 0;
}

// S->C Respawn
int sc_respawn (int client_fd, uint8_t dimension) {

  startPacket(client_fd, 0x4B);

  // dimension id (from server-sent registries)
  writeVarInt(client_fd, dimension);
  // dimension name
  write_dimension_name(client_fd, dimension);
  // hashed seed
  writeUint64(client_fd, 0x0123456789ABCDEF);
  // gamemode
  writeByte(client_fd, getConfiguredGameMode());
  // previous gamemode
  writeByte(client_fd, 0xFF);
  // is debug
  writeByte(client_fd, 0);
  // is flat
  writeByte(client_fd, 0);
  // has death location
  writeByte(client_fd, 0);
  // portal cooldown
  writeVarInt(client_fd, 10);
  // sea level
  writeVarInt(client_fd, 63);
  // data kept
  writeByte(client_fd, 0);

  endPacket(client_fd);

  return 0;
}

// C->S Client Status
int cs_clientStatus (int client_fd) {

  uint8_t id = readByte(client_fd);

  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return 1;

  if (id == 0) {
    sc_respawn(client_fd, DIMENSION_OVERWORLD);
    resetPlayerData(player);
    spawnPlayer(player);
  }

  return 0;
}

// S->C System Chat
int sc_systemChat (int client_fd, char* message, uint16_t len) {

  startPacket(client_fd, 0x72);

  // String NBT tag
  writeByte(client_fd, 8);
  writeUint16(client_fd, len);
  send_all(client_fd, message, len);

  // Is action bar message?
  writeByte(client_fd, false);

  endPacket(client_fd);

  return 0;
}

// C->S Chat Message
int cs_chat (int client_fd) {

  // To be safe, cap messages to 32 bytes before the buffer length
  readStringN(client_fd, 224);

  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return 1;

  size_t message_len = strlen((char *)recv_buffer);
  uint8_t name_len = strlen(player->name);

  if (recv_buffer[0] != '!' && recv_buffer[0] != '/') { // Standard chat message

    // Shift message contents forward to make space for player name tag
    memmove(recv_buffer + name_len + 3, recv_buffer, message_len + 1);
    // Copy player name to index 1
    memcpy(recv_buffer + 1, player->name, name_len);
    // Surround player name with brackets and a space
    recv_buffer[0] = '<';
    recv_buffer[name_len + 1] = '>';
    recv_buffer[name_len + 2] = ' ';

    // Forward message to all connected players
    for (int i = 0; i < MAX_PLAYERS; i ++) {
      if (player_data[i].client_fd == -1) continue;
      if (player_data[i].flags & 0x20) continue;
      sc_systemChat(player_data[i].client_fd, (char *)recv_buffer, message_len + name_len + 3);
    }

    goto cleanup;
  }

  // Handle chat commands
  if (!config.enable_commands) {
    sc_systemChat(client_fd, "§cCommands are disabled on this server", 39);
    goto cleanup;
  }

  if (recv_buffer[0] == '/') recv_buffer[0] = '!';

  if (!strncmp((char *)recv_buffer, "!msg", 4)) {

    int target_offset = 5;
    int target_end_offset = 0;
    int text_offset = 0;

    // Skip spaces after "!msg"
    while (recv_buffer[target_offset] == ' ') target_offset++;
    target_end_offset = target_offset;
    // Extract target name
    while (recv_buffer[target_end_offset] != ' ' && recv_buffer[target_end_offset] != '\0' && target_end_offset < 21) target_end_offset++;
    text_offset = target_end_offset;
    // Skip spaces before message
    while (recv_buffer[text_offset] == ' ') text_offset++;

    // Send usage guide if arguments are missing
    if (target_offset == target_end_offset || target_end_offset == text_offset) {
      sc_systemChat(client_fd, "§7Usage: !msg <player> <message>", 33);
      goto cleanup;
    }

    // Query the target player
    PlayerData *target = getPlayerByName(target_offset, target_end_offset, recv_buffer);
    if (target == NULL) {
      sc_systemChat(client_fd, "Player not found", 16);
      goto cleanup;
    }

    // Format output as a vanilla whisper
    int name_len = strlen(player->name);
    int text_len = message_len - text_offset;
    memmove(recv_buffer + name_len + 24, recv_buffer + text_offset, text_len);
    snprintf((char *)recv_buffer, sizeof(recv_buffer), "§7§o%s whispers to you:", player->name);
    recv_buffer[name_len + 23] = ' ';
    // Send message to target player
    sc_systemChat(target->client_fd, (char *)recv_buffer, (uint16_t)(name_len + 24 + text_len));

    // Format output for sending player
    int target_len = target_end_offset - target_offset;
    memmove(recv_buffer + target_len + 23, recv_buffer + name_len + 24, text_len);
    snprintf((char *)recv_buffer, sizeof(recv_buffer), "§7§oYou whisper to %s:", target->name);
    recv_buffer[target_len + 22] = ' ';
    // Report back to sending player
    sc_systemChat(client_fd, (char *)recv_buffer, (uint16_t)(target_len + 23 + text_len));

    goto cleanup;
  }

  if (!strncmp((char *)recv_buffer, "!creative", 9)) {
    // Creative mode commands
    if (!isCreativeModeEnabled()) {
      sc_systemChat(client_fd, "§cCreative mode is not enabled on this server", 45);
      goto cleanup;
    }

    int arg_offset = 10;  // After "!creative "
    while (arg_offset < 224 && recv_buffer[arg_offset] == ' ') arg_offset++;

    if (arg_offset >= 224 || recv_buffer[arg_offset] == '\0') {
      // No arguments - toggle UI
      toggleCreativeModeUI(client_fd);
    } else if (!strncmp((char *)recv_buffer + arg_offset, "close", 5)) {
      closeCreativeModeUI(client_fd);
    } else if (!strncmp((char *)recv_buffer + arg_offset, "list", 4)) {
      sendCreativeUIScreen(client_fd);
    } else if (!strncmp((char *)recv_buffer + arg_offset, "next", 4)) {
      int player_index = getClientIndex(client_fd);
      if (player_index >= 0 && player_index < MAX_PLAYERS) {
        uint16_t next_pos = creative_ui_states[player_index].scroll_position + 20;
        creative_ui_states[player_index].scroll_position = next_pos;
        sendCreativeItemList(client_fd, next_pos);
      }
    } else {
      // Try to find and give item by name
      char item_name[128];
      size_t name_len = strlen((char *)recv_buffer + arg_offset);

      // Trim trailing whitespace
      while (name_len > 0 && ((recv_buffer + arg_offset)[name_len - 1] == ' ' ||
             (recv_buffer + arg_offset)[name_len - 1] == '\t' ||
             (recv_buffer + arg_offset)[name_len - 1] == '\n' ||
             (recv_buffer + arg_offset)[name_len - 1] == '\r')) {
        name_len--;
      }

      if (name_len > 127) name_len = 127;
      strncpy(item_name, (char *)recv_buffer + arg_offset, name_len);
      item_name[name_len] = '\0';

      // Search for item by name (case-insensitive)
      uint16_t item_id = findCreativeItemByName(item_name);
      if (item_id > 0) {
        handleCreativeItemClick(client_fd, item_id);
      } else {
        sc_systemChat(client_fd, "§cItem not found. Use §f!creative list§c to see available items", 63);
      }
    }
    goto cleanup;
  }

  if (!strncmp((char *)recv_buffer, "!noclip", 7) && (recv_buffer[7] == '\0' || recv_buffer[7] == ' ')) {
    int arg_offset = 8;
    while (arg_offset < 224 && recv_buffer[arg_offset] == ' ') arg_offset++;

    uint8_t enabled = !isPlayerNoclipEnabled(player);
    if (recv_buffer[arg_offset] != '\0') {
      char *arg = (char *)recv_buffer + arg_offset;
      if (!strncmp(arg, "on", 2) && (arg[2] == '\0' || arg[2] == ' ')) {
        enabled = 1;
      } else if (!strncmp(arg, "off", 3) && (arg[3] == '\0' || arg[3] == ' ')) {
        enabled = 0;
      } else {
        const char usage[] = "§7Usage: !noclip [on|off]";
        sc_systemChat(client_fd, (char *)usage, (uint16_t)sizeof(usage) - 1);
        goto cleanup;
      }
    }

    setPlayerNoclip(player, enabled);
    if (enabled) {
      const char msg[] = "§aNoclip enabled. Use §f!noclip off§a to return.";
      sc_systemChat(client_fd, (char *)msg, (uint16_t)sizeof(msg) - 1);
    } else {
      const char msg[] = "§7Noclip disabled.";
      sc_systemChat(client_fd, (char *)msg, (uint16_t)sizeof(msg) - 1);
    }
    goto cleanup;
  }

  if (!strncmp((char *)recv_buffer, "!gamemode", 9) && isCommandBoundary(recv_buffer, 9)) {
    int arg_offset = 10;
    while (arg_offset < 224 && recv_buffer[arg_offset] == ' ') arg_offset++;

    int arg_end = arg_offset;
    while (arg_end < 224 && recv_buffer[arg_end] != '\0' && recv_buffer[arg_end] != ' ') arg_end++;
    if (arg_end <= arg_offset) {
      const char usage[] = "§7Usage: /gamemode <survival|creative|adventure|spectator>";
      sc_systemChat(client_fd, (char *)usage, (uint16_t)sizeof(usage) - 1);
      goto cleanup;
    }

    char mode_buf[16];
    int mode_len = arg_end - arg_offset;
    if (mode_len > 15) mode_len = 15;
    memcpy(mode_buf, recv_buffer + arg_offset, mode_len);
    mode_buf[mode_len] = '\0';

    uint8_t gamemode = 0;
    if (!parseGameModeToken(mode_buf, &gamemode)) {
      const char usage[] = "§7Usage: /gamemode <survival|creative|adventure|spectator>";
      sc_systemChat(client_fd, (char *)usage, (uint16_t)sizeof(usage) - 1);
      goto cleanup;
    }

    config.gamemode = gamemode;
    syncConfiguredGameModeToPlayers();

    char response[96];
    int resp_len = snprintf(response, sizeof(response), "§aServer gamemode set to §f%s", gameModeName(gamemode));
    sc_systemChat(client_fd, response, (uint16_t)resp_len);
    goto cleanup;
  }

  if (!strncmp((char *)recv_buffer, "!give", 5) && isCommandBoundary(recv_buffer, 5)) {
    int arg_start = 6;
    while (arg_start < 224 && recv_buffer[arg_start] == ' ') arg_start++;

    int arg_end = (int)message_len;
    while (arg_end > arg_start && (recv_buffer[arg_end - 1] == ' ' || recv_buffer[arg_end - 1] == '\t')) arg_end--;

    if (arg_end <= arg_start) {
      const char usage[] = "§7Usage: /give [player] <item_name> [count]";
      sc_systemChat(client_fd, (char *)usage, (uint16_t)sizeof(usage) - 1);
      goto cleanup;
    }

    PlayerData *target = player;
    int item_start = arg_start;
    int first_token_end = arg_start;
    while (first_token_end < arg_end && recv_buffer[first_token_end] != ' ') first_token_end++;
    if (first_token_end < arg_end) {
      PlayerData *maybe_target = getPlayerByName(arg_start, first_token_end, recv_buffer);
      if (maybe_target != NULL) {
        target = maybe_target;
        item_start = first_token_end;
        while (item_start < arg_end && recv_buffer[item_start] == ' ') item_start++;
      }
    }

    int item_end = arg_end;
    int count = 1;
    int last_token_start = item_end;
    while (last_token_start > item_start && recv_buffer[last_token_start - 1] != ' ') last_token_start--;
    if (isIntegerToken(recv_buffer, last_token_start, item_end)) {
      char count_buf[16];
      int count_len = item_end - last_token_start;
      if (count_len > 15) count_len = 15;
      memcpy(count_buf, recv_buffer + last_token_start, count_len);
      count_buf[count_len] = '\0';
      count = atoi(count_buf);
      if (count < 1) count = 1;
      if (count > 2304) count = 2304;
      item_end = last_token_start;
      while (item_end > item_start && recv_buffer[item_end - 1] == ' ') item_end--;
    }

    if (item_end <= item_start) {
      const char usage[] = "§7Usage: /give [player] <item_name> [count]";
      sc_systemChat(client_fd, (char *)usage, (uint16_t)sizeof(usage) - 1);
      goto cleanup;
    }

    char item_name[128];
    int item_len = item_end - item_start;
    if (item_len > 127) item_len = 127;
    memcpy(item_name, recv_buffer + item_start, item_len);
    item_name[item_len] = '\0';

    char *lookup_name = item_name;
    if (!strncmp(lookup_name, "minecraft:", 10)) lookup_name += 10;

    uint16_t item_id = findCreativeItemByName(lookup_name);
    if (item_id == 0) {
      const char msg[] = "§cItem not found. Try names like §fdiamond§c or §foak_log§c.";
      sc_systemChat(client_fd, (char *)msg, (uint16_t)sizeof(msg) - 1);
      goto cleanup;
    }

    int remaining = count;
    int given = 0;
    uint8_t stack_size = getItemStackSize(item_id);
    if (stack_size == 0) stack_size = 64;
    while (remaining > 0) {
      uint8_t give_count = remaining > stack_size ? stack_size : (uint8_t)remaining;
      if (givePlayerItem(target, item_id, give_count)) break;
      remaining -= give_count;
      given += give_count;
    }

    if (given == 0) {
      const char msg[] = "§cTarget inventory is full.";
      sc_systemChat(client_fd, (char *)msg, (uint16_t)sizeof(msg) - 1);
      goto cleanup;
    }

    char response[160];
    int resp_len = snprintf(response, sizeof(response), "§aGave §f%d§a x §f%s§a to §f%s", given, lookup_name, target->name);
    sc_systemChat(client_fd, response, (uint16_t)resp_len);
    if (target->client_fd != client_fd) {
      resp_len = snprintf(response, sizeof(response), "§aYou received §f%d§a x §f%s", given, lookup_name);
      sc_systemChat(target->client_fd, response, (uint16_t)resp_len);
    }
    goto cleanup;
  }

  if (!strncmp((char *)recv_buffer, "!kill", 5) && isCommandBoundary(recv_buffer, 5)) {
    PlayerData *target = player;
    int target_start = 6;
    while (target_start < 224 && recv_buffer[target_start] == ' ') target_start++;
    if (recv_buffer[target_start] != '\0') {
      int target_end = target_start;
      while (target_end < 224 && recv_buffer[target_end] != '\0' && recv_buffer[target_end] != ' ') target_end++;
      target = getPlayerByName(target_start, target_end, recv_buffer);
      if (target == NULL) {
        const char msg[] = "§cPlayer not found.";
        sc_systemChat(client_fd, (char *)msg, (uint16_t)sizeof(msg) - 1);
        goto cleanup;
      }
    }

    if (target->health == 0) {
      const char msg[] = "§7Target is already dead.";
      sc_systemChat(client_fd, (char *)msg, (uint16_t)sizeof(msg) - 1);
      goto cleanup;
    }

    hurtEntity(target->client_fd, -1, D_generic, 255);
    goto cleanup;
  }

  if (!strncmp((char *)recv_buffer, "!heal", 5) && isCommandBoundary(recv_buffer, 5)) {
    PlayerData *target = player;
    int target_start = 6;
    while (target_start < 224 && recv_buffer[target_start] == ' ') target_start++;
    if (recv_buffer[target_start] != '\0') {
      int target_end = target_start;
      while (target_end < 224 && recv_buffer[target_end] != '\0' && recv_buffer[target_end] != ' ') target_end++;
      target = getPlayerByName(target_start, target_end, recv_buffer);
      if (target == NULL) {
        const char msg[] = "§cPlayer not found.";
        sc_systemChat(client_fd, (char *)msg, (uint16_t)sizeof(msg) - 1);
        goto cleanup;
      }
    }

    target->health = 20;
    sc_setHealth(target->client_fd, target->health, target->hunger, target->saturation);

    char response[96];
    int resp_len = snprintf(response, sizeof(response), "§aHealed §f%s", target->name);
    sc_systemChat(client_fd, response, (uint16_t)resp_len);
    if (target->client_fd != client_fd) {
      const char msg[] = "§aYou have been healed.";
      sc_systemChat(target->client_fd, (char *)msg, (uint16_t)sizeof(msg) - 1);
    }
    goto cleanup;
  }

  if (!strncmp((char *)recv_buffer, "!feed", 5) && isCommandBoundary(recv_buffer, 5)) {
    PlayerData *target = player;
    int target_start = 6;
    while (target_start < 224 && recv_buffer[target_start] == ' ') target_start++;
    if (recv_buffer[target_start] != '\0') {
      int target_end = target_start;
      while (target_end < 224 && recv_buffer[target_end] != '\0' && recv_buffer[target_end] != ' ') target_end++;
      target = getPlayerByName(target_start, target_end, recv_buffer);
      if (target == NULL) {
        const char msg[] = "§cPlayer not found.";
        sc_systemChat(client_fd, (char *)msg, (uint16_t)sizeof(msg) - 1);
        goto cleanup;
      }
    }

    target->hunger = 20;
    if (target->saturation < 2500) target->saturation = 2500;
    sc_setHealth(target->client_fd, target->health, target->hunger, target->saturation);

    char response[96];
    int resp_len = snprintf(response, sizeof(response), "§aFed §f%s", target->name);
    sc_systemChat(client_fd, response, (uint16_t)resp_len);
    if (target->client_fd != client_fd) {
      const char msg[] = "§aYour hunger has been restored.";
      sc_systemChat(target->client_fd, (char *)msg, (uint16_t)sizeof(msg) - 1);
    }
    goto cleanup;
  }

  if (!strncmp((char *)recv_buffer, "!help", 5)) {
    // Send command guide
    const char help_msg[] = "§7Commands (use / or !):\n"
    "  /gamemode <survival|creative|adventure|spectator> - Set server gamemode\n"
    "  /tp <x> <y> <z> - Teleport to coordinates\n"
    "  /give [player] <item_name> [count] - Give items\n"
    "  /time set <ticks|day|noon|night|midnight> - Set world time\n"
    "  /weather <clear|rain|thunder> - Set world weather\n"
    "  /kill [player] - Kill a player\n"
    "  /heal [player] - Restore health\n"
    "  /feed [player] - Restore hunger\n"
    "  !msg <player> <message> - Send a private message\n"
    "  !portal - Build a Nether portal at your position\n"
    "  !biome [x z] - Show biome at current or given coordinates\n"
    "  !findbiome <name> [radius] - Find and teleport to nearest biome\n"
    "  !creative <item_name> - Give yourself an item (creative mode)\n"
    "  !noclip [on|off] - Toggle spectator-style noclip movement\n"
    "  !findstructure [radius] - Find nearest nether structure\n"
    "  /help - Show this help message";
    sc_systemChat(client_fd, (char *)help_msg, (uint16_t)sizeof(help_msg) - 1);
    goto cleanup;
  }

  // !portal - Build a Nether portal at the player's position
  if (!strncmp((char *)recv_buffer, "!portal", 7)) {
    short px = player->x;
    uint8_t py = player->y;
    short pz = player->z;

    char resp[128];
    int len = snprintf(resp, sizeof(resp), "§7Building portal at §f%d, %d, %d", px, py, pz);
    sc_systemChat(client_fd, resp, (uint16_t)len);

    // Build 4-wide x 5-tall portal frame, offset so the portal is to the north
    int ox = 0, oz = -3;
    uint8_t dim = player->dimension;

    // Bottom frame (Y = py)
    for (int w = 0; w < 4; w++)
      makeBlockChange(px + w + ox, py, pz + oz, B_obsidian, dim);

    // Top frame (Y = py + 4)
    for (int w = 0; w < 4; w++)
      makeBlockChange(px + w + ox, py + 4, pz + oz, B_obsidian, dim);

    // Left and right pillars
    for (int h = 1; h < 4; h++) {
      makeBlockChange(px + ox, py + h, pz + oz, B_obsidian, dim);
      makeBlockChange(px + 3 + ox, py + h, pz + oz, B_obsidian, dim);
    }

    // Fill interior with portal blocks
    for (int w = 1; w < 3; w++)
      for (int h = 1; h < 4; h++)
        makeBlockChange(px + w + ox, py + h, pz + oz, B_nether_portal, dim);

    sc_systemChat(client_fd, "§aPortal built! Step into it to travel to the Nether.", 56);
    goto cleanup;
  }

  // !biome [x z] - Show biome at coordinates
  if (!strncmp((char *)recv_buffer, "!biome", 6)) {
    int bx = player->x;
    int bz = player->z;
    // Parse optional coordinates
    if (message_len > 7) {
      char coord_buf[32];
      int ci = 7;
      // Skip spaces
      while (recv_buffer[ci] == ' ' && ci < 224) ci++;
      // Parse x
      int ci_start = ci;
      while (recv_buffer[ci] != ' ' && recv_buffer[ci] != '\0' && ci < 224) ci++;
      if (ci > ci_start) {
        memcpy(coord_buf, recv_buffer + ci_start, ci - ci_start);
        coord_buf[ci - ci_start] = '\0';
        bx = atoi(coord_buf);
      }
      // Skip spaces
      while (recv_buffer[ci] == ' ' && ci < 224) ci++;
      // Parse z
      ci_start = ci;
      while (recv_buffer[ci] != ' ' && recv_buffer[ci] != '\0' && ci < 224) ci++;
      if (ci > ci_start) {
        memcpy(coord_buf, recv_buffer + ci_start, ci - ci_start);
        coord_buf[ci - ci_start] = '\0';
        bz = atoi(coord_buf);
      }
    }
    uint8_t biome = getBiomeAtBlockCoords(bx, bz);
    const char* biome_name = getBiomeName(biome);
    char response[128];
    int resp_len = snprintf(response, sizeof(response), "§7Biome at %d, %d: §f%s", bx, bz, biome_name);
    sc_systemChat(client_fd, response, (uint16_t)resp_len);
    goto cleanup;
  }

  // !tp <x> <y> <z> - Teleport to coordinates
  if (!strncmp((char *)recv_buffer, "!tp", 3)) {
    char coord_buf[32];
    int ci = 4;
    // Skip spaces
    while (recv_buffer[ci] == ' ' && ci < 224) ci++;
    // Parse x
    int ci_start = ci;
    while (recv_buffer[ci] != ' ' && recv_buffer[ci] != '\0' && ci < 224) ci++;
    if (ci <= ci_start) {
      sc_systemChat(client_fd, "§7Usage: !tp <x> <y> <z>", 26);
      goto cleanup;
    }
    memcpy(coord_buf, recv_buffer + ci_start, ci - ci_start);
    coord_buf[ci - ci_start] = '\0';
    int tx = atoi(coord_buf);
    // Skip spaces
    while (recv_buffer[ci] == ' ' && ci < 224) ci++;
    // Parse y
    ci_start = ci;
    while (recv_buffer[ci] != ' ' && recv_buffer[ci] != '\0' && ci < 224) ci++;
    if (ci <= ci_start) {
      sc_systemChat(client_fd, "§7Usage: !tp <x> <y> <z>", 26);
      goto cleanup;
    }
    memcpy(coord_buf, recv_buffer + ci_start, ci - ci_start);
    coord_buf[ci - ci_start] = '\0';
    int ty = atoi(coord_buf);
    // Skip spaces
    while (recv_buffer[ci] == ' ' && ci < 224) ci++;
    // Parse z
    ci_start = ci;
    while (recv_buffer[ci] != ' ' && recv_buffer[ci] != '\0' && ci < 224) ci++;
    if (ci <= ci_start) {
      sc_systemChat(client_fd, "§7Usage: !tp <x> <y> <z>", 26);
      goto cleanup;
    }
    memcpy(coord_buf, recv_buffer + ci_start, ci - ci_start);
    coord_buf[ci - ci_start] = '\0';
    int tz = atoi(coord_buf);
    // Clamp values
    if (tx < -30000000) tx = -30000000;
    if (tx > 30000000) tx = 30000000;
    if (ty < -32768) ty = -32768;
    if (ty > 32767) ty = 32767;
    if (tz < -30000000) tz = -30000000;
    if (tz > 30000000) tz = 30000000;
    // Update player position
    player->x = (short)tx;
    player->y = (int16_t)ty;
    player->z = (short)tz;
    player->grounded_y = (int16_t)ty;
    // Send teleport to player
    sc_synchronizePlayerPosition(client_fd, (double)tx + 0.5, (double)ty, (double)tz + 0.5,
      (float)player->yaw * 180.0f / 127.0f, (float)player->pitch * 90.0f / 127.0f);
    // Broadcast teleport to other players
    for (int i = 0; i < MAX_PLAYERS; i++) {
      if (player_data[i].client_fd == -1) continue;
      if (player_data[i].client_fd == client_fd) continue;
      if (player_data[i].flags & 0x20) continue;
      sc_teleportEntity(player_data[i].client_fd, client_fd,
        (double)tx + 0.5, (double)ty, (double)tz + 0.5,
        (float)player->yaw * 180.0f / 127.0f, (float)player->pitch * 90.0f / 127.0f);
    }
    char response[128];
    int resp_len = snprintf(response, sizeof(response), "§7Teleported to §f%d, %d, %d", tx, ty, tz);
    sc_systemChat(client_fd, response, (uint16_t)resp_len);
    goto cleanup;
  }

  // /time set <ticks|day|noon|night|midnight> - Set the world time
  if ((!strncmp((char *)recv_buffer, "!time", 5) && isCommandBoundary(recv_buffer, 5)) ||
      (!strncmp((char *)recv_buffer, "!timeset", 8) && isCommandBoundary(recv_buffer, 8))) {
    uint8_t legacy_timeset = !strncmp((char *)recv_buffer, "!timeset", 8);
    int ci = legacy_timeset ? 9 : 6;
    while (ci < 224 && recv_buffer[ci] == ' ') ci++;

    uint8_t add_time = false;
    if (!legacy_timeset) {
      if (!strncmp((char *)recv_buffer + ci, "set", 3) && isCommandBoundary(recv_buffer, ci + 3)) {
        ci += 3;
        while (ci < 224 && recv_buffer[ci] == ' ') ci++;
      } else if (!strncmp((char *)recv_buffer + ci, "add", 3) && isCommandBoundary(recv_buffer, ci + 3)) {
        add_time = true;
        ci += 3;
        while (ci < 224 && recv_buffer[ci] == ' ') ci++;
      }
    }

    int ci_start = ci;
    while (ci < 224 && recv_buffer[ci] != ' ' && recv_buffer[ci] != '\0') ci++;
    if (ci <= ci_start) {
      const char usage[] = "§7Usage: /time set <ticks|day|noon|night|midnight>";
      sc_systemChat(client_fd, (char *)usage, (uint16_t)sizeof(usage) - 1);
      goto cleanup;
    }

    char time_buf[32];
    int time_len = ci - ci_start;
    if (time_len > 31) time_len = 31;
    memcpy(time_buf, recv_buffer + ci_start, time_len);
    time_buf[time_len] = '\0';

    int parsed_time = 0;
    if (!parseTimeToken(time_buf, &parsed_time)) {
      const char usage[] = "§7Usage: /time set <ticks|day|noon|night|midnight>";
      sc_systemChat(client_fd, (char *)usage, (uint16_t)sizeof(usage) - 1);
      goto cleanup;
    }

    world_time = add_time ? normalizeWorldTime((int)world_time + parsed_time) : normalizeWorldTime(parsed_time);
    broadcastTimeUpdate();

    char response[64];
    int resp_len = snprintf(response, sizeof(response), "§7World time set to §f%d", world_time);
    sc_systemChat(client_fd, response, (uint16_t)resp_len);
    goto cleanup;
  }

  // !findbiome <name> [radius] or !find biome <name> [radius] - Find nearest biome
  uint8_t findbiome_cmd =
    !strncmp((char *)recv_buffer, "!findbiome", 10) &&
    (recv_buffer[10] == '\0' || recv_buffer[10] == ' ');
  uint8_t find_biome_cmd =
    !strncmp((char *)recv_buffer, "!find biome", 11) &&
    (recv_buffer[11] == '\0' || recv_buffer[11] == ' ');

  if (findbiome_cmd || find_biome_cmd) {
    static const char findbiome_usage[] =
      "§7Usage: !findbiome <name> [radius]\n"
      "§7Usage: !find biome <name> [radius]\n"
      "§7Examples: !findbiome desert, !find biome dark forest, !findbiome snowy_plains 5000\n"
      "§7Biomes: plains, desert, forest, taiga, jungle, swamp, snowy plains, birch forest, dark forest, savanna, badlands, meadow, etc.";
    static const char findbiome_usage_short[] =
      "§7Usage: !findbiome <name> [radius]\n"
      "§7Usage: !find biome <name> [radius]";
    int ci = find_biome_cmd ? 11 : 10;
    int arg_start, arg_end, name_start, name_end, name_len;
    int radius = 2000; // Default search radius (blocks)

    while (recv_buffer[ci] == ' ' && ci < 224) ci++;
    arg_start = ci;
    arg_end = (int)message_len;
    while (arg_end > arg_start && recv_buffer[arg_end - 1] == ' ') arg_end--;

    if (arg_end <= arg_start) {
      sc_systemChat(client_fd, (char *)findbiome_usage, (uint16_t)sizeof(findbiome_usage) - 1);
      goto cleanup;
    }

    int token_start = arg_end;
    while (token_start > arg_start && recv_buffer[token_start - 1] != ' ') token_start--;
    if (isIntegerToken(recv_buffer, token_start, arg_end)) {
      char rad_buf[32];
      int rad_len = arg_end - token_start;
      if (rad_len > 31) rad_len = 31;
      memcpy(rad_buf, recv_buffer + token_start, rad_len);
      rad_buf[rad_len] = '\0';
      radius = atoi(rad_buf);
      if (radius <= 0) radius = 2000;
      if (radius > 30000) radius = 30000;
      arg_end = token_start;
      while (arg_end > arg_start && recv_buffer[arg_end - 1] == ' ') arg_end--;
    }

    name_start = arg_start;
    name_end = arg_end;
    while (name_start < name_end && recv_buffer[name_start] == ' ') name_start++;
    while (name_end > name_start && recv_buffer[name_end - 1] == ' ') name_end--;
    name_len = name_end - name_start;

    if (name_len <= 0) {
      sc_systemChat(client_fd, (char *)findbiome_usage_short, (uint16_t)sizeof(findbiome_usage_short) - 1);
      goto cleanup;
    }

    // Search using the same CHUNK_SIZE anchor grid that terrain generation uses.
    // This keeps biome-finder results aligned with the biome that actually
    // drives trees and surface blocks at the destination.
    int px = player->x;
    int pz = player->z;
    int found_x = -1, found_z = -1;
    uint8_t found_biome = 0;
    int player_ax = div_floor(px, CHUNK_SIZE);
    int player_az = div_floor(pz, CHUNK_SIZE);
    int radius_anchors = radius / CHUNK_SIZE;
    if (radius_anchors < 1) radius_anchors = 1;
    // Send search start message
    char search_msg[128];
    int smsg_len = snprintf(search_msg, sizeof(search_msg), "§7Searching for biome within %d blocks...", radius);
    sc_systemChat(client_fd, search_msg, (uint16_t)smsg_len);
    // Search in expanding anchor rings
    for (int ring = 0; ring <= radius_anchors; ring++) {
      if (ring == 0) {
        uint8_t biome = getChunkBiome(player_ax, player_az);
        if (biomeNameMatches(biome, (char *)recv_buffer + name_start, (uint8_t)name_len)) {
          found_x = player_ax * CHUNK_SIZE + (CHUNK_SIZE / 2);
          found_z = player_az * CHUNK_SIZE + (CHUNK_SIZE / 2);
          found_biome = biome;
          break;
        }
      } else {
        int cx, cz;
        // Top edge: z = -ring
        for (cx = -ring; cx <= ring; cx++) {
          int check_cx = player_ax + cx;
          int check_cz = player_az - ring;
          uint8_t biome = getChunkBiome(check_cx, check_cz);
          if (biomeNameMatches(biome, (char *)recv_buffer + name_start, (uint8_t)name_len)) {
            found_x = check_cx * CHUNK_SIZE + (CHUNK_SIZE / 2);
            found_z = check_cz * CHUNK_SIZE + (CHUNK_SIZE / 2);
            found_biome = biome;
            goto found;
          }
        }
        // Bottom edge: z = +ring
        for (cx = -ring; cx <= ring; cx++) {
          int check_cx = player_ax + cx;
          int check_cz = player_az + ring;
          uint8_t biome = getChunkBiome(check_cx, check_cz);
          if (biomeNameMatches(biome, (char *)recv_buffer + name_start, (uint8_t)name_len)) {
            found_x = check_cx * CHUNK_SIZE + (CHUNK_SIZE / 2);
            found_z = check_cz * CHUNK_SIZE + (CHUNK_SIZE / 2);
            found_biome = biome;
            goto found;
          }
        }
        // Left edge: x = -ring
        for (cz = -ring + 1; cz < ring; cz++) {
          int check_cx = player_ax - ring;
          int check_cz = player_az + cz;
          uint8_t biome = getChunkBiome(check_cx, check_cz);
          if (biomeNameMatches(biome, (char *)recv_buffer + name_start, (uint8_t)name_len)) {
            found_x = check_cx * CHUNK_SIZE + (CHUNK_SIZE / 2);
            found_z = check_cz * CHUNK_SIZE + (CHUNK_SIZE / 2);
            found_biome = biome;
            goto found;
          }
        }
        // Right edge: x = +ring
        for (cz = -ring + 1; cz < ring; cz++) {
          int check_cx = player_ax + ring;
          int check_cz = player_az + cz;
          uint8_t biome = getChunkBiome(check_cx, check_cz);
          if (biomeNameMatches(biome, (char *)recv_buffer + name_start, (uint8_t)name_len)) {
            found_x = check_cx * CHUNK_SIZE + (CHUNK_SIZE / 2);
            found_z = check_cz * CHUNK_SIZE + (CHUNK_SIZE / 2);
            found_biome = biome;
            goto found;
          }
        }
      }
    }
    found:
    if (found_x != -1) {
      // Get surface height at found location
      uint8_t found_y = getHeightAt(found_x, found_z);
      if (found_y < 1) found_y = 64; // Fallback
      // Teleport player
      player->x = (short)found_x;
      player->y = found_y;
      player->z = (short)found_z;
      player->grounded_y = found_y;
      sc_synchronizePlayerPosition(client_fd, (double)found_x + 0.5, (double)found_y, (double)found_z + 0.5,
        (float)player->yaw * 180.0f / 127.0f, (float)player->pitch * 90.0f / 127.0f);
      // Broadcast teleport
      for (int i = 0; i < MAX_PLAYERS; i++) {
        if (player_data[i].client_fd == -1) continue;
        if (player_data[i].client_fd == client_fd) continue;
        if (player_data[i].flags & 0x20) continue;
        sc_teleportEntity(player_data[i].client_fd, client_fd,
          (double)found_x + 0.5, (double)found_y, (double)found_z + 0.5,
          (float)player->yaw * 180.0f / 127.0f, (float)player->pitch * 90.0f / 127.0f);
      }
      const char* biome_name = getBiomeName(found_biome);
      char response[192];
      int resp_len = snprintf(response, sizeof(response), "§7Found §f%s§7 at §f%d, %d, %d§7. Teleported!", biome_name, found_x, found_y, found_z);
      sc_systemChat(client_fd, response, (uint16_t)resp_len);
    } else {
      char response[192];
      int resp_len = snprintf(response, sizeof(response), "§7No matching biome found within %d blocks of your position.", radius);
      sc_systemChat(client_fd, response, (uint16_t)resp_len);
    }
    goto cleanup;
  }

  // !findstructure [radius] - Find nearest nether structure
  if (!strncmp((char *)recv_buffer, "!findstructure", 14)) {
    int ci = 14;
    while (recv_buffer[ci] == ' ' && ci < 224) ci++;
    int radius = 2000;
    char rad_buf[32];
    int rad_len = 0;
    while (ci + rad_len < 224 && recv_buffer[ci + rad_len] != '\0' && recv_buffer[ci + rad_len] != ' ' && rad_len < 31) {
        rad_buf[rad_len] = recv_buffer[ci + rad_len];
        rad_len++;
    }
    if (rad_len > 0) {
        rad_buf[rad_len] = '\0';
        radius = atoi(rad_buf);
        if (radius <= 0) radius = 2000;
        if (radius > 30000) radius = 30000;
    }

    if (player->dimension != DIMENSION_NETHER) {
        sc_systemChat(client_fd, "§cCan only find nether structures while in the Nether.", 55);
        goto cleanup;
    }

    int sx, sy, sz;
    const char *sname;
    char search_msg[128];
    int smsg_len = snprintf(search_msg, sizeof(search_msg), "§7Searching for nether structures within %d blocks...", radius);
    sc_systemChat(client_fd, search_msg, (uint16_t)smsg_len);

    if (findNearestNetherStructure(player->x, player->z, radius, &sx, &sy, &sz, &sname)) {
        player->x = (short)sx;
        player->y = (int16_t)sy;
        player->z = (short)sz;
        player->grounded_y = (uint8_t)sy;
        sc_synchronizePlayerPosition(client_fd, (double)sx + 0.5, (double)sy, (double)sz + 0.5,
            (float)player->yaw * 180.0f / 127.0f, (float)player->pitch * 90.0f / 127.0f);
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (player_data[i].client_fd == -1) continue;
            if (player_data[i].client_fd == client_fd) continue;
            if (player_data[i].flags & 0x20) continue;
            sc_teleportEntity(player_data[i].client_fd, client_fd,
                (double)sx + 0.5, (double)sy, (double)sz + 0.5,
                (float)player->yaw * 180.0f / 127.0f, (float)player->pitch * 90.0f / 127.0f);
        }
        char response[192];
        int resp_len = snprintf(response, sizeof(response), "§7Found §f%s§7 at §f%d, %d, %d§7. Teleported!", sname, sx, sy, sz);
        sc_systemChat(client_fd, response, (uint16_t)resp_len);
    } else {
        char response[192];
        int resp_len = snprintf(response, sizeof(response), "§7No nether structures found within %d blocks.", radius);
        sc_systemChat(client_fd, response, (uint16_t)resp_len);
    }
    goto cleanup;
  }

  // !weather <clear|rain|thunder> - Set world weather
  if (!strncmp((char *)recv_buffer, "!weather", 8) && (recv_buffer[8] == '\0' || recv_buffer[8] == ' ')) {
    int arg_offset = 9;
    while (arg_offset < 224 && recv_buffer[arg_offset] == ' ') arg_offset++;

    if (recv_buffer[arg_offset] == '\0') {
      const char usage[] = "§7Usage: !weather <clear|rain|thunder>";
      sc_systemChat(client_fd, (char *)usage, (uint16_t)sizeof(usage) - 1);
      goto cleanup;
    }

    char *arg = (char *)recv_buffer + arg_offset;
    if (!strncmp(arg, "clear", 5) && (arg[5] == '\0' || arg[5] == ' ')) {
      world_weather_clear = 1;
      world_rain_level = 0.0f;
      world_thunder_level = 0.0f;
      world_weather_clear_time = 12000 + (int32_t)(fast_rand() % 168000);
      world_weather_rain_time = 0;
      world_weather_thunder_time = 0;
      for (int i = 0; i < MAX_PLAYERS; i++) {
        if (player_data[i].client_fd == -1) continue;
        if (player_data[i].flags & 0x20) continue;
        sc_gameEvent(player_data[i].client_fd, 2, 0.0f);
        sc_gameEvent(player_data[i].client_fd, 7, 0.0f);
        sc_gameEvent(player_data[i].client_fd, 8, 0.0f);
      }
      sc_systemChat(client_fd, "§aWeather set to clear.", 23);
    } else if (!strncmp(arg, "rain", 4) && (arg[4] == '\0' || arg[4] == ' ')) {
      world_weather_clear = 0;
      world_rain_level = 1.0f;
      world_thunder_level = 0.0f;
      world_weather_clear_time = 0;
      world_weather_rain_time = 12000 + (int32_t)(fast_rand() % 12000);
      world_weather_thunder_time = 0;
      for (int i = 0; i < MAX_PLAYERS; i++) {
        if (player_data[i].client_fd == -1) continue;
        if (player_data[i].flags & 0x20) continue;
        if (player_data[i].dimension != DIMENSION_OVERWORLD) {
          sc_gameEvent(player_data[i].client_fd, 2, 0.0f);
          sc_gameEvent(player_data[i].client_fd, 7, 0.0f);
          sc_gameEvent(player_data[i].client_fd, 8, 0.0f);
        } else {
          sc_gameEvent(player_data[i].client_fd, 1, 0.0f);
          sc_gameEvent(player_data[i].client_fd, 7, 1.0f);
          sc_gameEvent(player_data[i].client_fd, 8, 0.0f);
        }
      }
      sc_systemChat(client_fd, "§aWeather set to rain.", 22);
    } else if (!strncmp(arg, "thunder", 7) && (arg[7] == '\0' || arg[7] == ' ')) {
      world_weather_clear = 0;
      world_rain_level = 1.0f;
      world_thunder_level = 1.0f;
      world_weather_clear_time = 0;
      world_weather_rain_time = 12000 + (int32_t)(fast_rand() % 12000);
      world_weather_thunder_time = 3600 + (int32_t)(fast_rand() % 12000);
      if (world_weather_thunder_time > world_weather_rain_time)
        world_weather_thunder_time = world_weather_rain_time;
      for (int i = 0; i < MAX_PLAYERS; i++) {
        if (player_data[i].client_fd == -1) continue;
        if (player_data[i].flags & 0x20) continue;
        if (player_data[i].dimension != DIMENSION_OVERWORLD) {
          sc_gameEvent(player_data[i].client_fd, 2, 0.0f);
          sc_gameEvent(player_data[i].client_fd, 7, 0.0f);
          sc_gameEvent(player_data[i].client_fd, 8, 0.0f);
        } else {
          sc_gameEvent(player_data[i].client_fd, 1, 0.0f);
          sc_gameEvent(player_data[i].client_fd, 7, 1.0f);
          sc_gameEvent(player_data[i].client_fd, 8, 1.0f);
        }
      }
      sc_systemChat(client_fd, "§aWeather set to thunder.", 25);
    } else {
      const char usage[] = "§7Usage: !weather <clear|rain|thunder>";
      sc_systemChat(client_fd, (char *)usage, (uint16_t)sizeof(usage) - 1);
    }
    goto cleanup;
  }

  // Handle fall-through case
  sc_systemChat(client_fd, "§7Unknown command", 18);

cleanup:
  readUint64(client_fd); // Ignore timestamp
  readUint64(client_fd); // Ignore salt
  // Ignore signature (if any)
  uint8_t has_signature = readByte(client_fd);
  if (has_signature) recv_all(client_fd, recv_buffer, 256, false);
  readVarInt(client_fd); // Ignore message count
  // Ignore acknowledgement bitmask and checksum
  recv_all(client_fd, recv_buffer, 4, false);

  return 0;
}

// C->S Interact
int cs_interact (int client_fd) {

  int entity_id = readVarInt(client_fd);
  uint8_t type = readByte(client_fd);

  if (type == 2) {
    // Ignore target coordinates
    recv_all(client_fd, recv_buffer, 12, false);
  }
  if (type != 1) {
    // Ignore hand
    recv_all(client_fd, recv_buffer, 1, false);
  }

  // Ignore sneaking flag
  recv_all(client_fd, recv_buffer, 1, false);

  if (type == 0) { // Interact
    interactEntity(entity_id, client_fd);
  } else if (type == 1) { // Attack
    // Sync weapon attributes so the client shows correct cooldown
    PlayerData *attacker;
    if (!getPlayerData(client_fd, &attacker)) {
      sc_updateEntityAttributes(client_fd, client_fd, attacker->inventory_items[attacker->hotbar]);
    }
    hurtEntity(entity_id, client_fd, D_player_attack, 1);
  }

  return 0;
}

// S->C Entity Event
int sc_entityEvent (int client_fd, int entity_id, uint8_t status) {

  startPacket(client_fd, 0x1E);

  writeUint32(client_fd, entity_id);
  writeByte(client_fd, status);

  endPacket(client_fd);

  return 0;
}

// S->C Entity Sound Effect (0x6D)
int sc_soundEntity (int client_fd, int sound_id, int category, int entity_id, float volume, float pitch) {

  startPacket(client_fd, 0x6D);

  // Holder<SoundEvent> encoding: registry_id + 1 for references
  writeVarInt(client_fd, sound_id + 1);
  writeVarInt(client_fd, category);
  writeVarInt(client_fd, entity_id);
  writeFloat(client_fd, volume);
  writeFloat(client_fd, pitch);
  uint64_t seed = (uint64_t)fast_rand() << 32 | fast_rand();
  writeUint64(client_fd, seed);

  endPacket(client_fd);

  return 0;
}

// S->C Sound Effect at position (0x6E)
int sc_soundEffect (int client_fd, int sound_id, int category, double x, double y, double z, float volume, float pitch) {

  startPacket(client_fd, 0x6E);

  // Holder<SoundEvent> encoding: registry_id + 1 for references
  writeVarInt(client_fd, sound_id + 1);
  writeVarInt(client_fd, category);
  writeUint32(client_fd, (uint32_t)(int)(x * 8.0));
  writeUint32(client_fd, (uint32_t)(int)(y * 8.0));
  writeUint32(client_fd, (uint32_t)(int)(z * 8.0));
  writeFloat(client_fd, volume);
  writeFloat(client_fd, pitch);
  uint64_t seed = (uint64_t)fast_rand() << 32 | fast_rand();
  writeUint64(client_fd, seed);

  endPacket(client_fd);

  return 0;
}

// S->C Remove Entities, but for only one entity per packet
int sc_removeEntity (int client_fd, int entity_id) {

  startPacket(client_fd, 0x46);

  writeByte(client_fd, 1);
  writeVarInt(client_fd, entity_id);

  endPacket(client_fd);

  return 0;
}

// C->S Player Input
int cs_playerInput (int client_fd) {

  uint8_t flags = readByte(client_fd);

  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return 1;

  // Set or clear sneaking flag
  if (flags & 0x20) player->flags |= 0x04;
  else player->flags &= ~0x04;

  broadcastPlayerMetadata(player);

  return 0;
}

// C->S Player Command
int cs_playerCommand (int client_fd) {

  readVarInt(client_fd); // Ignore entity ID
  uint8_t action = readByte(client_fd);
  readVarInt(client_fd); // Ignore "Jump Boost" value

  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return 1;

  // Handle sprinting
  if (action == 1) player->flags |= 0x08;
  else if (action == 2) player->flags &= ~0x08;

  broadcastPlayerMetadata(player);

  return 0;
}

// S->C Pickup Item (take_item_entity)
int sc_pickupItem (int client_fd, int collected, int collector, uint8_t count) {

  startPacket(client_fd, 0x75);

  writeVarInt(client_fd, collected);
  writeVarInt(client_fd, collector);
  writeVarInt(client_fd, count);

  endPacket(client_fd);

  return 0;
}

// S->C Set Trade Offers (0x2D)
// Profession: codebase constants (FARMER=0, LIBRARIAN=1, CLERIC=2, ARMORER=3,
// BUTCHER=4, CARTOGRAPHER=5, FISHERMAN=6, FLETCHER=7, LEATHERWORKER=8,
// MASON=9, SHEPHERD=10, TOOLSMITH=11, WEAPONSMITH=12)
int sc_setTradeOffers(int client_fd, int window_id, uint8_t profession, int mob_index) {
  // Trade entries: {input_item, input_count, output_item, output_count, max_uses}
  struct TradeEntry {
    uint16_t in_item, in_cnt;
    uint16_t out_item, out_cnt;
    uint8_t max_uses;
  };

  // Farmer trades: food-focused
  static const struct TradeEntry farmer[] = {
    {I_emerald, 1, I_bread, 3, 16},
    {I_emerald, 1, I_apple, 2, 8},
    {I_emerald, 1, I_cookie, 4, 12},
    {I_emerald, 2, I_cake, 1, 4},
    {I_emerald, 3, I_golden_carrot, 1, 3},
  };
  // Librarian trades: books and paper
  static const struct TradeEntry librarian[] = {
    {I_emerald, 1, I_paper, 4, 12},
    {I_emerald, 2, I_book, 1, 8},
    {I_emerald, 4, I_bookshelf, 1, 4},
    {I_emerald, 5, I_golden_apple, 1, 2},
    {I_emerald, 1, I_glass, 4, 12},
  };
  // Cleric trades: ender pearls, glowstone, redstone
  static const struct TradeEntry cleric[] = {
    {I_emerald, 1, I_redstone, 2, 12},
    {I_emerald, 2, I_lapis_lazuli, 1, 8},
    {I_emerald, 3, I_glowstone, 1, 6},
    {I_emerald, 4, I_ender_pearl, 1, 3},
    {I_emerald, 5, I_experience_bottle, 1, 3},
  };
  // Armorer trades: armor and protection
  static const struct TradeEntry armorer[] = {
    {I_emerald, 3, I_iron_boots, 1, 4},
    {I_emerald, 4, I_iron_leggings, 1, 4},
    {I_emerald, 5, I_iron_chestplate, 1, 4},
    {I_emerald, 7, I_diamond_boots, 1, 2},
    {I_emerald, 6, I_iron_helmet, 1, 4},
  };
  // Butcher trades: meat and leather
  static const struct TradeEntry butcher[] = {
    {I_emerald, 1, I_cooked_beef, 3, 12},
    {I_emerald, 1, I_cooked_porkchop, 3, 12},
    {I_emerald, 1, I_cooked_mutton, 3, 12},
    {I_emerald, 1, I_leather, 2, 8},
    {I_emerald, 2, I_rabbit_stew, 1, 4},
  };
  // Cartographer trades: maps and exploration
  static const struct TradeEntry cartographer[] = {
    {I_emerald, 1, I_paper, 6, 12},
    {I_emerald, 2, I_compass, 1, 6},
    {I_emerald, 3, I_map, 1, 4},
    {I_emerald, 4, I_filled_map, 1, 3},
    {I_emerald, 5, I_globe_banner_pattern, 1, 2},
  };
  // Fisherman trades: fish and rods
  static const struct TradeEntry fisherman[] = {
    {I_emerald, 1, I_cooked_cod, 3, 12},
    {I_emerald, 1, I_cooked_salmon, 3, 12},
    {I_emerald, 1, I_cod, 3, 12},
    {I_emerald, 2, I_salmon, 3, 8},
    {I_emerald, 3, I_fishing_rod, 1, 4},
  };
  // Fletcher trades: bows and arrows
  static const struct TradeEntry fletcher[] = {
    {I_emerald, 1, I_arrow, 6, 12},
    {I_emerald, 1, I_flint, 4, 12},
    {I_emerald, 2, I_bow, 1, 6},
    {I_emerald, 3, I_crossbow, 1, 4},
    {I_emerald, 4, I_arrow, 12, 3},
  };
  // Leatherworker trades: leather armor
  static const struct TradeEntry leatherworker[] = {
    {I_emerald, 1, I_leather, 3, 12},
    {I_emerald, 2, I_leather_helmet, 1, 6},
    {I_emerald, 3, I_leather_chestplate, 1, 6},
    {I_emerald, 3, I_leather_leggings, 1, 6},
    {I_emerald, 4, I_saddle, 1, 3},
  };
  // Mason trades: stone building blocks
  static const struct TradeEntry mason[] = {
    {I_emerald, 1, I_stone, 6, 12},
    {I_emerald, 1, I_stone_bricks, 4, 12},
    {I_emerald, 2, I_chiseled_stone_bricks, 1, 6},
    {I_emerald, 2, I_polished_andesite, 4, 6},
    {I_emerald, 1, I_bricks, 4, 12},
  };
  // Shepherd trades: wool and carpets
  static const struct TradeEntry shepherd[] = {
    {I_emerald, 1, I_white_wool, 3, 12},
    {I_emerald, 1, I_white_carpet, 6, 12},
    {I_emerald, 2, I_shears, 1, 6},
    {I_emerald, 2, I_white_bed, 1, 4},
    {I_emerald, 1, I_string, 4, 12},
  };
  // Toolsmith trades: tools
  static const struct TradeEntry toolsmith[] = {
    {I_emerald, 2, I_stone_axe, 1, 6},
    {I_emerald, 2, I_stone_pickaxe, 1, 6},
    {I_emerald, 3, I_iron_axe, 1, 4},
    {I_emerald, 3, I_iron_pickaxe, 1, 4},
    {I_emerald, 4, I_diamond_hoe, 1, 2},
  };
  // Weaponsmith trades: swords and weapons
  static const struct TradeEntry weaponsmith[] = {
    {I_emerald, 2, I_iron_sword, 1, 6},
    {I_emerald, 3, I_iron_axe, 1, 4},
    {I_emerald, 4, I_diamond_sword, 1, 2},
    {I_emerald, 4, I_diamond_axe, 1, 2},
    {I_emerald, 3, I_iron_ingot, 2, 6},
  };
  // Default: basic items
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

  uint8_t villager_level = getVillagerTradeLevel(mob_index);
  int unlocked_trade_count = trade_count;
  if (unlocked_trade_count > villager_level) unlocked_trade_count = villager_level;
  if (unlocked_trade_count < 1) unlocked_trade_count = 1;

  startPacket(client_fd, 0x2D);
  writeVarInt(client_fd, window_id);
  writeVarInt(client_fd, unlocked_trade_count);

  for (int i = 0; i < unlocked_trade_count; i++) {
    // Input item 1 (simplified format: itemId, count, components)
    writeVarInt(client_fd, trades[i].in_item);
    writeVarInt(client_fd, trades[i].in_cnt);
    writeVarInt(client_fd, 0); // no components

    // Output item (full Slot format: count, itemId, addedComponents, removedComponents, data)
    writeVarInt(client_fd, trades[i].out_cnt);
    if (trades[i].out_cnt > 0) {
      writeVarInt(client_fd, trades[i].out_item);
      writeVarInt(client_fd, 0); // addedComponentCount
      writeVarInt(client_fd, 0); // removedComponentCount
    }

    // Input item 2 (optional, absent)
    writeByte(client_fd, 0); // false = no second input

    // Trade meta
    // Get trade uses and calculate if disabled
    uint8_t uses = (mob_index >= 0 && mob_index < MAX_MOBS && i < 5) ? mob_trade_uses[mob_index][i] : 0;
    uint8_t disabled = (uses >= trades[i].max_uses) ? 1 : 0;
    writeByte(client_fd, disabled);  // tradeDisabled (bool)
    writeUint32(client_fd, uses);   // uses
    writeUint32(client_fd, trades[i].max_uses);
    writeUint32(client_fd, 1);       // xp
    writeUint32(client_fd, 0);       // specialPrice
    writeFloat(client_fd, 0.05f);    // priceMultiplier
    writeUint32(client_fd, 0);       // demand
  }

  writeVarInt(client_fd, villager_level);
  writeVarInt(client_fd, getVillagerTradeDisplayExperience(mob_index));
  writeByte(client_fd, 1);          // isRegularVillager (bool)
  writeByte(client_fd, 0);          // canRestock (bool)
  endPacket(client_fd);
  return 0;
}

// C->S Select Trade (0x32) — used for trade selection
int cs_containerButtonClick(int client_fd) {
  int trade_index = readVarInt(client_fd);

  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return 1;

  if (player->merchant_open) {
    if (trade_index >= 0 && trade_index < 10) {
      selectMerchantTrade(player, (uint8_t)trade_index);
    }
  }

  return 0;
}

// Helper: write a prefixed string (VarInt length + data)
static void write_pstring(int client_fd, const char *str) {
  size_t len = strlen(str);
  writeVarInt(client_fd, len);
  send_all(client_fd, str, len);
}

// Helper: write a single ingredient slot (one option, one item)
// Item slot format: VarInt count, VarInt item_id, VarInt components
static void write_single_ingredient(int client_fd, uint16_t item_id) {
  writeVarInt(client_fd, 1); // 1 alternative
  writeVarInt(client_fd, 1); // stack count (1 item)
  writeVarInt(client_fd, item_id);
  writeVarInt(client_fd, 0); // no components
}

// Helper: write an empty ingredient slot
static void write_empty_ingredient(int client_fd) {
  writeVarInt(client_fd, 0); // 0 alternatives = empty
}

// Write a shaped recipe to the packet.
// 'ingredients' is a width*height array of item IDs (0 for empty).
// Result: count items of 'result_id'.
static void write_shaped_recipe(int client_fd, const char *id,
    int w, int h, const uint16_t *ingredients,
    uint16_t result_id, int result_count) {
  write_pstring(client_fd, "minecraft:crafting_shaped");
  write_pstring(client_fd, id);
  write_pstring(client_fd, ""); // group
  writeVarInt(client_fd, w);
  writeVarInt(client_fd, h);
  for (int _i = 0; _i < w * h; _i++) {
    if (ingredients[_i] == 0) {
      writeVarInt(client_fd, 0); // empty slot
    } else {
      writeVarInt(client_fd, 1); // 1 alternative
      writeVarInt(client_fd, 1); // stack count = 1
      writeVarInt(client_fd, ingredients[_i]);
      writeVarInt(client_fd, 0); // no components
    }
  }
  writeVarInt(client_fd, result_count); // result quantity
  writeVarInt(client_fd, result_id);
  writeVarInt(client_fd, 0); // no components
}

// Write a smelting recipe to the packet.
static void write_smelting_recipe(int client_fd, const char *id,
    uint16_t ingredient_id, uint16_t result_id,
    float xp, int cook_time) {
  write_pstring(client_fd, "minecraft:smelting");
  write_pstring(client_fd, id);
  write_pstring(client_fd, ""); // group
  writeVarInt(client_fd, 1); // 1 ingredient alternative
  writeVarInt(client_fd, 1); // stack count = 1
  writeVarInt(client_fd, ingredient_id);
  writeVarInt(client_fd, 0); // no components
  writeVarInt(client_fd, 1); // result count = 1
  writeVarInt(client_fd, result_id);
  writeVarInt(client_fd, 0); // no components
  writeFloat(client_fd, xp);
  writeVarInt(client_fd, cook_time);
}

// Write a shapeless recipe to the packet.
static void write_shapeless_recipe(int client_fd, const char *id,
    int num_ingredients, const uint16_t *ingredients,
    uint16_t result_id, int result_count) {
  write_pstring(client_fd, "minecraft:crafting_shapeless");
  write_pstring(client_fd, id);
  write_pstring(client_fd, ""); // group
  writeVarInt(client_fd, num_ingredients);
  for (int _i = 0; _i < num_ingredients; _i++) {
    writeVarInt(client_fd, 1); // 1 alternative
    writeVarInt(client_fd, 1); // stack count = 1
    writeVarInt(client_fd, ingredients[_i]);
    writeVarInt(client_fd, 0); // no components
  }
  writeVarInt(client_fd, result_count);
  writeVarInt(client_fd, result_id);
  writeVarInt(client_fd, 0); // no components
}

// ─── Recipe declaration helpers ────────────────────────────────────────────

// Write a single ingredient slot with one item
static void writeIngredient(int client_fd, uint16_t item) {
  writeVarInt(client_fd, 1); // 1 alternative
  writeVarInt(client_fd, 1); // stack count
  writeVarInt(client_fd, item);
  writeVarInt(client_fd, 0); // no components
}

// Write an empty ingredient slot
static void writeEmptyIng(int client_fd) {
  writeVarInt(client_fd, 0);
}

// Write a recipe result
static void writeResult(int client_fd, uint8_t count, uint16_t item) {
  writeVarInt(client_fd, count);
  writeVarInt(client_fd, item);
  writeVarInt(client_fd, 0); // no components
}

// Write a shaped recipe. `ing` is a w×h grid of item IDs (0 = empty slot)
static void writeShaped(int client_fd, const char *id, const char *group,
    int w, int h, const uint16_t *ing, int rcount, uint16_t ritem) {
  write_pstring(client_fd, "minecraft:crafting_shaped");
  write_pstring(client_fd, id);
  write_pstring(client_fd, group);
  writeVarInt(client_fd, w);
  writeVarInt(client_fd, h);
  for (int _i = 0; _i < w * h; _i++) {
    if (ing[_i]) writeIngredient(client_fd, ing[_i]);
    else writeEmptyIng(client_fd);
  }
  writeResult(client_fd, rcount, ritem);
}

// Write a smelting recipe.
// The ingredient is a single Ingredient compound (which already has its own
// alternatives count), so we write it directly — no extra wrapping list count.
static void writeSmelting(int client_fd, const char *id,
    uint16_t ingredient, uint16_t result, float exp, int cook_time) {
  write_pstring(client_fd, "minecraft:smelting");
  write_pstring(client_fd, id);
  write_pstring(client_fd, "");
  writeIngredient(client_fd, ingredient);
  writeResult(client_fd, 1, result);
  writeFloat(client_fd, exp);
  writeVarInt(client_fd, cook_time);
}

// Build a recipe ID string "rN" from a counter
#define RECIPE_ID_BUF_SIZE 16
static void recipeId(int n, char *buf) {
  buf[0] = 'r';
  int p = 1;
  if (n >= 10000) buf[p++] = '0' + (n / 10000) % 10;
  if (n >= 1000)  buf[p++] = '0' + (n / 1000) % 10;
  if (n >= 100)   buf[p++] = '0' + (n / 100) % 10;
  if (n >= 10)    buf[p++] = '0' + (n / 10) % 10;
  buf[p++] = '0' + n % 10;
  buf[p] = '\0';
}

// ─── Recipe data tables ────────────────────────────────────────────────────

// Log/Stem → 4 Planks (1×1)
#define LOG_PLANKS_ENTRIES 12
static const uint16_t g_log_planks[LOG_PLANKS_ENTRIES][2] = {
  {I_oak_log, I_oak_planks},
  {I_spruce_log, I_spruce_planks},
  {I_birch_log, I_birch_planks},
  {I_jungle_log, I_jungle_planks},
  {I_acacia_log, I_acacia_planks},
  {I_cherry_log, I_cherry_planks},
  {I_dark_oak_log, I_dark_oak_planks},
  {I_pale_oak_log, I_pale_oak_planks},
  {I_mangrove_log, I_mangrove_planks},
  {I_bamboo_block, I_bamboo_planks},
  {I_crimson_stem, I_crimson_planks},
  {I_warped_stem, I_warped_planks},
};

// Block → 9 Items (1×1) and block → 4 items
#define DECOMPOSE_ENTRIES 12
static const uint16_t g_decompose[DECOMPOSE_ENTRIES][3] = {
  {I_iron_block, I_iron_ingot, 9},
  {I_gold_block, I_gold_ingot, 9},
  {I_diamond_block, I_diamond, 9},
  {I_redstone_block, I_redstone, 9},
  {I_coal_block, I_coal, 9},
  {I_copper_block, I_copper_ingot, 9},
  {I_netherite_block, I_netherite_ingot, 9},
  {I_lapis_block, I_lapis_lazuli, 9},
  {I_emerald_block, I_emerald, 9},
  {I_hay_block, I_wheat, 9},
  {I_dried_kelp_block, I_dried_kelp, 9},
  {I_slime_block, I_slime_ball, 9},
};

// Log/Stem → 3 Wood/Hyphae (2×2)
#define WOOD_ENTRIES 11
static const uint16_t g_wood[WOOD_ENTRIES][2] = {
  {I_oak_log, I_oak_wood},
  {I_spruce_log, I_spruce_wood},
  {I_birch_log, I_birch_wood},
  {I_jungle_log, I_jungle_wood},
  {I_acacia_log, I_acacia_wood},
  {I_cherry_log, I_cherry_wood},
  {I_dark_oak_log, I_dark_oak_wood},
  {I_mangrove_log, I_mangrove_wood},
  {I_pale_oak_log, I_pale_oak_wood},
  {I_crimson_stem, I_crimson_hyphae},
  {I_warped_stem, I_warped_hyphae},
};

// 6 items from 9 -> 1 block (3×3 uniform)
#define BLOCK_CRAFT_ENTRIES 13
static const uint16_t g_block_craft[BLOCK_CRAFT_ENTRIES][2] = {
  {I_iron_ingot, I_iron_block},
  {I_gold_ingot, I_gold_block},
  {I_diamond, I_diamond_block},
  {I_redstone, I_redstone_block},
  {I_coal, I_coal_block},
  {I_copper_ingot, I_copper_block},
  {I_emerald, I_emerald_block},
  {I_netherite_ingot, I_netherite_block},
  {I_lapis_lazuli, I_lapis_block},
  {I_wheat, I_hay_block},
  {I_dried_kelp, I_dried_kelp_block},
  {I_slime_ball, I_slime_block},
  {I_bone_meal, I_bone_block},
};

// Plank types for per-wood-type recipes (12 types)
#define PLANK_TYPES 12
static const uint16_t g_planks[PLANK_TYPES] = {
  I_oak_planks, I_spruce_planks, I_birch_planks, I_jungle_planks,
  I_acacia_planks, I_cherry_planks, I_dark_oak_planks, I_pale_oak_planks,
  I_mangrove_planks, I_bamboo_planks, I_crimson_planks, I_warped_planks,
};

// Corresponding slabs, stairs, doors, trapdoors, fences, fence gates, buttons, pressure plates
#define SLAB_TYPES 12
static const uint16_t g_slabs[SLAB_TYPES] = {
  I_oak_slab, I_spruce_slab, I_birch_slab, I_jungle_slab,
  I_acacia_slab, I_cherry_slab, I_dark_oak_slab, I_pale_oak_slab,
  I_mangrove_slab, I_bamboo_slab, I_crimson_slab, I_warped_slab,
};

#define STAIR_TYPES 12
static const uint16_t g_stairs[STAIR_TYPES] = {
  I_oak_stairs, I_spruce_stairs, I_birch_stairs, I_jungle_stairs,
  I_acacia_stairs, I_cherry_stairs, I_dark_oak_stairs, I_pale_oak_stairs,
  I_mangrove_stairs, I_bamboo_stairs, I_crimson_stairs, I_warped_stairs,
};

#define DOOR_TYPES 10
static const uint16_t g_doors[DOOR_TYPES] = {
  I_oak_door, I_spruce_door, I_birch_door, I_jungle_door,
  I_acacia_door, I_cherry_door, I_dark_oak_door, I_pale_oak_door,
  I_mangrove_door, I_bamboo_door,
};

#define TRAPDOOR_TYPES 9
static const uint16_t g_trapdoors[TRAPDOOR_TYPES] = {
  I_oak_trapdoor, I_spruce_trapdoor, I_birch_trapdoor, I_jungle_trapdoor,
  I_acacia_trapdoor, I_cherry_trapdoor, I_dark_oak_trapdoor, I_pale_oak_trapdoor,
  I_mangrove_trapdoor,
};

#define FENCE_TYPES 10
static const uint16_t g_fences[FENCE_TYPES] = {
  I_oak_fence, I_spruce_fence, I_birch_fence, I_jungle_fence,
  I_acacia_fence, I_cherry_fence, I_dark_oak_fence, I_pale_oak_fence,
  I_mangrove_fence, I_bamboo_fence,
};

#define FENCE_GATE_TYPES 12
static const uint16_t g_fence_gates[FENCE_GATE_TYPES] = {
  I_oak_fence_gate, I_spruce_fence_gate, I_birch_fence_gate, I_jungle_fence_gate,
  I_acacia_fence_gate, I_cherry_fence_gate, I_dark_oak_fence_gate, I_pale_oak_fence_gate,
  I_mangrove_fence_gate, I_bamboo_fence_gate, I_crimson_fence_gate, I_warped_fence_gate,
};

#define BUTTON_TYPES 14
static const uint16_t g_buttons[BUTTON_TYPES] = {
  I_stone_button, I_polished_blackstone_button,
  I_oak_button, I_spruce_button, I_birch_button, I_jungle_button,
  I_acacia_button, I_cherry_button, I_dark_oak_button, I_pale_oak_button,
  I_mangrove_button, I_bamboo_button, I_crimson_button, I_warped_button,
};

#define BUTTON_MATERIALS 14
static const uint16_t g_button_materials[BUTTON_TYPES] = {
  I_stone, I_polished_blackstone,
  I_oak_planks, I_spruce_planks, I_birch_planks, I_jungle_planks,
  I_acacia_planks, I_cherry_planks, I_dark_oak_planks, I_pale_oak_planks,
  I_mangrove_planks, I_bamboo_planks, I_crimson_planks, I_warped_planks,
};

#define PRESSURE_PLATE_TYPES 14
static const uint16_t g_pressure_plates[PRESSURE_PLATE_TYPES] = {
  I_stone_pressure_plate, I_polished_blackstone_pressure_plate,
  I_oak_pressure_plate, I_spruce_pressure_plate, I_birch_pressure_plate,
  I_jungle_pressure_plate, I_acacia_pressure_plate, I_cherry_pressure_plate,
  I_dark_oak_pressure_plate, I_pale_oak_pressure_plate, I_mangrove_pressure_plate,
  I_bamboo_pressure_plate, I_crimson_pressure_plate, I_warped_pressure_plate,
};

static const uint16_t g_pressure_materials[PRESSURE_PLATE_TYPES] = {
  I_stone, I_polished_blackstone,
  I_oak_planks, I_spruce_planks, I_birch_planks, I_jungle_planks,
  I_acacia_planks, I_cherry_planks, I_dark_oak_planks, I_pale_oak_planks,
  I_mangrove_planks, I_bamboo_planks, I_crimson_planks, I_warped_planks,
};

// Tool materials: cobblestone, iron, gold, diamond, netherite (also planks handled separately)
#define TOOL_MATERIALS 5
static const uint16_t g_tool_materials[TOOL_MATERIALS] = {
  I_cobblestone, I_iron_ingot, I_gold_ingot, I_diamond, I_netherite_ingot,
};

static const uint16_t g_shovels[TOOL_MATERIALS] = {
  I_stone_shovel, I_iron_shovel, I_golden_shovel, I_diamond_shovel, I_netherite_shovel,
};

static const uint16_t g_swords[TOOL_MATERIALS] = {
  I_stone_sword, I_iron_sword, I_golden_sword, I_diamond_sword, I_netherite_sword,
};

static const uint16_t g_pickaxes[TOOL_MATERIALS] = {
  I_stone_pickaxe, I_iron_pickaxe, I_golden_pickaxe, I_diamond_pickaxe, I_netherite_pickaxe,
};

static const uint16_t g_axes[TOOL_MATERIALS] = {
  I_stone_axe, I_iron_axe, I_golden_axe, I_diamond_axe, I_netherite_axe,
};

// Armor materials: leather, iron, gold, diamond, netherite
#define ARMOR_MATERIALS 5
static const uint16_t g_armor_materials[ARMOR_MATERIALS] = {
  I_leather, I_iron_ingot, I_gold_ingot, I_diamond, I_netherite_ingot,
};

static const uint16_t g_helmets[ARMOR_MATERIALS] = {
  I_leather_helmet, I_iron_helmet, I_golden_helmet, I_diamond_helmet, I_netherite_helmet,
};

static const uint16_t g_chestplates[ARMOR_MATERIALS] = {
  I_leather_chestplate, I_iron_chestplate, I_golden_chestplate, I_diamond_chestplate, I_netherite_chestplate,
};

static const uint16_t g_leggings_arr[ARMOR_MATERIALS] = {
  I_leather_leggings, I_iron_leggings, I_golden_leggings, I_diamond_leggings, I_netherite_leggings,
};

static const uint16_t g_boots[ARMOR_MATERIALS] = {
  I_leather_boots, I_iron_boots, I_golden_boots, I_diamond_boots, I_netherite_boots,
};

// Beds and wools (16 colors)
#define BED_COLORS 16
static const uint16_t g_wools[BED_COLORS] = {
  I_white_wool, I_orange_wool, I_magenta_wool, I_light_blue_wool,
  I_yellow_wool, I_lime_wool, I_pink_wool, I_gray_wool,
  I_light_gray_wool, I_cyan_wool, I_purple_wool, I_blue_wool,
  I_brown_wool, I_green_wool, I_red_wool, I_black_wool,
};

static const uint16_t g_beds[BED_COLORS] = {
  I_white_bed, I_orange_bed, I_magenta_bed, I_light_blue_bed,
  I_yellow_bed, I_lime_bed, I_pink_bed, I_gray_bed,
  I_light_gray_bed, I_cyan_bed, I_purple_bed, I_blue_bed,
  I_brown_bed, I_green_bed, I_red_bed, I_black_bed,
};

// ─── Smelting recipe data ──────────────────────────────────────────────────

#define SMELTING_ENTRIES 26
static const uint16_t g_smelting_in[SMELTING_ENTRIES] = {
  I_cobblestone, I_oak_log, I_raw_iron, I_raw_gold, I_raw_copper,
  I_ancient_debris, I_sand, I_red_sand, I_netherrack, I_cactus,
  I_chicken, I_beef, I_porkchop, I_mutton, I_cod, I_salmon, I_rabbit,
  I_potato, I_iron_ore, I_gold_ore, I_copper_ore,
  I_deepslate_iron_ore, I_deepslate_gold_ore, I_deepslate_copper_ore,
  I_clay_ball, I_rotten_flesh,
};

static const uint16_t g_smelting_out[SMELTING_ENTRIES] = {
  I_stone, I_charcoal, I_iron_ingot, I_gold_ingot, I_copper_ingot,
  I_netherite_scrap, I_glass, I_glass, I_nether_brick, I_green_dye,
  I_cooked_chicken, I_cooked_beef, I_cooked_porkchop, I_cooked_mutton,
  I_cooked_cod, I_cooked_salmon, I_cooked_rabbit,
  I_baked_potato, I_iron_ingot, I_gold_ingot, I_copper_ingot,
  I_iron_ingot, I_gold_ingot, I_copper_ingot,
  I_brick, I_leather,
};

// Also smelting for stone -> smooth_stone, sponge -> wet_sponge and reverse
#define SMELTING_EXTRA 3
static const uint16_t g_smelting_extra_in[SMELTING_EXTRA] = { I_stone, I_sponge, I_wet_sponge };
static const uint16_t g_smelting_extra_out[SMELTING_EXTRA] = { I_smooth_stone, I_wet_sponge, I_sponge };

// ─── SlotDisplay / RecipeDisplay helpers (1.21.8) ─────────────────────────

// Write a SlotDisplay showing a single item (type 3 = "item_stack").
// Uses full ItemStack format so the client can identify the item for availability checks.
static void writeSlotItem(int client_fd, uint16_t item_id) {
  writeVarInt(client_fd, 3);  // type: "item_stack"
  writeVarInt(client_fd, 1);  // itemCount (non-zero = present)
  writeVarInt(client_fd, item_id);
  writeVarInt(client_fd, 0);  // addedComponentCount
  writeVarInt(client_fd, 0);  // removedComponentCount
}

// Write a SlotDisplay for an empty slot (type 0 = "empty").
static void writeSlotEmpty(int client_fd) {
  writeVarInt(client_fd, 0);  // type: "empty"
}

// Write a RecipeDisplay for a crafting_shaped recipe.
// `ing` is a w×h array of item IDs (0 = empty), `num_ing` = w × h.
static void writeDisplayShaped(int client_fd, int w, int h,
    const uint16_t *ing, int num_ing,
    uint16_t result, uint16_t station) {
  writeVarInt(client_fd, 1);  // type: crafting_shaped
  writeVarInt(client_fd, w);
  writeVarInt(client_fd, h);
  writeVarInt(client_fd, num_ing);
  for (int i = 0; i < num_ing; i++) {
    if (ing[i]) writeSlotItem(client_fd, ing[i]);
    else writeSlotEmpty(client_fd);
  }
  writeSlotItem(client_fd, result);
  writeSlotItem(client_fd, station);
}

// ─── Recipe iteration helpers ────────────────────────────────────────────

// Write one recipe declaration entry (for packet 0x7E).
static void writeDeclareEntry(int client_fd, const char *rid,
    const uint16_t *items, int num_items) {
  write_pstring(client_fd, rid);
  writeVarInt(client_fd, num_items);
  for (int _i = 0; _i < num_items; _i++) {
    writeVarInt(client_fd, items[_i]);
  }
}

// Write one recipe_book_add entry for a shaped recipe.
static void writeAddShaped(int client_fd, int display_id,
    int w, int h, const uint16_t *grid,
    uint16_t result, int category) {
  writeVarInt(client_fd, display_id);
  writeDisplayShaped(client_fd, w, h, grid, w * h, result, I_crafting_table);
  writeVarInt(client_fd, 0);  // group
  writeVarInt(client_fd, category);
  writeByte(client_fd, 0);  // requirements absent
  writeByte(client_fd, 0);  // flags
}

// ─── Declare Recipes (1.21.8, packet 0x7E) ───────────────────────────────
int sc_declareRecipes(int client_fd) {
  startPacket(client_fd, 0x7E);

  int total = 0;
  // Count recipes (same categories as recipe_book_add)
  total += LOG_PLANKS_ENTRIES; total += 1; total += DECOMPOSE_ENTRIES;
  total += 2; total += BUTTON_TYPES; total += PRESSURE_PLATE_TYPES;
  total += 1; total += 1; total += 1; total += 1; total += 1; total += 1;
  total += SLAB_TYPES + 5; total += 1 + TOOL_MATERIALS;
  total += 1 + TOOL_MATERIALS; total += 1; total += WOOD_ENTRIES + 4;
  total += ARMOR_MATERIALS; total += 1 + TOOL_MATERIALS;
  total += 1 + TOOL_MATERIALS; total += ARMOR_MATERIALS;
  total += ARMOR_MATERIALS; total += ARMOR_MATERIALS; total += 1;
  total += BED_COLORS; total += DOOR_TYPES; total += STAIR_TYPES;
  total += FENCE_TYPES; total += TRAPDOOR_TYPES; total += FENCE_GATE_TYPES;
  total += 5; total += 1; total += 1; total += 1; total += 1;
  total += 1; total += 1; total += 1; total += 1; total += 1;
  total += BLOCK_CRAFT_ENTRIES + 1; total += 1;
  total += SMELTING_ENTRIES + SMELTING_EXTRA;

  writeVarInt(client_fd, total);

  char rid[RECIPE_ID_BUF_SIZE];
  uint16_t grid[9];
  uint16_t items[16];
  int idx = 0;

  // Helper lambda-like blocks for each recipe group
  #define DECLARE_1x1(ing, res) do { \
    recipeId(idx++, rid); items[0] = ing; items[1] = res; \
    writeDeclareEntry(client_fd, rid, items, 2); } while(0)

  #define DECLARE_RESULT(ing1, ing2, res) do { \
    recipeId(idx++, rid); items[0] = ing1; items[1] = ing2; items[2] = res; \
    writeDeclareEntry(client_fd, rid, items, 3); } while(0)

  // 1. Log → 4 Planks
  for (int i = 0; i < LOG_PLANKS_ENTRIES; i++)
    DECLARE_1x1(g_log_planks[i][0], g_log_planks[i][1]);

  // 2. Bone → 3 Bone Meal
  DECLARE_1x1(I_bone, I_bone_meal);

  // 3. Block decomposition
  for (int i = 0; i < DECOMPOSE_ENTRIES; i++)
    DECLARE_1x1(g_decompose[i][0], g_decompose[i][1]);
  DECLARE_1x1(I_honey_block, I_honey_bottle);
  DECLARE_1x1(I_bone_block, I_bone_meal);

  // 4. Buttons (1×1)
  for (int i = 0; i < BUTTON_TYPES; i++)
    DECLARE_1x1(g_button_materials[i], g_buttons[i]);

  // 5. Pressure plates (2×1)
  for (int i = 0; i < PRESSURE_PLATE_TYPES; i++) {
    recipeId(idx++, rid);
    items[0] = items[1] = g_pressure_materials[i];
    items[2] = g_pressure_plates[i];
    writeDeclareEntry(client_fd, rid, items, 3);
  }

  // 6. Stick (2 planks vertical)
  { recipeId(idx++, rid); items[0] = g_planks[0]; items[1] = I_stick;
    writeDeclareEntry(client_fd, rid, items, 2); }

  // 7. Torch
  { recipeId(idx++, rid); items[0] = I_coal; items[1] = I_stick; items[2] = I_torch;
    writeDeclareEntry(client_fd, rid, items, 3); }

  // 8. Shears
  { recipeId(idx++, rid); items[0] = I_iron_ingot; items[1] = I_shears;
    writeDeclareEntry(client_fd, rid, items, 2); }

  // 9. Arrow
  { recipeId(idx++, rid); items[0] = I_flint; items[1] = I_stick; items[2] = I_feather; items[3] = I_arrow;
    writeDeclareEntry(client_fd, rid, items, 4); }

  // 10. Bucket
  { recipeId(idx++, rid); items[0] = I_iron_ingot; items[1] = I_bucket;
    writeDeclareEntry(client_fd, rid, items, 2); }

  // 11. Bread
  { recipeId(idx++, rid); items[0] = I_wheat; items[1] = I_bread;
    writeDeclareEntry(client_fd, rid, items, 2); }

  // 12. Slabs
  for (int i = 0; i < SLAB_TYPES; i++) {
    recipeId(idx++, rid); items[0] = g_planks[i]; items[1] = g_slabs[i];
    writeDeclareEntry(client_fd, rid, items, 2); }
  DECLARE_1x1(I_cobblestone, I_cobblestone_slab);
  DECLARE_1x1(I_stone, I_stone_slab);
  DECLARE_1x1(I_stone_bricks, I_stone_brick_slab);
  DECLARE_1x1(I_bricks, I_brick_slab);
  DECLARE_1x1(I_nether_bricks, I_nether_brick_slab);

  // 13. Shovels (1 material + 2 sticks)
  { recipeId(idx++, rid); items[0] = g_planks[0]; items[1] = I_stick; items[2] = I_wooden_shovel;
    writeDeclareEntry(client_fd, rid, items, 3); }
  for (int i = 0; i < TOOL_MATERIALS; i++) {
    recipeId(idx++, rid); items[0] = g_tool_materials[i]; items[1] = I_stick; items[2] = g_shovels[i];
    writeDeclareEntry(client_fd, rid, items, 3); }

  // 14. Swords
  { recipeId(idx++, rid); items[0] = g_planks[0]; items[1] = I_stick; items[2] = I_wooden_sword;
    writeDeclareEntry(client_fd, rid, items, 3); }
  for (int i = 0; i < TOOL_MATERIALS; i++) {
    recipeId(idx++, rid); items[0] = g_tool_materials[i]; items[1] = I_stick; items[2] = g_swords[i];
    writeDeclareEntry(client_fd, rid, items, 3); }

  // 15. Crafting Table
  { recipeId(idx++, rid); items[0] = g_planks[0]; items[1] = I_crafting_table;
    writeDeclareEntry(client_fd, rid, items, 2); }

  // 16. 2×2 Uniform
  for (int i = 0; i < WOOD_ENTRIES; i++)
    DECLARE_1x1(g_wood[i][0], g_wood[i][1]);
  DECLARE_1x1(I_snowball, I_snow_block);
  DECLARE_1x1(I_stone, I_stone_bricks);
  DECLARE_1x1(I_brick, I_bricks);
  DECLARE_1x1(I_nether_brick, I_nether_bricks);

  // 17. Boots
  for (int i = 0; i < ARMOR_MATERIALS; i++)
    DECLARE_1x1(g_armor_materials[i], g_boots[i]);

  // 18. Pickaxes
  { recipeId(idx++, rid); items[0] = g_planks[0]; items[1] = I_stick; items[2] = I_wooden_pickaxe;
    writeDeclareEntry(client_fd, rid, items, 3); }
  for (int i = 0; i < TOOL_MATERIALS; i++) {
    recipeId(idx++, rid); items[0] = g_tool_materials[i]; items[1] = I_stick; items[2] = g_pickaxes[i];
    writeDeclareEntry(client_fd, rid, items, 3); }

  // 19. Axes
  { recipeId(idx++, rid); items[0] = g_planks[0]; items[1] = I_stick; items[2] = I_wooden_axe;
    writeDeclareEntry(client_fd, rid, items, 3); }
  for (int i = 0; i < TOOL_MATERIALS; i++) {
    recipeId(idx++, rid); items[0] = g_tool_materials[i]; items[1] = I_stick; items[2] = g_axes[i];
    writeDeclareEntry(client_fd, rid, items, 3); }

  // 20. Helmets
  for (int i = 0; i < ARMOR_MATERIALS; i++)
    DECLARE_1x1(g_armor_materials[i], g_helmets[i]);

  // 21. Chestplates
  for (int i = 0; i < ARMOR_MATERIALS; i++)
    DECLARE_1x1(g_armor_materials[i], g_chestplates[i]);

  // 22. Leggings
  for (int i = 0; i < ARMOR_MATERIALS; i++)
    DECLARE_1x1(g_armor_materials[i], g_leggings_arr[i]);

  // 23. Bow
  { recipeId(idx++, rid); items[0] = I_stick; items[1] = I_string; items[2] = I_bow;
    writeDeclareEntry(client_fd, rid, items, 3); }

  // 24. Beds
  for (int i = 0; i < BED_COLORS; i++) {
    recipeId(idx++, rid); items[0] = g_wools[i]; items[1] = g_beds[i];
    writeDeclareEntry(client_fd, rid, items, 2); }

  // 25. Doors
  for (int i = 0; i < DOOR_TYPES; i++) {
    recipeId(idx++, rid); items[0] = g_planks[i]; items[1] = g_doors[i];
    writeDeclareEntry(client_fd, rid, items, 2); }

  // 26. Stairs
  for (int i = 0; i < STAIR_TYPES; i++) {
    recipeId(idx++, rid); items[0] = g_planks[i]; items[1] = g_stairs[i];
    writeDeclareEntry(client_fd, rid, items, 2); }

  // 27. Fences
  for (int i = 0; i < FENCE_TYPES; i++) {
    recipeId(idx++, rid); items[0] = g_planks[i]; items[1] = I_stick; items[2] = g_fences[i];
    writeDeclareEntry(client_fd, rid, items, 3); }

  // 28. Trapdoors
  for (int i = 0; i < TRAPDOOR_TYPES; i++) {
    recipeId(idx++, rid); items[0] = g_planks[i]; items[1] = g_trapdoors[i];
    writeDeclareEntry(client_fd, rid, items, 2); }

  // 29. Fence gates
  for (int i = 0; i < FENCE_GATE_TYPES; i++) {
    recipeId(idx++, rid); items[0] = g_planks[i]; items[1] = I_stick; items[2] = g_fence_gates[i];
    writeDeclareEntry(client_fd, rid, items, 3); }

  // 30-34. Glass, bars, wall, stairs
  DECLARE_1x1(I_glass, I_glass_pane);
  DECLARE_1x1(I_iron_ingot, I_iron_bars);
  DECLARE_1x1(I_cobblestone, I_cobblestone_wall);
  DECLARE_1x1(I_stone_bricks, I_stone_brick_stairs);
  DECLARE_1x1(I_cobblestone, I_cobblestone_stairs);

  // 35. Ladder
  { recipeId(idx++, rid); items[0] = I_stick; items[1] = I_ladder;
    writeDeclareEntry(client_fd, rid, items, 2); }

  // 36. Shield
  { recipeId(idx++, rid); items[0] = g_planks[0]; items[1] = I_iron_ingot; items[2] = I_shield;
    writeDeclareEntry(client_fd, rid, items, 3); }

  // 37. Composter
  DECLARE_1x1(I_oak_slab, I_composter);

  // 38. Furnace
  DECLARE_1x1(I_cobblestone, I_furnace);

  // 39. Chest
  DECLARE_1x1(g_planks[0], I_chest);

  // 40. Barrel
  { recipeId(idx++, rid); items[0] = g_planks[0]; items[1] = I_oak_slab; items[2] = I_barrel;
    writeDeclareEntry(client_fd, rid, items, 3); }

  // 41. Ender chest
  { recipeId(idx++, rid); items[0] = I_obsidian; items[1] = I_ender_eye; items[2] = I_ender_chest;
    writeDeclareEntry(client_fd, rid, items, 3); }

  // 42. Bookshelf
  { recipeId(idx++, rid); items[0] = g_planks[0]; items[1] = I_book; items[2] = I_bookshelf;
    writeDeclareEntry(client_fd, rid, items, 3); }

  // 43. Campfire
  { recipeId(idx++, rid); items[0] = I_stick; items[1] = I_coal; items[2] = I_oak_log; items[3] = I_campfire;
    writeDeclareEntry(client_fd, rid, items, 4); }

  // 44. 3×3 Uniform + Melon
  for (int i = 0; i < BLOCK_CRAFT_ENTRIES; i++)
    DECLARE_1x1(g_block_craft[i][0], g_block_craft[i][1]);
  DECLARE_1x1(I_melon_slice, I_melon);

  // 45. Flint & Steel
  { recipeId(idx++, rid); items[0] = I_iron_ingot; items[1] = I_flint; items[2] = I_flint_and_steel;
    writeDeclareEntry(client_fd, rid, items, 3); }

  // 46. Smelting
  for (int i = 0; i < SMELTING_ENTRIES; i++) {
    recipeId(idx++, rid); items[0] = g_smelting_in[i]; items[1] = g_smelting_out[i];
    writeDeclareEntry(client_fd, rid, items, 2); }
  for (int i = 0; i < SMELTING_EXTRA; i++) {
    recipeId(idx++, rid); items[0] = g_smelting_extra_in[i]; items[1] = g_smelting_extra_out[i];
    writeDeclareEntry(client_fd, rid, items, 2); }

  // Stonecutter recipes: none
  writeVarInt(client_fd, 0);

  endPacket(client_fd);
  return idx;
}

// ─── Recipe Book Add (1.21.8, packet 0x43) ───────────────────────────────
int sc_unlockRecipes(int client_fd) {
  startPacket(client_fd, 0x43);

  // Count entries (same as declare_recipes)
  int total = 0;
  total += LOG_PLANKS_ENTRIES; total += 1; total += DECOMPOSE_ENTRIES;
  total += 2; total += BUTTON_TYPES; total += PRESSURE_PLATE_TYPES;
  total += 1; total += 1; total += 1; total += 1; total += 1; total += 1;
  total += SLAB_TYPES + 5; total += 1 + TOOL_MATERIALS;
  total += 1 + TOOL_MATERIALS; total += 1; total += WOOD_ENTRIES + 4;
  total += ARMOR_MATERIALS; total += 1 + TOOL_MATERIALS;
  total += 1 + TOOL_MATERIALS; total += ARMOR_MATERIALS;
  total += ARMOR_MATERIALS; total += ARMOR_MATERIALS; total += 1;
  total += BED_COLORS; total += DOOR_TYPES; total += STAIR_TYPES;
  total += FENCE_TYPES; total += TRAPDOOR_TYPES; total += FENCE_GATE_TYPES;
  total += 5; total += 1; total += 1; total += 1; total += 1;
  total += 1; total += 1; total += 1; total += 1; total += 1;
  total += BLOCK_CRAFT_ENTRIES + 1; total += 1;
  total += SMELTING_ENTRIES + SMELTING_EXTRA;

  writeVarInt(client_fd, total);

  char rid[RECIPE_ID_BUF_SIZE];
  uint16_t grid[9];
  int idx = 0;
  (void)rid;

  // Helper: write shaped entry
  #define ADD_SHAPED(w, h, g, r, cat) do { \
    writeAddShaped(client_fd, idx++, w, h, g, r, cat); } while(0)

  // Categories: 0=building_blocks, 2=equipment, 3=misc
  // (skipping 1=redstone, no redstone recipes)

  // 1. Log → 4 Planks (1×1)
  for (int i = 0; i < LOG_PLANKS_ENTRIES; i++) {
    grid[0] = g_log_planks[i][0];
    ADD_SHAPED(1, 1, grid, g_log_planks[i][1], 0);
  }

  // 2. Bone → 3 Bone Meal (1×1)
  grid[0] = I_bone;
  ADD_SHAPED(1, 1, grid, I_bone_meal, 3);

  // 3. Block decomposition (1×1)
  for (int i = 0; i < DECOMPOSE_ENTRIES; i++) {
    grid[0] = g_decompose[i][0];
    ADD_SHAPED(1, 1, grid, g_decompose[i][1], 0);
  }
  grid[0] = I_honey_block; ADD_SHAPED(1, 1, grid, I_honey_bottle, 3);
  grid[0] = I_bone_block; ADD_SHAPED(1, 1, grid, I_bone_meal, 3);

  // 4. Buttons (1×1)
  for (int i = 0; i < BUTTON_TYPES; i++) {
    grid[0] = g_button_materials[i];
    ADD_SHAPED(1, 1, grid, g_buttons[i], 0);
  }

  // 5. Pressure plates (2×1)
  for (int i = 0; i < PRESSURE_PLATE_TYPES; i++) {
    grid[0] = grid[1] = g_pressure_materials[i];
    ADD_SHAPED(2, 1, grid, g_pressure_plates[i], 0);
  }

  // 6. Stick (2×1)
  grid[0] = grid[1] = g_planks[0];
  ADD_SHAPED(2, 1, grid, I_stick, 0);

  // 7. Torch (2×1)
  grid[0] = I_coal; grid[1] = I_stick;
  ADD_SHAPED(2, 1, grid, I_torch, 3);

  // 8. Shears (2×2 diagonal)
  grid[0] = I_iron_ingot; grid[1] = 0; grid[2] = 0; grid[3] = I_iron_ingot;
  ADD_SHAPED(2, 2, grid, I_shears, 2);

  // 9. Arrow (3×1)
  grid[0] = I_flint; grid[1] = I_stick; grid[2] = I_feather;
  ADD_SHAPED(3, 1, grid, I_arrow, 3);

  // 10. Bucket (3×2 V)
  grid[0] = I_iron_ingot; grid[1] = 0; grid[2] = I_iron_ingot;
  grid[3] = 0; grid[4] = I_iron_ingot; grid[5] = 0;
  ADD_SHAPED(3, 2, grid, I_bucket, 3);

  // 11. Bread (3×1)
  grid[0] = grid[1] = grid[2] = I_wheat;
  ADD_SHAPED(3, 1, grid, I_bread, 3);

  // 12. Slabs (3×1)
  for (int i = 0; i < SLAB_TYPES; i++) {
    grid[0] = grid[1] = grid[2] = g_planks[i];
    ADD_SHAPED(3, 1, grid, g_slabs[i], 0);
  }
  grid[0] = grid[1] = grid[2] = I_cobblestone;
  ADD_SHAPED(3, 1, grid, I_cobblestone_slab, 0);
  grid[0] = grid[1] = grid[2] = I_stone;
  ADD_SHAPED(3, 1, grid, I_stone_slab, 0);
  grid[0] = grid[1] = grid[2] = I_stone_bricks;
  ADD_SHAPED(3, 1, grid, I_stone_brick_slab, 0);
  grid[0] = grid[1] = grid[2] = I_bricks;
  ADD_SHAPED(3, 1, grid, I_brick_slab, 0);
  grid[0] = grid[1] = grid[2] = I_nether_bricks;
  ADD_SHAPED(3, 1, grid, I_nether_brick_slab, 0);

  // 13. Shovels (3×1)
  grid[0] = g_planks[0]; grid[1] = I_stick; grid[2] = I_stick;
  ADD_SHAPED(3, 1, grid, I_wooden_shovel, 2);
  for (int i = 0; i < TOOL_MATERIALS; i++) {
    grid[0] = g_tool_materials[i]; grid[1] = I_stick; grid[2] = I_stick;
    ADD_SHAPED(3, 1, grid, g_shovels[i], 2);
  }

  // 14. Swords (3×1)
  grid[0] = grid[1] = g_planks[0]; grid[2] = I_stick;
  ADD_SHAPED(3, 1, grid, I_wooden_sword, 2);
  for (int i = 0; i < TOOL_MATERIALS; i++) {
    grid[0] = grid[1] = g_tool_materials[i]; grid[2] = I_stick;
    ADD_SHAPED(3, 1, grid, g_swords[i], 2);
  }

  // 15. Crafting Table (2×2)
  grid[0] = grid[1] = grid[2] = grid[3] = g_planks[0];
  ADD_SHAPED(2, 2, grid, I_crafting_table, 0);

  // 16. 2×2 Uniform
  for (int i = 0; i < WOOD_ENTRIES; i++) {
    grid[0] = grid[1] = grid[2] = grid[3] = g_wood[i][0];
    ADD_SHAPED(2, 2, grid, g_wood[i][1], 0);
  }
  grid[0] = grid[1] = grid[2] = grid[3] = I_snowball;
  ADD_SHAPED(2, 2, grid, I_snow_block, 0);
  grid[0] = grid[1] = grid[2] = grid[3] = I_stone;
  ADD_SHAPED(2, 2, grid, I_stone_bricks, 0);
  grid[0] = grid[1] = grid[2] = grid[3] = I_brick;
  ADD_SHAPED(2, 2, grid, I_bricks, 0);
  grid[0] = grid[1] = grid[2] = grid[3] = I_nether_brick;
  ADD_SHAPED(2, 2, grid, I_nether_bricks, 0);

  // 17. Boots (3×2)
  for (int i = 0; i < ARMOR_MATERIALS; i++) {
    grid[0] = g_armor_materials[i]; grid[1] = 0; grid[2] = g_armor_materials[i];
    grid[3] = g_armor_materials[i]; grid[4] = 0; grid[5] = g_armor_materials[i];
    ADD_SHAPED(3, 2, grid, g_boots[i], 2);
  }

  // 18. Pickaxes (3×3)
  grid[0] = grid[1] = grid[2] = g_planks[0];
  grid[3] = 0; grid[4] = I_stick; grid[5] = 0;
  grid[6] = 0; grid[7] = I_stick; grid[8] = 0;
  ADD_SHAPED(3, 3, grid, I_wooden_pickaxe, 2);
  for (int i = 0; i < TOOL_MATERIALS; i++) {
    grid[0] = grid[1] = grid[2] = g_tool_materials[i];
    grid[3] = 0; grid[4] = I_stick; grid[5] = 0;
    grid[6] = 0; grid[7] = I_stick; grid[8] = 0;
    ADD_SHAPED(3, 3, grid, g_pickaxes[i], 2);
  }

  // 19. Axes (2×3)
  grid[0] = grid[1] = g_planks[0]; grid[2] = g_planks[0]; grid[3] = I_stick;
  grid[4] = 0; grid[5] = I_stick;
  ADD_SHAPED(2, 3, grid, I_wooden_axe, 2);
  for (int i = 0; i < TOOL_MATERIALS; i++) {
    grid[0] = grid[1] = g_tool_materials[i]; grid[2] = g_tool_materials[i];
    grid[3] = I_stick; grid[4] = 0; grid[5] = I_stick;
    ADD_SHAPED(2, 3, grid, g_axes[i], 2);
  }

  // 20. Helmets (3×2)
  for (int i = 0; i < ARMOR_MATERIALS; i++) {
    grid[0] = grid[1] = grid[2] = g_armor_materials[i];
    grid[3] = g_armor_materials[i]; grid[4] = 0; grid[5] = g_armor_materials[i];
    ADD_SHAPED(3, 2, grid, g_helmets[i], 2);
  }

  // 21. Chestplates (3×3)
  for (int i = 0; i < ARMOR_MATERIALS; i++) {
    grid[0] = g_armor_materials[i]; grid[1] = 0; grid[2] = g_armor_materials[i];
    grid[3] = g_armor_materials[i]; grid[4] = g_armor_materials[i]; grid[5] = g_armor_materials[i];
    grid[6] = g_armor_materials[i]; grid[7] = 0; grid[8] = g_armor_materials[i];
    ADD_SHAPED(3, 3, grid, g_chestplates[i], 2);
  }

  // 22. Leggings (3×3)
  for (int i = 0; i < ARMOR_MATERIALS; i++) {
    grid[0] = grid[1] = grid[2] = g_armor_materials[i];
    grid[3] = g_armor_materials[i]; grid[4] = 0; grid[5] = g_armor_materials[i];
    grid[6] = g_armor_materials[i]; grid[7] = 0; grid[8] = g_armor_materials[i];
    ADD_SHAPED(3, 3, grid, g_leggings_arr[i], 2);
  }

  // 23. Bow (3×3)
  grid[0] = 0; grid[1] = I_stick; grid[2] = I_string;
  grid[3] = I_stick; grid[4] = 0; grid[5] = I_string;
  grid[6] = 0; grid[7] = I_stick; grid[8] = I_string;
  ADD_SHAPED(3, 3, grid, I_bow, 2);

  // 24. Beds (3×2)
  for (int i = 0; i < BED_COLORS; i++) {
    grid[0] = grid[1] = grid[2] = g_wools[i];
    grid[3] = grid[4] = grid[5] = g_planks[0];
    ADD_SHAPED(3, 2, grid, g_beds[i], 0);
  }

  // 25. Doors (2×3)
  for (int i = 0; i < DOOR_TYPES; i++) {
    grid[0] = grid[2] = grid[3] = grid[5] = g_planks[i];
    grid[1] = grid[4] = g_planks[i];
    ADD_SHAPED(2, 3, grid, g_doors[i], 0);
  }

  // 26. Stairs (3×3 stair pattern)
  for (int i = 0; i < STAIR_TYPES; i++) {
    grid[0] = g_planks[i]; grid[1] = 0; grid[2] = 0;
    grid[3] = g_planks[i]; grid[4] = g_planks[i]; grid[5] = 0;
    grid[6] = g_planks[i]; grid[7] = g_planks[i]; grid[8] = g_planks[i];
    ADD_SHAPED(3, 3, grid, g_stairs[i], 0);
  }

  // 27. Fences (3×2)
  for (int i = 0; i < FENCE_TYPES; i++) {
    grid[0] = g_planks[i]; grid[1] = I_stick; grid[2] = g_planks[i];
    grid[3] = g_planks[i]; grid[4] = I_stick; grid[5] = g_planks[i];
    ADD_SHAPED(3, 2, grid, g_fences[i], 0);
  }

  // 28. Trapdoors (3×2)
  for (int i = 0; i < TRAPDOOR_TYPES; i++) {
    grid[0] = grid[1] = grid[2] = g_planks[i];
    grid[3] = grid[4] = grid[5] = g_planks[i];
    ADD_SHAPED(3, 2, grid, g_trapdoors[i], 0);
  }

  // 29. Fence gates (3×2)
  for (int i = 0; i < FENCE_GATE_TYPES; i++) {
    grid[0] = I_stick; grid[1] = g_planks[i]; grid[2] = I_stick;
    grid[3] = I_stick; grid[4] = g_planks[i]; grid[5] = I_stick;
    ADD_SHAPED(3, 2, grid, g_fence_gates[i], 0);
  }

  // 30-34. Glass pane, iron bars, wall, stairs
  grid[0] = grid[1] = grid[2] = grid[3] = grid[4] = grid[5] = I_glass;
  ADD_SHAPED(3, 2, grid, I_glass_pane, 0);
  grid[0] = grid[1] = grid[2] = grid[3] = grid[4] = grid[5] = I_iron_ingot;
  ADD_SHAPED(3, 2, grid, I_iron_bars, 0);
  grid[0] = grid[1] = grid[2] = grid[3] = grid[4] = grid[5] = I_cobblestone;
  ADD_SHAPED(3, 2, grid, I_cobblestone_wall, 0);
  grid[0] = I_stone_bricks; grid[1] = 0; grid[2] = 0;
  grid[3] = I_stone_bricks; grid[4] = I_stone_bricks; grid[5] = 0;
  grid[6] = I_stone_bricks; grid[7] = I_stone_bricks; grid[8] = I_stone_bricks;
  ADD_SHAPED(3, 3, grid, I_stone_brick_stairs, 0);
  grid[0] = I_cobblestone; grid[1] = 0; grid[2] = 0;
  grid[3] = I_cobblestone; grid[4] = I_cobblestone; grid[5] = 0;
  grid[6] = I_cobblestone; grid[7] = I_cobblestone; grid[8] = I_cobblestone;
  ADD_SHAPED(3, 3, grid, I_cobblestone_stairs, 0);

  // 35. Ladder (3×3)
  grid[0] = I_stick; grid[1] = 0; grid[2] = I_stick;
  grid[3] = I_stick; grid[4] = I_stick; grid[5] = I_stick;
  grid[6] = I_stick; grid[7] = 0; grid[8] = I_stick;
  ADD_SHAPED(3, 3, grid, I_ladder, 3);

  // 36. Shield (3×3)
  grid[0] = g_planks[0]; grid[1] = I_iron_ingot; grid[2] = g_planks[0];
  grid[3] = g_planks[0]; grid[4] = g_planks[0]; grid[5] = g_planks[0];
  grid[6] = 0; grid[7] = g_planks[0]; grid[8] = 0;
  ADD_SHAPED(3, 3, grid, I_shield, 2);

  // 37. Composter (3×3)
  grid[0] = I_oak_slab; grid[1] = 0; grid[2] = I_oak_slab;
  grid[3] = I_oak_slab; grid[4] = 0; grid[5] = I_oak_slab;
  grid[6] = I_oak_slab; grid[7] = I_oak_slab; grid[8] = I_oak_slab;
  ADD_SHAPED(3, 3, grid, I_composter, 3);

  // 38. Furnace (3×3)
  grid[0] = grid[1] = grid[2] = I_cobblestone;
  grid[3] = I_cobblestone; grid[4] = 0; grid[5] = I_cobblestone;
  grid[6] = grid[7] = grid[8] = I_cobblestone;
  ADD_SHAPED(3, 3, grid, I_furnace, 0);

  // 39. Chest (3×3)
  grid[0] = grid[1] = grid[2] = g_planks[0];
  grid[3] = g_planks[0]; grid[4] = 0; grid[5] = g_planks[0];
  grid[6] = grid[7] = grid[8] = g_planks[0];
  ADD_SHAPED(3, 3, grid, I_chest, 0);

  // 40. Barrel (3×3)
  grid[0] = g_planks[0]; grid[1] = I_oak_slab; grid[2] = g_planks[0];
  grid[3] = g_planks[0]; grid[4] = 0; grid[5] = g_planks[0];
  grid[6] = g_planks[0]; grid[7] = I_oak_slab; grid[8] = g_planks[0];
  ADD_SHAPED(3, 3, grid, I_barrel, 0);

  // 41. Ender chest (3×3)
  grid[0] = grid[1] = grid[2] = I_obsidian;
  grid[3] = I_obsidian; grid[4] = I_ender_eye; grid[5] = I_obsidian;
  grid[6] = grid[7] = grid[8] = I_obsidian;
  ADD_SHAPED(3, 3, grid, I_ender_chest, 0);

  // 42. Bookshelf (3×3)
  grid[0] = grid[1] = grid[2] = g_planks[0];
  grid[3] = grid[4] = grid[5] = I_book;
  grid[6] = grid[7] = grid[8] = g_planks[0];
  ADD_SHAPED(3, 3, grid, I_bookshelf, 3);

  // 43. Campfire (3×3)
  grid[0] = grid[1] = grid[2] = I_stick;
  grid[3] = I_stick; grid[4] = I_coal; grid[5] = I_stick;
  grid[6] = grid[7] = grid[8] = I_oak_log;
  ADD_SHAPED(3, 3, grid, I_campfire, 3);

  // 44. 3×3 Uniform + Melon
  for (int i = 0; i < BLOCK_CRAFT_ENTRIES; i++) {
    uint16_t m = g_block_craft[i][0];
    grid[0] = grid[1] = grid[2] = grid[3] = grid[4] = m;
    grid[5] = grid[6] = grid[7] = grid[8] = m;
    ADD_SHAPED(3, 3, grid, g_block_craft[i][1], 0);
  }
  { uint16_t m = I_melon_slice;
    grid[0] = grid[1] = grid[2] = grid[3] = grid[4] = m;
    grid[5] = grid[6] = grid[7] = grid[8] = m;
    ADD_SHAPED(3, 3, grid, I_melon, 3); }

  // 45. Flint & Steel (shapeless display)
  {
    writeVarInt(client_fd, idx++);  // displayId
    writeVarInt(client_fd, 0);      // type: crafting_shapeless
    writeVarInt(client_fd, 2);      // 2 ingredients
    writeSlotItem(client_fd, I_iron_ingot);
    writeSlotItem(client_fd, I_flint);
    writeSlotItem(client_fd, I_flint_and_steel);  // result
    writeSlotItem(client_fd, I_crafting_table);   // station
    writeVarInt(client_fd, 0);  // group
    writeVarInt(client_fd, 3);  // category: misc
    writeByte(client_fd, 0);    // requirements
    writeByte(client_fd, 0);    // flags
  }

  // 46. Smelting (furnace display)
  for (int i = 0; i < SMELTING_ENTRIES; i++) {
    writeVarInt(client_fd, idx++);
    writeVarInt(client_fd, 2);      // type: furnace
    writeSlotItem(client_fd, g_smelting_in[i]);   // ingredient
    writeVarInt(client_fd, 1);                     // fuel: any_fuel
    writeSlotItem(client_fd, g_smelting_out[i]);   // result
    writeSlotItem(client_fd, I_furnace);           // station
    writeVarInt(client_fd, 200);  // duration
    writeFloat(client_fd, 0.1f); // experience
    writeVarInt(client_fd, 0);    // group
    {
      // category: furnace_food or furnace_blocks
      int cat = 4;  // furnace_food
      uint16_t o = g_smelting_out[i];
      if (o == I_cooked_chicken || o == I_cooked_beef || o == I_cooked_porkchop ||
          o == I_cooked_mutton || o == I_cooked_cod || o == I_cooked_salmon ||
          o == I_cooked_rabbit || o == I_baked_potato) cat = 4; // food
      else if (o == I_green_dye || o == I_leather || o == I_brick) cat = 6; // misc
      else cat = 5; // blocks
      writeVarInt(client_fd, cat);
    }
    writeByte(client_fd, 0);  // requirements
    writeByte(client_fd, 0);  // flags
  }
  // Extra smelting (stone->smooth_stone, sponge->wet, wet->sponge)
  for (int i = 0; i < SMELTING_EXTRA; i++) {
    writeVarInt(client_fd, idx++);
    writeVarInt(client_fd, 2);
    writeSlotItem(client_fd, g_smelting_extra_in[i]);
    writeVarInt(client_fd, 1);  // any_fuel
    writeSlotItem(client_fd, g_smelting_extra_out[i]);
    writeSlotItem(client_fd, I_furnace);
    writeVarInt(client_fd, 200);
    writeFloat(client_fd, 0.1f);
    writeVarInt(client_fd, 0);
    writeVarInt(client_fd, 5);  // category: blocks
    writeByte(client_fd, 0);
    writeByte(client_fd, 0);
  }

  writeByte(client_fd, 0);    // replace = false

  endPacket(client_fd);
  return 0;
}

// Handle C->S Craft Recipe Request (0x26) — player clicked a recipe in the book.
int cs_craftRecipeRequest(int client_fd) {
  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return 1;

  int window_id = readVarInt(client_fd);
  int recipe_id = readVarInt(client_fd);
  readByte(client_fd); // make_all — unused

  if (window_id != 0) return 0;

  // Map recipe_id to ingredient items + grid info
  uint16_t ing[9] = {0};
  int w = 1, h = 1;
  int rid = recipe_id;

  // ── Ranges (matched to sc_declareRecipes order) ──
  // 1. Log → 4 Planks (12 × 1×1)
  if (rid < LOG_PLANKS_ENTRIES) {
    ing[0] = g_log_planks[rid][0]; w=1; h=1; goto found; }
  rid -= LOG_PLANKS_ENTRIES;

  // 2. Bone Meal
  if (rid == 0) { ing[0] = I_bone; goto found; } rid--;

  // 3. Block decompose (12) + honey + bone
  if (rid < DECOMPOSE_ENTRIES) { ing[0] = g_decompose[rid][0]; goto found; }
  rid -= DECOMPOSE_ENTRIES;
  if (rid == 0) { ing[0] = I_honey_block; goto found; } rid--;
  if (rid == 0) { ing[0] = I_bone_block; goto found; } rid--;

  // 4. Buttons (14 × 1×1)
  if (rid < BUTTON_TYPES) { ing[0] = g_button_materials[rid]; goto found; }
  rid -= BUTTON_TYPES;

  // 5. Pressure plates (14 × 2×1 horz)
  if (rid < PRESSURE_PLATE_TYPES) {
    ing[0]=ing[1]=g_pressure_materials[rid]; w=2; goto found; }
  rid -= PRESSURE_PLATE_TYPES;

  // 6. Stick
  if (rid == 0) { ing[0]=ing[1]=g_planks[0]; w=2; goto found; } rid--;

  // 7. Torch
  if (rid == 0) { ing[0]=I_coal; ing[1]=I_stick; w=2; goto found; } rid--;

  // 8. Shears (2×2 diagonal)
  if (rid == 0) { ing[0]=I_iron_ingot; ing[3]=I_iron_ingot; w=2; h=2; goto found; } rid--;

  // 9. Arrow
  if (rid == 0) { ing[0]=I_flint; ing[1]=I_stick; ing[2]=I_feather; w=3; goto found; } rid--;

  // 10. Bucket (3×2 V)
  if (rid == 0) {
    ing[0]=I_iron_ingot; ing[2]=I_iron_ingot; ing[4]=I_iron_ingot; w=3; h=2; goto found; } rid--;

  // 11. Bread
  if (rid == 0) { ing[0]=ing[1]=ing[2]=I_wheat; w=3; goto found; } rid--;

  // 12. Slabs: 12 plank + 5 stone
  if (rid < SLAB_TYPES) { ing[0]=ing[1]=ing[2]=g_planks[rid]; w=3; goto found; }
  rid -= SLAB_TYPES;
  if (rid == 0) { ing[0]=ing[1]=ing[2]=I_cobblestone; w=3; goto found; } rid--;
  if (rid == 0) { ing[0]=ing[1]=ing[2]=I_stone; w=3; goto found; } rid--;
  if (rid == 0) { ing[0]=ing[1]=ing[2]=I_stone_bricks; w=3; goto found; } rid--;
  if (rid == 0) { ing[0]=ing[1]=ing[2]=I_bricks; w=3; goto found; } rid--;
  if (rid == 0) { ing[0]=ing[1]=ing[2]=I_nether_bricks; w=3; goto found; } rid--;

  // 13. Shovels (1 wood + 5 tool)
  if (rid == 0) { ing[0]=g_planks[0]; ing[1]=ing[2]=I_stick; w=3; goto found; } rid--;
  if (rid < TOOL_MATERIALS) { ing[0]=g_tool_materials[rid]; ing[1]=ing[2]=I_stick; w=3; goto found; }
  rid -= TOOL_MATERIALS;

  // 14. Swords (1 wood + 5 tool)
  if (rid == 0) { ing[0]=ing[1]=g_planks[0]; ing[2]=I_stick; w=3; goto found; } rid--;
  if (rid < TOOL_MATERIALS) { ing[0]=ing[1]=g_tool_materials[rid]; ing[2]=I_stick; w=3; goto found; }
  rid -= TOOL_MATERIALS;

  // 15. Crafting Table
  if (rid == 0) { ing[0]=ing[1]=ing[2]=ing[3]=g_planks[0]; w=2; h=2; goto found; } rid--;

  // 16. 2×2 Uniform (11 wood + snow + stonebrick + brick + netherbrick)
  if (rid < WOOD_ENTRIES) {
    ing[0]=ing[1]=ing[2]=ing[3]=g_wood[rid][0]; w=2; h=2; goto found; }
  rid -= WOOD_ENTRIES;
  if (rid == 0) { ing[0]=ing[1]=ing[2]=ing[3]=I_snowball; w=2; h=2; goto found; } rid--;
  if (rid == 0) { ing[0]=ing[1]=ing[2]=ing[3]=I_stone; w=2; h=2; goto found; } rid--;
  if (rid == 0) { ing[0]=ing[1]=ing[2]=ing[3]=I_brick; w=2; h=2; goto found; } rid--;
  if (rid == 0) { ing[0]=ing[1]=ing[2]=ing[3]=I_nether_brick; w=2; h=2; goto found; } rid--;

  // 17. Boots (5 armor)
  if (rid < ARMOR_MATERIALS) {
    ing[0]=ing[2]=ing[3]=ing[5]=g_armor_materials[rid]; w=3; h=2; goto found; }
  rid -= ARMOR_MATERIALS;

  // 18. Pickaxes (1 wood + 5 tool)
  if (rid == 0) {
    ing[0]=ing[1]=ing[2]=g_planks[0]; ing[4]=ing[7]=I_stick; w=3; h=3; goto found; } rid--;
  if (rid < TOOL_MATERIALS) {
    ing[0]=ing[1]=ing[2]=g_tool_materials[rid]; ing[4]=ing[7]=I_stick; w=3; h=3; goto found; }
  rid -= TOOL_MATERIALS;

  // 19. Axes (1 wood + 5 tool)
  if (rid == 0) {
    ing[0]=ing[1]=g_planks[0]; ing[2]=g_planks[0]; ing[3]=I_stick; ing[5]=I_stick; w=2; h=3; goto found; } rid--;
  if (rid < TOOL_MATERIALS) {
    ing[0]=ing[1]=g_tool_materials[rid]; ing[2]=g_tool_materials[rid]; ing[3]=I_stick; ing[5]=I_stick; w=2; h=3; goto found; }
  rid -= TOOL_MATERIALS;

  // 20. Helmets (5 armor)
  if (rid < ARMOR_MATERIALS) {
    ing[0]=ing[1]=ing[2]=ing[3]=ing[5]=g_armor_materials[rid]; w=3; h=2; goto found; }
  rid -= ARMOR_MATERIALS;

  // 21. Chestplates (5 armor)
  if (rid < ARMOR_MATERIALS) {
    ing[0]=ing[2]=g_armor_materials[rid]; ing[3]=ing[4]=ing[5]=g_armor_materials[rid];
    ing[6]=ing[8]=g_armor_materials[rid]; w=3; h=3; goto found; }
  rid -= ARMOR_MATERIALS;

  // 22. Leggings (5 armor)
  if (rid < ARMOR_MATERIALS) {
    ing[0]=ing[1]=ing[2]=ing[3]=ing[5]=ing[6]=ing[8]=g_armor_materials[rid]; w=3; h=3; goto found; }
  rid -= ARMOR_MATERIALS;

  // 23. Bow — complex 3×3
  if (rid == 0) {
    ing[1]=I_stick; ing[2]=I_string; ing[3]=I_stick; ing[5]=I_string; ing[7]=I_stick; ing[8]=I_string; w=3; h=3; goto found; } rid--;

  // 24. Beds (16 × 3 wool + 3 planks)
  if (rid < BED_COLORS) {
    ing[0]=ing[1]=ing[2]=g_wools[rid]; ing[3]=ing[4]=ing[5]=g_planks[0]; w=3; h=2; goto found; }
  rid -= BED_COLORS;

  // 25. Doors (10 × 2×3)
  if (rid < DOOR_TYPES) {
    ing[0]=ing[1]=ing[2]=ing[3]=ing[4]=ing[5]=g_planks[rid]; w=2; h=3; goto found; }
  rid -= DOOR_TYPES;

  // 26. Stairs (12 × 3×3 pattern)
  if (rid < STAIR_TYPES) {
    ing[0]=ing[3]=ing[4]=ing[6]=ing[7]=ing[8]=g_planks[rid]; w=3; h=3; goto found; }
  rid -= STAIR_TYPES;

  // 27. Fences (10 × 3×2)
  if (rid < FENCE_TYPES) {
    ing[0]=ing[2]=ing[3]=ing[5]=g_planks[rid]; ing[1]=ing[4]=I_stick; w=3; h=2; goto found; }
  rid -= FENCE_TYPES;

  // 28. Trapdoors (9 × 3×2 full)
  if (rid < TRAPDOOR_TYPES) {
    ing[0]=ing[1]=ing[2]=ing[3]=ing[4]=ing[5]=g_planks[rid]; w=3; h=2; goto found; }
  rid -= TRAPDOOR_TYPES;

  // 29. Fence gates (12 × 3×2)
  if (rid < FENCE_GATE_TYPES) {
    ing[0]=ing[2]=I_stick; ing[1]=ing[4]=g_planks[rid]; ing[3]=ing[5]=I_stick; w=3; h=2; goto found; }
  rid -= FENCE_GATE_TYPES;

  // 30. Glass pane
  if (rid == 0) { ing[0]=ing[1]=ing[2]=ing[3]=ing[4]=ing[5]=I_glass; w=3; h=2; goto found; } rid--;
  // 31. Iron bars
  if (rid == 0) { ing[0]=ing[1]=ing[2]=ing[3]=ing[4]=ing[5]=I_iron_ingot; w=3; h=2; goto found; } rid--;
  // 32. Cobblestone wall
  if (rid == 0) { ing[0]=ing[1]=ing[2]=ing[3]=ing[4]=ing[5]=I_cobblestone; w=3; h=2; goto found; } rid--;
  // 33. Stone brick stairs
  if (rid == 0) {
    ing[0]=ing[3]=ing[4]=ing[6]=ing[7]=ing[8]=I_stone_bricks; w=3; h=3; goto found; } rid--;
  // 34. Cobblestone stairs
  if (rid == 0) {
    ing[0]=ing[3]=ing[4]=ing[6]=ing[7]=ing[8]=I_cobblestone; w=3; h=3; goto found; } rid--;

  // 35. Ladder
  if (rid == 0) {
    ing[0]=ing[2]=ing[3]=ing[4]=ing[5]=ing[6]=ing[8]=I_stick; w=3; h=3; goto found; } rid--;

  // 36. Shield
  if (rid == 0) {
    ing[0]=ing[2]=g_planks[0]; ing[1]=I_iron_ingot; ing[3]=ing[4]=ing[5]=ing[7]=g_planks[0]; w=3; h=3; goto found; } rid--;

  // 37. Composter
  if (rid == 0) {
    ing[0]=ing[2]=ing[3]=ing[5]=ing[6]=ing[7]=ing[8]=I_oak_slab; w=3; h=3; goto found; } rid--;

  // 38. Furnace
  if (rid == 0) {
    ing[0]=ing[1]=ing[2]=ing[3]=ing[5]=ing[6]=ing[7]=ing[8]=I_cobblestone; w=3; h=3; goto found; } rid--;

  // 39. Chest
  if (rid == 0) {
    ing[0]=ing[1]=ing[2]=ing[3]=ing[5]=ing[6]=ing[7]=ing[8]=g_planks[0]; w=3; h=3; goto found; } rid--;

  // 40. Barrel
  if (rid == 0) {
    ing[0]=ing[2]=ing[3]=ing[5]=ing[6]=ing[8]=g_planks[0]; ing[1]=ing[7]=I_oak_slab; w=3; h=3; goto found; } rid--;

  // 41. Ender chest
  if (rid == 0) {
    ing[0]=ing[1]=ing[2]=ing[3]=ing[5]=ing[6]=ing[7]=ing[8]=I_obsidian; ing[4]=I_ender_eye; w=3; h=3; goto found; } rid--;

  // 42. Bookshelf
  if (rid == 0) {
    ing[0]=ing[1]=ing[2]=ing[6]=ing[7]=ing[8]=g_planks[0]; ing[3]=ing[4]=ing[5]=I_book; w=3; h=3; goto found; } rid--;

  // 43. Campfire
  if (rid == 0) {
    ing[0]=ing[1]=ing[2]=ing[3]=ing[5]=I_stick; ing[4]=I_coal; ing[6]=ing[7]=ing[8]=I_oak_log; w=3; h=3; goto found; } rid--;

  // 44. 3×3 Uniform (13 + melon)
  if (rid < BLOCK_CRAFT_ENTRIES) {
    uint16_t m = g_block_craft[rid][0];
    for (int i=0;i<9;i++) ing[i]=m; w=3; h=3; goto found; }
  rid -= BLOCK_CRAFT_ENTRIES;
  if (rid == 0) { for (int i=0;i<9;i++) ing[i]=I_melon_slice; w=3; h=3; goto found; } rid--;

  // 45. Flint & Steel (shapeless — 2 items, any position)
  if (rid == 0) { ing[0]=I_iron_ingot; ing[1]=I_flint; w=2; h=1; goto found; } rid--;

  // 46. Smelting (furnace — single ingredient, not grid craftable)
  // Skip — smelting requires a furnace GUI, not the crafting grid.

  return 0;  // Unknown or furnace recipe

found:
  // We have the ingredients. Now fill the grid (server slots 0,1,3,4 for 2×2).
  // The `ing` array is row-major but only w×h slots matter.
  // Map to server craft slots {0,1,3,4}: slot 0=top-left, 1=top-right, 3=bottom-left, 4=bottom-right
  uint8_t craft_slots[] = {0, 1, 3, 4};
  for (int i = 0; i < 4; i++) {
    player->craft_items[craft_slots[i]] = 0;
    player->craft_count[craft_slots[i]] = 0;
  }

  // For a 1×1,1×2,2×1 recipe: slot 0 gets ing[0]; if w>1, slot1 gets ing[1]; if h>1, slot3 gets ing[2], slot4 gets ing[3]
  // ing layout: row-major: ing[0]=top-left, ing[1]=top-right, ing[2]=bottom-left, ing[3]=bottom-right
  // Map to server slots 0,1,3,4 (the 2×2 player crafting grid)
  int sip[9] = {0,1,-1,3,4,-1,-1,-1,-1}; // server slot index for each logical position
  int failed = 0;
  for (int row = 0; row < h && row < 2; row++) {
    for (int col = 0; col < w && col < 2; col++) {
      int si = row * 2 + col;  // logical slot within 2×2
      int gi = row * w + col;  // index into ing array
      uint16_t need = ing[gi];
      if (need == 0) continue;
      // Find in inventory
      int fs = -1;
      for (int k = 0; k < 36; k++) {
        if (player->inventory_items[k] == need && player->inventory_count[k] > 0) {
          fs = k; break;
        }
      }
      if (fs == -1) { failed = 1; break; }
      // Move from inventory to grid
      player->inventory_count[fs]--;
      if (player->inventory_count[fs] == 0) player->inventory_items[fs] = 0;
      uint8_t cs = serverSlotToClientSlot(0, fs);
      if (cs != 255)
        sc_setContainerSlot(client_fd, 0, cs, player->inventory_count[fs], player->inventory_items[fs]);
      // Place in grid at craft_slots[si]
      player->craft_items[craft_slots[si]] = need;
      player->craft_count[craft_slots[si]] = 1;
    }
    if (failed) break;
  }

  if (failed) return 0;

  syncCraftingSlots(player, 0);
  uint16_t out_item = 0;
  uint8_t out_count = 0;
  getCraftingOutput(player, &out_count, &out_item);
  sc_setContainerSlot(client_fd, 0, 0, out_count, out_item);
  return 1;
}

int cs_playerLoaded (int client_fd) {

  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return 1;

  // Redirect handling to player join procedure
  handlePlayerJoin(player);

  return 0;
}

// S->C Light Update - send block light data for a single chunk section
// S->C Registry Data (multiple packets) and Update Tags (configuration, multiple packets)
int sc_registries (int client_fd) {

  printf("Sending Registries\n\n");
  sendPreformattedPackets(client_fd, registries_bin, sizeof(registries_bin));

  printf("Sending Tags\n\n");
  sendPreformattedPackets(client_fd, tags_bin, sizeof(tags_bin));

  return 0;

}
