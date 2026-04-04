#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <limits.h>

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif

#ifdef ESP_PLATFORM
  #include "freertos/FreeRTOS.h"
  #include "freertos/task.h"
  #include "nvs_flash.h"
  #include "esp_wifi.h"
  #include "esp_event.h"
  #include "esp_timer.h"
  #include "lwip/sockets.h"
  #include "lwip/netdb.h"
#else
  #include <sys/types.h>
  #ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
  #else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
  #endif
  #include <unistd.h>
  #include <time.h>
#endif

#include "globals.h"
#include "tools.h"
#include "varnum.h"
#include "packets.h"
#include "worldgen.h"
#include "registries.h"
#include "procedures.h"
#include "serialize.h"
#include "chunk_generator.h"
#include "config.h"
#include <zlib.h>

#ifndef _WIN32
#include <signal.h>
#endif

static volatile sig_atomic_t server_running = 1;

#ifndef _WIN32
static void handle_shutdown_signal(int sig) {
  (void)sig;
  server_running = 0;
}
#endif

// Check if a chunk has been visited (sent to player)
static int isChunkVisited(PlayerData *player, int x, int z) {
  for (int i = 0; i < VISITED_HISTORY; i++) {
    if (player->visited_x[i] == x && player->visited_z[i] == z) {
      return 1;
    }
  }
  return 0;
}

// Mark a chunk as visited (sent to player) using circular buffer
static void markChunkVisited(PlayerData *player, int x, int z) {
  player->visited_x[player->visited_next] = x;
  player->visited_z[player->visited_next] = z;
  player->visited_next = (player->visited_next + 1) % VISITED_HISTORY;
}

// Check if chunk is within player's view distance from a reference position
static int isChunkInViewDistance(int chunk_x, int chunk_z, int player_chunk_x, int player_chunk_z, int view_distance) {
  int dx = chunk_x - player_chunk_x;
  int dz = chunk_z - player_chunk_z;
  return (dx >= -view_distance && dx <= view_distance && dz >= -view_distance && dz <= view_distance);
}

// Clear visited chunks that are now outside view distance
// This allows re-sending chunks when player returns to an area
static void clearDistantVisitedChunks(PlayerData *player, int center_x, int center_z, int view_distance) {
  for (int i = 0; i < VISITED_HISTORY; i++) {
    int vx = player->visited_x[i];
    int vz = player->visited_z[i];
    // Skip invalid entries
    if (vx == 32767 && vz == 32767) continue;
    // Clear if outside view distance
    if (!isChunkInViewDistance(vx, vz, center_x, center_z, view_distance)) {
      player->visited_x[i] = 32767;
      player->visited_z[i] = 32767;
    }
  }
}

// Dedicated chunk streaming worker.
// This keeps heavy chunk packet sending out of the movement packet path.
static pthread_t chunk_streamer_thread;
static volatile uint8_t chunk_streamer_running = 0;

typedef struct {
  int x;
  int z;
  int64_t retry_after_us;
} ChunkRetryEntry;

#define CHUNK_RETRY_TABLE_SIZE 2048
#define CHUNK_RETRY_COOLDOWN_US 120000
static ChunkRetryEntry chunk_retry_table[CHUNK_RETRY_TABLE_SIZE];
static uint8_t chunk_retry_table_initialized = 0;

static inline uint32_t chunkRetryHash(int x, int z) {
  uint32_t h = (uint32_t)x * 374761393u + (uint32_t)z * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}

static void initChunkRetryTable(void) {
  if (chunk_retry_table_initialized) return;
  for (int i = 0; i < CHUNK_RETRY_TABLE_SIZE; i++) {
    chunk_retry_table[i].x = INT_MIN;
    chunk_retry_table[i].z = INT_MIN;
    chunk_retry_table[i].retry_after_us = 0;
  }
  chunk_retry_table_initialized = 1;
}

static uint8_t isChunkInRetryCooldown(int x, int z, int64_t now_us) {
  ChunkRetryEntry *entry = &chunk_retry_table[chunkRetryHash(x, z) % CHUNK_RETRY_TABLE_SIZE];
  if (entry->x != x || entry->z != z) return false;
  return now_us < entry->retry_after_us;
}

static void setChunkRetryCooldown(int x, int z, int64_t now_us) {
  ChunkRetryEntry *entry = &chunk_retry_table[chunkRetryHash(x, z) % CHUNK_RETRY_TABLE_SIZE];
  entry->x = x;
  entry->z = z;
  entry->retry_after_us = now_us + CHUNK_RETRY_COOLDOWN_US;
}

