#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "globals.h"

#ifdef SYNC_WORLD_TO_DISK

#ifdef ESP_PLATFORM
  #include "esp_littlefs.h"
  #define FILE_PATH "/littlefs/world.json"
#else
  #include <stdio.h>
  #define FILE_PATH "world.json"
#endif

#include "tools.h"
#include "registries.h"
#include "procedures.h"
#include "serialize.h"
#include "special_block.h"
#include "terminal_ui.h"
#include "worldgen.h"
#include "../third_party/cjson/cJSON.h"

int64_t last_disk_sync_time = 0;

static void logSerializerErrno(const char *message) {
  terminal_ui_log("%s: %s", message, strerror(errno));
}

static cJSON *serializeBlockChanges(void) {
  cJSON *arr = cJSON_CreateArray();
  if (!arr) return NULL;

  for (int i = 0; i < block_changes_count; i++) {
    if (block_changes[i].block == 0xFF) {
      cJSON_AddItemToArray(arr, cJSON_CreateNull());
      continue;
    }

    cJSON *obj = cJSON_CreateObject();
    if (!obj) { cJSON_Delete(arr); return NULL; }

    cJSON_AddNumberToObject(obj, "x", block_changes[i].x);
    cJSON_AddNumberToObject(obj, "y", block_changes[i].y);
    cJSON_AddNumberToObject(obj, "z", block_changes[i].z);
    cJSON_AddNumberToObject(obj, "block", block_changes[i].block);
    cJSON_AddNumberToObject(obj, "dimension", block_changes[i].dimension);

    cJSON_AddItemToArray(arr, obj);

    // Chest inventory slots store raw byte-packed data; serialize as hex
    if (block_changes[i].block == B_chest) {
      int slots_end = i + 14;
      if (slots_end < block_changes_count) {
        size_t raw_bytes = 14 * sizeof(BlockChange);
        uint8_t *raw_data = (uint8_t *)&block_changes[i + 1];
        char *hex = (char *)malloc(raw_bytes * 2 + 1);
        if (hex) {
          for (size_t b = 0; b < raw_bytes; b++) {
            sprintf(hex + b * 2, "%02x", raw_data[b]);
          }
          hex[raw_bytes * 2] = '\0';
          cJSON_AddStringToObject(obj, "slots", hex);
          free(hex);
        }
      }
      // Write null placeholders for inventory slots to preserve indices
      for (int j = 1; j <= 14 && i + j < block_changes_count; j++) {
        cJSON_AddItemToArray(arr, cJSON_CreateNull());
      }
      i += 14;
    }
  }

  return arr;
}

static int deserializeBlockChanges(cJSON *arr) {
  if (!cJSON_IsArray(arr)) return 0;

  int count = cJSON_GetArraySize(arr);

  #ifdef INFINITE_BLOCK_CHANGES
    block_changes_capacity = count + 100;
    if (block_changes) free(block_changes);
    block_changes = (BlockChange *)calloc(block_changes_capacity, sizeof(BlockChange));
    if (!block_changes) return 0;
  #endif

  #ifdef INFINITE_BLOCK_CHANGES
  block_changes_count = count;
  for (int i = count; i < block_changes_capacity; i++) {
    block_changes[i].block = 0xFF;
  }
  #else
  if (count > MAX_BLOCK_CHANGES) count = MAX_BLOCK_CHANGES;
  block_changes_count = count;
  #endif

  {
    for (int i = 0; i < count; i++) {
      cJSON *obj = cJSON_GetArrayItem(arr, i);

      if (cJSON_IsNull(obj)) {
        block_changes[i].block = 0xFF;
        continue;
      }

      if (!cJSON_IsObject(obj)) {
        block_changes[i].block = 0xFF;
        continue;
      }

      cJSON *x = cJSON_GetObjectItem(obj, "x");
      cJSON *y = cJSON_GetObjectItem(obj, "y");
      cJSON *z = cJSON_GetObjectItem(obj, "z");
      cJSON *block = cJSON_GetObjectItem(obj, "block");
      cJSON *dim = cJSON_GetObjectItem(obj, "dimension");

      if (!cJSON_IsNumber(x) || !cJSON_IsNumber(y) || !cJSON_IsNumber(z) ||
          !cJSON_IsNumber(block) || !cJSON_IsNumber(dim)) {
        block_changes[i].block = 0xFF;
        continue;
      }

      block_changes[i].x = (short)cJSON_GetNumberValue(x);
      block_changes[i].y = (int16_t)cJSON_GetNumberValue(y);
      block_changes[i].z = (short)cJSON_GetNumberValue(z);
      block_changes[i].block = (uint16_t)cJSON_GetNumberValue(block);
      block_changes[i].dimension = (uint8_t)cJSON_GetNumberValue(dim);

      // Restore chest inventory from hex-encoded slots
      if ((uint16_t)cJSON_GetNumberValue(block) == B_chest) {
        cJSON *slots = cJSON_GetObjectItem(obj, "slots");
        if (cJSON_IsString(slots) && i + 14 < block_changes_capacity) {
          const char *hex = cJSON_GetStringValue(slots);
          size_t hex_len = strlen(hex);
          size_t raw_bytes = 14 * sizeof(BlockChange);
          if (hex_len == raw_bytes * 2) {
            uint8_t *raw_data = (uint8_t *)&block_changes[i + 1];
            for (size_t b = 0; b < raw_bytes; b++) {
              unsigned int byte_val;
              sscanf(hex + b * 2, "%2x", &byte_val);
              raw_data[b] = (uint8_t)byte_val;
            }
          }
        } else if (i + 14 < block_changes_capacity) {
          // No slots hex data (old format); init empty chest
          memset(&block_changes[i + 1], 0, 14 * sizeof(BlockChange));
        }
        i += 14;
      }
    }
  }

  return 1;
}

