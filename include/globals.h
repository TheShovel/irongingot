#ifndef H_GLOBALS
#define H_GLOBALS

#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <zlib.h>
#include "thread_pool.h"

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
  #define THREAD_LOCAL _Thread_local
#elif defined(__GNUC__)
  #define THREAD_LOCAL __thread
#else
  #define THREAD_LOCAL
#endif

#ifdef ESP_PLATFORM
  #define WIFI_SSID "your-ssid"
  #define WIFI_PASS "your-password"
  void task_yield ();
#else
  #define task_yield()
#endif

#define true 1
#define false 0

// TCP port, Minecraft's default is 25565
// Note: Actual value is loaded from config, this is a fallback default
#define PORT 25565

// How many players to keep in memory, NOT the amount of concurrent players
// Even when offline, players who have logged on before take up a slot
#define MAX_PLAYERS 16

// How many mobs to allocate memory for
#define MAX_MOBS (MAX_PLAYERS * 4)

// Manhattan distance at which mobs despawn
#define MOB_DESPAWN_DISTANCE 256

// Server game mode: 0 - survival; 1 - creative; 2 - adventure; 3 - spectator
#define GAMEMODE 0

// Max render distance, determines how many chunks to send
// Overridden by config.view_distance at runtime; this default is used only
// before config is loaded (e.g. during early init).
#define VIEW_DISTANCE_DEFAULT 5

// Dimension identifiers (avoid conflict with cubiomes DIM_NETHER/DIM_OVERWORLD)
#define DIMENSION_OVERWORLD 0
#define DIMENSION_NETHER 1
#define DIMENSION_END 2

// Time between server ticks in microseconds (default = 50ms for 20 TPS)
#define TIME_BETWEEN_TICKS 50000

// Calculated from TIME_BETWEEN_TICKS
#define TICKS_PER_SECOND ((float)1000000 / TIME_BETWEEN_TICKS)

// Sound event IDs (from minecraft:sound_event registry)
#define SOUND_CATEGORY_MASTER 0
#define SOUND_CATEGORY_MUSIC 1
#define SOUND_CATEGORY_RECORDS 2
#define SOUND_CATEGORY_WEATHER 3
#define SOUND_CATEGORY_BLOCKS 4
#define SOUND_CATEGORY_HOSTILE 5
#define SOUND_CATEGORY_NEUTRAL 6
#define SOUND_CATEGORY_PLAYERS 7
#define SOUND_CATEGORY_AMBIENT 8
#define SOUND_CATEGORY_VOICE 9

// Hostile mob sounds
#define S_CREEPER_PRIMED 397
#define S_CREEPER_HURT 396

#define S_SKELETON_AMBIENT 1322
#define S_SKELETON_SHOOT 1334
#define S_SKELETON_HURT 1333

#define S_SPIDER_AMBIENT 1424
#define S_SPIDER_HURT 1426

#define S_ZOMBIE_AMBIENT 1701
#define S_ZOMBIE_HURT 1711

#define S_ENDERMAN_AMBIENT 510
#define S_ENDERMAN_HURT 512
#define S_ENDERMAN_SCREAM 513
#define S_ENDERMAN_STARE 514
#define S_ENDERMAN_TELEPORT 515

#define S_PIGLIN_AMBIENT 1144
#define S_PIGLIN_ANGRY 1145
#define S_PIGLIN_HURT 1149
#define S_PIGLIN_ADMIRE 1143

#define S_ZOMBIFIED_PIGLIN_AMBIENT 1713
#define S_ZOMBIFIED_PIGLIN_ANGRY 1714
#define S_ZOMBIFIED_PIGLIN_HURT 1716

// Passive mob sounds
#define S_CHICKEN_AMBIENT 307
#define S_CHICKEN_HURT 310

#define S_COW_AMBIENT 369
#define S_COW_HURT 371

#define S_PIG_AMBIENT 1138
#define S_PIG_HURT 1140

