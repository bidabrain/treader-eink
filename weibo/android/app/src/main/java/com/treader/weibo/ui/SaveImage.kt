package com.treader.weibo.ui

import android.Manifest
import android.content.ContentValues
import android.content.Context
import android.content.pm.PackageManager
import android.media.MediaScannerConnection
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import coil.ImageLoader
import coil.annotation.ExperimentalCoilApi
import coil.request.ErrorResult
import coil.request.ImageRequest
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.io.IOException

/**
 * 看图页的“保存图片”：把屏幕上这张图（服务器处理好的灰度 JPEG，原样字节）存进手机相册 Pictures/<album>/。
 * 优先用 Coil 磁盘缓存里已经下好的那份，不重复联网；还没缓存就先下一次。安卓 10 以上走 MediaStore 不要权限，
 * 安卓 8、9 要一次存储权限。
 * "Save image" on the image screen: stores the image on screen (the server's grayscale JPEG, bytes as is) in the
 * phone's gallery under Pictures/<album>/. The copy already in Coil's disk cache is used first, so no extra download;
 * otherwise it is fetched once. Android 10+ uses MediaStore without permissions; Android 8 and 9 ask once for storage.
 */
@Composable
fun SaveBar(loader: ImageLoader, url: String, name: String, album: String, modifier: Modifier = Modifier) {
    val ctx = LocalContext.current
    val scope = rememberCoroutineScope()
    var note by remember(url) { mutableStateOf("") }
    var busy by remember(url) { mutableStateOf(false) }
    val save: () -> Unit = {
        busy = true
        note = "正在保存…"
        scope.launch {
            note = try {
                saveToGallery(ctx, loader, url, name, album)
                "已保存到相册 Pictures/$album"
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                "保存失败：${e.message ?: "未知错误"}"
            }
            busy = false
        }
    }
    val ask = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { ok ->
        if (ok) save() else note = "没有存储权限，存不了"
    }
    // 整条吃掉点按，免得点到下面的翻页区。/ The bar swallows taps so they do not reach the turn zones below.
    Column(modifier.fillMaxWidth().background(Paper).noRippleClick {}) {
        HLine(soft = false)
        Row(Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 10.dp), verticalAlignment = Alignment.CenterVertically) {
            T(note.ifEmpty { "存进手机相册（灰度图）" }, 13.sp, color = Mid, maxLines = 2, modifier = Modifier.weight(1f))
            Spacer(Modifier.width(12.dp))
            Pill("保存图片", filled = true, height = 40.dp, modifier = Modifier.width(112.dp)) {
                if (busy) return@Pill
                val legacy = Build.VERSION.SDK_INT < Build.VERSION_CODES.Q
                if (legacy && ContextCompat.checkSelfPermission(ctx, Manifest.permission.WRITE_EXTERNAL_STORAGE) !=
                    PackageManager.PERMISSION_GRANTED
                ) ask.launch(Manifest.permission.WRITE_EXTERNAL_STORAGE) else save()
            }
        }
    }
}

@OptIn(ExperimentalCoilApi::class)
private fun cached(loader: ImageLoader, url: String): ByteArray? =
    loader.diskCache?.openSnapshot(url)?.use { it.data.toFile().readBytes() }

private suspend fun saveToGallery(ctx: Context, loader: ImageLoader, url: String, name: String, album: String) {
    val bytes = withContext(Dispatchers.IO) { cached(loader, url) } ?: run {
        val r = loader.execute(ImageRequest.Builder(ctx).data(url).build())
        if (r is ErrorResult) throw IOException("图片没加载出来，检查网络后再试")
        withContext(Dispatchers.IO) { cached(loader, url) } ?: throw IOException("图片没加载出来，检查网络后再试")
    }
    withContext(Dispatchers.IO) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            val resolver = ctx.contentResolver
            val values = ContentValues().apply {
                put(MediaStore.Images.Media.DISPLAY_NAME, name)
                put(MediaStore.Images.Media.MIME_TYPE, "image/jpeg")
                put(MediaStore.Images.Media.RELATIVE_PATH, "${Environment.DIRECTORY_PICTURES}/$album")
                put(MediaStore.Images.Media.IS_PENDING, 1)
            }
            val uri = resolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values)
                ?: throw IOException("相册不让写入")
            try {
                (resolver.openOutputStream(uri) ?: throw IOException("相册不让写入")).use { it.write(bytes) }
                values.clear()
                values.put(MediaStore.Images.Media.IS_PENDING, 0)
                resolver.update(uri, values, null, null)
            } catch (e: Exception) {
                resolver.delete(uri, null, null)
                throw e
            }
        } else {
            @Suppress("DEPRECATION")
            val dir = File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_PICTURES), album)
            if (!dir.isDirectory && !dir.mkdirs()) throw IOException("建不了相册目录")
            // 同名就加序号，不覆盖以前存的。/ Number a clashing name instead of overwriting an earlier save.
            var file = File(dir, name)
            var k = 1
            while (file.exists()) file = File(dir, name.removeSuffix(".jpg") + "_${k++}.jpg")
            file.writeBytes(bytes)
            MediaScannerConnection.scanFile(ctx, arrayOf(file.path), arrayOf("image/jpeg"), null)
        }
    }
}
