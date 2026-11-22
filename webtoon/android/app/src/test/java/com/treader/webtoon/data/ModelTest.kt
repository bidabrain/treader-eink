package com.treader.webtoon.data

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** .webtoon、文件命名和服务器回复解析。/ .webtoon, file naming and server reply parsing. */
class ModelTest {
    @Test
    fun markRoundTrip() {
        val m = Mark("8420", "zh-hant", "我的虎系青梅竹马", "GIMAN、ETER", 66, 1795275780)
        assertEquals(m, Mark.parse(m.toJson()))
    }

    @Test
    fun markReadsReadmeExample() {
        val m = Mark.parse("""{"id": "8420", "lang": "zh-hant", "title": "我的虎系青梅竹马", "author": "GIMAN、ETER", "last": 66, "added": 1795275780}""")!!
        assertEquals("8420", m.id)
        assertEquals(66, m.last)
        assertEquals(1795275780L, m.added)
    }

    @Test
    fun markDefaultsAndBadInput() {
        assertNull(Mark.parse("not json"))
        assertNull(Mark.parse("""{"title": "no id"}"""))
        val m = Mark.parse("""{"id": "c1060813", "last": -3}""")!!
        assertEquals("zh-hant", m.lang)
        assertEquals(0, m.last)
    }

    @Test
    fun episodeFileIsPadded() {
        assertEquals("第0001话 序章.cbz", Names.episodeFile(1, "序章"))
        assertEquals("第0066话 第66话.cbz", Names.episodeFile(66, "第66话"))
        assertEquals("第12345话 x.cbz", Names.episodeFile(12345, "x"))
        assertEquals("第0007话.cbz", Names.episodeFile(7, "  "))
    }

    @Test
    fun episodeFileSortsByNumber() {
        val names = listOf(Names.episodeFile(10, "b"), Names.episodeFile(9, "z"), Names.episodeFile(100, "a")).sorted()
        assertEquals(listOf(9, 10, 100), names.map { Names.episodeNo(it) })
    }

    @Test
    fun sanitizeStripsPathAndControlCharacters() {
        assertEquals("a b c d", Names.sanitize("a/b\\c:d"))
        assertEquals("what", Names.sanitize("  what?  "))
        assertEquals("x y", Names.sanitize("x\n\ty"))
        assertEquals("dots", Names.sanitize("..dots.."))
        assertEquals("第0003话 Ep. 3 Who are you.cbz", Names.episodeFile(3, "Ep. 3: Who are you?"))
        assertEquals(60, Names.sanitize("字".repeat(100)).length)
        // 代理对不会被截成半个。/ Surrogate pairs are not cut in half.
        val emoji = "😀".repeat(80)
        assertEquals(60, Names.sanitize(emoji).codePointCount(0, Names.sanitize(emoji).length))
    }

    @Test
    fun folderFallsBackToId() {
        assertEquals("webtoon 8420", Names.folder("///", "8420"))
        assertEquals("我的虎系青梅竹马", Names.folder("我的虎系青梅竹马", "8420"))
    }

    @Test
    fun episodeNoFromFileName() {
        assertEquals(66, Names.episodeNo("第0066话 标题.cbz"))
        assertNull(Names.episodeNo("chapter 1.cbz"))
    }

    @Test
    fun parseCheckReply() {
        val o = JSONObject(
            """{"titles":[
                {"id":"8420","latest":68,"new":[{"no":68,"title":"第68话"},{"no":67,"title":"第67话"}]},
                {"id":"c1","error":"找不到作品","new":[]},
                {"latest":3}
            ]}"""
        )
        val r = Parse.check(o)
        assertEquals(2, r.size)
        assertEquals("8420", r[0].id)
        assertEquals(68, r[0].latest)
        assertEquals(listOf(67, 68), r[0].new.map { it.no })
        assertEquals("", r[0].error)
        assertEquals("找不到作品", r[1].error)
        assertTrue(r[1].new.isEmpty())
    }

    @Test
    fun checkBodyShape() {
        val o = JSONObject(Parse.checkBody("zh-hant", "s", listOf("8420" to 66, "c1" to 0)))
        assertEquals("zh-hant", o.getString("lang"))
        assertEquals("s", o.getString("zh"))
        val t = o.getJSONArray("titles")
        assertEquals(2, t.length())
        assertEquals("8420", t.getJSONObject(0).getString("id"))
        assertEquals(66, t.getJSONObject(0).getInt("last"))
    }

    @Test
    fun parseStoreAndTitle() {
        val s = Parse.store(JSONObject(
            """{"seg":"rank","key":"trending","keys":[{"key":"trending","name":"趋势"},{"key":"popular","name":"人气"}],
               "items":[{"id":"8420","title":"T","author":"A","genre":"G","likes":12345678901,"canvas":false,"adult":false}]}"""
        ))
        assertEquals("trending", s.key)
        assertEquals(listOf("趋势", "人气"), s.keys.map { it.name })
        assertEquals(12345678901L, s.items[0].likes)
        val t = Parse.title(JSONObject(
            """{"id":"8420","title":"T","author":"A","genre":"G","summary":"S","done":true,"latest":3,
               "episodes":[{"no":2,"title":"b","date":2},{"no":1,"title":"a","date":1},{"no":0,"title":"bad"}]}"""
        ))
        assertTrue(t.done)
        assertEquals(listOf(1, 2), t.episodes.map { it.no })
    }
}
