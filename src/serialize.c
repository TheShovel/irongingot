#include <stdlib.h>
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

int64_t last_disk_sync_time = 0;

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
    size_t read;
    int has_capacity_header = 0;
    long block_changes_bytes = 0;

    // Get file size to determine layout
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

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
      if (i >= block_changes_count) block_changes_count = i + 1;
    }
    // Read player data directly into memory
    read = fread(player_data, 1, sizeof(player_data), file);
    fclose(file);
    if (read != sizeof(player_data)) {
      printf("Read %u bytes from \"world.bin\", expected %u (player data). Aborting.\n", read, sizeof(player_data));
      return 1;
    }

  } else { // World file doesn't exist or failed to open
    printf("No \"world.bin\" file found, creating one...\n\n");

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
    fclose(file);
    if (written != sizeof(player_data)) {
      perror(
        "Failed to write initial player data to \"world.bin\".\n"
        "Consider checking permissions or disabling SYNC_WORLD_TO_DISK in \"globals.h\"."
      );
      return 1;
    }

  }

  return 0;
}

// Writes a range of block change entries to disk
void writeBlockChangesToDisk (int from, int to) {

  // Try to open the file in rw (without overwriting)
  FILE *file = fopen(FILE_PATH, "r+b");
  if (!file) {
    perror("Failed to open \"world.bin\". Block updates have been dropped.");
    return;
  }

  for (int i = from; i <= to; i ++) {
    // Seek to relevant offset in file
    if (fseek(file, i * sizeof(BlockChange), SEEK_SET) != 0) {
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

// Writes all player data to disk
void writePlayerDataToDisk () {

  // Try to open the file in rw (without overwriting)
  FILE *file = fopen(FILE_PATH, "r+b");
  if (!file) {
    perror("Failed to open \"world.bin\". Player updates have been dropped.");
    return;
  }
  // Seek past block changes in file
  if (fseek(file, 
    #ifdef INFINITE_BLOCK_CHANGES
      sizeof(int) + block_changes_capacity * sizeof(BlockChange),
    #else
      sizeof(block_changes),
    #endif
    SEEK_SET) != 0) {
    fclose(file);
    perror("Failed to seek in \"world.bin\". Player updates have been dropped.");
    return;
  }
  // Write full player data array to file
  // Since this is a bigger write, it should ideally be done infrequently
  if (fwrite(&player_data, 1, sizeof(player_data), file) != sizeof(player_data)) {
    fclose(file);
    perror("Failed to write to \"world.bin\". Player updates have been dropped.");
    return;
  }

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
  writeBlockChangesToDisk(0, block_changes_count);
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
