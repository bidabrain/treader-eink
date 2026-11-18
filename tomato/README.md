# tomato：番茄小说下载服务器（带面板）

把作者官方的 [Tomato-Novel-Downloader](https://github.com/zhongbai2333/Tomato-Novel-Downloader) 和我们的面板打包成一个 compose：一条命令部署，在网页面板里看状态、设设备密码、开外网访问。小纸 Pico 的番茄标签和番茄安卓 App 都连它，接口和直接用官方下载器完全一样，**固件和 App 不用改**。

```
小纸 Pico / 番茄安卓 App ──(x-tomato-password)──> tomato-hub :18423 ──> 官方下载器（内网，不对外开端口）
                                          │ 面板、下载器网页（只在局域网）
                                          └─ cloudflared（可选）──> Cloudflare ──> 外网的设备
```

- **一个 compose 文件部署**：`docker compose up -d`，然后打开网页面板。
- **设备密码在面板里管**：随机生成，或者设成你自己的（比如沿用以前下载器的密码，设备就不用改）；换密码不用重启下载器。
- **外网访问**：内置 Cloudflare Tunnel（快速隧道或隧道令牌），隧道只放行设备接口；面板和下载器网页始终只在局域网。
- **书存在宿主机**：`data/downloader/books`，重建容器不会丢。第一次启动时自动把下载器的默认格式设成 TXT（设备上番茄书用 TXT，更新只追加章节）。
- **下载器自己的网页**照常能用（搜书、下载、看书库、改设置），在面板里点“打开番茄下载器网页”，要先登录面板。

## 部署前准备

| 项目 | 要求 |
| --- | --- |
| 机器 | **x86-64**（amd64）的 Linux、NAS 或装了 Docker Desktop 的电脑 |
| 内存 | 几十 MB，下载时稍多 |
| 磁盘 | 两个镜像合计约 200 MB，加上下载的书 |
| 软件 | Docker 和 compose 插件。群晖 Container Manager、威联通 Container Station、Portainer 也可以 |

## 部署步骤

### 1. 准备文件夹

新建一个空文件夹（例如 NAS 上的 `/volume1/docker/tomato`），放入本目录的 `docker-compose.yml`：

```bash
mkdir tomato && cd tomato
curl -fsSLO https://raw.githubusercontent.com/bidabrain/treader-eink/main/tomato/docker-compose.yml
```

需要改端口、只允许本机访问、或者换下载器版本时，再下载 `.env.example` 改名为 `.env` 修改：

```bash
curl -fsSL -o .env https://raw.githubusercontent.com/bidabrain/treader-eink/main/tomato/.env.example
```

### 2. 启动

```bash
docker compose up -d
```

会下载两个镜像：`ghcr.io/bidabrain/treader-tomato`（面板，由本仓库的 GitHub Actions 自动构建）和官方的 `zhongbai233/tomato-novel-downloader-webui`（固定版本，在 `.env` 的 `DOWNLOADER_VERSION` 改）。所有数据在同一文件夹的 `data/` 里：`data/hub` 是面板设置，`data/downloader` 是下载器的配置和书。

> 以前用 `docker run` 单独跑过官方下载器的：先 `docker rm -f` 停掉它（不然 18423 端口冲突）。原来的书要接着用，把它挂载的目录里的书拷到 `data/downloader/books`。
>
> 想自己从源码构建面板：克隆仓库后在 `tomato/` 目录执行
> `docker compose -f docker-compose.yml -f docker-compose.build.yml up -d --build`。

### 3. 设置面板密码

浏览器打开 `http://服务器IP:18423`，第一次进入时设置面板密码（至少 8 位）。连续输错 5 次会锁定 5 分钟。概览页显示下载器在不在线、版本、书库里有几本书。

### 4. 让设备连上来

面板 → **设备与外网** 里有地址和设备密码。

- **设备**：设置 → 内容来源 → 番茄小说 → 番茄服务器，地址填 `http://服务器局域网IP:18423`，密码填面板上的设备密码。
- **番茄安卓 App**：设置里填同样的地址和密码。
- **设备密码**：默认随机 8 位（去掉了容易看错的 0、o、1、l，方便在设备键盘上输入）。也可以自己定一个；设成以前下载器的密码，已经填好的设备和 App 就不用改。换了以后设备和 App 要改成新的。
- **在外面用**：选一种外网方式，保存后显示外网地址：
  - **快速隧道**：什么都不用准备，得到一个随机的 `https://xxx.trycloudflare.com`，**每次重启都会变**，适合临时用。
  - **隧道令牌**（长期用，域名要托管在 Cloudflare）：在 [Cloudflare Zero Trust](https://one.dash.cloudflare.com/) → 网络 → 隧道 → 创建隧道，只复制 `eyJ` 开头的令牌；在“公共主机名”里加一条，服务类型 **HTTP**，URL **`localhost:18423`**。把令牌粘贴到面板，选“隧道令牌”，保存并应用。设备地址填 `https://你的子域名`。
  - 面板程序自己会检查：从隧道进来的请求（带 `CF-Connecting-IP` 头）只能访问设备接口（`/api/…`、`/download/…`），面板和下载器网页一律 403。**不要在路由器上把 18423 端口映射到公网**。

## 转发规则

| 路径 | 谁能用 | 去哪 |
| --- | --- | --- |
| `/api/search`、`/api/jobs…`、`/api/library`、`/api/updates`、`/api/status`、`/api/preview…`、`/api/history` | 带设备密码（`x-tomato-password`） | 原样转给下载器 |
| `/download/<路径>` | 带设备密码 | 转给下载器，流式传回 |
| 其他 `/api/…`（配置、自更新、登录） | 不开放 | 在下载器网页里改 |
| `/webui/…` | 登录面板的局域网浏览器 | 下载器自己的网页 |
| `/` | 局域网 | 面板 |

下载器跑在 compose 的内网里，不设自己的密码，也不对外开端口；能碰到它的只有面板程序。

## 日常维护

```bash
docker compose pull && docker compose up -d   # 升级（下载器版本在 .env 里改）
docker compose logs -f hub                    # 看面板日志
docker compose logs -f downloader             # 看下载器日志
docker compose down                           # 停止（数据保留在 data/）
```

- **忘记面板密码**：删除 `data/hub/auth.json`，`docker restart tomato-hub`，再打开面板重新设置。
- **下载格式**：设备要 TXT。在下载器网页的配置里改成 EPUB 的话，设备下载会拿到 EPUB，请改回 TXT 或“每次询问”。

## 目录

| 路径 | 内容 |
| --- | --- |
| `docker-compose.yml` | 部署用：面板（hub）+ 官方下载器 |
| `docker-compose.build.yml` | 从源码构建 hub |
| `.env.example` | 端口、下载器版本 |
| `hub/app/main.py` | 转发、设备密码、面板接口、隧道限制 |
| `hub/app/tunnel.py` | cloudflared 子进程 |
| `hub/app/web/` | 面板页面 |
