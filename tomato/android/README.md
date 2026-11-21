# 番茄安卓客户端

独立的安卓 App，功能和小纸 Pico 的 Treader 固件里的“书库”和“番茄”标签一样：读你自己部署的[番茄下载服务器](../README.md)，搜书、下载 TXT、检查更新、逛书城，再在手机上读。界面照固件的黑白风格做（封面也只显示灰度），阅读进度和番茄书架可以用 WebDAV 和小纸 Pico 互通。

和以前的纯安卓版 [Tomato-Novel](https://github.com/bidabrain/Tomato-Novel) 是两个 App：包名不同，可以同时装；同步文件也不同（这里用设备的 `treader-sync.json`），互不影响。

| 页面 | 和固件一样的地方 | 手机上的不同 |
| --- | --- | --- |
| 书库 | 书名、来源、进度条；“继续阅读”卡片；全部 / 番茄 / 其他书筛选、按最近 / 按书名排序、管理（批量清进度、删除）；长按一本看详情；番茄有更新时提示 | 上下滚动不分页；右上角“导入”从手机里选 TXT、EPUB 复制进来；搜索只按书名、文件名、作者找 |
| 阅读 | 按页翻：默认点左三成上一页、右三成下一页、中间开工具条，左右滑也翻；设置里的“翻页区域”可换成中下翻页（点上面开工具条，下面左边一条上一页、其余下一页）或点哪都翻（点上面开工具条，其余下一页），和固件一样；工具条：目录、排版、书库、A− 字号 A+、自动翻页（3～30 秒）、全书进度条，进度条两边“上一章”“下一章”直接跳到那一章开头（首末章变灰） | 音量键也能翻页；字号沿用设备的数（36～72），手机上约 ×0.4 sp；字体在设置里选宋体或黑体 |
| 排版 | 行距、段距、页边距、首行缩进、下划线，档位和设备一样，改了按当前位置重排 | — |
| 番茄书架 | 检查更新、下载全部更新（只在点的时候联网）、搜索、结果、详情、下载到书库 | 搜索用手机输入法；列表滚动 |
| 书城 | FanqieRankTracker 榜单：男频 / 女频、分类、三列封面榜单、详情、下载；github.io 连不上时自动换 jsDelivr 镜像 | — |
| 设置 | 番茄服务器地址和密码、测试连接、书城榜单地址、WebDAV 和立即同步 | 能直接粘贴，没有固件的“手机网页填写”；App 里没有内置服务器 |

## 安装和使用

1. 在 GitHub Releases 下载 `tomato-android-版本号.apk` 安装。以后的版本直接覆盖安装，书、进度和设置都保留。
2. 设置 → 番茄服务器：地址填 tomato 面板“设备与外网”里的地址（局域网 `http://服务器IP:18423`，在外面用 Cloudflare 隧道填 `https://你的域名`），服务器密码填那里的设备密码。自己部署的官方下载器也可以，填它的地址和密码。
3. 点“测试连接”，显示“正常”就好了。到“番茄”里搜书下载，书会出现在书库。

书、进度、番茄书架都在 App 自己的目录里（结构和 TF 卡一样：`books/`、`tomato/shelf.json` 等），卸载 App 会一起删掉；要保留就先用 WebDAV 同步。

## 和小纸 Pico 同步（WebDAV）

设置 → 同步：填 WebDAV 目录地址、账号、密码（坚果云用“应用密码”），点“立即同步”。和设备用同一个文件 `treader-sync.json`、同样的规则：

- **阅读进度**：按书的文件名对应。哪边改过就以哪边为准；本机从没同步过的书以服务器为准，新装的 App 不会冲掉设备上的进度。书的文件不传。
- **番茄书架**：按番茄书号合并。只在另一边有的书会加进书架、标成有新章节，点“下载全部更新”从**当前**服务器下载，换了下载服务器也能用。“上次打开时的章节数”两边取大，在一边看过，另一边的“+N 章”也消失。
- **对得上的前提**：进度记的是“第几章、章内第几个字节”。App 的 TXT、EPUB 解析逐字节照搬固件（单元测试拿固件代码生成的结果核对），所以同一个文件两边的位置完全一样。手机和设备分页不同，同步后打开的是包含那个位置的那一页。
- **文件不一样时**：如果两边的书不是同一个文件（比如换了下载服务器，生成的 TXT 有细微差别），位置超出这本书的进度不会写回，同步结果里会说“N 本书的文件和备份时不一样”。

## 版本和发布

- 版本号独立于固件、服务器和其他 App，写在 [version.properties](version.properties) 的 `versionName`。
- 发布：改 `versionName` 并提交，然后打 tag `tomato-android-v版本号` 推上去，GitHub Actions（`.github/workflows/tomato-android.yml`）先跑单元测试，再编译 APK 并发到 Releases。也可以在 Actions 页手动运行，只产出构建产物、不发 Release。
- CI 上 `versionCode` = `version.properties` 里的数 + 运行序号，每次都比上一次大，可以覆盖安装。

## 签名

发布版用番茄 App 自己的固定 keystore 签名（和小红书、微博 App 都不是同一把），每次发布、包括你本机编的版本，都能互相覆盖安装。keystore 不进仓库：

- 本机：`~/.android/tomato.keystore`（别名 `tomato`，口令 `android`）。没有这个文件编译发布版会失败。**请另外备份它**，丢了就发不出能覆盖安装的新版本。
- CI：从仓库 Secret `TOMATO_KEYSTORE_BASE64` 还原，用环境变量 `TOMATO_KEYSTORE` 告诉 Gradle 用它签名，编完核对 APK 签名和这把钥匙一致，没设 Secret 或对不上都直接失败。

设置 Secret：在 **PowerShell** 里运行下面这条（直接复制到剪贴板，不在屏幕上显示），粘贴到 GitHub 仓库 → Settings → Secrets and variables → Actions → New repository secret，名字 `TOMATO_KEYSTORE_BASE64`：

```powershell
[Convert]::ToBase64String([IO.File]::ReadAllBytes("$env:USERPROFILE\.android\tomato.keystore")) | Set-Clipboard
```

## 本机编译和测试

需要 JDK 17 以上和 Android SDK（在 `local.properties` 里写 `sdk.dir`，或设 `ANDROID_HOME`）：

```bash
cd tomato/android
./gradlew testDebugUnitTest assembleRelease
```

APK 在 `app/build/outputs/apk/release/app-release.apk`。

单元测试 `FirmwareParityTest` 用 `app/src/test/resources/books/` 里的测试书，核对 App 的章节表、每章文字和 EPUB 标题块和固件逐字节一致。标准答案 `firmware_chapters.tsv` 是用固件的 `main/book` 代码在 gcc 容器里打印的；改了固件的书源代码或测试书后，在仓库根目录重新生成（需要 Docker）：

```bash
bash tomato/android/tools/gen_fixtures.sh
```

## 目录

| 路径 | 内容 |
| --- | --- |
| `app/src/main/java/com/treader/tomato/book/` | 书源（`TxtBook`、`EpubBook`、`HtmlText`，照搬固件）和分页（`Layout`） |
| `app/src/main/java/com/treader/tomato/data/TomatoApi.kt`、`TomatoOps.kt` | 番茄服务器接口；检查更新、下载全部更新、搜索、下载（照固件 `tomato_sync.c`） |
| `app/src/main/java/com/treader/tomato/data/Shelf.kt`、`Progress.kt`、`Library.kt` | `shelf.json` / `removed.json`、阅读进度、书库 |
| `app/src/main/java/com/treader/tomato/data/Store.kt` | 书城榜单和缓存 |
| `app/src/main/java/com/treader/tomato/data/WebDav.kt` | 和设备互通的 WebDAV 同步 |
| `app/src/main/java/com/treader/tomato/ui/` | 黑白组件、书库、阅读、番茄、书城、设置 |
| `app/build.gradle.kts` 的 `gbkAsset` | 编译时从固件 `main/book/gbk_table.h` 生成 GBK 码表 `gbk.bin`（放进 assets，不入库） |
| `tools/` | 测试书生成、固件对照程序和生成脚本 |
