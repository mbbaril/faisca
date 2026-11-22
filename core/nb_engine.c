/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Mario Baril
 * The loop network. Per channel and sample:
 *   in  = return x (GAIN x own line + CROSS x the matrix of the other lines) + strike + a noise floor
 *   pre = the preamp (a soft knee that DRIVE hardens), DC blocked
 *   eq  = flat .. a unity-peak band-pass at the channel's partial (FOCUS)
 *   then the fixed chain, each effect mixed in by its MIX (at 0 it is skipped):
 *   FUZZ > BLAST > FOLD > WRAP > RECTIFY > DIVIDE > RING > STARVE > SHIFT > ECHO > PUMP > SWEEP > the line
 * SCREAM bends every read by the loop's own output. The output: 1+3 left, 2+4 right, the gate, the post DELAY and
 * REVERB, the master, and a limiter that is never off.
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
    e->k = 1.f;
    for (int c = 0; c < NB_CH; ++c) { e->per[c] = 200.f; e->g[c] = 0.1f; e->sh_c[c] = 1.f; }
}

/* The preamp: small-signal gain 1, a knee from tanh (drive 0) toward a hard clip (drive 1). */
NB_INLINE float preamp(float x, float drive)
{
    float s = nb_tanh(x);
    float h = nb_clamp(x, -1.f, 1.f);
    return s + (h - s) * drive;
}

NB_INLINE float read_line(const int16_t *l, uint32_t wp, float d)
{
    if (d < 2.f) d = 2.f;
    if (d > (float)(NB_LINE - 3)) d = (float)(NB_LINE - 3);
    int di = (int)d;
    float f = d - (float)di;
    uint32_t i0 = (wp - (uint32_t)di) & (NB_LINE - 1);
    uint32_t i1 = (i0 - 1u) & (NB_LINE - 1);
    float a = (float)l[i0], b = (float)l[i1];
    return (a + (b - a) * f) * (1.f / 32767.f);
}

/* an int16 line of any length, read d samples back with a linear tap; wraps by comparison, no division */
NB_INLINE float rd16(const int16_t *l, uint32_t wp, uint32_t len, float d)
{
    if (d < 1.f) d = 1.f;
    if (d > (float)(len - 2)) d = (float)(len - 2);
    uint32_t di = (uint32_t)(int)d;
    float f = d - (float)di;
    uint32_t i0 = wp >= di ? wp - di : wp + len - di;
    uint32_t i1 = i0 ? i0 - 1u : len - 1u;
    return ((float)l[i0] + ((float)l[i1] - (float)l[i0]) * f) * (1.f / 32767.f);
}
NB_INLINE int16_t to16(float x) { return (int16_t)(nb_clamp(x, -1.f, 1.f) * 32767.f); }

/* a one-pole low-pass coefficient for fc Hz */
NB_INLINE float lp_coef(float fc, float fs) { return 1.f - nb_exp2(-2.f * NB_PI * fc / fs * 1.4427f); }

/* one second-order allpass of SHIFT's Hilbert pair: y = a2 (x + y[n-2]) - x[n-2]; s = {x1, x2, y1, y2} */
NB_INLINE float hb_ap(float x, float a2, float *s)
{
    float y = a2 * (x + s[3]) - s[1];
    s[1] = s[0]; s[0] = x;
    s[3] = s[2]; s[2] = y;
    return y;
}

