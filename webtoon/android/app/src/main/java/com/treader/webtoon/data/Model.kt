package com.treader.webtoon.data

import org.json.JSONArray
import org.json.JSONObject

/*
 * 纯逻辑，不碰 Android：.webtoon 订阅标记、文件命名、服务器回复的解析、订阅起点和每次检查的下载计划。单元测试都在这里。
 * Pure logic with no Android: the .webtoon subscription mark, file naming, parsing server replies, the subscription
 * start and each check's download plan. The unit tests cover this file.
 */

/** 作品文件夹里的 .webtoon 订阅标记，字段照 webtoon/README.md。/ The .webtoon mark in a series folder, fields per
 *  webtoon/README.md. */
data class Mark(
    val id: String,
    val lang: String,
    val title: String,
    val author: String,
    /** 已下载的最大话号。/ Highest downloaded episode number. */
    val last: Int,
    /** 订阅时间（Unix 秒）。/ Subscription time in Unix seconds. */
    val added: Long,
) {
    fun toJson(): String = JSONObject()
        .put("id", id).put("lang", lang).put("title", title).put("author", author)
        .put("last", last).put("added", added)
        .toString()

    companion object {
        /** 读标记；看不懂或没有 id 返回 null（当普通本地漫画）。/ Parse a mark; null when unreadable or without an id
         *  (the folder is then plain local comics). */
        fun parse(text: String): Mark? = runCatching {
            val o = JSONObject(text)
            val id = o.optString("id").trim()
            if (id.isEmpty()) return null
            Mark(id, o.optString("lang").ifEmpty { "zh-hant" }, o.optString("title"), o.optString("author"),
                o.optInt("last", 0).coerceAtLeast(0), o.optLong("added", 0))
        }.getOrNull()
    }
}

/** 文件和文件夹名。/ File and folder names. */
object Names {
    const val MARK = ".webtoon"
    private const val MAX_TITLE = 60
    private val EPISODE_RE = Regex("""^第(\d+)话""")

    /** 去掉文件名里不能用的字符：路径符号、控制字符、首尾空格和点，连续空白合成一个，太长截断。/ Strip characters a
     *  file name cannot hold: path symbols and control characters, leading/trailing spaces and dots; collapse runs of
     *  whitespace and cut overly long names. */
    fun sanitize(s: String, max: Int = MAX_TITLE): String {
        val b = StringBuilder()
        for (ch in s) {
            b.append(
                when {
                    ch < ' ' || ch == '\u007f' -> ' '
                    ch in "/\\:*?\"<>|" -> ' '
                    else -> ch
                }
            )
        }
        var t = b.toString().replace(Regex("\\s+"), " ").trim().trim('.', ' ')
        if (t.codePointCount(0, t.length) > max) t = t.substring(0, t.offsetByCodePoints(0, max)).trim().trimEnd('.')
        return t
    }

    /** 作品文件夹名；书名不能用时退回作品编号。/ The series folder name; falls back to the title id. */
    fun folder(title: String, id: String): String = sanitize(title).ifEmpty { "webtoon $id" }

    /** “第NNNN话 标题.cbz”，话号补零到 4 位；标题不能用时只留话号。/ "第NNNN话 title.cbz" with the number padded
     *  to four digits; just the number when the title is unusable. */
    fun episodeFile(no: Int, title: String): String {
        val t = sanitize(title)
        val n = "第" + no.toString().padStart(4, '0') + "话"
        return if (t.isEmpty()) "$n.cbz" else "$n $t.cbz"
    }

    /** 从文件名读话号；不是这种名字返回 null。/ The episode number from a file name; null for other names. */
    fun episodeNo(fileName: String): Int? = EPISODE_RE.find(fileName)?.groupValues?.get(1)?.toIntOrNull()
}

/** 发现页、搜索里的一部作品。/ A title card in the store or search results. */
data class Card(
    val id: String, val title: String, val author: String, val genre: String,
    val likes: Long, val canvas: Boolean, val adult: Boolean,
)

/** 二级分段的一项（榜单、类型、星期）。/ One second-level key (rank, genre, weekday). */
data class Key(val key: String, val name: String)

/** 发现页一页。/ One store list. */
data class StoreList(val seg: String, val key: String, val keys: List<Key>, val items: List<Card>)

/** 一话。/ One episode. */
data class Ep(val no: Int, val title: String, val date: Long = 0)

/** 作品详情。/ Title details. */
data class Title(
    val id: String, val title: String, val author: String, val genre: String, val summary: String,
    val done: Boolean, val latest: Int, val episodes: List<Ep>,
)

/** 检查更新里一部作品的结果。/ One title's result in a check. */
data class CheckResult(val id: String, val latest: Int, val new: List<Ep>, val error: String)

/** 计划下载的一话。/ One episode planned for download. */
data class Planned(val id: String, val no: Int, val title: String)

/** 订阅从哪开始。/ Where a subscription starts. */
enum class Start(val label: String) {
    FIRST("从第 1 话"),
    NEW_ONLY("只追新的"),
    LAST5("最近 5 话"),
}

