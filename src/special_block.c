#include <stdlib.h>
#include <string.h>
#include "globals.h"
#include "special_block.h"
#include "registries.h"

/* Start small, grow as needed. Power-of-2 so (cap-1) works as a bitmask. */
#define INITIAL_BLOCK_CAPACITY 256
#define INITIAL_WHEAT_CAPACITY 512
/* Grow when load factor reaches 3/4 */
#define GROW_LIMIT(cap) ((cap) * 3 / 4)

SpecialBlockEntry *special_blocks = NULL;
int special_blocks_count = 0;
int special_blocks_capacity = 0;

WheatCoord *wheat_coords = NULL;
int wheat_count = 0;
int wheat_capacity = 0;

static pthread_mutex_t special_block_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── Internal helpers ─────────────────────────────────────────────── */

static inline uint8_t entry_is_empty(const SpecialBlockEntry *entry) {
    return entry->block == SPECIAL_BLOCK_EMPTY;
}

static inline void clear_entry(SpecialBlockEntry *entry) {
    entry->x = 0;
    entry->z = 0;
    entry->state = 0;
    entry->y = 0;
    entry->dimension = 0;
    entry->block = SPECIAL_BLOCK_EMPTY;
}

static inline uint32_t entry_hash(short x, uint8_t y, short z, uint8_t dimension) {
    uint32_t h = (uint16_t)x;
    h ^= ((uint32_t)(uint16_t)z << 1) | ((uint32_t)y << 17) | ((uint32_t)dimension << 25);
    h *= 0x9E3779B9u;
    h ^= h >> 16;
    return h;
}

static inline uint8_t entry_matches(const SpecialBlockEntry *entry, short x, uint8_t y, short z, uint8_t dimension) {
    return (
        entry->x == x &&
        entry->y == y &&
        entry->z == z &&
        entry->dimension == dimension
    );
}

/* Grow the hash table to the given capacity (must be a power of 2).
 * Scans the OLD table and manually hashes entries into the NEW table,
 * avoiding a call to insert_entry_locked (which would try to grow again). */
static int grow_blocks_locked(int new_cap) {
    SpecialBlockEntry *old = special_blocks;
    int old_cap = special_blocks_capacity;

    SpecialBlockEntry *new_tab = (SpecialBlockEntry *)calloc((size_t)new_cap, sizeof(SpecialBlockEntry));
    if (!new_tab) return -1;
    for (int i = 0; i < new_cap; i++) {
        new_tab[i].block = SPECIAL_BLOCK_EMPTY;
    }

    uint32_t nmask = (uint32_t)(new_cap - 1);
    int inserted = 0;

    if (old) {
        for (int i = 0; i < old_cap; i++) {
            if (old[i].block == SPECIAL_BLOCK_EMPTY) continue;
            uint32_t base = entry_hash(old[i].x, old[i].y, old[i].z, old[i].dimension) & nmask;
            for (uint32_t probe = 0; probe < (uint32_t)new_cap; probe++) {
                uint32_t idx = (base + probe) & nmask;
                if (new_tab[idx].block == SPECIAL_BLOCK_EMPTY) {
                    new_tab[idx] = old[i];
                    inserted++;
                    break;
                }
            }
        }
        free(old);
    }

    special_blocks = new_tab;
    special_blocks_capacity = new_cap;
    special_blocks_count = inserted;
    return 0;
}

/* Grow the wheat tracking list. */
static int grow_wheat_locked(void) {
    int new_cap = wheat_capacity == 0 ? INITIAL_WHEAT_CAPACITY : wheat_capacity * 2;
    WheatCoord *new_list = (WheatCoord *)realloc(wheat_coords, (size_t)new_cap * sizeof(WheatCoord));
    if (!new_list) return -1;
    wheat_coords = new_list;
    wheat_capacity = new_cap;
    return 0;
}

/* ── Hash table operations ────────────────────────────────────────── */

