#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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

// S->C Status Response (server list ping)
int sc_statusResponse (int client_fd) {

  char max_players_str[16];
  snprintf(max_players_str, sizeof(max_players_str), "%d", config.max_players);
  char online_players_str[16];
  // Subtract 1 because the pinging client is not yet logged in
  snprintf(online_players_str, sizeof(online_players_str), "%u", client_count > 0 ? client_count - 1 : 0);

  // Build the full JSON for debugging
  char json[512];
  snprintf(json, sizeof(json),
    "{\"version\":{\"name\":\"1.21.8\",\"protocol\":772},"
    "\"players\":{\"max\":%s,\"online\":%s},"
    "\"description\":{\"text\":\"%.*s\"}}",
    max_players_str, online_players_str, motd_len, motd);

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

  uint8_t name_length = strlen(name);
  startPacket(client_fd, 0x02);
  send_all(client_fd, uuid, 16);
  writeVarInt(client_fd, name_length);
  send_all(client_fd, name, name_length);
  writeVarInt(client_fd, 0);

  endPacket(client_fd);

  return 0;
}

int cs_clientInformation (int client_fd) {
  int tmp;
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
  printf("  Skin parts: %d\n", tmp);
  tmp = readVarInt(client_fd);
  if (recv_count == -1) return 1;
  if (tmp) printf("  Main hand: right\n");
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
int sc_loginPlay (int client_fd) {

  startPacket(client_fd, 0x2B);
  // entity id
  writeUint32(client_fd, client_fd);
  // hardcore
  writeByte(client_fd, false);
  // dimensions
  writeVarInt(client_fd, 1);
  writeVarInt(client_fd, 9);
  const char *dimension = "overworld";
  send_all(client_fd, dimension, 9);
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
  // the server only sends "overworld"
  writeVarInt(client_fd, 0);
  // dimension name
  writeVarInt(client_fd, 9);
  send_all(client_fd, dimension, 9);
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
  writeVarInt(client_fd, 0);
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

// S->C Game Event 13 (Start waiting for level chunks)
int sc_startWaitingForChunks (int client_fd) {
  startPacket(client_fd, 0x22);
  writeByte(client_fd, 13);
  writeUint32(client_fd, 0);
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

// S->C Chunk Data and Update Light
int sc_chunkDataAndUpdateLight (int client_fd, int _x, int _z) {
  chunk_debug_record_stream_request(client_fd, _x, _z);

  // Backpressure: if low-priority chunk backlog is already large, delay chunk packets
  // so gameplay packets (block updates, interactions) stay responsive.
  size_t chunk_queue_bytes = get_client_send_queue_bytes(client_fd);
  if (chunk_queue_bytes > (1024 * 1024)) {
    chunk_debug_record_backpressure_skip(client_fd, chunk_queue_bytes);
    request_chunk_generation(_x, _z);
    return 1;
  }

  // Use a static buffer to build the chunk data part
  // This avoids the overhead of allocating 1MB on every chunk send
  static THREAD_LOCAL uint8_t data_buf[MAX_PACKET_LEN];
  static THREAD_LOCAL uint8_t cached_sections[20][4096];
  static THREAD_LOCAL uint8_t cached_biomes[20];
  int data_offset = 0;
  const int MAX_DATA_OFFSET = MAX_PACKET_LEN - 1024;  // Safety margin

  int x = _x * 16, z = _z * 16;

  // Try to get cached chunk data
  if (!get_cached_chunk_copy(_x, _z, cached_sections, cached_biomes)) {
    // Not cached yet: queue generation and skip sending this chunk for now.
    // This keeps movement packet handling non-blocking.
    chunk_debug_record_cache_miss_skip(client_fd);
    request_chunk_generation(_x, _z);
    return 1;
  }

  // 1. Send chunk sections
  // send 4 chunk sections (up to Y=0) with no blocks (bedrock)
  for (int i = 0; i < 4; i ++) {
    if (data_offset >= MAX_DATA_OFFSET) {
      fprintf(stderr, "ERROR: Chunk data buffer overflow at bedrock sections\n");
      return 1;
    }
    // block count
    data_buf[data_offset++] = 4096 >> 8;
    data_buf[data_offset++] = 4096 & 0xFF;
    // block bits
    data_buf[data_offset++] = 0;
    // block palette (bedrock = 85)
    uint32_t val = 85;
    while (true) {
      if (data_offset >= MAX_DATA_OFFSET) {
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

  // send middle chunk sections using cached data
  for (int i = 0; i < 20; i ++) {
    if (data_offset >= MAX_DATA_OFFSET) {
      fprintf(stderr, "ERROR: Chunk data buffer overflow at middle sections\n");
      return 1;
    }
    
    uint8_t* section_data = cached_sections[i];
    uint8_t biome = cached_biomes[i];

    // Check if section is uniform
    uint8_t first_block = section_data[0];
    uint8_t uniform = true;
    for (int j = 1; j < 4096; j ++) {
      if (section_data[j] != first_block) {
        uniform = false;
        break;
      }
    }

    if (uniform) {
      uint16_t block_count = (first_block == 0) ? 0 : 4096;
      data_buf[data_offset++] = block_count >> 8;
      data_buf[data_offset++] = block_count & 0xFF;
      data_buf[data_offset++] = 0;
      uint32_t val = block_palette[first_block];
      while (true) {
        if (data_offset >= MAX_DATA_OFFSET) {
          fprintf(stderr, "ERROR: Chunk data buffer overflow at uniform palette\n");
          return 1;
        }
        if ((val & ~SEGMENT_BITS) == 0) { data_buf[data_offset++] = val; break; }
        data_buf[data_offset++] = (val & SEGMENT_BITS) | CONTINUE_BIT;
        val >>= 7;
      }
    } else {
      uint16_t block_count = 0;
      for (int j = 0; j < 4096; j ++) if (section_data[j] != 0) block_count ++;
      data_buf[data_offset++] = block_count >> 8;
      data_buf[data_offset++] = block_count & 0xFF;
      data_buf[data_offset++] = 8;
      uint32_t val = 256;
      while (true) {
        if (data_offset >= MAX_DATA_OFFSET) {
          fprintf(stderr, "ERROR: Chunk data buffer overflow at non-uniform header\n");
          return 1;
        }
        if ((val & ~SEGMENT_BITS) == 0) { data_buf[data_offset++] = val; break; }
        data_buf[data_offset++] = (val & SEGMENT_BITS) | CONTINUE_BIT;
        val >>= 7;
      }
      // Check space for palette and section data
      if (data_offset + sizeof(network_block_palette) + 4096 >= MAX_DATA_OFFSET) {
        fprintf(stderr, "ERROR: Chunk data buffer overflow at section copy\n");
        return 1;
      }
      memcpy(data_buf + data_offset, network_block_palette, sizeof(network_block_palette));
      data_offset += sizeof(network_block_palette);
      memcpy(data_buf + data_offset, section_data, 4096);
      data_offset += 4096;
    }

    data_buf[data_offset++] = 0;
    data_buf[data_offset++] = biome;
  }

  // send 8 chunk sections (up to Y=192) with no blocks (air)
  for (int i = 0; i < 8; i ++) {
    if (data_offset >= MAX_DATA_OFFSET) {
      fprintf(stderr, "ERROR: Chunk data buffer overflow at air sections\n");
      return 1;
    }
    data_buf[data_offset++] = 0; // block count
    data_buf[data_offset++] = 0;
    data_buf[data_offset++] = 0; // bits
    data_buf[data_offset++] = 0; // palette (air)
    data_buf[data_offset++] = 0; // biome bits
    data_buf[data_offset++] = 0; // biome palette
  }

  int chunk_data_size = data_offset;

  // 2. Build the full packet
  startPacket(client_fd, 0x27);

  writeUint32(client_fd, _x);
  writeUint32(client_fd, _z);

  writeVarInt(client_fd, 0); // omit heightmaps

  writeVarInt(client_fd, chunk_data_size);
  send_all(client_fd, data_buf, chunk_data_size);

  writeVarInt(client_fd, 0); // omit block entities

  // light data
  writeVarInt(client_fd, 1);
  writeUint64(client_fd, 0b11111111111111111111111111);
  writeVarInt(client_fd, 0);
  writeVarInt(client_fd, 0);
  writeVarInt(client_fd, 0);

  // sky light array
  writeVarInt(client_fd, 26);
  // Dedicated light buffer (do NOT reuse worldgen's global chunk_section).
  // Reusing chunk_section races with the chunk generator thread and corrupts chunks.
  static THREAD_LOCAL uint8_t light_buf[4096];
  memset(light_buf, 0xFF, 2048);
  memset(light_buf + 2048, 0, 2048);

  // Cache VarInt for 2048
  for (int i = 0; i < 8; i ++) {
    writeVarInt(client_fd, 2048);
    send_all(client_fd, light_buf + 2048, 2048);
  }
  for (int i = 0; i < 18; i ++) {
    writeVarInt(client_fd, 2048);
    send_all(client_fd, light_buf, 2048);
  }
  // don't send block light
  writeVarInt(client_fd, 0);

  if (endPacket(client_fd) != 0) {
    // Queue full or client state changed; retry this chunk later.
    chunk_debug_record_enqueue_result(client_fd, _x, _z, 0);
    return 1;
  }
  chunk_debug_record_enqueue_result(client_fd, _x, _z, 1);

  // Sending block updates
  for (int i = 0; i < block_changes_count; i ++) {
    #ifdef ALLOW_CHESTS
      if (block_changes[i].block != B_torch && block_changes[i].block != B_chest && !isStairBlock(block_changes[i].block)) {
        #ifdef ALLOW_DOORS
        if (!isDoorBlock(block_changes[i].block)) continue;
        #else
        continue;
        #endif
      }
    #else
      if (block_changes[i].block != B_torch && !isStairBlock(block_changes[i].block)) {
        #ifdef ALLOW_DOORS
        if (!isDoorBlock(block_changes[i].block)) continue;
        #else
        continue;
        #endif
      }
    #endif
    if (block_changes[i].x < x || block_changes[i].x >= x + 16) continue;
    if (block_changes[i].z < z || block_changes[i].z >= z + 16) continue;
    
    #ifdef ALLOW_DOORS
    if (isDoorBlock(block_changes[i].block)) {
      // Bounds check: ensure i+1 and i+2 are valid indices
      if (i + 2 >= block_changes_count) continue;
      
      // Verify that i+1 is the upper half of this door
      if (block_changes[i + 1].x != block_changes[i].x || 
          block_changes[i + 1].y != block_changes[i].y + 1 || 
          block_changes[i + 1].z != block_changes[i].z) continue;
      
      // Verify that i+2 is the state entry
      if (block_changes[i + 2].block != 0 || block_changes[i + 2].z != block_changes[i].z) continue;
      
      // Send door with proper state (both halves)
      uint8_t *state_ptr = (uint8_t *)(&block_changes[i + 2]);
      uint8_t direction = state_ptr[0];
      uint8_t state_flags = state_ptr[1];
      uint8_t open = state_flags & 0x01;
      uint8_t hinge = (state_flags >> 1) & 0x01;
      // Lower half
      sendDoorUpdate(client_fd, block_changes[i].x, block_changes[i].y, block_changes[i].z, block_changes[i].block, 0, open, direction, hinge);
      // Upper half
      sendDoorUpdate(client_fd, block_changes[i].x, block_changes[i].y + 1, block_changes[i].z, block_changes[i + 1].block, 1, open, direction, hinge);
      // Skip the next two entries (upper half and state data)
      i += 2;
      continue;
    }
    #endif

    if (isStairBlock(block_changes[i].block)) {
      // Bounds check: ensure i+1 is a valid index
      if (i + 1 >= block_changes_count) continue;
      
      // Verify that i+1 is the state entry
      if (block_changes[i + 1].block != 0 || block_changes[i + 1].z != block_changes[i].z) continue;
      
      // Send stair with proper state
      uint8_t *state_ptr = (uint8_t *)(&block_changes[i + 1]);
      uint8_t direction = state_ptr[0];
      uint8_t half = state_ptr[1];
      sendStairUpdate(client_fd, block_changes[i].x, block_changes[i].y, block_changes[i].z, block_changes[i].block, half, direction);
      // Skip the next entry (state data)
      i += 1;
      continue;
    }
    
    sc_blockUpdate(client_fd, block_changes[i].x, block_changes[i].y, block_changes[i].z, block_changes[i].block);
  }

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
int sc_blockUpdate (int client_fd, int64_t x, int64_t y, int64_t z, uint8_t block) {
  startPacket(client_fd, 0x08);
  writeUint64(client_fd, ((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF));
  writeVarInt(client_fd, block_palette[block]);
  endPacket(client_fd);
  return 0;
}

#ifdef ALLOW_DOORS
// S->C Block Update with door state support
int sc_blockUpdateDoor (int client_fd, int64_t x, int64_t y, int64_t z, uint8_t block, uint8_t is_upper, uint8_t open, uint8_t direction, uint8_t hinge) {
  startPacket(client_fd, 0x08);
  writeUint64(client_fd, ((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF));
  
  // Calculate door block state ID
  // Minecraft door block states:
  // - half: "lower" (0) or "upper" (1)
  // - open: false (0) or true (1)
  // - facing: north (0), south (1), west (2), east (3)
  // - hinge: "left" (0) or "right" (1)
  // Base palette ID + state offset
  uint16_t base_id = block_palette[block];
  
  // Door state encoding (simplified - using state ID offsets)
  // Each door type has 16 states (2 halves * 2 open * 4 directions * 2 hinges)
  // For simplicity, we'll use: base_id + (is_upper * 8) + (open * 4) + (direction * 1) + (hinge * 0)
  // This is a simplified mapping - actual Minecraft uses a different formula
  uint16_t state_id = base_id + (is_upper << 3) + (open << 2) + (direction << 1) + hinge;
  
  writeVarInt(client_fd, state_id);
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

  // Ignore yaw/pitch
  recv_all(client_fd, recv_buffer, 8, false);

  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return 1;

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

  uint8_t apply_changes = true;
  // prevent dropping items
  if (mode == 4 && clicked_slot != -999) {
    // when using drop button, re-sync the respective slot
    uint8_t slot = clientSlotToServerSlot(window_id, clicked_slot);
    sc_setContainerSlot(client_fd, window_id, clicked_slot, player->inventory_count[slot], player->inventory_items[slot]);
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

  uint8_t slot, count, craft = false;
  uint16_t item;
  int tmp;

  uint16_t *p_item;
  uint8_t *p_count;

  #ifdef ALLOW_CHESTS
  // See the handlePlayerUseItem function for more info on this hack
  uint8_t *storage_ptr;
  memcpy(&storage_ptr, player->craft_items, sizeof(storage_ptr));
  #endif

  for (int i = 0; i < changes_count; i ++) {

    slot = clientSlotToServerSlot(window_id, readUint16(client_fd));
    // slots outside of the inventory overflow into the crafting buffer
    if (slot > 40 && apply_changes) craft = true;

    #ifdef ALLOW_CHESTS
    if (window_id == 2 && slot > 40) {
      // Get item pointers from the player's storage pointer
      // See the handlePlayerUseItem function for more info on this hack
      p_item = (uint16_t *)(storage_ptr + (slot - 41) * 3);
      p_count = storage_ptr + (slot - 41) * 3 + 2;
    } else
    #endif
    {
      // Prevent accessing crafting-related slots when craft_items is locked
      if (slot > 40 && player->flags & 0x80) return 1;
      p_item = &player->inventory_items[slot];
      p_count = &player->inventory_count[slot];
    }

    if (!readByte(client_fd)) { // no item?
      if (slot != 255 && apply_changes) {
        *p_item = 0;
        *p_count = 0;
        #ifdef ALLOW_CHESTS
        if (window_id == 2 && slot > 40) {
          broadcastChestUpdate(client_fd, storage_ptr, 0, 0, slot - 41);
        }
        #endif
      }
      continue;
    }

    item = readVarInt(client_fd);
    count = (uint8_t)readVarInt(client_fd);

    // ignore components
    readLengthPrefixedData(client_fd);
    readLengthPrefixedData(client_fd);

    if (count > 0 && apply_changes) {
      *p_item = item;
      *p_count = count;
      #ifdef ALLOW_CHESTS
      if (window_id == 2 && slot > 40) {
        broadcastChestUpdate(client_fd, storage_ptr, item, count, slot - 41);
      }
      #endif
    }

  }

  // window 0 is player inventory, window 12 is crafting table
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

  givePlayerItem(player, player->flagval_16, player->flagval_8);
  sc_setCursorItem(client_fd, 0, 0);
  player->flagval_16 = 0;
  player->flagval_8 = 0;

  return 0;
}

// S->C Player Info Update, "Add Player" action
int sc_playerInfoUpdateAddPlayer (int client_fd, PlayerData player) {

  startPacket(client_fd, 0x3F); // Packet ID

  writeByte(client_fd, 0x01); // EnumSet: Add Player
  writeByte(client_fd, 1); // Player count (1 per packet)

  // Player UUID
  send_all(client_fd, player.uuid, 16);
  // Player name
  writeByte(client_fd, strlen(player.name));
  send_all(client_fd, player.name, strlen(player.name));
  // Properties (don't send any)
  writeByte(client_fd, 0);

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
int sc_respawn (int client_fd) {

  startPacket(client_fd, 0x4B);

  // dimension id (from server-sent registries)
  writeVarInt(client_fd, 0);
  // dimension name
  const char *dimension = "overworld";
  writeVarInt(client_fd, 9);
  send_all(client_fd, dimension, 9);
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
  writeVarInt(client_fd, 0);
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
    sc_respawn(client_fd);
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

  if (!strncmp((char *)recv_buffer, "!help", 5)) {
    // Send command guide
    const char help_msg[] = "§7Commands:\n"
    "  !msg <player> <message> - Send a private message\n"
    "  !help - Show this help message";
    sc_systemChat(client_fd, (char *)help_msg, (uint16_t)sizeof(help_msg) - 1);
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

// S->C Registry Data (multiple packets) and Update Tags (configuration, multiple packets)
int sc_registries (int client_fd) {

  printf("Sending Registries\n\n");
  sendPreformattedPackets(client_fd, registries_bin, sizeof(registries_bin));

  printf("Sending Tags\n\n");
  sendPreformattedPackets(client_fd, tags_bin, sizeof(tags_bin));

  return 0;

}
