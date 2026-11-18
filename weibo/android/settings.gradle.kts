// 微博独立安卓客户端：和 Treader 固件的微博标签功能一样，读自建 weibo 服务器。
// Standalone Weibo Android client: same as the Treader firmware's Weibo tab, reading the weibo server.
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
rootProject.name = "weibo-android"
include(":app")
