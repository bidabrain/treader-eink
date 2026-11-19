"""把 X 网页 GraphQL 返回的 JSON 整理成一条条推文：去广告和推荐关注，转推取原推，长推文取全文，t.co 换回原链接，
引用推文附在后面，图片和视频封面取 pbs.twimg.com 地址。

Turn X web GraphQL JSON into posts: drop ads and follow suggestions, unwrap retweets, take full long-post text,
expand t.co links, attach quoted posts, and collect pbs.twimg.com image and video-cover URLs.
"""
from __future__ import annotations

import html
from email.utils import parsedate_to_datetime
from typing import Any, Iterator


def _unwrap(r: dict | None) -> dict | None:
    if not r:
        return None
    if r.get("__typename") == "TweetWithVisibilityResults":
        r = r.get("tweet") or {}
    if not r.get("legacy") or not r.get("rest_id"):
        return None
    return r


def _user(r: dict) -> tuple[str, str]:
    u = (((r.get("core") or {}).get("user_results") or {}).get("result")) or {}
    core, leg = u.get("core") or {}, u.get("legacy") or {}
    return core.get("name") or leg.get("name") or "", core.get("screen_name") or leg.get("screen_name") or ""


def _text(r: dict, quoted_id: str = "") -> str:
    leg = r["legacy"]
    note = (((r.get("note_tweet") or {}).get("note_tweet_results") or {}).get("result")) or {}
    if note.get("text"):
        text = note["text"]
        urls = (note.get("entity_set") or {}).get("urls") or []
    else:
        text = leg.get("full_text") or ""
        rng = leg.get("display_text_range")
        if isinstance(rng, list) and len(rng) == 2:
            text = text[rng[0]:rng[1]]
        urls = (leg.get("entities") or {}).get("urls") or []
    for u in urls:
        short, full = u.get("url") or "", u.get("expanded_url") or u.get("display_url") or ""
        if not short:
            continue
        # 指向被引用推文的链接不用留（引用的内容附在后面）。/ Drop the link to the quoted post; it is attached.
        if quoted_id and f"/status/{quoted_id}" in full:
            full = ""
        text = text.replace(short, full)
    for m in (leg.get("entities") or {}).get("media") or []:
        if m.get("url"):
            text = text.replace(m["url"], "")
    return html.unescape(text).strip()


def _media(r: dict) -> tuple[list[str], bool]:
    urls, video = [], False
    for m in ((r["legacy"].get("extended_entities") or {}).get("media")) or []:
        base = m.get("media_url_https") or ""
        if not base:
            continue
        if m.get("type") in ("video", "animated_gif"):
            video = True
        info = m.get("original_info") or {}
        w, h = int(info.get("width") or 0), int(info.get("height") or 0)
        # 特别长的图取大图，切屏后还清楚。/ Very tall images use the large size so split screens stay sharp.
        urls.append(f"{base}?name={'large' if w and h and h / w > 2 else 'medium'}")
    return urls, video


def _ts(leg: dict) -> int:
    try:
        return int(parsedate_to_datetime(leg.get("created_at") or "").timestamp())
    except (TypeError, ValueError):
        return 0


def parse_tweet(raw: dict | None, rt_by: str = "") -> dict[str, Any] | None:
    r = _unwrap(raw)
    if not r:
        return None
    leg = r["legacy"]
    name, screen = _user(r)
    rt = (leg.get("retweeted_status_result") or {}).get("result")
    if rt:
        # 转推：存原推，记一下是谁转的。/ A retweet: keep the original and note who reposted it.
        return parse_tweet(rt, rt_by=name or screen)
    q = _unwrap((r.get("quoted_status_result") or {}).get("result"))
    media, video = _media(r)
    out: dict[str, Any] = {
        "id": r["rest_id"], "user_name": name, "screen_name": screen,
        "text": _text(r, q["rest_id"] if q else ""), "created_ts": _ts(leg),
        "likes": int(leg.get("favorite_count") or 0), "replies": int(leg.get("reply_count") or 0),
        "retweets": int(leg.get("retweet_count") or 0) + int(leg.get("quote_count") or 0),
        "views": int(((r.get("views") or {}).get("count")) or 0), "rt_by": rt_by,
        "reply_to": leg.get("in_reply_to_screen_name") or "", "media": media, "video": video,
        "qt_user": "", "qt_screen": "", "qt_text": "", "qt_media": [],
    }
    if q:
        qn, qs = _user(q)
        qm, qv = _media(q)
        out.update(qt_user=qn, qt_screen=qs, qt_text=_text(q), qt_media=qm)
        out["video"] = out["video"] or qv
    return out


def _instructions(node: Any) -> Iterator[dict]:
    """在返回里找所有 instructions 列表（不同接口路径不一样）。/ Find every instructions list in a response."""
    if isinstance(node, dict):
        for k, v in node.items():
            if k == "instructions" and isinstance(v, list):
                yield from (i for i in v if isinstance(i, dict))
            else:
                yield from _instructions(v)
    elif isinstance(node, list):
        for v in node:
            yield from _instructions(v)


def _entries(body: dict) -> Iterator[dict]:
    for ins in _instructions(body):
        for e in ins.get("entries") or ([ins["entry"]] if isinstance(ins.get("entry"), dict) else []):
            yield e


def _items(entry: dict) -> list[dict]:
    c = entry.get("content") or {}
    if c.get("itemContent"):
        return [c["itemContent"]]
    return [((i.get("item") or {}).get("itemContent")) or {} for i in c.get("items") or []]


SKIP = ("promoted-", "who-to-follow", "cursor-", "messageprompt", "module-", "superhero-", "trends-",
        "tweetdetailrelatedtweets")


def timeline(bodies: list[dict]) -> tuple[list[dict[str, Any]], int]:
    """时间线里的推文（按出现顺序、去重），以及去掉的广告条数。/ Timeline posts in order, deduplicated, plus the
    number of ads dropped."""
    out, seen, ads = [], set(), 0
    for body in bodies:
        for e in _entries(body):
            eid = e.get("entryId") or ""
            if eid.startswith("promoted-"):
                ads += 1
                continue
            if eid.startswith(SKIP):
                continue
            for ic in _items(e):
                if ic.get("itemType") != "TimelineTweet":
                    continue
                if ic.get("promotedMetadata"):
                    ads += 1
                    continue
                t = parse_tweet((ic.get("tweet_results") or {}).get("result"))
                if t and t["id"] not in seen:
                    seen.add(t["id"])
                    out.append(t)
    return out, ads


def replies(bodies: list[dict], focal: str) -> tuple[dict[str, Any] | None, list[dict[str, Any]]]:
    """推文详情：这条本身和每串回复的第一条。/ Post detail: the post itself and the first post of each reply
    thread."""
    main, out, seen = None, [], set()
    for body in bodies:
        for e in _entries(body):
            eid = e.get("entryId") or ""
            if eid.startswith(("promoted-", "cursor-", "tweetdetailrelatedtweets")):
                continue
            items = _items(e)
            if eid.startswith("tweet-"):
                t = parse_tweet((items[0].get("tweet_results") or {}).get("result")) if items else None
                if t and t["id"] == focal:
                    main = t
                continue
            if eid.startswith("conversationthread-"):
                for ic in items:
                    if ic.get("promotedMetadata"):
                        continue
                    t = parse_tweet((ic.get("tweet_results") or {}).get("result"))
                    if t and t["id"] != focal and t["id"] not in seen:
                        seen.add(t["id"])
                        out.append(t)
                    break
    return main, out
