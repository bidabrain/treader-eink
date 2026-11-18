# 微博安卓客户端

独立的安卓 App，功能和小纸 Pico 的 Treader 固件里的“微博”标签一样：读你自己部署的 [weibo 服务器](../README.md)，看关注的人和热门的微博。界面照固件的黑白风格做，只读，不点赞、不评论、不转发。

| 页面 | 和固件一样的地方 | 手机上的不同 |
| --- | --- | --- |
| 列表 | 关注 / 热门分段；一行一条：作者、时间、正文前几行（转发的接原文）、转评赞，有图的右边一张缩略图（多图标“N 图”），没看过的标“新” | 上下滑动，不分页 |
| 正文 | 顶部“正文 / 图片 / 评论”分段；时间、来源、转发和赞；转发的原文接在正文后面 | 文字直接上下滚动，不分页 |
| 看图 | 一屏一张，左右三成点按或滑动翻图，底栏“‹ 正文 · k / n · 评论 ›”；第一张再往前回正文，最后一张再往后是评论 | 彩色屏照样显示灰度图（服务器已转好） |
| 评论 | 第一次打开时服务器才去微博取一次（前 20 条），之后用缓存 | — |
| 设置 | 右上角设置键：服务器地址、设备令牌、测试连接 | 能直接粘贴，所以没有固件的“手机网页填写”，换成输入框旁的“粘贴” |

## 安装和使用

1. 在 GitHub Releases 下载 `weibo-android-版本号.apk` 安装。以后的版本直接覆盖安装，设置和缓存都保留。
2. 打开 App，点右上角设置键，填服务器地址和设备令牌（weibo 面板 → 设备与外网，复制后点“粘贴”）。在外面用 Cloudflare 隧道的话填 `https://你的域名`。
3. 点“测试连接”，显示“正常 · 关注 N 条”就好了。回到首页点“刷新”。

缓存（列表、看过的全文和评论最多 400 个文件、图片）在 App 自己的目录里，不联网也能看已经下好的。

## 版本和发布

- 版本号独立于固件、服务器和小红书 App，写在 [version.properties](version.properties) 的 `versionName`。
- 发布：改 `versionName` 并提交，然后打 tag `weibo-android-v版本号` 推上去，GitHub Actions（`.github/workflows/weibo-android.yml`）编译 APK 并发到 Releases。也可以在 Actions 页手动运行，只产出构建产物、不发 Release。
- CI 上 `versionCode` = `version.properties` 里的数 + 运行序号，每次都比上一次大，可以覆盖安装。

## 签名

和小红书 App 一样，发布版也用固定的 debug keystore 签名，每次发布、包括你本机编的版本，都能互相覆盖安装。keystore 不进仓库：本机用 `~/.android/debug.keystore`；CI 从仓库 Secret `DEBUG_KEYSTORE_BASE64`（和小红书 App 共用）还原，用环境变量 `WEIBO_KEYSTORE` 明确告诉 Gradle 用它签名，编完核对 APK 签名和这把钥匙一致。设置 Secret 的方法见 [rednote/android/README.md](../../rednote/android/README.md#签名)。

## 本机编译

需要 JDK 17 以上和 Android SDK（在 `local.properties` 里写 `sdk.dir`，或设 `ANDROID_HOME`）：

```bash
cd weibo/android
./gradlew assembleRelease
```

APK 在 `app/build/outputs/apk/release/app-release.apk`。

## 目录

| 路径 | 内容 |
| --- | --- |
| `app/src/main/java/com/treader/weibo/data/Api.kt` | weibo 设备接口、地址和令牌 |
| `app/src/main/java/com/treader/weibo/data/Repo.kt` | 列表、全文和评论缓存、“看过”标记 |
| `app/src/main/java/com/treader/weibo/AppVm.kt` | 页面状态和动作 |
| `app/src/main/java/com/treader/weibo/ui/` | 黑白组件、列表、正文、看图、评论、设置 |
