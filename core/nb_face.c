/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Mario Baril
 * The heretic face. Everything drawn is read from the engine: a lamp's light is its loop's level as heard,
 * the red around it the preamp's heat, a line a send (lime, or cold when it inverts), its light and its
 * running dots what passes on it. Lime only for what is active. */
#include "nb_face.h"
#include "nb_dsp.h"

#define CX 120.f
#define CY 118.f
#define RING 72.f
#define LR 28.f

typedef struct { float r, g, b; } rgb;
static const rgb LIME = { 194, 240, 0 };
static const rgb COLD = { 90, 190, 255 };
static const rgb HEAT = { 255, 91, 58 };
static const rgb DIM = { 58, 58, 55 };
static const rgb IDLE = { 77, 77, 73 };
static const rgb BODY = { 12, 12, 12 };
static const rgb INK = { 235, 238, 236 };
static const rgb GREY = { 150, 156, 154 };
static const rgb TRACK = { 46, 48, 47 };
static const rgb BLACK = { 0, 0, 0 };

typedef struct { unsigned y0, y1; uint16_t *pix; } band;

static inline void blend(band *b, int x, int y, rgb c, float a)
{
    if (a <= 0.f || x < 0 || x >= NB_FACE_W || y < (int)b->y0 || y >= (int)b->y1) return;
    if (a > 1.f) a = 1.f;
    uint16_t *p = &b->pix[(unsigned)(y - (int)b->y0) * NB_FACE_W + (unsigned)x];
    unsigned v = *p;
    float r = (float)((v >> 11) & 31) * (255.f / 31.f);
    float g = (float)((v >> 5) & 63) * (255.f / 63.f);
    float bl = (float)(v & 31) * (255.f / 31.f);
    r += (c.r - r) * a; g += (c.g - g) * a; bl += (c.b - bl) * a;
    *p = (uint16_t)(((unsigned)(r * (31.f / 255.f) + .5f) << 11) | ((unsigned)(g * (63.f / 255.f) + .5f) << 5) |
                    (unsigned)(bl * (31.f / 255.f) + .5f));
}

static inline int band_rows(const band *b, float cy, float r, int *ya, int *yb)
{
    int a = (int)(cy - r - 1.f), z = (int)(cy + r + 2.f);
    if (a < (int)b->y0) a = (int)b->y0;
    if (z > (int)b->y1) z = (int)b->y1;
    *ya = a; *yb = z;
    return a < z;
}

/* a disk with a soft edge; mode 0 flat, 1 a glow falling off squared from the centre */
static void disk(band *b, float cx, float cy, float r, rgb c, float a, int glow)
{
    int ya, yb;
    if (a <= 0.004f || !band_rows(b, cy, r, &ya, &yb)) return;
    int xa = (int)(cx - r - 1.f), xb = (int)(cx + r + 2.f);
    if (xa < 0) xa = 0;
    if (xb > NB_FACE_W) xb = NB_FACE_W;
    for (int y = ya; y < yb; ++y) {
        float dy = (float)y + .5f - cy;
        for (int x = xa; x < xb; ++x) {
            float dx = (float)x + .5f - cx;
            float d2 = dx * dx + dy * dy;
            if (d2 > (r + 1.f) * (r + 1.f)) continue;
            float d = nb_sqrt(d2);
            float k;
            if (glow) { float u = 1.f - d / r; k = u > 0.f ? u * u : 0.f; }
            else k = nb_clamp(r + .5f - d, 0.f, 1.f);
            blend(b, x, y, c, a * k);
        }
    }
}

static void ring(band *b, float cx, float cy, float r, float w, rgb c, float a)
{
    int ya, yb;
    float ro = r + w * .5f;
    if (a <= 0.004f || !band_rows(b, cy, ro, &ya, &yb)) return;
    int xa = (int)(cx - ro - 1.f), xb = (int)(cx + ro + 2.f);
    if (xa < 0) xa = 0;
    if (xb > NB_FACE_W) xb = NB_FACE_W;
    for (int y = ya; y < yb; ++y) {
        float dy = (float)y + .5f - cy;
        for (int x = xa; x < xb; ++x) {
            float dx = (float)x + .5f - cx;
            float d = nb_sqrt(dx * dx + dy * dy);
            float k = nb_clamp(w * .5f + .5f - nb_absf(d - r), 0.f, 1.f);
            if (k > 0.f) blend(b, x, y, c, a * k);
        }
    }
}