static cJSON *serializePlayerData(void) {
  cJSON *arr = cJSON_CreateArray();
  if (!arr) return NULL;

  for (int p = 0; p < MAX_PLAYERS; p++) {
    PlayerData *pd = &player_data[p];

    if (pd->uuid[0] == 0 && pd->name[0] == '\0') continue;

    cJSON *obj = cJSON_CreateObject();
    if (!obj) { cJSON_Delete(arr); return NULL; }

    cJSON *uuid_arr = cJSON_CreateArray();
    for (int i = 0; i < 16; i++) {
      cJSON_AddItemToArray(uuid_arr, cJSON_CreateNumber(pd->uuid[i]));
    }
    cJSON_AddItemToObject(obj, "uuid", uuid_arr);

    cJSON_AddStringToObject(obj, "name", pd->name);
    cJSON_AddNumberToObject(obj, "x", pd->x);
    cJSON_AddNumberToObject(obj, "y", pd->y);
    cJSON_AddNumberToObject(obj, "z", pd->z);
    cJSON_AddNumberToObject(obj, "yaw", pd->yaw);
    cJSON_AddNumberToObject(obj, "pitch", pd->pitch);
    cJSON_AddNumberToObject(obj, "grounded_y", pd->grounded_y);
    cJSON_AddNumberToObject(obj, "health", pd->health);
    cJSON_AddNumberToObject(obj, "hunger", pd->hunger);
    cJSON_AddNumberToObject(obj, "saturation", pd->saturation);
    cJSON_AddNumberToObject(obj, "hotbar", pd->hotbar);
    cJSON_AddNumberToObject(obj, "dimension", pd->dimension);
    cJSON_AddNumberToObject(obj, "flags", pd->flags);
    cJSON_AddNumberToObject(obj, "flagval_16", pd->flagval_16);
    cJSON_AddNumberToObject(obj, "flagval_8", pd->flagval_8);
    cJSON_AddNumberToObject(obj, "cursor_damage", pd->cursor_damage);
    cJSON_AddNumberToObject(obj, "xp_total", pd->xp_total);
    cJSON_AddNumberToObject(obj, "xp_level", pd->xp_level);
    cJSON_AddNumberToObject(obj, "xp_progress", pd->xp_progress);
    cJSON_AddNumberToObject(obj, "portal_valid", pd->portal_valid);
    cJSON_AddNumberToObject(obj, "last_bucket_tick", pd->last_bucket_tick);
    cJSON_AddNumberToObject(obj, "last_attack_time", (double)pd->last_attack_time);
    cJSON_AddNumberToObject(obj, "portal_ow_x", pd->portal_ow_x);
    cJSON_AddNumberToObject(obj, "portal_ow_y", pd->portal_ow_y);
    cJSON_AddNumberToObject(obj, "portal_ow_z", pd->portal_ow_z);
    cJSON_AddNumberToObject(obj, "spawn_set", pd->spawn_set);
    cJSON_AddNumberToObject(obj, "spawn_x", pd->spawn_x);
    cJSON_AddNumberToObject(obj, "spawn_y", pd->spawn_y);
    cJSON_AddNumberToObject(obj, "spawn_z", pd->spawn_z);
    cJSON_AddNumberToObject(obj, "spawn_dimension", pd->spawn_dimension);
    cJSON_AddNumberToObject(obj, "visited_next", pd->visited_next);

    cJSON *inv_arr = cJSON_CreateArray();
    for (int i = 0; i < 41; i++) {
      cJSON_AddItemToArray(inv_arr, cJSON_CreateNumber(pd->inventory_items[i]));
    }
    cJSON_AddItemToObject(obj, "inventory", inv_arr);

    cJSON *inv_cnt = cJSON_CreateArray();
    for (int i = 0; i < 41; i++) {
      cJSON_AddItemToArray(inv_cnt, cJSON_CreateNumber(pd->inventory_count[i]));
    }
    cJSON_AddItemToObject(obj, "inventory_count", inv_cnt);

    cJSON *craft_arr = cJSON_CreateArray();
    for (int i = 0; i < 9; i++) {
      cJSON_AddItemToArray(craft_arr, cJSON_CreateNumber(pd->craft_items[i]));
    }
    cJSON_AddItemToObject(obj, "craft_items", craft_arr);

    cJSON *craft_cnt = cJSON_CreateArray();
    for (int i = 0; i < 9; i++) {
      cJSON_AddItemToArray(craft_cnt, cJSON_CreateNumber(pd->craft_count[i]));
    }
    cJSON_AddItemToObject(obj, "craft_count", craft_cnt);

    cJSON *inv_dmg = cJSON_CreateArray();
    for (int i = 0; i < 41; i++) {
      cJSON_AddItemToArray(inv_dmg, cJSON_CreateNumber(pd->inventory_damage[i]));
    }
    cJSON_AddItemToObject(obj, "inventory_damage", inv_dmg);

    cJSON *craft_dmg = cJSON_CreateArray();
    for (int i = 0; i < 9; i++) {
      cJSON_AddItemToArray(craft_dmg, cJSON_CreateNumber(pd->craft_damage[i]));
    }
    cJSON_AddItemToObject(obj, "craft_damage", craft_dmg);

    cJSON *inv_uid = cJSON_CreateArray();
    for (int i = 0; i < 41; i++) {
      cJSON_AddItemToArray(inv_uid, cJSON_CreateNumber((double)pd->item_uid[i]));
    }
    cJSON_AddItemToObject(obj, "item_uid", inv_uid);

    cJSON *craft_uid = cJSON_CreateArray();
    for (int i = 0; i < 9; i++) {
      cJSON_AddItemToArray(craft_uid, cJSON_CreateNumber((double)pd->craft_uid[i]));
    }
    cJSON_AddItemToObject(obj, "craft_uid", craft_uid);

    cJSON_AddNumberToObject(obj, "cursor_uid", (double)pd->cursor_uid);

    cJSON *ec_arr = cJSON_CreateArray();
    for (int i = 0; i < 27; i++) {
      cJSON_AddItemToArray(ec_arr, cJSON_CreateNumber(pd->ender_chest_items[i]));
    }
    cJSON_AddItemToObject(obj, "ender_chest_items", ec_arr);

    cJSON *ec_cnt = cJSON_CreateArray();
    for (int i = 0; i < 27; i++) {
      cJSON_AddItemToArray(ec_cnt, cJSON_CreateNumber(pd->ender_chest_count[i]));
    }
    cJSON_AddItemToObject(obj, "ender_chest_count", ec_cnt);

    cJSON *ec_dmg = cJSON_CreateArray();
    for (int i = 0; i < 27; i++) {
      cJSON_AddItemToArray(ec_dmg, cJSON_CreateNumber(pd->ender_chest_damage[i]));
    }
    cJSON_AddItemToObject(obj, "ender_chest_damage", ec_dmg);

    cJSON *vx_arr = cJSON_CreateArray();
    for (int i = 0; i < VISITED_HISTORY; i++) {
      cJSON_AddItemToArray(vx_arr, cJSON_CreateNumber(pd->visited_x[i]));
    }
    cJSON_AddItemToObject(obj, "visited_x", vx_arr);

    cJSON *vz_arr = cJSON_CreateArray();
    for (int i = 0; i < VISITED_HISTORY; i++) {
      cJSON_AddItemToArray(vz_arr, cJSON_CreateNumber(pd->visited_z[i]));
    }
    cJSON_AddItemToObject(obj, "visited_z", vz_arr);

    cJSON_AddItemToArray(arr, obj);
  }

  return arr;
}

