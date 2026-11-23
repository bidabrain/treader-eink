"""webtoon hub 离线测试：不联网，用合成的网页片段和图片测解析、切页、打包和设备接口。

Offline tests for the webtoon hub: no network; synthetic page snippets and images exercise parsing, page cutting,
packing and the device API.

运行 / Run:  docker run --rm -v "$PWD/webtoon/hub:/srv" -w /srv treader-webtoon:dev sh -c "pip -q install pytest && pytest -q tests"
"""
import asyncio
import io
import json
import os
import re
import tempfile
import time
import zipfile

import httpx

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


def test_assign_name_sorted_source_and_renumber():
    # 有的插件按章节名的字符串排（第100话…第109话、第10话、第110话…）：按章节号编，第 1 话是第1话；没有章节号的
    # 番外跟在来源顺序里它前面那一章后面。/ Some extensions sort by name as strings (第100话 … 第109话, 第10话,
    # 第110话 …): number by chapter number so episode 1 is 第1话; an extra without a number follows the chapter
    # before it in source order.
    names = sorted([f"第{n}话" for n in range(1, 121)])
    chapters = [_ch(1000 + i, i + 1, float(n[1:-1]), name=n) for i, n in enumerate(names)]
    before = names[2]                                           # 来源顺序里番外前面那一章 / The chapter before the extra
    chapters.insert(3, _ch(9999, 3.5, -1, name="番外"))
    eps = mihon.assign(404, chapters)
    titles = [e["title"] for e in eps]
    assert titles[:3] == ["第1话", "第2话", "第3话"] and titles[9:11] == ["第10话", "第11话"]
    assert titles.index("番外") == titles.index(before) + 1 and titles[-1] == "第120话"
    assert [e["no"] for e in eps] == list(range(1, 122))
    # 编错的旧编号可以清掉重编。/ Wrong old numbers can be cleared and assigned again.
    with mihon._db_lock:
        con = mihon._db()
        con.execute("UPDATE chapters SET no = 999 WHERE manga = 404 AND name = '第1话'")
        con.commit()
        con.close()
    assert mihon.assign(404, chapters)[-1]["title"] == "第1话"
    assert mihon.renumber("m404") == 121
    assert mihon.assign(404, chapters)[0]["title"] == "第1话"


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
    monkeypatch.setattr(mihon, "_alive", (float("-inf"), False))
    mihon.forget()


def test_device_api_mihon(monkeypatch):
    _fake_source(monkeypatch)
    _fake_mihon(monkeypatch)
    with TestClient(main.app) as c:
        h = {"x-webtoon-token": config.load()["device_token"]}
        r = c.get("/api/device/v1/sources", headers=h).json()
        # 成人来源也发给设备（只带 nsfw 标记），按名字排。/ Adult sources reach the device too (flagged nsfw only),
        # sorted by name.
        assert r["mihon"] and [s["key"] for s in r["items"]] == ["webtoon", "88", "77", "0"]
        assert [s["nsfw"] for s in r["items"]] == [False, True, False, False]
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
        # 面板里用户自己勾掉的来源才不发。/ Only sources the user unticked in the panel are left out.
        c.post("/api/panel/setup", json={"password": "password123"})
        r = c.post("/api/panel/mihon/settings", json={"mihon_hidden": ["0"], "mihon_order": ["88", "77"]}).json()
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
    monkeypatch.setattr(mihon, "_alive", (float("-inf"), False))
    with TestClient(main.app) as c:
        h = {"x-webtoon-token": config.load()["device_token"]}
        r = c.get("/api/device/v1/sources", headers=h).json()
        assert r == {"mihon": False, "items": [{"key": "webtoon", "name": "WEBTOON", "lang": "", "latest": False,
                                                "nsfw": False}]}


def test_cloudflare_hint():
    from app import mihon
    assert mihon.CF_HINT in mihon._short("Exception while fetching data (/x) : java.lang.Exception: Cloudflare bypass currently disabled")
    assert mihon.CF_HINT in mihon.cf_hint("java.net.UnknownHostException: flaresolverr")
    assert mihon.CF_HINT not in mihon.cf_hint("FlareSolverr failed to solve the challenge")
    assert mihon._short("plain error") == "plain error"


