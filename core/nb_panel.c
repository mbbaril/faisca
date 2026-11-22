/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Mario Baril
 * The instrument's panel logic: the controls and MIDI turned into the loop's values. One fixed structure: eight
 * section buttons, four knobs each; MOD (hold and turn), MOTION (the knobs' gestures in a loop), MORPH (toward a
 * memory on the keys), the memories on the keys, the LEDs and what the screen says. */
#include "nb_panel.h"
#include "nb_dsp.h"
#include "nb_curves.h"

/* ---- the matrices: [to][from] ---- */
static const char *const topo_names[] = { "RING", "PAIRS", "CHAIN", "STAR", "ALL", "TWIST" };
#define NB_NTOPO 6
static const float topo[NB_NTOPO][NB_CH][NB_CH] = {
    /* RING: each loop feeds the next, 4 back into 1 */
    { {0, 0, 0, 1}, {1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0} },
    /* PAIRS: 1 and 2 feed each other, 3 and 4 */
    { {0, 1, 0, 0}, {1, 0, 0, 0}, {0, 0, 0, 1}, {0, 0, 1, 0} },
    /* CHAIN: 1 into 2 into 3 into 4, nothing back */
    { {0, 0, 0, 0}, {1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0} },
    /* STAR: 1 feeds the three, the three return to 1 weaker */
    { {0, .45f, .45f, .45f}, {.9f, 0, 0, 0}, {.9f, 0, 0, 0}, {.9f, 0, 0, 0} },
    /* ALL: everyone into everyone */
    { {0, .6f, .6f, .6f}, {.6f, 0, .6f, .6f}, {.6f, .6f, 0, .6f}, {.6f, .6f, .6f, 0} },
    /* TWIST: the ring with the polarity flipping at every other step, and the opposite loops crossed */
    { {0, 0, .5f, -1}, {1, 0, 0, .5f}, {-.5f, -1, 0, 0}, {0, -.5f, 1, 0} },
};

static const char *const part_names[] = { "UNISON", "FIFTHS", "HARMONIC", "ODD", "OCTAVES", "BELL", "BAR" };
#define NB_NPART 7
static const float parts[NB_NPART][NB_CH] = {
    { 1.f, 1.0031f, 0.9973f, 1.0062f },
    { 1.f, 1.5f, 2.f, 3.f },
    { 1.f, 2.f, 3.f, 4.f },
    { 1.f, 3.f, 5.f, 7.f },
    { 1.f, 2.f, 4.f, 8.f },
    { 1.f, 2.f, 2.4f, 3.f },
    { 1.f, 2.756f, 5.404f, 8.933f },
};

static const char *const src_names[NB_NSRC] = { "LFO 1", "LFO 2", "RANDOM", "FOLLOW" };

/* ---- the effects: name and four knobs, always in this order ---- */
static const char *const fx_names[NB_NFX] = {
    "FUZZ", "BLAST", "FOLD", "WRAP", "RECTIFY", "DIVIDE", "RING", "STARVE", "SHIFT", "ECHO", "PUMP", "SWEEP",
    "DELAY", "REVERB"
};
static const char *const fx_knobs[NB_NFX][4] = {
    { "GAIN", "TONE", "BIAS", "MIX" },        { "GAIN", "TONE", "EMPHASIS", "MIX" },
    { "DEPTH", "BIAS", "SOFT", "MIX" },        { "DEPTH", "BIAS", "SMOOTH", "MIX" },
    { "HALF", "BIAS", "TONE", "MIX" },         { "OCTAVE", "WIDTH", "TONE", "MIX" },
    { "CARRIER", "FREQ", "DC", "MIX" },        { "SAG", "GATE", "RECOVER", "MIX" },
    { "HZ", "SPREAD", "FEEDBACK", "MIX" },     { "TIME", "FEEDBACK", "WOW", "MIX" },
    { "DEPTH", "ATTACK", "RELEASE", "MIX" },   { "RANGE", "BASE", "RESO", "MIX" },
    { "TIME", "FEEDBACK", "TONE", "MIX" },     { "SIZE", "DAMP", "WIDTH", "MIX" },
};
/* where each effect's knobs rest (the MIX at 0: the effect is not there) */
static const float fx_rest[NB_NFX][4] = {
    { .5f, .6f, .3f, 0 }, { .5f, .5f, .5f, 0 }, { .4f, .5f, .2f, 0 }, { .4f, .5f, .2f, 0 },
    { 0, .1f, .6f, 0 },   { 0, 0, .5f, 0 },     { 0, .4f, 0, 0 },     { .5f, .3f, .4f, 0 },
    { .62f, .3f, .2f, 0 },{ .4f, .4f, .2f, 0 }, { .5f, .2f, .4f, 0 }, { .5f, .4f, .5f, 0 },
    { .35f, .4f, .4f, 0 },{ .55f, .4f, .6f, 0 },
};

static const char *const page_names[NB_NPAGE] = {
    "LOOP", "LOOP 1", "LOOP 2", "LOOP 3", "LOOP 4", "INTO 1", "INTO 2", "INTO 3", "INTO 4", "SHAPE",
    "FUZZ", "BLAST", "FOLD", "WRAP", "RECTIFY", "DIVIDE", "RING", "STARVE", "SHIFT", "ECHO", "PUMP", "SWEEP",
    "DELAY", "REVERB", "MOD", "SETUP"
};

/* ---- the memories: oblique names; each one sings by itself once DRONE is latched (tools/presetcheck) ---- */
static const char *const snap_names[NB_NSNAP] = {
    "TETHER", "EMBER", "CHALK", "DEW", "SMOKE", "DROUGHT", "SILL", "MILL",
    "HUSK", "NEEDLE", "MOTH", "GRAVEL", "KITE", "GRIST", "TIDE", "LANTERN"
};
/* a position in the flat list (+1, so 0 is an empty entry) and its value; a modulation (target +1, source, depth) */
typedef struct { uint8_t t1; float v; } fset_t;
typedef struct { uint8_t t1, src; float d; } fmod_t;
typedef struct {
    float pos[NB_NKNOB]; uint8_t topology, partials; float hiss;
    float rate[4];                                            /* LFO 1, LFO 2, RANDOM, SMOOTH; 0: the default */
    fset_t set[14];                                           /* anything else, over the resting sound */
    fmod_t mod[3];
} factory_t;
#define T_GAIN(c) (6 + (c))
#define T_PITCH(c) (10 + (c))
#define T_MIDN(c) (14 + (c))
#define T_LEVEL(c) (18 + (c))
#define T_SEND(to, from) (22 + (to) * 4 + (from))
#define T_FXK(f, k) (NB_T_FX + (f) * 4 + (k))
#define SET(t, v) { (uint8_t)((t) + 1), (v) }
#define FXK(f, k, v) SET(T_FXK(f, k), v)
#define MIX(f, v) SET(T_FXK(f, 3), v)
#define MOD(t, s, d) { (uint8_t)((t) + 1), (s), (d) }
/* semitones on a loop's PITCH knob (+-24) */
#define ST(x) (0.5f + (x) / 48.f)
/* every memory: { CROSS SCREAM GAIN MID FOCUS DRIVE }, topology, partials, hiss, { the mod rates }, { the positions
 * set }, { the modulations }; the rest rests where fx_rest and a new sound put it */
