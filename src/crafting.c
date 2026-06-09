#include <string.h>
#include <stdio.h>

#include "globals.h"
#include "registries.h"
#include "tools.h"
#include "crafting.h"
#include "config.h"

static uint16_t getSupportedBlockCraft(uint16_t item) {
  if (item == 0 || I_to_B(item) == 0) return 0;
  return item;
}

static uint8_t isBurnableLogOrWoodItem(uint16_t item) {
  switch (item) {
    case I_oak_log:
    case I_spruce_log:
    case I_birch_log:
    case I_jungle_log:
    case I_acacia_log:
    case I_cherry_log:
    case I_dark_oak_log:
    case I_pale_oak_log:
    case I_mangrove_log:
    case I_oak_wood:
    case I_spruce_wood:
    case I_birch_wood:
    case I_jungle_wood:
    case I_acacia_wood:
    case I_cherry_wood:
    case I_dark_oak_wood:
    case I_pale_oak_wood:
    case I_mangrove_wood:
      return 1;
    default:
      return 0;
  }
}

static uint8_t isBurnableSaplingItem(uint16_t item) {
  switch (item) {
    case I_oak_sapling:
    case I_spruce_sapling:
    case I_birch_sapling:
    case I_jungle_sapling:
    case I_acacia_sapling:
    case I_cherry_sapling:
    case I_dark_oak_sapling:
    case I_pale_oak_sapling:
    case I_mangrove_propagule:
      return 1;
    default:
      return 0;
  }
}

// Helper function to check if an item is any type of plank
uint8_t isPlankItem(uint16_t item) {
  switch (item) {
    case I_oak_planks:
    case I_spruce_planks:
    case I_birch_planks:
    case I_jungle_planks:
    case I_acacia_planks:
    case I_cherry_planks:
    case I_dark_oak_planks:
    case I_pale_oak_planks:
    case I_mangrove_planks:
    case I_bamboo_planks:
    case I_crimson_planks:
    case I_warped_planks:
      return 1;
    default:
      return 0;
  }
}

// Helper function to get slab item from plank item
uint16_t getSlabFromPlank(uint16_t plank) {
  uint16_t result = 0;
  switch (plank) {
    case I_oak_planks: result = I_oak_slab; break;
    case I_spruce_planks: result = I_spruce_slab; break;
    case I_birch_planks: result = I_birch_slab; break;
    case I_jungle_planks: result = I_jungle_slab; break;
    case I_acacia_planks: result = I_acacia_slab; break;
    case I_cherry_planks: result = I_cherry_slab; break;
    case I_dark_oak_planks: result = I_dark_oak_slab; break;
    case I_pale_oak_planks: result = I_pale_oak_slab; break;
    case I_mangrove_planks: result = I_mangrove_slab; break;
    case I_bamboo_planks: result = I_bamboo_slab; break;
    case I_crimson_planks: result = I_crimson_slab; break;
    case I_warped_planks: result = I_warped_slab; break;
    default: return 0;
  }
  return getSupportedBlockCraft(result);
}

// Helper function to get stair item from plank item
uint16_t getStairFromPlank(uint16_t plank) {
  uint16_t result = 0;
  switch (plank) {
    case I_oak_planks: result = I_oak_stairs; break;
    case I_spruce_planks: result = I_spruce_stairs; break;
    case I_birch_planks: result = I_birch_stairs; break;
    case I_jungle_planks: result = I_jungle_stairs; break;
    case I_acacia_planks: result = I_acacia_stairs; break;
    case I_cherry_planks: result = I_cherry_stairs; break;
    case I_dark_oak_planks: result = I_dark_oak_stairs; break;
    case I_pale_oak_planks: result = I_pale_oak_stairs; break;
    case I_mangrove_planks: result = I_mangrove_stairs; break;
    case I_bamboo_planks: result = I_bamboo_stairs; break;
    case I_crimson_planks: result = I_crimson_stairs; break;
    case I_warped_planks: result = I_warped_stairs; break;
    default: return 0;
  }
  return getSupportedBlockCraft(result);
}

