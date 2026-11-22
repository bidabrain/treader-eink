"""webtoon hub 离线测试：不联网，用合成的网页片段和图片测解析、切页、打包和设备接口。

Offline tests for the webtoon hub: no network; synthetic page snippets and images exercise parsing, page cutting,
packing and the device API.

运行 / Run:  docker run --rm -v "$PWD/webtoon/hub:/srv" -w /srv treader-webtoon:dev sh -c "pip -q install pytest && pytest -q tests"
"""
import asyncio
import io
import os
import tempfile
import zipfile

os.environ["DATA_DIR"] = tempfile.mkdtemp()

from fastapi.testclient import TestClient  # noqa: E402
from PIL import Image, ImageDraw  # noqa: E402

from app import config, convert, jobs, main, source  # noqa: E402

CARD = '''<li><a href="https://www.webtoons.com/zh-hant/fantasy/x/list?title_no={no}" class="link _genre_title_a"
 data-title-no="{no}" {extra}><div class="image_wrap" data-title-unsuitable-for-children="{adult}"><img
 src="https://img/{no}.png?type=q90" width="192"></div><div class="info_text"><div class="genre">奇幻冒險</div>
 <strong class="title">{title}</strong><div class="author">作者{no}</div><div class="view_count type_like">1.2萬</div>
 </div></a></li>'''


def test_parse_cards_and_genres():
    page = (CARD.format(no=1, title="勇者&amp;魔王", extra="", adult="false")
            + CARD.format(no=1, title="重複", extra="", adult="false")
            + CARD.format(no=7, title="業餘", extra='data-webtoon-type="CHALLENGE"', adult="true"))
    cards = source.parse_cards(page)
    assert [c["id"] for c in cards] == ["1", "c7"]
    assert cards[0]["title"] == "勇者&魔王" and cards[0]["likes"] == 12000 and cards[0]["author"] == "作者1"
    assert cards[1]["canvas"] and cards[1]["adult"]
    g = source.parse_genres('<a href="https://x/zh-hant/genres/romance?sortOrder=MANA" class="snb_tab" '
                            'data-genre="ROMANCE"> 愛情</a>')
    assert g == [{"key": "romance", "name": "愛情"}]


def test_parse_title_and_images():
    page = ('<h1 class="subj">作品</h1><p class="summary">第一行<br>第二行</p>'
            '<div class="author_area"> A , B <button>x</button></div><h2 class="genre g_fantasy">奇幻</h2>'
            '<meta property="og:image" content="https://img/c.png">')
    t = source.parse_title(page)
    assert t["title"] == "作品" and t["summary"] == "第一行\n第二行" and t["author"] == "A、B" and t["genre"] == "奇幻"
    imgs = source.parse_images('<img src="x" class="_images" data-url="https://a/1.png?type=opti&amp;x=1">'
                               '<img data-url="https://a/2.png" class="_images">')
    assert imgs == ["https://a/1.png?type=opti&x=1", "https://a/2.png"]


def test_num():
    assert source._num("95,421") == 95421 and source._num("1.5M") == 1500000 and source._num("") == 0


def _strip(h: int, panels: list[tuple[int, int]], w: int = 800) -> bytes:
    """一张白底长条，panels 里是黑框画格 (y0, y1)。/ A white strip with black-framed panels (y0, y1)."""
    im = Image.new("RGB", (w, h), "white")
    d = ImageDraw.Draw(im)
    for y0, y1 in panels:
        d.rectangle((20, y0, w - 20, y1), fill=(90, 90, 90), outline="black", width=4)
    out = io.BytesIO()
    im.save(out, "PNG")
    return out.getvalue()


def test_pager_cuts_in_gutters():
    # 800 宽缩到 648：画格之间留 120 像素空白，切点应该都落在空白里。/ 800 wide scales to 648; with 120 px gutters
    # every cut should land in a gutter.
    panels = [(y, y + 700) for y in range(100, 6000, 820)]
    p = convert.Pager(648, 1000)
    p.feed(_strip(3000, [(a, b) for a, b in panels if b < 3000]))
    p.feed(_strip(3200, [(a - 3000, b - 3000) for a, b in panels if a >= 3000 and b < 6200]))
    pages = p.finish()
    assert pages and all(pg.width == 648 and pg.height <= 1000 for pg in pages)
    for pg in pages:
        # 每页上下边都是空白（没切进画格）。/ Each page's top and bottom rows are blank (no panel was cut).
        assert pg.crop((0, pg.height - 1, 648, pg.height)).getextrema()[0] > 200
        assert pg.crop((0, 0, 648, 1)).getextrema()[0] > 200


def test_pager_drops_blank_and_merges_tail():
    p = convert.Pager(648, 1000)
    p.feed(_strip(4000, [(100, 900)]))        # 后面 3000 多像素都是空白 / 3000+ px of blank after
    pages = p.finish()
    assert len(pages) == 1