/* ---- what the knobs mean now: once per sub-block (exp2, tan) so the sample loop only multiplies ---- */
NB_INLINE void fx_coefficients(nb_engine *e)
{
    const float fs = e->fs;
    float (*s)[4] = e->fx_s;                                                /* smoothed in coefficients() */
    float (*q)[4] = e->coef;
    /* FUZZ: gain up to +54 dB, the tone after it, the bias (asymmetry) */
    q[NB_FX_FUZZ][0] = 2.f * nb_exp2(s[NB_FX_FUZZ][0] * 9.f);
    q[NB_FX_FUZZ][1] = lp_coef(600.f * nb_exp2(s[NB_FX_FUZZ][1] * 4.5f), fs);
    q[NB_FX_FUZZ][2] = s[NB_FX_FUZZ][2] * 0.8f;
    /* BLAST: gain up to +60 dB, the tone after, the high lift before */
    q[NB_FX_BLAST][0] = 2.f * nb_exp2(s[NB_FX_BLAST][0] * 10.f);
    q[NB_FX_BLAST][1] = lp_coef(500.f * nb_exp2(s[NB_FX_BLAST][1] * 4.7f), fs);
    q[NB_FX_BLAST][2] = s[NB_FX_BLAST][2] * 0.92f;
    /* FOLD: depth +0..+24 dB into the fold, a bias, a softening after */
    q[NB_FX_FOLD][0] = 1.f + 15.f * s[NB_FX_FOLD][0];
    q[NB_FX_FOLD][1] = (s[NB_FX_FOLD][1] - 0.5f) * 1.2f;
    q[NB_FX_FOLD][2] = 1.f - 0.92f * s[NB_FX_FOLD][2];
    /* WRAP: depth, bias, smoothing */
    q[NB_FX_WRAP][0] = 1.f + 7.f * s[NB_FX_WRAP][0];
    q[NB_FX_WRAP][1] = (s[NB_FX_WRAP][1] - 0.5f) * 1.2f;
    q[NB_FX_WRAP][2] = 1.f - 0.92f * s[NB_FX_WRAP][2];
    /* RECTIFY: full .. half wave, a threshold, the tone */
    q[NB_FX_RECTIFY][1] = s[NB_FX_RECTIFY][1] * 0.6f;
    q[NB_FX_RECTIFY][2] = lp_coef(300.f * nb_exp2(s[NB_FX_RECTIFY][2] * 5.5f), fs);
    /* DIVIDE: the tone of the squares */
    q[NB_FX_DIVIDE][2] = lp_coef(200.f * nb_exp2(s[NB_FX_DIVIDE][2] * 6.f), fs);
    /* RING: the own carrier's step (20 Hz .. 2.5 kHz) */
    q[NB_FX_RING][1] = 2.f * NB_PI * 20.f * nb_exp2(s[NB_FX_RING][1] * 7.f) / fs;
    /* STARVE: the sag, the dead zone, how fast the supply recovers (5 .. 300 ms) */
    q[NB_FX_STARVE][0] = 0.85f * s[NB_FX_STARVE][0];
    q[NB_FX_STARVE][1] = 0.3f * s[NB_FX_STARVE][1] * s[NB_FX_STARVE][1];
    q[NB_FX_STARVE][2] = lp_coef(1.f / (0.005f + 0.295f * s[NB_FX_STARVE][2]) / (2.f * NB_PI), fs);
    /* SHIFT: +-400 Hz, finer near zero; each loop apart by SPREAD; its own feedback */
    {
        float v = (s[NB_FX_SHIFT][0] - 0.5f) * 2.f;
        float hz = 400.f * v * nb_absf(v), sp = s[NB_FX_SHIFT][1];
        for (int c = 0; c < NB_CH; ++c) {
            float off = c == 0 ? 0.f : c == 1 ? 0.37f : c == 2 ? -0.29f : 0.61f;
            float w = 2.f * NB_PI * (hz + sp * 40.f * off * (1.f + nb_absf(v) * 3.f)) / fs, w2 = w * w;
            e->sh_dc[c] = 1.f - w2 * 0.5f + w2 * w2 * (1.f / 24.f);
            e->sh_ds[c] = w - w * w2 * (1.f / 6.f);
            float m = nb_sqrt(e->sh_c[c] * e->sh_c[c] + e->sh_s[c] * e->sh_s[c]);
            if (m > 0.f) { e->sh_c[c] /= m; e->sh_s[c] /= m; } else { e->sh_c[c] = 1.f; e->sh_s[c] = 0.f; }
        }
        q[NB_FX_SHIFT][2] = 0.85f * s[NB_FX_SHIFT][2];
    }
    /* ECHO: 2 .. 27 ms, its feedback, the wow */
    q[NB_FX_ECHO][0] = (0.002f + 0.025f * s[NB_FX_ECHO][0]) * fs;
    q[NB_FX_ECHO][1] = 0.9f * s[NB_FX_ECHO][1];
    q[NB_FX_ECHO][2] = 0.02f * s[NB_FX_ECHO][2];
    /* PUMP: depth (the ratio), attack 0.2 .. 20 ms, release 20 .. 800 ms */
    q[NB_FX_PUMP][0] = 30.f * s[NB_FX_PUMP][0];
    q[NB_FX_PUMP][1] = lp_coef(1.f / (0.0002f + 0.02f * s[NB_FX_PUMP][1]) / (2.f * NB_PI), fs);
    q[NB_FX_PUMP][2] = lp_coef(1.f / (0.02f + 0.78f * s[NB_FX_PUMP][2]) / (2.f * NB_PI), fs);
    /* SWEEP: how far the level moves it, its resting cutoff (80 Hz .. 2 kHz), resonance */
    q[NB_FX_SWEEP][0] = 1.f + 11.f * s[NB_FX_SWEEP][0];
    q[NB_FX_SWEEP][1] = 80.f * nb_exp2(s[NB_FX_SWEEP][1] * 4.6f);
    q[NB_FX_SWEEP][2] = 1.f - 0.93f * s[NB_FX_SWEEP][2];
    /* DELAY: 10 .. 120 ms, feedback, the tone of its repeats */
    q[NB_FX_DELAY][0] = (0.010f + 0.110f * s[NB_FX_DELAY][0]) * fs;
    q[NB_FX_DELAY][1] = 0.9f * s[NB_FX_DELAY][1];
    q[NB_FX_DELAY][2] = lp_coef(800.f * nb_exp2((1.f - s[NB_FX_DELAY][2]) * 4.f), fs);
    /* REVERB: the decay, the damping, the width */
    q[NB_FX_REVERB][0] = 0.45f + 0.5f * s[NB_FX_REVERB][0];
    q[NB_FX_REVERB][1] = 0.85f - 0.7f * s[NB_FX_REVERB][1];
    q[NB_FX_REVERB][2] = s[NB_FX_REVERB][2];
}

