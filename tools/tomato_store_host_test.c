/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：番茄书架“已看”章节数的主机测试：旧文件迁移、存盘与磁盘合并、打开清除提示、两种提示的计数。
 * 直接编进 tomato_store.c，路径用真实的 /sdcard/tomato，只在一次性容器里跑。
 *
 * English: host test of the Tomato shelf's seen count: old-file migration, merging with disk on save, clearing on
 * open, and both badge counts. Builds tomato_store.c in directly and uses the real /sdcard/tomato path, so run it
 * only in a throwaway container.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../components/tomato/tomato_store.c"

// tomato_http.c 里的几个小工具，照抄过来避免拉进联网代码。/ Small helpers from tomato_http.c, copied to avoid the network code.
void tomato_copy_utf8(char* out, size_t cap, const char* in) {
    if (!cap) return;
    size_t n = strlen(in);
    if (n >= cap) {
        n = cap - 1;
        while (n && ((unsigned char)in[n] & 0xC0) == 0x80) n--;
    }
    memcpy(out, in, n);
    out[n] = 0;
}
int tomato_json_int(const cJSON* obj, const char* key) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(v)) return v->valueint;
    if (cJSON_IsString(v) && v->valuestring) return atoi(v->valuestring);
    return 0;
}
void tomato_json_str(const cJSON* obj, const char* key, char* out, size_t cap) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    tomato_copy_utf8(out, cap, cJSON_IsString(v) && v->valuestring ? v->valuestring : "");
}
bool tomato_sync_defer_remove(const char* file) { (void)file; return false; }

static void write_text(const char* path, const char* text) {
    FILE* f = fopen(path, "wb");
    assert(f);
    fputs(text, f);
    fclose(f);
}

static tomato_book_t load_one(void) {
    tomato_book_t b[4];
    int n = 0;
    assert(tomato_shelf_load(b, 4, &n) == ESP_OK && n == 1);
    return b[0];
}

static tomato_book_t make(int chapters, int remote, int seen) {
    tomato_book_t b = {0};
    strcpy(b.book_id, "1");
    strcpy(b.title, "书");
    strcpy(b.file, "a.txt");
    b.chapters = (uint16_t)chapters;
    b.remote = (uint16_t)remote;
    b.seen = (uint16_t)seen;
    return b;
}

int main(void) {
    mkdir("/sdcard", 0775);
    mkdir(TOMATO_DIR, 0775);

    // 计数：不出负数。/ Counts never go negative.
    tomato_book_t c = make(105, 110, 100);
    assert(tomato_book_pending(&c) == 5 && tomato_book_fresh(&c) == 5);
    c = make(100, 90, 120);
    assert(tomato_book_pending(&c) == 0 && tomato_book_fresh(&c) == 0);

    // 旧文件没有 seen：只留待下载。/ An old file without seen keeps only pending.
    write_text(TOMATO_SHELF_PATH, "{\"books\":[{\"book_id\":\"1\",\"title\":\"书\",\"author\":\"\",\"file\":\"a.txt\","
                                  "\"chapters\":100,\"remote\":103}]}");
    tomato_book_t b = load_one();
    assert(b.seen == 100 && tomato_book_pending(&b) == 3 && tomato_book_fresh(&b) == 0);

    // 下载全部更新后仍提示，打开后消失。/ Still marked after downloading; cleared on open.
    b.chapters = 103;
    assert(tomato_shelf_save(&b, 1) == ESP_OK);
    b = load_one();
    assert(tomato_book_fresh(&b) == 3 && tomato_book_pending(&b) == 0);
    assert(tomato_shelf_mark_seen("a.txt") == ESP_OK);
    b = load_one();
    assert(b.seen == 103 && tomato_book_fresh(&b) == 0);
    assert(tomato_shelf_mark_seen("other.txt") == ESP_OK && load_one().seen == 103);

    // 后台任务的旧副本不能冲掉“已看”。/ A stale worker copy cannot undo seen.
    tomato_book_t start = make(110, 110, 103);
    assert(tomato_shelf_save(&start, 1) == ESP_OK);
    tomato_book_t stale = load_one();
    assert(tomato_shelf_mark_seen("a.txt") == ESP_OK && load_one().seen == 110);
    stale.remote = 115;
    assert(tomato_shelf_save(&stale, 1) == ESP_OK);
    b = load_one();
    assert(b.seen == 110 && b.remote == 115 && tomato_book_fresh(&b) == 0 && tomato_book_pending(&b) == 5);

    assert(s_io_buf.depth == 0);
    puts("tomato store host test passed");
    return 0;
}
