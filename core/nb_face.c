/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Mario Baril
 * The face (docs/TELA.md). Three zones on the band boundaries: the header (y 0-31: the page, MOD, K1..K4 as four
 * gauges), the stage (y 32-207: one of five views, or what was just touched), the footer (y 208-239: MOTION, the
 * memory or the pad, DRONE). Everything drawn is read from the engine or the hands; lime only for what is active.
 *
 * Cost: once a frame (band 0) a plan turns the view into whole pixels (a few hundred float operations, none per
 * pixel); every band then only fills row spans and copies 2-bit glyphs from nb_font.h through a 4-colour table.
 * One palette in RGB565, rebuilt when BRIGHT changes. Measured on the PC with tests/face_cost.c. */
#include "nb_face.h"
#include "nb_dsp.h"
#include "nb_font.h"

#define W NB_FACE_W
#define H NB_FACE_H

#ifdef NB_FACE_COUNT
static unsigned cnt_fill, cnt_glyph, cnt_blend;
#define CNT(var, n) (var += (unsigned)(n))
void nb_face_cost(unsigned *fill, unsigned *glyph, unsigned *blend)
{
    *fill = cnt_fill; *glyph = cnt_glyph; *blend = cnt_blend;
    cnt_fill = cnt_glyph = cnt_blend = 0;
}
#else
#define CNT(var, n) ((void)0)
#endif

/* ---- the palette ---- */
enum { C_BG, C_INK, C_GREY, C_IDLE, C_DIM, C_TRACK, C_LIME, C_HEAT, C_COLD, C_LIME40, C_COLD40, C_HEAT40, NCOL };
static const uint8_t RGB[NCOL][3] = {
    { 0, 0, 0 }, { 240, 240, 232 }, { 154, 160, 156 }, { 90, 92, 90 }, { 60, 62, 60 }, { 38, 40, 38 },
    { 194, 240, 0 }, { 255, 91, 58 }, { 90, 190, 255 }, { 78, 96, 0 }, { 36, 76, 102 }, { 102, 36, 23 },
};
static uint16_t pal[NCOL];
static int pal_k = -1;

