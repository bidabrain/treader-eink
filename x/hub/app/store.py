"""SQLite：处理好的推文（属于为你推荐 / 正在关注）和回复缓存。

SQLite: processed posts (in For You and/or Following) and cached replies.
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
            CREATE TABLE IF NOT EXISTS posts (
                id TEXT PRIMARY KEY, user_name TEXT, screen_name TEXT, text TEXT, created_ts INTEGER,
                likes INTEGER, replies INTEGER, retweets INTEGER, views INTEGER, rt_by TEXT DEFAULT '',
                reply_to TEXT DEFAULT '', qt_user TEXT DEFAULT '', qt_screen TEXT DEFAULT '', qt_text TEXT DEFAULT '',
                video INTEGER DEFAULT 0, images INTEGER DEFAULT 0, pics INTEGER DEFAULT 0, starts TEXT DEFAULT '',
                ratio REAL DEFAULT 1.333,
                in_foryou INTEGER DEFAULT 0, foryou_at REAL DEFAULT 0, foryou_rank INTEGER DEFAULT 0,
                in_following INTEGER DEFAULT 0, fetched_at REAL);
            CREATE INDEX IF NOT EXISTS posts_created ON posts(created_ts);
            CREATE INDEX IF NOT EXISTS posts_foryou ON posts(foryou_at);
            CREATE TABLE IF NOT EXISTS replies (post_id TEXT PRIMARY KEY, data TEXT, fetched_at REAL);
            """
        )
    return _db


def _rows(sql: str, args: tuple = ()) -> list[dict[str, Any]]:
    with _lock:
        return [dict(r) for r in db().execute(sql, args).fetchall()]


def _exec(sql: str, args: tuple = ()) -> None:
    with _lock:
        db().execute(sql, args)
        db().commit()


def has_post(pid: str) -> bool:
    return bool(_rows("SELECT 1 FROM posts WHERE id=?", (pid,)))


def save_post(p: dict[str, Any]) -> None:
    _exec(
        "INSERT OR REPLACE INTO posts(id, user_name, screen_name, text, created_ts, likes, replies, retweets, views, "
        "rt_by, reply_to, qt_user, qt_screen, qt_text, video, images, pics, starts, ratio, in_foryou, foryou_at, "
        "foryou_rank, in_following, fetched_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        (p["id"], p["user_name"], p["screen_name"], p["text"], p["created_ts"], p["likes"], p["replies"],
         p["retweets"], p["views"], p.get("rt_by", ""), p.get("reply_to", ""), p.get("qt_user", ""),
         p.get("qt_screen", ""), p.get("qt_text", ""), int(bool(p.get("video"))), p.get("images", 0),
         p.get("pics", 0), p.get("starts", ""), p.get("ratio", 1.333), int(p.get("in_foryou", 0)),
         p.get("foryou_at", 0), p.get("foryou_rank", 0), int(p.get("in_following", 0)), time.time()),
    )


def refresh(pid: str, p: dict[str, Any], feed: str, at: float, rank: int) -> None:
    """已有的推文：更新数字，标进这条流。/ An existing post: update counts and mark it in this feed."""
    _exec("UPDATE posts SET likes=?, replies=?, retweets=?, views=? WHERE id=?",
          (p["likes"], p["replies"], p["retweets"], p["views"], pid))
    if feed == "foryou":
        _exec("UPDATE posts SET in_foryou=1, foryou_at=?, foryou_rank=? WHERE id=?", (at, rank, pid))
    else:
        _exec("UPDATE posts SET in_following=1 WHERE id=?", (pid,))


def feed(seg: str, since: float = 0, limit: int = 100) -> list[dict[str, Any]]:
    if seg == "foryou":
        return _rows("SELECT * FROM posts WHERE in_foryou=1 AND foryou_at>? ORDER BY foryou_at DESC, foryou_rank ASC "
                     "LIMIT ?", (since, limit))
    return _rows("SELECT * FROM posts WHERE in_following=1 AND fetched_at>? ORDER BY created_ts DESC LIMIT ?",
                 (since, limit))


def post(pid: str) -> dict[str, Any] | None:
    rows = _rows("SELECT * FROM posts WHERE id=?", (pid,))
    return rows[0] if rows else None


def latest(limit: int = 60) -> list[dict[str, Any]]:
    return _rows("SELECT * FROM posts ORDER BY fetched_at DESC LIMIT ?", (limit,))


def count(seg: str | None = None) -> int:
    where = {"foryou": " WHERE in_foryou=1", "following": " WHERE in_following=1"}.get(seg or "", "")
    return _rows("SELECT COUNT(*) AS n FROM posts" + where)[0]["n"]


def drop_foryou_older(keep: int) -> list[str]:
    """为你推荐只留最近 keep 条；挤出去且不在正在关注里的返回去删。/ Keep the latest `keep` For You posts; return
    ids that fell out and are not in Following."""
    rows = _rows("SELECT id, in_following FROM posts WHERE in_foryou=1 ORDER BY foryou_at DESC, foryou_rank ASC")
    gone = []
    for r in rows[keep:]:
        if r["in_following"]:
            _exec("UPDATE posts SET in_foryou=0 WHERE id=?", (r["id"],))
        else:
            gone.append(r["id"])
    return gone


def old_posts(before: float) -> list[str]:
    return [r["id"] for r in _rows("SELECT id FROM posts WHERE fetched_at<?", (before,))]


def oldest_posts(limit: int) -> list[str]:
    return [r["id"] for r in _rows("SELECT id FROM posts ORDER BY fetched_at LIMIT ?", (limit,))]


def delete_post(pid: str) -> None:
    _exec("DELETE FROM posts WHERE id=?", (pid,))
    _exec("DELETE FROM replies WHERE post_id=?", (pid,))


def replies(pid: str) -> list[dict[str, Any]] | None:
    rows = _rows("SELECT data FROM replies WHERE post_id=?", (pid,))
    return json.loads(rows[0]["data"]) if rows else None


def save_replies(pid: str, items: list[dict[str, Any]]) -> None:
    _exec("INSERT OR REPLACE INTO replies(post_id, data, fetched_at) VALUES(?,?,?)",
          (pid, json.dumps(items, ensure_ascii=False), time.time()))
