# SPDX-FileCopyrightText: 2026 Treader contributors
# SPDX-License-Identifier: Apache-2.0
# 把设计稿 docs/design/ui.html 里每台“设备”的手绘示意换成真实界面代码画出的截图（build/page-render-demo），
# 截图以 16 级灰度 PNG 内嵌，文件下载后单独打开就能看。每个图用哪张渲染图见 ui_doc_scenes.json（按图的标题）。
# 可以反复运行：已经是截图的图换成新渲染；改说明或加新图直接改 ui.html，再在 json 里加一行。
# 先跑演示渲染（run.sh demo_*），再在仓库根运行 python tools/page_render/build_design_doc.py（需要 Pillow）。
# Replace every device mockup in docs/design/ui.html with a render of the real UI code (build/page-render-demo),
# embedded as 16-gray PNGs so the downloaded file opens on its own. ui_doc_scenes.json says which render each figure
# uses (by figure title). It can run again: screenshots are replaced by fresh renders; to change a caption or add a
# figure, edit ui.html directly and add a line to the json. Render the demo scenes first (run.sh demo_*), then run
# python tools/page_render/build_design_doc.py from the repository root (needs Pillow).
import base64
import io
import json
import re

from PIL import Image

DOC = "docs/design/ui.html"
SRC = "build/page-render-demo"


def png_data_uri(path):
    """半尺寸显示，原尺寸保存；量化成 16 级灰度让文件小。/ Keep full size, quantized to 16 grays to stay small."""
    im = Image.open(path).convert("L")
    pal = im.quantize(colors=16, method=Image.Quantize.MEDIANCUT)
    buf = io.BytesIO()
    pal.save(buf, format="PNG", optimize=True)
    return "data:image/png;base64," + base64.b64encode(buf.getvalue()).decode()


def frame(path, alt):
    return ('<div class="frame shot"><img src="%s" width="342" height="608" alt="%s"></div>\n'
            % (png_data_uri(path), alt.replace('"', "")))


def title_of(cap):
    m = re.search(r"<b>(.*?)</b>", cap, re.S)
    return re.sub(r"<[^>]+>", "", m.group(1)).strip() if m else ""


def main():
    s = open(DOC, encoding="utf-8").read()
    # 图按标题对应渲染图（标题在 <figcaption> 的 <b> 里）。/ Figures map to renders by title (the <b> in <figcaption>).
    scenes = json.load(open("tools/page_render/ui_doc_scenes.json", encoding="utf-8"))
    out, last, done = [], 0, 0
    for m in re.finditer(r"<figure>(.*?)</figure>", s, re.S):
        body = m.group(1)
        new_body = body
        if 'class="frame' in body and "<figcaption>" in body:
            cap = body[body.index("<figcaption>"):].strip()
            title = title_of(cap)
            if title in scenes:
                new_body = "\n" + frame("%s/%s" % (SRC, scenes[title]), title) + cap + "\n"
                done += 1
        out.append(s[last:m.start()])
        out.append("<figure>" + new_body + "</figure>")
        last = m.end()
    out.append(s[last:])
    s = "".join(out)
    # 截图铺满外框。/ Screenshots fill the frame.
    if ".frame.shot img" not in s:
        s = s.replace("</style>", "  .frame.shot img { display: block; width: 342px; height: 608px; }\n</style>", 1)
    open(DOC, "w", encoding="utf-8", newline="\n").write(s)
    print("replaced:", done, "of", len(scenes), "size:", len(s.encode()))


if __name__ == "__main__":
    main()