static const factory_t factory[NB_NSNAP] = {
    /* TETHER: the four loops in a ring on the harmonics, a warm held chord */
    { { .346f, 0.f, .674f, .364f, .752f, .336f }, 0, 2, 0.f, { 0 },
      { MIX(NB_FX_REVERB, .15f) }, { { 0 } } },
    /* EMBER: fuzz and fold over odd partials, SCREAM rocking slowly */
    { { .423f, .104f, .759f, .244f, .594f, .865f }, 1, 3, .1f, { 0 },
      { MIX(NB_FX_FUZZ, .6f), MIX(NB_FX_FOLD, .5f) }, { MOD(NB_K_GRITO, NB_SRC_LFO1, .15f) } },
    /* CHALK: the bell partials through the short echoes, a little room */
    { { .208f, 0.f, .445f, .444f, .907f, .195f }, 0, 5, 0.f, { 0 },
      { MIX(NB_FX_ECHO, .5f), MIX(NB_FX_REVERB, .3f) }, { { 0 } } },
    /* DEW: a chain of octaves through the shifter, each pass a little higher: glass, delay and plate */
    { { .159f, .036f, .9f, .524f, .1f, .100f }, 2, 4, 0.f, { 0 },
      { MIX(NB_FX_SHIFT, .6f), FXK(NB_FX_SHIFT, 0, .53f), MIX(NB_FX_DELAY, .2f), MIX(NB_FX_REVERB, .3f) },
      { MOD(NB_K_FOCUS, NB_SRC_FOLLOW, -.2f) } },
    /* SMOKE: everyone into everyone on the bar's partials, ring and wrap, MID drifting */
    { { .569f, .405f, .83f, .311f, .662f, .668f }, 4, 6, .2f, { 0 },
      { MIX(NB_FX_RING, .4f), FXK(NB_FX_RING, 2, .6f), MIX(NB_FX_WRAP, .4f) },
      { MOD(NB_K_MID, NB_SRC_RANDOM, .08f) } },
    /* DROUGHT: an op-amp rail to rail and the flip-flops, low and dry */
    { { .493f, .559f, .722f, .211f, .453f, 1.f }, 5, 1, .3f, { 0 },
      { MIX(NB_FX_BLAST, .7f), MIX(NB_FX_DIVIDE, .5f) }, { { 0 } } },
    /* SILL: a star in unison, the sweep and the pump in the loop */
    { { .423f, .234f, .70f, .411f, .836f, .514f }, 3, 0, 0.f, { 0 },
      { MIX(NB_FX_SWEEP, .6f), FXK(NB_FX_SWEEP, 0, .2f), MIX(NB_FX_PUMP, .5f), MIX(NB_FX_DELAY, .25f),
        MIX(NB_FX_REVERB, .2f) }, { { 0 } } },
    /* MILL: a starved supply and fuzz, everything crossed, grinding low */
    { { .869f, .756f, .888f, .178f, .291f, 1.f }, 4, 6, .5f, { 0 },
      { MIX(NB_FX_STARVE, .6f), MIX(NB_FX_FUZZ, .8f) }, { MOD(NB_K_CROSS, NB_SRC_LFO2, -.3f) } },
    /* HUSK: a low drone, each side two loops 5 Hz apart (and an octave over), trembling as they beat */
    { { 0.f, 0.f, .72f, .13f, .6f, .5f }, 1, 0, .05f, { 0 },
      { SET(T_PITCH(1), ST(12.f)), SET(T_PITCH(2), ST(1.84f)), SET(T_PITCH(3), ST(13.03f)), MIX(NB_FX_FUZZ, .3f),
        FXK(NB_FX_FUZZ, 1, .2f) }, { { 0 } } },
    /* NEEDLE: high fifths, piercing, a touch of plate */
    { { .2f, 0.f, .85f, .79f, .45f, .3f }, 0, 1, 0.f, { 0 },
      { SET(T_LEVEL(0), .6f), SET(T_LEVEL(1), .6f), SET(T_LEVEL(2), .6f), SET(T_LEVEL(3), .6f), MIX(NB_FX_REVERB, .2f) },
      { { 0 } } },
    /* MOTH: four loops a few cents apart and apart from each other, beating slowly */
    { { 0.f, 0.f, .68f, .40f, .7f, .5f }, 1, 0, 0.f, { 0 },
      { SET(T_LEVEL(2), .6f), SET(T_LEVEL(3), .6f), MIX(NB_FX_REVERB, .35f), FXK(NB_FX_REVERB, 0, .7f) },
      { { 0 } } },
    /* GRAVEL: a harsh wall: everything crossed, rail to rail, wrapped, ring-modulated by its own carrier */
    { { .7f, .6f, .85f, .26f, .35f, 1.f }, 4, 6, .4f, { 0 },
      { MIX(NB_FX_BLAST, .7f), FXK(NB_FX_BLAST, 1, .3f), MIX(NB_FX_WRAP, .5f), FXK(NB_FX_WRAP, 2, .5f),
        MIX(NB_FX_RING, .3f), FXK(NB_FX_RING, 0, 1.f), FXK(NB_FX_RING, 1, .6f) }, { { 0 } } },
    /* KITE: a whistle in unison, its pitch swaying, a wide room */
    { { .2f, 0.f, .78f, .68f, .6f, .2f }, 1, 0, 0.f, { .3f, 0, 0, 0 },
      { SET(T_LEVEL(0), .7f), SET(T_LEVEL(1), .7f), SET(T_LEVEL(2), .7f), SET(T_LEVEL(3), .7f), MIX(NB_FX_ECHO, .3f),
        FXK(NB_FX_ECHO, 2, .5f), MIX(NB_FX_DELAY, .25f), MIX(NB_FX_REVERB, .45f), FXK(NB_FX_REVERB, 0, .8f) },
      { MOD(NB_K_MID, NB_SRC_LFO1, .01f) } },
    /* GRIST: fuzz rectified and divided, a starved supply, grinding */
    { { .5f, .3f, .78f, .30f, .5f, .8f }, 5, 2, .2f, { 0 },
      { MIX(NB_FX_FUZZ, .7f), MIX(NB_FX_RECTIFY, .5f), MIX(NB_FX_DIVIDE, .4f), MIX(NB_FX_STARVE, .3f) },
      { MOD(NB_K_MID, NB_SRC_RANDOM, .03f) } },
    /* TIDE: a compressor in the loop and the band opening and closing, breathing */
    { { .3f, 0.f, .75f, .20f, .6f, .4f }, 3, 2, .4f, { .27f, 0, 0, 0 },
      { MIX(NB_FX_PUMP, .7f), FXK(NB_FX_PUMP, 0, .6f), FXK(NB_FX_PUMP, 1, .5f), FXK(NB_FX_PUMP, 2, 1.f) },
      { MOD(NB_K_FOCUS, NB_SRC_LFO1, -.5f), MOD(NB_T_HISS, NB_SRC_LFO1, .4f), MOD(NB_K_GAIN, NB_SRC_LFO1, .3f) } },
    /* LANTERN: a star on the bell, SCREAM and MID wandering, delay and plate */
    { { .4f, .45f, .72f, .35f, .7f, .4f }, 3, 5, 0.f, { 0, 0, .25f, 1.f },
      { MIX(NB_FX_ECHO, .3f), FXK(NB_FX_ECHO, 2, .6f), MIX(NB_FX_DELAY, .3f), FXK(NB_FX_DELAY, 1, .5f),
        MIX(NB_FX_REVERB, .4f) },
      { MOD(NB_K_MID, NB_SRC_RANDOM, .05f), MOD(NB_K_GRITO, NB_SRC_LFO1, .2f) } },
};

/* ---- the pads ---- */
static const uint8_t white_keys[16] = { 0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26 };
static const uint8_t black_keys[11] = { 1, 3, 5, 8, 10, 13, 15, 17, 20, 22, 25 };
static const uint8_t kick_mask[16] = { 1, 2, 4, 8, 1, 2, 4, 8, 3, 12, 5, 10, 15, 15, 15, 15 };
enum { G_CUT, G_OPEN, G_RETURN, G_CHOP32, G_CHOP16, G_CHOP8, G_CHOP4, G_SC4, G_SC8, G_SC16, G_SC32 };
static const char *const gate_names[11] = {
    "CUT", "OPEN", "RETURN", "CHOP 32", "CHOP 16", "CHOP 8", "CHOP 4",
    "SCATTER 4", "SCATTER 8", "SCATTER 16", "SCATTER 32"
};

const char *nb_snap_name(int i) { return (i >= 0 && i < NB_NSNAP) ? snap_names[i] : ""; }
const char *nb_topology_name(int i) { return (i >= 0 && i < NB_NTOPO) ? topo_names[i] : ""; }
const char *nb_partials_name(int i) { return (i >= 0 && i < NB_NPART) ? part_names[i] : ""; }
const char *nb_page_name(int i) { return (i >= 0 && i < NB_NPAGE) ? page_names[i] : ""; }
const char *nb_source_name(int i) { return (i >= 0 && i < NB_NSRC) ? src_names[i] : ""; }
const char *nb_fx_name(int i) { return (i >= 0 && i < NB_NFX) ? fx_names[i] : ""; }
int nb_topology_count(void) { return NB_NTOPO; }
int nb_partials_count(void) { return NB_NPART; }
const char *nb_pad_name(int key)
{
    for (int i = 0; i < 16; ++i) if (white_keys[i] == key) return "KICK";
    for (int i = 0; i < 11; ++i) if (black_keys[i] == key) return gate_names[i];
    return "";
}

/* the section buttons: row 1 FX SCL ENV LFO, row 2 HOME SAVE ARP SEQ */
static const uint8_t section_button[8] = { 2, 3, 4, 5, 8, 9, 10, 11 };
int nb_section_of(int page)
{
    if (page >= NB_PG_CH1 && page < NB_PG_CH1 + 4) return 1;
    if (page >= NB_PG_INTO1 && page < NB_PG_INTO1 + 4) return 2;
    if (page == NB_PG_SHAPE) return 3;
    if (page >= NB_PG_FX1 && page < NB_PG_FX1 + NB_NLOOPFX) return 4;
    if (page == NB_PG_DELAY) return 5;
    if (page == NB_PG_REVERB) return 6;
    if (page == NB_PG_MOD) return 7;
    return 0;                                   /* LOOP, and SETUP (from STORE) */
}
/* the three fixed encoders (SDK order 0, 1, 6) */
static const int8_t enc_fixed[7] = { NB_K_CROSS, NB_K_GRITO, -1, -1, -1, -1, NB_K_GAIN };

