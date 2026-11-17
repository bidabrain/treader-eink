/*
 * SPDX-FileCopyrightText: 2026 XH Lab, Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 拼音引擎实现。算法与词库格式来自 XH View OS For Pico（Apache-2.0）的笔记输入法，
 * 这里拆成与界面无关的组件：词库载入校验、二分查找、按代价的整句 DP 与末尾补全。
 *
 * Pinyin engine. The algorithm and lexicon format come from the note input of
 * XH View OS For Pico (Apache-2.0), split out here as a UI-free component:
 * lexicon load/validation, binary search, cost-based sentence DP and tail completion.
 *
 * 词库格式 PICOIME1（小端）：32 字节头 = "PICOIME1" + 词条数 + 负载字节 + FNV-1a 校验 + 12 保留；
 * 负载 = 全拼序偏移表 + 九键序偏移表 + 记录；记录 = 拼音长、文字长、频率惩罚、拼音\0、文字\0。
 * PICOIME1 (little endian): 32-byte header = magic, count, payload bytes, FNV-1a, 12 reserved;
 * payload = pinyin-ordered offsets, T9-ordered offsets, records of plen, tlen, rank, pinyin\0, text\0.
 */

#include "pinyin_ime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define IME_ALLOC(n) heap_caps_calloc((n), 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define IME_FREE(p) heap_caps_free(p)
#else
#define IME_ALLOC(n) calloc((n), 1)
#define IME_FREE(p) free(p)
#endif

#define IME_MAX_BYTES (2u * 1024u * 1024u)
#define IME_MAX_ENTRIES 65535u
#define IME_EXACT_SCAN 32
#define IME_TAIL_SCAN 400   // 补全窗口：最多看这么多条 / Completion window: entries examined
#define IME_TAIL_KEEP 12    // 补全只取频率最高的几条 / Completions keep only the most frequent few
#define IME_TAIL_SLACK 4
#define IME_INF 1000000

/* ---- 词库 / Lexicon ---- */

static uint8_t* s_blob;
static uint32_t s_count;
static uint32_t s_bytes;
static char s_path[96];
static char s_failed[96];   // 上次载入失败的文件，避免每次打开键盘都重读 / Last failed file, so it is not re-read on every open

static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

char pinyin_ime_t9_digit(char c) {
    if (c < 'a' || c > 'z') return c;
    return c <= 'c' ? '2' : c <= 'f' ? '3' : c <= 'i' ? '4' : c <= 'l' ? '5'
         : c <= 'o' ? '6' : c <= 's' ? '7' : c <= 'v' ? '8' : '9';
}

// 一个 UTF-8 字符的字节数，非法返回 0。/ Byte length of one UTF-8 char, 0 if invalid.
static size_t u8_step(const uint8_t* s, size_t left) {
    if (!left) return 0;
    uint8_t c = s[0];
    size_t n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
    if (!n || n > left) return 0;
    for (size_t i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0;
    }
    return n;
}

static bool u8_valid(const uint8_t* s, size_t n) {
    for (size_t at = 0; at < n;) {
        size_t step = u8_step(s + at, n - at);
        if (!step) return false;
        at += step;
    }
    return true;
}

static const uint8_t* entry_at(const uint8_t* blob, uint32_t count, uint32_t index, bool t9) {
    const uint8_t* table = blob + (t9 ? count * 4 : 0);
    return blob + count * 8 + rd32(table + index * 4);
}

// 词条拼音和查询键比较；九键时先把字母换成数字。/ Compare entry pinyin to key, mapping letters to digits for T9.
static int key_cmp(const char* pinyin, const char* key, size_t key_len, bool t9) {
    for (size_t i = 0; i < key_len; i++) {
        int a = (unsigned char)(t9 ? pinyin_ime_t9_digit(pinyin[i]) : pinyin[i]);
        int b = (unsigned char)key[i];
        if (!pinyin[i]) return -1;
        if (a != b) return a - b;
    }
    return pinyin[key_len] ? 1 : 0;
}

// 前缀比较：只比 key_len 个字符。/ Prefix compare over key_len chars.
static int prefix_cmp(const char* pinyin, const char* key, size_t key_len, bool t9) {
    for (size_t i = 0; i < key_len; i++) {
        if (!pinyin[i]) return -1;
        int a = (unsigned char)(t9 ? pinyin_ime_t9_digit(pinyin[i]) : pinyin[i]);
        int b = (unsigned char)key[i];
        if (a != b) return a - b;
    }
    return 0;
}