static inline uint16_t pack(unsigned r, unsigned g, unsigned b)
{
    return (uint16_t)((((r * 31u + 127u) / 255u) << 11) | (((g * 63u + 127u) / 255u) << 5) | ((b * 31u + 127u) / 255u));
}
static void palette(float bright)
{
    int k = (int)(nb_clamp(bright <= 0.f ? 1.f : bright, 0.f, 1.f) * 256.f + .5f);
    if (k == pal_k) return;
    pal_k = k;
    for (int c = 0; c < NCOL; ++c)
        pal[c] = pack((unsigned)(RGB[c][0] * k) >> 8, (unsigned)(RGB[c][1] * k) >> 8, (unsigned)(RGB[c][2] * k) >> 8);
}
/* fg over bg at lv thirds (1 or 2): for edges only, never a whole area */
static uint16_t mix(uint16_t fg, uint16_t bg, unsigned lv)
{
    unsigned m = 3u - lv;
    unsigned r = ((fg >> 11) * lv + (bg >> 11) * m) * 86u >> 8;
    unsigned g = (((fg >> 5) & 63u) * lv + ((bg >> 5) & 63u) * m) * 86u >> 8;
    unsigned b = ((fg & 31u) * lv + (bg & 31u) * m) * 86u >> 8;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* ---- a band, its clip (a curtain or a slide splits it) and the x offset of a slide ---- */
typedef struct { int y0; uint16_t *pix; int cy0, cy1, cx0, cx1, ox; } band;

static inline void hspan(const band *b, int y, int x0, int x1, uint16_t c)
{
    if (y < b->cy0 || y >= b->cy1) return;
    x0 += b->ox; x1 += b->ox;
    if (x0 < b->cx0) x0 = b->cx0;
    if (x1 > b->cx1) x1 = b->cx1;
    if (x0 >= x1) return;
    uint16_t *p = b->pix + (y - b->y0) * W;
    for (int x = x0; x < x1; ++x) p[x] = c;
    CNT(cnt_fill, x1 - x0);
}
static inline void pxa(const band *b, int x, int y, uint16_t c, int lv)
{
    if (lv <= 0) return;
    x += b->ox;
    if (y < b->cy0 || y >= b->cy1 || x < b->cx0 || x >= b->cx1) return;
    uint16_t *p = b->pix + (y - b->y0) * W + x;
    *p = lv >= 3 ? c : mix(c, *p, (unsigned)lv);
    CNT(cnt_blend, 1);
}
static void rect(const band *b, int x0, int y0, int x1, int y1, int col)
{
    if (y0 < b->cy0) y0 = b->cy0;
    if (y1 > b->cy1) y1 = b->cy1;
    for (int y = y0; y < y1; ++y) hspan(b, y, x0, x1, pal[col]);
}
/* a continuous interval [u, v] of row y in sixteenths of a pixel: whole pixels as a span, each edge pixel at a
 * third or two thirds */
static void cover(const band *b, int y, int u, int v, uint16_t c)
{
    if (v <= u || y < b->cy0 || y >= b->cy1) return;
    int a = (u + 15) >> 4, z = v >> 4;
    if (z < a) { pxa(b, u >> 4, y, c, ((v - u) * 3 + 8) >> 4); return; }
    if ((a << 4) > u) pxa(b, a - 1, y, c, (((a << 4) - u) * 3 + 8) >> 4);
    hspan(b, y, a, z, c);
    if (v > (z << 4)) pxa(b, z, y, c, ((v - (z << 4)) * 3 + 8) >> 4);
}

/* half-widths of a disc of radius r on the row i + 0.5 from its centre, in sixteenths (-1: none); made once */
#define RMAX 40
static int16_t hw[RMAX + 1][RMAX + 1];
static void hw_init(void)
{
    for (int r = 0; r <= RMAX; ++r)
        for (int i = 0; i <= RMAX; ++i) {
            float d = (float)r * (float)r - ((float)i + .5f) * ((float)i + .5f);
            hw[r][i] = (int16_t)(d > 0.f ? (int)(nb_sqrt(d) * 16.f) : -1);
        }
}
/* centre on a pixel corner (cx, cy), whole radii */
static void disk(const band *b, int cx, int cy, int r, int col)
{
    if (r <= 0) return;
    if (r > RMAX) r = RMAX;
    int ya = cy - r < b->cy0 ? b->cy0 : cy - r, yb = cy + r > b->cy1 ? b->cy1 : cy + r;
    for (int y = ya; y < yb; ++y) {
        int i = y >= cy ? y - cy : cy - 1 - y, h = hw[r][i];
        if (h >= 0) cover(b, y, cx * 16 - h, cx * 16 + h, pal[col]);
    }
}
static void ring(const band *b, int cx, int cy, int ri, int ro, int col)
{
    if (ro > RMAX) ro = RMAX;
    int ya = cy - ro < b->cy0 ? b->cy0 : cy - ro, yb = cy + ro > b->cy1 ? b->cy1 : cy + ro;
    for (int y = ya; y < yb; ++y) {
        int i = y >= cy ? y - cy : cy - 1 - y, ho = hw[ro][i], hi = i < ri ? hw[ri][i] : -1;
        if (ho < 0) continue;
        if (hi >= 0) {
            cover(b, y, cx * 16 - ho, cx * 16 - hi, pal[col]);
            cover(b, y, cx * 16 + hi, cx * 16 + ho, pal[col]);
        } else cover(b, y, cx * 16 - ho, cx * 16 + ho, pal[col]);
    }
}
/* a thick line as row spans: per row, where the distance to the line is <= w/2 and the projection falls inside
 * the segment (a few float operations a row, none a pixel) */
static void line(const band *b, float x0, float y0, float x1, float y1, float w, int col)
{
    float dx = x1 - x0, dy = y1 - y0, l2 = dx * dx + dy * dy;
    if (l2 < .25f) return;
    float L = nb_sqrt(l2), ux = dx / L, uy = dy / L, hwd = w * .5f;
    int ya = (int)((y0 < y1 ? y0 : y1) - hwd) - 1, yb = (int)((y0 > y1 ? y0 : y1) + hwd) + 2;
    if (ya < b->cy0) ya = b->cy0;
    if (yb > b->cy1) yb = b->cy1;
    for (int y = ya; y < yb; ++y) {
        float yc = (float)y + .5f - y0, lo = -1e9f, hi = 1e9f;
        if (nb_absf(uy) > 1e-4f) {
            float a = (yc * ux - hwd) / uy, c = (yc * ux + hwd) / uy;
            if (a > c) { float t = a; a = c; c = t; }
            if (a > lo) lo = a;
            if (c < hi) hi = c;
        } else if (nb_absf(yc) > hwd) continue;
        if (nb_absf(ux) > 1e-4f) {
            float a = -yc * uy / ux, c = (L - yc * uy) / ux;
            if (a > c) { float t = a; a = c; c = t; }
            if (a > lo) lo = a;
            if (c < hi) hi = c;
        } else if (yc * uy < 0.f || yc * uy > L) continue;
        if (hi > lo) cover(b, y, (int)((x0 + lo) * 16.f), (int)((x0 + hi) * 16.f), pal[col]);
    }
}

static void outline(const band *b, int x0, int y0, int x1, int y1, int col)
{
    rect(b, x0, y0, x1, y0 + 1, col); rect(b, x0, y1 - 1, x1, y1, col);
    rect(b, x0, y0, x0 + 1, y1, col); rect(b, x1 - 1, y0, x1, y1, col);
}

/* ---- text: 2-bit glyphs over a known solid background, through a 4-colour table ---- */
enum { FV, FL, FM, FS };
static const nb_font *const FONT[4] = { &nb_font_v, &nb_font_l, &nb_font_m, &nb_font_s };
enum { AL, AC, AR };

static int text_w(const char *s, int f, int gap)
{
    const nb_font *F = FONT[f];
    int x = 0, last = 0;
    if (gap < 0) gap = F->gap;
    for (; *s; ++s) {
        if (*s == ' ') { x += F->space; last = 0; continue; }
        unsigned gi = F->map[(unsigned char)*s & 127u];
        if (gi == 255u) continue;
        x += F->g[gi].w + gap; last = gap;
    }
    return x - last;
}
static int text(const band *b, const char *s, int x, int y, int f, int fg, int bg, int align, int gap)
{
    const nb_font *F = FONT[f];
    if (gap < 0) gap = F->gap;
    int tw = text_w(s, f, gap);
    if (align == AC) x -= tw / 2; else if (align == AR) x -= tw;
    if (y + F->cap <= b->cy0 || y - F->cap / 2 >= b->cy1) return tw;
    uint16_t lut[4] = { pal[bg], mix(pal[fg], pal[bg], 1), mix(pal[fg], pal[bg], 2), pal[fg] };
    for (; *s; ++s) {
        if (*s == ' ') { x += F->space; continue; }
        unsigned gi = F->map[(unsigned char)*s & 127u];
        if (gi == 255u) continue;
        const nb_glyph *g = &F->g[gi];
        int gx = x + b->ox, gy = y + g->yoff, rb = (g->w + 3) >> 2;
        int ya = gy < b->cy0 ? b->cy0 : gy, yb = gy + g->h > b->cy1 ? b->cy1 : gy + g->h;
        int xa = gx < b->cx0 ? b->cx0 : gx, xb = gx + g->w > b->cx1 ? b->cx1 : gx + g->w;
        for (int yy = ya; yy < yb; ++yy) {
            const uint8_t *row = F->bits + g->off + (yy - gy) * rb;
            uint16_t *p = b->pix + (yy - b->y0) * W;
            for (int xx = xa; xx < xb; ++xx) {
                int k = xx - gx;
                unsigned lv = (row[k >> 2] >> (6 - 2 * (k & 3))) & 3u;
                if (lv) p[xx] = lut[lv];
            }
            CNT(cnt_glyph, xb > xa ? xb - xa : 0);
        }
        x += g->w + gap;
    }
    return tw;
}
/* a value as the panel says it ("220 HZ", "+1.5 OCT", "RING"): the number and its unit, or a word */
static int split_value(const char *v, char *num, char *unit)
{
    int n = 0, sp = -1;
    while (v[n]) { if (v[n] == ' ') sp = n; ++n; }
    int numeric = (v[0] >= '0' && v[0] <= '9') || v[0] == '+' || v[0] == '-' || v[0] == '.';
    int cut = sp >= 0 ? sp : n;
    for (int i = 0; i < 15; ++i) num[i] = 0;
    for (int i = 0; i < 7; ++i) unit[i] = 0;
    if (!numeric) { for (int i = 0; i < n && i < 15; ++i) num[i] = v[i]; return 0; }
    for (int i = 0; i < cut && i < 15; ++i) num[i] = v[i];
    /* a unit glued to the number ("0.93X", "48%") is split too */
    int e = cut;
    while (e > 0 && !((num[e - 1] >= '0' && num[e - 1] <= '9') || num[e - 1] == '.')) --e;
    int u = 0;
    for (int i = e; i < cut && u < 6; ++i) unit[u++] = num[i];
    num[e] = 0;
    for (int i = sp + 1; sp >= 0 && v[i] && u < 6; ++i) unit[u++] = v[i];
    return 1;
}

/* ---- the plan: once a frame, everything the views show turned into whole pixels ---- */
#define LC0 66
#define LC1 174
static const int LCX[4] = { LC0, LC1, LC1, LC0 }, LCY[4] = { LC0, LC0, LC1, LC1 };
#define CLEAR 27
#define OFFS 5
#define DROWS 80

/* each send's line, made once: level and upright ones as a row band, the 45-degree ones row by row */
typedef struct {
    int8_t k, dir;
    int16_t c, a, b, len;           /* H: row c, x a..b; V: column c, y a..b (pixels); D: len */
    int16_t y0, n;                  /* D: first row, rows */
    int16_t xc[DROWS], t[DROWS];    /* D: the row's centre (sixteenths), its distance along (pixels) */
} sendline;
static sendline SL[4][4];

static struct {
    int init;
    uint32_t t_prev;
    float lv[4], ht[4], fl[4][4], ph[4][4], st[4][NB_NLOOPFX];
    int rq[4], hq[4], heard[4], cq[4][13], seg[4][13];
    uint8_t wa[4][NB_WAVE], wb[4][NB_WAVE];
    uint8_t sa[NB_SCOPE], sb[NB_SCOPE], sc[4][NB_SCOPE], so[NB_SCOPE];   /* so: bit 0 over the top, bit 1 under */
    uint8_t cov[4][13];                                                   /* a CHAIN stage over its range */
    float ang, spin, out, sp[NB_SPEC];
    int gr;
    int vx[4], vy[4], vr[4];
    float vz[4];
    int tr[4][12][2], ntr, frame;
    uint32_t hud_t0;
    int hud_enc, anchor;
    float secs;
} F;

static int g_struck, g_gated;   /* per frame: the loops a pad strikes (held, or its strike still running); a gate acting */

static int g_fxcut;               /* bit f: effect f is in the sound but the CPU budget leaves it out (drawn hollow red) */
/* a hollow box: an effect left out is drawn as its outline in red, never as nothing */
static void outline(const band *b, int x0, int y0, int x1, int y1, int col);

static void sends_init(void)
{
    for (int to = 0; to < 4; ++to)
        for (int from = 0; from < 4; ++from) {
            sendline *g = &SL[to][from];
            if (to == from) { g->k = 0; continue; }
            int ax = LCX[from], ay = LCY[from], bx = LCX[to], by = LCY[to];
            int dx = bx > ax ? 1 : bx < ax ? -1 : 0, dy = by > ay ? 1 : by < ay ? -1 : 0;
            float h = nb_sqrt((float)(CLEAR * CLEAR - OFFS * OFFS));
            if (dy == 0) {
                g->k = 'H'; g->dir = (int8_t)dx; g->c = (int16_t)(ay + OFFS * dx);
                g->a = (int16_t)((ax < bx ? ax : bx) + h + .5f); g->b = (int16_t)((ax > bx ? ax : bx) - h + .5f);
                g->len = (int16_t)(g->b - g->a);
            } else if (dx == 0) {
                g->k = 'V'; g->dir = (int8_t)dy; g->c = (int16_t)(ax - OFFS * dy);
                g->a = (int16_t)((ay < by ? ay : by) + h + .5f); g->b = (int16_t)((ay > by ? ay : by) - h + .5f);
                g->len = (int16_t)(g->b - g->a);
            } else {
                /* moved 7 px sideways (4.95 px off the centres), the rows within CLEAR of a lamp left out */
                float sh = 7.f * (float)(-dy), Ld = nb_sqrt((float)((bx - ax) * (bx - ax) + (by - ay) * (by - ay)));
                float perp = 7.f * 0.70710678f, chord = nb_sqrt((float)(CLEAR * CLEAR) - perp * perp);
                g->k = 'D'; g->n = 0; g->len = (int16_t)(Ld - 2.f * chord);
                int ylo = (ay < by ? ay : by) - 2, yhi = (ay > by ? ay : by) + 2;
                for (int y = ylo; y < yhi && g->n < DROWS; ++y) {
                    float yc = (float)y + .5f, xc = (float)ax + (yc - (float)ay) * (float)(dx * dy) + sh;
                    float s = ((xc - (float)ax) * (float)dx + (yc - (float)ay) * (float)dy) * 0.70710678f;
                    if (s < chord || s > Ld - chord) continue;
                    if (!g->n) g->y0 = (int16_t)y;
                    g->xc[g->n] = (int16_t)(xc * 16.f);
                    g->t[g->n] = (int16_t)(s - chord);
                    ++g->n;
                }
            }
        }
}
static int send_w(float s) { float a = nb_absf(s); return a < .34f ? 2 : a < .67f ? 3 : 4; }
/* the solid: a 256-step sine in Q14, a fixed tilt, orthographic */
static int16_t sinq[256];
static void solid_init(void)
{
    for (int i = 0; i < 256; ++i) {
        float a = (float)i * (6.2831853f / 256.f);
        if (a > 3.1415927f) a -= 6.2831853f;
        sinq[i] = (int16_t)(nb_sin(a) * 16384.f);
    }
}

static float follow(float x, float target, float dt, float up, float down)
{
    float tau = target > x ? up : down;
    float k = dt / (tau + dt);
    return x + (target - x) * k;
}
static int hyst(int q, float x) { return nb_absf(x - (float)q) > .6f ? (int)(x + .5f) : q; }

static void plan(const nb_view *v, const nb_hud *h, float fs)
{
    if (!F.init) {
        F.init = 1;
        hw_init(); sends_init(); solid_init();
        F.t_prev = v->time;
    }
    float dt = (float)(v->time - F.t_prev) / fs;
    F.t_prev = v->time;
    if (dt < 0.f || dt > .1f) dt = .033f;
    F.secs += dt;
    palette(h ? h->bright : 1.f);
    nb_engine_want_spectrum(h && h->view == NB_VIEW_FREQ);   /* measured only while it is on the screen */
    if (v->spec_on)
        for (int k = 0; k < NB_SPEC; ++k) F.sp[k] = follow(F.sp[k], (float)v->spec[k], dt, .03f, .3f);   /* dB */
    g_struck = (h ? h->struck : 0) | v->struck;
    g_fxcut = h ? h->fx_cut : 0;
    g_gated = h && h->gating && (v->gate < .99f || v->ret < .99f);
    F.out = follow(F.out, nb_clamp(v->out * 1.4142f, 0.f, 2.f), dt, .03f, .25f);
    {   /* the limiter's reduction as a red part of the output bar, 6 dB = 40 px */
        float gr = v->limit < 1.f && v->limit > 0.f ? -6.0206f * nb_log2(v->limit) : 0.f;
        F.gr = hyst(F.gr, nb_clamp(gr * 6.67f, 0.f, 60.f));
    }
    if (h && h->freeze) return;                       /* FREEZE: the picture holds while the sound goes on */
    uint16_t fx_in = h ? h->fx_in : 0;
    float flow_sum = 0.f;
    for (int i = 0; i < 4; ++i) {
        float lv = nb_clamp(v->loud[i] * 1.4142f, 0.f, 2.f);   /* 1 = the RMS of a full-scale sine */
        F.lv[i] = follow(F.lv[i], lv, dt, .03f, .25f);
        F.ht[i] = follow(F.ht[i], v->heat[i], dt, .05f, .4f);
        F.rq[i] = hyst(F.rq[i], 18.f * nb_sqrt(nb_clamp(F.lv[i] * .8f, 0.f, 1.f)));
        F.hq[i] = hyst(F.hq[i], 4.f * F.ht[i]);
        F.heard[i] = v->level[i] > 0.05f * (v->loud[i] + 1e-6f);
        /* the chain: stage 0 the preamp's heat, 1..12 the effects in the chain (out of it: not drawn) */
        for (int k = 0; k < 13; ++k) {
            float a;
            if (k == 0) a = F.ht[i];
            else { F.st[i][k - 1] = follow(F.st[i][k - 1], (float)v->stage[i][k - 1] * (1.f / 127.5f), dt, .05f, .3f); a = F.st[i][k - 1]; }
            int in = k == 0 || ((fx_in >> (k - 1)) & 1u);
            F.seg[i][k] = !in ? -1 : a < .08f ? 1 : a < .35f ? 2 : 3;
            F.cq[i][k] = hyst(F.cq[i][k], 22.f * nb_clamp(a * .8f, 0.f, 1.f));   /* a change as big as the input at 80 % */
            F.cov[i][k] = (uint8_t)(a * .8f > 1.f);
        }
        for (int j = 0; j < 4; ++j) {
            if (i == j) continue;
            F.fl[i][j] = follow(F.fl[i][j], nb_clamp(v->flow[i][j] * 3.f, 0.f, 1.f), dt, .15f, .15f);
            flow_sum += F.fl[i][j];
            const sendline *g = &SL[i][j];
            if (g->len > 0) {
                F.ph[i][j] += (12.f + 48.f * F.fl[i][j]) * dt;
                while (F.ph[i][j] >= (float)g->len) F.ph[i][j] -= (float)g->len;
            }
        }
        /* WAVES: each loop's columns to rows of its lane (centre 54 + 44 i, 18 px for half the full scale) */
        int yc = 54 + 44 * i, pa = -1, pb = -1;
        for (int c = 0; c < NB_WAVE; ++c) {
            int a = yc - (int)v->wave_hi[i][c] * 16 / 127, z = yc - (int)v->wave_lo[i][c] * 16 / 127 + 1;
            if (a < yc - 20) a = yc - 20;
            if (z > yc + 21) z = yc + 21;
            if (pa >= 0) { if (a > pb - 1) a = pb - 1; if (z < pa + 1) z = pa + 1; }
            if (z - a < 2) { a = (a + z) / 2 - 1; z = a + 2; }
            F.wa[i][c] = (uint8_t)a; F.wb[i][c] = (uint8_t)z;
            pa = a; pb = z;
        }
    }
    /* SCOPE: the output's columns to rows (centre 120, 84 px for half the full scale), each loop's line under it */
    {
        int pa = -1, pb = -1;
        for (int c = 0; c < NB_SCOPE; ++c) {
            int a = 120 - (int)v->scope_hi[c] * 84 / 127, z = 120 - (int)v->scope_lo[c] * 84 / 127 + 1;
            F.so[c] = (uint8_t)((v->scope_hi[c] >= 127) | ((v->scope_lo[c] <= -127) << 1));
            if (a < 36) a = 36;
            if (z > 205) z = 205;
            if (pa >= 0) { if (a > pb - 1) a = pb - 1; if (z < pa + 1) z = pa + 1; }
            if (z - a < 2) { a = (a + z) / 2 - 1; z = a + 2; }
            F.sa[c] = (uint8_t)a; F.sb[c] = (uint8_t)z;
            pa = a; pb = z;
            for (int i = 0; i < 4; ++i) {
                int y = 120 - (int)v->scope_ch[i][c] * 84 / 127;
                F.sc[i][c] = (uint8_t)(y < 36 ? 36 : y > 204 ? 204 : y);
            }
        }
    }
    /* TETRA: turns as fast as signal flows (silence: it stops), the SPIN keys add 30 degrees each */
    F.ang += 18.f * nb_clamp(flow_sum * .5f, 0.f, 1.f) * dt * (256.f / 360.f);
    while (F.ang >= 256.f) F.ang -= 256.f;
    F.spin = follow(F.spin, (float)(h ? h->spin : 0) * 21.333f, dt, .12f, .12f);
    {
        static const int8_t TV[4][3] = { { 1, 1, 1 }, { 1, -1, -1 }, { -1, 1, -1 }, { -1, -1, 1 } };
        int a = ((int)(F.ang + F.spin + 512.f)) & 255;
        float ca = (float)sinq[(a + 64) & 255] * (1.f / 16384.f), sa = (float)sinq[a] * (1.f / 16384.f);
        const float tc = 0.8139f, ts = 0.5810f, n3 = 0.57735f;   /* the tilt, 0.62 rad; 1/sqrt 3 */
        for (int i = 0; i < 4; ++i) {
            float x = TV[i][0] * n3, y = TV[i][1] * n3, z = TV[i][2] * n3;
            float x1 = x * ca + z * sa, z1 = -x * sa + z * ca;
            float y2 = y * tc - z1 * ts, z2 = y * ts + z1 * tc;
            float lvd = nb_clamp(F.lv[i] * .8f, 0.f, 1.f), R = 74.f * (.5f + .5f * nb_sqrt(lvd));
            F.vx[i] = (int)(120.f + x1 * R + .5f); F.vy[i] = (int)(120.f - y2 * R + .5f); F.vz[i] = z2;
            F.vr[i] = 3 + (int)(5.f * nb_sqrt(lvd) + .5f);
        }
        if (++F.frame % 2 == 0) {
            for (int i = 0; i < 4; ++i) {
                for (int k = 11; k > 0; --k) { F.tr[i][k][0] = F.tr[i][k - 1][0]; F.tr[i][k][1] = F.tr[i][k - 1][1]; }
                F.tr[i][0][0] = F.vx[i]; F.tr[i][0][1] = F.vy[i];
            }
            if (F.ntr < 12) ++F.ntr;
        }
    }
}

/* ---- the header, the footer ---- */
static const char *const VIEW_NAME[NB_NVIEW] = { "SCOPE", "LOOPS", "MATRIX", "CHAIN", "WAVES", "FREQ", "TETRA" };

static void header(const band *b, const nb_hud *h)
{
    if (b->cy0 >= 32) return;
    if (h->view_flash) {                   /* a view just chosen, or the view key held: its name where the page goes */
        int w = text_w(VIEW_NAME[h->view % NB_NVIEW], FM, -1);
        rect(b, 4, 2, 12 + w, 30, C_INK);
        text(b, VIEW_NAME[h->view % NB_NVIEW], 8, 5, FM, C_BG, C_INK, AL, -1);
    } else text(b, h->page, 8, 5, FM, C_INK, C_BG, AL, -1);
    if (h->any_mod) { rect(b, 144, 6, 189, 27, C_LIME); text(b, "MOD", 148, 9, FS, C_BG, C_LIME, AL, -1); }
    for (int k = 0; k < 4; ++k) {
        if (h->kkind[k] == NB_KIND_NONE) continue;
        int x0 = 196 + 9 * k, fill = h->turning == k ? C_LIME : C_GREY;
        rect(b, x0, 5, x0 + 6, 27, C_TRACK);
        float v = nb_clamp(h->kval[k], 0.f, 1.f);
        if (h->kkind[k] == NB_KIND_BI) {
            int yv = 27 - (int)(22.f * v + .5f);
            rect(b, x0, yv < 16 ? yv : 16, x0 + 6, (yv > 16 ? yv : 16) + (yv == 16), fill);
        } else { int hh = (int)(22.f * v + .5f); rect(b, x0, 27 - hh, x0 + 6, 27, fill); }
        if (h->kmod[k] >= 0.f) {            /* moving by itself (a modulation, a motion): a lime tick */
            int yt = 27 - (int)(22.f * nb_clamp(h->kmod[k], 0.f, 1.f) + .5f);
            rect(b, x0 - 1, yt - 1 < 5 ? 5 : yt - 1, x0 + 7, yt + 1 > 27 ? 27 : yt + 1, C_LIME);
        }
    }
}
static void footer(const band *b, const nb_hud *h)
{
    if (b->cy1 <= 208) return;
    if (h->motion) {
        rect(b, 0, 208, W, 211, C_DIM);
        rect(b, 0, 208, (int)(nb_clamp(h->motion_phase, 0.f, 1.f) * (float)W + .5f), 211, h->motion == 2 ? C_HEAT : C_LIME);
    }
    if (h->foot_hot) text(b, h->foot, 8, 216, FS, g_gated ? C_HEAT : C_INK, C_BG, AL, -1);
    else {
        const int x0 = 8, x1 = 150, y0 = 219, y1 = 229, full = x0 + (x1 - x0) * 4 / 5;
        rect(b, x0, y0, x1, y1, C_TRACK);
        int w = (int)((float)(x1 - x0) * nb_clamp(F.out * .8f, 0.f, 1.f) + .5f);
        if (w > 0) rect(b, x0, y0, x0 + w, y1, F.out > .01f ? C_GREY : C_TRACK);
        rect(b, full, y0 - 2, full + 1, y1 + 2, C_DIM);                  /* where full scale is */
        if (F.gr > 0) rect(b, x1 - F.gr, y0, x1, y1, C_HEAT);            /* the limiter's reduction, from the right */
    }
    if (h->drone) { rect(b, 161, 213, 232, 234, C_LIME); text(b, "DRONE", 165, 216, FS, C_BG, C_LIME, AL, -1); }
}
static void loop_label(const band *b, int i, int x, int y, int sel)
{
    char d[2] = { (char)('1' + i), 0 };
    if ((g_struck >> i) & 1) { rect(b, x - 3, y - 3, x + 19, y + 25, C_LIME); text(b, d, x, y, FM, C_BG, C_LIME, AL, -1); }
    else if (sel) { rect(b, x - 3, y - 3, x + 19, y + 25, C_INK); text(b, d, x, y, FM, C_BG, C_INK, AL, -1); }
    else text(b, d, x, y, FM, g_gated ? C_HEAT : C_GREY, C_BG, AL, -1);
}
static int lime_of(int i, int bright) { return F.heard[i] ? (bright ? C_LIME : C_LIME40) : C_LIME40; }

/* the loop's pitch from its period, as digits */
static void hz_text(const nb_view *v, int i, float fs, char *hz)
{
    float per = v->period[i] * (v->bend > 0.f ? v->bend : 1.f);
    int f = per > 1.f ? (int)(fs / per + .5f) : 0, n = 0;
    char d[8];
    /* four places at most (the column is the loop's number's neighbour): 892, 1.4K, 12K */
    int kilo = f >= 1000, tenth = -1;
    if (kilo) { if (f < 10000) tenth = (f % 1000) / 100; f /= 1000; }
    do { d[n++] = (char)('0' + f % 10); f /= 10; } while (f && n < 6);
    int m = 0;
    for (int k = 0; k < n; ++k) hz[m++] = d[n - 1 - k];
    if (tenth >= 0) { hz[m++] = '.'; hz[m++] = (char)('0' + tenth); }
    if (kilo) hz[m++] = 'K';
    hz[m] = 0;
}

/* ---- the views ---- */
/* SCOPE, the one after the splash: the output (the four as it has them) big, two periods of loop 1 so a steady drone
 * stands still; with the view key held, each loop alone under it, thin and dim, to see which one makes which part */
static void view_scope(const band *b, const nb_view *v, const nb_hud *h)
{
    rect(b, 0, 120, W, 121, C_DIM);
    for (int i = 0; i < 4 && h->layer; ++i) {         /* the view key held: each loop alone, under the output */
        if (F.rq[i] < 2) continue;
        uint16_t col = pal[C_LIME40];
        for (int c = 1; c < NB_SCOPE; ++c) {
            int y0 = F.sc[i][c - 1], y1 = F.sc[i][c], lo = y0 < y1 ? y0 : y1, hi = (y0 > y1 ? y0 : y1) + 1;
            if (hi <= b->cy0 || lo >= b->cy1) continue;          /* not in this band */
            if (lo < b->cy0) lo = b->cy0;
            if (hi > b->cy1) hi = b->cy1;
            for (int y = lo; y < hi; ++y) b->pix[(y - b->y0) * W + c] = col;
            CNT(cnt_fill, hi - lo);
        }
    }
    int on = 0;
    for (int i = 0; i < 4; ++i) on |= F.rq[i] >= 2 && F.heard[i];
    if (v->scope_free) text(b, "FREE", 232, 38, FS, C_GREY, C_BG, AR, -1);   /* no crossing: the window runs free */
    uint16_t col = pal[on ? C_LIME : C_IDLE];
    for (int c = 0; c < NB_SCOPE; ++c) {
        int lo = F.sa[c], hi = F.sb[c];
        if (hi <= b->cy0 || lo >= b->cy1) continue;
        if (lo < b->cy0) lo = b->cy0;
        if (hi > b->cy1) hi = b->cy1;
        for (int y = lo; y < hi; ++y) b->pix[(y - b->y0) * W + c] = col;
        CNT(cnt_fill, hi - lo);
    }
    for (int c = 0; c < NB_SCOPE; ++c) {                 /* over the range: a red mark at the edge, not a flat cut */
        if (F.so[c] & 1u) rect(b, c, 34, c + 1, 37, C_HEAT);
        if (F.so[c] & 2u) rect(b, c, 204, c + 1, 207, C_HEAT);
    }
}

/* LOOPS, the default: one strip a loop, the question "which loop sings what, how loud, how hot, which effects work
 * on it". The number, its pitch in Hz, its level as a bar, under it the twelve effects as ticks (lit by what each
 * changes now, in the chain's order: the white keys under FX), its heat as a red column. */
static void view_strips(const band *b, const nb_view *v, const nb_hud *h, float fs)
{
    for (int i = 0; i < 4; ++i) {
        int y0 = 32 + 44 * i, sing = F.rq[i] >= 2;
        if (y0 + 44 <= b->cy0 || y0 >= b->cy1) continue;
        loop_label(b, i, 8, y0 + 6, h->focus_loop == i);
        char hz[8];
        hz_text(v, i, fs, hz);
        text(b, hz, 98, y0 + 6, FM, sing ? C_INK : C_IDLE, C_BG, AR, -1);
        text(b, "HZ", 98, y0 + 30, FS, C_GREY, C_BG, AR, -1);
        const int x0 = 106, x1 = 202;
        rect(b, x0, y0 + 8, x1, y0 + 22, C_TRACK);
        int lw = (int)((float)(x1 - x0) * nb_clamp(F.lv[i] * .8f, 0.f, 1.f) + .5f);   /* full scale at 80 % */
        if (lw > 0) rect(b, x0, y0 + 8, x0 + lw, y0 + 22, sing ? lime_of(i, 1) : C_IDLE);
        rect(b, x0 + (x1 - x0) * 4 / 5, y0 + 6, x0 + (x1 - x0) * 4 / 5 + 1, y0 + 8, C_DIM);   /* where full scale is */
        if (F.lv[i] * .8f > 1.f) rect(b, x1 - 3, y0 + 6, x1, y0 + 24, C_HEAT);
        for (int k = 1; k < 13; ++k) {
            int x = x0 + (k - 1) * 8, s = F.seg[i][k];
            if (s < 0 && ((g_fxcut >> (k - 1)) & 1)) outline(b, x, y0 + 28, x + 7, y0 + 36, C_HEAT);
            else if (s < 0) rect(b, x, y0 + 33, x + 7, y0 + 34, C_DIM);
            else rect(b, x, y0 + 28, x + 7, y0 + 36, s == 1 ? C_DIM : s == 2 ? C_LIME40 : lime_of(i, 1));
        }
        rect(b, 210, y0 + 8, 234, y0 + 36, C_TRACK);
        int hh = (int)(28.f * nb_clamp(F.ht[i], 0.f, 1.f) + .5f);
        if (hh > 0) rect(b, 210, y0 + 36 - hh, 234, y0 + 36, C_HEAT);
    }
}

/* FREQ: "where the energy is, and which loop puts it there". The spectrum of what the loops send out (before the
 * post), LOW then thirds of an octave to 10 kHz, in dB: 0 dB (a full-scale sine of one loop) at 80 %, -60 dB the
 * floor, red above 0 dB; each loop's number over its pitch. */
static int freq_x(float hz)                   /* a pitch to x on the bands' scale */
{
    float b = hz > 40.f ? 1.f + 3.f * nb_log2(hz / 63.f) : 0.f;
    if (b < 0.f) b = 0.f;
    if (b > (float)(NB_SPEC - 1)) b = (float)(NB_SPEC - 1);
    return 42 + (int)((b + .5f) * 186.f / (float)NB_SPEC);
}
static void view_freq(const band *b, const nb_view *v, const nb_hud *h, float fs)
{
    (void)h;
    const int X0 = 42, BASE = 186, ZERO = BASE - 117;          /* 0 dB at 80 % of 146 px; the dB labels left of x 42 */
    if (v->spec_on)
        for (int k = 0; k < NB_SPEC; ++k) {
            int x = X0 + k * 186 / NB_SPEC, hh = (int)((F.sp[k] + 60.f) * (117.f / 60.f) + .5f);
            if (hh <= 0) continue;
            if (hh > 146) hh = 146;
            int top = BASE - hh;
            rect(b, x, top > ZERO ? top : ZERO, x + 6, BASE, C_LIME);
            if (top < ZERO) rect(b, x, top, x + 6, ZERO, C_HEAT);          /* over 0 dB */
        }
    rect(b, X0, BASE, 228, BASE + 1, C_DIM);
    static const char *const DB[3] = { "0", "-20", "-40" };
    for (int i = 0; i < 3; ++i) {
        int y = ZERO + i * 39;
        rect(b, 39, y, 41, y + 1, C_DIM);
        text(b, DB[i], 37, y - 7, FS, C_GREY, C_BG, AR, -1);
    }
    text(b, "LOW", X0, 190, FS, C_GREY, C_BG, AL, -1);
    text(b, "1K", freq_x(1000.f), 190, FS, C_GREY, C_BG, AC, -1);
    text(b, "10K", freq_x(10000.f), 190, FS, C_GREY, C_BG, AR, -1);
    for (int i = 0; i < 4; ++i) {               /* each loop's number over its pitch */
        float per = v->period[i] * (v->bend > 0.f ? v->bend : 1.f);
        if (per <= 1.f) continue;
        int x = freq_x(fs / per), col = F.rq[i] >= 2 ? lime_of(i, 1) : C_IDLE;
        char d[2] = { (char)('1' + i), 0 };
        text(b, d, x, 36 + 16 * (i & 1), FS, (g_struck >> i) & 1 ? C_LIME : g_gated ? C_HEAT : col == C_IDLE ? C_GREY : col, C_BG, AC, -1);
        rect(b, x - 1, BASE + 1, x + 1, BASE + 4, col);
    }
}

static void view_waves(const band *b, const nb_view *v, const nb_hud *h, float fs)
{
    for (int i = 0; i < 4; ++i) {
        int yc = 54 + 44 * i;
        if (yc + 22 <= b->cy0 || yc - 22 >= b->cy1) continue;
        loop_label(b, i, 8, yc - 21, h->focus_loop == i);
        char hz[8];
        hz_text(v, i, fs, hz);                    /* the loop's pitch, from its period and SCREAM's bend */
        text(b, hz, 6, yc + 4, FS, C_GREY, C_BG, AL, -1);
        rect(b, 58, yc, 58 + NB_WAVE, yc + 1, C_DIM);
        if ((v->wave_free >> i) & 1u) text(b, "FREE", 233, yc - 20, FS, C_GREY, C_BG, AR, -1);
        int col = F.rq[i] >= 2 ? lime_of(i, 1) : C_IDLE;
        for (int c = 0; c < NB_WAVE; ++c) rect(b, 58 + c, F.wa[i][c], 59 + c, F.wb[i][c], col);
    }
}

/* a send as a number: its effective share in percent, signed (-99..99) */
static void pct_text(float s, char *o)
{
    int v = (int)(s * 100.f + (s < 0.f ? -.5f : .5f)), n = 0;
    if (v > 99) v = 99;
    if (v < -99) v = -99;
    if (v < 0) { o[n++] = '-'; v = -v; }
    if (v >= 10) o[n++] = (char)('0' + v / 10);
    o[n++] = (char)('0' + v % 10);
    o[n] = 0;
}

/* MATRIX: "who feeds whom, and how much". Rows receive (INTO), columns send (FROM): the INTO pages laid out. Each
 * cell its send in percent (lime, blue inverted, grey set but silent) over a bar of its size; the diagonal the loop
 * itself (its level, its heat). The row K1..K4 edit (the INTO page) is framed; the column being turned is lime. */
static void view_matrix(const band *b, const nb_view *v, const nb_hud *h)
{
    const int GX = 52, GY = 64, PX = 44, PY = 36, CW = 40, CH = 32;
    for (int c = 0; c < 4; ++c) {
        char d[2] = { (char)('1' + c), 0 };
        text(b, d, GX + c * PX + 12, 36, FM, (h->into >= 0 && h->turning == c) ? C_LIME : C_GREY, C_BG, AL, -1);
    }
    for (int r = 0; r < 4; ++r) {
        int y = GY + r * PY;
        if (y + CH + 3 <= b->cy0 || y - 3 >= b->cy1) continue;
        loop_label(b, r, 28, y + 5, h->into == r || h->focus_loop == r);
        if (h->into == r) {                      /* the row K1..K4 edit now */
            rect(b, GX - 3, y - 3, GX + 3 * PX + CW + 3, y - 1, C_INK);
            rect(b, GX - 3, y + CH + 1, GX + 3 * PX + CW + 3, y + CH + 3, C_INK);
        }
        for (int c = 0; c < 4; ++c) {
            int x = GX + c * PX;
            if (r == c) {
                int hq = F.hq[r];
                if (hq >= 1) { rect(b, x, y, x + CW, y + CH, C_HEAT); rect(b, x + hq, y + hq, x + CW - hq, y + CH - hq, C_TRACK); }
                else rect(b, x, y, x + CW, y + CH, C_TRACK);
                int rr = F.rq[r] * 12 / 18;
                if (rr >= 2) disk(b, x + CW / 2, y + CH / 2, rr, lime_of(r, 1));
                continue;
            }
            rect(b, x, y, x + CW, y + CH, C_TRACK);
            float s = v->send[r][c];
            if (nb_absf(s) < .01f) continue;
            int live = F.fl[r][c] > .04f && (h->into < 0 || h->into == r);
            int col = !live ? C_GREY : s < 0.f ? C_COLD : C_LIME;
            char t[5];
            pct_text(s, t);
            text(b, t, x + CW / 2, y + 4, FS, col, C_TRACK, AC, -1);
            int w = (int)((float)(CW - 6) * nb_clamp(nb_absf(s), 0.f, 1.f) + .5f);
            if (w < 2) w = 2;
            rect(b, x + 3, y + 24, x + 3 + w, y + 28, live ? col : C_DIM);
        }
    }
}

/* CHAIN: "which effect does what to which loop". Each loop's stages in the chain's order (the preamp, then the twelve
 * as the white keys under FX), each a meter of what it changes now; under them the post and the limiter. On an
 * effect page the effect is named with its MIX, and its four knobs (what K1..K4 turn) are written out below. */
static void view_chain(const band *b, const nb_view *v, const nb_hud *h)
{
    const int X = 46, P = 14, CW = 11, Y = 64, R = 18, CH = 14;
    int focus = -1;
    for (int f = 0; f < NB_NLOOPFX; ++f) {
        const char *n = nb_fx_name(f), *q = h->page;
        while (*n && *n == *q) { ++n; ++q; }
        if (!*n && !*q) focus = f;
    }
    int page = focus >= 0;
    if (!page) {
        float best = .02f;
        for (int f = 0; f < NB_NLOOPFX; ++f)
            for (int i = 0; i < 4; ++i) if (F.st[i][f] > best) { best = F.st[i][f]; focus = f; }
    }
    if (b->cy0 < 60) {
        if (!page || h->view_flash) text(b, focus >= 0 ? nb_fx_name(focus) : "DRIVE", 8, 36, FM, C_INK, C_BG, AL, -1);
        if (page && h->bypass) text(b, "BYPASS", 232, 36, FM, C_HEAT, C_BG, AR, -1);   /* the MIX does nothing now */
        else if (page && ((g_fxcut >> focus) & 1)) text(b, "LEFT OUT", 232, 38, FS, C_HEAT, C_BG, AR, -1);   /* the budget */
        else if (page && h->ktext[3][0]) {      /* its MIX */
            int w = text_w(h->ktext[3], FM, -1);
            text(b, h->ktext[3], 232, 36, FM, (h->fx_in >> focus) & 1u ? C_LIME : C_GREY, C_BG, AR, -1);
            text(b, "MIX", 232 - w - 8, 40, FS, C_GREY, C_BG, AR, -1);
        }
        if (focus >= 0) { int x = X + (focus + 1) * P; rect(b, x, Y - 4, x + CW, Y - 2, C_INK); }
    }
    for (int i = 0; i < 4; ++i) {
        int y = Y + i * R;
        if (y + CH <= b->cy0 || y >= b->cy1) continue;
        char d[2] = { (char)('1' + i), 0 };
        text(b, d, 30, y + 1, FS, h->focus_loop == i ? C_INK : C_GREY, C_BG, AL, -1);
        for (int k = 0; k < 13; ++k) {
            int x = X + k * P;
            if (F.seg[i][k] < 0) {
                if (k > 0 && ((g_fxcut >> (k - 1)) & 1)) outline(b, x, y, x + CW, y + CH, C_HEAT);   /* left out */
                else rect(b, x, y + CH - 1, x + CW, y + CH, C_DIM);                                   /* off */
                continue;
            }
            int hh = F.cq[i][k] * CH / 22;
            rect(b, x, y, x + CW, y + CH - hh, C_TRACK);
            if (hh > 0) rect(b, x, y + CH - hh, x + CW, y + CH, k == 0 ? C_HEAT : lime_of(i, 1));
            if (F.cov[i][k]) rect(b, x, y - 2, x + CW, y, C_HEAT);
        }
    }
    if (!page) {   /* the post's return and the limiter's reduction (the network at the ceiling) */
        int y = Y + 4 * R + 4;
        if (y + CH > b->cy0 && y < b->cy1) {
            text(b, "LIMIT", 6, y + 1, FS, C_GREY, C_BG, AL, -1);
            float gr = v->limit < 1.f && v->limit > 0.f ? -6.0206f * nb_log2(v->limit) : 0.f;   /* the reduction, dB */
            int lim = (int)(CH * nb_clamp(gr / 7.5f, 0.f, 1.f) + .5f);                       /* 6 dB at 80 % */
            int x = X + 12 * P;
            rect(b, x, y, x + CW, y + CH - lim, C_TRACK);
            if (lim) rect(b, x, y + CH - lim, x + CW, y + CH, C_HEAT);
        }
    }
    if (page && b->cy1 > 139) {                 /* the effect's four knobs, one a line, as K1..K4 turn them */
        for (int k = 0; k < 4; ++k) {
            int y = 139 + 17 * k;
            for (int d = 0; d < 4; ++d) rect(b, 8 + 5 * d, y + 6, 12 + 5 * d, y + 10, d == k ? C_INK : C_DIM);
            text(b, h->knames[k], 34, y, FS, h->turning == k ? C_LIME : C_GREY, C_BG, AL, -1);
            text(b, h->ktext[k], 232, y, FS, h->turning == k ? C_LIME : C_INK, C_BG, AR, -1);
        }
    }
}

static void view_tetra(const band *b, const nb_view *v, const nb_hud *h)
{
    /* the trails: the last 0.8 s of each corner, stepped down (no fade per pixel) */
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k + 1 < F.ntr; ++k) {
            int col = k < 4 ? (F.rq[i] >= 2 ? C_LIME40 : C_DIM) : k < 8 ? C_DIM : C_TRACK;
            line(b, (float)F.tr[i][k][0], (float)F.tr[i][k][1], (float)F.tr[i][k + 1][0], (float)F.tr[i][k + 1][1], 2.f, col);
        }
    /* the six edges, back to front: each the stronger of its two sends (on INTO n: what goes into n) */
    int ei[6], ej[6], ord[6], n = 0;
    float ez[6];
    for (int i = 0; i < 4; ++i)
        for (int j = i + 1; j < 4; ++j) { ei[n] = i; ej[n] = j; ez[n] = (F.vz[i] + F.vz[j]) * .5f; ord[n] = n; ++n; }
    for (int a = 1; a < 6; ++a) for (int c = a; c > 0 && ez[ord[c]] < ez[ord[c - 1]]; --c) { int t = ord[c]; ord[c] = ord[c - 1]; ord[c - 1] = t; }
    for (int q = 0; q < 6; ++q) {
        int e = ord[q], i = ei[e], j = ej[e];
        float sij = v->send[i][j], sji = v->send[j][i];
        int into_j = h->into == j || (h->into != i && nb_absf(sji) >= nb_absf(sij));
        int from = into_j ? i : j, to = into_j ? j : i;
        float s = into_j ? sji : sij;
        float ax = (float)F.vx[from], ay = (float)F.vy[from], bx = (float)F.vx[to], by = (float)F.vy[to];
        if (nb_absf(s) < .01f) { line(b, ax, ay, bx, by, 1.f, C_DIM); continue; }
        int back = ez[e] < 0.f, live = F.fl[to][from] > .04f && (h->into < 0 || h->into == to);
        float w = (float)(send_w(s) - back);
        if (!live) { line(b, ax, ay, bx, by, w, C_DIM); continue; }
        line(b, ax, ay, bx, by, w, s < 0.f ? C_COLD40 : C_LIME40);
        if (back) continue;
        float dx = bx - ax, dy = by - ay, L = nb_sqrt(dx * dx + dy * dy);
        if (L < 2.f) continue;
        const sendline *g = &SL[to][from];
        float t0 = g->len > 0 ? F.ph[to][from] / (float)g->len * L : 0.f, t1 = t0 + 10.f > L ? L : t0 + 10.f;
        line(b, ax + dx / L * t0, ay + dy / L * t0, ax + dx / L * t1, ay + dy / L * t1, w, s < 0.f ? C_COLD : C_LIME);
    }
    /* the corners, back to front; the numbers first, outward from the centre (to the right near it) */
    int o[4] = { 0, 1, 2, 3 };
    for (int a = 1; a < 4; ++a) for (int c = a; c > 0 && F.vz[o[c]] < F.vz[o[c - 1]]; --c) { int t = o[c]; o[c] = o[c - 1]; o[c - 1] = t; }
    for (int q = 0; q < 4; ++q) {
        int i = o[q];
        float nx = (float)(F.vx[i] - 120), ny = (float)(F.vy[i] - 120), d = nb_sqrt(nx * nx + ny * ny);
        if (d < 24.f) { nx = 1.f; ny = -.4f; d = 1.077f; }
        int lx = (int)((float)F.vx[i] + nx / d * (float)(F.vr[i] + 18)) - 8, ly = (int)((float)F.vy[i] + ny / d * (float)(F.vr[i] + 18)) - 11;
        lx = lx < 2 ? 2 : lx > 222 ? 222 : lx;
        ly = ly < 36 ? 36 : ly > 184 ? 184 : ly;
        if (h->focus_loop == i) loop_label(b, i, lx, ly, 1);
        else { rect(b, lx - 1, ly - 1, lx + 17, ly + 23, C_BG); loop_label(b, i, lx, ly, 0); }
    }
    for (int q = 0; q < 4; ++q) {
        int i = o[q], r = F.vr[i];
        if (F.hq[i] >= 1) ring(b, F.vx[i], F.vy[i], r + 2, r + 2 + (F.hq[i] > 3 ? 3 : F.hq[i]), C_HEAT);
        if (F.rq[i] >= 2) disk(b, F.vx[i], F.vy[i], r, lime_of(i, 1));
        else ring(b, F.vx[i], F.vy[i], r - 2, r, C_IDLE);
    }
}