static int find_entry_locked(short x, uint8_t y, short z, uint8_t dimension) {
    int cap = special_blocks_capacity;
    if (cap == 0) return -1;
    uint32_t mask = (uint32_t)(cap - 1);
    uint32_t base = entry_hash(x, y, z, dimension) & mask;

    for (uint32_t probe = 0; probe < (uint32_t)cap; probe++) {
        uint32_t idx = (base + probe) & mask;
        if (entry_is_empty(&special_blocks[idx])) return -1;
        if (entry_matches(&special_blocks[idx], x, y, z, dimension)) return (int)idx;
    }
    return -1;
}

static int insert_entry_locked(short x, uint8_t y, short z, uint8_t dimension) {
    int cap = special_blocks_capacity;
    if (cap == 0) {
        /* First use: allocate initial table */
        if (grow_blocks_locked(INITIAL_BLOCK_CAPACITY) < 0) return -1;
        cap = special_blocks_capacity;
    } else if (special_blocks_count >= GROW_LIMIT(cap)) {
        /* Load factor exceeded — double the table */
        int new_cap = cap * 2;
        if (grow_blocks_locked(new_cap) < 0) return -1;
        cap = special_blocks_capacity;
    }

    uint32_t mask = (uint32_t)(cap - 1);
    uint32_t base = entry_hash(x, y, z, dimension) & mask;

    for (uint32_t probe = 0; probe < (uint32_t)cap; probe++) {
        uint32_t idx = (base + probe) & mask;
        if (entry_is_empty(&special_blocks[idx])) {
            special_blocks[idx].x = x;
            special_blocks[idx].y = y;
            special_blocks[idx].z = z;
            special_blocks[idx].dimension = dimension;
            special_blocks[idx].state = 0;
            special_blocks_count++;
            return (int)idx;
        }
        if (entry_matches(&special_blocks[idx], x, y, z, dimension)) return (int)idx;
    }
    return -1; /* Should never happen with dynamic growth */
}

static void reinsert_entry_locked(SpecialBlockEntry entry) {
    int idx = insert_entry_locked(entry.x, entry.y, entry.z, entry.dimension);
    if (idx < 0) return;
    special_blocks[idx] = entry;
}

/* ── Wheat tracking list helpers ──────────────────────────────────── */

static void wheat_list_add(short x, uint8_t y, short z, uint8_t dimension) {
    /* Dedup */
    for (int i = 0; i < wheat_count; i++) {
        if (wheat_coords[i].x == x && wheat_coords[i].y == y &&
            wheat_coords[i].z == z && wheat_coords[i].dimension == dimension) {
            return;
        }
    }
    /* Grow if needed */
    if (wheat_count >= wheat_capacity) {
        if (grow_wheat_locked() < 0) return;
    }
    wheat_coords[wheat_count].x = x;
    wheat_coords[wheat_count].y = y;
    wheat_coords[wheat_count].z = z;
    wheat_coords[wheat_count].dimension = dimension;
    wheat_count++;
}

static void wheat_list_remove(short x, uint8_t y, short z, uint8_t dimension) {
    for (int i = 0; i < wheat_count; i++) {
        if (wheat_coords[i].x == x && wheat_coords[i].y == y &&
            wheat_coords[i].z == z && wheat_coords[i].dimension == dimension) {
            wheat_coords[i] = wheat_coords[--wheat_count];
            return;
        }
    }
}

/* ── Public API ───────────────────────────────────────────────────── */

void special_block_init(void) {
    pthread_mutex_lock(&special_block_mutex);

    /* Free old arrays if re-initializing */
    if (special_blocks) free(special_blocks);
    if (wheat_coords) free(wheat_coords);

    special_blocks = NULL;
    special_blocks_count = 0;
    special_blocks_capacity = 0;
    wheat_coords = NULL;
    wheat_count = 0;
    wheat_capacity = 0;

    pthread_mutex_unlock(&special_block_mutex);
}

uint16_t special_block_get_state(short x, uint8_t y, short z, uint8_t dimension) {
    pthread_mutex_lock(&special_block_mutex);
    int idx = find_entry_locked(x, y, z, dimension);
    if (idx < 0) {
        pthread_mutex_unlock(&special_block_mutex);
        return 0;
    }
    uint16_t state = special_blocks[idx].state;
    pthread_mutex_unlock(&special_block_mutex);
    return state;
}

