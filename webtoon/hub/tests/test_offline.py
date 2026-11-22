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


# ---- Mihon 插件来源 / Mihon extension sources ----

from app import mihon  # noqa: E402


def _ch(cid, order, num, url=None, name=None):
    return {"id": cid, "sourceOrder": order, "chapterNumber": num, "url": url or f"/c/{cid}",
            "name": name or f"Ch {num}", "uploadDate": "1795311762452"}


def test_assign_numbers_stay_put():
    # 第一次按来源顺序从旧到新编号；之后新章节接在后面，补档也不打乱已有话号。/ First numbering follows source order
    # oldest first; later chapters append and a back-filled chapter never renumbers the others.
    eps = mihon.assign(101, [_ch(11, 1, 1), _ch(12, 2, 2), _ch(13, 3, 3)])
    assert [(e["no"], e["chapter"]) for e in eps] == [(1, 11), (2, 12), (3, 13)]
    eps = mihon.assign(101, [_ch(11, 1, 1), _ch(15, 2, 1.5), _ch(12, 3, 2), _ch(13, 4, 3), _ch(14, 5, 4)])
    assert [(e["no"], e["chapter"]) for e in eps] == [(1, 11), (2, 12), (3, 13), (4, 15), (5, 14)]
    # 章节 id 变了（插件刷新）话号不变；消失的章节不列出。/ A changed chapter id keeps its number; vanished chapters
    # are not listed.
    eps = mihon.assign(101, [_ch(21, 1, 1, url="/c/11"), _ch(13, 2, 3)])
    assert [(e["no"], e["chapter"]) for e in eps] == [(1, 21), (3, 13)]
    assert all(e["date"] == 1795311762 for e in eps)


def test_assign_flipped_order_and_dedupe():
    # sourceOrder 反过来的插件也是从旧到新；同一话号的第二个版本不列出；没有话号的章节照样编号。
    # An extension with reversed sourceOrder still numbers oldest first; a second version of a chapter number is
    # dropped; chapters without a number still get one.
    eps = mihon.assign(202, [_ch(1, 1, 3), _ch(2, 2, 2), _ch(3, 3, 2, name="other group"), _ch(4, 4, 1),
                             _ch(5, 5, -1, name="Extra")])
    assert [e["title"] for e in eps] == ["Extra", "Ch 1", "other group", "Ch 3"]
    eps = mihon.assign(203, [_ch(1, 1, 1), _ch(2, 2, 1, name="dup")], dedupe=False)
    assert [e["no"] for e in eps] == [1, 2]


def _img(w, h):
    im = Image.new("RGB", (w, h), "white")
    ImageDraw.Draw(im).rectangle((10, 10, w - 10, h - 10), fill=(60, 60, 60))
    out = io.BytesIO()
    im.save(out, "JPEG")
    return out.getvalue()


def test_page_pager_and_spreads():
    p = convert.PagePager(648, 1000)
    p.feed(_img(800, 1200))
    p.feed(_img(1600, 1200))                    # 跨页拆成两页 / A spread splits in two
    pages = p.finish()
    assert len(pages) == 3 and all(pg.width <= 648 and pg.height <= 1000 for pg in pages)
    assert pages[0].width == 648 or pages[0].height == 1000
    rot = convert.PagePager(648, 1000, spread="rotate")
    rot.feed(_img(1600, 1200))
    (pg,) = rot.finish()
    assert pg.height > pg.width
    # 右页在前：左半边画个白块，右页先出来就没有白块。/ Right page first: mark the left half and the first page
    # must be the unmarked right half.
    im = Image.new("L", (1600, 1200), 60)
    ImageDraw.Draw(im).rectangle((100, 100, 300, 300), fill=255)
    buf = io.BytesIO()
    im.save(buf, "PNG")
    rtl = convert.PagePager(648, 1000)
    rtl.feed(buf.getvalue())
    first, second = rtl.finish()
    assert first.getextrema()[1] < 200 and second.getextrema()[1] > 200


def test_auto_pager_decides():
    a = convert.AutoPager(648, 1000)
    for _ in range(3):
        a.feed(_strip(3000, [(100, 900), (1100, 2900)]))
    a.finish()
    assert a.strip and a.count >= 3
    b = convert.AutoPager(648, 1000)
    for _ in range(2):
        b.feed(_img(800, 1200))
    assert len(b.finish()) == 2 and not b.strip     # 不到 3 张也能收尾 / Finishes with fewer than 3 images


