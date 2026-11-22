/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Mario Baril
 * The loop network. Per channel and sample:
 *   in  = return x (GAIN x own line + CROSS x the matrix of the other lines) + strike + the circuit's floor
 *   pre = the preamp (an algebraic knee that DRIVE hardens toward a clip, both antialiased), DC blocked
 *   eq  = flat .. a unity-peak band-pass at the channel's partial (FOCUS)
 *   then the fixed chain, each effect mixed in by its MIX (at 0 it is skipped; the ones with a line run until their
 *   tail has died), each in its STAGE (0 orthodox, 1 heterodox, 2 heretic; a change crossfades over 10 ms):
 *   FUZZ > BLAST > FOLD > WRAP > RECTIFY > DIVIDE > RING > STARVE > SHIFT > ECHO > PUMP > SWEEP > the line
 * SCREAM bends every read by the loop's own output. The output: 1+3 left, 2+4 right, the gate, a coupling
 * capacitor, the post DELAY and REVERB, the master, and a limiter that is never off.
 * The analog care (docs/ESTAGIOS.md 7b, 7c): static curves antialiased by their antiderivative (ADAA1), hard edges by
 * polyBLEP, the loop read by a 4-point Hermite, the band's width kept against the bilinear squeeze, the loop's period
 * corrected for what the chain delays, coefficients ramped sample by sample, gates and the return as RC, the lines
 * rounded with the circuit's floor as dither, each loop a little off the others (tolerance) and wandering slowly,
 * denormals swept to zero.
 * Audio-path rule (the FM-1 runs this from RAM while Flash is written): no tables, no division by a variable
 * integer, no library call; everything is inlined into nb_engine_render. */
#include "nb_engine.h"
#include "nb_dsp.h"

void nb_engine_init(nb_engine *e, float fs)
{
    uint8_t *b = (uint8_t *)e;
    for (unsigned i = 0; i < sizeof *e; ++i) b[i] = 0;
    e->fs = fs;
    e->rng = 0x9e3779b9u;
    nb_fstate *f = &e->f;
    for (int c = 0; c < NB_CH; ++c) {
        f->per[c] = f->perq[c] = 200.f; f->g[c] = 0.1f; f->kq[c] = 1.f;
        f->pre_q1[c] = 1.f;
        for (int s = 0; s < 3; ++s) { f->shc[s][c] = 1.f; f->shs[s][c] = 0.f; }
        for (int j = 0; j < NB_CH; ++j) f->rew[c][j] = 1.f;
    }
    f->wow_c = 1.f; f->rv_modc = 1.f;
    f->fdn_len[0] = 601.f; f->fdn_len[1] = 743.f; f->fdn_len[2] = 919.f; f->fdn_len[3] = 1097.f;
    f->net_amt[0] = 1.f;
}

/* ---- small pieces ---- */

/* a one-pole low-pass coefficient for fc Hz */
NB_INLINE float lp_coef(float fc, float fs) { return 1.f - nb_exp2(-2.f * NB_PI * fc / fs * 1.4427f); }

/* the phase lead of a one-pole DC block (pole r) at w rad/sample, as samples it takes off a loop's period */
NB_INLINE float hp_lead(float w, float r)
{
    float sw = nb_sin(w), cw = nb_sin(w + 0.5f * NB_PI);
    float ph = (0.5f * NB_PI - 0.5f * w) - nb_atan(r * sw / (1.f - r * cw));
    return ph / w;
}

/* ADAA1 of the algebraic knee at u (q = sqrt(1 + u^2)), the last u1 and q1: exact, no division by the step */
NB_INLINE float ad_sig(float u, float q, float u1, float q1) { return (u + u1) / (q + q1); }

/* ADAA1 of the clip at +-1 */
NB_INLINE float ad_clip(float u, float u1)
{
    float a = nb_absf(u), a1 = nb_absf(u1);
    if (a <= 1.f && a1 <= 1.f) return 0.5f * (u + u1);
    if (u >= 1.f && u1 >= 1.f) return 1.f;
    if (u <= -1.f && u1 <= -1.f) return -1.f;
    float d = u - u1;
    if (d > -1e-6f && d < 1e-6f) return nb_clamp(0.5f * (u + u1), -1.f, 1.f);
    float F = a <= 1.f ? 0.5f * u * u : a - 0.5f, F1 = a1 <= 1.f ? 0.5f * u1 * u1 : a1 - 0.5f;
    return (F - F1) / d;
}

/* the triangle fold (period 4, tri(t) = t on [-1, 1]) and the wrap (period 2), their antiderivatives by an exact
 * floor, and ADAA1 over them */
NB_INLINE float tri_f(float t) { float p = t + 1.f; p -= 4.f * nb_floor(p * 0.25f); return p < 2.f ? p - 1.f : 3.f - p; }
NB_INLINE float tri_F(float t) { float p = t + 1.f; p -= 4.f * nb_floor(p * 0.25f); return p < 2.f ? 0.5f * p * p - p : -0.5f * p * p + 3.f * p - 4.f; }
NB_INLINE float saw_f(float t) { float p = t + 1.f; p -= 2.f * nb_floor(p * 0.5f); return p - 1.f; }
NB_INLINE float saw_F(float t) { float p = t + 1.f; p -= 2.f * nb_floor(p * 0.5f); return 0.5f * p * p - p; }
/* ADAA1 over any curve: F the antiderivative at t, z = {t1, F(t1)} kept from the last sample */
#define AD_STEP(fnF, fnf, t, z, eps) do {                                                                         float F_ = fnF, d_ = (t) - (z)[0];                                                                          float y_ = (d_ > (eps) || d_ < -(eps)) ? (F_ - (z)[1]) / d_ : fnf;                                           (z)[0] = (t); (z)[1] = F_; ad_out = y_; } while (0)
/* the rectifier over a threshold: full wave max(|x| - t, 0) signed back, or half wave max(x - t, 0); ADAA1 */
NB_INLINE float rect_F(float x, float t, float h)
{
    float a = nb_absf(x), fu = a > t ? 0.5f * (a - t) * (a - t) : 0.f;
    float full = x < 0.f ? -fu : fu, half = x > t ? 0.5f * (x - t) * (x - t) : 0.f;
    return full + (half - full) * h;
}
NB_INLINE float rect_f(float x, float t, float h)
{
    float a = nb_absf(x), full = a > t ? a - t : 0.f, half = x > t ? x - t : 0.f;
    return full + (half - full) * h;
}

/* the starved stage: a dead zone d, then a ceiling hd; odd; its antiderivative even */
NB_INLINE float starve_f(float x, float d, float hd)
{
    float a = nb_absf(x), z = a <= d ? 0.f : (a - d) / (1.f - d);
    if (z > hd) z = hd;
    return x < 0.f ? -z : z;
}
NB_INLINE float starve_F(float x, float d, float hd)
{
    float a = nb_absf(x), ah = d + hd * (1.f - d);
    if (a <= d) return 0.f;
    if (a <= ah) return (a - d) * (a - d) / (2.f * (1.f - d));
    return hd * hd * (1.f - d) * 0.5f + hd * (a - ah);
}


/* the loop line, read d samples back by a 4-point Hermite (int16, power-of-two length) */
NB_INLINE float read_line(const int16_t *l, uint32_t wp, float d)
{
    if (d < 2.f) d = 2.f;
    if (d > (float)(NB_LINE - 4)) d = (float)(NB_LINE - 4);
    int di = (int)d;
    float f = d - (float)di;
    uint32_t i0 = (wp - (uint32_t)di) & (NB_LINE - 1);
    float xm = (float)l[(i0 + 1u) & (NB_LINE - 1)], x0 = (float)l[i0];
    float x1 = (float)l[(i0 - 1u) & (NB_LINE - 1)], x2 = (float)l[(i0 - 2u) & (NB_LINE - 1)];
    float c1 = 0.5f * (x1 - xm), c2 = xm - 2.5f * x0 + 2.f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm) + 1.5f * (x0 - x1);
    return (((c3 * f + c2) * f + c1) * f + x0) * (1.f / 32767.f);
}

/* an int16 line of any length, read d samples back with a linear tap; wraps by comparison, no division (the 4-point
 * Hermite is kept for the loop's own read, where its pitch and brightness are heard; here it bought nothing measured) */
NB_INLINE uint32_t back(uint32_t wp, uint32_t len, uint32_t k) { return wp >= k ? wp - k : wp + len - k; }
NB_INLINE float rd16(const int16_t *l, uint32_t wp, uint32_t len, float d)
{
    if (d < 1.f) d = 1.f;
    if (d > (float)(len - 2)) d = (float)(len - 2);
    uint32_t di = (uint32_t)(int)d;
    float f = d - (float)di;
    uint32_t i0 = back(wp, len, di), i1 = i0 ? i0 - 1u : len - 1u;
    return ((float)l[i0] + ((float)l[i1] - (float)l[i0]) * f) * (1.f / 32767.f);
}
/* into a line: rounded (not truncated toward zero: no dead band around silence); the circuit's own floor, injected
 * in every loop, is the dither */
NB_INLINE int16_t to16(float x, uint32_t *rng)
{
    (void)rng;
    float v = nb_clamp(x, -1.f, 1.f) * 32766.f;
    return (int16_t)(v >= 0.f ? v + 0.5f : v - 0.5f);
}

/* one second-order allpass of SHIFT's Hilbert pair: y = a2 (x + y[n-2]) - x[n-2]; s = {x1, x2, y1, y2} */
NB_INLINE float hb_ap(float x, float a2, float *s)
{
    float y = a2 * (x + s[3]) - s[1];
    s[1] = s[0]; s[0] = x;
    s[3] = s[2]; s[2] = y;
    return y;
}
/* the Hilbert pair: the analytic signal (a, b) of x */
NB_INLINE void hilbert(float x, float *h, float *hz, float *a, float *b)
{
    float p = x;
    p = hb_ap(p, 0.6923878f * 0.6923878f, &h[0]);
    p = hb_ap(p, 0.9360654f * 0.9360654f, &h[4]);
    p = hb_ap(p, 0.9882295f * 0.9882295f, &h[8]);
    p = hb_ap(p, 0.9987488f * 0.9987488f, &h[12]);
    float q = *hz;
    *hz = x;
    q = hb_ap(q, 0.4021921f * 0.4021921f, &h[16]);
    q = hb_ap(q, 0.8561711f * 0.8561711f, &h[20]);
    q = hb_ap(q, 0.9722910f * 0.9722910f, &h[24]);
    q = hb_ap(q, 0.9952885f * 0.9952885f, &h[28]);
    *a = p; *b = q;
}

/* a coupling capacitor (one-pole high-pass, ~10 Hz): z = {x1, y1} */
NB_INLINE float couple(float x, float *z)
{
    float y = x - z[0] + 0.99858f * z[1];
    z[0] = x; z[1] = y;
    return y;
}

/* the latency each effect's stage adds to the loop (samples at full MIX): ADAA half a sample a curve, polyBLEP one.
 * ADAA stays only where it is nearly free on the FM-1 (FUZZ); BLAST, FOLD, WRAP, RECTIFY and STARVE went back to their
 * plain curves: their ADAA bought 8..13 dB for 40..70 ns a frame each (docs/ESTAGIOS.md 7b, the cost table) */
NB_INLINE float fx_latency(int f, int st)
{
    switch (f) {
    case NB_FX_FUZZ: return st == 1 ? 0.5f : 1.f;
    case NB_FX_DIVIDE: return st == 2 ? 0.f : 1.f;
    default: return 0.f;
    }
}

/* the simplest fraction n/m (m 1..4) nearest r > 0: the heterodox stages' tuned ratios */
NB_INLINE float snap_ratio(float r)
{
    float best = r, err = 1e9f;
    for (int m = 1; m <= 4; ++m) {
        float n = nb_floor(r * (float)m + 0.5f);
        if (n < 0.f) n = 0.f;
        float cand = n / (float)m, d = nb_absf(cand - r);
        if (d < err) { err = d; best = cand; }
    }
    return best;
}

/* ---- what the knobs mean now, per effect and stage: once per sub-block (exp2, tan), so the sample loop only
 * multiplies. Knob k is s[k], raw 0..1; the orthodox reads the measured curves (the panel), the others are linear
 * until measured ---- */
