/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Mario Baril
 * The loop network: four feedback loops joined by a 4x4 send matrix. Each loop is one fixed chain, always the
 * same; every effect has its four knobs, the fourth its MIX (at 0 the effect is not there):
 *   DRIVE (the preamp) > the band (MID, FOCUS) > FUZZ > BLAST > FOLD > WRAP > RECTIFY > DIVIDE > RING > STARVE
 *   > SHIFT > ECHO > PUMP > SWEEP > the loop's delay (its pitch) > back
 * then a ping-pong DELAY and a small plate REVERB, the master and a limiter that is never off.
 * Written from the CIRCUITO design notes; no code from the closed engine. */
#ifndef NB_ENGINE_H
#define NB_ENGINE_H
#include <stdint.h>

#define NB_CH 4
#define NB_LINE 2048            /* samples per loop line: 21.5 Hz at 44.1 kHz (MID down to 20 Hz) */
#define NB_BLOCK 16             /* control sub-block (coefficients, meters) */
#define NB_ECHO 1536            /* each loop's echo line: 35 ms, short, to excite the feedback */
#define NB_PDLY 5400            /* the post delay, each side: 122 ms */
#define NB_FDN 1200             /* each of the reverb's four lines */
#define NB_WAVE 176             /* the WAVES view's columns */
#define NB_SCOPE 240            /* the SCOPE view's columns */
#define NB_SPEC 24              /* the FREQ view's bands: LOW (below 63 Hz), then 63 Hz .. 10 kHz in thirds of an octave */
enum { NB_ORTHODOX, NB_HETERODOX, NB_HERETIC, NB_NSTAGE };
#define NB_STAGE_NET NB_NFX     /* the network's own stage, after the effects' */

/* the fixed chain's effects, in the order the signal meets them, then the two after the network */
enum { NB_FX_FUZZ, NB_FX_BLAST, NB_FX_FOLD, NB_FX_WRAP, NB_FX_RECTIFY, NB_FX_DIVIDE, NB_FX_RING, NB_FX_STARVE,
       NB_FX_SHIFT, NB_FX_ECHO, NB_FX_PUMP, NB_FX_SWEEP, NB_NLOOPFX,
       NB_FX_DELAY = NB_NLOOPFX, NB_FX_REVERB, NB_NFX };

/* What the engine plays: physical values (the panel turns knob positions into these). */
typedef struct {
    float gain;                 /* loop gain, 1 = unity (0..1.5) */
    float mid_hz;               /* band centre of channel 1 */
    float ratio[NB_CH];         /* each channel's partial of mid */
    float focus;                /* 0 flat .. 1 a narrow peak */
    float drive;                /* 0 soft knee .. 1 hard; scales the excess above unity */
    float cross;                /* how much of the matrix is sent (0..1) */
    float matrix[NB_CH][NB_CH]; /* [to][from], -1..1; the diagonal is ch_gain, not read here */
    float grito;                /* SCREAM: the loop's output bending its own delays (0..1) */
    float level;                /* the master, 0..1 */
    float ch_gain[NB_CH];       /* each loop's own gain, times GAIN (0..1.5; the matrix diagonal) */
    float ch_pitch[NB_CH];      /* each loop moved from its partial, semitones */
    float ch_mid[NB_CH];        /* each band moved from its loop's pitch, octaves (the comb stays) */
    float ch_level[NB_CH];      /* each loop in the output (0..1); still in the network at 0 */
    float hiss;                 /* the circuit's own noise floor, 0..1 */
    float fx[NB_NFX][4];        /* each effect's four knobs, positions 0..1 (the fourth: MIX) */
    uint8_t stage[NB_NFX + 1];  /* each effect's stage, then the network's: 0 orthodox, 1 heterodox, 2 heretic */
    uint8_t fx_on[NB_NFX];      /* each effect in (1) or out (0): out, its MIX fades to 0 (~80 ms) and it is skipped,
                                 * bit-exact and free; in again, from a clean state, faded in. The panel sets it */
    float transition;           /* TRANSITION, seconds (0..2): how a discrete choice moves. 0 = at once (a knob's
                                 * 15 ms, a stage's 10 ms); at 0.5, 90 % of the way in: PARTIALS 0.5 s (a glissando of the
                                 * network), a TOPOLOGY 0.3 s, a stage's crossfade 0.2 s, an effect in or out 80 ms, a
                                 * memory, a modulation's source or MOD CLEAR 0.3 s; all scale with it. A knob turned stays
                                 * fast whatever it is (a step bigger than a turn's is what counts as a choice) */
} nb_params;

