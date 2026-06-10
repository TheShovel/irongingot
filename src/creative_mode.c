#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "creative_mode.h"
#include "registries.h"
#include "procedures.h"
#include "packets.h"
#include "config.h"
#include "tools.h"

// Creative UI state for all players
CreativeUIState creative_ui_states[MAX_PLAYERS];

// All available items and blocks in the game
// Using ONLY items that actually exist in registries.h
static const CreativeItem creative_items[] = {
  // Building Blocks - Stone
  {I_stone, "Stone"},
  {I_granite, "Granite"},
  {I_polished_granite, "Polished Granite"},
  {I_diorite, "Diorite"},
  {I_polished_diorite, "Polished Diorite"},
  {I_andesite, "Andesite"},
  {I_polished_andesite, "Polished Andesite"},
  {I_cobblestone, "Cobblestone"},
  
  // Building Blocks - Dirt & Grass
  {I_grass_block, "Grass Block"},
  {I_dirt, "Dirt"},
  {I_coarse_dirt, "Coarse Dirt"},
  {I_sand, "Sand"},
  {I_red_sand, "Red Sand"},
  {I_gravel, "Gravel"},
  {I_mud, "Mud"},
  
  // Wood - Oak
  {I_oak_log, "Oak Log"},
  {I_oak_wood, "Oak Wood"},
  {I_oak_planks, "Oak Planks"},
  {I_oak_sapling, "Oak Sapling"},
  {I_oak_leaves, "Oak Leaves"},
  
  // Wood - Spruce
  {I_spruce_log, "Spruce Log"},
  {I_spruce_wood, "Spruce Wood"},
  {I_spruce_planks, "Spruce Planks"},
  {I_spruce_sapling, "Spruce Sapling"},
  {I_spruce_leaves, "Spruce Leaves"},
  
  // Wood - Birch
  {I_birch_log, "Birch Log"},
  {I_birch_wood, "Birch Wood"},
  {I_birch_planks, "Birch Planks"},
  {I_birch_sapling, "Birch Sapling"},
  {I_birch_leaves, "Birch Leaves"},
  
  // Wood - Jungle
  {I_jungle_log, "Jungle Log"},
  {I_jungle_wood, "Jungle Wood"},
  {I_jungle_planks, "Jungle Planks"},
  {I_jungle_sapling, "Jungle Sapling"},
  {I_jungle_leaves, "Jungle Leaves"},
  
  // Wood - Acacia
  {I_acacia_log, "Acacia Log"},
  {I_acacia_wood, "Acacia Wood"},
  {I_acacia_planks, "Acacia Planks"},
  {I_acacia_sapling, "Acacia Sapling"},
  {I_acacia_leaves, "Acacia Leaves"},
  
  // Wood - Dark Oak
  {I_dark_oak_log, "Dark Oak Log"},
  {I_dark_oak_wood, "Dark Oak Wood"},
  {I_dark_oak_planks, "Dark Oak Planks"},
  {I_dark_oak_sapling, "Dark Oak Sapling"},
  {I_dark_oak_leaves, "Dark Oak Leaves"},
  
  // Wood - Mangrove
  {I_mangrove_log, "Mangrove Log"},
  {I_mangrove_wood, "Mangrove Wood"},
  {I_mangrove_planks, "Mangrove Planks"},
  {I_mangrove_leaves, "Mangrove Leaves"},
  
  // Wood - Cherry
  {I_cherry_log, "Cherry Log"},
  {I_cherry_wood, "Cherry Wood"},
  {I_cherry_planks, "Cherry Planks"},
  {I_cherry_sapling, "Cherry Sapling"},
  {I_cherry_leaves, "Cherry Leaves"},
  
  // Bamboo
  {I_bamboo_block, "Bamboo Block"},
  {I_bamboo_planks, "Bamboo Planks"},
  {I_bamboo_mosaic, "Bamboo Mosaic"},
  
  // Stairs - Oak
  {I_oak_stairs, "Oak Stairs"},
  {I_spruce_stairs, "Spruce Stairs"},
  {I_birch_stairs, "Birch Stairs"},
  {I_jungle_stairs, "Jungle Stairs"},
  {I_acacia_stairs, "Acacia Stairs"},
  {I_dark_oak_stairs, "Dark Oak Stairs"},
  {I_mangrove_stairs, "Mangrove Stairs"},
  {I_cherry_stairs, "Cherry Stairs"},
  {I_pale_oak_stairs, "Pale Oak Stairs"},
  {I_cobblestone_stairs, "Cobblestone Stairs"},
  
  // Slabs
  {I_oak_slab, "Oak Slab"},
  {I_spruce_slab, "Spruce Slab"},
  {I_birch_slab, "Birch Slab"},
  {I_jungle_slab, "Jungle Slab"},
  {I_acacia_slab, "Acacia Slab"},
  {I_dark_oak_slab, "Dark Oak Slab"},
  {I_mangrove_slab, "Mangrove Slab"},
  {I_cherry_slab, "Cherry Slab"},
  {I_pale_oak_slab, "Pale Oak Slab"},
  {I_cobblestone_slab, "Cobblestone Slab"},
  
  // Doors
  {I_oak_door, "Oak Door"},
  {I_spruce_door, "Spruce Door"},
  {I_birch_door, "Birch Door"},
  {I_jungle_door, "Jungle Door"},
  {I_acacia_door, "Acacia Door"},
  {I_dark_oak_door, "Dark Oak Door"},
  {I_mangrove_door, "Mangrove Door"},
  {I_cherry_door, "Cherry Door"},
  {I_pale_oak_door, "Pale Oak Door"},
  
  // Trapdoors
  {I_oak_trapdoor, "Oak Trapdoor"},
  {I_spruce_trapdoor, "Spruce Trapdoor"},
  {I_birch_trapdoor, "Birch Trapdoor"},
  {I_jungle_trapdoor, "Jungle Trapdoor"},
  {I_acacia_trapdoor, "Acacia Trapdoor"},
  {I_dark_oak_trapdoor, "Dark Oak Trapdoor"},
  {I_mangrove_trapdoor, "Mangrove Trapdoor"},
  {I_cherry_trapdoor, "Cherry Trapdoor"},
  {I_pale_oak_trapdoor, "Pale Oak Trapdoor"},
  
  // Fences
  {I_oak_fence, "Oak Fence"},
  {I_spruce_fence, "Spruce Fence"},
  {I_birch_fence, "Birch Fence"},
  {I_jungle_fence, "Jungle Fence"},
  {I_acacia_fence, "Acacia Fence"},
  {I_dark_oak_fence, "Dark Oak Fence"},
  {I_mangrove_fence, "Mangrove Fence"},
  {I_cherry_fence, "Cherry Fence"},
  {I_pale_oak_fence, "Pale Oak Fence"},
  
  // Decorative & Flowers
  {I_torch, "Torch"},
  {I_dandelion, "Dandelion"},
  {I_poppy, "Poppy"},
  {I_blue_orchid, "Blue Orchid"},
  {I_allium, "Allium"},
  {I_azure_bluet, "Azure Bluet"},
  {I_red_tulip, "Red Tulip"},
  {I_orange_tulip, "Orange Tulip"},
  {I_white_tulip, "White Tulip"},
  {I_pink_tulip, "Pink Tulip"},
  {I_oxeye_daisy, "Oxeye Daisy"},
  {I_cornflower, "Cornflower"},
  {I_wither_rose, "Wither Rose"},
  {I_lily_of_the_valley, "Lily of the Valley"},
  
  // Ores
  {I_coal_ore, "Coal Ore"},
  {I_iron_ore, "Iron Ore"},
  {I_gold_ore, "Gold Ore"},
  {I_diamond_ore, "Diamond Ore"},
  {I_redstone_ore, "Redstone Ore"},
  {I_lapis_ore, "Lapis Ore"},
  {I_emerald_ore, "Emerald Ore"},
  {I_copper_ore, "Copper Ore"},
  
  // Mineral Blocks
  {I_iron_block, "Iron Block"},
  {I_gold_block, "Gold Block"},
  {I_diamond_block, "Diamond Block"},
  {I_copper_block, "Copper Block"},
  {I_coal_block, "Coal Block"},
  {I_lapis_block, "Lapis Block"},
  {I_redstone_block, "Redstone Block"},
  
  // Colored Wool
  {I_white_wool, "White Wool"},
  {I_orange_wool, "Orange Wool"},
  {I_magenta_wool, "Magenta Wool"},
  {I_light_blue_wool, "Light Blue Wool"},
  {I_yellow_wool, "Yellow Wool"},
  {I_lime_wool, "Lime Wool"},
  {I_pink_wool, "Pink Wool"},
  {I_gray_wool, "Gray Wool"},
  {I_light_gray_wool, "Light Gray Wool"},
  {I_cyan_wool, "Cyan Wool"},
  {I_purple_wool, "Purple Wool"},
  {I_blue_wool, "Blue Wool"},
  
  // Other Building Blocks
  {I_glass, "Glass"},
  {I_crafting_table, "Crafting Table"},
  {I_furnace, "Furnace"},
  {I_chest, "Chest"},
  {I_bedrock, "Bedrock"},
  {I_snow_block, "Snow Block"},
  {I_ice, "Ice"},
  {I_packed_ice, "Packed Ice"},
  {I_blue_ice, "Blue Ice"},
  {I_cactus, "Cactus"},
  {I_obsidian, "Obsidian"},
  {I_netherrack, "Netherrack"},
  {I_soul_sand, "Soul Sand"},
  {I_soul_soil, "Soul Soil"},
  {I_nether_bricks, "Nether Bricks"},
  {I_magma_block, "Magma Block"},
  {I_glowstone, "Glowstone"},
  {I_basalt, "Basalt"},
  {I_blackstone, "Blackstone"},
  
  // Tools - Pickaxes
  {I_wooden_pickaxe, "Wooden Pickaxe"},
  {I_stone_pickaxe, "Stone Pickaxe"},
  {I_iron_pickaxe, "Iron Pickaxe"},
  {I_golden_pickaxe, "Golden Pickaxe"},
  {I_diamond_pickaxe, "Diamond Pickaxe"},
  {I_netherite_pickaxe, "Netherite Pickaxe"},
  
  // Tools - Axes
  {I_wooden_axe, "Wooden Axe"},
  {I_stone_axe, "Stone Axe"},
  {I_iron_axe, "Iron Axe"},
  {I_golden_axe, "Golden Axe"},
  {I_diamond_axe, "Diamond Axe"},
  {I_netherite_axe, "Netherite Axe"},
  
  // Tools - Shovels
  {I_wooden_shovel, "Wooden Shovel"},
  {I_stone_shovel, "Stone Shovel"},
  {I_iron_shovel, "Iron Shovel"},
  {I_golden_shovel, "Golden Shovel"},
  {I_diamond_shovel, "Diamond Shovel"},
  {I_netherite_shovel, "Netherite Shovel"},
  
  // Tools - Swords
  {I_wooden_sword, "Wooden Sword"},
  {I_stone_sword, "Stone Sword"},
  {I_iron_sword, "Iron Sword"},
  {I_golden_sword, "Golden Sword"},
  {I_diamond_sword, "Diamond Sword"},
  {I_netherite_sword, "Netherite Sword"},
  
  // Tools - Hoes
  {I_wooden_hoe, "Wooden Hoe"},
  {I_stone_hoe, "Stone Hoe"},
  {I_iron_hoe, "Iron Hoe"},
  {I_golden_hoe, "Golden Hoe"},
  {I_diamond_hoe, "Diamond Hoe"},
  {I_netherite_hoe, "Netherite Hoe"},
  
  // Tools - Other
  {I_shears, "Shears"},
  {I_fishing_rod, "Fishing Rod"},
  {I_bow, "Bow"},
  {I_shield, "Shield"},
  {I_flint_and_steel, "Flint and Steel"},
  {I_bucket, "Bucket"},
  {I_water_bucket, "Water Bucket"},
  {I_lava_bucket, "Lava Bucket"},
  
  // Resources
  {I_coal, "Coal"},
  {I_wheat, "Wheat"},
  {I_wheat_seeds, "Wheat Seeds"},
  {I_charcoal, "Charcoal"},
  {I_diamond, "Diamond"},
  {I_emerald, "Emerald"},
  {I_lapis_lazuli, "Lapis Lazuli"},
  {I_redstone, "Redstone"},
  {I_copper_ingot, "Copper Ingot"},
  {I_iron_ingot, "Iron Ingot"},
  {I_gold_ingot, "Gold Ingot"},
  {I_netherite_ingot, "Netherite Ingot"},
  {I_netherite_scrap, "Netherite Scrap"},
  {I_stick, "Stick"},
  {I_string, "String"},
  {I_flint, "Flint"},
  {I_bone, "Bone"},
  {I_bone_meal, "Bone Meal"},
  
  // Food
  {I_apple, "Apple"},
  {I_bread, "Bread"},
  {I_porkchop, "Porkchop"},
  {I_cooked_porkchop, "Cooked Porkchop"},
  {I_beef, "Beef"},
  {I_cooked_beef, "Cooked Beef"},
  {I_chicken, "Chicken"},
  {I_cooked_chicken, "Cooked Chicken"},
  {I_mutton, "Mutton"},
  {I_cooked_mutton, "Cooked Mutton"},
  {I_rabbit, "Rabbit"},
  {I_cooked_rabbit, "Cooked Rabbit"},
  {I_cod, "Cod"},
  {I_cooked_cod, "Cooked Cod"},
  {I_salmon, "Salmon"},
  {I_cooked_salmon, "Cooked Salmon"},
  {I_tropical_fish, "Tropical Fish"},
  {I_pufferfish, "Pufferfish"},
  {I_carrot, "Carrot"},
  {I_golden_carrot, "Golden Carrot"},
  {I_potato, "Potato"},
  {I_baked_potato, "Baked Potato"},
  {I_poisonous_potato, "Poisonous Potato"},
  {I_beetroot, "Beetroot"},
  {I_melon_slice, "Melon Slice"},
  {I_pumpkin_pie, "Pumpkin Pie"},
  {I_cookie, "Cookie"},
  {I_cake, "Cake"},
  {I_mushroom_stew, "Mushroom Stew"},
  {I_beetroot_soup, "Beetroot Soup"},
  {I_rabbit_stew, "Rabbit Stew"},
  
  // Misc Items
  {I_paper, "Paper"},
  {I_book, "Book"},
  {I_leather, "Leather"},
  {I_feather, "Feather"},
  {I_egg, "Egg"},
  {I_snowball, "Snowball"},
  {I_ender_pearl, "Ender Pearl"},
  {I_ender_eye, "Ender Eye"},
  {I_arrow, "Arrow"},
  {I_gunpowder, "Gunpowder"},
  {I_slime_ball, "Slime Ball"},
  {I_spider_eye, "Spider Eye"},
  {I_fermented_spider_eye, "Fermented Spider Eye"},
  {I_blaze_rod, "Blaze Rod"},
  {I_blaze_powder, "Blaze Powder"},
  {I_ghast_tear, "Ghast Tear"},
  {I_nether_star, "Nether Star"},
  {I_nether_wart, "Nether Wart"},
  {I_enderman_spawn_egg, "Enderman Spawn Egg"},
};