def test_cloudflare_pauses_source(monkeypatch):
    """一个插件来源卡在 Cloudflare 上后，它排着的话直接失败，不再挨个去试；别的来源照常。/ Once an extension source
    fails a Cloudflare check its queued episodes fail at once; other sources still convert."""
    from app import mihon
    calls = []

    async def info(lang, tid):
        return {"title": tid, "author": "", "episodes": [], "src": "A" if tid in ("m1", "m2") else "B"}

    async def convert_(key, meta):
        calls.append(key[1])
        if key[1] == "m1":
            raise source.SourceError(502, mihon.cf_hint("java.io.IOException: CFClearance$CloudflareBypassException"))

    monkeypatch.setattr(jobs, "_info", info)
    monkeypatch.setattr(jobs, "_convert", convert_)
    monkeypatch.setattr(jobs, "_cf_block", {})

    async def run():
        task = asyncio.create_task(jobs.worker())
        await asyncio.sleep(0)
        for tid in ("m1", "m2", "m3"):
            jobs.status("", tid, 1)
        while not jobs._queue.empty() or len(calls) + 1 < 3:
            await asyncio.sleep(0.01)
        await asyncio.sleep(0.01)
        task.cancel()

    asyncio.run(run())
    assert calls == ["m1", "m3"]
    st = jobs.status("", "m2", 1, start=False)
    assert st["state"] == "error" and mihon.CF_FAIL_HINT in st["error"] and "分钟后再试" in st["error"]
    assert jobs.status("", "m3", 1, start=False)["state"] == "none"


# ---- E-Hentai ----

from app import ehentai  # noqa: E402

EH_LIST = ('<table class="itg gltc"><tr><td><a href="https://e-hentai.org/g/4350546/5fc5383ad3/">A</a></td>'
           '<td><a href="https://e-hentai.org/g/4350546/5fc5383ad3/">A again</a></td>'
           '<td><a href="https://e-hentai.org/g/123/0439fa3666/">B</a></td></tr></table>'
           '<a id="dnext" href="https://e-hentai.org/?f_search=x&amp;next=4350259">Next</a>')


def _eh_gallery(gid, start, end):
    return "".join(f'<a href="https://e-hentai.org/s/{i:010x}/{gid}-{i}"><div title="Page {i}: x.jpg"></div></a>'
                   for i in range(start, end + 1))


def test_eh_ids_and_parsing():
    tid = ehentai.make_id(4350546, "5fc5383ad3")
    assert len(tid) == 15 and ehentai.ID_RE.match(tid) and ehentai.split_id(tid) == (4350546, "5fc5383ad3")
    assert jobs.is_eh(tid) and not jobs.is_mihon(tid) and jobs.is_mihon("m12") and jobs.is_ext(tid)
    pairs, nxt = ehentai.parse_list(EH_LIST)
    assert pairs == [(4350546, "5fc5383ad3"), (123, "0439fa3666")] and nxt == "4350259"
    pages = ehentai.parse_pages(_eh_gallery(5, 1, 3) + _eh_gallery(5, 2, 2))
    assert sorted(pages) == [1, 2, 3] and pages[2].endswith("/5-2")
    img, nl = ehentai.parse_viewer('<img id="img" src="https://a.hath.network/h/x/_001.webp?a=1&amp;b=2" style="">'
                                   '<a href="#" id="loadfail" onclick="return nl(\'41575-498733\')">Reload</a>')
    assert img == "https://a.hath.network/h/x/_001.webp?a=1&b=2" and nl == "41575-498733"
    assert ehentai.is_quota("https://ehgt.org/g/509.gif") and not ehentai.is_quota("https://x/509/a.webp")
    assert ehentai.filter_tags('language:chinese$ female:"big breasts"$ -male:yaoi') == [
        "language:chinese", "female:big breasts"]
    assert ehentai.gallery_error('<div class="d">\n<p>Key missing, or incorrect key provided.</p>') == \
        "Key missing, or incorrect key provided."


