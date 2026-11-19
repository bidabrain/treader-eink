package com.treader.tomato.data

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import java.io.File

/** 书库里的一本书。/ One book in the library. */
data class LibBook(
    val file: File,
    val epub: Boolean,
    /** 番茄书架上的那条（番茄下载的书）。/ The Tomato shelf entry for Tomato downloads. */
    val tomato: TomatoBook?,
    val progress: Progress?,
) {
    val name get() = file.name
    val key get() = ProgressStore.key(file.name)
    val title get() = tomato?.title ?: file.nameWithoutExtension
    /** 已读百分比，没读过 -1。/ Percent read, -1 when unread. */
    val pct get() = progress?.pct ?: -1
}

/**
 * 书库：App 目录 books/ 里的 TXT 和 EPUB（固件的 /sdcard/books）。手机上用系统文件选择器导入，导入就是复制进来；
 * 删除书会同时清掉进度、番茄书架记录（记进“已删除”）和封面。
 * The library: TXT and EPUB files in the app's books/ (the firmware's /sdcard/books). Importing copies a file chosen
 * with the system picker; deleting a book also clears its progress, its Tomato shelf entry (recorded as removed) and
 * its cover.
 */
class Library(private val dirs: Dirs, private val shelf: ShelfStore, val progress: ProgressStore) {
    fun scan(): List<LibBook> {
        val tomato = shelf.load().filter { it.file.isNotEmpty() }.associateBy { it.file }
        val files = dirs.books.listFiles()?.filter { it.isFile && (it.name.endsWith(".txt", true) || it.name.endsWith(".epub", true)) }
            ?: emptyList()
        return files.map { f ->
            LibBook(f, f.name.endsWith(".epub", true), tomato[f.name], progress.load(ProgressStore.key(f.name), f.length()))
        }
    }

    fun delete(b: LibBook) {
        b.file.delete()
        progress.forget(b.key)
        shelf.removeFile(b.name)
        File(dirs.covers, "${b.name}.img").delete()
    }

    fun clearProgress(b: LibBook) = progress.forget(b.key)

    /** 从系统文件选择器导入一本；返回结果说明。/ Import one book from the system picker; returns a message. */
    fun import(ctx: Context, uri: Uri): String {
        var name = ctx.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use {
            if (it.moveToFirst()) it.getString(0) else null
        } ?: return "读不到文件名"
        val lower = name.lowercase()
        if (!lower.endsWith(".txt") && !lower.endsWith(".epub")) return "只支持 TXT 和 EPUB"
        name = name.map { if (it in "/\\:*?\"<>|" || it.code < 32) '_' else it }.joinToString("")
        var dest = File(dirs.books, name)
        var n = 2
        while (dest.exists()) {
            dest = File(dirs.books, name.substringBeforeLast('.') + " ($n)." + name.substringAfterLast('.'))
            n++
        }
        ctx.contentResolver.openInputStream(uri)?.use { input ->
            val tmp = File(dest.path + ".part")
            tmp.outputStream().use { input.copyTo(it) }
            tmp.renameTo(dest)
        } ?: return "打不开这个文件"
        return "已导入 ${dest.name}"
    }
}
