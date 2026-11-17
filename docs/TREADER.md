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
| 界面第二批：书库和阅读菜单改成新排版、锁屏书卡 | 待做 |
| WebDAV 同步 | 待做，见下文 |

## 计划：WebDAV 同步

Treader 设备和 WebDAV 服务器之间同步（与安卓版无关），只在设置里点击时运行：

- 同步内容：阅读进度（系统阅读器的 NVS 记录，每本几十字节）和番茄书架 `tomato/shelf.json`。书籍文件不传。
- 服务器上只放一个 JSON 文件，用 GET/PUT 读写；每本书的进度取更新时间较新的一方，书架按最后修改时间决定方向。
- 联网时用 SNTP 对时，时间戳可信。
- 配置（地址、用户名、密码）放在 `tomato/config.json`，在设置页编辑。

## TF 卡目录

```
/sdcard/
├── books/                 # 系统书架；番茄下载的 TXT 也放这里
├── fonts/                 # 中文 TTF（官方固件规则，默认寒蝉端黑体）
├── ime/pinyin-v1.bin      # 拼音词库，从仓库 sdcard/ime/ 整个复制
└── tomato/
    ├── config.json        # 服务器地址与密码，参考 sdcard/tomato/config.example.json
    └── shelf.json         # 番茄书架元数据，设备自动维护
```

`config.json`：

```json
{ "server": "http://192.168.1.10:8080", "password": "your-password-here" }
```

Wi-Fi 沿用系统“传书”页里保存的网络，不需要另外设置。

## 结构

| 路径 | 职责 |
| --- | --- |
| `components/pinyin_ime/` | 离线拼音引擎：载入 PICOIME1 词库到 PSRAM，全拼/九键整句候选。不画界面。 |
| `components/tomato/tomato_net.c` | 用系统保存的凭据一次性连 Wi-Fi，用完全部释放。 |
| `components/tomato/tomato_http.c` | 下载服务器的 JSON 请求和流式文件下载。 |
| `components/tomato/tomato_store.c` | TF 卡上的 `config.json` 和 `shelf.json`。 |
| `components/tomato/tomato_sync.c` | 后台任务：检查更新、搜索、下载。页面只轮询状态。 |
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
- 拼音引擎算法与 PICOIME1 词库格式改编自 [XH View OS For Pico](https://github.com/XHFO/XH-View-OS-For-Pico)（XH Lab，Apache-2.0），见 `components/pinyin_ime/NOTICE`。
- 词库数据来自 jieba 0.42.1 与 pypinyin 0.55.0（MIT），见 `sdcard/ime/licenses/`。
- 番茄服务器接口与更新规则来自 Tomato-Novel 安卓版。

## 界面代码

| 路径 | 职责 |
| --- | --- |
| `main/ui/tr_ui.c` | 新风格的绘制原语：状态栏、标签栏、文字封面、点线进度条、胶囊按钮、线条图标、自动换行。 |
| `main/ui/tr_kbd.c` | 屏幕键盘：26 键拼音、英文、数字符号，带候选条。 |
| `main/app/tr_nav.c` | 标签页切换，以及从别的页面直接打开一本书。 |
| `main/apps/app_tr_home.c` | 首页。 |
| `main/apps/app_tr_tomato.c` | 番茄标签页：书架、检查更新、下载全部更新、搜索、结果、详情。 |
| `main/apps/app_tr_settings.c` | 设置页，番茄服务器地址和密码在这里编辑。 |

带标签栏的页面在 `app_desc_t` 里设 `.tab_nav = true`，右下角菜单按钮的点击区让给标签栏；中键仍然打开菜单。
新增界面文字后要重新生成内置字体子集：`python tools/gen_builtin_font.py`（需要 fonttools）。