NB_INLINE void coefficients(nb_engine *e, const nb_params *p)
{
    float fs = e->fs, nyq = 0.4f * fs;
    /* every knob glides to where it was turned, about 15 ms, as a pot through an RC: a turn is a slide, never a
     * staircase, whatever the encoder's step (one factor per sub-block of NB_BLOCK samples) */
    const float a = 1.f - nb_exp2(-(float)NB_BLOCK / (0.015f * fs) * 1.4427f);
    for (int f = 0; f < NB_NFX; ++f)
        for (int k = 0; k < 4; ++k) e->fx_s[f][k] += (p->fx[f][k] - e->fx_s[f][k]) * a;
    fx_coefficients(e);
    if (e->mid_s <= 0.f) e->mid_s = p->mid_hz;
    e->mid_s += (p->mid_hz - e->mid_s) * (a * 0.5f);   /* MID, a tuning pot: twice as slow, about 30 ms */
    e->focus_s += (p->focus - e->focus_s) * a;
    e->hiss_s += (p->hiss - e->hiss_s) * a;
    for (int c = 0; c < NB_CH; ++c) {
        e->ratio_s[c] += (p->ratio[c] - e->ratio_s[c]) * a;
        e->pitch_s[c] += (p->ch_pitch[c] - e->pitch_s[c]) * a;
        e->chmid_s[c] += (p->ch_mid[c] - e->chmid_s[c]) * a;
        e->chg_s[c] += (p->ch_gain[c] - e->chg_s[c]) * a;
        e->chl_s[c] += (p->ch_level[c] - e->chl_s[c]) * a;
        for (int j = 0; j < NB_CH; ++j) e->mtx_s[c][j] += (p->matrix[c][j] - e->mtx_s[c][j]) * a;
    }
    for (int c = 0; c < NB_CH; ++c) {
        float f = e->mid_s * e->ratio_s[c] * nb_exp2(e->pitch_s[c] * (1.f / 12.f));
        if (f < 20.f) f = 20.f;
        float fb = f * nb_exp2(e->chmid_s[c]);
        if (fb > nyq) fb = nyq;
        if (fb < 20.f) fb = 20.f;
        e->g[c] = nb_tan(NB_PI * fb / fs);
        /* the loop tuned to the partial: its period, raised an octave until it fits the line */
        float per = fs / f;
        while (per > (float)(NB_LINE - 8)) per *= 0.5f;
        per -= 1.f;                                    /* the filter's own sample */
        if (per < 2.f) per = 2.f;
        e->per[c] += (per - e->per[c]) * 0.25f;        /* a glide over a few sub-blocks */
    }
    float fo = e->focus_s;
    e->mix = nb_clamp(fo * 4.f, 0.f, 1.f);
    float q = 0.6f * nb_exp2(fo * 5.6f);               /* 0.6 .. 29 */
    e->k = 1.f / q;
}

