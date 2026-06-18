#ifndef H_SPECIAL_BLOCK
#define H_SPECIAL_BLOCK

#include <stdint.h>

/*
 * Unified special block state system.
 *
 * Every special block stores its state in a compact hash table keyed by the
 * full block position. The state is a single uint16_t that encodes all
 * relevant properties.
 *
 * State encoding per block type:
 *
 * Doors (all supported wooden doors):
 *   Bits 0:     open (0=closed, 1=open)
 *   Bit  1:     hinge (0=left, 1=right)
 *   Bits 2-3:   direction (0=north, 1=east, 2=south, 3=west)
 *   Bits 4-15:  unused
 *
 * Trapdoors (all supported wooden trapdoors):
 *   Bits 0:     open (0=closed, 1=open)
 *   Bit  1:     half (0=bottom, 1=top)
 *   Bits 2-3:   direction (0=north, 1=east, 2=south, 3=west)
 *   Bits 4-15:  unused
 *
 * Stairs (all supported stairs):
 *   Bits 0-1:   half (0=bottom, 1=top)
 *   Bits 2-3:   direction (0=north, 1=east, 2=south, 3=west)
 *   Bits 4-6:   shape (0=straight, 1=inner_left, 2=inner_right, 3=outer_left, 4=outer_right)
 *   Bits 7-15:  unused
 *
 * Slabs (generated templates only):
 *   Bits 0-1:   type (0=bottom, 1=top, 2=double)
 *   Bits 2-15:  unused
 *
 * Furnaces (B_furnace):
 *   Bits 0-1:   direction (0=north, 1=east, 2=south, 3=west)
 *   Bit  2:     lit (0=unlit, 1=lit)
 *   Bits 3-15:  unused
 *
 * Chests (B_chest):
 *   Bits 0-1:   direction (0=north, 1=east, 2=south, 3=west)
 *   Bits 2-15:  unused
 *
 * Barrels (B_barrel):
 *   Bits 0-2:   direction (0=north, 1=east, 2=south, 3=west, 4=up, 5=down)
 *   Bit  3:     open (0=closed, 1=open)
 *   Bits 4-15:  unused
 *
 * Ender Chests (B_ender_chest):
 *   Bits 0-1:   direction (0=north, 1=east, 2=south, 3=west)
 *   Bit  2:     waterlogged (0=false, 1=true)
 *   Bits 3-15:  unused
 *
 * Fences (all supported wooden fences):
 *   Bits 0-3:   connections (0=north, 1=east, 2=south, 3=west)
 *   Bits 4-15:  unused
 *
 * Horizontal facing blocks (B_wall_torch, etc.):
 *   Bits 0-1:   direction (0=north, 1=south, 2=west, 3=east) - table order
 *   Bits 2-15:  unused
 *
 * Beds (all colors):
 *   Bits 0-1:   direction (0=north, 1=east, 2=south, 3=west)
 *   Bit  2:     head part (0=foot, 1=head)
 *   Bit  3:     occupied (0=false, 1=true)
 *   Bits 4-15:  unused
 */

/* Dynamically growing hash table for special block states */
typedef struct {
    int16_t x;
    int16_t z;
    uint16_t state;
    uint8_t y;
    uint8_t dimension;
    uint16_t block;
} SpecialBlockEntry;

#define SPECIAL_BLOCK_EMPTY 0xFF

extern SpecialBlockEntry *special_blocks;
extern int special_blocks_count;
extern int special_blocks_capacity;

/* Dynamically growing wheat coordinate tracking list */
typedef struct {
    short x;
    uint8_t y;
    short z;
    uint8_t dimension;
} WheatCoord;
extern WheatCoord *wheat_coords;
extern int wheat_count;
extern int wheat_capacity;

/* Hash table operations */
void special_block_init(void);
uint16_t special_block_get_state(short x, uint8_t y, short z, uint8_t dimension);
void special_block_set_state(short x, uint8_t y, short z, uint8_t dimension, uint16_t block, uint16_t state);
void special_block_clear(short x, uint8_t y, short z, uint8_t dimension);
uint8_t special_block_has_entry(short x, uint8_t y, short z, uint8_t dimension);