float nb_knob_value(int knob, float x)
{
    x = nb_clamp(x, 0.f, 1.f);
    switch (knob) {
    case NB_K_GAIN:  return nb_table17(nb_curve_gain, x);
    case NB_K_MID:   return 20.f * nb_exp2(nb_table17(nb_curve_mid, x) * 9.f);   /* 20 Hz .. 10 kHz */
    case NB_K_FOCUS: return nb_table17(nb_curve_focus, x);
    case NB_K_DRIVE: return nb_table17(nb_curve_drive, x);
    case NB_K_CROSS: return nb_table17(nb_curve_cross, x);
    case NB_K_GRITO: return nb_table17(nb_curve_grito, x);
    }
    return x;
}

/* ---- the flat list of positions (a modulation and a motion lane point at one) ---- */
static float *target_ptr(nb_snap *s, int t)
{
    if (t < 6) return &s->pos[t];
    if (t < 10) return &s->ch_gain[t - 6];
    if (t < 14) return &s->ch_pitch[t - 10];
    if (t < 18) return &s->ch_mid[t - 14];
    if (t < 22) return &s->ch_level[t - 18];
    if (t < 38) return &s->send[(t - 22) >> 2][(t - 22) & 3];
    if (t == NB_T_HISS) return &s->hiss;
    t -= NB_T_FX;
    return &s->fx[t >> 2][t & 3];
}

static float rate_hz(float pos) { return 0.05f * nb_exp2(pos * 8.64f); }   /* 0.05 .. 20 Hz */

static void load_topology(nb_snap *s, int t)
{
    s->topology = (uint8_t)t;
    for (int i = 0; i < NB_CH; ++i)
        for (int j = 0; j < NB_CH; ++j)
            if (i != j) s->send[i][j] = 0.5f + 0.5f * topo[t][i][j];
}

static void factory_snap(nb_snap *s, int i)
{
    static const float rate_default[4] = { .35f, .5f, .4f, .5f };
    const factory_t *f = &factory[i];
    uint8_t *b = (uint8_t *)s;
    for (unsigned k = 0; k < sizeof *s; ++k) b[k] = 0;
    for (int k = 0; k < NB_NKNOB; ++k) s->pos[k] = f->pos[k];
    for (int c = 0; c < NB_CH; ++c) {
        s->ch_gain[c] = 2.f / 3.f; s->ch_pitch[c] = .5f; s->ch_mid[c] = .5f; s->ch_level[c] = 1.f;
        s->send[c][c] = .5f;
    }
    load_topology(s, f->topology);
    s->partials = f->partials; s->hiss = f->hiss;
    for (int e = 0; e < NB_NFX; ++e) for (int k = 0; k < 4; ++k) s->fx[e][k] = fx_rest[e][k];
    for (int k = 0; k < 14 && f->set[k].t1; ++k) *target_ptr(s, f->set[k].t1 - 1) = f->set[k].v;
    for (int k = 0; k < 4; ++k) s->mod_rate[k] = f->rate[k] > 0.f ? f->rate[k] : rate_default[k];
    for (int m = 0; m < NB_NMOD; ++m) s->mod_t[m] = 0xff;
    for (int m = 0; m < 3 && f->mod[m].t1; ++m) {
        s->mod_t[m] = (uint8_t)(f->mod[m].t1 - 1); s->mod_s[m] = f->mod[m].src; s->mod_d[m] = f->mod[m].d;
    }
    for (int l = 0; l < NB_LANES; ++l) s->lane_target[l] = 0xff;
}

/* ---- keeping: the sounds packed into two banks of eight, one sector each, each with its check ---- */
static uint32_t store_check(const nb_store *s)
{
    const uint8_t *b = (const uint8_t *)&s->bank, *end = (const uint8_t *)&s->check;
    uint32_t h = 2166136261u;
    for (; b < end; ++b) { h ^= *b; h *= 16777619u; }
    return h;
}

static void pack(const nb_snap *s, nb_snap_packed *o)
{
    for (int t = 0; t < NB_NTARGET; ++t)
        o->v[t] = (uint16_t)(nb_clamp(*target_ptr((nb_snap *)s, t), 0.f, 1.f) * 65535.f + .5f);
    for (int m = 0; m < NB_NMOD; ++m) {
        o->mod_t[m] = s->mod_t[m]; o->mod_s[m] = s->mod_s[m];
        float d = nb_clamp(s->mod_d[m], -1.f, 1.f) * 127.f;
        o->mod_d[m] = (int8_t)(d < 0.f ? d - .5f : d + .5f);
    }
    for (int k = 0; k < 4; ++k) o->rate[k] = (uint8_t)(nb_clamp(s->mod_rate[k], 0.f, 1.f) * 255.f + .5f);
    for (int l = 0; l < NB_LANES; ++l) {
        o->lane_target[l] = s->lane_target[l];
        for (int i = 0; i < NB_LANE_PTS; ++i) o->lane[l][i] = s->lane[l][i];
    }
    o->motion_ms = s->motion_ms;
    o->topology = s->topology; o->partials = s->partials;
}

static void unpack(const nb_snap_packed *o, nb_snap *s)
{
    for (int t = 0; t < NB_NTARGET; ++t) *target_ptr(s, t) = (float)o->v[t] * (1.f / 65535.f);
    for (int m = 0; m < NB_NMOD; ++m) {
        s->mod_t[m] = o->mod_t[m] < NB_NTARGET ? o->mod_t[m] : 0xff;
        s->mod_s[m] = o->mod_s[m] < NB_NSRC ? o->mod_s[m] : 0;
        s->mod_d[m] = (float)o->mod_d[m] * (1.f / 127.f);
    }
    for (int k = 0; k < 4; ++k) s->mod_rate[k] = (float)o->rate[k] * (1.f / 255.f);
    for (int l = 0; l < NB_LANES; ++l) {
        s->lane_target[l] = o->lane_target[l] < NB_NTARGET ? o->lane_target[l] : 0xff;
        for (int i = 0; i < NB_LANE_PTS; ++i) s->lane[l][i] = o->lane[l][i];
    }
    s->motion_ms = o->motion_ms > 8000u ? 8000u : o->motion_ms;
    s->topology = o->topology < NB_NTOPO ? o->topology : 0;
    s->partials = o->partials < NB_NPART ? o->partials : 2;
}

/* the banks given as bits packed again (bit b: memories b * 8 .. + 7; the setup goes with bank 0) */
static void store_commit(nb_panel *p, int banks)
{
    for (int b = 0; b < NB_NBANK; ++b) {
        if (!((banks >> b) & 1)) continue;
        nb_store *s = &p->store[b];
        uint8_t *z = (uint8_t *)s;
        for (unsigned k = 0; k < sizeof *s; ++k) z[k] = 0;     /* the padding too: the check reads every byte */
        s->magic = NB_STORE_MAGIC;
        s->bank = (uint32_t)b;
        for (int i = 0; i < NB_BANK_SNAPS; ++i) pack(&p->snaps[b * NB_BANK_SNAPS + i], &s->snap[i]);
        if (b == 0) {
            s->setup.midi_ch = p->setup_midi_ch;
            s->setup.bright = (uint16_t)(nb_clamp(p->setup_bright, 0.f, 1.f) * 65535.f + .5f);
        }
        s->check = store_check(s);
    }
    p->store_dirty |= banks;
}

static void recall(nb_panel *p, int i)
{
    p->cur = p->snaps[i];
    p->snap_index = i;
    p->motion_t0 = p->last_ms;
    p->morph_target = -1;
}

/* ---- the screen's message: what was just touched, big ---- */
static void str_set(char *d, const char *s, int n)
{
    int i = 0;
    for (; s && s[i] && i < n - 1; ++i) d[i] = s[i];
    d[i] = 0;
}
static int str_add(char *d, int at, const char *s, int n)
{
    for (int i = 0; s[i] && at < n - 1; ++i) d[at++] = s[i];
    d[at] = 0;
    return at;
}
static int str_len(const char *s) { int n = 0; while (s[n]) ++n; return n; }
/* a number with `dec` decimals and an optional sign, then a unit */
static void fmt_num(char *d, int n, float v, int dec, int sign, const char *unit)
{
    char b[24]; int k = 0;
    if (v < 0.f) { b[k++] = '-'; v = -v; } else if (sign && v > 0.f) b[k++] = '+';
    int scale = dec == 2 ? 100 : dec == 1 ? 10 : 1;
    int x = (int)(v * (float)scale + 0.5f);
    int ip = x / scale, fp = x % scale;
    char t[12]; int m = 0;
    do { t[m++] = (char)('0' + ip % 10); ip /= 10; } while (ip && m < 10);
    while (m) b[k++] = t[--m];
    if (dec) {
        b[k++] = '.';
        if (dec == 2) b[k++] = (char)('0' + fp / 10);
        b[k++] = (char)('0' + fp % 10);
    }
    b[k] = 0;
    int at = str_add(d, (d[0] = 0, 0), b, n);
    if (unit[0]) { at = str_add(d, at, " ", n); str_add(d, at, unit, n); }
}

static void hud_show(nb_panel *p, int kind, uint32_t ms)
{
    p->hud.kind = kind;
    p->hud.t0 = ms;
}

/* ---- the slots: what each encoder plays in this section ---- */
typedef struct { float *pos; int kind; int steps; int target; const char *label; } slot_t;

