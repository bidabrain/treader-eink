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

// 发布版签名用验证器 App 自己的 keystore（别名 auth，和别的 App 都不是同一把）：CI 用环境变量 AUTH_KEYSTORE 指过来
// （从 Secret AUTH_KEYSTORE_BASE64 还原），本机默认 ~/.android/auth.keystore。找不到就编译失败，不会悄悄换钥匙。
// Release signing uses the authenticator app's own keystore (alias auth, not another app's key): CI points
// AUTH_KEYSTORE at the one restored from the AUTH_KEYSTORE_BASE64 secret; locally it defaults to
// ~/.android/auth.keystore. A missing file fails the build instead of silently using another key.
val releaseKeystore = file(System.getenv("AUTH_KEYSTORE") ?: "${System.getProperty("user.home")}/.android/auth.keystore")

android {
    namespace = "com.treader.auth"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.treader.auth"
        minSdk = 26
        targetSdk = 35
        versionCode = baseCode + runNumber
        versionName = versionProps.getProperty("versionName")
    }

    signingConfigs {
        create("fixed") {
            storeFile = releaseKeystore
            storePassword = "android"
            keyAlias = "auth"
            keyPassword = "android"
        }
    }

    buildTypes {
        release {
            signingConfig = signingConfigs.getByName("fixed")
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
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
    implementation("androidx.lifecycle:lifecycle-process:2.8.7")
    implementation("com.squareup.okhttp3:okhttp:4.12.0")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.9.0")
    // 本地扫码（Apache-2.0），不联网。/ On-device QR scanning (Apache-2.0), no network.
    implementation("com.journeyapps:zxing-android-embedded:4.3.0")
    testImplementation("junit:junit:4.13.2")
    testImplementation("org.json:json:20240303")
}
