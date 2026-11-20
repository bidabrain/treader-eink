# SPDX-FileCopyrightText: 2026 Treader contributors
# SPDX-License-Identifier: Apache-2.0
# 把演示渲染（build/page-render-demo，真实页面代码画的）拼成 README 和网站用的预览图：每张图四台“手机”加说明，
# site-hero 是深色底上的三台。清单在 previews.json。用法（仓库根，需要 Pillow）：
#   python tools/page_render/compose_previews.py [输出目录，默认 docs/images]
# Assemble the demo renders (build/page-render-demo, drawn by the real page code) into the README and site previews:
# four "phones" with captions per image, and site-hero with three on a dark background. The list is previews.json.
# Usage (repository root, needs Pillow): python tools/page_render/compose_previews.py [out dir, default docs/images]
import json
import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

SRC = "build/page-render-demo"
FONT = "sdcard/fonts/ChillDuanSansVF.ttf"
BG = (234, 232, 227)
FRAME = (248, 247, 244)
EDGE = (212, 209, 202)


def font(px):
    try:
        f = ImageFont.truetype(FONT, px)
        try:
            f.set_variation_by_axes([700])
        except Exception:
            pass
        return f
    except OSError:
        return ImageFont.load_default()


def phone(screen, width):
    """一台手机：圆角外框、白边、屏幕。/ One phone: rounded frame, bezel, screen."""
    pad = max(6, width // 42)
    sw = width - 2 * pad
    sh = round(sw * 1216 / 684)
    shot = screen.convert("RGB").resize((sw, sh), Image.LANCZOS)
    h = sh + 2 * pad
    radius = max(14, width // 15)
    body = Image.new("RGBA", (width, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(body)
    d.rounded_rectangle((0, 0, width - 1, h - 1), radius, fill=FRAME, outline=EDGE, width=2)
    mask = Image.new("L", (sw, sh), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, sw - 1, sh - 1), max(6, radius - pad), fill=255)
    body.paste(shot, (pad, pad), mask)
    return body


def shadowed(canvas, img, xy, strength=60):
    """贴一台带淡阴影的手机。/ Paste a phone with a soft shadow."""
    sh = Image.new("RGBA", (img.width + 40, img.height + 40), (0, 0, 0, 0))
    a = img.split()[3].point(lambda v: strength if v else 0)
    sh.paste((0, 0, 0, 255), (20, 24), a)
    sh = sh.filter(ImageFilter.GaussianBlur(10))
    canvas.alpha_composite(sh, (xy[0] - 20, xy[1] - 20))
    canvas.alpha_composite(img, xy)


def strip(items, out):
    """1508×708：四台一排，下面写说明。/ 1508x708: four in a row with captions below."""
    W, H, n = 1508, 708, len(items)
    canvas = Image.new("RGBA", (W, H), BG + (255,))
    pw, top = 334, 50
    gap = (W - 2 * 44 - n * pw) // max(1, n - 1)
    cap = font(20)
    d = ImageDraw.Draw(canvas)
    for i, (path, caption) in enumerate(items):
        p = phone(Image.open(os.path.join(SRC, path)), pw)
        x = 44 + i * (pw + gap)
        shadowed(canvas, p, (x, top), 40)
        tw = d.textlength(caption, font=cap)
        d.text((x + (pw - tw) / 2, top + p.height + 14), caption, fill=(58, 56, 52), font=cap)
    canvas.convert("RGB").save(out, optimize=True)


def hero(items, out):
    """2400×1400：深色底，三台手机居中，中间那台大一点。/ 2400x1400: dark background, three phones, the middle larger."""
    W, H = 2400, 1400
    canvas = Image.new("RGBA", (W, H), (24, 24, 24, 255))
    widths = [600, 680, 600]
    phones = [phone(Image.open(os.path.join(SRC, p)), w) for (p, _), w in zip(items, widths)]
    total = sum(widths) + 2 * 80
    x = (W - total) // 2
    for i, p in enumerate(phones):
        y = (H - p.height) // 2 + (0 if i == 1 else 60)
        shadowed(canvas, p, (x, y), 140)
        x += p.width + 80
    canvas.save(out, optimize=True)


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "docs/images"
    os.makedirs(out_dir, exist_ok=True)
    spec = json.load(open(os.path.join(os.path.dirname(__file__), "previews.json"), encoding="utf-8"))
    for name, items in spec.items():
        out = os.path.join(out_dir, name + ".png")
        (hero if name == "site-hero" else strip)(items, out)
        print("wrote", out)


if __name__ == "__main__":
    main()
