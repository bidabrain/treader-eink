// X 独立安卓客户端：照微博安卓 App 的样子，读自建 x 服务器（为你推荐、正在关注）。
// Standalone X Android client: modeled on the Weibo Android app, reading the self-hosted x server (For You, Following).
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
rootProject.name = "x-android"
include(":app")
