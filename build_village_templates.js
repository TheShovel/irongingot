#!/usr/bin/env node
"use strict";

const fs = require("fs");
const path = require("path");
const zlib = require("zlib");

const ROOT = __dirname;
const STRUCTURE_ROOT = path.join(
  ROOT,
  "notchian/generated/data/minecraft/structure",
);
const OUT_H = path.join(ROOT, "include/generated_village_templates.h");
const OUT_C = path.join(ROOT, "src/generated_village_templates.c");
const REGISTRY_H = path.join(ROOT, "include/registries.h");

const styles = ["plains", "desert", "savanna", "taiga", "snowy"];
const professions = [
  "farmer",
  "librarian",
  "cleric",
  "armorer",
  "butcher",
  "cartographer",
  "fisherman",
  "fletcher",
  "leatherworker",
  "mason",
  "shepherd",
  "toolsmith",
  "weaponsmith",
];

// Preferred vanilla template for each profession/style. These are real paths in
// data/minecraft/structure/village/<style>/houses from the vanilla server JAR.
const candidates = {
  plains: {
    farmer: ["plains_large_farm_1", "plains_small_farm_1"],
    librarian: ["plains_library_1", "plains_library_2"],
    cleric: ["plains_temple_3", "plains_temple_4"],
    armorer: ["plains_armorer_house_1"],
    butcher: ["plains_butcher_shop_1", "plains_butcher_shop_2"],
    cartographer: ["plains_cartographer_1"],
    fisherman: ["plains_fisher_cottage_1"],
    fletcher: ["plains_fletcher_house_1"],
    leatherworker: ["plains_tannery_1"],
    mason: ["plains_masons_house_1"],
    shepherd: ["plains_shepherds_house_1"],
    toolsmith: ["plains_tool_smith_1"],
    weaponsmith: ["plains_weaponsmith_1"],
  },
  desert: {
    farmer: ["desert_large_farm_1", "desert_farm_1", "desert_farm_2"],
    librarian: ["desert_library_1"],
    cleric: ["desert_temple_1", "desert_temple_2"],
    armorer: ["desert_armorer_1"],
    butcher: ["desert_butcher_shop_1"],
    cartographer: ["desert_cartographer_house_1"],
    fisherman: ["desert_fisher_1"],
    fletcher: ["desert_fletcher_house_1"],
    leatherworker: ["desert_tannery_1"],
    mason: ["desert_mason_1"],
    shepherd: ["desert_shepherd_house_1"],
    toolsmith: ["desert_tool_smith_1"],
    weaponsmith: ["desert_weaponsmith_1"],
  },
  savanna: {
    farmer: [
      "savanna_large_farm_1",
      "savanna_large_farm_2",
      "savanna_small_farm",
    ],
    librarian: ["savanna_library_1"],
    cleric: ["savanna_temple_1", "savanna_temple_2"],
    armorer: ["savanna_armorer_1"],
    butcher: ["savanna_butchers_shop_1", "savanna_butchers_shop_2"],
    cartographer: ["savanna_cartographer_1"],
    fisherman: ["savanna_fisher_cottage_1"],
    fletcher: ["savanna_fletcher_house_1"],
    leatherworker: ["savanna_tannery_1"],
    mason: ["savanna_mason_1"],
    shepherd: ["savanna_shepherd_1"],
    toolsmith: ["savanna_tool_smith_1"],
    weaponsmith: ["savanna_weaponsmith_1", "savanna_weaponsmith_2"],
  },
  taiga: {
    farmer: ["taiga_large_farm_1", "taiga_large_farm_2", "taiga_small_farm_1"],
    librarian: ["taiga_library_1"],
    cleric: ["taiga_temple_1"],
    armorer: ["taiga_armorer_house_1", "taiga_armorer_2"],
    butcher: ["taiga_butcher_shop_1"],
    cartographer: ["taiga_cartographer_house_1"],
    fisherman: ["taiga_fisher_cottage_1"],
    fletcher: ["taiga_fletcher_house_1"],
    leatherworker: ["taiga_tannery_1"],
    mason: ["taiga_masons_house_1"],
    shepherd: ["taiga_shepherds_house_1"],
    toolsmith: ["taiga_tool_smith_1"],
    weaponsmith: ["taiga_weaponsmith_1", "taiga_weaponsmith_2"],
  },
  snowy: {
    farmer: ["snowy_farm_1", "snowy_farm_2"],
    librarian: ["snowy_library_1"],
    cleric: ["snowy_temple_1"],
    armorer: ["snowy_armorer_house_1", "snowy_armorer_house_2"],
    butcher: ["snowy_butchers_shop_1", "snowy_butchers_shop_2"],
    cartographer: ["snowy_cartographer_house_1"],
    fisherman: ["snowy_fisher_cottage"],
    fletcher: ["snowy_fletcher_house_1"],
    leatherworker: ["snowy_tannery_1"],
    mason: ["snowy_masons_house_1", "snowy_masons_house_2"],
    shepherd: ["snowy_shepherds_house_1"],
    toolsmith: ["snowy_tool_smith_1"],
    weaponsmith: ["snowy_weapon_smith_1"],
  },
};