// Helper function to get door item from plank item
uint16_t getDoorFromPlank(uint16_t plank) {
  uint16_t result = 0;
  switch (plank) {
    case I_oak_planks: result = I_oak_door; break;
    case I_spruce_planks: result = I_spruce_door; break;
    case I_birch_planks: result = I_birch_door; break;
    case I_jungle_planks: result = I_jungle_door; break;
    case I_acacia_planks: result = I_acacia_door; break;
    case I_cherry_planks: result = I_cherry_door; break;
    case I_dark_oak_planks: result = I_dark_oak_door; break;
    case I_pale_oak_planks: result = I_pale_oak_door; break;
    case I_mangrove_planks: result = I_mangrove_door; break;
    case I_bamboo_planks: result = I_bamboo_door; break;
    default: return 0;
  }
  return getSupportedBlockCraft(result);
}

// Helper function to get trapdoor item from plank item
uint16_t getTrapdoorFromPlank(uint16_t plank) {
  uint16_t result = 0;
  switch (plank) {
    case I_oak_planks: result = I_oak_trapdoor; break;
    case I_spruce_planks: result = I_spruce_trapdoor; break;
    case I_birch_planks: result = I_birch_trapdoor; break;
    case I_jungle_planks: result = I_jungle_trapdoor; break;
    case I_acacia_planks: result = I_acacia_trapdoor; break;
    case I_cherry_planks: result = I_cherry_trapdoor; break;
    case I_dark_oak_planks: result = I_dark_oak_trapdoor; break;
    case I_pale_oak_planks: result = I_pale_oak_trapdoor; break;
    case I_mangrove_planks: result = I_mangrove_trapdoor; break;
    default: return 0;
  }
  return getSupportedBlockCraft(result);
}

// Helper function to get fence item from plank item
uint16_t getFenceFromPlank(uint16_t plank) {
  uint16_t result = 0;
  switch (plank) {
    case I_oak_planks: result = I_oak_fence; break;
    case I_spruce_planks: result = I_spruce_fence; break;
    case I_birch_planks: result = I_birch_fence; break;
    case I_jungle_planks: result = I_jungle_fence; break;
    case I_acacia_planks: result = I_acacia_fence; break;
    case I_cherry_planks: result = I_cherry_fence; break;
    case I_dark_oak_planks: result = I_dark_oak_fence; break;
    case I_pale_oak_planks: result = I_pale_oak_fence; break;
    case I_mangrove_planks: result = I_mangrove_fence; break;
    case I_bamboo_planks: result = I_bamboo_fence; break;
    default: return 0;
  }
  return getSupportedBlockCraft(result);
}

// Helper function to get chest item from plank item
uint16_t getChestFromPlank(uint16_t plank) {
  (void)plank;
  return I_chest;
}

// Helper function to get crafting table item from plank item
uint16_t getCraftingTableFromPlank(uint16_t plank) {
  (void)plank;
  return I_crafting_table;
}

