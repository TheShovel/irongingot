#ifndef H_SPECIAL_BLOCK
#define H_SPECIAL_BLOCK

#include <stdint.h>

/*
 * Unified special block state system.
 *
 * Instead of the old multi-entry BlockChange hack, every special block
 * stores its state in a compact hash table keyed by block position.
 * The state is a single uint16_t that encodes all relevant properties.
 *
 * State encoding per block type:
 *
 * Doors (B_oak_door, B_spruce_door, B_birch_door, B_iron_door):
 *   Bits 0:     open (0=closed, 1=open)
 *   Bit  1:     hinge (0=left, 1=right)
 *   Bits 2-3:   direction (0=north, 1=east, 2=south, 3=west)
 *   Bit  4:     half (0=lower, 1=upper) -- derived from Y, not stored
 *   Bits 5-15:  unused
 *
 * Stairs (B_oak_stairs, B_spruce_stairs, B_birch_stairs, B_cobblestone_stairs):
 *   Bits 0-1:   half (0=bottom, 1=top)
 *   Bits 2-3:   direction (0=north, 1=east, 2=south, 3=west)
 *   Bits 4-15:  unused
 *
 * Furnaces (B_furnace):
 *   Bits 0-1:   direction (0=north, 1=east, 2=south, 3=west)
 *   Bit  2:     lit (0=unlit, 1=lit)
 *   Bits 3-15:  unused
 *
 * Chests (B_chest):
 *   Bits 0-1:   direction (0=north, 1=east, 2=south, 3=west)
 *   Bits 2-15:  unused  (inventory stored separately)
 */

/* Maximum number of special block state entries we track. */
#define MAX_SPECIAL_BLOCKS 8192

/* Position key packed into a single 32-bit value for hashing. */
typedef struct {
    int32_t key;       /* packed (x << 16) | (z & 0xFFFF), y in upper bits */
    uint16_t state;    /* encoded state bits */
    uint8_t block;     /* block type for validation */
    uint8_t occupied;  /* 1 if slot is in use */
} SpecialBlockEntry;

extern SpecialBlockEntry special_blocks[MAX_SPECIAL_BLOCKS];
extern int special_blocks_count;

/* Hash table operations */
void special_block_init(void);
uint16_t special_block_get_state(short x, uint8_t y, short z);
void special_block_set_state(short x, uint8_t y, short z, uint8_t block, uint16_t state);
void special_block_clear(short x, uint8_t y, short z);
uint8_t special_block_has_entry(short x, uint8_t y, short z);

/* Block state ID computation for network packets */
uint16_t get_door_state_id(uint8_t block, uint8_t is_upper, uint8_t open, uint8_t direction, uint8_t hinge);
uint16_t get_trapdoor_state_id(uint8_t block, uint8_t open, uint8_t direction, uint8_t half);
uint16_t get_stair_state_id(uint8_t block, uint8_t half, uint8_t direction);
uint16_t get_oriented_state_id(uint8_t block, uint8_t direction);
uint16_t get_furnace_state_id(uint8_t direction, uint8_t lit);

/* Block type queries */
uint8_t is_door_block(uint8_t block);
uint8_t is_stair_block(uint8_t block);
uint8_t is_trapdoor_block(uint8_t block);
uint8_t is_oriented_block(uint8_t block);

/* State decode helpers */
uint8_t door_get_open(uint16_t state);
uint8_t door_get_hinge(uint16_t state);
uint8_t door_get_direction(uint16_t state);
uint8_t stair_get_half(uint16_t state);
uint8_t stair_get_direction(uint16_t state);
uint8_t oriented_get_direction(uint16_t state);
uint8_t furnace_get_direction(uint16_t state);
uint8_t furnace_get_lit(uint16_t state);

/* State encode helpers */
uint16_t door_encode_state(uint8_t open, uint8_t hinge, uint8_t direction);
uint16_t stair_encode_state(uint8_t half, uint8_t direction);
uint16_t oriented_encode_state(uint8_t direction);
uint16_t furnace_encode_state(uint8_t direction, uint8_t lit);

/* Interaction helpers */
uint8_t is_door_open_at(short x, uint8_t y, short z);
void toggle_door_state(short x, uint8_t y, short z);

#endif
