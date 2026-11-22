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
        for (t in 0 until 3) assertEquals(Progress.P(199, 200), again.get("s$t/ep.cbz"))
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
        assertEquals(Progress.P(5, 20), Progress(f).get("a/1.cbz"))
    }
}