#define S_SHEEP_AMBIENT 1292
#define S_SHEEP_HURT 1294
#define S_SHEEP_SHEAR 1295

#define S_VILLAGER_AMBIENT 1535
#define S_VILLAGER_HURT 1538
#define S_VILLAGER_TRADE 1540

// Fish sounds
#define S_COD_AMBIENT 329
#define S_COD_HURT 332
#define S_COD_FLOP 331

#define S_SALMON_AMBIENT 1246
#define S_SALMON_HURT 1249
#define S_SALMON_FLOP 1248

#define S_PUFFERFISH_FLOP 1208
#define S_PUFFERFISH_HURT 1209

#define S_TROPICAL_FISH_AMBIENT 1477
#define S_TROPICAL_FISH_HURT 1480
#define S_TROPICAL_FISH_FLOP 1479

// Step sounds
#define S_CHICKEN_STEP 311
#define S_COW_STEP 373
#define S_PIG_STEP 1142
#define S_SHEEP_STEP 1296
#define S_WOOD_STEP 1689
#define S_ZOMBIE_STEP 1717
#define S_SKELETON_STEP 1335
#define S_SPIDER_STEP 1427
#define S_PIGLIN_STEP 1151

// Generic sounds
#define S_GENERIC_EXPLODE 615
#define S_ITEM_SHIELD_BLOCK 1298

// Initial world generation seed, will be hashed on startup
// Used in generating terrain and biomes
#define INITIAL_WORLD_SEED 0xA103DE6C

// Initial general RNG seed, will be hashed on startup
// Used in random game events like item drops and mob behavior
#define INITIAL_RNG_SEED 0xE2B9419

// Size of each bilinearly interpolated area ("minichunk")
// For best performance, CHUNK_SIZE should be a power of 2
#define CHUNK_SIZE 8

// Terrain low point - should start a bit below sea level for rivers/lakes
#define TERRAIN_BASE_HEIGHT 55

// Cave generation Y level (deeper for better mountain caves)
#define CAVE_BASE_DEPTH 20

// Size of every major biome in multiples of CHUNK_SIZE
// For best performance, should also be a power of 2
#define BIOME_SIZE (CHUNK_SIZE * 8)

// Calculated from BIOME_SIZE
#define BIOME_RADIUS (BIOME_SIZE / 2)

// How many visited chunk coordinates to "remember"
// The server will not re-send chunks that the player has recently been in
// Must be at least 1, otherwise chunks will be sent on each position update
// Set to cover the entire view area (21x21 = 441 for VIEW_DISTANCE=10) plus extra
// for the circular buffer to wrap without losing visible chunks
#define VISITED_HISTORY 1024

// How many player-made block changes to allow
// Determines the fixed amount of memory allocated to blocks
// Set to 0 to enable infinite block changes (dynamic allocation)
#define MAX_BLOCK_CHANGES 20000

// If defined, allows unlimited block changes with dynamic memory allocation.
// Block changes will grow as needed. Can also be enabled via config file
// by setting infinite_block_changes = true
// Note: This can consume significant memory on systems with lots of building.
#define INFINITE_BLOCK_CHANGES

// If defined, writes and reads world data to/from disk (or flash).
// This is a synchronous operation, and can cause performance issues if
// frequent random disk access is slow. Data is still stored in and
// accessed from memory - reading from disk is only done on startup.
// When targeting ESP-IDF, LittleFS is used to manage flash reads and
// writes. Flash is typically *very* slow and unreliable, which is why
// this option is disabled by default when targeting ESP-IDF.
#ifndef ESP_PLATFORM
  #define SYNC_WORLD_TO_DISK
#endif

// The minimum interval (in microseconds) at which certain data is written
// to disk/flash. Bounded on the low end by TIME_BETWEEN_TICKS. By default,
// applies only to player data. Block changes are written as soon as they
// are made, but in much smaller portions. Set DISK_SYNC_BLOCKS_ON_INTERVAL
// to make this apply to block changes as well.
#define DISK_SYNC_INTERVAL 15000000

