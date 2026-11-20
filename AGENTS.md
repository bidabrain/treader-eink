# AGENTS.md

面向 AI agent 的仓库入口。人类也可读。Treader 的介绍、刷机和自建服务器见 [README.md](README.md)；硬件表和引脚见官方原版说明 [docs/official/README.zh-CN.md](docs/official/README.zh-CN.md)。

Agent entry for this tree. Humans can read it too. Treader overview, flashing and server setup live in [README.md](README.md); hardware and pinout are in the original MindReset README under [docs/official/](docs/official/README.md).

新人（人或 agent）上手看 [docs/ONBOARDING.md](docs/ONBOARDING.md)；接手进行中的任务先看 [docs/HANDOFF.md](docs/HANDOFF.md)。

Newcomers (human or agent): read [docs/ONBOARDING.md](docs/ONBOARDING.md). Taking over an in-flight task: read [docs/HANDOFF.md](docs/HANDOFF.md) first.

## 构建 / Build

需要 ESP-IDF v6.1。`components/read_pico/read_pico_flash_hpm.c` 依赖 v6 才有的 `esp_flash_chips/spi_flash_override.h`。

Requires ESP-IDF v6.1. `components/read_pico/read_pico_flash_hpm.c` needs `esp_flash_chips/spi_flash_override.h` (v6 only).

```
idf.py set-target esp32s3
idf.py build
```

- `sdkconfig.defaults`：真机 120 MHz flash / PSRAM 时序，依赖板上 Zbit 型号。/ Board timing for 120 MHz flash / PSRAM; depends on the Zbit part.
- `sdkconfig.ci`：默认时序，只用来在没有这块 flash 的机器上检查能否编过。CI 编的是产品配置（`sdkconfig.defaults`），不带内置番茄服务器，固件改动后自动发布到 Release（`treader-latest` 和每个新版本号的 `treader-v*`）。/ Default timing for compile checks without the board's flash. CI builds the product configuration (`sdkconfig.defaults`) without a built-in Tomato server and publishes Releases after firmware changes (`treader-latest`, plus `treader-v*` for each new version).

不要在没有对应 flash 的机器上把 CI 配置当产品默认。/ Do not ship the CI timing as the product default.

## 目录职责 / Layout

