# weibo hub：Playwright 官方 Python 镜像（自带 Chromium，用来生成访客身份和处理验证码）+ FastAPI + Pillow，
# 内置 cloudflared（固定版本）。
# weibo hub: the official Playwright Python image (Chromium for the visitor identity and captchas) + FastAPI +
# Pillow, with a pinned cloudflared.
FROM mcr.microsoft.com/playwright/python:v1.49.1-noble

ARG CLOUDFLARED_VERSION=2026.10.7
ADD https://github.com/cloudflare/cloudflared/releases/download/${CLOUDFLARED_VERSION}/cloudflared-linux-amd64 /usr/local/bin/cloudflared
RUN chmod 0755 /usr/local/bin/cloudflared

WORKDIR /srv
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY app ./app

ENV DATA_DIR=/data PORT=8092 PYTHONUNBUFFERED=1 TZ=Asia/Shanghai
VOLUME ["/data"]
EXPOSE 8092
CMD ["sh", "-c", "exec uvicorn app.main:app --host 0.0.0.0 --port ${PORT} --proxy-headers --forwarded-allow-ips 127.0.0.1"]