#define CREATIVE_ITEM_COUNT (sizeof(creative_items) / sizeof(CreativeItem))

// Helper for case-insensitive comparison
static int creative_strcasecmp_impl(const char *s1, const char *s2) {
  if (!s1 || !s2) return -1;
  for (; *s1 && *s2; s1++, s2++) {
    int c1 = tolower((unsigned char)*s1);
    int c2 = tolower((unsigned char)*s2);
    if (c1 != c2) return c1 - c2;
  }
  return *s1 - *s2;
}

// Find an item by name (case-insensitive)
// Supports both spaces and underscores as separators
uint16_t findCreativeItemByName(const char *name) {
  if (!name) return 0;
  
  for (uint16_t i = 0; i < CREATIVE_ITEM_COUNT; i++) {
    if (creative_strcasecmp_impl(name, creative_items[i].name) == 0) {
      return creative_items[i].item_id;
    }
  }
  
  // If not found with spaces, try replacing underscores with spaces
  char name_with_spaces[128];
  size_t len = strlen(name);
  if (len > 127) len = 127;
  
  for (size_t j = 0; j < len; j++) {
    name_with_spaces[j] = name[j] == '_' ? ' ' : name[j];
  }
  name_with_spaces[len] = '\0';
  
  for (uint16_t i = 0; i < CREATIVE_ITEM_COUNT; i++) {
    if (creative_strcasecmp_impl(name_with_spaces, creative_items[i].name) == 0) {
      return creative_items[i].item_id;
    }
  }
  
  return 0;  // Not found
}

