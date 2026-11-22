# webtoon：WEBTOON 发现与追更服务器

给小纸 Pico 的“漫画”标签做的发现页和追更服务器：在设备上逛 [WEBTOON](https://www.webtoons.com) 的排行、类型、星期更新表、已完结和搜索，点“订阅”后按“检查更新”下载新话。服务器只在设备要某一话时，才去 webtoons.com 拉那一话的原图，缩到屏幕宽度、转灰度、在分镜之间的空白处按屏幕高度切页，打包成 CBZ 给设备。

- **服务器不保存订阅**：订阅存在设备 TF 卡上（每部作品文件夹里一个 `.webtoon` 标记文件），设备来问“这几部有没有新话”。换服务器、重装都不丢订阅。
- **切页适合墨水屏**：竖长条漫画按 648×1000 切成一页一页，切点找分镜之间的留白，尽量不把画格和对白切成两半；很长的留白压短，整页空白的不要。每页是普通（基线）JPEG，设备能走快速解码，翻页和看本地 CBZ 一样快。
- **简繁**：中文版 WEBTOON 是繁体。设备用拼音输入法打的简体关键词，服务器先转成繁体再搜；作品名、简介默认转成简体显示（图里的字不变）。
- **一个 compose 文件部署**：`docker compose up -d`，然后打开网页面板。面板里也能逛发现页、试转一话下载 CBZ 看效果。
- **外网访问**：内置 Cloudflare Tunnel（快速隧道或隧道令牌），隧道只放行设备接口，面板始终只在局域网。

```
小纸 Pico（订阅在 TF 卡）──HTTPS(令牌)──> webtoon-hub :8095 ──> webtoons.com（公开免费的话）
                                              │ 面板（只在局域网）
                                              └─ cloudflared（可选）──> Cloudflare ──> 外网的设备
```

手机上也能看：[webtoon/android](android/README.md) 是和固件发现漫画功能一样的独立安卓 App，读同一台服务器、用同一个设备令牌。

> **只拉公开免费的内容，只给自己用。** 服务器不登录 WEBTOON，只读网页上不登录就能看的话，不碰付费、抢先看（Fast Pass）和等免费的话。WEBTOON 的作品有版权，它的使用条款不允许抓取：请自己部署、只给自己离线阅读，**不要把服务器开放给别人，也不要分享下载的漫画**。

## 部署

| 项目 | 要求 |
| --- | --- |
| 机器 | x86-64 的 Linux、NAS 或装了 Docker Desktop 的电脑 |
| 网络 | 能打开 webtoons.com（中国大陆打不开） |
| 内存 | 约 200 MB；转换时多一点；开插件来源再加约 1 GB |
| 磁盘 | 每话约 3～6 MB，缓存默认保留 14 天、上限 3 GB（面板里可调） |

```bash
mkdir webtoon && cd webtoon
curl -fLO https://raw.githubusercontent.com/bidabrain/treader-eink/main/webtoon/docker-compose.yml
docker compose up -d
```

打开 `http://服务器IP:8095`，第一次设置面板密码（至少 8 位）。端口被占用时在 `.env` 里写 `WEBTOON_PORT=其他端口`。

### 让设备连上来

面板 → **设备与外网** 里有地址和令牌。设备上：设置 → 内容来源 → 漫画，打开“手机网页填写”，用手机把地址和令牌粘贴进去。

- **在家里**：地址填 `http://服务器局域网IP:8095`。
- **在外面**：选一种外网方式，保存后显示外网地址：
  - **快速隧道**：什么都不用准备，得到一个随机的 `https://xxx.trycloudflare.com`，每次重启都会变。
  - **隧道令牌**（长期用，域名托管在 Cloudflare）：在 [Cloudflare Zero Trust](https://one.dash.cloudflare.com/) 建隧道，复制 `eyJ` 开头的令牌；公共主机名的**路径填 `^/api/device/`**，服务 **HTTP**，URL **`localhost:8095`**。把令牌粘贴到面板，选“隧道令牌”，保存并应用。
  - 面板程序也会挡：带 `CF-Connecting-IP` 的请求只能访问 `/api/device/`，其余一律 403。**不要在路由器上把 8095 映射到公网。**

## 设置（面板 → 设置）

| 项目 | 默认 | 说明 |
| --- | --- | --- |
| 页面宽度 / 高度 | 648 / 1000 | 小纸 Pico 漫画区域；改了只影响之后新转的话 |
| JPEG 质量 | 85 | 每页约 50～70 KB |
| 两张原图之间等待 | 0.4～1.2 秒 | 随机，别给 WEBTOON 添负担 |
| 列表和详情缓存 | 30 分钟 | 发现页列表、作品详情 |
| 保留天数 / 缓存上限 | 14 天 / 3000 MB | 转好的 CBZ 和封面；超了从最久没用的删 |

## 插件来源（可选）

开了这个功能后，同一个 compose 里会多起一个 [Suwayomi-Server](https://github.com/Suwayomi/Suwayomi-Server)，用来运行 Mihon / Tachiyomi 插件。hub 通过它读插件来源里的漫画，转换方式和 WEBTOON 的话一样：转灰度、按屏幕切页、打包 CBZ。设备上的订阅、检查更新和下载流程都不变。

> **本项目不提供也不推荐任何插件仓库。** 仓库地址由你自己在面板里添加。只添加你有权访问的来源，比如你自己的 Komga / Kavita 书库、官方免费连载，并遵守来源网站的条款。上面“只给自己用、不开放、不分享”的要求同样适用于所有来源。

**打开**：在 `docker-compose.yml` 旁边的 `.env` 里加一行，然后重新执行 `docker compose up -d`：

```
COMPOSE_PROFILES=mihon
```

- **内存**：Suwayomi 是 Java 程序，要多占约 1 GB 内存。Java 堆默认上限 768 MB，可以在 `.env` 里用 `MIHON_HEAP=1g` 调整。
- **安全**：Suwayomi 不映射端口，只有 hub 能在 compose 内网里访问它。两边用的密码在 hub 第一次启动时生成，存在 `data/mihon/suwayomi.secret`。Suwayomi 自己的网页界面是关掉的。插件数据存在 Docker 卷 `suwayomi` 里。
- **WebView**：没有装 Chromium（`KCEF_ENABLED=false`）。必须过网页验证的来源会报错。
- **本地漫画**：把 `docker-compose.yml` 里 `./local` 那一行的 `#` 去掉，就能用 Mihon 本地来源读 `./local` 里的漫画（每部一个文件夹，每话一个子文件夹或一个 CBZ）。

**面板 → 插件**：

- 添加或删除插件仓库；安装、更新、卸载插件。
- **设备上的来源**：勾选哪些来源出现在设备上，调整顺序；成人来源默认不显示，勾选“显示成人来源”后才显示。
- **同一话有多个版本**（不同汉化组）：默认只留最先出现的那个。
- **跨页**（横向的整张图）：默认拆成两页、右页在前；也可以改成左页在前、旋转 90° 或整张缩小。

**话号**：插件章节的编号可能是小数（如 12.5），也可能没有或重复，所以 hub 自己按来源顺序从旧到新分配整数话号（1、2、3…），存在 `data/mihon/chapters.sqlite`。分配过的话号不会再变。之后新出的章节排在最大话号后面，所以来源后来补上的旧章节会排在最后。这张表只是缓存，丢了会按同样的规则重建。

**切页**：先看每话前 3 张图的高宽比，中位数大于 2 就当竖长条，和 WEBTOON 一样在分镜间的空白处切页；否则当普通漫画，一张图一页，裁掉白边后缩进 648×1000。`ComicInfo.xml` 里会写明是长条（`Format=Webtoon`），还是右翻（`Manga=YesAndRightToLeft`）。

**限制**：Suwayomi 的数据（Docker 卷）如果被清空，作品编号会重新分配。这时 hub 会拒绝下载旧编号（返回 409，避免下到别的作品），已订阅的插件作品需要在设备上重新订阅。

## 设备接口

都要请求头 `X-Webtoon-Token: <令牌>`。`lang` 是语言版本（`zh-hant` 繁体中文、`en`、`es`、`fr`、`de`、`th`、`id`），默认 `zh-hant`；`zh=s`（默认）把繁体文字转成简体，`zh=t` 原样。作品编号：官方作品是数字（`8420`），业余 Canvas 作品前面加 `c`（`c1060813`）。

| 接口 | 返回 |
| --- | --- |
| `GET /api/device/v1/sources` | `{mihon, items:[{key,name,lang,latest,nsfw}]}`。第一项总是 `webtoon`；开了插件功能时，后面是面板里没有隐藏的插件来源，`key` 是来源编号（字符串），`latest` 表示这个来源有“最新” |
| `GET /api/device/v1/store?seg=rank\|genre\|day\|done&key=…&limit=60` | `{seg, key, keys:[{key,name}], items:[{id,title,author,genre,likes,canvas,adult}]}`。`rank` 的 key 是 `trending` / `popular` / `originals`；`genre` 的 key 从 `keys` 里来；`day` 的 key 是 `monday`…`sunday`（不带时是今天）；`done` 是已完结 |
| `GET /api/device/v1/search?q=…` | `{q, items:[…同上]}`，官方作品排前面 |
| `GET /api/device/v1/title/{id}` | `{id,title,author,genre,summary,done,latest,episodes:[{no,title,date}]}`，`episodes` 是全部免费话，从旧到新 |
| `GET /api/device/v1/cover/{id}.jpg` | 150×200 灰度封面（基线 JPEG） |
| `POST /api/device/v1/check` | 请求 `{lang, zh, titles:[{id,last}]}`（`last` 是已下载到第几话），返回 `{titles:[{id,latest,new:[{no,title}],error?}]}`。服务器不记这些 |
| `GET /api/device/v1/episode/{id}/{no}` | `{state:"ready",size}` / `{state:"pending",done,total}`（原图进度）/ `{state:"error",error}`；没转过就开始转。设备隔几秒再问 |
| `GET /api/device/v1/episode/{id}/{no}.cbz` | 转好的 CBZ：`0001.jpg` 起顺序编号的灰度页面（不压缩的 ZIP）加 `ComicInfo.xml` |

插件来源：`store` 和 `search` 加上 `src=<来源 key>`（不带时默认是 `webtoon`）。这时 `seg` 只能是 `pop`（人气）或 `latest`（最新），`keys` 为空；每项多一个 `src` 字段（来源名），`likes` 是 0。插件作品的编号是 `m` 加数字（如 `m12`）。`title`、`cover`、`check`、`episode` 都按编号前缀自动分开处理，不看 `lang` 和 `zh`，所以一次 `check` 可以同时带 WEBTOON 和插件作品。插件作品的 `title` 多两个字段：`src`（来源名）和 `error`（插件刷新失败但有旧数据时的原因）。

出错时返回 `{"detail": "原因"}`，401 是令牌不对。

## 设备上的文件（固件和安卓 App 约定）

```
comics/
└── <作品名>/                  # 一部系列一个文件夹（我的漫画只认一层）
    ├── .webtoon               # 订阅标记（JSON），我的漫画忽略它
    ├── 第0001话 标题.cbz
    └── 第0002话 标题.cbz      # 话号补零到 4 位，文件名自然排序就是话的顺序
```

`.webtoon` 的内容：

```json
{"id": "8420", "lang": "zh-hant", "title": "我的虎系青梅竹马", "author": "GIMAN、ETER", "last": 66, "added": 1795275780}
```

`last` 是已下载的最大话号；检查更新时把它发给服务器。文件夹改名、整个拷到另一张卡都不影响订阅。取消订阅就是删掉 `.webtoon`（已下载的话留着，变成普通的本地漫画）。

## 测试

```bash
docker build -t treader-webtoon:dev webtoon/hub
docker run --rm -v "$PWD/webtoon/hub:/srv" -w /srv treader-webtoon:dev sh -c "pip -q install pytest && python -m pytest -q tests"
```

离线测试，不联网：解析（合成的网页片段）、切页（合成的长条，切点要落在空白里；普通漫画一张一页、跨页拆分与旋转、长条与单页的判断）、打包、设备接口（替身数据源，包括插件来源和两种作品混在一起的检查更新）、插件话号表（首次编号、新章节追加、补档、重复版本、来源顺序反过来）、缓存清理。

## 目录

| 文件 | 作用 |
| --- | --- |
| `hub/app/source.py` | 读 webtoons.com：发现页列表、搜索、详情、免费话、每话原图地址；网页改版只改这里 |
| `hub/app/convert.py` | 原图 → 墨水屏页面：缩放、灰度、在空白处切页（长条）或一张一页（普通漫画）、打包 CBZ、封面 |
| `hub/app/mihon.py` | 插件来源：调 Suwayomi 的 GraphQL、话号表、插件与仓库管理 |
| `hub/app/jobs.py` | 转换队列（一次一话）和缓存清理 |
| `hub/app/main.py` | 面板和设备接口 |
| `hub/app/tunnel.py`、`auth.py` | Cloudflare 隧道、面板密码和设备令牌（和 rednote 一样） |
| `hub/app/web/` | 面板网页 |
