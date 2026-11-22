// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Mario Baril
// The audio thread: the core runs here, as on the device's audio interrupt and control task.
class Faisca extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const o = options.processorOptions;
    this.x = new WebAssembly.Instance(new WebAssembly.Module(o.wasm), {}).exports;
    this.mem = this.x.memory;
    this.x.nb_init(sampleRate);
    if (o.store) {
      new Uint8Array(this.mem.buffer, this.x.nb_store_ptr(), this.x.nb_store_size()).set(o.store);
      this.x.nb_store_load();
    }
    this.keys = 0; this.buttons = 0; this.keyPulse = 0; this.btnPulse = 0;
    this.port.onmessage = (e) => {
      const m = e.data;
      /* a press shorter than an audio block still reaches the core: held until it is read once */
      if (m.keys !== undefined) { this.keys = m.keys >>> 0; this.keyPulse |= this.keys; }
      if (m.buttons !== undefined) { this.buttons = m.buttons >>> 0; this.btnPulse |= this.buttons; }
      if (m.turn) this.x.nb_turn(m.turn[0], m.turn[1]);
      if (m.pot !== undefined) this.x.nb_set_pot(m.pot);
      if (m.midi) this.x.nb_midi(m.midi[0], m.midi[1] | 0, m.midi[2] | 0);
      if (m.midiCh !== undefined) this.x.nb_set_midi_ch(m.midiCh | 0);
    };
  }
  str(ptr) {
    const m = new Uint8Array(this.mem.buffer, ptr, 32);
    let s = '';
    for (let i = 0; i < 32 && m[i]; i++) s += String.fromCharCode(m[i]);
    return s;
  }
  process(inputs, outputs) {
    const out = outputs[0], L = out[0], R = out[1] || out[0], n = L.length;
    const x = this.x;
    x.nb_set_keys((this.keys | this.keyPulse) >>> 0);
    x.nb_set_buttons((this.buttons | this.btnPulse) >>> 0);
    this.keyPulse = 0; this.btnPulse = 0;
    const done = x.nb_process(n);
    const a = new Float32Array(this.mem.buffer, x.nb_audio(), 2 * n);
    for (let i = 0; i < n; i++) { L[i] = a[2 * i]; R[i] = a[2 * i + 1]; }
    if (done) {
      const scr = new Uint16Array(this.mem.buffer, x.nb_screen(), 240 * 240).slice();
      const leds = new Uint8Array(this.mem.buffer, x.nb_leds(), 41).slice();
      const slots = [0, 1, 2, 3, 4, 5, 6].map((e) => [x.nb_slot_v(e), x.nb_slot_k(e), this.str(x.nb_slot_l(e)), x.nb_slot_m(e)]);
      const state = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10].map((k) => x.nb_state(k));
      const msg = { scr, leds, slots, state, page: this.str(x.nb_name(5, state[5])) };
      if (x.nb_store_take()) msg.store = new Uint8Array(this.mem.buffer, x.nb_store_ptr(), x.nb_store_size()).slice();
      this.port.postMessage(msg, [scr.buffer]);
    }
    return true;
  }
}
registerProcessor('faisca', Faisca);
