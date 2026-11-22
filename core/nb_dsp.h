/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Mario Baril
 * Small math for the noisebox core: no libc, the same results on every target. */
#ifndef NB_DSP_H
#define NB_DSP_H
#include <stdint.h>
#include "nb_platform.h"

#define NB_PI 3.14159265358979f

NB_INLINE float nb_clamp(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
NB_INLINE float nb_absf(float x) { return x < 0.f ? -x : x; }
NB_INLINE float nb_lerp(float a, float b, float t) { return a + (b - a) * t; }
NB_INLINE int nb_isbad(float x) { return !(x == x) || x > 1e6f || x < -1e6f; }

/* 2^x for |x| < 30: integer part in the exponent, a cubic on the fraction (error < 1e-4). */
NB_INLINE float nb_exp2(float x)
{
    if (x < -30.f) return 0.f;
    if (x > 30.f) x = 30.f;
    int i = (int)(x + 64.f) - 64;           /* floor */
    float f = x - (float)i;
    float p = 1.f + f * (0.6951786f + f * (0.2261251f + f * 0.0781013f));
    union { float f; uint32_t u; } v;
    v.u = (uint32_t)(i + 127) << 23;
    return p * v.f;
}

/* log2(x) for x > 0, error < 1e-3: exponent plus a polynomial on the mantissa. */
NB_INLINE float nb_log2(float x)
{
    union { float f; uint32_t u; } v = { x };
    int e = (int)((v.u >> 23) & 255u) - 127;
    v.u = (v.u & 0x007fffffu) | 0x3f800000u;
    float m = v.f;                          /* 1 <= m < 2 */
    float p = -1.7417939f + m * (2.8212026f + m * (-1.4699568f + m * (0.44717955f + m * -0.056570851f)));
    return (float)e + p;
}

/* tanh by a rational (Pade 7/6 style), clamped: monotone, |error| < 2e-3. */
NB_INLINE float nb_tanh(float x)
{
    if (x > 4.97f) return 1.f;
    if (x < -4.97f) return -1.f;
    float x2 = x * x;
    float a = x * (135135.f + x2 * (17325.f + x2 * (378.f + x2)));
    float b = 135135.f + x2 * (62370.f + x2 * (3150.f + x2 * 28.f));
    return a / b;
}

/* tan for 0 <= x <= 1.3 (the filter warp), Pade, error < 1e-3 in range. */
NB_INLINE float nb_tan(float x)
{
    float x2 = x * x;
    return x * (15.f - x2) / (15.f - 6.f * x2);
}

/* sine on [-pi, pi], a parabola refined once (error < 1e-3). */
NB_INLINE float nb_sin(float x)
{
    while (x > NB_PI) x -= 2.f * NB_PI;
    while (x < -NB_PI) x += 2.f * NB_PI;
    float y = 1.2732395f * x - 0.4052847f * x * nb_absf(x);
    return 0.225f * (y * nb_absf(y) - y) + y;
}

NB_INLINE float nb_sqrt(float x)
{
    if (x <= 0.f) return 0.f;
    union { float f; uint32_t u; } v = { x };
    v.u = 0x1fbd1df5u + (v.u >> 1);
    float y = v.f;
    y = 0.5f * (y + x / y);
    y = 0.5f * (y + x / y);
    return y;
}

/* white noise, xorshift32: -1..1 */
NB_INLINE float nb_noise(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return (float)(int32_t)x * (1.f / 2147483648.f);
}

/* A 17-point table, x in 0..1: the measured knob curves. */
NB_INLINE float nb_table17(const float *t, float x)
{
    x = nb_clamp(x, 0.f, 1.f) * 16.f;
    int i = (int)x;
    if (i >= 16) return t[16];
    return t[i] + (t[i + 1] - t[i]) * (x - (float)i);
}

#endif
