"""Render the Inno Setup wizard images in the Sample Grabber brand (dark).

Usage: python3 installer/make-images.py   (writes installer/images/*.png)
Needs Pillow. Fonts come from resources/fonts. Output is committed, CI only reads it.
"""
import math
import os
import random

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "installer", "images")
FONTS = os.path.join(ROOT, "resources", "fonts")

INK = (11, 11, 12)
PAPER = (245, 244, 239)
RED = (255, 59, 33)
SUB = (143, 143, 138)
DIM = tuple(round(INK[i] + (PAPER[i] - INK[i]) * 0.26) for i in range(3))
RULE = tuple(round(INK[i] + (PAPER[i] - INK[i]) * 0.15) for i in range(3))

SS = 4  # supersampling


def heights(n, seed=7, kick=16):
    rnd = random.Random(seed)
    hs = []
    for i in range(n):
        ph = i % kick
        k = math.exp(-ph / 3.6)
        hat = 0.18 if ph == kick // 2 else 0.0
        swell = 0.10 * math.sin(i / n * math.pi * 2.2 + 0.4)
        v = 0.12 + 0.58 * k + hat + swell + 0.30 * rnd.random()
        if (i // kick) % 4 == 3 and ph > kick * 0.6:
            v *= 0.55
        hs.append(max(0.07, min(1.0, v)))
    return hs


def brackets(d, xl, xr, top, bot, st, fl):
    d.rectangle([xl, top, xl + st + fl, top + st], fill=RED)
    d.rectangle([xl, top, xl + st, bot], fill=RED)
    d.rectangle([xl, bot - st, xl + st + fl, bot], fill=RED)
    d.rectangle([xr - st - fl, top, xr, top + st], fill=RED)
    d.rectangle([xr - st, top, xr, bot], fill=RED)
    d.rectangle([xr - st - fl, bot - st, xr, bot], fill=RED)


def font(name, px):
    return ImageFont.truetype(os.path.join(FONTS, name), px)


def tracked(d, xy, text, f, fill, tracking):
    x, y = xy
    for ch in text:
        d.text((x, y), ch, font=f, fill=fill)
        x += d.textlength(ch, font=f) + tracking


def wizard_image(w, h):
    """Left panel of the welcome/finish pages (164:314). Vertical waveform, one grabbed slice."""
    W, H = w * SS, h * SS
    s = W / 202.0
    img = Image.new("RGB", (W, H), INK)
    d = ImageDraw.Draw(img)

    # horizontal bars stacked vertically: a waveform turned on its side
    n = 34
    top, bot = 64 * s, H - 92 * s
    pitch = (bot - top) / n
    bw = pitch * 0.5
    cx = W / 2
    hs = heights(n, seed=11)
    sel = (13, 20)
    for i, a in enumerate(hs):
        y = top + i * pitch
        L = max(4 * s, a * (W - 48 * s))
        on = sel[0] <= i <= sel[1]
        d.rectangle([cx - L / 2, y, cx + L / 2, y + bw], fill=PAPER if on else DIM)
    gap = pitch - bw
    yt = top + sel[0] * pitch - gap / 2 - 2 * s
    yb = top + sel[1] * pitch + bw + gap / 2 + 2 * s
    st, fl = 2.5 * s, 9 * s
    # trim handles rotated: brackets left/right become top/bottom rails
    x0, x1 = 14 * s, W - 14 * s
    d.rectangle([x0, yt, x0 + fl + st, yt + st], fill=RED)
    d.rectangle([x0, yt, x0 + st, yt + fl + st], fill=RED)
    d.rectangle([x1 - fl - st, yt, x1, yt + st], fill=RED)
    d.rectangle([x1 - st, yt, x1, yt + fl + st], fill=RED)
    d.rectangle([x0, yb - st, x0 + fl + st, yb], fill=RED)
    d.rectangle([x0, yb - fl - st, x0 + st, yb], fill=RED)
    d.rectangle([x1 - fl - st, yb - st, x1, yb], fill=RED)
    d.rectangle([x1 - st, yb - fl - st, x1, yb], fill=RED)

    # labels
    mono = font("JetBrainsMono-Medium.ttf", round(8.5 * s))
    tracked(d, (16 * s, 24 * s), "SG/01", mono, RED, 0.8 * s)
    tracked(d, (16 * s, 36 * s), "MYTHEN STUDIOS", mono, SUB, 0.8 * s)
    d.rectangle([16 * s, H - 78 * s, W - 16 * s, H - 78 * s + max(1, s)], fill=RULE)
    title = font("Inter-SemiBold.ttf", round(17 * s))
    d.text((16 * s, H - 68 * s), "SAMPLE", font=title, fill=PAPER)
    d.text((16 * s, H - 47 * s), "GRABBER", font=title, fill=PAPER)
    tracked(d, (16 * s, H - 22 * s), "VST3 \u00b7 STANDALONE", mono, SUB, 0.8 * s)
    return img.resize((w, h), Image.LANCZOS)


def small_image(px):
    """Top-right header image: the mark on ink, no tile."""
    S = px * SS
    img = Image.new("RGB", (S, S), INK)
    d = ImageDraw.Draw(img)
    hs = [0.30, 0.46, 0.62, 0.80, 1.0, 0.86, 0.66, 0.48, 0.32]
    sel = (3, 5)
    n = len(hs)
    pitch = S * 0.78 / n
    bw = pitch * 0.5
    x0 = (S - (n * pitch - (pitch - bw))) / 2
    cy = S / 2
    for i, a in enumerate(hs):
        hh = a * S * 0.56
        x = x0 + i * pitch
        d.rectangle([x, cy - hh / 2, x + bw, cy + hh / 2], fill=PAPER if sel[0] <= i <= sel[1] else DIM)
    gap = pitch - bw
    xl = x0 + sel[0] * pitch - gap / 2 - S * 0.03
    xr = x0 + sel[1] * pitch + bw + gap / 2 + S * 0.03
    brackets(d, xl, xr, S * 0.18, S * 0.82, max(SS, S * 0.035), S * 0.07)
    return img.resize((px, px), Image.LANCZOS)


def main():
    os.makedirs(OUT, exist_ok=True)
    # 100/125/150/175/200/250 % (Inno picks the closest)
    for pct in (100, 125, 150, 175, 200, 250):
        w, h = round(202 * pct / 100), round(386 * pct / 100)
        wizard_image(w, h).save(os.path.join(OUT, f"wizard-{pct}.png"), optimize=True)
        sp = round(58 * pct / 100)
        small_image(sp).save(os.path.join(OUT, f"small-{pct}.png"), optimize=True)
    # Setup.exe icon from the tracked app icon
    icon = Image.open(os.path.join(ROOT, "brand", "icon", "icon-256.png")).convert("RGBA")
    icon.save(os.path.join(OUT, "setup.ico"), sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])


if __name__ == "__main__":
    main()
