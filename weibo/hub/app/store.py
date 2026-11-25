"""SQLite：关注的人、处理好的微博（属于关注 / 热门）、评论、每日计数。

SQLite: followed users, processed posts (in following / hot), comments and daily counters.
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

USER_FIELDS = ("id", "screen_name", "description", "avatar_url", "cover_url", "followers_count", "follow_count",
               "statuses_count", "verified", "verified_reason", "special")


def db() -> sqlite3.Connection:
    global _db
    if _db is None:
        _db = sqlite3.connect(config.DB_PATH, check_same_thread=False)
        _db.row_factory = sqlite3.Row
        _db.executescript(
            """
            CREATE TABLE IF NOT EXISTS users (
                id TEXT PRIMARY KEY, screen_name TEXT, description TEXT, avatar_url TEXT, cover_url TEXT,
                followers_count TEXT, follow_count INTEGER, statuses_count INTEGER, verified INTEGER,
                verified_reason TEXT, special INTEGER DEFAULT 0, added_at REAL, last_fetched_at REAL DEFAULT 0,
                last_error TEXT DEFAULT '');
            CREATE TABLE IF NOT EXISTS posts (
                id TEXT PRIMARY KEY, user_id TEXT, user_name TEXT, text TEXT, created_ts INTEGER,
                likes INTEGER, comments INTEGER, reposts INTEGER, source TEXT,
                rt_user TEXT DEFAULT '', rt_text TEXT DEFAULT '', images INTEGER, ratio REAL DEFAULT 1.333,
                in_follow INTEGER DEFAULT 0, in_hot INTEGER DEFAULT 0, hot_at REAL DEFAULT 0, hot_rank INTEGER DEFAULT 0,
                fetched_at REAL);
            CREATE INDEX IF NOT EXISTS posts_created ON posts(created_ts);
            CREATE INDEX IF NOT EXISTS posts_hot ON posts(hot_at);
            CREATE TABLE IF NOT EXISTS comments (post_id TEXT PRIMARY KEY, data TEXT, fetched_at REAL);
            CREATE TABLE IF NOT EXISTS counters (day TEXT PRIMARY KEY, requests INTEGER);
            """
        )
        # 0.1.3 起每张原图有方形小图：原图张数和每张的第一页。/ From 0.1.3 each source image has a square small image:
        # the source image count and each one's first page.
        cols = [r[1] for r in _db.execute("PRAGMA table_info(posts)")]
        if "pics" not in cols:
            _db.execute("ALTER TABLE posts ADD COLUMN pics INTEGER NOT NULL DEFAULT 0")
            _db.execute("ALTER TABLE posts ADD COLUMN starts TEXT NOT NULL DEFAULT ''")
            _db.commit()
    return _db


def _rows(sql: str, args: tuple = ()) -> list[dict[str, Any]]:
    with _lock:
        return [dict(r) for r in db().execute(sql, args).fetchall()]


def _exec(sql: str, args: tuple = ()) -> None:
    with _lock:
        db().execute(sql, args)
        db().commit()


# ---- 关注的人 / Followed users ----

def users() -> list[dict[str, Any]]:
    rows = _rows("SELECT * FROM users ORDER BY added_at")
    for r in rows:
        r["verified"] = bool(r["verified"])
        r["special"] = bool(r["special"])
    return rows


def user_ids() -> set[str]:
    return {r["id"] for r in _rows("SELECT id FROM users")}


def add_user(u: dict[str, Any]) -> None:
    """加人或更新资料；special 和抓取进度保留。/ Add a user or refresh the profile, keeping special and progress."""
    _exec(
        "INSERT INTO users(id, screen_name, description, avatar_url, cover_url, followers_count, follow_count, "
        "statuses_count, verified, verified_reason, special, added_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(id) DO UPDATE SET screen_name=excluded.screen_name, description=excluded.description, "
        "avatar_url=excluded.avatar_url, cover_url=excluded.cover_url, followers_count=excluded.followers_count, "
        "follow_count=excluded.follow_count, statuses_count=excluded.statuses_count, verified=excluded.verified, "
        "verified_reason=excluded.verified_reason",
        (str(u.get("id")), u.get("screen_name", ""), u.get("description", ""), u.get("avatar_url", ""),
         u.get("cover_url", ""), str(u.get("followers_count", "")), int(u.get("follow_count") or 0),
         int(u.get("statuses_count") or 0), int(bool(u.get("verified"))), u.get("verified_reason", ""),
         int(bool(u.get("special"))), time.time()),
    )


def remove_user(uid: str) -> None:
    _exec("DELETE FROM users WHERE id=?", (uid,))


def set_special(uid: str, special: bool) -> None:
    _exec("UPDATE users SET special=? WHERE id=?", (int(special), uid))


def replace_users(rows: list[dict[str, Any]]) -> None:
    """WebDAV 恢复：换成备份里的名单（已有的人保留抓取进度）。/ WebDAV restore: replace the list (existing users keep
    their progress)."""
    keep = {str(u.get("id")) for u in rows if u.get("id")}
    for uid in user_ids() - keep:
        remove_user(uid)
    for u in rows:
        if u.get("id"):
            add_user(u)
            set_special(str(u["id"]), bool(u.get("special")))


def mark_fetched(uid: str, error: str = "", name: str = "", avatar: str = "") -> None:
    _exec("UPDATE users SET last_fetched_at=?, last_error=?, "
          "screen_name=CASE WHEN ?<>'' THEN ? ELSE screen_name END, "
          "avatar_url=CASE WHEN ?<>'' THEN ? ELSE avatar_url END WHERE id=?",
          (time.time(), error, name, name, avatar, avatar, uid))


def post_stats_by_user() -> dict[str, dict[str, int]]:
    """每人已存微博的条数和最新、最旧发帖时间（毫秒），用来估算发帖频率。/ Per-user count and newest/oldest times."""
    rows = _rows("SELECT user_id, COUNT(*) AS n, MAX(created_ts) AS newest, MIN(created_ts) AS oldest "
                 "FROM posts WHERE in_follow=1 GROUP BY user_id")
    return {r["user_id"]: {"count": r["n"], "newest": r["newest"] or 0, "oldest": r["oldest"] or 0} for r in rows}


# ---- 微博 / Posts ----

def has_post(pid: str) -> bool:
    return bool(_rows("SELECT 1 FROM posts WHERE id=?", (pid,)))


def save_post(p: dict[str, Any]) -> None:
    _exec(
        "INSERT OR REPLACE INTO posts(id, user_id, user_name, text, created_ts, likes, comments, reposts, source, "
        "rt_user, rt_text, images, ratio, in_follow, in_hot, hot_at, hot_rank, fetched_at, pics, starts) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        (p["id"], p["user_id"], p["user_name"], p["text"], p["created_ts"], p["likes"], p["comments"], p["reposts"],
         p.get("source", ""), p.get("rt_user", ""), p.get("rt_text", ""), p["images"], p.get("ratio", 1.333),
         int(p.get("in_follow", 0)), int(p.get("in_hot", 0)), p.get("hot_at", 0), p.get("hot_rank", 0), time.time(),
         int(p.get("pics", 0)), p.get("starts", "")),
    )


def flag_follow(pid: str) -> None:
    _exec("UPDATE posts SET in_follow=1 WHERE id=?", (pid,))


def flag_hot(pid: str, at: float, rank: int) -> None:
    _exec("UPDATE posts SET in_hot=1, hot_at=?, hot_rank=? WHERE id=?", (at, rank, pid))


def update_counts(pid: str, likes: int, comments: int, reposts: int) -> None:
    _exec("UPDATE posts SET likes=?, comments=?, reposts=? WHERE id=?", (likes, comments, reposts, pid))


def cursor(p: dict[str, Any]) -> str:
    """关注列表里一条的位置，设备拿它要更早的。/ A following row's place; the device sends it back for older rows."""
    return f"{int(p['created_ts'] or 0)}|{p['id']}"


