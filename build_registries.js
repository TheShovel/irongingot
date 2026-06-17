const fs = require("fs/promises");
const path = require("path");

// Overrides for block-to-item conversion
const blockToItemOverrides = {
  grass_block: "dirt",
  snowy_grass_block: "dirt",
  stone: "cobblestone",
  end_portal_frame_eye_north: "end_portal_frame",
  end_portal_frame_eye_south: "end_portal_frame",
  end_portal_frame_eye_west: "end_portal_frame",
  end_portal_frame_eye_east: "end_portal_frame",
  diamond_ore: "diamond",
  deepslate_diamond_ore: "diamond",
  gold_ore: "raw_gold",
  deepslate_gold_ore: "raw_gold",
  nether_gold_ore: "raw_gold",
  redstone_ore: "redstone",
  deepslate_redstone_ore: "redstone",
  iron_ore: "raw_iron",
  deepslate_iron_ore: "raw_iron",
  coal_ore: "coal",
  deepslate_coal_ore: "coal",
  copper_ore: "raw_copper",
  deepslate_copper_ore: "raw_copper",
  emerald_ore: "emerald",
  deepslate_emerald_ore: "emerald",
  lapis_ore: "lapis_lazuli",
  deepslate_lapis_ore: "lapis_lazuli",
  nether_quartz_ore: "quartz",
  snow: "snowball",
  dead_bush: "stick",
  farmland: "dirt",
  wheat: "wheat_seeds",
  wheat_1: "wheat_seeds",
  wheat_2: "wheat_seeds",
  wheat_3: "wheat_seeds",
  wheat_4: "wheat_seeds",
  wheat_5: "wheat_seeds",
  wheat_6: "wheat_seeds",
  wheat_7: "wheat_seeds",
  wall_torch: "torch",
};

// Blacklisted block name strings
const blockBlacklist = ["infested_", "stained_", "_head"];

// Whitelisted blocks, i.e. guaranteed to be included
const blockWhitelist = [
  "air",
  "water",
  "water_1",
  "water_2",
  "water_3",
  "water_4",
  "water_5",
  "water_6",
  "water_7",
  "lava",
  "lava_2",
  "lava_4",
  "lava_6",
  "snowy_grass_block",
  "mud",
  "moss_carpet",
  "composter",
  "coal_block",
  "copper_ore",
  "copper_block",
  "mycelium",
  "podzol",
  "red_sand",
  "terracotta",
  "white_terracotta",
  "orange_terracotta",
  "yellow_terracotta",
  "brown_terracotta",
  "red_terracotta",
  "light_gray_terracotta",
  "calcite",
  "snow_block",
  "packed_ice",
  "blue_ice",
  "magma_block",
  "spruce_log",
  "spruce_leaves",
  "birch_log",
  "birch_leaves",
  "jungle_log",
  "jungle_leaves",
  "acacia_log",
  "acacia_leaves",
  "dark_oak_log",
  "dark_oak_leaves",
  "cherry_log",
  "cherry_leaves",
  "mangrove_log",
  "mangrove_leaves",
  "bamboo_block",
  "cactus",
  "lily_pad",
  "snow",
  "redstone_block",
  "redstone_ore",
  "ice",
  "short_grass",
  "sugar_cane",
  // Essential functional blocks
  "chest",
  "crafting_table",
  "furnace",
  "diamond_ore",
  "diamond_block",
  // Wood slabs (all types)
  "oak_slab",
  "spruce_slab",
  "birch_slab",
  "jungle_slab",
  "acacia_slab",
  "dark_oak_slab",
  "mangrove_slab",
  "cherry_slab",
  "pale_oak_slab",
  // Wood stairs (all types)
  "oak_stairs",
  "spruce_stairs",
  "birch_stairs",
  "jungle_stairs",
  "acacia_stairs",
  "dark_oak_stairs",
  "mangrove_stairs",
  "cherry_stairs",
  "pale_oak_stairs",
  // Wood doors (all types)
  "oak_door",
  "spruce_door",
  "birch_door",
  "jungle_door",
  "acacia_door",
  "cherry_door",
  "dark_oak_door",
  "pale_oak_door",
  "mangrove_door",
  // Wood trapdoors (all types)
  "oak_trapdoor",
  "spruce_trapdoor",
  "birch_trapdoor",
  "jungle_trapdoor",
  "acacia_trapdoor",
  "dark_oak_trapdoor",
  "mangrove_trapdoor",
  "cherry_trapdoor",
  "pale_oak_trapdoor",
  // Wood fences (all types)
  "oak_fence",
  "spruce_fence",
  "birch_fence",
  "jungle_fence",
  "acacia_fence",
  "dark_oak_fence",
  "mangrove_fence",
  "cherry_fence",
  "pale_oak_fence",
  // Cobblestone
  "cobblestone_slab",
  "cobblestone_stairs",
  // Essential blocks referenced in code
  "torch",
  "dandelion",
  "poppy",
  "blue_orchid",
  "allium",
  "azure_bluet",
  "red_tulip",
  "orange_tulip",
  "white_tulip",
  "pink_tulip",
  "oxeye_daisy",
  "cornflower",
  "wither_rose",
  "lily_of_the_valley",
  "torchflower",
  "iron_block",
  "gold_block",
];

