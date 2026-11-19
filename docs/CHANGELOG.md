# 版本变更 / Changelog

按日期和作者简述对用户可见的功能变化；详细实现历史见 Git。使用方法见 [README](../README.md)。
User-visible changes by date and author; Git retains implementation history. See [README](../README.md) for usage.

## 未发布 / Unreleased

- 番茄安卓 App 1.0.1：修正书库里同时有读过和没读过的书时，一打开就闪退。
  Tomato Android app 1.0.1: fix a crash on launch when the library holds both read and unread books.
- 新增番茄安卓 App（`tomato/android`，1.0.0）：功能和界面照固件的书库与番茄标签——搜书、下载 TXT、检查更新、下载全部更新、书城、导入 TXT / EPUB、按页阅读（目录、字号、排版、自动翻页）。书源解析逐字节照搬固件，阅读进度和番茄书架用 WebDAV 和小纸 Pico 互通。
  New Tomato Android app (`tomato/android`, 1.0.0) mirroring the firmware's library and Tomato tab: search, TXT downloads, update checks, download all updates, the bookstore, TXT / EPUB import and paged reading (TOC, size, layout, auto turn). Book parsing copies the firmware byte for byte, and reading progress and the Tomato shelf sync with the Read Pico over WebDAV.
- WebDAV 同步修正（设备和 App）：先同步、后下载的书也能接上进度（本机从没同步过的书不再要求进度“比上次同步新”）；写回进度前核对章节和位置，书的文件和备份时不一样（比如换了下载服务器）就不用，同步结果里说明有几本。
  WebDAV sync fixes (device and app): progress now lines up for books downloaded after syncing (a book never synced here no longer needs to be "newer than the last sync"); chapter and offset are checked before writing progress back, and a file that differs from the backup (for example after switching download servers) is skipped and counted in the result.