function readRegistry() {
  const map = new Map();
  if (!fs.existsSync(REGISTRY_H)) return map;
  const text = fs.readFileSync(REGISTRY_H, "utf8");
  for (const match of text.matchAll(/^#define B_([A-Za-z0-9_]+)\s+(\d+)$/gm)) {
    map.set(match[1], Number(match[2]));
  }
  return map;
}

class NbtReader {
  constructor(buf) {
    this.buf = buf;
    this.off = 0;
  }
  u8() {
    return this.buf[this.off++];
  }
  i8() {
    const v = this.buf.readInt8(this.off);
    this.off += 1;
    return v;
  }
  i16() {
    const v = this.buf.readInt16BE(this.off);
    this.off += 2;
    return v;
  }
  i32() {
    const v = this.buf.readInt32BE(this.off);
    this.off += 4;
    return v;
  }
  i64() {
    const hi = this.buf.readInt32BE(this.off);
    const lo = this.buf.readUInt32BE(this.off + 4);
    this.off += 8;
    return (BigInt(hi) << 32n) | BigInt(lo);
  }
  f32() {
    const v = this.buf.readFloatBE(this.off);
    this.off += 4;
    return v;
  }
  f64() {
    const v = this.buf.readDoubleBE(this.off);
    this.off += 8;
    return v;
  }
  str() {
    const len = this.buf.readUInt16BE(this.off);
    this.off += 2;
    const s = this.buf.toString("utf8", this.off, this.off + len);
    this.off += len;
    return s;
  }
  payload(type) {
    switch (type) {
      case 1:
        return this.i8();
      case 2:
        return this.i16();
      case 3:
        return this.i32();
      case 4:
        return this.i64();
      case 5:
        return this.f32();
      case 6:
        return this.f64();
      case 7: {
        const len = this.i32();
        const out = this.buf.subarray(this.off, this.off + len);
        this.off += len;
        return out;
      }
      case 8:
        return this.str();
      case 9: {
        const child = this.u8();
        const len = this.i32();
        const arr = [];
        for (let i = 0; i < len; i++) arr.push(this.payload(child));
        return arr;
      }
      case 10: {
        const obj = {};
        while (true) {
          const t = this.u8();
          if (t === 0) break;
          const name = this.str();
          obj[name] = this.payload(t);
        }
        return obj;
      }
      case 11: {
        const len = this.i32();
        const arr = [];
        for (let i = 0; i < len; i++) arr.push(this.i32());
        return arr;
      }
      case 12: {
        const len = this.i32();
        const arr = [];
        for (let i = 0; i < len; i++) arr.push(this.i64());
        return arr;
      }
      default:
        throw new Error(`Unsupported NBT tag ${type} at offset ${this.off}`);
    }
  }
  root() {
    const type = this.u8();
    if (type !== 10) throw new Error(`Expected root compound, got ${type}`);
    this.str();
    return this.payload(type);
  }
}

function readNbt(file) {
  const raw = fs.readFileSync(file);
  const buf = raw[0] === 0x1f && raw[1] === 0x8b ? zlib.gunzipSync(raw) : raw;
  return new NbtReader(buf).root();
}

const dirMap = { north: 0, east: 1, south: 2, west: 3 };
const prefixOrder = [
  "oak",
  "spruce",
  "birch",
  "jungle",
  "acacia",
  "dark_oak",
  "mangrove",
  "cherry",
  "pale_oak",
];

function pick(reg, ...names) {
  for (const n of names) if (reg.has(n)) return reg.get(n);
  return undefined;
}

function prefixOf(name) {
  for (const p of prefixOrder) if (name.startsWith(`${p}_`)) return p;
  return "oak";
}

function mapBlock(state, reg, missing) {
  let name = String(state.Name || "minecraft:air").replace("minecraft:", "");
  const props = state.Properties || {};

  if (
    name === "structure_void" ||
    name === "jigsaw" ||
    name === "structure_block"
  )
    return 0xffff;
  if (name === "air" || name === "cave_air" || name === "void_air")
    return pick(reg, "air") ?? 0;
  if (name === "grass_path") name = "dirt_path";
  if (name === "redstone_wall_torch") name = "redstone_torch";
  if (name === "trapped_chest") name = "chest";
  if (name === "water_cauldron") name = "cauldron";
  if (name === "lava_cauldron") name = "cauldron";
  if (name === "powder_snow_cauldron") name = "cauldron";
  if (name.endsWith("_crop")) name = name.replace(/_crop$/, "");

  if (name === "wheat" && props.age && reg.has(`wheat_${props.age}`))
    return reg.get(`wheat_${props.age}`);

  // Handle wall_torch with horizontal facing encoding
  if (name === "wall_torch" && reg.has("wall_torch") && props.facing) {
    const base = reg.get("wall_torch");
    const horizMap = { north: 0, south: 1, west: 2, east: 3 };
    const tableIdx = horizMap[props.facing] ?? 0;
    return 0x8000 | base | (tableIdx << 9);
  }

  // Handle lectern with horizontal facing encoding
  if (name === "lectern" && reg.has("lectern") && props.facing) {
    const base = reg.get("lectern");
    const horizMap = { north: 0, south: 1, west: 2, east: 3 };
    const tableIdx = horizMap[props.facing] ?? 0;
    return 0x8000 | base | (tableIdx << 9);
  }

  // Handle beds with facing/head/occupied encoding
  if (name.endsWith("_bed") && reg.has(name) && props.facing) {
    const base = reg.get(name);
    const d = dirMap[props.facing] ?? 0;
    const head = props.part === "head" ? 1 : 0;
    const occupied = props.occupied === "true" ? 1 : 0;
    return 0x8000 | base | (d << 9) | (head << 11) | (occupied << 12);
  }

  // Handle lantern with hanging encoding
  if (name === "lantern" && reg.has("lantern") && props.hanging) {
    const base = reg.get("lantern");
    const hanging = props.hanging === "true" ? 1 : 0;
    return 0x8000 | base | (hanging << 9);
  }

  if (name.endsWith("_door") && reg.has(name)) {
    const base = reg.get(name);
    const d = dirMap[props.facing] ?? 0;
    const hinge = props.hinge === "right" ? 1 : 0;
    const open = props.open === "true" ? 1 : 0;
    const upper = props.half === "upper" ? 1 : 0;
    return (
      0x8000 | base | (d << 9) | (hinge << 12) | (open << 13) | (upper << 14)
    );
  }

  if (name === "chest" && reg.has("chest")) {
    // Chest internal facing order: north=0, south=1, west=2, east=3
    // (matches get_oriented_state_id's off[] array)
    const chestMap = { north: 0, south: 1, west: 2, east: 3 };
    const d = chestMap[props.facing] ?? 0;
    return 0x8000 | reg.get("chest") | (d << 9);
  }

  // Handle barrel with facing encoding (6 directions: north=0, east=1, south=2, west=3, up=4, down=5)
  if (name === "barrel" && reg.has("barrel") && props.facing) {
    const base = reg.get("barrel");
    const barrelMap = { north: 0, east: 1, south: 2, west: 3, up: 4, down: 5 };
    const d = barrelMap[props.facing] ?? 0;
    return 0x8000 | base | (d << 9);
  }

  // Handle stairs with facing and half
  if (name.endsWith("_stairs") && reg.has(name) && props.facing) {
    const base = reg.get(name);
    const d = dirMap[props.facing] ?? 0;
    const half = props.half === "top" ? 1 : 0;
    // Use bits: facing in 9-10, half in 11
    return 0x8000 | base | (d << 9) | (half << 11);
  }

  // Handle trapdoors with facing, half, open
  if (name.endsWith("_trapdoor") && reg.has(name) && props.facing) {
    const base = reg.get(name);
    const d = dirMap[props.facing] ?? 0;
    const half = props.half === "top" ? 1 : 0;
    const open = props.open === "true" ? 1 : 0;
    // Use bits: facing in 9-10, half in 11, open in 12
    return 0x8000 | base | (d << 9) | (half << 11) | (open << 12);
  }

  if (reg.has(name)) return reg.get(name);

  let fallback;
  const p = prefixOf(name);
  if (name.endsWith("_stairs")) {
    fallback = pick(reg, `${p}_stairs`, "cobblestone_stairs", "oak_stairs");
    if (fallback !== undefined && fallback !== 0xffff && props.facing) {
      const d = dirMap[props.facing] ?? 0;
      const half = props.half === "top" ? 1 : 0;
      return 0x8000 | fallback | (d << 9) | (half << 11);
    }
  } else if (name.endsWith("_slab"))
    fallback = pick(reg, `${p}_slab`, "cobblestone_slab", "oak_slab");
  else if (name.endsWith("_trapdoor")) {
    fallback = pick(reg, `${p}_trapdoor`, "oak_trapdoor");
    if (fallback !== undefined && fallback !== 0xffff && props.facing) {
      const d = dirMap[props.facing] ?? 0;
      const half = props.half === "top" ? 1 : 0;
      const open = props.open === "true" ? 1 : 0;
      return 0x8000 | fallback | (d << 9) | (half << 11) | (open << 12);
    }
  } else if (name.endsWith("_fence")) {
    fallback = pick(reg, `${p}_fence`, "oak_fence");
    // Fence connectivity will be computed later in compileTemplate()
    return fallback;
  } else if (name.endsWith("_fence_gate"))
    fallback = pick(reg, `${p}_fence`, "oak_fence");
  else if (name.endsWith("_wall")) fallback = pick(reg, "cobblestone");
  else if (name.endsWith("_button")) fallback = 0xffff;
  else if (name.endsWith("_pressure_plate"))
    fallback = pick(
      reg,
      `${p}_pressure_plate`,
      "stone_pressure_plate",
      "oak_pressure_plate",
    );
  else if (
    name.endsWith("_sign") ||
    name.endsWith("_wall_sign") ||
    name.endsWith("_hanging_sign") ||
    name.endsWith("_wall_hanging_sign")
  )
    fallback = 0xffff;
  else if (name.startsWith("potted_")) fallback = 0xffff;
  else if (name.endsWith("_carpet"))
    fallback = pick(reg, name, "white_carpet", "white_wool", "snow");
  else if (name.endsWith("_wool"))
    fallback = pick(reg, "white_wool", "snow_block");
  else if (name === "glass_pane" || name.endsWith("_glass_pane"))
    fallback = pick(reg, "glass_pane", "glass");
  else if (name.endsWith("_terracotta")) fallback = pick(reg, "terracotta");
  else if (name.endsWith("_bed")) fallback = pick(reg, name, "white_bed");
  else if (name === "bell") fallback = pick(reg, "gold_block");
  else if (name === "lantern") fallback = pick(reg, "torch");
  else if (name === "soul_lantern") fallback = pick(reg, "torch");
  else if (name === "campfire") fallback = pick(reg, "torch");
  else if (name === "flower_pot") fallback = 0xffff;
  else if (name === "vine") fallback = 0xffff;
  else if (name === "ladder") fallback = pick(reg, "oak_planks");
  else if (name === "iron_bars")
    fallback = pick(reg, "iron_bars", "iron_block");
  else if (name === "chain") fallback = pick(reg, "iron_bars", "iron_block");
  else if (name === "smooth_stone") fallback = pick(reg, "stone");
  else if (name === "smooth_sandstone") fallback = pick(reg, "sandstone");
  else if (name === "bricks")
    fallback = pick(reg, "stone_bricks", "cobblestone");
  else if (name === "clay") fallback = pick(reg, "terracotta", "dirt");
  else if (name === "large_fern") fallback = pick(reg, "fern", "short_grass");
  else if (name === "tall_grass") fallback = pick(reg, "short_grass");
  else if (name === "pumpkin")
    fallback = pick(reg, "hay_block", "bamboo_block");
  else if (name.endsWith("_wall_banner")) fallback = 0xffff;
  else if (name === "grass_block" && props.snowy === "true")
    fallback = pick(reg, "snowy_grass_block", "grass_block");
  // Extra village-relevant fallbacks for blocks not in our palette
  else if (
    name === "melon_stem" ||
    name === "pumpkin_stem" ||
    name === "attached_melon_stem" ||
    name === "attached_pumpkin_stem" ||
    name === "beetroots" ||
    name === "carrots" ||
    name === "potatoes"
  )
    fallback = pick(reg, "short_grass", "fern");
  else if (name === "cocoa" || name === "sweet_berry_bush")
    fallback = pick(reg, "short_grass", "fern");
  else if (name === "smooth_stone_slab")
    fallback = pick(reg, "cobblestone_slab", "oak_slab");
  else if (name === "cake") fallback = 0xffff;

  if (fallback !== undefined) return fallback;
  missing.add(name);
  return 0xffff;
}

function templatePath(style, profession) {
  for (const base of candidates[style][profession]) {
    const file = path.join(
      STRUCTURE_ROOT,
      "village",
      style,
      "houses",
      `${base}.nbt`,
    );
    if (fs.existsSync(file)) return file;
  }
  return null;
}

function compileTemplate(style, profession, reg, missing) {
  const file = templatePath(style, profession);
  if (!file)
    return {
      style,
      profession,
      file: null,
      size: [0, 0, 0],
      origin: [0, 0, 0],
      blocks: [],
    };
  const nbt = readNbt(file);
  const size = nbt.size || [0, 0, 0];
  const palette = nbt.palette || [];
  const blocks = [];
  const seen = new Set();
  for (const b of nbt.blocks || []) {
    const pos = b.pos;
    const block = mapBlock(
      palette[b.state] || { Name: "minecraft:air" },
      reg,
      missing,
    );
    if (block === 0xffff) continue;
    const idx = (pos[1] * size[2] + pos[2]) * size[0] + pos[0];
    // Keep the later block if duplicate positions ever appear.
    const key = String(idx);
    if (seen.has(key)) {
      const existing = blocks.findIndex((entry) => entry.index === idx);
      if (existing >= 0) blocks.splice(existing, 1);
    }
    seen.add(key);
    blocks.push({ index: idx, block });
  }
  blocks.sort((a, b) => a.index - b.index);

  // Cache glass_pane's current block ID from the registry (avoids hardcoding)
  const glassPaneId = reg.get("glass_pane");

  // Compute fence connectivity from neighbors in the template
  const posMap = new Map();
  for (const b of blocks) {
    const idx = b.index;
    const x = idx % size[0];
    const z = Math.floor(idx / size[0]) % size[2];
    const y = Math.floor(idx / (size[0] * size[2]));
    posMap.set(`${x},${y},${z}`, b.block);
  }

  function isFenceConnectingBlock(blockVal) {
    if (blockVal === undefined || blockVal === 0xffff) return false;
    const blockId = blockVal & 0x1ff;
    if (blockId >= 99 && blockId <= 107) return true;
    if (glassPaneId !== undefined && blockId === glassPaneId) return true;
    return blockId > 0;
  }

  for (const b of blocks) {
    const blockVal = b.block;
    if (blockVal === 0xffff) continue;
    const idx = b.index;
    const x = idx % size[0];
    const z = Math.floor(idx / size[0]) % size[2];
    const y = Math.floor(idx / (size[0] * size[2]));
    const blockId = blockVal & 0x1ff;

    if (
      (blockId >= 99 && blockId <= 107) ||
      (glassPaneId !== undefined && blockId === glassPaneId)
    ) {
      let north = 0,
        east = 0,
        south = 0,
        west = 0;
      // Check neighbors (Minecraft coords: -z = north, +z = south, +x = east, -x = west)
      if (isFenceConnectingBlock(posMap.get(`${x},${y},${z - 1}`))) north = 1;
      if (isFenceConnectingBlock(posMap.get(`${x + 1},${y},${z}`))) east = 1;
      if (isFenceConnectingBlock(posMap.get(`${x},${y},${z + 1}`))) south = 1;
      if (isFenceConnectingBlock(posMap.get(`${x - 1},${y},${z}`))) west = 1;
      // Encode: 0x8000 | blockId | (north << 9) | (east << 10) | (south << 11) | (west << 12)
      b.block =
        0x8000 |
        blockId |
        (north << 9) |
        (east << 10) |
        (south << 11) |
        (west << 12);
    }
  }

  return {
    style,
    profession,
    file,
    size,
    origin: [Math.floor(size[0] / 2), 0, Math.floor(size[2] / 2)],
    blocks,
  };
}

function cIdent(style, profession) {
  return `village_template_${style}_${profession}`;
}

function emit(templates, missing) {
  fs.mkdirSync(path.dirname(OUT_H), { recursive: true });
  fs.mkdirSync(path.dirname(OUT_C), { recursive: true });

  const h = `#ifndef H_GENERATED_VILLAGE_TEMPLATES\n#define H_GENERATED_VILLAGE_TEMPLATES\n\n#include <stdint.h>\n\n#define VILLAGE_TEMPLATE_STYLE_COUNT ${styles.length}\n#define VILLAGE_TEMPLATE_PROFESSION_COUNT ${professions.length}\n#define VILLAGE_TEMPLATE_NONE 0xFFFF\n\ntypedef struct {\n  uint16_t index;\n  uint16_t block;\n} VillageTemplateBlock;\n\ntypedef struct {\n  uint8_t size_x;\n  uint8_t size_y;\n  uint8_t size_z;\n  uint8_t origin_x;\n  uint8_t origin_y;\n  uint8_t origin_z;\n  uint16_t block_count;\n  const VillageTemplateBlock *blocks;\n} VillageTemplate;\n\nextern const VillageTemplate village_templates[VILLAGE_TEMPLATE_STYLE_COUNT][VILLAGE_TEMPLATE_PROFESSION_COUNT];\nuint8_t villageTemplateExists(uint8_t style, uint8_t profession);\nuint16_t getVillageTemplateBlockAt(uint8_t style, uint8_t profession, int dx, int dy, int dz);\n\n#endif\n`;

  const chunks = [];
  chunks.push(`#include "generated_village_templates.h"\n\n`);
  for (const row of templates) {
    for (const t of row) {
      const ident = cIdent(t.style, t.profession);
      if (t.blocks.length === 0) {
        chunks.push(
          `static const VillageTemplateBlock ${ident}_blocks[1] = {{0, 0}};\n`,
        );
        continue;
      }
      chunks.push(`// ${t.file ? path.relative(ROOT, t.file) : "missing"}\n`);
      chunks.push(`static const VillageTemplateBlock ${ident}_blocks[] = {\n`);
      for (let i = 0; i < t.blocks.length; i++) {
        const b = t.blocks[i];
        chunks.push(
          `  {${b.index}, ${b.block}}${i + 1 === t.blocks.length ? "" : ","}\n`,
        );
      }
      chunks.push(`};\n`);
    }
  }

  chunks.push(
    `\nconst VillageTemplate village_templates[VILLAGE_TEMPLATE_STYLE_COUNT][VILLAGE_TEMPLATE_PROFESSION_COUNT] = {\n`,
  );
  for (const row of templates) {
    chunks.push(`  {\n`);
    for (const t of row) {
      const ident = cIdent(t.style, t.profession);
      chunks.push(
        `    {${t.size[0]}, ${t.size[1]}, ${t.size[2]}, ${t.origin[0]}, ${t.origin[1]}, ${t.origin[2]}, ${t.blocks.length}, ${ident}_blocks},\n`,
      );
    }
    chunks.push(`  },\n`);
  }
  chunks.push(`};\n\n`);
  chunks.push(
    `uint8_t villageTemplateExists(uint8_t style, uint8_t profession) {\n`,
  );
  chunks.push(
    `  if (style >= VILLAGE_TEMPLATE_STYLE_COUNT || profession >= VILLAGE_TEMPLATE_PROFESSION_COUNT) return 0;\n`,
  );
  chunks.push(
    `  return village_templates[style][profession].block_count > 0;\n`,
  );
  chunks.push(`}\n\n`);
  chunks.push(
    `uint16_t getVillageTemplateBlockAt(uint8_t style, uint8_t profession, int dx, int dy, int dz) {\n`,
  );
  chunks.push(
    `  if (style >= VILLAGE_TEMPLATE_STYLE_COUNT || profession >= VILLAGE_TEMPLATE_PROFESSION_COUNT) return VILLAGE_TEMPLATE_NONE;\n`,
  );
  chunks.push(
    `  const VillageTemplate *t = &village_templates[style][profession];\n`,
  );
  chunks.push(`  if (t->block_count == 0) return VILLAGE_TEMPLATE_NONE;\n`);
  chunks.push(`  int lx = dx + (int)t->origin_x;\n`);
  chunks.push(`  int ly = dy + (int)t->origin_y;\n`);
  chunks.push(`  int lz = dz + (int)t->origin_z;\n`);
  chunks.push(
    `  if (lx < 0 || ly < 0 || lz < 0 || lx >= t->size_x || ly >= t->size_y || lz >= t->size_z) return VILLAGE_TEMPLATE_NONE;\n`,
  );
  chunks.push(
    `  uint16_t idx = (uint16_t)((ly * t->size_z + lz) * t->size_x + lx);\n`,
  );
  chunks.push(`  int lo = 0, hi = (int)t->block_count - 1;\n`);
  chunks.push(`  while (lo <= hi) {\n`);
  chunks.push(`    int mid = (lo + hi) >> 1;\n`);
  chunks.push(`    uint16_t cur = t->blocks[mid].index;\n`);
  chunks.push(`    if (cur == idx) return t->blocks[mid].block;\n`);
  chunks.push(`    if (cur < idx) lo = mid + 1; else hi = mid - 1;\n`);
  chunks.push(`  }\n`);
  chunks.push(`  return VILLAGE_TEMPLATE_NONE;\n`);
  chunks.push(`}\n`);

  fs.writeFileSync(OUT_H, h);
  fs.writeFileSync(OUT_C, chunks.join(""));

  let loaded = 0;
  for (const row of templates)
    for (const t of row) if (t.blocks.length > 0) loaded++;
  console.log(
    `Generated ${loaded}/${styles.length * professions.length} village templates.`,
  );
  if (missing.size > 0) {
    const names = [...missing].sort();
    console.log(
      `Mapped ${names.length} unsupported block names to air/void. First entries: ${names.slice(0, 30).join(", ")}`,
    );
  }
}

function main() {
  const reg = readRegistry();
  const missing = new Set();
  const templates = styles.map((style) =>
    professions.map((profession) =>
      compileTemplate(style, profession, reg, missing),
    ),
  );
  emit(templates, missing);
}

main();
