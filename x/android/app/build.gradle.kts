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

// 发布版签名用 X App 自己的 keystore（别名 x，和微博、小红书 App 的都不是同一把）：CI 用环境变量 X_KEYSTORE 指过来
// （从 Secret X_KEYSTORE_BASE64 还原），本机默认 ~/.android/x.keystore。路径写明，找不到就编译失败，不会悄悄换钥匙。
// Release signing uses the X app's own keystore (alias x, not the Weibo or Xiaohongshu app's key): CI points
// X_KEYSTORE at the one restored from the X_KEYSTORE_BASE64 secret; locally it defaults to ~/.android/x.keystore.
// The path is explicit, so a missing file fails the build instead of silently using another key.
val releaseKeystore = file(System.getenv("X_KEYSTORE") ?: "${System.getProperty("user.home")}/.android/x.keystore")

android {
    namespace = "com.treader.x"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.treader.x"
        minSdk = 26
        targetSdk = 35
        versionCode = baseCode + runNumber
        versionName = versionProps.getProperty("versionName")
    }

    signingConfigs {
        create("fixed") {
            storeFile = releaseKeystore
            storePassword = "android"
            keyAlias = "x"
            keyPassword = "android"
        }
    }

    buildTypes {
        release {
            // 本机和 CI 用同一把 X keystore，所以每次发布都能覆盖安装。/ Local and CI builds share the same X keystore,
            // so every release installs over the previous one.
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
    implementation("com.squareup.okhttp3:okhttp:4.12.0")
    implementation("io.coil-kt:coil-compose:2.7.0")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.9.0")
}
