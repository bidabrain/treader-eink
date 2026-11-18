# tomato hub：Python 3.12 + FastAPI，放在官方番茄下载器前面转发设备请求；内置 cloudflared（固定版本）。
# tomato hub: Python 3.12 + FastAPI in front of the official Tomato downloader, forwarding device requests; with a
# pinned cloudflared.
FROM python:3.12-slim

ARG CLOUDFLARED_VERSION=2026.10.7
ADD https://github.com/cloudflare/cloudflared/releases/download/${CLOUDFLARED_VERSION}/cloudflared-linux-amd64 /usr/local/bin/cloudflared
RUN chmod 0755 /usr/local/bin/cloudflared

WORKDIR /srv
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY app ./app

ENV DATA_DIR=/data PORT=18423 PYTHONUNBUFFERED=1 TOMATO_UPSTREAM=http://downloader:18423/webui
VOLUME ["/data"]
EXPOSE 18423
CMD ["sh", "-c", "exec uvicorn app.main:app --host 0.0.0.0 --port ${PORT} --proxy-headers --forwarded-allow-ips 127.0.0.1"]
