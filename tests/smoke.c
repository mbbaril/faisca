/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Mario Baril
 * Smoke tests of the core, run natively (no sound played): T-01 a silent start, T-02 the limiter under the worst
 * settings (every effect in, every knob at both ends), T-04 nothing turns into NaN, T-08 the sections and the
 * sixteen memories answer the buttons, kept as two banks, T-09 every factory memory sings by itself under DRONE
 * (tools/presetcheck.c).
 *   cc -O2 -o build/smoke tests/smoke.c core/nb_engine.c core/nb_panel.c -lm && build/smoke */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../core/nb_engine.h"
#include "../core/nb_panel.h"
#define PRESETCHECK_NO_MAIN
#include "../tools/presetcheck.c"

#define FS 44100.f
static nb_engine e;
static nb_panel p;
static float buf[256];
static int fails;
static uint32_t ms;

static void check(int ok, const char *what) { printf("%s %s\n", ok ? "pass" : "FAIL", what); if (!ok) ++fails; }

static void run(nb_controls *c, float secs, float *peak, int *bad)
{
    *peak = 0.f; *bad = 0;
    int n = (int)(secs * FS);
    for (int t = 0; t < n; t += 128) {
        ms += 3; c->ms = ms;
        nb_panel_update(&p, c);
        for (int k = 0; k < 7; ++k) c->enc[k] = 0;
        nb_engine_render(&e, &p.params, &p.touch, buf, 128);
        for (int i = 0; i < 256; ++i) {
            if (!(buf[i] == buf[i])) *bad = 1;
            float a = fabsf(buf[i]);
            if (a > *peak) *peak = a;
        }
    }
}
static void press(nb_controls *c, int button)
{
    float pk; int bad;
    c->buttons |= 1u << button; run(c, .01f, &pk, &bad);
    c->buttons &= ~(1u << button); run(c, .01f, &pk, &bad);
}