static int deserializePlayerData(cJSON *arr) {
  if (!cJSON_IsArray(arr)) return 0;

  int count = cJSON_GetArraySize(arr);
  if (count > MAX_PLAYERS) count = MAX_PLAYERS;

  for (int p = 0; p < count; p++) {
    cJSON *obj = cJSON_GetArrayItem(arr, p);
    if (!cJSON_IsObject(obj)) continue;

    PlayerData *pd = &player_data[p];
    memset(pd, 0, sizeof(PlayerData));

    cJSON *uuid = cJSON_GetObjectItem(obj, "uuid");
    if (cJSON_IsArray(uuid)) {
      int ucount = cJSON_GetArraySize(uuid);
      if (ucount > 16) ucount = 16;
      for (int i = 0; i < ucount; i++) {
        cJSON *u = cJSON_GetArrayItem(uuid, i);
        if (cJSON_IsNumber(u)) pd->uuid[i] = (uint8_t)cJSON_GetNumberValue(u);
      }
    }

    cJSON *name = cJSON_GetObjectItem(obj, "name");
    if (cJSON_IsString(name)) {
      strncpy(pd->name, cJSON_GetStringValue(name), 15);
      pd->name[15] = '\0';
    }

    #define READ_NUMBER(field, json_name) do { \
      cJSON *v = cJSON_GetObjectItem(obj, json_name); \
      if (cJSON_IsNumber(v)) pd->field = (typeof(pd->field))cJSON_GetNumberValue(v); \
    } while(0)

    READ_NUMBER(x, "x");
    READ_NUMBER(y, "y");
    READ_NUMBER(z, "z");
    READ_NUMBER(yaw, "yaw");
    READ_NUMBER(pitch, "pitch");
    READ_NUMBER(grounded_y, "grounded_y");
    READ_NUMBER(health, "health");
    READ_NUMBER(hunger, "hunger");
    READ_NUMBER(saturation, "saturation");
    READ_NUMBER(hotbar, "hotbar");
    READ_NUMBER(dimension, "dimension");
    READ_NUMBER(flags, "flags");
    READ_NUMBER(flagval_16, "flagval_16");
    READ_NUMBER(flagval_8, "flagval_8");
    READ_NUMBER(cursor_damage, "cursor_damage");
    READ_NUMBER(xp_total, "xp_total");
    READ_NUMBER(xp_level, "xp_level");
    READ_NUMBER(xp_progress, "xp_progress");
    READ_NUMBER(portal_valid, "portal_valid");
    READ_NUMBER(last_bucket_tick, "last_bucket_tick");
    READ_NUMBER(last_attack_time, "last_attack_time");
    READ_NUMBER(portal_ow_x, "portal_ow_x");
    READ_NUMBER(portal_ow_y, "portal_ow_y");
    READ_NUMBER(portal_ow_z, "portal_ow_z");
    READ_NUMBER(spawn_set, "spawn_set");
    READ_NUMBER(spawn_x, "spawn_x");
    READ_NUMBER(spawn_y, "spawn_y");
    READ_NUMBER(spawn_z, "spawn_z");
    READ_NUMBER(spawn_dimension, "spawn_dimension");
    READ_NUMBER(visited_next, "visited_next");

    #define READ_ARRAY(field, json_name, len) do { \
      cJSON *a = cJSON_GetObjectItem(obj, json_name); \
      if (cJSON_IsArray(a)) { \
        int alen = cJSON_GetArraySize(a); \
        if (alen > len) alen = len; \
        for (int i = 0; i < alen; i++) { \
          cJSON *v = cJSON_GetArrayItem(a, i); \
          if (cJSON_IsNumber(v)) pd->field[i] = (typeof(pd->field[0]))cJSON_GetNumberValue(v); \
        } \
      } \
    } while(0)

    READ_ARRAY(inventory_items, "inventory", 41);
    READ_ARRAY(inventory_count, "inventory_count", 41);
    READ_ARRAY(inventory_damage, "inventory_damage", 41);
    READ_ARRAY(craft_items, "craft_items", 9);
    READ_ARRAY(craft_count, "craft_count", 9);
    READ_ARRAY(craft_damage, "craft_damage", 9);
    READ_ARRAY(item_uid, "item_uid", 41);
    READ_ARRAY(craft_uid, "craft_uid", 9);
    READ_NUMBER(cursor_uid, "cursor_uid");
    READ_ARRAY(ender_chest_items, "ender_chest_items", 27);
    READ_ARRAY(ender_chest_count, "ender_chest_count", 27);
    READ_ARRAY(ender_chest_damage, "ender_chest_damage", 27);
    READ_ARRAY(visited_x, "visited_x", VISITED_HISTORY);
    READ_ARRAY(visited_z, "visited_z", VISITED_HISTORY);

    for (int i = 0; i < 41; i++) {
      uint16_t max_damage = getItemMaxDamage(pd->inventory_items[i]);
      if (pd->inventory_count[i] == 0 || max_damage == 0) pd->inventory_damage[i] = 0;
      else if (pd->inventory_damage[i] >= max_damage) pd->inventory_damage[i] = max_damage - 1;
    }
    for (int i = 0; i < 9; i++) {
      uint16_t max_damage = getItemMaxDamage(pd->craft_items[i]);
      if (pd->craft_count[i] == 0 || max_damage == 0) pd->craft_damage[i] = 0;
      else if (pd->craft_damage[i] >= max_damage) pd->craft_damage[i] = max_damage - 1;
    }
    for (int i = 0; i < 27; i++) {
      uint16_t max_damage = getItemMaxDamage(pd->ender_chest_items[i]);
      if (pd->ender_chest_count[i] == 0 || max_damage == 0) pd->ender_chest_damage[i] = 0;
      else if (pd->ender_chest_damage[i] >= max_damage) pd->ender_chest_damage[i] = max_damage - 1;
    }

    #undef READ_NUMBER
    #undef READ_ARRAY

    pd->client_fd = -1;
  }

  player_data_count = count;
  return 1;
}