object Parse {
    fun cards(a: JSONArray?): List<Card> {
        if (a == null) return emptyList()
        return (0 until a.length()).mapNotNull { i ->
            val o = a.optJSONObject(i) ?: return@mapNotNull null
            val id = o.optString("id")
            if (id.isEmpty()) null
            else Card(id, o.optString("title"), o.optString("author"), o.optString("genre"), o.optLong("likes", 0),
                o.optBoolean("canvas", false), o.optBoolean("adult", false))
        }
    }

    fun store(o: JSONObject): StoreList {
        val ka = o.optJSONArray("keys")
        val keys = if (ka == null) emptyList() else (0 until ka.length()).mapNotNull { i ->
            val k = ka.optJSONObject(i) ?: return@mapNotNull null
            Key(k.optString("key"), k.optString("name").ifEmpty { k.optString("key") }).takeIf { it.key.isNotEmpty() }
        }
        return StoreList(o.optString("seg"), o.optString("key"), keys, cards(o.optJSONArray("items")))
    }

    fun eps(a: JSONArray?): List<Ep> {
        if (a == null) return emptyList()
        return (0 until a.length()).mapNotNull { i ->
            val o = a.optJSONObject(i) ?: return@mapNotNull null
            val no = o.optInt("no", 0)
            if (no <= 0) null else Ep(no, o.optString("title"), o.optLong("date", 0))
        }.sortedBy { it.no }
    }

    fun title(o: JSONObject): Title = Title(
        o.optString("id"), o.optString("title"), o.optString("author"), o.optString("genre"), o.optString("summary"),
        o.optBoolean("done", false), o.optInt("latest", 0), eps(o.optJSONArray("episodes")),
    )

    /** 检查更新的回复：{titles:[{id,latest,new:[{no,title}],error?}]}。/ The check reply. */
    fun check(o: JSONObject): List<CheckResult> {
        val a = o.optJSONArray("titles") ?: return emptyList()
        return (0 until a.length()).mapNotNull { i ->
            val t = a.optJSONObject(i) ?: return@mapNotNull null
            val id = t.optString("id")
            if (id.isEmpty()) null
            else CheckResult(id, t.optInt("latest", 0), eps(t.optJSONArray("new")), t.optString("error"))
        }
    }

    /** 检查更新的请求体。/ The check request body. */
    fun checkBody(lang: String, zh: String, titles: List<Pair<String, Int>>): String {
        val a = JSONArray()
        titles.forEach { (id, last) -> a.put(JSONObject().put("id", id).put("last", last)) }
        return JSONObject().put("lang", lang).put("zh", zh).put("titles", a).toString()
    }
}

object Plan {
    /** 订阅时写进 .webtoon 的 last：从第 1 话是 0，只追新的是最新一话，最近 5 话是倒数第 6 话（不够 5 话就是 0）。
     *  The last written into .webtoon on subscribing: 0 from episode 1, the newest episode for new-only, and the sixth
     *  from the end for the latest five (0 when there are five or fewer). */
    fun startLast(episodes: List<Ep>, start: Start): Int {
        val nos = episodes.map { it.no }.sorted()
        return when (start) {
            Start.FIRST -> 0
            Start.NEW_ONLY -> nos.lastOrNull() ?: 0
            Start.LAST5 -> if (nos.size <= 5) 0 else nos[nos.size - 6]
        }
    }

    /**
     * 这次检查要下载哪些话：每部从旧到新，几部之间轮流取，一共不超过 cap（0 表示不限）；已经在卡上的话跳过。
     * 每部取的总是它最旧的那几话，所以 last 能接着往前推。
     * Which episodes to download this check: oldest first within each title, round-robin across titles, at most cap in
     * total (0 = no limit); episodes already on disk are skipped. Each title always takes its oldest ones, so last can
     * keep moving forward.
     */
    fun downloads(results: List<CheckResult>, have: (String) -> Set<Int>, cap: Int): List<Planned> {
        val queues = results.filter { it.error.isEmpty() }.map { r ->
            val owned = have(r.id)
            ArrayDeque(r.new.sortedBy { it.no }.filter { it.no !in owned }.map { Planned(r.id, it.no, it.title) })
        }.filter { it.isNotEmpty() }
        val out = ArrayList<Planned>()
        val limit = if (cap <= 0) Int.MAX_VALUE else cap
        while (out.size < limit && queues.any { it.isNotEmpty() }) {
            for (q in queues) {
                if (out.size >= limit) break
                q.removeFirstOrNull()?.let { out += it }
            }
        }
        return out
    }

    /**
     * 下载完一部之后的 last：从旧到新，已在卡上的话都算好，遇到第一话没下好的就停在它前面，下次检查会再来。
     * The last after one title's downloads: walking oldest to newest, episodes now on disk count as done; stop just
     * before the first one that is missing so the next check retries it.
     */
    fun advance(old: Int, newNos: List<Int>, onDisk: Set<Int>): Int {
        var last = old
        for (no in newNos.sorted()) {
            if (no <= last) continue
            if (no !in onDisk) break
            last = no
        }
        return last
    }
}