/* What the pads do this block (the panel fills it). */
typedef struct {
    int hold;                   /* the loop's return open (DRONE, a KICK or OPEN held) */
    int ret_cut;                /* RETURN held: the return cut even under DRONE */
    int out_cut;                /* CUT held: the output closed */
    int chop;                   /* 0, or the crossings per cut (CHOP held) */
    float scatter_hz;           /* 0, or the rate of random cuts (SCATTER held) */
    uint8_t kick_mask;          /* channels struck on this block (one-shot) */
    float kick_color;           /* 0 dark and long .. 1 bright and short */
} nb_touch;

/* The truth of the sound, read by the face: levels as heard, heat, what passes on each send. */
typedef struct {
    float level[NB_CH];         /* RMS of each channel, times the output gate */
    float heat[NB_CH];          /* how far each preamp is into its knee (0..1) */
    float flow[NB_CH][NB_CH];   /* RMS of each send [to][from] */
    float send[NB_CH][NB_CH];   /* the effective send, signed */
    float period[NB_CH];        /* each loop's delay in samples */
    float gate;                 /* the output gate now (0..1) */
    float out;                  /* output RMS after the limiter */
    float limit;                /* the limiter's gain reduction (1 = none) */
    float ret;                  /* the return gate now (0 closed .. 1 open): RETURN, or no DRONE and no pad */
    uint8_t struck;             /* bit c: loop c is being struck now (a pad's strike still running) */
    int8_t spec[NB_SPEC];       /* FREQ: each band's level in dB (0 = a full-scale sine of one loop), -90..+14 */
    uint8_t spec_on;            /* the bands are measured (only while the FREQ view asks) */
    float fx_on;                /* how many loop effects are in (their MIX above 0), 0..1 */
    float wet;                  /* the post's level (0..1): the halo */
    uint32_t time;              /* samples since start */
    /* the screen's views (docs/TELA.md 13) */
    float loud[NB_CH];          /* each loop's level in the network (before its LEVEL and the output gate) */
    uint8_t stage[NB_CH][NB_NLOOPFX];   /* CHAIN: how much each effect changes its loop's signal now, 0..255 = 0..2x */
    int8_t wave_lo[NB_CH][NB_WAVE], wave_hi[NB_CH][NB_WAVE];   /* WAVES: each loop's last two periods, per column */
    int8_t scope_lo[NB_SCOPE], scope_hi[NB_SCOPE];  /* SCOPE: the four as the output has them (half their sum), on loop 1;
                                                       full scale 102 of 127, 127 at or over */
    int8_t scope_ch[NB_CH][NB_SCOPE];                /* the same window, each loop alone (full scale 102) */
    float bend;                 /* SCREAM's bend of every loop's period, mean over ~40 ms (1 = none) */
    uint8_t wave_free;          /* bit c: WAVES found no rising crossing for loop c (the window runs free) */
    uint8_t scope_free;         /* SCOPE found no rising crossing (the window runs free) */
} nb_view;

/* Every float the engine keeps from sample to sample, in one block: swept for denormals a slice per sub-block (a
 * decaying state that would sink below 1e-15 is set to zero: the analog floor, not a slow CPU path). */
