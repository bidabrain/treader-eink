"""在 hub 容器里运行 cloudflared：快速隧道（随机 trycloudflare 地址）或隧道令牌。隧道只放行设备接口，面板不出外网。

Run cloudflared inside the hub container: a quick tunnel (random trycloudflare address) or a tunnel token. The tunnel
only carries the device API; the panel never goes public.
"""
from __future__ import annotations

import asyncio
import re
import shutil

from . import config

state = {"mode": "off", "url": "", "status": "已关闭", "error": ""}
_proc: asyncio.subprocess.Process | None = None
_reader: asyncio.Task | None = None
URL_RE = re.compile(r"https://[a-z0-9-]+\.trycloudflare\.com")


async def _watch(proc: asyncio.subprocess.Process) -> None:
    assert proc.stderr
    async for raw in proc.stderr:
        line = raw.decode("utf-8", "replace")
        m = URL_RE.search(line)
        if m:
            state["url"] = m.group(0)
            state["status"] = "已连接"
        elif "Registered tunnel connection" in line:
            state["status"] = "已连接"
        elif " ERR " in line or "error" in line.lower():
            state["error"] = line.strip()[-200:]
    code = await proc.wait()
    if state["mode"] != "off":
        state["status"] = f"cloudflared 已退出（{code}）"


async def stop() -> None:
    global _proc, _reader
    if _proc and _proc.returncode is None:
        _proc.terminate()
        try:
            await asyncio.wait_for(_proc.wait(), 10)
        except asyncio.TimeoutError:
            _proc.kill()
            await _proc.wait()
    # 旧的读日志任务收掉，免得它稍后把新进程的状态改成“已退出”。/ Finish the old log reader so it cannot later mark
    # the new process as exited.
    if _reader and not _reader.done():
        _reader.cancel()
        try:
            await _reader
        except (asyncio.CancelledError, Exception):
            pass
    _proc = _reader = None
    state.update(mode="off", url="", status="已关闭", error="")


async def apply() -> None:
    """按配置启动或停止隧道。/ Start or stop the tunnel per settings."""
    global _proc, _reader
    await stop()
    cfg = config.load()
    mode = cfg.get("tunnel_mode") or "off"
    if mode == "off":
        return
    binary = shutil.which("cloudflared")
    if not binary:
        state.update(mode=mode, status="镜像里没有 cloudflared", error="")
        return
    local = f"http://127.0.0.1:{config.PORT}"
    if mode == "quick":
        args = [binary, "tunnel", "--no-autoupdate", "--url", local]
    elif mode == "token" and cfg.get("tunnel_token"):
        args = [binary, "tunnel", "--no-autoupdate", "run", "--token", cfg["tunnel_token"]]
    else:
        state.update(mode=mode, status="缺少隧道令牌", error="")
        return
    state.update(mode=mode, url="", status="正在连接…", error="")
    _proc = await asyncio.create_subprocess_exec(*args, stdout=asyncio.subprocess.DEVNULL,
                                                 stderr=asyncio.subprocess.PIPE)
    _reader = asyncio.create_task(_watch(_proc))