void special_block_set_state(short x, uint8_t y, short z, uint8_t dimension, uint16_t block, uint16_t state) {
    pthread_mutex_lock(&special_block_mutex);
    int idx = insert_entry_locked(x, y, z, dimension);
    if (idx < 0) {
        pthread_mutex_unlock(&special_block_mutex);
        return;
    }
    special_blocks[idx].state = state;
    special_blocks[idx].block = block;
    special_blocks[idx].dimension = dimension;

    /* Track wheat in dedicated list for fast growth iteration */
    if (block == B_wheat) {
        wheat_list_add(x, y, z, dimension);
    }

    pthread_mutex_unlock(&special_block_mutex);
}

void special_block_clear(short x, uint8_t y, short z, uint8_t dimension) {
    pthread_mutex_lock(&special_block_mutex);
    int idx = find_entry_locked(x, y, z, dimension);
    if (idx < 0) {
        pthread_mutex_unlock(&special_block_mutex);
        return;
    }

    /* Remove from wheat tracking list before clearing */
    if (special_blocks[idx].block == B_wheat) {
        wheat_list_remove(special_blocks[idx].x, special_blocks[idx].y,
                          special_blocks[idx].z, special_blocks[idx].dimension);
    }

    clear_entry(&special_blocks[idx]);
    special_blocks_count--;

    int cap = special_blocks_capacity;
    uint32_t mask = (uint32_t)(cap - 1);
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

uint8_t special_block_has_entry(short x, uint8_t y, short z, uint8_t dimension) {
    pthread_mutex_lock(&special_block_mutex);
    uint8_t has_entry = find_entry_locked(x, y, z, dimension) >= 0;
    pthread_mutex_unlock(&special_block_mutex);
    return has_entry;
}

/* ── Block type queries ───────────────────────────────────────────── */

uint8_t is_door_block(uint16_t block) {
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

uint8_t is_stair_block(uint16_t block) {
    return (
        block == B_oak_stairs ||
        block == B_spruce_stairs ||
        block == B_birch_stairs ||
        block == B_jungle_stairs ||
        block == B_acacia_stairs ||
        block == B_cherry_stairs ||
        block == B_dark_oak_stairs ||
        block == B_pale_oak_stairs ||
        block == B_mangrove_stairs ||
        block == B_cobblestone_stairs ||
        block == B_smooth_sandstone_stairs
    );
}

uint8_t is_slab_block(uint16_t block) {
    return (
        block == B_oak_slab ||
        block == B_spruce_slab ||
        block == B_birch_slab ||
        block == B_jungle_slab ||
        block == B_acacia_slab ||
        block == B_cherry_slab ||
        block == B_dark_oak_slab ||
        block == B_pale_oak_slab ||
        block == B_mangrove_slab ||
        block == B_cobblestone_slab ||
        block == B_smooth_sandstone_slab ||
        block == B_sandstone_slab
    );
}

uint8_t is_trapdoor_block(uint16_t block) {
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

uint8_t is_oriented_block(uint16_t block) {
    return (
        block == B_chest ||
        block == B_barrel
    );
}

uint8_t is_fence_block(uint16_t block) {
    return (
        block == B_oak_fence ||
        block == B_spruce_fence ||
        block == B_birch_fence ||
        block == B_jungle_fence ||
        block == B_acacia_fence ||
        block == B_cherry_fence ||
        block == B_dark_oak_fence ||
        block == B_pale_oak_fence ||
        block == B_mangrove_fence ||
        block == B_glass_pane
    );
}

uint8_t is_horizontal_facing_block(uint16_t block) {
    return (
        block == B_wall_torch ||
        block == B_lectern ||
        block == B_ladder
    );
}

uint8_t is_bed_block(uint16_t block) {
    return block >= B_white_bed && block <= B_black_bed;
}

/* ── State decode helpers ─────────────────────────────────────────── */

uint8_t door_get_open(uint16_t state) { return state & 1; }
uint8_t door_get_hinge(uint16_t state) { return (state >> 1) & 1; }
uint8_t door_get_direction(uint16_t state) { return (state >> 2) & 3; }
uint8_t trapdoor_get_open(uint16_t state) { return state & 1; }
uint8_t trapdoor_get_half(uint16_t state) { return (state >> 1) & 1; }
uint8_t trapdoor_get_direction(uint16_t state) { return (state >> 2) & 3; }
uint8_t stair_get_half(uint16_t state) { return state & 3; }
uint8_t stair_get_direction(uint16_t state) { return (state >> 2) & 3; }
uint8_t stair_get_shape(uint16_t state) { return (state >> 4) & 7; }
uint8_t slab_get_type(uint16_t state) { return state & 3; }
uint8_t oriented_get_direction(uint16_t state) { return state & 3; }
uint8_t furnace_get_direction(uint16_t state) { return state & 3; }
uint8_t furnace_get_lit(uint16_t state) { return (state >> 2) & 1; }
uint8_t barrel_get_direction(uint16_t state) { return state & 7; }
uint8_t barrel_get_open(uint16_t state) { return (state >> 3) & 1; }
uint8_t ender_chest_get_direction(uint16_t state) { return state & 3; }
uint8_t ender_chest_get_waterlogged(uint16_t state) { return (state >> 2) & 1; }
uint8_t fence_get_north(uint16_t state) { return state & 1; }
uint8_t fence_get_east(uint16_t state) { return (state >> 1) & 1; }
uint8_t fence_get_south(uint16_t state) { return (state >> 2) & 1; }
uint8_t fence_get_west(uint16_t state) { return (state >> 3) & 1; }
uint8_t bed_get_direction(uint16_t state) { return state & 3; }
uint8_t bed_get_head(uint16_t state) { return (state >> 2) & 1; }
uint8_t bed_get_occupied(uint16_t state) { return (state >> 3) & 1; }

/* ── State ID computation ─────────────────────────────────────────── */

uint16_t get_door_state_id(uint16_t block, uint8_t is_upper, uint8_t open, uint8_t direction, uint8_t hinge) {
    uint8_t row = door_block_to_row[block];
    if (row == 255) return block_palette[block];
    // Generated table layout: facing(n,s,w,e) × half(upper,lower) × hinge(left,right) × open(true,false)
    //   Index = facing*8 + half*4 + hinge*2 + open
    // Convert C params to table indices:
    //   facing (JS order): north=0, south=1, west=2, east=3
    //   half: 0=upper, 1=lower
    //   hinge: 0=left, 1=right
    //   open: 0=true, 1=false
    static const uint8_t tf[4] = {0, 3, 1, 2}; // Minecraft direction → table facing
    uint8_t idx = tf[direction] * 8 + (1 - is_upper) * 4 + hinge * 2 + (1 - open);
    return door_state_rows[row][idx];
}

uint16_t get_stair_state_id(uint16_t block, uint8_t half, uint8_t direction) {
    return get_stair_shape_state_id(block, half, direction, 0);
}

uint16_t get_stair_shape_state_id(uint16_t block, uint8_t half, uint8_t direction, uint8_t shape) {
    uint8_t row = stair_block_to_row[block];
    if (row == 255) return block_palette[block];
    static const uint8_t table_facing[4] = {0, 3, 1, 2};
    uint8_t tf = table_facing[direction & 3];
    uint8_t idx = (uint8_t)(tf * 10 + (half & 1) * 5 + (shape > 4 ? 0 : shape));
    return stair_state_rows[row][idx];
}

uint16_t get_slab_state_id(uint16_t block, uint8_t type) {
    uint8_t row = slab_block_to_row[block];
    if (row == 255) return block_palette[block];
    return slab_state_rows[row][type > 2 ? 0 : type];
}

uint16_t get_trapdoor_state_id(uint16_t block, uint8_t open, uint8_t direction, uint8_t half) {
    uint8_t row = trapdoor_block_to_row[block];
    if (row == 255) return block_palette[block];
    static const uint8_t table_facing[4] = {0, 3, 1, 2};
    uint8_t tf = table_facing[direction];
    uint8_t idx = tf * 4 + half * 2 + open;
    return trapdoor_state_rows[row][idx];
}

uint16_t get_oriented_state_id(uint16_t block, uint8_t direction) {
    uint16_t base_id = block_palette[block];
    if (block == B_chest) {
        static const uint16_t off[4] = {0, 6, 12, 18};
        return base_id + off[direction];
    } else if (block == B_furnace) {
        return base_id + (uint16_t)(direction * 2);
    } else if (block == B_barrel) {
        return base_id + (uint16_t)(direction * 2 + 1);
    }
    return base_id;
}

uint16_t get_furnace_state_id(uint8_t direction, uint8_t lit) {
    return block_palette[B_furnace] + (uint16_t)(direction * 2 + lit);
}

uint16_t get_fence_state_id(uint16_t block, uint8_t connections) {
    uint8_t row = fence_block_to_row[block];
    if (row == 255) return block_palette[block];
    return fence_state_rows[row][connections & 0xF];
}

uint16_t get_horizontal_state_id(uint16_t block, uint8_t direction) {
    uint8_t row = horizontal_block_to_row[block];
    if (row == 255) return block_palette[block];
    return horizontal_state_rows[row][direction & 3];
}

uint16_t get_bed_state_id(uint16_t block, uint8_t head, uint8_t occupied, uint8_t direction) {
    static const uint8_t table_facing[4] = {0, 3, 1, 2};
    uint8_t tf = table_facing[direction & 3];
    uint8_t idx = (uint8_t)(tf * 4 + (occupied ? 0 : 2) + (head ? 0 : 1));
    return (uint16_t)(block_palette[block] + idx - 3);
}

/* ── State encode helpers ─────────────────────────────────────────── */

uint16_t door_encode_state(uint8_t open, uint8_t hinge, uint8_t direction) {
    return (uint16_t)(open | (hinge << 1) | ((direction & 3) << 2));
}

uint16_t trapdoor_encode_state(uint8_t open, uint8_t half, uint8_t direction) {
    return (uint16_t)(open | ((half & 1) << 1) | ((direction & 3) << 2));
}

uint16_t stair_encode_state(uint8_t half, uint8_t direction) {
    return stair_shape_encode_state(half, direction, 0);
}

uint16_t stair_shape_encode_state(uint8_t half, uint8_t direction, uint8_t shape) {
    return (uint16_t)((half & 3) | ((direction & 3) << 2) | ((shape & 7) << 4));
}

uint16_t slab_encode_state(uint8_t type) {
    return (uint16_t)(type & 3);
}

uint16_t oriented_encode_state(uint8_t direction) {
    return (uint16_t)(direction & 3);
}

uint16_t furnace_encode_state(uint8_t direction, uint8_t lit) {
    return (uint16_t)((direction & 3) | ((lit & 1) << 2));
}

uint16_t barrel_encode_state(uint8_t direction, uint8_t open) {
    return (uint16_t)((direction & 7) | ((open & 1) << 3));
}

uint16_t ender_chest_encode_state(uint8_t direction, uint8_t waterlogged) {
    return (uint16_t)((direction & 3) | ((waterlogged & 1) << 2));
}

uint16_t fence_encode_state(uint8_t north, uint8_t east, uint8_t south, uint8_t west) {
    return (uint16_t)((north & 1) | ((east & 1) << 1) | ((south & 1) << 2) | ((west & 1) << 3));
}

uint16_t horizontal_facing_encode_state(uint8_t direction) {
    return (uint16_t)(direction & 3);
}

uint16_t bed_encode_state(uint8_t head, uint8_t occupied, uint8_t direction) {
    return (uint16_t)((direction & 3) | ((head & 1) << 2) | ((occupied & 1) << 3));
}

/* ── Interaction helpers ──────────────────────────────────────────── */

uint8_t is_door_open_at(short x, uint8_t y, short z, uint8_t dimension) {
    uint16_t state = special_block_get_state(x, y, z, dimension);
    return state & 1;
}

void toggle_door_state(short x, uint8_t y, short z, uint8_t dimension) {
    uint16_t state = special_block_get_state(x, y, z, dimension);
    state ^= 1; /* flip open bit */
    special_block_set_state(x, y, z, dimension, door_encode_state(state & 1, (state >> 1) & 1, (state >> 2) & 3), state);
    (void)dimension;
}