static const char *const ch_labels[4][4] = {
    { "GAIN 1", "PITCH 1", "MID 1", "LEVEL 1" }, { "GAIN 2", "PITCH 2", "MID 2", "LEVEL 2" },
    { "GAIN 3", "PITCH 3", "MID 3", "LEVEL 3" }, { "GAIN 4", "PITCH 4", "MID 4", "LEVEL 4" },
};
static const char *const from_labels[4] = { "FROM 1", "FROM 2", "FROM 3", "FROM 4" };
static const char *const knob_labels[NB_NKNOB] = { "CROSS", "SCREAM", "GAIN", "MID", "FOCUS", "DRIVE" };
static const char *const mod_labels[4] = { "LFO 1", "LFO 2", "RANDOM", "SMOOTH" };
static const char *const setup_labels[4] = { "MIDI CH", "BRIGHT", "", "" };
static const char *const shape_labels[4] = { "TOPOLOGY", "PARTIALS", "", "" };
static const int8_t loop_t[4] = { NB_K_MID, NB_K_FOCUS, NB_K_DRIVE, NB_T_HISS };
static const char *const loop_l[4] = { "MID", "FOCUS", "DRIVE", "HISS" };

static float step_pos(int idx, int n) { return n > 1 ? (float)idx / (float)(n - 1) : 0.f; }

static slot_t slot_of(nb_panel *p, int enc)
{
    slot_t s = { 0, NB_KIND_UNI, 0, -1, "" };
    nb_snap *c = &p->cur;
    if (enc_fixed[enc] >= 0) {
        s.target = enc_fixed[enc]; s.pos = &c->pos[s.target]; s.label = knob_labels[s.target];
        return s;
    }
    int k = enc - 2, pg = p->page;
    if (pg == NB_PG_LOOP) {
        s.target = loop_t[k]; s.pos = target_ptr(c, s.target); s.label = loop_l[k];
        return s;
    }
    if (pg >= NB_PG_CH1 && pg < NB_PG_CH1 + 4) {
        int ch = pg - NB_PG_CH1;
        s.target = 6 + k * 4 + ch;
        s.pos = target_ptr(c, s.target); s.label = ch_labels[ch][k];
        s.kind = (k == 1 || k == 2) ? NB_KIND_BI : NB_KIND_UNI;
        return s;
    }
    if (pg >= NB_PG_INTO1 && pg < NB_PG_INTO1 + 4) {
        int to = pg - NB_PG_INTO1;
        s.label = from_labels[k];
        if (k == to) { s.target = 6 + to; s.pos = &c->ch_gain[to]; return s; }   /* the diagonal: its own gain */
        s.target = 22 + to * 4 + k; s.pos = &c->send[to][k]; s.kind = NB_KIND_BI;
        return s;
    }
    if (pg == NB_PG_SHAPE) {
        s.label = shape_labels[k];
        if (k < 2) { s.kind = NB_KIND_STEP; s.steps = k == 0 ? NB_NTOPO : NB_NPART; }
        else s.kind = NB_KIND_NONE;
        return s;
    }
    if (pg >= NB_PG_FX1 && pg <= NB_PG_REVERB) {
        int f = pg - NB_PG_FX1;
        s.label = fx_knobs[f][k];
        s.target = NB_T_FX + f * 4 + k; s.pos = &c->fx[f][k];
        if (f == NB_FX_SHIFT && k == 0) s.kind = NB_KIND_BI;      /* SHIFT: down or up from the centre */
        return s;
    }
    if (pg == NB_PG_MOD) { s.label = mod_labels[k]; s.pos = &c->mod_rate[k]; return s; }
    s.label = setup_labels[k];                       /* SETUP */
    if (k == 0) { s.kind = NB_KIND_STEP; s.steps = 17; return s; }
    if (k == 1) { s.pos = &p->setup_bright; return s; }
    s.kind = NB_KIND_NONE;
    return s;
}

static int step_index(const nb_panel *p, int k)
{
    if (p->page == NB_PG_SETUP) return p->setup_midi_ch;
    return k == 0 ? p->cur.topology : p->cur.partials;
}
static void step_set(nb_panel *p, int k, int idx)
{
    if (p->page == NB_PG_SETUP) { p->setup_midi_ch = (uint8_t)idx; p->setup_dirty = 1; return; }
    if (k == 0) load_topology(&p->cur, idx);
    else p->cur.partials = (uint8_t)idx;
}

static int mod_find(const nb_snap *s, int target)
{
    for (int m = 0; m < NB_NMOD; ++m) if (s->mod_t[m] == target) return m;
    return -1;
}

const char *nb_slot_label(const nb_panel *p, int enc)
{
    if (enc < 0 || enc > 6) return "";
    return slot_of((nb_panel *)p, enc).label;
}
int nb_slot_kind(const nb_panel *p, int enc)
{
    if (enc < 0 || enc > 6) return 0;
    return slot_of((nb_panel *)p, enc).kind;
}
float nb_slot_value(const nb_panel *p, int enc)
{
    if (enc < 0 || enc > 6) return 0.f;
    slot_t s = slot_of((nb_panel *)p, enc);
    if (s.kind == NB_KIND_STEP) return step_pos(step_index(p, enc - 2), s.steps);
    if (s.kind == NB_KIND_NONE) return 0.f;
    return *s.pos;
}
float nb_slot_mod(const nb_panel *p, int enc)
{
    if (enc < 0 || enc > 6) return 0.f;
    slot_t s = slot_of((nb_panel *)p, enc);
    int m = s.target >= 0 ? mod_find(&p->cur, s.target) : -1;
    return m >= 0 ? p->cur.mod_d[m] : 0.f;
}

/* an effect knob as the sound has it */
static int fx_text(int f, int k, float v, char *out, int n)
{
    switch (f * 4 + k) {
    case NB_FX_FUZZ * 4 + 0:   fmt_num(out, n, 6.f + 54.f * v, 0, 1, "DB"); return 1;
    case NB_FX_BLAST * 4 + 0:  fmt_num(out, n, 6.f + 60.f * v, 0, 1, "DB"); return 1;
    case NB_FX_FOLD * 4 + 0:   fmt_num(out, n, 20.f * nb_log2(1.f + 15.f * v) * 0.30103f, 0, 1, "DB"); return 1;
    case NB_FX_WRAP * 4 + 0:   fmt_num(out, n, 20.f * nb_log2(1.f + 7.f * v) * 0.30103f, 0, 1, "DB"); return 1;
    case NB_FX_RING * 4 + 1:   fmt_num(out, n, 20.f * nb_exp2(v * 7.f), 0, 0, "HZ"); return 1;
    case NB_FX_SHIFT * 4 + 0: { float w = (v - .5f) * 2.f, hz = 400.f * w * nb_absf(w);
                                fmt_num(out, n, hz, nb_absf(hz) < 10.f ? 1 : 0, 1, "HZ"); return 1; }
    case NB_FX_ECHO * 4 + 0:   fmt_num(out, n, 2.f + 25.f * v, 1, 0, "MS"); return 1;
    case NB_FX_PUMP * 4 + 1:   fmt_num(out, n, 0.2f + 20.f * v, 1, 0, "MS"); return 1;
    case NB_FX_PUMP * 4 + 2:   fmt_num(out, n, 20.f + 780.f * v, 0, 0, "MS"); return 1;
    case NB_FX_SWEEP * 4 + 1:  fmt_num(out, n, 80.f * nb_exp2(v * 4.6f), 0, 0, "HZ"); return 1;
    case NB_FX_STARVE * 4 + 2: fmt_num(out, n, 5.f + 295.f * v, 0, 0, "MS"); return 1;
    case NB_FX_DELAY * 4 + 0:  fmt_num(out, n, 10.f + 110.f * v, 0, 0, "MS"); return 1;
    }
    return 0;
}

