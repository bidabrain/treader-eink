[中文](README.md) | **English**

# webtoon: WEBTOON discovery and follow-along server

The Discover page and follow-along server for the Comics tab on Read Pico: browse [WEBTOON](https://www.webtoons.com)'s rankings, genres, weekday schedule, completed series and search on the device, tap "Subscribe" (订阅), then download new episodes with "Check for updates" (检查更新). The server only fetches an episode's original images from webtoons.com when the device asks for that episode; it scales them to the screen width, converts them to grayscale, cuts pages at screen height in the blank space between panels, and packs them into a CBZ for the device.

- **The server keeps no subscriptions**: subscriptions are stored on the device's SD card (one `.webtoon` marker file in each title's folder), and the device asks "do these titles have new episodes?". Changing servers or reinstalling never loses subscriptions.
- **Page cuts suited to e-ink**: tall vertical-scroll comics are cut into pages of 648×1000, with cut points found in the white space between panels so that panels and speech bubbles are rarely split in half; very long gaps are shortened and fully blank pages are dropped. Each page is a plain (baseline) JPEG, so the device can use fast decoding and page turns are as fast as reading a local CBZ.
- **Simplified / Traditional Chinese**: the Chinese edition of WEBTOON uses Traditional Chinese. Simplified keywords typed with the device's pinyin input are converted to Traditional by the server before searching; titles and summaries are shown in Simplified by default (text inside the images is unchanged).
- **Deploy from one compose file**: `docker compose up -d`, then open the web panel. The panel can also browse the Discover page and convert a test episode to download as a CBZ and see the result.
- **Internet access**: built-in Cloudflare Tunnel (quick tunnel or tunnel token). The tunnel only lets the device API through; the panel always stays on the LAN.

```
Read Pico (subscriptions on SD card) ──HTTPS (token)──> webtoon-hub :8095 ──> webtoons.com (public free episodes)
                                                          │ panel (LAN only)
                                                          └─ cloudflared (optional) ──> Cloudflare ──> devices on the internet
```

You can also read on your phone: [webtoon/android](android/README.md) is a standalone Android app with the same features as the firmware comic store, reading from the same server with the same device token.

> **Only public free content, only for yourself.** The server does not sign in to WEBTOON and only reads episodes viewable on the website without signing in; it never touches paid, early-access (Fast Pass) or wait-for-free episodes. WEBTOON titles are copyrighted and its terms of use do not allow scraping: deploy it yourself and use it only for your own offline reading. **Do not open the server to others, and do not share downloaded comics**.

## Deployment

| Item | Requirement |
| --- | --- |
| Machine | x86-64 Linux, a NAS, or a computer with Docker Desktop |
| Network | Must be able to open webtoons.com (not reachable from mainland China) |
| Memory | About 200 MB; a bit more while converting; about 1 GB more with plugin sources on |
| Disk | About 3–6 MB per episode; by default the cache is kept for 14 days with a 3 GB limit (adjustable in the panel) |

```bash
mkdir webtoon && cd webtoon
curl -fLO https://raw.githubusercontent.com/bidabrain/treader-eink/main/webtoon/docker-compose.yml
docker compose up -d
```

Open `http://server-IP:8095` and set the panel password on first visit (at least 8 characters). If the port is taken, write `WEBTOON_PORT=another-port` in `.env`.

### `.env` settings (optional)

`.env` is a text file in the same directory as `docker-compose.yml`, with one `NAME=value` per line. `docker compose` reads it automatically every time it runs, so you never need to edit the compose file. Without this file, all defaults are used.

