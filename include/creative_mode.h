#ifndef H_CREATIVE_MODE
#define H_CREATIVE_MODE

#include <stdint.h>
#include "globals.h"

// Creative mode UI state for a player
typedef struct {
  uint16_t scroll_position;  // Current scroll position in the item list
  uint8_t ui_visible;        // Whether the UI is currently shown
} CreativeUIState;

// Item entry in the creative mode inventory
typedef struct {
  uint16_t item_id;
  const char *name;          // Display name for the UI
} CreativeItem;

// Functions for managing creative mode UI
void initCreativeModeUI(int player_index);
void toggleCreativeModeUI(int client_fd);
void sendCreativeUIScreen(int client_fd);
void sendCreativeItemList(int client_fd, uint16_t start_index);
void handleCreativeItemClick(int client_fd, uint16_t item_id);
void closeCreativeModeUI(int client_fd);

// Utility functions
uint8_t isCreativeModeEnabled(void);
uint16_t getCreativeItemCount(void);
uint16_t findCreativeItemByName(const char *name);
uint16_t getRandomCreativeItem(void);

extern CreativeUIState creative_ui_states[MAX_PLAYERS];

#endif