static void stage(const band *b, const nb_view *v, const nb_hud *h, float fs)
{
    if (b->cy1 <= 32 || b->cy0 >= 208) return;
    switch (h->view) {
    case NB_VIEW_WAVES: view_waves(b, v, h, fs); break;
    case NB_VIEW_MATRIX: view_matrix(b, v, h); break;
    case NB_VIEW_CHAIN: view_chain(b, v, h); break;
    case NB_VIEW_FREQ: view_freq(b, v, h, fs); break;
    case NB_VIEW_TETRA: view_tetra(b, v, h); break;
    case NB_VIEW_LOOPS: view_strips(b, v, h, fs); break;
    default: view_scope(b, v, h); break;
    }
}

/* ---- what was just touched ---- */
static void bar(const band *b, int x0, int y0, int x1, int y1, float v, int bip, int fill)
{
    rect(b, x0, y0, x1, y1, C_TRACK);
    int xv = x0 + (int)((float)(x1 - x0) * nb_clamp(v, 0.f, 1.f) + .5f);
    if (bip) {
        int xm = (x0 + x1) / 2;
        rect(b, xv < xm ? xv : xm, y0, (xv > xm ? xv : xm) + (xv == xm), y1, fill);
        rect(b, xm - 1, y0 - 4, xm + 1, y1 + 4, C_INK);
    } else rect(b, x0, y0, xv, y1, fill);
}
/* a number in V with its unit in M (the number right-aligned on column xr), or a word in L */
static void value_line(const band *b, const char *val, int y, int xr)
{
    char num[16], unit[8];
    if (split_value(val, num, unit)) {
        if (xr < 0) { int wn = text_w(num, FV, -1), gw = wn + (unit[0] ? 8 + text_w(unit, FM, -1) : 0); xr = 120 - gw / 2 + wn; }
        text(b, num, xr, y, FV, C_INK, C_BG, AR, -1);
        if (unit[0]) text(b, unit, xr + 8, y + 51 - 22, FM, C_GREY, C_BG, AL, -1);
    } else text(b, num, 120, y + 10, text_w(num, FL, -1) <= 228 ? FL : FM, C_INK, C_BG, AC, -1);
}