// Supported biomes
const biomes = [
  "ocean",
  "plains",
  "desert",
  "windswept_hills",
  "forest",
  "taiga",
  "swamp",
  "river",
  "frozen_ocean",
  "frozen_river",
  "snowy_plains",
  "mushroom_fields",
  "beach",
  "jungle",
  "birch_forest",
  "dark_forest",
  "snowy_taiga",
  "savanna",
  "badlands",
  "deep_ocean",
  "mangrove_swamp",
  "stony_peaks",
  "jagged_peaks",
  "frozen_peaks",
  "meadow",
  "cherry_grove",
  "old_growth_pine_taiga",
  "bamboo_jungle",
  // Additional biomes
  "windswept_forest",
  "windswept_savanna",
  "snowy_slopes",
  "grove",
  "sunflower_plains",
  "flower_forest",
  "ice_spikes",
  "eroded_badlands",
  // Nether biomes
  "nether_wastes",
  "soul_sand_valley",
  "crimson_forest",
  "warped_forest",
  "basalt_deltas",
  // End biomes
  "the_end",
  "small_end_islands",
  "end_midlands",
  "end_highlands",
  "end_barrens",
];

// Extract item and block data from registry dump
async function extractItemsAndBlocks() {
  // Block network IDs are defined in their own JSON file
  // The item JSON file doesn't define IDs, we get those from the registries
  const blockSource = JSON.parse(
    await fs.readFile(
      `${__dirname}/notchian/generated/reports/blocks.json`,
      "utf8",
    ),
  );

  // Get registry data for extracting item IDs
  const registriesJSON = JSON.parse(
    await fs.readFile(
      `${__dirname}/notchian/generated/reports/registries.json`,
      "utf8",
    ),
  );
  const itemSource = registriesJSON["minecraft:item"].entries;
  // Retrieve the registry list for blocks too, used later in tags
  const blockRegistrySource = registriesJSON["minecraft:block"].entries;

  // Sort blocks by their network ID
  // Since we're only storing 256 blocks, this prioritizes the "common" ones first
  const sortedBlocks = Object.entries(blockSource);
  sortedBlocks.sort((a, b) => {
    const aState = a[1].states.find((c) => c.default);
    if (!aState) return 1;
    const bState = b[1].states.find((c) => c.default);
    if (!bState) return -1;
    return aState.id - bState.id;
  });

  // Create name-id pair objects for easier parsing
  const blocks = {},
    items = {};

  // Trapdoor state lookup tables: { name: [16 state IDs] }
  const trapdoorStateTables = {};
  // Stair state lookup tables: { name: [8 state IDs] }
  const stairStateTables = {};
  // Door state lookup tables: { name: [32 state IDs] }
  // Iteration order: facing(n,s,w,e) x half(upper,lower) x hinge(left,right) x open(true,false), powered=false
  const doorStateTables = {};
  // Fence state lookup tables: { name: [16 state IDs] }, mask bits: north/east/south/west.
  const fenceStateTables = {};
  // Horizontal facing lookup tables: { name: [4 state IDs] }, order: north/south/west/east.
  const horizontalStateTables = {};

  for (const entry of sortedBlocks) {
    const defaultState = entry[1].states.find((c) => c.default);
    if (!defaultState) continue;
    // Check if a part of this block's name is in the blacklist
    let found = false;
    for (const str of blockBlacklist) {
      if (entry[0].includes(str)) {
        found = true;
        break;
      }
    }
    if (found) continue;
    // Register the block ID
    blocks[entry[0].replace("minecraft:", "")] = defaultState.id;

    // Extract trapdoor state IDs for lookup table generation
    if (entry[0].endsWith("_trapdoor")) {
      const name = entry[0].replace("minecraft:", "");
      const facingOrder = ["north", "south", "west", "east"];
      const halfOrder = ["bottom", "top"];
      const openOrder = [false, true];
      const trapdoorRow = [];
      for (const f of facingOrder) {
        for (const h of halfOrder) {
          for (const o of openOrder) {
            const st = entry[1].states.find(
              (s) =>
                s.properties.facing === f &&
                s.properties.half === h &&
                s.properties.open === String(o) &&
                s.properties.waterlogged === "false" &&
                s.properties.powered === "false",
            );
            trapdoorRow.push(st ? st.id : 0);
          }
        }
      }
      trapdoorStateTables[name] = trapdoorRow;
    }

    // Extract stair state IDs for lookup table generation
    if (entry[0].endsWith("_stairs")) {
      const name = entry[0].replace("minecraft:", "");
      const facingOrder = ["north", "south", "west", "east"];
      const halfOrder = ["bottom", "top"];
      const stairRow = [];
      for (const f of facingOrder) {
        for (const h of halfOrder) {
          const st = entry[1].states.find(
            (s) =>
              s.properties.facing === f &&
              s.properties.half === h &&
              s.properties.shape === "straight" &&
              s.properties.waterlogged === "false",
          );
          stairRow.push(st ? st.id : 0);
        }
      }
      stairStateTables[name] = stairRow;
    }

    // Extract door state IDs for lookup table generation
    if (entry[0].endsWith("_door")) {
      const name = entry[0].replace("minecraft:", "");
      const facingOrder = ["north", "south", "west", "east"];
      const halfOrder = ["upper", "lower"];
      const hingeOrder = ["left", "right"];
      const openOrder = [true, false];
      const doorRow = [];
      for (const f of facingOrder) {
        for (const h of halfOrder) {
          for (const hi of hingeOrder) {
            for (const o of openOrder) {
              const st = entry[1].states.find(
                (s) =>
                  s.properties.facing === f &&
                  s.properties.half === h &&
                  s.properties.hinge === hi &&
                  s.properties.open === String(o) &&
                  s.properties.powered === "false",
              );
              doorRow.push(st ? st.id : 0);
            }
          }
        }
      }
      doorStateTables[name] = doorRow;
    }

    if (
      (entry[0].endsWith("_fence") && !entry[0].endsWith("_fence_gate")) ||
      entry[0].endsWith("glass_pane")
    ) {
      const name = entry[0].replace("minecraft:", "");
      const fenceRow = [];
      for (let mask = 0; mask < 16; mask++) {
        const st = entry[1].states.find(
          (s) =>
            s.properties &&
            s.properties.north === String((mask & 1) !== 0) &&
            s.properties.east === String((mask & 2) !== 0) &&
            s.properties.south === String((mask & 4) !== 0) &&
            s.properties.west === String((mask & 8) !== 0) &&
            (!("waterlogged" in s.properties) ||
              s.properties.waterlogged === "false"),
        );
        fenceRow.push(st ? st.id : defaultState.id);
      }
      fenceStateTables[name] = fenceRow;
    }

    if (entry[1].properties && Array.isArray(entry[1].properties.facing)) {
      const faces = entry[1].properties.facing;
      if (["north", "south", "west", "east"].every((f) => faces.includes(f))) {
        const name = entry[0].replace("minecraft:", "");
        const facingRow = [];
        for (const f of ["north", "south", "west", "east"]) {
          const st = entry[1].states.find((s) => {
            if (!s.properties || s.properties.facing !== f) return false;
            for (const [key, value] of Object.entries(
              defaultState.properties || {},
            )) {
              if (key === "facing") continue;
              if (s.properties[key] !== value) return false;
            }
            return true;
          });
          facingRow.push(st ? st.id : defaultState.id);
        }
        horizontalStateTables[name] = facingRow;
      }
    }

    // Include "snowy" variants of blocks as well
    if ("properties" in defaultState && "snowy" in defaultState.properties) {
      const snowyState = entry[1].states.find((c) => c.properties.snowy);
      blocks["snowy_" + entry[0].replace("minecraft:", "")] = snowyState.id;
    }
    // Include levels for fluids
    if ("fluid" in entry[1].definition) {
      for (let i = 1; i <= 7; i++) {
        blocks[entry[0].replace("minecraft:", "") + "_" + i] =
          defaultState.id + i;
      }
    }

    // End portal frames need the filled eye=true states for generated
    // stronghold portals. Store each facing as its own palette entry so chunk
    // generation can emit the exact state ID without runtime state metadata.
    if (entry[0] === "minecraft:end_portal_frame") {
      for (const facing of ["north", "south", "west", "east"]) {
        const state = entry[1].states.find(
          (s) =>
            s.properties &&
            s.properties.eye === "true" &&
            s.properties.facing === facing,
        );
        if (state) {
          blocks[`end_portal_frame_eye_${facing}`] = state.id;
        }
      }
    }

    if (entry[0] === "minecraft:wheat") {
      for (let age = 1; age <= 7; age++) {
        const state = entry[1].states.find(
          (s) => s.properties && s.properties.age === String(age),
        );
        if (state) {
          blocks[`wheat_${age}`] = state.id;
        }
      }
    }
  }

  for (const item in itemSource) {
    items[item.replace("minecraft:", "")] = itemSource[item].protocol_id;
  }

  /**
   * Create a block network ID palette.
   * The goal is to pack as many meaningful blocks into 256 IDs as
   * possible. For this, we only include blocks that have corresponding
   * items, outside of some exceptions.
   */
  const palette = {};

  // While we're at it, map block IDs to item IDs
  const mapping = [],
    mappingWithOverrides = [];

  // Handle explicitly whitelisted blocks first
  for (const block of blockWhitelist) {
    palette[block] = blocks[block];
    mapping.push(items[block] || 0);
    mappingWithOverrides.push(
      items[blockToItemOverrides[block]] || items[block] || 0,
    );
    if (mapping.length === 256) break;
  }

  // Continue adding blocks with matching items
  for (const block in blocks) {
    if (!(block in items)) continue;
    if (blockWhitelist.includes(block)) continue;
    palette[block] = blocks[block];
    mapping.push(items[block] || 0);
    mappingWithOverrides.push(
      items[blockToItemOverrides[block]] || items[block] || 0,
    );
    if (mapping.length === 256) break;
  }

  // Now add nether/essential blocks after the initial 256
  const extraBlocks = [
    "netherrack",
    "soul_sand",
    "soul_soil",
    "glowstone",
    "nether_quartz_ore",
    "obsidian",
    "ancient_debris",
    "basalt",
    "blackstone",
    "crimson_nylium",
    "warped_nylium",
    "shroomlight",
    "nether_bricks",
    "cracked_nether_bricks",
    "nether_wart_block",
    "warped_wart_block",
    "crimson_stem",
    "warped_stem",
    "fire",
    "gilded_blackstone",
    "nether_portal",
    "emerald_ore",
    "stone_bricks",
    "mossy_stone_bricks",
    "cracked_stone_bricks",
    "chiseled_stone_bricks",
    "iron_bars",
    "bookshelf",
    "end_portal_frame",
    "end_portal_frame_eye_north",
    "end_portal_frame_eye_south",
    "end_portal_frame_eye_west",
    "end_portal_frame_eye_east",
    "end_portal",
    "end_stone",
    "dirt_path",
    "farmland",
    "wheat",
    "wheat_1",
    "wheat_2",
    "wheat_3",
    "wheat_4",
    "wheat_5",
    "wheat_6",
    "wheat_7",
    "wall_torch",
    "lectern",
    "fletching_table",
    "smithing_table",
    "blast_furnace",
    "smoker",
    "brewing_stand",
    "cartography_table",
    "grindstone",
    "loom",
    "stonecutter",
    "cauldron",
    "hay_block",
    "white_wool",
    "mossy_cobblestone",
    // Village structure blocks (added as extras to avoid shifting essential block IDs)
    "white_carpet",
    "oak_pressure_plate",
    "spruce_pressure_plate",
    "acacia_pressure_plate",
    "stone_pressure_plate",
    "smooth_sandstone",
    "smooth_sandstone_stairs",
    "smooth_sandstone_slab",
    "sandstone_slab",
    "sandstone_wall",
    "cobblestone_wall",
    "lantern",
    "ladder",
    "campfire",
    "bell",
    "glass_pane",
    "white_glazed_terracotta",
    "tall_grass",
    "large_fern",
    "spawner",
    "barrel",
    "ender_chest",
  ];
  for (const block of extraBlocks) {
    if (block in palette) continue; // Already included in first 256
    if (!(block in blocks)) continue; // Not in notchian data
    palette[block] = blocks[block];
    mapping.push(items[block] || 0);
    mappingWithOverrides.push(
      items[blockToItemOverrides[block]] || items[block] || 0,
    );
  }

  // Build list of block IDs, but from the registries
  // Tags refer to these IDs, not the actual blocks
  const blockRegistry = {};
  for (const block in blockRegistrySource) {
    blockRegistry[block.replace("minecraft:", "")] =
      blockRegistrySource[block].protocol_id;
  }

  return {
    blocks,
    items,
    palette,
    mapping,
    mappingWithOverrides,
    blockRegistry,
    trapdoorStateTables,
    stairStateTables,
    doorStateTables,
    fenceStateTables,
    horizontalStateTables,
  };
}

