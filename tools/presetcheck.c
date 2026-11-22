/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Mario Baril
 * presetcheck: every factory memory has to sing on its own. For each one, from power-on: MEMORY + its white key,
 * then DRONE (PLAY) latched, no pad struck; 8 s rendered offline (nothing is played aloud) at the emulator's level
 * (the pot at 700 of 1023). Over the last 6 s: the RMS of each 0.5 s window, the peak, the spectral centroid.
 * PASS: every window above -30 dBFS and within 9 dB of the median window (sustained, neither dying nor pulsing
 * away), the peak under the limiter's ceiling (0.98).
 *   cc -O2 -o build/presetcheck tools/presetcheck.c core/nb_engine.c core/nb_panel.c -lm && build/presetcheck
 *   build/presetcheck -v        every window too
 *   build/presetcheck -w DIR    also writes DIR/NN_NAME.wav (16-bit stereo) to listen to later
 *   build/presetcheck -r 48000  another sample rate (the browser's)
 *   build/presetcheck -s        stress: each memory also at 48 kHz, recalled while DRONE already sings (no strike),
 *                               and with GAIN and MID turned a little either way; how many of the 8 runs pass
 * tests/smoke.c includes this file (PRESETCHECK_NO_MAIN) for T-09. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/nb_engine.h"
#include "../core/nb_panel.h"

#define PC_SECS 8.f
#define PC_SKIP 2.f             /* the first 2 s: the network finding its voice */
#define PC_WIN 0.5f
#define PC_NWIN 12              /* (PC_SECS - PC_SKIP) / PC_WIN */
#define PC_POT 700
#define PC_FLOOR_DB -30.f
#define PC_SPREAD_DB 9.f
#define PC_CEIL 0.98f

typedef struct {
    float win_db[PC_NWIN];
    float median_db, min_db, max_db, peak, centroid_hz;
    float flatness;             /* the spectrum's flatness, 0 a pure tone .. 1 white noise */
    float peaks_hz[4];          /* the strongest spectral peaks */
    float trem_db;              /* the 20 ms envelope's spread (p90 - p10), dB: how much it moves inside a window */
    float trem_hz;              /* its strongest movement rate, 0.5 .. 20 Hz */
    float corr;                 /* left-right correlation: 1 mono .. 0 wide */
    int pass;
} pc_result;

static nb_engine pc_e;
static nb_panel pc_p;
static float pc_ms;
static void (*pc_hook)(nb_panel *p, int i);   /* (a tool's) changes to the memories before the recall */
static int pc_switch;                          /* the memory recalled while memory 1 already drones */
static float pc_dgain, pc_dmid;                /* GAIN and MID moved from the memory's (stress) */

/* one control read and its block of audio; out may be 0 */
static void pc_block(nb_controls *c, float fs, float *out, int n)
{
    static float scratch[2 * 128];
    pc_ms += (float)n * 1000.f / fs;
    c->ms = (uint32_t)pc_ms;
    nb_panel_update(&pc_p, c);
    for (int k = 0; k < 7; ++k) c->enc[k] = 0;
    nb_engine_render(&pc_e, &pc_p.params, &pc_p.touch, out ? out : scratch, n);
    nb_view v;
    nb_engine_view(&pc_e, &pc_p.params, &v);
    c->follow = v.out;
}
static void pc_idle(nb_controls *c, float fs, float secs)
{
    int n = (int)(secs * fs);
    for (int t = 0; t < n; t += 128) pc_block(c, fs, 0, 128);
}

/* an in-place radix-2 FFT (n a power of two) */
static void pc_fft(float *re, float *im, int n)
{
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double a = -2.0 * 3.14159265358979 / len;
        float wr = (float)cos(a), wi = (float)sin(a);
        for (int i = 0; i < n; i += len) {
            float cr = 1.f, ci = 0.f;
            for (int k = 0; k < len / 2; ++k) {
                int u = i + k, v = u + len / 2;
                float tr = re[v] * cr - im[v] * ci, ti = re[v] * ci + im[v] * cr;
                re[v] = re[u] - tr; im[v] = im[u] - ti;
                re[u] += tr; im[u] += ti;
                float nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr; cr = nr;
            }
        }
    }
}