- 小红书、微博：设置里可以选列表条数 100 / 200 / 300（设备在“设置 › 内容来源”，安卓 App 在设置页），刷新时一次取这么多条；条数越多，TF 卡上存的缩略图越多。rednote 服务器 0.3.0、weibo 服务器 0.1.4 起每次最多给 300 条。
  Xiaohongshu and Weibo: settings offer a list size of 100 / 200 / 300 (on the device under "Settings > Content sources", in the Android apps' settings); a refresh fetches that many, and more posts keep more thumbnails on the TF card. The rednote server 0.3.0 and weibo server 0.1.4 return up to 300.
- 修正：升级前缓存的小红书笔记“加载更多评论”一直是灰的。按钮改由设备和 App 自己判断（评论总数比显示的多、还没加载过）；服务器升级前拉的笔记没有令牌时，点了会说明原因（rednote 0.3.1、安卓 1.3.1）。
  Fix: "Load more comments" stayed gray on Xiaohongshu notes cached before the upgrade. The device and app now decide the state themselves (more comments than shown, not loaded yet); a note fetched before the server upgrade without a token explains why when pressed (rednote 0.3.1, Android 1.3.1).
- 小红书评论末尾加“加载更多评论”（设备和安卓 App 1.3.0）：点了让服务器现拉这篇最多 60 条评论，弹等待框，约 20～60 秒，算一次按需请求。评论本来就不比已显示的多、已经加载过、或服务器太旧时按钮是灰的。需要 rednote 服务器 0.3.0；旧笔记要等服务器下一轮拉取补上令牌后才能用。微博安卓 App 1.2.0 只加了列表条数。
  Xiaohongshu comments end with "Load more comments" (device and Android app 1.3.0): the server fetches up to 60 comments for the note behind a wait dialog, about 20-60 s, counted as one on-demand request. The button is gray when there are no more comments than shown, after loading once, or with an older server. Needs the rednote server 0.3.0; older notes work once the next fetch round gives them a token. The Weibo Android app 1.2.0 only adds the list size.

- 新增番茄下载服务器一键部署（仓库 `tomato/`，镜像 `treader-tomato`）：官方下载器加网页面板，面板里看状态、设设备密码、开 Cloudflare 隧道（只放行设备接口），书存在宿主机，默认格式自动设成 TXT；设备和安卓 App 不用改。
  New one-step Tomato downloader deployment (repo `tomato/`, image `treader-tomato`): the official downloader plus a web panel for status, the device password and a Cloudflare tunnel (device API only); books stay on the host and the default format is set to TXT; no device or app change.

- 小红书、微博：双击顶部当前分段（关注 / 推荐 / 热门）回到最新的第一页（设备和安卓 App）；安卓 App（小红书 1.2.0、微博 1.1.1）看完一篇返回时停在原来的位置，不再跳回最上面。
  Xiaohongshu and Weibo: double-tap the current segment (Following / Recommended / Hot) to jump to the newest first page (device and Android apps); the Android apps (Xiaohongshu 1.2.0, Weibo 1.1.1) return to the same scroll position after a post instead of jumping to the top.

- CI 用产品配置编译固件（不带内置番茄服务器，书城默认 FanqieRankTracker），固件一改动就自动发布到 Release，可以直接刷；项目主页改版，刷机网页上能直接下载最新固件，并说明番茄小说需要自建服务器。
  CI builds the firmware with the product configuration (no built-in Tomato server, FanqieRankTracker as the default bookstore) and publishes a Release after every firmware change, ready to flash; the project homepage was redesigned, and the flasher page offers the latest firmware with a note that Tomato novels need a self-hosted server.

- 微博改成卡片排版（设备和安卓 App 1.1.0）：头像圆、名字和时间来源、转发原文的灰框、图片放在正文下面（设备一排小图，安卓按微博的宫格排）、转评赞三栏带图标；点哪张图从哪张看。weibo 服务器 0.1.3 给每张原图做方形小图。
  Weibo moves to a card layout (device and Android app 1.1.0): avatar circle, name with time and source, a gray box for reposts, images under the text (a strip on the device, Weibo-style grids on Android), and a three-column count bar with icons; tapping an image opens that one. The weibo server 0.1.3 makes a square small image for each source image.
- rednote 服务器 0.2.2：拉取进行中点“立即拉一轮”不再被丢掉，按钮会说明结果（比如今天的请求次数已用满）。
  rednote server 0.2.2: "Run now" pressed during a round is no longer lost, and the button explains the outcome (for example, today's request cap is used up).

- 小红书、微博看大图时可以“存为壁纸”：点图片中间或长按，图片复制到 TF 卡 wallpapers/，也可以直接设成锁屏。锁屏壁纸预览里可以删除壁纸（点两次确认）。
  Xiaohongshu and Weibo image views can save the image as a wallpaper: tap the middle or long-press, and it is copied to wallpapers/ on the TF card, optionally set as the lock screen. The wallpaper preview can delete a wallpaper (two taps to confirm).
- rednote 服务器 0.2.0：关注的博主可以用 WebDAV 备份和恢复（rednote_backup.json）。
  rednote server 0.2.0: followed creators can be backed up to and restored from WebDAV (rednote_backup.json).

- 新增微博标签：经自建 weibo 服务器（仓库 `weibo/`，独立 Docker 镜像，不用登录微博）看关注的人和热门的图文微博。一行一条带缩略图；点开先看全文（长微博已补全、转发接原文），往后是图片和评论。服务器在面板里管理关注，WebDAV 备份和 weiboX 互通，带视频的不收，图片在服务器上转成墨水屏灰度图，表情显示成文字。另有同样功能的独立安卓 App（`weibo/android`，1.0.0）。
  New Weibo tab: followed users' and hot image posts through the self-hosted weibo server (repo `weibo/`, its own Docker image, no Weibo sign-in). One post per row with a thumbnail; a post opens on its full text (long posts completed, reposts followed by the original), then images and comments. The server manages follows in its panel, its WebDAV backups interoperate with weiboX, video posts are skipped, images are turned into e-ink grayscale on the server and emoji show as text. A standalone Android app with the same features ships as `weibo/android` 1.0.0.
- 底部标签最多同时显示七个：开满后其余开关变灰；“设置 › 内容来源”分成番茄、小红书、微博三页，手机网页填写两个服务器都能用。
  The tab bar shows at most seven tabs: once full, the other switches turn gray. "Settings > Content sources" splits into Tomato, Xiaohongshu and Weibo pages, and the phone web form works for both servers.

- rednote 服务器：点开推荐笔记更快。正文拉到就先返回，图片在后台同时处理 4 张（取到还没做好的那页时服务器等它）；推荐请求间隔默认从 8 秒改成 3 秒（之前没改过的配置自动改过来）。
  rednote server: opening a recommended note is faster. The text returns first while images process four at a time in the background (a request for an unfinished page waits for it); the default recommendation gap drops from 8 s to 3 s (untouched configs move over automatically).

- 小红书列表改成两列封面卡片：设备上每页 2×2、按页翻；安卓 App 1.1.0 改成上下滑动的瀑布流，封面按原图比例。服务器的封面改成宽 300、保留原图比例（高宽比 3:4～4:3），接口多了 `ratio`。
  Xiaohongshu lists become two-column cover cards: 2x2 pages on the device; the Android app 1.1.0 uses a scrolling waterfall with covers in their original ratio. Server covers are now 300 wide keeping the original ratio (3:4 to 4:3), and the API adds `ratio`.

## 2026-11-18 · Treader 0.5.0

- 新增小红书标签：经自建 rednote 服务器（仓库 `rednote/`）看关注博主和首页推荐的图文，一屏一张看图，再看正文和评论。推荐只在点“刷新推荐”时由服务器去读小红书。缓存按篇数和总大小封顶。
  New Xiaohongshu tab: browse followed creators and home recommendations through the self-hosted rednote server (repo `rednote/`), one image per screen, then text and comments. The server reads Xiaohongshu for recommendations only when you tap refresh. The cache is capped by count and total size.
- 小红书服务器地址和令牌可以用手机网页填写，不用在设备上打长令牌。
  The Xiaohongshu server address and token can be entered from a phone web page instead of typing the long token on the device.
- 新增 USB 传书：用数据线连电脑，TF 卡当 U 盘直接拷文件；传输期间设备只显示传输页，结束后重启恢复。平时插线仍是串口，刷机照常。
  New USB transfer: connect a cable and the TF card becomes a USB drive; only the transfer page shows meanwhile, and the device restarts to recover. Normally the port stays a serial port, so flashing works as before.
- 设置改版：首页六个分类，点进去是二级页。“隐藏虎扑”换成“底部标签”，番茄、漫画、小红书、虎扑都可以关，书库和设置固定；原来隐藏了虎扑的升级后照旧。
  Settings redesign: six categories on the home, each with its own page. "Hide Hupu" becomes "Tab bar", where Tomato, comics, Xiaohongshu and Hupu can be turned off (library and settings stay); an old hidden Hupu carries over.
- 联网任务前台优先：别的页留下的浏览类任务（虎扑、小红书、书城、搜索）会立刻让给你正在操作的页面；下载和同步不打断，前台请求排在后面自动开始。
  Foreground first: browsing jobs left by other pages (Hupu, Xiaohongshu, bookstore, search) yield at once to the page you are using; downloads and sync are not interrupted and the new request starts after them.
- rednote 服务器：设备按需拉推荐（面板可设开关、篇数、缓存、每日次数和间隔）；浏览器桌面不再接受从隧道来的连接。
  rednote server: on-demand recommendations for the device (switch, list size, cache, daily cap and gap in the panel); the browser desktop no longer accepts tunnel connections.
- 新增 `rednote/`：小红书内容服务器。Docker 一键部署，网页面板扫码登录、添加博主、定时拉取图文笔记（跳过视频），图片在服务器上处理成墨水屏用的灰度 JPEG，内置 Cloudflare Tunnel 给设备外网访问。
  New `rednote/`: a Xiaohongshu content server. One-command Docker deployment with a web panel for QR login and creators, scheduled fetching of image notes (videos skipped), server-side grayscale JPEGs for e-ink, and a built-in Cloudflare Tunnel for devices away from home.

## 2026-11-18 · Treader 0.4.1

- 虎扑：打开新帖子、换专区或联赛时，立刻取消还在跑的旧任务，马上加载你正在看的内容；连点多次只执行最后一次。不再出现“正在加载”一直挂着、或联网时误报“还没有缓存”。
  Hupu: opening another post, zone or league cancels the old job at once and loads what you are looking at; repeated taps run only the last one. No more stuck "loading" or false "not cached" while online.
- 虎扑：再打开缓存过的帖子时，没下好的图在后台自动补下；图片下载超时从 15 秒缩到 8 秒，网络卡住时切换更快。
  Hupu: reopening a cached post fills in the images still missing; the image timeout drops from 15 to 8 seconds so switching is quicker on a stalled network.
- 图片解码：PNG 改成边解压边缩放，再大的 PNG 都能显示（漫画、EPUB 插图都受益）；渐进式 JPEG 按剩余内存放宽上限；CBZ 里单张图超过 4 MB 不再导致整个包打不开。
  Image decoding: PNG now streams and scales while inflating, so any size displays (comics and EPUB illustrations alike); progressive JPEG gets a limit based on free memory; a single image over 4 MB no longer makes a whole CBZ unopenable.

## 2026-11-18 · Treader 0.4.0

- 阅读和漫画新增定时自动翻页：工具条里调“自动翻页”，关或 3～30 秒（每档 3 秒），阅读和漫画各记一份；碰屏幕或按键重新计时，工具条开着暂停，翻到最后一页停下；自动翻页期间不空闲锁屏。
  Timed auto page turn for the reader and comics: set "auto turn" in the toolbar to off or 3-30 s in 3 s steps, kept separately for each; a touch or key restarts the countdown, the open toolbar pauses it, the last page stops it, and the device does not idle-lock meanwhile.
- 新增漫画标签：读 TF 卡 `comics/` 里的 CBZ，和书库分开管理；一个子文件夹是一部系列，多话按文件名自然排序；封面网格、继续看、话列表；一屏一页翻页，一话看完接下一话，可切从右往左；长按清进度或删除。
  New comics tab for CBZ files in `comics/`, managed apart from the library: a subfolder is a series with episodes in natural order; cover grid, continue card and episode list; one page per screen, automatic next episode, optional right-to-left; long-press to clear progress or delete.
- 传书网页新增“上传漫画”：在手机浏览器里把每页转成灰度 JPEG、缩到屏幕大小（WebP、PNG 都能转，长条漫画按屏切开），再上传，体积约为原来的 1/5。
  The transfer page can upload comics: the phone's browser converts every page to a screen-sized grayscale JPEG (WebP and PNG included, tall strips split into screens) before uploading, about a fifth of the original size.
- 传书给 TF 卡留出预留空间（100 MB 和卡容量 5% 取大的），网页和设备都显示扣掉预留后还能传多少，不会把卡写满。
  Transfers keep a reserve on the TF card (the larger of 100 MB and 5% of the card); the page and the device show the space left after it, so uploads never fill the card.
- 设置新增“隐藏虎扑”：打开后底部只剩四个标签，菜单也不再有虎扑。
  Settings can hide Hupu: the tab bar drops to four tabs and the menu skips it.
- 修复虎扑：后台清理图片缓存时可能删到界面正在读写的文件（加了缓存锁）；连接中途断开的页面不再当成功；写缓存失败时保留旧文件，不会丢掉关注列表；刷新赛程时不再打乱正在看的评分页；专区编号只允许字母数字；网断时图片连续失败 3 张就停；关注上限改成和关注流一致的 12 个。
  Hupu fixes: cache pruning could delete files the UI had open (now locked); truncated pages no longer count as success; a failed cache write keeps the old file so follows are never lost; a schedule refresh no longer scrambles the open rating view; zone ids are restricted to letters and digits; image fetching stops after three failures in a row; the follow limit now matches the follow feed (12).
- 阅读：工具条空白处点一下不再翻页。
  Reader: tapping empty parts of the toolbar no longer turns the page.

## 2026-11-18 · Treader 0.3.0

- 书城：榜单地址是 `xxx.github.io/仓库` 时，直连超时或失败会自动改从 jsDelivr 镜像（fastly、gcore、cdn）取同一仓库 main 分支的数据，用通的那个记到重启；国内直连 GitHub Pages 常超时。
  Store: for `xxx.github.io/repo` rank sites, a timeout or failure falls back to jsDelivr mirrors (fastly, gcore, cdn) of the repo's main branch, keeping the working one until reboot; GitHub Pages often times out from China.
- 修复 HTTPS：证书链以“被 GlobalSign Root CA 交叉签名的 GTS Root R4”结尾的站点（Cloudflare 后面的自建服务器常见）握手失败；打开证书包的交叉签名校验。
  Fixed HTTPS to sites whose chain ends in GTS Root R4 cross-signed by GlobalSign Root CA (common behind Cloudflare) by enabling cross-signed verification in the certificate bundle.
- 联网：路由器 DNS 解析不了服务器域名时（常见于开了“DNS 重绑定保护”、域名指向内网 IP 的自建服务器），自动改用阿里 223.5.5.5 / 腾讯 119.29.29.29 公共 DNS 再试一次；失败提示能分辨“域名解析失败”“HTTPS 握手失败”“连接服务器超时”。
  Networking: when the router's DNS cannot resolve the server name (common with DNS-rebinding protection and self-hosted servers on LAN IPs), retry once via AliDNS 223.5.5.5 / Tencent 119.29.29.29; messages now distinguish DNS, HTTPS handshake and connect-timeout failures.
- 阅读新增排版设置：行距、段距、页边距、首行缩进（自动缩进两字）、每行下划线；在阅读工具条点“排版”调整，立即重排并对所有书生效。工具条的“清残影”去掉了，整屏刷新用长按中键。
  Reading layout options: line and paragraph spacing, margins, automatic two-character first-line indent and rules under lines, set from the reading toolbar and applied at once to all books. The toolbar's full-refresh tool is gone; hold the middle key instead.
- 修复联网：传书页启动失败（比如选了“已有 WiFi”但没配好）后，状态一直停在“出错”，之后番茄、书城、虎扑都以为传书还占着 Wi-Fi，连不上网，设置里扫描和保存 Wi-Fi 也失败；虎扑还不显示任何提示。Wi-Fi 改按中国信道（1～13），路由器在 12、13 信道也能连。
  Fixed networking: after the transfer page failed to start, its state stayed "error", so Tomato, store and Hupu all believed the transfer held Wi-Fi and never connected, and scanning/saving Wi-Fi in settings failed too; Hupu showed no message. Wi-Fi now uses the Chinese channel plan (1-13) so routers on channels 12-13 work.
- 修复键盘：字母 d、e、f、g、h、i、j 被当成了“分词、删除、123、全拼、空格、搜索、清空”（功能键编号和字母撞了）。
  Fixed the keyboard: letters d-j acted as function keys because their ids collided.
- 修复九键和全拼候选缺字：没打完的音节现在在全部匹配的词条里挑最常用的补全（原来只看前 1600 条），只按一个键也有候选；整句组词优先常用词。
  Fixed missing T9 and pinyin candidates: unfinished syllables now complete from every matching entry by frequency (previously only the first 1,600), a single key yields candidates, and sentence building prefers common words.
- 联网失败的提示后面带上具体原因（如“密码不对或信号太弱”“连不上服务器 errno 113”“HTTP 404”），一行放不下就换两行显示，原因不会被截掉。
  Network failure messages now carry the cause (wrong password or weak signal, server unreachable with errno, HTTP status) and wrap to two lines so it is not cut off.
- 新增虎扑标签（只读）：推荐带缩略图；在设备上关注专区并混排最新帖子，全部专区按分类浏览，专区帖子可按最新回复或最新发布；比分看 NBA、CBA、英超、LOL、网球赛程和已结束比赛的球员评分；帖子正文分页并内嵌图片、点图全屏看，“亮了”和前面的回复。设备直接取虎扑，不需要服务器；不联网时看缓存。
  New Hupu tab (read only): feed with thumbnails; follow zones on the device with an interleaved feed, browse all zones by category and zone posts by latest reply or newest; scores for NBA, CBA, EPL, LoL and tennis with player ratings for finished games; paged post bodies with inline images, a full-screen viewer, lit and first replies. The device reads Hupu directly with no server and shows caches offline.

## 2026-11-17 · Treader 0.2.0

- 首页并入书库：开机进书库，底部标签只剩书库、番茄、设置；书架顶部加“继续阅读”卡片（代替续读弹窗），番茄有更新时状态行提示；中键回书库。
  Home is merged into the library: boot opens the library, the tab bar has Library, Tomato and Settings; a continue-reading card (replacing the resume dialog) sits atop the shelf and Tomato updates show in the status line; the home key returns to the library.
- 书城榜单改成 3×3 封面网格：打开分类时后台一次取完封面，缩略图生成完整页刷新一次；超过约 6 个分类的封面缓存会自动清理。
  The store rank list is now a 3x3 cover grid: covers download in one background session when a category opens and the page refreshes once when thumbnails are ready; the cover cache is pruned past about six categories.
- 番茄书架、搜索结果、书城榜单和壁纸列表支持滑动翻整页（左滑 / 上滑下一页，右滑 / 下滑上一页），所有列表都不滚动。
  The Tomato shelf, search results, store ranks and wallpaper list turn a whole page per swipe; no list scrolls.

## 2026-11-17 · Treader 0.1.0

- 封面：EPUB 显示书内封面，番茄书下载时一起取封面；书库、首页和锁屏显示图片封面，缩略图缓存在 TF 卡 `.treader/covers/`。
  Covers: EPUBs show their embedded cover and Tomato downloads fetch one; the library, home and lock screen show image covers cached under `.treader/covers/` on the TF card.
- 设置新增“关于 Treader”：版本、项目地址和开源致谢；设备信息从这里进入。
  Settings gains "About Treader" with the version, project link and open-source credits; device info opens from there.
- 中键改为 home 键：短按回首页，长按 0.5 秒整屏刷新；阅读时短按仍开关工具条。
  The middle key is now Home: a short press goes home and a 500 ms hold does a full refresh; while reading a short press still toggles the toolbar.
- 删书时一起删掉封面缓存；壁纸页只保留当前壁纸的缓存；书城换榜单时清掉旧缓存和详情页封面，TF 卡不再越用越大。
  Deleting a book also deletes its cover cache; the wallpaper page keeps only the current wallpaper cache; new ranks clear old store caches and covers.
- 分区表改成两个 7 MB 程序槽，去掉不用的 5 MB 内置书库分区；新增 TF 卡升级（关于 Treader → 版本），新固件起不来自动退回。从旧分区表换过来要“完整安装”一次。
  Partition table now has two 7 MB app slots (the unused 5 MB book partition is gone); TF-card upgrade added (About -> Version) with automatic rollback. Moving from the old table needs one full install.
- 网页刷机：GitHub Pages 上的刷机页，选择本地的 treader-full.bin 写入，默认保留设置；固件不上传、仓库里不放固件。
  Web flasher: a GitHub Pages tool that writes a local treader-full.bin, keeping settings by default; firmware is never uploaded or stored in the repo.
- 书城：番茄页加“书架 / 书城”切换，按频道和分类看 FanqieRankTracker 榜单，详情页直接下载到书库；榜单地址可在设置里改，编译时可用 `STORE_URL` 设默认。
  Bookstore: a "shelf / store" switch on the Tomato tab browses FanqieRankTracker ranks by channel and category and downloads from the detail page; the rank URL is configurable in Settings with a `STORE_URL` build default.
- 设置里“Wi-Fi”和“导入书籍”合并为“Wi-Fi 与传书”。
  Settings merges "Wi-Fi" and "Import books" into "Wi-Fi and transfer".
- 锁屏壁纸：设置里从 TF 卡 `wallpapers` 文件夹选图并全屏预览；锁屏铺满壁纸，底部显示在读的书和电量。
  Lock wallpaper: pick an image from the card's `wallpapers` folder in Settings with a full-screen preview; the lock shows it with the current book and battery.
- 设置新增“TF 卡”：状态、重新挂载、两步确认的格式化，格式化后建好 Treader 用的文件夹。
  Settings gains "TF card": status, remount, and a twice-confirmed format that recreates Treader's folders.
- 拔卡后重新插卡会自动挂载并恢复；书和图片只存 TF 卡，不再使用内置 storage 分区。
  Reinserting a card remounts it automatically; books and images live only on the card, no longer in internal storage.
- 番茄服务器可在编译时内置（`local.properties`，同安卓版）；设置里填写的优先，界面不显示内置地址和密码。
  A default Tomato server can be built in via `local.properties` (as in the Android app); a server set in Settings wins, and the UI never shows the built-in one.

## 2026-10-09

- 锁屏：未单独选择时默认关机（EN=0，长按约 1 秒开机）。已保存的浅睡或深睡不变。
  Power-key lock defaults to power-off when no mode is stored. A saved light or deep choice stays.
- 空闲 2 分钟自动锁屏，睡眠档仍按已选的浅睡、深睡或关机。传书页停留期间不计时。
  Lock after 2 minutes idle, using the saved light, deep, or power-off mode. The transfer page does not count idle time.

## 2026-09-28 · UNSaWEN

- 阅读：进入书架先确认是否续读；ZIP目录连续读取、字宽测量不生成位图，当前章先排起始/续读页，其余分页分批补齐。
  Confirm resume before opening the last book; read ZIP directories sequentially, measure advances without rasterizing, and paginate the initial/resume pages before completing the chapter incrementally.
- EPUB插图：默认占位，点击后单独预览本地JPEG/PNG，返回保持正文位置；标题前的章首图片及同资源单独标为标题图，其他重复图片提示本次已读章节中最早出现的位置，开章不再反复解码重复图片。
  Show image placeholders and load local JPEG/PNG previews only on tap, returning to the same text position; label opening title images and matching resources separately, retaining earliest-visited hints for other repeated illustrations and avoid decoding images on chapter opening.
- 手势：滑动翻页阈值由120缩短到64像素，保留抬手提交、轻点容差和短拖取消。
  Reduce swipe distance from 120 to 64 pixels while retaining release-to-commit, tap tolerance and short-drag cancellation.
- EPUB：ZIP 条目、资源项及章节上限提高到 32768，使用固定窗口扫描ZIP目录和紧凑索引查找，标题按实际长度保存，OPF/导航上限4 MiB；打开和排版显示等待提示，失败显示具体原因。
  Raise EPUB ZIP, manifest and spine limits to 32768, scan ZIP directories through a fixed window, use compact indexed lookup and actual-length titles, and allow 4 MiB OPF/navigation metadata; show parsing/layout wait hints and failure reasons.
- WiFi：TF 卡支持上传完整 TTF 字体，32 MiB 上限、同名确认、提交前结构校验及中断恢复；传书期间暂停卡上字体读取，离页恢复。
  Upload complete TTF fonts to TF over WiFi with a 32 MiB limit, confirmed replacement, pre-commit structural checks and interruption recovery; suspend card font reads during transfer and resume on exit.
- 部署：随仓库提供未修改的 ChillDuanSans VF v1.30、原 OFL-1.1 许可和来源校验；内置子集命名为 Read Pico UI，保留原版权与许可。
  Bundle unmodified ChillDuanSans VF v1.30, original OFL-1.1 terms and source verification; name the embedded subset Read Pico UI and retain attribution and licensing.

## 2026-09-27 · UNSaWEN

- EPUB 图片位置增加“[图片]”占位，避免纯图片章节显示为空白；暂不解码图片。
  Show “[图片]” placeholders at EPUB image positions so image-only chapters are visible; image decoding is not yet supported.

- 修复长篇 EPUB 因条目或章节超过 512 而无法打开的问题；上限提高到 2048，单资源解压上限仍为 2 MiB。
  Fix long EPUBs failing to open above 512 entries or chapters; raise the limit to 2048 while retaining the 2 MiB uncompressed resource limit.

- 图书阅读：支持 TF 卡或内置存储的 UTF-8/GBK TXT 与纯文本 EPUB，目录、逐书续读、字号、手势翻页及默认关闭的实验晃动翻页。
  Read UTF-8/GBK TXT and text-only EPUB from TF or internal storage, with a TOC, per-book resume, font sizes, gestures and optional experimental shake-to-turn.
- 图书管理：增加拼音/首字母/英文搜索、来源筛选、名称/最近阅读排序、详情，以及分别确认的单本/批量删除和清进度。
  Add pinyin/initials/English search, source filters, name/recent sorting, details and separately confirmed single/batch deletion or progress reset.
- WiFi 传书：支持设备热点和已有网络、触屏/网页配网、连接与网址二维码；网页管理当前存储，支持确认替换、取消和重试；停止后返回进入前的位置。
  Transfer over the device hotspot or an existing network, with touchscreen/web provisioning and connection/URL QR codes. Manage current storage in the browser, confirm replacements, cancel or retry uploads, and return to the entry location on stop.
- 可靠性：处理保存失败与部分成功重试、中文文件名和覆盖中断恢复；修复显示欠载后相位队列残留导致的死锁；检测 TF 失效后停止相关消费者并回退字体，插回后需显式重挂。
  Handle save failures, partial-operation retries, Chinese filenames and interrupted replacements. Fix a display queue deadlock after underrun; stop affected consumers and fall back fonts on TF loss, requiring explicit remount after reinsertion.