// Expose items for searching
const CreativeItem* creative_items_get(uint16_t index) {
  if (index >= CREATIVE_ITEM_COUNT) return NULL;
  return &creative_items[index];
}

// Initialize creative mode UI for a player
void initCreativeModeUI(int player_index) {
  if (player_index < 0 || player_index >= MAX_PLAYERS) return;
  creative_ui_states[player_index].scroll_position = 0;
  creative_ui_states[player_index].ui_visible = 0;
}

// Check if creative mode is enabled
uint8_t isCreativeModeEnabled(void) {
  return config.gamemode == 1;  // Creative is mode 1
}

// Get the total count of creative items
uint16_t getCreativeItemCount(void) {
  return CREATIVE_ITEM_COUNT;
}

// Toggle creative mode UI visibility for a player
void toggleCreativeModeUI(int client_fd) {
  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return;
  
  int player_index = getClientIndex(client_fd);
  if (player_index < 0 || player_index >= MAX_PLAYERS) return;
  
  if (creative_ui_states[player_index].ui_visible) {
    closeCreativeModeUI(client_fd);
  } else {
    sendCreativeUIScreen(client_fd);
  }
}

// Send the creative mode UI screen (a custom book/sign UI)
void sendCreativeUIScreen(int client_fd) {
  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return;
  
  int player_index = getClientIndex(client_fd);
  if (player_index < 0 || player_index >= MAX_PLAYERS) return;
  
  creative_ui_states[player_index].ui_visible = 1;
  creative_ui_states[player_index].scroll_position = 0;
  
  // Send a system message with creative mode info
  sc_systemChat(client_fd, "§6=== Creative Mode ===", 23);
  sc_systemChat(client_fd, "§7Use §f!creative <item_name>§7 to get an item", 45);
  sc_systemChat(client_fd, "§7Type §f!creative list§7 to see available items", 47);
  sc_systemChat(client_fd, "§7Type §f!creative close§7 to close the UI", 42);
  
  // Send first batch of items
  sendCreativeItemList(client_fd, 0);
}