static void streamChunksForPlayer(PlayerData *player) {
  if (player->client_fd == -1) return;
  if (player->flags & 0x20) return;  // still loading

  int center_x = div_floor(player->x, 16);
  int center_z = div_floor(player->z, 16);

  clearDistantVisitedChunks(player, center_x, center_z, VIEW_DISTANCE);

  size_t chunk_backlog = get_client_send_queue_bytes(player->client_fd);
  if (chunk_backlog > (1024 * 1024)) {
    // Let queued chunk packets drain first.
    return;
  }

  int chunks_per_cycle = chunk_backlog > (512 * 1024) ? 1 : 2;
  int chunk_attempt_budget = chunks_per_cycle * 3;
  int64_t now_us = get_program_time();

  for (int radius = 0; radius <= VIEW_DISTANCE &&
       chunks_per_cycle > 0 &&
       chunk_attempt_budget > 0; radius++) {
    for (int dz = -radius; dz <= radius &&
         chunks_per_cycle > 0 &&
         chunk_attempt_budget > 0; dz++) {
      for (int dx = -radius; dx <= radius &&
           chunks_per_cycle > 0 &&
           chunk_attempt_budget > 0; dx++) {
        if (radius != 0 &&
            dx != -radius && dx != radius &&
            dz != -radius && dz != radius) {
          continue;
        }

        int check_x = center_x + dx;
        int check_z = center_z + dz;
        if (isChunkVisited(player, check_x, check_z)) continue;
        if (isChunkInRetryCooldown(check_x, check_z, now_us)) {
          chunk_attempt_budget--;
          continue;
        }

        if (sc_chunkDataAndUpdateLight(player->client_fd, check_x, check_z) == 0) {
          markChunkVisited(player, check_x, check_z);
          chunks_per_cycle--;
        } else {
          // Avoid hammering not-yet-generated chunks every streamer cycle.
          setChunkRetryCooldown(check_x, check_z, now_us);
        }

        chunk_attempt_budget--;
      }
    }
  }
}

static void* chunk_streamer_worker(void* arg) {
  (void)arg;
  while (chunk_streamer_running) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
      streamChunksForPlayer(&player_data[i]);
    }
    usleep(20000);  // 20ms
  }
  return NULL;
}

static void init_chunk_streamer(void) {
  initChunkRetryTable();
  chunk_streamer_running = 1;
  pthread_create(&chunk_streamer_thread, NULL, chunk_streamer_worker, NULL);
  printf("Chunk streamer thread started\n");
}

static void shutdown_chunk_streamer(void) {
  if (!chunk_streamer_running) return;
  chunk_streamer_running = 0;
  pthread_join(chunk_streamer_thread, NULL);
  printf("Chunk streamer thread stopped\n");
}

#define MAX_PACKETS_PER_CLIENT_TURN 8
#define MAX_MOVEMENT_PACKETS_PER_CLIENT_TURN 2

static inline uint8_t isMovementPacketInPlayState(int state, int packet_id) {
  if (state != STATE_PLAY) return false;
  return (
    packet_id == 0x1D ||  // Set Player Position
    packet_id == 0x1E ||  // Set Player Position and Rotation
    packet_id == 0x1F ||  // Set Player Rotation
    packet_id == 0x20     // Set Player Movement Flags
  );
}

#ifdef DEV_LOG_ALL_PACKETS
static void logIncomingPacket(int client_fd, int state, int packet_id, int payload_len, int frame_len, int compressed) {
  printf(
    "[pkt-in] fd=%d state=%d id=0x%X payload=%d frame=%d compressed=%d\n",
    client_fd,
    state,
    packet_id,
    payload_len,
    frame_len,
    compressed
  );
}
#endif

/**
 * Routes an incoming packet to its packet handler or procedure.
 *
 * Full disclosure, I think this whole thing is a bit of a mess.
 * The packet handlers started out as having proper error checks and
 * handling, but that turned out to be very tedious and space/time
 * consuming, and didn't really help with resolving errors. Not to mention
 * that all those checks likely compound into a non-negligible performance
 * hit on embedded systems.
 *
 * I think the way forward would be to gut the return values of the packet
 * handlers, as most of them only ever return 0, and others aren't checked
 * here. The length discrepancy checks at the bottom already do a good job
 * at preventing this from derailing completely in case of a bad packet,
 * and I think leaning into those is fine.
 *
 * In other words, I think the sc_/cs_ handlers should be of type `void`,
 * and should simply return early when there's a failure that prevents the
 * server from handling a packet. Any data that's left unhandled/unread
 * will be caught by the length discrepancy checks. That's more or less
 * how it already works, just not explicitly.
 *
 * Why have I not done this yet? Well, I'm close to uploading the video,
 * and I don't want to risk refactoring anything this close to release.
 */
