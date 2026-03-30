#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include "perlin.h"

void java_random_set_seed(JavaRandom* r, uint64_t seed) {
    r->seed = (seed ^ 0x5DEECE66DL) & ((1ULL << 48) - 1);
}

int32_t java_random_next(JavaRandom* r, int bits) {
    r->seed = (r->seed * 0x5DEECE66DL + 0xBL) & ((1ULL << 48) - 1);
    return (int32_t)(r->seed >> (48 - bits));
}

int32_t java_random_next_int(JavaRandom* r, int32_t n) {
    if (n <= 0) return 0;
    if ((n & -n) == n)
        return (int32_t)((n * (int64_t)java_random_next(r, 31)) >> 31);

    int32_t bits, val;
    do {
        bits = java_random_next(r, 31);
        val = bits % n;
    } while (bits - val + (n - 1) < 0);
    return val;
}

double java_random_next_double(JavaRandom* r) {
    return (((int64_t)java_random_next(r, 26) << 27) + java_random_next(r, 27)) / (double)(1ULL << 53);
}

static inline double fade(double t) {
    return t * t * t * (t * (t * 6 - 15) + 10);
}

static inline double lerp(double t, double a, double b) {
    return a + t * (b - a);
}

static inline double grad(int hash, double x, double y, double z) {
    int h = hash & 15;
    double u = h < 8 ? x : y;
    double v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
    return ((h & 1) == 0 ? u : -u) + ((h & 2) == 0 ? v : -v);
}

void perlin_init(PerlinNoiseSampler* s, JavaRandom* r) {
    s->originX = java_random_next_double(r) * 256.0;
    s->originY = java_random_next_double(r) * 256.0;
    s->originZ = java_random_next_double(r) * 256.0;

    for (int i = 0; i < 256; i++) s->p[i] = (uint8_t)i;

    for (int i = 0; i < 256; i++) {
        int j = java_random_next_int(r, 256 - i);
        uint8_t temp = s->p[i];
        s->p[i] = s->p[i + j];
        s->p[i + j] = temp;
        s->p[i + 256] = s->p[i];
    }
}

double perlin_sample(PerlinNoiseSampler* s, double x, double y, double z) {
    double d = x + s->originX;
    double e = y + s->originY;
    double f = z + s->originZ;
    
    int i = (int)floor(d);
    int j = (int)floor(e);
    int k = (int)floor(f);
    
    double g = d - i;
    double h = e - j;
    double l = f - k;
    
    double u = fade(g);
    double v = fade(h);
    double w = fade(l);
    
    int A = (s->p[i & 255] + j) & 255;
    int AA = (s->p[A] + k) & 255;
    int AB = (s->p[(A + 1) & 255] + k) & 255;
    int B = (s->p[(i + 1) & 255] + j) & 255;
    int BA = (s->p[B] + k) & 255;
    int BB = (s->p[(B + 1) & 255] + k) & 255;

    return lerp(u, 
        lerp(v, lerp(w, grad(s->p[AA], g, h, l), grad(s->p[AA + 1], g, h, l - 1)),
                lerp(w, grad(s->p[AB], g, h - 1, l), grad(s->p[AB + 1], g, h - 1, l - 1))),
        lerp(v, lerp(w, grad(s->p[BA], g - 1, h, l), grad(s->p[BA + 1], g - 1, h, l - 1)),
                lerp(w, grad(s->p[BB], g - 1, h - 1, l), grad(s->p[BB + 1], g - 1, h - 1, l - 1)))
    );
}

void octave_init(OctavePerlinNoiseSampler* s, uint64_t seed, int octaves) {
    JavaRandom r;
    java_random_set_seed(&r, seed);
    s->count = octaves;
    s->samplers = malloc(sizeof(PerlinNoiseSampler) * octaves);
    s->lacunarity = 2.0;
    s->persistence = 0.5;

    for (int i = 0; i < octaves; i++) {
        perlin_init(&s->samplers[i], &r);
    }
}

void octave_destroy(OctavePerlinNoiseSampler* s) {
    if (s->samplers) {
        free(s->samplers);
        s->samplers = NULL;
    }
}

double octave_sample(OctavePerlinNoiseSampler* s, double x, double y, double z) {
    double value = 0;
    double freq = 1.0;
    double amp = 1.0;

    for (int i = 0; i < s->count; i++) {
        value += perlin_sample(&s->samplers[i], x * freq, y * freq, z * freq) * amp;
        freq *= s->lacunarity;
        amp *= s->persistence;
    }
    return value;
}
