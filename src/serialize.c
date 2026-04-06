#include <stdlib.h>
#include <string.h>
#include "globals.h"

#ifdef SYNC_WORLD_TO_DISK

#ifdef ESP_PLATFORM
  #include "esp_littlefs.h"
  #define FILE_PATH "/littlefs/world.bin"
#else
  #include <stdio.h>
  #define FILE_PATH "world.bin"
#endif

#include "tools.h"
#include "registries.h"
#include "procedures.h"
#include "serialize.h"
#include "special_block.h"

int64_t last_disk_sync_time = 0;

typedef struct {
  int32_t key;
  uint16_t state;
  uint8_t block;
  uint8_t occupied;
} LegacySpecialBlockEntry;

static inline uint32_t legacy_pack_special_key(short x, uint8_t y, short z) {
  return ((uint32_t)(uint16_t)x << 16) | ((uint32_t)y << 8) | (uint16_t)z;
}

static uint8_t isSpecialStateBlock(uint8_t block) {
  return (
    is_door_block(block) ||
    is_trapdoor_block(block) ||
    is_stair_block(block) ||
    is_oriented_block(block)
  );
}

static uint8_t looksLikeCurrentSpecialBlockTable(const SpecialBlockEntry *entries, int sb_count) {
  int occupied = 0;

  for (int i = 0; i < MAX_SPECIAL_BLOCKS; i++) {
    if (entries[i].block == SPECIAL_BLOCK_EMPTY) continue;
    if (!isSpecialStateBlock(entries[i].block)) return 0;
    occupied++;
  }

  return occupied == sb_count;
}

static int recoverLegacySpecialBlockTable(const LegacySpecialBlockEntry *entries) {
  int recovered = 0;
  int ambiguous = 0;

  for (int i = 0; i < MAX_SPECIAL_BLOCKS; i++) {
    if (!entries[i].occupied) continue;
    if (!isSpecialStateBlock(entries[i].block)) continue;

    int matches = 0;
    short match_x = 0;
    uint8_t match_y = 0;
    short match_z = 0;

    for (int j = 0; j < block_changes_count; j++) {
      if (block_changes[j].block == 0xFF) continue;

      uint8_t candidate_block = block_changes[j].block;
      uint8_t skip = 0;

      if (candidate_block == B_chest) skip = 14;
      else if (is_door_block(candidate_block)) skip = 2;
      else if (is_stair_block(candidate_block) || candidate_block == B_furnace) skip = 1;

      if (
        candidate_block == entries[i].block &&
        legacy_pack_special_key(block_changes[j].x, block_changes[j].y, block_changes[j].z) == (uint32_t)entries[i].key
      ) {
        matches++;
        match_x = block_changes[j].x;
        match_y = block_changes[j].y;
        match_z = block_changes[j].z;
        if (matches > 1) break;
      }

      j += skip;
    }

    if (matches == 1) {
      special_block_set_state(match_x, match_y, match_z, entries[i].block, entries[i].state);
      recovered++;
    } else if (matches > 1) {
      ambiguous++;
    }
  }

  if (ambiguous > 0) {
    fprintf(stderr, "[LOAD] Skipped %d ambiguous legacy special block entries\n", ambiguous);
  }

  return recovered;
}

