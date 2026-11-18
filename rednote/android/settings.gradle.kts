// 小红书独立安卓客户端：和 Treader 固件的小红书标签功能一样，读自建 rednote 服务器。
// Standalone Xiaohongshu Android client: same as the Treader firmware's Xiaohongshu tab, reading the rednote server.
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
rootProject.name = "rednote-android"
include(":app")
