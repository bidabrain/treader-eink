# Treader

基于[小纸 Pico 官方固件](https://github.com/MindReset/read_pico_firmware)的分支，把安卓版
[Tomato-Novel](https://github.com/bidabrain/Tomato-Novel) 的核心功能（搜索番茄小说、下载 TXT、
检查更新）做进系统。阅读器、目录、字体、传书全部沿用官方固件，只新增番茄同步和拼音输入。

设计原则：设备是 ESP32-S3 单片机，**能少做就少做**。只有黑白界面，没有颜色和主题，没有动画；
封面用文字生成，不解码图片。界面风格参考 RickyOS，设计稿见 [design/ui.html](design/ui.html)。联网只在一次操作期间打开，做完就断。

## 当前状态

全部只确认了能编译，**都还没上机验证**。

| 部分 | 状态 |
| --- | --- |
| 菜单精简，开机进首页 | 完成 |
| 拼音输入法引擎 `components/pinyin_ime` | 完成 |
| 番茄同步 `components/tomato`：连网对时、搜索、下载、检查更新（检查与下载分开） | 完成 |
| 界面第一批：底部标签栏、首页、番茄（书架/搜索键盘/结果/详情）、设置（含番茄服务器编辑） | 完成 |
| 番茄更新后保留阅读进度（`book_progress_resize`） | 完成 |
| 界面第二批：书库（带标签栏、文字封面、番茄标注）、阅读工具条、锁屏书卡 | 完成 |
| WebDAV 同步：阅读进度和番茄书架 | 完成，见下文 |
| 九键拼音；书库搜索、目录、管理弹窗改成新风格 | 完成 |
| 字体、睡眠、传书、关于本机、自检页改纯中文；字体页去掉跑分 | 完成 |
| 时间写进 PMU 的 RTC，断电后开机自动恢复 | 完成 |
| 番茄服务器扫描里没有的书，改查番茄目录接口数章节 | 完成 |
| 书库删书时同步移出番茄书架；最近搜索存 NVS | 完成 |
| 图片封面：EPUB 内封面、番茄下载时取封面；书库、首页、锁屏显示 | 完成 |
| 设置 → 关于 Treader：版本、项目地址、开源致谢 | 完成 |
| 锁屏壁纸：设置里从 TF 卡 wallpapers 文件夹选图、全屏预览 | 完成 |
| 设置 → TF 卡：状态、重新挂载、格式化（两步确认）并建好文件夹 | 完成 |
| 拔卡不崩、重新插卡自动挂载；书和图片只存 TF 卡 | 完成，见下文 |
| 书城：FanqieRankTracker 榜单，频道/分类/榜单/详情，直接下载到书库 | 完成，见下文 |
| 分区表改成两个 7 MB 程序槽；TF 卡升级，起不来自动退回 | 完成，见下文 |

## 其他行为说明

- **时间**：每次番茄同步或 WebDAV 同步联网时用 SNTP 对时，随后写进 PMU 的 RTC；开机进首页时从 RTC 读回。
- **章节数兜底**：检查更新时，服务器扫描结果里没有的书，直接请求
  `https://fanqienovel.com/api/reader/directory/detail?bookId=…`，边下载边数 `"itemId"` 出现次数作为章节数，不解析整个 JSON。
- **阅读中被更新**：正在读的番茄书被后台更新替换后，阅读器保存进度时改用新的文件大小（只追加章节，位置仍有效）。
- **封面**：EPUB 按 OPF 里的 `cover-image` 或 `<meta name="cover">` 找封面，找不到就用文件名带 cover 的图片；
  番茄书在下载时取搜索结果里的 `thumb_url`，存成 `tomato/covers/<文件名>.img`（只收 JPEG/PNG，拿到 WEBP 会去掉图片处理参数再要一次）。
  第一次在书库或首页看到某本书时生成 300×400 灰度缩略图，缓存到 `.treader/covers/`，一本约 120 KB；
  生成期间先显示文字封面，全部生成完整页刷新一次。源文件大小或修改时间变了会重建。锁屏只读缓存，不解码。
  打开书时释放内存里的缩略图（约 1 MB PSRAM）。
- **锁屏壁纸**：设置 → 锁屏壁纸，只列 TF 卡 `wallpapers/` 里的 JPG/PNG（单张不超过 2 MB）。点一张先做全屏缓存并预览，
  再选“设为壁纸”。缓存是 684×1216 的 16 级灰度（Floyd–Steinberg 抖动），在 `.treader/wall/`。
  原图先被解码器限制在 648×1000 以内再放大，所以大图会略软。锁屏时壁纸铺满，底部白条写在读的书、进度和电量；
  缓存丢了（比如换卡）会在屏幕上电前重建。路径存在 NVS，图片被删就退回书卡。
- **书和图片只放 TF 卡**：系统 flash 只放固件、设置和阅读进度（NVS）。官方固件没卡时会把书放进内置 5 MB 的 storage
  分区，Treader 去掉了这个分区（空间给了两个程序槽）：没卡时书库提示插卡，传书页不开始传。书库的筛选改成 全部 / 番茄 / 其他书。
- **TF 卡管理**：设置 → TF 卡。格式化要先点“清空并格式化”，再点一次“再点一次确认”；同步进行中不允许。
  格式化前切回内置字体、释放封面缓存和词库，格式化后建好 `books fonts ime wallpapers tomato` 并写一份 `README.txt`，
  清掉壁纸设置。字体和词库要从电脑重新复制。
- **拔卡和换卡**：
  - 拔卡时主循环通知当前页收尾（书库关书、传书停服务、番茄页停同步），取消后台同步，字体切回内置。
    之后读卡的地方都按“文件不存在”处理：封面画文字封面，锁屏用书卡，番茄页提示失败。
  - 卡重新插上（或开机后才插卡）时，等后台同步结束，自动重新挂载；挂好后重载词库、清封面缓存、当前页重新进入一次，
    卡上的字体由定期重试恢复。坏卡或没格式化的卡挂载失败后不会反复重试，拔出再插或到设置 → TF 卡处理。
  - 这些都只做了代码检查，**还没在真机上拔插测试**。
- **键盘**：语言键在 全拼 → 九键 → 英文 之间循环，“123”切数字符号。书库搜索默认英文（匹配拼音首字母），番茄搜索默认全拼。

## 系统升级（TF 卡）

- 分区表（`partitions_16M.csv`）：nvs 0x9000、phy_init 0xE000 不变；ota_0 0x10000 和 ota_1 0x710000 各 7 MB；otadata 0xE10000。
  没有 factory 分区。`sdkconfig.defaults` 打开了 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`。
- 入口：设置 → 关于 Treader → “版本”那一行。页面扫描 TF 卡 `update/` 和根目录的 `.bin`（最多 8 个），检查文件头：
  ESP32-S3（chip id 9）、工程名 Read_Pico、大小不超过程序槽。`Read_Pico.bin` 和合并镜像都能用，合并镜像只取 0x10000 之后的主程序。
- 写入在 `on_tick` 里分块做（每次 32 KB，边擦边写），每 5% 局部刷新进度条；`esp_ota_end` 校验通过后切换启动分区并重启。
  本页设了 `holds_pmu` 和 `blocks_idle_sleep`，升级时电源键不锁屏、不自动睡眠。电量低于 30% 且没充电、或后台同步在跑时拒绝开始。
  拔卡会取消升级，当前版本不受影响。
- 回退：`app_main` 在屏幕和触摸都起来、进入主循环前调用 `esp_ota_mark_app_valid_cancel_rollback()`。新固件没走到这一步就复位的话，
  引导程序下次启动会退回旧槽。
- 网页刷机写完程序会把 otadata 擦成全 FF，保证从刚写入的 ota_0 启动（设备之前可能在 ota_1 上跑）。

## 书城

番茄页顶部“书架 / 书城”切换（番茄标签会记住上次停在哪个）。数据来自 FanqieRankTracker 网站，和安卓版书城同源：

- `api/lastest/index.json`（约 6 KB）列出 37 个分类和各自的文件；每个分类一个文件（约 55 KB，20 本）。不取 1 MB 的 `all.json`。
- **只在点的时候联网**：点“刷新”取分类列表；打开没缓存的分类时取这个分类；进详情页时只为这一本取封面；点“下载到书库”下载。
  取到的内容缓存在 TF 卡 `tomato/store/`，之后打开直接显示。榜单日期变了，旧分类缓存会被清掉。
- 榜单里的番茄链接带 `book_id`，所以详情页可以直接下载（走番茄服务器，和搜索结果的下载同一条路），不用再搜一次。
  详情页取过的封面在下载时直接复制，不重复下载。
- **榜单地址**：`local.properties` 里 `STORE_URL=` 设编译默认（不写就是 `https://bidabrain.github.io/FanqieRankTracker`）；
  设置 → 书城榜单 可以改成自定义地址，留空恢复默认，存在 `config.json` 的 `store_url`。填安卓版那种 `.../api/lastest/all.json`
  也可以，会自动截到网站根。这个地址不是秘密，固件里明文保存，界面正常显示。
- 不做安卓版的轮播横幅、随机抽 10 本和封面网格。

## WebDAV 同步

Treader 设备和 WebDAV 服务器之间同步（与安卓版无关），只在“设置 → 立即同步”时运行，用完断网：

- 服务器目录下放一个 `treader-sync.json`，用 GET 读、PUT 写，HTTP Basic 认证。坚果云要用“应用密码”。
- **阅读进度**：TF 卡 `tomato/webdav_state.json` 记着上次同步时每本书的进度签名。本机进度变了就以本机为准上传；
  本机没变、服务器那条在上次同步之后更新过，就以服务器为准写回本机。本机文件不能比记录时小（番茄更新只追加章节）。
- **番茄书架**：按 `book_id` 取并集。只在服务器上的书加进本机书架，标成有新章节，点“下载全部更新”即可拉下来。
- 书籍文件本身不传；删除不同步。

## TF 卡目录

```
/sdcard/
├── books/                 # 系统书架；番茄下载的 TXT 也放这里
├── fonts/                 # 中文 TTF（官方固件规则，默认寒蝉端黑体）
├── ime/pinyin-v1.bin      # 拼音词库，从仓库 sdcard/ime/ 整个复制
├── wallpapers/            # 锁屏壁纸，JPG/PNG
├── .treader/              # 设备自动生成的缓存，可以整个删掉
│   ├── covers/            #   封面缩略图
│   └── wall/              #   全屏壁纸
└── tomato/
    ├── config.json        # 服务器地址与密码，参考 sdcard/tomato/config.example.json
    ├── shelf.json         # 番茄书架元数据，设备自动维护
    ├── store/             # 书城榜单缓存和详情页封面
    └── covers/            # 番茄书封面原图，下载时自动保存
```

`config.json`：

```json
{
  "server": "http://192.168.1.10:18423",
  "password": "your-password-here",
  "webdav_url": "https://dav.jianguoyun.com/dav/treader",
  "webdav_user": "you@example.com",
  "webdav_password": "app-password"
}
```

这些都可以直接在设备的“设置”里用键盘填写。

### 内置默认服务器

和安卓版一样，可以在编译时把默认番茄服务器打进固件。在项目根目录建 `local.properties`（已在 `.gitignore` 里，不会入库）：

```properties
DOWNLOADER_URL=http://你的服务器:端口
DOWNLOADER_PASSWORD=你的密码
```

也可以用同名环境变量。编译时会打印 `Treader: built-in Tomato server enabled`（不打印值）。规则：

- 设置里填了服务器就用填的；没填（或填写时留空）就用内置的。
- 界面上只显示“内置服务器”，不显示地址和密码；编辑时也不回显。
- 地址和密码在固件里是异或混淆的，`strings` 之类的工具搜不到明文。但这只是混淆不是加密：
  拿到固件的人仍然能逆向出来，和安卓 APK 里的 BuildConfig 一样。**带内置服务器编出的 `.bin` 不要公开发布。**
- 公开仓库的 CI 没有这个文件，编出来的固件不带内置服务器。

Wi-Fi 沿用系统“传书”页里保存的网络，不需要另外设置。

## 结构

| 路径 | 职责 |
| --- | --- |
| `components/pinyin_ime/` | 离线拼音引擎：载入 PICOIME1 词库到 PSRAM，全拼/九键整句候选。不画界面。 |
| `components/tomato/tomato_net.c` | 用系统保存的凭据一次性连 Wi-Fi，用完全部释放。 |
| `components/tomato/tomato_http.c` | 下载服务器的 JSON 请求和流式文件下载。 |
| `components/tomato/tomato_store.c` | TF 卡上的 `config.json` 和 `shelf.json`。 |
| `components/tomato/tomato_sync.c` | 后台任务：检查更新、搜索、下载、WebDAV 同步。页面只轮询状态。 |
| `components/tomato/tomato_webdav.c` | WebDAV 同步与合并规则。 |
| `components/tomato/tomato_rank.c` | 书城榜单地址、TF 卡缓存和解析；联网在 `tomato_sync.c` 的 OP_RANK / OP_RANK_COVER。 |
| `main/apps/app_tr_store.c` | 书城页：频道和分类、分类榜单、书籍详情。 |
| `main/apps/app_tr_update.c` | 系统升级：扫描 TF 卡固件、分块写入另一个程序槽、校验后重启。 |
| `main/book/book_cover.c` | 封面提取、缩略图缓存（TF 卡）和绘制，只在主任务调用。 |
| `components/read_pico_transfer/` | 官方传书组件；新增 `read_pico_transfer_load_wifi()` 供番茄同步读取凭据。 |
| `tools/ime/build_pico_ime_dictionary.py` | 重新生成拼音词库。 |

检查更新和下载是两个独立操作，都只在用户点击时运行，**不自动检查**：

- `tomato_sync_start_check()`：只向服务器取最新章节数，写进 `shelf.json` 的 `remote` 字段。
- `tomato_sync_start_update()`：下载 `remote` 大于本地 `chapters` 的书，用的是上次检查的结果。没有待下载的书时不连网。

更新判断沿用安卓版 v1.7.3 的规则：服务器章节数取 `max(remote_total, local_total)`，大于本地章节数才算有更新。

搜索结果带简介（服务器 `/api/search` 返回的 `abstract`，截到约 100 个汉字），用于书籍详情页；点“下载到书库”才下载。

## 编译

需要 ESP-IDF v6.1：

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p COM3 flash monitor
```

## 致谢与许可

- 官方固件：MindReset，Apache-2.0；`components/epdiy` 为 LGPL-3.0；内置字体为 SIL OFL-1.1。
- 界面风格参考 [RickyOS](https://dot.mindreset.tech/docs/read_0/start/rickyos)（Ricky AI Studio）；锁屏壁纸、图片封面、网页刷机、TF 卡升级的思路参考 [Take Easy](https://take-easy.pages.dev/)。只借鉴设计与思路，未使用其代码。
- 拼音引擎算法与 PICOIME1 词库格式改编自 [XH View OS For Pico](https://github.com/XHFO/XH-View-OS-For-Pico)（XH Lab，Apache-2.0），见 `components/pinyin_ime/NOTICE`。
- 词库数据来自 jieba 0.42.1 与 pypinyin 0.55.0（MIT），见 `sdcard/ime/licenses/`。
- 番茄服务器接口与更新规则来自 Tomato-Novel 安卓版。

## 静态检查与已知限制

静态分析构建（GCC `-fanalyzer` 加 `-fstack-usage`，只作用于 Treader 相关代码）：

```bash
idf.py -B build-analyze -DTREADER_ANALYZE=1 build
```

每个函数的栈占用写在 `build-analyze/**/*.su`。目前 Treader 代码没有 analyzer 警告；
后台同步任务栈 14 KB，大缓冲都在 PSRAM 工作区里。大的静态数组用 `EXT_RAM_BSS_ATTR` 放 PSRAM
（`sdkconfig.defaults` 打开了 `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`）。

为了不出错而定的规则：

- **正在阅读的书不更新**：阅读器还开着旧文件，替换会读到乱码；“下载全部更新”会跳过它并提示。
- **同步期间不自动锁屏**；按电源键锁屏且会断电时，先取消同步并等它停下（最多 8 秒）。
- **书库删书**时若同步正在进行，删除记录排队，等同步结束再处理，免得被写回。
- 删掉的番茄书记在 `tomato/removed.json`，WebDAV 两边合并时不会再加回来。
- **WebDAV**：没对上时不同步；服务器文件读得到但解析失败时不覆盖；某本书在本机从没同步过而服务器有，以服务器为准。
- 文件写入都是“写 .tmp 再改名”，改名失败或掉电时下次读取用 .tmp 恢复。

已知限制（没做）：

- 查询服务器任务状态时每次轮询都重新建立 TLS 连接，没有复用。
- WebDAV 没用 ETag，两台设备同时同步可能丢掉其中一方的这次更新（下次同步会补上）。
- 番茄页进入时会对每本书做一次 `stat` 和 NVS 读取（64 本以内，约百毫秒级）。
- 浅睡锁屏期间换卡，驱动看不到拔出，解锁后仍当成原来那张卡。默认锁屏是关机，不受影响；用浅睡时换卡请先解锁再换。
- 用 esptool 把合并镜像整个写到 0x0 不会重置 otadata；设备上次从 ota_1 启动的话，新写的 ota_0 不会生效。命令行请用 `idf.py flash`。

## 界面代码

| 路径 | 职责 |
| --- | --- |
| `main/ui/tr_ui.c` | 新风格的绘制原语：状态栏、标签栏、文字封面、点线进度条、胶囊按钮、线条图标、自动换行。 |
| `main/ui/tr_kbd.c` | 屏幕键盘：26 键拼音、英文、数字符号，带候选条。 |
| `main/app/tr_nav.c` | 标签页切换，以及从别的页面直接打开一本书。 |
| `main/apps/app_tr_home.c` | 首页。 |
| `main/apps/app_tr_tomato.c` | 番茄标签页：书架、检查更新、下载全部更新、搜索、结果、详情。 |
| `main/apps/app_tr_settings.c` | 设置页：番茄服务器、WebDAV 的编辑和“立即同步”；提供进度导出/导入给 WebDAV。 |
| `main/apps/app_book.c` | 官方书库与阅读器，书架和工具条改成新排版，逻辑不变。 |
| `main/apps/tr_lock.c` | 锁屏书卡；没有在读的书时用官方锁屏图。 |

带标签栏的页面在 `app_desc_t` 里设 `.tab_nav = true`，右下角菜单按钮的点击区让给标签栏；中键仍然打开菜单。
新增界面文字后要重新生成内置字体子集：`python tools/gen_builtin_font.py`（需要 fonttools）。