static void knob_card(const band *b, const nb_hud *h)
{
    if (b->cy1 <= 32 || b->cy0 >= 208) return;
    text(b, h->title, 120, 40, text_w(h->title, FL, -1) <= 228 ? FL : FM, C_GREY, C_BG, AC, -1);
    /* the number stays on the column it opened on: fixed-width digits, nothing moves while it turns */
    char num[16], unit[8];
    if (F.hud_t0 != h->t0 || F.hud_enc != h->enc) {
        F.hud_t0 = h->t0; F.hud_enc = h->enc; F.anchor = -1;
        if (split_value(h->value, num, unit)) {
            int wn = text_w(num, FV, -1), gw = wn + (unit[0] ? 8 + text_w(unit, FM, -1) : 0);
            F.anchor = 120 - gw / 2 + wn;
        }
    }
    value_line(b, h->value, 82, F.anchor);
    if (h->bar >= 0.f) bar(b, 12, 148, 228, 164, h->bar, h->bipolar, C_LIME);
    if (h->mod_now >= 0.f) {
        int lo = 12 + (int)(216.f * h->mod_lo), hi = 12 + (int)(216.f * h->mod_hi), x = 12 + (int)(216.f * nb_clamp(h->mod_now, 0.f, 1.f));
        rect(b, lo, 167, hi, 171, C_LIME40);
        rect(b, x - 1, 144, x + 2, 172, C_LIME);
        char src[16];
        int n = 0, sp = -1;
        while (h->modline[n] && n < 15) { src[n] = h->modline[n]; if (src[n] == ' ') sp = n; ++n; }
        src[n] = 0;
        if (sp > 0) {
            src[sp] = 0;
            int wa = text_w(src, FM, -1), wb = text_w(src + sp + 1, FM, -1), x0 = 120 - (wa + 12 + wb) / 2;
            text(b, src, x0, 182, FM, C_LIME, C_BG, AL, -1);
            text(b, src + sp + 1, x0 + wa + 12, 182, FM, C_GREY, C_BG, AL, -1);
        }
    }
}

