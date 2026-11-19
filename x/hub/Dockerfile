# x hub：Playwright 官方 Python 镜像（自带 Chromium）+ Xvfb 虚拟屏幕 + x11vnc / noVNC（面板里手动登录 X）
# + FastAPI + Pillow，内置 cloudflared（固定版本）。
# x hub: the official Playwright Python image (Chromium) + an Xvfb screen + x11vnc / noVNC (sign in to X by hand from
# the panel) + FastAPI + Pillow, with a pinned cloudflared.
FROM mcr.microsoft.com/playwright/python:v1.49.1-noble

ARG CLOUDFLARED_VERSION=2026.10.7
ADD https://github.com/cloudflare/cloudflared/releases/download/${CLOUDFLARED_VERSION}/cloudflared-linux-amd64 /usr/local/bin/cloudflared
RUN chmod 0755 /usr/local/bin/cloudflared

WORKDIR /srv
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
# 先装 Python 包再装 noVNC：它拖进来的 Debian 版 Python 包会让 pip 卸不掉旧版本。
# Python packages go before noVNC: the Debian Python packages it pulls in would block pip upgrades.
# 用正式版 Google Chrome，不用 Playwright 自带的 Chromium：X 登录对后者常报“暂时限制登录”。
# Use stable Google Chrome rather than Playwright's bundled Chromium: X sign-in often refuses the latter.
ADD https://dl.google.com/linux/direct/google-chrome-stable_current_amd64.deb /tmp/chrome.deb
RUN apt-get update \
 && apt-get install -y --no-install-recommends xvfb x11vnc novnc fonts-noto-cjk /tmp/chrome.deb \
 && rm -rf /var/lib/apt/lists/* /tmp/chrome.deb
COPY app ./app

ENV DATA_DIR=/data PORT=8093 DISPLAY=:99 PYTHONUNBUFFERED=1 TZ=Asia/Shanghai
VOLUME ["/data"]
EXPOSE 8093
CMD ["sh", "-c", "exec uvicorn app.main:app --host 0.0.0.0 --port ${PORT} --proxy-headers --forwarded-allow-ips 127.0.0.1"]