| 路径 / Path | 职责 / Role |
| --- | --- |
| `main/app_main.c` | 开机装配：板级 init、读设置、定 VCOM、开机图、字体，然后交给 `app_loop`。/ Boot wiring, then `app_loop`. |
| `main/app/app.h` | `app_desc_t` / `app_ctx_t` / `app_redraw_t` 契约。/ Page contract. |
| `main/app/app_loop.c` | 与页面无关的事件循环：触摸去抖、三键与菜单把手（KEY3 / 把手打开“全部”页）、切页、刷屏、锁屏、SD 字体延迟加载。/ Shared event loop; KEY3 and the handle open the All page. |
| `main/app/app_registry.c` | 只剩开机首页和自检页两个入口；功能列表在 `tr_ui` 的 `tr_tab_t`，切页映射在 `tr_nav.c`。/ Only the boot and self-test entries; features are `tr_tab_t` in `tr_ui`, mapped to pages in `tr_nav.c`. |
| `main/app/app_config.h` | 均衡 / ALL_DU 刷新档位。/ Refresh profile. |
| `main/ui/` | `ui_scale.h` 显示大小（`UI_S`/`TR_S`）；`ui_kit` 排版常量与绘制原语；`tr_ui` 新风格控件与底部标签栏；`ui_menu` 只剩菜单把手和屏下三键区。页面互不引用。/ Shared drawing, the tab bar, and the handle / key strip; pages do not include each other. |
| `main/font/` | stb_truetype 字形缓存。`fallback.h` 是生成位图，勿手改。/ Glyph cache. Do not edit `fallback.h` by hand. |
| `main/factory/` | 设备功能自检与出厂 VCOM 标定。/ Device self-test and factory VCOM. |
| `main/display.*` `sleep.*` `settings.*` | 刷屏封装、睡眠/锁屏、NVS 设置。/ Present helpers, sleep/lock, NVS. |
| `components/read_pico/` | 板级 BSP：I2C、EPD 板定义与扫描时序、TF 卡、蜂鸣器、flash HPM。/ Board BSP. |
| `components/read_pico_search/` | 离线拼音/首字母/英文书名匹配，设备和网页共用；字表由工具生成。/ Shared offline pinyin, initials and English matching with a generated table. |
| `components/read_pico_usb/` | USB 传书：esp_tinyusb MSC 把 SD 卡交给电脑，报告连接、弹出和读写量。/ USB transfer: esp_tinyusb MSC exposes the SD card and reports connection, eject and bytes moved. |
| `components/read_pico_pmu/` | CW32L010 协议主机端。线格式在 `read_pico_pmu_protocol.h`。/ PMU host. |
| `components/epdiy/` | 上游裁剪 fork（LCD 路径）。改动清单在其 LICENSE。不要为注释去改它。/ Trimmed upstream fork. Leave it alone for comment work. |
| `components/continuous_du/` | 连续 DU，触摸跟手。/ Continuous DU for finger tracking. |
| `components/cst836u/` `sc7a20h/` `fca9555/` `sy7636a/` | 芯片驱动。/ Chip drivers. |
| `components/e0470_epaper_waveform/` | 面板波形与裁剪。`waveforms/*.h` 是数据表。/ Panel waveforms. |
| `components/pwm_audio/` | PWM 音频（蜂鸣器底层之一）。/ PWM audio helper. |
| `docs/CHANGELOG.md` | 按版本简述对用户可见的功能变化与修复。/ Brief user-visible changes and fixes by version. |
| `rednote/` | 小红书内容服务器（Docker：Python 面板 hub + xiaohongshu-mcp），独立于固件，部署说明在其 README。/ Xiaohongshu content server (Docker: Python hub + xiaohongshu-mcp), separate from the firmware; see its README. |
| `rednote/android/` | 小红书安卓客户端（Kotlin + Compose），功能和界面照固件小红书标签，独立版本号；tag `rednote-android-v*` 发布，签名用 Secret 里的 debug keystore。/ Xiaohongshu Android client (Kotlin + Compose) mirroring the firmware tab, with its own version; released on `rednote-android-v*` tags, signed with the debug keystore from a secret. |
| `tomato/` | 番茄下载服务器一键部署（Docker：官方 Tomato-Novel-Downloader + Python 面板 hub），hub 核对设备密码后把设备接口原样转给下载器，带 cloudflared；固件和安卓 App 不用改，部署说明在其 README。/ One-step Tomato downloader deployment (Docker: the official Tomato-Novel-Downloader plus a Python panel hub); the hub checks the device password and forwards the device API unchanged, with cloudflared; no firmware or app change; see its README. |
| `tomato/android/` | 番茄安卓客户端（Kotlin + Compose），功能和界面照固件书库与番茄标签，独立版本号；TXT / EPUB 书源逐字节照搬 `main/book`（单元测试用固件代码生成的结果核对），WebDAV 和设备共用 `treader-sync.json`；tag `tomato-android-v*` 发布，签名用自己的 keystore（Secret `TOMATO_KEYSTORE_BASE64`）。改 `main/book` 书源时重新运行 `tomato/android/tools/gen_fixtures.sh`。/ Tomato Android client (Kotlin + Compose) mirroring the firmware library and Tomato tab, with its own version; the TXT / EPUB sources copy `main/book` byte for byte (unit tests check against output from the firmware code) and WebDAV shares `treader-sync.json` with the device; released on `tomato-android-v*` tags, signed with its own keystore (secret `TOMATO_KEYSTORE_BASE64`). Rerun `tomato/android/tools/gen_fixtures.sh` after changing the `main/book` sources. |
| `weibo/` | 微博内容服务器（Docker：Python hub + Playwright Chromium 访客身份），不登录，关注 / 热门两类，图片在服务器转灰度；关注管理与 WebDAV 备份照 weiboX-server，部署说明在其 README。/ Weibo content server (Docker: Python hub + Playwright Chromium visitor identity), no sign-in, following and hot feeds, images converted on the server; follow management and WebDAV backup match weiboX-server; see its README. |
| `x/` | X 内容服务器（Docker：Python hub + Xvfb 上的 Google Chrome，noVNC 手动登录），按随机间隔和睡眠时段拉“为你推荐”“正在关注”，去广告，图片转灰度；部署说明在其 README。/ X content server (Docker: Python hub + Google Chrome on Xvfb, sign-in by hand over noVNC) fetching For You and Following at random intervals outside a sleep window, dropping ads and turning images gray; see its README. |
| `x/android/` | X 安卓客户端（Kotlin + Compose），功能和界面照固件 X 标签，独立版本号；tag `x-android-v*` 发布，签名用自己的 keystore（Secret `X_KEYSTORE_BASE64`）。/ X Android client (Kotlin + Compose) mirroring the firmware tab, with its own version; released on `x-android-v*` tags, signed with its own keystore (secret `X_KEYSTORE_BASE64`). |
| `weibo/android/` | 微博安卓客户端（Kotlin + Compose），功能和界面照固件微博标签，独立版本号；tag `weibo-android-v*` 发布，签名用自己的 keystore（Secret `WEIBO_KEYSTORE_BASE64`，和小红书 App 不同）。/ Weibo Android client (Kotlin + Compose) mirroring the firmware tab, with its own version; released on `weibo-android-v*` tags, signed with its own keystore (secret `WEIBO_KEYSTORE_BASE64`, separate from the Xiaohongshu app). |

