/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Mario Baril
 * Smoke tests of the core, run natively (no sound played): T-01 a silent start, T-02 the limiter under the worst
 * settings (every effect in, every knob at both ends), T-04 nothing turns into NaN, T-08 the sections and the
 * sixteen memories answer the buttons, kept as two banks, T-09 every factory memory sings by itself under DRONE,
 * T-10 the stages kept with each memory (and a bank from before them still loads); tests/stages.c the stages' own
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
/* run with FOLLOW fed from the network, as the platforms do; the RMS over the time, dBFS */
static float run_follow(nb_controls *c, float secs)
{
    double ss = 0.0;
    int n = (int)(secs * FS), k = 0;
    for (int t = 0; t < n; t += 128, ++k) {
        ms += 3; c->ms = ms;
        nb_panel_update(&p, c);
        for (int j = 0; j < 7; ++j) c->enc[j] = 0;
        nb_engine_render(&e, &p.params, &p.touch, buf, 128);
        nb_view v;
        nb_engine_view(&e, &p.params, &v);
        c->follow = v.out;
        for (int i = 0; i < 256; ++i) ss += (double)buf[i] * buf[i];
    }
    return (float)(10.0 * log10(ss / (256.0 * (k ? k : 1)) + 1e-20));
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
    {   /* HOME held, SCL tapped meanwhile, HOME let go: the channel stays, and HOME then finds the effects again
         * (found by the soak: the effect index went negative and HOME led to a CHAN page) */
        c.buttons = 1u << 8; run(&c, .01f, &peak, &bad);
        press(&c, 3);
        c.buttons = 1u << 8; run(&c, .01f, &peak, &bad);
        c.buttons = 0; run(&c, .01f, &peak, &bad);
        int on_ch = p.page >= NB_PG_CH1 && p.page < NB_PG_CH1 + 4;
        press(&c, 8);
        check(on_ch && p.page >= NB_PG_FX1 && p.page < NB_PG_FX1 + NB_NLOOPFX && p.last_fx >= 0,
              "T-08 HOME held over another section: no stray page");
    }
    press(&c, 10); check(p.page == NB_PG_REVERB, "T-08 ARP: REVERB");
    press(&c, 13); check(p.page == NB_PG_SETUP, "T-08 a tap on STORE: SETUP");
    press(&c, 13);                                             /* SETUP again: back */
    c.keys = 1u << 2; run(&c, .05f, &peak, &bad);              /* a tap on the second white key */
    c.keys = 0; run(&c, .01f, &peak, &bad);
    check(p.snap_index == 1, "T-08 a tap on white key 2: the second memory (no button)");
    c.keys = 1u << 26; run(&c, .05f, &peak, &bad);             /* the sixteenth white key, the last key */
    c.keys = 0; run(&c, .01f, &peak, &bad);
    check(p.snap_index == 15 && !strcmp(nb_snap_name(15), "LANTERN"), "T-08 a tap on white key 16: the sixteenth memory");
    {   /* held, the sound MORPHs toward the memory; let go half way, it stays half way (the memory is not changed) */
        float from = p.cur.pos[NB_K_MID], to = p.snaps[4].pos[NB_K_MID];
        c.keys = 1u << 7; run(&c, 1.f, &peak, &bad);           /* white key 5 held 1 s: 0.25 s a tap, then ~50 % */
        float mid = p.cur.pos[NB_K_MID];
        c.keys = 0; run(&c, .05f, &peak, &bad);
        float t = (mid - from) / (to - from);
        printf("  MORPH toward memory 5 after 1 s held: %.0f %% of the way, kept after letting go: %s\n", 100.f * t,
               fabsf(p.cur.pos[NB_K_MID] - mid) < 1e-6f ? "yes" : "no");
        check(t > 0.3f && t < 0.7f && fabsf(p.cur.pos[NB_K_MID] - mid) < 1e-6f && p.snap_index == 15,
              "T-08 a white key held MORPHs toward its memory, let go half way it stays");
    }
    {   /* the black keys: NODE 2 strikes loop 2 alone, ALL all four; CHOP held cuts every 16 crossings, + K1 every 8;
         * SCATTER held 8 a second */
        c.keys = 1u << 3; nb_panel_update(&p, &c);
        int n2 = p.touch.kick_mask == 2 && p.touch.hold;
        c.keys = 0; run(&c, .01f, &peak, &bad);
        c.keys = 1u << 10; nb_panel_update(&p, &c);
        int all = p.touch.kick_mask == 15;
        c.keys = 0; run(&c, .01f, &peak, &bad);
        c.keys = 1u << 15; run(&c, .01f, &peak, &bad);
        int chop16 = p.touch.chop == 16;
        c.enc[2] = 1; run(&c, .01f, &peak, &bad);
        int chop8 = p.touch.chop == 8 && !strcmp(p.hud.title, "CHOP");
        c.keys = 0; run(&c, .01f, &peak, &bad);
        c.keys = 1u << 17; run(&c, .01f, &peak, &bad);
        int sc8 = p.touch.scatter_hz == 8.f && p.touch.chop == 0;
        c.keys = 0; run(&c, .01f, &peak, &bad);
        check(n2 && all && chop16 && chop8 && sc8 && !strcmp(nb_pad_name(25), "VIEW") && !strcmp(nb_pad_name(1), "NODE 1"),
              "T-08 black keys: NODE 1-4, ALL, CUT, CHOP, SCATTER (+ K1: the rate), RETURN, OPEN, VIEW");
    }
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
    {   /* T-10 the stages are kept: a stage on a memory survives the store; a bank kept before the stages ("NB07",
         * the same bytes without the 4 of stages) still loads, every sound as it was and orthodox */
        nb_panel_set_stage(&p, NB_FX_WRAP, 2, 0);
        nb_panel_set_stage(&p, NB_STAGE_NET, 1, 0);
        p.cur.pos[NB_K_MID] = 0.321f;
        c.buttons = 1u << NB_B_STORE; run(&c, .01f, &peak, &bad);
        c.keys = 1u << 19; run(&c, .01f, &peak, &bad);
        c.keys = 0; c.buttons = 0; run(&c, .01f, &peak, &bad);
        (void)nb_panel_take_store(&p);
        static nb_store b1;
        b1 = p.store[1];
        nb_panel q;
        nb_panel_init(&q);
        nb_panel_load(&q, 1, &b1, sizeof b1);
        check(q.snaps[11].stage[NB_FX_WRAP] == 2 && q.snaps[11].stage[NB_STAGE_NET] == 1 && q.snaps[11].stage[NB_FX_FUZZ] == 0
              && sizeof(nb_store) <= 3840u, "T-10 the stages kept with the memory (NB08), the bank within 3840 B");
        static uint8_t v7[NB_STORE_V7_BYTES];
        unsigned at = 0;
        uint32_t magic = 0x4e423037u, bank = 1u;
        memcpy(v7 + at, &magic, 4); at += 4;
        memcpy(v7 + at, &bank, 4); at += 4;
        for (int i = 0; i < NB_BANK_SNAPS; ++i) { memcpy(v7 + at, &b1.snap[i], sizeof b1.snap[i] - 6); at += sizeof b1.snap[i] - 6; }
        memcpy(v7 + at, &b1.setup, sizeof b1.setup); at += sizeof b1.setup;
        uint32_t h = 2166136261u;
        for (unsigned k = 4; k < at; ++k) { h ^= v7[k]; h *= 16777619u; }
        memcpy(v7 + at, &h, 4); at += 4;
        nb_panel_init(&q);
        nb_panel_load(&q, 1, v7, at);
        check(at == NB_STORE_V7_BYTES && fabsf(q.snaps[11].pos[NB_K_MID] - 0.321f) < 1e-4f && q.snaps[11].stage[NB_FX_WRAP] == 0,
              "T-10 a bank kept before the stages (NB07) loads, all orthodox");
        {   /* NB08 (with the stages, before the switch): the same, every effect on */
            static uint8_t v8[NB_STORE_V8_BYTES];
            unsigned a8 = 0;
            uint32_t m8 = 0x4e423038u;
            memcpy(v8 + a8, &m8, 4); a8 += 4;
            memcpy(v8 + a8, &bank, 4); a8 += 4;
            for (int i = 0; i < NB_BANK_SNAPS; ++i) { memcpy(v8 + a8, &b1.snap[i], sizeof b1.snap[i] - 2); a8 += sizeof b1.snap[i] - 2; }
            memcpy(v8 + a8, &b1.setup, sizeof b1.setup); a8 += sizeof b1.setup;
            uint32_t h8 = 2166136261u;
            for (unsigned k = 4; k < a8; ++k) { h8 ^= v8[k]; h8 *= 16777619u; }
            memcpy(v8 + a8, &h8, 4); a8 += 4;
            nb_panel_init(&q);
            nb_panel_load(&q, 1, v8, a8);
            check(a8 == NB_STORE_V8_BYTES && q.snaps[11].stage[NB_FX_WRAP] == 2 && q.snaps[11].fx_off == 0
                  && fabsf(q.snaps[11].pos[NB_K_MID] - 0.321f) < 1e-4f, "T-10 a bank kept as NB08 loads, every effect on");
        }
    }
    {   /* T-20: FX held, a long press (600 ms) on an effect's white key switches it off: out of the path (the engine's
         * fx_on), its MIX kept; another switches it on; the switch is kept with the memory */
        nb_panel_init(&p);
        memset(&c, 0, sizeof c);
        c.pot = 700;
        run(&c, .05f, &peak, &bad);
        p.cur.fx[NB_FX_FUZZ][3] = 0.6f;
        c.buttons = 1u << NB_B_FX; run(&c, .02f, &peak, &bad);
        c.keys = 1u << 0; run(&c, .2f, &peak, &bad);           /* a short press: the page, still on */
        int page = p.page == NB_PG_FX1 + NB_FX_FUZZ && p.params.fx_on[NB_FX_FUZZ];
        run(&c, .5f, &peak, &bad);                             /* held past 600 ms: off */
        int off = !p.params.fx_on[NB_FX_FUZZ] && p.cur.fx[NB_FX_FUZZ][3] == 0.6f && !strcmp(p.hud.title, "FUZZ OFF");
        c.keys = 0; run(&c, .02f, &peak, &bad);
        c.buttons = 0; run(&c, .02f, &peak, &bad);
        c.buttons = 1u << NB_B_STORE; run(&c, .01f, &peak, &bad);
        c.keys = 1u << 2; run(&c, .01f, &peak, &bad);          /* kept in memory 2 */
        c.keys = 0; c.buttons = 0; run(&c, .01f, &peak, &bad);
        (void)nb_panel_take_store(&p);
        nb_panel q2;
        nb_panel_init(&q2);
        nb_panel_load(&q2, 0, &p.store[0], sizeof p.store[0]);
        int kept = ((q2.snaps[1].fx_off >> NB_FX_FUZZ) & 1u) && fabsf(q2.snaps[1].fx[NB_FX_FUZZ][3] - 0.6f) < 1e-4f;
        c.buttons = 1u << NB_B_FX; run(&c, .02f, &peak, &bad);
        c.keys = 1u << 0; run(&c, .7f, &peak, &bad);
        int on = p.params.fx_on[NB_FX_FUZZ] && !strcmp(p.hud.title, "FUZZ ON");
        c.keys = 0; c.buttons = 0; run(&c, .02f, &peak, &bad);
        check(page && off && kept && on, "T-20 FX + a long press: the effect off (its MIX kept, kept in the memory), on again");
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
        c.keys = (pass & 1) ? 1u << 1 : 0u;                    /* NODE 1 held now and then */
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
    /* T-16: DRONE latched, the memory singing; GAIN at zero until the loops are quiet; the same memory recalled */
    int back = 0;
    for (int i = 0; i < NB_NSNAP; ++i) {
        static const uint8_t white[16] = { 0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26 };
        nb_engine_init(&e, FS);
        nb_panel_init(&p);
        memset(&c, 0, sizeof c);
        c.pot = 700;
        c.buttons = 1u << NB_B_MEMORY; run_follow(&c, .01f);
        c.keys = 1u << white[i]; run_follow(&c, .01f);
        c.keys = 0; c.buttons = 0; run_follow(&c, .01f);
        c.buttons = 1u << NB_B_DRONE; run_follow(&c, .01f);
        c.buttons = 0; run_follow(&c, 2.f);
        float g = p.cur.pos[NB_K_GAIN];
        p.cur.pos[NB_K_GAIN] = 0.f; run_follow(&c, 5.f);
        p.cur.pos[NB_K_GAIN] = g;
        c.buttons = 1u << NB_B_MEMORY; run_follow(&c, .01f);
        c.keys = 1u << white[i]; run_follow(&c, .01f);
        c.keys = 0; c.buttons = 0; run_follow(&c, 2.f);
        float db = run_follow(&c, 4.f);
        if (db > -30.f) ++back;
        else printf("  %s stays quiet (%.1f dBFS)\n", nb_snap_name(i), db);
    }
    printf("  %d of %d memories sing again\n", back, NB_NSNAP);
    /* the engine's (the silent loops' strike on a recall under DRONE was 852f59d on the 0.1 branch, before the engine
     * moved to its own session): said, not counted, until the engine has it */
    printf("%s T-16 under DRONE, a memory recalled on a quiet network sings again (the engine's)\n", back == NB_NSNAP ? "pass" : "note");
    /* T-18: SETUP kept without leaving its page (the soak found a change lost by a power-off on SETUP), one write
     * for a whole turn, not before 1.5 s */
    {
        nb_panel_init(&p);
        memset(&c, 0, sizeof c);
        c.pot = 700;
        run(&c, .05f, &peak, &bad);
        press(&c, NB_B_STORE);                                  /* a tap on STORE: SETUP */
        nb_panel_take_store(&p);
        int early = 0;
        for (int k = 0; k < 3; ++k) { c.enc[2] = 1; run(&c, .1f, &peak, &bad); early |= nb_panel_take_store(&p); }
        run(&c, .5f, &peak, &bad);
        early |= nb_panel_take_store(&p);
        run(&c, 1.5f, &peak, &bad);
        int later = nb_panel_take_store(&p);
        check(p.page == NB_PG_SETUP && p.setup_midi_ch == 3 && !early && later == 1 && p.store[0].setup.midi_ch == 3,
              "T-18 SETUP kept 1.5 s after its last change, on its page, in one write");
    }
    /* T-19: the effects within the device's budget (its PERF MAX, every MIX up, overran it): a MIX turned up from 0
     * that would pass NB_FX_BUDGET stays at 0 and the screen says FX FULL with what is left; whatever else brings a MIX
     * up, the engine gets the sound within the budget */
    {
        nb_engine_init(&e, FS);
        nb_panel_init(&p);
        memset(&c, 0, sizeof c);
        c.pot = 700;
        run(&c, .05f, &peak, &bad);
        int in0 = 0;
        for (int f = 0; f < NB_NFX; ++f) in0 += p.cur.fx[f][3] > 0.f;
        press(&c, 8);                                           /* HOME: the first effect */
        int refused = -1, said = 0;
        for (int f = 0; f < NB_NLOOPFX && refused < 0; ++f) {
            if (p.cur.fx[f][3] == 0.f) {
                c.enc[5] = 4; run(&c, .05f, &peak, &bad);        /* K4: its MIX */
                if (p.cur.fx[f][3] == 0.f) { refused = f; said = !strcmp(p.hud.title, "FX FULL"); }
            }
            press(&c, 8);                                       /* HOME again: the next effect */
        }
        int in = 0;
        for (int f = 0; f < NB_NFX; ++f) in += p.cur.fx[f][3] > 0.f;
        float load = nb_fx_load(&p.cur, -1);
        for (int f = 0; f < NB_NFX; ++f) p.cur.fx[f][3] = 1.f;  /* forced past the panel: the engine still gets it within */
        run(&c, .05f, &peak, &bad);
        float sent = nb_fx_cost(-1);
        for (int f = 0; f < NB_NFX; ++f) if (p.params.fx[f][3] > 0.f) sent += nb_fx_cost(f);
        printf("  %d in at the start, %d after the turns (%.0f %%), refused the %s (%s %s), %.0f %% sent when all are forced\n",
               in0, in, load, refused >= 0 ? nb_fx_name(refused) : "none", p.hud.title, p.hud.value, sent);
        check(refused >= 0 && said && load <= NB_FX_BUDGET && sent <= NB_FX_BUDGET, "T-19 the effects within the budget: FX FULL");
    }
    /* T-21: SETUP's TRANSITION (0..2 s, 0.5 by default) is the engine's nb_params.transition, said in seconds, kept */
    {
        nb_panel_init(&p);
        memset(&c, 0, sizeof c);
        c.pot = 700;
        run(&c, .05f, &peak, &bad);
        int def = p.params.transition >= 0.f && p.params.transition <= 0.5f;
        press(&c, NB_B_STORE);                                  /* SETUP */
        for (int k = 0; k < 8; ++k) { c.enc[4] = 3; run(&c, .02f, &peak, &bad); }   /* K3 up */
        float tr = p.params.transition;
        int said = !strcmp(p.hud.title, "TRANSITION");
        run(&c, 2.f, &peak, &bad);                              /* kept after 1.5 s */
        nb_panel q3;
        nb_panel_init(&q3);
        nb_panel_load(&q3, 0, &p.store[0], sizeof p.store[0]);
        printf("  TRANSITION (%.2f s asked) by default, %.2f s turned up (%s %s), %.2f s read back\n", 0.5, tr, p.hud.title,
               p.hud.value, 2.f * q3.setup_transition);
        check(def && tr > 0.6f && said && fabsf(2.f * q3.setup_transition - tr) < 0.01f,
              "T-21 SETUP's TRANSITION: the engine's, in seconds, kept");
        press(&c, NB_B_STORE);
    }
    /* T-22: the lights tell the truth: the memory playing on, a stored one dim, the playing one breathing once edited;
     * a node's key on, dim or off with its loop's level; with FX held, an effect switched off dim */
    {
        static const uint8_t W[16] = { 0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26 };
        nb_panel_init(&p);
        memset(&c, 0, sizeof c);
        c.pot = 700;
        run(&c, .05f, &peak, &bad);
        int mem_on = p.led[14 + W[0]] == NB_LED_ON && p.led[14 + W[1]] == 0;
        p.cur.pos[NB_K_MID] = 0.77f;
        c.buttons = 1u << NB_B_STORE; run(&c, .01f, &peak, &bad);
        c.keys = 1u << W[1]; run(&c, .01f, &peak, &bad);      /* stored in memory 2 */
        c.keys = 0; c.buttons = 0; run(&c, .01f, &peak, &bad);
        c.keys = 1u << W[0]; run(&c, .02f, &peak, &bad);      /* back to memory 1 */
        c.keys = 0; run(&c, .02f, &peak, &bad);
        int stored_dim = p.led[14 + W[1]] == NB_LED_DIM && p.led[14 + W[0]] == NB_LED_ON;
        c.enc[4] = 3; run(&c, .02f, &peak, &bad);              /* an edit: memory 1 breathes */
        int breath = 0;
        for (int k = 0; k < 40; ++k) { run(&c, .03f, &peak, &bad); breath |= p.led[14 + W[0]] == NB_LED_DIM ? 1 : 0; breath |= p.led[14 + W[0]] == NB_LED_ON ? 2 : 0; }
        c.loud[0] = 0.3f; c.loud[1] = 0.01f; c.loud[2] = 0.f; c.loud[3] = 0.f;
        run(&c, .01f, &peak, &bad);
        int nodes = p.led[14 + 1] == NB_LED_ON && p.led[14 + 3] == NB_LED_DIM && p.led[14 + 5] == 0 && p.led[14 + 10] == NB_LED_ON;
        p.cur.fx[NB_FX_FUZZ][3] = 0.5f; p.cur.fx[NB_FX_FOLD][3] = 0.5f; p.cur.fx_off = 1u << NB_FX_FOLD;
        c.buttons = 1u << NB_B_FX; run(&c, .02f, &peak, &bad);
        int fx = p.led[14 + W[NB_FX_FUZZ]] == NB_LED_ON && p.led[14 + W[NB_FX_FOLD]] == NB_LED_DIM;
        c.buttons = 0; run(&c, .02f, &peak, &bad);
        memset(c.loud, 0, sizeof c.loud);
        check(mem_on && stored_dim && breath == 3 && nodes && fx, "T-22 the lights: memories, an edit breathing, the nodes' levels, FX's in and off");
    }
    /* T-23: never a silent loss: a sound whose effects pass the budget (a stored sound, a morph, a modulation) has the
     * ones left out said, FX LEFT OUT and the first one's name, and kept visible (hud.fx_cut, their keys blinking
     * under FX held) */
    {
        static const uint8_t W[16] = { 0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26 };
        nb_panel_init(&p);
        memset(&c, 0, sizeof c);
        c.pot = 700;
        run(&c, .05f, &peak, &bad);
        for (int f = 0; f < NB_NFX; ++f) p.cur.fx[f][3] = 1.f;  /* as a sound stored past the budget would come */
        run(&c, .02f, &peak, &bad);
        int said = !strcmp(p.hud.title, "FX LEFT OUT") && p.hud.value[0];
        uint16_t cut = p.hud.fx_cut;
        int sent_cut = 1, first = -1;
        for (int f = 0; f < NB_NFX; ++f) if ((cut >> f) & 1u) { sent_cut &= !(p.params.fx[f][3] > 0.f); if (first < 0 && f < NB_NLOOPFX) first = f; }
        c.buttons = 1u << NB_B_FX;
        int seen_on = 0, seen_off = 0;
        for (int k = 0; k < 40; ++k) {
            run(&c, .02f, &peak, &bad);
            if (first >= 0) { seen_on |= p.led[14 + W[first]] == NB_LED_ON; seen_off |= p.led[14 + W[first]] == 0; }
        }
        c.buttons = 0; run(&c, .02f, &peak, &bad);
        printf("  left out %s (%s): %d effects\n", p.hud.value, first >= 0 ? nb_fx_name(first) : "-", __builtin_popcount(cut));
        check(said && cut && sent_cut && first >= 0 && seen_on && seen_off, "T-23 effects the budget leaves out are said and shown, never silent");
    }
    /* T-17: no DC out: every effect in at full MIX, GAIN DRIVE SCREAM at the top, the other knobs at both ends; the
     * mean of each second (the soak found half the scale with every effect in) */
    {
        nb_engine_init(&e, FS);
        nb_panel_init(&p);
        memset(&c, 0, sizeof c);
        c.pot = 1023;
        c.buttons = 1u << NB_B_DRONE; run_follow(&c, .01f);
        c.buttons = 0;
        float dc_worst = 0.f;
        for (int pass = 0; pass < 8; ++pass) {
            for (int k = 0; k < NB_NKNOB; ++k) p.cur.pos[k] = (k == NB_K_GAIN || k == NB_K_DRIVE || k == NB_K_GRITO) ? 1.f : (float)((pass >> 2) & 1);
            for (int f = 0; f < NB_NFX; ++f)
                for (int k = 0; k < 3; ++k) p.cur.fx[f][k] = (pass >> (k & 1)) & 1 ? 1.f : 0.f;
            for (int f = 0; f < NB_NFX; ++f) p.cur.fx[f][3] = 1.f;
            run_follow(&c, 1.f);
            for (int s = 0; s < 3; ++s) {
                double sum[2] = { 0, 0 };
                int n = 0;
                for (int t = 0; t < (int)FS; t += 128, n += 128) {
                    ms += 3; c.ms = ms;
                    nb_panel_update(&p, &c);
                    nb_engine_render(&e, &p.params, &p.touch, buf, 128);
                    for (int i = 0; i < 128; ++i) { sum[0] += buf[2 * i]; sum[1] += buf[2 * i + 1]; }
                }
                for (int k = 0; k < 2; ++k) if (fabs(sum[k] / n) > dc_worst) dc_worst = (float)fabs(sum[k] / n);
            }
        }
        printf("  worst DC %.4f\n", dc_worst);
        /* the engine's (the output's high-pass was 7ea12d1 on the 0.1 branch, before the engine moved to its own
         * session; main's coupling capacitor lets more through): said, not counted, until the engine has it */
        printf("%s T-17 no DC out with every effect in (each second's mean under 0.01; the engine's)\n",
               dc_worst < 0.01f ? "pass" : "note");
    }
    printf("%s\n", fails ? "smoke: FAILED" : "smoke: passed");
    return fails ? 1 : 0;
}
