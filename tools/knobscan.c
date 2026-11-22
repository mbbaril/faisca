/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Mario Baril
 * The knob bench: steps a knob's raw value across its range on reference sounds, measures the audible change
 * between neighbouring steps, and writes the 17-point curve that is the inverse of the accumulated change: equal
 * turns, equal steps to the ear.
 *   The change: a spectral distance on third-octave bands (level normalised, bands more than 50 dB under the loudest
 *   floored: what cannot be heard does not count), the level change, and the change of the envelope's rhythm (its
 *   autocorrelation to 230 ms: echoes, repeats, a compressor's breathing). Each step is rendered twice with another
 *   noise, and the distance between the two (the chaos of a singing loop, not the knob) is taken off the
 *   neighbours' distance.
 *   The stereo too (side over mid per octave): a reverb's width, a ping-pong.
 *   The contexts: five memories (TETHER EMBER CHALK SMOKE MILL), the loop singing (two of them hotter) and struck;
 *   the effect's other knobs at rest or moved (a knob can do nothing while another sits at 0: RING's FREQ). Each
 *   memory counts the same. An effect's MIX is scanned first; its other knobs are then measured at the MIX where
 *   half and three quarters of its change are in: heard, and the loop still alive (RECTIFY kills it above ~0.4).
 *   cc -O2 -o build/knobscan tools/knobscan.c core/nb_engine.c core/nb_panel.c -lm
 *   build/knobscan             the loop's six knobs: where the change falls, the curves
 *   build/knobscan loop K      one loop knob (0 GAIN, 1 FOCUS, 2 DRIVE, 3 CROSS, 4 SCREAM, 5 MID)
 *   build/knobscan --fx        the 56 effect knobs: the table nb_curve_fx, in C, on stdout
 *   build/knobscan fx F        one effect's block of that table (F 0..13), to run them side by side
 *   build/knobscan probe F K   per memory and step: level, neighbour distance | noise (F -1 for a loop knob) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/nb_engine.h"
#include "../core/nb_panel.h"

#define FS 44100.f
#define STEPS 33
#define NFFT 4096
#define NBANDS 28
#define ENV_HOP 256
#define NLAG 40
#define NCTX 5
#define FLOOR_DB 50.0
#define W_ENV 8.0
#define NOCT 9
#define W_SIDE 0.5

static double pow2(double x) { return exp2(x); }

static void fft(double *re, double *im, int n)
{
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double a = -2 * M_PI / len, wr = cos(a), wi = sin(a);
        for (int i = 0; i < n; i += len) {
            double cr = 1, ci = 0;
            for (int k = 0; k < len / 2; ++k) {
                double ur = re[i + k], ui = im[i + k];
                double vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
                double vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
                re[i + k] = ur + vr; im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
                double t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t;
            }
        }
    }
}

typedef struct { double band[NBANDS]; double level; double ac[NLAG]; double side[NOCT]; } print_t;

