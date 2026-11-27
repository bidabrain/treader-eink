/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：缓存包主机测试：读写、覆盖、删除、重启后重建、掉电留下的半条、数据和段尾损坏、上限与回收、门槛升级、
 * 超限拒写、剩余空间不足、换卡、旧目录挪进回收站并清空，以及和内存模型对照的随机读写。
 *
 * English: host test for the cache pack: read and write, replace, delete, rebuild after a restart, half records left
 * by a power cut, damaged data and footers, the cap and collection, threshold escalation, refusing writes far over the
 * cap, low free space, card swaps, old folders moved to the trash and emptied, and random operations checked against an
 * in-memory model.
 */

#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cache_pack.h"

#define BASE "/tmp/cpack_test"
#define SD BASE "/sd"
#define ROOT SD "/.treader/cache"
#define TRASH SD "/.treader/trash"
#define POOL_DIR ROOT "/red"
#define P(k) SD "/rednote/n/" k

static int s_judge = 1;           // 0：gc_begin 返回 NULL / gc_begin returns NULL
static uint32_t s_gen = 1;
static uint64_t s_free = UINT64_MAX;

static void* gc_begin(void* user) { (void)user; return s_judge ? (void*)1 : NULL; }
static cpack_live_t live(void* ctx, const char* key) {
    (void)ctx;
    if (strstr(key, "/keep")) return CPACK_LIVE_KEEP;
    if (strstr(key, "/room")) return CPACK_LIVE_ROOM;
    return CPACK_LIVE_DROP;
}
static void gc_end(void* ctx) { (void)ctx; }
static uint32_t gen(void) { return s_gen; }
static uint64_t free_bytes(void) { return s_free; }

static const char* const OLD_DIRS[] = {SD "/rednote/n", SD "/rednote/rc", NULL};

static void sh(const char* cmd) { assert(system(cmd) == 0); }

static void setup(uint64_t cap, uint32_t seg_max) {
    cpack_test_reset();
    sh("rm -rf " BASE " && mkdir -p " SD "/rednote");
    s_judge = 1;
    s_gen = 1;
    s_free = UINT64_MAX;
    assert(cpack_init(ROOT, TRASH) == ESP_OK);
    cpack_test_set_segment_max(seg_max);
    const cpack_pool_cfg_t cfg = {
        .name = "red",
        .prefixes = {SD "/rednote/n/", SD "/rednote/rc/", NULL},
        .root = SD "/rednote/",
        .cap_bytes = cap,
        .old_dirs = OLD_DIRS,
        .gc_begin = gc_begin,
        .live = live,
        .gc_end = gc_end,
    };
    assert(cpack_register(&cfg) == ESP_OK);
    cpack_set_generation_fn(gen);
    cpack_set_free_fn(free_bytes);
}

static void fill(uint8_t* buf, size_t n, uint32_t seed) {
    for (size_t i = 0; i < n; i++) { seed = seed * 1103515245u + 12345u; buf[i] = (uint8_t)(seed >> 16); }
}

static void put_seeded(const char* path, size_t n, uint32_t seed) {
    uint8_t* buf = malloc(n ? n : 1);
    fill(buf, n, seed);
    assert(cpack_put(path, buf, n) == ESP_OK);
    free(buf);
}

// 读回来必须和种子生成的一样。/ What reads back must match the seeded data.
static bool same_seeded(const char* path, size_t n, uint32_t seed) {
    uint8_t* got = NULL;
    size_t len = 0;
    if (cpack_get(path, 1u << 24, &got, &len) != ESP_OK) return false;
    uint8_t* want = malloc(n ? n : 1);
    fill(want, n, seed);
    const bool ok = len == n && memcmp(got, want, n) == 0 && got[n] == 0;
    free(want);
    cpack_free(got);
    return ok;
}

static int count_files(const char* dir) {
    DIR* d = opendir(dir);
    if (!d) return -1;
    int n = 0;
    struct dirent* e;
    while ((e = readdir(d))) if (e->d_name[0] != '.') n++;
    closedir(d);
    return n;
}

