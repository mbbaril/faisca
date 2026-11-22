# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Mario Baril
# Inline the worklet and the wasm into the page: one file, nothing fetched.
import base64, json, sys
shell, worklet, wasm, out = sys.argv[1:5]
page = open(shell, encoding="utf-8").read()
w = open(worklet, encoding="utf-8").read()
b = base64.b64encode(open(wasm, "rb").read()).decode()
assert "/*@WORKLET@*/" in page and "@WASM@" in page
page = page.replace("/*@WORKLET@*/", "const WORKLET_SRC = " + json.dumps(w).replace("</", "<\\/") + ";")
page = page.replace("@WASM@", b)
open(out, "w", encoding="utf-8", newline="\n").write(page)
