#!/usr/bin/env python3
# Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE)
# The Q-LiM icon (after assets/icons/qlim.svg) in all sizes, drawn at a large magnification and scaled down
# (smooth edges). The small sizes (<= 32) are simplified: fewer and thicker bars, so they stay readable.
#   python3 tools/make_icons.py            -> assets/icons/qlim-*.png and src/icon_data.h (the window icon)
import os, sys
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BARS = [(92,236,40,1),(108,216,80,1),(124,196,120,1),(140,221,70,1),(156,181,150,1),(172,206,100,1),(188,226,60,1),
        (204,191,130,.8),(220,211,90,.6),(236,171,170,.5),(252,201,110,.5),(268,176,160,.5),(284,216,80,.6),
        (300,186,140,.8),(316,226,60,1),(332,196,120,1),(348,206,100,1),(364,221,70,1),(380,201,110,1),
        (396,231,50,1),(412,241,30,1)]   # x, y, height, opacity (from the SVG; width 8)
BG, FRAME, BAR, ACCENT = (17,17,22), (36,36,44), (233,233,238), (255,163,26)

def bez(p0, p1, p2, p3, n=600):
    pts = []
    for i in range(n + 1):
        t = i / n; u = 1 - t
        pts.append((u*u*u*p0[0] + 3*u*u*t*p1[0] + 3*u*t*t*p2[0] + t*t*t*p3[0],
                    u*u*u*p0[1] + 3*u*u*t*p1[1] + 3*u*t*t*p2[1] + t*t*t*p3[1]))
    return pts

def render(size):
    k = 8 if size <= 512 else 4
    S = size * k
    f = S / 512.0
    small = size <= 32
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img, "RGBA")
    m = 16 if not small else 6
    d.rounded_rectangle([m*f, m*f, (512-m)*f, (512-m)*f], radius=(112 if not small else 96)*f, fill=BG + (255,),
                        outline=FRAME + (255,), width=max(1, int(2*f)) if not small else 0)
    def layer_shape(fn):                    # semi-transparent: drawn on a separate layer and composited (no "holes")
        nonlocal img, d
        ly = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        fn(ImageDraw.Draw(ly))
        img = Image.alpha_composite(img, ly)
        d = ImageDraw.Draw(img, "RGBA")
    if not small:
        layer_shape(lambda dd: dd.rounded_rectangle([208*f, 150*f, 304*f, 362*f], radius=14*f, fill=(255, 255, 255, 13)))
        bars, bw = BARS, 8
    else:                                   # simplified: every second bar, twice as wide and tall
        bars, bw = [(x - 4, y - h * 0.15, h * 1.3, a) for (x, y, h, a) in BARS[::2]], 16
    for (x, y, h, a) in bars:
        layer_shape(lambda dd, x=x, y=y, h=h, a=a: dd.rounded_rectangle([x*f, y*f, (x+bw)*f, (y+h)*f], radius=bw/2*f,
                                                                       fill=BAR + (int(255*a),)))
    sw = (6 if not small else 16) * f
    for curve in (bez((208,158),(248,158),(268,354),(304,354)), bez((208,354),(244,354),(264,158),(304,158))):
        pts = [(x*f, y*f) for (x, y) in curve]
        d.line(pts, fill=ACCENT + (255,), width=int(sw), joint="curve")
        for (x, y) in (pts[0], pts[-1]):
            d.ellipse([x - sw/2, y - sw/2, x + sw/2, y + sw/2], fill=ACCENT + (255,))
    return img.resize((size, size), Image.LANCZOS)

def render_doc(size):
    """The project file icon (.limproject): a dark sheet with a folded orange corner and the Q-LiM sign."""
    k = 8 if size <= 512 else 4
    S = size * k
    f = S / 512.0
    small = size <= 32
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    x0, y0, x1, y1, fold = 72, 20, 440, 492, (120 if not small else 150)
    page = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    pd = ImageDraw.Draw(page)
    pd.rounded_rectangle([x0*f, y0*f, x1*f, y1*f], radius=36*f, fill=BG + (255,), outline=(70, 70, 82, 255),
                         width=max(1, int((3 if not small else 10)*f)))
    pd.polygon([((x1 - fold)*f, (y0 - 2)*f), ((x1 + 2)*f, (y0 - 2)*f), ((x1 + 2)*f, (y0 + fold)*f)], fill=(0, 0, 0, 0))   # cut corner
    img = Image.alpha_composite(img, page)
    d = ImageDraw.Draw(img, "RGBA")
    d.polygon([((x1 - fold)*f, y0*f), (x1*f, (y0 + fold)*f), ((x1 - fold)*f, (y0 + fold)*f)], fill=ACCENT + (255,))   # the folded part
    # the sign: the waveform + "X", smaller, in the lower part of the sheet
    sc, cx, cy = (0.62 if not small else 0.8), 256, 300
    def T(x, y): return ((cx + (x - 256) * sc) * f, (cy + (y - 256) * sc) * f)
    bars = BARS if not small else [(x - 4, y - h * 0.15, h * 1.3, a) for (x, y, h, a) in BARS[::2]]
    bw = 8 if not small else 16
    for (x, y, h, a) in bars:
        ly = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        (ax, ay), (bx, by) = T(x, y), T(x + bw, y + h)
        ImageDraw.Draw(ly).rounded_rectangle([ax, ay, bx, by], radius=(bx - ax) / 2, fill=BAR + (int(255 * a),))
        img = Image.alpha_composite(img, ly)
    d = ImageDraw.Draw(img, "RGBA")
    sw = (6 if not small else 18) * f * sc
    for curve in (bez((208,158),(248,158),(268,354),(304,354)), bez((208,354),(244,354),(264,158),(304,158))):
        pts = [T(x, y) for (x, y) in curve]
        d.line(pts, fill=ACCENT + (255,), width=int(sw), joint="curve")
        for (x, y) in (pts[0], pts[-1]):
            d.ellipse([x - sw/2, y - sw/2, x + sw/2, y + sw/2], fill=ACCENT + (255,))
    return img.resize((size, size), Image.LANCZOS)

def main():
    out = os.path.join(ROOT, "assets", "icons")
    for s in (16, 24, 32, 48, 64, 128, 256, 512, 1024):
        render(s).save(os.path.join(out, "qlim-%d.png" % s), optimize=True)
        render_doc(s).save(os.path.join(out, "qlim-project-%d.png" % s), optimize=True)   # step 36l
        print("icon", s)
    win = render(256).convert("RGBA")       # the window icon (taskbar, Alt+Tab)
    data = win.tobytes()
    with open(os.path.join(ROOT, "src", "icon_data.h"), "w") as fh:
        fh.write("/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */\n")
        fh.write("/* The Q-LiM icon, 256x256 RGBA (tools/make_icons.py) - for the window */\n")
        fh.write("#define QLIM_ICON_W 256\n#define QLIM_ICON_H 256\nstatic const unsigned char qlim_icon_rgba[] = {\n")
        for i in range(0, len(data), 32):
            fh.write(",".join("0x%02x" % b for b in data[i:i+32]) + ",\n")
        fh.write("};\n")
    print("icon_data.h 256x256")

if __name__ == "__main__":
    main()