void getCraftingOutput (PlayerData *player, uint8_t *count, uint16_t *item) {

  // Exit early if craft_items has been locked
  if (player->flags & 0x80) {
    *count = 0;
    *item = 0;
    return;
  }

  uint8_t i, filled = 0, first = 10, identical = true;
  for (i = 0; i < 9; i ++) {
    if (player->craft_items[i]) {
      filled ++;
      if (first == 10) first = i;
      else if (player->craft_items[i] != player->craft_items[first]) {
        identical = false;
      }
    }
  }

  uint16_t first_item = player->craft_items[first];
  uint8_t first_col = first % 3, first_row = first / 3;

  switch (filled) {

    case 0:
      *item = 0;
      *count = 0;
      return;

    case 1:
      switch (first_item) {
        // Log to planks recipes
        case I_oak_log: *item = I_oak_planks; *count = 4; return;
        case I_spruce_log: *item = I_spruce_planks; *count = 4; return;
        case I_birch_log: *item = I_birch_planks; *count = 4; return;
        case I_jungle_log: *item = I_jungle_planks; *count = 4; return;
        case I_acacia_log: *item = I_acacia_planks; *count = 4; return;
        case I_cherry_log: *item = I_cherry_planks; *count = 4; return;
        case I_dark_oak_log: *item = I_dark_oak_planks; *count = 4; return;
        case I_pale_oak_log: *item = I_pale_oak_planks; *count = 4; return;
        case I_mangrove_log: *item = I_mangrove_planks; *count = 4; return;
        case I_bamboo_block: *item = I_bamboo_planks; *count = 4; return;
        case I_crimson_stem: *item = I_crimson_planks; *count = 4; return;
        case I_warped_stem: *item = I_warped_planks; *count = 4; return;
        // Block decomposition recipes
        case I_iron_block: *item = I_iron_ingot; *count = 9; return;
        case I_gold_block: *item = I_gold_ingot; *count = 9; return;
        case I_diamond_block: *item = I_diamond; *count = 9; return;
        case I_redstone_block: *item = I_redstone; *count = 9; return;
        case I_coal_block: *item = I_coal; *count = 9; return;
        case I_copper_block: *item = I_copper_ingot; *count = 9; return;

        default: break;
      }
      break;

    case 2:
      // Check for stick recipe (2 planks vertical)
      if (isPlankItem(first_item) && first_row != 2 && isPlankItem(player->craft_items[first + 3])) {
        *item = I_stick;
        *count = 4;
        return;
      }
      // Non-plank case 2 recipes
      switch (first_item) {
        case I_charcoal:
        case I_coal:
          if (first_row != 2 && player->craft_items[first + 3] == I_stick) {
            *item = I_torch;
            *count = 4;
            return;
          }
          break;
        case I_iron_ingot:
          if (
            (
              first_row != 2 && first_col != 2 &&
              player->craft_items[first + 4] == I_iron_ingot
            ) || (
              first_row != 2 && first_col != 0 &&
              player->craft_items[first + 2] == I_iron_ingot
            )
          ) {
            *item = I_shears;
            *count = 1;
            return;
          }
          // Flint and steel: iron ingot + flint (shapeless)
          if (!identical) {
            uint16_t other_item = 0;
            for (uint8_t j = 0; j < 9; j++) {
              if (j != first && player->craft_items[j]) {
                other_item = player->craft_items[j];
                break;
              }
            }
            if (other_item == I_flint) {
              *item = I_flint_and_steel;
              *count = 1;
              return;
            }
          }
          break;
        case I_flint:
          // Flint and steel: flint + iron ingot (shapeless)
          if (!identical) {
            uint16_t other_item = 0;
            for (uint8_t j = 0; j < 9; j++) {
              if (j != first && player->craft_items[j]) {
                other_item = player->craft_items[j];
                break;
              }
            }
            if (other_item == I_iron_ingot) {
              *item = I_flint_and_steel;
              *count = 1;
              return;
            }
          }
          break;

        default: break;
      }
      break;

    case 3:
      // Arrow recipe: flint above stick above feather
      if (
        first_item == I_flint &&
        first_row == 0 &&
        player->craft_items[first + 3] == I_stick &&
        player->craft_items[first + 6] == I_feather
      ) {
        *item = I_arrow;
        *count = 4;
        return;
      }
      // Bucket recipe: iron ingots in a V shape
      if (
        first_item == I_iron_ingot &&
        first_col == 0 &&
        first_row < 2 &&
        player->craft_items[first + 2] == I_iron_ingot &&
        player->craft_items[first + 4] == I_iron_ingot
      ) {
        *item = I_bucket;
        *count = 1;
        return;
      }
      // Bread: 3 wheat in a row
      if (first_item == I_wheat && first_col == 0 &&
          player->craft_items[first + 1] == I_wheat &&
          player->craft_items[first + 2] == I_wheat) {
        *item = I_bread;
        *count = 1;
        return;
      }
      // Slab recipes for all plank types (3 planks horizontal = 6 slabs)
      if (isPlankItem(first_item) && first_col == 0 &&
          player->craft_items[first + 1] == first_item &&
          player->craft_items[first + 2] == first_item) {
        uint16_t result = getSlabFromPlank(first_item);
        if (result != 0) {
          *item = result;
          *count = 6;
          return;
        }
      }
      // Shovel recipes for any planks
      if (isPlankItem(first_item) &&
          first_row == 0 &&
          player->craft_items[first + 3] == I_stick &&
          player->craft_items[first + 6] == I_stick) {
        *item = I_wooden_shovel;
        *count = 1;
        return;
      }
      // Sword recipes for any planks (check before the switch)
      if (isPlankItem(first_item) &&
          first_row == 0 &&
          player->craft_items[first + 3] == first_item &&
          player->craft_items[first + 6] == I_stick) {
        *item = I_wooden_sword;
        *count = 1;
        return;
      }
      switch (first_item) {
        case I_cobblestone:
        case I_stone:
        case I_snow_block:
          // Non-plank slab recipes
          if (
            first_col == 0 &&
            player->craft_items[first + 1] == first_item &&
            player->craft_items[first + 2] == first_item
          ) {
            if (first_item == I_cobblestone) *item = I_cobblestone_slab;
            else if (first_item == I_stone) *item = I_stone_slab;
            else if (first_item == I_snow_block) *item = I_snow;
            *count = 6;
            return;
          }
        case I_iron_ingot:
        case I_gold_ingot:
        case I_diamond:
        case I_netherite_ingot:
          // Shovel recipes
          if (
            first_row == 0 &&
            player->craft_items[first + 3] == I_stick &&
            player->craft_items[first + 6] == I_stick
          ) {
            if (isPlankItem(first_item)) *item = I_wooden_shovel;
            else if (first_item == I_cobblestone) *item = I_stone_shovel;
            else if (first_item == I_iron_ingot) *item = I_iron_shovel;
            else if (first_item == I_gold_ingot) *item = I_golden_shovel;
            else if (first_item == I_diamond) *item = I_diamond_shovel;
            else if (first_item == I_netherite_ingot) *item = I_netherite_shovel;
            *count = 1;
            return;
          }
          // Sword recipes
          if (
            first_row == 0 &&
            player->craft_items[first + 3] == first_item &&
            player->craft_items[first + 6] == I_stick
          ) {
            if (isPlankItem(first_item)) *item = I_wooden_sword;
            else if (first_item == I_cobblestone) *item = I_stone_sword;
            else if (first_item == I_iron_ingot) *item = I_iron_sword;
            else if (first_item == I_gold_ingot) *item = I_golden_sword;
            else if (first_item == I_diamond) *item = I_diamond_sword;
            else if (first_item == I_netherite_ingot) *item = I_netherite_sword;
            *count = 1;
            return;
          }
          break;

        default: break;
      }
      break;

    case 4:
      // 2x2 crafting table from any planks
      if (isPlankItem(first_item) &&
          first_col != 2 && first_row != 2 &&
          player->craft_items[first + 1] == first_item &&
          player->craft_items[first + 3] == first_item &&
          player->craft_items[first + 4] == first_item) {
        *item = getCraftingTableFromPlank(first_item);
        *count = 1;
        return;
      }
      switch (first_item) {
        case I_oak_log:
        case I_snowball:
          // Uniform 2x2 shaped recipes
          if (
            first_col != 2 && first_row != 2 &&
            player->craft_items[first + 1] == first_item &&
            player->craft_items[first + 3] == first_item &&
            player->craft_items[first + 4] == first_item
          ) {
            if (first_item == I_oak_log) { *item = I_oak_wood; *count = 3; }
            else if (first_item == I_snowball) { *item = I_snow_block; *count = 3; }
            return;
          }
          break;
        case I_leather:
        case I_iron_ingot:
        case I_gold_ingot:
        case I_diamond:
        case I_netherite_ingot:
          if (
            first_col == 0 && first_row < 2 &&
            player->craft_items[first + 2] == first_item &&
            player->craft_items[first + 3] == first_item &&
            player->craft_items[first + 5] == first_item
          ) {
            if (first_item == I_leather) *item = I_leather_boots;
            else if (first_item == I_iron_ingot) *item = I_iron_boots;
            else if (first_item == I_gold_ingot) *item = I_golden_boots;
            else if (first_item == I_diamond) *item = I_diamond_boots;
            else if (first_item == I_netherite_ingot) *item = I_netherite_boots;
            *count = 1;
            return;
          }
          break;

        default: break;
      }
      break;

    case 5:
      // Wooden pickaxe and axe from any planks (check before stone/metal)
      if (isPlankItem(first_item)) {
        // Pickaxe recipe
        if (
          first == 0 &&
          player->craft_items[first + 1] == first_item &&
          player->craft_items[first + 2] == first_item &&
          player->craft_items[first + 4] == I_stick &&
          player->craft_items[first + 7] == I_stick
        ) {
          *item = I_wooden_pickaxe;
          *count = 1;
          return;
        }
        // Axe recipe
        if (
          first < 2 &&
          player->craft_items[first + 1] == first_item &&
          ((
            player->craft_items[first + 3] == first_item &&
            player->craft_items[first + 4] == I_stick &&
            player->craft_items[first + 7] == I_stick
          ) || (
            player->craft_items[first + 4] == first_item &&
            player->craft_items[first + 3] == I_stick &&
            player->craft_items[first + 6] == I_stick
          ))
        ) {
          *item = I_wooden_axe;
          *count = 1;
          return;
        }
      }
      switch (first_item) {
        case I_cobblestone:
        case I_iron_ingot:
        case I_gold_ingot:
        case I_diamond:
        case I_netherite_ingot:
          // Pickaxe recipes
          if (
            first == 0 &&
            player->craft_items[first + 1] == first_item &&
            player->craft_items[first + 2] == first_item &&
            player->craft_items[first + 4] == I_stick &&
            player->craft_items[first + 7] == I_stick
          ) {
            if (first_item == I_cobblestone) *item = I_stone_pickaxe;
            else if (first_item == I_iron_ingot) *item = I_iron_pickaxe;
            else if (first_item == I_gold_ingot) *item = I_golden_pickaxe;
            else if (first_item == I_diamond) *item = I_diamond_pickaxe;
            else if (first_item == I_netherite_ingot) *item = I_netherite_pickaxe;
            *count = 1;
            return;
          }
          // Axe recipes
          if (
            first < 2 &&
            player->craft_items[first + 1] == first_item &&
            ((
              player->craft_items[first + 3] == first_item &&
              player->craft_items[first + 4] == I_stick &&
              player->craft_items[first + 7] == I_stick
            ) || (
              player->craft_items[first + 4] == first_item &&
              player->craft_items[first + 3] == I_stick &&
              player->craft_items[first + 6] == I_stick
            ))
          ) {
            if (first_item == I_cobblestone) *item = I_stone_axe;
            else if (first_item == I_iron_ingot) *item = I_iron_axe;
            else if (first_item == I_gold_ingot) *item = I_golden_axe;
            else if (first_item == I_diamond) *item = I_diamond_axe;
            else if (first_item == I_netherite_ingot) *item = I_netherite_axe;
            *count = 1;
            return;
          }
        case I_leather:
          // Helmet recipes
          if (
            first_col == 0 && first_row < 2 &&
            player->craft_items[first + 1] == first_item &&
            player->craft_items[first + 2] == first_item &&
            player->craft_items[first + 3] == first_item &&
            player->craft_items[first + 5] == first_item
          ) {
            if (first_item == I_leather) *item = I_leather_helmet;
            else if (first_item == I_iron_ingot) *item = I_iron_helmet;
            else if (first_item == I_gold_ingot) *item = I_golden_helmet;
            else if (first_item == I_diamond) *item = I_diamond_helmet;
            else if (first_item == I_netherite_ingot) *item = I_netherite_helmet;
            *count = 1;
            return;
          }
          break;

        default: break;
      }
      break;

    case 6:
      // Bow recipe: 3 sticks and 3 string in a curved shape (either direction)
      if (
        (
          player->craft_items[1] == I_stick &&
          player->craft_items[3] == I_stick &&
          player->craft_items[7] == I_stick &&
          player->craft_items[2] == I_string &&
          player->craft_items[5] == I_string &&
          player->craft_items[8] == I_string
        ) || (
          player->craft_items[1] == I_stick &&
          player->craft_items[5] == I_stick &&
          player->craft_items[7] == I_stick &&
          player->craft_items[0] == I_string &&
          player->craft_items[3] == I_string &&
          player->craft_items[6] == I_string
        )
      ) {
        *item = I_bow;
        *count = 1;
        return;
      }
      // Door recipes (2x3 pattern of planks) - all plank types
      if (config.allow_doors &&
          isPlankItem(first_item) && first_col == 0 && first_row == 0 &&
          player->craft_items[1] == first_item &&
          player->craft_items[3] == first_item &&
          player->craft_items[4] == first_item &&
          player->craft_items[6] == first_item &&
          player->craft_items[7] == first_item) {
        uint16_t result = getDoorFromPlank(first_item);
        if (result != 0) {
          *item = result;
          *count = 3;
          return;
        }
      }
      // Stair recipes (6 planks in stair pattern) - all plank types
      // Pattern 1: X.. / XX. / XXX (slots 0,3,4,6,7,8)
      if (isPlankItem(first_item) && first_col == 0 && first_row == 0 &&
          player->craft_items[3] == first_item &&
          player->craft_items[4] == first_item &&
          player->craft_items[6] == first_item &&
          player->craft_items[7] == first_item &&
          player->craft_items[8] == first_item) {
        uint16_t result = getStairFromPlank(first_item);
        if (result != 0) {
          *item = result;
          *count = 4;
          return;
        }
      }
      // Pattern 2: ..X / .XX / XXX (slots 2,4,5,6,7,8)
      if (isPlankItem(first_item) && first_col == 2 && first_row == 0 &&
          player->craft_items[4] == first_item &&
          player->craft_items[5] == first_item &&
          player->craft_items[6] == first_item &&
          player->craft_items[7] == first_item &&
          player->craft_items[8] == first_item) {
        uint16_t result = getStairFromPlank(first_item);
        if (result != 0) {
          *item = result;
          *count = 4;
          return;
        }
      }
      // Fence recipe (4 planks + 2 sticks)
      // Pattern: plank-stick-plank / plank-stick-plank
      // Slots: 0,2,3,5=planks, 1,4=sticks (6 items, rows 0-1 filled)
      if (
        first_col == 0 && first_row == 0 &&
        isPlankItem(first_item) &&
        player->craft_items[2] == first_item &&
        player->craft_items[3] == first_item &&
        player->craft_items[5] == first_item &&
        player->craft_items[1] == I_stick &&
        player->craft_items[4] == I_stick
      ) {
        uint16_t result = getFenceFromPlank(first_item);
        if (result != 0) {
          *item = result;
          *count = 3;
          return;
        }
      }
      // Trapdoor recipe (6 planks, 3×2 pattern — full first 2 rows)
      if (config.allow_doors &&
          isPlankItem(first_item) && first_col == 0 && first_row == 0 &&
          player->craft_items[1] == first_item &&
          player->craft_items[2] == first_item &&
          player->craft_items[3] == first_item &&
          player->craft_items[4] == first_item &&
          player->craft_items[5] == first_item) {
        uint16_t result = getTrapdoorFromPlank(first_item);
        if (result != 0) {
          *item = result;
          *count = 2;
          return;
        }
      }
      // Cobblestone stairs (6 cobblestone in stair pattern)
      // Pattern 1: X.. / XX. / XXX (slots 0,3,4,6,7,8)
      if (first_item == I_cobblestone && first_col == 0 && first_row == 0 &&
          player->craft_items[3] == first_item &&
          player->craft_items[4] == first_item &&
          player->craft_items[6] == first_item &&
          player->craft_items[7] == first_item &&
          player->craft_items[8] == first_item) {
        *item = I_cobblestone_stairs;
        *count = 4;
        return;
      }
      // Pattern 2: ..X / .XX / XXX (slots 2,4,5,6,7,8)
      if (first_item == I_cobblestone && first_col == 2 && first_row == 0 &&
          player->craft_items[4] == first_item &&
          player->craft_items[5] == first_item &&
          player->craft_items[6] == first_item &&
          player->craft_items[7] == first_item &&
          player->craft_items[8] == first_item) {
        *item = I_cobblestone_stairs;
        *count = 4;
        return;
      }
      break;

    case 7:
      // Legging recipes
      if (identical && player->craft_items[4] == 0 && player->craft_items[7] == 0) {
        switch (first_item) {
          case I_leather: *item = I_leather_leggings; *count = 1; return;
          case I_iron_ingot: *item = I_iron_leggings; *count = 1; return;
          case I_gold_ingot: *item = I_golden_leggings; *count = 1; return;
          case I_diamond: *item = I_diamond_leggings; *count = 1; return;
          case I_netherite_ingot: *item = I_netherite_leggings; *count = 1; return;
          default: break;
        }
      }
      switch (first_item) {
        case I_oak_slab:
          if (
            identical &&
            player->craft_items[1] == 0 &&
            player->craft_items[4] == 0
          ) {
            *item = I_composter;
            *count = 1;
            return;
          }
          break;

        default: break;
      }
      break;

    case 8:
      if (identical) {
        if (player->craft_items[4] == 0) {
          // Center slot clear
          switch (first_item) {
            case I_cobblestone: *item = I_furnace; *count = 1; return;
            #ifdef ALLOW_CHESTS
            default:
              if (config.allow_chests && isPlankItem(first_item)) {
                *item = getChestFromPlank(first_item);
                *count = 1;
                return;
              }
              break;
            #endif
          }
        } else if (player->craft_items[1] == 0) {
          // Top-middle slot clear (chestplate recipes)
          switch (first_item) {
            case I_leather: *item = I_leather_chestplate; *count = 1; return;
            case I_iron_ingot: *item = I_iron_chestplate; *count = 1; return;
            case I_gold_ingot: *item = I_golden_chestplate; *count = 1; return;
            case I_diamond: *item = I_diamond_chestplate; *count = 1; return;
            case I_netherite_ingot: *item = I_netherite_chestplate; *count = 1; return;
            default: break;
          }
        }
      }
      break;

    case 9:
      // Uniform 3x3 shaped recipes
      if (identical) switch (first_item) {
        case I_iron_ingot: *item = I_iron_block; *count = 1; return;
        case I_gold_ingot: *item = I_gold_block; *count = 1; return;
        case I_diamond: *item = I_diamond_block; *count = 1; return;
        case I_redstone: *item = I_redstone_block; *count = 1; return;
        case I_coal: *item = I_coal_block; *count = 1; return;
        case I_copper_ingot: *item = I_copper_block; *count = 1; return;
        default: break;
      }
      break;

    default: break;

  }

  *count = 0;
  *item = 0;

}

