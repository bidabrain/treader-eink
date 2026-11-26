# 版本变更 / Changelog

按日期和作者简述对用户可见的功能变化；详细实现历史见 Git。使用方法见 [README](../README.md)。
User-visible changes by date and author; Git retains implementation history. See [README](../README.md) for usage.

## 2026-11-27 · Treader 1.2.15

- 显示大小放大后，漫画书架和书库网格一页能放更多：100% 和 115% 都是每页 6 本（115% 封面缩小，“继续看 / 继续阅读”卡片保留），130% 每页 3 本、封面放大填满屏幕，不再下面空一大块。100% 的样子不变。
  At larger display sizes the comics shelf and the library grid fit more per page: six at 100% and 115% (smaller covers at 115%, with the continue card kept), and three at 130% with larger covers filling the screen instead of leaving a big gap below. 100% looks the same.

## 2026-11-27 · webtoon 服务器 0.3.5

- 面板新增“上传”页：上传自己的 CBZ（ZIP）或 PDF，服务器和下载的话一样转灰度、按页面大小分页（竖长条在空白处切，普通漫画一张一页），打包成 CBZ 放进缓存，在“缓存”页下载；可以一次选几个文件，源文件转完就删。
  The panel has a new Upload page: upload your own CBZ (ZIP) or PDF and the server grays and pages it like downloaded episodes (tall strips cut in gaps, regular comics one image per page), packs a CBZ into the cache for download on the Cache page; several files at once, sources deleted once converted.
- 概览新增“当前任务”：列出正在转和排着的任务（作品、话、来源、进度），每 3 秒刷新；每个都能取消，也能全部取消，正在转的在下一张图之前停下。设备要的话被取消后，设备上显示“已在面板取消”。固件和安卓 App 不用改。
  Home gains Current tasks listing running and queued jobs (title, episode, origin, progress), refreshed every 3 s; each can be cancelled, or all at once, and a running one stops before its next image. A cancelled device request shows "已在面板取消" on the device. No firmware or Android change.

## 2026-11-26 · webtoon 服务器 0.3.4

- 面板“发现”里点开作品后能像客户端一样选话下载到服务器缓存：每话前有勾选框，可以全选、选最近 5 话或填“第几话到第几话”，点“下载到缓存”后服务器从旧到新一话一话转；每话旁边显示排队中、转换进度或“下载 CBZ”，转好的也在“缓存”页。详情默认列最新 30 话，可展开全部；还没开始的可以“取消排队”，正在转的那一话会转完。选得太多快超过缓存上限时会提示（超出会先清掉最久没用的）。设备接口和固件、安卓 App 都不用改。
  In the panel's Store, an opened title now lets you pick episodes to download into the server cache like the clients: tick episodes, select all, the latest 5 or a "from episode N to M" range, then "下载到缓存" converts them one by one oldest first; each episode shows queued, conversion progress or "下载 CBZ", and finished ones also appear on the Cache page. The detail lists the newest 30 episodes and can expand to all; episodes not yet started can be cancelled, while the one converting finishes. A selection close to the cache cap gets a warning (the least recently used go first when it is exceeded). No device, firmware or Android change.

## 2026-11-26 · Treader 1.2.14

- Wi-Fi 能保存多个网络，用法像手机：选网页里保存过的网络排在前面、标“当前”或“已保存”，点一下直接连；密码页多了“保存这个网络”（默认勾），改成先连后存，连上才保存，不勾只在这次开机用；新加“已保存的网络”页，列出所有保存的网络和上次连上的时间，逐个“忘记”。列表加密存在 TF 卡（钥匙在设备里，不用设密码，单拿卡解不开）；当前网络连不上时自动换试附近保存过的网络，连上的记成当前网络。没插卡时照旧只有一个网络。
  Wi-Fi keeps several networks, like a phone: saved networks lead the list marked "Current" or "Saved" and join on a tap; the password view gains "Save this network" (checked by default) and now connects before saving, so a network is saved only once it joins and unchecked means this boot only; a new "Saved networks" view lists every saved network with when it last joined and forgets them one at a time. The list is encrypted on the SD card (the key stays in the device with no password to set, so the card alone opens nothing); when the current network fails, nearby saved networks are tried and the one that joins becomes current. Without a card there is still one network, as before.
- 状态栏右上角、电量左边显示当前要连的 Wi-Fi 名；设置首页的 Wi-Fi 一行写当前网络和保存了几个。
  The status bar shows the current Wi-Fi name at the top right, left of the battery; the Settings home Wi-Fi row shows the current network and how many are saved.

## 2026-11-26 · Treader 1.2.13

- 文字深浅整体加深一档：去掉原来最浅的“标准”，原来的“深”现在叫“标准”，原来的“更深”现在叫“深”，新加的“更深”正文再加粗一级、灰字变纯黑。升级后看起来和原来一样：原来选“深”的落到新“标准”，选“更深”的落到新“深”，原来选“标准”的落到新“标准”。
  Text darkness moves one step darker: the old lightest "Standard" is gone, the old "Dark" is now "Standard", the old "Darker" is now "Dark", and the new "Darker" makes body text one weight bolder and gray text pure black. After the update the text looks as before: old "Dark" lands on the new "Standard", old "Darker" on the new "Dark", and old "Standard" on the new "Standard".
- 阅读排版新增“空行：忽略 / 保留”，默认忽略：TXT 里只有空白的行不再占一整行高度，段与段之间只按“段距”隔开，原文每段之间空一行的书不会再显得稀稀拉拉；一段折行后剩下的行尾空格不算空行。选“保留”就照原文显示。番茄安卓 App 的排版面板也加了这一项。
  Reading layout gains "Blank lines: Skip / Keep", skipping by default: lines of only blanks in a TXT no longer take a full line, so paragraphs are separated by the paragraph spacing alone and books with an empty line between every paragraph no longer look sparse; trailing blanks left by a wrapped line are not a blank line. "Keep" shows the text as written. The Tomato Android app's layout panel gains the same option.
- 书库的“检查更新 / 下载 N 本更新”从状态行右边的字换成筛选条最右边的按钮，和漫画的“检查更新”一样；正在检查或更新时点一下先变“再点取消”，再点才取消，点别处复原，不会再一碰就取消。
  The library's "Check updates / Download N updates" moves from text at the right of the status line to a button at the right end of the chip row, like the comics' "Check updates"; while checking or updating, one tap shows "Tap again to cancel" and only a second tap cancels, while tapping elsewhere resets it, so a stray touch no longer cancels.
- 没选过睡眠方式的设备，锁屏默认改为深睡（短按电源键开机），不再是关机；在设置里选过的照旧。
  Devices that never chose a sleep mode now default to deep sleep on lock (a short press of the power key boots) instead of power off; a choice made in Settings stays.

## 2026-11-26 · Treader 1.2.12

- 修正用一阵子以后小红书、微博、X、虎扑的大图卡在“正在处理图片…”、漫画报“这一页设备解不了”，重启才好：原因是字形缓存的几千个小块和图片要的大块混在同一片 PSRAM 里，把内存切碎了，总空闲还有两三 MB 却凑不出一块全屏图。现在字形缓存开机时就放进一块专用区（上限从 1.5 MB 改成 1 MB），离开漫画会还回进度表、书架和话名表（约 800 KB）；万一还是解不开，先放掉内存里缓存的图再试一次，真不行就显示“这张图打不开”，不会一直停在“正在处理”。
  Fixed large images in RedNote, Weibo, X and Hupu sticking at "Processing image…" and comics reporting "this page cannot be decoded" after a while of use until a restart: thousands of small glyph-cache blocks mixed with the large blocks images need shredded PSRAM, so two or three megabytes stayed free yet no full-screen block could be found. The glyph cache now lives in its own block reserved at boot (its cap goes from 1.5 MB to 1 MB), and leaving comics returns the progress table, shelf and episode names (about 800 KB); if an image still will not decode, cached images in memory are freed and it is tried once more, and a real failure shows "this image cannot be opened" instead of "Processing" forever.

## 2026-11-26 · Treader 1.2.11