// Write an integer as a VarInt
function writeVarInt(value) {
  const bytes = [];
  while (true) {
    if ((value & ~0x7f) === 0) {
      bytes.push(value);
      return Buffer.from(bytes);
    }
    bytes.push((value & 0x7f) | 0x80);
    value >>>= 7;
  }
}

// Scan directory recursively to find all JSON files
async function scanDirectory(basePath, currentPath = "") {
  const entries = {};
  const items = await fs.readdir(path.join(basePath, currentPath), {
    withFileTypes: true,
  });

  for (const item of items) {
    const relativePath = path.join(currentPath, item.name);
    if (item.isDirectory()) {
      const subEntries = await scanDirectory(basePath, relativePath);
      Object.assign(entries, subEntries);
    } else if (item.name.endsWith(".json")) {
      const dirPath = path.dirname(relativePath);
      const registryName = dirPath === "." ? "" : dirPath;
      const entryName = path.basename(item.name, ".json");

      if (!entries[registryName]) {
        entries[registryName] = [];
      }
      entries[registryName].push(entryName);
    }
  }

  return entries;
}

// Serialize a single registry
function serializeRegistry(name, entries) {
  const parts = [];

  // Packet ID for Registry Data
  parts.push(Buffer.from([0x07]));

  // Registry name
  const nameBuf = Buffer.from(name, "utf8");
  parts.push(writeVarInt(nameBuf.length));
  parts.push(nameBuf);

  // Entry count
  parts.push(writeVarInt(entries.length));

  // Serialize entries
  for (const entryName of entries) {
    const entryBuf = Buffer.from(entryName, "utf8");
    parts.push(writeVarInt(entryBuf.length));
    parts.push(entryBuf);
    parts.push(Buffer.from([0x00]));
  }

  // Combine all parts
  const fullData = Buffer.concat(parts);

  // Prepend packet length
  const lengthBuf = writeVarInt(fullData.length);

  return Buffer.concat([lengthBuf, fullData]);
}

