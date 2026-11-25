[中文](README.md) | **English**

# x: X (Twitter) content server

The server runs a Chromium with a display, and you sign in to your own X account by hand over VNC from the panel. After that it opens x.com at random intervals, fetches "For You" and "Following", drops ads, and saves the images as grayscale pictures for the e-ink screen, for Read Pico and the Android app to read offline.

> Only the server (panel) exists for now. The device tab and the Android app are not done yet.

## Know the risks first

This is automated scraping with your own account and does not comply with X's terms of service. X may ask you to verify the account, and in serious cases may lock it. The following practices reduce the risk:

- All scraping is done by X's own web page: the browser opens the home page, switches tabs and scrolls down as normal, and the hub only listens in on the requests the page sends. The hub does not call the API directly or forge request headers.
- Read-only: no likes, reposts or follows.
- Automatic fetch intervals are picked at random between a minimum and a maximum, and the minimum interval cannot be below 30 minutes. Nothing is fetched during the sleep window, and the first fetch after waking is also delayed at random, never exactly on the hour.
- On an expired sign-in, account verification or a 429 rate limit, automatic fetching stops immediately and does not retry on its own.
- You sign in on the server, so the sign-in IP is the same IP used for fetching. It is best to keep the server at home, using the connection you normally browse with.

## Deployment

```bash
mkdir x && cd x
curl -O https://raw.githubusercontent.com/bidabrain/treader-eink/main/x/docker-compose.yml
docker compose up -d
```

If the image has not been published yet, build from source:

```bash
docker compose -f docker-compose.yml -f docker-compose.build.yml up -d --build
```

Then open `http://server-IP:8093` and set the panel password on first visit.

To change the port or time zone, copy `.env.example` to `.env` and edit it. Without a `.env`, the defaults are used: port 8093, time zone Asia/Shanghai.

## Usage

1. **Sign in to X**: on the panel's "登录 X" (Sign in to X) page, click "打开登录浏览器" (Open sign-in browser). The screen of the browser on the server appears in the page; sign in directly inside it. Two-step verification and captchas are also completed there.
   - Sign-in uses a completely ordinary Google Chrome (the same profile used for fetching), with no automation control at all during sign-in.
   - Once you see the home page, click "登录好了" (Signed in); the server saves the sign-in and closes the screen.
   - If the sign-in later expires, or X asks you to verify the account, the overview page tells you; come back to this page and sign in again.
   - If the screen does not work, you can use "导入 cookie" (Import cookie) on the same page as a fallback.
2. **Proxy**: if the server cannot reach X, enter a proxy under "设置" (Settings). Both the browser and image downloads go through this proxy.
   - To use a proxy on the local machine with Docker Desktop, write `http://host.docker.internal:端口` (where 端口 is the port).
3. **Scheduling**: configure it under "设置" (Settings).
   - For You and Following each have their own minimum and maximum interval.
   - The number of scrolls per fetch is also random.
   - You can set a sleep window, which may cross midnight.
   - Following has a master switch. When it is off, this feed is no longer fetched, manually or automatically; the device and the app read this state and grey out the "Following" tab.
4. **Manual fetch**: the "立即拉取" (Fetch now) button on the overview page ignores the interval and the sleep window. After it finishes, that feed's automatic timer starts over.
5. **After a 429**: automatic fetching stays stopped. Wait a while, make sure X works normally on your phone, then click "恢复自动拉取" (Resume automatic fetching). One successful manual fetch also resumes it automatically.

## Data

All state is in `./data`:

| Contents | Location |
| --- | --- |
| Browser profile (including sign-in state) | `browser/` |
| Backup of the sign-in cookies | `x_cookies.json` |
| Post database | `x.db` |
| Images | `posts/` |
| Settings | `config.json` |
| Panel password hash | `auth.json` |

**This directory is equivalent to your X sign-in credentials. Do not share it.**

## Security

- The panel and the VNC screen can only be opened on the LAN. Requests coming in through a Cloudflare tunnel can only reach the device API (`/api/device/`).
- Opening the VNC screen requires signing in to the panel first. VNC only runs while the sign-in window is open, and only listens inside the container.

## Device API

The fields match the Weibo hub, so clients can reuse the same code. Pass the token with the `X-Hub-Token` request header or the `?token=` parameter.

| Endpoint | Description |
| --- | --- |
| `GET /api/device/v1/status` | Account status; each feed's master switch (`feeds.following.on`), last and next fetch times |
| `GET /api/device/v1/feed?seg=foryou\|following&since=&limit=` | List. When the master switch is off, returns an empty list and `on: false`. Since 0.1.6 the reply also has `next`: a cursor while older posts remain, which passed back as `before=` returns the next batch; empty means nothing older is left |
| `POST /api/device/v1/refresh?seg=foryou` | Fetch once now, same as a manual fetch in the panel |
| `GET /api/device/v1/post/{id}` | Full text; a quoted post goes in `rt_user` and `rt_text` |
| `GET /api/device/v1/post/{id}/comments` | Replies. On the first request the browser opens this post to fetch them; cached after that |
| `GET /api/device/v1/img/{id}/{n}.jpg` | Images: `thumb.jpg` is the cover, `s<k>.jpg` is a square thumbnail |