def feed(seg: str, since: float = 0, limit: int = 50, before: str = "") -> list[dict[str, Any]]:
    """关注按发布时间从新到旧，before 是上一批最后一条的游标（只对关注）。/ Following is newest first by post time;
    before is the previous batch's last cursor (following only)."""
    if seg == "hot":
        return _rows("SELECT * FROM posts WHERE in_hot=1 AND hot_at>? ORDER BY hot_at DESC, hot_rank ASC LIMIT ?",
                     (since, limit))
    where, args = "in_follow=1 AND fetched_at>?", [since]
    if before:
        key, _, pid = before.partition("|")
        where += " AND (created_ts<? OR (created_ts=? AND id<?))"
        args += [int(key), int(key), pid]
    return _rows(f"SELECT * FROM posts WHERE {where} ORDER BY created_ts DESC, id DESC LIMIT ?", (*args, limit))


def post(pid: str) -> dict[str, Any] | None:
    rows = _rows("SELECT * FROM posts WHERE id=?", (pid,))
    return rows[0] if rows else None


def latest(limit: int = 60) -> list[dict[str, Any]]:
    return _rows("SELECT * FROM posts ORDER BY fetched_at DESC LIMIT ?", (limit,))


def drop_hot_older(keep: int) -> list[str]:
    """热门只留最近 keep 条；被挤出且不在关注里的返回去删图。/ Keep the latest `keep` hot posts; return ids that
    dropped out and are not in following, so their images can go."""
    rows = _rows("SELECT id, in_follow FROM posts WHERE in_hot=1 ORDER BY hot_at DESC, hot_rank ASC")
    gone = []
    for r in rows[keep:]:
        if r["in_follow"]:
            _exec("UPDATE posts SET in_hot=0 WHERE id=?", (r["id"],))
        else:
            gone.append(r["id"])
    return gone