/* x the mid (L+R)/2, y the side (L-R)/2 */
static void fingerprint(const float *x, const float *y, int n, print_t *p)
{
    static double re[NFFT], im[NFFT], acc[NFFT / 2], sacc[NFFT / 2];
    memset(acc, 0, sizeof acc);
    memset(sacc, 0, sizeof sacc);
    int frames = 0;
    double pw = 0;
    for (int at = 0; at + NFFT <= n; at += NFFT / 2, ++frames) {
        for (int i = 0; i < NFFT; ++i) {
            double w = 0.5 - 0.5 * cos(2 * M_PI * i / (NFFT - 1));
            re[i] = x[at + i] * w; im[i] = y[at + i] * w;
        }
        fft(re, im, NFFT);                                              /* two real signals in one transform */
        for (int k = 0; k < NFFT / 2; ++k) {
            int m = (NFFT - k) & (NFFT - 1);
            double xr = 0.5 * (re[k] + re[m]), xi = 0.5 * (im[k] - im[m]);
            double yr = 0.5 * (im[k] + im[m]), yi = -0.5 * (re[k] - re[m]);
            acc[k] += xr * xr + xi * xi;
            sacc[k] += yr * yr + yi * yi;
        }
    }
    /* the stereo: side over mid per octave, -30 .. +10 dB */
    for (int o = 0; o < NOCT; ++o) {
        int k0 = (int)(40 * pow2(o) / FS * NFFT), k1 = (int)(40 * pow2(o + 1) / FS * NFFT);
        double em = 1e-12, es = 1e-12;
        for (int k = k0; k < k1 && k < NFFT / 2; ++k) { em += acc[k]; es += sacc[k]; }
        double r = 10 * log10(es / em);
        p->side[o] = r < -30 ? -30 : r > 10 ? 10 : r;
    }
    for (int i = 0; i < n; ++i) pw += (double)x[i] * x[i];
    p->level = 10 * log10(pw / n + 1e-12);
    double top = -1e9;
    for (int b = 0; b < NBANDS; ++b) {
        double f0 = 40 * pow2(b / 3.0), f1 = 40 * pow2((b + 1) / 3.0);
        int k0 = (int)(f0 / FS * NFFT), k1 = (int)(f1 / FS * NFFT);
        if (k1 <= k0) k1 = k0 + 1;
        double e = 1e-12;
        for (int k = k0; k < k1 && k < NFFT / 2; ++k) e += acc[k];
        p->band[b] = 10 * log10(e / (frames ? frames : 1));
        if (p->band[b] > top) top = p->band[b];
    }
    double mean = 0;
    for (int b = 0; b < NBANDS; ++b) { if (p->band[b] < top - FLOOR_DB) p->band[b] = top - FLOOR_DB; mean += p->band[b]; }
    mean /= NBANDS;
    for (int b = 0; b < NBANDS; ++b) p->band[b] -= mean;
    /* the envelope's rhythm: RMS per hop, its autocorrelation */
    static double env[(int)FS * 2 / ENV_HOP + 1];
    int ne = n / ENV_HOP;
    double em = 0, ev = 0;
    for (int i = 0; i < ne; ++i) {
        double s = 0;
        for (int j = 0; j < ENV_HOP; ++j) s += (double)x[i * ENV_HOP + j] * x[i * ENV_HOP + j];
        env[i] = sqrt(s / ENV_HOP);
        em += env[i];
    }
    em /= ne;
    for (int i = 0; i < ne; ++i) { env[i] -= em; ev += env[i] * env[i]; }
    for (int l = 0; l < NLAG; ++l) {
        double s = 0;
        for (int i = 0; i + l + 1 < ne; ++i) s += env[i] * env[i + l + 1];
        p->ac[l] = ev > 1e-9 * ne ? s / ev : 0;
    }
}

static double distance(const print_t *a, const print_t *b)
{
    double d = 0, r = 0, st = 0;
    for (int i = 0; i < NBANDS; ++i) d += fabs(a->band[i] - b->band[i]);
    for (int o = 0; o < NOCT; ++o) st += fabs(a->side[o] - b->side[o]);
    for (int l = 0; l < NLAG; ++l) r += fabs(a->ac[l] - b->ac[l]);
    double lv = a->level - b->level;
    if (lv < -60) lv = -60;
    if (lv > 60) lv = 60;
    return d / NBANDS + 0.5 * fabs(lv) + W_ENV * r / NLAG + W_SIDE * st / NOCT;
}

static void average(const print_t *a, const print_t *b, print_t *o)
{
    for (int i = 0; i < NBANDS; ++i) o->band[i] = 0.5 * (a->band[i] + b->band[i]);
    for (int l = 0; l < NLAG; ++l) o->ac[l] = 0.5 * (a->ac[l] + b->ac[l]);
    for (int k = 0; k < NOCT; ++k) o->side[k] = 0.5 * (a->side[k] + b->side[k]);
    o->level = 0.5 * (a->level + b->level);
}


