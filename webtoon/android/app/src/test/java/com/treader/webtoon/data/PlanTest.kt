package com.treader.webtoon.data

import org.junit.Assert.assertEquals
import org.junit.Test

/** 订阅起点、每次检查的下载计划和 last 的推进。/ Subscription start, per-check plans and moving last forward. */
class PlanTest {
    private fun eps(vararg nos: Int) = nos.map { Ep(it, "第${it}话") }

    @Test
    fun startChoices() {
        val all = eps(1, 2, 3, 4, 5, 6, 7, 8, 9, 10)
        assertEquals(0, Plan.startLast(all, Start.FIRST))
        assertEquals(10, Plan.startLast(all, Start.NEW_ONLY))
        assertEquals(5, Plan.startLast(all, Start.LAST5))
    }

    @Test
    fun startChoicesWithGapsAndFewEpisodes() {
        // 免费话不一定从 1 连续（前面的可能已经收费）。/ Free episodes need not start at 1 or be contiguous.
        // 最近 5 话是 42、43、44、45、50，last 停在 41；顺序乱的也一样。/ The latest five are 42..45 and 50, so last
        // stops at 41, also when unsorted.
        assertEquals(41, Plan.startLast(eps(40, 41, 42, 43, 44, 45, 50), Start.LAST5))
        assertEquals(41, Plan.startLast(eps(50, 40, 45, 41, 42, 43, 44), Start.LAST5))
        assertEquals(0, Plan.startLast(eps(1, 2, 3), Start.LAST5))
        assertEquals(0, Plan.startLast(eps(1, 2, 3, 4, 5), Start.LAST5))
        assertEquals(0, Plan.startLast(emptyList(), Start.NEW_ONLY))
    }

    private fun result(id: String, vararg nos: Int, error: String = "") = CheckResult(id, nos.maxOrNull() ?: 0, eps(*nos), error)

    @Test
    fun capRoundRobinOldestFirst() {
        val r = listOf(result("a", 3, 1, 2, 4), result("b", 10, 11), result("c", 7))
        val p = Plan.downloads(r, { emptySet() }, 5)
        assertEquals(listOf("a" to 1, "b" to 10, "c" to 7, "a" to 2, "b" to 11), p.map { it.id to it.no })
    }

    @Test
    fun noCapTakesEverything() {
        val r = listOf(result("a", 1, 2, 3), result("b", 5))
        assertEquals(4, Plan.downloads(r, { emptySet() }, 0).size)
    }

    @Test
    fun skipsErrorsAndEpisodesOnDisk() {
        val r = listOf(result("a", 1, 2, 3), result("b", 5, error = "坏了"))
        val p = Plan.downloads(r, { if (it == "a") setOf(1, 3) else emptySet() }, 10)
        assertEquals(listOf("a" to 2), p.map { it.id to it.no })
    }

    @Test
    fun advanceStopsAtFirstMissing() {
        assertEquals(12, Plan.advance(10, listOf(11, 12, 13, 14), setOf(11, 12, 14)))
        assertEquals(14, Plan.advance(10, listOf(14, 13, 12, 11), setOf(11, 12, 13, 14)))
        assertEquals(10, Plan.advance(10, listOf(11), emptySet()))
        // 有间隔的话号照样往前推。/ Gapped numbers still move forward.
        assertEquals(50, Plan.advance(45, listOf(50), setOf(50)))
    }

    @Test
    fun capThenAdvanceKeepsLastContiguous() {
        val r = listOf(result("a", 1, 2, 3, 4, 5, 6))
        val p = Plan.downloads(r, { emptySet() }, 3)
        val onDisk = p.map { it.no }.toSet()
        assertEquals(3, Plan.advance(0, r[0].new.map { it.no }, onDisk))
    }
}