def old_posts(before: float) -> list[str]:
    """拉到时间早于 before（秒）的。/ Posts fetched before `before` (seconds)."""
    return [r["id"] for r in _rows("SELECT id FROM posts WHERE fetched_at<?", (before,))]


def oldest_posts(limit: int) -> list[str]:
    return [r["id"] for r in _rows("SELECT id FROM posts ORDER BY fetched_at LIMIT ?", (limit,))]


def delete_post(pid: str) -> None:
    _exec("DELETE FROM posts WHERE id=?", (pid,))
    _exec("DELETE FROM comments WHERE post_id=?", (pid,))


def post_count(seg: str | None = None) -> int:
    if seg == "hot":
        return _rows("SELECT COUNT(*) AS n FROM posts WHERE in_hot=1")[0]["n"]
    if seg == "follow":
        return _rows("SELECT COUNT(*) AS n FROM posts WHERE in_follow=1")[0]["n"]
    return _rows("SELECT COUNT(*) AS n FROM posts")[0]["n"]


# ---- 评论 / Comments ----

def comments(pid: str) -> list[dict[str, Any]] | None:
    rows = _rows("SELECT data FROM comments WHERE post_id=?", (pid,))
    return json.loads(rows[0]["data"]) if rows else None


def save_comments(pid: str, items: list[dict[str, Any]]) -> None:
    _exec("INSERT OR REPLACE INTO comments(post_id, data, fetched_at) VALUES(?,?,?)",
          (pid, json.dumps(items, ensure_ascii=False), time.time()))


# ---- 每日计数 / Daily counters ----

def _day(kind: str) -> str:
    return f"{kind}:{time.strftime('%Y-%m-%d')}"


def today(kind: str) -> int:
    rows = _rows("SELECT requests FROM counters WHERE day=?", (_day(kind),))
    return rows[0]["requests"] if rows else 0


def count(kind: str) -> None:
    _exec("INSERT INTO counters(day, requests) VALUES(?,1) ON CONFLICT(day) DO UPDATE SET requests=requests+1",
          (_day(kind),))