// 检查一条记录完全落在数据区内，且字段合法。写成减法形式，偏移再大也不会回绕。
// Check one record lies fully inside the data and is well formed. Written with subtraction so huge offsets cannot wrap.
static bool record_ok(const uint8_t* data, uint32_t data_bytes, uint32_t off) {
    if (off >= data_bytes || data_bytes - off < 5) return false;
    const uint8_t* e = data + off;
    unsigned plen = e[0], tlen = e[1];
    if (!plen || plen > PINYIN_IME_MAX_INPUT || !tlen || tlen >= PINYIN_IME_CAND_TEXT) return false;
    if (data_bytes - off < 5u + plen + tlen || e[3 + plen] || e[4 + plen + tlen]) return false;
    for (unsigned j = 0; j < plen; j++) {
        if (e[3 + j] < 'a' || e[3 + j] > 'z') return false;
    }
    return u8_valid(e + 4 + plen, tlen);
}

static bool validate(const uint8_t* blob, uint32_t count, uint32_t bytes) {
    const uint32_t data_bytes = bytes - count * 8;
    const uint8_t* data = blob + count * 8;
    uint32_t previous = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t off = rd32(blob + i * 4);
        if (!record_ok(data, data_bytes, off)) return false;
        // 记录按拼音序依次写入，全拼表偏移严格递增、拼音不减。/ Records are in pinyin order: offsets increase, pinyin never decreases.
        if (i && (off <= previous || strcmp((const char*)data + previous + 3, (const char*)data + off + 3) > 0)) return false;
        previous = off;
    }
    // 九键表：每个偏移都指向一条合法记录（O(1)），并按数字序排列。内存安全只需要这两点。
    // T9 table: every offset points at a valid record (O(1)) and the order is by digits; that is all memory safety needs.
    for (uint32_t i = 0; i < count; i++) {
        uint32_t off = rd32(blob + (count + i) * 4);
        if (!record_ok(data, data_bytes, off)) return false;
        if (i) {
            const char* a = (const char*)entry_at(blob, count, i - 1, true) + 3;
            const char* b = (const char*)data + off + 3;
            while (*a && *b && pinyin_ime_t9_digit(*a) == pinyin_ime_t9_digit(*b)) { a++; b++; }
            char da = *a ? pinyin_ime_t9_digit(*a) : 0, db = *b ? pinyin_ime_t9_digit(*b) : 0;
            if (da > db) return false;
        }
    }
    return true;
}

bool pinyin_ime_load(const char* path) {
    if (!path) path = PINYIN_IME_DEFAULT_PATH;
    if (s_blob && !strcmp(path, s_path)) return true;
    if (!strcmp(path, s_failed)) return false;
    FILE* f = fopen(path, "rb");
    if (!f) return false;   // 文件不存在不记失败，插卡后还能再试 / A missing file is not remembered; retry after inserting a card
    uint8_t header[32];
    bool ok = fread(header, 1, sizeof(header), f) == sizeof(header) && !memcmp(header, "PICOIME1", 8);
    uint32_t count = ok ? rd32(header + 8) : 0;
    uint32_t bytes = ok ? rd32(header + 12) : 0;
    ok = ok && count && count <= IME_MAX_ENTRIES && bytes > count * 8 && bytes <= IME_MAX_BYTES;
    uint8_t* blob = ok ? IME_ALLOC(bytes) : NULL;
    ok = ok && blob && fread(blob, 1, bytes, f) == bytes && fgetc(f) == EOF;
    fclose(f);
    if (ok) {
        uint32_t hash = 2166136261u;
        for (uint32_t i = 0; i < bytes; i++) hash = (hash ^ blob[i]) * 16777619u;
        ok = hash == rd32(header + 16) && validate(blob, count, bytes);
    }
    if (!ok) {
        IME_FREE(blob);
        snprintf(s_failed, sizeof(s_failed), "%s", path);
        return false;
    }
    pinyin_ime_unload();
    s_blob = blob;
    s_count = count;
    s_bytes = bytes;
    snprintf(s_path, sizeof(s_path), "%s", path);
    return true;
}

