# WEBTOON 漫画安卓客户端

独立的安卓 App，功能和小纸 Pico 固件里的 WEBTOON 发现与追更一样：读你自己部署的 [webtoon 服务器](../README.md)，逛 WEBTOON 的排行、类型、星期更新表、已完结和搜索，订阅后点“检查更新”把新话下载成 CBZ，再在手机上一屏一页地看。界面照固件的黑白风格做（封面也只显示灰度），翻页不滚动。

> **只拉公开免费的内容，只给自己用。** 服务器不登录 WEBTOON，只读不登录就能看的话。请自己部署、只给自己离线阅读，不要把服务器开放给别人，也不要分享下载的漫画。

| 页面 | 内容 |
| --- | --- |
| 我的漫画（首页） | 全部 / 已订阅；一部一行：封面、话数、下载到第几话、“N 话新”（下载了还没打开的话）。右上角“检查更新”：把全部订阅发给服务器问新话，每话等服务器转好（每 3 秒问一次，显示进度）再下载，先写 `.part`、下完改名；一次最多下载设置里的话数，失败的那话跳过，结束时列出来。点一部进话列表：继续看、详情（订阅的）或删除（本地的） |
| 发现页 | 搜索栏（服务器把简体关键词转成繁体再搜）；排行 / 类型 / 星期 / 完结，二级分段用服务器给的列表；三列封面网格分页，左右滑也翻页 |
| 详情 | 封面、作者、类型、免费话数、连载 / 完结、简介；订阅时选“从第 1 话”“只追新的”“最近 5 话”，订阅后马上下载这一部；取消订阅时问要不要一起删掉已下载的话（默认保留，变成普通本地漫画） |
| 阅读 | 一屏一页（服务器已经切成 648×1000 的灰度页）；点左三成上一页、右三成下一页、中间开工具条，左右滑、音量键也翻页；每话记住看到第几页；翻过最后一页是“本话完”，一点看下一话。工具条：回我的漫画、页码和进度条、上一话、第一页、下一话 |
| 设置 | 服务器地址、设备令牌（请求头 `X-Webtoon-Token`）、测试连接；语言版本（繁体中文 / English）、简体显示、每次最多下载（5 / 10 / 20 / 全部） |

## 安装和使用

1. 部署 webtoon 服务器，见 [../README.md](../README.md)。
2. 在 GitHub Releases 下载 `webtoon-android-版本号.apk` 安装。以后的版本直接覆盖安装，漫画、进度和设置都保留。
3. 设置 → 服务器地址填 webtoon 面板“设备与外网”里的地址（局域网 `http://服务器IP:8095`，在外面用 Cloudflare 隧道填 `https://你的域名`），设备令牌粘贴那里的令牌。点“测试连接”，显示“正常”就好了。
4. 到发现页点一部，“订阅”，选从哪一话开始。回我的漫画看下载进度，以后点“检查更新”追新话。

## 文件

漫画在 App 自己的目录里，结构和设备 TF 卡一样（[../README.md](../README.md) 的“设备上的文件”）：

```
comics/
└── <作品名>/
    ├── .webtoon               # 订阅标记：{"id","lang","title","author","last","added"}
    ├── 第0001话 标题.cbz
    └── 第0002话 标题.cbz      # 话号补零到 4 位
```

订阅就是扫这些 `.webtoon`，没有别的数据库；`last` 是已下载的最大话号，某一话下载失败时 `last` 停在它前面，下次检查会再下。阅读进度在 `progress.json`（按“文件夹/文件名”记页码）。卸载 App 会一起删掉。

语言版本和简体显示只影响发现页和以后的订阅；已订阅的作品按 `.webtoon` 里记的语言追更。

## 版本和发布

- 版本号独立于固件、服务器和其他 App，写在 [version.properties](version.properties) 的 `versionName`。
- 发布：改 `versionName` 并提交，然后打 tag `webtoon-android-v版本号` 推上去，GitHub Actions（`.github/workflows/webtoon-android.yml`）先跑单元测试，再编译 APK 并发到 Releases（`webtoon-android-版本号.apk`）。也可以在 Actions 页手动运行，只产出构建产物、不发 Release。
- CI 上 `versionCode` = `version.properties` 里的数 + 运行序号，每次都比上一次大，可以覆盖安装。

## 签名

发布版用 WEBTOON App 自己的固定 keystore 签名（和其他 App 都不是同一把），每次发布、包括你本机编的版本，都能互相覆盖安装。keystore 不进仓库：

- 本机：`~/.android/webtoon.keystore`（别名 `webtoon`，口令 `android`）。没有这个文件编译发布版会失败（`assembleDebug` 不受影响）。第一次可以这样生成，**生成后请另外备份**，丢了就发不出能覆盖安装的新版本：

  ```bash
  keytool -genkeypair -keystore ~/.android/webtoon.keystore -storepass android -keypass android \
    -alias webtoon -keyalg RSA -keysize 2048 -validity 36500 -dname "CN=webtoon"
  ```

- CI：从仓库 Secret `WEBTOON_KEYSTORE_BASE64` 还原，用环境变量 `WEBTOON_KEYSTORE` 告诉 Gradle 用它签名，编完核对 APK 签名和这把钥匙一致，没设 Secret 或对不上都直接失败。

设置 Secret：在 **PowerShell** 里运行下面这条（直接复制到剪贴板，不在屏幕上显示），粘贴到 GitHub 仓库 → Settings → Secrets and variables → Actions → New repository secret，名字 `WEBTOON_KEYSTORE_BASE64`：

```powershell
[Convert]::ToBase64String([IO.File]::ReadAllBytes("$env:USERPROFILE\.android\webtoon.keystore")) | Set-Clipboard
```

## 本机编译和测试

需要 JDK 17 以上和 Android SDK（在 `local.properties` 里写 `sdk.dir`，或设 `ANDROID_HOME`）：

```bash
cd webtoon/android
./gradlew testDebugUnitTest assembleRelease
```

APK 在 `app/build/outputs/apk/release/app-release.apk`。单元测试（`app/src/test/`）核对 `.webtoon` 读写、文件命名（补零、去掉不能用的字符）、检查更新回复的解析、订阅起点、每次检查的下载上限和 `last` 的推进，以及扫 `comics/` 得到订阅；阅读进度在后台写，多个线程同时改也不会写乱。

## 目录

| 路径 | 内容 |
| --- | --- |
| `app/src/main/java/com/treader/webtoon/data/Model.kt` | 纯逻辑：`.webtoon`、文件命名、回复解析、订阅起点和下载计划 |
| `app/src/main/java/com/treader/webtoon/data/Api.kt` | 设备接口和本机设置 |
| `app/src/main/java/com/treader/webtoon/data/Shelf.kt` | 扫 `comics/`、订阅 / 取消订阅、阅读进度 |
| `app/src/main/java/com/treader/webtoon/data/Updater.kt` | 检查更新：问新话、等转换、下载 |
| `app/src/main/java/com/treader/webtoon/data/Cbz.kt` | 打开 CBZ，一次解一页 |
| `app/src/main/java/com/treader/webtoon/ui/` | 黑白组件、分页列表、我的漫画、发现页和详情、阅读、设置 |
