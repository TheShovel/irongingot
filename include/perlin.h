#ifndef H_PERLIN
#define H_PERLIN

#include <stdint.h>

typedef struct {
    uint64_t seed;
} JavaRandom;

void java_random_set_seed(JavaRandom* r, uint64_t seed);
int32_t java_random_next(JavaRandom* r, int bits);
int32_t java_random_next_int(JavaRandom* r, int32_t n);
double java_random_next_double(JavaRandom* r);

typedef struct {
    uint8_t p[512];
    double originX, originY, originZ;
} PerlinNoiseSampler;

void perlin_init(PerlinNoiseSampler* s, JavaRandom* r);
double perlin_sample(PerlinNoiseSampler* s, double x, double y, double z);

typedef struct {
    PerlinNoiseSampler* samplers;
    int count;
    double lacunarity;
    double persistence;
} OctavePerlinNoiseSampler;

void octave_init(OctavePerlinNoiseSampler* s, uint64_t seed, int octaves);
void octave_destroy(OctavePerlinNoiseSampler* s);
double octave_sample(OctavePerlinNoiseSampler* s, double x, double y, double z);

#endif
