**中文** | [English](README.en.md)

# x：X（Twitter）内容服务器

服务器上跑一个有界面的 Chromium，你在面板里通过 VNC 手动登录自己的 X 账号。之后它按随机间隔打开 x.com，拉取“为你推荐”和“正在关注”，去掉广告，把图片转成墨水屏用的灰度图存下来，给小纸 Pico 和安卓 App 离线看。

> 目前只有服务器（面板）。设备标签和安卓 App 还没做。

## 先知道的风险

这是用你自己的账号做的自动抓取，不符合 X 的服务条款。X 可能要求验证账号，严重时会锁号。下面这些做法能降低风险：

- 抓取全由 X 自己的网页完成：浏览器正常打开首页、切换标签、往下滚动，hub 只是旁听网页发出的请求。hub 不直接调用接口，也不伪造请求头。
- 只读：不点赞、不转推、不关注。
- 自动拉取的间隔在最小值和最大值之间随机取，最小间隔不能低于 30 分钟。睡眠时段里完全不拉，醒来后的第一次也随机推迟，不会准点拉。
- 遇到登录失效、账号验证或 429 限流，自动拉取立刻停下，不会自动重试。
- 在服务器上登录，登录 IP 就是抓取用的 IP。最好把服务器放在家里，走你平时上网的出口。

## 部署

```bash
mkdir x && cd x
curl -O https://raw.githubusercontent.com/bidabrain/treader-eink/main/x/docker-compose.yml
docker compose up -d
```

镜像还没发布时，从源码构建：

```bash
docker compose -f docker-compose.yml -f docker-compose.build.yml up -d --build
```

然后打开 `http://服务器IP:8093`，第一次进入时设置面板密码。

想改端口或时区，把 `.env.example` 复制成 `.env` 再改。不建 `.env` 就用默认值：端口 8093，时区 Asia/Shanghai。

## 用法

1. **登录 X**：在面板的“登录 X”页点“打开登录浏览器”，页面里会出现服务器上那个浏览器的画面，直接在里面登录。两步验证、验证码也都在画面里完成。
   - 登录用的是一个完全普通的 Google Chrome（和拉取用同一份资料），登录期间没有任何自动化控制。
   - 看到首页后点“登录好了”，服务器会保存登录并关掉画面。
   - 以后登录失效，或 X 要求验证账号时，概览页会提示，回到这一页再登录一次就行。
   - 画面用不了时，可以在同一页用“导入 cookie”作为备用办法。
2. **代理**：服务器访问不了 X 时，在“设置”里填代理。浏览器和图片下载都会走这个代理。
   - 在 Docker Desktop 上用本机代理，写 `http://host.docker.internal:端口`。
3. **调度**：在“设置”里配置。
   - 为你推荐和正在关注各有一组最小、最大间隔。
   - 每次往下滚动几次也是随机的。
   - 可以设一个睡眠时段，支持跨零点。
   - 正在关注有一个总开关。关掉后这条流手动、自动都不再拉取；设备和 App 会读到这个状态，把“正在关注”标签变灰。
4. **手动拉取**：概览页的“立即拉取”按钮不受间隔和睡眠时段限制。拉完后，这条流的自动计时器重新开始计算。
5. **429 之后**：自动拉取会停住。过一段时间，确认在手机上用 X 一切正常，再点“恢复自动拉取”。手动拉取成功一次也会自动恢复。

## 数据

所有状态都在 `./data` 里：

| 内容 | 位置 |
| --- | --- |
| 浏览器资料（含登录状态） | `browser/` |
| 登录 cookie 的备份 | `x_cookies.json` |
| 推文数据库 | `x.db` |
| 图片 | `posts/` |
| 设置 | `config.json` |
| 面板密码哈希 | `auth.json` |

**这个目录等于你的 X 登录凭据，不要分享。**

## 安全

- 面板和 VNC 画面只在局域网能打开。经 Cloudflare 隧道进来的请求，只能访问设备接口（`/api/device/`）。
- 打开 VNC 画面需要先登录面板。VNC 只在登录窗口打开期间运行，并且只监听容器内部。

## 设备接口

字段和微博 hub 一致，客户端可以复用同一套代码。令牌通过请求头 `X-Hub-Token` 或参数 `?token=` 传递。

| 接口 | 说明 |
| --- | --- |
| `GET /api/device/v1/status` | 账号状态；每条流的总开关（`feeds.following.on`）、上次和下次拉取时间 |
| `GET /api/device/v1/feed?seg=foryou\|following&since=&limit=` | 列表。总开关关着时返回空列表和 `on: false` |
| `POST /api/device/v1/refresh?seg=foryou` | 立即拉一次，等同于面板上的手动拉取 |
| `GET /api/device/v1/post/{id}` | 全文；引用推文放在 `rt_user` 和 `rt_text` |
| `GET /api/device/v1/post/{id}/comments` | 回复。第一次请求时用浏览器打开这条推文去取，之后走缓存 |
| `GET /api/device/v1/img/{id}/{n}.jpg` | 图片：`thumb.jpg` 是封面，`s<k>.jpg` 是方形小图 |
| `POST /api/device/v1/imgs` | 一次取多张列表小图（设备用，0.1.5 起）：请求体 `{"paths": [...]}`，每项是相对 `/api/device/v1/` 的图片地址（`img/{id}/thumb.jpg`、`img/{id}/s{k}.jpg`），最多 40 项；回 `application/octet-stream`，按请求顺序每张是 4 字节大端长度加 JPEG，服务器上没有的长度为 0。不等还在做的图 |
