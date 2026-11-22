package com.treader.webtoon.data

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext
import java.io.File

/** 检查更新的进度，给界面显示。/ Update-check progress for the UI. */
data class UpdateStatus(
    val running: Boolean = false,
    /** 当前在做什么。/ What is happening now. */
    val line: String = "",
    /** 已处理 / 计划下载的话数。/ Episodes handled / planned. */
    val done: Int = 0,
    val total: Int = 0,
    /** 结束后的结果。/ The result once finished. */
    val message: String = "",
)

/**
 * 检查更新，照固件发现漫画：把全部订阅（按语言分组）发给服务器问新话，按每次上限轮流挑话；每话隔 3 秒问一次状态，
 * 转好了下载 CBZ（先写 .part 再改名）。某一话失败就跳过它、最后报告；.webtoon 的 last 只推进到第一话没下好的前面。
 * Update check after the firmware's comic store: send every subscription (grouped by language) to the server for new
 * episodes, pick them round-robin up to the per-check cap; poll each episode every 3 s and download the CBZ once it
 * is ready (.part, then a rename). A failing episode is skipped and reported; .webtoon's last only moves up to just
 * before the first episode that is missing.
 */
class Updater(private val api: Api, private val shelf: Shelf) {

    suspend fun run(onlyId: String?, cap: Int, report: (UpdateStatus) -> Unit): String {
        val subs = withContext(Dispatchers.IO) { shelf.scan() }
            .filter { it.mark != null && (onlyId == null || it.mark.id == onlyId) }
        if (subs.isEmpty()) return "还没有订阅"
        report(UpdateStatus(true, "正在检查 ${subs.size} 部…"))

        val byId = subs.associateBy { it.mark!!.id }
        val results = ArrayList<CheckResult>()
        val errors = ArrayList<String>()
        for ((lang, group) in subs.groupBy { it.mark!!.lang }) {
            try {
                results += api.check(lang, group.map { it.mark!!.id to it.mark.last })
            } catch (e: Api.ApiError) {
                if (results.isEmpty() && subs.size == group.size) throw e
                errors += "${group.size} 部（$lang）：${e.message}"
            }
        }
        results.filter { it.error.isNotEmpty() }.forEach { r -> errors += "${byId[r.id]?.name ?: r.id}：${r.error}" }

        val plan = withContext(Dispatchers.IO) {
            Plan.downloads(results, { id -> byId[id]?.let { shelf.have(it.dir) }.orEmpty() }, cap)
        }
        val waiting = results.filter { it.error.isEmpty() }.sumOf { r ->
            val have = byId[r.id]?.let { shelf.have(it.dir) }.orEmpty()
            r.new.count { it.no !in have }
        }
        var ok = 0
        val failed = ArrayList<String>()
        var offline = ""
        for ((i, p) in plan.withIndex()) {
            val s = byId[p.id] ?: continue
            val label = "${s.name} 第 ${p.no} 话"
            try {
                fetchRetry(s, p) { line -> report(UpdateStatus(true, "$label：$line", i, plan.size)) }
                ok++
            } catch (e: Api.NetworkError) {
                // 连不上服务器：后面的话也一样下不了，停下，下次检查更新从这里接着下。
                // The server is unreachable, so later episodes would fail too: stop; the next check resumes here.
                offline = "$label：${e.message}"
                break
            } catch (e: Api.ApiError) {
                failed += "$label：${e.message}"
            }
            report(UpdateStatus(true, label, i + 1, plan.size))
        }

        // 推进每部的 last。/ Move each title's last forward.
        withContext(Dispatchers.IO) {
            for (r in results) {
                if (r.error.isNotEmpty()) continue
                val s = byId[r.id] ?: continue
                val m = s.mark!!
                val last = Plan.advance(m.last, r.new.map { it.no }, shelf.have(s.dir))
                if (last != m.last) shelf.writeMark(s.dir, m.copy(last = last))
            }
        }

        val left = waiting - ok - failed.size
        return buildString {
            append(if (plan.isEmpty() && errors.isEmpty()) "没有新话" else "下载了 $ok 话")
            if (offline.isNotEmpty()) append("；网络断了，先停下。还有 $left 话，网络好了再点一次检查更新\n$offline")
            else if (left > 0) append("；还有 $left 话，再点一次检查更新")
            if (failed.isNotEmpty()) append("\n${failed.size} 话失败：\n" + failed.joinToString("\n"))
            if (errors.isNotEmpty()) append("\n检查出错：\n" + errors.joinToString("\n"))
        }
    }

    // 连不上时等一会儿再试一次（网络偶尔抖一下不算断）；还不行就交给调用方停下。
    // On a network error wait a little and try once more (a brief blip is not an outage); if it still fails, let the
    // caller stop.
    private suspend fun fetchRetry(s: Series, p: Planned, line: (String) -> Unit) {
        try {
            fetch(s, p, line)
        } catch (e: Api.NetworkError) {
            line("连不上服务器，${RETRY_WAIT_MS / 1000} 秒后再试")
            delay(RETRY_WAIT_MS)
            fetch(s, p, line)
        }
    }

    // 等服务器转好一话（每 3 秒问一次，最多约 15 分钟），再下载。/ Wait for the server to convert one episode (asking
    // every 3 s, about 15 minutes at most), then download it.
    private suspend fun fetch(s: Series, p: Planned, line: (String) -> Unit) {
        val lang = s.mark!!.lang
        var tries = 0
        while (true) {
            val st = api.episode(p.id, p.no, lang)
            when (st.optString("state")) {
                "ready" -> break
                "error" -> throw Api.ApiError(st.optString("error").ifEmpty { "服务器转换失败" })
                "pending" -> {
                    val total = st.optInt("total", 0)
                    line(if (total > 0) "服务器转换中 ${st.optInt("done", 0)}/$total" else "排队等服务器转换")
                }
                else -> line("等服务器开始转换")
            }
            if (++tries > 300) throw Api.ApiError("服务器转换太久")
            delay(3000)
        }
        line("下载中")
        val dest = File(s.dir, Names.episodeFile(p.no, p.title))
        api.download(p.id, p.no, lang, dest)
    }

    companion object {
        const val RETRY_WAIT_MS = 10_000L
    }
}