NB_INLINE void fx_coef(nb_engine *e, int f, int st)
{
    const float fs = e->fs;
    nb_fstate *zs = &e->f;
    float *s = zs->fx_s[f];
    float *q = zs->coef[f][st];
    switch (f * 3 + st) {
    /* FUZZ: gain up to +54 dB, the tone after it, the bias (asymmetry) */
    case NB_FX_FUZZ * 3 + 0:
        q[0] = 2.f * nb_exp2(s[0] * 9.f);
        q[1] = lp_coef(600.f * nb_exp2(s[1] * 4.5f), fs);
        q[2] = s[2] * 0.8f;
        q[3] = nb_sig(q[2]);
        break;
    /* the capacitor that breathes: the gain; the tone in harmonics of the loop (x1..x16); the coupling capacitor's
     * time, 1 .. 200 ms */
    case NB_FX_FUZZ * 3 + 1:
        q[0] = 2.f * nb_exp2(s[0] * 9.f);
        q[1] = 1.f + 15.f * s[1];
        q[2] = lp_coef(1.f / (0.001f + 0.199f * s[2]) / (2.f * NB_PI), fs);
        break;
    /* SCHMITT: the window (0..100 % of the envelope), the tone, the window's skew */
    case NB_FX_FUZZ * 3 + 2:
        q[0] = s[0];
        q[1] = lp_coef(600.f * nb_exp2(s[1] * 4.5f), fs);
        q[2] = (s[2] - 0.5f) * 1.8f;
        break;
    /* BLAST: gain up to +60 dB, the tone after, the high lift before */
    case NB_FX_BLAST * 3 + 0:
        q[0] = 2.f * nb_exp2(s[0] * 10.f);
        q[1] = lp_coef(500.f * nb_exp2(s[1] * 4.7f), fs);
        q[2] = s[2] * 0.92f;
        break;
    /* the slew rate: the gain; the most the output moves a sample (0.002 .. 2 V/us on 9 V rails); the rails' skew */
    case NB_FX_BLAST * 3 + 1:
        q[0] = 2.f * nb_exp2(s[0] * 10.f);
        q[1] = 0.005f * nb_exp2(s[1] * 10.f);
        q[2] = (s[2] - 0.5f) * 1.f;
        break;
    /* the phase reversal: the gain; where past the common-mode range it flips (100 .. 30 % of the rail); how long it
     * sticks, 0 .. 50 ms */
    case NB_FX_BLAST * 3 + 2:
        q[0] = 2.f * nb_exp2(s[0] * 10.f);
        q[1] = 1.f - 0.7f * s[1];
        q[2] = s[2] * 0.05f * fs;
        q[3] = lp_coef(12000.f, fs);
        break;
    /* FOLD: depth +0..+24 dB into the fold, a bias, a softening after */
    case NB_FX_FOLD * 3 + 0:
        q[0] = 1.f + 15.f * s[0];
        q[1] = (s[1] - 0.5f) * 1.2f;
        q[2] = 1.f - 0.92f * s[2];
        break;
    /* the tuned folder: the folds (1..12), the bias, how much the folds stick to whole numbers */
    case NB_FX_FOLD * 3 + 1: {
        float n = 1.f + 11.f * s[0], r = nb_floor(n + 0.5f);
        q[0] = n + (r - n) * s[2];
        q[1] = (s[1] - 0.5f) * 1.2f;
        break;
    }
    /* folding time: the span (1 .. 40 ms of the loop's own line), the rate (0.25 .. 8 loop periods a turn), the
     * skew (more forward .. more backward) */
    case NB_FX_FOLD * 3 + 2:
        q[0] = (0.001f + 0.039f * s[0]) * fs;
        q[1] = 0.25f * nb_exp2(s[1] * 5.f);
        q[2] = 0.05f + 0.9f * s[2];
        break;
    /* WRAP: depth, bias, smoothing */
    case NB_FX_WRAP * 3 + 0:
        q[0] = 1.f + 7.f * s[0];
        q[1] = (s[1] - 0.5f) * 1.2f;
        q[2] = 1.f - 0.92f * s[2];
        break;
    /* the overflow's spark: depth; the strike on the next loop (-60 .. -6 dB); its colour (200 Hz .. 8 kHz) */
    case NB_FX_WRAP * 3 + 1:
        q[0] = 1.f + 7.f * s[0];
        q[1] = nb_exp2((-60.f + 54.f * s[1]) * 0.1661f);
        q[2] = lp_coef(200.f * nb_exp2(s[2] * 5.32f), fs);
        break;
    /* BITS: how many high bits under the sign (1..15), the source (the neighbour .. its own line a period back),
     * the tone */
    case NB_FX_WRAP * 3 + 2:
        q[0] = 1.f + nb_floor(14.f * s[0] + 0.5f);
        q[1] = s[1];
        q[2] = lp_coef(200.f * nb_exp2(s[2] * 6.f), fs);
        break;
    /* RECTIFY: full .. half wave, a threshold, the tone */
    case NB_FX_RECTIFY * 3 + 0:
        q[0] = s[0];
        q[1] = s[1] * 0.6f;
        q[2] = lp_coef(300.f * nb_exp2(s[2] * 5.5f), fs);
        break;
    /* cascaded octaves: how many (0..3), the threshold, the tone */
    case NB_FX_RECTIFY * 3 + 1:
        q[0] = 3.f * s[0];
        q[1] = s[1] * 0.6f;
        q[2] = lp_coef(300.f * nb_exp2(s[2] * 5.5f), fs);
        break;
    /* the rectifier that counts: one half-cycle in N (2..16) flips, which one, the tone */
    case NB_FX_RECTIFY * 3 + 2: {
        float n = 2.f + nb_floor(14.f * s[0] + 0.5f);
        q[0] = n;
        q[1] = nb_floor(s[1] * (n - 1.f) + 0.5f);
        q[2] = lp_coef(300.f * nb_exp2(s[2] * 5.5f), fs);
        break;
    }
    /* DIVIDE: the tone of the squares */
    case NB_FX_DIVIDE * 3 + 0:
        q[2] = lp_coef(200.f * nb_exp2(s[2] * 6.f), fs);
        break;
    /* subharmonics: two divisors (2..16), the tone */
    case NB_FX_DIVIDE * 3 + 1:
        q[0] = 2.f + nb_floor(14.f * s[0] + 0.5f);
        q[1] = 2.f + nb_floor(14.f * s[1] + 0.5f);
        q[2] = lp_coef(200.f * nb_exp2(s[2] * 6.f), fs);
        break;
    /* EUCLID: steps (2..32), hits (1..steps), crossings per step (1 .. 512) */
    case NB_FX_DIVIDE * 3 + 2: {
        float n = 2.f + nb_floor(30.f * s[0] + 0.5f);
        q[0] = n;
        q[1] = 1.f + nb_floor(s[1] * (n - 1.f) + 0.5f);
        q[2] = nb_floor(nb_exp2(s[2] * 9.f) + 0.5f);
        q[3] = lp_coef(320.f, fs);                   /* the gate's RC, ~0.5 ms */
        break;
    }
    /* RING: the own carrier (20 Hz .. 2.5 kHz), DC (ring .. AM), the diodes' shape (sine .. rounded square) */
    case NB_FX_RING * 3 + 0:
        q[0] = 2.f * NB_PI * 20.f * nb_exp2(s[0] * 7.f) / fs;
        break;
    /* the network as carrier: the neighbour .. a sine tuned to the loop at a simple ratio (x0.25 .. x8), DC */
    case NB_FX_RING * 3 + 1:
        q[1] = snap_ratio(0.25f * nb_exp2(s[1] * 5.f));
        break;
    /* the product of all: how many loops (2..4), how much its level is held to the loop's, the tone */
    case NB_FX_RING * 3 + 2:
        q[0] = 2.f + nb_floor(2.f * s[0] + 0.5f);
        q[2] = lp_coef(300.f * nb_exp2(s[2] * 5.5f), fs);
        break;
    /* STARVE: the sag, the dead zone, how fast the supply recovers (5 .. 300 ms) */
    case NB_FX_STARVE * 3 + 0:
        q[0] = 0.85f * s[0];
        q[1] = 0.3f * s[1] * s[1];
        q[2] = lp_coef(1.f / (0.005f + 0.295f * s[2]) / (2.f * NB_PI), fs);
        break;
    /* one supply for the four: the sag, how much of it is shared, the recovery */
    case NB_FX_STARVE * 3 + 1:
        q[0] = 0.85f * s[0];
        q[1] = s[1];
        q[2] = lp_coef(1.f / (0.005f + 0.295f * s[2]) / (2.f * NB_PI), fs);
        break;
    /* the motorboat: its depth, the supply's impedance (0.5 .. 30 Hz under load), the recovery */
    case NB_FX_STARVE * 3 + 2:
        q[0] = s[0];
        q[1] = 0.5f * nb_exp2(s[1] * 5.9f);
        q[2] = lp_coef(1.f / (0.005f + 0.295f * s[2]) / (2.f * NB_PI), fs);
        break;
    /* SHIFT: +-400 Hz, finer near zero; each loop apart by SPREAD; its own feedback. Heterodox: the shift a
     * fraction of each loop's own pitch (sticking to simple ones). Heretic: the mirror around an axis (50 .. 2000 Hz) */
    case NB_FX_SHIFT * 3 + 0: case NB_FX_SHIFT * 3 + 1: case NB_FX_SHIFT * 3 + 2: {
        float v = (s[0] - 0.5f) * 2.f, sp = s[1];
        for (int c = 0; c < NB_CH; ++c) {
            float off = c == 0 ? 0.f : c == 1 ? 0.37f : c == 2 ? -0.29f : 0.61f, hz;
            if (st == 0) hz = 400.f * v * nb_absf(v) + sp * 40.f * off * (1.f + nb_absf(v) * 3.f);
            else if (st == 1) {
                float a = nb_absf(v), r = a + (snap_ratio(a) - a) * 0.7f;
                hz = (v < 0.f ? -r : r) * fs / zs->per[c] * (1.f + sp * 0.1f * off);
            } else hz = 2.f * 50.f * nb_exp2(s[0] * 5.32f) + sp * 40.f * off;
            float w = 2.f * NB_PI * hz / fs, w2 = w * w;
            zs->shdc[st][c] = 1.f - w2 * 0.5f + w2 * w2 * (1.f / 24.f);
            zs->shds[st][c] = w - w * w2 * (1.f / 6.f);
        }
        q[2] = 0.85f * s[2];
        break;
    }
    /* ECHO: 2 .. 27 ms, its feedback, the wow; the clock glides ~50 ms */
    case NB_FX_ECHO * 3 + 0:
        q[0] = (0.002f + 0.025f * s[0]) * fs;
        q[1] = 0.9f * s[1];
        q[2] = 0.02f * s[2];
        q[3] = 1.f - nb_exp2(-1.f / (0.05f * fs) * 1.4427f);
        break;
    /* the tuned echo: its time in loop periods (0.25 .. 8, sticking to simple ratios), feedback +-, the damping */
    case NB_FX_ECHO * 3 + 1: {
        float r = 0.25f * nb_exp2(s[0] * 5.f);
        q[0] = r + (snap_ratio(r) - r) * 0.7f;
        q[1] = (s[1] - 0.5f) * 1.8f;
        q[2] = lp_coef(500.f * nb_exp2(s[2] * 4.6f), fs);
        q[3] = 1.f - nb_exp2(-1.f / (0.05f * fs) * 1.4427f);
        break;
    }
    /* the echo backwards: the window (2 .. 17 ms), feedback, forward .. backward */
    case NB_FX_ECHO * 3 + 2:
        q[0] = (0.002f + 0.015f * s[0]) * fs;
        q[1] = 0.9f * s[1];
        q[2] = s[2];
        q[3] = 1.f / q[0];
        break;
    /* PUMP: depth (the ratio), attack 0.2 .. 20 ms, release 20 .. 800 ms */
    case NB_FX_PUMP * 3 + 0:
        q[0] = 30.f * s[0];
        q[1] = lp_coef(1.f / (0.0002f + 0.02f * s[1]) / (2.f * NB_PI), fs);
        q[2] = lp_coef(1.f / (0.02f + 0.78f * s[2]) / (2.f * NB_PI), fs);
        break;
    /* the chase: depth, the source (its own .. the loop before .. the others), release; a 2 ms attack */
    case NB_FX_PUMP * 3 + 1:
        q[0] = 30.f * s[0];
        q[1] = s[1];
        q[2] = lp_coef(1.f / (0.02f + 0.78f * s[2]) / (2.f * NB_PI), fs);
        q[3] = lp_coef(1.f / 0.002f / (2.f * NB_PI), fs);
        break;
    /* the negative ratio: 1:1 .. -1:4 above -16 dBFS, attack, release */
    case NB_FX_PUMP * 3 + 2:
        q[0] = -5.f * s[0];
        q[1] = lp_coef(1.f / (0.0002f + 0.02f * s[1]) / (2.f * NB_PI), fs);
        q[2] = lp_coef(1.f / (0.02f + 0.78f * s[2]) / (2.f * NB_PI), fs);
        break;
    /* SWEEP: how far the level moves it, its resting cutoff (80 Hz .. 2 kHz), resonance */
    case NB_FX_SWEEP * 3 + 0:
        q[0] = 1.f + 11.f * s[0];
        q[1] = 80.f * nb_exp2(s[1] * 4.6f);
        q[2] = 1.f - 0.93f * s[2];
        break;
    /* the tuned formant: its range in harmonics (0..16), the harmonic it rests on (1..16), resonance */
    case NB_FX_SWEEP * 3 + 1:
        q[0] = 16.f * s[0];
        q[1] = 1.f + 15.f * s[1];
        q[2] = 1.f - 0.93f * s[2];
        break;
    /* the follower at audio rate: octaves per unit of signal (0..4), the resting cutoff, resonance */
    case NB_FX_SWEEP * 3 + 2:
        q[0] = 4.f * s[0];
        q[1] = 80.f * nb_exp2(s[1] * 4.6f);
        q[2] = 1.f - 0.93f * s[2];
        break;
    /* DELAY: 10 .. 120 ms, feedback, the tone of its repeats */
    case NB_FX_DELAY * 3 + 0: case NB_FX_DELAY * 3 + 2:
        q[0] = (0.010f + 0.110f * s[0]) * fs;
        q[1] = st == 0 ? 0.9f * s[1] : 0.4f;
        q[2] = lp_coef(800.f * nb_exp2((1.f - s[2]) * 4.f), fs);
        q[3] = st == 2 ? nb_exp2((-60.f + 54.f * s[1]) * 0.1661f) : 0.f;   /* the heretic: back into the loops */
        break;
    /* the tuned delay: whole periods of loop 1 (1..64, as many as fit), feedback, tone */
    case NB_FX_DELAY * 3 + 1: {
        float per = zs->per[0] > 2.f ? zs->per[0] : 2.f, n = 1.f + nb_floor(63.f * s[0] + 0.5f);
        float most = nb_floor((float)(NB_PDLY - 100) / per);
        if (n > most) n = most;
        if (n < 1.f) n = 1.f;
        q[0] = per * n;
        q[1] = 0.9f * s[1];
        q[2] = lp_coef(800.f * nb_exp2((1.f - s[2]) * 4.f), fs);
        q[3] = 0.f;
        break;
    }
    /* REVERB: RT60 0.2 .. 4 s, the damping, the width. Heterodox: the lines pulled to multiples of the loops'
     * periods (TUNE). Heretic: the room that swallows (SWALLOW). Both: decay, the knob, damping */
    case NB_FX_REVERB * 3 + 0: case NB_FX_REVERB * 3 + 1: case NB_FX_REVERB * 3 + 2: {
        float rt = 0.2f * nb_exp2(s[0] * 4.32f);
        for (int k = 0; k < 4; ++k) {
            float base = k == 0 ? 601.f : k == 1 ? 743.f : k == 2 ? 919.f : 1097.f, tgt = base;
            if (st == 1) {
                float pk = zs->per[k] > 8.f ? zs->per[k] : 8.f, m = nb_floor(base / pk + 0.5f);
                if (m < 1.f) m = 1.f;
                tgt = base + (m * pk - base) * s[1];
                tgt = nb_clamp(tgt, 100.f, (float)(NB_FDN - 12));
            }
            zs->fdn_len[k] += (tgt - zs->fdn_len[k]) * 0.004f;     /* the lines stretch slowly (~90 ms) */
            zs->fdn_g[k] = nb_exp2(-9.966f * zs->fdn_len[k] / (fs * rt));
        }
        q[1] = 0.85f - 0.7f * (st == 0 ? s[1] : s[2]);
        q[2] = st == 0 ? s[2] : 0.6f;
        q[3] = st == 2 ? s[1] : 0.f;
        break;
    }
    }
}