// Whether to sync block changes to disk on an interval, instead of syncing
// on each change. On systems with fast random disk access, this shouldn't
// be necessary.
// #define DISK_SYNC_BLOCKS_ON_INTERVAL

// Time in microseconds to spend waiting for data transmission before
// timing out. Default is 15s, which leaves 5s to prevent starving other
// clients from Keep Alive packets.
#define NETWORK_TIMEOUT_TIME 15000000

// Size of the receive buffer for incoming string data
#define MAX_RECV_BUF_LEN 256

// If defined, sends the server brand to clients. Doesn't do much, but will
// show up in the top-left of the F3/debug menu, in the Minecraft client.
// You can change the brand string in the "brand" variable in src/globals.c
#define SEND_BRAND

// If defined, rebroadcasts ALL incoming movement updates, disconnecting
// movement from the server's tickrate. This makes movement much smoother
// on very low tickrates, at the cost of potential network instability when
// hosting more than just a couple of players. When disabling this on low
// tickrates, consider disabling SCALE_MOVEMENT_UPDATES_TO_PLAYER_COUNT too.
// DISABLED BY DEFAULT - causes lag with proxies/multiple players
// #define BROADCAST_ALL_MOVEMENT

// If defined, scales the frequency at which player movement updates are
// broadcast based on the amount of players, reducing overhead for higher
// player counts. For very many players, makes movement look jittery.
// It is not recommended to use this if BROADCAST_ALL_MOVEMENT is disabled
// on low tickrates, as that might drastically decrease the update rate.
#define SCALE_MOVEMENT_UPDATES_TO_PLAYER_COUNT

// If defined, calculates fluid flow when blocks are updated near fluids
// Somewhat computationally expensive and potentially unstable
#define DO_FLUID_FLOW

// If defined, allows players to craft and use chests.
// Chests take up 15 block change slots each, require additional checks,
// and use some terrible memory hacks to function. On some platforms, this
// could cause bad performance or even crashes during gameplay.
#define ALLOW_CHESTS

// If defined, allows players to craft and use doors.
// Doors take up 3 block change slots each (lower half + upper half + state),
// and require additional checks for placement and interaction.
#define ALLOW_DOORS

// If defined, enables flight for all players. As a side-effect, allows
// players to sprint when starving.
// #define ENABLE_PLAYER_FLIGHT

// If defined, enables the item pickup animation when mining a block/
// Does not affect how item pickups work! Items from broken blocks still
// get placed directly in the inventory, this is just an animation.
// Relatively inexpensive, though requires sending a few more packets
// every time a block is broken.
#define ENABLE_PICKUP_ANIMATION

// If defined, players are able to receive damage from nearby cacti.
#define ENABLE_CACTUS_DAMAGE

// If defined, logs unrecognized packet IDs
// #define DEV_LOG_UNKNOWN_PACKETS

// If defined, logs cases when packet length doesn't match parsed byte count
#define DEV_LOG_LENGTH_DISCREPANCY

// If defined, logs every parsed, enqueued and socket-sent packet.
// Extremely verbose; use only for debugging.
// #define DEV_LOG_ALL_PACKETS

// If defined, log chunk generation events
// #define DEV_LOG_CHUNK_GENERATION

// If defined, prints per-second chunk stream diagnostics.
// Useful to debug chunk spam/backpressure and packet queue stalls.
// #define DEV_LOG_CHUNK_STREAM_STATS
// If defined together with DEV_LOG_CHUNK_STREAM_STATS, also logs each
// chunk enqueue result (very noisy).
// #define DEV_LOG_CHUNK_STREAM_VERBOSE

// If defined, allows dumping world data by sending 0xBEEF (big-endian),
// and uploading world data by sending 0xFEED, followed by the data buffer.
// Doesn't implement authentication, hence disabled by default.
// #define DEV_ENABLE_BEEF_DUMPS