- 新增“设置 → 代理”：填一台局域网里的 SOCKS5 或 HTTP 代理（地址:端口，可选用户名密码，也能写 socks5://… 或 http://… 自动选协议），打开后书城、番茄、小红书、微博、X、漫画、WebDAV 和固件升级的联网都经它出去，改完马上对新连接生效。域名交给代理解析，所以被污染的域名也能通；局域网地址、.local 名字直连，校时不走代理；TLS 仍在设备和目标之间端到端，代理只转发密文。握手只在新建连接时多两三次往返，不占 PSRAM、不多占连接。
  New "Settings > Proxy": enter a SOCKS5 or HTTP proxy on your LAN (address:port, optional user name and password; socks5://… or http://… picks the protocol), and the store, Tomato, RedNote, Weibo, X, comics, WebDAV and firmware updates all leave through it, applied to new connections at once. Names are resolved by the proxy, so poisoned domains work too; LAN addresses and .local names go direct and time sync skips it; TLS stays end to end between the device and the target and the proxy only relays ciphertext. The handshake adds two or three round trips only when a connection is opened, uses no PSRAM and holds no extra connection.
- 番茄设置的“手机网页填写”多了“书城榜单地址”一栏，扫码就能在手机上填番茄服务器和书城榜单；网页上服务器地址留空表示不改，可以只改榜单，榜单地址留空恢复默认。
  The Tomato settings "phone web form" gains a "rank site address" field, so the Tomato server and the store ranking can both be filled from the phone; a blank server address leaves it unchanged so the rank site can be set alone, and a blank rank address restores the default.

## 2026-11-26 · Treader 1.2.10

- 书库里“下载 N 本更新”在跑时，状态行下面多一条进度条，左边写正在做哪一步：连接 Wi-Fi、服务器生成 TXT，或者“书名 · 3.2 / 9.0 MB”，下载中每 2 秒刷新一次。以前一本大书要下好几分钟，画面一动不动，看起来像卡住了。服务器不给文件大小时，进度按旧文件大小和新增章节估算。书城和番茄详情页的下载条知道文件大小时也按真实进度走，MB 数下载中会跟着变。
  While "Download N updates" runs in the library, a progress bar appears under the status line and the left side says which step is under way: joining Wi-Fi, the server making the TXT, or "title · 3.2 / 9.0 MB", refreshed every 2 seconds while downloading. Before, a large book took minutes with nothing moving on screen and looked stuck. When the server gives no file size, progress is estimated from the old file and the new chapters. The store and Tomato detail bars follow the real progress when the size is known, and their MB count now moves while downloading.
- 修正书库网格视图在放大显示时，选中框和按下框比书大很多、下面拖出一大截：框现在只包住封面、书名和进度。
  Fixed the library grid's selection and press outlines reaching far below the book at larger display sizes: they now wrap only the cover, title and progress.

## 2026-11-25 · Treader 1.2.9

- 锁屏用壁纸时，底部白条连书名也不写了，只留唤醒提示和电量。
  With a lock wallpaper, the bottom strip drops the book title too, keeping only the wake hint and battery.
- 修正手机上传壁纸时某张图一传就“网络中断”：同名（手机相册里常有好几张都叫 image.jpg）或文件名太长时，设备拒收会直接断开连接。现在同名由设备自动加 -2、-3 存下，文件名在手机上按字节截短，真有问题时网页会写出原因。
  Fixed a wallpaper upload failing with "connection lost" every time: a clashing name (phone galleries often hand over several image.jpg) or an over-long name made the device refuse by dropping the connection. The device now saves clashing names as -2, -3, the phone shortens names by bytes, and real problems show their reason on the page.

## 2026-11-25 · Treader 1.2.8

- 设置 › 内容来源 › 小红书 / 微博 / X 新增“往后翻接着拉”（三处是同一个开关，默认关）。开着时，小红书关注、微博关注、X 正在关注和为你推荐翻完刷新拿到的那批，会继续向服务器要更早的，一批是“每次拉取”的条数，直到和卡上以前的缓存接上，两次刷新之间漏掉的帖子就补上了；卡上缓存也翻完时还能接着往后要，直到服务器没有更早的。拉到后自动翻到下一页；还能再拉时页码写成“3 / 5+”。没网或服务器出错时提示一下，接着显示卡上缓存。开着时列表最多 300 条（关着仍是 100 条）。需要 rednote 0.3.5、weibo 0.1.7、x 0.1.6 或更新的服务器。
  Settings > Content sources > Xiaohongshu / Weibo / X gain "Keep pulling" (one switch shared by all three, off by default). When on, paging past the batch a refresh brought in Xiaohongshu Following, Weibo Following and X Following and For You keeps asking the server for older posts, a "fetch size" at a time, until they meet the earlier cache on the card, so posts missed between refreshes are filled in; once the card's cache runs out too it keeps going until the server has nothing older. The page turns by itself when a batch arrives, and the pager reads "3 / 5+" while more can be pulled. Without a network or on a server error it says so and shows the card's cache. With the switch on, lists hold up to 300 (still 100 when off). Needs rednote 0.3.5, weibo 0.1.7, x 0.1.6 or newer.
- 设置 → 显示与阅读 → 锁屏壁纸右上角新增“手机上传”：设备显示二维码和网址，手机浏览器选图后自动缩成屏幕大小的灰度图（可选铺满裁边或完整留白）再传进 TF 卡的 wallpapers 文件夹，同名自动加序号；传完点“完成”就能在列表里选来当壁纸。不用再拿读卡器拷图。
  Settings > Display and reading > Lock wallpaper gains "From phone" at the top right: the device shows a QR code and address, and the phone's browser shrinks the chosen pictures to screen-size grayscale (filled and cropped, or whole with margins) before sending them to the wallpapers folder on the TF card, numbering clashing names; tap "Done" and pick one in the list. No card reader needed.

## 2026-11-25 · rednote 服务器 0.3.5、weibo 服务器 0.1.7、x 服务器 0.1.6

- 设备列表接口 `GET /api/device/v1/feed` 新增可选参数 `before`，回复里多一个 `next` 游标：还有更早的帖子时给出，原样放进 `before=` 再请求就得到接下来的一批（rednote 关注、weibo 关注、x 正在关注和为你推荐；weibo 热门不给）。用游标而不是“第几条”，服务器中途抓到新帖也不会重复或漏掉。旧固件和安卓 App 不传 `before`，结果和以前一样；只是发布时间完全相同的几条现在按编号固定排序。
  The device list endpoint `GET /api/device/v1/feed` gains an optional `before` parameter and a `next` cursor in the reply: present while older posts remain, it is passed back as `before=` for the next batch (rednote following, weibo following, x Following and For You; not weibo hot). A cursor rather than a position means posts fetched meanwhile cause no repeats or gaps. Older firmware and the Android apps send no `before` and get the same results as before, except that posts with identical times are now ordered by id.

## 2026-11-25 · Treader 1.2.7

- 小红书列表改成和微博、X 一样一行一篇：左边大封面（按原图比例），右边头像、作者、时间和图数、标题、赞和评论；每页固定两篇，三档显示大小都一样。列表区左右各三成点一下翻页，中间打开。封面比例和评论数来自服务器已有的数据，升级后点一次“刷新”就有；老缓存先按竖图显示。
  The Xiaohongshu list is now one note per row like Weibo and X: a large cover on the left (in its original proportions) with the avatar, author, time and image count, title, likes and comments on the right; two per page at every display size. The outer 30% on each side of the list turns the page and the middle opens. Cover ratios and comment counts come from data the server already has, so one Refresh after upgrading fills them in; older caches show portrait covers until then.
- 小红书、微博、X 点“刷新”拿到新列表后停在原来那一页，不再跳回第一页；要看最新的，双击顶部当前分段。
  Refreshing Xiaohongshu, Weibo or X now keeps the current page instead of jumping back to the first; double-tap the current segment at the top for the newest.
- X 的“为你推荐”刷新后也接上以前缓存的推文（和正在关注一样，最多 100 条），不再每次整个替换；为你推荐仍不标“新”。
  X For You now keeps earlier cached posts behind the fresh ones after a refresh (like Following, up to 100) instead of being replaced whole; For You still carries no "new" marks.
- 锁屏用壁纸时，底部白条只写书名和电量，不再显示读书进度。
  With a lock wallpaper, the bottom strip shows only the book title and battery, no longer the reading progress.
- 修正设置 → 同步的“手机网页填写”扫码后打开的是小红书那套表单（地址示例、“设备令牌”），填不了 WebDAV：设备回给网页的设置内容超过 512 字节被截断，网页读不出来就停在默认文案。现在回复上限放大，WebDAV 正确显示目录地址、用户名和密码三栏；以后网页读不到设备设置会直接提示刷新，不再显示默认文案。番茄小说页填了长地址时也有同样问题，一并修好。
  Fixed Settings > Sync "Fill in on phone" opening the Xiaohongshu form (its address example and "device token") instead of WebDAV: the settings the device sent the page exceeded 512 bytes and were cut, so the page fell back to its default texts. The reply limit is larger now and the page shows the WebDAV directory address, user name and password; if the page ever cannot read the device's settings it says to reload instead of showing defaults. The Tomato page had the same problem with long addresses and is fixed too.

## 2026-11-25 · Treader 1.2.6

- 小红书、微博、X 联网下载改成两条连接同时下：列表缩略图每批分成两半各走一条，帖子和笔记的大图两张一对同时取。一条连接卡住时另一条照样在收；两条各自留着复用，不会每次重新握手。
  Xiaohongshu, Weibo and X now download over two connections at once: each batch of list thumbnails is split between them, and post and note images are fetched two at a time. One stalled connection no longer holds up the other, and each keeps its connection for reuse instead of handshaking every time.

## 2026-11-25 · Treader 1.2.5

- 小红书、微博、X 的列表缩略图（还有小红书推荐的封面）改成成批下载：缺的图每 18 张用一个请求取回，不再一张一张各等一个来回（经 Cloudflare 隧道实测每张 1～3.6 秒）。一批传到一半断了，已经收全的图先存下，剩下的换新连接再要；一轮一张都没多收到就停下，翻到这页时再补。需要 rednote 0.3.4、weibo 0.1.6、x 0.1.5 或更新的服务器；旧服务器自动退回逐张取。
  Xiaohongshu, Weibo and X list thumbnails (and Xiaohongshu recommendation covers) download in batches: missing images come 18 to a request instead of a round trip each (1 to 3.6 s per image through the Cloudflare tunnel in testing). When a batch breaks midway, the complete images are kept and the rest asked for again on a fresh connection; a round that gains nothing stops until the page is visited again. Needs rednote 0.3.4, weibo 0.1.6, x 0.1.5 or newer; older servers fall back to one by one.
- 漫画页第一次没解出来（多半是内存一时不够）时，先放掉字形缓存和书库封面缓存再解一次，减少“这一页显示不了”。
  A comic page that fails to decode the first time (usually memory briefly short) is decoded once more after freeing the glyph cache and the library cover cache, so fewer pages fail to show.
- 修正 USB 传书结束后电脑找不到刷机口（也看不到串口日志），只有强制重启才恢复：传书时 USB 接口切给了 U 盘，重启前现在会先让电脑看到 U 盘拔出，再把接口交还刷机口。
  Fixed the computer finding no flashing port (and no serial log) after USB transfer until a forced restart: the USB interface was handed to the drive during transfer, and now, before restarting, the computer first sees the drive unplugged and the interface goes back to the flashing port.

## 2026-11-25 · rednote 服务器 0.3.4、weibo 服务器 0.1.6、x 服务器 0.1.5

- 新增设备接口 `POST /api/device/v1/imgs`：一次请求取回多张列表小图（缩略图、方形小图、推荐封面），按请求顺序拼在一起返回，没有的图长度为 0。设备逐张取时每张都要等一个来回（经 Cloudflare 隧道实测每张 1～3.6 秒），一页十几张就是十几个来回；配合之后的固件改成一批一个来回。旧固件和安卓 App 不受影响，照旧逐张取。
  New device endpoint `POST /api/device/v1/imgs` returns several list images (thumbnails, square images, recommendation covers) in one request, concatenated in request order with length 0 for missing ones. One by one, every image costs a round trip (1 to 3.6 s each through the Cloudflare tunnel in testing), a dozen or more per page; with the upcoming firmware a batch costs one. Older firmware and the Android apps are unaffected and keep fetching one by one.

## 2026-11-25 · Treader 1.2.4

- 锁屏时，书城或书库里的番茄书下载、漫画“检查更新”的下载不再被打断：不管设置里选的是浅睡、软睡还是关机，锁屏画面照常显示（触摸停掉，口袋里不误触），设备不睡、不关机，留着继续下，下完才按设置去睡或关机。按电源键就醒来，下载还在跑，要取消在页面上取消。下载 3 分钟没有进展、已经留了 90 分钟、或电池电压低于 3500 mV 时不再留着，先停掉下载再睡。其他后台联网任务锁屏时照旧取消。
  Locking no longer interrupts a bookstore or library Tomato download or a comic update download: whatever sleep mode is set (light, soft or off), the lock face shows as usual (touch is parked, so no pocket taps) but the device neither sleeps nor powers off and keeps downloading, then sleeps or powers off per the setting once it finishes. The power key wakes it with the download still running; cancel from the page. It stops holding when a download makes no progress for 3 minutes, after 90 minutes, or when the battery voltage drops below 3500 mV, and stops the download before sleeping. Other background network jobs are still cancelled at lock.
- 设置 → 同步新增“手机网页填写”：WebDAV 的地址、用户名和密码在手机网页上粘贴提交，和小红书、微博那页一样；密码留空表示不改，保存后设备自动收到。网页多了“用户名”一栏。
  Settings > Sync gains "Fill in on phone": paste the WebDAV address, user name and password on a phone web page, like the Xiaohongshu and Weibo pages; a blank password keeps the old one, and the device picks up the save by itself. The page gains a "User name" field.
- 验证器的口令换成 4 位数字 PIN，用和锁屏密码同款的整屏数字键盘，设置、解锁、修改都一样；解锁从约 9 秒降到约 1 秒，不再触发看门狗告警。以前设的字母数字口令仍能解锁，在设置 → 内容来源 → 验证器的“修改 PIN”（旧的显示“换成 PIN”）里换成 PIN，换了以后旧口令就解不开了。输错 4 次后要等的限制照旧。注意：4 位数字只有 1 万种，防得住别人拿起设备随手试，防不住同时拿到 TF 卡和设备、专门破解的人。
  The authenticator's passphrase is now a 4-digit PIN on the same full-screen keypad as the lock PIN, for setting, unlocking and changing alike; unlocking drops from about 9 s to about 1 s and no longer trips the watchdog. A passphrase set earlier still unlocks, and is switched to a PIN with "Change PIN" (shown as "Switch to PIN" for the old one) under Settings > Content sources > Authenticator, after which the old passphrase opens nothing. The wait after four wrong tries is unchanged. Note: four digits are only 10,000 combinations, which stops someone picking up the device and trying but not a determined attacker holding both the TF card and the device.

## 2026-11-25 · Treader 1.2.3

- 联网更快更稳：任务做完 Wi-Fi 先留 20 秒，切帖子、翻页补图直接用，不再每次重新连网握手（实测每次多 15 秒以上）；同一服务器重连走 TLS 简化握手，椭圆曲线运算加速；开 TCP 选择性确认，丢包后只补缺的。锁屏、传书前照样先断开 Wi-Fi。
  Faster, steadier networking: Wi-Fi stays up for 20 seconds after a job, so switching posts or filling thumbnails reuses it instead of rejoining and redoing the handshake (measured at 15 s or more each time); reconnects to the same server take the short TLS handshake and elliptic-curve math is faster; TCP selective acknowledgements resend only what was lost. Wi-Fi is still dropped before a lock or transfer.
- 帖子图片传到一半停住、或连接悄悄断了，自动换新连接重试一次，图片等待从 15 秒缩到 8 秒。
  An image that stalls midway or whose connection died silently is retried once on a fresh connection, and image waits drop from 15 to 8 seconds.
- 小红书、微博、X：设置里的“列表条数”改名“每次拉取”，选项改成 20 / 30 / 50，默认 20；刷新只先下前两页的缩略图，往后翻时自动补当前页和下一页，刷新几十秒就结束。
  Xiaohongshu, Weibo and X: the "List size" setting is renamed "Fetch size" with options 20 / 30 / 50, 20 by default; a refresh fetches thumbnails for the first two pages only and the current and next page fill in as you go, so a refresh ends in seconds.
- 小红书关注、微博关注、X 正在关注：刷新后保留以前缓存的条目，新拉的排在前面，列表最长 100 条（缓存上限也是 100）；超出的最老的条目掉出列表，缓存随即清理，所以列表和缓存都不会越积越多。推荐、热门、为你推荐仍每次整个替换。
  Xiaohongshu Following, Weibo Following and X Following: a refresh keeps earlier cached entries behind the new ones, up to 100 in the list (the cache limit is also 100); the oldest fall off the end and are pruned, so neither the list nor the cache grows without bound. Recommendations, Hot and For You are still replaced whole.
- 虎扑：设置里新增“关注每个专区”取 1 / 2 / 3 页；不开图片时列表按标题行数排，一页多放一两条，下面不再空一大块。
  Hupu: a new "Pages per zone" setting (1 / 2 / 3) for the following feed; with images off, list rows follow the title length, so a page fits one or two more and no large blank is left.
- 修正漫画发现里输入关键词后点键盘“搜索”键直接退到“全部”页、搜索提交不了：这个页面右下角有一块看不见的菜单把手点击区，正好压在“搜索”键上，现在让给页面。
  Fixed the comic store's search keyboard: tapping its Go key jumped to the All page and the search never submitted, because an invisible menu-handle area in the bottom-right corner covered the key; it now yields to the page.
- 漫画发现的封面也按页载入：刷新、切分类、搜索只先下前两页的封面，翻页时补当前页和下一页，不再一口气下完整个列表。
  The comic store's covers load by page too: a refresh, category change or search fetches covers for the first two pages only, and paging fills the current and next page instead of downloading the whole list at once.
- 漫画“检查更新”时，我的漫画顶部显示状态卡片：第几话、这一步在做什么（服务器转换中、排队、下载中、连不上等重试）和进度条，做完显示结果，点一下收起。
  During a comic update, My comics shows a status card at the top: which episode, the step under way (converting on the server, queued, downloading, waiting to retry) and a progress bar, then the result, dismissed with a tap.
- TF 卡文件包新增中日韩字体 Noto Sans CJK SC（可变 TTF，SIL OFL），在“字体”页选用后能显示日文和韩文。
  The TF card pack adds the Noto Sans CJK SC font (variable TTF, SIL OFL); choose it on the Fonts page to show Japanese and Korean.

## 2026-11-25 · Treader 1.2.2

- 书的目录右上角的“正序 / 倒序”换成跳转按钮：不在最后一页时写“最后一章”，点了翻到目录最后一页；在最后一页时写“第一章”，点了回到第一页。目录始终按正序排，只有一页时没有这个按钮。
  The book TOC's "Asc / Desc" button becomes a jump button: "Last" before the last page turns to the TOC's last page, and "First" on the last page returns to page one. The TOC always stays in order, and a one-page TOC has no button.

## 2026-11-25 · Treader 1.2.1

- 锁屏底部按睡眠方式写怎么醒：关机模式写“长按电源键开机”（短按只亮红灯，原来一律写“按电源键或拿起设备继续”，看着像睡死了），软睡写“按电源键开机”，浅睡没开拿起唤醒时只写“按电源键继续”。
  The lock screen's bottom line now says how to wake for the sleep mode: power-off reads "Hold power to turn on" (a short press only lights the red LED, and the old one-size "Press power or pick up to continue" made it look hung), soft sleep "Press power to turn on", and light sleep without pick-up wake just "Press power to continue".
- 修正浅睡模式下按电源键锁屏后偶尔睡死（红灯亮、屏幕没反应，要长按关机）：后台还在联网（刚刷新或在下载）或在传书页时，浅睡前 Wi-Fi 没关。现在每次锁屏都先停掉后台联网和传书服务，和深睡一样。
  Fixed occasional hangs after locking with the power key in light sleep mode (red light on, screen unresponsive, only a long press to power off helps): Wi-Fi stayed on into light sleep while a background job was still online (right after a refresh or during a download) or on the transfer page. Every lock now stops background networking and the transfer service first, as deep sleep does.
- 书的目录右上角加“正序 / 倒序”按钮，点一下换顺序，停在在读这章所在的那页；本次开机内记住。
  The book TOC gains an "Asc / Desc" button at the top right that flips the order and lands on the page with the chapter being read; it is remembered until restart.
- 小红书、微博、X 帖子里的大图：连不上服务器时先再试一次，还连不上就停下，不再每张图各等一轮超时；没下载成功的图写“图片没下载成功”和原因，点这里重试（原来一直停在“图片还没下载”）。小红书推荐的笔记，服务器还在做的大图先跳过、隔两秒再来一轮（最多 90 秒），不再等 15 秒超时就算失败。
  Full images in Xiaohongshu, Weibo and X posts: a connection that fails is tried once more, and if the server still cannot be reached the download stops instead of waiting out a timeout for every image; a missing image says "Image not downloaded" with the reason and offers a retry (it used to stay on "not downloaded yet"). For recommended Xiaohongshu notes, images the server is still making are skipped and fetched in another round two seconds later (up to 90 seconds) instead of failing after a 15-second timeout.
- 微博、X 列表按屏幕剩下的空间排版：一页尽量多装一条，剩下的空白先把图片放回正常大小，再多显示几行正文，最后把图片放大，不再一条下面留一大片空白；三档显示大小都一样处理。列表区像看书一样分左中右：点左右两边翻页，点中间打开，底部翻页按钮还在。
  Weibo and X lists now lay out to the space left on screen: a page fits one more post where it can, and the leftover space first brings images back to normal size, then shows more lines of text, then enlarges images, instead of leaving a large blank under a post; all three display sizes work the same way. The list area splits like a book page: tap the sides to turn pages and the middle to open; the bottom pager stays.

## 2026-11-25 · rednote 服务器 0.3.3

- 缩略图接口加 `?wait=0`：这篇的图还在做时不等，直接说没有。配合 Treader 1.2.0，小红书拉列表不再被还没做好的缩略图卡住。
  The thumbnail endpoint takes `?wait=0` and answers at once when a note's images are still being made, so with Treader 1.2.0 a Xiaohongshu list fetch no longer stalls on unfinished thumbnails.

## 2026-11-25 · Treader 1.2.0

- 番茄并进书库，照漫画的样子：底部和“全部”页不再有“番茄”。设置了番茄服务器时，书库右上角多出“书城”；状态行右边是“检查更新”，有新章节后变成“下载 N 本更新”，在跑时可以“取消”；长按番茄书，详情里多了“检查这本更新”，只查这一本、有新章节直接下载。来源胶囊改叫“全部 / 本地 / 番茄”。原来放在底部的番茄自动去掉。
  Tomato moved into the library, the way comics work: the bar and the All page no longer have Tomato. With a Tomato server set, "Store" appears at the library's top right; the status line offers "Check for updates", then "Download updates (N)" once new chapters are found, with "Cancel" while it runs; a Tomato book's long-press detail gains "Check this book", which checks just that one and downloads its new chapters. The source pill reads All / Local / Tomato. A Tomato tab pinned to the bar is dropped automatically.
- 书城顶上改成“‹ 书城”，返回书库；搜索栏往上挪，分类多露出一些。
  The store's top row is now "‹ Store" back to the library; the search bar moves up and more categories show.
- WEBTOON 作品详情：下载漫画时“检查更新”变成“停止下载”，下载卡住时能手动停下，别的功能就能用了。
  WEBTOON title details: while comics download, "Check for updates" becomes "Stop download", so a stuck download can be stopped and other features become usable.
- 联网下载快很多：联网期间关掉 Wi-Fi 省电（Wi-Fi 本来只在后台任务联网时开着），TCP 接收窗口从 5.6 KB 加到 32 KB（缓冲在 PSRAM）。番茄下载从每秒几百字节到约 100 KB/s，小红书、微博、漫画等拉图和连已有 Wi-Fi 传书也一起变快。下载中途断流不再报 ESP_FAIL 从头来，已经收到的留着，从断的地方接着下，只要还有进展就一直接。
  Network downloads are much faster: Wi-Fi power save is off while online (Wi-Fi is only up during background jobs anyway) and the TCP receive window grows from 5.6 KB to 32 KB (buffers in PSRAM). Tomato downloads go from a few hundred bytes a second to about 100 KB/s, and images for Xiaohongshu, Weibo, comics and the rest, plus book transfer over an existing network, speed up too. A mid-download stall no longer ends in ESP_FAIL and a restart: what arrived is kept and the download continues from where it stopped, for as long as it keeps making progress.
- 修正番茄书下载后没有封面：搜索给的封面是设备解不了的 HEIC，改要不带签名的 300×400 JPEG；受限的封面没有这种地址，从番茄书籍网页开头取带签名的 JPEG 地址。已经下好、没有封面的书，在书库点“检查更新”（一次最多补 10 本）或长按点“检查这本更新”时补上封面。
  Fixed Tomato books arriving without a cover: search hands out HEIC covers the device cannot decode, so the device now asks for an unsigned 300x400 JPEG; restricted covers with no such address take the signed JPEG address from the top of the Tomato book page. Books already downloaded without a cover get one from the library's "Check for updates" (up to 10 per run) or a book's "Check this book".
- 修正小红书拉取时一直“拉取中”：服务器还在做的图片，设备超时后不再换个连接再等一轮。
  Fixed Xiaohongshu fetches stuck on "Fetching": when the server is still making an image, the device no longer waits a second round on a new connection after timing out.

- 设置首页新增“Wi-Fi”：扫描、输密码保存后马上试连一次并告诉你结果，也能测试连接、遗忘网络。页面写明只能保存一个网络，连新网络会替换旧的。
  New Wi-Fi entry at the top of Settings: scan, save a network with its password and get an immediate test result, test the connection or forget the network. The page says only one network is kept and a new one replaces it.
- 小红书、微博、X、WEBTOON、番茄书城、虎扑拉封面和图片时同一个服务器只握手一次，不再每张图都重新建连接，拉取快很多。
  Covers and images for Xiaohongshu, Weibo, X, WEBTOON, the Tomato store and Hupu reuse one connection per server instead of reconnecting for every image, so fetching is much faster.
- 拉取都能取消：小红书、微博、X 右上角显示“拉取中 · 取消”，虎扑、WEBTOON 发现是“加载中 · 取消”，书城取榜单和番茄搜索时也有“取消”，点一下就停，已经下好的留着。别的标签在拉取时，本页的“刷新”照样能点，会先让那边停下。
  Every fetch can be cancelled: Xiaohongshu, Weibo and X show "Fetching · Cancel", Hupu and the WEBTOON store "Loading · Cancel", and the store's rank fetch and Tomato search have "Cancel"; what already arrived stays. While another tab fetches, Refresh still works here and asks that tab to stop first.
- 小红书、微博、X 和 WEBTOON 发现页拉列表时，当前这一页的缩略图一到就显示，不用等后面的全部下完。
  While Xiaohongshu, Weibo, X and the WEBTOON store fetch a list, the current page's thumbnails show as soon as they arrive instead of waiting for everything after them.
- 小红书拉关注列表时只取服务器已经做好的缩略图，还在做的跳过、下次刷新补上，不再一张张卡十几秒（需要 rednote 服务器 0.3.3）。
  Xiaohongshu following lists take only thumbnails the server has finished; unfinished ones are skipped until the next refresh instead of stalling for seconds each (needs rednote server 0.3.3).
- 设置 › 内容来源 › 番茄小说新增“手机网页填写”：手机扫码打开网页，粘贴服务器地址和设备密码，和小红书一样。
  Settings › Content sources › Tomato gains the phone web form: scan the code and paste the server address and device password, as with Xiaohongshu.
- 设置 → 显示与阅读新增“文字深浅”：标准 / 深 / 更深，界面文字和阅读正文一起加粗，说明、摘要这类灰字也跟着变黑。
  New Settings › Display & reading › Text darkness: Standard / Dark / Darker thickens UI and reading text together and darkens gray notes and summaries.
- 验证器在“全部”页和底部标签设置里排在设置前面；设置固定在底部时永远是最后一个。
  The authenticator now sits before Settings on the All page and in the tab-bar settings; a pinned Settings is always last.
- 后台联网时刷屏多留余量，修正书城边下封面边翻页时偶尔整屏刷白、之后刷屏变慢。
  The display keeps more margin while the background is online, fixing an occasional white flash followed by slower refreshes when the bookstore fetched covers during a page turn.
- 波形表、字体列表、图片队列和几个下载缓冲挪到 PSRAM，给 Wi-Fi 和加密连接多留约 57 KB 内部内存。
  Waveform tables, the font list, image queues and several download buffers moved to PSRAM, leaving about 57 KB more internal memory for Wi-Fi and encrypted connections.

## 2026-11-25 · Treader 1.1.2

- 修正虎扑、番茄书城等联网时报“HTTPS 握手失败”（在线升级却能连）：用 RSA 证书的站点验签时内部内存不够，现在加密连接改用 PSRAM。
  Fixed "HTTPS handshake failed" on Hupu, the Tomato store and others while online update still worked: signature checks for RSA-certificate sites ran out of internal memory; encrypted connections now use PSRAM.

## 2026-11-23 · Treader 1.1.1

- 设置 → 显示与阅读新增“翻页刷新”：正常（原来的效果）或快速。快速档正文和页脚一次刷完，并改用 8 级灰阶，翻一页快约四分之一，字边缘的灰阶少一些。
  New Settings › Display & reading › Page refresh: Normal (as before) or Fast. Fast refreshes the text and footer in one pass with 8 gray levels, turning pages about a quarter faster with fewer gray steps on letter edges.

## 2026-11-25 · webtoon 服务器 0.3.3

- 面板新增“缓存”页：列出服务器上还留着的转好的话（系列名、话名、页数、大小、转好时间，新的在前），点“下载”用浏览器把 CBZ 存到电脑，文件名是“系列名 第N话.cbz”，再拷进 TF 卡的 `comics` 文件夹。下载不会刷新这一话的“最近用过”时间，所以不会因此逃过自动清理。设备接口和固件、安卓 App 都不用改。
  The panel has a new Cache page listing the converted episodes still kept on the server (series, episode title, pages, size and conversion time, newest first); "下载" saves the CBZ in the browser as "series 第N话.cbz" to copy into the TF card's `comics` folder. A download does not refresh the episode's last-used time, so it does not escape pruning. No device, firmware or Android change.

## 2026-11-23 · webtoon 服务器 0.3.2

- E-Hentai 改成填 cookie 登录，去掉用户名密码登录：登录页整站要过 Cloudflare 验证，服务器和 FlareSolverr 都过不去，而且 FlareSolverr 会把提交的密码记进日志。在浏览器登录后，把 cookie 整段粘贴到面板（Cookie 请求头或浏览器导出的 JSON 都行），面板自动挑出要用的几项；保存时服务器当场试一次，告诉你能不能用，能用时顺便取好 ExHentai 的 igneous。
  E-Hentai sign-in now takes cookies instead of a user name and password: the sign-in site sits behind a Cloudflare check that neither the server nor FlareSolverr passes, and FlareSolverr logs the submitted password. Sign in in a browser and paste the cookies into the panel whole (a Cookie header or a browser's JSON export); the panel picks out the ones it needs, and on save the server tries them at once, says whether they work and, if so, fetches ExHentai's igneous.

## 2026-11-23 · webtoon 服务器 0.3.1

- 修正 E-Hentai 账号登录总说“用户名或密码不对”：登录页所在的论坛整站要过 Cloudflare 验证，服务器的请求被拦下了。现在被拦时改由 FlareSolverr 的浏览器提交登录（`.env` 的 `COMPOSE_PROFILES` 里要有 `flaresolverr`），没开时提示怎么打开或手动填 cookie；其他失败也照实说原因。
  Fixed E-Hentai sign-in always saying the user name or password was wrong: the forum hosting the sign-in page sits behind a Cloudflare check that blocked the server's request. A blocked sign-in now goes through FlareSolverr's browser (`flaresolverr` must be in `COMPOSE_PROFILES` in `.env`), and without it the error says how to turn it on or paste cookies by hand; other failures report their real cause.

## 2026-11-23 · webtoon 服务器 0.3.0

- 新增 E-Hentai 通道：服务器自己读 e-hentai.org（登录后也能读 exhentai.org），不用插件。在面板的 E-Hentai 页打开后，设备和 App 的来源里多一个 E-Hentai，有人气、最新和搜索，可以设默认过滤（比如只看中文）和不显示的分类。一部画廊按张数切成几话（默认 80 张一话）。账号在面板里用用户名和密码登录，服务器只保存登录 cookie。服务器连不上 e-hentai.org 时可以打开“直连 IP”。图片额度用完或 IP 被暂时封了时，排着的话 10 分钟内直接失败。固件和 App 不用升级。
  New E-Hentai channel: the server reads e-hentai.org itself (and exhentai.org once signed in), with no extension. Turned on in the panel's E-Hentai page, it adds E-Hentai to the device and app sources with Popular, Latest and search, a default filter (such as Chinese only) and hidden categories. Galleries are split into episodes by image count (80 per episode by default). Sign in with the user name and password in the panel; the server keeps only the sign-in cookies. Turn on Direct IP when the server cannot reach e-hentai.org. When the image quota runs out or the IP is temporarily banned, queued episodes fail at once for 10 minutes. No firmware or app update needed.

## 2026-11-22 · Treader 1.1.0

- 漫画“检查更新”可以取消：更新中点“更新中”胶囊，变成“再点取消”，再点一下就停；没下好的话下次检查更新接着下。显示放大时“我的漫画”上面三个胶囊不再叠在一起。
  Comic updates can be cancelled: tap the "Updating" pill, which turns into "Cancel?", and tap again to stop; unfinished episodes resume on the next check. At larger display sizes the three pills above My comics no longer overlap.
- 新增英文界面版固件：发布里带 `-en` 的 `treader-<版本>-full-en.bin` / `-upgrade-en.bin`，功能和中文版一样，设备网页（传书、验证器导入、服务器设置）也是英文。中文版在线升级只升到中文版，英文版只升到英文版；想换语言就刷另一版的升级包，设置和进度保留。网站和 README 加了英文版。
  New English firmware: `treader-<version>-full-en.bin` / `-upgrade-en.bin` in the releases, the same features as the Chinese build, with English device web pages (transfer, authenticator import, server setup). The Chinese build updates online only to Chinese and the English only to English; to switch, flash the other build's upgrade package, which keeps settings and progress. The website and README have English versions.

## 2026-11-22 · webtoon 服务器 0.2.4

- 插件来源过不了 Cloudflare 验证时不再拖住整个队列：一个来源失败后，它排着的话 10 分钟内直接失败，不再每话等 FlareSolverr 超时，别的来源和 WEBTOON 照常下。错误里注明是 FlareSolverr 没过去（IP 被拦或验证解不了）和还要等几分钟。
  An extension source that fails its Cloudflare check no longer holds up the whole queue: its queued episodes fail at once for 10 minutes instead of each waiting for FlareSolverr to time out, while other sources and WEBTOON keep downloading. The error says FlareSolverr did not get through (blocked IP or an unsolvable check) and how many minutes are left.

## 2026-11-22 · Treader 1.0.9 · 番茄安卓 App 1.3.2

- 书库可以在列表和封面网格之间切换：右上角搜索左边的图标，选择会记住。网格一行三本，封面下面是书名和进度，番茄新章节标在封面右上角；管理书库也能用网格，选中的描粗框。固件上网格一页 6 本（特大显示且有“继续阅读”卡片时 3 本），App 上滚动显示。
  The library can switch between a list and a cover grid with the icon left of search, and remembers the choice. The grid shows three books per row with the title and progress under each cover and Tomato's new-chapter badge on the cover's top right; manage mode works in the grid too, with a thick outline on selected books. On the firmware the grid holds 6 books per page (3 at the largest display size with the continue card); the app scrolls.

## 2026-11-22 · 番茄安卓 App 1.3.1

- 修正番茄下载的书只显示“Part 1、Part 2…”，章节识别和固件 1.0.8 一样：章名在 40 个字以内都能认出，行首有空格或全角空格的章名也能认出。已经打开过的书重新打开后生效。
  Fixed Tomato books showing only "Part 1, Part 2…", with the same chapter detection as firmware 1.0.8: titles up to 40 characters are recognized, including ones indented with spaces or ideographic spaces. Takes effect when a book is reopened.
- 阅读页脚照固件加一条胶囊形全书进度条，工具条里的进度条也改成胶囊形。
  The reading footer gains a capsule whole-book bar like the firmware, and the toolbar's bar is a capsule too.

## 2026-11-22 · Treader 1.0.8

- 阅读页脚改小：上面一条胶囊形进度条，下面一行小字（章名、页码 · 百分比），每页能多放一两行字。打开工具条时的进度条也改成胶囊形，章节分界是条上的白色细缝。
  Slimmer reading footer: a capsule progress bar above one small line (chapter, page · percent), so each page holds one or two more lines. The toolbar's progress bar is a capsule too, with chapter boundaries as thin white gaps.
- 修正番茄下载的书只显示“Part 1、Part 2…”：章名超过十几个字时以前认不出章节，现在章名在 40 个字以内都能认出（目录里只显示前十几个字）；行首有空格或全角空格的章名也能认出。已经在书库里的书重新打开后生效。
  Fixed Tomato books showing only "Part 1, Part 2…": chapter titles longer than about a dozen characters were not recognized; titles up to 40 characters now are (the table of contents shows the first dozen or so), as are titles indented with spaces or ideographic spaces. Takes effect when a book is reopened.
- 修正换了 TF 卡字体后“A−”、自动翻页的“−”等按钮显示成方框：字体缺这些符号时换成长得像的字。
  Fixed boxes on buttons such as "A−" and the auto-turn "−" with some card fonts: missing symbols fall back to look-alikes.

## 2026-11-22 · WEBTOON 安卓 App 1.3.0

- 我的漫画可以在列表和三列封面网格之间切换（分段右边的按钮），选择记在本机。
  The shelf switches between the list and a three-column cover grid (the button right of the segments); the choice is remembered on the phone.
- 阅读工具条的进度条可以点：点在哪就跳到那一页，和固件一样。
  The reader toolbar's progress bar can be tapped to jump to that page, as on the firmware.
- 修正点进一部漫画总是“继续看第 1 话”：以前接着看的是第一个没看完的话，前面有没看过的话就总落在它上面。现在照固件，接着看最近看的那一话，看完了就下一话。进度里多记最后看的时间，旧进度照样能读。
  Fixed opening a series always offering to continue episode 1: it used to pick the first unfinished episode, so any unread earlier episode won. It now follows the firmware and continues the most recently read episode, or the next one when that is finished. Progress now also records when each episode was last read; older progress still loads.

## 2026-11-22 · webtoon 服务器 0.2.3

- 新增可选的 FlareSolverr：插件站点要过 Cloudflare 验证时（报错“Cloudflare bypass currently disabled”），在 `.env` 里写 `COMPOSE_PROFILES=mihon,flaresolverr`，多起一个 FlareSolverr 替插件过验证。没开时这类错误后面会提示怎么打开。需要换新的 `docker-compose.yml`。
  Added optional FlareSolverr: when an extension site needs a Cloudflare check ("Cloudflare bypass currently disabled"), set `COMPOSE_PROFILES=mihon,flaresolverr` in `.env` to add FlareSolverr, which passes the check for extensions. Without it, such errors now say how to turn it on. Needs the new `docker-compose.yml`.

## 2026-11-22 · Treader 1.0.7

- WEBTOON 检查更新时，失败的话下次检查会自动重下：“下载到第几话”只推进到第一话没下好的前面（失败、取消、超过每次上限），以前会越过失败的话、只能靠“补全缺的话”找回。卡上已有的话照旧跳过。
  WEBTOON update checks now retry failed episodes on the next check: the downloaded-through episode only moves up to just before the first episode not downloaded (failed, cancelled or past the per-check cap). Before, it moved past failures and only 补全缺的话 brought them back. Episodes already on the card are still skipped.
- 修正设备上订阅插件来源的作品失败：作品编号是 m 开头，订阅时被当成无效编号。
  Fixed subscribing to extension-source titles on the device: their ids start with m and were rejected as invalid.

## 2026-11-22 · WEBTOON 安卓 App 1.2.0

- 已订阅作品的详情里加“补全缺的话”，照固件：按服务器的全部话列表，下手机上没有的，包括订阅起点之前的和自己删掉的，同样受每次上限限制。失败的话本来就会在下次检查更新时重下。
  Added 补全缺的话 to subscribed title details, like the firmware: every episode in the server's list that is not on the phone is downloaded, including ones before the subscription start and ones you deleted, within the per-check cap. Failed episodes were already retried on the next update check.

## 2026-11-22 · webtoon 服务器 0.2.2

- 修正插件来源的话号顺序：有的插件按章节名字符串排章节（“第100话”排在“第10话”前面），服务器照搬后“从第 1 话订阅”下到的是第 100 话。现在大半章节有章节号时按章节号编话号。已经编错的作品在面板的作品详情里点“重新编号”，再在设备或 App 上取消订阅、删掉下错的话后重新订阅。
  Fixed episode order for extension sources: some extensions sort chapters by name as strings (第100话 before 第10话), and the server copied that order, so subscribing "from episode 1" downloaded chapter 100. Episodes are now numbered by chapter number when most chapters have one. For a title already numbered wrong, press 重新编号 in its panel detail, then unsubscribe on the device or app, delete the wrong episodes and subscribe again.

## 2026-11-22 · WEBTOON 安卓 App 1.1.1

- 修正平板和大字号上发现页只排一行、下面空一大块，封面下的小字被截掉一半：两行字的高度改用设备上真实的字体量，排得下就多排一行（封面缩小一点），换过系统字体、调大字号也能排满。
  Fixed the store showing a single row with a large gap below and the line under each cover cut in half on tablets and with large text: the two text lines are now measured with the device's real font, and another row is added when it fits by shrinking the covers a little, so the grid fills the screen with replaced system fonts and larger text too.

## 2026-11-22 · webtoon 服务器 0.2.1

- 服务器不再按内容分级替用户过滤插件来源：装好的来源（包括标成成人的）都发给设备和 App，用哪个由用户自己选；面板去掉“显示成人来源”开关，只在“设备上的来源”里标“成人”。面板里手动勾掉的来源照旧不发。
  The server no longer filters extension sources by content rating: every installed source, adult-rated ones included, reaches devices and the app, and the user chooses; the panel drops the "显示成人来源" switch and only labels such sources "成人" under "设备上的来源". Sources unticked in the panel are still left out.

## 2026-11-22 · WEBTOON 安卓 App 1.1.0

- 和固件 1.0.6 一样，发现页支持 [webtoon 服务器 0.2.0](../webtoon/README.md) 的插件来源：服务器开了 Mihon 插件时多一行“来源”，可以在 WEBTOON 和各个插件来源之间切换；插件来源按“人气 / 最新”逛，搜索只搜当前来源，订阅、检查更新和下载照旧。服务器没开插件或是旧版本时界面不变。
  As in firmware 1.0.6, the store supports the extension sources of [webtoon server 0.2.0](../webtoon/README.md): with Mihon extensions on the server a "来源" row switches between WEBTOON and each extension source; extension sources browse by Popular / Latest, search covers the current source only, and subscribing, update checks and downloads work as before. Without extensions on the server, or with an older server, nothing changes.

## 2026-11-22 · Treader 1.0.6

- 发现漫画支持 [webtoon 服务器 0.2.0](../webtoon/README.md) 的插件来源：服务器开了 Mihon 插件时，搜索栏右边多一个来源按钮，可以在 WEBTOON 和各个插件来源之间切换。插件来源按“人气 / 最新”逛，搜索只搜当前来源；订阅、检查更新和下载和 WEBTOON 一样，下好的话放进我的漫画。服务器没开插件或是旧版本时，界面和以前一样。
  The comic store supports the extension sources of [webtoon server 0.2.0](../webtoon/README.md): when the server runs Mihon extensions, a source button right of the search bar switches between WEBTOON and each extension source. Extension sources browse by Popular / Latest and search covers the current source only; subscribing, update checks and downloads work as for WEBTOON, with episodes landing on the comic shelf. Without extensions on the server, or with an older server, the page looks as before.

## 2026-11-22 · webtoon 服务器 0.2.0

- 可选的插件来源：在 `.env` 里写 `COMPOSE_PROFILES=mihon`，会多起一个 Suwayomi-Server 运行 Mihon / Tachiyomi 插件。在面板的“插件”页添加你自己的仓库、安装插件，再选哪些来源出现在设备上。设备接口多了 `/sources` 和 `src` 参数，插件作品的编号以 `m` 开头；订阅、检查更新和下载照旧，没升级的设备和 App 不受影响。本项目不提供任何插件仓库，只添加你有权访问的来源。详见 [webtoon/README](../webtoon/README.md)。
  Optional extension sources: with `COMPOSE_PROFILES=mihon` in `.env`, a Suwayomi-Server runs Mihon / Tachiyomi extensions. The panel's 插件 page adds your own repositories, installs extensions and picks which sources devices see. The device API gains `/sources` and a `src` parameter, and extension titles have ids starting with `m`; subscriptions, update checks and downloads work as before, and devices and apps that have not been updated are unaffected. This project ships no extension repository; only add sources you have the right to access. See [webtoon/README](../webtoon/README.md).
- 普通漫画（一张图一页）不再按长条切：裁掉白边后整页缩进屏幕，横向跨页默认拆成两页、右页在前（面板可改）；长条照旧在分镜之间的空白处切。插件章节由服务器分配固定的整数话号，同一话有多个汉化组版本时默认只留一个。
  Ordinary one-image-per-page comics are no longer cut like strips: white borders are trimmed and each page fits the screen, and wide spreads split into two pages, right first by default (changeable in the panel); strips are still cut in the gaps between panels. The server gives extension chapters fixed integer numbers and by default keeps one version when several scanlation groups release the same chapter.

## 2026-11-22 · Treader 1.0.5 · WEBTOON 安卓 App 1.0.1

- 修正：检查更新时连不上 webtoon 服务器（断网、域名查不到、手机锁屏后后台没网），不再把剩下每一话都试一遍、报一长串失败；先等 10 秒再试一次，还不行就停下并说明，下次检查更新从断的那一话接着下。只和某一话有关的失败（服务器说不能看、转换出错）仍然跳过它接着下。
  Fix: when an update check cannot reach the webtoon server (offline, name lookup failed, no background network after the phone locks), it no longer tries every remaining episode and lists a long run of failures; it waits 10 s and tries once more, then stops and says so, and the next check resumes from that episode. Failures that concern only one episode (the server refuses it, conversion fails) are still skipped.

## 2026-11-22 · Treader 1.0.4

- 漫画新增 WEBTOON 发现和追更：连自建的 [webtoon 服务器](../webtoon/README.md)，在我的漫画右上角“发现”里逛排行、类型、星期更新表、已完结，或搜索（打简体也能搜到繁体中文版）；点开作品看简介和免费话数，选“从第 1 话 / 只追新的 / 最近 5 话”订阅。服务器把每一话的竖长条转成灰度、在分镜之间的空白处按屏幕切页，设备下载成 CBZ 放进我的漫画，阅读和本地漫画一样快。
  Comics gain a WEBTOON store and follow-along: with a self-hosted [webtoon server](../webtoon/README.md), "发现" at the comic shelf's top right browses rankings, genres, the weekday schedule and completed titles, or searches (simplified input finds the traditional Chinese edition); a title shows its summary and free episode count and subscribes "from episode 1 / new only / latest 5". The server grays each episode's tall strip and cuts it into screens in the gaps between panels; the device downloads CBZ files onto the comic shelf, read as fast as local comics.
- 订阅存在作品文件夹里的 `.webtoon` 标记，服务器不保存。我的漫画加“全部 / 本地 / WEBTOON”筛选和“检查更新”，订阅的封面角上标“WT”，新下好没看的写“N 话新”；长按系列可以检查这部的更新、补全缺的话或取消订阅（下载好的留着）。
  Subscriptions are `.webtoon` markers in each title's folder; the server keeps none. The comic shelf adds an All / Local / WEBTOON filter and "检查更新"; subscribed covers carry a "WT" tag and unread new downloads show "N 话新"; a long press on a series checks its updates, fills missing episodes or unsubscribes (downloads stay).
- 设置 › 内容来源 › 漫画：手机网页填写服务器地址和令牌、测试连接、语言版本（繁体中文 / English）、简体显示、每次最多下载几话。只拉网页上公开免费的话，请自己部署、只给自己用。
  Settings › Content sources › 漫画: server address and token by phone, connection test, language version, simplified display and episodes per check. Only free public episodes are fetched; self-host for personal use only.

## 2026-11-21 · 番茄安卓 App 1.3.0

- 和固件 1.0.3 一样，阅读工具条的进度条两边加了“上一章”“下一章”，点了跳到那一章开头并收起工具条；第一章和最后一章对应的按钮变灰。
  As in firmware 1.0.3, the reading toolbar gets "previous chapter" and "next chapter" beside the progress bar, jumping to that chapter's start and closing the toolbar; the button greys out on the first or last chapter.

## 2026-11-21 · Treader 1.0.3

- 阅读工具条的进度条两边加了“上一章”“下一章”，点了直接跳到那一章开头；第一章和最后一章对应的按钮变灰。
  The reading toolbar gets "previous chapter" and "next chapter" beside the progress bar, jumping straight to that chapter's start; the button greys out on the first or last chapter.
- 修正浅睡时按电源键有时怎么按都叫不醒、只能拿起唤醒的问题：醒来读电源芯片失败会重试，积压的事件都清干净，清不掉时改为定时检查按键。万一还是按不醒，按住电源键 10 秒强制关机，再长按开机。
  Fixed light sleep sometimes ignoring the power key so that only pickup could wake it: a failed read of the power chip after waking is retried, its event backlog is cleared, and if it will not clear the key is checked on a timer. If the key still does nothing, hold it for 10 seconds to force power-off, then long-press to power on.
- 开机不再占用约 800 KB 没人用的内存（连续 DU 状态表，原来只给已删掉的触摸演示页用），看书、看图时内存更宽裕。
  About 800 KB of unused memory (the continuous DU state table, used only by the removed touch demo) is no longer taken at boot, leaving more room for reading and images.

## 2026-11-21 · 番茄安卓 App 1.2.0

- 和固件 1.0.2 一样，看书时点屏幕翻页的区域可以选了（设置 → 翻页区域，点一下换一种）：左右（默认）、中下翻页、点哪都翻。只存在手机上，不和设备同步。
  As in firmware 1.0.2, the reader's tap zones are a setting (Settings > Page turn zones, tap to cycle): sides (default), lower or anywhere. Stored on the phone only, not synced with the device.

## 2026-11-21 · Treader 1.0.2

- 看书时点屏幕翻页的区域可以选了（设置 → 显示与阅读 → 翻页区域）：左右（默认，和以前一样）、中下翻页（点上面开工具条，下面左边一条上一页、其余下一页）、点哪都翻（点上面开工具条，其余都是下一页）。选项旁边画了示意图。
  The reader's tap zones are now a setting (Settings > Display & reading > Page turn zones): sides (default, as before), lower (top opens the toolbar; below, a left strip goes back and the rest forward) or anywhere (top opens the toolbar; the rest goes forward). Each choice has a sketch beside it.

## 2026-11-21 · Treader 1.0.1

- 虎扑加“显示图片”开关（设置 → 内容来源 → 虎扑），默认关：只看文字，不下载也不解码图片；打开前提醒会大大增加设备负担。图片下载按实际大小申请内存（原来每张先要 1 MB，内存碎了就下不了），没下好的图会说明原因，点图重试。
  Hupu gets a "show images" switch (Settings > Content sources > Hupu), off by default: text only, no image downloads or decoding; turning it on warns about the much heavier load. Image downloads allocate by the real size (each used to ask for 1 MB and failed once memory fragmented); missing images say why, and tapping one retries.
- 番茄书城的“刷新”一直显示：原来后台在跑别的任务时会藏起来，任务结束后也不回来；现在点了会让浏览类任务让路，或者说明在等下载、同步。
  The Tomato bookstore's Refresh always shows: it used to hide while another job ran and never came back; now a tap makes browsing jobs yield or says it is waiting for a download or sync.
- 发布里多了 `treader-版本号-sdcard.zip`（中文字体、拼音词库和文件夹），刷机网页加了“TF 卡文件”按钮和第一次刷机要复制到 TF 卡的说明。
  Releases now include `treader-<version>-sdcard.zip` (Chinese font, pinyin lexicon and folders), and the flashing page adds a "TF card files" button with first-flash instructions.

## 2026-11-21 · 验证器安卓 App 1.0.0

- 新的独立安卓 App“验证器”：功能和界面照固件验证器标签，离线显示两步验证码；扫 Google Authenticator 导出二维码、粘贴链接或手动导入；账号加密存在 App 里（口令加手机 Keystore 密钥），不进备份、禁止截图。可以把账号“发送到小纸 Pico”：扫设备导入页的热点二维码，手机自动连上热点发过去，在设备上确认，固件不用改。
  New standalone Android app "Authenticator": mirrors the firmware Authenticator tab with offline two-step codes; imports by scanning the Google Authenticator export QR, pasting links or manual entry; accounts are encrypted in the app (passphrase plus a phone Keystore key), excluded from backups, with screenshots blocked. "Send to Read Pico" scans the hotspot QR on the device's import page, joins it automatically and sends the accounts for confirmation on the device, with no firmware change.

## 2026-11-21 · Treader 1.0.0

- 新功能“验证器”：在设备上离线显示两步验证码（和 Google Authenticator 一样的 TOTP），不需要服务器。账号加密存在 TF 卡上，要验证器口令加这台设备才能解开；锁屏、重启、拔卡都会上锁。用设备热点上的网页导入（GA 导出二维码照片、otpauth 链接或手动填），在设备上确认后写入；长按删除；时间可能不准时提醒校时。口令、自动上锁、清空在设置 → 内容来源 → 验证器。默认在“全部”页里。
  New Authenticator feature: offline two-step codes on the device (the same TOTP as Google Authenticator), no server needed. Accounts are encrypted on the TF card and need the authenticator passphrase plus this device to open; locking, restarting or removing the card locks it. Import through a web page on the device hotspot (a photo of the GA export QR code, otpauth links or manual entry), written after confirming on the device; long-press to delete; it asks for a time sync when the clock may be off. Passphrase, auto-lock and clearing live under Settings > Content sources > Authenticator. It starts on the All page.
- 功能多于 8 个后，底部标签的隐藏设置改存 16 位（升级时自动换算）。
  With more than 8 features, the hidden-tab setting is now stored as 16 bits (converted automatically on upgrade).

- USB 传书页的提示把“点 ⏏”改成“点“推出””：默认字体里没有这个符号，以前显示成空方框。
  The USB transfer hint now says to click "推出" instead of showing the ⏏ symbol, which the default font lacks and drew as an empty box.
- 漫画工具条的阅读方向箭头“⇠ / ⇢”改成“← / →”，Wi-Fi 传书密码提示里的“8–63”改成“8～63”：原来的字符字体里没有，显示成空方框。
  The comic toolbar's reading-direction arrows "⇠ / ⇢" became "← / →" and the Wi-Fi password hint's "8–63" became "8～63"; the old characters were missing from the fonts and drew as empty boxes.

## 2026-11-21 · 安卓 App：番茄 1.1.0、小红书 1.5.0、微博 1.4.0、X 1.1.0

- 四个安卓 App 的界面都改成和固件 0.9.0 一样的 Material 3 灰阶：分段按钮、胶囊按钮、角标、卡片、对话框、线条图标；桌面图标换成固件对应标签的图标，浅灰底黑线条。功能和操作不变。
  All four Android apps now use the firmware 0.9.0 Material 3 grayscale look: segmented buttons, pills, badges, cards, dialogs and line icons; launcher icons are the matching firmware tab icons in black on light gray. Features and controls are unchanged.
- 番茄：书库 / 番茄 / 设置底部标签栏、继续阅读卡片、书城搜索栏、阅读工具条和排版面板都照固件样式。
  Tomato: the library / Tomato / Settings tab bar, continue card, store search bar, reader toolbar and layout panel follow the firmware.

## 2026-11-21 · Treader 0.9.0

- 番茄新章节提示（“+N 章 待下载” / “+N 章”，见下面番茄安卓 App 1.0.3 一节）随本版固件正式发布。
  The Tomato new-chapter badges ("+N 章 待下载" / "+N 章", see Tomato Android app 1.0.3 below) ship with this firmware release.
- 界面改成 Material 3 灰阶风格（底部标签栏、按钮、分段、卡片、弹窗、工具条、锁屏书卡等），功能和操作不变；会局部快速刷新的地方保持黑白。
  The UI moves to a Material 3 grayscale style (tab bar, buttons, segments, cards, dialogs, toolbars, lock card and more); features and controls are unchanged, and areas refreshed by fast partial updates stay black and white.
- 番茄搜索入口从书架搬到书城页最上面；从书城进的搜索返回时回书城，书架多显示一行。
  The Tomato search entry moves from the shelf to the top of the bookstore page; searches started there return to the bookstore, and the shelf shows one more row.
- 底部标签栏改成“书库 + 常用 + 全部”：最后一格“全部”打开所有功能的大图标页（一页放不下就翻页）；KEY3、工具条的“菜单”也打开这一页，代替原来的列表菜单。设置 → 底部标签里挑哪几个放底部、排顺序、隐藏。升级后原来底部的标签按原顺序放进常用，放不下的（默认是微博和设置）在“全部”里。
  The tab bar becomes "library + pinned + All": the last cell opens a large-icon page of every feature (paged when it overflows); KEY3 and the toolbar "Menu" open it too, replacing the old list menu. Settings > Tab bar picks which features sit in the bar, their order, and which are hidden. After upgrading, the old bar's tabs become the pins in order; those that do not fit (Weibo and Settings by default) are on the All page.

- 设置 → 显示与阅读 → 显示大小：正常、大、特大（100% / 115% / 130%），像安卓一样把字、图标、按钮和间距一起放大，固定的界面文字都完整显示；放不下的列表每页少几行，底部标签栏变成 5 格 / 4 格。阅读正文、漫画和图片不受影响。
  Settings > Display and reading > Display size: normal, large, largest (100% / 115% / 130%) scales text, icons, buttons and spacing together like Android, with every fixed UI label shown in full; lists that no longer fit show fewer rows per page and the tab bar drops to 5 / 4 cells. Reading text, comics and images are unaffected.

- 修正一批问题：
  - 锁屏密码输错 5 次后的等待时间，关机再开不再清零。
  - “睡眠与锁屏”里的“试一下”：醒来要过锁屏密码；关机和深睡前先停下后台下载，免得写坏 TF 卡上的文件。
  - 每次锁屏前（包括浅睡）先保存书和漫画的阅读进度，锁屏书卡显示最新进度；TF 卡短暂失联时漫画进度也先存下。
  - 阅读工具条上点进度条跳到章节中间时，不再跳回本章开头；用内置字体时，工具条顶部的“‹ 书库”不再误开字体页。
  - 清掉进度后，书库的“继续阅读”和漫画的“继续看”卡片跟着更新。
  - 选了打不开的字体文件不再写进设置。
  - 系统升级写完、准备重启时离开页面也会马上重启，不会等到下次关机才悄悄换版本。
  - 番茄：离开页面期间完成的搜索或下载结果不再丢；状态行不再显示别的页面的后台消息；搜索结果页左上角“‹”能返回；正在下载的书和其他书的详情页分开显示；后台忙时点“下载全部更新”会说明原因。
  - 书城：排队中的下载只属于点下载的那本书，“取消”能撤掉排队。
  - 番茄和书城：一本书在下载时，点另一本的“下载到书库”会提示“正在下载另一本，下完再点”（后台忙别的也会说明原因），不再没反应。
  - 微博、X：列表还没加载时点空白处不再打开旧帖子；清理缓存不再删掉列表卡片的小图；X 等服务器取回复时不再每 3 秒整页重刷。
  Fixes: the PIN wait after five wrong tries survives a restart; the sleep page's trial asks for the PIN on wake and stops background downloads before powering off; reading progress for books and comics is saved before every lock and on a transient card loss; tapping the reader's progress track keeps the position inside the chapter, and the toolbar's back button no longer opens the font page with the built-in font; continue cards follow cleared progress; an unopenable font is not saved; leaving the upgrade page after writing restarts at once; Tomato keeps results finished while away, shows only its own status, lets "‹" leave results, separates the downloading book's detail from others, and explains a busy "download all updates"; the store ties a queued download to its book and lets 取消 drop it; tapping download on another book while one downloads now says to wait instead of doing nothing; Weibo / X no longer open an old post from an empty list, keep list thumbnails when pruning, and X no longer redraws every 3 s while waiting for replies.
- 修正：锁屏选“关机”或“深睡”时，屏幕放着会慢慢变黑（像渗墨）。原因是画完锁屏后没先关屏幕高压电就断了主机，高压和 VCOM 一直开着；现在断电前先关掉，进睡眠前还会读引脚确认屏幕真的断电了（没断就再关一次）；平时每次刷完屏也马上撤掉面板驱动和 VCOM，只留高压电等下一次刷新。
  Fix: with the lock set to power off or deep sleep, the screen slowly darkened while left alone (like ink bleeding), because the host powered down without first turning off the panel's high-voltage rails and VCOM after drawing the lock face; they are now switched off first, and before sleeping the pins are read to confirm the panel is really off (powering it off again if not); after every refresh the panel drive and VCOM are also released at once, leaving only the rails up for the next refresh.
- 修正：Wi-Fi 传书结束后字体一直停在内置字库，要重新插卡才恢复；现在离开传书页几秒内自动换回设置里选的字体。漫画书架右上角的传书按钮改成和书库一样，先到“设置 › 传书”选 Wi-Fi 还是 USB。
  Fix: after Wi-Fi transfer the font stayed on the built-in one until the card was reinserted; it now returns to the chosen font within seconds of leaving the transfer page. The comics shelf's transfer button now goes to Settings > Transfer to pick Wi-Fi or USB first, like the library's.

## 2026-11-20 · 番茄安卓 App 1.0.3

- 番茄新章节提示（番茄安卓 App 1.0.3；固件已在最新构建 treader-latest 里，下个固件版本号一起发布）：检查到还没下载的显示“+N 章 待下载”；下载以后不再消失，改成“+N 章”，一直留到打开这本书，书库和番茄书架都一样。WebDAV 同步后，在手机或设备上打开过的书两边都不再提示。升级前已经下载、还没打开的书不会补上提示。
  Tomato new-chapter badges (firmware and the Tomato Android app): chapters found but not downloaded show "+N 章 待下载"; after downloading the badge no longer vanishes but shows "+N 章" until the book is opened, in both the library and the Tomato shelf. After a WebDAV sync, opening the book on either the phone or the device clears it on both. Books downloaded but not opened before upgrading get no badge.

## 2026-11-20 · Treader 0.8.0

- 在线升级：设置 → 关于 Treader → 系统升级 → “检查更新”，从 GitHub 读最新版本；有新版本就先下载到 TF 卡 `update/`（断了可以接着下，换次也行），大小和 SHA-256 校验通过后自动写入并重启，新版本起来后自动删掉下载的升级包。只有线上版本更新才提示；带内置番茄服务器的本地版本升级前会提醒升级后没有内置服务器。
  Online upgrade: Settings > About Treader > System upgrade > "Check for updates" reads the latest version from GitHub; a newer one downloads to the TF card's `update/` first (resuming after a break, even on a later visit), is written and restarted into once its size and SHA-256 check out, and the downloaded package is removed once the new version boots. Only a newer online version is offered; a local build with a built-in Tomato server warns that the upgrade has no built-in server.
- 图片解码更快：普通基线 JPEG（漫画、EPUB 插图、小红书 / 微博 / X 的图）改用 JPEGDEC 解，约快 2.5 倍；渐进式等其他格式照旧。以前打不开的大图（宽或高超过 8192、超过 1600 万像素）和 4:4:0 采样的大图现在能打开。
  Faster image decoding: plain baseline JPEGs (comics, EPUB illustrations, Xiaohongshu / Weibo / X images) now decode with JPEGDEC, about 2.5x faster; progressive and other formats are unchanged. Large images that failed before (a side over 8192, over 16 megapixels) and large 4:4:0 images now open.

## 2026-11-20 · Treader 0.7.1

- 设置 → 电源与设备 → 时间与时区：自动校时（默认开，联网时对时）、15 个常用时区（默认北京时间，欧美和悉尼自动切夏令时）、关掉自动校时后可以手动设置日期和时间。修正：开机时从电源芯片的时钟恢复时间（以前写好了但没有调用，深睡、关机醒来后要等联网才有时间）。
  Settings > Power and device > Time and zone: auto time (on by default, synced when online), 15 common time zones (Beijing by default; Europe, the US and Sydney switch daylight saving automatically), and a manual date and time once auto time is off. Fix: boot now restores the clock from the power chip's RTC (the code existed but was never called, so after deep sleep or power-off the time stayed blank until the device went online).

## 2026-11-20 · Treader 0.7.0

- TF 卡更稳：换卡时（包括拔插太快、浅睡锁屏时换卡）设备会主动问卡、发现换过就重新挂载；I2C 偶尔读错不再被当成拔卡（以前会关掉正在看的书）；卡上的字体在这张卡上加载失败只试一次，不再每 3 秒卸载一次字体，换卡后再试；字体切换和阅读页后台排版互相加锁，离开阅读页时也等排版画完。番茄更新替换书文件、复制封面都改成先写临时文件，中途拔卡书不会从书库消失，封面不会一直坏着。
  Sturdier TF card handling: the device asks the card after waking and every few seconds, so a swap (even a quick one or one during light-sleep lock) is noticed and remounted; an occasional I2C read error no longer counts as a removal (it used to close the open book); a TF font that fails on a card is tried once instead of being unloaded every 3 s, and again after a swap; font switches and the reader's background layout now lock each other, and leaving the reader waits for that layout to finish. Tomato updates replace book files and copy covers through temporary files, so a pull midway never makes a book vanish or leaves a broken cover.
- 设置 → 电源与设备：新增“自动锁屏”（1、2、5、10 分钟或从不，默认 2 分钟）和“锁屏密码”（4 位数字，默认关）。开了密码，开机和每次解锁都要先输密码；输错 5 次后要等一会儿才能再输。忘了密码只能用电脑擦掉设置分区（见 docs/TREADER.md）。
  Settings > Power and device: new "Auto-lock" (1, 2, 5 or 10 minutes, or never; 2 minutes by default) and "Lock PIN" (4 digits, off by default). With a PIN, boot and every unlock ask for it first; after 5 wrong tries each new try waits a while. A forgotten PIN can only be cleared by erasing the settings partition from a computer (see docs/TREADER.md).

## 2026-11-20 · Treader 0.6.1

- 修正（固件 0.6.1、番茄安卓 App 1.0.2）：追更到最后的书“下载全部更新”后，书库和番茄书架一直显示“读完”。下载更新时进度百分比按新的全书大小重算，有新章节就最多 99%，位置还是接着上次读的地方；已经显示“读完”的书打开翻一页就恢复正常。
- 番茄安卓 App 1.0.2：修正看书时有时点屏幕翻不了页（跨章以后手势还在用上一章的排版，会跳页或停在最后一页点了没反应）。
  Tomato Android app 1.0.2: fix taps sometimes not turning pages while reading (after crossing a chapter the gestures still used the previous chapter's layout, skipping pages or getting stuck on the last page).
  Fix (firmware 0.6.1, Tomato Android app 1.0.2): books read to the end kept showing "finished" in the library and Tomato shelf after "download all updates". The progress percentage is now rescaled to the new book size when updates download, at most 99% when new chapters arrived, and reading still resumes where you left off; books already showing "finished" recover after opening and turning one page.
- x 服务器 0.1.4：每次拉完（列表或回复）把浏览器切到空白页。浏览器和登录都还在，下次照常拉；两次拉取之间 X 的网页不再在后台定时访问，内存也降下来。
  x server 0.1.4: after every fetch (lists or replies) the browser switches to a blank page. The browser and sign-in stay for the next fetch; between fetches X's page no longer polls in the background and memory drops.
- x 服务器 0.1.3：修正拉取“为你推荐”“正在关注”时报错 `NameError: name 'started' is not defined`（0.1.0 起取回复的 45 秒上限误写进了拉列表的代码）。
  x server 0.1.3: fix For You and Following fetches failing with `NameError: name 'started' is not defined` (the 45 s cap for replies had landed in the list-fetch code).
- x 服务器 0.1.2：修正在较慢的机器（如 NAS）上登录 X 后存不住。服务器盯着浏览器资料里的 Cookies 库，登录一写进硬盘就自动收尾；点“登录好了”时也先等它写盘（最多约 45 秒），再让浏览器正常退出（最多等 90 秒），不再提前强杀。登录窗口仍是不带任何调试接口的普通 Chrome（0.1.1 加的调试管道会让 X 暂时限制登录，已去掉）。
  x server 0.1.2: fix the X sign-in not being kept on slower machines such as a NAS. The server watches the profile's Cookies database and finishes as soon as the sign-in is on disk; "done" also waits for that write (about 45 s at most) and then lets the browser exit cleanly (up to 90 s) instead of killing it early. The sign-in window stays an ordinary Chrome with no debugging interface (the debugging pipe added in 0.1.1 made X limit sign-ins and is gone).

## 2026-11-20 · Treader 0.6.0

- 新增 X 标签（固件，默认隐藏，在“设置 › 底部标签”打开）和 X 安卓 App（`x/android`，1.0.0），配合新的 x 服务器（仓库 `x/`）：服务器上的浏览器登录你的 X 账号（面板里用 VNC 手动登录），按随机间隔和睡眠时段拉取“为你推荐”和“正在关注”，去广告、图片转灰度；设备和 App 离线看，回复第一次打开时服务器才去取。面板关掉“正在关注”时，设备和 App 上这个标签变灰。
  New X tab (firmware, hidden by default; turn it on under "Settings > Tab bar") and X Android app (`x/android`, 1.0.0), backed by the new x server (repo `x/`): a browser on the server signed in to your X account (by hand over VNC in the panel) fetches For You and Following at random intervals outside a sleep window, drops ads and turns images gray; the device and app read offline, and replies are fetched the first time they are opened. When the panel turns Following off, that tab greys out on the device and app.
- 前台优先更彻底（固件，rednote 服务器 0.3.2、weibo 服务器 0.1.5、x 服务器）：刷新小红书推荐、点开推荐笔记、加载更多评论、第一次看微博评论和 X 回复时，服务器改在后台做，设备隔几秒问一次，不再一直挂着等；这期间切到别的标签，那边的加载马上开始。旧服务器照旧能用，只是还会挂着等。安卓 App 不受影响。
  Foreground first, all the way (firmware, rednote server 0.3.2, weibo server 0.1.5, x server): refreshing Xiaohongshu recommendations, opening a recommended note, loading more comments, and first opening Weibo comments or X replies now run in the background on the server while the device asks every few seconds instead of holding the line; switching tabs meanwhile starts the other tab's loading at once. Older servers still work but keep the old wait. The Android apps are unaffected.
- 小红书、微博安卓 App（小红书 1.4.0、微博 1.3.0）：看大图时点中间弹出“保存图片”，把屏幕上这张图存进手机相册（`Pictures/Treader 小红书`、`Pictures/Treader 微博`）。存的是服务器处理好的灰度图，长图只存当前这一屏；服务器不用升级。
  Xiaohongshu and Weibo Android apps (Xiaohongshu 1.4.0, Weibo 1.3.0): tapping the middle of a full image shows "Save image", which stores the image on screen in the phone's gallery (`Pictures/Treader 小红书`, `Pictures/Treader 微博`). It saves the server's grayscale version, and only the current screen of a split long image; no server upgrade needed.
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