// Send a list of creative items to the player
void sendCreativeItemList(int client_fd, uint16_t start_index) {
  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return;
  
  if (start_index >= CREATIVE_ITEM_COUNT) {
    sc_systemChat(client_fd, "§cEnd of item list", 18);
    return;
  }
  
  // Display 20 items at a time
  uint16_t end_index = start_index + 20;
  if (end_index > CREATIVE_ITEM_COUNT) {
    end_index = CREATIVE_ITEM_COUNT;
  }
  
  // Send header
  char header[64];
  snprintf(header, sizeof(header), "§7Items %d-%d of %d:", start_index + 1, end_index, CREATIVE_ITEM_COUNT);
  sc_systemChat(client_fd, header, strlen(header));
  
  // Send items
  for (uint16_t i = start_index; i < end_index; i++) {
    char item_msg[128];
    snprintf(item_msg, sizeof(item_msg), "§a%d§7: %s", i + 1, creative_items[i].name);
    sc_systemChat(client_fd, item_msg, strlen(item_msg));
  }
  
  // Send navigation hints
  if (end_index < CREATIVE_ITEM_COUNT) {
    sc_systemChat(client_fd, "§7Type §f!creative next§7 for more items", 39);
  }
}

// Handle a click on an item in the creative UI
void handleCreativeItemClick(int client_fd, uint16_t item_id) {
  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return;
  
  // Find the item in the creative list
  for (uint16_t i = 0; i < CREATIVE_ITEM_COUNT; i++) {
    if (creative_items[i].item_id == item_id) {
      // Get the stack size for this item
      uint8_t stack_size = getItemStackSize(item_id);
      
      // Give the player the item
      int result = givePlayerItem(player, item_id, stack_size);
      
      if (result == 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "§aGave you §f%s§a (%d)", creative_items[i].name, stack_size);
        sc_systemChat(client_fd, msg, strlen(msg));
      } else {
        sc_systemChat(client_fd, "§cYour inventory is full!", 24);
      }
      return;
    }
  }
  
  sc_systemChat(client_fd, "§cItem not found", 15);
}

// Close the creative mode UI
void closeCreativeModeUI(int client_fd) {
  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return;
  
  int player_index = getClientIndex(client_fd);
  if (player_index < 0 || player_index >= MAX_PLAYERS) return;
  
  creative_ui_states[player_index].ui_visible = 0;
  sc_systemChat(client_fd, "§7Creative mode UI closed", 25);
}

// Returns a random item ID from the creative item list
uint16_t getRandomCreativeItem(void) {
  uint16_t count = getCreativeItemCount();
  if (count == 0) return 0;
  return creative_items[fast_rand() % count].item_id;
}
