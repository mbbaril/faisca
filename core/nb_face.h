/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Mario Baril
 * The screen: four lamps in a circle, lit by what passes through the loops, and the sends between them.
 * Drawn by bands (240 x h, RGB565) so the device needs no full frame in RAM. */
#ifndef NB_FACE_INCLUDED
#define NB_FACE_INCLUDED
#include <stdint.h>
#include "nb_engine.h"
#include "nb_panel.h"

#define NB_FACE_W 240
#define NB_FACE_H 240
#define NB_FACE_BAND 16

/* Draw rows y0 .. y0+h-1 into pix (240 * h pixels, native-endian RGB565). */
/* hud: what was just touched, said big over the lamps (may be NULL) */
void nb_face_band(const nb_view *v, const nb_hud *hud, float sample_rate, unsigned y0, unsigned h, uint16_t *pix);

#endif