int main(void)
{
    nb_controls c;
    memset(&c, 0, sizeof c);
    c.pot = 1023;
    nb_engine_init(&e, FS);
    nb_panel_init(&p);
    float peak; int bad;

    /* T-01: two seconds after power, untouched */
    run(&c, 2.f, &peak, &bad);
    check(peak < 3e-5f && !bad, "T-01 silent start (peak < -90 dBFS)");

    /* T-08: the sections */
    press(&c, 2);  check(p.page == NB_PG_LOOP, "T-08 FX button: LOOP");
    press(&c, 3);  check(p.page == NB_PG_CH1, "T-08 SCL: CHAN 1");
    press(&c, 3);  check(p.page == NB_PG_CH1 + 1, "T-08 SCL again: CHAN 2");
    press(&c, 8);  check(p.page == NB_PG_FX1, "T-08 HOME: the first effect");
    press(&c, 8);  check(p.page == NB_PG_FX1 + 1, "T-08 HOME again: the next effect");
    press(&c, 10); check(p.page == NB_PG_REVERB, "T-08 ARP: REVERB");
    press(&c, 13); check(p.page == NB_PG_SETUP, "T-08 a tap on STORE: SETUP");
    c.buttons = 1u << NB_B_MEMORY; run(&c, .01f, &peak, &bad);
    c.keys = 1u << 2; run(&c, .01f, &peak, &bad);              /* the second white key */
    c.keys = 0; c.buttons = 0; run(&c, .01f, &peak, &bad);
    check(p.snap_index == 1, "T-08 MEMORY + white key 2: the second memory");
    c.buttons = 1u << NB_B_MEMORY; run(&c, .01f, &peak, &bad);
    c.keys = 1u << 26; run(&c, .01f, &peak, &bad);             /* the sixteenth white key, the last key */
    c.keys = 0; c.buttons = 0; run(&c, .01f, &peak, &bad);
    check(p.snap_index == 15 && !strcmp(nb_snap_name(15), "LANTERN"), "T-08 MEMORY + white key 16: the sixteenth memory");
    {   /* STORE on the twelfth white key: bank 1 alone is saved; its blob loads back, a broken one does not */
        static nb_store b1;
        p.cur.pos[NB_K_MID] = 0.123f;
        c.buttons = 1u << NB_B_STORE; run(&c, .01f, &peak, &bad);
        c.keys = 1u << 19; run(&c, .01f, &peak, &bad);
        c.keys = 0; c.buttons = 0; run(&c, .01f, &peak, &bad);
        int banks = nb_panel_take_store(&p);
        check(banks == 2 && p.store[1].bank == 1u && sizeof(nb_store) <= 3840u,
              "T-08 STORE + white key 12: bank 1 saved alone, one bank fits 3840 B");
        b1 = p.store[1];
        nb_panel q;
        nb_panel_init(&q);
        nb_panel_load(&q, 0, &b1, sizeof b1);                  /* the wrong bank: ignored */
        float before = q.snaps[11].pos[NB_K_MID];
        nb_panel_load(&q, 1, &b1, sizeof b1);
        check(before != q.snaps[11].pos[NB_K_MID] && fabsf(q.snaps[11].pos[NB_K_MID] - 0.123f) < 1e-4f
              && q.snaps[0].pos[NB_K_MID] == p.snaps[0].pos[NB_K_MID], "T-08 bank 1 loads into memories 9..16 only");
        nb_panel_init(&q);
        b1.snap[3].v[NB_K_MID] ^= 0x5a5au;                      /* a broken bank: the factory sound stays */
        nb_panel_load(&q, 1, &b1, sizeof b1);
        check(fabsf(q.snaps[11].pos[NB_K_MID] - before) < 1e-6f, "T-08 a bank that fails its check keeps the factory sounds");
    }

    /* T-02/T-04: DRONE on, everything at the top, every effect in, their knobs at both ends */
    press(&c, NB_B_DRONE);
    for (int k = 0; k < NB_NKNOB; ++k) p.cur.pos[k] = 1.f;
    for (int ch = 0; ch < NB_CH; ++ch) { p.cur.ch_gain[ch] = 1.f; p.cur.ch_level[ch] = 1.f; }
    p.cur.hiss = 1.f;
    float worst = 0.f; int anybad = 0;
    for (int pass = 0; pass < 4; ++pass) {
        for (int f = 0; f < NB_NFX; ++f)
            for (int k = 0; k < 3; ++k) p.cur.fx[f][k] = (pass >> (k & 1)) & 1 ? 1.f : 0.f;
        for (int f = 0; f < NB_NFX; ++f) p.cur.fx[f][3] = 1.f;
        c.keys = (pass & 1) ? 1u : 0u;                         /* a pad held now and then */
        run(&c, 1.f, &peak, &bad);
        if (peak > worst) worst = peak;
        if (bad) anybad = 1;
        /* each effect alone, at the extremes */
        for (int f = 0; f < NB_NFX; ++f) {
            for (int g = 0; g < NB_NFX; ++g) p.cur.fx[g][3] = g == f ? 1.f : 0.f;
            run(&c, .2f, &peak, &bad);
            if (peak > worst) worst = peak;
            if (bad) { anybad = 1; printf("  NaN: %s, pass %d\n", nb_fx_name(f), pass); }
        }
    }
    printf("  worst peak %.4f\n", worst);
    check(worst <= 0.9801f, "T-02 limiter: peak <= 0.98 everywhere");
    check(!anybad, "T-04 no NaN anywhere");

    /* T-09: each factory memory, from power-on, recalled and DRONE latched, no pad: sustained, not percussive */
    int sung = 0;
    for (int i = 0; i < NB_NSNAP; ++i) {
        pc_result r;
        preset_check(i, FS, &r, 0);
        if (!r.pass) preset_print(i, &r, 1);
        sung += r.pass;
    }
    printf("  %d of %d memories sing\n", sung, NB_NSNAP);
    check(sung == NB_NSNAP, "T-09 every memory sustains by itself (presetcheck)");
    printf("%s\n", fails ? "smoke: FAILED" : "smoke: passed");
    return fails ? 1 : 0;
}
