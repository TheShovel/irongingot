#include <string.h>
#include <stdio.h>

#include "globals.h"
#include "registries.h"
#include "tools.h"
#include "crafting.h"

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
  switch (plank) {
    case I_oak_planks: return I_oak_slab;
    case I_spruce_planks: return I_spruce_slab;
    case I_birch_planks: return I_birch_slab;
    default: return I_oak_slab;  // Fallback for unavailable types
  }
}

// Helper function to get stair item from plank item
uint16_t getStairFromPlank(uint16_t plank) {
  switch (plank) {
    case I_oak_planks: return I_oak_stairs;
    case I_spruce_planks: return I_spruce_stairs;
    case I_birch_planks: return I_birch_stairs;
    default: return I_oak_stairs;  // Fallback for unavailable types
  }
}

// Helper function to get door item from plank item
uint16_t getDoorFromPlank(uint16_t plank) {
  switch (plank) {
    case I_oak_planks: return I_oak_door;
    case I_spruce_planks: return I_spruce_door;
    case I_birch_planks: return I_birch_door;
    case I_jungle_planks: return I_jungle_door;
    case I_acacia_planks: return I_acacia_door;
    case I_cherry_planks: return I_cherry_door;
    case I_dark_oak_planks: return I_dark_oak_door;
    case I_pale_oak_planks: return I_pale_oak_door;
    case I_mangrove_planks: return I_mangrove_door;
    case I_bamboo_planks: return I_bamboo_door;
    default: return I_oak_door;
  }
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
          break;

        default: break;
      }
      break;

    case 3:
      // Slab recipes for all plank types (3 planks horizontal = 6 slabs)
      if (isPlankItem(first_item) && first_col == 0 &&
          player->craft_items[first + 1] == first_item &&
          player->craft_items[first + 2] == first_item) {
        *item = getSlabFromPlank(first_item);
        *count = 6;
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
      // Door recipes (2x3 pattern of planks) - all plank types
      if (isPlankItem(first_item) && first_col == 0 && first_row == 0 &&
          player->craft_items[1] == first_item &&
          player->craft_items[3] == first_item &&
          player->craft_items[4] == first_item &&
          player->craft_items[6] == first_item &&
          player->craft_items[7] == first_item) {
        *item = getDoorFromPlank(first_item);
        *count = 3;
        return;
      }
      // Stair recipes (6 planks in stair pattern) - all plank types
      // Pattern 1: X.. / XX. / XXX (slots 0,3,4,6,7,8)
      if (isPlankItem(first_item) && first_col == 0 && first_row == 0 &&
          player->craft_items[3] == first_item &&
          player->craft_items[4] == first_item &&
          player->craft_items[6] == first_item &&
          player->craft_items[7] == first_item &&
          player->craft_items[8] == first_item) {
        *item = getStairFromPlank(first_item);
        *count = 4;
        return;
      }
      // Pattern 2: ..X / .XX / XXX (slots 2,4,5,6,7,8)
      if (isPlankItem(first_item) && first_col == 2 && first_row == 0 &&
          player->craft_items[4] == first_item &&
          player->craft_items[5] == first_item &&
          player->craft_items[6] == first_item &&
          player->craft_items[7] == first_item &&
          player->craft_items[8] == first_item) {
        *item = getStairFromPlank(first_item);
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
              if (isPlankItem(first_item)) {
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
  else if (*fuel == I_oak_planks) fuel_value = 1 + (fast_rand() & 1);
  else if (*fuel == I_oak_log) fuel_value = 1 + (fast_rand() & 1);
  else if (*fuel == I_crafting_table) fuel_value = 1 + (fast_rand() & 1);
  else if (*fuel == I_stick) fuel_value = (fast_rand() & 1);
  else if (*fuel == I_oak_sapling) fuel_value = (fast_rand() & 1);
  else if (*fuel == I_wooden_axe) fuel_value = 1;
  else if (*fuel == I_wooden_pickaxe) fuel_value = 1;
  else if (*fuel == I_wooden_shovel) fuel_value = 1;
  else if (*fuel == I_wooden_sword) fuel_value = 1;
  else if (*fuel == I_wooden_hoe) fuel_value = 1;
  else return;

  uint8_t exchange = *material_count > fuel_value ? fuel_value : *material_count;

  registerSmeltingRecipe(I_cobblestone, I_stone);
  else registerSmeltingRecipe(I_oak_log, I_charcoal);
  else registerSmeltingRecipe(I_oak_wood, I_charcoal);
  else registerSmeltingRecipe(I_raw_iron, I_iron_ingot);
  else registerSmeltingRecipe(I_raw_gold, I_gold_ingot);
  else registerSmeltingRecipe(I_sand, I_glass);
  else registerSmeltingRecipe(I_chicken, I_cooked_chicken);
  else registerSmeltingRecipe(I_beef, I_cooked_beef);
  else registerSmeltingRecipe(I_porkchop, I_cooked_porkchop);
  else registerSmeltingRecipe(I_mutton, I_cooked_mutton);
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
