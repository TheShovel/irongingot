#ifndef H_MOJANG
#define H_MOJANG

#include <stdint.h>

#include "globals.h"

void init_mojang_api(void);
void shutdown_mojang_api(void);
uint8_t mojang_skin_lookup_supported(void);
const char *mojang_skin_backend_name(void);
uint8_t fetchMojangPlayerAppearance(const uint8_t *uuid, const char *name, PlayerAppearance *appearance);

#endif