static int pc_cmp(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

/* memory i from power-on, DRONE latched; the whole render kept in *audio (malloc'd, the caller frees) if asked */
static void preset_check(int i, float fs, pc_result *r, float **audio)
{
    nb_controls c;
    memset(&c, 0, sizeof c);
    memset(r, 0, sizeof *r);
    c.pot = PC_POT;
    pc_ms = 0.f;
    nb_engine_init(&pc_e, fs);
    nb_panel_init(&pc_p);
    if (pc_hook) pc_hook(&pc_p, i);
    pc_p.snaps[i].pos[NB_K_GAIN] = fminf(fmaxf(pc_p.snaps[i].pos[NB_K_GAIN] + pc_dgain, 0.f), 1.f);
    pc_p.snaps[i].pos[NB_K_MID] = fminf(fmaxf(pc_p.snaps[i].pos[NB_K_MID] + pc_dmid, 0.f), 1.f);
    pc_idle(&c, fs, 0.05f);
    if (pc_switch) {                                                    /* DRONE on memory 1 first, 2 s */
        c.buttons = 1u << NB_B_DRONE; pc_idle(&c, fs, 0.02f);
        c.buttons = 0; pc_idle(&c, fs, 2.f);
    }
    static const uint8_t white[16] = { 0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26 };
    c.buttons = 1u << NB_B_MEMORY; pc_idle(&c, fs, 0.02f);              /* MEMORY held */
    c.keys = 1u << white[i]; pc_idle(&c, fs, 0.02f);                    /* + its white key */
    c.keys = 0; c.buttons = 0; pc_idle(&c, fs, 0.02f);
    if (!pc_switch) {
        c.buttons = 1u << NB_B_DRONE; pc_idle(&c, fs, 0.02f);           /* DRONE tapped: latched */
        c.buttons = 0;
    }

    int frames = (int)(PC_SECS * fs) / 128 * 128;
    float *lr = malloc(sizeof(float) * 2u * (size_t)frames);
    for (int t = 0; t < frames; t += 128) pc_block(&c, fs, lr + 2 * t, 128);

    int skip = (int)(PC_SKIP * fs), wlen = (int)(PC_WIN * fs);
    float sorted[PC_NWIN];
    for (int w = 0; w < PC_NWIN; ++w) {
        double acc = 0.0;
        for (int k = 0; k < wlen; ++k) {
            int at = skip + w * wlen + k;
            if (at >= frames) break;
            float L = lr[2 * at], R = lr[2 * at + 1];
            acc += 0.5 * ((double)L * L + (double)R * R);
        }
        double rms = sqrt(acc / wlen);
        r->win_db[w] = (float)(20.0 * log10(rms > 1e-9 ? rms : 1e-9));
        sorted[w] = r->win_db[w];
    }
    for (int t = skip; t < frames; ++t) {
        float a = fabsf(lr[2 * t]), b = fabsf(lr[2 * t + 1]);
        if (a > r->peak) r->peak = a;
        if (b > r->peak) r->peak = b;
    }
    qsort(sorted, PC_NWIN, sizeof(float), pc_cmp);
    r->median_db = 0.5f * (sorted[PC_NWIN / 2 - 1] + sorted[PC_NWIN / 2]);
    r->min_db = sorted[0]; r->max_db = sorted[PC_NWIN - 1];

    /* the centroid: Hann frames of 4096 over the last 6 s, the power spectra summed */
    enum { N = 4096 };
    static float re[N], im[N], pw[N / 2];
    memset(pw, 0, sizeof pw);
    for (int at = skip; at + N <= frames; at += N) {
        for (int k = 0; k < N; ++k) {
            float hw = 0.5f - 0.5f * cosf(2.f * 3.14159265f * (float)k / (float)N);
            re[k] = 0.5f * (lr[2 * (at + k)] + lr[2 * (at + k) + 1]) * hw;
            im[k] = 0.f;
        }
        pc_fft(re, im, N);
        for (int k = 1; k < N / 2; ++k) pw[k] += re[k] * re[k] + im[k] * im[k];
    }
    double num = 0.0, den = 0.0;
    for (int k = 1; k < N / 2; ++k) { num += (double)pw[k] * k * fs / N; den += pw[k]; }
    r->centroid_hz = den > 0.0 ? (float)(num / den) : 0.f;
    {   /* the flatness (geometric over arithmetic mean, 40 Hz .. 16 kHz) and the strongest peaks */
        double lg = 0.0, ar = 0.0; int m = 0;
        for (int k = 1; k < N / 2; ++k) {
            float hz = (float)k * fs / N;
            if (hz < 40.f || hz > 16000.f) continue;
            lg += log(pw[k] + 1e-20); ar += pw[k]; ++m;
        }
        r->flatness = m && ar > 0.0 ? (float)(exp(lg / m) / (ar / m)) : 0.f;
        float top = 0.f;                                      /* the peaks within 40 dB of the strongest */
        for (int q = 0; q < 4; ++q) {
            int best = -1;
            for (int k = 2; k < N / 2 - 1; ++k) {
                if (!(pw[k] > pw[k - 1] && pw[k] >= pw[k + 1])) continue;
                int near = 0;
                for (int z = 0; z < q; ++z) if (fabsf(r->peaks_hz[z] - (float)k * fs / N) < 3.f * fs / N) near = 1;
                if (near) continue;
                if (best < 0 || pw[k] > pw[best]) best = k;
            }
            r->peaks_hz[q] = best > 0 && (q == 0 || pw[best] > top * 1e-4f) ? (float)best * fs / N : 0.f;
            if (q == 0 && best > 0) top = pw[best];
        }
    }
    {   /* the envelope in 20 ms steps: its spread, and the rate it moves at (a DFT of the dB curve) */
        enum { NE = 300 };
        static float ev[NE], es[NE];
        int el = (int)(0.02f * fs), ne = 0;
        double sl = 0.0, sr = 0.0, slr = 0.0;
        for (int at = skip; at + el <= frames && ne < NE; at += el, ++ne) {
            double a = 0.0;
            for (int k = 0; k < el; ++k) {
                float L = lr[2 * (at + k)], R = lr[2 * (at + k) + 1];
                a += 0.5 * ((double)L * L + (double)R * R);
                sl += (double)L * L; sr += (double)R * R; slr += (double)L * R;
            }
            ev[ne] = es[ne] = (float)(10.0 * log10(a / el + 1e-18));
        }
        r->corr = sl > 0.0 && sr > 0.0 ? (float)(slr / sqrt(sl * sr)) : 1.f;
        qsort(es, (size_t)ne, sizeof(float), pc_cmp);
        r->trem_db = es[ne * 9 / 10] - es[ne / 10];
        double mean = 0.0;
        for (int k = 0; k < ne; ++k) mean += ev[k];
        mean /= ne;
        double best = 0.0;
        for (float hz = 0.5f; hz <= 20.f; hz += 0.1f) {
            double cr = 0.0, ci = 0.0;
            for (int k = 0; k < ne; ++k) {
                double ph = 2.0 * 3.14159265358979 * hz * k * 0.02;
                cr += (ev[k] - mean) * cos(ph); ci += (ev[k] - mean) * sin(ph);
            }
            double mg = cr * cr + ci * ci;
            if (mg > best) { best = mg; r->trem_hz = hz; }
        }
    }

    r->pass = r->peak <= PC_CEIL;
    for (int w = 0; w < PC_NWIN; ++w)
        if (r->win_db[w] < PC_FLOOR_DB || fabsf(r->win_db[w] - r->median_db) > PC_SPREAD_DB) r->pass = 0;
    if (audio) *audio = lr; else free(lr);
}

static void preset_print(int i, const pc_result *r, int verbose)
{
    printf("%2d %-8s %s  median %6.1f dB  min %6.1f  max %6.1f  peak %.3f  centroid %6.0f Hz\n", i + 1,
           nb_snap_name(i), r->pass ? "PASS" : "FAIL", r->median_db, r->min_db, r->max_db, r->peak, r->centroid_hz);
    if (verbose) {
        printf("    peaks %.0f %.0f %.0f %.0f Hz  flat %.3f  moves %.1f dB at %.1f Hz  corr %.2f\n   ",
               r->peaks_hz[0], r->peaks_hz[1], r->peaks_hz[2], r->peaks_hz[3], r->flatness, r->trem_db, r->trem_hz,
               r->corr);
        for (int w = 0; w < PC_NWIN; ++w) printf(" %5.1f", r->win_db[w]);
        printf("\n");
    }
}

#ifndef PRESETCHECK_NO_MAIN
static void pc_wav(const char *path, const float *lr, int frames, int fs)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "presetcheck: cannot write %s\n", path); return; }
    uint32_t data = (uint32_t)frames * 4u, rate = (uint32_t)fs, brate = rate * 4u, riff = 36u + data, fmt = 16u;
    uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f);
    fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&brate, 4, 1, f);
    fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    for (int i = 0; i < 2 * frames; ++i) {
        float v = lr[i] * 32767.f;
        int16_t s = (int16_t)(v > 32767.f ? 32767 : v < -32768.f ? -32768 : (int)lrintf(v));
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    int verbose = 0, stress = 0;
    float fs = 44100.f;
    const char *dir = 0;
    for (int a = 1; a < argc; ++a) {
        if (!strcmp(argv[a], "-v")) verbose = 1;
        else if (!strcmp(argv[a], "-w") && a + 1 < argc) dir = argv[++a];
        else if (!strcmp(argv[a], "-r") && a + 1 < argc) fs = (float)atof(argv[++a]);
        else if (!strcmp(argv[a], "-s")) stress = 1;
        else { fprintf(stderr, "usage: presetcheck [-v] [-w DIR] [-r RATE] [-s]\n"); return 2; }
    }
    int fails = 0;
    for (int i = 0; i < NB_NSNAP; ++i) {
        pc_result r;
        float *lr = 0;
        preset_check(i, fs, &r, dir ? &lr : 0);
        preset_print(i, &r, verbose);
        if (!r.pass) ++fails;
        if (stress) {
            static const struct { float fs; int sw; float dg, dm; const char *what; } run[8] = {
                { 48000.f, 0, 0, 0, "48k" }, { 0, 1, 0, 0, "switched" }, { 0, 0, -.03f, 0, "gain-" },
                { 0, 0, .03f, 0, "gain+" }, { 0, 0, 0, -.05f, "mid-" }, { 0, 0, 0, .05f, "mid+" },
                { 48000.f, 1, 0, 0, "48k switched" }, { 0, 1, -.03f, 0, "switched gain-" } };
            int ok = 0;
            printf("   stress:");
            for (int k = 0; k < 8; ++k) {
                pc_result q;
                pc_switch = run[k].sw; pc_dgain = run[k].dg; pc_dmid = run[k].dm;
                preset_check(i, run[k].fs > 0.f ? run[k].fs : fs, &q, 0);
                ok += q.pass;
                if (!q.pass) printf(" [%s: %.1f..%.1f dB]", run[k].what, q.min_db, q.max_db);
            }
            pc_switch = 0; pc_dgain = pc_dmid = 0.f;
            printf(" %d/8\n", ok);
            if (ok < 8 && r.pass) ++fails;
        }
        if (dir) {
            char path[512];
            snprintf(path, sizeof path, "%s/%02d_%s.wav", dir, i + 1, nb_snap_name(i));
            pc_wav(path, lr, (int)(PC_SECS * fs) / 128 * 128, (int)fs);
            free(lr);
        }
    }
    printf("presetcheck: %d of %d pass\n", NB_NSNAP - fails, NB_NSNAP);
    return fails ? 1 : 0;
}
#endif
