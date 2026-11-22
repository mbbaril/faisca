#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Mario Baril
# Assemble the publishable site (faisca.ruido.art.br) into build/site/: the pages, the emulator (web/play.html, built
# by tools/build_web.sh with the wasm and the worklet inlined), the style, the fonts with their licences, the icon,
# CNAME, robots.txt and 404.html. Nothing else: no firmware, no updater. Then check that the result holds no firmware
# package or update page, links to none, and that every local link and asset it names is there.
#   tools/make_site.sh            (from the repository root; sh, any POSIX system)
set -e
cd "$(dirname "$0")/.."
OUT=build/site
PAGES="index.html play.html manual.html theory.html cheatsheet.html labels.html legal.html recovery.html 404.html"
ASSETS="site.css site.js icon.svg"
FONTS="BarlowCondensed-Bold.ttf DMSans.ttf IBMPlexMono-Regular.ttf OFL-Barlow.txt OFL-DMSans.txt OFL-IBMPlexMono.txt"

[ -f web/play.html ] || { echo "web/play.html is missing: run tools/build_web.sh first" >&2; exit 1; }
for f in web/shell.html web/worklet.js web/nb_web.c core/*.c core/*.h; do
    if [ "$f" -nt web/play.html ]; then echo "warning: $f is newer than web/play.html (rebuild with tools/build_web.sh)" >&2; fi
done

rm -rf "$OUT"
mkdir -p "$OUT/fonts"
for f in $PAGES $ASSETS; do cp "web/$f" "$OUT/$f"; done
for f in $FONTS; do cp "web/fonts/$f" "$OUT/fonts/$f"; done
printf 'faisca.ruido.art.br\n' > "$OUT/CNAME"
printf 'User-agent: *\nAllow: /\n' > "$OUT/robots.txt"
: > "$OUT/.nojekyll"

# ---- the checks ----
fail=0
bad=$(find "$OUT" \( -iname '*.fwsc' -o -iname '*.bin' -o -iname '*.elf' -o -iname 'update*.html' -o -iname '*.ufw' \) -print)
if [ -n "$bad" ]; then echo "error: firmware or updater files in the site:" >&2; echo "$bad" >&2; fail=1; fi
if grep -l -i -E 'update\.html|\.fwsc' "$OUT"/*.html >/dev/null 2>&1; then
    echo "error: a page names an update page or a firmware package:" >&2
    grep -n -i -E 'update\.html|\.fwsc' "$OUT"/*.html >&2; fail=1
fi
# every local href/src in the pages (not http:, mailto:, #anchors, data:, blob:) must exist in the site
for page in "$OUT"/*.html; do
    for ref in $(grep -o -E '(href|src)="[^"#?]*' "$page" | sed -E 's/^(href|src)="//' | sort -u); do
        case "$ref" in ''|http:*|https:*|mailto:*|data:*|blob:*|javascript:*) continue ;; esac
        path=${ref#/}
        if [ ! -e "$OUT/$path" ]; then echo "error: $(basename "$page") links to $ref, which is not in the site" >&2; fail=1; fi
    done
done
[ "$fail" = 0 ] || exit 1
echo "site $OUT: $(find "$OUT" -type f | wc -l | tr -d ' ') files, $(du -sk "$OUT" | cut -f1) KB"
find "$OUT" -type f | sed "s|^$OUT/||" | sort
