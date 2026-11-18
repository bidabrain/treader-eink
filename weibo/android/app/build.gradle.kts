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

// 发布版签名用的 keystore：CI 用环境变量 WEIBO_KEYSTORE 指过来（从 Secret 还原），本机默认 ~/.android/debug.keystore。
// 明确写出路径，不靠构建工具找“默认 debug keystore”：那个位置在 CI 上不一定是 ~/.android，找不到就会每次新生成一把，
// 发出去的 APK 签名各不相同、互相装不上。
// Keystore for release signing: CI points WEIBO_KEYSTORE at the one restored from a secret; locally it defaults to
// ~/.android/debug.keystore. The path is explicit instead of relying on the build tool's "default debug keystore",
// whose location on CI is not necessarily ~/.android; when missing it silently generates a new key each run and the
// released APKs cannot install over each other.
val releaseKeystore = file(System.getenv("WEIBO_KEYSTORE") ?: "${System.getProperty("user.home")}/.android/debug.keystore")

android {
    namespace = "com.treader.weibo"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.treader.weibo"
        minSdk = 26
        targetSdk = 35
        versionCode = baseCode + runNumber
        versionName = versionProps.getProperty("versionName")
    }

    signingConfigs {
        create("fixed") {
            storeFile = releaseKeystore
            storePassword = "android"
            keyAlias = "androiddebugkey"
            keyPassword = "android"
        }
    }

    buildTypes {
        release {
            // 发布版也用 debug 签名：本机是 ~/.android/debug.keystore，CI 从 Secret 还原同一个文件，所以每次都能覆盖安装。
            // Release builds use the debug key: ~/.android/debug.keystore locally, restored from a Secret on CI, so
            // every release installs over the previous one.
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