功能操作见 README 和 [docs/TREADER.md](docs/TREADER.md)；页面契约以本文及 `main/app/app.h` 为准。/ See the README and docs/TREADER.md for feature usage; this file and `main/app/app.h` define the page contract.

### 文档交付边界 / Documentation delivery boundary

对外交付功能实现、可复跑测试、功能说明及必要的维护契约。功能变化只在 `docs/CHANGELOG.md` 按版本轻量记录；代码演进由 Git 管理，不另建逐任务变更账本。
Deliver implementation, reproducible tests, feature documentation and necessary maintenance contracts. Summarize feature changes only in `docs/CHANGELOG.md`; Git owns code history, not per-task ledgers.

计划、检查点、过程审查、验收结果和调试日志在 `docs/local/` 或被忽略的构建目录内部闭环，不加入提交。长期有效的行为与限制整理进对应功能文档，不要求读者查内部计划。只有需要交接未完功能或责任时才按 `docs/HANDOFF.md` 提供最小必要摘要；本机账本仍不入库。
Keep plans, checkpoints, review records, acceptance results and debug logs in ignored `docs/local/` or build directories. Put lasting behavior and limits in feature documentation without dependencies on internal plans. Use `docs/HANDOFF.md` for the minimal necessary handoff of unfinished work or responsibility; the local ledger remains untracked.

### demo 页 / Demo pages

每个文件只导出 `app_desc_t`。

One file, one `app_desc_t`.

