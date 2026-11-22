/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Mario Baril
 * What the core needs to know about where it runs. On the FM-1 the audio path lives in RAM (it keeps playing
 * while the Flash is written); elsewhere these are empty. */
#ifndef NB_PLATFORM_H
#define NB_PLATFORM_H
#if defined(NB_FM1B)           /* on fm1-base: its RAM code section (plat.h PLAT_RAM_CODE) */
#define NB_AUDIO_CODE __attribute__((section(".app_ram"), noinline, used))
#elif defined(NB_FM1)
#define NB_AUDIO_CODE __attribute__((section(".volatile_ram_code"), noinline, used))
#else
#define NB_AUDIO_CODE
#endif
#define NB_INLINE static inline __attribute__((always_inline))
#endif