def _fake_eh(monkeypatch, filecount=170):
    """假的 E-Hentai：列表、gdata、每页 20 张的详情页、查看页、图片。/ A fake E-Hentai: lists, gdata, gallery pages
    of 20, viewer pages and images."""
    seen = []

    class R:
        def __init__(self, status=200, text="", content=b"", ctype="text/html", cookies=()):
            self.status_code, self.text, self.content = status, text, content
            self.headers = httpx.Headers([("content-type", ctype)] + [("set-cookie", c) for c in cookies])

        def json(self):
            return json.loads(self.text)

    async def request(method, url, **kw):
        seen.append(url)
        if url == ehentai.API:
            out = [{"gid": g, "token": t, "title": "[Group] Title &amp; more", "title_jpn": "标题", "category": "Manga",
                    "thumb": "https://ehgt.org/t.webp", "uploader": "up", "posted": "1700000000",
                    "filecount": str(filecount), "expunged": False,
                    "tags": ["language:chinese", "artist:someone", "female:x"]} for g, t in kw["json"]["gidlist"]]
            return R(text=json.dumps({"gmetadata": out}))
        if "/popular" in url or ".org/?" in url or url.endswith(".org/"):
            return R(text=EH_LIST.replace("next=4350259", "next=") if "next=" in url else EH_LIST)
        m = re.search(r"/g/(\d+)/[0-9a-f]+/\?p=(\d+)", url)
        if m:
            p = int(m.group(2))
            return R(text=_eh_gallery(int(m.group(1)), p * 20 + 1, min(filecount, p * 20 + 20)))
        if "/s/" in url:
            return R(text=f'<img id="img" src="https://img.hath.network/{url.rsplit("-", 1)[1]}.jpg" style="">')
        if "hath.network" in url or "ehgt.org" in url:
            return R(content=_img(800, 1200), ctype="image/jpeg")
        raise AssertionError(url)

    monkeypatch.setattr(ehentai, "_request", request)
    ehentai.forget()
    return seen


def test_eh_title_chunks_and_images(monkeypatch):
    seen = _fake_eh(monkeypatch, filecount=170)
    config.save({"eh_chunk": 80})
    tid = ehentai.make_id(4350546, "5fc5383ad3")
    t = asyncio.run(ehentai.title(tid))
    assert t["title"] == "标题" and t["author"] == "someone" and t["genre"] == "漫画" and t["done"]
    assert [(e["no"], e["title"]) for e in t["episodes"]] == [(1, "第 1–80 张"), (2, "第 81–160 张"),
                                                               (3, "第 161–170 张")]
    seen.clear()
    urls = asyncio.run(ehentai.images(tid, 2))
    assert len(urls) == 80 and urls[0].endswith("-81") and urls[-1].endswith("-160")
    # 只翻需要的详情页：第 0 页（量每页几张）和 4～7 页。/ Only the needed gallery pages: page 0 (to learn the page
    # size) and pages 4-7.
    assert sorted(int(u.rsplit("=", 1)[1]) for u in seen if "?p=" in u) == [0, 4, 5, 6, 7]
    config.save({"eh_jpn_title": False})
    ehentai.forget()
    assert asyncio.run(ehentai.title(tid))["title"] == "[Group] Title & more"
    config.save({"eh_jpn_title": True})


def test_eh_get_image_retries_and_quota(monkeypatch):
    calls = []

    async def page(url):
        calls.append(url)
        return ('<img id="img" src="https://bad.hath.network/x.jpg" style="">'
                "<a onclick=\"return nl('9-9')\">r</a>") if "nl=" not in url else \
            '<img id="img" src="https://ehgt.org/g/509.gif" style="">'

    async def download(url):
        if "bad" in url:
            raise source.SourceError(502, "取图失败（HTTP 500）")
        return await real(url)

    real = ehentai._download
    monkeypatch.setattr(ehentai, "_page", page)
    monkeypatch.setattr(ehentai, "_download", download)
    try:
        asyncio.run(ehentai.get_image("https://e-hentai.org/s/0000000001/5-1", pace=False))
        raise AssertionError("expected SourceError")
    except source.SourceError as e:
        assert e.status == 509 and ehentai.is_blocking(str(e))
    assert calls == ["https://e-hentai.org/s/0000000001/5-1", "https://e-hentai.org/s/0000000001/5-1?nl=9-9"]


def test_eh_direct_ip(monkeypatch):
    """直连 IP：连的是表里的 IP、Host 头是真域名，坏的 IP 换下一个。/ Direct IPs: the connection goes to a table IP
    with the real domain in Host, and a failing IP moves on to the next."""
    tried = []

    class Fake:
        async def request(self, method, url, headers=None, **kw):
            tried.append((str(url.host), headers.get("Host")))
            if len(tried) == 1:
                raise httpx.ConnectError("reset")
            return httpx.Response(200, text="ok")

    monkeypatch.setattr(ehentai, "client", lambda direct=False: Fake())
    monkeypatch.setattr(ehentai, "_bad_ip", {})
    monkeypatch.setattr(ehentai, "_next_ip", {})
    config.save({"eh_direct": True})
    try:
        r = asyncio.run(ehentai._send("GET", "https://e-hentai.org/popular", {}))
    finally:
        config.save({"eh_direct": False})
    assert r.status_code == 200
    assert tried == [("172.66.132.196", "e-hentai.org"), ("172.66.140.62", "e-hentai.org")]
    assert "172.66.132.196" in ehentai._bad_ip and ehentai._ips("e-hentai.org")[-1] == "172.66.132.196"


