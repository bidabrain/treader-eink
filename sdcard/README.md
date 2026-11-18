# TF 卡模板 / TF card template

把**本文件夹里的全部内容**（不是 `sdcard` 这个文件夹本身）复制到 FAT32 格式 TF 卡的根目录，就是 Treader 需要的完整结构：

Copy **everything inside this folder** (not the `sdcard` folder itself) to the root of a FAT32 TF card.

```
TF 卡根目录/
├── books/          放 TXT、EPUB；番茄下载的书也存这里
├── comics/         放 CBZ 漫画；一部漫画的多话放进同一个子文件夹
├── fonts/          中文字体 ChillDuanSansVF.ttf（必需，否则缺字）
├── ime/            拼音词库 pinyin-v1.bin（输入中文必需）
├── wallpapers/     锁屏壁纸，JPG/PNG，单张不超过 2 MB，建议 684×1216 竖图
├── update/         TF 卡升级用，放编译出的 Read_Pico.bin 或 treader-full.bin
└── tomato/
    └── config.example.json   可选：改名为 config.json 并填写服务器、WebDAV、榜单地址；也可以在设备“设置”里填
```

- 设备运行时会自己生成这些，不用复制，都可以整个删掉（删了会重新生成或重新下载）：
  - `tomato/shelf.json`、`tomato/store/`、`tomato/covers/`：番茄书架、书城和封面；
  - `.treader/`：书库封面和壁纸缓存；`comics/.treader/`：漫画进度和封面；
  - `hupu/`：虎扑缓存；
  - `rednote/`：小红书缓存（关注和推荐列表、笔记图片），总大小有上限。
- 小红书的服务器地址和令牌存在设备里（设置 → 内容来源），不在 TF 卡上。
- 各文件夹里的 `.gitkeep` 只是为了让 git 保留空文件夹，设备会忽略它，复制过去也没关系。
- 放进本文件夹 `books/`、`comics/`、`wallpapers/`、`update/` 的文件不会被 git 提交；`update/` 里的固件可能带着你的内置服务器，不要公开。
- 已经刷好 Treader 的话，也可以在 设置 → 电源与设备 → TF 卡 里格式化，设备会建好同样的文件夹，然后再从这里复制 `fonts/` 和 `ime/`。

## 字体部署 / Font deployment

将本目录的 `fonts/` 整个复制到 TF 卡根目录，包括字体及 `OFL.txt`。也可在设备 WiFi 网页的“上传字体”区选择 `fonts/ChillDuanSansVF.ttf`；保存后停止传书，在设备“字体”页选用。网页仅接受 TTF，许可文本随本部署目录保留。

Copy the entire `fonts/` directory, including `OFL.txt`, to the TF card root. Alternatively, select `fonts/ChillDuanSansVF.ttf` in the device webpage's font upload section, stop transfer after saving, and select the font on the device's Fonts page. The webpage accepts TTF only; keep the license with this deployment package.

此字体不嵌入固件，不占用内置图书分区；运行时按需从 TF 卡读取。没有 TF 卡时，仍使用仅覆盖界面与演示文字的内置子集。

This font is not embedded in firmware and does not consume the internal book partition. It is read from the TF card on demand. Without a card, the built-in subset covers the UI and demo text only.

## 字体来源与许可 / Font source and license

- 字体 / Font: ChillDuanSans VF，寒蝉端黑体。
- 上游发行 / Upstream release: [Warren2060/ChillDuanSans v1.30](https://github.com/Warren2060/ChillDuanSans/releases/tag/v1.30).
- 发行包 / Archive: `ChillDuanSans.v1.30.zip`，原文件 / original entry: `ChillDuanSans v1.3/ChillDuanSansVF.ttf`.
- 文件大小 / File size: 16,320,196 bytes.
- SHA-256: `966286fc44907d4123528e1efb98c08e51fff013cd083140fdad1bf6b593555c`.
- 随附字体逐字节保留上游原文件，未经子集化、重命名或修改；版权声明和 SIL Open Font License 1.1 原文见 [fonts/OFL.txt](fonts/OFL.txt)。
  The bundled font is byte-identical to the upstream original, without subsetting, renaming or modification. Copyright notices and the SIL Open Font License 1.1 are preserved in [fonts/OFL.txt](fonts/OFL.txt).

字体适用 OFL-1.1，不适用本仓库主体的 Apache-2.0。允许随软件部署、嵌入和再分发，但须保留版权和许可，不得单独销售字体；修改版须遵守保留字体名称等条款。以随附许可原文为准。

The font is licensed under OFL-1.1, not the repository's Apache-2.0 license. It may be bundled, embedded and redistributed with software while retaining copyright and license notices; it must not be sold on its own. Modified versions must follow the reserved font name conditions and other terms. The bundled license text governs.

内置 `main/assets/builtin.ttf` 是此字体的界面子集，生成器将其字体名设为 `Read Pico UI`，保留原版权与许可字段，以区别于未修改的上游字体。

The embedded `main/assets/builtin.ttf` is a UI subset. Its generator uses the font name `Read Pico UI` and preserves original copyright and license fields, distinguishing it from the unmodified upstream font.
