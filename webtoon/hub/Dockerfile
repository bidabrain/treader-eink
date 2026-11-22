# webtoon hub：Python 3.12 + FastAPI + Pillow + OpenCC，内置 cloudflared（固定版本）。
# webtoon hub: Python 3.12 + FastAPI + Pillow + OpenCC with a pinned cloudflared.
FROM python:3.12-slim

ARG CLOUDFLARED_VERSION=2026.10.7
ADD https://github.com/cloudflare/cloudflared/releases/download/${CLOUDFLARED_VERSION}/cloudflared-linux-amd64 /usr/local/bin/cloudflared
RUN chmod 0755 /usr/local/bin/cloudflared

WORKDIR /srv
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY app ./app

ENV DATA_DIR=/data PORT=8095 PYTHONUNBUFFERED=1
VOLUME ["/data"]
EXPOSE 8095
CMD ["sh", "-c", "exec uvicorn app.main:app --host 0.0.0.0 --port ${PORT} --proxy-headers --forwarded-allow-ips 127.0.0.1"]
