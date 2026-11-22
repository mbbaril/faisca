#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Mario Baril
# Build the web emulator: the core in WebAssembly (clang --target=wasm32, wasm-ld; no Emscripten needed),
# then web/play.html, the emulator in one self-contained page (web/shell.html with the wasm and the worklet inlined).
# Run in WSL (Ubuntu): tools/build_web.sh
set -e
cd "$(dirname "$0")/.."
mkdir -p build
clang --target=wasm32 -O2 -nostdlib -ffreestanding -std=c11 -Wall -Wextra -Wno-unused-parameter \
    -Wl,--no-entry -Wl,--export-dynamic -Wl,--initial-memory=1048576 -Wl,-z,stack-size=65536 -Wl,--strip-all \
    -o build/faisca.wasm web/nb_web.c core/nb_engine.c core/nb_panel.c core/nb_face.c
echo "wasm build/faisca.wasm $(wc -c < build/faisca.wasm) B"
python3 tools/inline.py web/shell.html web/worklet.js build/faisca.wasm web/play.html
echo "page web/play.html $(wc -c < web/play.html) B"