void handlePacket (int client_fd, int length, int packet_id, int state) {

  // Count the amount of bytes received to catch length discrepancies
  uint64_t bytes_received_start = total_bytes_received;

  switch (packet_id) {

    case 0x00:
      if (state == STATE_NONE) {
        if (cs_handshake(client_fd)) break;
      } else if (state == STATE_STATUS) {
        if (sc_statusResponse(client_fd)) break;
      } if (state == STATE_LOGIN) {
        uint8_t uuid[16];
        char name[16];
        if (cs_loginStart(client_fd, uuid, name)) break;
        if (reservePlayerData(client_fd, uuid, name)) {
          recv_count = 0;
          return;
        }
        sc_setCompression(client_fd, 256);
        if (sc_loginSuccess(client_fd, uuid, name)) break;
      } else if (state == STATE_CONFIGURATION) {
        if (cs_clientInformation(client_fd)) break;
        if (sc_knownPacks(client_fd)) break;
        if (sc_registries(client_fd)) break;

        #ifdef SEND_BRAND
        if (sc_sendPluginMessage(client_fd, "minecraft:brand", (uint8_t *)brand, brand_len)) break;
        #endif
      }
      break;

    case 0x01:
      // Handle status ping
      if (state == STATE_STATUS) {
        // No need for a packet handler, just echo back the long verbatim
        writeByte(client_fd, 9);
        writeByte(client_fd, 0x01);
        writeUint64(client_fd, readUint64(client_fd));
        // Close connection after this
        recv_count = 0;
        return;
      }
      break;

    case 0x02:
      if (state == STATE_CONFIGURATION) cs_pluginMessage(client_fd);
      break;

    case 0x03:
      if (state == STATE_LOGIN) {
        printf("Client Acknowledged Login\n\n");
        setClientState(client_fd, STATE_CONFIGURATION);
      } else if (state == STATE_CONFIGURATION) {
        printf("Client Acknowledged Configuration\n\n");

        // Enter client into "play" state
        setClientState(client_fd, STATE_PLAY);
        sc_loginPlay(client_fd);

        PlayerData *player;
        if (getPlayerData(client_fd, &player)) break;

        // Send full client spawn sequence
        spawnPlayer(player);

        // Register all existing players and spawn their entities
        for (int i = 0; i < MAX_PLAYERS; i ++) {
          if (player_data[i].client_fd == -1) continue;
          // Note that this will also filter out the joining player
          if (player_data[i].flags & 0x20) continue;
          sc_playerInfoUpdateAddPlayer(client_fd, player_data[i]);
          sc_spawnEntityPlayer(client_fd, player_data[i]);
        }

        // Send information about all other entities (mobs):
        // Use a random number for the first half of the UUID
        uint8_t uuid[16];
        uint32_t r = fast_rand();
        memcpy(uuid, &r, 4);
        // Send allocated living mobs, use ID for second half of UUID
        for (int i = 0; i < MAX_MOBS; i ++) {
          if (mob_data[i].type == 0) continue;
          if ((mob_data[i].data & 31) == 0) continue;
          memcpy(uuid + 4, &i, 4);
          // For more info on the arguments here, see the spawnMob function
          sc_spawnEntity(
            client_fd, -2 - i, uuid,
            mob_data[i].type, mob_data[i].x, mob_data[i].y, mob_data[i].z,
            0, 0
          );
          broadcastMobMetadata(client_fd, -2 - i);
        }

      }
      break;

    case 0x07:
      if (state == STATE_CONFIGURATION) {
        printf("Received Client's Known Packs\n");
        printf("  Finishing configuration\n\n");
        sc_finishConfiguration(client_fd);
      }
      break;

    case 0x08:
      if (state == STATE_PLAY) cs_chat(client_fd);
      break;

    case 0x0B:
      if (state == STATE_PLAY) cs_clientStatus(client_fd);
      break;

    case 0x0C: // Client tick (ignored)
      break;

    case 0x11:
      if (state == STATE_PLAY) cs_clickContainer(client_fd);
      break;

    case 0x12:
      if (state == STATE_PLAY) cs_closeContainer(client_fd);
      break;

    case 0x1B:
      if (state == STATE_PLAY) {
        // Serverbound keep-alive (ignored)
        discard_all(client_fd, length, false);
      }
      break;

    case 0x19:
      if (state == STATE_PLAY) cs_interact(client_fd);
      break;

    case 0x1D:
    case 0x1E:
    case 0x1F:
    case 0x20:
      if (state == STATE_PLAY) {

        double x, y, z;
        float yaw, pitch;
        uint8_t on_ground;

        // Read player position (and rotation)
        if (packet_id == 0x1D) cs_setPlayerPosition(client_fd, &x, &y, &z, &on_ground);
        else if (packet_id == 0x1F) cs_setPlayerRotation (client_fd, &yaw, &pitch, &on_ground);
        else if (packet_id == 0x20) cs_setPlayerMovementFlags (client_fd, &on_ground);
        else cs_setPlayerPositionAndRotation(client_fd, &x, &y, &z, &yaw, &pitch, &on_ground);

        PlayerData *player;
        if (getPlayerData(client_fd, &player)) break;

        uint8_t block_feet = getBlockAt(player->x, player->y, player->z);
        uint8_t swimming = block_feet >= B_water && block_feet < B_water + 8;

        // Handle fall damage
        if (on_ground) {
          int16_t damage = player->grounded_y - player->y - 3;
          if (damage > 0 && (GAMEMODE == 0 || GAMEMODE == 2) && !swimming) {
            hurtEntity(client_fd, -1, D_fall, damage);
          }
          player->grounded_y = player->y;
        } else if (swimming) {
          player->grounded_y = player->y;
        }

        // Don't continue if all we got were flags
        if (packet_id == 0x20) break;

        // Update rotation in player data (if applicable)
        if (packet_id != 0x1D) {
          player->yaw = ((short)(yaw + 540) % 360 - 180) * 127 / 180;
          player->pitch = pitch / 90.0f * 127.0f;
        }

        // Whether to broadcast player position to other players
        uint8_t should_broadcast = true;

        #ifndef BROADCAST_ALL_MOVEMENT
          // If applicable, tie movement updates to the tickrate by using
          // a flag that gets reset on every tick. It might sound better
          // to just make the tick handler broadcast position updates, but
          // then we lose precision. While position is stored using integers,
          // here the client gives us doubles and floats directly.
          should_broadcast = !(player->flags & 0x40);
          if (should_broadcast) player->flags |= 0x40;
        #endif

        #ifdef SCALE_MOVEMENT_UPDATES_TO_PLAYER_COUNT
          // If applicable, broadcast only every client_count-th movement update
          if (++player->packets_since_update < client_count) {
            should_broadcast = false;
          } else {
            // Note that this does not explicitly set should_broadcast to true
            // This allows the above BROADCAST_ALL_MOVEMENT check to compound
            // Whether that's ever favorable is up for debate
            player->packets_since_update = 0;
          }
        #endif

        if (should_broadcast) {
          // If the packet had no rotation data, calculate it from player data
          if (packet_id == 0x1D) {
            yaw = player->yaw * 180 / 127;
            pitch = player->pitch * 90 / 127;
          }
          // Send current position data to all connected players
          for (int i = 0; i < MAX_PLAYERS; i ++) {
            if (player_data[i].client_fd == -1) continue;
            if (player_data[i].flags & 0x20) continue;
            if (player_data[i].client_fd == client_fd) continue;
            // Find the client state for this player
            uint8_t target_in_play = 0;
            for (int k = 0; k < MAX_PLAYERS; k ++) {
              if (client_states[k].client_fd == player_data[i].client_fd) {
                target_in_play = (client_states[k].state == STATE_PLAY);
                break;
              }
            }
            if (!target_in_play) continue;
            if (packet_id == 0x1F) {
              sc_updateEntityRotation(player_data[i].client_fd, client_fd, player->yaw, player->pitch);
            } else {
              sc_teleportEntity(player_data[i].client_fd, client_fd, x, y, z, yaw, pitch);
            }
            sc_setHeadRotation(player_data[i].client_fd, client_fd, player->yaw);
          }
        }

        // Don't continue if all we got was rotation data
        if (packet_id == 0x1F) break;

        // Players send movement packets roughly 20 times per second when
        // moving, and much less frequently when standing still. We can
        // use this correlation between actions and packet count to cheaply
        // simulate hunger with a timer-based system, where the timer ticks
        // down with each position packet. The timer value itself then
        // naturally works as a substitute for saturation.
        if (player->saturation == 0) {
          if (player->hunger > 0) player->hunger--;
          player->saturation = 200;
          sc_setHealth(client_fd, player->health, player->hunger, player->saturation);
        } else if (player->flags & 0x08) {
          player->saturation -= 1;
        }

        // Cast the values to short to get integer position
        short cx = x, cy = y, cz = z;
        if (x < 0) cx -= 1;
        if (z < 0) cz -= 1;
        // Determine the player's chunk coordinates
        short _x = div_floor(cx, 16), _z = div_floor(cz, 16);
        // Calculate distance between previous and current chunk coordinates
        short dx = _x - div_floor(player->x, 16);
        short dz = _z - div_floor(player->z, 16);

        // Prevent players from leaving the world
        if (cy < 0) {
          cy = 0;
          player->grounded_y = 0;
          sc_synchronizePlayerPosition(client_fd, cx, 0, cz, player->yaw * 180 / 127, player->pitch * 90 / 127);
        } else if (cy > 255) {
          cy = 255;
          sc_synchronizePlayerPosition(client_fd, cx, 255, cz, player->yaw * 180 / 127, player->pitch * 90 / 127);
        }

        // Update position in player data
        player->x = cx;
        player->y = cy;
        player->z = cz;

        // Only update center chunk when crossing chunk borders.
        // Actual chunk streaming is handled asynchronously by chunk_streamer_worker.
        if (dx != 0 || dz != 0) {
          sc_setCenterChunk(client_fd, _x, _z);
        }

        // Exit early if no chunk borders were crossed (skip mob spawning etc.)
        if (dx == 0 && dz == 0) break;

      }
      break;

    case 0x29:
      if (state == STATE_PLAY) cs_playerCommand(client_fd);
      break;

    case 0x2A:
      if (state == STATE_PLAY) cs_playerInput(client_fd);
      break;

    case 0x2B:
      if (state == STATE_PLAY) cs_playerLoaded(client_fd);
      break;

    case 0x34:
      if (state == STATE_PLAY) cs_setHeldItem(client_fd);
      break;
	
    case 0x3C:
      if (state == STATE_PLAY) cs_swingArm(client_fd);
      break;

    case 0x28:
      if (state == STATE_PLAY) cs_playerAction(client_fd);
      break;

    case 0x3F:
      if (state == STATE_PLAY) cs_useItemOn(client_fd);
      break;

    case 0x40:
      if (state == STATE_PLAY) cs_useItem(client_fd);
      break;

    default:
      #ifdef DEV_LOG_UNKNOWN_PACKETS
        printf("Unknown packet: 0x");
        if (packet_id < 16) printf("0");
        printf("%X, length: %d, state: %d\n\n", packet_id, length, state);
      #endif
      discard_all(client_fd, length, false);
      break;

  }

  // Detect and fix incorrectly parsed packets
  int processed_length = total_bytes_received - bytes_received_start;
  if (processed_length == length) return;

  if (length > processed_length) {
    discard_all(client_fd, length - processed_length, false);
  }

  #ifdef DEV_LOG_LENGTH_DISCREPANCY
  if (processed_length != 0) {
    printf("WARNING: Packet 0x");
    if (packet_id < 16) printf("0");
    printf("%X parsed incorrectly!\n  Expected: %d, parsed: %d\n\n", packet_id, length, processed_length);
  }
  #endif
  #ifdef DEV_LOG_UNKNOWN_PACKETS
  if (processed_length == 0) {
    printf("Unknown packet: 0x");
    if (packet_id < 16) printf("0");
    printf("%X, length: %d, state: %d\n\n", packet_id, length, state);
  }
  #endif

}