/* what is stepped: a loop knob (its raw value) or an effect knob (its raw value) */
enum { L_GAIN, L_FOCUS, L_DRIVE, L_CROSS, L_GRITO, L_MID, NLOOP };
static const char *loop_names[NLOOP] = { "GAIN", "FOCUS", "DRIVE", "CROSS", "SCREAM", "MID" };
static const float loop_max[NLOOP] = { 1.5f, 1, 1, 1, 1, 1 };
/* the contexts: a memory, how hot the loop sings at least, where the effect's other knobs sit (-1 their rest),
 * and which of the two measuring MIXes */
static const int ctx_snap[NCTX] = { 0, 1, 2, 4, 6 };
static const float ctx_gain[NCTX] = { 1.06f, 1.2f, 1.06f, 1.35f, 1.06f };
static const float ctx_other[NCTX] = { -1.f, .25f, .75f, -1.f, .5f };
static const int ctx_hot[NCTX] = { 1, 0, 1, 0, 1 };
/* the effect measured: its MIX where half and three quarters of its change are in (set by its MIX's own scan) */
static float meas_mix[2] = { .6f, 1.f };

static void render(int ci, uint32_t seed, int fx, int knob, float value, print_t *out)
{
    static nb_engine e;
    static nb_panel p;
    static float buf[2 * 128], mid[(int)FS * 2], side[(int)FS * 2];
    nb_engine_init(&e, FS);
    e.rng ^= seed;
    nb_panel_init(&p);
    nb_controls c;
    memset(&c, 0, sizeof c);
    c.pot = 1023;
    nb_panel_update(&p, &c);
    p.cur = p.snaps[ctx_snap[ci]];                                    /* the context's memory */
    p.drone = 1;
    int total = (int)(FS * 1.6f), n = 0, keep0 = (int)(FS * 0.5f), kick_every = (int)(FS * 0.35f);
    for (int t = 0; t < total; t += 128) {
        c.ms += 3;
        nb_panel_update(&p, &c);
        nb_params q = p.params;
        for (int f = 0; f < NB_NFX; ++f) q.fx[f][3] = 0.f;            /* only the effect measured is in */
        if (q.gain < ctx_gain[ci]) q.gain = ctx_gain[ci];             /* the loop singing */
        if (fx < 0) {
            switch (knob) {
            case L_GAIN: q.gain = value; break;
            case L_FOCUS: q.focus = value; break;
            case L_DRIVE: q.drive = value; break;
            case L_CROSS: q.cross = value; break;
            case L_MID: q.mid_hz = 20.f * exp2f(value * 9.f); break;
            default: q.grito = value; break;
            }
        } else {
            for (int k = 0; k < 3; ++k) q.fx[fx][k] = ctx_other[ci] >= 0.f ? ctx_other[ci] : p.cur.fx[fx][k];
            q.fx[fx][3] = meas_mix[ctx_hot[ci]];
            q.fx[fx][knob] = value;
        }
        if (t % kick_every < 128) { p.touch.kick_mask = 15; p.touch.kick_color = 0.5f; }
        nb_engine_render(&e, &q, &p.touch, buf, 128);
        for (int i = 0; i < 128; ++i, ++n)
            if (n >= keep0 && n - keep0 < (int)(sizeof mid / sizeof mid[0])) {
                mid[n - keep0] = 0.5f * (buf[2 * i] + buf[2 * i + 1]);
                side[n - keep0] = 0.5f * (buf[2 * i] - buf[2 * i + 1]);
            }
    }
    fingerprint(mid, side, n - keep0, out);
}

/* the change between neighbouring steps of v0 .. v1 in each context, the noise's own share taken off (the chaos
 * of a singing loop: two renders with another noise differ that much without the knob moving). The effects after
 * the network (DELAY, REVERB) do not feed back: the network is the same at every step, nothing to take off. */
