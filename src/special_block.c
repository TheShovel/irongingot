#include <string.h>
#include "globals.h"
#include "special_block.h"
#include "registries.h"

SpecialBlockEntry special_blocks[MAX_SPECIAL_BLOCKS];
int special_blocks_count = 0;

/* ── Hash table internals ─────────────────────────────────────────── */

static inline uint32_t pack_key(short x, uint8_t y, short z) {
    return ((uint32_t)(uint16_t)x << 16) | ((uint32_t)y << 8) | (uint16_t)z;
}

static inline uint32_t entry_hash(uint32_t key) {
    uint32_t h = key * 0x9E3779B9u;
    return h ^ (h >> 16);
}

static int find_entry(short x, uint8_t y, short z) {
    uint32_t key = pack_key(x, y, z);
    uint32_t mask = MAX_SPECIAL_BLOCKS - 1;
    uint32_t base = entry_hash(key) & mask;

    for (uint32_t probe = 0; probe < MAX_SPECIAL_BLOCKS; probe++) {
        uint32_t idx = (base + probe) & mask;
        if (!special_blocks[idx].occupied) return -1;
        if (special_blocks[idx].key == (int32_t)key) return (int)idx;
    }
    return -1;
}

static int insert_entry(short x, uint8_t y, short z) {
    uint32_t key = pack_key(x, y, z);
    uint32_t mask = MAX_SPECIAL_BLOCKS - 1;
    uint32_t base = entry_hash(key) & mask;

    for (uint32_t probe = 0; probe < MAX_SPECIAL_BLOCKS; probe++) {
        uint32_t idx = (base + probe) & mask;
        if (!special_blocks[idx].occupied) {
            special_blocks[idx].key = (int32_t)key;
            special_blocks[idx].occupied = 1;
            special_blocks_count++;
            return (int)idx;
        }
        if (special_blocks[idx].key == (int32_t)key) return (int)idx;
    }
    return -1;  /* table full */
}

/* ── Public API ───────────────────────────────────────────────────── */

void special_block_init(void) {
    memset(special_blocks, 0, sizeof(special_blocks));
    special_blocks_count = 0;
}

uint16_t special_block_get_state(short x, uint8_t y, short z) {
    int idx = find_entry(x, y, z);
    if (idx < 0) return 0;
    return special_blocks[idx].state;
}

void special_block_set_state(short x, uint8_t y, short z, uint8_t block, uint16_t state) {
    int idx = insert_entry(x, y, z);
    if (idx < 0) return;
    special_blocks[idx].state = state;
    special_blocks[idx].block = block;
}

void special_block_clear(short x, uint8_t y, short z) {
    int idx = find_entry(x, y, z);
    if (idx < 0) return;
    special_blocks[idx].occupied = 0;
    special_blocks[idx].state = 0;
    special_blocks[idx].block = 0;
    special_blocks[idx].key = 0;
    special_blocks_count--;
    if (special_blocks_count < 0) special_blocks_count = 0;
}

uint8_t special_block_has_entry(short x, uint8_t y, short z) {
    return find_entry(x, y, z) >= 0;
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
        block == B_pale_oak_stairs
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
    uint16_t base_id = block_palette[block];
    /*
     * Stair state offsets:
     *   facing: north=0, east=60, south=20, west=40
     *   top half: -10
     */
    static const int16_t facing_off[4] = {0, 60, 20, 40};
    int16_t offset = facing_off[direction];
    if (half) offset -= 10;
    return base_id + (uint16_t)offset;
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
uint8_t stair_get_half(uint16_t state)    { return (state >> 0) & 3; }
uint8_t stair_get_direction(uint16_t state){ return (state >> 2) & 3; }
uint8_t oriented_get_direction(uint16_t state) { return state & 3; }
uint8_t furnace_get_direction(uint16_t state) { return state & 3; }
uint8_t furnace_get_lit(uint16_t state)   { return (state >> 2) & 1; }

/* Encode helpers */
uint16_t door_encode_state(uint8_t open, uint8_t hinge, uint8_t direction) {
    return (uint16_t)((direction << 2) | (hinge << 1) | open);
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
    /* Look up state for this position (lower or upper half share the same entry key) */
    uint16_t state = special_block_get_state(x, y, z);
    return door_get_open(state);
}

void toggle_door_state(short x, uint8_t y, short z) {
    uint16_t state = special_block_get_state(x, y, z);
    state ^= 0x01;  /* flip open bit */
    special_block_set_state(x, y, z, 0, state);  /* block type doesn't matter for update */
}