// Serialize a tag update
function serializeTags(tags) {
  const parts = [];

  // Packet ID for Update Tags
  parts.push(Buffer.from([0x0d]));

  // Tag type count
  parts.push(writeVarInt(Object.keys(tags).length));

  // Tag registry entry
  for (const type in tags) {
    // Tag registry identifier
    const identifier = Buffer.from(type, "utf8");
    parts.push(writeVarInt(identifier.length));
    parts.push(identifier);

    // Tag count
    parts.push(writeVarInt(Object.keys(tags[type]).length));

    // Write tag data
    for (const tag in tags[type]) {
      // Tag identifier
      const identifier = Buffer.from(tag, "utf8");
      parts.push(writeVarInt(identifier.length));
      parts.push(identifier);
      // Array of IDs
      parts.push(writeVarInt(Object.keys(tags[type][tag]).length));
      for (const id of tags[type][tag]) {
        parts.push(writeVarInt(id));
      }
    }
  }

  // Combine all parts
  const fullData = Buffer.concat(parts);

  // Prepend packet length
  const lengthBuf = writeVarInt(fullData.length);

  return Buffer.concat([lengthBuf, fullData]);
}

function toVarIntBuffer(array) {
  const parts = [];
  for (const num of array) {
    parts.push(writeVarInt(num));
  }
  return Buffer.concat(parts);
}