def test_pager_sink_streams_pages(tmp_path):
    # 有 sink 时内存里最多留两页，切出的页和不用 sink 一样。/ With a sink at most two pages stay in memory and the
    # pages match the plain mode.
    panels = [(y, y + 700) for y in range(100, 6000, 820)]
    strip = _strip(6200, panels)
    plain = convert.Pager(648, 1000)
    plain.feed(strip)
    want = plain.finish()
    path = tmp_path / "x.cbz"
    w = convert.CbzWriter(path, 80)
    held = []
    p = convert.Pager(648, 1000, sink=lambda pg: (held.append(len(p.pages)), w.add(pg)))
    p.feed(strip)
    for pg in p.finish():
        w.add(pg)
    w.close({"no": 1})
    assert max(held) <= 3 and p.count == len(want)
    assert len(zipfile.ZipFile(path).namelist()) == len(want) + 1


def test_cbz_and_cover():
    pg = Image.new("L", (648, 900), 128)
    data = convert.cbz([pg, pg], 80, {"series": "A&B", "no": 3, "title": "t", "lang": "en"})
    z = zipfile.ZipFile(io.BytesIO(data))
    assert z.namelist() == ["0001.jpg", "0002.jpg", "ComicInfo.xml"]
    assert z.getinfo("0001.jpg").compress_type == zipfile.ZIP_STORED
    assert "<Series>A&amp;B</Series>" in z.read("ComicInfo.xml").decode()
    jpg = z.read("0001.jpg")
    assert jpg[:3] == b"\xff\xd8\xff" and b"\xff\xc2" not in jpg        # 基线 JPEG / Baseline JPEG
    c = Image.open(io.BytesIO(convert.cover(_strip(400, [(10, 300)], 300))))
    assert c.size == (150, 200) and c.mode == "L"


def _fake_source(monkeypatch):
    async def store(lang, seg, key=""):
        return {"seg": seg, "key": "romance", "keys": [{"key": "romance", "name": "愛情"}],
                "items": [{"id": "5", "title": "戀愛", "author": "甲", "genre": "愛情", "likes": 3, "cover": "u",
                           "canvas": False, "adult": False}]}

    async def episodes(tid):
        return [{"no": n, "title": f"第{n}話", "date": 0} for n in (1, 2, 3)]

    async def title(lang, tid):
        return {"id": tid, "title": "戀愛", "summary": "說明", "author": "甲", "genre": "愛情", "cover": "",
                "done": False, "episodes": await episodes(tid), "latest": 3}

    async def images(lang, tid, no):
        return ["a", "b"]

    async def get_image(url, pace=True):
        return _strip(1200, [(100, 1100)])

    for name, fn in [("store", store), ("episodes", episodes), ("title", title), ("images", images),
                     ("get_image", get_image)]:
        monkeypatch.setattr(source, name, fn)


def test_device_api(monkeypatch):
    _fake_source(monkeypatch)
    with TestClient(main.app) as c:
        tok = config.load()["device_token"]
        h = {"x-webtoon-token": tok}
        assert c.get("/api/device/v1/store").status_code == 401
        r = c.get("/api/device/v1/store?seg=genre", headers=h).json()
        assert r["items"][0]["title"] == "恋爱" and r["keys"][0]["name"] == "爱情"     # 转成简体 / Simplified
        r = c.get("/api/device/v1/store?seg=genre&zh=t", headers=h).json()
        assert r["items"][0]["title"] == "戀愛"
        r = c.post("/api/device/v1/check", headers=h, json={"titles": [{"id": "5", "last": 1}, {"id": "../x"}]}).json()
        assert [e["no"] for e in r["titles"][0]["new"]] == [2, 3] and r["titles"][1]["new"] == []
        assert c.get("/api/device/v1/episode/5/0", headers=h).status_code == 400
        assert c.get("/api/device/v1/episode/x5/1", headers=h).status_code == 400
        st = c.get("/api/device/v1/episode/5/2", headers=h).json()
        assert st["state"] in ("pending", "ready")
        for _ in range(100):
            st = c.get("/api/device/v1/episode/5/2", headers=h).json()
            if st["state"] != "pending":
                break
            asyncio.run(asyncio.sleep(0.05))
        assert st["state"] == "ready", st
        r = c.get("/api/device/v1/episode/5/2.cbz", headers=h)
        assert r.status_code == 200 and zipfile.ZipFile(io.BytesIO(r.content)).namelist()[-1] == "ComicInfo.xml"
        assert c.get("/api/device/v1/episode/5/3.cbz", headers=h).status_code == 404
        # 隧道来的请求只能到设备接口。/ Tunnel requests reach only the device API.
        assert c.get("/api/panel/state", headers={"cf-connecting-ip": "1.2.3.4"}).status_code == 403
        assert c.get("/api/device/v1/store", headers={**h, "cf-connecting-ip": "1.2.3.4"}).status_code == 200
        assert c.get("/api/panel/status").status_code == 401


def test_prune():
    p = jobs.cbz_path("en", "9", 1)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(b"x" * 10)
    old = 1_000_000
    os.utime(p, (old, old))
    assert jobs.prune() >= 1 and not p.exists()