/* an effect coming back in starts from rest: its states (every stage, every loop) zeroed; the long lines are already
 * silent (an effect is skipped only once its tail has died). Through volatile: a plain loop would become a memset call */
NB_INLINE void fx_clean(nb_engine *e, int f)
{
    if (f < NB_NLOOPFX) {
        volatile float *zf = &e->f.z[f][0][0][0];
        for (int k = 0; k < 3 * NB_CH * NB_ZS; ++k) zf[k] = 0.f;
        volatile int *zi = &e->zi[f][0][0][0];
        for (int k = 0; k < 3 * NB_CH * 4; ++k) zi[k] = 0;
        if (f == NB_FX_SHIFT) {
            volatile float *h = &e->f.hb[0][0][0];
            for (int k = 0; k < 3 * NB_CH * NB_HB; ++k) h[k] = 0.f;
        }
    }
}

/* one smoothed value, once per sub-block: a target that jumped (more than J in one control step: a list's choice, a
 * memory, a modulation's source, MOD CLEAR) glides at the slow rate until it arrives; a knob turned moves in small
 * steps and keeps the fast one */
NB_INLINE void glide(float *cur, float tgt, float *prev, uint8_t *slow, float J, float a_fast, float a_slow)
{
    float d = tgt - *prev;
    if (d > J || d < -J) *slow = 1;
    *prev = tgt;
    *cur += (tgt - *cur) * (*slow ? a_slow : a_fast);
    if (*slow) { float r = tgt - *cur; if (r < J * 0.01f && r > -J * 0.01f) *slow = 0; }
}
/* a one-pole factor per sub-block for a time constant tau (s); 0 = the fast one */
NB_INLINE float block_a(float tau, float fs, float fast)
{
    return tau < 0.015f ? fast : nb_rc((float)NB_BLOCK / (tau * fs));
}

