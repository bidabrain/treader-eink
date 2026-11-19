import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

// 独立版本号在 version.properties；CI 上 versionCode 加运行序号，保证能覆盖安装。
// The standalone version lives in version.properties; on CI the run number is added to versionCode so each release
// installs over the previous one.
val versionProps = Properties().apply { rootProject.file("version.properties").inputStream().use { load(it) } }
val baseCode = versionProps.getProperty("versionCode").toInt()
val runNumber = System.getenv("GITHUB_RUN_NUMBER")?.toIntOrNull() ?: 0

// 发布版签名用番茄 App 自己的 keystore（别名 tomato，和小红书、微博 App 的都不是同一把）：CI 用环境变量 TOMATO_KEYSTORE 指过来
// （从 Secret TOMATO_KEYSTORE_BASE64 还原），本机默认 ~/.android/tomato.keystore。路径写明，找不到就编译失败，不会悄悄换钥匙。
// Release signing uses the Tomato app's own keystore (alias tomato, not the Xiaohongshu or Weibo key): CI points
// TOMATO_KEYSTORE at the one restored from the TOMATO_KEYSTORE_BASE64 secret; locally it defaults to
// ~/.android/tomato.keystore. The path is explicit, so a missing file fails the build instead of silently using
// another key.
val releaseKeystore = file(System.getenv("TOMATO_KEYSTORE") ?: "${System.getProperty("user.home")}/.android/tomato.keystore")

// GBK 码表直接从固件的 main/book/gbk_table.h 生成（不另存一份二进制），保证和设备解码一模一样。
// The GBK table is generated from the firmware's main/book/gbk_table.h (no second binary copy), so decoding matches
// the device exactly.
val gbkDir = layout.buildDirectory.dir("generated/gbk")
val gbkAsset = tasks.register("gbkAsset") {
    val src = rootProject.file("../../main/book/gbk_table.h")
    inputs.file(src)
    outputs.dir(gbkDir)
    doLast {
        val text = src.readText()
        val body = text.substring(text.indexOf('{') + 1, text.lastIndexOf('}'))
        val values = Regex("0x([0-9a-fA-F]+)").findAll(body).map { it.groupValues[1].toInt(16) }.toList()
        check(values.size == 23940) { "gbk_table.h: ${values.size} entries" }
        val out = gbkDir.get().file("gbk.bin").asFile
        out.parentFile.mkdirs()
        out.writeBytes(ByteArray(values.size * 2) { i -> (values[i / 2] shr (8 * (i % 2))).toByte() })
    }
}
tasks.named("preBuild") { dependsOn(gbkAsset) }

android {
    namespace = "com.treader.tomato"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.treader.tomato"
        minSdk = 26
        targetSdk = 35
        versionCode = baseCode + runNumber
        versionName = versionProps.getProperty("versionName")
    }

    signingConfigs {
        create("fixed") {
            storeFile = releaseKeystore
            storePassword = "android"
            keyAlias = "tomato"
            keyPassword = "android"
        }
    }

    buildTypes {
        release {
            // 本机和 CI 用同一把番茄 keystore，所以每次发布都能覆盖安装。/ Local and CI builds share the same Tomato
            // keystore, so every release installs over the previous one.
            signingConfig = signingConfigs.getByName("fixed")
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
    sourceSets["main"].assets.srcDir(gbkDir)
    buildFeatures {
        compose = true
        buildConfig = true
    }
}

dependencies {
    val composeBom = platform("androidx.compose:compose-bom:2024.12.01")
    implementation(composeBom)
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.foundation:foundation")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.activity:activity-compose:1.9.3")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.8.7")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.7")
    implementation("com.squareup.okhttp3:okhttp:4.12.0")
    implementation("io.coil-kt:coil-compose:2.7.0")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.9.0")
    testImplementation("junit:junit:4.13.2")
}