static void strip(const band *b, int n, int cur, int y0, int y1, int x0, int width, uint16_t on_mask, int cur_col, int framed,
                  uint16_t cut_mask)
{
    int pitch = width / n, cw = pitch - 4;
    for (int i = 0; i < n; ++i) {
        int x = x0 + i * pitch;
        if (framed && i == cur) { rect(b, x - 3, y0 - 3, x + cw + 3, y1 + 3, C_INK); rect(b, x - 1, y0 - 1, x + cw + 1, y1 + 1, C_BG); }
        if ((cut_mask >> i) & 1u) { outline(b, x, y0, x + cw, y1, C_HEAT); continue; }     /* left out by the budget */
        int col = ((on_mask >> i) & 1u) ? C_LIME : (!framed && i == cur) ? cur_col : C_TRACK;
        rect(b, x, y0, x + cw, y1, col);
    }
}

static void page_card(const band *b, const nb_hud *h)
{
    int tw = text(b, h->title, 10, 6, FL, C_INK, C_BG, AL, -1);
    if (h->strip_n == NB_NLOOPFX && h->strip_cur >= 0 && ((g_fxcut >> h->strip_cur) & 1))
        rect(b, 8, 19, 12 + tw, 22, C_HEAT);                     /* this effect left out by the budget: struck through */
    if (h->strip_n > 0 && !h->rows_text)
        strip(b, h->strip_n, h->strip_cur, 42, 50, 14, 216, h->strip_n == NB_NLOOPFX ? h->fx_in : 0, C_INK, 1,
              h->strip_n == NB_NLOOPFX ? (uint16_t)g_fxcut : 0);
    for (int k = 0; k < 4; ++k) {
        int y0 = 58 + 45 * k;
        if (y0 + 40 <= b->cy0 || y0 >= b->cy1) continue;
        if (h->rows_text) { text(b, h->lines[k], 34, y0, FM, C_INK, C_BG, AL, -1); continue; }
        for (int d = 0; d < 4; ++d) rect(b, 10 + 5 * d, y0 + 9, 14 + 5 * d, y0 + 13, d == k && h->kkind[k] != NB_KIND_NONE ? C_INK : C_DIM);
        if (h->kkind[k] == NB_KIND_NONE) continue;
        int turning = h->turning == k;
        if (h->kkind[k] == NB_KIND_STEP) {
            if (text_w(h->lines[k], FM, -1) + 8 + text_w(h->words[k], FM, -1) <= 194) {
                text(b, h->lines[k], 34, y0, FM, C_GREY, C_BG, AL, -1);
                text(b, h->words[k], 228, y0, FM, C_INK, C_BG, AR, -1);
            } else text(b, h->words[k], 34, y0, FM, C_INK, C_BG, AL, -1);
        } else text(b, h->lines[k], 34, y0, FM, turning ? C_INK : C_GREY, C_BG, AL, -1);
        bar(b, 34, y0 + 29, 228, y0 + 37, h->kval[k], h->kkind[k] == NB_KIND_BI, turning ? C_LIME : C_INK);
    }
}

