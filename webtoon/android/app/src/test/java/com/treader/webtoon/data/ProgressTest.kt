package com.treader.webtoon.data

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import java.io.File
import kotlin.concurrent.thread

/** 进度：后台写、多线程一起改不写乱、删除后不留。/ Progress: background writes, concurrent changes, forgetting. */
class ProgressTest {
    private fun tmpFile() = File.createTempFile("progress", ".json").apply { delete(); deleteOnExit() }

    @Test
    fun concurrentChangesEndUpInFile() {
        val f = tmpFile()
        val p = Progress(f)
        // 一个线程翻页、几个线程像后台任务一样同时改别的话和写文件。/ One thread turns pages while others change other
        // episodes and write the file, like background jobs.
        val workers = (0 until 4).map { t ->
            thread {
                repeat(200) { i ->
                    p.set("s$t/ep.cbz", i, 200)
                    if (i % 7 == 0) p.writeNow()
                }
            }
        }
        workers.forEach { it.join() }
        p.forget("s3/")
        p.writeNow()
        val again = Progress(f)
        for (t in 0 until 3) assertEquals(199 to 200, again.get("s$t/ep.cbz")?.let { it.page to it.pages })
        assertNull(again.get("s3/ep.cbz"))
    }

    @Test
    fun flushWritesInBackground() {
        val f = tmpFile()
        val p = Progress(f)
        p.set("a/1.cbz", 5, 20)
        p.flush()
        val deadline = System.currentTimeMillis() + 3000
        while (System.currentTimeMillis() < deadline && Progress(f).get("a/1.cbz") == null) Thread.sleep(20)
        assertEquals(5 to 20, Progress(f).get("a/1.cbz")?.let { it.page to it.pages })
    }

    @Test
    fun resumeLatestRead() {
        val p = Progress(tmpFile())
        val keys = (1..5).map { "s/$it.cbz" }
        assertEquals(0, p.resume(keys))                         // 都没看过 / Nothing read
        p.set("s/4.cbz", 3, 20, at = 100)                       // 先看了第 4 话 / Read episode 4 first
        p.set("s/1.cbz", 0, 20, at = 200)                       // 又点开第 1 话看了一眼 / Then peeked at episode 1
        assertEquals(0, p.resume(keys))
        p.set("s/4.cbz", 5, 20, at = 300)                       // 回去接着看第 4 话 / Back to episode 4
        assertEquals(3, p.resume(keys))
        p.set("s/4.cbz", 19, 20, at = 400)                      // 看完第 4 话，接第 5 话 / Finished 4: continue with 5
        assertEquals(4, p.resume(keys))
        p.set("s/5.cbz", 19, 20, at = 500)                      // 最后一话也看完，停在它 / Last one finished: stay on it
        assertEquals(4, p.resume(keys))
    }

    @Test
    fun resumeOldEntriesWithoutTime() {
        val f = tmpFile()
        f.writeText("""{"s/1.cbz":{"page":19,"pages":20},"s/3.cbz":{"page":2,"pages":20}}""")
        val p = Progress(f)
        // 旧文件没有时间：越往后的算越近，第 3 话没看完就接着它。/ No times in an old file: later counts as more recent.
        assertEquals(2, p.resume((1..4).map { "s/$it.cbz" }))
    }
}
