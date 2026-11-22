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
    float fx_on;                /* how many loop effects are in (their MIX above 0), 0..1 */
    float wet;                  /* the post's level (0..1): the halo */
    uint32_t time;              /* samples since start */
} nb_view;

typedef struct {
    int16_t line[NB_CH][NB_LINE];
    uint32_t wp;
    float y[NB_CH];             /* last channel output (written into the line) */
    float dc_x[NB_CH], dc_y[NB_CH];
    float s1[NB_CH], s2[NB_CH]; /* band-pass states */
    float g[NB_CH], k;          /* band-pass coefficients */
    float per[NB_CH];           /* smoothed delay, samples */
    float mid_s;                /* MID gliding to the knob (no steps as it turns), Hz */
    /* every other knob glides too, as a pot through an RC would: no step anywhere (sub-block rate, ~15 ms) */
    float ratio_s[NB_CH], pitch_s[NB_CH], chmid_s[NB_CH];
    float mtx_s[NB_CH][NB_CH], chg_s[NB_CH], chl_s[NB_CH], hiss_s;
    float focus_s, gain_s, cross_s, drive_s, grito_s, level_s;
    float fx_s[NB_NFX][4];      /* the effects' knobs, smoothed per sub-block */
    float mix;                  /* flat..peak blend */
    float ret, ret_target;      /* return gate */
    float gate, gate_target;    /* output gate */
    float out_prev;
    int chop_count, chop_on;
    float scat_left; int scat_on;
    /* the strike */
    int kick_left[NB_CH];
    float kick_lp[NB_CH], kick_amp, kick_a, kick_dec;
    float env;                  /* limiter envelope */
    /* the effects' own states */
    float fz_lp[NB_CH], fz_dc[NB_CH], fz_dcy[NB_CH];          /* FUZZ */
    float bl_pre[NB_CH], bl_lp[NB_CH];                        /* BLAST */
    float wr_lp[NB_CH], fo_lp[NB_CH];                         /* WRAP, FOLD smoothing */
    float lvl_env[NB_CH], rc_lp[NB_CH];                       /* RECTIFY */
    int div_q[NB_CH], div_q2[NB_CH], div_prev[NB_CH];         /* DIVIDE */
    float dv_lp[NB_CH], dv_env[NB_CH];
    float rg_ph;                                              /* RING's own carrier */
    float st_env[NB_CH];                                      /* STARVE */
    float hb[NB_CH][32], hb_z[NB_CH], sh_fb[NB_CH];           /* SHIFT: the Hilbert pair, its own feedback */
    float sh_c[NB_CH], sh_s[NB_CH], sh_dc[NB_CH], sh_ds[NB_CH];
    int16_t echo[NB_CH][NB_ECHO];                             /* ECHO */
    uint32_t echo_wp;
    float echo_ph;
    float pump_env[NB_CH];                                    /* PUMP */
    float sw_env[NB_CH], sw1[NB_CH], sw2[NB_CH];              /* SWEEP */
    float coef[NB_NFX][4];                                    /* per sub-block: what the knobs mean now */
    /* POST */
    int16_t pdl[2][NB_PDLY];
    uint32_t pdl_wp;
    float pdl_lp[2];
    int16_t fdn[4][NB_FDN];
    uint32_t fdn_wp;
    float fdn_lp[4];
    /* meters */
    float m_lv[NB_CH], m_heat[NB_CH], m_flow[NB_CH][NB_CH], m_out;
    uint32_t rng, time;
    float fs;
} nb_engine;

void nb_engine_init(nb_engine *e, float sample_rate);
/* Render n frames (any n), interleaved stereo float, -1..1, the limiter always on. */
void nb_engine_render(nb_engine *e, const nb_params *p, nb_touch *t, float *out, int n);
void nb_engine_view(const nb_engine *e, const nb_params *p, nb_view *v);

#endif
