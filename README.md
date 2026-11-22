# FAÍSCA

A feedback noisebox for the M-VAVE FM-1, by [ruído](https://ruido.art.br). Four feedback loops joined by a 4x4 send
matrix, each loop one fixed chain of twelve effects (FUZZ BLAST FOLD WRAP RECTIFY DIVIDE RING STARVE SHIFT ECHO PUMP
SWEEP), then a ping-pong DELAY and a small REVERB, a limiter that is never off. Sixteen memories that sing on their
own once DRONE is on.

**Play it in the browser:** https://faisca.ruido.art.br/play.html (any recent browser; MIDI needs Chrome or Edge).
**The project page, manual and theory:** https://faisca.ruido.art.br

## Status

The browser emulator is complete and plays the same core the device will run. **The device firmware is not released
yet**: it is being built and tested in stages, each checked on the unit, and nothing is offered to install until it
is ready. It is built on [fm1-base](https://github.com/charlesvestal/fm1-base) and
[felucca-bsp](https://github.com/hugelton/felucca-bsp): if the firmware fails to start twice, the platform comes up in
a safe mode with no audio that still talks over USB and accepts a reinstall or a return to the official firmware. See
[recovery](https://faisca.ruido.art.br/recovery.html) for every way back.

## What is here

- `core/` the instrument, in plain C with no dependencies: `nb_engine` (the loops, the effects, the post, the
  limiter; the audio render calls nothing and allocates nothing), `nb_panel` (controls, pages, memories, MOD, MOTION,
  MORPH, the store), `nb_face` (the 240x240 screen), `nb_curves.h` (each knob's measured response curve).
- `web/` the emulator (`nb_web.c` to WebAssembly, `worklet.js`, `shell.html`) and the site's pages.
- `tests/smoke.c` silent start, sections and memories, the limiter everywhere, no NaN, every memory sustaining.
- `tools/` `build_web.sh` (the emulator), `make_site.sh` (the published site), `knobscan.c` (measures the knob
  curves), `presetcheck.c` (checks every memory sustains and is not percussive).

## Build

```sh
cc -O2 -o smoke tests/smoke.c core/nb_engine.c core/nb_panel.c -lm && ./smoke
sh tools/build_web.sh        # clang with the wasm32 target: web/play.html, the emulator in one page
sh tools/make_site.sh        # the site as published, in build/site
```

Serve `web/` with any static server and open `play.html`; press POWER. The page starts silent and the limiter is
always on.

## License

FAÍSCA's code is GPL-3.0-only (`LICENSE`). The fonts in `web/fonts` are SIL OFL 1.1, each with its licence text.
FAÍSCA is unofficial: it is not made, reviewed or endorsed by M-VAVE; M-VAVE and FM-1 are trademarks of their
owners. There is no warranty: see [legal](https://faisca.ruido.art.br/legal.html).