def test_eh_sign_in(monkeypatch):
    async def send(method, url, headers, **kw):
        if kw["data"]["PassWord"] == "right":
            return httpx.Response(200, text="ok", headers=[
                ("set-cookie", "ipb_member_id=42; path=/; domain=.e-hentai.org"),
                ("set-cookie", "ipb_pass_hash=" + "a" * 32 + "; path=/"), ("set-cookie", "ipb_session_id=x")])
        if kw["data"]["PassWord"] == "captcha":
            return httpx.Response(200, text="The captcha was not entered correctly")
        return httpx.Response(200, text="Username or password incorrect")

    async def request(method, url, **kw):
        ehentai._keep_igneous(httpx.Response(200, text="x", headers=[("set-cookie", "igneous=abc123; path=/")]))
        return httpx.Response(200, text="x")

    monkeypatch.setattr(ehentai, "_send", send)
    monkeypatch.setattr(ehentai, "_request", request)
    try:
        for pw, want in (("wrong", "用户名或密码不对"), ("captcha", "验证码")):
            try:
                asyncio.run(ehentai.sign_in("u", pw))
                raise AssertionError("expected SourceError")
            except source.SourceError as e:
                assert want in str(e)
        r = asyncio.run(ehentai.sign_in("u", "right"))
        cfg = config.load()
        assert r == {"member_id": "42", "exhentai": True}
        assert cfg["eh_member_id"] == "42" and cfg["eh_pass_hash"] == "a" * 32 and cfg["eh_igneous"] == "abc123"
        config.save({"eh_site": "ex"})
        assert ehentai.ex() and ehentai.base() == "https://exhentai.org" and "igneous=abc123" in ehentai._cookies(
            "exhentai.org") and ehentai._cookies("img.hath.network") == ""
    finally:
        config.save({"eh_member_id": "", "eh_pass_hash": "", "eh_igneous": "", "eh_site": "e"})


def test_eh_sign_in_through_flaresolverr(monkeypatch):
    """论坛整站要过 Cloudflare 验证：改由 FlareSolverr 提交表单；它也不在时说清楚怎么办，不说密码不对。/ The forum
    sits behind a Cloudflare check: FlareSolverr submits the form; without it the error says what to do instead of
    blaming the password."""
    async def send(method, url, headers, **kw):
        return httpx.Response(403, text="<title>Just a moment...</title>", headers=[("cf-mitigated", "challenge")])

    posted = []

    async def solver(form):
        posted.append(form["PassWord"])
        if form["PassWord"] == "right":
            return {"ipb_member_id": "7", "ipb_pass_hash": "b" * 32, "cf_clearance": "x"}, 200, "<p>Thanks</p>"
        return {}, 200, "<div class='errorwrap'>Username or password incorrect</div>"

    async def request(method, url, **kw):
        return httpx.Response(200, text="x")

    monkeypatch.setattr(ehentai, "_send", send)
    monkeypatch.setattr(ehentai, "_request", request)
    monkeypatch.setattr(ehentai, "FLARESOLVERR", "http://127.0.0.1:9")      # 没开 / Not running
    try:
        try:
            asyncio.run(ehentai.sign_in("u", "right"))
            raise AssertionError("expected SourceError")
        except source.SourceError as e:
            assert "flaresolverr" in str(e) and "手动填 cookie" in str(e)
        monkeypatch.setattr(ehentai, "_solver_login", solver)
        try:
            asyncio.run(ehentai.sign_in("u", "wrong"))
            raise AssertionError("expected SourceError")
        except source.SourceError as e:
            assert "用户名或密码不对" in str(e)
        assert asyncio.run(ehentai.sign_in("u", "right"))["member_id"] == "7" and posted == ["wrong", "right"]
        assert config.load()["eh_pass_hash"] == "b" * 32
        err = ehentai._login_error(200, "<html><body>Something else happened</body></html>")
        assert "HTTP 200" in str(err) and "Something else" in str(err)
    finally:
        config.save({"eh_member_id": "", "eh_pass_hash": "", "eh_igneous": ""})


