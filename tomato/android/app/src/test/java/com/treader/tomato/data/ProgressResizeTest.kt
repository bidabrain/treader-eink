package com.treader.tomato.data

import org.junit.Assert.assertEquals
import org.junit.Test

/** 番茄更新后进度百分比的重算，和固件 book_progress_resize 一致。/ Percentage rescaling after a Tomato update, matching
 *  the firmware's book_progress_resize. */
class ProgressResizeTest {
    @Test
    fun finishedBookIsNoLongerFinishedAfterUpdate() {
        assertEquals(95, ProgressStore.rescaledPct(100, 1_000_000, 1_050_000))
    }

    @Test
    fun tinyUpdateStillLeavesUnread() {
        assertEquals(99, ProgressStore.rescaledPct(100, 1_000_000, 1_000_010))
    }

    @Test
    fun partReadScalesDown() {
        assertEquals(50, ProgressStore.rescaledPct(60, 1_000_000, 1_200_000))
    }

    @Test
    fun sameOrSmallerSizeKeepsPercent() {
        assertEquals(100, ProgressStore.rescaledPct(100, 1_000_000, 1_000_000))
        assertEquals(40, ProgressStore.rescaledPct(40, 1_000_000, 900_000))
    }
}