#define STATE_NONE 0
#define STATE_STATUS 1
#define STATE_LOGIN 2
#define STATE_TRANSFER 3
#define STATE_CONFIGURATION 4
#define STATE_PLAY 5

// Entity type identifiers
#define E_CREEPER 30
#define E_SKELETON 110
#define E_SPIDER 119
#define E_VILLAGER 134
#define E_PIGLIN 96
#define E_ZOMBIE 145
#define E_ENDERMAN 39
// Fish entity IDs for protocol 772 (1.21.8)
#define E_COD 26
#define E_SALMON 105
#define E_PUFFERFISH 102
#define E_TROPICAL_FISH 131

// Experience Orb entity type for protocol 772 (1.21.8)
#define E_XP_ORB 47

typedef struct OutPacket OutPacket;

typedef struct {
  int client_fd;
  int state;
  int compression_threshold;
  // Protects outbound queue fields below.
  pthread_mutex_t send_mutex;
  pthread_cond_t send_cond;
  // High-priority queue for gameplay/control packets.
  OutPacket *send_head_hi;
  OutPacket *send_tail_hi;
  // Low-priority queue for chunk stream packets.
  OutPacket *send_head_lo;
  OutPacket *send_tail_lo;
  // Total queued bytes (high + low).
  size_t queued_bytes;
  // Queued bytes in low-priority chunk queue only.
  size_t queued_chunk_bytes;
  // Monotonic enqueue order across high + low queues.
  uint64_t next_send_sequence;
  uint32_t connection_generation;
  // Reusable z_stream for compressed packet decompression
  z_stream inflate_stream;
  uint8_t inflate_initialized;
  uint8_t *compressed_buf;
  size_t compressed_buf_cap;
} ClientState;

extern ClientState client_states[MAX_PLAYERS];

// Initial outbound packet buffer size and incoming decompression buffer size.
// Outbound packets grow dynamically for large chunk-with-light packets.
#define MAX_PACKET_LEN 131072
extern THREAD_LOCAL uint8_t *packet_buffer;
extern THREAD_LOCAL int packet_buffer_offset;
extern THREAD_LOCAL int packet_mode;

extern uint8_t in_packet_buffer[MAX_PACKET_LEN];
extern int in_packet_buffer_offset;
extern int in_packet_buffer_len;

extern ssize_t recv_count;
extern uint8_t recv_buffer[MAX_RECV_BUF_LEN];

extern uint32_t world_seed;
extern uint32_t rng_seed;

extern uint16_t world_time;
extern uint64_t world_day_time;
extern uint32_t server_ticks;

extern float world_rain_level;
extern float world_thunder_level;
extern uint8_t world_weather_clear;
extern int32_t world_weather_clear_time;
extern int32_t world_weather_rain_time;
extern int32_t world_weather_thunder_time;

// Fluid update queue for deferred processing
#define FLUID_QUEUE_SIZE 8192
#define FLUID_UPDATES_PER_TICK 64

typedef struct {
  short x;
  int16_t y;
  short z;
  uint16_t fluid;
  uint16_t block;
} FluidUpdateEntry;

extern FluidUpdateEntry fluid_queue[FLUID_QUEUE_SIZE];
extern volatile int fluid_queue_head;
extern volatile int fluid_queue_tail;

#define MOTD_MAX_LEN 256
extern char motd[MOTD_MAX_LEN];
extern uint8_t motd_len;

// Favicon (server icon) - base64-encoded PNG with data URI prefix
#define FAVICON_MAX_LEN 16384
extern char favicon[FAVICON_MAX_LEN];
extern uint16_t favicon_len;  // length of the base64 string (excluding null terminator)

#ifdef SEND_BRAND
  #define BRAND_MAX_LEN 64
  extern char brand[BRAND_MAX_LEN];
  extern uint8_t brand_len;
#endif

extern uint16_t client_count;
extern uint8_t player_noclip[MAX_PLAYERS];