/* Block state ID computation for network packets */
uint16_t get_door_state_id(uint16_t block, uint8_t is_upper, uint8_t open, uint8_t direction, uint8_t hinge);
uint16_t get_trapdoor_state_id(uint16_t block, uint8_t open, uint8_t direction, uint8_t half);
uint16_t get_stair_state_id(uint16_t block, uint8_t half, uint8_t direction);
uint16_t get_stair_shape_state_id(uint16_t block, uint8_t half, uint8_t direction, uint8_t shape);
uint16_t get_slab_state_id(uint16_t block, uint8_t type);
uint16_t get_oriented_state_id(uint16_t block, uint8_t direction);
uint16_t get_furnace_state_id(uint8_t direction, uint8_t lit);
uint16_t get_fence_state_id(uint16_t block, uint8_t connections);
uint16_t get_horizontal_state_id(uint16_t block, uint8_t direction);
uint16_t get_bed_state_id(uint16_t block, uint8_t head, uint8_t occupied, uint8_t direction);

/* Block type queries */
uint8_t is_door_block(uint16_t block);
uint8_t is_stair_block(uint16_t block);
uint8_t is_slab_block(uint16_t block);
uint8_t is_trapdoor_block(uint16_t block);
uint8_t is_oriented_block(uint16_t block);
uint8_t is_fence_block(uint16_t block);
uint8_t is_horizontal_facing_block(uint16_t block);
uint8_t is_bed_block(uint16_t block);
uint8_t is_full_block(uint16_t block);

/* State decode helpers */
uint8_t door_get_open(uint16_t state);
uint8_t door_get_hinge(uint16_t state);
uint8_t door_get_direction(uint16_t state);
uint8_t trapdoor_get_open(uint16_t state);
uint8_t trapdoor_get_half(uint16_t state);
uint8_t trapdoor_get_direction(uint16_t state);
uint8_t stair_get_half(uint16_t state);
uint8_t stair_get_direction(uint16_t state);
uint8_t stair_get_shape(uint16_t state);
uint8_t slab_get_type(uint16_t state);
uint8_t oriented_get_direction(uint16_t state);
uint8_t furnace_get_direction(uint16_t state);
uint8_t furnace_get_lit(uint16_t state);
uint8_t barrel_get_direction(uint16_t state);
uint8_t barrel_get_open(uint16_t state);
uint8_t ender_chest_get_direction(uint16_t state);
uint8_t ender_chest_get_waterlogged(uint16_t state);
uint8_t fence_get_north(uint16_t state);
uint8_t fence_get_east(uint16_t state);
uint8_t fence_get_south(uint16_t state);
uint8_t fence_get_west(uint16_t state);
uint8_t bed_get_direction(uint16_t state);
uint8_t bed_get_head(uint16_t state);
uint8_t bed_get_occupied(uint16_t state);

/* State encode helpers */
uint16_t door_encode_state(uint8_t open, uint8_t hinge, uint8_t direction);
uint16_t trapdoor_encode_state(uint8_t open, uint8_t half, uint8_t direction);
uint16_t stair_encode_state(uint8_t half, uint8_t direction);
uint16_t stair_shape_encode_state(uint8_t half, uint8_t direction, uint8_t shape);
uint16_t slab_encode_state(uint8_t type);
uint16_t oriented_encode_state(uint8_t direction);
uint16_t furnace_encode_state(uint8_t direction, uint8_t lit);
uint16_t barrel_encode_state(uint8_t direction, uint8_t open);
uint16_t ender_chest_encode_state(uint8_t direction, uint8_t waterlogged);
uint16_t fence_encode_state(uint8_t north, uint8_t east, uint8_t south, uint8_t west);
uint16_t horizontal_facing_encode_state(uint8_t direction);
uint16_t bed_encode_state(uint8_t head, uint8_t occupied, uint8_t direction);

/* Interaction helpers */
uint8_t is_door_open_at(short x, uint8_t y, short z, uint8_t dimension);
void toggle_door_state(short x, uint8_t y, short z, uint8_t dimension);

#endif