/* the value as the sound has it: Hz, semitones, octaves, dB, the gain, a name */
static void slot_text(nb_panel *p, int enc, char *out, int n, float *bar, int *bip)
{
    slot_t s = slot_of(p, enc);
    const nb_params *q = &p->params;
    *bip = s.kind == NB_KIND_BI;
    if (s.kind == NB_KIND_NONE) { out[0] = 0; *bar = -1.f; return; }
    if (s.kind == NB_KIND_STEP) {
        int k = enc - 2, i = step_index(p, k);
        if (p->page == NB_PG_SETUP) {
            if (i == 0) str_set(out, "ANY", n); else fmt_num(out, n, (float)i, 0, 0, "");
        } else str_set(out, k == 0 ? topo_names[i] : part_names[i], n);
        *bar = step_pos(i, s.steps);
        return;
    }
    float v = *s.pos;
    *bar = v;
    int pg = p->page, k = enc - 2;
    if (enc_fixed[enc] == NB_K_GAIN) { fmt_num(out, n, q->gain, 2, 0, "X"); return; }
    if (enc_fixed[enc] < 0 && pg == NB_PG_LOOP && k == 0) { fmt_num(out, n, q->mid_hz, 0, 0, "HZ"); return; }
    if (enc_fixed[enc] < 0 && pg >= NB_PG_CH1 && pg < NB_PG_CH1 + 4) {
        int ch = pg - NB_PG_CH1;
        if (k == 0) { fmt_num(out, n, q->gain * q->ch_gain[ch], 2, 0, "X"); return; }
        if (k == 1) { fmt_num(out, n, q->ch_pitch[ch], 1, 1, "ST"); return; }
        if (k == 2) { fmt_num(out, n, q->ch_mid[ch], 1, 1, "OCT"); return; }
    }
    if (enc_fixed[enc] < 0 && pg >= NB_PG_INTO1 && pg < NB_PG_INTO1 + 4 && k == pg - NB_PG_INTO1) {
        fmt_num(out, n, q->gain * q->ch_gain[k], 2, 0, "X");
        return;
    }
    if (enc_fixed[enc] < 0 && pg >= NB_PG_FX1 && pg <= NB_PG_REVERB && fx_text(pg - NB_PG_FX1, k, v, out, n)) return;
    if (enc_fixed[enc] < 0 && pg == NB_PG_MOD) {
        if (k < 3) { float hz = rate_hz(v); fmt_num(out, n, hz, hz < 10.f ? 2 : 1, 0, "HZ"); }
        else fmt_num(out, n, v * 100.f, 0, 0, "%");
        return;
    }
    if (enc_fixed[enc] < 0 && pg == NB_PG_SETUP) { fmt_num(out, n, 25.f + 75.f * v, 0, 0, "%"); return; }
    if (*bip) { fmt_num(out, n, (v - 0.5f) * 200.f, 0, 1, "%"); return; }
    fmt_num(out, n, v * 100.f, 0, 0, "%");
}

static void hud_knob(nb_panel *p, int enc, uint32_t ms)
{
    str_set(p->hud.title, slot_of(p, enc).label, sizeof p->hud.title);
    p->hud.enc = enc;
    hud_show(p, NB_HUD_KNOB, ms);
}
static void hud_text(nb_panel *p, const char *title, const char *value, uint32_t ms)
{
    str_set(p->hud.title, title, sizeof p->hud.title);
    str_set(p->hud.value, value, sizeof p->hud.value);
    p->hud.bar = -1.f;
    hud_show(p, NB_HUD_TEXT, ms);
}
/* MOD held and a knob turned: which source moves it, and how much */
static void hud_mod(nb_panel *p, int enc, uint32_t ms)
{
    slot_t s = slot_of(p, enc);
    char t[16] = "LFO ";
    str_add(t, 4, s.label, sizeof t);
    str_set(p->hud.title, t, sizeof p->hud.title);
    int m = s.target >= 0 ? mod_find(&p->cur, s.target) : -1;
    float d = m >= 0 ? p->cur.mod_d[m] : 0.f;
    char v[16];
    if (m < 0 && s.target >= 0 && mod_find(&p->cur, 0xff) < 0) str_set(v, "FULL", sizeof v);
    else str_set(v, d == 0.f ? "OFF" : src_names[m >= 0 ? p->cur.mod_s[m] : p->mod_sel], sizeof v);
    if (d != 0.f) {
        char num[12];
        fmt_num(num, sizeof num, d * 100.f, 0, 1, "%");
        int at = str_add(v, str_len(v), " ", sizeof v);
        str_add(v, at, num, sizeof v);
    }
    str_set(p->hud.value, v, sizeof p->hud.value);
    p->hud.bar = 0.5f + 0.5f * d;
    p->hud.bipolar = 1;
    p->hud.enc = -1;
    hud_show(p, NB_HUD_KNOB, ms);
}

void nb_panel_init(nb_panel *p)
{
    uint8_t *b = (uint8_t *)p;
    for (unsigned i = 0; i < sizeof *p; ++i) b[i] = 0;
    for (int i = 0; i < NB_NSNAP; ++i) factory_snap(&p->snaps[i], i);
    p->setup_midi_ch = 0;
    p->setup_bright = 1.f;
    store_commit(p, (1 << NB_NBANK) - 1);
    p->store_dirty = 0;
    p->last_gate_key = -1;
    p->splash = 1;
    p->level = 0.7f;
    p->focus = 2;               /* K1 */
    p->prev_pot = 0xffffu;
    p->midi_level = -1;
    p->morph_target = -1;
    p->rng = 0x2545f491u;
    p->rnd_t = 1.f;
    recall(p, 0);
}

void nb_panel_load(nb_panel *p, int bank, const void *blob, unsigned bytes)
{
    if (!blob || bank < 0 || bank >= NB_NBANK || bytes != sizeof(nb_store)) return;
    static nb_store s;
    const uint8_t *src = (const uint8_t *)blob;
    uint8_t *dst = (uint8_t *)&s;
    for (unsigned i = 0; i < sizeof s; ++i) dst[i] = src[i];
    if (s.magic != NB_STORE_MAGIC || s.bank != (uint32_t)bank || s.check != store_check(&s)) return;
    p->store[bank] = s;
    for (int i = 0; i < NB_BANK_SNAPS; ++i) unpack(&s.snap[i], &p->snaps[bank * NB_BANK_SNAPS + i]);
    if (bank == 0) {
        p->setup_midi_ch = s.setup.midi_ch <= 16 ? s.setup.midi_ch : 0;
        p->setup_bright = (float)s.setup.bright * (1.f / 65535.f);
    }
    recall(p, p->snap_index);
}

int nb_panel_take_store(nb_panel *p)
{
    int d = p->store_dirty;
    p->store_dirty = 0;
    return d;
}

static int key_index(const uint8_t *list, int n, int key)
{
    for (int i = 0; i < n; ++i) if (list[i] == key) return i;
    return -1;
}

/* the step follows how fast the knob turns (detents a second, whatever the frame rate): turned slowly a detent is
 * 1/320 of the travel (a third of a semitone on MID's nine octaves), and the step grows smoothly with the speed up
 * to 1/24, so a whole sweep stays a quick turn */
static float turn_step(nb_panel *p, int enc, int d, uint32_t ms)
{
    int a = d < 0 ? -d : d;
    uint32_t dt = ms - p->turn_ms[enc];
    p->turn_ms[enc] = ms;
    if (dt < 4u) dt = 4u;
    if (dt > 1000u) dt = 1000u;
    float r = (float)a * (1000.f / 12.f) / (float)dt;  /* the speed over 12 detents a second */
    float step = (1.f / 320.f) * (1.f + r * r);
    return step > 1.f / 24.f ? 1.f / 24.f : step;
}

static void slot_turn(nb_panel *p, int enc, int d, uint32_t ms)
{
    slot_t s = slot_of(p, enc);
    if (s.kind == NB_KIND_NONE) return;
    if (s.kind == NB_KIND_STEP) {
        int k = enc - 2, i = step_index(p, k) + (d > 0 ? 1 : -1);
        if (i < 0) i = 0;
        if (i >= s.steps) i = s.steps - 1;
        step_set(p, k, i);
        return;
    }
    float step = turn_step(p, enc, d, ms);
    float v = *s.pos + (float)d * step;
    /* a bipolar knob rests a moment at its centre */
    if (s.kind == NB_KIND_BI && (*s.pos - 0.5f) * (v - 0.5f) < 0.f) v = 0.5f;
    *s.pos = nb_clamp(v, 0.f, 1.f);
    if (p->page == NB_PG_SETUP && enc_fixed[enc] < 0) p->setup_dirty = 1;
}

static void slot_set(nb_panel *p, int enc, float x)
{
    slot_t s = slot_of(p, enc);
    if (s.kind == NB_KIND_NONE) return;
    if (s.kind == NB_KIND_STEP) {
        int i = (int)(x * (float)(s.steps - 1) + 0.5f);
        step_set(p, enc - 2, i);
        return;
    }
    *s.pos = nb_clamp(x, 0.f, 1.f);
    if (p->page == NB_PG_SETUP && enc_fixed[enc] < 0) p->setup_dirty = 1;
}

void nb_panel_set_slot(nb_panel *p, int enc, float x, uint32_t ms)
{
    if (enc < 0 || enc > 6) return;
    p->focus = enc;
    slot_set(p, enc, x);
    hud_knob(p, enc, ms);
}

void nb_panel_midi(nb_panel *p, const uint8_t *m, unsigned n)
{
    if (!m || n < 2) return;
    unsigned st = m[0] & 0xf0u, d1 = m[1] & 127u, d2 = n > 2 ? (m[2] & 127u) : 0u;
    unsigned ch = p->setup_midi_ch;
    if (ch && (unsigned)(m[0] & 15u) != ch - 1u) return;      /* SETUP: one channel, or any */
    if (st == 0x90u && d2 == 0u) st = 0x80u;
    if (st == 0x90u || st == 0x80u) {
        int on = st == 0x90u;
        if (d1 >= 53u && d1 <= 79u) {
            uint32_t bit = 1u << (d1 - 53u);
            p->midi_keys = on ? (p->midi_keys | bit) : (p->midi_keys & ~bit);
        } else if (d1 >= 36u && d1 <= 49u) {
            uint32_t bit = 1u << (d1 - 36u);
            p->midi_buttons = on ? (p->midi_buttons | bit) : (p->midi_buttons & ~bit);
        }
        return;
    }
    if (st == 0xb0u && n > 2) {
        static const int8_t cc_enc[7] = { 2, 3, 4, 5, 0, 1, 6 };   /* CC 20.. -> K1..K4, CROSS, SCREAM, GAIN */
        if (d1 >= 20u && d1 <= 26u) {
            p->midi_cc_enc = cc_enc[d1 - 20u];
            p->midi_cc_val = (float)d2 / 127.f;
            p->midi_cc_pending = 1;
        } else if (d1 == 7u) p->midi_level = (int)d2;
        else if (d1 == 123u || d1 == 120u) { p->midi_keys = 0; p->midi_buttons = 0; }   /* all notes off */
    }
}

