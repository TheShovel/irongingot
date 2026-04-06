#include <string.h>
#include "globals.h"
#include "special_block.h"
#include "registries.h"

SpecialBlockEntry special_blocks[MAX_SPECIAL_BLOCKS];
int special_blocks_count = 0;
static pthread_mutex_t special_block_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── Hash table internals ─────────────────────────────────────────── */

static inline uint8_t entry_is_empty(const SpecialBlockEntry *entry) {
    return entry->block == SPECIAL_BLOCK_EMPTY;
}

static inline void clear_entry(SpecialBlockEntry *entry) {
    entry->x = 0;
    entry->z = 0;
    entry->state = 0;
    entry->y = 0;
    entry->block = SPECIAL_BLOCK_EMPTY;
}

static inline uint32_t entry_hash(short x, uint8_t y, short z) {
    uint32_t h = (uint16_t)x;
    h ^= ((uint32_t)(uint16_t)z << 1) | ((uint32_t)y << 17);
    h *= 0x9E3779B9u;
    h ^= h >> 16;
    return h;
}

static inline uint8_t entry_matches(const SpecialBlockEntry *entry, short x, uint8_t y, short z) {
    return (
        entry->x == x &&
        entry->y == y &&
        entry->z == z
    );
}

static int find_entry_locked(short x, uint8_t y, short z) {
    uint32_t mask = MAX_SPECIAL_BLOCKS - 1;
    uint32_t base = entry_hash(x, y, z) & mask;

    for (uint32_t probe = 0; probe < MAX_SPECIAL_BLOCKS; probe++) {
        uint32_t idx = (base + probe) & mask;
        if (entry_is_empty(&special_blocks[idx])) return -1;
        if (entry_matches(&special_blocks[idx], x, y, z)) return (int)idx;
    }
    return -1;
}

static int insert_entry_locked(short x, uint8_t y, short z) {
    uint32_t mask = MAX_SPECIAL_BLOCKS - 1;
    uint32_t base = entry_hash(x, y, z) & mask;

    for (uint32_t probe = 0; probe < MAX_SPECIAL_BLOCKS; probe++) {
        uint32_t idx = (base + probe) & mask;
        if (entry_is_empty(&special_blocks[idx])) {
            special_blocks[idx].x = x;
            special_blocks[idx].y = y;
            special_blocks[idx].z = z;
            special_blocks[idx].state = 0;
            special_blocks_count++;
            return (int)idx;
        }
        if (entry_matches(&special_blocks[idx], x, y, z)) return (int)idx;
    }
    return -1;  /* table full */
}

static void reinsert_entry_locked(SpecialBlockEntry entry) {
    int idx = insert_entry_locked(entry.x, entry.y, entry.z);
    if (idx < 0) return;
    special_blocks[idx] = entry;
}

/* ── Public API ───────────────────────────────────────────────────── */

void special_block_init(void) {
    pthread_mutex_lock(&special_block_mutex);
    for (int i = 0; i < MAX_SPECIAL_BLOCKS; i++) {
        clear_entry(&special_blocks[i]);
    }
    special_blocks_count = 0;
    pthread_mutex_unlock(&special_block_mutex);
}

uint16_t special_block_get_state(short x, uint8_t y, short z) {
    pthread_mutex_lock(&special_block_mutex);
    int idx = find_entry_locked(x, y, z);
    if (idx < 0) {
        pthread_mutex_unlock(&special_block_mutex);
        return 0;
    }
    uint16_t state = special_blocks[idx].state;
    pthread_mutex_unlock(&special_block_mutex);
    return state;
}

void special_block_set_state(short x, uint8_t y, short z, uint8_t block, uint16_t state) {
    pthread_mutex_lock(&special_block_mutex);
    int idx = insert_entry_locked(x, y, z);
    if (idx < 0) {
        pthread_mutex_unlock(&special_block_mutex);
        return;
    }
    special_blocks[idx].state = state;
    special_blocks[idx].block = block;
    pthread_mutex_unlock(&special_block_mutex);
}