| 文件 / File | 演示 / Demo | 依赖 / Depends on |
| --- | --- | --- |
| `app_home.c` | 概览：I2C 普查、识别码、电池、构建时间。/ Overview. | `read_pico`, `read_pico_pmu` |
| `app_refresh.c` | GC16 / DU / 灰阶梯子与耗时。/ Refresh matrix. | `epdiy`, `e0470_epaper_waveform` |
| `app_reading.c` | 内置正文翻页。/ Reading demo. | `ttf_font` |
| `app_touch.c` | 两点跟手连续 DU、深睡唤醒。/ Touch + continuous DU. | `cst836u`, `continuous_du` |
| `app_axis.c` | 读数页 Live/Tap/Orient 与诊断页 Setup/Event/FIFO/Test。/ IMU pages. | `sc7a20h` |
| `app_power.c` | 电池与 SY7636A 只读。/ Battery + rails. | `read_pico_pmu`, `sy7636a` |
| `app_pmu.c` | CW32 协议五视图。/ Protocol views. | `read_pico_pmu` |
| `app_key.c` | 电源键原始事件。/ Power-key events. | `read_pico_pmu` |
| `app_sleep.c` | 浅睡 / 深睡 / 关机。/ Sleep modes. | `sleep`, `read_pico_pmu`, `sc7a20h` |
| `app_sd.c` | TF 卡与蜂鸣器。/ Card + buzzer. | `read_pico_sd`, `read_pico_buzzer` |
| `app_font_pick.c` | 卡上 TTF 列表与字重。/ Font picker. | `ttf_font` |
| `app_ioe.c` | FCA9555 Port-0。/ Expander pins. | `fca9555` |
| `app_selftest.c` | 设备功能自检入口。/ Device self-test UI. | `pmu_selftest` |
| `app_comic.c` | CBZ 漫画书架、系列话列表、一屏一页阅读与工具条；和书库分开，只读 TF 卡 `comics/`。/ CBZ comics shelf, series list, one-page reader and toolbar; separate from the library, reads `comics/` only. | `comic_store`, `zip_reader`, `book_image`, `hupu_img` |
| `app_book.c` | TXT / EPUB 书架、目录、字号与逐书进度；晃动翻页实验默认关。/ TXT / EPUB shelf, TOC, size and per-book progress; experimental shake defaults off. | `book_*`, `ttf_font` |
| `app_tr_xhs.c` | 小红书标签：关注 / 推荐列表、一屏一张看图、正文与评论；只读自建 rednote 服务器写到 TF 卡 `rednote/` 的缓存。/ Xiaohongshu tab: following and recommendation lists, one image per screen, text and comments; reads only the `rednote/` cache fetched from the self-hosted rednote server. | `tomato`（`tomato_rednote.c`）, `hupu_img` |
| `app_tr_weibo.c` | 微博标签：关注 / 热门列表（一行一条带缩略图）、全文、一屏一张看图、评论；只读自建 weibo 服务器写到 TF 卡 `weibo/` 的缓存。/ Weibo tab: following and hot lists (one post per row with a thumbnail), full text, one image per screen, comments; reads only the `weibo/` cache fetched from the self-hosted weibo server. | `tomato`（`tomato_weibo.c`）, `hupu_img` |
| `app_tr_x.c` | X 标签（默认隐藏）：为你推荐 / 正在关注、全文、一屏一张看图、回复；服务器关掉正在关注时这一段变灰；只读自建 x 服务器写到 TF 卡 `x/` 的缓存。/ X tab (hidden by default): For You / Following, full text, one image per screen, replies; Following greys out when the server turns it off; reads only the `x/` cache fetched from the self-hosted x server. | `tomato`（`tomato_x.c`）, `hupu_img` |
| `app_tr_all.c` | “全部”页：没隐藏的功能排成大图标网格、翻页；底部最后一格、KEY3、工具条“菜单”、老页面把手都打开它，最后一页 KEY3 回到来处。/ The All page: every visible feature as a paged large-icon grid; opened by the bar's last cell, KEY3, the toolbar Menu and the legacy handle; KEY3 on the last page returns. | `tr_ui`, `tr_nav` |
| `app_usb.c` | USB 传书：开始前说明；开始后卸 TF 卡交给电脑当 U 盘，只显示这一页（接管三键、占 PMU、无标签栏）；结束一律重启。/ USB transfer: intro, then the TF card goes to the computer as a USB drive with only this page shown (owns keys and PMU, no tab bar); ending always restarts. | `read_pico_usb`, `read_pico_sd` |
| `app_transfer.c` | 设备热点/已有 WiFi 传 TXT/EPUB 与 TF 卡 TTF 字体，触屏/网页配网与热点二维码，离页停止；停留期间不计空闲自动休眠；停止按钮返回进入前的位置。/ AP/STA book and TF font upload with touchscreen/web provisioning and hotspot QR; stops on exit; idle auto-sleep does not run while the page is current; the stop button returns to the entry origin. | `read_pico_transfer`, `book_store` |

