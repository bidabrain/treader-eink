# rednote：小红书内容服务器

在自己的服务器上登录小红书网页版，定时拉取你关注的博主的**图文笔记**，把图片处理成墨水屏能直接显示的灰度图，再让小纸 Pico 来取。

- **一个 compose 文件部署**：`docker compose up -d`，然后打开网页面板完成所有设置。
- **网页面板**：扫码登录小红书、添加博主、查看拉到的内容、设置拉取频率、生成设备令牌、开外网访问。
- **外网访问**：内置 Cloudflare Tunnel（快速隧道或隧道令牌），设备在外面也能取；面板始终只在局域网。
- **省设备的事**：视频笔记直接跳过；图片在服务器上转灰度、缩到不超过 648×1000、太长的按屏切开，存成普通 JPEG（每张约 30～70 KB），另附一张宽 300、保留原图比例的封面。
- **只读**：只拉内容，不发帖、不点赞、不评论。

取数据用的是开源项目 [xiaohongshu-mcp](https://github.com/xpzouying/xiaohongshu-mcp)（Apache-2.0）：它在容器里用真实浏览器登录和浏览，不逆向签名。本目录只是在它外面加了一层面板、定时拉取和图片处理。

> **设备端**：小纸 Pico 的 Treader 固件里有“小红书”标签（0.5.0 起）。在设备的“设置 → 内容来源 → 小红书”打开“手机网页填写”，用手机把下面第 6 步里的地址和令牌粘贴进去即可。

手机上也能看：[rednote/android](android/README.md) 是和固件小红书标签功能一样的独立安卓 App，读同一台服务器、用同一个设备令牌。

```
小纸 Pico ──HTTPS(令牌)──> rednote-hub :8091 ──> rednote-mcp（浏览器登录小红书）──> 小红书
                              │ 面板（只在局域网）
                              └─ cloudflared（可选）──> Cloudflare ──> 外网的设备
```

## 部署前准备

| 项目 | 要求 |
| --- | --- |
| 机器 | **x86-64**（amd64）的 Linux、NAS 或装了 Docker Desktop 的电脑。xiaohongshu-mcp 官方镜像没有 ARM 版，树莓派和 ARM 群晖用不了 |
| 位置 | **建议在国内**。出口 IP 在海外时，小红书会跳到海外版 rednote：搜索用不了、推荐是海外内容、第一次登录要在“浏览器桌面”里手动点“登录” |
| 内存 | 至少 2 GB 空闲。浏览器空闲时约 130 MB，拉取时 0.5～1 GB |
| 磁盘 | 镜像约 2.5 GB，加上缓存（默认上限 2 GB，可调） |
| 账号 | 一个小红书账号，**建议用实名过的小号**（新号不实名会被要求认证） |
| 软件 | Docker 和 compose 插件。群晖 Container Manager、威联通 Container Station、Portainer 也可以 |

## 部署步骤

### 1. 准备文件夹

新建一个空文件夹（例如 NAS 上的 `/volume1/docker/rednote`），放入本目录的 `docker-compose.yml`：

```bash
mkdir rednote && cd rednote
curl -fsSLO https://raw.githubusercontent.com/bidabrain/treader-eink/main/rednote/docker-compose.yml
```

需要改端口、只允许本机访问、或者配代理时，再下载 `.env.example` 改名为 `.env` 修改（说明写在文件里）：

```bash
curl -fsSL -o .env https://raw.githubusercontent.com/bidabrain/treader-eink/main/rednote/.env.example
```

### 2. 启动

```bash
docker compose up -d
```

第一次会下载两个镜像（合计约 2.5 GB）：`ghcr.io/bidabrain/treader-rednote`（面板，由本仓库的 GitHub Actions 自动构建）和 `xpzouying/xiaohongshu-mcp`（取数据）。所有数据都在同一文件夹的 `data/` 里。

> 想自己从源码构建面板：克隆仓库后在 `rednote/` 目录执行
> `docker compose -f docker-compose.yml -f docker-compose.build.yml up -d --build`。

### 3. 设置面板密码

浏览器打开 `http://服务器IP:8091`，第一次进入时设置面板密码（至少 8 位）。连续输错 5 次会锁定 5 分钟。

### 4. 登录小红书

面板 → **小红书账号** → **获取登录二维码**，用小红书 App 扫码确认，稍等几秒后点 **查询登录状态**，显示“已登录”就好了。

- 二维码半分钟还没出来：点 **打开浏览器桌面**，在里面的小红书页面点“登录”，二维码就会出现在面板上（服务器出口在海外时常见）。
- **登录后不要在其他电脑的浏览器里登录这个号**，否则服务器会被踢下线；手机 App 照常用。
- 登录状态保存在 `data/mcp/cookies.json`，重启容器不用重新扫。过期了就再扫一次。

### 5. 添加博主

面板 → **博主**，两种方式：

- **粘贴主页链接**：在小红书网页版打开博主主页，复制地址栏里的完整链接（要带 `xsec_token=`），粘贴后点“添加”。
- **从首页推荐里挑**：列出推荐里的作者，点“关注”。这会请求一次小红书。

添加后点概览页的 **立即拉一轮**，或者等定时任务。拉到的笔记在 **内容** 页能看到缩略图。

### 6. 让设备连上来

面板 → **设备与外网** 里有地址和令牌。

- **在家里**：地址填 `http://服务器局域网IP:8091`。
- **在外面**：选一种外网方式，保存后显示外网地址：
  - **快速隧道**：什么都不用准备，得到一个随机的 `https://xxx.trycloudflare.com`，**每次重启都会变**，适合临时用。
  - **隧道令牌**（长期用，域名要托管在 Cloudflare）：
    1. [Cloudflare Zero Trust](https://one.dash.cloudflare.com/) → 网络 → 隧道 → 创建隧道，类型选 Cloudflared。页面给出的安装命令不用执行，只复制其中 `eyJ` 开头的令牌。
    2. 在隧道的“已发布的应用程序路由 / 公共主机名”里加一条：子域随意（如 `pico`），域选你的域名，**路径填 `^/api/device/`**，服务类型 **HTTP**，URL **`localhost:8091`**（是 localhost，因为 cloudflared 跑在面板容器里）。路径填了以后，面板相关的请求在 Cloudflare 就被挡掉，到不了你的服务器。
    3. 不要在 Access → 应用程序里给它加登录验证，设备过不了那个登录页。
    4. 把令牌粘贴到面板，选“隧道令牌”，保存并应用。设备外网地址就是 `https://pico.你的域名`。
- 面板程序（hub）自己也会检查：从隧道进来的请求（带 `CF-Connecting-IP` 头）只能访问 `/api/device/`，其余一律 403，浏览器桌面也连不上。所以快速隧道（不能填路径）同样安全。**不要在路由器上把 8091 端口映射到公网**。

## 拉取规则（防风控）

默认值都可以在面板 → **设置** 里改：

| 设置 | 默认 | 说明 |
| --- | --- | --- |
| 每轮间隔 | 120 分钟 | 最少 30 分钟 |
| 请求间隔 | 15～30 秒随机 | 最少 5 秒。每次读博主主页、每篇笔记详情都算一次请求 |
| 每个博主每轮新拉 | 5 篇 | 只拉没拉过的图文；视频在列表阶段就跳过 |
| 每天请求上限 | 200 次 | 到上限后本轮停止，第二天继续 |
| 每篇图片 | 18 张 | 太长的图切屏后会多出几页 |
| 缓存 | 30 天 / 2000 MB | 超过就删最旧的 |

### 推荐（设备按需拉取）

除了关注的博主，设备还可以看这个账号的首页推荐。服务器**不会定时刷首页**：只有设备上点“刷新推荐”时才读一次首页（视频直接过滤掉），点开哪篇才拉哪篇的详情和图片，和人拿手机刷的节奏差不多，推荐也会按你真正点开的内容调整。等待时间：刷新约 5～15 秒；点开一篇约 6～12 秒，正文拉到就先显示，图片在后台同时处理 4 张，翻到还没做好的那页会稍等一下。

| 设置 | 默认 | 说明 |
| --- | --- | --- |
| 允许设备拉推荐 | 开 | 关掉后设备的推荐接口返回 403 |
| 一次列表最多 | 12 篇图文 | 最多 30 |
| 列表缓存 | 10 分钟 | 期间再刷新直接给上次的列表，防止连点；最少 5 分钟 |
| 推荐每天上限 | 60 次 | 刷新和点开都算，同时计入上面的每日总上限 |
| 两次推荐请求间隔 | 3 秒 + 0～3 秒随机 | 和定时拉取共用一个浏览器，会排队；最少 3 秒 |

小红书要求验证（滑块）或登录失效时，拉取会**自动暂停**，概览页写着原因。去 **浏览器桌面** 处理好（或重新扫码）后，点“立即拉一轮”继续。

## 设备接口

请求头带 `x-rednote-token: 设备令牌`（也可以用 `?token=`）。

| 接口 | 返回 |
| --- | --- |
| `GET /api/device/v1/feed?since=时间戳&limit=50` | 关注博主的笔记：`{now, notes:[{id, title, author, time, images, likes, comments, fetched_at, source, ratio}]}`，按拉取时间倒序；`since` 传上次返回的 `now` 就只取新的 |
| `GET /api/device/v1/rec?refresh=1` | 推荐列表 `{fetched_at, refresh_after, left_today, notes:[{id, title, author, likes, ratio, thumb, ready}]}`。`refresh=1` 且过了缓存期才去读首页，否则返回上次的；`ready` 表示已经拉过详情 |
| `GET /api/device/v1/rec/cover/{id}.jpg` | 推荐列表里的封面，规格和下面的 `thumb.jpg` 一样 |
| `GET /api/device/v1/rec/note/{id}` | 点开推荐里的一篇：没拉过就现拉（视频返回 415），返回和 `note/{id}` 一样；之后图片用下面的 `img` 接口。次数用完 429，暂停中 503，不在近期列表里 404 |
| `GET /api/device/v1/note/{id}` | 上面的字段加 `body`（正文）和 `comment_list`（前 20 条：`user`、`text`、`likes`） |
| `GET /api/device/v1/img/{id}/{n}.jpg` | 第 n 页图（从 1 开始），灰度基线 JPEG，不超过 648×1000 |
| `GET /api/device/v1/img/{id}/thumb.jpg` | 封面：宽 300，保留原图比例（高宽比夹在 3:4～4:3，即接口里的 `ratio`） |

## 日常维护

```bash
docker compose pull && docker compose up -d   # 升级
docker compose logs -f hub                    # 看面板日志
docker compose down                           # 停止（数据保留在 data/）
```

- **备份 / 迁移**：停掉后拷贝整个文件夹（重点是 `data/`）。
- **忘记面板密码**：删除 `data/auth.json`，`docker restart rednote-hub`，再打开面板重新设置。
- **xiaohongshu-mcp 版本**：compose 里固定为 `v2.7.4`，在 `.env` 的 `MCP_VERSION` 改。新版本先在面板里试一轮再长期用。
- **不要同时跑两套**（比如测试机和 NAS）用同一个小红书号。

## 安全与合规

- 小红书登录状态只在 `data/mcp/`，不会发到设备；设备只拿得到处理好的图文。
- 面板密码用 PBKDF2 存哈希；设备令牌可以随时在面板里换。
- 浏览器桌面（noVNC）只能在登录面板后打开，不经过隧道。
- 自动抓取不符合小红书用户协议，账号有被风控的风险。请只用于个人阅读，控制频率，不要公开部署给别人用。

## 目录

| 路径 | 内容 |
| --- | --- |
| `docker-compose.yml` | 部署用：面板（hub）+ xiaohongshu-mcp |
| `docker-compose.build.yml` | 从源码构建 hub |
| `.env.example` | 端口、mcp 版本、代理 |
| `hub/app/main.py` | 路由：面板、设备接口、隧道限制、浏览器桌面转发 |
| `hub/app/fetcher.py` | 定时拉取和清理 |
| `hub/app/rec.py` | 推荐：设备按需刷新首页、点开现拉 |
| `hub/app/images.py` | 图片转灰度、缩放、切屏、缩略图 |
| `hub/app/mcp.py` | xiaohongshu-mcp 接口客户端 |
| `hub/app/tunnel.py` | cloudflared 子进程 |
| `hub/app/web/` | 面板页面 |
| `android/` | 安卓客户端（和固件小红书标签功能一样），有自己的版本号和发布流程 |
