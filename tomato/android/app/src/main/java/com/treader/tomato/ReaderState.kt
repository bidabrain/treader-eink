package com.treader.tomato

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import com.treader.tomato.book.Block
import com.treader.tomato.book.BookSource
import com.treader.tomato.data.LibBook
import com.treader.tomato.data.Progress
import com.treader.tomato.data.ProgressStore

/**
 * 正在读的一本书：当前章、章内偏移（页的起点，UTF-8 字节）和字号。页码由排版按偏移算出来，所以换字号、换排版后还停在原处。
 * The open book: current chapter, offset in the chapter (a page start in UTF-8 bytes) and font size. The page number
 * comes from the layout and the offset, so the position holds across font or layout changes.
 */
class ReaderState(val book: LibBook, val source: BookSource, private val store: ProgressStore, start: Progress?) {
    var chapter by mutableStateOf((start?.chapter ?: 0).coerceIn(0, maxOf(0, source.chapterCount - 1)))
    var off by mutableStateOf(if (start != null && start.chapter == chapter) start.off else 0)
    /** 进新章时停在最后一页（往回翻进上一章）。/ Land on the last page when entering a chapter backwards. */
    var toEnd by mutableStateOf(false)
    var px by mutableStateOf(start?.px?.takeIf { it in PX_MIN..PX_MAX && (it - PX_MIN) % 4 == 0 } ?: PX_DEFAULT)
    var pageCount by mutableStateOf(1)
    var page by mutableStateOf(0)

    private var cachedChapter = -1
    private var cachedBytes = ByteArray(0)
    private var cachedBlocks = emptyList<Block>()

    fun text(): ByteArray {
        if (cachedChapter != chapter) {
            cachedBytes = source.chapterUtf8(chapter)
            cachedBlocks = source.chapterBlocks(chapter)
            cachedChapter = chapter
        }
        return cachedBytes
    }

    fun blocks(): List<Block> { text(); return cachedBlocks }

    /** 全书百分比：本章在原文件里的位置按章内偏移插值。/ Whole-book percent: interpolate the chapter's source span
     *  by the offset within the chapter. */
    fun pct(): Int {
        val total = source.total.toLong().coerceAtLeast(1)
        val s = source.chapterSourceOffset(chapter).toLong()
        val e = if (chapter + 1 < source.chapterCount) source.chapterSourceOffset(chapter + 1).toLong() else total
        val len = text().size.coerceAtLeast(1)
        val atEnd = chapter + 1 >= source.chapterCount && page + 1 >= pageCount
        if (atEnd) return 100
        return ((s + (e - s) * off / len) * 100 / total).toInt().coerceIn(0, 99)
    }

    fun save() {
        store.save(book.key, Progress(book.file.length(), chapter, off, px, pct()))
    }

    companion object {
        const val PX_MIN = 36
        const val PX_MAX = 72
        const val PX_DEFAULT = 48
    }
}