typedef struct {
  short x;
  short z;
  int16_t y;
  uint16_t block;
  uint8_t dimension;
} BlockChange;

#pragma pack(push, 1)

typedef struct {
  uint8_t uuid[16];
  char name[16];
  int client_fd;
  short x;
  int16_t y;
  short z;
  short visited_x[VISITED_HISTORY];
  short visited_z[VISITED_HISTORY];
  uint16_t visited_next;  // Next slot to write in circular buffer
  #ifdef SCALE_MOVEMENT_UPDATES_TO_PLAYER_COUNT
    uint16_t packets_since_update;
  #endif
  int8_t yaw;
  int8_t pitch;
  int16_t grounded_y;
  uint8_t health;
  uint8_t hunger;
  uint16_t saturation;
  uint16_t air;  // Remaining air ticks (300 = full, 0 = drowning)
  uint8_t hotbar;
  uint16_t inventory_items[41];
  uint16_t craft_items[9];
  uint8_t inventory_count[41];
  uint8_t craft_count[9];
  uint16_t inventory_damage[41];
  uint16_t craft_damage[9];
  uint16_t cursor_damage;
  // Usage depends on player's flags, see below
  // When no flags are set, acts as cursor item ID
  uint16_t flagval_16;
  // Usage depends on player's flags, see below
  // When no flags are set, acts as cursor item count
  uint8_t flagval_8;
  // 0x01 - attack cooldown, uses flagval_8 as the timer
  // 0x02 - has not spawned yet
  // 0x04 - sneaking
  // 0x08 - sprinting
  // 0x10 - eating, makes flagval_16 act as eating timer
  // 0x20 - client loading, uses flagval_16 as fallback timer
  // 0x40 - movement update cooldown
  // 0x80 - craft_items lock (for storing pointers)
  // 0x0100 - blocking with a shield
  uint16_t flags;
  // Experience tracking
  uint16_t xp_total;    // Total XP accumulated
  uint16_t xp_level;    // Current XP level
  float xp_progress;    // Progress towards next level (0.0 to 1.0)
  uint8_t dimension;
  uint8_t portal_valid;  // 0 = none, 1 = portal_ow fields track overworld entry
  uint32_t last_bucket_tick;  // Tick of last successful block-targeted bucket action (prevents duplicate via face==255)
  int64_t last_attack_time;  // Microsecond timestamp of last player attack (for cooldown/charge calculation)
  short portal_ow_x;
  int16_t portal_ow_y;
  short portal_ow_z;
  // Short post-dimension-change movement lock while destination chunks become collidable.
  uint8_t position_lock_ticks;
  short locked_x;
  int16_t locked_y;
  short locked_z;
  // Bed respawn point (standing position, usually one block above the bed)
  uint8_t spawn_set;
  short spawn_x;
  int16_t spawn_y;
  short spawn_z;
  uint8_t spawn_dimension;
  // Merchant/trade state (villager trading GUI)
  uint8_t merchant_open;         // 1 when merchant GUI is active
  int merchant_villager_eid;     // Entity ID of the villager
  uint8_t selected_trade;        // Currently selected trade index
  // Ender chest inventory (per-player, cross-dimension)
  uint16_t ender_chest_items[27];
  uint8_t ender_chest_count[27];
  uint16_t ender_chest_damage[27];
} PlayerData;

#define PLAYER_TEXTURE_VALUE_MAX 4096
#define PLAYER_TEXTURE_SIGNATURE_MAX 2048

typedef struct {
  char *texture_value;       // dynamically allocated (was char[4096])
  char *texture_signature;   // dynamically allocated (was char[2048])
  uint16_t texture_value_len;
  uint16_t texture_signature_len;
  uint8_t has_texture;
  uint8_t has_signature;
  uint8_t skin_parts;
  uint8_t main_hand;
} PlayerAppearance;

