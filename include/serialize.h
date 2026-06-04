#ifndef SERIALIZE_H
#define SERIALIZE_H

#include "globals.h"

#ifdef SYNC_WORLD_TO_DISK
  int initSerializer ();
  void writeBlockChangesToDisk (int from, int to);
  void writeChestChangesToDisk (int chest_idx, uint8_t slot);
  void writePlayerDataToDisk ();
  void writeDataToDiskOnInterval ();
#else
  // Define no-op placeholders for when disk syncing isn't enabled
  #define writeBlockChangesToDisk(a, b)
  #define writeChestChangesToDisk(a, b) ((void)0)
  #define writePlayerDataToDisk()
  #define writeDataToDiskOnInterval()
  #define initSerializer() 0
#endif

#endif