/* ---- once per sub-block: every knob glides, the stages, the loops' tuning and band ---- */
NB_INLINE void coefficients(nb_engine *e, const nb_params *p)
{
    nb_fstate *z = &e->f;
    const float fs = e->fs, nyq = 0.4f * fs;
    /* every knob glides to where it was turned, about 15 ms, as a pot through an RC: a turn is a slide, never a
     * staircase, whatever the encoder's step (one factor per sub-block of NB_BLOCK samples) */
    const float a = 1.f - nb_exp2(-(float)NB_BLOCK / (0.015f * fs) * 1.4427f);
    /* TRANSITION: the pace of every discrete choice (docs/ESTAGIOS.md 7d) */
    const float T = nb_clamp(p->transition, 0.f, 2.f);
    /* time constants so that 90 % of the change is reached (2.3 of them) at: PARTIALS 1.0 T, TOPOLOGY 0.6 T, an effect
     * in or out 0.16 T, a memory or a modulation's source 0.6 T; a stage's crossfade is linear over 0.4 T */
    const float a_part = block_a(0.435f * T, fs, a), a_topo = block_a(0.26f * T, fs, a), a_onoff = block_a(0.07f * T, fs, a);
    const float a_jump = block_a(0.26f * T, fs, a);
    const float a_switch = block_a(0.07f * T > 0.035f ? 0.07f * T : 0.035f, fs, a);   /* fx_on: ~80 ms at least */
    {
        float st = 0.4f * T;
        e->xf_step = 1.f / ((st > 0.01f ? st : 0.01f) * fs);
        float sm = nb_rc(1.f / (0.010f * fs)), smj = T * 0.26f > 0.010f ? nb_rc(1.f / (0.26f * T * fs)) : sm;
        const float tg[5] = { p->gain, p->cross, p->drive, p->grito, p->level };
        for (int k = 0; k < 5; ++k) {   /* GAIN, CROSS, DRIVE, SCREAM, LEVEL: per sample, slow only after a jump */
            float d = tg[k] - e->pt_misc[3 + (k < 5 ? k : 0)];
            if (k < 5) {
                if (d > 0.15f || d < -0.15f) e->sl_misc[3 + k] = 1;
                e->pt_misc[3 + k] = tg[k];
            }
        }
        float *cs[5] = { &z->gain_s, &z->cross_s, &z->drive_s, &z->grito_s, &z->level_s };
        for (int k = 0; k < 5; ++k) {
            if (e->sl_misc[3 + k]) { float r = tg[k] - *cs[k]; if (r < 0.0015f && r > -0.0015f) e->sl_misc[3 + k] = 0; }
            e->rt_misc[k] = e->sl_misc[3 + k] ? smj : sm;
        }
    }
    for (int f = 0; f < NB_NFX; ++f) {
        for (int k = 0; k < 3; ++k) glide(&z->fx_s[f][k], p->fx[f][k], &e->pt_fx[f][k], &e->sl_fx[f][k], 0.1f, a, a_jump);
        {   /* the MIX: an effect switched in or out fades over ~80 ms (at 0.5; never faster than ~80 ms when it is its
             * in/out switch); a jump elsewhere at the jumps' pace (sl 2: in or out, 1: a jump). Out and faded, it is
             * skipped (bit-exact: the loop's signal untouched, nothing computed); in again, its state starts clean */
            uint8_t on = p->fx_on[f] != 0;
            float was = e->pt_fx[f][3], now = on ? p->fx[f][3] : 0.f, d = now - was;
            uint8_t *sl = &e->sl_fx[f][3];
            if (on != e->on_prev[f]) {
                *sl = 3;
                if (on && z->fx_s[f][3] == 0.f && z->xf[f] == 0.f) fx_clean(e, f);
                e->on_prev[f] = on;
            } else if ((was <= 1e-4f) != (now <= 1e-4f)) { if (*sl != 3) *sl = 2; }
            else if (d > 0.1f || d < -0.1f) { if (*sl < 2) *sl = 1; }
            e->pt_fx[f][3] = now;
            z->fx_s[f][3] += (now - z->fx_s[f][3]) * (*sl == 3 ? a_switch : *sl == 2 ? a_onoff : *sl ? a_jump : a);
            if (now == 0.f && z->fx_s[f][3] < 1e-4f) z->fx_s[f][3] = 0.f;      /* out: exactly out */
            if (*sl) { float r = now - z->fx_s[f][3]; if (r < 1e-3f && r > -1e-3f) *sl = 0; }
        }
        z->dmixr[f] = (z->fx_s[f][3] - z->mixr[f]) * (1.f / NB_BLOCK);   /* the MIX ramped sample by sample */
    }
    z->mix_moving = 0.f;                    /* the ramps run only while a MIX moves: at rest they cost nothing */
    for (int f = 0; f < NB_NFX; ++f) if (z->dmixr[f] > 1e-9f || z->dmixr[f] < -1e-9f) z->mix_moving = 1.f;
    if (z->mix_moving == 0.f) for (int f = 0; f < NB_NFX; ++f) z->mixr[f] = z->fx_s[f][3];
    /* the stages: a loop effect that is in crossfades from the old one (10 ms); the post ones morph; the network's
     * amounts glide */
    int job_f[2 * NB_NFX], job_s[2 * NB_NFX], nj = 0;
    for (int f = 0; f < NB_NFX; ++f) {
        uint8_t ns = p->stage[f] < NB_NSTAGE ? p->stage[f] : 0;
        if (ns != e->stg[f]) {
            if (f < NB_NLOOPFX && z->fx_s[f][3] > 1e-4f) { e->stg_old[f] = e->stg[f]; z->xf[f] = 1.f; }
            e->stg[f] = ns;
        }
        /* only what is in, still ringing or fading: an effect at MIX 0 costs nothing */
        int live = z->fx_s[f][3] > 1e-4f || p->fx[f][3] > 1e-4f || z->xf[f] > 0.f
                || (f == NB_FX_ECHO && z->tail[0] > 1e-5f) || (f == NB_FX_DELAY && z->tail[1] > 1e-5f)
                || (f == NB_FX_REVERB && z->tail[2] > 1e-5f);
        if (!live) continue;
        job_f[nj] = f; job_s[nj++] = e->stg[f];
        if (f < NB_NLOOPFX && z->xf[f] > 0.f) { job_f[nj] = f; job_s[nj++] = e->stg_old[f]; }
    }
    z->xf_any = 0.f;
    for (int f = 0; f < NB_NLOOPFX; ++f) if (z->xf[f] > 0.f) z->xf_any = 1.f;
    NB_NOUNROLL
    for (int j = 0; j < nj; ++j) fx_coef(e, job_f[j], job_s[j]);     /* one copy of every effect's coefficients */
    e->stg[NB_STAGE_NET] = p->stage[NB_STAGE_NET] < NB_NSTAGE ? p->stage[NB_STAGE_NET] : 0;
    for (int k = 0; k < 3; ++k) z->net_amt[k] += ((k == e->stg[NB_STAGE_NET] ? 1.f : 0.f) - z->net_amt[k]) * a;
    /* SHIFT's turning phasors kept on the unit circle */
    for (int s = 0; s < 3; ++s)
        for (int c = 0; c < NB_CH; ++c) {
            float m = nb_sqrt(z->shc[s][c] * z->shc[s][c] + z->shs[s][c] * z->shs[s][c]);
            if (m > 0.f) { z->shc[s][c] /= m; z->shs[s][c] /= m; } else { z->shc[s][c] = 1.f; z->shs[s][c] = 0.f; }
        }
    /* what the chain delays: taken off the loops' periods so they sing where MID says */
    float lat = 0.f;
    for (int f = 0; f < NB_NLOOPFX; ++f) lat += z->fx_s[f][3] * fx_latency(f, e->stg[f]);
    z->lat = lat;

    if (z->mid_s <= 0.f) { z->mid_s = p->mid_hz; e->pt_misc[0] = p->mid_hz; }
    {   /* MID, a tuning pot: twice as slow, about 30 ms; a jump (a memory) glides at the jumps' pace, a glissando */
        float r = p->mid_hz / (e->pt_misc[0] > 1.f ? e->pt_misc[0] : 1.f);
        if (r > 1.072f || r < 0.933f) e->sl_misc[0] = 1;       /* more than ~1.2 semitones in one step */
        e->pt_misc[0] = p->mid_hz;
        z->mid_s += (p->mid_hz - z->mid_s) * (e->sl_misc[0] ? a_jump : a * 0.5f);
        if (e->sl_misc[0] && nb_absf(p->mid_hz - z->mid_s) < 0.001f * p->mid_hz) e->sl_misc[0] = 0;
    }
    glide(&z->focus_s, p->focus, &e->pt_misc[1], &e->sl_misc[1], 0.15f, a, a_jump);
    glide(&z->hiss_s, p->hiss, &e->pt_misc[2], &e->sl_misc[2], 0.15f, a, a_jump);
    for (int c = 0; c < NB_CH; ++c) {
        z->ratio_s[c] += (p->ratio[c] - z->ratio_s[c]) * a_part;      /* PARTIALS: only ever a choice: the glissando */
        glide(&z->pitch_s[c], p->ch_pitch[c], &e->pt_ch[c][0], &e->sl_ch[c][0], 1.f, a, a_jump);
        glide(&z->chmid_s[c], p->ch_mid[c], &e->pt_ch[c][1], &e->sl_ch[c][1], 0.25f, a, a_jump);
        glide(&z->chg_s[c], p->ch_gain[c], &e->pt_ch[c][2], &e->sl_ch[c][2], 0.15f, a, a_jump);
        glide(&z->chl_s[c], p->ch_level[c], &e->pt_ch[c][3], &e->sl_ch[c][3], 0.15f, a, a_jump);
        for (int j = 0; j < NB_CH; ++j)   /* TOPOLOGY: the matrix crossfades to the new routing */
            glide(&z->mtx_s[c][j], p->matrix[c][j], &e->pt_mtx[c][j], &e->sl_mtx[c][j], 0.15f, a, a_topo);
    }
    /* the analog circuit: each loop a little off the others (tolerance, fixed), wandering slowly (a few cents over
     * seconds); the heterodox network's heat (each loop warms with its level over ~2 s and goes flat); the heretic
     * network's sends following the level difference (energy from the loud to the quiet), 3 s .. 0.3 s by SCREAM */
    const float a_heat = 1.f - nb_exp2(-(float)NB_BLOCK / (2.f * fs) * 1.4427f);
    const float a_drift = 1.f - nb_exp2(-(float)NB_BLOCK / (3.f * fs) * 1.4427f);
    float lv[NB_CH];
    for (int c = 0; c < NB_CH; ++c) lv[c] = nb_sqrt(z->m_lv[c]);
    for (int c = 0; c < NB_CH; ++c) {
        if (((e->time >> 4) & 1023u) == (uint32_t)(c * 256)) z->drift_t[c] = 2.f * nb_noise(&e->rng);
        z->drift[c] += (z->drift_t[c] - z->drift[c]) * a_drift;
        z->heat[c] += (nb_clamp(lv[c] * 2.f, 0.f, 1.f) - z->heat[c]) * a_heat;
        if (z->net_amt[2] > 1e-3f) {
            float rate = (float)NB_BLOCK / ((3.f - 2.7f * z->grito_s) * fs);
            for (int j = 0; j < NB_CH; ++j) {
                float t = nb_clamp(1.f + 4.f * (lv[j] - lv[c]) / (lv[j] + lv[c] + 1e-3f), 0.f, 2.f);
                z->rew[c][j] += (t - z->rew[c][j]) * rate;
            }
        }
    }
    NB_NOUNROLL
    for (int c = 0; c < NB_CH; ++c) {
        float tol = c == 0 ? 1.8f : c == 1 ? -2.3f : c == 2 ? 0.9f : -1.2f;          /* cents */
        float qtol = c == 0 ? 1.03f : c == 1 ? 0.97f : c == 2 ? 1.02f : 0.985f;
        float heat = z->heat[c] * z->net_amt[1];
        float cents = tol + z->drift[c] - 30.f * heat * (0.3f + 0.7f * z->drive_s);
        float f = z->mid_s * z->ratio_s[c] * nb_exp2(z->pitch_s[c] * (1.f / 12.f) + cents * (1.f / 1200.f));
        if (f < 20.f) f = 20.f;
        float fb = f * nb_exp2(z->chmid_s[c] - 0.15f * heat);
        {   /* nothing that sets this loop's band or period moved (a few thousandths of a cent): keep them */
            float *k0 = z->tkey[c];
            if (nb_absf(f - k0[0]) < 2e-5f * f && nb_absf(fb - k0[1]) < 2e-5f * fb && nb_absf(z->focus_s - k0[2]) < 1e-5f
                && nb_absf(lat - k0[3]) < 1e-3f) {
                z->dg[c] = 0.f; z->dk[c] = 0.f;
                z->per[c] += (z->perq[c] - z->per[c]) * 0.25f;
                continue;
            }
            k0[0] = f; k0[1] = fb; k0[2] = z->focus_s; k0[3] = lat;
        }
        if (fb > nyq) fb = nyq;
        if (fb < 20.f) fb = 20.f;
        float g = nb_tan(NB_PI * fb / fs);
        /* the band's width as the analog band has it (fc / Q): the edges prewarped, so the bilinear squeeze near
         * Nyquist does not narrow it */
        float fo = z->focus_s;
        float Q = 0.6f * nb_exp2(fo * 5.6f) * qtol, hb = 0.5f / Q, rr = nb_sqrt(1.f + hb * hb);
        float f1 = fb * (rr - hb), f2 = fb * (rr + hb);
        if (f2 > 0.41f * fs) f2 = 0.41f * fs;
        float k = (nb_tan(NB_PI * f2 / fs) - nb_tan(NB_PI * f1 / fs)) / g;
        if (k < 0.02f) k = 0.02f;
        z->dg[c] = (g - z->g[c]) * (1.f / NB_BLOCK);
        z->dk[c] = (k - z->kq[c]) * (1.f / NB_BLOCK);
        /* the loop tuned to the partial: its period, less what the chain delays, plus what the DC block's phase
         * lead takes; raised an octave until it fits the line */
        float w = 2.f * NB_PI * f / fs;
        float per = fs / f;
        while (per > (float)(NB_LINE - 8)) { per *= 0.5f; w *= 2.f; }
        per += hp_lead(w, 0.9977f) - lat;
        if (per < 2.f) per = 2.f;
        z->perq[c] = per;
        z->per[c] += (per - z->per[c]) * 0.25f;        /* a glide over a few sub-blocks */
    }
    float mixb = nb_clamp(z->focus_s * 4.f, 0.f, 1.f);
    z->dmixb = (mixb - z->mixb) * (1.f / NB_BLOCK);
    z->band_moving = z->dmixb != 0.f ? 1.f : 0.f;      /* the band's ramps run only while it moves */
    for (int c = 0; c < NB_CH; ++c) if (z->dg[c] != 0.f || z->dk[c] != 0.f) z->band_moving = 1.f;
}

/* ---- one effect, one stage, one loop, one sample: the signal it hands on (MIX blends it with x) ----
 * wr: this stage may write what it shares with the others (lines, sparks); the stage fading out may not */