void special_block_clear(short x, uint8_t y, short z) {
    pthread_mutex_lock(&special_block_mutex);
    int idx = find_entry_locked(x, y, z);
    if (idx < 0) {
        pthread_mutex_unlock(&special_block_mutex);
        return;
    }

    clear_entry(&special_blocks[idx]);
    special_blocks_count--;

    uint32_t mask = MAX_SPECIAL_BLOCKS - 1;
    for (uint32_t probe = ((uint32_t)idx + 1) & mask;
         !entry_is_empty(&special_blocks[probe]);
         probe = (probe + 1) & mask) {
        SpecialBlockEntry entry = special_blocks[probe];
        clear_entry(&special_blocks[probe]);
        special_blocks_count--;
        reinsert_entry_locked(entry);
    }

    if (special_blocks_count < 0) special_blocks_count = 0;
    pthread_mutex_unlock(&special_block_mutex);
}

uint8_t special_block_has_entry(short x, uint8_t y, short z) {
    pthread_mutex_lock(&special_block_mutex);
    uint8_t has_entry = find_entry_locked(x, y, z) >= 0;
    pthread_mutex_unlock(&special_block_mutex);
    return has_entry;
}

/* ── Block type queries ───────────────────────────────────────────── */

uint8_t is_door_block(uint8_t block) {
    return (
        block == B_oak_door ||
        block == B_spruce_door ||
        block == B_birch_door ||
        block == B_jungle_door ||
        block == B_acacia_door ||
        block == B_cherry_door ||
        block == B_dark_oak_door ||
        block == B_pale_oak_door ||
        block == B_mangrove_door
    );
}

uint8_t is_stair_block(uint8_t block) {
    return (
        block == B_oak_stairs ||
        block == B_spruce_stairs ||
        block == B_birch_stairs ||
        block == B_jungle_stairs ||
        block == B_acacia_stairs ||
        block == B_dark_oak_stairs ||
        block == B_mangrove_stairs ||
        block == B_cherry_stairs ||
        block == B_pale_oak_stairs ||
        block == B_cobblestone_stairs
    );
}

uint8_t is_trapdoor_block(uint8_t block) {
    return (
        block == B_oak_trapdoor ||
        block == B_spruce_trapdoor ||
        block == B_birch_trapdoor ||
        block == B_jungle_trapdoor ||
        block == B_acacia_trapdoor ||
        block == B_cherry_trapdoor ||
        block == B_dark_oak_trapdoor ||
        block == B_pale_oak_trapdoor ||
        block == B_mangrove_trapdoor
    );
}

uint8_t is_oriented_block(uint8_t block) {
    return (
        block == B_chest ||
        block == B_furnace
    );
}

/* ── State encoding / decoding ────────────────────────────────────── */

/*
 * Minecraft block state offsets (derived from actual registry layout).
 * These offsets are added to the base palette ID to get the final state ID.
 */

uint16_t get_door_state_id(uint8_t block, uint8_t is_upper, uint8_t open, uint8_t direction, uint8_t hinge) {
    uint16_t base_id = block_palette[block];
    /*
     * Door state offsets:
     *   facing: north=0, east=48, south=16, west=32
     *   hinge right: +4
     *   open: -2
     *   upper half: -8
     */
    static const int16_t facing_off[4] = {0, 48, 16, 32};
    int16_t offset = facing_off[direction];
    if (hinge)  offset += 4;
    if (open)   offset -= 2;
    if (is_upper) offset -= 8;
    return base_id + (uint16_t)offset;
}

uint16_t get_stair_state_id(uint8_t block, uint8_t half, uint8_t direction) {
    /*
     * State layout: facing(n,s,w,e) × half(bottom,top), straight shape,
     * non-waterlogged.
     */
    static const uint8_t table_facing[4] = {0, 3, 1, 2};  /* internal(n,e,s,w) → table(n,s,w,e) */
    uint8_t tf = table_facing[direction];
    uint8_t idx = tf * 2 + (half ? 1 : 0);
    uint8_t row = stair_block_to_row[block];
    return stair_state_rows[row][idx];
}