static uint8_t specialBlockHasPersistedBlockChange(short x, uint8_t y, short z, uint8_t dimension) {
  for (int i = 0; i < block_changes_count; i++) {
    uint16_t b = block_changes[i].block;
    if (b == 0xFF) continue;

    uint8_t matches = (
      block_changes[i].x == x &&
      block_changes[i].y == y &&
      block_changes[i].z == z &&
      block_changes[i].dimension == dimension
    );

    if (b == B_chest || b == B_barrel) {
      if (matches) return 1;
      i += 14;
      continue;
    }

    if (is_door_block(b)) {
      if (matches) return 1;
      i += 2;
      continue;
    }

    if (is_stair_block(b) || b == B_furnace || b == B_ender_chest ||
        is_fence_block(b) || is_horizontal_facing_block(b) || b == B_lantern) {
      if (matches) return 1;
      i += 1;
      continue;
    }

    if (matches) return 1;
  }

  return 0;
}

static cJSON *serializeSpecialBlocks(void) {
  cJSON *arr = cJSON_CreateArray();
  if (!arr) return NULL;

  int sb_cap = special_blocks_capacity;
  for (int i = 0; i < sb_cap; i++) {
    if (special_blocks[i].block == SPECIAL_BLOCK_EMPTY) continue;
    if (!specialBlockHasPersistedBlockChange(
      special_blocks[i].x,
      special_blocks[i].y,
      special_blocks[i].z,
      special_blocks[i].dimension
    )) continue;

    cJSON *obj = cJSON_CreateObject();
    if (!obj) { cJSON_Delete(arr); return NULL; }

    cJSON_AddNumberToObject(obj, "x", special_blocks[i].x);
    cJSON_AddNumberToObject(obj, "y", special_blocks[i].y);
    cJSON_AddNumberToObject(obj, "z", special_blocks[i].z);
    cJSON_AddNumberToObject(obj, "state", special_blocks[i].state);
    cJSON_AddNumberToObject(obj, "block", special_blocks[i].block);
    cJSON_AddNumberToObject(obj, "dimension", special_blocks[i].dimension);

    cJSON_AddItemToArray(arr, obj);
  }

  return arr;
}

