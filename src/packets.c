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
  writeVarInt(client_fd, 1);
  if (dimension == DIMENSION_OVERWORLD) {
    writeVarInt(client_fd, 18);
    send_all(client_fd, "minecraft:overworld", 18);
  } else {
    writeVarInt(client_fd, 19);
    send_all(client_fd, "minecraft:the_nether", 19);
  }
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
  if (dimension == DIMENSION_OVERWORLD) {
    writeVarInt(client_fd, 18);
    send_all(client_fd, "minecraft:overworld", 18);
  } else {
    writeVarInt(client_fd, 19);
    send_all(client_fd, "minecraft:the_nether", 19);
  }
  // hashed seed
  writeUint64(client_fd, 0x0123456789ABCDEF);
  // gamemode
  writeByte(client_fd, GAMEMODE);
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
  // Nether:     min_y=0,   height=256 -> 16 sections
  int terrain_sections = (dimension == DIMENSION_NETHER) ? 16 : 20;
  int bedrock_sections = (dimension == DIMENSION_NETHER) ? 0  : 4;
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

  // Scan terrain sections for village-generated chests that need block entities
  // in the chunk data packet (otherwise they render invisible).
  int chest_count = 0;
  int chest_wx[64], chest_wy[64], chest_wz[64];
  for (int _s = 0; _s < terrain_sections && chest_count < 64; _s++) {
    int section_y = (dimension == DIMENSION_NETHER) ? _s * 16 : _s * 16;
    for (int _j = 0; _j < 4096 && chest_count < 64; _j++) {
      uint16_t raw = cached_sections[_s][_j];
      if ((raw & 0x8000) && (raw & 0x1FF) == B_chest) {
        int addr = (_j & ~7) | (7 - (_j & 7));
        chest_wx[chest_count] = x + (addr & 15);
        chest_wz[chest_count] = z + ((addr >> 4) & 15);
        chest_wy[chest_count] = section_y + (addr >> 8);
        chest_count++;
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

  // Block entities section — needed for chests (and other tile entity blocks)
  // to render correctly. Without it, chests appear invisible with only a hitbox.
  writeVarInt(client_fd, chest_count);
  for (int _be = 0; _be < chest_count; _be++) {
    writeByte(client_fd, ((chest_wx[_be] & 15) << 4) | (chest_wz[_be] & 15));
    writeByte(client_fd, (chest_wy[_be] >> 8) & 0xFF);
    writeByte(client_fd, chest_wy[_be] & 0xFF);
    writeVarInt(client_fd, 1); // block entity type ID for chest
    // Anonymous network NBT: TAG_Compound type byte, then compound data (no name),
    // terminated by TAG_End.
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
      !isOrientedBlock(block_changes_snapshot[i].block) &&
      !isStairBlock(block_changes_snapshot[i].block)
      #ifdef ALLOW_DOORS
      && !isDoorBlock(block_changes_snapshot[i].block)
      && !isTrapdoorBlock(block_changes_snapshot[i].block)
      #endif
    ) continue;

    if (block_changes_snapshot[i].x < x || block_changes_snapshot[i].x >= x + 16) {
      if (block_changes_snapshot[i].block == B_chest) {
        if (i + 14 >= block_changes_snapshot_count) continue;
        i += 14;
      } else if (isStairBlock(block_changes_snapshot[i].block) || block_changes_snapshot[i].block == B_furnace) i += 1;
      else if (isDoorBlock(block_changes_snapshot[i].block)) i += 2;
      continue;
    }
    if (block_changes_snapshot[i].z < z || block_changes_snapshot[i].z >= z + 16) {
      if (block_changes_snapshot[i].block == B_chest) {
        if (i + 14 >= block_changes_snapshot_count) continue;
        i += 14;
      } else if (isStairBlock(block_changes_snapshot[i].block) || block_changes_snapshot[i].block == B_furnace) i += 1;
      else if (isDoorBlock(block_changes_snapshot[i].block)) i += 2;
      continue;
    }

    #ifdef ALLOW_DOORS
    if (isDoorBlock(block_changes_snapshot[i].block)) {
      // Skip upper half of doors - they are sent together with lower half
      uint16_t lower_state = special_block_get_state(block_changes_snapshot[i].x, block_changes_snapshot[i].y - 1, block_changes_snapshot[i].z);
      if (lower_state != 0) {
        // This is an upper half (lower half exists below), skip it
        // Still need to skip state entries
        if (i + 2 < block_changes_snapshot_count) i += 2;
        continue;
      }

      // Bounds check: ensure i+1 and i+2 are valid indices
      if (i + 2 >= block_changes_snapshot_count) continue;

      // Read state from the unified special block table
      uint16_t state = special_block_get_state(block_changes_snapshot[i].x, block_changes_snapshot[i].y, block_changes_snapshot[i].z);
      uint8_t open = door_get_open(state);
      uint8_t hinge = door_get_hinge(state);
      uint8_t direction = door_get_direction(state);

      // Send door with proper state (both halves)
      sendDoorUpdate(client_fd, block_changes_snapshot[i].x, block_changes_snapshot[i].y, block_changes_snapshot[i].z, block_changes_snapshot[i].block, 0, open, direction, hinge);
      // Check if upper half exists in special block table
      uint16_t upper_state = special_block_get_state(block_changes_snapshot[i].x, block_changes_snapshot[i].y + 1, block_changes_snapshot[i].z);
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
      uint16_t state = special_block_get_state(block_changes_snapshot[i].x, block_changes_snapshot[i].y, block_changes_snapshot[i].z);
      uint8_t open = trapdoor_get_open(state);
      uint8_t half = trapdoor_get_half(state);
      uint8_t direction = trapdoor_get_direction(state);

      sendTrapdoorUpdate(client_fd, block_changes_snapshot[i].x, block_changes_snapshot[i].y, block_changes_snapshot[i].z, block_changes_snapshot[i].block, open, direction, half);
      continue;
    }
    #endif

    if (isOrientedBlock(block_changes_snapshot[i].block)) {
      // Read direction from the unified special block table
      uint16_t state = special_block_get_state(block_changes_snapshot[i].x, block_changes_snapshot[i].y, block_changes_snapshot[i].z);
      uint8_t direction = oriented_get_direction(state);

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
      uint16_t state = special_block_get_state(block_changes_snapshot[i].x, block_changes_snapshot[i].y, block_changes_snapshot[i].z);
      uint8_t half = stair_get_half(state);
      uint8_t direction = stair_get_direction(state);

      sendStairUpdate(client_fd, block_changes_snapshot[i].x, block_changes_snapshot[i].y, block_changes_snapshot[i].z, block_changes_snapshot[i].block, half, direction);
      // Skip the next entry (state data)
      i += 1;
      continue;
    }

    sc_blockUpdate(client_fd, block_changes_snapshot[i].x, block_changes_snapshot[i].y, block_changes_snapshot[i].z, block_changes_snapshot[i].block);
  }

  // Send updates for village-generated doors and chests (not in block changes).
  // Chunk data has packed values; correct state and special_block entries
  // are applied here.
  #ifdef ALLOW_DOORS
  int processed_village[64][3];
  int num_village = 0;
  for (int i = 0; i < terrain_sections; i++) {
    int section_y = (dimension == DIMENSION_NETHER) ? i * 16 : i * 16;
    if (section_y > 200 || section_y + 16 < 60) continue;
    uint16_t *section = cached_sections[i];
    for (int j = 0; j < 4096; j++) {
      uint16_t raw = section[j];
      uint16_t block_type = (raw & 0x8000) ? (raw & 0x1FF) : raw;
      if (!isDoorBlock(block_type) && block_type != B_chest) continue;

      int address = (j & ~7) | (7 - (j & 7));
      int wx = x + (address & 15);
      int wz = z + ((address >> 4) & 15);
      int wy = section_y + (address >> 8);

      int skip = 0;
      for (int p = 0; p < num_village; p++)
        if (processed_village[p][0] == wx &&
            processed_village[p][1] == wy &&
            processed_village[p][2] == wz) { skip = 1; break; }
      if (skip) continue;

      for (int k = 0; k < block_changes_snapshot_count && !skip; k++) {
        if (block_changes_snapshot[k].block == 0xFF) continue;
        if ((isDoorBlock(block_changes_snapshot[k].block) || block_changes_snapshot[k].block == B_chest) &&
            block_changes_snapshot[k].x == wx &&
            block_changes_snapshot[k].y == wy &&
            block_changes_snapshot[k].z == wz) skip = 1;
      }
      if (skip) continue;

      if (block_type == B_chest) {
        uint8_t direction = (raw >> 9) & 3;
        if (!special_block_has_entry(wx, wy, wz))
          special_block_set_state(wx, wy, wz, block_type, oriented_encode_state(direction));
        uint16_t st = special_block_get_state(wx, wy, wz);
        sendOrientedUpdate(client_fd, wx, wy, wz, block_type, oriented_get_direction(st));
        if (num_village < 62) {
          processed_village[num_village][0] = wx;
          processed_village[num_village][1] = wy;
          processed_village[num_village][2] = wz;
          num_village++;
        }
      } else {
        // Read existing state or init default
        if (!special_block_has_entry(wx, wy, wz)) {
          uint16_t vs = door_encode_state(0, 0, 1);
          special_block_set_state(wx, wy, wz, block_type, vs);
          if (!special_block_has_entry(wx, wy + 1, wz))
            special_block_set_state(wx, wy + 1, wz, block_type, vs);
        }
        // Send correct state to client (chunk data may have stale default)
        uint16_t st = special_block_get_state(wx, wy, wz);
        uint8_t open = door_get_open(st);
        uint8_t hinge = door_get_hinge(st);
        uint8_t dir = door_get_direction(st);
        sendDoorUpdate(client_fd, wx, wy, wz, block_type, 0, open, dir, hinge);
        sendDoorUpdate(client_fd, wx, wy + 1, wz, block_type, 1, open, dir, hinge);

        if (num_village < 62) {
          processed_village[num_village][0] = wx;
          processed_village[num_village][1] = wy;
          processed_village[num_village][2] = wz;
          num_village++;
          processed_village[num_village][0] = wx;
          processed_village[num_village][1] = wy + 1;
          processed_village[num_village][2] = wz;
          num_village++;
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

  handlePlayerUseItem(player, 0, 0, 0, 255);

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

  handlePlayerUseItem(player, x, y, z, face);

  return 0;
}

static void readChangedSlotPayload (int client_fd, uint8_t *present, uint16_t *item, uint8_t *count) {

  *present = readByte(client_fd);
  *item = 0;
  *count = 0;

  if (!*present) return;

  *item = readVarInt(client_fd);
  *count = (uint8_t)readVarInt(client_fd);

  // ignore components
  readLengthPrefixedData(client_fd);
  readLengthPrefixedData(client_fd);

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
  #endif

  for (int i = 0; i < changes_count; i ++) {

    slot = clientSlotToServerSlot(window_id, readUint16(client_fd));
    readChangedSlotPayload(client_fd, &present, &item, &count);
    if (slot > 67) continue; // skip out-of-bounds slots after consuming their payload
    // slots outside of the inventory overflow into the crafting buffer
    if (slot > 40 && apply_changes) craft = true;

    #ifdef ALLOW_CHESTS
    if (window_id == 2 && slot > 40) {
      uint8_t *base = (uint8_t *)&block_changes[chest_idx + 1];
      p_item = (uint16_t *)(base + (slot - 41) * 3);
      p_count = base + (slot - 41) * 3 + 2;
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
        if (window_id == 2 && slot > 40) {
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
      if (window_id == 2 && slot > 40) {
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
  }

  // assign cursor-carried item slot
  if (readByte(client_fd)) {
    player->flagval_16 = readVarInt(client_fd);
    player->flagval_8 = readVarInt(client_fd);
    // ignore components
    readLengthPrefixedData(client_fd);
    readLengthPrefixedData(client_fd);
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
  if (window_id == 2) {
    memcpy(&chest_idx, player->craft_items, sizeof(chest_idx));
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
  #endif

  givePlayerItem(player, player->flagval_16, player->flagval_8);
  sc_setCursorItem(client_fd, 0, 0);
  player->flagval_16 = 0;
  player->flagval_8 = 0;

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
  writeVarInt(client_fd, GAMEMODE);

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
  uint8_t yaw, uint8_t pitch
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

  // Velocity
  writeUint16(client_fd, 0);
  writeUint16(client_fd, 0);
  writeUint16(client_fd, 0);

  endPacket(client_fd);

  return 0;
}

// S->C Set Entity Velocity
int sc_setEntityVelocity (int client_fd, int id, int16_t vx, int16_t vy, int16_t vz) {
  startPacket(client_fd, 0x1C);

  writeVarInt(client_fd, id); // Entity ID
  writeInt16(client_fd, vx);
  writeInt16(client_fd, vy);
  writeInt16(client_fd, vz);

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

// S->C Spawn Entity (from PlayerData)
int sc_spawnEntityPlayer (int client_fd, PlayerData player) {
  return sc_spawnEntity(
    client_fd,
    player.client_fd, player.uuid, 149,
    player.x > 0 ? (double)player.x + 0.5 : (double)player.x - 0.5,
    player.y,
    player.z > 0 ? (double)player.z + 0.5 : (float)player.z - 0.5,
    player.yaw, player.pitch
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
  if (dimension == DIMENSION_OVERWORLD) {
    writeVarInt(client_fd, 18);
    send_all(client_fd, "minecraft:overworld", 18);
  } else {
    writeVarInt(client_fd, 19);
    send_all(client_fd, "minecraft:the_nether", 19);
  }
  // hashed seed
  writeUint64(client_fd, 0x0123456789ABCDEF);
  // gamemode
  writeByte(client_fd, GAMEMODE);
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

  if (recv_buffer[0] != '!') { // Standard chat message

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

  if (!strncmp((char *)recv_buffer, "!help", 5)) {
    // Send command guide
    const char help_msg[] = "§7Commands:\n"
    "  !msg <player> <message> - Send a private message\n"
    "  !portal - Build a Nether portal at your position\n"
    "  !biome [x z] - Show biome at current or given coordinates\n"
    "  !tp <x> <y> <z> - Teleport to coordinates\n"
    "  !timeset <ticks> - Set world time (0=day, 12000=night)\n"
    "  !findbiome <name> [radius] - Find and teleport to nearest biome\n"
    "  !find biome <name> [radius] - Alias for !findbiome\n"
    "  !creative - Toggle creative mode UI / !creative list - List all items\n"
    "  !creative <item_name> - Give yourself an item (e.g., !creative oak_log)\n"
    "  !noclip [on|off] - Toggle spectator-style noclip movement\n"
    "  !help - Show this help message";
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

  // !timeset <ticks> - Set the world time (0-23999, 12000=night, 0=day)
  if (!strncmp((char *)recv_buffer, "!timeset", 8)) {
    if (message_len < 10) {
      sc_systemChat(client_fd, "§7Usage: !timeset <ticks>", 28);
      goto cleanup;
    }
    int ci = 9;
    while (recv_buffer[ci] == ' ' && ci < 224) ci++;
    int ci_start = ci;
    while (recv_buffer[ci] != ' ' && recv_buffer[ci] != '\0' && ci < 224) ci++;
    if (ci <= ci_start) {
      sc_systemChat(client_fd, "§7Usage: !timeset <ticks>", 28);
      goto cleanup;
    }
    char time_buf[16];
    memcpy(time_buf, recv_buffer + ci_start, ci - ci_start);
    time_buf[ci - ci_start] = '\0';
    int new_time = atoi(time_buf);
    new_time = new_time % 24000;
    world_time = new_time;
    char response[64];
    int resp_len = snprintf(response, sizeof(response), "§7World time set to §f%d", new_time);
    sc_systemChat(client_fd, response, (uint16_t)resp_len);
    for (int i = 0; i < MAX_PLAYERS; i++) {
      if (player_data[i].client_fd == -1) continue;
      if (player_data[i].flags & 0x20) continue;
      sc_updateTime(player_data[i].client_fd, world_time);
    }
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
    hurtEntity(entity_id, client_fd, D_generic, 1);
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

// C->S Player Loaded
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