NB_INLINE float fx_run(nb_engine *e, int f, int st, int c, float x, const float *fbk, int wr)
{
    nb_fstate *zs = &e->f;
    const float *q = zs->coef[f][st];
    const float *s = zs->fx_s[f];
    float *z = zs->z[f][st][c];
    int *zi = e->zi[f][st][c];
    switch (f * 3 + st) {
    case NB_FX_FUZZ * 3 + 0: {  /* two stages, the second asymmetric like mismatched diodes; both antialiased */
        float b = q[2];
        float v = x * q[0] * 0.5f + b, qv = nb_hyp(v);
        float a1 = ad_sig(v, qv, z[0], z[1]) - q[3];
        z[0] = v; z[1] = qv;
        float u = a1 * 2.f, A = 1.f - 0.6f * b, k = 1.f + b;
        float qu = nb_hyp(u), qk = nb_hyp(k * u), u1 = z[2], y;
        if (u >= 0.f && u1 >= 0.f) y = ad_sig(u, qu, u1, z[3]);
        else if (u < 0.f && u1 < 0.f) y = A * k * (u + u1) / (qk + z[4]);
        else {
            float G = u >= 0.f ? u * u / (qu + 1.f) : A * k * u * u / (qk + 1.f);
            float G1 = u1 >= 0.f ? u1 * u1 / (z[3] + 1.f) : A * k * u1 * u1 / (z[4] + 1.f);
            y = (G - G1) / (u - u1);
        }
        z[2] = u; z[3] = qu; z[4] = qk;
        z[5] += (y - z[5]) * q[1];
        float dcv = z[5] - z[6] + 0.995f * z[7];          /* its own DC block, as before */
        z[6] = z[5]; z[7] = dcv;
        return dcv * 0.95f;
    }
    case NB_FX_FUZZ * 3 + 1: {  /* the coupling capacitor charges with the level and pushes the bias: loud, it chokes */
        z[2] += (nb_absf(z[3]) - z[2]) * q[2];
        float b = 0.15f + 1.6f * z[2];
        float v = x * q[0] * 0.5f + b, qv = nb_hyp(v);
        float y = (ad_sig(v, qv, z[0], z[1]) - nb_sig(b)) * 2.f;
        z[0] = v; z[1] = qv;
        float w = 2.f * NB_PI * q[1] / zs->per[c];          /* the tone, in harmonics of this loop */
        z[3] += (nb_clamp(y, -1.5f, 1.5f) - z[3]) * (w / (1.f + w));
        return couple(z[3], &z[4]) * 0.9f;
    }
    case NB_FX_FUZZ * 3 + 2: {  /* SCHMITT: infinite gain, a comparator with a window; the edges polyBLEP'd */
        float ax = nb_absf(x);
        z[0] += (ax - z[0]) * (ax > z[0] ? 0.5f : 0.0005f);
        float th = q[0] * z[0], hi = th * (1.f + q[2]) + 1e-4f, lo = -th * (1.f - q[2]) - 1e-4f, xp = z[2];
        float old = zi[0] ? 0.7f : -0.7f, out = old;
        int flip = 0;
        float t = 0.f;
        if (!zi[0] && x > hi) { zi[0] = 1; flip = 1; t = (hi - xp) / (x - xp + 1e-9f); }
        else if (zi[0] && x < lo) { zi[0] = 0; flip = 1; t = (lo - xp) / (x - xp - 1e-9f); }
        if (flip) {
            out = zi[0] ? 0.7f : -0.7f;
            float h = out - old, tt = nb_clamp(t, 0.f, 1.f), d = 1.f - tt;
            z[1] += h * 0.5f * d * d;
            out += -h * 0.5f * tt * tt;
        }
        float held = z[1];
        z[1] = out; z[2] = x;
        z[3] += (held - z[3]) * q[1];
        return couple(z[3], &z[4]);
    }
    case NB_FX_BLAST * 3 + 0: { /* an op-amp rail to rail, the highs lifted before */
        float pre = x - q[2] * z[0];
        z[0] = x;
        float y = nb_clamp(pre * q[0], -1.f, 1.f);
        z[2] += (y - z[2]) * q[1];
        return z[2] * 0.9f;
    }
    case NB_FX_BLAST * 3 + 1: { /* the slew rate: the output can only move so fast; the rails skewed */
        float cc = 0.5f * q[2];
        float tgt = nb_clamp(x * q[0] - cc, -1.f, 1.f) + cc;
        z[1] += nb_clamp(tgt - z[1], -q[1], q[1]);
        return couple(z[1], &z[2]) * 0.9f;
    }
    case NB_FX_BLAST * 3 + 2: { /* the phase reversal: past the common-mode range the output jumps to the other rail
                                 * and sticks there; the op-amp's own bandwidth smooths the jumps */
        float y = nb_clamp(x * q[0], -1.f, 1.f);
        if (nb_absf(x) > q[1]) { zi[0] = (int)q[2] + 1; zi[1] = x > 0.f ? -1 : 1; }
        if (zi[0] > 0) { --zi[0]; y = (float)zi[1]; }
        z[1] += (y - z[1]) * q[3];
        return z[1] * 0.9f;
    }
    case NB_FX_FOLD * 3 + 0: {  /* a triangle fold, biased, then softened; a coupling capacitor after */
        z[2] += (tri_f((x + q[1]) * q[0]) - z[2]) * q[2];
        return z[2];
    }
    case NB_FX_FOLD * 3 + 1: {  /* the tuned folder: its level held, so N folds of a sine bring harmonic N forward */
        float env = zs->lvl[c] + 0.02f;
        return tri_f((x / env + q[1]) * q[0]) * env;
    }
    case NB_FX_FOLD * 3 + 2: {  /* folding time: the loop's own past read back and forth across the span */
        z[0] += 1.f / (zs->per[c] * q[1]);
        if (z[0] >= 1.f) z[0] -= 1.f;
        float ph = z[0], b = q[2], tri = ph < b ? ph / b : (1.f - ph) / (1.f - b);
        return read_line(e->line[c], e->wp, 2.f + q[0] * tri);
    }
    case NB_FX_WRAP * 3 + 0: {  /* past the edge, the other side */
        z[2] += (saw_f((x + q[1]) * q[0]) - z[2]) * q[2];
        return z[2];
    }
    case NB_FX_WRAP * 3 + 1: {  /* the spark: each overflow strikes the next loop */
        float t = x * q[0], ad_out = saw_f(t);
        int k = (int)nb_floor((t + 1.f) * 0.5f);
        if (k != zi[0]) {
            if (wr) zs->spark[(c + 1) & 3] += k > zi[0] ? q[1] : -q[1];
            zi[0] = k;
        }
        return ad_out;
    }
    case NB_FX_WRAP * 3 + 2: {  /* BITS: the sample as a 16-bit integer, its high bits XOR the source's */
        float src = fbk[(c + 1) & 3] + (fbk[c] - fbk[(c + 1) & 3]) * q[1];
        int a = (int)(nb_clamp(x, -1.f, 1.f) * 32767.f), b = (int)(nb_clamp(src, -1.f, 1.f) * 32767.f);
        /* the sign stays the loop's (its polarity is what keeps it singing): the XOR takes the n bits under it */
        int n = (int)q[0];
        uint32_t mask = (0x7fffu << (15 - n)) & 0x7fffu;
        int16_t r = (int16_t)(((uint32_t)a & 0xffffu) ^ ((uint32_t)b & mask));
        z[0] += ((float)r * (1.f / 32767.f) - z[0]) * q[2];
        return couple(z[0], &z[1]);
    }
    case NB_FX_RECTIFY * 3 + 0: {   /* full wave (the octave up) .. half wave, over a threshold */
        float h = q[0];
        float r = rect_f(x, q[1], h) * 2.f - (1.27f - 0.6f * h) * zs->lvl[c];
        z[2] += (r - z[2]) * q[2];
        return z[2];
    }
    case NB_FX_RECTIFY * 3 + 1: {   /* octaves in cascade: each full wave doubles the frequency */
        float r1 = couple(rect_f(x, q[1], 0.f) * 2.f, &z[2]);
        float r2 = couple(nb_absf(r1) * 2.f, &z[4]);
        float r3 = couple(nb_absf(r2) * 2.f, &z[6]);
        float o = q[0], y;
        if (o < 1.f) y = x + (r1 - x) * o;
        else if (o < 2.f) y = r1 + (r2 - r1) * (o - 1.f);
        else y = r2 + (r3 - r2) * (o - 2.f);
        z[8] += (y - z[8]) * q[2];
        return z[8];
    }
    case NB_FX_RECTIFY * 3 + 2: {   /* the rectifier that counts: one half-cycle in N flips, at its crossing */
        int up = x > 1e-4f, dn = x < -1e-4f;
        if ((up && zi[1] < 0) || (dn && zi[1] > 0)) { if (++zi[0] >= (int)q[0]) zi[0] = 0; }
        if (up) zi[1] = 1; else if (dn) zi[1] = -1;
        float y = zi[0] == (int)q[1] ? -x : x;
        z[0] += (y - z[0]) * q[2];
        return couple(z[0], &z[1]);
    }
    case NB_FX_DIVIDE * 3 + 0: case NB_FX_DIVIDE * 3 + 1: {
        /* flip-flops (an octave and two below) or counters (two subharmonics, divide by N); a window on the
         * detector against the noise; each edge polyBLEP'd */
        float env = zs->lvl[c], hy = 0.002f + 0.03f * env, xp = z[0];
        int edge = 0;
        float comb_old, comb_new;
#define DIV_COMB(v) do { if (st == 0) {                                                                     \
            float s1_ = zi[0] ? 1.f : -1.f, s2_ = zi[1] ? 1.f : -1.f, oct_ = s1_ + (s2_ - s1_) * s[0];         \
            float pu_ = (zi[0] && zi[1]) ? 1.f : -1.f;                                                         \
            v = oct_ + (pu_ - oct_) * s[1];                                                                    \
        } else {                                                                                               \
            v = 0.5f * ((zi[0] < ((int)q[0] >> 1) ? 1.f : -1.f) + (zi[1] < ((int)q[1] >> 1) ? 1.f : -1.f));   \
        } } while (0)
        if (!zi[2] && x > hy) {                       /* a rising crossing of the upper threshold */
            DIV_COMB(comb_old);                       /* (the shape before the edge: only at an edge) */
            zi[2] = 1;
            edge = 1;
            if (st == 0) { zi[0] = !zi[0]; if (zi[0]) zi[1] = !zi[1]; }
            else { if (++zi[0] >= (int)q[0]) zi[0] = 0; if (++zi[1] >= (int)q[1]) zi[1] = 0; }
        } else { comb_old = 0.f; if (zi[2] && x < -hy) zi[2] = 0; }
        DIV_COMB(comb_new);
#undef DIV_COMB
        float out = comb_new * env * 1.4f;
        if (edge) {
            float hstep = (comb_new - comb_old) * env * 1.4f;
            float t = nb_clamp((hy - xp) / (x - xp + 1e-9f), 0.f, 1.f), d = 1.f - t;
            z[1] += hstep * 0.5f * d * d;             /* the sample before the edge */
            out += -hstep * 0.5f * t * t;             /* this one */
        }
        float held = z[1];
        z[1] = out; z[0] = x;
        z[2] += (held - z[2]) * q[2];
        return z[2];
    }
    case NB_FX_DIVIDE * 3 + 2: {    /* EUCLID: the loop's crossings counted into a euclidean pattern that gates it */
        float hy = 0.002f + 0.03f * zs->lvl[c];
        if (!zi[2] && x > hy) {
            zi[2] = 1;
            if (++zi[0] >= (int)q[2]) {               /* a step */
                zi[0] = 0;
                zi[1] += (int)q[1];
                if (zi[1] >= (int)q[0]) { zi[1] -= (int)q[0]; zi[3] = 1; } else zi[3] = 0;
            }
        } else if (zi[2] && x < -hy) zi[2] = 0;
        z[0] += ((zi[3] ? 1.f : 0.f) - z[0]) * q[3];
        return x * z[0];
    }
    case NB_FX_RING * 3 + 0: {  /* the ring of diodes with its own oscillator: ring .. AM, sine .. rounded square */
        float dc = s[1];
        return x * (zs->rg_car * (1.f - 0.5f * dc) + dc) * 2.f;
    }
    case NB_FX_RING * 3 + 1: {  /* the carrier from the network: the neighbour .. a sine at a simple ratio of the loop */
        z[0] += 2.f * NB_PI * q[1] / zs->per[c];
        if (z[0] > NB_PI) z[0] -= 2.f * NB_PI;
        float nb = fbk[(c + 1) & 3], car = nb + (nb_sin(z[0]) - nb) * s[0], dc = s[2];
        return x * (car * (1.f - 0.5f * dc) + dc) * 2.f;
    }
    case NB_FX_RING * 3 + 2: {  /* the product of all: each loop times the others, each held to its level */
        float p = x;
        int n = (int)q[0];
        for (int k = 1; k < n; ++k) {
            int j = (c + k) & 3;
            p *= fbk[j] / (zs->lvl[j] + 0.05f);
        }
        float ap = nb_absf(p);
        z[1] += (ap - z[1]) * (ap > z[1] ? 0.2f : 0.002f);
        p *= 1.f - s[1] + s[1] * zs->lvl[c] / (z[1] + 1e-3f);
        z[0] += (nb_clamp(p, -1.5f, 1.5f) - z[0]) * q[2];
        return z[0];
    }
    case NB_FX_STARVE * 3 + 0: case NB_FX_STARVE * 3 + 1: case NB_FX_STARVE * 3 + 2: {
        /* a weak supply: the headroom sags with the level, small signals die. Heterodox: one supply for the
         * four. Heretic: the supply motorboats, faster under load */
        float ax = nb_absf(x), dead = st == 0 ? q[1] : 0.01f, head;
        z[0] += (ax - z[0]) * (ax > z[0] ? 0.5f : q[2]);
        if (st == 2) {
            if (c == 0) {
                float load = 0.25f * (zs->lvl[0] + zs->lvl[1] + zs->lvl[2] + zs->lvl[3]);
                zs->mb_ph += 2.f * NB_PI * q[1] * (0.3f + 1.4f * load) / e->fs;
                if (zs->mb_ph > NB_PI) zs->mb_ph -= 2.f * NB_PI;
                float sn = nb_sin(zs->mb_ph), dip = sn > 0.f ? sn * sn : 0.f;
                zs->mb_head += ((1.f - 0.9f * q[0] * dip) - zs->mb_head) * q[2];
            }
            head = zs->mb_head;
        } else {
            float se = z[0];
            if (st == 1) {
                float sh = 0.f;
                for (int j = 0; j < NB_CH; ++j) sh += zs->z[NB_FX_STARVE][1][j][0];
                se += (0.25f * sh - se) * q[1];
            }
            head = 1.f - q[0] * nb_clamp(se * 1.5f, 0.f, 1.f);
        }
        return starve_f(x, dead, head);
    }
    case NB_FX_SHIFT * 3 + 0: case NB_FX_SHIFT * 3 + 1: case NB_FX_SHIFT * 3 + 2: {
        /* a frequency shifter: the analytic signal times a turning phasor (the heretic: its conjugate, the
         * spectrum mirrored around the axis) */
        float in = x + q[2] * z[0], a, b;
        hilbert(in, zs->hb[st][c], &zs->hbz[st][c], &a, &b);
        float sb = st == 2 ? -b : b;
        float y = nb_clamp(a * zs->shc[st][c] - sb * zs->shs[st][c], -1.5f, 1.5f);
        if (nb_isbad(y)) {                         /* through volatile: a plain loop would become a memset call */
            volatile float *h = zs->hb[st][c];
            for (int k = 0; k < NB_HB; ++k) h[k] = 0.f;
            y = 0.f;
        }
        z[0] = y;
        return y;
    }
    case NB_FX_ECHO * 3 + 0: {  /* a short echo in each loop, the four a little apart, its clock gliding, wow */
        float spread = c == 0 ? 1.f : c == 1 ? 1.13f : c == 2 ? 0.87f : 1.27f;
        float ph = c == 0 ? zs->wow_s : c == 1 ? zs->wow_s * 0.5403f + zs->wow_c * 0.8415f
                 : c == 2 ? zs->wow_s * -0.4161f + zs->wow_c * 0.9093f : zs->wow_s * -0.99f + zs->wow_c * 0.1411f;
        if (z[0] <= 0.f) z[0] = q[0] * spread;
        z[0] += (q[0] * spread - z[0]) * q[3];
        float d = z[0] * (1.f + q[2] * ph);
        float r = rd16(e->echo[c], e->echo_wp, NB_ECHO, d);
        if (wr) e->echo[c][e->echo_wp] = to16(x + q[1] * r, &e->rng);
        float ar = nb_absf(r);
        if (ar > zs->tail[0]) zs->tail[0] = ar;
        return x + r;                                 /* the repeats added to the loop */
    }
    case NB_FX_ECHO * 3 + 1: {  /* the tuned echo: a comb at a ratio of the loop's period, damped */
        float tgt = nb_clamp(zs->per[c] * q[0], 2.f, (float)(NB_ECHO - 4));
        if (z[0] <= 0.f) z[0] = tgt;
        z[0] += (tgt - z[0]) * q[3];
        float r = rd16(e->echo[c], e->echo_wp, NB_ECHO, z[0]);
        z[1] += (r - z[1]) * q[2];
        if (wr) e->echo[c][e->echo_wp] = to16(x + q[1] * z[1], &e->rng);
        float ar = nb_absf(r);
        if (ar > zs->tail[0]) zs->tail[0] = ar;
        return x + z[1];
    }
    case NB_FX_ECHO * 3 + 2: {  /* the echo backwards: two heads read the window from its end to its start */
        z[0] += q[3];
        if (z[0] >= 1.f) z[0] -= 1.f;
        float p1 = z[0], p2 = p1 + 0.5f;
        if (p2 >= 1.f) p2 -= 1.f;
        float g1 = 1.f - nb_absf(2.f * p1 - 1.f), g2 = 1.f - g1;
        float rev = g1 * rd16(e->echo[c], e->echo_wp, NB_ECHO, 2.f + 2.f * q[0] * p1)
                  + g2 * rd16(e->echo[c], e->echo_wp, NB_ECHO, 2.f + 2.f * q[0] * p2);
        float fwd = rd16(e->echo[c], e->echo_wp, NB_ECHO, q[0]);
        float r = fwd + (rev - fwd) * q[2];
        if (wr) e->echo[c][e->echo_wp] = to16(x + q[1] * r, &e->rng);
        float ar = nb_absf(r);
        if (ar > zs->tail[0]) zs->tail[0] = ar;
        return x + r;
    }
    case NB_FX_PUMP * 3 + 0: {  /* a compressor in the loop: loud, it ducks; then it swells back */
        float ax = nb_absf(x);
        z[0] += (ax - z[0]) * (ax > z[0] ? q[1] : q[2]);
        float k = q[0];
        return x * (1.f + 0.3f * k) / (1.f + k * z[0]);
    }
    case NB_FX_PUMP * 3 + 1: {  /* the chase: ducked by the loop before it in the ring, or by all the others */
        float ax = nb_absf(x);
        z[0] += (ax - z[0]) * (ax > z[0] ? q[3] : q[2]);
        float own = z[0], prev = zs->z[NB_FX_PUMP][1][(c + 3) & 3][0], oth = 0.f;
        for (int j = 0; j < NB_CH; ++j) if (j != c) oth += zs->z[NB_FX_PUMP][1][j][0];
        oth *= (1.f / 3.f);
        float u = q[1], sc = u < 0.5f ? own + (prev - own) * (u * 2.f) : prev + (oth - prev) * (u * 2.f - 1.f);
        float k = q[0];
        return x * (1.f + 0.3f * k) / (1.f + k * sc);
    }
    case NB_FX_PUMP * 3 + 2: {  /* the negative ratio: above the threshold, louder in is quieter out */
        float ax = nb_absf(x);
        z[0] += (ax - z[0]) * (ax > z[0] ? q[1] : q[2]);
        float g = 1.f;
        if (z[0] > 0.15f) g = nb_clamp(nb_exp2(nb_log2(z[0] * 6.6667f) * q[0]), 0.01f, 4.f);
        return x * g;
    }
    case NB_FX_SWEEP * 3 + 0: case NB_FX_SWEEP * 3 + 1: case NB_FX_SWEEP * 3 + 2: {
        /* a band-pass the loop's own level moves; heterodox: on the loop's harmonics; heretic: moved by the signal
         * itself, at audio rate */
        float ax = nb_absf(x);
        z[0] += (ax - z[0]) * (ax > z[0] ? 0.02f : 0.0015f);
        float fc;
        if (st == 0) fc = q[1] * nb_exp2(nb_clamp(z[0] * q[0], 0.f, 6.f));
        else if (st == 1) fc = e->fs / zs->per[c] * (q[1] + nb_clamp(z[0] * 2.f, 0.f, 1.f) * q[0]);
        else fc = q[1] * nb_exp2(nb_clamp(x, -1.5f, 1.5f) * q[0]);
        float wc = NB_PI * fc / e->fs;
        float g = nb_tan(wc < 1.2f ? wc : 1.2f);
        float k = q[2];
        float hp = (x - (k + g) * z[1] - z[2]) / (1.f + g * (k + g));
        float bp = g * hp + z[1];
        z[1] = nb_clamp(g * hp + bp, -4.f, 4.f);
        float lp = g * bp + z[2];
        z[2] = nb_clamp(g * bp + lp, -4.f, 4.f);
        if (nb_isbad(bp)) { z[1] = z[2] = 0.f; bp = 0.f; }
        return bp * (k + 0.3f) * 2.f;
    }
    }
    return x;
}

