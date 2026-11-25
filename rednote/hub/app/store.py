"""SQLite：关注的博主、拉到的笔记（来源：关注 / 推荐）、每日请求计数。

SQLite: followed creators, fetched notes (source: follow / rec) and the daily request counters.
"""
from __future__ import annotations

import json
import sqlite3
import threading
import time
from typing import Any

from . import config

_lock = threading.Lock()
_db: sqlite3.Connection | None = None


def db() -> sqlite3.Connection:
    global _db
    if _db is None:
        _db = sqlite3.connect(config.DB_PATH, check_same_thread=False)
        _db.row_factory = sqlite3.Row
        _db.executescript(
            """
            CREATE TABLE IF NOT EXISTS creators (
                user_id TEXT PRIMARY KEY, nickname TEXT, xsec_token TEXT, added_at REAL,
                last_fetch REAL, last_error TEXT);
            CREATE TABLE IF NOT EXISTS notes (
                id TEXT PRIMARY KEY, user_id TEXT, author TEXT, title TEXT, body TEXT, time INTEGER,
                likes TEXT, comment_count TEXT, comments TEXT, images INTEGER, fetched_at REAL);
            CREATE INDEX IF NOT EXISTS notes_fetched ON notes(fetched_at);
            CREATE TABLE IF NOT EXISTS counters (day TEXT PRIMARY KEY, requests INTEGER);
            """
        )
        # 旧库补上来源列；旧笔记都是关注拉的。/ Add the source column to old databases; old notes came from follows.
        cols = [r[1] for r in _db.execute("PRAGMA table_info(notes)")]
        if "source" not in cols:
            _db.execute("ALTER TABLE notes ADD COLUMN source TEXT NOT NULL DEFAULT 'follow'")
            _db.commit()
        # 封面高宽比（瀑布流用）；旧笔记当 4:3 竖图。/ Cover height/width for waterfalls; old notes count as 4:3.
        if "ratio" not in cols:
            _db.execute("ALTER TABLE notes ADD COLUMN ratio REAL NOT NULL DEFAULT 1.333")
            _db.commit()
        # 这篇的 xsec_token（加载更多评论要用）和是否已经加载过更多评论；旧笔记没有令牌。
        # The note's xsec_token (needed to load more comments) and whether more comments were loaded; old notes have
        # no token.
        if "xsec_token" not in cols:
            _db.execute("ALTER TABLE notes ADD COLUMN xsec_token TEXT NOT NULL DEFAULT ''")
            _db.execute("ALTER TABLE notes ADD COLUMN more_comments INTEGER NOT NULL DEFAULT 0")
            _db.commit()
    return _db


def _rows(sql: str, args: tuple = ()) -> list[dict[str, Any]]:
    with _lock:
        return [dict(r) for r in db().execute(sql, args).fetchall()]


def _exec(sql: str, args: tuple = ()) -> None:
    with _lock:
        db().execute(sql, args)
        db().commit()


# ---- 博主 / Creators ----

def creators() -> list[dict[str, Any]]:
    return _rows("SELECT * FROM creators ORDER BY added_at")


def add_creator(user_id: str, xsec_token: str, nickname: str = "") -> None:
    _exec(
        "INSERT INTO creators(user_id, nickname, xsec_token, added_at) VALUES(?,?,?,?) "
        "ON CONFLICT(user_id) DO UPDATE SET xsec_token=excluded.xsec_token, "
        "nickname=CASE WHEN excluded.nickname<>'' THEN excluded.nickname ELSE creators.nickname END",
        (user_id, nickname, xsec_token, time.time()),
    )


def remove_creator(user_id: str) -> None:
    _exec("DELETE FROM creators WHERE user_id=?", (user_id,))


def replace_creators(rows: list[dict[str, Any]]) -> None:
    """WebDAV 恢复：换成备份里的名单（已有的人保留抓取进度）。/ WebDAV restore: replace the list (existing creators
    keep their progress)."""
    keep = {r["user_id"] for r in rows}
    for c in creators():
        if c["user_id"] not in keep:
            remove_creator(c["user_id"])
    for r in rows:
        add_creator(r["user_id"], r["xsec_token"], r["nickname"])


def mark_creator(user_id: str, nickname: str | None, error: str, token: str | None = None) -> None:
    _exec(
        "UPDATE creators SET last_fetch=?, last_error=?, nickname=COALESCE(?, nickname), "
        "xsec_token=COALESCE(?, xsec_token) WHERE user_id=?",
        (time.time(), error, nickname or None, token or None, user_id),
    )


