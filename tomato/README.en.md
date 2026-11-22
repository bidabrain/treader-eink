[中文](README.md) | **English**

# tomato: Tomato novel download server (with panel)

Packs the author's official [Tomato-Novel-Downloader](https://github.com/zhongbai2333/Tomato-Novel-Downloader) and our panel into a single compose: deploy with one command, then use the web panel to check status, set the device password and turn on internet access. The Tomato tab on Read Pico, this repository's [Tomato Android app](android/README.md) and the older Tomato-Novel Android app all connect to it. The API is exactly the same as using the official downloader directly, so **no firmware or app changes are needed**.

```
Read Pico / Tomato Android app ──(x-tomato-password)──> tomato-hub :18423 ──> official downloader (internal network, no published port)
                                              │ panel, downloader web UI (LAN only)
                                              └─ cloudflared (optional) ──> Cloudflare ──> devices on the internet
```

- **Deploy from one compose file**: `docker compose up -d`, then open the web panel.
- **Device password managed in the panel**: generate a random one, or set your own (for example, keep the password from your old downloader so devices need no change). Changing the password does not restart the downloader.
- **Internet access**: built-in Cloudflare Tunnel (quick tunnel or tunnel token). The tunnel only lets the device API through; the panel and the downloader web UI always stay on the LAN.
- **Books stored on the host**: `data/downloader/books`, so rebuilding containers loses nothing. On first start the downloader's default format is set to TXT automatically (Tomato books on the device use TXT, and updates only append chapters).
- **The downloader's own web UI** still works (search, download, browse the library, change settings). Click "打开番茄下载器网页" (Open the Tomato downloader web UI) in the panel; you must be signed in to the panel first.

## Before you deploy

| Item | Requirement |
| --- | --- |
| Machine | **x86-64** (amd64) Linux, a NAS, or a computer with Docker Desktop |
| Memory | A few dozen MB, a bit more while downloading |
| Disk | About 200 MB for both images, plus the downloaded books |
| Software | Docker and the compose plugin. Synology Container Manager, QNAP Container Station and Portainer also work |

## Deployment

### 1. Prepare a folder

Create an empty folder (for example `/volume1/docker/tomato` on a NAS) and put this directory's `docker-compose.yml` in it:

```bash
mkdir tomato && cd tomato
curl -fsSLO https://raw.githubusercontent.com/bidabrain/treader-eink/main/tomato/docker-compose.yml
```

If you need to change the port, allow access from this machine only, or switch the downloader version, also download `.env.example`, rename it to `.env` and edit it:

```bash
curl -fsSL -o .env https://raw.githubusercontent.com/bidabrain/treader-eink/main/tomato/.env.example
```

### 2. Start

```bash
docker compose up -d
```

Two images are downloaded: `ghcr.io/bidabrain/treader-tomato` (the panel, built automatically by this repository's GitHub Actions) and the official `zhongbai233/tomato-novel-downloader-webui` (pinned version; change it with `DOWNLOADER_VERSION` in `.env`). All data lives in `data/` in the same folder: `data/hub` holds the panel settings and `data/downloader` holds the downloader's configuration and books.

> If you previously ran the official downloader on its own with `docker run`: stop it first with `docker rm -f` (otherwise port 18423 conflicts). To keep using your existing books, copy the books from the directory it mounted into `data/downloader/books`.
>
> To build the panel from source yourself: clone the repository and run, in the `tomato/` directory,
> `docker compose -f docker-compose.yml -f docker-compose.build.yml up -d --build`.

### 3. Set the panel password

Open `http://server-IP:18423` in a browser and set the panel password on first visit (at least 8 characters). Five wrong attempts in a row lock it for 5 minutes. The overview page shows whether the downloader is online, its version, and how many books are in the library.

### 4. Connect the device

Panel → **设备与外网** (Devices & internet) shows the address and the device password.

- **Device**: Settings → Content sources → Tomato novels → Tomato server (设置 → 内容来源 → 番茄小说 → 番茄服务器). Enter `http://server-LAN-IP:18423` as the address and the device password from the panel as the password.
- **Tomato Android app** ([tomato/android](android/README.md), or the older Tomato-Novel): enter the same address and password in its settings.
- **Device password**: a random 8 characters by default (without the easily confused 0, o, 1 and l, so it is easy to type on the device keyboard). You can also choose your own; set it to your old downloader's password and devices and apps already set up need no change. After changing it, update the devices and apps to the new one.
- **Using it away from home**: choose an internet access method; after saving, the public address is shown:
  - **Quick tunnel**: nothing to prepare. You get a random `https://xxx.trycloudflare.com` that **changes on every restart**, suitable for temporary use.
  - **Tunnel token** (for long-term use; your domain must be hosted on Cloudflare): in [Cloudflare Zero Trust](https://one.dash.cloudflare.com/) → Networks → Tunnels → Create a tunnel, copy only the token starting with `eyJ`. Under "Public hostname", add an entry with service type **HTTP** and URL **`localhost:18423`**. Paste the token into the panel, choose "隧道令牌" (Tunnel token), then save and apply. Use `https://your-subdomain` as the device address.
  - The panel program also checks on its own: requests coming in through the tunnel (with a `CF-Connecting-IP` header) can only reach the device API (`/api/…`, `/download/…`); the panel and the downloader web UI always return 403. **Do not forward port 18423 to the internet on your router**.

## Forwarding rules

| Path | Who can use it | Where it goes |
| --- | --- | --- |
| `/api/search`, `/api/jobs…`, `/api/library`, `/api/updates`, `/api/status`, `/api/preview…`, `/api/history` | With the device password (`x-tomato-password`) | Forwarded unchanged to the downloader |
| `/download/<path>` | With the device password | Forwarded to the downloader, streamed back |
| Other `/api/…` (configuration, self-update, sign-in) | Not exposed | Change these in the downloader web UI |
| `/webui/…` | LAN browsers signed in to the panel | The downloader's own web UI |
| `/` | LAN | Panel |

The downloader runs on the compose internal network, has no password of its own and publishes no port; only the panel program can reach it.

## Maintenance

```bash
docker compose pull && docker compose up -d   # upgrade (change the downloader version in .env)
docker compose logs -f hub                    # panel logs
docker compose logs -f downloader             # downloader logs
docker compose down                           # stop (data stays in data/)
```

- **Forgot the panel password**: delete `data/hub/auth.json`, run `docker restart tomato-hub`, then open the panel and set it again.
- **Download format**: the device needs TXT. If you change it to EPUB in the downloader web UI's configuration, device downloads will get EPUB; change it back to TXT or "每次询问" (Ask every time).

## Layout

| Path | Contents |
| --- | --- |
| `docker-compose.yml` | For deployment: panel (hub) + official downloader |
| `docker-compose.build.yml` | Build the hub from source |
| `.env.example` | Port, downloader version |
| `hub/app/main.py` | Forwarding, device password, panel API, tunnel restrictions |
| `hub/app/tunnel.py` | cloudflared subprocess |
| `hub/app/web/` | Panel pages |
| `android/` | Tomato Android client (same as the firmware Library and Tomato tab), with its own version number and release process |
