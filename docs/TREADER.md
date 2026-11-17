# Treader

基于[小纸 Pico 官方固件](https://github.com/MindReset/read_pico_firmware)的分支，把安卓版
[Tomato-Novel](https://github.com/bidabrain/Tomato-Novel) 的核心功能（搜索番茄小说、下载 TXT、
检查更新）做进系统。阅读器、目录、字体、传书全部沿用官方固件，只新增番茄同步和拼音输入。

设计原则：设备是 ESP32-S3 单片机，**能少做就少做**。只有黑白界面，没有颜色和主题，没有动画，
没有封面。联网只在一次操作期间打开，做完就断。

## 当前状态

| 部分 | 状态 |
| --- | --- |
| 菜单精简，开机进书架 | 完成（未上机验证） |
| 拼音输入法引擎 `components/pinyin_ime` | 完成（未上机验证） |
| 番茄同步 `components/tomato`：连网、搜索、下载、检查更新 | 完成（未上机验证） |
| 界面：番茄书架、搜索键盘、结果、下载确认、设置 | 设计稿见 [design/ui.html](design/ui.html)（RickyOS 风格），待确认后实现 |
| WebDAV 同步 | 暂缓，待定是否加回 |

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
