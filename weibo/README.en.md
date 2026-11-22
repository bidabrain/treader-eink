[中文](README.md) | **English**

# weibo: Weibo content server

Runs on your own server **without signing in to Weibo**, periodically fetches **image posts** from the people you follow and from the hot list, turns the images into grayscale pictures the e-ink screen can show directly, and lets Read Pico pick them up.

- **Two feeds**: Following (everyone you follow merged into one timeline) and Hot.
- **Deploy from one compose file**: `docker compose up -d`, then open the web panel to do all the setup.
- **No sign-in needed**: the server uses a headless browser to generate a Weibo visitor identity. The following list is managed in the panel (add by UID, profile link or nickname search), with the same logic as the follow management in [weiboX-server](https://github.com/bidabrain/weiboX-server). You can also enter a signed-in cookie, which is more reliable.
- **WebDAV backup**: uses the same `weibox_backup.json` as the weiboX Android app and weiboX-server (following list, special follows, cookie), so all three can restore from each other.
- **Less work for the device**: posts with video are not collected. Long posts get their full text filled in on the server. Images (including those of the original post in a repost) are converted to grayscale on the server, scaled to fit within 648×1000, cut into screens when too tall, and saved as plain JPEG, plus a cover 300 wide that keeps the original aspect ratio.
- **Read-only**: it only fetches content; it never posts, likes or comments, and there are no push notifications.

```
Read Pico ──HTTPS (token)──> weibo-hub :8092 ──(visitor identity / cookie)──> m.weibo.cn
                                │ panel (LAN only)
                                └─ cloudflared (optional) ──> Cloudflare ──> devices on the internet
```

## Before you deploy

| Item | Requirement |
| --- | --- |
| Machine | **x86-64** (amd64) Linux, a NAS, or a computer with Docker Desktop |
| Memory | At least 1 GB free. The browser only starts to generate the visitor identity and to handle verification |
| Disk | About 2 GB for the image (includes Chromium), plus the cache (2 GB limit by default, adjustable) |
| Software | Docker and the compose plugin. Synology Container Manager, QNAP Container Station and Portainer also work |

## Deployment

### 1. Prepare a folder

Create an empty folder (for example `/volume1/docker/weibo` on a NAS) and put this directory's `docker-compose.yml` in it:

```bash
mkdir weibo && cd weibo
curl -fsSLO https://raw.githubusercontent.com/bidabrain/treader-eink/main/weibo/docker-compose.yml
```

If you need to change the port or allow access from this machine only, also download `.env.example`, rename it to `.env` and edit it:

```bash
curl -fsSL -o .env https://raw.githubusercontent.com/bidabrain/treader-eink/main/weibo/.env.example
```

### 2. Start

```bash
docker compose up -d
```

The image is `ghcr.io/bidabrain/treader-weibo`, built automatically by this repository's GitHub Actions. All data lives in `data/` in the same folder.

> To build from source yourself: clone the repository and run, in the `weibo/` directory,
> `docker compose -f docker-compose.yml -f docker-compose.build.yml up -d --build`.

### 3. Set the panel password

Open `http://server-IP:8092` in a browser and set the panel password on first visit (at least 8 characters). Five wrong attempts in a row lock it for 5 minutes.

### 4. Add people to follow

Panel → **关注** (Following), three ways:

- **UID or profile link**: `1234567890`, `https://weibo.com/u/1234567890` and `https://m.weibo.cn/u/1234567890` all work.
- **Search by nickname**: once found, click "关注" (Follow).
- **Restore from WebDAV**: fill in the address under **设置 → WebDAV 备份** (Settings → WebDAV backup) and click "从备份恢复" (Restore from backup) to use the following list from weiboX directly.

After adding, click **立即拉一轮** (Fetch a round now) on the overview page, or wait for the scheduled job. Fetched posts appear on the **内容** (Content) page.

### 5. Connect the device

Panel → **设备与外网** (Devices & internet) shows the address and the token. Internet access works the same as for rednote (quick tunnel or tunnel token; for the token method, set the route **path to `^/api/device/`** and the URL to **`localhost:8092`**); see [rednote/README.en.md](../rednote/README.en.md#6-connect-the-device) for details. Requests coming in through the tunnel can only reach `/api/device/`; the panel and the verification page always return 403. **Do not forward port 8092 to the internet on your router**.

## Fetch rules (avoiding risk control)

Scheduling follows weiboX-server: each round sorts people by how "due" they are (time since the last read ÷ how often this person posts on average), and the most due are read first. All defaults can be changed under panel → **设置** (Settings):

| Setting | Default | Notes |
| --- | --- | --- |
| Interval between rounds | 30 minutes | At least 30 minutes |
| Between two people | Random 30–60 seconds | At least 5 seconds |
| Recent posts read per person per round | 20 | One request; existing posts only get their repost / comment / like counts updated |
| Minimum gap for the same person | 5 minutes | |
| Hot | On, 30 posts | Read once at the end of each round |
| Full text of long posts | 150 per day, 3 seconds apart | When used up, only the truncated text is stored |
| Images per post | 18 | Tall images cut into screens add a few more pages |
| Cache | 30 days / 2000 MB | The oldest are deleted beyond that; Hot keeps only the most recent posts |

When Weibo asks for verification, fetching **pauses automatically** and the overview page shows the reason. On the **验证** (Verification) page, click and drag the slider directly on the server browser's screen, then click "完成" (Done) when finished. An expired visitor identity is regenerated automatically.

Comments are not fetched on a schedule: they are fetched once (the first 20) the first time the device opens the comments of a post, and served from cache after that.

## Device API

Send the header `x-weibo-token: device-token` (or use `?token=`).

| Endpoint | Returns |
| --- | --- |
| `GET /api/device/v1/feed?seg=follow&since=时间戳&limit=100` | `seg` is `follow` (newest posted first) or `hot` (by time on the list and rank). `limit` up to 300. `since` is a timestamp. `{now, posts:[{id, user, time, source, text, more, images, pics, ratio, likes, comments, reposts, rt_user, rt_text, fetched_at}]}`; `text` is cut at 140 characters, `more` means there is full text or reposted content; `images` is the number of large image pages, `pics` is the number of original images that have a square thumbnail (0 for posts fetched before 0.1.3) |
| `GET /api/device/v1/post/{id}` | The fields above, with `text` and `rt_text` as full text, plus `starts` (the first page of each original image, starting at 0) |
| `GET /api/device/v1/post/{id}/comments` | `{comments:[{user, text, likes}]}`, fetched on the first request |
| `GET /api/device/v1/img/{id}/{n}.jpg` | Image page n (starting at 1), grayscale baseline JPEG, within 648×1000 |
| `GET /api/device/v1/img/{id}/s{k}.jpg` | Square thumbnail of original image k (starting at 1), 240×240 grayscale, for lists and grids |
| `GET /api/device/v1/img/{id}/thumb.jpg` | Cover: 300 wide, keeps the original aspect ratio (height-to-width ratio clamped between 3:4 and 4:3, which is `ratio`) |

## Maintenance

```bash
docker compose pull && docker compose up -d   # upgrade
docker compose logs -f hub                    # logs
docker compose down                           # stop (data stays in data/)
```

- **Forgot the panel password**: delete `data/auth.json`, run `docker restart weibo-hub`, then open the panel and set it again.
- **Do not run it at a high frequency alongside weiboX-server on the same outgoing IP**; the requests from both add up.

## Security and compliance

- The cookie and WebDAV password are stored only in `data/config.json`; the panel does not display them back, and they are never sent to the device.
- The panel password is stored as a PBKDF2 hash; the device token can be changed in the panel at any time.
- Automated scraping does not comply with the Weibo user agreement. Use it only for personal reading, keep the frequency low, and do not deploy it publicly for others.

## Layout

| Path | Contents |
| --- | --- |
| `docker-compose.yml` | For deployment |
| `docker-compose.build.yml` | Build from source |
| `.env.example` | Port |
| `hub/app/main.py` | Routes: panel, device API, verification page, tunnel restrictions |
| `hub/app/fetcher.py` | Scheduled fetching and cleanup |
| `hub/app/images.py` | Grayscale conversion, scaling, screen cutting, thumbnails |
| `hub/app/webdav.py` | WebDAV backup compatible with weiboX |
| `hub/app/weibo/` | Weibo API, parsing, visitor identity, captcha (ported from weiboX-server) |
| `hub/app/tunnel.py` | cloudflared subprocess |
| `hub/app/web/` | Panel pages |
