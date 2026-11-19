"""面板密码（PBKDF2 存哈希、连续输错锁定）和会话；设备令牌校验。

Panel password (PBKDF2 hash, lockout after repeated failures) and sessions; device token checks.
"""
from __future__ import annotations

import hashlib
import hmac
import json
import os
import secrets
import time

from . import config

SESSION_COOKIE = "x_session"
SESSION_TTL = 7 * 24 * 3600
FAIL_LIMIT = 5
LOCK_SECONDS = 300

_sessions: dict[str, float] = {}
_fails = 0
_locked_until = 0.0


def _hash(password: str, salt: bytes) -> str:
    return hashlib.pbkdf2_hmac("sha256", password.encode(), salt, 200_000).hex()


def has_password() -> bool:
    return config.AUTH_PATH.exists()


def set_password(password: str) -> None:
    salt = os.urandom(16)
    data = {"salt": salt.hex(), "hash": _hash(password, salt)}
    tmp = config.AUTH_PATH.with_suffix(".tmp")
    tmp.write_text(json.dumps(data), encoding="utf-8")
    os.replace(tmp, config.AUTH_PATH)
    _sessions.clear()


def check_password(password: str) -> tuple[bool, str]:
    """返回 (是否通过, 失败原因)。/ Returns (ok, reason)."""
    global _fails, _locked_until
    now = time.time()
    if now < _locked_until:
        return False, f"输错太多次，{int(_locked_until - now)} 秒后再试"
    try:
        data = json.loads(config.AUTH_PATH.read_text(encoding="utf-8"))
        ok = hmac.compare_digest(_hash(password, bytes.fromhex(data["salt"])), data["hash"])
    except (OSError, ValueError, KeyError):
        ok = False
    if ok:
        _fails = 0
        return True, ""
    _fails += 1
    if _fails >= FAIL_LIMIT:
        _fails = 0
        _locked_until = now + LOCK_SECONDS
        return False, f"输错太多次，已锁定 {LOCK_SECONDS // 60} 分钟"
    return False, "密码不对"


def new_session() -> str:
    token = secrets.token_urlsafe(32)
    _sessions[token] = time.time() + SESSION_TTL
    return token


def session_ok(token: str | None) -> bool:
    if not token:
        return False
    expiry = _sessions.get(token)
    if not expiry or expiry < time.time():
        _sessions.pop(token, None)
        return False
    return True


def drop_session(token: str | None) -> None:
    if token:
        _sessions.pop(token, None)


def device_ok(token: str | None) -> bool:
    expected = config.load().get("device_token") or ""
    return bool(token) and bool(expected) and hmac.compare_digest(token, expected)
