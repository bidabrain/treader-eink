// WEBTOON 漫画独立安卓客户端：和小纸 Pico 固件的发现漫画一样，读自建 webtoon 服务器。
// Standalone WEBTOON comics Android client: the Read Pico firmware's comic store, reading your own webtoon server.
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
rootProject.name = "webtoon-android"
include(":app")