def test_device_api_ehentai(monkeypatch):
    _fake_eh(monkeypatch, filecount=30)
    monkeypatch.setattr(mihon, "_alive", (time.time(), False))
    with TestClient(main.app) as c:
        h = {"x-webtoon-token": config.load()["device_token"]}
        assert c.get("/api/device/v1/store?src=ehentai", headers=h).status_code == 404     # 没打开 / Off
        c.post("/api/panel/setup", json={"password": "password123"})
        c.post("/api/panel/login", json={"password": "password123"})
        try:
            config.save({"delay_min": 0, "delay_max": 0})
            r = c.post("/api/panel/eh", json={"eh_on": True, "eh_chunk": 20, "eh_cats": 4}).json()
            assert r["eh_on"] and not r["signed_in"] and "eh_pass_hash" not in r
            assert c.post("/api/panel/eh", json={"eh_site": "x"}).status_code == 400
            r = c.get("/api/device/v1/sources", headers=h).json()
            assert [s["key"] for s in r["items"]] == ["webtoon", "ehentai"] and r["items"][1]["latest"]
            # 人气榜在服务器上按分类筛：排除了漫画，就一部都不剩。/ The popular list is filtered on the server:
            # with Manga excluded nothing is left.
            assert c.get("/api/device/v1/store?src=ehentai&seg=pop", headers=h).json()["items"] == []
            c.post("/api/panel/eh", json={"eh_cats": 0})
            r = c.get("/api/device/v1/store?src=ehentai&seg=latest", headers=h).json()
            tid = r["items"][0]["id"]
            assert len(r["items"]) == 2 and r["items"][0]["src"] == "E-Hentai" and jobs.is_eh(tid)
            assert c.get(f"/api/device/v1/search?src=ehentai&q=abc", headers=h).json()["items"][0]["id"] == tid
            t = c.get(f"/api/device/v1/title/{tid}?lang=mihon", headers=h).json()
            assert [e["no"] for e in t["episodes"]] == [1, 2] and t["latest"] == 2
            r = c.post("/api/device/v1/check", headers=h, json={"titles": [{"id": tid, "last": 1}]}).json()
            assert [e["no"] for e in r["titles"][0]["new"]] == [2]
            assert c.get(f"/api/device/v1/cover/{tid}.jpg", headers=h).status_code == 200
            for _ in range(100):
                st = c.get(f"/api/device/v1/episode/{tid}/2?lang=mihon", headers=h).json()
                if st["state"] != "pending":
                    break
                asyncio.run(asyncio.sleep(0.05))
            assert st["state"] == "ready", st
            z = zipfile.ZipFile(io.BytesIO(c.get(f"/api/device/v1/episode/{tid}/2.cbz", headers=h).content))
            assert len([n for n in z.namelist() if n.endswith(".jpg")]) == 10
            assert jobs.cbz_path(ehentai.LANG, tid, 2).exists()
            assert c.get("/api/device/v1/episode/mezzzzzzzzzzzzz/1", headers=h).status_code in (400, 200)
        finally:
            config.save({"eh_on": False, "eh_chunk": 80, "eh_cats": 0, "delay_min": 0.4, "delay_max": 1.2})


def test_eh_quota_pauses_source(monkeypatch):
    calls = []
    a, b = ehentai.make_id(1, "0000000001"), ehentai.make_id(2, "0000000002")

    async def info(lang, tid):
        return {"title": tid, "author": "", "episodes": [], "src": "E-Hentai" if jobs.is_eh(tid) else "B"}

    async def convert_(key, meta):
        calls.append(key[1])
        if key[1] == a:
            raise source.SourceError(509, ehentai.QUOTA_HINT)

    monkeypatch.setattr(jobs, "_info", info)
    monkeypatch.setattr(jobs, "_convert", convert_)
    monkeypatch.setattr(jobs, "_cf_block", {})

    async def run():
        task = asyncio.create_task(jobs.worker())
        await asyncio.sleep(0)
        for tid in (a, b, "m3"):
            jobs.status("", tid, 1)
        while not jobs._queue.empty() or len(calls) + 1 < 3:
            await asyncio.sleep(0.01)
        await asyncio.sleep(0.01)
        task.cancel()

    asyncio.run(run())
    assert calls == [a, "m3"]
    st = jobs.status("", b, 1, start=False)
    assert st["state"] == "error" and "额度" in st["error"] and "分钟后再试" in st["error"]