/* ---- MOD: the four sources, advanced by the control clock ---- */
static float frand(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return (float)(x >> 8) * (1.f / 16777216.f) * 2.f - 1.f;
}

static void mod_advance(nb_panel *p, float dt, float follow)
{
    const nb_snap *c = &p->cur;
    for (int i = 0; i < 2; ++i) {
        p->lfo_ph[i] += rate_hz(c->mod_rate[i]) * dt;
        p->lfo_ph[i] -= (float)(int)p->lfo_ph[i];
    }
    p->mod_val[NB_SRC_LFO1] = 4.f * nb_absf(p->lfo_ph[0] - 0.5f) - 1.f;                 /* triangle */
    float sq = p->lfo_ph[1] < 0.5f ? 1.f : -1.f;                                         /* square, its edge */
    p->mod_val[NB_SRC_LFO2] += (sq - p->mod_val[NB_SRC_LFO2]) * nb_clamp(dt / 0.002f, 0.f, 1.f);  /* softened */
    p->rnd_t += rate_hz(c->mod_rate[2]) * dt;
    if (p->rnd_t >= 1.f) {
        p->rnd_t -= (float)(int)p->rnd_t;
        p->rnd_from = p->mod_val[NB_SRC_RANDOM];
        p->rnd_to = frand(&p->rng);
    }
    /* SMOOTH: 0 a step, 1 a glide over the whole period (a slow DRIFT) */
    float sm = c->mod_rate[3], u = sm > 0.001f ? nb_clamp(p->rnd_t / sm, 0.f, 1.f) : 1.f;
    u = u * u * (3.f - 2.f * u);
    p->mod_val[NB_SRC_RANDOM] = p->rnd_from + (p->rnd_to - p->rnd_from) * u;
    p->mod_val[NB_SRC_FOLLOW] = nb_clamp(follow * 2.5f, 0.f, 1.f);
}

/* ---- MOTION ---- */
static int lane_for(nb_snap *s, int target, int alloc)
{
    for (int l = 0; l < NB_LANES; ++l) if (s->lane_target[l] == target) return l;
    if (!alloc) return -1;
    for (int l = 0; l < NB_LANES; ++l) if (s->lane_target[l] == 0xff) { s->lane_target[l] = (uint8_t)target; return l; }
    return -1;
}

static float lane_read(const uint8_t *lane, float phase)
{
    float x = phase * (float)NB_LANE_PTS;
    int i = (int)x;
    float f = x - (float)i;
    int a = i % NB_LANE_PTS, b = (i + 1) % NB_LANE_PTS;
    return ((float)lane[a] + ((float)lane[b] - (float)lane[a]) * f) * (1.f / 255.f);
}

/* while recording, where in the lane this moment falls: the loop's own length, or an 8 s grid for a new loop */
static int rec_index(const nb_panel *p, uint32_t ms)
{
    uint32_t len = p->cur.motion_ms, t;
    if (len) { t = (ms - p->motion_t0) % len; return (int)((uint64_t)t * NB_LANE_PTS / len); }
    t = ms - p->rec_t0;
    if (t > 7999u) t = 7999u;
    return (int)((uint64_t)t * NB_LANE_PTS / 8000u);
}

static void motion_touch(nb_panel *p, int target, float before)
{
    int l = lane_for(&p->cur, target, 1);
    if (l < 0) return;
    if (!p->rec_lane[l]) {
        p->rec_lane[l] = 1;
        uint8_t v = (uint8_t)(nb_clamp(before, 0.f, 1.f) * 255.f + .5f);
        for (int i = 0; i < NB_LANE_PTS; ++i) p->cur.lane[l][i] = v;
    }
    p->rec_used = 1;
}

static void motion_end(nb_panel *p, uint32_t ms)
{
    p->rec = 0;
    uint32_t held = ms - p->rec_t0;
    if (!p->rec_used) {
        if (held < 400u) {                          /* a tap: the loop stops and is cleared */
            for (int l = 0; l < NB_LANES; ++l) p->cur.lane_target[l] = 0xff;
            p->cur.motion_ms = 0;
        }
        return;
    }
    if (!p->cur.motion_ms) {
        /* a new loop: its length is how long MOTION was held; the 8 s grid squeezed to it */
        uint32_t len = held < 250u ? 250u : held > 8000u ? 8000u : held;
        float f = (float)len / 8000.f;
        for (int l = 0; l < NB_LANES; ++l) {
            if (!p->rec_lane[l]) continue;
            uint8_t tmp[NB_LANE_PTS];
            for (int i = 0; i < NB_LANE_PTS; ++i) {
                float x = (float)i * f;
                int a = (int)x, b = a + 1 < NB_LANE_PTS ? a + 1 : a;
                float w = x - (float)a;
                tmp[i] = (uint8_t)((float)p->cur.lane[l][a] + ((float)p->cur.lane[l][b] - (float)p->cur.lane[l][a]) * w + .5f);
            }
            for (int i = 0; i < NB_LANE_PTS; ++i) p->cur.lane[l][i] = tmp[i];
        }
        p->cur.motion_ms = (uint16_t)len;
        p->motion_t0 = ms;
    }
    for (int l = 0; l < NB_LANES; ++l) p->rec_lane[l] = 0;
}

static void motion_run(nb_panel *p, uint32_t ms)
{
    nb_snap *c = &p->cur;
    if (p->rec) {
        int idx = rec_index(p, ms);
        for (int l = 0; l < NB_LANES; ++l)
            if (p->rec_lane[l] && c->lane_target[l] != 0xff)
                c->lane[l][idx] = (uint8_t)(nb_clamp(*target_ptr(c, c->lane_target[l]), 0.f, 1.f) * 255.f + .5f);
    }
    if (!c->motion_ms) return;
    float phase = (float)((ms - p->motion_t0) % c->motion_ms) / (float)c->motion_ms;
    for (int l = 0; l < NB_LANES; ++l) {
        if (c->lane_target[l] == 0xff || (p->rec && p->rec_lane[l])) continue;
        *target_ptr(c, c->lane_target[l]) = lane_read(c->lane[l], phase);
    }
}

/* ---- MORPH: from where it started toward a memory, x 0..1 ---- */
static void morph_apply(nb_panel *p, float x)
{
    nb_snap *a = &p->morph_a, *b = &p->snaps[p->morph_target], *c = &p->cur;
    for (int t = 0; t < NB_NTARGET; ++t) *target_ptr(c, t) = *target_ptr(a, t) + (*target_ptr(b, t) - *target_ptr(a, t)) * x;
    for (int k = 0; k < 4; ++k) c->mod_rate[k] = a->mod_rate[k] + (b->mod_rate[k] - a->mod_rate[k]) * x;
    /* the lists, and the modulations, change where the nearer end changes */
    const nb_snap *near = x < 0.5f ? a : b;
    c->topology = near->topology;
    c->partials = near->partials;
    for (int m = 0; m < NB_NMOD; ++m) { c->mod_t[m] = near->mod_t[m]; c->mod_s[m] = near->mod_s[m]; c->mod_d[m] = near->mod_d[m]; }
}

static void go_page(nb_panel *p, int np, uint32_t ms)
{
    if (p->page == NB_PG_SETUP && np != NB_PG_SETUP && p->setup_dirty) { p->setup_dirty = 0; store_commit(p, 1); }
    if (np != p->page) p->hud.slide = 1;
    p->page = np;
    hud_show(p, NB_HUD_PAGE, ms);
}