// Restores world data from disk, or writes world file if it doesn't exist
int initSerializer () {

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
      printf("LittleFS error %d\n", ret);
      perror("Failed to mount LittleFS. Aborting.");
      return 1;
    }
  #endif

  // Attempt to open existing world file
  FILE *file = fopen(FILE_PATH, "rb");
  if (file) {
    fprintf(stderr, "[LOAD] Opened world.bin for reading\n");
    size_t read;
    int has_capacity_header = 0;
    long block_changes_bytes = 0;

    // Get file size to determine layout
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    fprintf(stderr, "[LOAD] File size: %ld bytes\n", file_size);

    // Read block changes from the start of the file directly into memory
    #ifdef INFINITE_BLOCK_CHANGES
      // For dynamic mode, first try to read the capacity header
      int stored_capacity;
      size_t header_read = fread(&stored_capacity, sizeof(int), 1, file);
      // Check if this looks like a valid capacity header or old fixed-format file
      // A valid capacity should be positive and reasonable (< 100 million entries)
      // Also check if file size matches expected size for that capacity
      long expected_size_for_capacity = sizeof(int) + (long)stored_capacity * sizeof(BlockChange) + sizeof(player_data);
      if (header_read == 1 && stored_capacity > 0 && stored_capacity <= 100000000 && file_size >= expected_size_for_capacity - 1000) {
        // Valid capacity header (new format)
        has_capacity_header = 1;
        block_changes_capacity = stored_capacity;
        block_changes = (BlockChange *)malloc(block_changes_capacity * sizeof(BlockChange));
        if (!block_changes) {
          printf("Failed to allocate memory for block changes. Aborting.\n");
          fclose(file);
          return 1;
        }
        // Read block changes data
        read = fread(block_changes, sizeof(BlockChange), block_changes_capacity, file);
        if (read != (size_t)block_changes_capacity) {
          printf("Read %u block changes from \"world.bin\", expected %d. Aborting.\n", read, block_changes_capacity);
          fclose(file);
          return 1;
        }
      } else {
        // Old fixed-format file, rewind and calculate size from file
        printf("Detected old world file format, converting...\n");
        rewind(file);
        // Calculate actual block changes size from file size
        block_changes_bytes = file_size - sizeof(player_data);
        if (block_changes_bytes <= 0) {
          printf("Invalid world file size. Aborting.\n");
          fclose(file);
          return 1;
        }
        // Start with enough capacity to hold the data
        block_changes_capacity = (block_changes_bytes / sizeof(BlockChange)) + 100;
        block_changes = (BlockChange *)malloc(block_changes_capacity * sizeof(BlockChange));
        if (!block_changes) {
          printf("Failed to allocate memory for block changes. Aborting.\n");
          fclose(file);
          return 1;
        }
        // Read the block changes data
        read = fread(block_changes, 1, block_changes_bytes, file);
        if (read != (size_t)block_changes_bytes) {
          printf("Read %u bytes from \"world.bin\", expected %ld. Aborting.\n", read, block_changes_bytes);
          fclose(file);
          return 1;
        }
      }
      // Seek past block changes to start reading player data
      long seek_offset = has_capacity_header 
        ? sizeof(int) + block_changes_capacity * sizeof(BlockChange)
        : block_changes_bytes;
      if (fseek(file, seek_offset, SEEK_SET) != 0) {
        perror("Failed to seek to player data in \"world.bin\". Aborting.");
        fclose(file);
        return 1;
      }
    #else
      size_t read = fread(block_changes, 1, sizeof(block_changes), file);
      if (read != sizeof(block_changes)) {
        printf("Read %u bytes from \"world.bin\", expected %u (block changes). Aborting.\n", read, sizeof(block_changes));
        fclose(file);
        return 1;
      }
      // Seek past block changes to start reading player data
      if (fseek(file, sizeof(block_changes), SEEK_SET) != 0) {
        perror("Failed to seek to player data in \"world.bin\". Aborting.");
        fclose(file);
        return 1;
      }
    #endif
    // Find the index of the last occupied entry to recover block_changes_count
    for (int i = 0; i < (
      #ifdef INFINITE_BLOCK_CHANGES
        block_changes_capacity
      #else
        MAX_BLOCK_CHANGES
      #endif
    ); i ++) {
      if (block_changes[i].block == 0xFF) continue;
      if (block_changes[i].block == B_chest) i += 14;
      #ifdef ALLOW_DOORS
      if (isDoorBlock(block_changes[i].block)) i += 2;
      #endif
      if (isStairBlock(block_changes[i].block) || block_changes[i].block == B_furnace) i += 1;
      if (i >= block_changes_count) block_changes_count = i + 1;
    }
    // Read player data directly into memory
    read = fread(player_data, 1, sizeof(player_data), file);
    if (read != sizeof(player_data)) {
      printf("Read %u bytes from \"world.bin\", expected %u (player data). Aborting.\n", read, sizeof(player_data));
      fclose(file);
      return 1;
    }

    // Read special block state table (new format -- appended after player data)
    special_block_init();
    fprintf(stderr, "[LOAD] About to read sb_count, file pos=%ld\n", ftell(file));
    int sb_count = 0;
    read = fread(&sb_count, sizeof(int), 1, file);
    fprintf(stderr, "[LOAD] Read sb_count: read=%zd, value=%d\n", read, sb_count);
    if (read == 1 && sb_count > 0 && sb_count <= MAX_SPECIAL_BLOCKS) {
      size_t expected = sizeof(SpecialBlockEntry) * MAX_SPECIAL_BLOCKS;
      void *raw_special_blocks = malloc(expected);

      fprintf(stderr, "[LOAD] Reading %zd bytes of special block data\n", expected);
      if (!raw_special_blocks) {
        fprintf(stderr, "[LOAD] ERROR: failed to allocate temporary special block buffer\n");
      } else {
        read = fread(raw_special_blocks, 1, expected, file);
        fprintf(stderr, "[LOAD] Read %zd of %zd bytes\n", read, expected);
        if (read == expected) {
          if (looksLikeCurrentSpecialBlockTable((SpecialBlockEntry *)raw_special_blocks, sb_count)) {
            memcpy(special_blocks, raw_special_blocks, expected);
            special_blocks_count = sb_count;
            fprintf(stderr, "[LOAD] Successfully loaded %d special block entries\n", special_blocks_count);
          } else {
            int recovered = recoverLegacySpecialBlockTable((LegacySpecialBlockEntry *)raw_special_blocks);
            fprintf(stderr, "[LOAD] Recovered %d entries from legacy special block table\n", recovered);
          }
        } else {
          fprintf(stderr, "[LOAD] ERROR: short read of special block data\n");
        }
        free(raw_special_blocks);
      }
    } else {
      fprintf(stderr, "[LOAD] No special block table found (read=%zd, sb_count=%d)\n", read, sb_count);
    }

    /* Migrate legacy state data from block_changes entries into the
     * unified special_block table. This handles worlds saved before
     * the special_block system was introduced.
     * Only populates entries that are MISSING, so it won't overwrite
     * correct state from a previously-saved new-format world. */
    int migrated = 0;
    for (int i = 0; i < block_changes_count; i++) {
      if (block_changes[i].block == 0xFF) continue;

      if (block_changes[i].block == B_chest) {
        fprintf(stderr, "[MIGRATE] chest at (%d,%d,%d) idx=%d has_entry=%d\n",
          block_changes[i].x, block_changes[i].y, block_changes[i].z, i,
          special_block_has_entry(block_changes[i].x, block_changes[i].y, block_changes[i].z));
        if (i + 14 < block_changes_count &&
            !special_block_has_entry(block_changes[i].x, block_changes[i].y, block_changes[i].z)) {
          uint8_t dir = block_changes[i + 14].y;
          if (dir > 3) dir = 0;
          special_block_set_state(block_changes[i].x, block_changes[i].y,
                                  block_changes[i].z, B_chest,
                                  (uint16_t)(dir & 3));
          migrated++;
        }
        i += 14;
      }
      #ifdef ALLOW_DOORS
      else if (isDoorBlock(block_changes[i].block)) {
        if (i + 2 < block_changes_count &&
            !special_block_has_entry(block_changes[i].x, block_changes[i].y, block_changes[i].z)) {
          uint8_t dir   = block_changes[i + 2].x;
          uint8_t flags = block_changes[i + 2].y;
          if (dir > 3) dir = 0;
          uint8_t open  = flags & 0x01;
          uint8_t hinge = (flags >> 1) & 0x01;
          uint16_t state = (uint16_t)((dir << 2) | (hinge << 1) | open);
          special_block_set_state(block_changes[i].x, block_changes[i].y,
                                  block_changes[i].z, block_changes[i].block,
                                  state);
          migrated++;
        }
        i += 2;
      }
      #endif
      else if (isStairBlock(block_changes[i].block) || block_changes[i].block == B_furnace) {
        if (i + 1 < block_changes_count &&
            !special_block_has_entry(block_changes[i].x, block_changes[i].y, block_changes[i].z)) {
          uint8_t dir   = block_changes[i + 1].x;
          uint8_t extra = block_changes[i + 1].y;
          if (dir > 3) dir = 0;
          if (isStairBlock(block_changes[i].block)) {
            uint8_t half = extra & 0x03;
            uint16_t state = (uint16_t)((dir << 2) | (half & 3));
            special_block_set_state(block_changes[i].x, block_changes[i].y,
                                    block_changes[i].z, block_changes[i].block,
                                    state);
          } else {
            uint8_t lit = (extra >> 2) & 0x01;
            uint16_t state = (uint16_t)((lit << 2) | (dir & 3));
            special_block_set_state(block_changes[i].x, block_changes[i].y,
                                    block_changes[i].z, B_furnace,
                                    state);
          }
          migrated++;
        }
        i += 1;
      }
    }
    printf("Loaded special block table: %d entries (migrated %d legacy)\n",
           special_blocks_count, migrated);
    fprintf(stderr, "[LOAD] Loaded special block table: %d entries, migrated %d legacy, file_size=%ld\n",
           special_blocks_count, migrated, file_size);

    fclose(file);

  } else { // World file doesn't exist or failed to open
    printf("No \"world.bin\" file found, creating one...\n\n");

    // Initialize special block table
    special_block_init();

    // Try to create the file in binary write mode
    file = fopen(FILE_PATH, "wb");
    if (!file) {
      perror(
        "Failed to open \"world.bin\" for writing.\n"
        "Consider checking permissions or disabling SYNC_WORLD_TO_DISK in \"globals.h\"."
      );
      return 1;
    }
    // Write initial block changes array
    // This should be done after all entries have had `block` set to 0xFF
    #ifdef INFINITE_BLOCK_CHANGES
      // Write capacity header first
      size_t written = fwrite(&block_changes_capacity, sizeof(int), 1, file);
      if (written != 1) {
        perror("Failed to write block changes header to \"world.bin\".");
        fclose(file);
        return 1;
      }
      // Write block changes data
      written = fwrite(block_changes, sizeof(BlockChange), block_changes_capacity, file);
      if (written != (size_t)block_changes_capacity) {
        perror(
          "Failed to write initial block data to \"world.bin\".\n"
          "Consider checking permissions or disabling SYNC_WORLD_TO_DISK in \"globals.h\"."
        );
        fclose(file);
        return 1;
      }
    #else
      size_t written = fwrite(block_changes, 1, sizeof(block_changes), file);
      if (written != sizeof(block_changes)) {
        perror(
          "Failed to write initial block data to \"world.bin\".\n"
          "Consider checking permissions or disabling SYNC_WORLD_TO_DISK in \"globals.h\"."
        );
        fclose(file);
        return 1;
      }
    #endif
    // Seek past written block changes to start writing player data
    if (fseek(file,
      #ifdef INFINITE_BLOCK_CHANGES
        sizeof(int) + block_changes_capacity * sizeof(BlockChange),
      #else
        sizeof(block_changes),
      #endif
      SEEK_SET) != 0) {
      perror(
        "Failed to seek past block changes in \"world.bin\"."
        "Consider checking permissions or disabling SYNC_WORLD_TO_DISK in \"globals.h\"."
      );
      fclose(file);
      return 1;
    }
    // Write initial player data to disk (should be just nulls?)
    written = fwrite(player_data, 1, sizeof(player_data), file);
    if (written != sizeof(player_data)) {
      perror(
        "Failed to write initial player data to \"world.bin\".\n"
        "Consider checking permissions or disabling SYNC_WORLD_TO_DISK in \"globals.h\"."
      );
      fclose(file);
      return 1;
    }
    // Write special block count (0 initially)
    int sb_count = 0;
    written = fwrite(&sb_count, sizeof(int), 1, file);
    fclose(file);
    if (written != 1) {
      perror("Failed to write special block header to \"world.bin\".");
      return 1;
    }

  }

  return 0;
}