static void last_segment(char* out, size_t cap) {
    DIR* d = opendir(POOL_DIR);
    assert(d);
    char best[64] = "";
    struct dirent* e;
    while ((e = readdir(d))) if (e->d_name[0] != '.' && strcmp(e->d_name, best) > 0) snprintf(best, sizeof(best), "%.60s", e->d_name);
    closedir(d);
    assert(best[0]);
    snprintf(out, cap, POOL_DIR "/%s", best);
}

static long fsize(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

/* ---- 基本 / Basics ---- */

static void test_basic(void) {
    setup(1u << 30, 1u << 20);
    assert(!cpack_owns(SD "/rednote/feed.json"));
    assert(cpack_owns(P("a/t.jpg")));
    assert(cpack_owns(SD "/rednote/rc/x.jpg"));
    assert(!cpack_owns(SD "/rednote/n/"));
    assert(cpack_put(SD "/rednote/feed.json", "x", 1) == ESP_ERR_INVALID_ARG);
    size_t len = 99;
    assert(!cpack_size(P("a/t.jpg"), &len));
    put_seeded(P("a/t.jpg"), 1000, 1);
    assert(cpack_size(P("a/t.jpg"), &len) && len == 1000);
    assert(same_seeded(P("a/t.jpg"), 1000, 1));
    put_seeded(P("a/t.jpg"), 3000, 2);   // 覆盖 / Replace
    assert(cpack_size(P("a/t.jpg"), &len) && len == 3000);
    assert(same_seeded(P("a/t.jpg"), 3000, 2));
    // 两段合成一条 / Two parts become one record
    uint8_t two[700];
    fill(two, sizeof(two), 44);
    assert(cpack_put2(P("a/two"), two, 8, two + 8, sizeof(two) - 8) == ESP_OK);
    assert(same_seeded(P("a/two"), sizeof(two), 44));
    put_seeded(P("a/empty"), 0, 3);
    assert(cpack_size(P("a/empty"), &len) && len == 0);
    assert(same_seeded(P("a/empty"), 0, 3));
    uint8_t* out = (uint8_t*)1;
    assert(cpack_get(P("a/t.jpg"), 100, &out, &len) == ESP_ERR_INVALID_SIZE && out == NULL);
    char head[16];
    size_t got = 0;
    assert(cpack_read_head(P("a/t.jpg"), head, sizeof(head), &got) == ESP_OK && got == sizeof(head));
    uint8_t want[16];
    fill(want, sizeof(want), 2);
    assert(memcmp(head, want, sizeof(want)) == 0);
    assert(cpack_remove(P("a/t.jpg")) == ESP_OK);
    assert(!cpack_size(P("a/t.jpg"), NULL));
    assert(cpack_get(P("a/t.jpg"), 1u << 20, &out, &len) == ESP_ERR_NOT_FOUND);
    assert(cpack_remove(P("a/none")) == ESP_OK);
    // key 太长 / Key too long
    char longp[300];
    snprintf(longp, sizeof(longp), P("%0200d"), 0);
    assert(cpack_put(longp, "x", 1) == ESP_ERR_INVALID_ARG);
    // 一条比段还大 / A record larger than a segment
    setup(64 * 1024, 4096);
    uint8_t big[5000] = {0};
    assert(cpack_put(P("a/big"), big, sizeof(big)) == ESP_ERR_INVALID_SIZE);
    puts("basic ok");
}

/* ---- 重启重建 / Rebuild after restart ---- */

static void test_persist(void) {
    setup(256 * 1024, 8192);
    char path[96];
    for (int i = 0; i < 200; i++) {
        snprintf(path, sizeof(path), P("p%d/%d.jpg"), i % 37, i);
        put_seeded(path, 50 + (size_t)(i * 13) % 900, (uint32_t)i);
    }
    for (int i = 0; i < 200; i += 7) { snprintf(path, sizeof(path), P("p%d/%d.jpg"), i % 37, i); assert(cpack_remove(path) == ESP_OK); }
    for (int i = 0; i < 200; i += 11) { snprintf(path, sizeof(path), P("p%d/%d.jpg"), i % 37, i); put_seeded(path, 77, 9000u + (uint32_t)i); }
    assert(count_files(POOL_DIR) > 5);
    for (int round = 0; round < 2; round++) {
        cpack_test_unload();
        for (int i = 0; i < 200; i++) {
            snprintf(path, sizeof(path), P("p%d/%d.jpg"), i % 37, i);
            if (i % 11 == 0) assert(same_seeded(path, 77, 9000u + (uint32_t)i));
            else if (i % 7 == 0) assert(!cpack_size(path, NULL));
            else assert(same_seeded(path, 50 + (size_t)(i * 13) % 900, (uint32_t)i));
        }
    }
    cpack_stats_t st;
    assert(cpack_stats("red", &st) && st.loaded && st.records > 150 && st.live_bytes <= st.bytes);
    puts("persist ok");
}

/* ---- 掉电半条 / Half records ---- */

static void test_torn(void) {
    setup(1u << 30, 1u << 20);
    put_seeded(P("t/1"), 500, 1);
    put_seeded(P("t/2"), 500, 2);
    put_seeded(P("t/3"), 500, 3);
    char seg[160];
    last_segment(seg, sizeof(seg));
    const long n = fsize(seg);
    assert(truncate(seg, n - 100) == 0);   // 最后一条写了一半 / The last record half written
    cpack_test_unload();
    assert(same_seeded(P("t/1"), 500, 1));
    assert(same_seeded(P("t/2"), 500, 2));
    assert(!cpack_size(P("t/3"), NULL));
    assert(fsize(seg) == n - 100 - (24 + 5 + 500 - 100));   // 半条截掉了 / The half record was cut
    put_seeded(P("t/4"), 300, 4);
    cpack_test_unload();
    assert(same_seeded(P("t/2"), 500, 2) && same_seeded(P("t/4"), 300, 4));
    // 尾巴上的垃圾 / Garbage at the tail
    FILE* f = fopen(seg, "ab");
    assert(f);
    for (int i = 0; i < 77; i++) fputc(0xA5, f);
    fclose(f);
    cpack_test_unload();
    assert(same_seeded(P("t/4"), 300, 4));
    put_seeded(P("t/5"), 200, 5);
    cpack_test_unload();
    assert(same_seeded(P("t/5"), 200, 5) && same_seeded(P("t/1"), 500, 1));
    // 段头坏了：整段丢掉 / A broken segment header drops the segment
    f = fopen(seg, "r+b");
    fputc(0, f);
    fclose(f);
    cpack_test_unload();
    assert(!cpack_size(P("t/1"), NULL));
    put_seeded(P("t/6"), 10, 6);
    assert(same_seeded(P("t/6"), 10, 6));
    puts("torn ok");
}

/* ---- 损坏 / Damage ---- */

static void test_corrupt(void) {
    setup(128 * 1024, 4096);
    char path[64];
    for (int i = 0; i < 30; i++) { snprintf(path, sizeof(path), P("c/%d"), i); put_seeded(path, 400, (uint32_t)i); }
    // 第一段里第一条的数据改一个字节 / Flip one data byte of the first record in the first segment
    char first[160];
    snprintf(first, sizeof(first), POOL_DIR "/00000001.SEG");
    FILE* f = fopen(first, "r+b");
    assert(f);
    fseek(f, 16 + 24 + 5 + 10, SEEK_SET);
    const int c = fgetc(f);
    fseek(f, 16 + 24 + 5 + 10, SEEK_SET);
    fputc(c ^ 0xFF, f);
    fclose(f);
    uint8_t* out;
    size_t len;
    assert(cpack_get(P("c/0"), 1u << 20, &out, &len) == ESP_ERR_NOT_FOUND);
    assert(!cpack_size(P("c/0"), NULL));
    assert(same_seeded(P("c/1"), 400, 1));
    // 段尾索引坏了：退回逐条扫描 / A broken footer falls back to a scan
    f = fopen(first, "r+b");
    fseek(f, -3, SEEK_END);
    fputc(0x00, f);
    fclose(f);
    cpack_test_unload();
    for (int i = 1; i < 30; i++) { snprintf(path, sizeof(path), P("c/%d"), i); assert(same_seeded(path, 400, (uint32_t)i)); }
    puts("corrupt ok");
}

/* ---- 回收 / Collection ---- */

static void test_gc(void) {
    const uint64_t cap = 64 * 1024;
    const uint32_t seg = 8192;
    setup(cap, seg);
    char path[64];
    // 先写一些要留的 / Some records that stay
    for (int i = 0; i < 10; i++) { snprintf(path, sizeof(path), P("k%d/keep"), i); put_seeded(path, 1500, 100u + (uint32_t)i); }
    for (int i = 0; i < 2000; i++) {
        snprintf(path, sizeof(path), P("d%d/x"), i);
        put_seeded(path, 100 + (size_t)(i * 37) % 1900, (uint32_t)i);
        if (i % 5 == 0) {
            cpack_test_service();
            cpack_stats_t st;
            assert(cpack_stats("red", &st));
            assert(st.bytes <= cap + 2 * seg);
        }
    }
    cpack_test_service();
    cpack_stats_t st;
    assert(cpack_stats("red", &st) && st.bytes <= cap);
    for (int i = 0; i < 10; i++) { snprintf(path, sizeof(path), P("k%d/keep"), i); assert(same_seeded(path, 1500, 100u + (uint32_t)i)); }
    snprintf(path, sizeof(path), P("d%d/x"), 1999);
    assert(same_seeded(path, 100 + (size_t)(1999 * 37) % 1900, 1999));
    snprintf(path, sizeof(path), P("d%d/x"), 0);
    assert(!cpack_size(path, NULL));
    // 段文件个数也有限 / The segment count is bounded too
    assert(count_files(POOL_DIR) <= (int)(cap / seg) + 3);
    // 重启后照样 / Same after a restart
    cpack_test_unload();
    for (int i = 0; i < 10; i++) { snprintf(path, sizeof(path), P("k%d/keep"), i); assert(same_seeded(path, 1500, 100u + (uint32_t)i)); }
    puts("gc ok");
}

static void test_escalate(void) {
    const uint64_t cap = 32 * 1024;
    const uint32_t seg = 4096;
    setup(cap, seg);
    char path[64];
    // 全是“尽量留”的，加起来远超上限：门槛要升上去，总量仍然有限。/ All "keep", far over the cap in total: the threshold
    // must rise and the total stays bounded.
    for (int i = 0; i < 400; i++) {
        snprintf(path, sizeof(path), P("e%d/keep"), i);
        put_seeded(path, 700, (uint32_t)i);
        if (i % 10 == 0) cpack_test_service();
    }
    cpack_test_service();
    cpack_stats_t st;
    assert(cpack_stats("red", &st) && st.bytes <= cap);
    snprintf(path, sizeof(path), P("e%d/keep"), 399);
    assert(same_seeded(path, 700, 399));
    puts("escalate ok");
}

static void test_refuse(void) {
    const uint64_t cap = 16 * 1024;
    const uint32_t seg = 4096;
    setup(cap, seg);
    char path[64];
    bool refused = false;
    // 后台不回收时，超过上限两段以上就拒写。/ With no collection running, writes are refused past the cap plus two
    // segments.
    for (int i = 0; i < 200 && !refused; i++) {
        snprintf(path, sizeof(path), P("r%d/x"), i);
        uint8_t buf[500];
        fill(buf, sizeof(buf), (uint32_t)i);
        const esp_err_t err = cpack_put(path, buf, sizeof(buf));
        assert(err == ESP_OK || err == ESP_ERR_NO_MEM);
        refused = err == ESP_ERR_NO_MEM;
    }
    assert(refused);
    cpack_stats_t st;
    assert(cpack_stats("red", &st) && st.bytes <= cap + 3 * seg);
    // 判断不了（列表读坏）时不回收，也不删东西。/ When judging is impossible (a list failed to read) nothing is collected
    // or deleted.
    s_judge = 0;
    const uint64_t before = st.bytes;
    cpack_test_service();
    assert(cpack_stats("red", &st) && st.bytes >= before - 4096);
    s_judge = 1;
    cpack_test_service();
    assert(cpack_stats("red", &st) && st.bytes <= cap);
    snprintf(path, sizeof(path), P("r%d/x"), 0);
    assert(cpack_put(path, "ok", 2) == ESP_OK);
    puts("refuse ok");
}

static void test_low_space(void) {
    const uint64_t cap = 256 * 1024;
    const uint32_t seg = 8192;
    setup(cap, seg);
    char path[64];
    for (int i = 0; i < 300; i++) { snprintf(path, sizeof(path), P("s%d/x"), i); put_seeded(path, 600, (uint32_t)i); }
    cpack_test_service();
    cpack_stats_t st;
    assert(cpack_stats("red", &st));
    const uint64_t before = st.bytes;
    s_free = 48ull * 1024 * 1024 - 100 * 1024;   // 差 100 KB / 100 KB short
    cpack_test_service();
    assert(cpack_stats("red", &st) && st.bytes + 90 * 1024 <= before);
    puts("low space ok");
}

/* ---- 换卡 / Card swap ---- */

static void test_generation(void) {
    setup(1u << 30, 1u << 20);
    put_seeded(P("g/1"), 100, 1);
    assert(cpack_size(P("g/1"), NULL));
    sh("rm -rf " POOL_DIR);   // 换了一张没有缓存的卡 / A card without the cache
    s_gen++;
    assert(!cpack_size(P("g/1"), NULL));
    put_seeded(P("g/2"), 100, 2);
    assert(same_seeded(P("g/2"), 100, 2));
    // 运行中缓存目录被外面删了：头一次写失败并重读，之后照常。/ The cache folder deleted from outside while running:
    // the first write fails and reloads, then things carry on.
    sh("rm -rf " ROOT);
    assert(cpack_put(P("g/4"), "abc", 3) == ESP_FAIL);
    put_seeded(P("g/5"), 100, 5);
    assert(same_seeded(P("g/5"), 100, 5));
    assert(!cpack_size(P("g/2"), NULL));
    cpack_test_unload();
    assert(same_seeded(P("g/5"), 100, 5));
    cpack_pause();
    assert(cpack_put(P("g/3"), "x", 1) != ESP_OK);
    assert(!cpack_size(P("g/5"), NULL));
    cpack_resume();
    assert(same_seeded(P("g/5"), 100, 5));
    puts("generation ok");
}

/* ---- 旧目录 / Old folders ---- */

static void test_migrate(void) {
    setup(1u << 30, 1u << 20);
    sh("mkdir -p " SD "/rednote/n/a " SD "/rednote/n/b " SD "/rednote/rc");
    for (int i = 0; i < 100; i++) {
        char cmd[160];
        snprintf(cmd, sizeof(cmd), "echo x > " SD "/rednote/n/%c/%d.jpg", i % 2 ? 'a' : 'b', i);
        sh(cmd);
    }
    sh("echo x > " SD "/rednote/rc/c.jpg && echo y > " SD "/rednote/feed.json");
    put_seeded(P("a/new"), 10, 1);   // 第一次用池：旧目录挪走 / First use of the pool: old folders move away
    struct stat st;
    assert(stat(SD "/rednote/n", &st) != 0 && stat(SD "/rednote/rc", &st) != 0);
    assert(stat(SD "/rednote/feed.json", &st) == 0);   // 列表文件不动 / The list file stays
    assert(count_files(TRASH) == 2);
    cpack_test_service();
    assert(count_files(TRASH) == 0);
    assert(same_seeded(P("a/new"), 10, 1));
    puts("migrate ok");
}

static void test_many_segments(void) {
    // 段大小按上限放大，段数有限。/ The segment size scales with the cap, so the segment count stays bounded.
    setup(1u << 20, 2048);
    char path[64];
    for (int i = 0; i < 600; i++) {
        snprintf(path, sizeof(path), P("n%d/keep"), i);
        put_seeded(path, 600, (uint32_t)i);
        if (i % 10 == 0) cpack_test_service();
    }
    cpack_test_service();
    assert(count_files(POOL_DIR) <= 40);
    snprintf(path, sizeof(path), P("n%d/keep"), 599);
    assert(same_seeded(path, 600, 599));
    puts("many segments ok");
}

// 回收搬完了、旧段还没删就掉电：旧段又出现时，读到的必须是最新的。/ Power lost after collection moved records but
// before the old segment was deleted: when the old segment reappears, reads must return the newest data.
static void test_crash_mid_gc(void) {
    setup(16 * 1024, 4096);
    char path[64];
    for (int i = 0; i < 6; i++) { snprintf(path, sizeof(path), P("z%d/keep"), i); put_seeded(path, 500, (uint32_t)i); }
    sh("cp " POOL_DIR "/00000001.SEG " BASE "/saved.seg");
    snprintf(path, sizeof(path), P("z%d/keep"), 0);
    put_seeded(path, 300, 777);   // 新版本 / A newer version
    for (int i = 0; i < 60; i++) {
        snprintf(path, sizeof(path), P("f%d/x"), i);
        put_seeded(path, 500, 1000u + (uint32_t)i);
        if (i % 5 == 0) cpack_test_service();
    }
    cpack_test_service();
    struct stat st;
    assert(stat(POOL_DIR "/00000001.SEG", &st) != 0);
    sh("cp " BASE "/saved.seg " POOL_DIR "/00000001.SEG");
    cpack_test_unload();
    snprintf(path, sizeof(path), P("z%d/keep"), 0);
    assert(same_seeded(path, 300, 777));
    for (int i = 1; i < 6; i++) { snprintf(path, sizeof(path), P("z%d/keep"), i); assert(same_seeded(path, 500, (uint32_t)i)); }
    cpack_test_service();
    cpack_stats_t stt;
    assert(cpack_stats("red", &stt) && stt.bytes <= 16 * 1024);
    snprintf(path, sizeof(path), P("z%d/keep"), 0);
    assert(same_seeded(path, 300, 777));
    puts("crash mid gc ok");
}

// 乱放进来的大段号文件不能让新写的排到旧的前面。/ A stray huge segment id must not sort new writes before old ones.
static void test_stray_id(void) {
    setup(1u << 20, 4096);
    put_seeded(P("s/a"), 100, 1);
    cpack_test_unload();
    sh("echo junk > " POOL_DIR "/FFFFFFFF.SEG && echo junk > " POOL_DIR "/-0000001.SEG");
    put_seeded(P("s/a"), 100, 2);
    cpack_test_unload();
    assert(same_seeded(P("s/a"), 100, 2));
    put_seeded(P("s/a"), 100, 3);
    cpack_test_unload();
    assert(same_seeded(P("s/a"), 100, 3));
    puts("stray id ok");
}

// 列表里的条目占了上限的九成多：写放大要有限，大记录（超过搬运块）照样搬对。/ Listed items fill over 90% of the cap:
// write amplification stays bounded and large records (bigger than the copy chunk) still move intact.
static void test_amplification(void) {
    const uint64_t cap = 2u << 20;
    setup(cap, 64 * 1024);
    char path[64];
    // 约 1.8 MB 要留：一半“有空间就留”的大图（40 KB，大过搬运块），一半“尽量留”。/ About 1.8 MB to keep: half "keep
    // while there is room" big images (40 KB, larger than the copy chunk), half "keep".
    for (int i = 0; i < 22; i++) { snprintf(path, sizeof(path), P("a%d/room"), i); put_seeded(path, 40 * 1024, 500u + (uint32_t)i); }
    for (int i = 0; i < 22; i++) { snprintf(path, sizeof(path), P("a%d/keep"), i); put_seeded(path, 40 * 1024, 900u + (uint32_t)i); }
    const uint64_t before = cpack_test_written();
    uint64_t user = 0;
    for (int i = 0; i < 2000; i++) {
        snprintf(path, sizeof(path), P("z%d/x"), i);
        put_seeded(path, 10 * 1024, (uint32_t)i);
        user += 10 * 1024;
        if (i % 4 == 0) cpack_test_service();
        cpack_stats_t st;
        assert(cpack_stats("red", &st) && st.bytes <= cap + 3 * 64 * 1024);
    }
    cpack_test_service();
    const double amp = (double)(cpack_test_written() - before) / (double)user;
    printf("amplification %.2fx\n", amp);
    assert(amp < 3.0);
    for (int i = 0; i < 22; i++) { snprintf(path, sizeof(path), P("a%d/keep"), i); assert(same_seeded(path, 40 * 1024, 900u + (uint32_t)i)); }
    cpack_test_unload();
    for (int i = 0; i < 22; i++) { snprintf(path, sizeof(path), P("a%d/keep"), i); assert(same_seeded(path, 40 * 1024, 900u + (uint32_t)i)); }
    puts("amplification ok");
}

/* ---- 随机对照 / Random model check ---- */

#define KEYS 60

typedef struct {
    bool have;
    size_t len;
    uint32_t seed;
} model_t;

static void key_path(int k, char* out, size_t cap) {
    snprintf(out, cap, P("m%d/%s"), k, k % 3 == 0 ? "keep" : k % 3 == 1 ? "room" : "x");
}

static void test_model(bool tight) {
    setup(tight ? 48 * 1024 : 8u << 20, tight ? 4096 : 65536);
    model_t m[KEYS] = {0};
    uint32_t r = 12345;
    char path[64];
    for (int step = 0; step < 6000; step++) {
        r = r * 1664525u + 1013904223u;
        const int k = (int)((r >> 8) % KEYS);
        const int op = (int)((r >> 20) % 100);
        key_path(k, path, sizeof(path));
        if (op < 45) {
            const size_t len = (r >> 4) % 1500;
            uint8_t* buf = malloc(len ? len : 1);
            fill(buf, len, (uint32_t)step);
            const esp_err_t err = cpack_put(path, buf, len);
            free(buf);
            if (err == ESP_OK) m[k] = (model_t){.have = true, .len = len, .seed = (uint32_t)step};
            else { assert(tight && err == ESP_ERR_NO_MEM); }
        } else if (op < 55) {
            assert(cpack_remove(path) == ESP_OK);
            m[k].have = false;
        } else if (op < 90) {
            uint8_t* got = NULL;
            size_t len = 0;
            const esp_err_t err = cpack_get(path, 1u << 20, &got, &len);
            if (err == ESP_OK) {
                // 读到的只能是最后一次写的，绝不能是旧版本。/ Whatever reads back must be the last write, never an
                // older version.
                assert(m[k].have && len == m[k].len);
                uint8_t* want = malloc(len ? len : 1);
                fill(want, len, m[k].seed);
                assert(memcmp(got, want, len) == 0);
                free(want);
                cpack_free(got);
            } else {
                assert(err == ESP_ERR_NOT_FOUND);
                // 宽松的上限下什么都不会丢。/ Nothing is lost under a loose cap.
                assert(tight || !m[k].have);
                if (tight) m[k].have = false;
            }
        } else if (op < 95) {
            cpack_test_service();
            if (tight) {
                cpack_stats_t st;
                assert(cpack_stats("red", &st) && st.bytes <= 48 * 1024);
            }
        } else {
            cpack_test_unload();
        }
    }
    printf("model %s ok\n", tight ? "tight" : "loose");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    test_basic();
    test_persist();
    test_torn();
    test_corrupt();
    test_gc();
    test_escalate();
    test_refuse();
    test_low_space();
    test_generation();
    test_migrate();
    test_many_segments();
    test_crash_mid_gc();
    test_stray_id();
    test_amplification();
    test_model(false);
    test_model(true);
    cpack_test_reset();
    sh("rm -rf " BASE);
    puts("cache_pack host test passed");
    return 0;
}
