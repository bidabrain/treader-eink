package com.treader.webtoon.data

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import java.io.Closeable
import java.io.File
import java.util.zip.ZipFile

/**
 * 打开一个 CBZ：图片按文件名排序就是页序（服务器出的是 0001.jpg 起顺序编号的灰度页）。一次只解一页。
 * Open a CBZ: images sorted by name are the page order (the server writes grayscale pages numbered from 0001.jpg).
 * Pages are decoded one at a time.
 */
class Cbz(file: File) : Closeable {
    private val zip = ZipFile(file)
    private val pages = try {
        zip.entries().asSequence()
            .filter { !it.isDirectory && IMAGE.containsMatchIn(it.name) && !it.name.substringAfterLast('/').startsWith(".") }
            .sortedBy { it.name }
            .toList()
    } catch (e: Throwable) {
        zip.close()
        throw e
    }

    val count get() = pages.size

    /**
     * 解一页。页是灰度，用 RGB_565 省一半内存；特别大的图（比如别处来的长条）按 2 的幂缩到长边不超过 MAX_SIDE。
     * Decode one page. Pages are gray, so RGB_565 halves the memory; an oversized image (say a long strip from
     * elsewhere) is scaled by powers of two until its long side is at most MAX_SIDE.
     */
    @Synchronized
    fun page(i: Int): Bitmap? {
        val e = pages.getOrNull(i) ?: return null
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        zip.getInputStream(e).use { BitmapFactory.decodeStream(it, null, bounds) }
        var sample = 1
        while (maxOf(bounds.outWidth, bounds.outHeight) / sample > MAX_SIDE) sample *= 2
        val opts = BitmapFactory.Options().apply {
            inSampleSize = sample
            inPreferredConfig = Bitmap.Config.RGB_565
        }
        return zip.getInputStream(e).use { BitmapFactory.decodeStream(it, null, opts) }
    }

    override fun close() = zip.close()

    companion object {
        private val IMAGE = Regex("""\.(jpe?g|png|webp|gif|bmp)$""", RegexOption.IGNORE_CASE)
        private const val MAX_SIDE = 4096
    }
}