// Writes a range of block change entries to disk
void writeBlockChangesToDisk (int from, int to) {
  if (from > to) return;

  // Try to open the file in rw (without overwriting)
  FILE *file = fopen(FILE_PATH, "r+b");
  if (!file) {
    perror("Failed to open \"world.bin\". Block updates have been dropped.");
    return;
  }

  for (int i = from; i <= to; i ++) {
    // Seek to relevant offset in file
    long offset =
      #ifdef INFINITE_BLOCK_CHANGES
        sizeof(int) + (long)i * sizeof(BlockChange);
      #else
        (long)i * sizeof(BlockChange);
      #endif
    if (fseek(file, offset, SEEK_SET) != 0) {
      fclose(file);
      perror("Failed to seek in \"world.bin\". Block updates have been dropped.");
      return;
    }
    // Write block change entry to file
    if (fwrite(&block_changes[i], 1, sizeof(BlockChange), file) != sizeof(BlockChange)) {
      fclose(file);
      perror("Failed to write to \"world.bin\". Block updates have been dropped.");
      return;
    }
  }

  fclose(file);
}

// Writes all player data and special block state table to disk.
// Rewrites the ENTIRE file to keep the layout consistent with the
// current block_changes_capacity (which may have grown via realloc).
void writePlayerDataToDisk () {

  fprintf(stderr, "[SAVE] writePlayerDataToDisk called, sb_count=%d, bc_capacity=%d\n",
    special_blocks_count, block_changes_capacity);

  FILE *file = fopen(FILE_PATH, "wb");  /* Rewrite entire file */
  if (!file) {
    perror("Failed to open \"world.bin\" for writing. Player updates have been dropped.");
    return;
  }

  #ifdef INFINITE_BLOCK_CHANGES
    /* Write capacity header */
    if (fwrite(&block_changes_capacity, sizeof(int), 1, file) != 1) {
      fclose(file);
      perror("Failed to write block changes capacity header.");
      return;
    }
    /* Write entire block_changes array */
    if (fwrite(block_changes, sizeof(BlockChange), block_changes_capacity, file) != (size_t)block_changes_capacity) {
      fclose(file);
      perror("Failed to write block changes.");
      return;
    }
  #else
    if (fwrite(block_changes, 1, sizeof(block_changes), file) != sizeof(block_changes)) {
      fclose(file);
      perror("Failed to write block changes.");
      return;
    }
  #endif

  /* Write player data */
  if (fwrite(&player_data, 1, sizeof(player_data), file) != sizeof(player_data)) {
    fclose(file);
    perror("Failed to write player data.");
    return;
  }

  /* Write special block state table */
  int sb_count = special_blocks_count;
  if (fwrite(&sb_count, sizeof(int), 1, file) != 1) {
    fclose(file);
    perror("Failed to write special block count.");
    return;
  }
  size_t expected = sizeof(SpecialBlockEntry) * MAX_SPECIAL_BLOCKS;
  if (fwrite(special_blocks, 1, expected, file) != expected) {
    fclose(file);
    perror("Failed to write special blocks.");
    return;
  }

  fprintf(stderr, "[SAVE] wrote full world file successfully\n");
  fclose(file);
}