uint16_t get_trapdoor_state_id(uint8_t block, uint8_t open, uint8_t direction, uint8_t half) {
    /*
     * Use registry-generated lookup table.
     * State layout: facing(n,s,w,e) × half(bottom,top) × open(false,true), non-waterlogged, non-powered
     * Internal direction: 0=north, 1=east, 2=south, 3=west
     * Table facing order: 0=north, 1=south, 2=west, 3=east
     */
    static const uint8_t table_facing[4] = {0, 3, 1, 2};  // internal(n,e,s,w) → table(n,s,w,e)
    uint8_t tf = table_facing[direction];
    uint8_t idx = tf * 4 + half * 2 + open;
    uint8_t row = trapdoor_block_to_row[block];
    return trapdoor_state_rows[row][idx];
}

uint16_t get_oriented_state_id(uint8_t block, uint8_t direction) {
    uint16_t base_id = block_palette[block];
    if (block == B_chest) {
        /* chest: north=0, south=6, west=12, east=18 */
        static const uint16_t off[4] = {0, 6, 12, 18};
        return base_id + off[direction];
    } else if (block == B_furnace) {
        /* furnace: north=0, south=2, west=4, east=6 */
        return base_id + (uint16_t)(direction * 2);
    }
    return base_id;
}

uint16_t get_furnace_state_id(uint8_t direction, uint8_t lit) {
    uint16_t base = get_oriented_state_id(B_furnace, direction);
    /* lit state is typically +1 offset from unlit in the palette */
    if (lit) {
        /* furnace lit uses a different block ID entirely in most registries */
        /* For now we just return the base + direction offset */
        /* The actual lit variant should be looked up from registry */
    }
    return base;
}

/* Decode helpers */
uint8_t door_get_open(uint16_t state)     { return (state >> 0) & 1; }
uint8_t door_get_hinge(uint16_t state)    { return (state >> 1) & 1; }
uint8_t door_get_direction(uint16_t state){ return (state >> 2) & 3; }
uint8_t trapdoor_get_open(uint16_t state) { return door_get_open(state); }
uint8_t trapdoor_get_half(uint16_t state) { return door_get_hinge(state); }
uint8_t trapdoor_get_direction(uint16_t state) { return door_get_direction(state); }
uint8_t stair_get_half(uint16_t state)    { return (state >> 0) & 3; }
uint8_t stair_get_direction(uint16_t state){ return (state >> 2) & 3; }
uint8_t oriented_get_direction(uint16_t state) { return state & 3; }
uint8_t furnace_get_direction(uint16_t state) { return state & 3; }
uint8_t furnace_get_lit(uint16_t state)   { return (state >> 2) & 1; }

/* Encode helpers */
uint16_t door_encode_state(uint8_t open, uint8_t hinge, uint8_t direction) {
    return (uint16_t)((direction << 2) | (hinge << 1) | open);
}
uint16_t trapdoor_encode_state(uint8_t open, uint8_t half, uint8_t direction) {
    return door_encode_state(open, half, direction);
}
uint16_t stair_encode_state(uint8_t half, uint8_t direction) {
    return (uint16_t)((direction << 2) | (half & 3));
}
uint16_t oriented_encode_state(uint8_t direction) {
    return (uint16_t)(direction & 3);
}
uint16_t furnace_encode_state(uint8_t direction, uint8_t lit) {
    return (uint16_t)((lit << 2) | (direction & 3));
}

/* ── Interaction helpers ──────────────────────────────────────────── */

uint8_t is_door_open_at(short x, uint8_t y, short z) {
    uint16_t state = special_block_get_state(x, y, z);
    return door_get_open(state);
}

void toggle_door_state(short x, uint8_t y, short z) {
    pthread_mutex_lock(&special_block_mutex);
    int idx = find_entry_locked(x, y, z);
    if (idx >= 0) {
        special_blocks[idx].state ^= 0x01;
    }
    pthread_mutex_unlock(&special_block_mutex);
}
