"""xiaohongshu-mcp 的 HTTP 接口客户端。只用读取类接口，不发帖、不点赞、不评论。

Client for the xiaohongshu-mcp HTTP API. Read-only endpoints only: no posting, liking or commenting.
"""
from __future__ import annotations

from typing import Any

import httpx

from . import config

# 浏览器操作慢：详情和主页通常 5～10 秒，二维码在海外出口时要等人工点开，给足时间。
# Browser actions are slow: details and profiles take 5-10 s, and the QR may wait for a manual click abroad.
TIMEOUT = httpx.Timeout(180.0, connect=10.0)


class McpError(Exception):
    pass


async def _call(method: str, path: str, body: dict[str, Any] | None = None) -> dict[str, Any]:
    url = f"{config.MCP_URL}/api/v1{path}"
    try:
        async with httpx.AsyncClient(timeout=TIMEOUT) as client:
            resp = await client.request(method, url, json=body)
    except httpx.HTTPError as e:
        raise McpError(f"连不上小红书服务（mcp）：{type(e).__name__}") from e
    try:
        data = resp.json()
    except ValueError as e:
        raise McpError(f"mcp 返回不是 JSON（HTTP {resp.status_code}）") from e
    if resp.status_code != 200 or not data.get("success"):
        detail = data.get("details") or data.get("error") or data.get("message") or f"HTTP {resp.status_code}"
        raise McpError(str(detail))
    return data.get("data") or {}


async def health() -> bool:
    try:
        async with httpx.AsyncClient(timeout=5.0) as client:
            resp = await client.get(f"{config.MCP_URL}/health")
        return resp.status_code == 200
    except httpx.HTTPError:
        return False


async def login_status() -> dict[str, Any]:
    return await _call("GET", "/login/status")


async def login_qrcode() -> dict[str, Any]:
    return await _call("GET", "/login/qrcode")


async def feeds() -> list[dict[str, Any]]:
    return (await _call("GET", "/feeds/list")).get("feeds") or []


async def profile(user_id: str, xsec_token: str) -> dict[str, Any]:
    data = await _call("POST", "/user/profile", {"user_id": user_id, "xsec_token": xsec_token})
    return data.get("data") or data


async def detail(note_id: str, xsec_token: str) -> dict[str, Any]:
    data = await _call("POST", "/feeds/detail", {"feed_id": note_id, "xsec_token": xsec_token})
    return data.get("data") or data
