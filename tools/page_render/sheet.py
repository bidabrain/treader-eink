# SPDX-FileCopyrightText: 2026 Treader contributors
# SPDX-License-Identifier: Apache-2.0
# 把 build/page-render 里同一场景的 100 / 115 / 130 三张图拼成一行，一组页面一张总览图，写到输出目录。
# 用法：python tools/page_render/sheet.py 输出目录 [组名=页/场景,页/场景… …]（不给组时用默认的审阅组）。需要 Pillow。
# Put the 100 / 115 / 130 shots of each scene side by side, one overview sheet per group, into the output directory.
# Usage: python tools/page_render/sheet.py OUT_DIR [group=page/scene,page/scene … …] (default review groups when
# none are given). Needs Pillow.
import os
import sys

from PIL import Image, ImageDraw, ImageFont

SRC = "build/page-render"
SCALE = 0.42
DEFAULT = {
    "1_nav_settings": ["all/default", "settings/home", "settings/display", "settings/tabs_worst", "settings/about",
                       "settings/edit_host_url"],
    "2_library_reader": ["book/shelf_card", "book/shelf_search", "book/reader_toolbar", "book/reader_typeset",
                         "book/toc", "lock/card"],
    "3_comic_transfer": ["comic/shelf", "comic/series", "comic/toolbar_series", "transfer/home_ap_worst",
                         "transfer/networks_saved", "usb/intro"],
    "4_tomato_store": ["tomato/list_pending", "tomato/search", "tomato/detail", "store/home_male", "store/rank",
                       "kbd/pinyin_cands"],
    "5_hupu": ["hupu/feed", "hupu/scores", "hupu/rate", "hupu/zones", "hupu/post_body", "hupu/image"],
    "6_social": ["weibo/list_p1", "weibo/post_p1", "weibo/comments", "x/following_off", "xhs/list_p1",
                 "xhs/note_p1"],
    "7_system": ["pin/set", "update/files", "wall/list", "wall/wallsave_bar", "card/info", "font/first_page"],
}


def main():
    out = sys.argv[1]
    groups = DEFAULT
    if len(sys.argv) > 2:
        groups = {g.split("=")[0]: g.split("=")[1].split(",") for g in sys.argv[2:]}
    os.makedirs(out, exist_ok=True)
    try:
        font = ImageFont.truetype("sdcard/fonts/ChillDuanSansVF.ttf", 22)
    except OSError:
        font = ImageFont.load_default()
    w, h = int(684 * SCALE), int(1216 * SCALE)
    gap, label = 16, 34
    for name, scenes in groups.items():
        sheet = Image.new("L", (3 * w + 4 * gap, len(scenes) * (h + label + gap) + gap), 200)
        draw = ImageDraw.Draw(sheet)
        for r, scene in enumerate(scenes):
            y = gap + r * (h + label + gap)
            for c, pct in enumerate((100, 115, 130)):
                x = gap + c * (w + gap)
                path = os.path.join(SRC, f"{scene}_{pct}.png")
                draw.text((x, y), f"{scene}  {pct}%", fill=0, font=font)
                if os.path.exists(path):
                    sheet.paste(Image.open(path).convert("L").resize((w, h), Image.LANCZOS), (x, y + label))
        sheet.save(os.path.join(out, f"{name}.png"))
        print("wrote", os.path.join(out, f"{name}.png"))


if __name__ == "__main__":
    main()
