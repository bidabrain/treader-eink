# 版本变更 / Changelog

按日期和作者简述对用户可见的功能变化；详细实现历史见 Git。使用方法见 [README](../README.md)。
User-visible changes by date and author; Git retains implementation history. See [README](../README.md) for usage.

## 2026-11-17 · Treader 0.1.0

- 封面：EPUB 显示书内封面，番茄书下载时一起取封面；书库、首页和锁屏显示图片封面，缩略图缓存在 TF 卡 `.treader/covers/`。
  Covers: EPUBs show their embedded cover and Tomato downloads fetch one; the library, home and lock screen show image covers cached under `.treader/covers/` on the TF card.
- 设置新增“关于 Treader”：版本、项目地址和开源致谢；设备信息从这里进入。
  Settings gains "About Treader" with the version, project link and open-source credits; device info opens from there.
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
