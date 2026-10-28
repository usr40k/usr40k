#!/usr/bin/env python3
"""Convert Terminus BDF bitmaps to the pixel-grid TTY16 font (TTF + WOFF2).
Usage: FONT_SRC=<dir with ter-u16n.bdf and ter-u16b.bdf> python3 tools/bdf2ttf.py <out_dir>
Sources: freebsd-src contrib/terminus (SIL OFL 1.1). Needs: pip install fonttools brotli"""
import sys, os
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont

SRC = os.environ["FONT_SRC"]
OUT = sys.argv[1]
PX = 64            # font units per pixel -> upm 1024 (16px cell)
UPM = 16 * PX
ASC, DESC = 12, 4

def parse(path):
    glyphs = {}
    cur = None
    bitmap = False
    for line in open(path, encoding="latin-1"):
        line = line.strip()
        if line.startswith("STARTCHAR"):
            cur = {"rows": []}
        elif cur is not None and line.startswith("ENCODING"):
            cur["enc"] = int(line.split()[1])
        elif cur is not None and line.startswith("BBX"):
            _, w, h, x, y = line.split()
            cur["bbx"] = tuple(map(int, (w, h, x, y)))
        elif line == "BITMAP":
            bitmap = True
        elif line == "ENDCHAR":
            bitmap = False
            if cur.get("enc", -1) >= 0:
                glyphs[cur["enc"]] = cur
            cur = None
        elif bitmap:
            cur["rows"].append(line)
    return glyphs

def rects(g):
    """merge pixel runs into rectangles in pixel coords (origin = baseline, left)."""
    w, h, xo, yo = g["bbx"]
    runs = []  # (row_index_from_top, x0, x1)
    for r, hexrow in enumerate(g["rows"]):
        bits = bin(int(hexrow, 16))[2:].zfill(len(hexrow) * 4)
        x = 0
        while x < len(bits):
            if bits[x] == "1":
                s = x
                while x < len(bits) and bits[x] == "1":
                    x += 1
                if s < w:
                    runs.append((r, s, min(x, w)))
            else:
                x += 1
    # vertical merge of identical runs
    out, open_ = [], {}
    for r in range(h):
        row = {(s, e) for rr, s, e in runs if rr == r}
        for key in list(open_):
            if key not in row:
                out.append((key[0], key[1], open_.pop(key), r))
        for key in row:
            if key not in open_:
                open_[key] = r
    for key, r0 in open_.items():
        out.append((key[0], key[1], r0, h))
    res = []
    for s, e, r0, r1 in out:
        x0 = xo + s
        x1 = xo + e
        top = yo + h - r0
        bot = yo + h - r1
        res.append((x0, bot, x1, top))
    return res

def build(bdf, family, style, weight, fname):
    glyphs = parse(bdf)
    order = [".notdef"]
    cmap, gs, adv = {}, {}, {}
    def name(cp): return "uni%04X" % cp
    for cp in sorted(glyphs):
        if cp > 0xFFFF: continue
        order.append(name(cp))
    # .notdef = DEFAULT_CHAR 65533 if present else box
    for gn in order:
        cp = None if gn == ".notdef" else int(gn[3:], 16)
        src = glyphs.get(65533 if cp is None else cp)
        pen = TTGlyphPen(None)
        if src:
            for (x0, y0, x1, y1) in rects(src):
                pen.moveTo((x0 * PX, y0 * PX)); pen.lineTo((x0 * PX, y1 * PX))
                pen.lineTo((x1 * PX, y1 * PX)); pen.lineTo((x1 * PX, y0 * PX)); pen.closePath()
        gs[gn] = pen.glyph()
        adv[gn] = (8 * PX, 0)
        if cp is not None: cmap[cp] = gn
    fb = FontBuilder(UPM, isTTF=True)
    fb.setupGlyphOrder(order)
    fb.setupCharacterMap(cmap)
    fb.setupGlyf(gs)
    metrics = {}
    for gn in order:
        g = gs[gn]; g.recalcBounds(fb.font["glyf"]) if hasattr(g, "recalcBounds") else None
        metrics[gn] = (8 * PX, getattr(g, "xMin", 0) or 0)
    fb.setupHorizontalMetrics(metrics)
    fb.setupHorizontalHeader(ascent=ASC * PX, descent=-DESC * PX, lineGap=0)
    full = f"{family} {style}".strip()
    fb.setupNameTable({
        "familyName": family, "styleName": style, "uniqueFontIdentifier": f"{family}-{style}-1.0",
        "fullName": full, "psName": f"{family}-{style}".replace(" ", ""), "version": "Version 1.0",
        "copyright": "Copyright (C) 2020 Dimitar Toshkov Zhekov. Pixel-outline conversion by usr40k.",
        "licenseDescription": "SIL Open Font License 1.1 (see OFL.txt). Derived from Terminus; renamed per the Reserved Font Name clause.",
        "licenseInfoURL": "https://openfontlicense.org",
    })
    fb.setupOS2(sTypoAscender=ASC * PX, sTypoDescender=-DESC * PX, sTypoLineGap=0,
                usWinAscent=ASC * PX, usWinDescent=DESC * PX, usWeightClass=weight,
                fsType=0, sxHeight=8 * PX, sCapHeight=10 * PX)
    fb.setupPost(isFixedPitch=1, underlinePosition=-PX, underlineThickness=PX)
    fb.font["head"].macStyle = 1 if weight >= 700 else 0
    fb.setupDummyDSIG = None
    fb.save(os.path.join(OUT, fname + ".ttf"))
    f = TTFont(os.path.join(OUT, fname + ".ttf"))
    f.flavor = "woff2"
    f.save(os.path.join(OUT, fname + ".woff2"))
    print(fname, len(order), "glyphs")

os.makedirs(OUT, exist_ok=True)
build(os.path.join(SRC, "ter-u16n.bdf"), "TTY16", "Regular", 400, "TTY16-Regular")
build(os.path.join(SRC, "ter-u16b.bdf"), "TTY16", "Bold", 700, "TTY16-Bold")