// Writes data queued for interval writes, but only if enough time has passed
void writeDataToDiskOnInterval () {

  // Skip this write if enough time hasn't passed since the last one
  if (get_program_time() - last_disk_sync_time < DISK_SYNC_INTERVAL) return;
  last_disk_sync_time = get_program_time();

  // Write full player data and block changes buffers
  writePlayerDataToDisk();
  #ifdef DISK_SYNC_BLOCKS_ON_INTERVAL
  writeBlockChangesToDisk(0, block_changes_count - 1);
  #endif

}

#ifdef ALLOW_CHESTS
// Writes a chest slot change to disk
void writeChestChangesToDisk (uint8_t *storage_ptr, uint8_t slot) {
  /**
   * More chest-related memory hacks!!
   *
   * Since chests are implemented in the block_changes array, any
   * changes to the contents of a chest have to be synced to the block
   * changes part of the world file. The index of the "blocks" is
   * determined as such:
   *
   * The storage pointer points to the block entry directly following
   * the chest itself. To get the index of this entry, we can subtract
   * the pointer to the block changes array (cast to uint8_t*) from the
   * storage pointer. This gets us the amount of bytes between the start
   * of the block changes array and the chest's item data, as a pointer.
   * To get the actual block index, we cast this weird pointer to an
   * integer, and divide it by the byte size of the BlockChange struct.
   * Finally, the chest slot divided by 2 is added to this index to get
   * the block entry pertaining to the relevant chest slot, as each
   * entry encodes exactly 2 slots.
   */
  int index = (int)(storage_ptr - (uint8_t *)block_changes) / sizeof(BlockChange) + slot / 2;
  writeBlockChangesToDisk(index, index);
}
#endif

#endif