#define NB_ZS 12                /* state slots per effect, stage and channel */
#define NB_HB 32                /* SHIFT's Hilbert pair: 8 allpasses x 4 states */
#define NB_DIF1 142             /* the REVERB's input diffusion, two allpasses */
#define NB_DIF2 107
typedef struct {
    /* the network */
    float y[NB_CH], dc_x[NB_CH], dc_y[NB_CH], s1[NB_CH], s2[NB_CH];
    float g[NB_CH], dg[NB_CH], kq[NB_CH], dk[NB_CH], mixb, dmixb;   /* the band, ramped sample by sample */
    float per[NB_CH], perq[NB_CH];                                   /* the loop's delay, gliding; its target */
    float tkey[NB_CH][4];       /* what set each loop's band and period last (f, band, FOCUS, latency): unchanged, skip */
    float xf_any;               /* a stage fade running somewhere */
    float mix_moving, band_moving;   /* a MIX, the band: ramp sample by sample only while they move */
    float rg_car;               /* RING's carrier this sample (shaped), one for the four loops */
    float mid_s, ratio_s[NB_CH], pitch_s[NB_CH], chmid_s[NB_CH];
    float mtx_s[NB_CH][NB_CH], chg_s[NB_CH], chl_s[NB_CH], hiss_s;
    float focus_s, gain_s, cross_s, drive_s, grito_s, level_s;
    float fx_s[NB_NFX][4];      /* the effects' knobs, smoothed per sub-block */
    float coef[NB_NFX][3][4];   /* per sub-block and stage: what the knobs mean now */
    float mixr[NB_NFX], dmixr[NB_NFX];   /* each MIX ramped sample by sample (no 16-sample steps in a wet level) */
    float lvl[NB_CH];           /* each loop's level inside the chain (fast up, slow down) */
    float lat;                  /* the samples the chain itself delays (ADAA, polyBLEP), taken off the period */
    float ret, gate, out_prev;
    float m_bend;               /* SCREAM's bend of the reads, its mean (the screen's pitch) */
    float kick_lp[NB_CH], kick_amp, kick_a, kick_dec;
    float env;                  /* the limiter's envelope */
    float pre_x1[NB_CH], pre_q1[NB_CH];   /* the preamp's ADAA: last input, last sqrt(1 + x^2) */
    float ohp_x[2], ohp_y[2];   /* the output's coupling capacitor */
    float drift[NB_CH], drift_t[NB_CH];   /* each loop's slow wander (cents) and where it goes */
    float pink[NB_CH][2];       /* the floor's colour */
    float heat[NB_CH];          /* the network's heterodox: each loop's temperature */
    float rew[NB_CH][NB_CH];    /* the network's heretic: each send's own weight */
    float net_amt[3];           /* how much of each network stage is in (glides) */
    /* the effects: per effect, stage and channel, NB_ZS slots; the crossfade between stages */
    float z[NB_NLOOPFX][3][NB_CH][NB_ZS];
    float xf[NB_NFX];           /* 1 -> 0 over 10 ms after a stage change */
    float hb[3][NB_CH][NB_HB], hbz[3][NB_CH];                        /* SHIFT, per stage */
    float shc[3][NB_CH], shs[3][NB_CH], shdc[3][NB_CH], shds[3][NB_CH];
    float rg_ph, rg_ph2, mb_ph, mb_head;  /* RING's carrier (and its quadrature), STARVE's motorboat */
    float wow_c, wow_s;         /* ECHO's wow, a quadrature pair turning */
    float spark[NB_CH], spark_lp[NB_CH];  /* WRAP's sparks into the next loop */
    float tail[3];              /* ECHO, DELAY, REVERB: how much is still ringing (they run until it stops) */
    /* POST */
    float pdl_lp[2], pdl_d, pdl_dk, pdl_bb[2][2], pdl_bbc, pdl_ret[2], pdl_hp[2], pdl_cp[2][2], rv_cp[2][2];
    float fdn_lp[4], fdn_len[4], fdn_g[4], rv_mod, rv_modc, rv_mods, sw_amt, rv_damp, rv_wd;
    float dif1[NB_DIF1], dif2[NB_DIF2];
    /* meters */
    float m_lv[NB_CH], m_heat[NB_CH], m_flow[NB_CH][NB_CH], m_out;
    float m_sd[NB_CH][NB_NLOOPFX], m_si[NB_CH][NB_NLOOPFX];   /* each stage's change and input, mean squares (CHAIN) */
} nb_fstate;

typedef struct {
    int16_t line[NB_CH][NB_LINE];
    uint32_t wp;
    nb_fstate f;
    float ret_target, gate_target;
    int chop_count, chop_on;
    float scat_left; int scat_on;
    int kick_left[NB_CH];
    int zi[NB_NLOOPFX][3][NB_CH][4];      /* the effects' integer states (flip-flops, counters) */
    uint8_t stg[NB_NFX + 1], stg_old[NB_NFX];   /* the stage playing, the one fading out */
    int16_t echo[NB_CH][NB_ECHO];         /* ECHO */
    uint32_t echo_wp;
    int16_t pdl[2][NB_PDLY];              /* POST */
    uint32_t pdl_wp;
    int16_t fdn[4][NB_FDN];
    uint32_t fdn_wp, dif_wp1, dif_wp2;
    float tp[NB_NLOOPFX + 1];             /* the signal between the chain's stages, this sample (the screen's CHAIN) */
    /* the transitions: each target as it was last sub-block, and whether it jumped (then it glides at TRANSITION's
     * pace until it arrives; a turn of a knob moves in small steps and stays fast) */
    float pt_fx[NB_NFX][4], pt_mtx[NB_CH][NB_CH], pt_ch[NB_CH][4], pt_misc[8];
    uint8_t sl_fx[NB_NFX][4], sl_mtx[NB_CH][NB_CH], sl_ch[NB_CH][4], sl_misc[8];
    float rt_misc[5];                     /* the per-sample glide of GAIN, CROSS, DRIVE, SCREAM, LEVEL now */
    float xf_step;                        /* a stage change's crossfade, per sample */
    uint8_t on_prev[NB_NFX];              /* fx_on as it was (an effect coming back in starts clean) */
    uint32_t rng, time, sweep;            /* sweep: where the denormal sweep is */
    float fs;
} nb_engine;

void nb_engine_init(nb_engine *e, float sample_rate);
/* Render n frames (any n), interleaved stereo float, -1..1, the limiter always on. */
void nb_engine_render(nb_engine *e, const nb_params *p, nb_touch *t, float *out, int n);
void nb_engine_view(const nb_engine *e, const nb_params *p, nb_view *v);
/* The FREQ view asks for its spectrum (measured in nb_engine_view, every other call, only while asked). */
void nb_engine_want_spectrum(int on);

#endif