/* ---- the fixed chain, one loop, one sample ---- */
NB_INLINE float chain(nb_engine *e, int c, float x, float neighbour, float ring_car)
{
    float (*s)[4] = e->fx_s;
    float (*q)[4] = e->coef;
    float m;

    if ((m = s[NB_FX_FUZZ][3]) > 1e-4f) {          /* two stages, the second asymmetric like mismatched diodes */
        float b = q[NB_FX_FUZZ][2];
        float a = nb_tanh(x * q[NB_FX_FUZZ][0] * 0.5f + b) - nb_tanh(b);
        float u = a * 2.f;
        float y = u > 0.f ? nb_tanh(u) : (1.f - 0.6f * b) * nb_tanh(u * (1.f + b));
        e->fz_lp[c] += (y - e->fz_lp[c]) * q[NB_FX_FUZZ][1];
        float dc = e->fz_lp[c] - e->fz_dc[c] + 0.995f * e->fz_dcy[c];
        e->fz_dc[c] = e->fz_lp[c]; e->fz_dcy[c] = dc;
        x += (dc * 0.95f - x) * m;
    }
    if ((m = s[NB_FX_BLAST][3]) > 1e-4f) {         /* an op-amp rail to rail, the highs lifted before */
        float pre = x - q[NB_FX_BLAST][2] * e->bl_pre[c];
        e->bl_pre[c] = x;
        float u = nb_clamp(pre * q[NB_FX_BLAST][0], -1.f, 1.f);
        e->bl_lp[c] += (u - e->bl_lp[c]) * q[NB_FX_BLAST][1];
        x += (e->bl_lp[c] * 0.9f - x) * m;
    }
    if ((m = s[NB_FX_FOLD][3]) > 1e-4f) {          /* a triangle fold, biased, then softened */
        float u = ((x + q[NB_FX_FOLD][1]) * q[NB_FX_FOLD][0] + 1.f) * 0.25f + 1000.f;
        u -= (float)(int)u;
        float y = u < 0.5f ? u * 4.f - 1.f : 3.f - u * 4.f;
        e->fo_lp[c] += (y - e->fo_lp[c]) * q[NB_FX_FOLD][2];
        x += (e->fo_lp[c] - x) * m;
    }
    if ((m = s[NB_FX_WRAP][3]) > 1e-4f) {          /* past the edge, the other side */
        float u = ((x + q[NB_FX_WRAP][1]) * q[NB_FX_WRAP][0] + 1.f) * 0.5f + 1000.f;
        u -= (float)(int)u;
        float y = u * 2.f - 1.f;
        e->wr_lp[c] += (y - e->wr_lp[c]) * q[NB_FX_WRAP][2];
        x += (e->wr_lp[c] - x) * m;
    }
    float ax = nb_absf(x);
    float env = e->lvl_env[c];
    env += (ax - env) * (ax > env ? 0.5f : 0.0008f);
    e->lvl_env[c] = env;
    if ((m = s[NB_FX_RECTIFY][3]) > 1e-4f) {       /* full wave (the octave up) .. half wave, over a threshold */
        float t = q[NB_FX_RECTIFY][1], h = s[NB_FX_RECTIFY][0];
        float full = ax > t ? ax - t : 0.f, half = x > t ? x - t : 0.f;
        float r = (full + (half - full) * h) * 2.f - (1.27f - 0.6f * h) * env;
        e->rc_lp[c] += (r - e->rc_lp[c]) * q[NB_FX_RECTIFY][2];
        x += (e->rc_lp[c] - x) * m;
    }
    if ((m = s[NB_FX_DIVIDE][3]) > 1e-4f) {        /* flip-flops: an octave and two below */
        int up = x > 0.f;
        if (up && !e->div_prev[c]) {
            e->div_q[c] = !e->div_q[c];
            if (e->div_q[c]) e->div_q2[c] = !e->div_q2[c];
        }
        e->div_prev[c] = up;
        float s1 = e->div_q[c] ? 1.f : -1.f, s2 = e->div_q2[c] ? 1.f : -1.f;
        float oct = s1 + (s2 - s1) * s[NB_FX_DIVIDE][0];
        float pulse = (e->div_q[c] && e->div_q2[c]) ? 1.f : -1.f;            /* WIDTH: toward a 25 % pulse */
        float sq = (oct + (pulse - oct) * s[NB_FX_DIVIDE][1]) * env * 1.4f;
        e->dv_lp[c] += (sq - e->dv_lp[c]) * q[NB_FX_DIVIDE][2];
        x += (e->dv_lp[c] - x) * m;
    }
    if ((m = s[NB_FX_RING][3]) > 1e-4f) {          /* the neighbour loop .. its own carrier; ring .. AM */
        float car = neighbour + (ring_car - neighbour) * s[NB_FX_RING][0];
        float dc = s[NB_FX_RING][2];
        float r = x * (car * (1.f - 0.5f * dc) + dc) * 2.f;
        x += (r - x) * m;
    }
    if ((m = s[NB_FX_STARVE][3]) > 1e-4f) {        /* a weak supply: the headroom sags with the level, small signals die */
        float se = e->st_env[c];
        se += (ax - se) * (ax > se ? 0.5f : q[NB_FX_STARVE][2]);
        e->st_env[c] = se;
        float head = 1.f - q[NB_FX_STARVE][0] * nb_clamp(se * 1.5f, 0.f, 1.f);
        float dead = q[NB_FX_STARVE][1];
        float y = ax <= dead ? 0.f : (x > 0.f ? x - dead : x + dead) / (1.f - dead);
        x += (nb_clamp(y, -head, head) - x) * m;
    }
    if ((m = s[NB_FX_SHIFT][3]) > 1e-4f) {         /* a frequency shifter: the analytic signal times a turning phasor */
        float *h = e->hb[c];
        float in = x + q[NB_FX_SHIFT][2] * e->sh_fb[c];
        float a = in;
        a = hb_ap(a, 0.6923878f * 0.6923878f, &h[0]);
        a = hb_ap(a, 0.9360654f * 0.9360654f, &h[4]);
        a = hb_ap(a, 0.9882295f * 0.9882295f, &h[8]);
        a = hb_ap(a, 0.9987488f * 0.9987488f, &h[12]);
        float b = e->hb_z[c];
        e->hb_z[c] = in;
        b = hb_ap(b, 0.4021921f * 0.4021921f, &h[16]);
        b = hb_ap(b, 0.8561711f * 0.8561711f, &h[20]);
        b = hb_ap(b, 0.9722910f * 0.9722910f, &h[24]);
        b = hb_ap(b, 0.9952885f * 0.9952885f, &h[28]);
        float y = nb_clamp(a * e->sh_c[c] - b * e->sh_s[c], -1.5f, 1.5f);
        if (nb_isbad(y)) {                         /* through volatile: a plain loop would become a memset call */
            volatile float *z = h;
            for (int k = 0; k < 32; ++k) z[k] = 0.f;
            y = 0.f;
        }
        e->sh_fb[c] = y;
        x += (y - x) * m;
    }
    if ((m = s[NB_FX_ECHO][3]) > 1e-4f) {          /* a short echo in each loop, the four a little apart, wow */
        float spread = c == 0 ? 1.f : c == 1 ? 1.13f : c == 2 ? 0.87f : 1.27f;
        float d = q[NB_FX_ECHO][0] * spread * (1.f + q[NB_FX_ECHO][2] * nb_sin(e->echo_ph + (float)c));
        float r = rd16(e->echo[c], e->echo_wp, NB_ECHO, d);
        e->echo[c][e->echo_wp] = to16(x + q[NB_FX_ECHO][1] * r);
        x += r * m;                                   /* the repeats added to the loop */
    }
    if ((m = s[NB_FX_PUMP][3]) > 1e-4f) {          /* a compressor in the loop: loud, it ducks; then it swells back */
        float ax2 = nb_absf(x), pe = e->pump_env[c];
        pe += (ax2 - pe) * (ax2 > pe ? q[NB_FX_PUMP][1] : q[NB_FX_PUMP][2]);
        e->pump_env[c] = pe;
        float k = q[NB_FX_PUMP][0];
        float y = x * (1.f + 0.3f * k) / (1.f + k * pe);
        x += (y - x) * m;
    }
    if ((m = s[NB_FX_SWEEP][3]) > 1e-4f) {         /* a band-pass the loop's own level moves */
        float ax3 = nb_absf(x), sw = e->sw_env[c];
        sw += (ax3 - sw) * (ax3 > sw ? 0.02f : 0.0015f);
        e->sw_env[c] = sw;
        float fc = q[NB_FX_SWEEP][1] * nb_exp2(nb_clamp(sw * q[NB_FX_SWEEP][0], 0.f, 6.f));
        float g = NB_PI * fc / e->fs;                  /* tan(x) ~ x below 0.15 fs: no tan per sample */
        if (g > 0.9f) g = 0.9f;
        float k = q[NB_FX_SWEEP][2];
        float hp = (x - (k + g) * e->sw1[c] - e->sw2[c]) / (1.f + g * (k + g));
        float bp = g * hp + e->sw1[c];
        e->sw1[c] = g * hp + bp;
        float lp = g * bp + e->sw2[c];
        e->sw2[c] = g * bp + lp;
        if (nb_isbad(bp)) { e->sw1[c] = e->sw2[c] = 0.f; bp = 0.f; }
        x += (bp * (k + 0.3f) * 2.f - x) * m;
    }
    return x;
}

