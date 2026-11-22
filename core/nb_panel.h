/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Mario Baril
 * The instrument: the FM-1's controls turned into the loop network's values. One fixed structure ("it is what it
 * is"): every section is one button and four knobs, every effect is always in its place.
 *
 *   encoders  SELECT CROSS, ALGORITHM SCREAM, PRESETS GAIN (always); K1..K4 the section's four
 *   pot       the master level
 *   sections  FX LOOP, SCL CHAN (again: the next loop), ENV MATRIX (again: the next INTO), LFO SHAPE,
 *             HOME FX (again: the next effect; held + white key 1..12: that effect), SAVE DELAY, ARP REVERB, SEQ MOD
 *   buttons   OCT- MOTION (held: the knobs' gestures recorded in a loop; a tap clears),
 *             OCT+ RIBBON (held: the keys set the last knob touched),
 *             EDIT (free for now: a touch says the memory),
 *             GLO MOD (held + turn a knob: its modulation; + white key 1..4: the source; two taps: clear),
 *             PLAY DRONE (latch), REC STORE (held + white key 1..16: keep the sound there; a tap: SETUP)
 *   pages     LOOP (MID FOCUS DRIVE HISS), LOOP 1..4 (GAIN PITCH MID LEVEL), INTO 1..4 (FROM 1..4),
 *             SHAPE (TOPOLOGY PARTIALS), the twelve effects (four knobs each, the fourth MIX), DELAY, REVERB,
 *             MOD (LFO 1, LFO 2, RANDOM rates, SMOOTH), SETUP (MIDI CH, BRIGHT)
 *   keys      white: the 16 memories, direct (a tap recalls; held, the sound MORPHs toward it while held)
 *             black: NODE 1..4 (each strikes its loop; held, the return stays open), ALL, CUT, CHOP, SCATTER (held +
 *             K1: their rate), RETURN, OPEN, VIEW (docs/CONTROLES.md R3.8: no key does what another does) */
#ifndef NB_PANEL_H
#define NB_PANEL_H
#include <stdint.h>
#include "nb_engine.h"

enum { NB_K_CROSS, NB_K_GRITO, NB_K_GAIN, NB_K_MID, NB_K_FOCUS, NB_K_DRIVE, NB_NKNOB };
enum { NB_PG_LOOP, NB_PG_CH1, NB_PG_INTO1 = 5, NB_PG_SHAPE = 9, NB_PG_FX1 = 10,
       NB_PG_DELAY = NB_PG_FX1 + NB_NLOOPFX, NB_PG_REVERB, NB_PG_MOD, NB_PG_SETUP, NB_NPAGE };
enum { NB_KIND_UNI, NB_KIND_BI, NB_KIND_STEP, NB_KIND_NONE };
enum { NB_SRC_LFO1, NB_SRC_LFO2, NB_SRC_RANDOM, NB_SRC_FOLLOW, NB_NSRC };
#define NB_NSNAP 16             /* the memories, one per white key */
#define NB_NBANK 2              /* kept as two banks of eight: each fits one store object (3840 B) */
#define NB_BANK_SNAPS (NB_NSNAP / NB_NBANK)
#define NB_NKEY 27
#define NB_NBUTTON 14
#define NB_NLED 41
/* the positions a sound holds, one flat list (a modulation or a motion lane points at one):
 * 0..5 CROSS SCREAM GAIN MID FOCUS DRIVE, 6..9 GAIN n, 10..13 PITCH n, 14..17 MID n, 18..21 LEVEL n,
 * 22..37 the sends [to][from], 38 HISS, 39.. the effects' knobs [effect][4] (the twelve, DELAY, REVERB) */
#define NB_T_HISS 38
#define NB_T_FX 39
#define NB_NTARGET (NB_T_FX + NB_NFX * 4)
#define NB_LED_DIM 1
#define NB_LED_ON 2
#define NB_FX_BUDGET 85.f       /* the effects in may take the render up to this % of the device's budget (FX FULL) */
#define NB_NMOD 8               /* modulations per sound */
#define NB_LANES 6
#define NB_LANE_PTS 40

enum {
    NB_B_MOTION = 0, NB_B_RIBBON = 1, NB_B_MEMORY = 6, NB_B_MOD = 7, NB_B_FX = 8,
    NB_B_DRONE = 12, NB_B_STORE = 13
};

typedef struct {
    uint32_t keys;              /* bits 0..26, F3..G5 */
    uint32_t buttons;           /* bits 0..13 */
    int16_t enc[7];             /* detents since the last read, SDK order (SELECT, ALGORITHM, K1..K4, PRESETS) */
    uint16_t pot;               /* 0..1023 */
    uint32_t ms;                /* the control clock, milliseconds (wraps) */
    float follow;               /* the network's output level now, 0..1 (FOLLOW) */
    float loud[NB_CH];          /* each loop's level in the network (nb_view.loud): the node keys' lights; 0 unknown */
} nb_controls;

/* what the screen says big for a moment after a touch */
enum { NB_HUD_NONE, NB_HUD_KNOB, NB_HUD_TEXT, NB_HUD_PAGE, NB_HUD_LEVEL, NB_HUD_SPLASH };
typedef struct {
    int kind, enc, bipolar;
    uint32_t t0;
    float alpha;                /* 1 shown, fading to 0 */
    uint32_t age;               /* ms since shown (the page slides in) */
    int slide;                  /* the page changed: slide in */
    int drone;                  /* the DRONE latch, drawn as the ring through the lamps */
    int motion;                 /* 0 none, 1 playing, 2 recording (a mark at the top of the screen) */
    float motion_phase;         /* where the motion loop is, 0..1 */
    float bright;               /* the screen's brightness, 0.25..1 (SETUP) */
    float bar;                  /* 0..1, or < 0 for none */
    char title[16], value[16];
    char lines[4][12];          /* a page: the four knobs */
    /* the frame around everything (docs/TELA.md 5, 6): filled at every update */
    char page[12];              /* the page K1..K4 play now */
    char foot[16];              /* the memory's name, or the held pad's, or the view layer's action */
    uint8_t foot_hot;           /* foot holds a pad's or an action's words (else the footer meters the output) */
    uint8_t struck;             /* bit c: a held pad strikes loop c (its number lit) */
    uint8_t gating;             /* a gate pad held (CUT, RETURN, CHOP, SCATTER): the loops' numbers red while it acts */
    int8_t morph_to;            /* where a MORPH goes, the memory's index; -1 */
    uint8_t any_mod;            /* some modulation is on */
    int8_t turning;             /* 0..3: the K being turned now (its gauge lime); -1 */
    uint8_t kkind[4];           /* NB_KIND_* of K1..K4 */
    float kval[4];              /* their positions, 0..1 */
    float kmod[4];              /* the position as modulated now, or -1 */
    char words[4][12];          /* a step knob's value as a word (TOPOLOGY RING) */
    char knames[4][12], ktext[4][12];   /* K1..K4's names and values as the sound has them (220 HZ, +48 %) */
    uint16_t fx_in;             /* bit f: effect f in the chain (its MIX above 0) */
    uint16_t fx_cut;            /* bit f: in the sound, left out by the CPU budget (said FX LEFT OUT, never silent) */
    int8_t strip_n, strip_cur;  /* the page's or the card's strip of cells (effects, loops, memories, sources) */
    uint8_t strip_hot;          /* the current cell red (STORE) */
    uint8_t rows_text;          /* a page card whose rows are plain text (PERF), not knobs */
    int8_t focus_loop, into;    /* the loop in focus (LOOP n, INTO n, LISTEN n, PULL n, ISOLATE n); INTO n; -1 */
    char modline[16];           /* a knob's modulation: its source */
    float mod_lo, mod_hi, mod_now;   /* its range on the bar and where it is now; mod_now < 0: none */
    /* the stage's views (docs/TELA.md 13) and the keys that play them */
    uint8_t view;               /* NB_VIEW_* */
    uint8_t layer;              /* the view key held: the keys play the view */
    uint8_t view_flash;         /* the view just changed: its name where the page goes */
    int8_t listen, pull, iso;   /* -1, or the loop */
    uint8_t flip, freeze, bypass;
    int8_t spin;                /* SPIN steps taken, +-: the solid turns 30 degrees a step */
} nb_hud;

/* the stage's views, in the order a tap steps them; the first is the one after the splash (Mario: the oscilloscope) */
enum { NB_VIEW_SCOPE, NB_VIEW_LOOPS, NB_VIEW_MATRIX, NB_VIEW_CHAIN, NB_VIEW_WAVES, NB_VIEW_FREQ, NB_VIEW_TETRA, NB_NVIEW };
/* the view key (until the controls revision moves the layer to GLO): F#5, the last black key. A tap steps the views,
 * held + K1 steps them too, held + keys plays the view (white 1-4 LISTEN, 5-8 PULL, 9-12 ISOLATE; black 1 FLIP,
 * 2 FREEZE, 3 BYPASS, 4-5 SPIN), all momentary */
#define NB_KEY_VIEW 25

/* a sound, in RAM: every position (0..1), its modulations, its motion loop, the two lists */
typedef struct {
    float pos[NB_NKNOB];
    float ch_gain[NB_CH], ch_pitch[NB_CH], ch_mid[NB_CH], ch_level[NB_CH];
    float send[NB_CH][NB_CH];   /* [to][from]; 0.5 is no send */
    float hiss;
    float fx[NB_NFX][4];        /* each effect's four knobs; the fourth is its MIX */
    uint8_t mod_t[NB_NMOD];     /* the target of each modulation, 0xff empty */
    uint8_t mod_s[NB_NMOD];     /* its source */
    float mod_d[NB_NMOD];       /* its depth, -1..1 of the travel */
    float mod_rate[4];          /* LFO 1, LFO 2, RANDOM rates; RANDOM smooth (positions) */
    uint8_t lane_target[NB_LANES];        /* 0xff: empty */
    uint8_t lane[NB_LANES][NB_LANE_PTS];  /* positions, 0..255 */
    uint16_t motion_ms;                   /* the loop's length; 0 none */
    uint8_t topology, partials;
    uint8_t stage[NB_NFX + 1];            /* each effect's stage, then the network's (0 orthodox .. 2 heretic) */
    uint16_t fx_off;                      /* the effects switched off (bit f): out of the path, their MIX kept */
} nb_snap;

/* the few settings a dedicated instrument needs (page SETUP) */
typedef struct {
    uint8_t midi_ch;            /* 0 any channel, 1..16 */
    uint8_t transition;         /* TRANSITION, 0..255: 0..2 s (in an NB09 bank; older ones carried a zero pad here) */
    uint16_t bright;            /* 0..65535: 25..100 % */
} nb_setup;

/* a sound as kept (Flash, browser): packed so eight fit one 4 KB sector */
typedef struct {
    uint16_t v[NB_NTARGET];
    uint8_t mod_t[NB_NMOD], mod_s[NB_NMOD];
    int8_t mod_d[NB_NMOD];
    uint8_t rate[4];
    uint8_t lane_target[NB_LANES];
    uint8_t lane[NB_LANES][NB_LANE_PTS];
    uint16_t motion_ms;
    uint8_t topology, partials;
    uint8_t stage[4];           /* the 15 stages, 2 bits each (effect i at bits 2i of the 30) */
    uint16_t fx_off;            /* the effects switched off (bit f), their MIX kept */
} nb_snap_packed;

/* one bank: memories bank * 8 .. + 7, each bank with its own check (a bank that fails it keeps the factory sounds);
 * the setup is bank 0's (bank 1 carries zeros there) */
#define NB_STORE_MAGIC 0x4e423039u   /* "NB09": with the stages and the effects switched off */
#define NB_STORE_V8_BYTES 3792u      /* a bank kept with the stages, before the switch ("NB08"): it loads, every effect on */
#define NB_STORE_V7_BYTES 3760u      /* a bank kept by the build before the stages ("NB07"): it loads, all orthodox */
typedef struct {
    uint32_t magic;
    uint32_t bank;              /* 0 or 1 */
    nb_snap_packed snap[NB_BANK_SNAPS];
    nb_setup setup;
    uint32_t check;
} nb_store;

typedef struct {
    nb_snap cur;                /* what is played (the knobs write here) */
    nb_snap eff;                /* cur with the modulation applied: what the loop hears */
    nb_snap snaps[NB_NSNAP];
    nb_snap morph_a;            /* where a MORPH starts */
    nb_store store[NB_NBANK];   /* the packed copies the platform keeps, one per bank */
    float setup_bright;         /* SETUP's BRIGHT as a knob position */
    float setup_transition;     /* SETUP's TRANSITION as a knob position: 0..1 = 0..2 s (the engine's nb_params.transition) */
    float setup_seen_transition;
    uint8_t setup_midi_ch;
    int snap_index;
    int drone;
    int page, focus;            /* focus: the encoder touched last */
    int last_ch, last_into, last_fx, fx_used, store_used;
    uint32_t prev_keys, prev_buttons;
    uint16_t prev_pot;
    uint32_t midi_keys, midi_buttons;   /* what MIDI holds down, joined to the panel's own */
    int midi_level, midi_level_shown;   /* a level sent by MIDI (0..127) until the pot moves; -1 none */
    int midi_cc_enc, midi_cc_pending;
    float midi_cc_val;
    nb_hud hud;
    int last_gate_key;
    uint8_t chop_sel, scat_sel;     /* CHOP's crossings (32 16 8 4) and SCATTER's rate (4 8 16 32 a second), by index */
    int mem_key;                    /* the memory whose white key is held (-1 none), since mem_t0: a tap jumps, held MORPHs */
    int fxk;                        /* FX held: the effect whose white key is held (-1 none), since fxk_t0; fxk_done: */
    uint32_t fxk_t0;                /* ... its long press has switched it already */
    int fxk_done;
    uint32_t mem_t0;
    int splash;                 /* the name at power-on, until the first update has a clock */
    int hud_held;               /* MEMORY, STORE, FX, RIBBON, MOD held: the message stays */
    int setup_dirty;
    uint32_t setup_ms;          /* when SETUP was last changed: kept 1.5 s after, so a power-off on the page loses nothing */
    uint8_t setup_seen_ch;
    float setup_seen_bright;
    int store_dirty;            /* the banks to save, bit b: store[b] */
    int fx_full;                /* a MIX refused (over NB_FX_BUDGET): FX FULL said at the next update */
    /* MOD */
    int mod_sel, mod_used;
    uint32_t mod_tap_ms;
    float mod_val[NB_NSRC], lfo_ph[2], rnd_from, rnd_to, rnd_t;
    uint32_t rng, last_ms;
    uint32_t turn_ms[7];        /* when each encoder last turned: its speed sets the step */
    /* MOTION */
    int rec, rec_used;
    uint32_t rec_t0, motion_t0;
    uint8_t rec_lane[NB_LANES];  /* lanes written by this recording */
    /* the views */
    int view, layer_used, spin;
    uint32_t view_ms;
    int8_t listen, pull, iso;
    uint8_t flip, freeze, bypass;
    /* MORPH */
    int morph_target;
    float level;
    nb_params params;
    nb_touch touch;
    uint8_t led[NB_NLED];       /* 0 off, NB_LED_DIM, NB_LED_ON (the platform's two levels; blinking is the panel's) */
    uint16_t stored;            /* the memories holding a stored sound (not the factory's): their white keys dim */
    int edited;                 /* the sound played was changed since its recall or its store: its key breathes */
    int fx_full_key;            /* the effect FX FULL refused (its key flashes 600 ms from fx_full_ms), -1 none */
    uint16_t fx_cut, fx_cut_said;   /* the effects the budget leaves out of the sound now; those already said */
    uint32_t fx_full_ms;
} nb_panel;

void nb_panel_init(nb_panel *p);
/* Load one bank of stored sounds (a blob from the platform, sizeof(nb_store) bytes); ignored unless it is that bank
 * and valid (the factory sounds stay). */
void nb_panel_load(nb_panel *p, int bank, const void *blob, unsigned bytes);
/* A stage (0 orthodox, 1 heterodox, 2 heretic) for an effect (0..NB_NFX-1) or the network (NB_STAGE_NET): the minimal
 * hook; where it sits on the panel is docs/CONTROLES.md's to decide. The engine crossfades the change. */
void nb_panel_set_stage(nb_panel *p, int unit, int stage, uint32_t ms);
int nb_panel_stage(const nb_panel *p, int unit);
/* One read of the controls: updates params, touch and the LEDs. */
void nb_panel_update(nb_panel *p, const nb_controls *c);
/* One short MIDI message. The default map: notes 53..79 the 27 pads, notes 36..49 the 14 buttons,
 * CC 20..23 K1..K4 (the section's), CC 24 CROSS, 25 SCREAM, 26 GAIN, CC 7 LEVEL. Takes effect at the next update. */
void nb_panel_midi(nb_panel *p, const uint8_t *msg, unsigned bytes);
/* An encoder's slot set to an absolute position 0..1 (a MIDI CC, a ribbon). */
void nb_panel_set_slot(nb_panel *p, int enc, float x, uint32_t ms);
/* Once after a STORE or a SETUP change, the banks to save as bits (bit b: p->store[b], sizeof(nb_store) bytes);
 * 0 when nothing changed. */
int nb_panel_take_store(nb_panel *p);

/* What an encoder plays now (SDK order): its label, kind and position 0..1, its modulation depth. */
const char *nb_slot_label(const nb_panel *p, int enc);
int nb_slot_kind(const nb_panel *p, int enc);
float nb_slot_value(const nb_panel *p, int enc);
float nb_slot_mod(const nb_panel *p, int enc);
const char *nb_page_name(int page);
const char *nb_source_name(int src);
const char *nb_fx_name(int fx);
/* which of the eight section buttons a page belongs to (0..7) */
int nb_section_of(int page);

const char *nb_snap_name(int i);
const char *nb_topology_name(int i);
const char *nb_partials_name(int i);
const char *nb_pad_name(int key);
int nb_topology_count(void);
int nb_partials_count(void);
/* an effect's cost and the network's own, % of the device's render budget (the engine's nb_fx_cost when it has it);
 * the load of a sound: the network plus its effects in (MIX above 0, not switched off), but `except` */
float nb_fx_cost(int fx);
float nb_fx_load(const nb_snap *s, int except);
/* the knob's position mapped to its physical value (the measured curves) */
float nb_knob_value(int knob, float pos);

#endif