# ---- 笔记 / Notes ----

def has_note(note_id: str) -> bool:
    return bool(_rows("SELECT 1 FROM notes WHERE id=?", (note_id,)))


def note_source(note_id: str) -> str | None:
    rows = _rows("SELECT source FROM notes WHERE id=?", (note_id,))
    return rows[0]["source"] if rows else None


def set_source(note_id: str, source: str) -> None:
    """推荐里看过的笔记后来出现在关注博主主页时，改记为关注，不再重拉。
    A note first opened from recommendations that later shows up on a followed profile is re-labelled, not refetched.
    """
    _exec("UPDATE notes SET source=?, fetched_at=? WHERE id=?", (source, time.time(), note_id))


def save_note(note: dict[str, Any], source: str = "follow") -> None:
    _exec(
        "INSERT OR REPLACE INTO notes(id, user_id, author, title, body, time, likes, comment_count, comments, "
        "images, fetched_at, source, ratio, xsec_token) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        (note["id"], note["user_id"], note["author"], note["title"], note["body"], note["time"], note["likes"],
         note["comment_count"], json.dumps(note["comments"], ensure_ascii=False), note["images"], time.time(),
         source, note.get("ratio", 1.333), note.get("xsec_token", "")),
    )


def fill_token(note_id: str, token: str) -> None:
    _exec("UPDATE notes SET xsec_token=? WHERE id=? AND xsec_token=''", (token, note_id))


def save_comments(note_id: str, comment_count: str, comments: list[dict[str, Any]]) -> None:
    """加载更多评论后换掉这篇的评论，并记下已经加载过。/ Replace a note's comments after loading more, and mark it."""
    _exec("UPDATE notes SET comment_count=?, comments=?, more_comments=1 WHERE id=?",
          (comment_count, json.dumps(comments, ensure_ascii=False), note_id))


def cursor(n: dict[str, Any]) -> str:
    """一条在列表里的位置，设备拿它要更早的。/ A row's place in the list; the device sends it back for older rows."""
    return f"{n['fetched_at']!r}|{n['id']}"


def notes(since: float = 0, limit: int = 50, source: str | None = None, before: str = "") -> list[dict[str, Any]]:
    """按抓取时间从新到旧；before 是上一批最后一条的游标，只要比它更早的。/ Newest fetched first; before is the
    last row's cursor from the previous batch, asking only for older rows."""
    where, args = "fetched_at>?", [since]
    if source:
        where += " AND source=?"
        args.append(source)
    if before:
        key, _, nid = before.partition("|")
        where += " AND (fetched_at<? OR (fetched_at=? AND id<?))"
        args += [float(key), float(key), nid]
    rows = _rows(f"SELECT * FROM notes WHERE {where} ORDER BY fetched_at DESC, id DESC LIMIT ?", (*args, limit))
    for r in rows:
        r["comments"] = json.loads(r["comments"] or "[]")
    return rows


def note(note_id: str) -> dict[str, Any] | None:
    rows = _rows("SELECT * FROM notes WHERE id=?", (note_id,))
    if not rows:
        return None
    rows[0]["comments"] = json.loads(rows[0]["comments"] or "[]")
    return rows[0]


def old_notes(before: float) -> list[str]:
    return [r["id"] for r in _rows("SELECT id FROM notes WHERE fetched_at<?", (before,))]


def oldest_notes(limit: int) -> list[str]:
    return [r["id"] for r in _rows("SELECT id FROM notes ORDER BY fetched_at LIMIT ?", (limit,))]


def delete_note(note_id: str) -> None:
    _exec("DELETE FROM notes WHERE id=?", (note_id,))


def note_count() -> int:
    return _rows("SELECT COUNT(*) AS n FROM notes")[0]["n"]


# ---- 每日请求计数 / Daily request counters ----
# 总数记在“日期”行，推荐另记在“rec:日期”行。/ The total uses the date row; recommendations also use "rec:date".

def _today(kind: str = "") -> str:
    day = time.strftime("%Y-%m-%d")
    return f"{kind}:{day}" if kind else day


def requests_today(kind: str = "") -> int:
    rows = _rows("SELECT requests FROM counters WHERE day=?", (_today(kind),))
    return rows[0]["requests"] if rows else 0


def count_request(kind: str = "") -> None:
    keys = [_today()] + ([_today(kind)] if kind else [])
    for key in keys:
        _exec(
            "INSERT INTO counters(day, requests) VALUES(?,1) ON CONFLICT(day) DO UPDATE SET requests=requests+1",
            (key,),
        )