/* the screen's CHAIN (docs/TELA.md 13.3): the signal between the stages, one store each; read by probe_stages */
#ifdef NB_NO_PROBE              /* a build without the CHAIN view's measure (its stages read 0) */
#define NB_TAP(k) ((void)0)
#else
#define NB_TAP(k) (e->tp[k] = x)
#endif

/* ---- the fixed chain, one loop, one sample; each effect in its stage, crossfading after a change ---- */
NB_INLINE float chain(nb_engine *e, int c, float x, const float *fbk)
{
    nb_fstate *zs = &e->f;
    float ax = nb_absf(x), env = zs->lvl[c];
    env += (ax - env) * (ax > env ? 0.5f : 0.0008f);
    zs->lvl[c] = env;
    NB_NOUNROLL
    for (int f = 0; f < NB_NLOOPFX; ++f) {
        NB_TAP(f);
        float m = zs->mixr[f];
        int on = m > 1e-4f || (f == NB_FX_ECHO && zs->tail[0] > 1e-5f);
        if (!on) continue;
        float xf = zs->xf[f], y = 0.f;
        int passes = xf > 0.f ? 2 : 1;
        NB_NOUNROLL
        for (int pass = 0; pass < passes; ++pass) {
            int st = pass ? e->stg_old[f] : e->stg[f];
            float v = fx_run(e, f, st, c, x, fbk, !pass);
            y = pass ? y + (v - y) * xf : v;
        }
        x += (y - x) * m;
    }
    NB_TAP(NB_NLOOPFX);
    return x;
}

/* how much each stage in the chain changes the signal: the difference across it against what goes in, both mean
 * squares over ~40 ms, on one sample in four; the stages that are out cost nothing */
NB_INLINE void probe_stages(nb_engine *e, int c)
{
    nb_fstate *z = &e->f;
    const float a = 100.f / e->fs;
    for (int f = 0; f < NB_NLOOPFX; ++f) {
        if (z->mixr[f] <= 1e-4f) continue;
        float i = e->tp[f], d = e->tp[f + 1] - i;
        z->m_sd[c][f] += (d * d - z->m_sd[c][f]) * a;
        z->m_si[c][f] += (i * i - z->m_si[c][f]) * a;
    }
}

/* ---- after the network: a short ping-pong DELAY (a bucket brigade), then a small plate REVERB ---- */
NB_INLINE void post_fx(nb_engine *e, float *L, float *R)
{
    nb_fstate *z = &e->f;
    float (*s)[4] = z->fx_s;
    float dm = z->mixr[NB_FX_DELAY], rm = z->mixr[NB_FX_REVERB];
    (void)s;
    if (dm > 1e-4f || z->tail[1] > 1e-5f) {
        const float *q = z->coef[NB_FX_DELAY][e->stg[NB_FX_DELAY]];
        if (z->pdl_d <= 0.f) z->pdl_d = q[0];
        z->pdl_d += (q[0] - z->pdl_d) * z->pdl_dk;       /* the clock glides: a bend, not a jump */
        float d = z->pdl_d;
        float dl = rd16(e->pdl[0], e->pdl_wp, NB_PDLY, d), dr = rd16(e->pdl[1], e->pdl_wp, NB_PDLY, d);
        {   /* the brigade's reconstruction filter: two poles a side, its corner following the clock */
            float b = z->pdl_bbc;
            z->pdl_bb[0][0] += (dl - z->pdl_bb[0][0]) * b; z->pdl_bb[0][1] += (z->pdl_bb[0][0] - z->pdl_bb[0][1]) * b;
            z->pdl_bb[1][0] += (dr - z->pdl_bb[1][0]) * b; z->pdl_bb[1][1] += (z->pdl_bb[1][0] - z->pdl_bb[1][1]) * b;
            dl = z->pdl_bb[0][1]; dr = z->pdl_bb[1][1];
        }
        dl = couple(dl, z->pdl_cp[0]); dr = couple(dr, z->pdl_cp[1]);   /* its coupling capacitors: no DC goes round */
        float fb = q[1], tc = q[2];
        z->pdl_lp[0] += (dr - z->pdl_lp[0]) * tc;        /* each side feeds the other: ping-pong, its tone */
        z->pdl_lp[1] += (dl - z->pdl_lp[1]) * tc;
        {   /* into the brigade: a soft saturation (each repeat a little rounder) and its compander's faint hiss */
            float h = 0.0006f * nb_noise(&e->rng);
            e->pdl[0][e->pdl_wp] = to16(nb_tanh(*L + fb * z->pdl_lp[0]) + h, &e->rng);
            e->pdl[1][e->pdl_wp] = to16(nb_tanh(*R + fb * z->pdl_lp[1]) - h, &e->rng);
        }
        float a = nb_absf(dl) > nb_absf(dr) ? nb_absf(dl) : nb_absf(dr);
        if (a > z->tail[1]) z->tail[1] = a;
        *L += dm * dl;
        *R += dm * dr;
        z->pdl_ret[0] = dl; z->pdl_ret[1] = dr;
        z->pdl_hp[0] += (q[3] * dm - z->pdl_hp[0]) * 0.001f;   /* the heretic's return, gliding ~20 ms */
    } else { z->pdl_ret[0] = z->pdl_ret[1] = 0.f; z->pdl_hp[0] = 0.f; }
    if (++e->pdl_wp >= NB_PDLY) e->pdl_wp = 0;
    if (rm > 1e-4f || z->tail[2] > 1e-5f) {
        const float *q = z->coef[NB_FX_REVERB][e->stg[NB_FX_REVERB]];
        /* the input diffused by two allpasses (no flutter of separate echoes), then four lines mixed by a Hadamard
         * matrix, each damped, each with its own loss for the decay asked; two of them drift slowly (no metal) */
        float in = 0.35f * (*L + *R);
        {
            uint32_t w1 = e->dif_wp1, w2 = e->dif_wp2;
            float d1 = z->dif1[w1], v1 = in + 0.6f * d1;
            z->dif1[w1] = v1; in = d1 - 0.6f * v1;
            float d2 = z->dif2[w2], v2 = in + 0.6f * d2;
            z->dif2[w2] = v2; in = d2 - 0.6f * v2;
            e->dif_wp1 = w1 + 1u >= NB_DIF1 ? 0u : w1 + 1u;
            e->dif_wp2 = w2 + 1u >= NB_DIF2 ? 0u : w2 + 1u;
        }
        {   /* the slow drift: a quadrature pair at ~0.4 Hz */
            float c0 = z->rv_modc, s0 = z->rv_mods, wv = 2.f * NB_PI * 0.4f / e->fs;
            z->rv_modc = c0 - wv * s0; z->rv_mods = s0 + wv * z->rv_modc;
        }
        uint32_t w = e->fdn_wp;
        float o0 = rd16(e->fdn[0], w, NB_FDN, z->fdn_len[0] + 6.f * z->rv_modc);
        float o1 = rd16(e->fdn[1], w, NB_FDN, z->fdn_len[1]);
        float o2 = rd16(e->fdn[2], w, NB_FDN, z->fdn_len[2] + 6.f * z->rv_mods);
        float o3 = rd16(e->fdn[3], w, NB_FDN, z->fdn_len[3]);
        z->rv_damp += (q[1] - z->rv_damp) * 0.002f;     /* damping and width glide: a stage change is a turn */
        z->rv_wd += (q[2] - z->rv_wd) * 0.002f;
        float damp = z->rv_damp, wd = z->rv_wd;
        z->fdn_lp[0] += ((o0 + o1 + o2 + o3) * 0.5f * z->fdn_g[0] - z->fdn_lp[0]) * damp;
        z->fdn_lp[1] += ((o0 - o1 + o2 - o3) * 0.5f * z->fdn_g[1] - z->fdn_lp[1]) * damp;
        z->fdn_lp[2] += ((o0 + o1 - o2 - o3) * 0.5f * z->fdn_g[2] - z->fdn_lp[2]) * damp;
        z->fdn_lp[3] += ((o0 - o1 - o2 + o3) * 0.5f * z->fdn_g[3] - z->fdn_lp[3]) * damp;
        for (int k = 0; k < 4; ++k) e->fdn[k][w] = to16(in + z->fdn_lp[k], &e->rng);
        float mid = 0.35f * (o0 + o1 + o2 + o3), side = 0.7f * (o0 + o2 - o1 - o3);
        float a = nb_absf(mid) + nb_absf(side);
        if (a > z->tail[2]) z->tail[2] = a;
        float wl = couple(mid + side * wd, z->rv_cp[0]), wr = couple(mid - side * wd, z->rv_cp[1]);   /* no DC from the room */
        *L += rm * wl;
        *R += rm * wr;
        z->sw_amt += (q[3] * nb_clamp(rm * 2.f, 0.f, 1.f) - z->sw_amt) * 0.001f;   /* the heretic: the room swallows */
    } else z->sw_amt *= 0.999f;
    if (++e->fdn_wp >= NB_FDN) e->fdn_wp = 0;
}

