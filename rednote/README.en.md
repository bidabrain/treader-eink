[中文](README.md) | **English**

# rednote: RedNote (Xiaohongshu) content server

Signs in to the RedNote web version on your own server, periodically fetches the **image notes** of the creators you follow, turns the images into grayscale pictures the e-ink screen can show directly, and lets Read Pico pick them up.

- **Deploy from one compose file**: `docker compose up -d`, then open the web panel to do all the setup.
- **Web panel**: sign in to RedNote by QR code, add creators, view fetched content, set the fetch frequency, generate a device token and turn on internet access.
- **Internet access**: built-in Cloudflare Tunnel (quick tunnel or tunnel token), so the device can fetch while you are out; the panel always stays on the LAN.
- **Less work for the device**: video notes are skipped. Images are converted to grayscale on the server, scaled to fit within 648×1000, cut into screens when too tall, and saved as plain JPEG (about 30–70 KB each), plus a cover 300 wide that keeps the original aspect ratio.
- **Read-only**: it only fetches content; it never posts, likes or comments.

Data is fetched by the open-source project [xiaohongshu-mcp](https://github.com/xpzouying/xiaohongshu-mcp) (Apache-2.0): it signs in and browses with a real browser inside the container, without reverse-engineering signatures. This directory only adds a panel, scheduled fetching and image processing around it.

> **On the device**: the Treader firmware on Read Pico has a RedNote tab (since 0.5.0). On the device, open Settings → Content sources → RedNote (设置 → 内容来源 → 小红书) and turn on "Fill in from phone" (手机网页填写), then paste the address and token from step 6 below using your phone.

You can also read on your phone: [rednote/android](android/README.md) is a standalone Android app with the same features as the firmware RedNote tab; it reads from the same server with the same device token.

```
Read Pico ──HTTPS (token)──> rednote-hub :8091 ──> rednote-mcp (browser signed in to RedNote) ──> RedNote
                                │ panel (LAN only)
                                └─ cloudflared (optional) ──> Cloudflare ──> devices on the internet
```

## Before you deploy

| Item | Requirement |
| --- | --- |
| Machine | **x86-64** (amd64) Linux, a NAS, or a computer with Docker Desktop. The official xiaohongshu-mcp image has no ARM build, so Raspberry Pi and ARM Synology models will not work |
| Location | **Mainland China recommended**. When the outgoing IP is overseas, Xiaohongshu redirects to the international rednote: search does not work, recommendations are international content, and the first sign-in requires clicking "登录" (Sign in) by hand in the "浏览器桌面" (Browser desktop) |
| Memory | At least 2 GB free. The browser uses about 130 MB when idle and 0.5–1 GB while fetching |
| Disk | About 2.5 GB for the images, plus the cache (2 GB limit by default, adjustable) |
| Account | A RedNote account; **a secondary account with real-name verification is recommended** (new accounts without it get asked to verify) |
| Software | Docker and the compose plugin. Synology Container Manager, QNAP Container Station and Portainer also work |

## Deployment

### 1. Prepare a folder

Create an empty folder (for example `/volume1/docker/rednote` on a NAS) and put this directory's `docker-compose.yml` in it:

```bash
mkdir rednote && cd rednote
curl -fsSLO https://raw.githubusercontent.com/bidabrain/treader-eink/main/rednote/docker-compose.yml
```

If you need to change the port, allow access from this machine only, or configure a proxy, also download `.env.example`, rename it to `.env` and edit it (instructions are in the file):

```bash
curl -fsSL -o .env https://raw.githubusercontent.com/bidabrain/treader-eink/main/rednote/.env.example
```

### 2. Start

```bash
docker compose up -d
```

The first run downloads two images (about 2.5 GB together): `ghcr.io/bidabrain/treader-rednote` (the panel, built automatically by this repository's GitHub Actions) and `xpzouying/xiaohongshu-mcp` (data fetching). All data lives in `data/` in the same folder.

> To build the panel from source yourself: clone the repository and run, in the `rednote/` directory,
> `docker compose -f docker-compose.yml -f docker-compose.build.yml up -d --build`.

### 3. Set the panel password

Open `http://server-IP:8091` in a browser and set the panel password on first visit (at least 8 characters). Five wrong attempts in a row lock it for 5 minutes.

### 4. Sign in to RedNote

Panel → **小红书账号** (RedNote account) → **获取登录二维码** (Get sign-in QR code). Scan it with the RedNote app and confirm, wait a few seconds, then click **查询登录状态** (Check sign-in status). Once it shows "已登录" (Signed in), you are done.

- If the QR code has not appeared after half a minute: click **打开浏览器桌面** (Open browser desktop) and click "登录" (Sign in) on the RedNote page inside it; the QR code then appears in the panel (common when the server's outgoing IP is overseas).
- **After signing in, do not sign in to this account in a browser on another computer**, or the server gets kicked off; the phone app keeps working as usual.
- The sign-in state is saved in `data/mcp/cookies.json`, so restarting containers does not require scanning again. When it expires, just scan again.

### 5. Add creators

Panel → **博主** (Creators), two ways:

- **Paste a profile link**: open the creator's profile on the RedNote website, copy the full link from the address bar (it must include `xsec_token=`), paste it and click "添加" (Add).
- **Pick from the home feed recommendations**: lists the authors in the recommendations; click "关注" (Follow). This makes one request to RedNote.

After adding, click **立即拉一轮** (Fetch a round now) on the overview page, or wait for the scheduled job. Fetched notes appear as thumbnails on the **内容** (Content) page.

### 6. Connect the device

Panel → **设备与外网** (Devices & internet) shows the address and the token.

- **At home**: use `http://server-LAN-IP:8091` as the address.
- **Away from home**: choose an internet access method; after saving, the public address is shown:
  - **Quick tunnel**: nothing to prepare. You get a random `https://xxx.trycloudflare.com` that **changes on every restart**, suitable for temporary use.
  - **Tunnel token** (for long-term use; your domain must be hosted on Cloudflare):
    1. [Cloudflare Zero Trust](https://one.dash.cloudflare.com/) → Networks → Tunnels → Create a tunnel, type Cloudflared. You do not need to run the install command the page shows; just copy the token starting with `eyJ` from it.
    2. Under the tunnel's "Published application routes / Public hostname", add an entry: any subdomain (such as `pico`), your domain, **path `^/api/device/`**, service type **HTTP**, URL **`localhost:8091`** (localhost, because cloudflared runs inside the panel container). With the path set, panel requests are blocked at Cloudflare and never reach your server.
    3. Do not add sign-in protection for it under Access → Applications; the device cannot get past that sign-in page.
    4. Paste the token into the panel, choose "隧道令牌" (Tunnel token), then save and apply. The device's public address is `https://pico.your-domain`.
- The panel program (hub) also checks on its own: requests coming in through the tunnel (with a `CF-Connecting-IP` header) can only reach `/api/device/`; everything else returns 403, and the browser desktop cannot be reached either. So a quick tunnel (which cannot have a path) is just as safe. **Do not forward port 8091 to the internet on your router**.

## Fetch rules (avoiding risk control)

All defaults can be changed under panel → **设置** (Settings):

| Setting | Default | Notes |
| --- | --- | --- |
| Interval between rounds | 120 minutes | At least 30 minutes |
| Interval between requests | Random 15–30 seconds | At least 5 seconds. Each read of a creator profile and each note detail counts as one request |
| New notes per creator per round | 5 | Only image notes not fetched before; videos are skipped at the list stage |
| Daily request limit | 200 | When reached, the round stops and resumes the next day |
| Images per note | 18 | Tall images cut into screens add a few more pages |
| Cache | 30 days / 2000 MB | The oldest are deleted beyond that |

### Recommendations (fetched on demand by the device)

Besides the creators you follow, the device can also show this account's home feed recommendations. The server **does not refresh the home feed on a schedule**: it reads the home feed only when you tap "Refresh recommendations" (刷新推荐) on the device (videos are filtered out), and fetches the details and images only of the note you open, at a pace similar to a person scrolling on a phone; the recommendations also adapt to what you actually open. Wait times: about 5–15 seconds for a refresh; about 6–12 seconds to open a note. The text is shown as soon as it arrives, images are processed 4 at a time in the background, and turning to a page that is not ready yet waits briefly.

| Setting | Default | Notes |
| --- | --- | --- |
| Allow the device to fetch recommendations | On | When off, the device's recommendation API returns 403 |
| Maximum per list | 12 image notes | At most 30 |
| List cache | 10 minutes | Refreshing within this time returns the previous list, to prevent repeated taps; at least 5 minutes |
| Daily recommendation limit | 60 | Refreshes and opens both count, and also count toward the daily total limit above |
| Interval between recommendation requests | 3 seconds + random 0–3 seconds | Shares one browser with scheduled fetching, so requests queue; at least 3 seconds |

When RedNote asks for verification (a slider) or the sign-in expires, fetching **pauses automatically** and the overview page shows the reason. Resolve it in the **浏览器桌面** (Browser desktop) (or scan the QR code again), then click "立即拉一轮" (Fetch a round now) to continue.

## Device API

Send the header `x-rednote-token: device-token` (or use `?token=`).

| Endpoint | Returns |
| --- | --- |
| `GET /api/device/v1/feed?since=时间戳&limit=50` | Notes from followed creators (`limit` up to 300): `{now, notes:[{id, title, author, time, images, likes, comments, fetched_at, source, ratio}]}`, newest fetch first; pass the `now` from the previous response as `since` (a timestamp) to get only new ones |
| `GET /api/device/v1/rec?refresh=1` | Recommendation list `{fetched_at, refresh_after, left_today, notes:[{id, title, author, likes, ratio, thumb, ready}]}`. The home feed is read only with `refresh=1` after the cache period; otherwise the previous list is returned. `ready` means the details have been fetched |
| `GET /api/device/v1/rec/cover/{id}.jpg` | Cover in the recommendation list, same format as `thumb.jpg` below |
| `GET /api/device/v1/rec/note/{id}` | Opens a note from the recommendations: fetched on the spot if not fetched yet (videos return 415), same response as `note/{id}`; after that, use the `img` endpoint below for images. 429 when the quota is used up, 503 while paused, 404 if not in the recent list |
| `GET /api/device/v1/note/{id}` | The fields above plus `body` (text), `comment_list` (first 20: `user`, `text`, `likes`) and `comments_more` (more comments can be loaded: the server has this note's token, they have not been loaded yet, and the total comment count is higher than what is stored) |
| `GET /api/device/v1/note/{id}/more_comments?start=1` | Load more comments: `start=1` makes the server fetch up to 60 for this note on the spot (the browser scrolls down, about 20–60 seconds; counts as one on-demand request, subject to the recommendation and daily total limits). Then ask repeatedly without `start`, each call waiting up to 20 seconds: `{"loading": true}` while still fetching, and the same content as `note/{id}` when done. It is split into separate calls to stay clear of Cloudflare's roughly 100-second timeout. Returns 409 when there is no token for this note (fetched before the server upgrade and not seen again on the creator's profile since) |
| `GET /api/device/v1/img/{id}/{n}.jpg` | Image page n (starting at 1), grayscale baseline JPEG, within 648×1000 |
| `GET /api/device/v1/img/{id}/thumb.jpg` | Cover: 300 wide, keeps the original aspect ratio (height-to-width ratio clamped between 3:4 and 4:3, which is the `ratio` in the API) |

## Maintenance

```bash
docker compose pull && docker compose up -d   # upgrade
docker compose logs -f hub                    # panel logs
docker compose down                           # stop (data stays in data/)
```

- **Backup / migration**: stop it, then copy the whole folder (mainly `data/`).
- **WebDAV backup of followed creators**: panel → 设置 → WebDAV 备份 (Settings → WebDAV backup). Enter the address, user name and password (Jianguoyun and similar services work), click "立即备份" (Back up now), and the list (creator IDs, nicknames, and the `xsec_token` needed for adding) is saved on WebDAV as `rednote_backup.json`. On a new machine, enter the same WebDAV and click "从备份恢复" (Restore from backup); the list in the backup replaces the current one (fetch progress for existing creators is kept). The RedNote sign-in state is not in the backup; scan the QR code again after restoring.
- **Forgot the panel password**: delete `data/auth.json`, run `docker restart rednote-hub`, then open the panel and set it again.
- **xiaohongshu-mcp version**: pinned to `v2.7.4` in the compose file; change it with `MCP_VERSION` in `.env`. Try a new version for one round in the panel before using it long-term.
- **Do not run two setups at once** (for example a test machine and a NAS) with the same RedNote account.

## Security and compliance

- The RedNote sign-in state stays only in `data/mcp/` and is never sent to the device; the device only gets processed text and images.
- The panel password is stored as a PBKDF2 hash; the device token can be changed in the panel at any time.
- The browser desktop (noVNC) can only be opened after signing in to the panel, and never goes through the tunnel.
- Automated scraping does not comply with the RedNote user agreement, and the account risks being flagged by risk control. Use it only for personal reading, keep the frequency low, and do not deploy it publicly for others.

## Layout

| Path | Contents |
| --- | --- |
| `docker-compose.yml` | For deployment: panel (hub) + xiaohongshu-mcp |
| `docker-compose.build.yml` | Build the hub from source |
| `.env.example` | Port, mcp version, proxy |
| `hub/app/main.py` | Routes: panel, device API, tunnel restrictions, browser desktop forwarding |
| `hub/app/fetcher.py` | Scheduled fetching and cleanup |
| `hub/app/rec.py` | Recommendations: on-demand home feed refresh by the device, fetch on open |
| `hub/app/images.py` | Grayscale conversion, scaling, screen cutting, thumbnails |
| `hub/app/mcp.py` | xiaohongshu-mcp API client |
| `hub/app/tunnel.py` | cloudflared subprocess |
| `hub/app/web/` | Panel pages |
| `android/` | Android client (same features as the firmware RedNote tab), with its own version number and release process |