void pinyin_ime_unload(void) {
    IME_FREE(s_blob);
    s_blob = NULL;
    s_count = s_bytes = 0;
    s_path[0] = 0;
    s_failed[0] = 0;   // 换卡后允许重试 / Allow a retry after a card change
}

bool pinyin_ime_ready(void) { return s_blob != NULL; }
uint32_t pinyin_ime_entry_count(void) { return s_count; }

/* ---- 候选生成 / Candidate search ---- */

typedef struct {
    pinyin_ime_cand_t* out;
    int max;
    int count;
    bool t9;
    int n;                                   // 有效编码长度 / Effective input length
    char input[PINYIN_IME_MAX_INPUT + 1];    // 去掉分词符的编码 / Input without separators
    uint8_t raw_end[PINYIN_IME_MAX_INPUT];   // 第 i 个编码对应原串的结束字节 / Raw end byte of input i
    uint8_t boundary[PINYIN_IME_MAX_INPUT + 1]; // i 之前有分词符 / Separator before i
    struct { int cost; char text[PINYIN_IME_CAND_TEXT]; } path[PINYIN_IME_MAX_INPUT + 1];
    const char* raw;                         // 原始编码串 / Raw composition
} ime_work_t;

// 选中后要消耗的原始字节数，连同紧跟的分词符。/ Raw bytes to consume on pick, including a following separator.
static int consumed_through(const ime_work_t* w, int end) {
    int c = w->raw_end[end - 1];
    while (w->raw[c] == '\'') c++;
    return c;
}

static void cand_add(ime_work_t* w, const char* text, int consumed, int score) {
    for (int i = 0; i < w->count; i++) {
        if (strcmp(w->out[i].text, text)) continue;
        if (w->out[i].score <= score) return;
        memmove(&w->out[i], &w->out[i + 1], sizeof(w->out[0]) * (size_t)(w->count - i - 1));
        w->count--;
        break;
    }
    int at = 0;
    while (at < w->count && w->out[at].score <= score) at++;
    if (at >= w->max) return;
    int last = w->count < w->max ? w->count++ : w->max - 1;
    memmove(&w->out[at + 1], &w->out[at], sizeof(w->out[0]) * (size_t)(last - at));
    snprintf(w->out[at].text, sizeof(w->out[at].text), "%s", text);
    w->out[at].consumed = (uint8_t)consumed;
    w->out[at].score = (int16_t)(score > 32767 ? 32767 : score);
}

// 尝试把一个词条接在 start 位置：完整匹配或末尾补全都推进 DP，并在 start==0 时出候选。
// Try an entry at start: an exact match or a tail completion advances the DP; at start 0 it also yields a candidate.
static void try_entry(ime_work_t* w, const char* pinyin, const char* text, int rank, int start) {
    int plen = (int)strlen(pinyin), same = 0;
    while (same < plen && start + same < w->n && (!same || !w->boundary[start + same])) {
        char letter = w->t9 ? pinyin_ime_t9_digit(pinyin[same]) : pinyin[same];
        if (letter != w->input[start + same]) break;
        same++;
    }
    bool exact = same == plen;
    bool completion = same > 0 && start + same == w->n && plen - same <= IME_TAIL_SLACK;
    if (!exact && !completion) return;
    int end = start + same;
    if (w->path[start].cost < IME_INF) {
        int cost = w->path[start].cost + 8 + (exact ? 0 : 18 + (plen - same) * 8);
        size_t have = strlen(w->path[start].text), add = strlen(text);
        if (cost < w->path[end].cost && have + add < sizeof(w->path[end].text)) {
            memcpy(w->path[end].text, w->path[start].text, have);
            memcpy(w->path[end].text + have, text, add + 1);
            w->path[end].cost = cost;
        }
    }
    if (start) return;
    int score = exact ? (end == w->n ? 0 : 240 - end * 2) : 90 + (plen - same) * 4;
    cand_add(w, text, consumed_through(w, end), score + rank);
}