static void changes(int fx, int knob, float v0, float v1, double d[NCTX][STEPS])
{
    int post = fx >= NB_NLOOPFX;
    for (int ci = 0; ci < NCTX; ++ci) {
        print_t a, b, prev, cur;
        double fprev = 0;
        d[ci][0] = 0;
        for (int s = 0; s < STEPS; ++s) {
            float v = v0 + (v1 - v0) * (float)s / (STEPS - 1);
            render(ci, 0, fx, knob, v, &a);
            render(ci, 0x5bd1e995u, fx, knob, v, &b);
            average(&a, &b, &cur);
            double fl = post ? 0 : distance(&a, &b) * 0.70710678;      /* the noise in a difference of two means */
            if (s) {
                double raw = distance(&prev, &cur), f = 0.5 * (fl + fprev);
                d[ci][s] = raw > f ? sqrt(raw * raw - f * f) : 0;
            }
            prev = cur; fprev = fl;
        }
    }
}

/* the contexts joined: each memory counts the same (its share of its own change), a context where the knob
 * hardly moves anything (under a sixth of the busiest) left out; returns the total change, all contexts */
static double join(double d[NCTX][STEPS], double *out, int n)
{
    double tot[NCTX], top = 0, all = 0;
    for (int ci = 0; ci < NCTX; ++ci) {
        tot[ci] = 0;
        for (int s = 0; s < n; ++s) tot[ci] += d[ci][s];
        if (tot[ci] > top) top = tot[ci];
        all += tot[ci];
    }
    for (int s = 0; s < n; ++s) out[s] = 0;
    for (int ci = 0; ci < NCTX; ++ci) {
        if (tot[ci] <= 0 || tot[ci] < top / 6) continue;
        for (int s = 0; s < n; ++s) out[s] += d[ci][s] / tot[ci];
    }
    return all;
}

/* where 10/25/50/75/90 % of the change falls on the raw travel, in % */
static void quantiles(const double *cum, int n, int q[5])
{
    static const double qs[5] = { .1, .25, .5, .75, .9 };
    for (int k = 0; k < 5; ++k) {
        int i = 0;
        while (i < n - 1 && cum[i + 1] < qs[k] * cum[n - 1]) ++i;
        double f = 0;
        if (i < n - 1 && cum[i + 1] > cum[i]) f = (qs[k] * cum[n - 1] - cum[i]) / (cum[i + 1] - cum[i]);
        if (f < 0) f = 0;
        if (f > 1) f = 1;
        q[k] = (int)(100.0 * (i + f) / (n - 1) + .5);
    }
}

/* the measured share of a curve: 85 %, less for a knob that hardly changes anything here (a total under 15 over
 * the five contexts), where the measure is mostly noise */
static double share(double total)
{
    double w = (total - 3) / 12;
    return 0.85 * (w < 0 ? 0 : w > 1 ? 1 : w);
}

/* the inverse of the cumulative change on `pts` points (0..1), the rest linear so a quiet stretch keeps its place */
static void inverse(const double *cum, int n, float *curve, int pts, double sh)
{
    for (int k = 0; k < pts; ++k) {
        double target = (double)k / (pts - 1) * cum[n - 1];
        int i = 0;
        while (i < n - 1 && cum[i + 1] < target) ++i;
        double frac = 0;
        if (i < n - 1 && cum[i + 1] > cum[i]) frac = (target - cum[i]) / (cum[i + 1] - cum[i]);
        if (frac > 1) frac = 1;
        double x = ((double)i + frac) / (n - 1);
        if (cum[n - 1] < 1e-9) x = (double)k / (pts - 1);              /* inert here: leave it linear */
        if (k == pts - 1) x = 1;
        curve[k] = (float)(sh * x + (1 - sh) * (double)k / (pts - 1));
    }
}

/* how the change falls on the knob's travel through its curve: the share in the first and last quarter */
static void ends(const double *cum, int n, const float *curve, float max, int *lo, int *hi)
{
    double at[2] = { curve[4] / max, curve[12] / max }, c[2];
    for (int j = 0; j < 2; ++j) {
        double x = at[j] * (n - 1);
        int i = (int)x;
        if (i >= n - 1) { c[j] = cum[n - 1]; continue; }
        c[j] = cum[i] + (cum[i + 1] - cum[i]) * (x - i);
    }
    double t = cum[n - 1] > 0 ? cum[n - 1] : 1;
    *lo = (int)(100 * c[0] / t + .5);
    *hi = (int)(100 * (t - c[1]) / t + .5);
}