NB_AUDIO_CODE void nb_engine_render(nb_engine *e, const nb_params *p, nb_touch *t, float *out, int n)
{
    nb_fstate *z = &e->f;
    const float fs = e->fs;
    const float sm = 1.f - nb_exp2(-1.f / (0.010f * fs) * 1.4427f);   /* 10 ms smoothing */
    const float a_gate = 1.f - nb_exp2(-1.f / (0.001f * fs) * 1.4427f);   /* the gates: an RC, 1 ms */
    const float a_up = 1.f - nb_exp2(-1.f / (0.005f * fs) * 1.4427f);     /* the return opens, 5 ms */
    const float a_dn = 1.f - nb_exp2(-1.f / (0.020f * fs) * 1.4427f);     /* and closes, 20 ms */
    const float a_att = 1.f - nb_exp2(-1.f / (0.001f * fs) * 1.4427f);    /* the limiter's attack, 1 ms */
    const float rel = nb_exp2(-1.f / (0.150f * fs) * 1.4427f);        /* limiter release */
    const float lv_a = 1.f - nb_exp2(-1.f / (0.040f * fs) * 1.4427f); /* meters, 40 ms */
    const float tail_d = nb_exp2(-1.f / (0.050f * fs) * 1.4427f);     /* the tails' envelopes fall, 50 ms */
    const float xf_step0 = 1.f / (0.010f * fs);                       /* before the first sub-block: 10 ms */
    const float wow_w = 2.f * NB_PI * 0.6f / fs;

    /* the strike: one-shot, read once */
    if (t->kick_mask) {
        int len = (int)((0.008f - 0.007f * t->kick_color) * fs);
        for (int c = 0; c < NB_CH; ++c)
            if (t->kick_mask & (1u << c)) e->kick_left[c] = len;
        z->kick_amp = 0.9f;
        z->kick_a = 0.05f + 0.9f * t->kick_color;                     /* noise colour */
        z->kick_dec = nb_exp2(-1.f / ((float)len * 0.35f) * 1.4427f);
        t->kick_mask = 0;
    }
    e->ret_target = (t->hold && !t->ret_cut) ? 1.f : 0.f;

    for (int i = 0; i < n; ++i) {
        if ((e->time & (NB_BLOCK - 1)) == 0) {
            coefficients(e, p);
            {   /* a slice of the state swept for denormals: the whole block every 64 sub-blocks */
                float *fl = (float *)z;
                const uint32_t nf = (uint32_t)(sizeof *z / sizeof(float)), chunk = nf / 64u + 1u;
                uint32_t k0 = e->sweep, k1 = k0 + chunk > nf ? nf : k0 + chunk;
                for (uint32_t k = k0; k < k1; ++k) if (fl[k] > -1e-15f && fl[k] < 1e-15f) fl[k] = 0.f;
                e->sweep = k1 >= nf ? 0u : k1;
            }
            {   /* the ECHO and DELAY clocks, the brigade's passband following the delay's clock */
                z->pdl_dk = 1.f - nb_exp2(-1.f / (0.12f * fs) * 1.4427f);
                float sec = z->pdl_d / fs;
                float fbb = 0.18f * 4096.f / (sec > 0.001f ? sec : 0.001f);
                if (fbb > 9000.f) fbb = 9000.f;
                z->pdl_bbc = lp_coef(fbb, fs);
            }
        }
        {   /* 10 ms, or TRANSITION's pace after a jump (a memory, a modulation's source) */
            const float *r = e->rt_misc;
            float r0 = r[0] > 0.f ? r[0] : sm, r1 = r[1] > 0.f ? r[1] : sm, r2 = r[2] > 0.f ? r[2] : sm;
            float r3 = r[3] > 0.f ? r[3] : sm, r4 = r[4] > 0.f ? r[4] : sm;
            z->gain_s += (p->gain - z->gain_s) * r0;
            z->cross_s += (p->cross - z->cross_s) * r1;
            z->drive_s += (p->drive - z->drive_s) * r2;
            z->grito_s += (p->grito - z->grito_s) * r3;
            z->level_s += (p->level - z->level_s) * r4;
        }
        {   /* SHIFT's phasors turn one step (each stage its own); ECHO's wow; RING's carrier; the stages' fades */
            if (z->mixr[NB_FX_SHIFT] > 1e-4f)
                for (int k = 0; k < (z->xf[NB_FX_SHIFT] > 0.f ? 2 : 1); ++k) {
                    int s = k ? e->stg_old[NB_FX_SHIFT] : e->stg[NB_FX_SHIFT];
                    for (int c = 0; c < NB_CH; ++c) {
                        float nc = z->shc[s][c] * z->shdc[s][c] - z->shs[s][c] * z->shds[s][c];
                        z->shs[s][c] = z->shc[s][c] * z->shds[s][c] + z->shs[s][c] * z->shdc[s][c];
                        z->shc[s][c] = nc;
                    }
                }
            if (z->mix_moving > 0.f) for (int f = 0; f < NB_NFX; ++f) z->mixr[f] += z->dmixr[f];
            if (z->mixr[NB_FX_ECHO] > 1e-4f || z->tail[0] > 0.f) { z->wow_c -= wow_w * z->wow_s; z->wow_s += wow_w * z->wow_c; }
            {   /* RING's own carrier, once for the four loops, always turning (in at any moment without a jump) */
                z->rg_ph += z->coef[NB_FX_RING][0][0];
                if (z->rg_ph > NB_PI) z->rg_ph -= 2.f * NB_PI;
                float car = nb_sin(z->rg_ph), sh = z->fx_s[NB_FX_RING][2];
                z->rg_car = sh > 1e-4f ? car + (nb_sig(4.f * car) * 1.0308f - car) * sh : car;
            }
            if (z->xf_any > 0.f)
                for (int f = 0; f < NB_NLOOPFX; ++f) if (z->xf[f] > 0.f) {
                    z->xf[f] -= e->xf_step > 0.f ? e->xf_step : xf_step0;
                    if (z->xf[f] < 0.f) z->xf[f] = 0.f;
                }
            for (int k = 0; k < 3; ++k) if (z->tail[k] > 0.f) z->tail[k] *= tail_d;
            if (z->band_moving > 0.f) {
                for (int c = 0; c < NB_CH; ++c) { z->g[c] += z->dg[c]; z->kq[c] += z->dk[c]; }
                z->mixb += z->dmixb;
            }
        }

        /* the return: an RC, opening in ~5 ms, closing in ~20 */
        z->ret += (e->ret_target - z->ret) * (e->ret_target > z->ret ? a_up : a_dn);

        float cr = z->cross_s * z->ret;
        /* SCREAM: the loop's output bends every read (phase feedback on top of the signal feedback) */
        float bend = nb_exp2(z->grito_s * z->grito_s * 3.f * z->out_prev);
        z->m_bend += (bend - z->m_bend) * lv_a;                  /* the screen's pitch: the mean bend */

        float fbk[NB_CH];
        for (int c = 0; c < NB_CH; ++c) fbk[c] = read_line(e->line[c], e->wp, z->per[c] * bend);
        float sumL = 0.f, sumR = 0.f, tot = 0.f;
        const float rw = z->net_amt[2];
        NB_NOUNROLL
        for (int c = 0; c < NB_CH; ++c) {
            /* DRIVE also multiplies how far each loop is above unity: a loop that sings hotter */
            float tolg = c == 0 ? 1.004f : c == 1 ? 0.996f : c == 2 ? 1.002f : 0.998f;
            float gc = z->gain_s * z->chg_s[c] * tolg;
            if (gc > 1.f) gc = 1.f + (gc - 1.f) * (1.f + 3.f * z->drive_s);
            float in = gc * z->ret * fbk[c];
            for (int j = 0; j < NB_CH; ++j) {
                if (j == c) continue;
                float m = z->mtx_s[c][j];
                if (m > -1e-5f && m < 1e-5f) continue;
                if (rw > 1e-3f) m *= 1.f + (z->rew[c][j] - 1.f) * rw;
                float s = cr * m * fbk[j];
                in += s;
                z->m_flow[c][j] += (s * s - z->m_flow[c][j]) * lv_a;
            }
            /* the strike and the floor every circuit has, WRAP's sparks */
            float nz = nb_noise(&e->rng);
            float floor_n = nz;
            float hs = z->hiss_s * z->hiss_s + 0.3f * z->heat[c] * z->net_amt[1] * z->hiss_s;
            in += (0.00003f * floor_n + 0.02f * hs * nz) * z->ret;
            if (e->kick_left[c] > 0) {
                z->kick_lp[c] += (nz - z->kick_lp[c]) * z->kick_a;
                in += z->kick_lp[c] * z->kick_amp * (1.f + 2.f * (1.f - z->kick_a));
                --e->kick_left[c];
            }
            /* WRAP's sparks: each overflow of the loop before, a strike coloured by its COLOR */
            if (z->spark[c] != 0.f || z->spark_lp[c] != 0.f) {
                z->spark_lp[c] = z->spark_lp[c] * (1.f - z->coef[NB_FX_WRAP][1][2]) + z->spark[c];
                z->spark[c] = 0.f;
                in += z->spark_lp[c];
            }
            /* the DELAY's heretic: its repeats back into the loops (left into 1 and 3, right into 2 and 4) */
            if (z->pdl_hp[0] > 0.f) in += z->pdl_ret[c & 1] * z->pdl_hp[0];
            /* the preamp: a knee from tanh (DRIVE 0) toward a clip (DRIVE 1); plain (its ADAA bought ~5 dB at 3 kHz
             * for 43 ns a frame: not paid on the FM-1) */
            float sk = nb_tanh(in), hk = nb_clamp(in, -1.f, 1.f);
            float pre = sk + (hk - sk) * z->drive_s;
            float hv = nb_absf(in) - nb_absf(pre);
            z->m_heat[c] += ((hv > 0.f ? hv : 0.f) - z->m_heat[c]) * lv_a;
            /* DC block, ~16 Hz */
            float dc = pre - z->dc_x[c] + 0.9977f * z->dc_y[c];
            z->dc_x[c] = pre; z->dc_y[c] = dc;
            /* band-pass, TPT state variable, unity at the peak */
            float gg = z->g[c], k = z->kq[c];
            float hp = (dc - (k + gg) * z->s1[c] - z->s2[c]) / (1.f + gg * (k + gg));
            float bp = gg * hp + z->s1[c];
            z->s1[c] = gg * hp + bp;
            float lp = gg * bp + z->s2[c];
            z->s2[c] = gg * bp + lp;
            float eq = dc + (bp * k - dc) * z->mixb;
            float y = chain(e, c, eq, fbk);
            y = nb_clamp(y, -1.f, 1.f);
            if (nb_isbad(y) || nb_isbad(z->s1[c]) || nb_isbad(z->s2[c])) {
                y = 0.f; z->s1[c] = z->s2[c] = z->dc_x[c] = z->dc_y[c] = 0.f;
            }
            e->line[c][e->wp & (NB_LINE - 1)] = to16(y, &e->rng);
            z->y[c] = y;
            z->m_lv[c] += (y * y - z->m_lv[c]) * lv_a;
#ifndef NB_NO_PROBE
            if ((e->time & 3u) == 0u) probe_stages(e, c);
#endif
            float yo = y;
            if (z->sw_amt > 1e-4f)      /* the REVERB's heretic: what repeats a period later is swallowed */
                yo -= z->sw_amt * read_line(e->line[c], e->wp, z->per[c]);
            if (c & 1) sumR += yo * z->chl_s[c]; else sumL += yo * z->chl_s[c];
            tot += y;
        }
        e->wp = (e->wp + 1u) & (NB_LINE - 1);
        if (++e->echo_wp >= NB_ECHO) e->echo_wp = 0;
        z->out_prev = tot * 0.25f;

        /* the output gate: CUT, CHOP (the loop's own rhythm: rising crossings), SCATTER (random cuts) */
        int open = !t->out_cut;
        if (t->chop) {
            int above = z->out_prev > 0.f;
            if (above && !(e->chop_count & 0x40000000)) {
                e->chop_count = (e->chop_count + 1) | 0x40000000;
                if (((e->chop_count & 0xffff) % t->chop) == 0) e->chop_on = !e->chop_on;
            } else if (!above) e->chop_count &= ~0x40000000;
            open = open && e->chop_on;
        } else e->chop_on = 1;
        if (t->scatter_hz > 0.f) {
            e->scat_left -= 1.f;
            if (e->scat_left <= 0.f) {
                float r = nb_noise(&e->rng) * 0.5f + 0.5f;
                e->scat_left = fs / t->scatter_hz * (0.25f + 1.5f * r);
                e->scat_on = nb_noise(&e->rng) > -0.2f ? !e->scat_on : e->scat_on;
            }
            open = open && e->scat_on;
        } else e->scat_on = 1;
        e->gate_target = open ? 1.f : 0.f;
        z->gate += (e->gate_target - z->gate) * a_gate;   /* a VCA with its RC: no corner to click */

        if (z->kick_amp > 0.f) z->kick_amp *= z->kick_dec;

        float L = sumL * 0.45f * z->gate, R = sumR * 0.45f * z->gate;
        /* the output's coupling capacitor (~5 Hz): no DC leaves the box */
        {
            float yl = L - z->ohp_x[0] + 0.99929f * z->ohp_y[0], yr = R - z->ohp_x[1] + 0.99929f * z->ohp_y[1];
            z->ohp_x[0] = L; z->ohp_y[0] = yl; z->ohp_x[1] = R; z->ohp_y[1] = yr;
            L = yl; R = yr;
        }
        post_fx(e, &L, &R);                                      /* the tails ring on after a CUT */
        float lvl = z->level_s * z->level_s;
        L *= lvl; R *= lvl;
        /* the limiter: a 1 ms attack, 150 ms release, to -3 dBFS, then a soft ceiling that catches what the attack
         * lets through */
        float pk = nb_absf(L) > nb_absf(R) ? nb_absf(L) : nb_absf(R);
        z->env = pk > z->env ? z->env + (pk - z->env) * a_att : z->env * rel;
        float gr = z->env > 0.7f ? 0.7f / z->env : 1.f;
        L *= gr; R *= gr;
        L = 0.98f * nb_tanh(L * 1.0204f);
        R = 0.98f * nb_tanh(R * 1.0204f);
        out[2 * i] = L; out[2 * i + 1] = R;
        z->m_out += (0.5f * (L * L + R * R) - z->m_out) * lv_a;
        e->time++;
    }
}