#define registerSmeltingRecipe(a, b) \
  if (*material == a && (*output_item == b || *output_item == 0)) *output_item = b

void getSmeltingOutput (PlayerData *player) {

  uint8_t *material_count = &player->craft_count[0];
  uint8_t *fuel_count = &player->craft_count[1];

  // Don't process if we're missing material or fuel
  if (*material_count == 0 || *fuel_count == 0) return;

  uint16_t *material = &player->craft_items[0];
  uint16_t *fuel = &player->craft_items[1];

  // Don't process if we're missing material or fuel
  if (*material == 0 || *fuel == 0) return;

  // Furnace output is 3rd crafting table slot
  uint8_t *output_count = &player->craft_count[2];
  uint16_t *output_item = &player->craft_items[2];

  // Determine fuel efficiency based on the type of item
  // Since we can't represent fractions, some items use a random component
  // to represent the fractional part. In some cases (e.g. sticks), this
  // can lead to a fuel_value of 0, which means that the fuel gets consumed
  // without processing any materials.
  uint8_t fuel_value = 0;
  if (*fuel == I_coal) fuel_value = 8;
  else if (*fuel == I_charcoal) fuel_value = 8;
  else if (*fuel == I_coal_block) fuel_value = 80;
  else if ((isPlankItem(*fuel) && *fuel != I_crimson_planks && *fuel != I_warped_planks) || *fuel == I_bamboo_block) fuel_value = 1 + (fast_rand() & 1);
  else if (isBurnableLogOrWoodItem(*fuel)) fuel_value = 1 + (fast_rand() & 1);
  else if (*fuel == I_crafting_table) fuel_value = 1 + (fast_rand() & 1);
  else if (*fuel == I_stick) fuel_value = (fast_rand() & 1);
  else if (isBurnableSaplingItem(*fuel)) fuel_value = (fast_rand() & 1);
  else if (*fuel == I_wooden_axe) fuel_value = 1;
  else if (*fuel == I_wooden_pickaxe) fuel_value = 1;
  else if (*fuel == I_wooden_shovel) fuel_value = 1;
  else if (*fuel == I_wooden_sword) fuel_value = 1;
  else if (*fuel == I_wooden_hoe) fuel_value = 1;
  else return;

  uint8_t exchange = *material_count > fuel_value ? fuel_value : *material_count;

  registerSmeltingRecipe(I_cobblestone, I_stone);
  else registerSmeltingRecipe(I_oak_log, I_charcoal);
  else registerSmeltingRecipe(I_spruce_log, I_charcoal);
  else registerSmeltingRecipe(I_birch_log, I_charcoal);
  else registerSmeltingRecipe(I_jungle_log, I_charcoal);
  else registerSmeltingRecipe(I_acacia_log, I_charcoal);
  else registerSmeltingRecipe(I_cherry_log, I_charcoal);
  else registerSmeltingRecipe(I_dark_oak_log, I_charcoal);
  else registerSmeltingRecipe(I_pale_oak_log, I_charcoal);
  else registerSmeltingRecipe(I_mangrove_log, I_charcoal);
  else registerSmeltingRecipe(I_oak_wood, I_charcoal);
  else registerSmeltingRecipe(I_spruce_wood, I_charcoal);
  else registerSmeltingRecipe(I_birch_wood, I_charcoal);
  else registerSmeltingRecipe(I_jungle_wood, I_charcoal);
  else registerSmeltingRecipe(I_acacia_wood, I_charcoal);
  else registerSmeltingRecipe(I_cherry_wood, I_charcoal);
  else registerSmeltingRecipe(I_dark_oak_wood, I_charcoal);
  else registerSmeltingRecipe(I_pale_oak_wood, I_charcoal);
  else registerSmeltingRecipe(I_mangrove_wood, I_charcoal);
  else registerSmeltingRecipe(I_raw_iron, I_iron_ingot);
  else registerSmeltingRecipe(I_raw_gold, I_gold_ingot);
  else registerSmeltingRecipe(I_raw_copper, I_copper_ingot);
  else registerSmeltingRecipe(I_ancient_debris, I_netherite_scrap);
  else registerSmeltingRecipe(I_sand, I_glass);
  else registerSmeltingRecipe(I_red_sand, I_glass);
  else registerSmeltingRecipe(I_netherrack, I_nether_brick);
  else registerSmeltingRecipe(I_cactus, I_green_dye);
  else registerSmeltingRecipe(I_chicken, I_cooked_chicken);
  else registerSmeltingRecipe(I_beef, I_cooked_beef);
  else registerSmeltingRecipe(I_porkchop, I_cooked_porkchop);
  else registerSmeltingRecipe(I_mutton, I_cooked_mutton);
  else registerSmeltingRecipe(I_cod, I_cooked_cod);
  else registerSmeltingRecipe(I_salmon, I_cooked_salmon);
  else registerSmeltingRecipe(I_rabbit, I_cooked_rabbit);
  else registerSmeltingRecipe(I_potato, I_baked_potato);
  else return;

  *output_count += exchange;
  *material_count -= exchange;

  *fuel_count -= 1;
  if (*fuel_count == 0) *fuel = 0;

  if (*material_count <= 0) {
    *material_count = 0;
    *material = 0;
  } else return getSmeltingOutput(player);

  return;

}
