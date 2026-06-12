#ifndef H_GENERATED_VILLAGE_TEMPLATES
#define H_GENERATED_VILLAGE_TEMPLATES

#include <stdint.h>

#define VILLAGE_TEMPLATE_STYLE_COUNT 5
#define VILLAGE_TEMPLATE_PROFESSION_COUNT 13
#define VILLAGE_TEMPLATE_NONE 0xFFFF

typedef struct {
  uint16_t index;
  uint16_t block;
} VillageTemplateBlock;

typedef struct {
  uint8_t size_x;
  uint8_t size_y;
  uint8_t size_z;
  uint8_t origin_x;
  uint8_t origin_y;
  uint8_t origin_z;
  uint16_t block_count;
  const VillageTemplateBlock *blocks;
} VillageTemplate;

extern const VillageTemplate village_templates[VILLAGE_TEMPLATE_STYLE_COUNT][VILLAGE_TEMPLATE_PROFESSION_COUNT];
uint8_t villageTemplateExists(uint8_t style, uint8_t profession);
uint16_t getVillageTemplateBlockAt(uint8_t style, uint8_t profession, int dx, int dy, int dz);

#endif