static int deserializeSpecialBlocks(cJSON *arr) {
  if (!cJSON_IsArray(arr)) return 0;

  special_block_init();

  int count = cJSON_GetArraySize(arr);
  for (int i = 0; i < count; i++) {
    cJSON *obj = cJSON_GetArrayItem(arr, i);
    if (!cJSON_IsObject(obj)) continue;

    cJSON *x = cJSON_GetObjectItem(obj, "x");
    cJSON *y = cJSON_GetObjectItem(obj, "y");
    cJSON *z = cJSON_GetObjectItem(obj, "z");
    cJSON *state = cJSON_GetObjectItem(obj, "state");
    cJSON *block = cJSON_GetObjectItem(obj, "block");
    cJSON *dim = cJSON_GetObjectItem(obj, "dimension");

    if (!cJSON_IsNumber(x) || !cJSON_IsNumber(y) || !cJSON_IsNumber(z) ||
        !cJSON_IsNumber(state) || !cJSON_IsNumber(block) || !cJSON_IsNumber(dim)) continue;

    short sx = (short)cJSON_GetNumberValue(x);
    uint8_t sy = (uint8_t)cJSON_GetNumberValue(y);
    short sz = (short)cJSON_GetNumberValue(z);
    uint8_t sdim = (uint8_t)cJSON_GetNumberValue(dim);

    if (!specialBlockHasPersistedBlockChange(sx, sy, sz, sdim)) continue;

    special_block_set_state(
      sx,
      sy,
      sz,
      sdim,
      (uint16_t)cJSON_GetNumberValue(block),
      (uint16_t)cJSON_GetNumberValue(state)
    );
  }

  return 1;
}