int main () {
  #ifdef _WIN32 //initialize windows socket
    WSADATA wsa;
      if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        exit(EXIT_FAILURE);
      }
  #endif

  // Initialize config with defaults
  init_config_defaults();
  
  // Load config from file (if exists)
  load_config("server.conf");
  
  // Update global variables from config
  strncpy(motd, config.motd, MOTD_MAX_LEN - 1);
  motd[MOTD_MAX_LEN - 1] = '\0';
  motd_len = strlen(motd);

  // Load server icon (favicon)
  load_favicon("serverIcon.png");

  #ifdef SEND_BRAND
    strncpy(brand, config.brand, BRAND_MAX_LEN - 1);
    brand[BRAND_MAX_LEN - 1] = '\0';
    brand_len = strlen(brand);
  #endif
  
  // Print config summary
  printf("Server Configuration:\n");
  printf("  Port: %d\n", config.port);
  printf("  Max Players: %d\n", config.max_players);
  printf("  View Distance: %d\n", config.view_distance);
  printf("  Gamemode: %d\n", config.gamemode);
  printf("  Chunk Cache Size: %d\n", config.chunk_cache_size);
  printf("  MOTD: %s\n", config.motd);
  printf("\n");

  // Hash the seeds to ensure they're random enough
  world_seed = splitmix64(config.world_seed);
  printf("World seed (hashed): ");
  for (int i = 3; i >= 0; i --) printf("%X", (unsigned int)((world_seed >> (8 * i)) & 255));

  rng_seed = splitmix64(config.rng_seed);
  printf("\nRNG seed (hashed): ");
  for (int i = 3; i >= 0; i --) printf("%X", (unsigned int)((rng_seed >> (8 * i)) & 255));
  printf("\n\n");

  // Initialize world generation
  printf("Initializing world generation...\n");
  printf("\n");

  // Initialize global thread pool for parallel operations
  init_global_thread_pool();

  // Initialize block changes
  #ifdef INFINITE_BLOCK_CHANGES
    // Start with a minimal capacity, will grow as needed
    block_changes_capacity = 10;
    block_changes = (BlockChange *)malloc(block_changes_capacity * sizeof(BlockChange));
    if (!block_changes) {
      perror("Failed to allocate memory for block changes");
      exit(EXIT_FAILURE);
    }
    printf("Block changes: dynamic (initial capacity: %d)\n", block_changes_capacity);
  #else
    printf("Block changes: fixed (%d)\n", MAX_BLOCK_CHANGES);
  #endif
  // Initialize all block change entries as unallocated (0xFF = empty)
  for (int i = 0; i < (
    #ifdef INFINITE_BLOCK_CHANGES
      block_changes_capacity
    #else
      MAX_BLOCK_CHANGES
    #endif
  ); i ++) {
    block_changes[i].block = 0xFF;
  }

  // Start the disk/flash serializer (if applicable)
  if (initSerializer()) exit(EXIT_FAILURE);
  rebuildBlockChangeIndexes();
  printf("Loaded block changes: %d\n", block_changes_count);

  // Initialize all file descriptor references to -1 (unallocated)
  int clients[MAX_PLAYERS], client_index = 0;
  for (int i = 0; i < MAX_PLAYERS; i ++) {
    clients[i] = -1;
    client_states[i].client_fd = -1;
    client_states[i].state = STATE_NONE;
    client_states[i].compression_threshold = 0;
    pthread_mutex_init(&client_states[i].send_mutex, NULL);
    pthread_cond_init(&client_states[i].send_cond, NULL);
    client_states[i].send_head_hi = NULL;
    client_states[i].send_tail_hi = NULL;
    client_states[i].send_head_lo = NULL;
    client_states[i].send_tail_lo = NULL;
    client_states[i].queued_bytes = 0;
    client_states[i].queued_chunk_bytes = 0;
    client_states[i].connection_generation = 1;
    player_data[i].client_fd = -1;
  }

  // Start packet sender workers (asynchronous outbound network writes)
  set_queue_limits(config.send_queue_limit, config.chunk_queue_limit);
  init_packet_sender_workers();

  // Create server TCP socket
  int server_fd, opt = 1;
  struct sockaddr_in server_addr, client_addr;
  socklen_t addr_len = sizeof(client_addr);

  server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd == -1) {
    perror("socket failed");
    exit(EXIT_FAILURE);
  }
