**中文** | [English](README.en.md)

# weibo：微博内容服务器

在自己的服务器上**不登录微博**，定时拉取你关注的人和热门的**图文微博**，把图片处理成墨水屏能直接显示的灰度图，再让小纸 Pico 来取。

- **两个分类**：关注（所有关注的人合成一条时间线）和热门。
- **一个 compose 文件部署**：`docker compose up -d`，然后打开网页面板完成所有设置。
- **不用登录**：服务器用无头浏览器生成微博访客身份；关注名单在面板里管理（按 UID、主页链接或昵称搜索添加），逻辑和 [weiboX-server](https://github.com/bidabrain/weiboX-server) 的关注管理一样。也可以填一份登录过的 Cookie，会更稳。
- **WebDAV 备份**：和 weiboX 安卓版、weiboX-server 用同一个 `weibox_backup.json`（关注名单、特别关注、Cookie），三边可以互相恢复。
- **省设备的事**：带视频的微博直接不收；长微博在服务器上补全文；图片（含转发原微博的图）在服务器上转灰度、缩到不超过 648×1000、太长的按屏切开，存成普通 JPEG，另附一张宽 300、保留原图比例的封面。
- **只读**：只拉内容，不发帖、不点赞、不评论，也没有推送。

```
小纸 Pico ──HTTPS(令牌)──> weibo-hub :8092 ──(访客身份 / Cookie)──> m.weibo.cn
                              │ 面板（只在局域网）
                              └─ cloudflared（可选）──> Cloudflare ──> 外网的设备
```

## 部署前准备

| 项目 | 要求 |
| --- | --- |
| 机器 | **x86-64**（amd64）的 Linux、NAS 或装了 Docker Desktop 的电脑 |
| 内存 | 至少 1 GB 空闲。浏览器只在生成访客身份和处理验证时启动 |
| 磁盘 | 镜像约 2 GB（带 Chromium），加上缓存（默认上限 2 GB，可调） |
| 软件 | Docker 和 compose 插件。群晖 Container Manager、威联通 Container Station、Portainer 也可以 |

## 部署步骤

### 1. 准备文件夹

新建一个空文件夹（例如 NAS 上的 `/volume1/docker/weibo`），放入本目录的 `docker-compose.yml`：

```bash
mkdir weibo && cd weibo
curl -fsSLO https://raw.githubusercontent.com/bidabrain/treader-eink/main/weibo/docker-compose.yml
```

需要改端口或只允许本机访问时，再下载 `.env.example` 改名为 `.env` 修改：

```bash
curl -fsSL -o .env https://raw.githubusercontent.com/bidabrain/treader-eink/main/weibo/.env.example
```

### 2. 启动

```bash
docker compose up -d
```

镜像是 `ghcr.io/bidabrain/treader-weibo`，由本仓库的 GitHub Actions 自动构建。所有数据都在同一文件夹的 `data/` 里。

> 想自己从源码构建：克隆仓库后在 `weibo/` 目录执行
> `docker compose -f docker-compose.yml -f docker-compose.build.yml up -d --build`。

### 3. 设置面板密码

浏览器打开 `http://服务器IP:8092`，第一次进入时设置面板密码（至少 8 位）。连续输错 5 次会锁定 5 分钟。

### 4. 添加关注

面板 → **关注**，三种方式：

- **UID 或主页链接**：`1234567890`、`https://weibo.com/u/1234567890`、`https://m.weibo.cn/u/1234567890` 都行。
- **按昵称搜索**：搜到后点“关注”。
- **从 WebDAV 恢复**：在 **设置 → WebDAV 备份** 填好地址，点“从备份恢复”，直接用 weiboX 里的关注名单。

添加后点概览页的 **立即拉一轮**，或者等定时任务。拉到的微博在 **内容** 页能看到。

### 5. 让设备连上来

面板 → **设备与外网** 里有地址和令牌。外网访问和 rednote 一样（快速隧道或隧道令牌，令牌方式的路由**路径填 `^/api/device/`**，URL 填 **`localhost:8092`**），详见 [rednote/README.md](../rednote/README.md#6-让设备连上来)。从隧道进来的请求只能访问 `/api/device/`，面板和验证页一律 403。**不要在路由器上把 8092 端口映射到公网**。

## 拉取规则（防风控）

调度照 weiboX-server：每轮按“该更新了”的程度排序（距上次读的时间 ÷ 这个人平均多久发一条），最该看的人先读。默认值都可以在面板 → **设置** 里改：

| 设置 | 默认 | 说明 |
| --- | --- | --- |
| 每轮间隔 | 30 分钟 | 最少 30 分钟 |
| 两个人之间 | 30～60 秒随机 | 最少 5 秒 |
| 每人每轮读最近 | 20 条 | 一次请求；已有的只更新转评赞 |
| 同一人至少隔 | 5 分钟 | |
| 热门 | 开，30 条 | 每轮最后读一次 |
| 长微博全文 | 每天 150 次，间隔 3 秒 | 用完就只存截断的正文 |
| 每条图片 | 18 张 | 太长的图切屏后会多出几页 |
| 缓存 | 30 天 / 2000 MB | 超过就删最旧的；热门只留最近若干条 |

微博要求验证时拉取会**自动暂停**，概览页写着原因。到 **验证** 页直接在服务器浏览器的画面上点击、拖动滑块，做完点“完成”。访客身份失效会自动重新生成。

评论不定时拉：设备第一次打开某条的评论时才去取一次（前 20 条），之后用缓存。

## 设备接口

请求头带 `x-weibo-token: 设备令牌`（也可以用 `?token=`）。

| 接口 | 返回 |
| --- | --- |
| `GET /api/device/v1/feed?seg=follow&since=时间戳&limit=100` | `seg` 是 `follow`（按发布时间倒序）或 `hot`（按上榜时间、名次）。`limit` 最多 300。`{now, posts:[{id, user, time, source, text, more, images, pics, ratio, likes, comments, reposts, rt_user, rt_text, fetched_at}]}`；`text` 截到 140 字，`more` 表示还有全文或转发内容；`images` 是大图页数，`pics` 是有方形小图的原图张数（0.1.3 之前拉的为 0） |
| `GET /api/device/v1/post/{id}` | 上面的字段，`text` 和 `rt_text` 是全文，另加 `starts`（每张原图的第一页，从 0 起） |
| `GET /api/device/v1/post/{id}/comments` | `{comments:[{user, text, likes}]}`，第一次请求时现取 |
| `GET /api/device/v1/img/{id}/{n}.jpg` | 第 n 页图（从 1 开始），灰度基线 JPEG，不超过 648×1000 |
| `GET /api/device/v1/img/{id}/s{k}.jpg` | 第 k 张原图（从 1 开始）的方形小图，240×240 灰度，列表和宫格用 |
| `GET /api/device/v1/img/{id}/thumb.jpg` | 封面：宽 300，保留原图比例（高宽比夹在 3:4～4:3，即 `ratio`） |
| `POST /api/device/v1/imgs` | 一次取多张列表小图（设备用，0.1.6 起）：请求体 `{"paths": [...]}`，每项是相对 `/api/device/v1/` 的图片地址（`img/{id}/thumb.jpg`、`img/{id}/s{k}.jpg`），最多 40 项；回 `application/octet-stream`，按请求顺序每张是 4 字节大端长度加 JPEG，服务器上没有的长度为 0。不等还在做的图 |

## 日常维护

```bash
docker compose pull && docker compose up -d   # 升级
docker compose logs -f hub                    # 看日志
docker compose down                           # 停止（数据保留在 data/）
```

- **忘记面板密码**：删除 `data/auth.json`，`docker restart weibo-hub`，再打开面板重新设置。
- **不要和 weiboX-server 在同一个出口 IP 上用很密的频率同时跑**，两边的请求会叠加。

## 安全与合规

- Cookie 和 WebDAV 密码只存在 `data/config.json`，面板不回显，不会发到设备。
- 面板密码用 PBKDF2 存哈希；设备令牌可以随时在面板里换。
- 自动抓取不符合微博用户协议。请只用于个人阅读，控制频率，不要公开部署给别人用。

## 目录

| 路径 | 内容 |
| --- | --- |
| `docker-compose.yml` | 部署用 |
| `docker-compose.build.yml` | 从源码构建 |
| `.env.example` | 端口 |
| `hub/app/main.py` | 路由：面板、设备接口、验证页、隧道限制 |
| `hub/app/fetcher.py` | 定时拉取和清理 |
| `hub/app/images.py` | 图片转灰度、缩放、切屏、缩略图 |
| `hub/app/webdav.py` | 和 weiboX 兼容的 WebDAV 备份 |
| `hub/app/weibo/` | 微博接口、解析、访客身份、验证码（移植自 weiboX-server） |
| `hub/app/tunnel.py` | cloudflared 子进程 |
| `hub/app/web/` | 面板页面 |
