# X 安卓客户端

独立的安卓 App，照[微博安卓 App](../../weibo/android/README.md) 的样子做：读你自己部署的 [x 服务器](../README.md)，看“为你推荐”和“正在关注”的推文。界面和墨水屏固件一样是 Material 3 灰阶风格，只读，不点赞、不回复、不转推。

| 页面 | 内容 |
| --- | --- |
| 列表 | 为你推荐 / 正在关注分段（默认为你推荐）；一条一张卡片：转推的顶上一行“某某 转推”，头像圆、名字、@用户名和时间，“回复 @某人”，正文最多 5 行，引用的推文在灰框里，视频带“视频”角标（图片是封面），图片宫格，回复 / 转推 / 赞 / 浏览四栏；“正在关注”里没看过的头像上一个黑点 |
| 正在关注总开关 | x 面板里把“正在关注”总开关关掉后，这一段变灰、点不了；正在看的话自动回到为你推荐。打开 App、刷新和测试连接时会问一次服务器状态，上次知道的结果跟缓存一起存着，不联网时也照用 |
| 正文 | 顶部头像、名字和“正文 / 图片 / 回复”分段；引用的推文在灰框里；图片宫格和四栏计数 |
| 看图 | 一屏一张，左右三成点按或滑动翻图，底栏“‹ 正文 · k / n · 回复 ›”；点中间弹出“保存图片”，存进相册 `Pictures/Treader X/`（安卓 8、9 第一次要存储权限） |
| 回复 | 第一次打开时服务器才开浏览器去 X 取一次，之后用缓存；服务器忙或要重新登录时显示服务器给的说明 |
| 设置 | 右上角设置键：服务器地址、设备令牌、测试连接、列表条数（100 / 200 / 300） |

## 安装和使用

1. 安装 APK。以后的版本直接覆盖安装，设置和缓存都保留。
2. 打开 App，点右上角设置键，填服务器地址（例如 `http://192.168.1.20:8093`）和设备令牌（x 面板 → 设备与外网，复制后点“粘贴”）。在外面用 Cloudflare 隧道的话填 `https://你的域名`。
3. 点“测试连接”，显示“正常 · 推荐 N 条”就好了。回到首页点“刷新”。

缓存（列表、看过的全文和回复最多 400 个文件、图片）在 App 自己的目录里，不联网也能看已经下好的。

## 版本

版本号独立于固件、服务器和其他 App，写在 [version.properties](version.properties) 的 `versionName`；发布用的 tag 前缀是 `x-android-v`。

## 签名

发布版用 X App 自己的固定 keystore 签名（和微博、小红书、番茄 App 都不是同一把）。keystore 不进仓库：

- 本机：`~/.android/x.keystore`（别名 `x`，口令 `android`）。没有这个文件编译会失败。**请另外备份它**，丢了就发不出能覆盖安装的新版本。
- 也可以用环境变量 `X_KEYSTORE` 指到别的位置（比如 CI 从 Secret `X_KEYSTORE_BASE64` 还原出来的文件）。

## 本机编译

需要 JDK 17 以上和 Android SDK（在 `local.properties` 里写 `sdk.dir`，或设 `ANDROID_HOME`）：

```bash
cd x/android
./gradlew assembleRelease
```

APK 在 `app/build/outputs/apk/release/app-release.apk`。

## 目录

| 路径 | 内容 |
| --- | --- |
| `app/src/main/java/com/treader/x/data/Api.kt` | x 设备接口、地址和令牌 |
| `app/src/main/java/com/treader/x/data/Repo.kt` | 列表、全文和回复缓存、“看过”标记、正在关注总开关 |
| `app/src/main/java/com/treader/x/AppVm.kt` | 页面状态和动作 |
| `app/src/main/java/com/treader/x/ui/` | Material 3 灰阶组件、列表、正文、看图、回复、设置 |