加页：在 `main/apps/` 新建文件，实现需要的回调，加进 `main/CMakeLists.txt`。要作为一个功能出现在底部和“全部”页，在 `tr_ui.h` 的 `tr_tab_t` 里加一项（标签名、图标在 `tr_ui.c`），在 `tr_nav.c` 的 `tr_nav_tab_app()` 映射到页面；页面内的子页由所属功能页用 `request_app` 打开。隐藏掩码是 `uint8_t`，功能超过 8 个时要先把它加宽。不要改 `app_loop.c`。

To add a page: new file in `main/apps/`, implement the callbacks you need, add it to `main/CMakeLists.txt`. To make it a feature in the bar and on the All page, add a `tr_tab_t` entry (label and icon in `tr_ui.c`) and map it in `tr_nav_tab_app()` in `tr_nav.c`; sub-pages are opened by their feature page with `request_app`. The hidden mask is a `uint8_t`; widen it before going past 8 features. Leave `app_loop.c` alone.

## `app_desc_t` 契约 / Contract

定义在 [`main/app/app.h`](main/app/app.h)。主循环按返回值刷屏，见 [`main/app/app_loop.c`](main/app/app_loop.c) 的 `app_present()`。

Defined in [`main/app/app.h`](main/app/app.h). The loop presents via `app_present()` in [`main/app/app_loop.c`](main/app/app_loop.c).

| 返回值 / Return | 主循环做什么 / Loop does |
| --- | --- |
| `APP_REDRAW_NONE` | 不刷。/ No update. |
| `APP_REDRAW_AREA` | 只推 `area_hint()`。回调必须先画好 fb。模式是 `APP_DYNAMIC_REFRESH_MODE`（DU）。/ Push `area_hint()` only. Caller paints fb first. |
| `APP_REDRAW_PAGE` | 调 `render()`，整页 `APP_PAGE_REFRESH_MODE`（均衡档是 GL16）。/ `render()` then page mode. |
| `APP_REDRAW_FULL` | 调 `render()`；`APP_PAGE_FORCE_FULL` 时整屏 GC16，否则仍走页模式。/ `render()`; GC16 when force-full is on. |
| `APP_REDRAW_DONE` | 页面自己已经刷完。/ Page already presented. |

回调纯度：

- `render()`：纯绘制。禁止 I2C 写、蜂鸣、睡眠、改设置。中键长按的整屏强刷会复用它。/ Paint only. No I2C writes, buzzer, sleep, or settings. The middle-key hold refresh reuses it.
- `on_enter()`：上电、唤醒传感器、拉一次数据。/ Power-up and first sample.
- `on_media_lost()` 可选：主循环检测已挂载卡失效时调用，先停止文件/后台消费者，不画屏、不重挂载；返回后主循环回退字体并重绘。/ Optional mounted-media-loss callback: stop file/background consumers without drawing or remounting; the loop then falls back fonts and redraws.
- `on_exit()`：掉电、停传感器。/ Power-down.
- `on_lock()` 可选：每次锁屏前调用（浅睡也调），只存进度这类收尾，不画屏、不切页。/ Optional: before every lock (light sleep too); only wind-down such as saving progress, no drawing or page switch.
- `present()`：自定义推屏。返回 true 表示已经刷过，主循环不再推。/ Custom present; true means done.
- `on_touch()` / `on_gesture()` / `on_key()` / `on_tick()`：可有副作用，用返回值要刷屏。/ Side effects OK; return the redraw.
- `on_key_long()` 可选：仅接管三键的页面在同键单指保持500ms后触发一次，原按下动作先执行；滑出、多点、读错、睡眠、字体重载或切页取消。/ Optional owned-key hold callback fires once at 500ms after the normal press; leaving, multitouch, read errors, sleep, font reload and page switches cancel.
- `on_gesture()` 可选；提供后不再接收 `on_touch()`。主循环负责识别与全局中断取消，页面解释动作。/ Optional gesture callback replaces `on_touch`; the loop recognizes and cancels, the page interprets.
- `on_key()` 默认只收 `UI_KEY_1`；`owns_keys` 页面接收 KEY1、KEY3（按下时）和 KEY2（短按松手时）。中键（home）短按默认回主页（书库）、长按 0.5 秒整屏刷新，都由主循环处理。/ Normally KEY1 only; `owns_keys` pages get KEY1/KEY3 on press and KEY2 on a short-press release outside the menu. The middle (home) key goes to the home page (the library) on a short press and refreshes on a 500 ms hold, both handled by the loop.

