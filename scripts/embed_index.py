# Pre-build script: embed the web UI assets into the firmware.
#
# data/index.html is gzipped and emitted as src/index_html.h (PROGMEM array).
# data/favicon.svg is embedded raw as src/favicon_svg.h.
# The web server serves them from flash, so a single OTA upload updates the
# firmware, the UI and the icon atomically - LittleFS is not involved.
#
# Runs on every build (takes a few ms); the headers are rewritten only when
# their content actually changes, so scons doesn't recompile web.cpp for nothing.

Import("env")

import gzip
import os

# SCons scripts have no __file__; PlatformIO always provides PROJECT_DIR
ROOT = env.get("PROJECT_DIR") or os.getcwd()
SRC = os.path.join(ROOT, "data", "index.html")
DST = os.path.join(ROOT, "src", "index_html.h")
FAV_SRC = os.path.join(ROOT, "data", "favicon.svg")
FAV_DST = os.path.join(ROOT, "src", "favicon_svg.h")


def write_if_changed(path, content):
    old = ""
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as f:
            old = f.read()
    if old != content:
        with open(path, "w", encoding="utf-8") as f:
            f.write(content)
        return True
    return False


with open(SRC, "rb") as f:
    raw = f.read()
gz = gzip.compress(raw, 9)

out = []
out.append("// AUTO-GENERATED from data/index.html by scripts/embed_index.py - do not edit")
out.append("// Served from PROGMEM (gzip) by web.cpp; OTA updates the UI together with the firmware.")
out.append("#pragma once")
out.append("#include <pgmspace.h>")
out.append("")
out.append("#define INDEX_HTML_GZ_LEN %d" % len(gz))
out.append("")
out.append("static const uint8_t INDEX_HTML_GZ[] PROGMEM = {")
for i in range(0, len(gz), 16):
    out.append("    " + ", ".join("0x%02x" % b for b in gz[i:i + 16]) + ",")
out.append("};")
out.append("")
if write_if_changed(DST, "\n".join(out)):
    print("[embed] data/index.html -> src/index_html.h (%d -> %d bytes gz)"
          % (len(raw), len(gz)))

fav = b""
if os.path.exists(FAV_SRC):
    with open(FAV_SRC, "rb") as f:
        fav = f.read()

out = []
out.append("// AUTO-GENERATED from data/favicon.svg by scripts/embed_index.py - do not edit")
out.append("// Served from PROGMEM by web.cpp at /favicon.svg.")
out.append("#pragma once")
out.append("#include <pgmspace.h>")
out.append("")
out.append("#define FAVICON_SVG_LEN %d" % len(fav))
out.append("")
out.append("static const uint8_t FAVICON_SVG[] PROGMEM = {")
for i in range(0, len(fav), 16):
    out.append("    " + ", ".join("0x%02x" % b for b in fav[i:i + 16]) + ",")
out.append("};")
out.append("")
if write_if_changed(FAV_DST, "\n".join(out)):
    print("[embed] data/favicon.svg -> src/favicon_svg.h (%d bytes)" % len(fav))
