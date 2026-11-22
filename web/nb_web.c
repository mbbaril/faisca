/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Mario Baril
 * The web platform: the noisebox core in WebAssembly, run inside an AudioWorklet. The page sends the raw
 * controls (keys, buttons, encoder detents, the pot); this file reads them as the device's control task
 * would, renders the audio, and draws the screen a band at a time, as the device does. */
#include "../core/nb_engine.h"
#include "../core/nb_panel.h"
#include "../core/nb_face.h"

#define EXPORT(name) __attribute__((export_name(#name)))

void *memset(void *d, int c, unsigned long n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, unsigned long n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }

static nb_engine engine;
static nb_panel panel;
static nb_controls ctl;
static nb_view view;
static float fs;
static float audio[2 * 128];
static uint16_t screen[NB_FACE_W * NB_FACE_H];
static unsigned band_y;
static uint8_t store_blob[NB_NBANK * sizeof(nb_store)];   /* the banks one after the other */

EXPORT(nb_init) void nb_init(float sample_rate)
{
    fs = sample_rate;
    nb_engine_init(&engine, fs);
    nb_panel_init(&panel);
    ctl.pot = 700;
    band_y = 0;
}

EXPORT(nb_store_ptr) uint8_t *nb_store_ptr(void) { return store_blob; }
EXPORT(nb_store_size) unsigned nb_store_size(void) { return sizeof store_blob; }
/* each bank on its own: a bank that is not whole keeps the factory sounds (an older, shorter blob: bank 0 only) */
EXPORT(nb_store_load) void nb_store_load(void)
{
    for (int b = 0; b < NB_NBANK; ++b) nb_panel_load(&panel, b, store_blob + b * sizeof(nb_store), sizeof(nb_store));
}
/* 1 when a STORE happened: the blob (every bank) is ready to keep */
EXPORT(nb_store_take) int nb_store_take(void)
{
    if (!nb_panel_take_store(&panel)) return 0;
    const uint8_t *s = (const uint8_t *)panel.store;
    for (unsigned i = 0; i < sizeof store_blob; ++i) store_blob[i] = s[i];
    return 1;
}

EXPORT(nb_set_keys) void nb_set_keys(uint32_t k) { ctl.keys = k; }
EXPORT(nb_set_buttons) void nb_set_buttons(uint32_t b) { ctl.buttons = b; }
EXPORT(nb_turn) void nb_turn(int enc, int detents) { if (enc >= 0 && enc < 7) ctl.enc[enc] += (int16_t)detents; }
EXPORT(nb_midi) void nb_midi(int b0, int b1, int b2)
{
    uint8_t m[3] = { (uint8_t)b0, (uint8_t)b1, (uint8_t)b2 };
    nb_panel_midi(&panel, m, 3);
}
EXPORT(nb_set_pot) void nb_set_pot(int v) { ctl.pot = (uint16_t)(v < 0 ? 0 : v > 1023 ? 1023 : v); }
/* the MIDI channel the instrument listens to (SETUP's MIDI CH), set from the page's MIDI panel: 0 any, 1..16 */
EXPORT(nb_set_midi_ch) void nb_set_midi_ch(int ch)
{
    if (ch < 0 || ch > 16 || panel.setup_midi_ch == (uint8_t)ch) return;
    panel.setup_midi_ch = (uint8_t)ch;
    panel.setup_dirty = 1;
}

/* one audio quantum: the control read, the audio, two bands of the screen; 1 when a frame is complete */
static uint64_t samples;
EXPORT(nb_process) int nb_process(int frames)
{
    if (frames > 128) frames = 128;
    samples += (uint64_t)frames;
    ctl.ms = (uint32_t)(samples * 1000u / (uint64_t)fs);
    ctl.follow = view.out;
    nb_panel_update(&panel, &ctl);
    for (int e = 0; e < 7; ++e) ctl.enc[e] = 0;
    nb_engine_render(&engine, &panel.params, &panel.touch, audio, frames);
    int done = 0;
    for (int k = 0; k < 2; ++k) {
        if (band_y == 0) nb_engine_view(&engine, &panel.params, &view);
        nb_face_band(&view, &panel.hud, fs, band_y, NB_FACE_BAND, &screen[band_y * NB_FACE_W]);
        band_y += NB_FACE_BAND;
        if (band_y >= NB_FACE_H) { band_y = 0; done = 1; }
    }
    return done;
}

EXPORT(nb_audio) float *nb_audio(void) { return audio; }
EXPORT(nb_screen) uint16_t *nb_screen(void) { return screen; }
EXPORT(nb_leds) uint8_t *nb_leds(void) { return panel.led; }
EXPORT(nb_knob) float nb_knob(int k) { return (k >= 0 && k < NB_NKNOB) ? panel.cur.pos[k] : 0.f; }
EXPORT(nb_state) int nb_state(int which)
{
    switch (which) {
    case 0: return panel.snap_index;
    case 1: return panel.cur.topology;
    case 2: return nb_section_of(panel.page);
    case 3: return panel.cur.partials;
    case 4: return panel.drone;
    case 5: return panel.page;
    case 6: return panel.focus;
    case 7: return panel.mod_sel;
    case 8: return panel.cur.motion_ms ? (panel.rec ? 2 : 1) : (panel.rec ? 2 : 0);
    case 9: return panel.morph_target;
    case 10: return panel.setup_midi_ch;
    }
    return 0;
}
EXPORT(nb_slot_v) float nb_slot_v(int enc) { return nb_slot_value(&panel, enc); }
EXPORT(nb_slot_m) float nb_slot_m(int enc) { return nb_slot_mod(&panel, enc); }
EXPORT(nb_slot_k) int nb_slot_k(int enc) { return nb_slot_kind(&panel, enc); }
EXPORT(nb_slot_l) const char *nb_slot_l(int enc) { return nb_slot_label(&panel, enc); }
EXPORT(nb_out_level) float nb_out_level(void) { return view.out; }
EXPORT(nb_name) const char *nb_name(int kind, int i)
{
    switch (kind) {
    case 0: return nb_snap_name(i);
    case 1: return nb_topology_name(i);
    case 2: return nb_fx_name(i);
    case 3: return nb_partials_name(i);
    case 4: return nb_pad_name(i);
    case 5: return nb_page_name(i);
    case 6: return nb_source_name(i);
    }
    return "";
}
