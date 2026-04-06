
#include "globals.h"
#include "tools.h"
#include "registries.h"
#include "worldgen.h"
#include "procedures.h"
#include "structures.h"

static inline int abs_i(int value) {
  return value < 0 ? -value : value;
}

void setBlockIfReplaceable (short x, uint8_t y, short z, uint8_t block) {
  uint8_t target = getBlockAt(x, y, z);
  if (!isReplaceableBlock(target) && !isLeafBlock(target) && !isSaplingBlock(target)) return;
  makeBlockChange(x, y, z, block);
}

static void setTrunkColumn(short x, uint8_t y, short z, uint8_t log, uint8_t height) {
  makeBlockChange(x, y - 1, z, B_dirt);
  for (int i = 0; i < height; i++) {
    setBlockIfReplaceable(x, y + i, z, log);
  }
}

static void setRoundCanopy(short x, uint8_t y, short z, uint8_t leaves, uint8_t radius, uint8_t skip_corners) {
  for (int dx = -radius; dx <= radius; dx++) {
    for (int dz = -radius; dz <= radius; dz++) {
      int manhattan = abs_i(dx) + abs_i(dz);
      if (manhattan > radius) continue;
      if (skip_corners && manhattan == radius && abs_i(dx) == radius && abs_i(dz) == radius) continue;
      setBlockIfReplaceable(x + dx, y, z + dz, leaves);
    }
  }
}

static void placeRoundedTree(short x, uint8_t y, short z, uint8_t log, uint8_t leaves, uint8_t min_height, uint8_t height_variation) {
  uint32_t r = fast_rand();
  uint8_t height = min_height + (r % height_variation);

  setTrunkColumn(x, y, z, log, height);
  setRoundCanopy(x, y + height - 3, z, leaves, 3, 1);
  setRoundCanopy(x, y + height - 2, z, leaves, 3, 1);
  setRoundCanopy(x, y + height - 1, z, leaves, 2, 0);
  setRoundCanopy(x, y + height, z, leaves, 2, 0);
  setBlockIfReplaceable(x, y + height + 1, z, leaves);
}

static void placeConiferTree(short x, uint8_t y, short z, uint8_t log, uint8_t leaves) {
  uint8_t height = 6 + (fast_rand() % 4);
  uint8_t leaf_top = y + height;

  setTrunkColumn(x, y, z, log, height);

  for (uint8_t ly = y + 2; ly <= leaf_top; ly++) {
    int radius = 1 + (leaf_top - ly) / 2;
    for (int dx = -radius; dx <= radius; dx++) {
      for (int dz = -radius; dz <= radius; dz++) {
        int manhattan = abs_i(dx) + abs_i(dz);
        if (manhattan == 0 || manhattan > radius) continue;
        setBlockIfReplaceable(x + dx, ly, z + dz, leaves);
      }
    }
  }

  setBlockIfReplaceable(x, leaf_top + 1, z, leaves);
}

static void placeJungleTree(short x, uint8_t y, short z) {
  uint8_t height = 7 + (fast_rand() % 4);

  setTrunkColumn(x, y, z, B_jungle_log, height);
  setRoundCanopy(x, y + height - 2, z, B_jungle_leaves, 3, 0);
  setRoundCanopy(x, y + height - 1, z, B_jungle_leaves, 3, 0);
  setRoundCanopy(x, y + height, z, B_jungle_leaves, 2, 0);
  setRoundCanopy(x, y + height + 1, z, B_jungle_leaves, 2, 0);
  setBlockIfReplaceable(x, y + height + 2, z, B_jungle_leaves);
}

static void placeFlatCanopyTree(short x, uint8_t y, short z, uint8_t log, uint8_t leaves, uint8_t min_height, uint8_t height_variation) {
  uint8_t height = min_height + (fast_rand() % height_variation);
  uint8_t canopy_y = y + height - 1;

  setTrunkColumn(x, y, z, log, height);
  setRoundCanopy(x, canopy_y, z, leaves, 3, 1);
  setRoundCanopy(x, canopy_y + 1, z, leaves, 3, 1);
  setRoundCanopy(x, canopy_y + 2, z, leaves, 2, 0);
}

static void placeDenseTree(short x, uint8_t y, short z, uint8_t log, uint8_t leaves) {
  uint8_t height = 6 + (fast_rand() % 3);

  setTrunkColumn(x, y, z, log, height);
  setRoundCanopy(x, y + height - 2, z, leaves, 3, 0);
  setRoundCanopy(x, y + height - 1, z, leaves, 3, 0);
  setRoundCanopy(x, y + height, z, leaves, 2, 0);
  setBlockIfReplaceable(x, y + height + 1, z, leaves);
}

void placeSaplingStructure (short x, uint8_t y, short z, uint8_t sapling_block) {
  switch (sapling_block) {
    case B_spruce_sapling:
      placeConiferTree(x, y, z, B_spruce_log, B_spruce_leaves);
      return;

    case B_birch_sapling:
      placeRoundedTree(x, y, z, B_birch_log, B_birch_leaves, 5, 3);
      return;

    case B_jungle_sapling:
      placeJungleTree(x, y, z);
      return;

    case B_acacia_sapling:
      placeFlatCanopyTree(x, y, z, B_acacia_log, B_acacia_leaves, 5, 3);
      return;

    case B_cherry_sapling:
      placeFlatCanopyTree(x, y, z, B_cherry_log, B_cherry_leaves, 5, 3);
      return;

    case B_dark_oak_sapling:
      placeDenseTree(x, y, z, B_dark_oak_log, B_dark_oak_leaves);
      return;

    case B_pale_oak_sapling:
      placeRoundedTree(x, y, z, B_pale_oak_log, B_pale_oak_leaves, 5, 3);
      return;

    case B_mangrove_propagule:
      placeRoundedTree(x, y, z, B_mangrove_log, B_mangrove_leaves, 5, 3);
      return;

    case B_oak_sapling:
    default:
      placeRoundedTree(x, y, z, B_oak_log, B_oak_leaves, 4, 3);
      return;
  }
}

// Places an oak tree centered on the input coordinates
void placeTreeStructure (short x, uint8_t y, short z) {
  placeSaplingStructure(x, y, z, B_oak_sapling);
}