/* ---- after the network: a short ping-pong DELAY, then a small plate REVERB ---- */
NB_INLINE void post_fx(nb_engine *e, float *L, float *R)
{
    float (*s)[4] = e->fx_s;
    float (*q)[4] = e->coef;
    float dm = s[NB_FX_DELAY][3], rm = s[NB_FX_REVERB][3];
    if (dm > 1e-4f) {
        float d = q[NB_FX_DELAY][0];
        float dl = rd16(e->pdl[0], e->pdl_wp, NB_PDLY, d), dr = rd16(e->pdl[1], e->pdl_wp, NB_PDLY, d);
        float fb = q[NB_FX_DELAY][1], tc = q[NB_FX_DELAY][2];
        e->pdl_lp[0] += (dr - e->pdl_lp[0]) * tc;        /* each side feeds the other: ping-pong, its tone */
        e->pdl_lp[1] += (dl - e->pdl_lp[1]) * tc;
        e->pdl[0][e->pdl_wp] = to16(*L + fb * e->pdl_lp[0]);
        e->pdl[1][e->pdl_wp] = to16(*R + fb * e->pdl_lp[1]);
        if (++e->pdl_wp >= NB_PDLY) e->pdl_wp = 0;
        *L += dm * dl;
        *R += dm * dr;
    }
    if (rm > 1e-4f) {
        /* four lines (601, 743, 919, 1097 samples) mixed by a Hadamard matrix, each damped */
        uint32_t w = e->fdn_wp;
        float o0 = (float)e->fdn[0][w >= 601u ? w - 601u : w + NB_FDN - 601u] * (1.f / 32767.f);
        float o1 = (float)e->fdn[1][w >= 743u ? w - 743u : w + NB_FDN - 743u] * (1.f / 32767.f);
        float o2 = (float)e->fdn[2][w >= 919u ? w - 919u : w + NB_FDN - 919u] * (1.f / 32767.f);
        float o3 = (float)e->fdn[3][w >= 1097u ? w - 1097u : w + NB_FDN - 1097u] * (1.f / 32767.f);
        float g = q[NB_FX_REVERB][0], damp = q[NB_FX_REVERB][1], wd = q[NB_FX_REVERB][2];
        float in = 0.35f * (*L + *R);
        e->fdn_lp[0] += ((o0 + o1 + o2 + o3) * 0.5f * g - e->fdn_lp[0]) * damp;
        e->fdn_lp[1] += ((o0 - o1 + o2 - o3) * 0.5f * g - e->fdn_lp[1]) * damp;
        e->fdn_lp[2] += ((o0 + o1 - o2 - o3) * 0.5f * g - e->fdn_lp[2]) * damp;
        e->fdn_lp[3] += ((o0 - o1 - o2 + o3) * 0.5f * g - e->fdn_lp[3]) * damp;
        for (int k = 0; k < 4; ++k) e->fdn[k][w] = to16(in + e->fdn_lp[k]);
        if (++e->fdn_wp >= NB_FDN) e->fdn_wp = 0;
        float mid = 0.35f * (o0 + o1 + o2 + o3), side = 0.7f * (o0 + o2 - o1 - o3);
        *L += rm * (mid + side * wd);
        *R += rm * (mid - side * wd);
    }
}