static void segment(band *b, float x0, float y0, float x1, float y1, float w, rgb c, float a)
{
    float hw = w * .5f;
    float top = (y0 < y1 ? y0 : y1) - hw - 1.f, bot = (y0 > y1 ? y0 : y1) + hw + 2.f;
    if (a <= 0.004f || bot < (float)b->y0 || top > (float)b->y1) return;
    int ya = (int)top, yb = (int)bot;
    if (ya < (int)b->y0) ya = (int)b->y0;
    if (yb > (int)b->y1) yb = (int)b->y1;
    int xa = (int)((x0 < x1 ? x0 : x1) - hw - 1.f), xb = (int)((x0 > x1 ? x0 : x1) + hw + 2.f);
    if (xa < 0) xa = 0;
    if (xb > NB_FACE_W) xb = NB_FACE_W;
    float vx = x1 - x0, vy = y1 - y0, l2 = vx * vx + vy * vy;
    if (l2 < 1e-6f) l2 = 1e-6f;
    for (int y = ya; y < yb; ++y)
        for (int x = xa; x < xb; ++x) {
            float px = (float)x + .5f - x0, py = (float)y + .5f - y0;
            float t = nb_clamp((px * vx + py * vy) / l2, 0.f, 1.f);
            float dx = px - t * vx, dy = py - t * vy;
            float d = nb_sqrt(dx * dx + dy * dy);
            float k = nb_clamp(hw + .5f - d, 0.f, 1.f);
            /* segments of one curve overlap at the joints: keep the brighter, never add */
            if (k > 0.f) blend(b, x, y, c, a * k * (t > 0.f && t < 1.f ? 1.f : .6f));
        }
}