标志：

- `holds_pmu`：长时间独占 PMU（自检）。主循环的锁屏按键轮询让路。/ Page owns the PMU; lock-key poll yields.
- `owns_keys`：接管三键；菜单把手仍归主循环。/ Own all three keys; the handle stays with the loop.
- `enter_full`：进页走 `APP_REDRAW_FULL`，避免差分刷留边。/ Enter with a full refresh.
- `fixed_scale`：不跟显示大小，始终按 100% 画（出厂、演示页）。/ Ignore the display size and draw at 100% (factory and demo pages).

`app_ctx_t.request_app` 非空时，主循环在本轮末尾切页；`request_menu` 请求打开“全部”页。同时设置时切页优先。/ At tick end, `request_app` switches pages and `request_menu` opens the All page; a page request takes priority.

## 术语表 / Glossary

| 术语 / Term | 中文 | English |
| --- | --- | --- |
| DU | 快速差分刷新，只驱动变化像素，灰阶少、残影多。 | Fast differential update. Drives changed pixels only. |
| GC16 | 16 级灰度全像素刷新。区域内白像素先压黑再擦白，刷新处一条黑带。 | 16-gray full-pixel update. Whites go black then white; a black flash. |
| GL16 | 16 级灰度，白底保持，不先压黑。 | 16-gray update that keeps an already-white background. |
| 跟随 DU / FOLLOW DU | 开机生成的 8 帧短波形，一次刷新连 diff 带扫描，用来跟手数字或读数。 | Boot-built 8-frame short waveform: one refresh does diff + scan. Used for live digits. |
| 连续 DU / continuous DU | 每轮只扫一个相位，软件记每像素剩余相位。帧率与黑度解耦。 | One DU phase per scan; software tracks leftover phases per pixel. |
| 跟手 | 手指或传感器在动时用 DU/连续 DU 追画面。 | Live tracking with DU / continuous DU while the finger or sensor moves. |
| 定稿 | 动作停下后用 GC16/GL16 整页（或指定区）清残影。 | Settle after motion with GC16/GL16 to clear ghosting. |
| 把手 | 没有标签栏的老页面右下角菜单按钮或 KEY3。打开“全部”页，不交给当前页。 | Menu handle (bottom-right on pages without a tab bar, or KEY3). Opens the All page; not dispatched to the page. |
| 冻结 / Frozen | 文件头里的产品决策。agent 不得改行为去“优化”它。 | Product decisions in the file banner. Do not “improve” them. |
| 均衡配置 | `APP_REFRESH_BALANCED`：整页 GL16，周期 GC16 压灰底，动态区 DU。 | Default refresh profile: GL16 pages, periodic GC16, DU for motion. |
| ALL_DU | 实验档：整机都走 DU，只看速度。 | Experimental profile: everything is DU. |
| 锁屏 | 电源键短按后的锁页。浅睡按键或拿起回原页。 | Lock face after a short power-key press. |
| 软睡 | `SOFT_SLEEP`：PMU 拉低 EN，再短按开机，不是硬复位。 | Soft sleep: PMU drops EN; a short press powers on without a hard reset. |
| VCOM | 面板公共电压（绝对值 mV）。出厂写入 PMU，主机只读。 | Panel common voltage (absolute mV). Factory-written in the PMU; host is read-only. |

