package com.treader.webtoon.data

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

/** 扫 comics/ 得到订阅，和 README 的目录约定一致。/ Scanning comics/ yields subscriptions, per the README layout. */
class ShelfTest {
    @get:Rule
    val tmp = TemporaryFolder()

    @Test
    fun subscribeScanUnsubscribe() {
        val shelf = Shelf(File(tmp.root, "comics"))
        val s = shelf.subscribe(Mark("8420", "zh-hant", "我的虎系青梅竹马", "A", 0, 1))
        assertEquals("我的虎系青梅竹马", s.dir.name)
        File(s.dir, Names.episodeFile(2, "b")).writeText("x")
        File(s.dir, Names.episodeFile(1, "a")).writeText("x")
        File(s.dir, Names.episodeFile(3, "c") + ".part").writeText("x")

        val found = shelf.find("8420")!!
        assertEquals(listOf(1, 2), found.episodes.map { it.no })
        assertEquals(setOf(1, 2), shelf.have(found.dir))

        // 文件夹改名不影响订阅。/ Renaming the folder keeps the subscription.
        val renamed = File(found.dir.parentFile, "改名了")
        assertTrue(found.dir.renameTo(renamed))
        assertEquals(renamed, shelf.find("8420")!!.dir)

        shelf.unsubscribe(shelf.find("8420")!!, deleteFiles = false)
        assertNull(shelf.find("8420"))
        val local = shelf.scan().single()
        assertFalse(local.subscribed)
        assertEquals(2, local.episodes.size)
    }

    @Test
    fun sameFolderNameForAnotherTitleGetsId() {
        val shelf = Shelf(File(tmp.root, "comics"))
        val a = shelf.subscribe(Mark("1", "zh-hant", "同名", "", 0, 1))
        val b = shelf.subscribe(Mark("2", "en", "同名", "", 0, 1))
        assertEquals("同名", a.dir.name)
        assertEquals("同名 2", b.dir.name)
        assertEquals(2, shelf.scan().count { it.subscribed })
    }

    @Test
    fun unsubscribeAndDeleteRemovesFolder() {
        val shelf = Shelf(File(tmp.root, "comics"))
        val s = shelf.subscribe(Mark("1", "zh-hant", "T", "", 0, 1))
        File(s.dir, Names.episodeFile(1, "a")).writeText("x")
        shelf.unsubscribe(shelf.find("1")!!, deleteFiles = true)
        assertFalse(s.dir.exists())
        assertTrue(shelf.scan().isEmpty())
    }

    @Test
    fun writeMarkUpdatesLast() {
        val shelf = Shelf(File(tmp.root, "comics"))
        val s = shelf.subscribe(Mark("1", "zh-hant", "T", "", 0, 1))
        shelf.writeMark(s.dir, s.mark!!.copy(last = 9))
        assertEquals(9, Mark.parse(File(s.dir, Names.MARK).readText())!!.last)
        assertFalse(File(s.dir, Names.MARK + ".tmp").exists())
    }
}