| Name | Default | Effect |
| --- | --- | --- |
| `WEBTOON_PORT` | `8095` | Host port for the panel and the device API; change it if taken |
| `WEBTOON_BIND` | `0.0.0.0` | Address to listen on; write `127.0.0.1` to allow access from this machine only (for example, behind a reverse proxy) |
| `COMPOSE_PROFILES` | Not set | `mihon` also starts Suwayomi and turns on [plugin sources](#plugin-sources-optional); `mihon,flaresolverr` adds FlareSolverr to get plugins past [Cloudflare verification](#cloudflare-verification); unset means WEBTOON only |
| `MIHON_HEAP` | `768m` | Maximum Java memory for Suwayomi; can be raised to `1g` with many plugins or large images |

For example, to turn on plugins and switch to port 8096:

```
COMPOSE_PROFILES=mihon
WEBTOON_PORT=8096
```

After changing `.env`, run `docker compose up -d` to apply it.

### Upgrading

In the directory containing `docker-compose.yml`, first download the latest compose file (it overwrites the old one; a new version may add services or settings), then pull the new images and recreate the containers:

```bash
curl -fLO https://raw.githubusercontent.com/bidabrain/treader-eink/main/webtoon/docker-compose.yml
docker compose pull && docker compose up -d
```

- `./data` (panel password, device token, tunnel settings, converted cache) and `.env` are kept, so the device does not need to be set up again.
- If you edited the compose file directly (for example the port), those edits are overwritten: put them in `.env` instead.
- After upgrading, the panel shows the new version number in the top-left corner. For a freshly pushed version, wait for the image build on GitHub to finish (a few minutes); a `pull` too early still gets the old version, so run it again a little later.
- The hub disconnects for a few seconds while being recreated; a check for updates running on the device at that moment may fail once, and the next check continues the downloads.

### Connect the device

Panel → **设备与外网** (Devices & internet) shows the address and the token. On the device: Settings → Content sources → Comics (设置 → 内容来源 → 漫画), turn on "Fill in from phone" (手机网页填写), and paste the address and token using your phone.

- **At home**: use `http://server-LAN-IP:8095` as the address.
- **Away from home**: choose an internet access method; after saving, the public address is shown:
  - **Quick tunnel**: nothing to prepare. You get a random `https://xxx.trycloudflare.com` that changes on every restart.
  - **Tunnel token** (for long-term use; your domain hosted on Cloudflare): create a tunnel in [Cloudflare Zero Trust](https://one.dash.cloudflare.com/) and copy the token starting with `eyJ`; for the public hostname, set **path `^/api/device/`**, service **HTTP**, URL **`localhost:8095`**. Paste the token into the panel, choose "隧道令牌" (Tunnel token), then save and apply.
  - The panel program also blocks on its own: requests with `CF-Connecting-IP` can only reach `/api/device/`; everything else returns 403. **Do not forward 8095 to the internet on your router.**

## Settings (panel → 设置 / Settings)

| Item | Default | Notes |
| --- | --- | --- |
| Page width / height | 648 / 1000 | The Read Pico comic area; changes only affect episodes converted afterwards |
| JPEG quality | 85 | About 50–70 KB per page |
| Wait between two original images | 0.4–1.2 seconds | Random, to avoid burdening WEBTOON |
| List and details cache | 30 minutes | Discover page lists, title details |
| Days kept / cache limit | 14 days / 3000 MB | Converted CBZs and covers; beyond that, the least recently used are deleted first |

## Plugin sources (optional)

With this feature on, the same compose starts an extra [Suwayomi-Server](https://github.com/Suwayomi/Suwayomi-Server) to run Mihon / Tachiyomi plugins. The hub reads comics from plugin sources through it and converts them the same way as WEBTOON episodes: grayscale, cut into screen pages, packed as CBZ. Subscribing, checking for updates and downloading on the device all work the same.

> **This project neither provides nor recommends any plugin repository.** You add repository addresses yourself in the panel. Only add sources you have the right to access, such as your own Komga / Kavita library or official free serializations, and follow the source site's terms. The "only for yourself, not open to others, not shared" requirement above applies to all sources too.

**Turning it on**: add one line to the [`.env`](#env-settings-optional) next to `docker-compose.yml`, then run `docker compose up -d` again (if upgrading from an older version, first replace the compose file as described in [Upgrading](#upgrading); the plugin services are defined in it):

```
COMPOSE_PROFILES=mihon
```

- **Memory**: Suwayomi is a Java program and takes about 1 GB more memory. The Java heap limit defaults to 768 MB and can be adjusted with `MIHON_HEAP=1g` in `.env`.
- **First start**: the Suwayomi image (about 2 GB) must be downloaded first; it starts only after the hub is healthy, and it takes another minute or two before the "插件" (Plugins) tab appears in the panel.
- **Installing plugins**: panel → 插件 (Plugins) → add a repository (an `index.min.json` address) → refresh the list → install. Tick sources under "设备上的来源" (Sources on the device) and save; then tap "Refresh" (刷新) once on the Discover page on the device or app, and the source can be selected. To uninstall, click "卸载" (Uninstall); downloaded episodes stay, checking for updates on that source's subscriptions reports an error, and you can unsubscribe in My comics (我的漫画).
- **Security**: Suwayomi publishes no port; only the hub can reach it on the compose internal network. The password both sides use is generated on the hub's first start and stored in `data/mihon/suwayomi.secret`. Suwayomi's own web UI is turned off. Plugin data is stored in the Docker volume `suwayomi`.
- **WebView**: Chromium is not installed in Suwayomi (`KCEF_ENABLED=false`). For sources that need to get past Cloudflare verification, see [Cloudflare verification](#cloudflare-verification) below; sources that require signing in on a web page do not work.
- **Local comics**: remove the `#` from the `./local` line in `docker-compose.yml` to use the Mihon local source to read comics in `./local` (one folder per title, one subfolder or one CBZ per episode).

### Cloudflare verification

Some plugin sites make requests pass a Cloudflare human check first, and are more likely to block you when downloading many episodes in a row. Without preparation, those episodes fail with an error containing `Cloudflare bypass currently disabled` or failing to connect to `flaresolverr`, and the hub appends instructions on how to turn it on. To turn it on, change `.env` to:

```
COMPOSE_PROFILES=mihon,flaresolverr
```

Then run `docker compose up -d` (if upgrading from before 0.2.3, first replace the compose file as described in [Upgrading](#upgrading)). An extra [FlareSolverr](https://github.com/FlareSolverr/FlareSolverr) container starts: it opens the site with headless Chromium, passes the check, and hands the resulting cookie to Suwayomi, taking about 500 MB more memory. It publishes no port, only Suwayomi uses it on the compose internal network, and only when a site requires verification. FlareSolverr does not always succeed (the server IP is blocked, or the site uses a check it cannot solve); the error is then `CloudflareBypassException`, and the hub notes that FlareSolverr did not get through. Once a source fails this way, its remaining queued episodes fail at once for 10 minutes (the error says how many minutes are left) instead of each waiting for FlareSolverr to time out, so other sources and WEBTOON keep downloading; check for updates again after 10 minutes to retry.

Failed episodes do not need to be fixed by hand: on the next "Check for updates", the device (Treader 1.0.7 and later) and the app re-download starting from the first episode that did not finish, skipping those already on the card.

**Panel → 插件 (Plugins)**:

- Add or remove plugin repositories; install, update and uninstall plugins.
- **设备上的来源** (Sources on the device): choose which sources appear on the device and their order. All installed sources are sent to the device by default; the server does not filter by content rating for you. Adult sources are only marked "成人" (Adult) in the panel, and which to use is your own choice on the device or app.
- **Several versions of the same episode** (different scanlation groups): by default only the first one seen is kept.
- **Spreads** (whole landscape images): split into two pages with the right page first by default; can also be changed to left page first, rotated 90°, or scaled down whole.

**Episode numbers**: plugin chapter numbers may be decimals (such as 12.5), missing or duplicated, so the hub assigns its own integer episode numbers from oldest to newest (1, 2, 3…), stored in `data/mihon/chapters.sqlite`. When most chapters have chapter numbers, they are numbered in ascending chapter-number order (some plugins hand them over sorted as chapter-name strings, for example "第100话" (Episode 100) before "第10话" (Episode 10), so that order cannot be used as is); extras without a chapter number follow the chapter before them in the source order; when none have chapter numbers, source order is used. Assigned episode numbers never change. Chapters released later go after the highest episode number, so old chapters the source adds later end up at the end. This table is only a cache; if lost, it is rebuilt by the same rules.

**Renumbering**: if a title's episode numbers are wrong (for example episode 1 is not the earliest chapter), go to panel → 发现 (Discover), choose that source, open the title and click "重新编号" (Renumber): the server forgets its episode numbers, deletes the title's already converted episodes on the server, and renumbers it by the current rules. On devices and apps already subscribed to it, first unsubscribe and delete the episodes downloaded with the old numbers, then subscribe again.

**Page cutting**: the hub first looks at the height-to-width ratio of the first 3 images of each episode. If the median is above 2, it is treated as a vertical-scroll comic and cut in the blank space between panels as with WEBTOON; otherwise it is treated as a regular comic, one image per page, with white borders trimmed and scaled to fit 648×1000. `ComicInfo.xml` records whether it is vertical scroll (`Format=Webtoon`) or right-to-left (`Manga=YesAndRightToLeft`).

**Limitation**: if Suwayomi's data (the Docker volume) is wiped, title IDs are reassigned. The hub then refuses to download old IDs (returns 409, to avoid downloading a different title), and subscribed plugin titles must be subscribed again on the device.

## E-Hentai channel (optional)

The hub reads e-hentai.org itself (and exhentai.org once signed in), without extensions or Suwayomi. It follows [EhViewer](https://github.com/xiaojieonly/Ehviewer_CN_SXJ) and [JHenTai](https://github.com/jiangtian616/JHenTai): list pages only give gallery links; title, category, tags, page count and cover come from the official API; images come from each page's viewer, and a failed image server is retried once on another.

> Adult content. As above: personal use only, not opened or shared, follow the site's terms and do not shorten the request delays.

**Turn it on**: panel → E-Hentai → tick "打开 E-Hentai 通道" → save. Tap refresh once on the device or app store page and E-Hentai appears among the sources (with Popular and Latest).

- **Direct IP**: turn it on when the server cannot reach e-hentai.org (for example the connection is reset). Like JHenTai's domain fronting, it connects to the site's fixed IPs with no domain in the handshake and the real domain only in the request header; certificates are not verified for those IPs. Image servers and covers connect as usual.
- **Account**: sign in with your E-Hentai user name and password under Account. The password only goes to E-Hentai in exchange for sign-in cookies; the server keeps only the cookies (`data/config.json`) and the panel never shows them. Signing in raises the image quota; if the account can enter ExHentai, `igneous` is fetched too and the site can be switched to ExHentai. E-Hentai sometimes asks for a captcha the server cannot fill in: sign in in a browser and paste `ipb_member_id`, `ipb_pass_hash` (and `igneous`) under the manual cookie fields.
- **Default filter**: EH search syntax added to every search and to Latest, such as `language:chinese$` for Chinese only. Popular takes no search, so the server filters it by the `tag:value` words (words starting with `-` are ignored).
- **Categories**: grayed-out categories are hidden (Doujinshi, Manga, Cosplay…).
- **Title**: the Japanese / original title is preferred by default (translated galleries usually carry the Chinese title there); untick for the romanized title.
- **Images per episode**: a gallery is split into episodes of this many images (default 80, named "第 1–80 张"); a short one is a single "全本". A device waits at most 6 minutes per episode and takes CBZs up to 60 MB, so do not set it too high.
- **Spreads**: as with extension sources, the first few images decide between a tall strip and one image per page, and wide spreads follow this setting.

**Title ids**: 15 alphanumerics starting with `me` (`me` plus the gallery gid and token in base 36). They start with `m`, so the firmware and app treat them as extension titles with no update; the token rides in the id, so the server keeps no lookup table.

**Errors**: when the image quota runs out (509), the server's IP is temporarily banned or ExHentai is not signed in, the channel's queued episodes fail at once for 10 minutes instead of each being tried. The quota recovers over time and is larger when signed in.

## Device API

Every request needs the header `X-Webtoon-Token: <令牌>` (the device token). `lang` is the language edition (`zh-hant` Traditional Chinese, `en`, `es`, `fr`, `de`, `th`, `id`), default `zh-hant`; `zh=s` (default) converts Traditional text to Simplified, `zh=t` leaves it as is. Title IDs: official titles are numbers (`8420`), amateur Canvas titles are prefixed with `c` (`c1060813`).

| Endpoint | Returns |
| --- | --- |
| `GET /api/device/v1/sources` | `{mihon, items:[{key,name,lang,latest,nsfw}]}`. The first item is always `webtoon`; with plugins on, it is followed by the plugin sources not hidden in the panel. `key` is the source ID (a string), `latest` means the source has "Latest", and `nsfw` is only a flag (the server does not filter by it) |
| `GET /api/device/v1/store?seg=rank\|genre\|day\|done&key=…&limit=60` | `{seg, key, keys:[{key,name}], items:[{id,title,author,genre,likes,canvas,adult}]}`. For `rank` the key is `trending` / `popular` / `originals`; for `genre` the key comes from `keys`; for `day` the key is `monday`…`sunday` (today when omitted); `done` is completed series |
| `GET /api/device/v1/search?q=…` | `{q, items:[…same as above]}`, official titles first |
| `GET /api/device/v1/title/{id}` | `{id,title,author,genre,summary,done,latest,episodes:[{no,title,date}]}`; `episodes` is all free episodes, oldest to newest |
| `GET /api/device/v1/cover/{id}.jpg` | 150×200 grayscale cover (baseline JPEG) |
| `POST /api/device/v1/check` | Request `{lang, zh, titles:[{id,last}]}` (`last` is the episode downloaded up to), returns `{titles:[{id,latest,new:[{no,title}],error?}]}`. The server records none of this |
| `GET /api/device/v1/episode/{id}/{no}` | `{state:"ready",size}` / `{state:"pending",done,total}` (original image progress) / `{state:"error",error}`; starts converting if not converted yet. The device asks again every few seconds |
| `GET /api/device/v1/episode/{id}/{no}.cbz` | The converted CBZ: grayscale pages numbered in order from `0001.jpg` (uncompressed ZIP) plus `ComicInfo.xml` |

Plugin sources: add `src=<来源 key>` (the source key) to `store` and `search` (defaults to `webtoon` when omitted). In that case `seg` can only be `pop` (Popular) or `latest` (Latest), and `keys` is empty; each item has an extra `src` field (the source name), and `likes` is 0. Plugin title IDs are `m` followed by a number (such as `m12`). `title`, `cover`, `check` and `episode` all dispatch automatically by ID prefix and ignore `lang` and `zh`, so a single `check` can include both WEBTOON and plugin titles. With the E-Hentai channel on, `sources` has an `ehentai` item after `webtoon` (`latest` and `nsfw` true); `src=ehentai` works like an extension source, and its title IDs start with `me`. A plugin title's `title` has two extra fields: `src` (the source name) and `error` (the reason when a plugin refresh failed but older data exists).

Errors return `{"detail": "原因"}` (the reason); 401 means the token is wrong.

## Files on the device (firmware and Android app convention)

```
comics/
└── <作品名>/                  # one folder per series (My comics only looks one level deep)
    ├── .webtoon               # subscription marker (JSON), ignored by My comics
    ├── 第0001话 标题.cbz
    └── 第0002话 标题.cbz      # episode number zero-padded to 4 digits, so natural file name order is episode order
```

Here `<作品名>` is the title name and `标题` is the episode title.

Contents of `.webtoon`:

```json
{"id": "8420", "lang": "zh-hant", "title": "我的虎系青梅竹马", "author": "GIMAN、ETER", "last": 66, "added": 1795275780}
```

`last` is "downloaded up to episode N": it is sent to the server when checking for updates, and only advances to just before the first episode that did not finish downloading, so a failed episode is asked about again on the next check. Renaming the folder or copying it whole to another card does not affect the subscription. Unsubscribing just deletes `.webtoon` (downloaded episodes stay and become ordinary local comics).

## Tests

```bash
docker build -t treader-webtoon:dev webtoon/hub
docker run --rm -v "$PWD/webtoon/hub:/srv" -w /srv treader-webtoon:dev sh -c "pip -q install pytest && python -m pytest -q tests"
```

Offline tests, no network: parsing (synthetic web page fragments), page cutting (synthetic long strips, where cut points must land in blank space; regular comics one image per page, spread splitting and rotation, telling vertical scroll from single pages), packing, the device API (a stand-in data source, including plugin sources and checking for updates with both kinds of titles mixed), the plugin episode number table (first numbering, appending new chapters, back-filled chapters, duplicate versions, reversed source order), the E-Hentai channel (ids, list / gallery / viewer page parsing, splitting by image count while fetching only the needed gallery pages, switching image servers, pausing after 509, direct-IP failover, sign-in) and cache cleanup.

## Layout

| File | Role |
| --- | --- |
| `hub/app/source.py` | Reads webtoons.com: Discover lists, search, details, free episodes, original image URLs for each episode; a website redesign only needs changes here |
| `hub/app/convert.py` | Original images → e-ink pages: scaling, grayscale, cutting in blank space (vertical scroll) or one image per page (regular comics), CBZ packing, covers |
| `hub/app/mihon.py` | Plugin sources: Suwayomi GraphQL calls, the episode number table, plugin and repository management |
| `hub/app/ehentai.py` | E-Hentai channel: lists, the official API, splitting galleries into episodes, image fetching, direct IP, sign-in |
| `hub/app/jobs.py` | Conversion queue (one episode at a time) and cache cleanup |
| `hub/app/main.py` | Panel and device API |
| `hub/app/tunnel.py`, `auth.py` | Cloudflare tunnel, panel password and device token (same as rednote) |
| `hub/app/web/` | Panel web pages |
