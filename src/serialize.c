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

    cJSON_AddItemToArray(arr, obj);
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

  block_changes_count = count;

  #ifdef INFINITE_BLOCK_CHANGES
  for (int i = count; i < block_changes_capacity; i++) {
    block_changes[i].block = 0xFF;
  }
  #endif

  {
    #ifndef INFINITE_BLOCK_CHANGES
      if (count > MAX_BLOCK_CHANGES) count = MAX_BLOCK_CHANGES;
    #endif

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
    cJSON_AddNumberToObject(obj, "portal_valid", pd->portal_valid);
    cJSON_AddNumberToObject(obj, "last_bucket_tick", pd->last_bucket_tick);
    cJSON_AddNumberToObject(obj, "last_attack_time", (double)pd->last_attack_time);
    cJSON_AddNumberToObject(obj, "portal_ow_x", pd->portal_ow_x);
    cJSON_AddNumberToObject(obj, "portal_ow_y", pd->portal_ow_y);
    cJSON_AddNumberToObject(obj, "portal_ow_z", pd->portal_ow_z);
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
    READ_NUMBER(portal_valid, "portal_valid");
    READ_NUMBER(last_bucket_tick, "last_bucket_tick");
    READ_NUMBER(last_attack_time, "last_attack_time");
    READ_NUMBER(portal_ow_x, "portal_ow_x");
    READ_NUMBER(portal_ow_y, "portal_ow_y");
    READ_NUMBER(portal_ow_z, "portal_ow_z");
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
    READ_ARRAY(craft_items, "craft_items", 9);
    READ_ARRAY(craft_count, "craft_count", 9);
    READ_ARRAY(visited_x, "visited_x", VISITED_HISTORY);
    READ_ARRAY(visited_z, "visited_z", VISITED_HISTORY);

    #undef READ_NUMBER
    #undef READ_ARRAY

    pd->client_fd = -1;
  }

  player_data_count = count;
  return 1;
}

static cJSON *serializeSpecialBlocks(void) {
  cJSON *arr = cJSON_CreateArray();
  if (!arr) return NULL;

  for (int i = 0; i < MAX_SPECIAL_BLOCKS; i++) {
    if (special_blocks[i].block == SPECIAL_BLOCK_EMPTY) continue;

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

    special_block_set_state(
      (short)cJSON_GetNumberValue(x),
      (uint8_t)cJSON_GetNumberValue(y),
      (short)cJSON_GetNumberValue(z),
      (uint8_t)cJSON_GetNumberValue(dim),
      (uint16_t)cJSON_GetNumberValue(block),
      (uint16_t)cJSON_GetNumberValue(state)
    );
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

  cJSON_AddNumberToObject(root, "format_version", 1);

  cJSON *bc = serializeBlockChanges();
  if (bc) cJSON_AddItemToObject(root, "block_changes", bc);
  else cJSON_AddArrayToObject(root, "block_changes");

  cJSON *pd = serializePlayerData();
  if (pd) cJSON_AddItemToObject(root, "players", pd);
  else cJSON_AddArrayToObject(root, "players");

  cJSON *sb = serializeSpecialBlocks();
  if (sb) cJSON_AddItemToObject(root, "special_blocks", sb);
  else cJSON_AddArrayToObject(root, "special_blocks");

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

  terminal_ui_log("[LOAD] Loaded %d block changes, %d players, %d special blocks",
    block_changes_count, player_data_count, special_blocks_count);

  cJSON_Delete(root);
  return 0;
}

void writeBlockChangesToDisk(int from, int to) {
  (void)from;
  (void)to;
  writeWorldJson();
}

void writePlayerDataToDisk(void) {
  terminal_ui_log("[SAVE] writePlayerDataToDisk called, sb_count=%d, bc_count=%d",
    special_blocks_count, block_changes_count);
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
