#ifndef H_PROCEDURES
#define H_PROCEDURES

#include <unistd.h>

#include "globals.h"

extern ClientState client_states[MAX_PLAYERS];

void setClientState (int client_fd, int new_state);
int getClientState (int client_fd);
int getClientIndex (int client_fd);
void setCompressionThreshold (int client_fd, int threshold);
int getCompressionThreshold (int client_fd);

void resetPlayerData (PlayerData *player);
void resetPlayerAppearance (int player_index);
void loadPlayerAppearance (int player_index, const uint8_t *uuid, const char *name);
void updatePlayerAppearanceClientSettings (int client_fd, uint8_t skin_parts, uint8_t main_hand);
int reservePlayerData (int client_fd, uint8_t *uuid, char* name);
int getPlayerData (int client_fd, PlayerData **output);
PlayerData *getPlayerByName (int start_offset, int end_offset, uint8_t *buffer);
void handlePlayerDisconnect (int client_fd);
void handlePlayerJoin (PlayerData* player);
void disconnectClient (int *client_fd, int cause);
int givePlayerItem (PlayerData *player, uint16_t item, uint8_t count);
void spawnPlayer (PlayerData *player);

void sendPlayerMetadata (int client_fd, PlayerData *player);
void sendPlayerEquipment (int client_fd, PlayerData *player);
void broadcastPlayerMetadata (PlayerData *player);
void broadcastPlayerEquipment (PlayerData *player);
void broadcastMobMetadata (int client_fd, int entity_id);

uint8_t serverSlotToClientSlot (int window_id, uint8_t slot);
uint8_t clientSlotToServerSlot (int window_id, uint8_t slot);

uint16_t getBlockChange (short x, int16_t y, short z, uint8_t dimension);
uint8_t makeBlockChange (short x, int16_t y, short z, uint16_t block, uint8_t dimension);
uint8_t isChunkModified (int chunk_x, int chunk_z);
void rebuildBlockChangeIndexes (void);
BlockChange *copyBlockChangesSnapshot (int *count);
void freeBlockChangesSnapshot (BlockChange *snapshot);

uint8_t isInstantlyMined (PlayerData *player, uint16_t block);
uint8_t isColumnBlock (uint16_t block);
uint8_t isLeafBlock (uint16_t block);
uint8_t isSaplingBlock (uint16_t block);
uint8_t isStairBlock (uint16_t block);
uint8_t isOrientedBlock (uint16_t block);
uint8_t isPassableBlock (uint16_t block);
uint8_t isPassableSpawnBlock (uint16_t block);
uint8_t isReplaceableBlock (uint16_t block);
uint32_t isCompostItem (uint16_t item);
uint8_t getItemStackSize (uint16_t item);

#ifdef ALLOW_DOORS
uint8_t isDoorBlock (uint16_t block);
uint8_t isTrapdoorBlock (uint16_t block);
uint8_t isDoorItem (uint16_t item);
uint16_t getDoorItemFromBlock (uint16_t block);
uint8_t getDoorBlockFromItem (uint16_t item);
uint8_t isDoorOpen (short x, uint8_t y, short z, uint8_t dimension);
uint16_t getDoorStateId (uint16_t block, uint8_t is_upper, uint8_t open, uint8_t direction, uint8_t hinge);
void sendDoorUpdate (int client_fd, short x, uint8_t y, short z, uint16_t block, uint8_t is_upper, uint8_t open, uint8_t direction, uint8_t hinge);
uint16_t getTrapdoorStateId (uint16_t block, uint8_t open, uint8_t direction, uint8_t half);
void sendTrapdoorUpdate (int client_fd, short x, uint8_t y, short z, uint16_t block, uint8_t open, uint8_t direction, uint8_t half);
uint16_t getOrientedStateId (uint16_t block, uint8_t direction);
void sendOrientedUpdate (int client_fd, short x, uint8_t y, short z, uint16_t block, uint8_t direction);
uint16_t getStairStateId (uint16_t block, uint8_t half, uint8_t direction);
void sendStairUpdate (int client_fd, short x, uint8_t y, short z, uint16_t block, uint8_t half, uint8_t direction);
#endif

uint16_t getMiningResult (uint16_t held_item, uint16_t block);

void handlePlayerAction (PlayerData *player, int action, short x, short y, short z);
void handlePlayerUseItem (PlayerData *player, short x, short y, short z, uint8_t face);

void checkFluidUpdate (short x, int16_t y, short z, uint16_t block);
void processFluidQueue (void);

void spawnMob (uint8_t type, short x, uint8_t y, short z, uint8_t health, uint8_t dimension);
void interactEntity (int entity_id, int interactor_id);
void hurtEntity (int entity_id, int attacker_id, uint8_t damage_type, uint8_t damage);
void handleServerTick (int64_t time_since_last_tick);

void switchPlayerDimension (PlayerData *player);
void handlePortalTravel (PlayerData *player);
uint8_t isPlayerNoclipEnabled (PlayerData *player);
void setPlayerNoclip (PlayerData *player, uint8_t enabled);
void syncPlayerNoclipState (PlayerData *player);

void broadcastChestUpdate (int origin_fd, int chest_idx, uint16_t item, uint8_t count, uint8_t slot);

ssize_t writeEntityData (int client_fd, EntityData *data);

int sizeEntityData (EntityData *data);
int sizeEntityMetadata (EntityData *metadata, size_t length);

#endif
