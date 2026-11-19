// 番茄独立安卓客户端：和 Treader 固件的书库、番茄标签一样，读自建番茄下载服务器。
// Standalone Tomato Android client: the Treader firmware's library and Tomato tab, reading your own downloader server.
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
rootProject.name = "tomato-android"
include(":app")
