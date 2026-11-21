# 验证器安卓客户端

独立的安卓 App，功能和界面照 Treader 固件里的“验证器”标签做：离线显示两步验证码（TOTP，RFC 6238），和 Google Authenticator 算出的一样，不连任何服务器。还能把账号**发送到小纸 Pico**，省掉在设备上导入时要另一部手机拍照的麻烦。界面和墨水屏固件一样是 Material 3 灰阶风格。

手机上的 Google Authenticator 请一直保留作备份：这个 App 不导出密钥、不做云同步，忘了口令只能清空重来。

| 页面 | 内容 |
| --- | --- |
| 第一次 | 设验证器口令：至少 8 位，字母和数字都有 |
| 口令 | 打开 App 或回到 App（按设置）先输口令；输错 4 次以内直接重输，之后等 30 秒起、逐次加倍、最长 15 分钟，重启 App 也不清零 |
| 列表 | 每行发行方 · 名称、大号验证码、右边倒计时条，剩 5 秒内显示下一个码；按页翻（左右滑或底部 ‹ ›），点一行单独大字显示，长按删除。右上角：导入、发送到小纸 Pico、上锁、设置 |
| 导入 | 扫 Google Authenticator“转移账号 → 导出账号”的二维码（分几张就一张张扫，缺哪张会提示）或网站给的单个账号二维码；粘贴 `otpauth://` / `otpauth-migration://` 链接（一行一个）；手动填密钥。识别结果先列出来勾选，导入后清空输入。支持 SHA1 / SHA256 / SHA512、6 / 8 位、30 / 60 秒；HOTP（计数器型）不支持 |
| 发送到小纸 Pico | 设备（固件 1.0.0 起）打开验证器 → 导入；App 里选账号，扫设备屏幕上的热点二维码，手机自动连上设备热点（安卓 10 起，系统会问一下；更早的系统先在 Wi-Fi 设置里手动连），把账号发到 `http://192.168.4.1/form`，再在设备上点“导入”确认。只在手机和设备之间传，不经过路由器和互联网；固件不用改 |
| 设置 | 修改口令（先旧后新，重新加密）、自动上锁（离开 App 就上锁 / 离开 5 分钟后）、清空验证器（连点两次确认）、版本 |

## 安全

- **加密**：账号表用 AES-256-GCM 加密后存在 App 私有目录 `vault.bin`，格式和固件的 TF 卡文件同一套（KDF 编号 2）。钥匙 = Keystore HMAC 密钥（别名 `treader_auth_device`，拿不出手机）混合 PBKDF2-HMAC-SHA256(口令, 随机盐, 迭代次数)；迭代次数第一次设口令时按手机速度定成约 0.8 秒（至少 10 万次）。只有口令或只拿到文件都解不开。
- **上锁**：解锁后钥匙和账号只在内存里；上锁、App 进后台（按设置）、进程结束都会清掉。
- **不外泄**：不进系统备份和换机迁移（`allowBackup=false`、`dataExtractionRules`）；整个窗口禁止截图和录屏，最近任务里看不到内容。
- **权限**：相机（扫码，ZXing 本地识别）；网络只为连设备热点，明文 http 只放行 `192.168.4.1`（`network_security_config.xml`）。

## 版本

版本号独立于固件和其他 App，写在 [version.properties](version.properties) 的 `versionName`；发布用的 tag 前缀是 `auth-android-v`。

## 签名

发布版用验证器 App 自己的固定 keystore 签名（和其他 App 都不是同一把）。keystore 不进仓库：

- 本机：`~/.android/auth.keystore`（别名 `auth`，口令 `android`）。没有这个文件编译会失败。**请另外备份它**，丢了就发不出能覆盖安装的新版本。
- CI：Secret `AUTH_KEYSTORE_BASE64`（这个文件的 base64），工作流 `.github/workflows/auth-android.yml` 还原后用环境变量 `AUTH_KEYSTORE` 指过去，并核对 APK 签名。

## 本机编译和测试

需要 JDK 17 以上和 Android SDK（在 `local.properties` 里写 `sdk.dir`，或设 `ANDROID_HOME`）：

```bash
cd auth/android
./gradlew testReleaseUnitTest assembleRelease
```

单元测试（`app/src/test`）：RFC 6238 附录 B 向量、base32、otpauth 和迁移数据解析（迁移数据在测试里编码）、热点二维码、加密文件读写与损坏、口令规则和等待时间。APK 在 `app/build/outputs/apk/release/app-release.apk`。

## 目录

| 路径 | 内容 |
| --- | --- |
| `app/src/main/java/com/treader/auth/data/Totp.kt` | 验证码、base32、账号 |
| `app/src/main/java/com/treader/auth/data/OtpParse.kt` | otpauth、迁移数据、热点二维码解析（照搬固件导入网页） |
| `app/src/main/java/com/treader/auth/data/Vault.kt` | 加密文件格式和读写 |
| `app/src/main/java/com/treader/auth/data/AuthStore.kt` | Keystore 设备钥匙、会话、输错计数、自动上锁 |
| `app/src/main/java/com/treader/auth/data/PicoLink.kt` | 连设备热点、发送、等设备确认 |
| `app/src/main/java/com/treader/auth/AppVm.kt` | 页面状态和动作 |
| `app/src/main/java/com/treader/auth/ui/` | Material 3 灰阶组件、列表、口令、导入、发送、设置 |