static cJSON *serializeMobData(void) {
  cJSON *arr = cJSON_CreateArray();
  if (!arr) return NULL;

  for (int i = 0; i < MAX_MOBS; i++) {
    MobData *mob = &mob_data[i];
    if (mob->type == 0 || (mob->data & 31) == 0) continue;

    cJSON *obj = cJSON_CreateObject();
    if (!obj) { cJSON_Delete(arr); return NULL; }

    cJSON_AddNumberToObject(obj, "index", i);
    cJSON_AddNumberToObject(obj, "type", mob->type);
    cJSON_AddNumberToObject(obj, "x", mob->x);
    cJSON_AddNumberToObject(obj, "y", mob->y);
    cJSON_AddNumberToObject(obj, "z", mob->z);
    cJSON_AddNumberToObject(obj, "move_dx", mob->move_dx);
    cJSON_AddNumberToObject(obj, "move_dz", mob->move_dz);
    cJSON_AddNumberToObject(obj, "move_dy", mob->move_dy);
    cJSON_AddNumberToObject(obj, "move_timer", mob->move_timer);
    cJSON_AddNumberToObject(obj, "anger_timer", mob->anger_timer);
    cJSON_AddNumberToObject(obj, "yaw_store", mob->yaw_store);
    cJSON_AddNumberToObject(obj, "data", mob->data);
    cJSON_AddNumberToObject(obj, "profession", mob->profession);
    cJSON_AddNumberToObject(obj, "dimension", mob->dimension);

    if (mob->type == E_VILLAGER) {
      cJSON *uses = cJSON_CreateArray();
      if (uses) {
        for (int t = 0; t < 5; t++) {
          cJSON_AddItemToArray(uses, cJSON_CreateNumber(mob_trade_uses[i][t]));
        }
        cJSON_AddItemToObject(obj, "trade_uses", uses);
      }
    }

    cJSON_AddItemToArray(arr, obj);
  }

  return arr;
}

static int deserializeMobData(cJSON *arr) {
  if (!cJSON_IsArray(arr)) return 0;

  memset(mob_data, 0, sizeof(mob_data));
  memset(mob_trade_uses, 0, sizeof(mob_trade_uses));

  int count = cJSON_GetArraySize(arr);
  for (int n = 0; n < count; n++) {
    cJSON *obj = cJSON_GetArrayItem(arr, n);
    if (!cJSON_IsObject(obj)) continue;

    cJSON *idx_json = cJSON_GetObjectItem(obj, "index");
    if (!cJSON_IsNumber(idx_json)) continue;
    int i = (int)cJSON_GetNumberValue(idx_json);
    if (i < 0 || i >= MAX_MOBS) continue;

    MobData *mob = &mob_data[i];

    #define READ_MOB_NUMBER(field, json_name) do { \
      cJSON *v = cJSON_GetObjectItem(obj, json_name); \
      if (cJSON_IsNumber(v)) mob->field = (typeof(mob->field))cJSON_GetNumberValue(v); \
    } while(0)

    READ_MOB_NUMBER(type, "type");
    READ_MOB_NUMBER(x, "x");
    READ_MOB_NUMBER(y, "y");
    READ_MOB_NUMBER(z, "z");
    READ_MOB_NUMBER(move_dx, "move_dx");
    READ_MOB_NUMBER(move_dz, "move_dz");
    READ_MOB_NUMBER(move_dy, "move_dy");
    READ_MOB_NUMBER(move_timer, "move_timer");
    READ_MOB_NUMBER(anger_timer, "anger_timer");
    READ_MOB_NUMBER(yaw_store, "yaw_store");
    READ_MOB_NUMBER(data, "data");
    READ_MOB_NUMBER(profession, "profession");
    READ_MOB_NUMBER(dimension, "dimension");

    #undef READ_MOB_NUMBER

    cJSON *uses = cJSON_GetObjectItem(obj, "trade_uses");
    if (cJSON_IsArray(uses)) {
      int ucount = cJSON_GetArraySize(uses);
      if (ucount > 5) ucount = 5;
      for (int t = 0; t < ucount; t++) {
        cJSON *v = cJSON_GetArrayItem(uses, t);
        if (cJSON_IsNumber(v)) mob_trade_uses[i][t] = (uint8_t)cJSON_GetNumberValue(v);
      }
    }
  }

  return 1;
}

static int writeWorldJson(void) {
  FILE *file = fopen(FILE_PATH, "w");
  if (!file) {
    logSerializerErrno("Failed to open world.json for writing");
    return 0;
  }

  cJSON *root = cJSON_CreateObject();
  if (!root) { fclose(file); return 0; }

  cJSON_AddNumberToObject(root, "format_version", 2);
  cJSON_AddNumberToObject(root, "world_day_time", (double)world_day_time);

  cJSON *bc = serializeBlockChanges();
  if (bc) cJSON_AddItemToObject(root, "block_changes", bc);
  else cJSON_AddArrayToObject(root, "block_changes");

  cJSON *pd = serializePlayerData();
  if (pd) cJSON_AddItemToObject(root, "players", pd);
  else cJSON_AddArrayToObject(root, "players");

  cJSON *sb = serializeSpecialBlocks();
  if (sb) cJSON_AddItemToObject(root, "special_blocks", sb);
  else cJSON_AddArrayToObject(root, "special_blocks");

  cJSON *md = serializeMobData();
  if (md) cJSON_AddItemToObject(root, "mobs", md);
  else cJSON_AddArrayToObject(root, "mobs");

  char *json = cJSON_Print(root);
  if (!json) {
    cJSON_Delete(root);
    fclose(file);
    return 0;
  }

  size_t len = strlen(json);
  size_t written = fwrite(json, 1, len, file);

  cJSON_Delete(root);
  free(json);
  fclose(file);

  if (written != len) {
    logSerializerErrno("Failed to write all data to world.json");
    return 0;
  }

  return 1;
}

