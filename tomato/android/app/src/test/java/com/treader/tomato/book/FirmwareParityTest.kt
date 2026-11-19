package com.treader.tomato.book

import org.junit.Assert.assertEquals
import org.junit.BeforeClass
import org.junit.Test
import java.io.File

/**
 * 书源和固件逐字节一致：同一批测试书，App 的章节表（原文偏移、标题、每章 UTF-8 的长度和哈希）必须和固件 main/book
 * 打印的 firmware_chapters.tsv 完全一样。标准答案由 tools/gen_fixtures.sh 在 gcc 容器里用固件代码生成。
 * The book source matches the firmware byte for byte: for the same test books the app's chapter table (source
 * offsets, titles, each chapter's UTF-8 length and hash) must equal firmware_chapters.tsv printed by the firmware's
 * main/book code. tools/gen_fixtures.sh regenerates the reference in a gcc container.
 */
class FirmwareParityTest {
    companion object {
        @JvmStatic
        @BeforeClass
        fun table() {
            val b = File("build/generated/gbk/gbk.bin").readBytes()
            TxtBook.gbkTable = CharArray(b.size / 2) { i -> ((b[2 * i].toInt() and 0xFF) or ((b[2 * i + 1].toInt() and 0xFF) shl 8)).toChar() }
        }

        fun fnv(b: ByteArray): String {
            var h = -0x340d631b7bdddcdbL
            for (x in b) { h = h xor (x.toLong() and 0xFF); h *= 0x100000001b3L }
            return "%016x".format(h)
        }

        fun dump(f: File): List<String> {
            val src: BookSource = if (f.name.endsWith(".epub")) EpubBook.open(f) else TxtBook.open(f)
            val out = mutableListOf("FILE\t${f.name}\t${src.total}\t${src.chapterCount}")
            for (i in 0 until src.chapterCount) {
                val t = src.chapterUtf8(i)
                out += "$i\t${src.chapterSourceOffset(i)}\t${src.chapterTitle(i)}\t${t.size}\t${fnv(t)}"
                // EPUB 的块表：起点、长度、是不是标题。/ EPUB block table: offset, length, heading flag.
                if (src is EpubBook) out += "B\t$i" + src.chapterBlocks(i).joinToString("") { "\t${it.offset}:${it.len}:${if (it.heading) 1 else 0}" }
            }
            return out
        }
    }

    @Test
    fun chaptersMatchFirmware() {
        val res = File("src/test/resources")
        val expected = File(res, "firmware_chapters.tsv").readLines().filter { it.isNotEmpty() }
        val books = File(res, "books").listFiles()!!.sortedBy { it.name }
        val actual = books.flatMap { dump(it) }
        // 按书比，出错时看得出是哪本。/ Compare book by book so a failure names the book.
        val exp = expected.joinToString("\n").split("FILE\t").filter { it.isNotEmpty() }.associateBy { it.substringBefore('\t') }
        val act = actual.joinToString("\n").split("FILE\t").filter { it.isNotEmpty() }.associateBy { it.substringBefore('\t') }
        assertEquals(exp.keys, act.keys)
        for (k in exp.keys) assertEquals("book $k", exp[k]!!.trim(), act[k]!!.trim())
    }
}
