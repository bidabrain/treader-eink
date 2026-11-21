// 验证器独立安卓客户端：照固件“验证器”标签，离线显示两步验证码，可以把账号发送到小纸 Pico。
// Standalone authenticator Android client: mirrors the firmware Authenticator tab, shows two-step codes offline and
// can send accounts to Read Pico.
pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}
rootProject.name = "auth-android"
include(":app")