def _fake_mihon(monkeypatch):
    async def gql(query, variables=None, partial=False):
        if "sources" in query and "totalCount" in query:
            return {"sources": {"totalCount": 2}}
        if "sources" in query:
            return {"sources": {"nodes": [
                {"id": "0", "name": "Local source", "lang": "localsourcelang", "displayName": "Local source",
                 "supportsLatest": True, "contentWarning": "SAFE"},
                {"id": "77", "name": "Shelf", "lang": "en", "displayName": "Shelf (EN)", "supportsLatest": False,
                 "contentWarning": "SAFE"},
                {"id": "88", "name": "Adult", "lang": "en", "displayName": "Adult", "supportsLatest": True,
                 "contentWarning": "NSFW"}]}}
        if "fetchSourceManga" in query:
            return {"fetchSourceManga": {"hasNextPage": variables["page"] < 2, "mangas": [
                {"id": variables["page"], "title": f"T{variables['page']}", "author": None, "artist": "Art",
                 "thumbnailUrl": None}]}}
        if "fetchMangaAndChapters" in query:
            return {"fetchMangaAndChapters": {
                "manga": {"id": variables["id"], "title": "Pages", "author": "Au", "artist": None, "description": " d ",
                          "genre": ["A", "B"], "status": "COMPLETED", "thumbnailUrl": None, "sourceId": "77",
                          "url": f"/m/{variables['id']}", "realUrl": "https://x/m", "source": {"displayName": "Shelf"}},
                "chapters": [_ch(31, 1, 1), _ch(32, 2, 2)]}}
        if "fetchChapterPages" in query:
            return {"fetchChapterPages": {"pages": ["/p/0", "/p/1"]}}
        raise AssertionError(query)

    async def get_image(url, pace=True):
        return _img(800, 1200)

    monkeypatch.setattr(mihon, "gql", gql)
    monkeypatch.setattr(mihon, "get_image", get_image)
    monkeypatch.setattr(mihon, "_alive", (0.0, False))
    mihon.forget()


def test_device_api_mihon(monkeypatch):
    _fake_source(monkeypatch)
    _fake_mihon(monkeypatch)
    with TestClient(main.app) as c:
        h = {"x-webtoon-token": config.load()["device_token"]}
        r = c.get("/api/device/v1/sources", headers=h).json()
        # 成人来源默认不显示，本地来源按名字排在后面。/ Adult sources hidden by default; local sorts by name.
        assert r["mihon"] and [s["key"] for s in r["items"]] == ["webtoon", "77", "0"]
        r = c.get("/api/device/v1/store?src=77&seg=pop", headers=h).json()
        assert [i["id"] for i in r["items"]] == ["m1", "m2"] and r["items"][0]["author"] == "Art"
        assert r["items"][0]["src"] == "Shelf (EN)" and r["keys"] == []
        assert c.get("/api/device/v1/store?src=abc", headers=h).status_code == 400
        t = c.get("/api/device/v1/title/m9?lang=zh-hant", headers=h).json()
        assert t["done"] and t["latest"] == 2 and t["summary"] == "d" and t["genre"] == "A、B"
        assert [e["no"] for e in t["episodes"]] == [1, 2]
        r = c.post("/api/device/v1/check", headers=h,
                   json={"titles": [{"id": "m9", "last": 1}, {"id": "5", "last": 2}]}).json()
        assert [e["no"] for e in r["titles"][0]["new"]] == [2] and [e["no"] for e in r["titles"][1]["new"]] == [3]
        for _ in range(100):
            st = c.get("/api/device/v1/episode/m9/2?lang=zh-hant", headers=h).json()
            if st["state"] != "pending":
                break
            asyncio.run(asyncio.sleep(0.05))
        assert st["state"] == "ready", st
        z = zipfile.ZipFile(io.BytesIO(c.get("/api/device/v1/episode/m9/2.cbz", headers=h).content))
        assert z.namelist() == ["0001.jpg", "0002.jpg", "ComicInfo.xml"]
        info = z.read("ComicInfo.xml").decode()
        assert "<Manga>YesAndRightToLeft</Manga>" in info and "<Series>Pages</Series>" in info
        assert jobs.cbz_path("mihon", "m9", 2).exists()
        assert c.get("/api/device/v1/episode/m9x/1", headers=h).status_code == 400
        # 面板隐藏来源、打开成人来源。/ The panel hides a source and allows adult ones.
        c.post("/api/panel/setup", json={"password": "password123"})
        r = c.post("/api/panel/mihon/settings", json={"mihon_hidden": ["0"], "mihon_nsfw": True,
                                                        "mihon_order": ["88", "77"]}).json()
        assert [s["key"] for s in r["items"]] == ["webtoon", "88", "77", "0"] and r["items"][3]["hidden"]
        r = c.get("/api/device/v1/sources", headers=h).json()
        assert [s["key"] for s in r["items"]] == ["webtoon", "88", "77"]
        assert c.post("/api/panel/mihon/settings", json={"mihon_spread": "x"}).status_code == 400


def test_mihon_identity_guard(monkeypatch):
    _fake_mihon(monkeypatch)
    asyncio.run(mihon.title("m500"))
    mihon.forget()
    # 同一个 id 的作品变了（Suwayomi 数据库重置过）：拒绝。/ The same id now names another title (Suwayomi's database
    # was reset): refuse.
    with mihon._db_lock:
        con = mihon._db()
        con.execute("UPDATE mangas SET url = '/other' WHERE manga = 500")
        con.commit()
        con.close()
    try:
        asyncio.run(mihon.title("m500"))
        raise AssertionError("expected SourceError")
    except source.SourceError as e:
        assert e.status == 409


def test_mihon_unavailable(monkeypatch):
    async def down(*a, **k):
        raise source.SourceError(502, "down")
    monkeypatch.setattr(mihon, "gql", down)
    monkeypatch.setattr(mihon, "_alive", (0.0, False))
    with TestClient(main.app) as c:
        h = {"x-webtoon-token": config.load()["device_token"]}
        r = c.get("/api/device/v1/sources", headers=h).json()
        assert r == {"mihon": False, "items": [{"key": "webtoon", "name": "WEBTOON", "lang": "", "latest": False,
                                                "nsfw": False}]}
