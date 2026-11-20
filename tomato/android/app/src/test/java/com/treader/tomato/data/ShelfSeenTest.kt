package com.treader.tomato.data

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/** 番茄书架的“已看”章节数：迁移、存盘合并、打开清除，和固件 tomato_shelf_* 一致。/ The Tomato shelf's seen count:
 *  migration, merge on save and clearing on open, matching the firmware's tomato_shelf_*. */
class ShelfSeenTest {
    @get:Rule val tmp = TemporaryFolder()

    private fun store() = ShelfStore(Dirs(tmp.root))
    private fun book(chapters: Int, remote: Int, seen: Int = chapters, file: String = "a.txt") =
        TomatoBook("1", "书", "作者", chapters, remote, file, seen)

    @Test
    fun counts() {
        val b = book(chapters = 105, remote = 110, seen = 100)
        assertEquals(5, b.pendingCount)
        assertEquals(5, b.freshCount)
        // 另一台设备看得更多：不出负数。/ Seen further on another device: never negative.
        assertEquals(0, book(chapters = 100, remote = 100, seen = 120).freshCount)
        assertEquals(0, book(chapters = 100, remote = 90).pendingCount)
    }

    @Test
    fun oldShelfWithoutSeenKeepsOnlyPending() {
        val s = store()
        Dirs(tmp.root).shelf.writeText("""{"books":[{"book_id":"1","title":"书","author":"作者","file":"a.txt","chapters":100,"remote":103}]}""")
        val b = s.load().single()
        assertEquals(100, b.seen)
        assertEquals(3, b.pendingCount)
        assertEquals(0, b.freshCount)
    }

    @Test
    fun downloadedChaptersStayMarkedUntilOpened() {
        val s = store()
        s.save(listOf(book(chapters = 100, remote = 103)))
        // 下载全部更新：章节数到 103，seen 不动。/ Download all updates: chapters reach 103, seen stays.
        s.save(listOf(s.load().single().copy(chapters = 103)))
        assertEquals(3, s.load().single().freshCount)
        assertTrue(s.markSeen("a.txt"))
        assertEquals(0, s.load().single().freshCount)
        assertFalse(s.markSeen("a.txt"))
        assertFalse(s.markSeen("other.txt"))
    }

    @Test
    fun staleCopyCannotUndoSeen() {
        val s = store()
        s.save(listOf(book(chapters = 103, remote = 103, seen = 100)))
        // 后台检查开始时读的副本。/ The copy a background check read when it started.
        val stale = s.load()
        s.markSeen("a.txt")
        s.save(stale.map { it.copy(remote = 108) })
        val b = s.load().single()
        assertEquals(103, b.seen)
        assertEquals(108, b.remote)
        assertEquals(0, b.freshCount)
    }
}