#ifdef _WIN32
  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
      (const char*)&opt, sizeof(opt)) < 0) {
#else
  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
#endif    
    perror("socket options failed");
    exit(EXIT_FAILURE);
  }

  // Bind socket to IP/port
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(config.port);

  if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    perror("bind failed");
    close(server_fd);
    exit(EXIT_FAILURE);
  }

  // Listen for incoming connections
  if (listen(server_fd, 5) < 0) {
    perror("listen failed");
    close(server_fd);
    exit(EXIT_FAILURE);
  }
  printf("Server listening on port %d...\n", PORT);

  // Start chunk generator thread
  init_chunk_generator();
  // Start chunk streaming thread (asynchronous chunk sending)
  init_chunk_streamer();

  // Make the socket non-blocking
  // This is necessary to not starve the idle task during slow connections
  #ifdef _WIN32
    u_long mode = 1;  // 1 = non-blocking
    if (ioctlsocket(server_fd, FIONBIO, &mode) != 0) {
      fprintf(stderr, "Failed to set non-blocking mode\n");
      exit(EXIT_FAILURE);
    }
  #else
  int flags = fcntl(server_fd, F_GETFL, 0);
  fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);

  /* Install signal handlers for graceful shutdown */
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = handle_shutdown_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  /* Ignore SIGPIPE to avoid crashes when clients disconnect during writes */
  sa.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &sa, NULL);
  #endif

  // Track time of last server tick (in microseconds)
  int64_t last_tick_time = get_program_time();

  /**
   * Cycles through all connected clients and handles small packet bursts
   * per client. Burst handling drains packet backlogs without letting one
   * connection monopolize the loop.
   */
  while (server_running) {
    // Check if it's time to yield to the idle task
    task_yield();

    // Attempt to accept a new connection
    for (int i = 0; i < MAX_PLAYERS; i ++) {
      if (clients[i] != -1) continue;
      clients[i] = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
      // If the accept was successful, make the client non-blocking too
      if (clients[i] != -1) {
        printf("New client, fd: %d\n", clients[i]);
      #ifdef _WIN32
        u_long mode = 1;
        ioctlsocket(clients[i], FIONBIO, &mode);
      #else
        int flags = fcntl(clients[i], F_GETFL, 0);
        fcntl(clients[i], F_SETFL, flags | O_NONBLOCK);
      #endif
        client_count ++;
      }
      break;
    }

    // Look for the next valid connected client.
    // Skip empty slots in one pass instead of burning full loop turns on them.
    int searched = 0;
    do {
      client_index++;
      if (client_index == MAX_PLAYERS) client_index = 0;
      searched++;
    } while (searched < MAX_PLAYERS && clients[client_index] == -1);
    if (searched == MAX_PLAYERS && clients[client_index] == -1) continue;

    // Handle periodic events (server ticks)
    int64_t time_since_last_tick = get_program_time() - last_tick_time;
    if (time_since_last_tick > TIME_BETWEEN_TICKS) {
      handleServerTick(time_since_last_tick);
      // Periodically drain old packets to free memory
      drain_client_queues();
      last_tick_time = get_program_time();
    }

    // Handle this individual client in a bounded burst.
    int movement_packets_processed = 0;
    for (int packets_processed = 0; packets_processed < MAX_PACKETS_PER_CLIENT_TURN; packets_processed++) {
      int client_fd = clients[client_index];
      if (client_fd == -1) break;

      // Check if at least 2 bytes are available for reading.
      #ifdef _WIN32
      recv_count = recv(client_fd, recv_buffer, 2, MSG_PEEK);
      if (recv_count == 0) {
        disconnectClient(&clients[client_index], 1);
        break;
      }
      if (recv_count == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
          break; // no more data right now
        }
        disconnectClient(&clients[client_index], 1);
        break;
      }
      #else
      recv_count = recv(client_fd, &recv_buffer, 2, MSG_PEEK);
      if (recv_count < 2) {
        if (recv_count == 0 || (recv_count < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
          disconnectClient(&clients[client_index], 1);
        }
        break;
      }
      #endif

      // Handle 0xBEEF and 0xFEED packets for dumping/uploading world data.
      #ifdef DEV_ENABLE_BEEF_DUMPS
      if (recv_buffer[0] == 0xBE && recv_buffer[1] == 0xEF && getClientState(client_fd) == STATE_NONE) {
        // Send block changes and player data back to back.
        #ifdef INFINITE_BLOCK_CHANGES
          send_all(client_fd, &block_changes_capacity, sizeof(int));
          send_all(client_fd, block_changes, block_changes_capacity * sizeof(BlockChange));
        #else
          send_all(client_fd, block_changes, sizeof(block_changes));
        #endif
        send_all(client_fd, player_data, sizeof(player_data));
        shutdown(client_fd, SHUT_WR);
        recv_all(client_fd, recv_buffer, sizeof(recv_buffer), false);
        disconnectClient(&clients[client_index], 6);
        break;
      }

      if (recv_buffer[0] == 0xFE && recv_buffer[1] == 0xED && getClientState(client_fd) == STATE_NONE) {
        // Consume 0xFEED bytes (previous read was just a peek).
        recv_all(client_fd, recv_buffer, 2, false);
        #ifdef INFINITE_BLOCK_CHANGES
          int new_capacity;
          recv_all(client_fd, &new_capacity, sizeof(int), false);
          if (new_capacity > block_changes_capacity) {
            BlockChange *new_block_changes = (BlockChange *)realloc(block_changes, new_capacity * sizeof(BlockChange));
            if (!new_block_changes) {
              perror("Failed to reallocate block changes for FEED packet");
              disconnectClient(&clients[client_index], 7);
              break;
            }
            block_changes = new_block_changes;
            block_changes_capacity = new_capacity;
          }
          recv_all(client_fd, block_changes, block_changes_capacity * sizeof(BlockChange), false);
        #else
          recv_all(client_fd, block_changes, sizeof(block_changes), false);
        #endif
        recv_all(client_fd, player_data, sizeof(player_data), false);
        for (int i = 0; i < (
          #ifdef INFINITE_BLOCK_CHANGES
            block_changes_capacity
          #else
            MAX_BLOCK_CHANGES
          #endif
        ); i ++) {
          if (block_changes[i].block == 0xFF) continue;
          if (block_changes[i].block == B_chest) i += 14;
          #ifdef ALLOW_DOORS
          if (isDoorBlock(block_changes[i].block)) { i += 2; continue; }
          #endif
          if (isStairBlock(block_changes[i].block) || block_changes[i].block == B_furnace) i += 1;
          if (i >= block_changes_count) block_changes_count = i + 1;
        }
        rebuildBlockChangeIndexes();
        writeBlockChangesToDisk(0, block_changes_count);
        writePlayerDataToDisk();
        disconnectClient(&clients[client_index], 7);
        break;
      }
      #endif

      int packet_length = readVarInt(client_fd);
      if (packet_length == VARNUM_ERROR) {
        disconnectClient(&clients[client_index], 2);
        break;
      }

      int packet_id;
      int threshold = getCompressionThreshold(client_fd);

      if (threshold > 0) {
        int data_length = readVarInt(client_fd);
        if (data_length == 0) {
          // Uncompressed packet while compression is enabled.
          packet_id = readVarInt(client_fd);
          int header_size = sizeVarInt(0) + sizeVarInt(packet_id);
          int payload_len = packet_length - header_size;
          int state = getClientState(client_fd);
          uint8_t movement_packet = isMovementPacketInPlayState(state, packet_id);
          uint8_t should_skip_movement = movement_packet && movement_packets_processed >= MAX_MOVEMENT_PACKETS_PER_CLIENT_TURN;

          #ifdef DEV_LOG_ALL_PACKETS
          logIncomingPacket(client_fd, state, packet_id, payload_len, packet_length, 0);
          #endif

          if (should_skip_movement) {
            discard_all(client_fd, payload_len, false);
          } else {
            handlePacket(client_fd, payload_len, packet_id, state);
            if (movement_packet) movement_packets_processed++;
          }
        } else {
          // Compressed packet body.
          int compressed_len = packet_length - sizeVarInt(data_length);
          if (compressed_len > MAX_PACKET_LEN || data_length > MAX_PACKET_LEN) {
            printf("ERROR: Compressed packet too large (%d or %d > %d)\n", compressed_len, data_length, MAX_PACKET_LEN);
            disconnectClient(&clients[client_index], 8);
            break;
          }

          uint8_t *compressed_buf = malloc(compressed_len);
          if (!compressed_buf) {
            perror("Failed to allocate compressed buffer");
            disconnectClient(&clients[client_index], 8);
            break;
          }

          recv_all(client_fd, compressed_buf, compressed_len, false);

          z_stream strm;
          strm.zalloc = Z_NULL;
          strm.zfree = Z_NULL;
          strm.opaque = Z_NULL;
          strm.avail_in = compressed_len;
          strm.next_in = compressed_buf;
          strm.avail_out = MAX_PACKET_LEN;
          strm.next_out = in_packet_buffer;

          int ret = inflateInit(&strm);
          if (ret != Z_OK) {
            fprintf(stderr, "inflateInit failed: %d\n", ret);
            free(compressed_buf);
            disconnectClient(&clients[client_index], 8);
            break;
          }

          ret = inflate(&strm, Z_FINISH);
          if (ret != Z_STREAM_END) {
            fprintf(stderr, "inflate failed: %d\n", ret);
            inflateEnd(&strm);
            free(compressed_buf);
            disconnectClient(&clients[client_index], 8);
            break;
          }

          in_packet_buffer_len = strm.total_out;
          inflateEnd(&strm);
          free(compressed_buf);

          in_packet_buffer_offset = 0;
          packet_id = readVarInt(client_fd);
          int payload_len = in_packet_buffer_len - sizeVarInt(packet_id);
          int state = getClientState(client_fd);
          uint8_t movement_packet = isMovementPacketInPlayState(state, packet_id);
          uint8_t should_skip_movement = movement_packet && movement_packets_processed >= MAX_MOVEMENT_PACKETS_PER_CLIENT_TURN;

          #ifdef DEV_LOG_ALL_PACKETS
          logIncomingPacket(client_fd, state, packet_id, payload_len, packet_length, 1);
          #endif

          if (should_skip_movement) {
            in_packet_buffer_len = 0;
            in_packet_buffer_offset = 0;
          } else {
            handlePacket(client_fd, payload_len, packet_id, state);
            if (movement_packet) movement_packets_processed++;
            in_packet_buffer_len = 0;
            in_packet_buffer_offset = 0;
          }
        }
      } else {
        packet_id = readVarInt(client_fd);
        if (packet_id == VARNUM_ERROR) {
          disconnectClient(&clients[client_index], 3);
          break;
        }
        int payload_len = packet_length - sizeVarInt(packet_id);
        int state = getClientState(client_fd);
        uint8_t movement_packet = isMovementPacketInPlayState(state, packet_id);
        uint8_t should_skip_movement = movement_packet && movement_packets_processed >= MAX_MOVEMENT_PACKETS_PER_CLIENT_TURN;

        #ifdef DEV_LOG_ALL_PACKETS
        logIncomingPacket(client_fd, state, packet_id, payload_len, packet_length, 0);
        #endif

        if (should_skip_movement) {
          discard_all(client_fd, payload_len, false);
        } else {
          handlePacket(client_fd, payload_len, packet_id, state);
          if (movement_packet) movement_packets_processed++;
        }
      }

      if (recv_count == 0 || (recv_count == -1 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        disconnectClient(&clients[client_index], 4);
        break;
      }
    }

  }

  // Save all data before exiting
  #ifdef SYNC_WORLD_TO_DISK
  printf("Saving world data...\n");
  writePlayerDataToDisk();
  #endif

  // Shutdown chunk streaming thread
  shutdown_chunk_streamer();
  // Shutdown chunk generator thread
  shutdown_chunk_generator();

  // Shutdown global thread pool
  shutdown_global_thread_pool();
  // Shutdown packet sender workers
  shutdown_packet_sender_workers();

  close(server_fd);
 
  #ifdef _WIN32 //cleanup windows socket
    WSACleanup();
  #endif

  printf("Server closed.\n");

}

#ifdef ESP_PLATFORM

void bareiron_main (void *pvParameters) {
  main();
  vTaskDelete(NULL);
}

static void wifi_event_handler (void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    esp_wifi_connect();
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    printf("Got IP, starting server...\n\n");
    xTaskCreate(bareiron_main, "bareiron", 4096, NULL, 5, NULL);
  }
}

void wifi_init () {
  nvs_flash_init();
  esp_netif_init();
  esp_event_loop_create_default();
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);

  esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
  esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

  wifi_config_t wifi_config = {
    .sta = {
      .ssid = WIFI_SSID,
      .password = WIFI_PASS,
      .threshold.authmode = WIFI_AUTH_WPA2_PSK
    }
  };

  esp_wifi_set_mode(WIFI_MODE_STA);
  esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_start();
}

void app_main () {
  esp_timer_early_init();
  wifi_init();
}

#endif