// Convert to C-style hex byte array string
function toCArray(buffer) {
  const hexBytes = [...buffer].map(
    (b) => `0x${b.toString(16).padStart(2, "0")}`,
  );
  const lines = [];
  for (let i = 0; i < hexBytes.length; i += 12) {
    lines.push("  " + hexBytes.slice(i, i + 12).join(", "));
  }
  return lines.join(",\n");
}

const requiredRegistries = [
  "cat_variant",
  "chicken_variant",
  "cow_variant",
  "frog_variant",
  "painting_variant",
  "pig_variant",
  "wolf_sound_variant",
  "wolf_variant",
  "damage_type",
];

async function convert() {
  const inputPath = __dirname + "/notchian/generated/data/minecraft";
  const outputPath = __dirname + "/src/registries.c";
  const headerPath = __dirname + "/include/registries.h";

  const registries = await scanDirectory(inputPath);
  const registryBuffers = [];

  for (const registry of requiredRegistries) {
    if (!(registry in registries)) {
      console.error(`Missing required registry "${registry}"!`);
      return;
    }
    if (registry.endsWith("variant")) {
      // The mob "variants" only require one valid variant to be accepted
      // Send "temperate" if available, otherwise shortest string to save memory
      if (registries[registry].includes("temperate")) {
        registryBuffers.push(serializeRegistry(registry, ["temperate"]));
      } else {
        const shortest = registries[registry].sort(
          (a, b) => a.length - b.length,
        )[0];
        registryBuffers.push(serializeRegistry(registry, [shortest]));
      }
    } else {
      registryBuffers.push(serializeRegistry(registry, registries[registry]));
    }
  }
  // Send biomes separately - only "plains" is actually required
  registryBuffers.push(serializeRegistry("worldgen/biome", biomes));
  // Send dimensions separately. Order must match DIMENSION_* IDs in globals.h.
  registryBuffers.push(
    serializeRegistry("dimension_type", ["overworld", "the_nether", "the_end"]),
  );
  const fullRegistryBuffer = Buffer.concat(registryBuffers);

  const itemsAndBlocks = await extractItemsAndBlocks();

  const tagBlocks = (names) => [
    ...new Set(
      names
        .map((name) => itemsAndBlocks.blockRegistry[name])
        .filter((id) => id !== undefined),
    ),
  ];
  const tagItems = (names) => [
    ...new Set(
      names
        .map((name) => itemsAndBlocks.items[name])
        .filter((id) => id !== undefined),
    ),
  ];

  const woodTypes = [
    "oak",
    "spruce",
    "birch",
    "jungle",
    "acacia",
    "cherry",
    "dark_oak",
    "pale_oak",
    "mangrove",
  ];
  const leafTypes = [
    "oak",
    "spruce",
    "birch",
    "jungle",
    "acacia",
    "cherry",
    "dark_oak",
    "pale_oak",
    "mangrove",
    "azalea",
    "flowering_azalea",
  ];

  const tagBuffer = serializeTags({
    fluid: {
      // Water and lava, both flowing and still states
      water: [1, 2],
      lava: [3, 4],
    },
    block: {
      "mineable/pickaxe": tagBlocks([
        "stone",
        "end_stone",
        "granite",
        "polished_granite",
        "diorite",
        "polished_diorite",
        "andesite",
        "polished_andesite",
        "cobblestone",
        "cobblestone_slab",
        "cobblestone_stairs",
        "sandstone",
        "chiseled_sandstone",
        "cut_sandstone",
        "sandstone_slab",
        "calcite",
        "terracotta",
        "white_terracotta",
        "orange_terracotta",
        "yellow_terracotta",
        "brown_terracotta",
        "red_terracotta",
        "light_gray_terracotta",
        "ice",
        "packed_ice",
        "blue_ice",
        "magma_block",
        "furnace",
        "dispenser",
        "piston",
        "sticky_piston",
        "powered_rail",
        "detector_rail",
        "coal_ore",
        "deepslate_coal_ore",
        "iron_ore",
        "deepslate_iron_ore",
        "copper_ore",
        "gold_ore",
        "deepslate_gold_ore",
        "redstone_ore",
        "diamond_ore",
        "emerald_ore",
        "lapis_ore",
        "deepslate_lapis_ore",
        "nether_gold_ore",
        "nether_quartz_ore",
        "ancient_debris",
        "coal_block",
        "copper_block",
        "iron_block",
        "gold_block",
        "diamond_block",
        "redstone_block",
        "lapis_block",
        "netherrack",
        "basalt",
        "blackstone",
        "gilded_blackstone",
        "nether_bricks",
        "cracked_nether_bricks",
        "stone_bricks",
        "mossy_stone_bricks",
        "cracked_stone_bricks",
        "chiseled_stone_bricks",
        "iron_bars",
        "end_portal_frame",
        "crimson_nylium",
        "warped_nylium",
        "obsidian",
        "smooth_sandstone",
        "smooth_sandstone_stairs",
        "smooth_sandstone_slab",
        "sandstone_wall",
        "cobblestone_wall",
        "white_glazed_terracotta",
        "lantern",
        "bell",
        "stone_pressure_plate",
      ]),
      "mineable/axe": tagBlocks([
        "composter",
        "chest",
        "crafting_table",
        "note_block",
        "bookshelf",
        "bamboo_block",
        "stripped_bamboo_block",
        "bamboo_planks",
        "bamboo_mosaic",
        "crimson_stem",
        "warped_stem",
        ...woodTypes.flatMap((type) => [
          `${type}_log`,
          `${type}_wood`,
          `${type}_planks`,
          `${type}_slab`,
          `${type}_stairs`,
          `${type}_door`,
          `${type}_trapdoor`,
          `${type}_fence`,
          `stripped_${type}_log`,
          `stripped_${type}_wood`,
        ]),
        "ladder",
        "campfire",
        "oak_pressure_plate",
        "spruce_pressure_plate",
        "acacia_pressure_plate",
      ]),
      "mineable/shovel": tagBlocks([
        "grass_block",
        "snowy_grass_block",
        "dirt",
        "coarse_dirt",
        "podzol",
        "mycelium",
        "mud",
        "farmland",
        "sand",
        "red_sand",
        "suspicious_sand",
        "gravel",
        "suspicious_gravel",
        "snow",
        "snow_block",
        "soul_sand",
        "soul_soil",
        "mangrove_roots",
        "muddy_mangrove_roots",
        "dirt_path",
      ]),
      "mineable/hoe": tagBlocks([
        ...leafTypes.map((type) => `${type}_leaves`),
        "nether_wart_block",
        "warped_wart_block",
        "shroomlight",
        "sponge",
        "wet_sponge",
        "moss_carpet",
        "tall_grass",
        "large_fern",
      ]),
      needs_stone_tool: tagBlocks([
        "iron_ore",
        "deepslate_iron_ore",
        "copper_ore",
        "lapis_ore",
        "deepslate_lapis_ore",
      ]),
      needs_iron_tool: tagBlocks([
        "gold_ore",
        "deepslate_gold_ore",
        "redstone_ore",
        "diamond_ore",
        "emerald_ore",
      ]),
      needs_diamond_tool: tagBlocks(["obsidian", "ancient_debris"]),
      leaves: tagBlocks(leafTypes.map((type) => `${type}_leaves`)),
      climbable: tagBlocks([
        "ladder",
        "vine",
        "twisting_vines",
        "weeping_vines",
        "scaffolding",
      ]),
    },
    item: {
      planks: tagItems([
        "oak_planks",
        "spruce_planks",
        "birch_planks",
        "jungle_planks",
        "acacia_planks",
        "cherry_planks",
        "dark_oak_planks",
        "pale_oak_planks",
        "mangrove_planks",
        "bamboo_planks",
      ]),
    },
  });

  const networkBlockPalette = toVarIntBuffer(
    Object.values(itemsAndBlocks.palette),
  );

  // Build trapdoor state table and block-to-row mapping
  // Map palette INDEX to row index in trapdoor_state_rows
  const paletteEntries = Object.keys(itemsAndBlocks.palette);
  const trapdoorRowMap = [];
  for (let i = 0; i < paletteEntries.length; i++) trapdoorRowMap.push(0);
  const trapdoorRows = [];
  for (let paletteIdx = 0; paletteIdx < paletteEntries.length; paletteIdx++) {
    const name = paletteEntries[paletteIdx];
    const row = itemsAndBlocks.trapdoorStateTables[name];
    if (row && row.length === 16) {
      trapdoorRowMap[paletteIdx] = trapdoorRows.length;
      trapdoorRows.push(row);
    }
  }

  // Build stair state table and block-to-row mapping
  // Map palette INDEX to row index in stair_state_rows
  const stairRowMap = [];
  for (let i = 0; i < paletteEntries.length; i++) stairRowMap.push(0);
  const stairRows = [];
  for (let paletteIdx = 0; paletteIdx < paletteEntries.length; paletteIdx++) {
    const name = paletteEntries[paletteIdx];
    const row = itemsAndBlocks.stairStateTables[name];
    if (row && row.length === 8) {
      stairRowMap[paletteIdx] = stairRows.length;
      stairRows.push(row);
    }
  }

  // Build door state table and block-to-row mapping
  // Map palette INDEX to row index in door_state_rows
  const doorRowMap = [];
  for (let i = 0; i < paletteEntries.length; i++) doorRowMap.push(0);
  const doorRows = [];
  for (let paletteIdx = 0; paletteIdx < paletteEntries.length; paletteIdx++) {
    const name = paletteEntries[paletteIdx];
    const row = itemsAndBlocks.doorStateTables[name];
    if (row && row.length === 32) {
      doorRowMap[paletteIdx] = doorRows.length;
      doorRows.push(row);
    }
  }

  // Build fence state table and block-to-row mapping
  const fenceRowMap = [];
  for (let i = 0; i < paletteEntries.length; i++) fenceRowMap.push(255);
  const fenceRows = [];
  for (let paletteIdx = 0; paletteIdx < paletteEntries.length; paletteIdx++) {
    const name = paletteEntries[paletteIdx];
    const row = itemsAndBlocks.fenceStateTables[name];
    if (row && row.length === 16) {
      fenceRowMap[paletteIdx] = fenceRows.length;
      fenceRows.push(row);
    }
  }

  // Build horizontal-facing state table and block-to-row mapping
  const horizontalRowMap = [];
  for (let i = 0; i < paletteEntries.length; i++) horizontalRowMap.push(255);
  const horizontalRows = [];
  for (let paletteIdx = 0; paletteIdx < paletteEntries.length; paletteIdx++) {
    const name = paletteEntries[paletteIdx];
    const row = itemsAndBlocks.horizontalStateTables[name];
    if (row && row.length === 4) {
      horizontalRowMap[paletteIdx] = horizontalRows.length;
      horizontalRows.push(row);
    }
  }

  const sourceCode = `\\
#include <stdint.h>
#include "registries.h"

// Binary contents of required "Registry Data" packets
uint8_t registries_bin[] = {
${toCArray(fullRegistryBuffer)}
};
// Binary contents of "Update Tags" packets
uint8_t tags_bin[] = {
${toCArray(tagBuffer)}
};

// Block palette
uint16_t block_palette[] = { ${Object.values(itemsAndBlocks.palette).join(", ")} };
// Block palette as VarInt buffer
uint8_t network_block_palette[] = {
${toCArray(networkBlockPalette)}
};

// Block-to-item mapping
uint16_t B_to_I[] = { ${itemsAndBlocks.mappingWithOverrides.join(", ")} };
// Item-to-block mapping
uint16_t I_to_B (uint16_t item) {
  switch (item) {
    ${itemsAndBlocks.mapping.map((c, i) => (c ? `case ${c}: return ${i};\n    ` : "")).join("")}
    default: break;
  }
  return 0;
}

// Trapdoor state IDs: [block_palette_index][16 states]
// State layout: facing(n,s,w,e) × half(bottom,top) × open(false,true), non-waterlogged
extern const uint8_t trapdoor_block_to_row[];
extern const uint16_t trapdoor_state_rows[][16];
const uint8_t trapdoor_block_to_row[] = { ${trapdoorRowMap.join(", ")} };
const uint16_t trapdoor_state_rows[${trapdoorRows.length}][16] = {
  ${trapdoorRows.map((r) => `{ ${r.join(", ")} }`).join(",\n  ")}
};

// Stair state IDs: [block_palette_index][8 states]
// State layout: facing(n,s,w,e) × half(bottom,top), straight shape, non-waterlogged
extern const uint8_t stair_block_to_row[];
extern const uint16_t stair_state_rows[][8];
const uint8_t stair_block_to_row[] = { ${stairRowMap.join(", ")} };
const uint16_t stair_state_rows[${stairRows.length}][8] = {
  ${stairRows.map((r) => `{ ${r.join(", ")} }`).join(",\n  ")}
};

// Door state IDs: [block_palette_index][32 states]
// State layout: facing(n,s,w,e) × half(upper,lower) × hinge(left,right) × open(true,false), powered=false
extern const uint8_t door_block_to_row[];
extern const uint16_t door_state_rows[][32];
const uint8_t door_block_to_row[] = { ${doorRowMap.join(", ")} };
const uint16_t door_state_rows[${doorRows.length}][32] = {
  ${doorRows.map((r) => `{ ${r.join(", ")} }`).join(",\n  ")}
};

// Fence state IDs: [block_palette_index][16 states], mask bits north/east/south/west
extern const uint8_t fence_block_to_row[];
extern const uint16_t fence_state_rows[][16];
const uint8_t fence_block_to_row[] = { ${fenceRowMap.join(", ")} };
const uint16_t fence_state_rows[${fenceRows.length}][16] = {
  ${fenceRows.map((r) => `{ ${r.join(", ")} }`).join(",\n  ")}
};

// Horizontal facing state IDs: [block_palette_index][4 states], table order north/south/west/east
extern const uint8_t horizontal_block_to_row[];
extern const uint16_t horizontal_state_rows[][4];
const uint8_t horizontal_block_to_row[] = { ${horizontalRowMap.join(", ")} };
const uint16_t horizontal_state_rows[${horizontalRows.length}][4] = {
  ${horizontalRows.map((r) => `{ ${r.join(", ")} }`).join(",\n  ")}
};`;

  const headerCode = `
#ifndef H_REGISTRIES
#define H_REGISTRIES

#include <stdint.h>

// Binary packet data (${fullRegistryBuffer.length + tagBuffer.length} bytes total)
extern uint8_t registries_bin[${fullRegistryBuffer.length}];
extern uint8_t tags_bin[${tagBuffer.length}];

extern uint16_t block_palette[${paletteEntries.length}]; // Block palette
extern uint8_t network_block_palette[${networkBlockPalette.length}]; // Block palette as VarInt buffer
extern uint16_t B_to_I[${paletteEntries.length}]; // Block-to-item mapping
uint16_t I_to_B (uint16_t item); // Item-to-block mapping

// Trapdoor state lookup (generated from registry data)
extern const uint8_t trapdoor_block_to_row[${paletteEntries.length}];
extern const uint16_t trapdoor_state_rows[][16];

// Stair state lookup (generated from registry data)
extern const uint8_t stair_block_to_row[${paletteEntries.length}];
extern const uint16_t stair_state_rows[][8];

// Door state lookup (generated from registry data)
extern const uint8_t door_block_to_row[${paletteEntries.length}];
extern const uint16_t door_state_rows[][32];

// Fence state lookup (generated from registry data)
extern const uint8_t fence_block_to_row[${paletteEntries.length}];
extern const uint16_t fence_state_rows[][16];

// Horizontal facing state lookup (generated from registry data)
extern const uint8_t horizontal_block_to_row[${paletteEntries.length}];
extern const uint16_t horizontal_state_rows[][4];

// Block identifiers
${Object.keys(itemsAndBlocks.palette)
  .map((c, i) => `#define B_${c} ${i}`)
  .join("\n")}

// Item identifiers
${Object.entries(itemsAndBlocks.items)
  .map((c) => `#define I_${c[0]} ${c[1]}`)
  .join("\n")}

// Biome identifiers
${biomes.map((c, i) => `#define W_${c} ${i}`).join("\n")}

// Damage type identifiers
${registries["damage_type"].map((c, i) => `#define D_${c} ${i}`).join("\n")}

#endif
`;

  await fs.writeFile(outputPath, sourceCode);
  await fs.writeFile(headerPath, headerCode);
  console.log("Done. Wrote to `registries.c` and `registries.h`");
}

convert().catch(console.error);
