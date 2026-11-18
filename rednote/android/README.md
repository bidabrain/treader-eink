# 小红书安卓客户端

独立的安卓 App，功能和小纸 Pico 的 Treader 固件里的“小红书”标签一样：读你自己部署的 [rednote 服务器](../README.md)，看关注博主和首页推荐的图文。界面照固件的黑白风格做，只读，不点赞、不评论。

| 页面 | 和固件一样的地方 | 手机上的不同 |
| --- | --- | --- |
| 列表 | 关注 / 推荐分段；封面、标题两行、作者 · 赞，没看过的封面上标“新”，推荐里下好的标“已下载” | 两列瀑布流、上下滑动，封面按原图比例（设备上是两列卡片、按页翻） |
| 推荐 | 点“刷新推荐”服务器才去读小红书首页；点开没下载过的弹等待框，点框外取消 | — |
| 看图 | 一屏一张，左右三成点按或滑动翻图，底栏“‹ 返回 · k / n · 正文 ›”，最后一张再往后是正文 | 彩色屏照样显示灰度图（服务器已转好） |
| 正文 / 评论 | 顶部“图片 / 正文 / 评论”分段，前 20 条评论 | 文字直接上下滚动，不分页 |
| 设置 | 右上角设置键：服务器地址、设备令牌、测试连接 | 能直接粘贴，所以没有固件的“手机网页填写”，换成输入框旁的“粘贴” |

## 安装和使用

1. 在 GitHub Releases 下载 `rednote-android-版本号.apk` 安装。以后的版本直接覆盖安装，设置和缓存都保留。
2. 打开 App，点右上角设置键，填服务器地址和设备令牌（rednote 面板 → 设备与外网，复制后点“粘贴”）。在外面用 Cloudflare 隧道的话填 `https://你的域名`。
3. 点“测试连接”，显示“正常 · 关注 N 篇”就好了。回到首页点“刷新”。

缓存（列表、看过的正文最多 200 篇、图片）在 App 自己的目录里，不联网也能看已经下好的。

## 版本和发布

- 版本号独立于固件和服务器，写在 [version.properties](version.properties) 的 `versionName`。
- 发布：改 `versionName` 并提交，然后打 tag `rednote-android-v版本号` 推上去，GitHub Actions（`.github/workflows/rednote-android.yml`）编译 APK 并发到 Releases。也可以在 Actions 页手动运行，只产出构建产物、不发 Release。
- CI 上 `versionCode` = `version.properties` 里的数 + 运行序号，每次都比上一次大，可以覆盖安装。

## 签名

发布版也用固定的 debug keystore 签名，所以每次发布、包括你本机编的版本，都能互相覆盖安装。keystore 不进仓库：

- 本机：Android SDK 自动生成的 `~/.android/debug.keystore`（别名 `androiddebugkey`，口令 `android`）。
- CI：从仓库 Secret `DEBUG_KEYSTORE_BASE64` 还原到同一个位置。没设这个 Secret，CI 会直接失败，不会发出签名不一样、装不上的 APK。

设置 Secret（和 Tomato-Novel 用的是同一个 keystore 的话，值也一样）：

```bash
base64 -w0 ~/.android/debug.keystore
```

把输出粘贴到 GitHub 仓库 → Settings → Secrets and variables → Actions → New repository secret，名字 `DEBUG_KEYSTORE_BASE64`。Windows 上在 **PowerShell** 里运行下面这条（直接复制到剪贴板，不在屏幕上显示）：

```powershell
[Convert]::ToBase64String([IO.File]::ReadAllBytes("$env:USERPROFILE\.android\debug.keystore")) | Set-Clipboard
```

## 本机编译

需要 JDK 17 以上和 Android SDK（在 `local.properties` 里写 `sdk.dir`，或设 `ANDROID_HOME`）：

```bash
cd rednote/android
./gradlew assembleRelease
```

APK 在 `app/build/outputs/apk/release/app-release.apk`。

## 目录

| 路径 | 内容 |
| --- | --- |
| `app/src/main/java/com/treader/rednote/data/Api.kt` | rednote 设备接口、地址和令牌 |
| `app/src/main/java/com/treader/rednote/data/Repo.kt` | 列表和正文缓存、“看过”标记 |
| `app/src/main/java/com/treader/rednote/AppVm.kt` | 页面状态和动作 |
| `app/src/main/java/com/treader/rednote/ui/` | 黑白组件、列表、看图、正文、设置 |