void nb_panel_update(nb_panel *p, const nb_controls *c_in)
{
    nb_controls cc = *c_in;
    cc.keys |= p->midi_keys;
    cc.buttons |= p->midi_buttons;
    const nb_controls *c = &cc;
    uint32_t ms = c->ms;
    float dt = (float)(ms - p->last_ms) * 0.001f;
    if (dt < 0.f || dt > 0.1f) dt = 0.f;
    p->last_ms = ms;
    if (p->midi_cc_pending) { p->midi_cc_pending = 0; nb_panel_set_slot(p, p->midi_cc_enc, p->midi_cc_val, ms); }

    uint32_t bdown = c->buttons & ~p->prev_buttons;
    uint32_t bup = ~c->buttons & p->prev_buttons;
    uint32_t kdown = c->keys & ~p->prev_keys;
    int storing = (c->buttons >> NB_B_STORE) & 1u;
    int remembering = (c->buttons >> NB_B_MEMORY) & 1u;
    int ribbon = (c->buttons >> NB_B_RIBBON) & 1u;
    int modding = (c->buttons >> NB_B_MOD) & 1u;
    int fxing = (c->buttons >> NB_B_FX) & 1u;

    mod_advance(p, dt, c->follow);

    /* MOTION: held, the knobs are recorded */
    if (bdown & (1u << NB_B_MOTION)) {
        p->rec = 1; p->rec_used = 0; p->rec_t0 = ms;
        for (int l = 0; l < NB_LANES; ++l) p->rec_lane[l] = 0;
        hud_text(p, "MOTION", p->cur.motion_ms ? "OVERDUB" : "RECORD", ms);
    }
    if (bup & (1u << NB_B_MOTION)) {
        int had = p->cur.motion_ms != 0, used = p->rec_used;
        motion_end(p, ms);
        if (!used && had && !p->cur.motion_ms) hud_text(p, "MOTION", "CLEAR", ms);
        else if (used) {
            char v[16];
            fmt_num(v, sizeof v, (float)p->cur.motion_ms * 0.001f, 1, 0, "S");
            hud_text(p, "MOTION", v, ms);
        }
    }

    /* MOD: held, a turned knob gets a depth instead of a value; two taps clear everything */
    if (bdown & (1u << NB_B_MOD)) { p->mod_used = 0; hud_text(p, "LFO", src_names[p->mod_sel], ms); }
    if ((bup & (1u << NB_B_MOD)) && !p->mod_used) {
        if (ms - p->mod_tap_ms < 450u) {
            for (int m = 0; m < NB_NMOD; ++m) { p->cur.mod_t[m] = 0xff; p->cur.mod_d[m] = 0.f; }
            hud_text(p, "LFO", "CLEAR", ms);
            p->mod_tap_ms = 0;
        } else p->mod_tap_ms = ms;
    }

    /* the encoders */
    for (int e = 0; e < 7; ++e) {
        int d = c->enc[e];
        if (!d) continue;
        p->focus = e;
        slot_t s = slot_of(p, e);
        if (modding) {
            if (s.target >= 0) {
                int m = mod_find(&p->cur, s.target);
                if (m < 0) {
                    m = mod_find(&p->cur, 0xff);                  /* a free modulation */
                    if (m >= 0) { p->cur.mod_t[m] = (uint8_t)s.target; p->cur.mod_d[m] = 0.f; }
                }
                if (m >= 0) {
                    if (p->cur.mod_d[m] == 0.f) p->cur.mod_s[m] = (uint8_t)p->mod_sel;
                    float od = p->cur.mod_d[m], nd = od + (float)d * 2.f * turn_step(p, e, d, ms);   /* depth -1..1 */
                    if (od * nd < 0.f) nd = 0.f;                  /* it rests at zero on the way across */
                    p->cur.mod_d[m] = nb_clamp(nd, -1.f, 1.f);
                    if (p->cur.mod_d[m] == 0.f) p->cur.mod_t[m] = 0xff;   /* back to zero: the slot is free */
                }
            }
            p->mod_used = 1;
            hud_mod(p, e, ms);
            continue;
        }
        float before = s.pos ? *s.pos : 0.f;
        slot_turn(p, e, d, ms);
        if (p->rec && s.target >= 0) motion_touch(p, s.target, before);
        hud_knob(p, e, ms);
    }
    if (c->pot != p->prev_pot) {
        if (p->prev_pot == 0xffffu) p->prev_pot = c->pot;
        int dp = (int)c->pot - (int)p->prev_pot;
        if (dp > 4 || dp < -4 || p->hud.kind == NB_HUD_LEVEL) { hud_show(p, NB_HUD_LEVEL, ms); p->midi_level = -1; }
        p->prev_pot = c->pot;
    }
    if (p->midi_level >= 0) {
        if (p->hud.kind != NB_HUD_LEVEL || p->midi_level_shown != p->midi_level) {
            p->midi_level_shown = p->midi_level;
            hud_show(p, NB_HUD_LEVEL, ms);
        }
        p->level = (float)p->midi_level * (1.f / 127.f);
    } else p->level = (float)c->pot * (1.f / 1023.f);

    /* the section buttons: one touch, one section; CHAN, MATRIX and FX step on when touched again */
    for (int i = 0; i < 8; ++i) {
        int b = section_button[i];
        if (i == 4) continue;                               /* FX: decided on release (held, it picks by key) */
        if (!(bdown & (1u << b))) continue;
        int pg = p->page, np;
        if (i == 0) np = NB_PG_LOOP;
        else if (i == 1) {
            np = (pg >= NB_PG_CH1 && pg < NB_PG_CH1 + 4) ? NB_PG_CH1 + ((pg - NB_PG_CH1 + 1) & 3) : NB_PG_CH1 + p->last_ch;
            p->last_ch = np - NB_PG_CH1;
        } else if (i == 2) {
            np = (pg >= NB_PG_INTO1 && pg < NB_PG_INTO1 + 4) ? NB_PG_INTO1 + ((pg - NB_PG_INTO1 + 1) & 3) : NB_PG_INTO1 + p->last_into;
            p->last_into = np - NB_PG_INTO1;
        } else np = i == 3 ? NB_PG_SHAPE : i == 5 ? NB_PG_DELAY : i == 6 ? NB_PG_REVERB : NB_PG_MOD;
        go_page(p, np, ms);
    }
    if (bdown & (1u << NB_B_FX)) {
        p->fx_used = 0;
        if (!(p->page >= NB_PG_FX1 && p->page < NB_PG_FX1 + NB_NLOOPFX)) { go_page(p, NB_PG_FX1 + p->last_fx, ms); p->fx_used = 1; }
        else hud_show(p, NB_HUD_PAGE, ms);
    }
    if ((bup & (1u << NB_B_FX)) && !p->fx_used) {           /* touched again: the next effect */
        p->last_fx = (p->page - NB_PG_FX1 + 1) % NB_NLOOPFX;
        go_page(p, NB_PG_FX1 + p->last_fx, ms);
    }
    if (bdown & (1u << NB_B_DRONE)) {
        p->drone = !p->drone;
        /* the drone starts at once: a soft strike into the four loops, as a hand would give it */
        if (p->drone) { p->touch.kick_mask |= 15; p->touch.kick_color = 0.4f; }
    }
    if (bdown & (1u << NB_B_MEMORY)) hud_text(p, "MEMORY", snap_names[p->snap_index], ms);
    if (bdown & (1u << NB_B_STORE)) { p->store_used = 0; hud_text(p, "STORE", snap_names[p->snap_index], ms); }
    if ((bup & (1u << NB_B_STORE)) && !p->store_used)        /* a tap on STORE: SETUP, and back */
        go_page(p, p->page == NB_PG_SETUP ? NB_PG_LOOP : NB_PG_SETUP, ms);
    if (bdown & (1u << NB_B_RIBBON)) {
        if (p->morph_target >= 0) {
            char v[16] = "> ";
            str_add(v, 2, snap_names[p->morph_target], sizeof v);
            hud_text(p, "MORPH", v, ms);
        } else hud_knob(p, p->focus, ms);
    }
    /* the message stays on screen while MEMORY, STORE, FX, RIBBON or MOD is held, and its usual time after */
    int held = remembering || storing || ribbon || modding || fxing;
    if (p->hud_held && !held) { p->hud.t0 = ms; p->hud.slide = 0; }
    p->hud_held = held;

    /* the keys: a memory, an effect, a source, the ribbon (or MORPH), or the pads */
    nb_touch *t = &p->touch;
    t->out_cut = 0; t->ret_cut = 0; t->chop = 0; t->scatter_hz = 0.f;
    int kick_held = 0, open_held = 0;
    uint8_t mask = 0; float color = -1.f;
    if (remembering || storing) {
        /* the sixteen white keys are the memories: MEMORY recalls, STORE keeps, MEMORY + RIBBON picks where a
         * MORPH goes */
        for (int key = 0; key < NB_NKEY; ++key) {
            int i = key_index(white_keys, 16, key);
            if (!((kdown >> key) & 1u) || i < 0 || i >= NB_NSNAP) continue;
            if (storing) {
                p->snaps[i] = p->cur;
                p->snap_index = i;
                p->store_used = 1;
                store_commit(p, 1 << (i / NB_BANK_SNAPS));
                hud_text(p, "STORED", snap_names[i], ms);
            } else if (ribbon) {
                if (p->morph_target == i) { p->morph_target = -1; hud_text(p, "MORPH", "OFF", ms); }
                else {
                    p->morph_target = i;
                    p->morph_a = p->cur;
                    char v[16] = "> ";
                    str_add(v, 2, snap_names[i], sizeof v);
                    hud_text(p, "MORPH", v, ms);
                }
            } else {
                recall(p, i);
                hud_text(p, "MEMORY", snap_names[i], ms);
            }
        }
    } else if (fxing) {
        /* FX held: the first twelve white keys are the twelve effects, in the chain's order */
        for (int key = 0; key < NB_NKEY; ++key) {
            int i = key_index(white_keys, 16, key);
            if (!((kdown >> key) & 1u) || i < 0 || i >= NB_NLOOPFX) continue;
            p->last_fx = i; p->fx_used = 1;
            go_page(p, NB_PG_FX1 + i, ms);
        }
    } else if (modding) {
        for (int key = 0; key < NB_NKEY; ++key) {
            int w = key_index(white_keys, 16, key);
            if (!((kdown >> key) & 1u) || w < 0 || w >= NB_NSRC) continue;
            p->mod_sel = w; p->mod_used = 1;
            hud_text(p, "LFO", src_names[w], ms);
        }
    } else if (ribbon) {
        for (int key = 0; key < NB_NKEY; ++key) {
            if (!((kdown >> key) & 1u)) continue;
            float x = (float)key / 26.f;
            if (p->morph_target >= 0) {
                morph_apply(p, x);
                char v[16];
                fmt_num(v, sizeof v, x * 100.f, 0, 0, "%");
                str_set(p->hud.title, "MORPH", sizeof p->hud.title);
                str_set(p->hud.value, v, sizeof p->hud.value);
                p->hud.bar = x; p->hud.bipolar = 0; p->hud.enc = -1;
                hud_show(p, NB_HUD_KNOB, ms);
            } else {
                float before = nb_slot_value(p, p->focus);
                slot_set(p, p->focus, x);
                slot_t s = slot_of(p, p->focus);
                if (p->rec && s.target >= 0) motion_touch(p, s.target, before);
                hud_knob(p, p->focus, ms);
            }
        }
    } else {
        for (int key = 0; key < NB_NKEY; ++key) {
            int hold = (c->keys >> key) & 1u, hit = (kdown >> key) & 1u;
            int w = key_index(white_keys, 16, key);
            if (w >= 0) {
                if (hold) kick_held = 1;
                if (hit) { mask |= kick_mask[w]; color = (float)w / 15.f; }
                continue;
            }
            int g = key_index(black_keys, 11, key);
            if (g < 0 || !hold) continue;
            if (hit) p->last_gate_key = g;
            if (g == G_CUT) t->out_cut = 1;
            else if (g == G_OPEN) open_held = 1;
            else if (g == G_RETURN) t->ret_cut = 1;
        }
        /* one cut-up at a time: the gate pad pressed last, while it is held */
        int lg = p->last_gate_key;
        if (lg >= G_CHOP32 && ((c->keys >> black_keys[lg]) & 1u)) {
            if (lg <= G_CHOP4) t->chop = 32 >> (lg - G_CHOP32);
            else t->scatter_hz = (float)(4 << (lg - G_SC4));
        }
    }
    if (mask) { t->kick_mask |= mask; t->kick_color = color; }
    t->hold = p->drone || kick_held || open_held;

    motion_run(p, ms);

    /* what the loop hears: the positions with the modulation on them */
    p->eff = p->cur;
    for (int m = 0; m < NB_NMOD; ++m) {
        int tg = p->cur.mod_t[m];
        if (tg == 0xff || p->cur.mod_d[m] == 0.f) continue;
        float *v = target_ptr(&p->eff, tg);
        *v = nb_clamp(*v + p->cur.mod_d[m] * p->mod_val[p->cur.mod_s[m] < NB_NSRC ? p->cur.mod_s[m] : 0], 0.f, 1.f);
    }

    /* the values the loop plays */
    nb_params *q = &p->params;
    const nb_snap *s = &p->eff;
    q->gain = nb_knob_value(NB_K_GAIN, s->pos[NB_K_GAIN]);
    q->mid_hz = nb_knob_value(NB_K_MID, s->pos[NB_K_MID]);
    q->focus = nb_knob_value(NB_K_FOCUS, s->pos[NB_K_FOCUS]);
    q->drive = nb_knob_value(NB_K_DRIVE, s->pos[NB_K_DRIVE]);
    q->cross = nb_knob_value(NB_K_CROSS, s->pos[NB_K_CROSS]);
    q->grito = nb_knob_value(NB_K_GRITO, s->pos[NB_K_GRITO]);
    q->level = p->level;
    q->hiss = s->hiss;
    for (int f = 0; f < NB_NFX; ++f)
        for (int k = 0; k < 4; ++k) q->fx[f][k] = nb_table17(nb_curve_fx[f][k], s->fx[f][k]);   /* the measured curves */
    for (int i = 0; i < NB_CH; ++i) {
        q->ratio[i] = parts[s->partials][i];
        q->ch_gain[i] = s->ch_gain[i] * 1.5f;
        q->ch_pitch[i] = (s->ch_pitch[i] - 0.5f) * 48.f;
        q->ch_mid[i] = (s->ch_mid[i] - 0.5f) * 4.f;
        q->ch_level[i] = s->ch_level[i];
        for (int j = 0; j < NB_CH; ++j) q->matrix[i][j] = i == j ? 0.f : (s->send[i][j] - 0.5f) * 2.f;
    }

    /* the screen's message */
    nb_hud *h = &p->hud;
    if (h->kind == NB_HUD_KNOB && h->enc >= 0) {
        slot_text(p, h->enc, h->value, sizeof h->value, &h->bar, &h->bipolar);
        str_set(h->title, slot_of(p, h->enc).label, sizeof h->title);
    } else if (h->kind == NB_HUD_LEVEL) {
        str_set(h->title, "LEVEL", sizeof h->title);
        fmt_num(h->value, sizeof h->value, p->level * 100.f, 0, 0, "%");
        h->bar = p->level; h->bipolar = 0;
    } else if (h->kind == NB_HUD_PAGE) {
        str_set(h->title, page_names[p->page], sizeof h->title);
        for (int k = 0; k < 4; ++k) str_set(h->lines[k], slot_of(p, 2 + k).label, sizeof h->lines[k]);
    }
    if (p->splash) { p->splash = 0; h->kind = NB_HUD_SPLASH; h->t0 = ms; str_set(h->value, "FA\001SCA", sizeof h->value); }
    uint32_t age = ms - h->t0;
    h->age = age;
    if (age > 200u) h->slide = 0;
    h->drone = p->drone;
    h->motion = p->rec ? 2 : p->cur.motion_ms ? 1 : 0;
    h->motion_phase = p->cur.motion_ms ? (float)((ms - p->motion_t0) % p->cur.motion_ms) / (float)p->cur.motion_ms
                    : p->rec ? nb_clamp((float)(ms - p->rec_t0) / 8000.f, 0.f, 1.f) : 0.f;
    h->bright = 0.25f + 0.75f * nb_clamp(p->setup_bright, 0.f, 1.f);
    uint32_t hold = h->kind == NB_HUD_SPLASH ? 3500u : 1100u;
    h->alpha = h->kind == NB_HUD_NONE ? 0.f : age < hold ? 1.f : age < hold + 300u ? (float)(hold + 300u - age) / 300.f : 0.f;
    if (p->hud_held && h->kind != NB_HUD_NONE && h->kind != NB_HUD_SPLASH) h->alpha = 1.f;
    if (h->alpha <= 0.f) h->kind = NB_HUD_NONE;

    /* the LEDs: what is on */
    int any_mod = 0;
    for (int m = 0; m < NB_NMOD; ++m) if (p->cur.mod_t[m] != 0xff && p->cur.mod_d[m] != 0.f) { any_mod = 1; break; }
    for (int i = 0; i < NB_NLED; ++i) p->led[i] = 0;
    if (p->page != NB_PG_SETUP) p->led[section_button[nb_section_of(p->page)]] = 1;
    p->led[NB_B_DRONE] = (uint8_t)p->drone;
    p->led[NB_B_STORE] = (uint8_t)(storing || p->page == NB_PG_SETUP);
    p->led[NB_B_MEMORY] = (uint8_t)remembering;
    p->led[NB_B_RIBBON] = (uint8_t)(ribbon || p->morph_target >= 0);
    p->led[NB_B_MOD] = (uint8_t)(modding || any_mod);
    p->led[NB_B_MOTION] = (uint8_t)(p->rec ? ((ms >> 7) & 1u) : p->cur.motion_ms != 0);
    if (remembering || storing) {
        p->led[14 + white_keys[p->snap_index]] = 1;
        if (p->morph_target >= 0) p->led[14 + white_keys[p->morph_target]] = (uint8_t)((ms >> 8) & 1u);
    } else if (fxing) {
        for (int f = 0; f < NB_NLOOPFX; ++f) if (p->cur.fx[f][3] > 0.f) p->led[14 + white_keys[f]] = 1;   /* the effects that are in */
    } else if (modding) p->led[14 + white_keys[p->mod_sel]] = 1;
    else if (ribbon) {
        int k = p->morph_target >= 0 ? -1 : (int)(nb_slot_value(p, p->focus) * 26.f + 0.5f);
        if (k >= 0) p->led[14 + k] = 1;
    } else for (int key = 0; key < NB_NKEY; ++key) p->led[14 + key] = (uint8_t)((c->keys >> key) & 1u);

    p->prev_buttons = c->buttons;
    p->prev_keys = c->keys;
}