/* WAVES: the loop's own line (what it sings, written every sample), two periods from a rising zero crossing found
 * within one period, so the picture stands still while the loop does; min and max of the samples under each of the
 * NB_WAVE columns (no aliasing). At most ~2 K reads a loop, once a frame, from the control task. */
static void wave_of(const nb_engine *e, int c, nb_view *v)
{
    const int16_t *ln = e->line[c];
    const uint32_t mk = NB_LINE - 1u, wp = e->wp;
    int per = (int)e->f.per[c];
    if (per < 8) per = 8;
    int win = 2 * per, look = per;
    if (win + look > NB_LINE - 4) { look = 0; if (win > NB_LINE - 4) win = NB_LINE - 4; }
    uint32_t s0 = wp - 1u - (uint32_t)win;                 /* the oldest sample shown, if no crossing is found */
    int found = 0;
    for (int k = 0; k < look; ++k) {
        uint32_t at = s0 - (uint32_t)k;
        if (ln[(at - 1u) & mk] < 0 && ln[at & mk] >= 0) { s0 = at; found = 1; break; }
    }
    if (!found) v->wave_free |= (uint8_t)(1u << c);
    for (int col = 0; col < NB_WAVE; ++col) {
        int a = col * win / NB_WAVE, b = (col + 1) * win / NB_WAVE;
        if (b <= a) b = a + 1;
        int lo = 32767, hi = -32768;
        for (int k = a; k < b; ++k) {
            int x = ln[(s0 + (uint32_t)k) & mk];
            if (x < lo) lo = x;
            if (x > hi) hi = x;
        }
        v->wave_lo[c][col] = (int8_t)(lo >> 8);
        v->wave_hi[c][col] = (int8_t)(hi >> 8);
    }
}

/* SCOPE: the four loops as the output has them (each by its LEVEL and the gate; half their sum), two periods of loop 1
 * from a rising zero crossing found within one period (on a steady drone it stands still), min and max per column;
 * and each loop alone in the same window. Before the post (DELAY, REVERB) and the limiter. Once a frame. */
static void scope_of(const nb_engine *e, const nb_params *p, nb_view *v)
{
    const uint32_t mk = NB_LINE - 1u, wp = e->wp;
    float k[NB_CH];
    for (int c = 0; c < NB_CH; ++c) k[c] = p->ch_level[c] * e->f.gate * 0.5f;
#define NB_MONO(at) (k[0] * e->line[0][(at) & mk] + k[1] * e->line[1][(at) & mk] + k[2] * e->line[2][(at) & mk] +                      k[3] * e->line[3][(at) & mk])
    int per = (int)e->f.per[0];
    if (per < 8) per = 8;
    int win = 2 * per, look = per;
    if (win + look > NB_LINE - 4) { look = 0; if (win > NB_LINE - 4) win = NB_LINE - 4; }
    uint32_t s0 = wp - 1u - (uint32_t)win;
    v->scope_free = 1;
    for (int j = 0; j < look; ++j) {
        uint32_t at = s0 - (uint32_t)j;
        if (NB_MONO(at - 1u) < 0.f && NB_MONO(at) >= 0.f) { s0 = at; v->scope_free = 0; break; }
    }
    for (int col = 0; col < NB_SCOPE; ++col) {
        int a = col * win / NB_SCOPE, b = (col + 1) * win / NB_SCOPE;
        if (b <= a) b = a + 1;
        float lo = 1e9f, hi = -1e9f;
        for (int j = a; j < b; ++j) {
            float x = NB_MONO(s0 + (uint32_t)j);
            if (x < lo) lo = x;
            if (x > hi) hi = x;
        }
        /* full scale (one loop's line at its limit) is 102 of 127: 25 % headroom; 127 means at or over it */
        v->scope_lo[col] = (int8_t)nb_clamp(lo * (1.f / 320.f), -127.f, 127.f);
        v->scope_hi[col] = (int8_t)nb_clamp(hi * (1.f / 320.f), -127.f, 127.f);
        for (int c = 0; c < NB_CH; ++c) v->scope_ch[c][col] = (int8_t)((e->line[c][(s0 + (uint32_t)a) & mk] * 102) >> 15);
    }
#undef NB_MONO
}

/* FREQ: where the energy is. A Goertzel bank on the control task: the four lines as the output has them (each by its
 * LEVEL and the gate, before the post), 23 bands of a third of an octave from 63 Hz to 10 kHz plus LOW (40 Hz, the
 * longest window), each over a Hann window of Q 4.3 periods (at most 1536 samples, 35 ms). One call in three,
 * only while the view asks: ~15 K samples a measure, nothing in the audio path. */
static int spec_want, spec_tick, spec_init_done;
#define NB_SPEC_N 1536          /* the longest window: 35 ms (LOW, and the bands under ~100 Hz) */
static float spec_mono[NB_SPEC_N], spec_coef[NB_SPEC], spec_hann[257];
static int16_t spec_n[NB_SPEC];
void nb_engine_want_spectrum(int on) { spec_want = on; }
static void spectrum_of(const nb_engine *e, const nb_params *p, nb_view *v)
{
    if (!spec_want) { v->spec_on = 0; return; }
    if (!spec_init_done) {
        spec_init_done = 1;
        for (int k = 0; k < NB_SPEC; ++k) {
            float f = k ? 63.f * nb_exp2((float)(k - 1) * (1.f / 3.f)) : 40.f;
            int n = k ? (int)(4.3f * e->fs / f) : NB_SPEC_N;
            spec_n[k] = (int16_t)(n > NB_SPEC_N ? NB_SPEC_N : n);
            spec_coef[k] = 2.f * nb_sin(2.f * NB_PI * f / e->fs + .5f * NB_PI);
        }
        for (int i = 0; i <= 256; ++i) spec_hann[i] = .5f - .5f * nb_sin(2.f * NB_PI * (float)i / 256.f + .5f * NB_PI);
    }
    if (++spec_tick < 3) return;                          /* one call in three measures; the others keep it */
    spec_tick = 0;
    const uint32_t mk = NB_LINE - 1u, wp = e->wp;
    float k4[NB_CH];
    for (int c = 0; c < NB_CH; ++c) k4[c] = p->ch_level[c] * e->f.gate * (1.f / 32767.f);
    for (int j = 0; j < NB_SPEC_N; ++j) {
        uint32_t at = wp - (uint32_t)NB_SPEC_N + (uint32_t)j;
        spec_mono[j] = k4[0] * e->line[0][at & mk] + k4[1] * e->line[1][at & mk] + k4[2] * e->line[2][at & mk] +
                       k4[3] * e->line[3][at & mk];
    }
    for (int k = 0; k < NB_SPEC; ++k) {
        int n = spec_n[k];
        const float *x = spec_mono + (NB_SPEC_N - n), c = spec_coef[k];
        float s1 = 0.f, s2 = 0.f, step = 256.f / (float)n, ph = 0.f;
        for (int j = 0; j < n; ++j, ph += step) {
            float s0 = x[j] * spec_hann[(int)ph] + c * s1 - s2;
            s2 = s1; s1 = s0;
        }
        float pw = s1 * s1 + s2 * s2 - c * s1 * s2, amp = 4.f * nb_sqrt(pw > 0.f ? pw : 0.f) / (float)n;   /* Hann: N/2 */
        float db = amp > 1e-5f ? 6.0206f * nb_log2(amp) : -90.f;
        v->spec[k] = (int8_t)nb_clamp(db + (db < 0.f ? -.5f : .5f), -90.f, 14.f);
    }
    v->spec_on = 1;
}

void nb_engine_view(const nb_engine *e, const nb_params *p, nb_view *v)
{
    const nb_fstate *z = &e->f;
    for (int c = 0; c < NB_CH; ++c) {
        v->level[c] = nb_sqrt(z->m_lv[c]) * z->gate * p->ch_level[c];
        v->heat[c] = nb_clamp(z->m_heat[c] * 4.f, 0.f, 1.f);
        v->period[c] = z->per[c];
        for (int j = 0; j < NB_CH; ++j) {
            v->flow[c][j] = j == c ? 0.f : nb_sqrt(z->m_flow[c][j]);
            v->send[c][j] = j == c ? 0.f : p->matrix[c][j] * z->cross_s;
        }
    }
    int on = 0;
    for (int f = 0; f < NB_NLOOPFX; ++f) if (p->fx[f][3] > 0.f) ++on;
    v->fx_on = (float)on / (float)NB_NLOOPFX;
    v->wet = nb_clamp(p->fx[NB_FX_DELAY][3] + p->fx[NB_FX_REVERB][3], 0.f, 1.f);
    v->gate = z->gate;
    v->ret = z->ret;
    v->struck = 0;
    for (int c = 0; c < NB_CH; ++c) if (e->kick_left[c] > 0) v->struck |= (uint8_t)(1u << c);
    v->out = nb_sqrt(z->m_out);
    v->limit = z->env > 0.7f ? 0.7f / z->env : 1.f;
    v->time = e->time;
    /* the screen's views (docs/TELA.md 13): each loop in the network, each stage's work, each loop's last periods */
    for (int c = 0; c < NB_CH; ++c) {
        v->loud[c] = nb_sqrt(z->m_lv[c]);
        for (int f = 0; f < NB_NLOOPFX; ++f) {
            float r = (p->fx[f][3] > 0.f && z->m_si[c][f] > 1e-9f) ? nb_sqrt(z->m_sd[c][f] / z->m_si[c][f]) : 0.f;
            v->stage[c][f] = (uint8_t)(nb_clamp(r, 0.f, 2.f) * 127.5f);   /* 0..2: a stage can add more than it gets */
        }
        if (c == 0) v->wave_free = 0;
        wave_of(e, c, v);
    }
    scope_of(e, p, v);
    spectrum_of(e, p, v);
    v->bend = e->f.m_bend > 0.f ? e->f.m_bend : 1.f;
}