/* ---- a 5 x 7 font, drawn big: the screen must read from a distance ---- */
/* \001 is the I with an acute (FAÍSCA): the accent on the first row, the letter in the six below */
static const char font_chars[] = " +-.%:0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ\001abdiortuy";
static const uint8_t font_rows[][7] = {
    {0,0,0,0,0,0,0}, {0,4,4,31,4,4,0}, {0,0,0,31,0,0,0}, {0,0,0,0,0,12,12}, {24,25,2,4,8,19,3}, {0,12,12,0,12,12,0},
    {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14}, {14,17,1,2,4,8,31}, {31,2,4,2,1,17,14}, {2,6,10,18,31,2,2},
    {31,16,30,1,1,17,14}, {6,8,16,30,17,17,14}, {31,1,2,4,8,8,8}, {14,17,17,14,17,17,14}, {14,17,17,15,1,2,12},
    {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30}, {14,17,16,16,16,17,14}, {28,18,17,17,17,18,28},
    {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16}, {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
    {14,4,4,4,4,4,14}, {7,2,2,2,2,18,12}, {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17}, {17,17,25,21,19,17,17}, {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
    {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17}, {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
    {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4}, {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
    {17,17,17,10,4,4,4}, {31,1,2,4,8,16,31}, {2,14,4,4,4,4,14},
    /* the few lower-case letters the splash's address needs: a b d i o r t u y */
    {0,0,14,1,15,17,15}, {16,16,22,25,17,17,30}, {1,1,13,19,17,17,15}, {4,0,12,4,4,4,14},
    {0,0,14,17,17,17,14}, {0,0,22,25,16,16,16}, {8,8,28,8,8,9,6}, {0,0,17,17,17,19,13}, {0,0,17,17,15,1,14},
};

static int text_len(const char *s) { int n = 0; while (s[n]) ++n; return n; }

/* a line of text centred on cx, top at y; the scale shrinks to fit the width */
static void text(band *b, const char *s, float cx, int y, int scale, rgb c, float a)
{
    int n = text_len(s);
    if (!n || a <= 0.f) return;
    while (scale > 1 && n * 6 * scale - scale > 228) --scale;
    if (y + 7 * scale <= (int)b->y0 || y >= (int)b->y1) return;
    int x0 = (int)(cx - (float)(n * 6 * scale - scale) * 0.5f);
    for (int i = 0; i < n; ++i) {
        int g = 0;
        for (int j = 0; font_chars[j]; ++j) if (font_chars[j] == s[i]) { g = j; break; }
        for (int r = 0; r < 7; ++r) {
            int yy = y + r * scale;
            if (yy + scale <= (int)b->y0 || yy >= (int)b->y1) continue;
            for (int col = 0; col < 5; ++col) {
                if (!((font_rows[g][r] >> (4 - col)) & 1)) continue;
                int xx = x0 + i * 6 * scale + col * scale;
                for (int py = yy; py < yy + scale; ++py)
                    for (int px = xx; px < xx + scale; ++px) blend(b, px, py, c, a);
            }
        }
    }
}

static void rect(band *b, int x0, int y0, int x1, int y1, rgb c, float a)
{
    if (y0 < (int)b->y0) y0 = (int)b->y0;
    if (y1 > (int)b->y1) y1 = (int)b->y1;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) blend(b, x, y, c, a);
}

static void hud_draw(band *b, const nb_hud *h)
{
    float a = h->alpha;
    if (h->kind == NB_HUD_SPLASH) {
        /* power-on: black, the name as big as the screen takes it, a spark where the I's accent is, flickering
         * as it catches (its rays grow out over the first half second), the address under it; then the lamps come
         * through as it fades */
        rect(b, 0, (int)b->y0, NB_FACE_W, (int)b->y1, BLACK, a);
        text(b, "FA SCA", 120.f, 92, 6, INK, a);                  /* the I is drawn below, without its accent */
        const int s = 6, x0 = 120 - (6 * 6 * s - s) / 2, ix = x0 + 2 * 6 * s;
        rect(b, ix + s, 92 + s, ix + 4 * s, 92 + 2 * s, INK, a);   /* the I: top bar, stem, foot */
        rect(b, ix + 2 * s, 92 + 2 * s, ix + 3 * s, 92 + 6 * s, INK, a);
        rect(b, ix + s, 92 + 6 * s, ix + 4 * s, 92 + 7 * s, INK, a);
        {
            float t = (float)h->age * 0.001f, grow = 1.f;   /* lit from the first instant (the boot screen has it whole) */
            float cx = (float)ix + 2.5f * (float)s, cy = 92.f - 12.f;
            float fl = 0.75f + 0.25f * nb_sin(t * 37.f) * nb_sin(t * 23.f + 1.f);
            for (int r = 0; r < 8; ++r) {
                float ang = (float)r * (NB_PI / 4.f) + 0.2f;
                float len = (r & 1 ? 9.f : 17.f) * grow * fl;
                float dx = nb_sin(ang + NB_PI * 0.5f), dy = nb_sin(ang);
                segment(b, cx + dx * 3.f, cy + dy * 3.f, cx + dx * (3.f + len), cy + dy * (3.f + len),
                        r & 1 ? 2.f : 3.f, LIME, a * (r & 1 ? 0.7f : 1.f));
            }
            disk(b, cx, cy, 4.f, LIME, a, 0);
        }
        text(b, "by ruido.art.br", 120.f, 196, 2, GREY, a * 0.85f);
        return;
    }
    rect(b, 0, (int)b->y0, NB_FACE_W, (int)b->y1, BLACK, 0.82f * a);
    if (h->kind == NB_HUD_PAGE) {
        /* a new page slides in from the right in 160 ms (an ease-out), the four names under it */
        float u = h->slide ? nb_clamp((float)h->age / 160.f, 0.f, 1.f) : 1.f;
        float dx = (1.f - u) * (1.f - u) * 240.f;
        text(b, h->title, 120.f + dx, 20, 5, INK, a);
        for (int k = 0; k < 4; ++k) {
            char num[2] = { (char)('1' + k), 0 };
            text(b, num, 26.f + dx, 86 + k * 36, 3, GREY, a);
            text(b, h->lines[k], 132.f + dx, 86 + k * 36, 3, INK, a);
        }
        return;
    }
    if (h->kind == NB_HUD_TEXT) {
        if (h->title[0]) text(b, h->title, 120.f, 58, 3, GREY, a);
        text(b, h->value, 120.f, h->title[0] ? 104 : 92, 6, INK, a);
        return;
    }
    /* a knob: its name, its value as the sound has it, and a bar (from the centre when bipolar) */
    text(b, h->title, 120.f, 30, 4, GREY, a);
    text(b, h->value, 120.f, 92, 6, INK, a);
    if (h->bar >= 0.f) {
        const int x0 = 20, x1 = 220, y0 = 178, y1 = 198;
        rect(b, x0, y0, x1, y1, TRACK, a);
        int xv = x0 + (int)((float)(x1 - x0) * nb_clamp(h->bar, 0.f, 1.f) + .5f);
        int xs = h->bipolar ? (x0 + x1) / 2 : x0;
        int lo = xs < xv ? xs : xv, hi = xs < xv ? xv : xs;
        if (hi - lo < 2) { lo -= 1; hi += 1; }
        rect(b, lo, y0, hi, y1, (h->bar > 0.f || h->bipolar) ? LIME : TRACK, a);
        if (h->bipolar) rect(b, (x0 + x1) / 2 - 1, y0 - 4, (x0 + x1) / 2 + 1, y1 + 4, GREY, a);
    }
}

static void lamp_xy(int i, float *x, float *y)
{
    static const float ca[4] = { -0.70710678f, 0.70710678f, 0.70710678f, -0.70710678f };
    static const float sa[4] = { -0.70710678f, -0.70710678f, 0.70710678f, 0.70710678f };
    *x = CX + RING * ca[i];
    *y = CY + RING * sa[i];
}

/* the quadratic curve from lamp j to lamp i, bowed to one side so the two directions never overlap */
static void curve_pt(int from, int to, float u, float *x, float *y)
{
    float ax, ay, bx, by;
    lamp_xy(from, &ax, &ay);
    lamp_xy(to, &bx, &by);
    float mx = (ax + bx) * .5f + (by - ay) * .2f, my = (ay + by) * .5f - (bx - ax) * .2f;
    float v = 1.f - u;
    *x = v * v * ax + 2.f * v * u * mx + u * u * bx;
    *y = v * v * ay + 2.f * v * u * my + u * u * by;
}

void nb_face_band(const nb_view *v, const nb_hud *hud, float fs, unsigned y0, unsigned h, uint16_t *pix)
{
    band b = { y0, y0 + h, pix };
    for (unsigned i = 0; i < NB_FACE_W * h; ++i) pix[i] = 0;
    float secs = (float)v->time / fs;

    /* the DRONE latch: the ring through the four lamps, lit while the loops are held open */
    if (hud && hud->drone) ring(&b, CX, CY, RING, 2.f, LIME, 0.45f);

    /* the sends */
    for (int to = 0; to < NB_CH; ++to)
        for (int from = 0; from < NB_CH; ++from) {
            float s = v->send[to][from];
            if (to == from || nb_absf(s) < 0.01f) continue;
            float f = nb_clamp(v->flow[to][from] * 3.f, 0.f, 1.f);
            int live = f > 0.03f;
            rgb c = !live ? DIM : (s < 0.f ? COLD : LIME);
            float a = live ? 0.25f + 0.65f * f : 1.f;
            float w = 1.f + nb_absf(s) * 2.5f;
            /* skip the lamps' own discs: the curve runs from rim to rim */
            float px, py;
            curve_pt(from, to, 0.2f, &px, &py);
            for (int k = 1; k <= 10; ++k) {
                float qx, qy;
                curve_pt(from, to, 0.2f + 0.06f * (float)k, &qx, &qy);
                segment(&b, px, py, qx, qy, w, c, a);
                px = qx; py = qy;
            }
            if (live) {
                /* the dots run toward the receiving lamp, faster with more passing */
                float speed = 0.25f + 0.9f * f;
                for (int d = 0; d < 3; ++d) {
                    float u = secs * speed + (float)d / 3.f + (float)(from * 4 + to) * 0.071f;
                    u -= (float)(int)u;
                    float dx, dy;
                    curve_pt(from, to, 0.2f + 0.6f * u, &dx, &dy);
                    disk(&b, dx, dy, 2.6f, s < 0.f ? COLD : LIME, 0.35f + 0.65f * f, 0);
                }
            }
        }

    /* the lamps */
    for (int i = 0; i < NB_CH; ++i) {
        float x, y;
        lamp_xy(i, &x, &y);
        float lv = nb_clamp(v->level[i] * 1.8f, 0.f, 1.f);
        int sing = lv > 0.03f;
        disk(&b, x, y, LR * 2.1f, LIME, lv * 0.55f, 1);
        if (v->heat[i] > 0.02f) ring(&b, x, y, LR + 6.f, 3.f, HEAT, nb_clamp(v->heat[i] * 1.5f, 0.f, 0.9f));
        disk(&b, x, y, LR, BODY, 1.f, 0);
        if (sing) disk(&b, x, y, LR - 4.f, LIME, 0.15f + 0.85f * nb_sqrt(lv), 0);
        ring(&b, x, y, LR, 2.5f, sing ? LIME : IDLE, 1.f);
    }
    /* MOTION: a thin line along the top, as far as the loop has run (lime playing, red while recording) */
    if (hud && hud->motion) {
        int w = (int)(nb_clamp(hud->motion_phase, 0.f, 1.f) * (float)NB_FACE_W + .5f);
        rect(&b, 0, 0, NB_FACE_W, 4, DIM, 1.f);
        rect(&b, 0, 0, w, 4, hud->motion == 2 ? HEAT : LIME, 1.f);
    }

    if (hud && hud->kind != NB_HUD_NONE && hud->alpha > 0.f) hud_draw(&b, hud);

    /* SETUP's BRIGHT: the whole band scaled (the panel has no backlight control) */
    if (hud && hud->bright > 0.f && hud->bright < 0.999f) {
        unsigned k = (unsigned)(hud->bright * 256.f);
        for (unsigned i = 0; i < NB_FACE_W * h; ++i) {
            unsigned v565 = pix[i];
            unsigned r = (((v565 >> 11) & 31u) * k) >> 8, g = (((v565 >> 5) & 63u) * k) >> 8, bl = ((v565 & 31u) * k) >> 8;
            pix[i] = (uint16_t)((r << 11) | (g << 5) | bl);
        }
    }
}
