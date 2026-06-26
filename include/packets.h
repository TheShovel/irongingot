#ifndef H_PACKETS
#define H_PACKETS

// Serverbound packets
int cs_handshake (int client_fd);
int cs_loginStart (int client_fd, uint8_t *uuid, char *name);
int cs_clientInformation (int client_fd);
int cs_pluginMessage (int client_fd);
int cs_playerAction (int client_fd);
int cs_useItemOn (int client_fd);
int cs_useItem (int client_fd);
int cs_setPlayerPositionAndRotation (int client_fd, double *x, double *y, double *z, float *yaw, float *pitch, uint8_t *on_ground);
int cs_setPlayerPosition (int client_fd, double *x, double *y, double *z, uint8_t *on_ground);
int cs_setPlayerRotation (int client_fd, float *yaw, float *pitch, uint8_t *on_ground);
int cs_setPlayerMovementFlags (int client_fd, uint8_t *on_ground);
int cs_setHeldItem (int client_fd);
int cs_swingArm (int client_fd);
int cs_clickContainer (int client_fd);
int cs_closeContainer (int client_fd);
int cs_clientStatus (int client_fd);
int cs_chat (int client_fd);
int cs_chat_command (int client_fd);
int cs_interact (int client_fd);
int cs_playerInput (int client_fd);
int cs_playerCommand (int client_fd);
int cs_playerLoaded (int client_fd);

// Clientbound packets
int sc_statusResponse (int client_fd);
int sc_setCompression (int client_fd, int threshold);
int sc_loginSuccess (int client_fd, uint8_t *uuid, char *name);
int sc_knownPacks (int client_fd);
int sc_sendPluginMessage (int client_fd, const char *channel, const uint8_t *data, size_t data_len);
int sc_finishConfiguration (int client_fd);
int sc_loginPlay (int client_fd, uint8_t dimension);
int sc_synchronizePlayerPosition (int client_fd, double x, double y, double z, float yaw, float pitch);
int sc_setDefaultSpawnPosition (int client_fd, int64_t x, int64_t y, int64_t z);
int sc_gameEvent (int client_fd, uint8_t event, float value);
int sc_startWaitingForChunks (int client_fd);
int sc_changeDifficulty (int client_fd, uint8_t difficulty, uint8_t locked);
int sc_chunkBatchStart (int client_fd);
int sc_chunkBatchFinished (int client_fd, int batchSize);
int sc_playerAbilities (int client_fd, uint8_t flags);
int sc_updateTime (int client_fd, uint64_t ticks);
int sc_setCenterChunk (int client_fd, int x, int y);
void compute_section_block_light(const uint16_t section[4096], uint8_t light_out[2048]);
int sc_chunkDataAndUpdateLight (int client_fd, int _x, int _z, uint8_t dimension);
int sc_keepAlive (int client_fd);
int sc_setContainerSlot (int client_fd, int window_id, uint16_t slot, uint8_t count, uint16_t item);
int sc_setCursorItem (int client_fd, uint16_t item, uint8_t count);
int sc_setCursorItemWithDamage (int client_fd, uint16_t item, uint8_t count, uint16_t damage);
int sc_setHeldItem (int client_fd, uint8_t slot);
int sc_blockUpdate (int client_fd, int64_t x, int64_t y, int64_t z, uint16_t block);
int sc_blockUpdateState (int client_fd, int64_t x, int64_t y, int64_t z, uint16_t state_id);
int sc_blockEvent (int client_fd, int64_t x, int64_t y, int64_t z, int action, int data, uint16_t block);
#ifdef ALLOW_DOORS
int sc_blockUpdateDoor (int client_fd, int64_t x, int64_t y, int64_t z, uint8_t block, uint8_t is_upper, uint8_t open, uint8_t direction, uint8_t hinge);
#endif
int sc_openScreen (int client_fd, uint8_t window, const char *title, uint16_t length);
int sc_acknowledgeBlockChange (int client_fd, int sequence);
int sc_playerInfoUpdateAddPlayer (int client_fd, PlayerData player);
int sc_playerInfoRemovePlayer (int client_fd, PlayerData player);
int sc_playerInfoUpdateUpdateGamemode (int client_fd, PlayerData player, uint8_t gamemode);
int sc_spawnEntity (int client_fd, int id, uint8_t *uuid, int type, double x, double y, double z, uint8_t yaw, uint8_t pitch, int16_t vx, int16_t vy, int16_t vz);
int sc_spawnEntityPlayer (int client_fd, PlayerData player);
int sc_setEntityMetadata (int client_fd, int id, EntityData *metadata, size_t length);
int sc_setEquipment (int client_fd, int entity_id, PlayerData *player);
int sc_setMobEquipment (int client_fd, int entity_id, uint16_t item);
int sc_entityAnimation (int client_fd, int id, uint8_t animation);
int sc_teleportEntity (int client_fd, int id, double x, double y, double z, float yaw, float pitch);
int sc_setHeadRotation (int client_fd, int id, uint8_t yaw);
int sc_updateEntityRotation (int client_fd, int id, uint8_t yaw, uint8_t pitch);
int sc_damageEvent (int client_fd, int id, int type);
int sc_setEntityVelocity (int client_fd, int id, int16_t vx, int16_t vy, int16_t vz);
int sc_updateEntityAttributes (int client_fd, int entity_id, uint16_t held_item);
int sc_setExperience (int client_fd, uint16_t xp_total, uint16_t xp_level, float xp_progress);
int sc_setHealth (int client_fd, uint8_t health, uint8_t food, uint16_t saturation);
int sc_respawn (int client_fd, uint8_t dimension);
int sc_systemChat (int client_fd, char* message, uint16_t len);
int sc_entityEvent (int client_fd, int entity_id, uint8_t status);
int sc_soundEntity (int client_fd, int sound_id, int category, int entity_id, float volume, float pitch);
int sc_soundEffect (int client_fd, int sound_id, int category, double x, double y, double z, float volume, float pitch);
int sc_removeEntity (int client_fd, int entity_id);
int sc_pickupItem (int client_fd, int collected, int collector, uint8_t count);
int sc_registries (int client_fd);
int sc_declareRecipes(int client_fd);
int sc_unlockRecipes(int client_fd);
int sc_setTradeOffers(int client_fd, int window_id, uint8_t profession, int mob_index);
int cs_containerButtonClick(int client_fd);
int cs_craftRecipeRequest(int client_fd);

void writeItemSlot(int client_fd, uint8_t count, uint16_t item);
void writeItemSlotWithDamage(int client_fd, uint8_t count, uint16_t item, uint16_t damage);

#endif