static void scan_from(ime_work_t* w, int start) {
    char key[PINYIN_IME_MAX_INPUT + 1];
    // 每个长度做一次精确二分。/ One exact binary search per length.
    for (int end = start + 1; end <= w->n; end++) {
        if (end > start + 1 && w->boundary[end - 1]) break;
        size_t len = (size_t)(end - start);
        memcpy(key, w->input + start, len);
        key[len] = 0;
        uint32_t lo = 0, hi = s_count;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            if (key_cmp((const char*)entry_at(s_blob, s_count, mid, w->t9) + 3, key, len, w->t9) < 0) lo = mid + 1;
            else hi = mid;
        }
        // 下界处连前缀都对不上，更长的键也不可能匹配。/ If even the prefix fails at the bound, longer keys cannot match.
        if (lo >= s_count || prefix_cmp((const char*)entry_at(s_blob, s_count, lo, w->t9) + 3, key, len, w->t9)) break;
        for (uint32_t at = lo, seen = 0; at < s_count && seen < IME_EXACT_SCAN; at++, seen++) {
            const uint8_t* e = entry_at(s_blob, s_count, at, w->t9);
            const char* py = (const char*)e + 3;
            if (key_cmp(py, key, len, w->t9)) break;
            try_entry(w, py, py + e[0] + 1, e[2], start);
        }
    }
    // 末尾不完整音节：前缀窗口内补全。/ Unfinished tail syllable: complete within a bounded prefix window.
    int remaining = w->n - start;
    if (remaining < 2) return;
    memcpy(key, w->input + start, (size_t)remaining);
    key[remaining] = 0;
    uint32_t lo = 0, hi = s_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (prefix_cmp((const char*)entry_at(s_blob, s_count, mid, w->t9) + 3, key, (size_t)remaining, w->t9) < 0) lo = mid + 1;
        else hi = mid;
    }
    // 窗口按拼音排序而不是按频率，所以先挑出频率最高的几条再用。/ The window is in pinyin order, not by frequency: pick the most frequent first.
    const uint8_t* best[IME_TAIL_KEEP];
    int best_n = 0;
    for (uint32_t at = lo, seen = 0; at < s_count && seen < IME_TAIL_SCAN && at - lo < 4 * IME_TAIL_SCAN; at++) {
        const uint8_t* e = entry_at(s_blob, s_count, at, w->t9);
        const char* py = (const char*)e + 3;
        if (prefix_cmp(py, key, (size_t)remaining, w->t9)) break;
        if (e[0] == remaining || e[0] - remaining > IME_TAIL_SLACK) continue;
        seen++;
        if (best_n < IME_TAIL_KEEP) { best[best_n++] = e; continue; }
        int worst = 0;
        for (int k = 1; k < best_n; k++) if (best[k][2] > best[worst][2]) worst = k;
        if (e[2] < best[worst][2]) best[worst] = e;
    }
    for (int k = 0; k < best_n; k++) {
        const char* py = (const char*)best[k] + 3;
        try_entry(w, py, py + best[k][0] + 1, best[k][2], start);
    }
}

int pinyin_ime_query(const char* composition, pinyin_ime_mode_t mode, pinyin_ime_cand_t* out, int max) {
    if (!s_blob || !composition || !out || max <= 0) return 0;
    // 工作区约 3 KB，放静态区，避免压主循环栈。/ ~3 KB workspace kept static to spare the loop stack.
    static ime_work_t w;
    memset(&w, 0, sizeof(w));
    w.out = out;
    w.max = max;
    w.t9 = mode == PINYIN_IME_T9;
    w.raw = composition;
    for (int i = 0; composition[i]; i++) {
        char c = composition[i];
        if (c == '\'') { w.boundary[w.n] = 1; continue; }
        bool ok = w.t9 ? (c >= '2' && c <= '9') : (c >= 'a' && c <= 'z');
        if (!ok || w.n >= PINYIN_IME_MAX_INPUT) return 0;
        w.input[w.n] = c;
        w.raw_end[w.n++] = (uint8_t)(i + 1);
    }
    if (!w.n) return 0;
    // 末尾分词符随候选一起消耗。/ A trailing separator is consumed with its candidate.
    w.raw_end[w.n - 1] = (uint8_t)strlen(composition);
    for (int i = 0; i <= w.n; i++) w.path[i].cost = IME_INF;
    w.path[0].cost = 0;
    for (int start = 0; start < w.n; start++) {
        if (start && w.path[start].cost >= IME_INF) continue;
        scan_from(&w, start);
    }
    if (w.path[w.n].cost < IME_INF) {
        cand_add(&w, w.path[w.n].text, (int)strlen(composition), 8 + w.path[w.n].cost / 20);
    }
    return w.count;
}