NB_AUDIO_CODE void nb_engine_render(nb_engine *e, const nb_params *p, nb_touch *t, float *out, int n)
{
    const float fs = e->fs;
    const float sm = 1.f - nb_exp2(-1.f / (0.010f * fs) * 1.4427f);   /* 10 ms smoothing */
    const float gate_step = 1.f / (0.0005f * fs);                     /* 0.5 ms gate ramp */
    const float ret_step = 1.f / (0.060f * fs);                       /* the return closes in 60 ms */
    const float rel = nb_exp2(-1.f / (0.150f * fs) * 1.4427f);        /* limiter release */
    const float lv_a = 1.f - nb_exp2(-1.f / (0.040f * fs) * 1.4427f); /* meters, 40 ms */

    /* the strike: one-shot, read once */
    if (t->kick_mask) {
        int len = (int)((0.008f - 0.007f * t->kick_color) * fs);
        for (int c = 0; c < NB_CH; ++c)
            if (t->kick_mask & (1u << c)) e->kick_left[c] = len;
        e->kick_amp = 0.9f;
        e->kick_a = 0.05f + 0.9f * t->kick_color;                     /* noise colour */
        e->kick_dec = nb_exp2(-1.f / ((float)len * 0.35f) * 1.4427f);
        t->kick_mask = 0;
    }
    e->ret_target = (t->hold && !t->ret_cut) ? 1.f : 0.f;

    for (int i = 0; i < n; ++i) {
        if ((e->time & (NB_BLOCK - 1)) == 0) coefficients(e, p);
        e->gain_s += (p->gain - e->gain_s) * sm;
        e->cross_s += (p->cross - e->cross_s) * sm;
        e->drive_s += (p->drive - e->drive_s) * sm;
        e->grito_s += (p->grito - e->grito_s) * sm;
        e->level_s += (p->level - e->level_s) * sm;
        {   /* SHIFT's oscillators turn one step; ECHO's wow; RING's own carrier */
            for (int c = 0; c < NB_CH; ++c) {
                float nc = e->sh_c[c] * e->sh_dc[c] - e->sh_s[c] * e->sh_ds[c];
                e->sh_s[c] = e->sh_c[c] * e->sh_ds[c] + e->sh_s[c] * e->sh_dc[c];
                e->sh_c[c] = nc;
            }
            e->echo_ph += 2.f * NB_PI * 0.6f / fs;
            if (e->echo_ph > NB_PI) e->echo_ph -= 2.f * NB_PI;
            e->rg_ph += e->coef[NB_FX_RING][1];
            if (e->rg_ph > NB_PI) e->rg_ph -= 2.f * NB_PI;
        }
        float ring_car = nb_sin(e->rg_ph);

        /* the return gate */
        if (e->ret < e->ret_target) { e->ret += ret_step * 4.f; if (e->ret > 1.f) e->ret = 1.f; }
        else if (e->ret > e->ret_target) { e->ret -= ret_step; if (e->ret < 0.f) e->ret = 0.f; }

        float cr = e->cross_s * e->ret;
        /* SCREAM: the loop's output bends every read (phase feedback on top of the signal feedback) */
        float bend = nb_exp2(e->grito_s * e->grito_s * 3.f * e->out_prev);

        float fbk[NB_CH];
        for (int c = 0; c < NB_CH; ++c) fbk[c] = read_line(e->line[c], e->wp, e->per[c] * bend);
        float sumL = 0.f, sumR = 0.f, tot = 0.f;
        for (int c = 0; c < NB_CH; ++c) {
            /* DRIVE also multiplies how far each loop is above unity: a loop that sings hotter */
            float gc = e->gain_s * e->chg_s[c];
            if (gc > 1.f) gc = 1.f + (gc - 1.f) * (1.f + 3.f * e->drive_s);
            float in = gc * e->ret * fbk[c];
            for (int j = 0; j < NB_CH; ++j) {
                if (j == c) continue;
                float m = e->mtx_s[c][j];
                if (m > -1e-5f && m < 1e-5f) continue;
                float s = cr * m * fbk[j];
                in += s;
                e->m_flow[c][j] += (s * s - e->m_flow[c][j]) * lv_a;
            }
            /* the strike and the floor every circuit has */
            float nz = nb_noise(&e->rng);
            in += (0.00003f + 0.02f * e->hiss_s * e->hiss_s) * nz * e->ret;
            if (e->kick_left[c] > 0) {
                e->kick_lp[c] += (nz - e->kick_lp[c]) * e->kick_a;
                in += e->kick_lp[c] * e->kick_amp * (1.f + 2.f * (1.f - e->kick_a));
                --e->kick_left[c];
            }
            float pre = preamp(in, e->drive_s);
            float hv = nb_absf(in) - nb_absf(pre);
            e->m_heat[c] += ((hv > 0.f ? hv : 0.f) - e->m_heat[c]) * lv_a;
            /* DC block, ~16 Hz */
            float dc = pre - e->dc_x[c] + 0.9977f * e->dc_y[c];
            e->dc_x[c] = pre; e->dc_y[c] = dc;
            /* band-pass, TPT state variable, unity at the peak */
            float gg = e->g[c], k = e->k;
            float hp = (dc - (k + gg) * e->s1[c] - e->s2[c]) / (1.f + gg * (k + gg));
            float bp = gg * hp + e->s1[c];
            e->s1[c] = gg * hp + bp;
            float lp = gg * bp + e->s2[c];
            e->s2[c] = gg * bp + lp;
            float eq = dc + (bp * k - dc) * e->mix;
            float y = chain(e, c, eq, fbk[(c + 1) & 3], ring_car);
            y = nb_clamp(y, -1.f, 1.f);
            if (nb_isbad(y) || nb_isbad(e->s1[c]) || nb_isbad(e->s2[c])) {
                y = 0.f; e->s1[c] = e->s2[c] = e->dc_x[c] = e->dc_y[c] = 0.f;
            }
            e->line[c][e->wp & (NB_LINE - 1)] = (int16_t)(y * 32767.f);
            e->y[c] = y;
            e->m_lv[c] += (y * y - e->m_lv[c]) * lv_a;
            if (c & 1) sumR += y * e->chl_s[c]; else sumL += y * e->chl_s[c];
            tot += y;
        }
        e->wp = (e->wp + 1u) & (NB_LINE - 1);
        if (++e->echo_wp >= NB_ECHO) e->echo_wp = 0;
        e->out_prev = tot * 0.25f;

        /* the output gate: CUT, CHOP (the loop's own rhythm: rising crossings), SCATTER (random cuts) */
        int open = !t->out_cut;
        if (t->chop) {
            int above = e->out_prev > 0.f;
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
        if (e->gate < e->gate_target) { e->gate += gate_step; if (e->gate > 1.f) e->gate = 1.f; }
        else if (e->gate > e->gate_target) { e->gate -= gate_step; if (e->gate < 0.f) e->gate = 0.f; }

        if (e->kick_amp > 0.f) e->kick_amp *= e->kick_dec;

        float L = sumL * 0.45f * e->gate, R = sumR * 0.45f * e->gate;
        post_fx(e, &L, &R);                                      /* the tails ring on after a CUT */
        float lvl = e->level_s * e->level_s;
        L *= lvl; R *= lvl;
        /* the limiter: instant attack, 150 ms release, to -3 dBFS, then a soft ceiling */
        float pk = nb_absf(L) > nb_absf(R) ? nb_absf(L) : nb_absf(R);
        e->env = pk > e->env ? pk : e->env * rel;
        float gr = e->env > 0.7f ? 0.7f / e->env : 1.f;
        L *= gr; R *= gr;
        L = 0.98f * nb_tanh(L * 1.0204f);
        R = 0.98f * nb_tanh(R * 1.0204f);
        out[2 * i] = L; out[2 * i + 1] = R;
        e->m_out += (0.5f * (L * L + R * R) - e->m_out) * lv_a;
        e->time++;
    }
}

void nb_engine_view(const nb_engine *e, const nb_params *p, nb_view *v)
{
    for (int c = 0; c < NB_CH; ++c) {
        v->level[c] = nb_sqrt(e->m_lv[c]) * e->gate * p->ch_level[c];
        v->heat[c] = nb_clamp(e->m_heat[c] * 4.f, 0.f, 1.f);
        v->period[c] = e->per[c];
        for (int j = 0; j < NB_CH; ++j) {
            v->flow[c][j] = j == c ? 0.f : nb_sqrt(e->m_flow[c][j]);
            v->send[c][j] = j == c ? 0.f : p->matrix[c][j] * e->cross_s;
        }
    }
    int on = 0;
    for (int f = 0; f < NB_NLOOPFX; ++f) if (p->fx[f][3] > 0.f) ++on;
    v->fx_on = (float)on / (float)NB_NLOOPFX;
    v->wet = nb_clamp(p->fx[NB_FX_DELAY][3] + p->fx[NB_FX_REVERB][3], 0.f, 1.f);
    v->gate = e->gate;
    v->out = nb_sqrt(e->m_out);
    v->limit = e->env > 0.7f ? 0.7f / e->env : 1.f;
    v->time = e->time;
}