/* a memory's slot: its bank (A, B) and its number in the bank (A1..A8, B1..B8) */
static void slot_name(int i, char *d)
{
    if (i < 0) i = 0;
    d[0] = (char)('A' + i / 8); d[1] = (char)('1' + i % 8); d[2] = 0;
}

static void text_card(const band *b, const nb_hud *h)
{
    if (b->cy1 <= 32 || b->cy0 >= 208) return;
    int red = h->strip_hot || (h->motion == 2 && h->title[0] == 'M' && h->title[1] == 'O');
    if (h->title[0]) text(b, h->title, 120, 40, FM, red ? C_HEAT : C_GREY, C_BG, AC, -1);
    if (h->strip_n == NB_NSNAP) {             /* a memory: its slot (bank and number) big, the sixteen keys; no name */
        char d[8];
        slot_name(h->strip_cur, d);
        if (h->morph_to >= 0 && h->title[1] == 'O') {       /* MORPH: from where, to where */
            char to[4];
            slot_name(h->morph_to, to);
            int n = 0;
            while (d[n]) ++n;
            d[n++] = '>'; d[n++] = to[0]; d[n++] = to[1]; d[n] = 0;
        }
        text(b, d, 120, 82, FV, C_INK, C_BG, AC, -1);
        strip(b, NB_NSNAP, h->strip_cur, 180, 190, 8, 224, 0, red ? C_HEAT : C_INK, 0, 0);
        return;
    }
    value_line(b, h->value, h->strip_n ? 72 : 82, -1);
    if (h->strip_n) strip(b, h->strip_n, h->strip_cur, 180, 190, 10, 220, (uint16_t)(1u << h->strip_cur), C_LIME, 0, 0);
    if (red && h->motion == 2) { rect(b, 12, 148, 228, 164, C_TRACK); rect(b, 12, 148, 12 + (int)(216.f * nb_clamp(h->motion_phase, 0.f, 1.f)), 164, C_HEAT); }
}

