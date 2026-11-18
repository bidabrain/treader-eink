"""SQLite：关注的博主、拉到的笔记、每日请求计数。

SQLite: followed creators, fetched notes and the daily request counter.
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


def mark_creator(user_id: str, nickname: str | None, error: str, token: str | None = None) -> None:
    _exec(
        "UPDATE creators SET last_fetch=?, last_error=?, nickname=COALESCE(?, nickname), "
        "xsec_token=COALESCE(?, xsec_token) WHERE user_id=?",
        (time.time(), error, nickname or None, token or None, user_id),
    )


# ---- 笔记 / Notes ----

def has_note(note_id: str) -> bool:
    return bool(_rows("SELECT 1 FROM notes WHERE id=?", (note_id,)))


def save_note(note: dict[str, Any]) -> None:
    _exec(
        "INSERT OR REPLACE INTO notes(id, user_id, author, title, body, time, likes, comment_count, comments, "
        "images, fetched_at) VALUES(?,?,?,?,?,?,?,?,?,?,?)",
        (note["id"], note["user_id"], note["author"], note["title"], note["body"], note["time"], note["likes"],
         note["comment_count"], json.dumps(note["comments"], ensure_ascii=False), note["images"], time.time()),
    )


def notes(since: float = 0, limit: int = 50) -> list[dict[str, Any]]:
    rows = _rows("SELECT * FROM notes WHERE fetched_at>? ORDER BY fetched_at DESC LIMIT ?", (since, limit))
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


# ---- 每日请求计数 / Daily request counter ----

def _today() -> str:
    return time.strftime("%Y-%m-%d")


def requests_today() -> int:
    rows = _rows("SELECT requests FROM counters WHERE day=?", (_today(),))
    return rows[0]["requests"] if rows else 0


def count_request() -> None:
    _exec(
        "INSERT INTO counters(day, requests) VALUES(?,1) ON CONFLICT(day) DO UPDATE SET requests=requests+1",
        (_today(),),
    )
