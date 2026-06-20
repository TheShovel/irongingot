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
  
  // Wood - Pale Oak
  {I_pale_oak_log, "Pale Oak Log"},
  {I_pale_oak_wood, "Pale Oak Wood"},
  {I_pale_oak_planks, "Pale Oak Planks"},
  {I_pale_oak_leaves, "Pale Oak Leaves"},
  {I_pale_oak_sapling, "Pale Oak Sapling"},
  
  // Bamboo
  {I_bamboo_block, "Bamboo Block"},
  {I_bamboo_planks, "Bamboo Planks"},
  {I_bamboo_mosaic, "Bamboo Mosaic"},
  
  // Wood - Crimson
  {I_crimson_stem, "Crimson Stem"},
  {I_stripped_crimson_stem, "Stripped Crimson Stem"},
  {I_crimson_hyphae, "Crimson Hyphae"},
  {I_stripped_crimson_hyphae, "Stripped Crimson Hyphae"},
  {I_crimson_planks, "Crimson Planks"},
  {I_crimson_nylium, "Crimson Nylium"},
  
  // Wood - Warped
  {I_warped_stem, "Warped Stem"},
  {I_stripped_warped_stem, "Stripped Warped Stem"},
  {I_warped_hyphae, "Warped Hyphae"},
  {I_stripped_warped_hyphae, "Stripped Warped Hyphae"},
  {I_warped_planks, "Warped Planks"},
  {I_warped_nylium, "Warped Nylium"},
  
  // Stripped Logs
  {I_stripped_oak_log, "Stripped Oak Log"},
  {I_stripped_spruce_log, "Stripped Spruce Log"},
  {I_stripped_birch_log, "Stripped Birch Log"},
  {I_stripped_jungle_log, "Stripped Jungle Log"},
  {I_stripped_acacia_log, "Stripped Acacia Log"},
  {I_stripped_dark_oak_log, "Stripped Dark Oak Log"},
  {I_stripped_mangrove_log, "Stripped Mangrove Log"},
  {I_stripped_cherry_log, "Stripped Cherry Log"},
  {I_stripped_pale_oak_log, "Stripped Pale Oak Log"},
  {I_stripped_bamboo_block, "Stripped Bamboo Block"},
  
  // Stripped Wood
  {I_stripped_oak_wood, "Stripped Oak Wood"},
  {I_stripped_spruce_wood, "Stripped Spruce Wood"},
  {I_stripped_birch_wood, "Stripped Birch Wood"},
  {I_stripped_jungle_wood, "Stripped Jungle Wood"},
  {I_stripped_acacia_wood, "Stripped Acacia Wood"},
  {I_stripped_dark_oak_wood, "Stripped Dark Oak Wood"},
  {I_stripped_mangrove_wood, "Stripped Mangrove Wood"},
  {I_stripped_cherry_wood, "Stripped Cherry Wood"},
  {I_stripped_pale_oak_wood, "Stripped Pale Oak Wood"},
  
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
  
  // Fence Gates
  {I_oak_fence_gate, "Oak Fence Gate"},
  {I_spruce_fence_gate, "Spruce Fence Gate"},
  {I_birch_fence_gate, "Birch Fence Gate"},
  {I_jungle_fence_gate, "Jungle Fence Gate"},
  {I_acacia_fence_gate, "Acacia Fence Gate"},
  {I_dark_oak_fence_gate, "Dark Oak Fence Gate"},
  {I_mangrove_fence_gate, "Mangrove Fence Gate"},
  {I_cherry_fence_gate, "Cherry Fence Gate"},
  {I_pale_oak_fence_gate, "Pale Oak Fence Gate"},
  {I_bamboo_fence_gate, "Bamboo Fence Gate"},
  {I_crimson_fence_gate, "Crimson Fence Gate"},
  {I_warped_fence_gate, "Warped Fence Gate"},
  
  // Buttons
  {I_stone_button, "Stone Button"},
  {I_oak_button, "Oak Button"},
  {I_spruce_button, "Spruce Button"},
  {I_birch_button, "Birch Button"},
  {I_jungle_button, "Jungle Button"},
  {I_acacia_button, "Acacia Button"},
  {I_dark_oak_button, "Dark Oak Button"},
  {I_mangrove_button, "Mangrove Button"},
  {I_cherry_button, "Cherry Button"},
  {I_pale_oak_button, "Pale Oak Button"},
  {I_bamboo_button, "Bamboo Button"},
  {I_crimson_button, "Crimson Button"},
  {I_warped_button, "Warped Button"},
  {I_polished_blackstone_button, "Polished Blackstone Button"},
  
  // Pressure Plates
  {I_oak_pressure_plate, "Oak Pressure Plate"},
  {I_spruce_pressure_plate, "Spruce Pressure Plate"},
  {I_birch_pressure_plate, "Birch Pressure Plate"},
  {I_jungle_pressure_plate, "Jungle Pressure Plate"},
  {I_acacia_pressure_plate, "Acacia Pressure Plate"},
  {I_dark_oak_pressure_plate, "Dark Oak Pressure Plate"},
  {I_mangrove_pressure_plate, "Mangrove Pressure Plate"},
  {I_cherry_pressure_plate, "Cherry Pressure Plate"},
  {I_pale_oak_pressure_plate, "Pale Oak Pressure Plate"},
  {I_bamboo_pressure_plate, "Bamboo Pressure Plate"},
  {I_crimson_pressure_plate, "Crimson Pressure Plate"},
  {I_warped_pressure_plate, "Warped Pressure Plate"},
  {I_stone_pressure_plate, "Stone Pressure Plate"},
  {I_polished_blackstone_pressure_plate, "Polished Blackstone Pressure Plate"},
  {I_light_weighted_pressure_plate, "Light Weighted Pressure Plate"},
  {I_heavy_weighted_pressure_plate, "Heavy Weighted Pressure Plate"},
  
  // Signs
  {I_oak_sign, "Oak Sign"},
  {I_spruce_sign, "Spruce Sign"},
  {I_birch_sign, "Birch Sign"},
  {I_jungle_sign, "Jungle Sign"},
  {I_acacia_sign, "Acacia Sign"},
  {I_dark_oak_sign, "Dark Oak Sign"},
  {I_mangrove_sign, "Mangrove Sign"},
  {I_cherry_sign, "Cherry Sign"},
  {I_pale_oak_sign, "Pale Oak Sign"},
  {I_bamboo_sign, "Bamboo Sign"},
  {I_crimson_sign, "Crimson Sign"},
  {I_warped_sign, "Warped Sign"},
  
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
  {I_brown_wool, "Brown Wool"},
  {I_red_wool, "Red Wool"},
  {I_green_wool, "Green Wool"},
  {I_black_wool, "Black Wool"},
  
  // Beds
  {I_white_bed, "White Bed"},
  {I_orange_bed, "Orange Bed"},
  {I_magenta_bed, "Magenta Bed"},
  {I_light_blue_bed, "Light Blue Bed"},
  {I_yellow_bed, "Yellow Bed"},
  {I_lime_bed, "Lime Bed"},
  {I_pink_bed, "Pink Bed"},
  {I_gray_bed, "Gray Bed"},
  {I_light_gray_bed, "Light Gray Bed"},
  {I_cyan_bed, "Cyan Bed"},
  {I_purple_bed, "Purple Bed"},
  {I_blue_bed, "Blue Bed"},
  {I_brown_bed, "Brown Bed"},
  {I_green_bed, "Green Bed"},
  {I_red_bed, "Red Bed"},
  {I_black_bed, "Black Bed"},
  
  // Carpets
  {I_white_carpet, "White Carpet"},
  {I_orange_carpet, "Orange Carpet"},
  {I_magenta_carpet, "Magenta Carpet"},
  {I_light_blue_carpet, "Light Blue Carpet"},
  {I_yellow_carpet, "Yellow Carpet"},
  {I_lime_carpet, "Lime Carpet"},
  {I_pink_carpet, "Pink Carpet"},
  {I_gray_carpet, "Gray Carpet"},
  {I_light_gray_carpet, "Light Gray Carpet"},
  {I_cyan_carpet, "Cyan Carpet"},
  {I_purple_carpet, "Purple Carpet"},
  {I_blue_carpet, "Blue Carpet"},
  {I_brown_carpet, "Brown Carpet"},
  {I_green_carpet, "Green Carpet"},
  {I_red_carpet, "Red Carpet"},
  {I_black_carpet, "Black Carpet"},
  
  // Other Building Blocks
  {I_glass, "Glass"},
  {I_crafting_table, "Crafting Table"},
  {I_furnace, "Furnace"},
  {I_chest, "Chest"},
  {I_barrel, "Barrel"},
  {I_ender_chest, "Ender Chest"},
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
  
  // Terracotta
  {I_terracotta, "Terracotta"},
  {I_white_terracotta, "White Terracotta"},
  {I_orange_terracotta, "Orange Terracotta"},
  {I_magenta_terracotta, "Magenta Terracotta"},
  {I_light_blue_terracotta, "Light Blue Terracotta"},
  {I_yellow_terracotta, "Yellow Terracotta"},
  {I_lime_terracotta, "Lime Terracotta"},
  {I_pink_terracotta, "Pink Terracotta"},
  {I_gray_terracotta, "Gray Terracotta"},
  {I_light_gray_terracotta, "Light Gray Terracotta"},
  {I_cyan_terracotta, "Cyan Terracotta"},
  {I_purple_terracotta, "Purple Terracotta"},
  {I_blue_terracotta, "Blue Terracotta"},
  {I_brown_terracotta, "Brown Terracotta"},
  {I_green_terracotta, "Green Terracotta"},
  {I_red_terracotta, "Red Terracotta"},
  {I_black_terracotta, "Black Terracotta"},
  
  // Stone Bricks & Variants
  {I_stone_bricks, "Stone Bricks"},
  {I_stone_brick_slab, "Stone Brick Slab"},
  {I_stone_brick_stairs, "Stone Brick Stairs"},
  {I_stone_brick_wall, "Stone Brick Wall"},
  {I_mossy_stone_bricks, "Mossy Stone Bricks"},
  {I_mossy_stone_brick_slab, "Mossy Stone Brick Slab"},
  {I_mossy_stone_brick_stairs, "Mossy Stone Brick Stairs"},
  {I_mossy_stone_brick_wall, "Mossy Stone Brick Wall"},
  {I_cracked_stone_bricks, "Cracked Stone Bricks"},
  {I_chiseled_stone_bricks, "Chiseled Stone Bricks"},
  {I_stone_slab, "Stone Slab"},
  {I_mossy_cobblestone, "Mossy Cobblestone"},
  {I_mossy_cobblestone_slab, "Mossy Cobblestone Slab"},
  {I_mossy_cobblestone_stairs, "Mossy Cobblestone Stairs"},
  {I_mossy_cobblestone_wall, "Mossy Cobblestone Wall"},
  {I_cobblestone_wall, "Cobblestone Wall"},
  {I_andesite_slab, "Andesite Slab"},
  {I_andesite_stairs, "Andesite Stairs"},
  {I_andesite_wall, "Andesite Wall"},
  {I_diorite_slab, "Diorite Slab"},
  {I_diorite_stairs, "Diorite Stairs"},
  {I_diorite_wall, "Diorite Wall"},
  {I_granite_slab, "Granite Slab"},
  {I_granite_stairs, "Granite Stairs"},
  {I_granite_wall, "Granite Wall"},
  {I_bricks, "Bricks"},
  {I_brick_slab, "Brick Slab"},
  {I_brick_stairs, "Brick Stairs"},
  {I_brick_wall, "Brick Wall"},
  {I_sandstone, "Sandstone"},
  {I_sandstone_slab, "Sandstone Slab"},
  {I_sandstone_stairs, "Sandstone Stairs"},
  {I_sandstone_wall, "Sandstone Wall"},
  {I_cut_sandstone, "Cut Sandstone"},
  {I_cut_sandstone_slab, "Cut Sandstone Slab"},
  {I_chiseled_sandstone, "Chiseled Sandstone"},
  {I_smooth_sandstone, "Smooth Sandstone"},
  {I_end_stone, "End Stone"},
  {I_ancient_debris, "Ancient Debris"},
  {I_gilded_blackstone, "Gilded Blackstone"},
  {I_crying_obsidian, "Crying Obsidian"},
  {I_shroomlight, "Shroomlight"},
  {I_nether_wart_block, "Nether Wart Block"},
  {I_warped_wart_block, "Warped Wart Block"},
  {I_bone_block, "Bone Block"},
  {I_dried_kelp_block, "Dried Kelp Block"},
  {I_slime_block, "Slime Block"},
  {I_honey_block, "Honey Block"},
  {I_honeycomb_block, "Honeycomb Block"},
  {I_tnt, "TNT"},
  {I_chain, "Chain"},
  {I_lightning_rod, "Lightning Rod"},
  
  // Functional Blocks
  {I_bookshelf, "Bookshelf"},
  {I_hay_block, "Hay Bale"},
  {I_note_block, "Note Block"},
  {I_dispenser, "Dispenser"},
  {I_dropper, "Dropper"},
  {I_observer, "Observer"},
  {I_piston, "Piston"},
  {I_sticky_piston, "Sticky Piston"},
  {I_ladder, "Ladder"},
  {I_glass_pane, "Glass Pane"},
  {I_iron_bars, "Iron Bars"},
  {I_lantern, "Lantern"},
  {I_campfire, "Campfire"},
  {I_bell, "Bell"},
  {I_cauldron, "Cauldron"},
  {I_composter, "Composter"},
  {I_cobweb, "Cobweb"},
  {I_lectern, "Lectern"},
  {I_fletching_table, "Fletching Table"},
  {I_smithing_table, "Smithing Table"},
  {I_blast_furnace, "Blast Furnace"},
  {I_smoker, "Smoker"},
  {I_brewing_stand, "Brewing Stand"},
  {I_cartography_table, "Cartography Table"},
  {I_grindstone, "Grindstone"},
  {I_loom, "Loom"},
  {I_stonecutter, "Stonecutter"},
  {I_enchanting_table, "Enchanting Table"},
  {I_anvil, "Anvil"},
  {I_jukebox, "Jukebox"},
  {I_target, "Target"},
  {I_daylight_detector, "Daylight Detector"},
  {I_redstone_lamp, "Redstone Lamp"},
  {I_lever, "Lever"},
  
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
  snprintf(header, sizeof(header), "§7Items %d-%d of %zu:", start_index + 1, end_index, CREATIVE_ITEM_COUNT);
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