int initSerializer(void) {
  last_disk_sync_time = get_program_time();

  #ifdef ESP_PLATFORM
    esp_vfs_littlefs_conf_t conf = {
      .base_path = "/littlefs",
      .partition_label = "littlefs",
      .format_if_mount_failed = true,
      .dont_mount = false
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
      terminal_ui_log("LittleFS error %d", ret);
      logSerializerErrno("Failed to mount LittleFS. Aborting.");
      return 1;
    }
  #endif

  FILE *file = fopen(FILE_PATH, "rb");
  if (!file) {
    terminal_ui_log("No \"world.json\" file found, creating one...");
    special_block_init();
    return writeWorldJson() ? 0 : 1;
  }

  terminal_ui_log("[LOAD] Opened world.json for reading");

  fseek(file, 0, SEEK_END);
  long file_size = ftell(file);
  fseek(file, 0, SEEK_SET);
  terminal_ui_log("[LOAD] File size: %ld bytes", file_size);

  char *buf = (char *)malloc((size_t)file_size + 1);
  if (!buf) {
    terminal_ui_log("Failed to allocate memory for world.json. Aborting.");
    fclose(file);
    return 1;
  }

  size_t read = fread(buf, 1, (size_t)file_size, file);
  buf[read] = '\0';
  fclose(file);

  if (read != (size_t)file_size) {
    terminal_ui_log("Short read of world.json. Aborting.");
    free(buf);
    return 1;
  }

  cJSON *root = cJSON_Parse(buf);
  free(buf);

  if (!root) {
    terminal_ui_log("Failed to parse world.json: %s", cJSON_GetErrorPtr());
    return 1;
  }

  cJSON *bc = cJSON_GetObjectItem(root, "block_changes");
  if (bc && !deserializeBlockChanges(bc)) {
    terminal_ui_log("Failed to deserialize block changes");
  }

  cJSON *pd = cJSON_GetObjectItem(root, "players");
  if (pd && !deserializePlayerData(pd)) {
    terminal_ui_log("Failed to deserialize player data");
  }

  cJSON *sb = cJSON_GetObjectItem(root, "special_blocks");
  if (sb && !deserializeSpecialBlocks(sb)) {
    terminal_ui_log("Failed to deserialize special blocks");
  } else if (!sb) {
    special_block_init();
  }

  cJSON *md = cJSON_GetObjectItem(root, "mobs");
  if (md && !deserializeMobData(md)) {
    terminal_ui_log("Failed to deserialize mob data");
  }

  // Restore world time from saved state (for persistence across restarts)
  cJSON *wdt = cJSON_GetObjectItem(root, "world_day_time");
  if (cJSON_IsNumber(wdt)) {
    world_day_time = (uint64_t)cJSON_GetNumberValue(wdt);
    world_time = (uint16_t)(world_day_time % 24000);
  }

  // Rebuild any missing special block state entries from block changes.
  // This handles upgrades from older world.json files and any edge cases
  // where block state was not serialized for fences, wall torches, etc.
  for (int i = 0; i < block_changes_count; i++) {
    if (block_changes[i].block == 0xFF) continue;
    uint16_t b = block_changes[i].block;
    short bx = block_changes[i].x;
    uint8_t by = (uint8_t)block_changes[i].y;
    short bz = block_changes[i].z;
    uint8_t bdim = block_changes[i].dimension;
    if (is_horizontal_facing_block(b) && !special_block_has_entry(bx, by, bz, bdim)) {
      // Try to recover direction from the state entry's backup (block_changes[i+1].block)
      // The torch handler stores direction+1 (1-4) in the state entry's block field.
      uint8_t recover_dir = 0;
      if (i + 1 < block_changes_count && block_changes[i + 1].block >= 1 && block_changes[i + 1].block <= 4) {
        recover_dir = (uint8_t)(block_changes[i + 1].block - 1);
      }
      special_block_set_state(bx, by, bz, bdim, b, horizontal_facing_encode_state(recover_dir));
    } else if (is_fence_block(b) && !special_block_has_entry(bx, by, bz, bdim)) {
      // Recompute fence connections from neighbors
      uint8_t _fn = 0, _fe = 0, _fs = 0, _fw = 0;
      uint16_t _fb = getBlockAt2(bx - 1, by, bz, bdim);
      if (is_fence_block(_fb) || _fb == B_cobblestone || _fb == B_stone || _fb == B_stone_bricks || (_fb >= B_oak_planks && _fb <= B_bamboo_mosaic) || _fb == B_sandstone || _fb == B_cut_sandstone || _fb == B_chiseled_sandstone || _fb == B_bedrock || _fb == B_obsidian || _fb == B_nether_bricks || _fb == B_blackstone || _fb == B_basalt || _fb == B_end_stone) _fw = 1;
      _fb = getBlockAt2(bx + 1, by, bz, bdim);
      if (is_fence_block(_fb) || _fb == B_cobblestone || _fb == B_stone || _fb == B_stone_bricks || (_fb >= B_oak_planks && _fb <= B_bamboo_mosaic) || _fb == B_sandstone || _fb == B_cut_sandstone || _fb == B_chiseled_sandstone || _fb == B_bedrock || _fb == B_obsidian || _fb == B_nether_bricks || _fb == B_blackstone || _fb == B_basalt || _fb == B_end_stone) _fe = 1;
      _fb = getBlockAt2(bx, by, bz - 1, bdim);
      if (is_fence_block(_fb) || _fb == B_cobblestone || _fb == B_stone || _fb == B_stone_bricks || (_fb >= B_oak_planks && _fb <= B_bamboo_mosaic) || _fb == B_sandstone || _fb == B_cut_sandstone || _fb == B_chiseled_sandstone || _fb == B_bedrock || _fb == B_obsidian || _fb == B_nether_bricks || _fb == B_blackstone || _fb == B_basalt || _fb == B_end_stone) _fn = 1;
      _fb = getBlockAt2(bx, by, bz + 1, bdim);
      if (is_fence_block(_fb) || _fb == B_cobblestone || _fb == B_stone || _fb == B_stone_bricks || (_fb >= B_oak_planks && _fb <= B_bamboo_mosaic) || _fb == B_sandstone || _fb == B_cut_sandstone || _fb == B_chiseled_sandstone || _fb == B_bedrock || _fb == B_obsidian || _fb == B_nether_bricks || _fb == B_blackstone || _fb == B_basalt || _fb == B_end_stone) _fs = 1;
      special_block_set_state(bx, by, bz, bdim, b, fence_encode_state(_fn, _fe, _fs, _fw));
    } else if (is_stair_block(b) && !special_block_has_entry(bx, by, bz, bdim)) {
      special_block_set_state(bx, by, bz, bdim, b, stair_encode_state(0, 0));
    } else if (b == B_furnace && !special_block_has_entry(bx, by, bz, bdim)) {
      special_block_set_state(bx, by, bz, bdim, b, furnace_encode_state(0, 0));
    } else if (is_door_block(b) && !special_block_has_entry(bx, by, bz, bdim)) {
      special_block_set_state(bx, by, bz, bdim, b, door_encode_state(0, 0, 0));
    } else if (b == B_wheat && !special_block_has_entry(bx, by, bz, bdim)) {
      special_block_set_state(bx, by, bz, bdim, b, 0);
    } else if (b == B_lantern && !special_block_has_entry(bx, by, bz, bdim)) {
      // Try to recover hanging from the state entry backup
      uint8_t rec_hang = 0;
      if (i + 1 < block_changes_count && block_changes[i + 1].block >= 1 && block_changes[i + 1].block <= 2) {
        rec_hang = (uint8_t)(block_changes[i + 1].block - 1);
      }
      special_block_set_state(bx, by, bz, bdim, b, horizontal_facing_encode_state(rec_hang));
    }
    // Skip state entries for special blocks
    if (is_stair_block(b) || b == B_furnace || b == B_ender_chest || is_fence_block(b) || is_horizontal_facing_block(b) || b == B_lantern) i += 1;
    else if (is_door_block(b)) i += 2;
    else if (b == B_chest || b == B_barrel) i += 14;
  }

  int mob_count = 0;
  for (int i = 0; i < MAX_MOBS; i++) if (mob_data[i].type != 0) mob_count++;
  terminal_ui_log("[LOAD] Loaded %d block changes, %d players, %d special blocks, %d mobs",
    block_changes_count, player_data_count, special_blocks_count, mob_count);

  cJSON_Delete(root);
  return 0;
}

void writeBlockChangesToDisk(int from, int to) {
  (void)from;
  (void)to;
  writeWorldJson();
}

void writePlayerDataToDisk(void) {
  writeWorldJson();
}

void writeDataToDiskOnInterval(void) {
  if (get_program_time() - last_disk_sync_time < DISK_SYNC_INTERVAL) return;
  last_disk_sync_time = get_program_time();
  writePlayerDataToDisk();
}

#ifdef ALLOW_CHESTS
void writeChestChangesToDisk(int chest_idx, uint8_t slot) {
  (void)chest_idx;
  (void)slot;
  writeWorldJson();
}
#endif

#endif