static void splash(const band *b, const nb_hud *h)
{
    /* black, the name in V, a spark where the I's accent is (flickering as it catches), the address under it */
    static const char name[] = "FAISCA";
    int x = 120 - text_w(name, FV, 4) / 2;
    text(b, name, x, 90, FV, C_INK, C_BG, AL, 4);
    if (b->cy0 < 90 && b->cy1 > 50) {
        const nb_font *f = &nb_font_v;
        int xi = x + f->g[f->map['F']].w + 4 + f->g[f->map['A']].w + 4;
        float cx = (float)xi + 4.5f, cy = 76.f, fl = 1.f;   /* still: nothing on the screen moves unless sound does */
        (void)h;
        for (int r = 0; r < 8; ++r) {
            float a = (float)r * .7853982f + .2f;
            float dx = nb_sin((a + 1.5707963f)), dy = nb_sin((a));
            float len = (r & 1 ? 8.f : 14.f) * fl;
            line(b, cx + dx * 3.f, cy + dy * 3.f, cx + dx * (3.f + len), cy + dy * (3.f + len), r & 1 ? 2.f : 3.f, r & 1 ? C_LIME40 : C_LIME);
        }
        disk(b, (int)(cx + .5f), (int)(cy + .5f), 4, C_LIME);
    }
    text(b, "by ruido.art.br", 120, 206, FS, C_GREY, C_BG, AC, -1);
}