typedef struct {
  uint8_t type;
  double x;  // Floating-point position for smooth movement
  // When the mob is dead (health is 0), the Y coordinate acts
  // as a timer for deleting and deallocating the mob
  double y;
  double z;
  // Movement direction (persisted across ticks for smoother walking)
  double move_dx;
  double move_dz;
  double move_dy;  // Vertical movement for swimming/flying
  int move_timer;  // How many ticks to keep moving in this direction
  int anger_timer; // Hostile-neutral mobs attack while this is positive
  uint8_t yaw_store;  // Consistent yaw while walking
  // Lower 5 bits: health
  // Middle 1 bit: sheep sheared, unused for other mobs
  // Upper 2 bits: panic timer
  uint8_t data;
  uint8_t profession;  // Village profession ID (for villagers)
  uint8_t dimension;
  uint8_t look_timer;  // Ticks remaining in random look-around (0 = not looking)
  uint8_t look_yaw;    // Head yaw during random look-around
  int8_t  look_pitch;  // Head pitch during random look-around
} MobData;

// Experience Orb data structure
#define MAX_XP_ORBS 128
#define XP_ORB_ENTITY_ID_BASE -300

typedef struct {
  uint8_t active;
  double x;
  double y;
  double z;
  uint8_t value;   // XP value this orb provides
  uint8_t count;   // Visual size (1-3 for small/medium/large)
  uint8_t dimension;
  uint32_t age;    // Ticks since spawn
} XpOrbData;

// Item entity data structure
#define MAX_ITEM_ENTITIES 256
#define ITEM_ENTITY_ID_BASE -500

typedef struct {
  uint8_t active;
  double x;
  double y;
  double z;
  double vx;
  double vy;
  double vz;
  uint8_t on_ground;
  uint16_t item;
  uint8_t count;
  uint8_t dimension;
  uint32_t age;
} ItemEntityData;

#ifdef ALLOW_DOORS
typedef struct {
  // Door state flags:
  // Bit 0: open (0 = closed, 1 = open)
  // Bit 1: hinge (0 = left, 1 = right)
  // Bit 2-3: powered state (for redstone)
  // Bits 4-5: direction (0-3: North, East, South, West)
  // Bit 6: upper half flag (0 = lower, 1 = upper)
  uint8_t flags;
} DoorData;
#endif

#pragma pack(pop)

union EntityDataValue {
  uint8_t byte;
  int varint;
  int pose;
};

typedef struct {
  uint8_t index;
  // 0 - Byte
  // 1 - VarInt
  // 21 - Pose
  int type;
  union EntityDataValue value;
} EntityData;

#ifdef INFINITE_BLOCK_CHANGES
  extern BlockChange *block_changes;
  extern int block_changes_capacity;
#else
  extern BlockChange block_changes[MAX_BLOCK_CHANGES];
#endif
extern int block_changes_count;

extern PlayerData player_data[MAX_PLAYERS];
extern int player_data_count;
extern PlayerAppearance *player_appearance;

extern MobData mob_data[MAX_MOBS];
extern uint8_t mob_trade_uses[MAX_MOBS][5];
extern XpOrbData xp_orb_data[MAX_XP_ORBS];
extern ItemEntityData item_entity_data[MAX_ITEM_ENTITIES];

// Projectile entity type identifiers for protocol 772 (1.21.8)
#define E_ARROW 6
#define E_ENDER_PEARL 42

#define MAX_PROJECTILES 16

typedef struct {
  uint8_t active;
  uint8_t type;
  int owner_index;
  uint8_t dimension;
  double x, y, z;
  double vx, vy, vz;
  uint64_t spawn_tick;
  uint8_t damage;
  uint8_t stuck;
  uint8_t uuid[16];
} ProjectileData;

extern ProjectileData projectile_data[MAX_PROJECTILES];

// Thread pool functions
void init_global_thread_pool(void);
ThreadPool* get_global_thread_pool(void);
void shutdown_global_thread_pool(void);

// Verbose debug logging (set via --verbose / -v command-line flag)
extern int verbose_mode;

#endif