注释里不要再展开这些词。/ Do not re-explain these terms in comments.

## 硬约束 / Hard rules

- 对外产品文案：中文用“小纸 Pico”，英文和日文用“Read Pico”；Read/0 仅可作为内部代号保留。/ Public product copy: use “小纸 Pico” in Chinese and “Read Pico” in English and Japanese; reserve Read/0 for the internal codename.

`冻结 / Frozen:` 段落是产品决策，不是建议。改行为前必须先改这段，并说明为什么决策变了。

A `冻结 / Frozen:` block is a decision, not a hint. Change the behavior only after rewriting that block and saying why the decision changed.

- 面板 VCOM 存在 PMU 里。主机开机读一次，不写 ESP NVS，产品页不提供修改入口。标定页（`vcom_setup.c`）是未标定时的出厂拦住路径。/ VCOM lives in the PMU. Host reads once. No NVS copy. No product UI to edit it. `vcom_setup.c` is the factory gate when unset.
- 不要写 SY7636A 的 VCOM / VLDO / 放电 / 延时 / VCOMCTL。电源页只读。/ Do not write SY7636A VCOM / VLDO / discharge / delay / VCOMCTL. Power page is read-only.
- `sdkconfig.defaults` 的 120 MHz 时序依赖真机 Zbit flash。覆盖 HPM 表见 `read_pico_flash_hpm.c`。/ 120 MHz timing needs the onboard Zbit part. HPM override is `read_pico_flash_hpm.c`.
- 不要改 `components/epdiy/**` 来“整理注释”。/ Do not touch `components/epdiy/**` for comment cleanup.
- 不要手改 `main/font/fallback.h`、`main/font/stb_truetype.h`、`components/e0470_epaper_waveform/waveforms/*.h`。/ Do not hand-edit generated or vendored tables.
- 不改 UI 字面量或日志字符串，除非任务明确要求。/ Do not change UI strings or log text unless asked.
- 界面长度（字号、行高、间距、图标、按钮）一律经 `TR_S()` / `UI_S()` 随显示大小放大，左右边距和细线除外；改页面后用 `tools/page_render`（`run.sh` 画三档并检查，`compare.sh` 确认 100% 逐像素不变）。/ UI lengths (font sizes, row heights, gaps, icons, buttons) go through `TR_S()` / `UI_S()` so they follow the display size, except side margins and hairlines; after changing a page, run `tools/page_render` (`run.sh` draws and checks three sizes, `compare.sh` proves 100% is pixel-identical).

## 注释规范 / Comment style

全仓中英双语。只改注释，不改逻辑。

Every comment is bilingual. Comments only; no logic changes.

文件头：

```
/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：这个文件负责什么、边界在哪。
 *
 * English: what this file owns and where it stops.
 *
 * 冻结：不得做的事。
 * Frozen: things this file must not do.
 */
```

- 头文件公开 API 与 struct 成员用 `///`。/ Public API and struct fields: `///`.
- `.c` 内部用 `//`。不要 `/** */`。/ `.c` internals: `//`. No `/** */`.
- 单行：`// 中文。/ English.`
- 超过一行：中文一行，英文紧跟一行。/ Multi-line: Chinese line, then English line.
- `enum` 成员短尾注：`///< 中文 / English`
- 大 `.c` 用 `/* ---- 区块名 / Section ---- */` 分区。/ Large `.c` files use section banners.
- 术语见表，不在注释里重复解释。/ Use the glossary; do not redefine terms.