typedef struct { float curve[17]; int q[5]; double total; int lo, hi; } result_t;

/* scan one knob: its curve (raw values), where the change fell, the total */
static void scan(int fx, int knob, result_t *r)
{
    static double d[NCTX][STEPS], d2[NCTX][STEPS];
    double cum[2 * STEPS - 1], j1[STEPS], j2[STEPS];
    if (fx == NB_FX_SHIFT && knob == 0) {
        /* SHIFT's HZ is bipolar: each half measured from the centre, the two summed, the curve mirrored so the
         * centre stays at zero */
        changes(fx, knob, .5f, 1.f, d);
        changes(fx, knob, .5f, 0.f, d2);
        double t1 = join(d, j1, STEPS), t2 = join(d2, j2, STEPS);
        cum[0] = 0;
        for (int s = 1; s < STEPS; ++s) cum[s] = cum[s - 1] + j1[s] + j2[s];
        float half[9];
        r->total = t1 + t2;
        inverse(cum, STEPS, half, 9, share(r->total));
        for (int k = 0; k <= 8; ++k) { r->curve[8 + k] = .5f + .5f * half[k]; r->curve[8 - k] = .5f - .5f * half[k]; }
        cum[0] = 0;                                                    /* the whole travel, down then up */
        for (int s = 1; s < STEPS; ++s) cum[s] = cum[s - 1] + j2[STEPS - s];
        for (int s = 1; s < STEPS; ++s) cum[STEPS - 1 + s] = cum[STEPS - 2 + s] + j1[s];
        quantiles(cum, 2 * STEPS - 1, r->q);
        ends(cum, 2 * STEPS - 1, r->curve, 1.f, &r->lo, &r->hi);
        return;
    }
    float max = fx < 0 ? loop_max[knob] : 1.f;
    changes(fx, knob, 0.f, max, d);
    r->total = join(d, j1, STEPS);
    cum[0] = 0;
    for (int s = 1; s < STEPS; ++s) cum[s] = cum[s - 1] + j1[s];
    inverse(cum, STEPS, r->curve, 17, share(r->total));
    for (int k = 0; k <= 16; ++k) r->curve[k] *= max;
    quantiles(cum, STEPS, r->q);
    ends(cum, STEPS, r->curve, max, &r->lo, &r->hi);
}

static void print_curve(const float c[17])
{
    printf("{");
    for (int i = 0; i <= 16; ++i) printf(" %.4ff%s", c[i], i < 16 ? "," : "");
    printf(" }");
}

static void loop_knob(int k)
{
    result_t r;
    scan(-1, k, &r);
    printf("/* %s: raw linear %d/%d/%d/%d/%d %%, total change %.1f */\n", loop_names[k], r.q[0], r.q[1], r.q[2], r.q[3],
           r.q[4], r.total);
    printf("    ");
    print_curve(r.curve);
    printf("\n");
    fprintf(stderr, "%s: raw linear %d/%d/%d/%d/%d %%, total %.1f, first/last quarter of the turn %d/%d %%\n",
            loop_names[k], r.q[0], r.q[1], r.q[2], r.q[3], r.q[4], r.total, r.lo, r.hi);
    fflush(stdout);
}