/* ---- a band: the frame, then what was touched over it, split by a curtain or a slide ---- */
static void frame_home(band *b, const nb_view *v, const nb_hud *h, float fs)
{
    header(b, h);
    stage(b, v, h, fs);
    footer(b, h);
}
static void frame_over(band *b, const nb_view *v, const nb_hud *h, float fs)
{
    (void)v; (void)fs;
    switch (h->kind) {
    case NB_HUD_SPLASH: splash(b, h); return;
    case NB_HUD_PAGE: page_card(b, h); return;
    case NB_HUD_TEXT: header(b, h); text_card(b, h); footer(b, h); return;
    default: header(b, h); knob_card(b, h); footer(b, h); return;
    }
}
static float ease(float x) { x = nb_clamp(x, 0.f, 1.f); x = 1.f - x; return 1.f - x * x * x; }

void nb_face_band(const nb_view *v, const nb_hud *hud, float fs, unsigned y0, unsigned hgt, uint16_t *pix)
{
    static nb_hud none;
    const nb_hud *h = hud ? hud : &none;
    if (y0 == 0 || !F.init) plan(v, h, fs);
    uint16_t bg = pal[C_BG];
    for (unsigned i = 0; i < NB_FACE_W * hgt; ++i) pix[i] = bg;
    band b = { (int)y0, pix, (int)y0, (int)(y0 + hgt), 0, W, 0 };
    int over = h->kind != NB_HUD_NONE && h->alpha > 0.f;
    if (!over) { frame_home(&b, v, h, fs); return; }
    if (h->alpha < 1.f) {
        /* leaving: a curtain from the top down (the bands go out in the same direction: no tearing) */
        int edge = (int)(ease(1.f - h->alpha) * (float)H + .5f);
        band u = b; if (u.cy1 > edge) u.cy1 = edge;
        if (u.cy0 < u.cy1) frame_home(&u, v, h, fs);
        band o = b; if (o.cy0 < edge) o.cy0 = edge;
        if (o.cy0 < o.cy1) frame_over(&o, v, h, fs);
        return;
    }
    if (h->kind == NB_HUD_PAGE && h->slide && h->age < 120u) {
        /* a page comes in from the right in 120 ms */
        int off = (int)((1.f - ease((float)h->age / 120.f)) * (float)W + .5f);
        band u = b; u.cx1 = off;
        if (u.cx0 < u.cx1) frame_home(&u, v, h, fs);
        band o = b; o.cx0 = off; o.ox = off;
        frame_over(&o, v, h, fs);
        return;
    }
    frame_over(&b, v, h, fs);
}