static void fx_block(int f)
{
    static const char *const kn[NB_NFX][4] = {
        { "GAIN", "TONE", "BIAS", "MIX" },        { "GAIN", "TONE", "EMPHASIS", "MIX" },
        { "DEPTH", "BIAS", "SOFT", "MIX" },        { "DEPTH", "BIAS", "SMOOTH", "MIX" },
        { "HALF", "BIAS", "TONE", "MIX" },         { "OCTAVE", "WIDTH", "TONE", "MIX" },
        { "CARRIER", "FREQ", "DC", "MIX" },        { "SAG", "GATE", "RECOVER", "MIX" },
        { "HZ", "SPREAD", "FEEDBACK", "MIX" },     { "TIME", "FEEDBACK", "WOW", "MIX" },
        { "DEPTH", "ATTACK", "RELEASE", "MIX" },   { "RANGE", "BASE", "RESO", "MIX" },
        { "TIME", "FEEDBACK", "TONE", "MIX" },     { "SIZE", "DAMP", "WIDTH", "MIX" },
    };
    result_t r[4];
    /* the MIX first (its other knobs at rest and around): where it is half in sets how the others are heard */
    meas_mix[0] = .6f; meas_mix[1] = 1.f;
    scan(f, 3, &r[3]);
    meas_mix[0] = r[3].q[2] / 100.f;
    meas_mix[1] = r[3].q[3] / 100.f;
    if (meas_mix[0] < .1f) meas_mix[0] = .1f;
    if (meas_mix[1] < meas_mix[0] + .1f) meas_mix[1] = meas_mix[0] + .1f;
    if (meas_mix[1] > 1.f) meas_mix[1] = 1.f;
    for (int k = 0; k < 3; ++k) scan(f, k, &r[k]);
    for (int k = 0; k < 4; ++k)
        fprintf(stderr, "%s %s: raw linear %d/%d/%d/%d/%d %%, total %.1f, first/last quarter of the turn %d/%d %%%s\n",
                nb_fx_name(f), kn[f][k], r[k].q[0], r[k].q[1], r[k].q[2], r[k].q[3], r[k].q[4], r[k].total,
                r[k].lo, r[k].hi, k == 3 ? "" : "");
    fprintf(stderr, "%s measured at MIX %.2f / %.2f\n", nb_fx_name(f), meas_mix[0], meas_mix[1]);
    printf("    /* %s (MIX %.2f/%.2f):", nb_fx_name(f), meas_mix[0], meas_mix[1]);
    for (int k = 0; k < 4; ++k) printf(" %s %d/%d/%d%s", kn[f][k], r[k].q[1], r[k].q[2], r[k].q[3], k < 3 ? "," : "");
    printf(" */\n    {\n");
    for (int k = 0; k < 4; ++k) { printf("      "); print_curve(r[k].curve); printf("%s\n", k < 3 ? "," : ""); }
    printf("    }%s\n", f < NB_NFX - 1 ? "," : "");
    fflush(stdout);
}

/* a look inside: per context and step, the level, the neighbour distance and the noise's */
static void probe(int fx, int knob)
{
    for (int ci = 0; ci < NCTX; ++ci) {
        print_t a, b, prev, cur;
        double fprev = 0;
        printf("ctx %d:", ci);
        for (int s = 0; s < STEPS; s += 2) {
            float v = (float)s / (STEPS - 1) * (fx < 0 ? loop_max[knob] : 1.f);
            render(ci, 0, fx, knob, v, &a);
            render(ci, 0x5bd1e995u, fx, knob, v, &b);
            average(&a, &b, &cur);
            double fl = distance(&a, &b) * 0.70710678;
            printf(" %.0f", cur.level);
            if (s) printf("(%.1f|%.1f)", distance(&prev, &cur), 0.5 * (fl + fprev));
            prev = cur; fprev = fl;
        }
        printf("\n");
    }
}

int main(int argc, char **argv)
{
    if (argc > 3 && !strcmp(argv[1], "probe")) {
        if (argc > 5) { meas_mix[0] = (float)atof(argv[4]); meas_mix[1] = (float)atof(argv[5]); }
        probe(atoi(argv[2]), atoi(argv[3]));
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "loop")) { loop_knob(atoi(argv[2])); return 0; }
    if (argc > 2 && !strcmp(argv[1], "fx")) { fx_block(atoi(argv[2])); return 0; }
    if (argc > 1 && !strcmp(argv[1], "--fx")) {
        printf("static const float nb_curve_fx[14][4][17] = {\n");
        for (int f = 0; f < NB_NFX; ++f) fx_block(f);
        printf("};\n");
        return 0;
    }
    for (int k = 0; k < NLOOP; ++k) loop_knob(k);
    return 0;
}
