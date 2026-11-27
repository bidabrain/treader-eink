/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：缓存包的实现，接口和约定见 cache_pack.h。
 * 段文件：16 字节段头，之后是一条接一条的记录（24 字节记录头 + key + 数据），封口时在末尾写全段索引和 16 字节段尾。
 * 同一个池同一时刻只往最新的一个段追加；按段号先后就是写入先后，后写的覆盖先写的。内存里只放 key 的哈希到
 * “段号、偏移、总长”的表，读的时候再核对 key 和校验。
 *
 * English: The cache pack implementation; see cache_pack.h for the interface and rules.
 * A segment file is a 16-byte header, then records one after another (a 24-byte record header + key + data); sealing
 * appends the whole segment's index and a 16-byte trailer. A pool appends only to its newest segment at any time, so
 * segment order is write order and later records replace earlier ones. Memory holds only a table from key hash to
 * "segment, offset, total length"; reads check the key and checksums.
 *
 * 冻结：段文件按小端写；记录读不出来就当没有，不修；回收只删最旧的段，删之前先把要留的搬走。
 * Frozen: segment files are little-endian; an unreadable record counts as absent and is not repaired; collection only
 * deletes the oldest segment, after moving out what stays.
 */
#include "cache_pack.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#define TAG "cache_pack"
#define LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#define LOGW(...) ESP_LOGW(TAG, __VA_ARGS__)
typedef SemaphoreHandle_t lock_t;
static bool lock_make(lock_t* l) { return (*l = xSemaphoreCreateMutex()) != NULL; }
static void lock_take(lock_t* l) { xSemaphoreTake(*l, portMAX_DELAY); }
static bool lock_try(lock_t* l) { return xSemaphoreTake(*l, 0) == pdTRUE; }
static void lock_give(lock_t* l) { xSemaphoreGive(*l); }
static void* mem_alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
static void* mem_realloc(void* p, size_t n) { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
static void mem_free(void* p) { heap_caps_free(p); }
static uint32_t crc32_le(uint32_t crc, const void* buf, size_t len) {
    return esp_rom_crc32_le(crc, (const uint8_t*)buf, (uint32_t)len);
}
static void yield_now(void) { vTaskDelay(1); }
// 清回收站时每删一个歇一下，少和界面抢卡。/ Pause after each trash delete so the UI gets the card more often.
static void trash_pause(void) { vTaskDelay(pdMS_TO_TICKS(10)); }
#else
#include <pthread.h>
#define LOGI(...) do { if (getenv("CPACK_VERBOSE")) { printf(__VA_ARGS__); printf("\n"); } } while (0)
#define LOGW(...) do { if (getenv("CPACK_VERBOSE")) { printf("W: " __VA_ARGS__); printf("\n"); } } while (0)
typedef pthread_mutex_t lock_t;
static bool lock_make(lock_t* l) { return pthread_mutex_init(l, NULL) == 0; }
static void lock_take(lock_t* l) { pthread_mutex_lock(l); }
static bool lock_try(lock_t* l) { return pthread_mutex_trylock(l) == 0; }
static void lock_give(lock_t* l) { pthread_mutex_unlock(l); }
static void* mem_alloc(size_t n) { return malloc(n); }
static void* mem_realloc(void* p, size_t n) { return realloc(p, n); }
static void mem_free(void* p) { free(p); }
static uint32_t crc32_le(uint32_t crc, const void* buf, size_t len) {
    static uint32_t table[256];
    if (!table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
            table[i] = c;
        }
    }
    const uint8_t* p = buf;
    crc = ~crc;
    while (len--) crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
static void yield_now(void) { }
static void trash_pause(void) { }
#endif

/* ---- 格式 / Format ---- */

#define SEG_MAGIC 0x314B5054u      // "TPK1"
#define REC_MAGIC 0x31525054u      // "TPR1"
#define FOOT_MAGIC 0x31465054u     // "TPF1"
#define SEG_VERSION 1u
#define SEG_HEAD 16u
#define REC_HEAD 24u
#define FOOT_ENTRY 20u
#define FOOT_TAIL 16u
#define TYPE_PUT 1u
#define TYPE_DEL 2u
// 段文件写到这么大就封口另起一个。/ A segment is sealed and a new one started once it reaches this size.
#define SEG_MAX_DEFAULT (8u * 1024u * 1024u)
#define SEGS_MAX 96
// 段号上限：用到这么大（几千年也写不到）就不再认，免得回绕到 0。/ Segment id limit: never reached in practice, and ids
// at or above it are ignored so they cannot wrap to 0.
#define SEG_ID_LIMIT 0xFFFFFF00u
// 段数到这么多也算超限（上限很大、段很小时防止段表满）。/ This many segments counts as over the cap too (keeps the
// segment table from filling when the cap is large and segments small).
#define SEGS_SOFT (SEGS_MAX - 8)
#define TABLE_MIN 512u
#define TABLE_MAX (1u << 17)
// 卡上剩余空间低于这个就压低上限，让出空间。/ Below this much free card space the cap shrinks to give space back.
#define FREE_LOW (48ull * 1024 * 1024)
// 一次清回收站最多删这么多个文件就让一让。/ Emptying the trash yields after this many files.
#define TRASH_BATCH 32
#define PATH_MAX_LEN 200

typedef struct {
    uint32_t id;
    uint32_t size;      // 文件长度（字节）/ File length in bytes
    uint32_t live;      // 被索引指着的记录总长 / Bytes of records the index points at
    bool sealed;        // 不再追加 / No more appends
} seg_t;

typedef struct {
    uint64_t hash;      // 0 表示空槽 / 0 marks an empty slot
    uint32_t seg;
    uint32_t off;
    uint32_t total;     // 记录头 + key + 数据 / Header + key + data
    uint8_t klen;
} entry_t;

typedef struct {
    uint32_t off;
    uint32_t total;
    uint64_t hash;
    uint8_t type;
    uint8_t klen;
} foot_t;

typedef struct {
    cpack_pool_cfg_t cfg;
    size_t root_len;
    lock_t lock;
    bool loaded;
    bool gc_busy;
    uint32_t gen;
    seg_t* segs;        // 按段号从小到大 / Ascending segment id
    int nsegs;
    entry_t* tab;
    uint32_t tabcap, count;
    foot_t* foot;       // 正在写的段的记录表，封口时写进段尾 / The open segment's records, written out when sealed
    uint32_t nfoot, footcap;
    uint64_t bytes;
    uint64_t cap_eff;
    uint32_t next_id;
    int thr;            // 回收时留下记录的门槛 / Keep threshold while collecting
    uint64_t moved;     // 这次超限以来搬走的字节 / Bytes moved since the pool last went over the cap
    bool collecting;    // 正在回收（超限开始，到 85% 以下停）/ Collecting (starts over the cap, stops below 85%)
    uint32_t stuck_id;  // 连着回收不了的段和次数 / The segment that keeps failing to collect, and how often
    int stuck_n;
    uint32_t seg_max;   // 这个池的段大小：至少上限的 1/32，段数才有限 / This pool's segment size: at least 1/32 of the
                        // cap so the segment count stays bounded
} pool_t;

static pool_t s_pools[CPACK_POOLS_MAX];
static int s_npools;
static char s_root[96];
static char s_trash[96];
static bool s_inited;
static volatile bool s_suspended;
// 后台碰卡（回收、清回收站、问剩余空间）时拿着，暂停时等它做完手上这一步。/ Held while the background touches the
// card (collection, trash emptying, free-space queries); pausing waits for the step in hand.
static lock_t s_bg_lock;
static uint32_t (*s_gen_fn)(void);
static uint64_t (*s_free_fn)(void);
static uint32_t s_seg_max = SEG_MAX_DEFAULT;
#ifdef CPACK_HOST_TEST
static uint64_t s_written;   // 测试量写放大用 / For measuring write amplification in tests
#define COUNT_WRITE(n) (s_written += (n))
#else
#define COUNT_WRITE(n) ((void)0)
#endif

static void wake_service(void);

/* ---- 小工具 / Helpers ---- */

static void put_u32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get_u32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put_u64(uint8_t* p, uint64_t v) { put_u32(p, (uint32_t)v); put_u32(p + 4, (uint32_t)(v >> 32)); }
static uint64_t get_u64(const uint8_t* p) { return (uint64_t)get_u32(p) | (uint64_t)get_u32(p + 4) << 32; }

static uint64_t key_hash(const char* key, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= (uint8_t)key[i]; h *= 1099511628211ull; }
    return h ? h : 1;
}

static uint32_t generation(void) { return s_gen_fn ? s_gen_fn() : 0; }

static void seg_path(const pool_t* p, uint32_t id, char* out, size_t cap) {
    snprintf(out, cap, "%s/%s/%08X.SEG", s_root, p->cfg.name, (unsigned)id);
}

static void mkdir_one(const char* path) {
    if (mkdir(path, 0775) != 0 && errno != EEXIST) LOGW("mkdir %s: %d", path, errno);
}

// 逐级建目录（/sdcard 本身不建）。/ Create each level (not /sdcard itself).
static void mkdir_all(const char* path) {
    char buf[PATH_MAX_LEN];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char* s = buf + 1; *s; s++) {
        if (*s != '/') continue;
        *s = 0;
        if (strchr(buf + 1, '/')) mkdir_one(buf);
        *s = '/';
    }
    mkdir_one(buf);
}

static bool file_size(FILE* f, uint32_t* out) {
    if (fseek(f, 0, SEEK_END) != 0) return false;
    const long n = ftell(f);
    if (n < 0 || (unsigned long)n > 0x7FFFFFFFul) return false;
    *out = (uint32_t)n;
    return true;
}

static bool read_at(FILE* f, uint32_t off, void* buf, size_t n) {
    return fseek(f, (long)off, SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
}

/* ---- 池查找 / Pool lookup ---- */

static pool_t* pool_of(const char* path, const char** key) {
    if (!path || !s_inited) return NULL;
    for (int i = 0; i < s_npools; i++) {
        pool_t* p = &s_pools[i];
        for (int k = 0; k < CPACK_PREFIXES_MAX && p->cfg.prefixes[k]; k++) {
            const size_t n = strlen(p->cfg.prefixes[k]);
            if (strncmp(path, p->cfg.prefixes[k], n) == 0 && path[n]) {
                if (key) *key = path + p->root_len;
                return p;
            }
        }
    }
    return NULL;
}

/* ---- 索引表 / Index table ---- */

static seg_t* seg_find(pool_t* p, uint32_t id) {
    int lo = 0, hi = p->nsegs - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (p->segs[mid].id == id) return &p->segs[mid];
        if (p->segs[mid].id < id) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

static int64_t tab_find(const pool_t* p, uint64_t h) {
    if (!p->tab) return -1;
    const uint32_t mask = p->tabcap - 1;
    for (uint32_t i = (uint32_t)h & mask, n = 0; n < p->tabcap; i = (i + 1) & mask, n++) {
        if (p->tab[i].hash == h) return i;
        if (!p->tab[i].hash) return -1;
    }
    return -1;
}

static void tab_place(entry_t* tab, uint32_t cap, const entry_t* e) {
    const uint32_t mask = cap - 1;
    uint32_t i = (uint32_t)e->hash & mask;
    while (tab[i].hash) i = (i + 1) & mask;
    tab[i] = *e;
}

static bool tab_grow(pool_t* p) {
    if ((uint64_t)(p->count + 1) * 4 <= (uint64_t)p->tabcap * 3) return true;
    const uint32_t cap = p->tabcap ? p->tabcap * 2 : TABLE_MIN;
    if (cap > TABLE_MAX) return false;
    entry_t* tab = mem_alloc((size_t)cap * sizeof(entry_t));
    if (!tab) return false;
    memset(tab, 0, (size_t)cap * sizeof(entry_t));
    for (uint32_t i = 0; i < p->tabcap; i++) if (p->tab[i].hash) tab_place(tab, cap, &p->tab[i]);
    mem_free(p->tab);
    p->tab = tab;
    p->tabcap = cap;
    return true;
}

// 去掉一格，后面挤着的往前挪（线性探测的删除）。/ Remove a slot and shift followers back (linear-probing delete).
static void tab_remove_at(pool_t* p, uint32_t i) {
    seg_t* s = seg_find(p, p->tab[i].seg);
    if (s) s->live = s->live >= p->tab[i].total ? s->live - p->tab[i].total : 0;
    const uint32_t mask = p->tabcap - 1;
    uint32_t j = i;
    for (;;) {
        p->tab[i].hash = 0;
        for (;;) {
            j = (j + 1) & mask;
            if (!p->tab[j].hash) { p->count--; return; }
            const uint32_t home = (uint32_t)p->tab[j].hash & mask;
            // home 不在 (i, j] 里才能挪到 i。/ Move j into i only when its home is outside (i, j].
            if (i <= j ? (home <= i || home > j) : (home <= i && home > j)) break;
        }
        p->tab[i] = p->tab[j];
        i = j;
    }
}

// 记一条（覆盖同 key 的旧记录）。/ Record an entry, replacing an older one for the same key.
static bool tab_set(pool_t* p, uint64_t h, uint32_t seg, uint32_t off, uint32_t total, uint8_t klen) {
    const int64_t at = tab_find(p, h);
    if (at >= 0) {
        entry_t* e = &p->tab[at];
        seg_t* old = seg_find(p, e->seg);
        if (old) old->live = old->live >= e->total ? old->live - e->total : 0;
        e->seg = seg; e->off = off; e->total = total; e->klen = klen;
    } else {
        if (!tab_grow(p)) return false;
        const entry_t e = {.hash = h, .seg = seg, .off = off, .total = total, .klen = klen};
        tab_place(p->tab, p->tabcap, &e);
        p->count++;
    }
    seg_t* s = seg_find(p, seg);
    if (s) s->live += total;
    return true;
}

static void tab_del(pool_t* p, uint64_t h) {
    const int64_t at = tab_find(p, h);
    if (at >= 0) tab_remove_at(p, (uint32_t)at);
}

/* ---- 记录头 / Record header ---- */

typedef struct {
    uint8_t type, klen;
    uint32_t len, data_crc;
} rec_t;

static void rec_encode(uint8_t* h, uint8_t type, const char* key, uint8_t klen, uint32_t len, uint32_t data_crc) {
    memset(h, 0, REC_HEAD);
    put_u32(h, REC_MAGIC);
    h[4] = type;
    h[5] = klen;
    put_u32(h + 8, len);
    put_u32(h + 12, data_crc);
    put_u32(h + 20, crc32_le(crc32_le(0, h, 20), key, klen));
}

// 读 off 处的记录头和 key（key 至少 CPACK_KEY_MAX+1 字节，补 0）；limit 是这条不能越过的位置。
// Read the record header and key at off (key holds at least CPACK_KEY_MAX+1 bytes, NUL-terminated); limit is where
// the record must end by.
static bool rec_read(FILE* f, uint32_t off, uint32_t limit, rec_t* r, char* key) {
    uint8_t h[REC_HEAD];
    if (off > limit || limit - off < REC_HEAD || !read_at(f, off, h, REC_HEAD)) return false;
    r->type = h[4];
    r->klen = h[5];
    r->len = get_u32(h + 8);
    r->data_crc = get_u32(h + 12);
    if (get_u32(h) != REC_MAGIC || (r->type != TYPE_PUT && r->type != TYPE_DEL) || !r->klen ||
        r->klen > CPACK_KEY_MAX || r->len > CPACK_RECORD_MAX || (r->type == TYPE_DEL && r->len) ||
        (uint64_t)off + REC_HEAD + r->klen + r->len > limit)
        return false;
    if (fread(key, 1, r->klen, f) != r->klen) return false;
    key[r->klen] = 0;
    return get_u32(h + 20) == crc32_le(crc32_le(0, h, 20), key, r->klen);
}

/* ---- 段 / Segments ---- */

static bool foot_reserve(pool_t* p, uint32_t n) {
    if (n <= p->footcap) return true;
    const uint32_t cap = p->footcap ? p->footcap * 2 : 64;
    foot_t* f = mem_realloc(p->foot, (size_t)(cap < n ? n : cap) * sizeof(foot_t));
    if (!f) return false;
    p->foot = f;
    p->footcap = cap < n ? n : cap;
    return true;
}

static seg_t* active_seg(pool_t* p) {
    return p->nsegs && !p->segs[p->nsegs - 1].sealed ? &p->segs[p->nsegs - 1] : NULL;
}

static bool segs_reserve(pool_t* p) {
    if (p->nsegs >= SEGS_MAX) return false;
    if (!p->segs) {
        p->segs = mem_alloc(SEGS_MAX * sizeof(seg_t));
        if (!p->segs) return false;
    }
    return true;
}

// 给正在写的段封口：写全段索引和段尾。失败也不再往这段追加，重建时会逐条扫描。
// Seal the open segment: write its index and trailer. On failure it still takes no more appends and a rebuild scans
// it record by record.
static void seal_active(pool_t* p) {
    seg_t* s = active_seg(p);
    if (!s) return;
    s->sealed = true;
    const size_t n = (size_t)p->nfoot * FOOT_ENTRY + FOOT_TAIL;
    uint8_t* buf = mem_alloc(n);
    char path[PATH_MAX_LEN];
    seg_path(p, s->id, path, sizeof(path));
    FILE* f = buf ? fopen(path, "r+b") : NULL;
    bool ok = f != NULL;
    if (ok) {
        for (uint32_t i = 0; i < p->nfoot; i++) {
            uint8_t* e = buf + (size_t)i * FOOT_ENTRY;
            memset(e, 0, FOOT_ENTRY);
            put_u32(e, p->foot[i].off);
            put_u32(e + 4, p->foot[i].total);
            put_u64(e + 8, p->foot[i].hash);
            e[16] = p->foot[i].type;
            e[17] = p->foot[i].klen;
        }
        uint8_t* t = buf + (size_t)p->nfoot * FOOT_ENTRY;
        put_u32(t, FOOT_MAGIC);
        put_u32(t + 4, p->nfoot);
        put_u32(t + 8, s->size);
        put_u32(t + 12, crc32_le(crc32_le(0, buf, (size_t)p->nfoot * FOOT_ENTRY), t, 12));
        ok = fseek(f, (long)s->size, SEEK_SET) == 0 && fwrite(buf, 1, n, f) == n;
        ok = fclose(f) == 0 && ok;
    }
    mem_free(buf);
    if (ok) { s->size += (uint32_t)n; p->bytes += n; }
    else LOGW("%s: seal %08X failed", p->cfg.name, (unsigned)s->id);
    p->nfoot = 0;
}

static seg_t* new_segment(pool_t* p) {
    if (!segs_reserve(p)) return NULL;
    const uint32_t id = p->next_id;
    char path[PATH_MAX_LEN];
    seg_path(p, id, path, sizeof(path));
    uint8_t h[SEG_HEAD];
    put_u32(h, SEG_MAGIC);
    put_u32(h + 4, SEG_VERSION);
    put_u32(h + 8, id);
    put_u32(h + 12, crc32_le(0, h, 12));
    FILE* f = fopen(path, "wb");
    bool ok = f && fwrite(h, 1, SEG_HEAD, f) == SEG_HEAD;
    if (f) ok = fclose(f) == 0 && ok;
    if (!ok) { remove(path); return NULL; }
    p->next_id = id + 1;
    seg_t* s = &p->segs[p->nsegs++];
    *s = (seg_t){.id = id, .size = SEG_HEAD};
    p->bytes += SEG_HEAD;
    p->nfoot = 0;
    return s;
}

// 追加一条记录，成功时 *seg_id / *off 是它的位置。/ Append one record; on success *seg_id / *off give its place.
static void unload(pool_t* p);

// 数据可以分两段给（a 在前 b 在后），合成一条记录。/ The data may come in two parts (a then b), joined into one record.
static esp_err_t append(pool_t* p, uint8_t type, const char* key, size_t klen, const void* a, size_t alen,
                        const void* b, size_t blen, uint32_t* seg_id, uint32_t* off) {
    const size_t len = alen + blen;
    const uint32_t total = REC_HEAD + (uint32_t)klen + (uint32_t)len;
    seg_t* s = active_seg(p);
    if (s && s->size > SEG_HEAD && (uint64_t)s->size + total > p->seg_max) { seal_active(p); s = NULL; }
    if (!s && !(s = new_segment(p))) return ESP_FAIL;
    if (!foot_reserve(p, p->nfoot + 1)) return ESP_ERR_NO_MEM;
    uint8_t h[REC_HEAD];
    uint32_t crc = alen ? crc32_le(0, a, alen) : 0;
    if (blen) crc = crc32_le(crc, b, blen);
    rec_encode(h, type, key, (uint8_t)klen, (uint32_t)len, crc);
    char path[PATH_MAX_LEN];
    seg_path(p, s->id, path, sizeof(path));
    FILE* f = fopen(path, "r+b");
    if (!f && errno == ENOENT) {
        // 段文件被外面删了（电脑上删了缓存目录）：内存里的索引全不可信，放掉，下次从卡上重读。/ The segment was
        // deleted from outside (the cache folder removed on a computer): the in-memory index is stale, so drop it and
        // read the card again next time.
        LOGW("%s: segment %08X vanished, reloading", p->cfg.name, (unsigned)s->id);
        unload(p);
        return ESP_FAIL;
    }
    bool ok = f && fseek(f, (long)s->size, SEEK_SET) == 0 && fwrite(h, 1, REC_HEAD, f) == REC_HEAD &&
              fwrite(key, 1, klen, f) == klen && (!alen || fwrite(a, 1, alen, f) == alen) &&
              (!blen || fwrite(b, 1, blen, f) == blen);
    if (f) ok = fclose(f) == 0 && ok;
    if (!ok) {
        // 写了一半：截回原长，下一条从同一处写。/ Half written: truncate back so the next record starts at the same
        // place.
        if ((f = fopen(path, "r+b"))) { (void)ftruncate(fileno(f), (off_t)s->size); fclose(f); }
        return ESP_FAIL;
    }
    const uint64_t h64 = key_hash(key, klen);
    p->foot[p->nfoot++] = (foot_t){.off = s->size, .total = total, .hash = h64, .type = type, .klen = (uint8_t)klen};
    *seg_id = s->id;
    *off = s->size;
    s->size += total;
    p->bytes += total;
    COUNT_WRITE(total);
    return ESP_OK;
}

// 回收时从旧段 src（读位置已在这条的数据开头）按块搬一条记录过来，省得为大记录要一整块内存。数据校验不对就撤回，
// 返回 ESP_ERR_INVALID_CRC。/ During collection, move one record from the old segment src (positioned at the record's
// data) in chunks, so a large record needs no single big buffer. A failed data checksum undoes the copy and returns
// ESP_ERR_INVALID_CRC.
static esp_err_t append_copy(pool_t* p, const char* key, size_t klen, FILE* src, uint32_t len, uint32_t crc,
                             uint8_t* chunk, size_t chunk_cap, uint32_t* seg_id, uint32_t* off) {
    const uint32_t total = REC_HEAD + (uint32_t)klen + len;
    seg_t* s = active_seg(p);
    if (s && s->size > SEG_HEAD && (uint64_t)s->size + total > p->seg_max) { seal_active(p); s = NULL; }
    if (!s && !(s = new_segment(p))) return ESP_FAIL;
    if (!foot_reserve(p, p->nfoot + 1)) return ESP_ERR_NO_MEM;
    uint8_t h[REC_HEAD];
    rec_encode(h, TYPE_PUT, key, (uint8_t)klen, len, crc);
    char path[PATH_MAX_LEN];
    seg_path(p, s->id, path, sizeof(path));
    FILE* f = fopen(path, "r+b");
    if (!f && errno == ENOENT) { unload(p); return ESP_FAIL; }
    bool ok = f && fseek(f, (long)s->size, SEEK_SET) == 0 && fwrite(h, 1, REC_HEAD, f) == REC_HEAD &&
              fwrite(key, 1, klen, f) == klen;
    bool bad_data = false;
    uint32_t got_crc = 0;
    for (uint32_t done = 0; ok && done < len;) {
        const size_t n = len - done < chunk_cap ? len - done : chunk_cap;
        ok = fread(chunk, 1, n, src) == n;
        if (!ok) { bad_data = true; break; }
        got_crc = crc32_le(got_crc, chunk, n);
        ok = fwrite(chunk, 1, n, f) == n;
        done += (uint32_t)n;
    }
    if (ok && got_crc != crc) { ok = false; bad_data = true; }
    if (f) ok = fclose(f) == 0 && ok;
    if (!ok) {
        if ((f = fopen(path, "r+b"))) { (void)ftruncate(fileno(f), (off_t)s->size); fclose(f); }
        return bad_data ? ESP_ERR_INVALID_CRC : ESP_FAIL;
    }
    p->foot[p->nfoot++] = (foot_t){.off = s->size, .total = total, .hash = key_hash(key, klen), .type = TYPE_PUT,
                                   .klen = (uint8_t)klen};
    *seg_id = s->id;
    *off = s->size;
    s->size += total;
    p->bytes += total;
    COUNT_WRITE(total);
    return ESP_OK;
}

/* ---- 载入 / Loading ---- */

static void unload(pool_t* p) {
    mem_free(p->tab);
    mem_free(p->segs);
    mem_free(p->foot);
    p->tab = NULL; p->tabcap = p->count = 0;
    p->segs = NULL; p->nsegs = 0;
    p->foot = NULL; p->nfoot = p->footcap = 0;
    p->bytes = 0;
    p->loaded = false;
}

// 套一条段索引或扫描出来的记录。/ Apply one record from a footer or a scan.
static bool apply(pool_t* p, uint32_t seg, const foot_t* r) {
    if (r->type == TYPE_DEL) { tab_del(p, r->hash); return true; }
    return tab_set(p, r->hash, seg, r->off, r->total, r->klen);
}

// 读段尾索引；格式不对返回 false，由调用方逐条扫描。/ Read the footer index; false when malformed, and the caller scans.
static bool load_footer(pool_t* p, FILE* f, seg_t* s) {
    uint8_t t[FOOT_TAIL];
    if (s->size < SEG_HEAD + FOOT_TAIL || !read_at(f, s->size - FOOT_TAIL, t, FOOT_TAIL) || get_u32(t) != FOOT_MAGIC)
        return false;
    const uint32_t count = get_u32(t + 4), at = get_u32(t + 8);
    if (at < SEG_HEAD || at > s->size || count > (at - SEG_HEAD) / REC_HEAD ||
        (uint64_t)at + (uint64_t)count * FOOT_ENTRY + FOOT_TAIL != s->size)
        return false;
    const size_t n = (size_t)count * FOOT_ENTRY;
    uint8_t* buf = n ? mem_alloc(n) : NULL;
    if (n && (!buf || !read_at(f, at, buf, n))) { mem_free(buf); return false; }
    bool ok = get_u32(t + 12) == crc32_le(crc32_le(0, buf, n), t, 12);
    // 先整体核对，再套用，坏的索引不能留下一半。/ Check it all before applying, so a bad footer leaves nothing half done.
    uint32_t next = SEG_HEAD;
    for (uint32_t i = 0; ok && i < count; i++) {
        const uint8_t* e = buf + (size_t)i * FOOT_ENTRY;
        const uint32_t off = get_u32(e), total = get_u32(e + 4);
        const uint8_t type = e[16], klen = e[17];
        ok = off >= next && total >= REC_HEAD + (uint32_t)klen && (uint64_t)off + total <= at && klen &&
             klen <= CPACK_KEY_MAX && (type == TYPE_PUT || (type == TYPE_DEL && total == REC_HEAD + klen));
        next = off + total;
    }
    for (uint32_t i = 0; ok && i < count; i++) {
        const uint8_t* e = buf + (size_t)i * FOOT_ENTRY;
        const foot_t r = {.off = get_u32(e), .total = get_u32(e + 4), .hash = get_u64(e + 8), .type = e[16], .klen = e[17]};
        ok = apply(p, s->id, &r);
    }
    mem_free(buf);
    return ok;
}

// 逐条扫描，返回最后一条完好记录的结尾。keep_foot 时把记录表留给正在写的段。/ Scan record by record and return the
// end of the last intact record; with keep_foot the record list is kept for the open segment.
static bool scan(pool_t* p, FILE* f, seg_t* s, bool keep_foot, uint32_t* end) {
    char key[CPACK_KEY_MAX + 1];
    uint32_t off = SEG_HEAD;
    rec_t r;
    while (rec_read(f, off, s->size, &r, key)) {
        const uint32_t total = REC_HEAD + r.klen + r.len;
        const foot_t e = {.off = off, .total = total, .hash = key_hash(key, r.klen), .type = r.type, .klen = r.klen};
        if (!apply(p, s->id, &e)) return false;
        if (keep_foot) {
            if (!foot_reserve(p, p->nfoot + 1)) return false;
            p->foot[p->nfoot++] = e;
        }
        off += total;
    }
    *end = off;
    return true;
}

static int cmp_u32(const void* a, const void* b) {
    const uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return x < y ? -1 : x > y;
}

static void trash_move(const char* dir, const char* tag);

static bool load(pool_t* p, uint32_t gen) {
    unload(p);
    p->gen = gen;
    if (p->cfg.old_dirs) {
        for (int i = 0; p->cfg.old_dirs[i]; i++) {
            struct stat st;
            if (stat(p->cfg.old_dirs[i], &st) == 0 && S_ISDIR(st.st_mode)) trash_move(p->cfg.old_dirs[i], p->cfg.name);
        }
    }
    char dir[PATH_MAX_LEN];
    snprintf(dir, sizeof(dir), "%s/%s", s_root, p->cfg.name);
    mkdir_all(dir);
    uint32_t ids[SEGS_MAX];
    int n = 0;
    DIR* d = opendir(dir);
    if (!d) return false;
    struct dirent* e;
    while ((e = readdir(d))) {
        const char* name = e->d_name;
        // 名字必须正好是 8 位十六进制加 .SEG。/ The name must be exactly 8 hex digits plus .SEG.
        uint32_t id = 0;
        bool hex = strlen(name) == 12 && strcasecmp(name + 8, ".seg") == 0;
        for (int k = 0; hex && k < 8; k++) {
            const char c = name[k];
            const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (v < 0) hex = false;
            else id = id << 4 | (uint32_t)v;
        }
        if (!hex) continue;
        if (n < SEGS_MAX && id && id < SEG_ID_LIMIT) ids[n++] = id;
        else {
            // 段太多（不该发生）或段号不对：直接删（只在包自己的目录里）。/ Too many segments (should not happen) or a bad
            // id: delete it (only inside the pack's own folder).
            char path[PATH_MAX_LEN + 40];
            snprintf(path, sizeof(path), "%s/%.32s", dir, name);
            remove(path);
        }
    }
    closedir(d);
    qsort(ids, (size_t)n, sizeof(ids[0]), cmp_u32);
    if (!segs_reserve(p)) return false;
    // 下一个段号只从校验过的段里算，乱放进来的文件名不能把它带偏。/ The next segment id comes only from validated
    // segments, so a stray file name cannot throw it off.
    p->next_id = 1;
    for (int i = 0; i < n; i++) {
        char path[PATH_MAX_LEN];
        seg_path(p, ids[i], path, sizeof(path));
        FILE* f = fopen(path, "rb");
        uint8_t h[SEG_HEAD];
        uint32_t size = 0;
        const bool head_ok = f && file_size(f, &size) && read_at(f, 0, h, SEG_HEAD) && get_u32(h) == SEG_MAGIC &&
                             get_u32(h + 4) == SEG_VERSION && get_u32(h + 8) == ids[i] &&
                             get_u32(h + 12) == crc32_le(0, h, 12);
        if (!head_ok) {
            if (f) fclose(f);
            // 坏的或旧版本的段：整段丢掉。/ A broken or older-version segment is dropped whole.
            LOGW("%s: drop bad segment %08X", p->cfg.name, (unsigned)ids[i]);
            remove(path);
            continue;
        }
        seg_t* s = &p->segs[p->nsegs++];
        *s = (seg_t){.id = ids[i], .size = size, .sealed = true};
        p->next_id = ids[i] + 1;
        const bool last = i == n - 1;
        bool ok = load_footer(p, f, s);
        uint32_t end = size;
        if (!ok) {
            p->nfoot = 0;
            ok = scan(p, f, s, last, &end);
            if (ok && last) s->sealed = false;
        }
        fclose(f);
        if (!ok) { unload(p); return false; }
        if (!s->sealed && end < size) {
            // 掉电留下的半条：截掉再接着写。/ A half record left by a power cut: truncate it and carry on.
            FILE* w = fopen(path, "r+b");
            const bool cut = w && ftruncate(fileno(w), (off_t)end) == 0;
            if (w) fclose(w);
            if (cut) s->size = end;
            else s->sealed = true;
        }
        p->bytes += s->size;
    }
    p->loaded = true;
    if (!p->cap_eff) p->cap_eff = p->cfg.cap_bytes;
    LOGI("%s: %d segments, %u records, %.1f MB", p->cfg.name, p->nsegs, (unsigned)p->count, p->bytes / 1048576.0);
    if (p->bytes > p->cap_eff) wake_service();
    return true;
}

// 拿着锁调用：池能用（必要时重新载入）。/ Called with the lock held: the pool is usable (reloaded when needed).
static bool ready(pool_t* p) {
    if (s_suspended) return false;
    const uint32_t gen = generation();
    if (p->loaded && p->gen == gen) return true;
    return load(p, gen);
}

/* ---- 读写 / Access ---- */

static bool key_ok(const char* key) {
    const size_t n = strlen(key);
    return n > 0 && n <= CPACK_KEY_MAX;
}

bool cpack_owns(const char* path) { return pool_of(path, NULL) != NULL; }

esp_err_t cpack_put(const char* path, const void* data, size_t len) { return cpack_put2(path, data, len, NULL, 0); }

esp_err_t cpack_put2(const char* path, const void* a, size_t alen, const void* b, size_t blen) {
    const char* key;
    pool_t* p = pool_of(path, &key);
    if (!p || !key_ok(key) || (alen && !a) || (blen && !b) || alen > CPACK_RECORD_MAX || blen > CPACK_RECORD_MAX)
        return ESP_ERR_INVALID_ARG;
    const size_t len = alen + blen;
    const size_t klen = strlen(key);
    if (len > CPACK_RECORD_MAX || (uint64_t)REC_HEAD + klen + len + SEG_HEAD > p->seg_max) return ESP_ERR_INVALID_SIZE;
    lock_take(&p->lock);
    esp_err_t err = ESP_FAIL;
    bool over = false;
    if (ready(p)) {
        if (p->bytes > p->cap_eff + 2ull * p->seg_max) {
            err = ESP_ERR_NO_MEM;
        } else {
            uint32_t seg, off;
            err = append(p, TYPE_PUT, key, klen, a, alen, b, blen, &seg, &off);
            if (err == ESP_OK && !tab_set(p, key_hash(key, klen), seg, off, REC_HEAD + (uint32_t)(klen + len), (uint8_t)klen))
                err = ESP_ERR_NO_MEM;
        }
        over = p->bytes > p->cap_eff || p->nsegs > SEGS_SOFT;
    }
    lock_give(&p->lock);
    if (over) wake_service();
    return err;
}

// 拿着锁：打开 e 指的记录，核对 key，返回文件和记录头。/ With the lock: open the record e points at, check the key,
// return the file and header.
static FILE* open_record(pool_t* p, const entry_t* e, const char* key, rec_t* r, bool* corrupt) {
    *corrupt = false;
    const seg_t* s = seg_find(p, e->seg);
    if (!s) { *corrupt = true; return NULL; }
    char path[PATH_MAX_LEN];
    seg_path(p, e->seg, path, sizeof(path));
    FILE* f = fopen(path, "rb");
    // 打不开（段刚被回收删掉，或卡出了问题）也去掉这条：留着会一直“有却读不出”，图就不会重下。
    // Unopenable (the segment was just collected, or the card failed) drops the entry too: kept, it would stay
    // "present but unreadable" and the image would never be fetched again.
    // 只是句柄暂时用完就不算，下次再读。/ Merely running out of handles does not count; read again next time.
    if (!f) { *corrupt = errno != EMFILE && errno != ENFILE; return NULL; }
    char got[CPACK_KEY_MAX + 1];
    if (!rec_read(f, e->off, s->size, r, got) || r->type != TYPE_PUT || REC_HEAD + r->klen + r->len != e->total) {
        fclose(f);
        *corrupt = true;
        return NULL;
    }
    // key 不同是哈希碰撞，不算坏，也不删别人的记录。/ A different key is a hash collision: not corruption, and the other
    // key's record stays.
    if (strcmp(got, key) != 0) { fclose(f); return NULL; }
    return f;
}

esp_err_t cpack_get(const char* path, size_t max, uint8_t** out, size_t* len) {
    if (!out || !len) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    *len = 0;
    const char* key;
    pool_t* p = pool_of(path, &key);
    if (!p || !key_ok(key)) return ESP_ERR_INVALID_ARG;
    const size_t klen = strlen(key);
    lock_take(&p->lock);
    esp_err_t err = ESP_ERR_NOT_FOUND;
    const int64_t at = ready(p) ? tab_find(p, key_hash(key, klen)) : -1;
    if (at >= 0) {
        const entry_t e = p->tab[at];
        rec_t r;
        bool corrupt;
        FILE* f = open_record(p, &e, key, &r, &corrupt);
        if (f) {
            uint8_t* buf = r.len > max ? NULL : mem_alloc((size_t)r.len + 1);
            if (r.len > max) err = ESP_ERR_INVALID_SIZE;
            else if (!buf) err = ESP_ERR_NO_MEM;
            else if (fread(buf, 1, r.len, f) != r.len) err = ESP_FAIL;
            else if ((r.len ? crc32_le(0, buf, r.len) : 0) != r.data_crc) corrupt = true;
            else { buf[r.len] = 0; *out = buf; *len = r.len; buf = NULL; err = ESP_OK; }
            mem_free(buf);
            fclose(f);
        }
        if (corrupt) {
            LOGW("%s: drop unreadable %s", p->cfg.name, key);
            tab_del(p, e.hash);
            err = ESP_ERR_NOT_FOUND;
        }
    }
    lock_give(&p->lock);
    return err;
}

esp_err_t cpack_read_head(const char* path, void* buf, size_t cap, size_t* got) {
    if (!got || (cap && !buf)) return ESP_ERR_INVALID_ARG;
    *got = 0;
    const char* key;
    pool_t* p = pool_of(path, &key);
    if (!p || !key_ok(key)) return ESP_ERR_INVALID_ARG;
    lock_take(&p->lock);
    esp_err_t err = ESP_ERR_NOT_FOUND;
    const int64_t at = ready(p) ? tab_find(p, key_hash(key, strlen(key))) : -1;
    if (at >= 0) {
        const entry_t e = p->tab[at];
        rec_t r;
        bool corrupt;
        FILE* f = open_record(p, &e, key, &r, &corrupt);
        if (f) {
            const size_t n = cap < r.len ? cap : r.len;
            err = fread(buf, 1, n, f) == n ? ESP_OK : ESP_FAIL;
            if (err == ESP_OK) *got = n;
            fclose(f);
        }
        if (corrupt) { tab_del(p, e.hash); err = ESP_ERR_NOT_FOUND; }
    }
    lock_give(&p->lock);
    return err;
}

bool cpack_size(const char* path, size_t* len) {
    const char* key;
    pool_t* p = pool_of(path, &key);
    if (!p || !key_ok(key)) return false;
    const size_t klen = strlen(key);
    lock_take(&p->lock);
    const int64_t at = ready(p) ? tab_find(p, key_hash(key, klen)) : -1;
    const bool have = at >= 0 && p->tab[at].klen == klen;
    if (have && len) *len = p->tab[at].total - REC_HEAD - klen;
    lock_give(&p->lock);
    return have;
}

esp_err_t cpack_remove(const char* path) {
    const char* key;
    pool_t* p = pool_of(path, &key);
    if (!p || !key_ok(key)) return ESP_ERR_INVALID_ARG;
    const size_t klen = strlen(key);
    const uint64_t h = key_hash(key, klen);
    lock_take(&p->lock);
    esp_err_t err = ESP_FAIL;
    if (ready(p)) {
        err = ESP_OK;
        if (tab_find(p, h) >= 0) {
            // 删除也记一条，重建时才不会把旧的找回来；记不上也照样从内存里去掉（最坏是缓存又回来）。
            // A delete is logged too so a rebuild does not bring the old record back; if logging fails it still leaves
            // memory (at worst the cache comes back).
            uint32_t seg, off;
            if (append(p, TYPE_DEL, key, klen, NULL, 0, NULL, 0, &seg, &off) != ESP_OK) err = ESP_FAIL;
            tab_del(p, h);
        }
    }
    lock_give(&p->lock);
    return err;
}

void cpack_free(void* ptr) { mem_free(ptr); }

bool cpack_stats(const char* name, cpack_stats_t* out) {
    for (int i = 0; i < s_npools; i++) {
        pool_t* p = &s_pools[i];
        if (strcmp(p->cfg.name, name)) continue;
        lock_take(&p->lock);
        memset(out, 0, sizeof(*out));
        out->loaded = p->loaded;
        out->segments = (uint32_t)p->nsegs;
        out->records = p->count;
        out->bytes = p->bytes;
        for (int k = 0; k < p->nsegs; k++) out->live_bytes += p->segs[k].live;
        out->cap_bytes = p->cap_eff;
        lock_give(&p->lock);
        return true;
    }
    return false;
}

/* ---- 回收 / Collection ---- */

// 一个段的记录表：有段尾索引读索引，没有就逐条扫描。/ A segment's record list: from the footer when present, else a scan.
static foot_t* segment_records(FILE* f, uint32_t size, uint32_t* count) {
    *count = 0;
    uint8_t t[FOOT_TAIL];
    if (size >= SEG_HEAD + FOOT_TAIL && read_at(f, size - FOOT_TAIL, t, FOOT_TAIL) && get_u32(t) == FOOT_MAGIC) {
        const uint32_t n = get_u32(t + 4), at = get_u32(t + 8);
        if (at >= SEG_HEAD && at <= size && n <= (at - SEG_HEAD) / REC_HEAD &&
            (uint64_t)at + (uint64_t)n * FOOT_ENTRY + FOOT_TAIL == size) {
            uint8_t* buf = n ? mem_alloc((size_t)n * FOOT_ENTRY) : NULL;
            foot_t* list = n ? mem_alloc((size_t)n * sizeof(foot_t)) : NULL;
            if (n && buf && list && read_at(f, at, buf, (size_t)n * FOOT_ENTRY) &&
                get_u32(t + 12) == crc32_le(crc32_le(0, buf, (size_t)n * FOOT_ENTRY), t, 12)) {
                for (uint32_t i = 0; i < n; i++) {
                    const uint8_t* e = buf + (size_t)i * FOOT_ENTRY;
                    list[i] = (foot_t){.off = get_u32(e), .total = get_u32(e + 4), .hash = get_u64(e + 8), .type = e[16],
                                       .klen = e[17]};
                }
                mem_free(buf);
                *count = n;
                return list;
            }
            mem_free(buf);
            mem_free(list);
            if (!n) return NULL;
        }
    }
    uint32_t cap = 64, n = 0, off = SEG_HEAD;
    foot_t* list = mem_alloc(cap * sizeof(foot_t));
    char key[CPACK_KEY_MAX + 1];
    rec_t r;
    while (list && rec_read(f, off, size, &r, key)) {
        if (n == cap) {
            foot_t* grown = mem_realloc(list, (size_t)cap * 2 * sizeof(foot_t));
            if (!grown) { mem_free(list); return NULL; }
            list = grown;
            cap *= 2;
        }
        const uint32_t total = REC_HEAD + r.klen + r.len;
        list[n++] = (foot_t){.off = off, .total = total, .hash = key_hash(key, r.klen), .type = r.type, .klen = r.klen};
        off += total;
    }
    *count = n;
    return list;
}

// 这一轮生效的上限：卡上快满时压低。free_bytes 为 UINT64_MAX 表示不知道。/ The cap for this round, lowered when the
// card is nearly full; free_bytes of UINT64_MAX means unknown.
static uint64_t effective_cap(const pool_t* p, uint64_t free_bytes) {
    const uint64_t cap = p->cfg.cap_bytes;
    if (free_bytes >= FREE_LOW) return cap;
    const uint64_t shrink = FREE_LOW - free_bytes, floor = 2ull * p->seg_max;
    const uint64_t lowered = p->bytes > shrink + floor ? p->bytes - shrink : floor;
    return lowered < cap ? lowered : cap;
}

// 回收一个最旧的段；返回 true 表示做了事。超过上限开始回收，一直回收到上限的 85% 以下才停（滞回），免得每存一点就
// 搬一整段。/ Collect the oldest segment; true when work was done. Collection starts above the cap and runs until
// below 85% of it (hysteresis), so a little more data does not move a whole segment every time.
static bool collect_one(pool_t* p, uint64_t free_bytes) {
    lock_take(&p->lock);
    if (!ready(p)) { lock_give(&p->lock); return false; }
    p->cap_eff = effective_cap(p, free_bytes);
    const uint64_t low = p->cap_eff / 100 * 85;
    if (!p->collecting && (p->bytes > p->cap_eff || p->nsegs > SEGS_SOFT)) p->collecting = true;
    if (p->collecting && p->bytes <= low && p->nsegs <= SEGS_SOFT) {
        p->collecting = false;
        p->moved = 0;
        p->thr = CPACK_LIVE_ROOM;
    }
    if (!p->collecting) { lock_give(&p->lock); return false; }
    if (p->nsegs && !p->segs[0].sealed) {
        // 只剩正在写的一个段：先封口，下一轮才能回收它。/ Only the open segment remains: seal it so a later round can
        // collect it.
        seal_active(p);
        lock_give(&p->lock);
        return true;
    }
    if (!p->nsegs) { p->collecting = false; lock_give(&p->lock); return false; }
    const seg_t victim = p->segs[0];
    const uint32_t gen = p->gen;
    if (p->thr < CPACK_LIVE_ROOM) p->thr = CPACK_LIVE_ROOM;
    const int thr = p->thr;
    p->gc_busy = true;
    lock_give(&p->lock);

    void* ctx = NULL;
    bool judge = true;
    if (p->cfg.gc_begin) {
        ctx = p->cfg.gc_begin(p->cfg.user);
        judge = ctx != NULL;
    }
    char path[PATH_MAX_LEN];
    seg_path(p, victim.id, path, sizeof(path));
    FILE* f = judge ? fopen(path, "rb") : NULL;
    uint32_t count = 0;
    foot_t* recs = f ? segment_records(f, victim.size, &count) : NULL;
    // 搬记录用的小块缓冲：大记录分块搬，不要一整块内存。/ A small chunk buffer: large records move in pieces, never
    // needing one big block.
    enum { CHUNK = 16 * 1024 };
    uint8_t* chunk = f ? mem_alloc(CHUNK) : NULL;
    uint64_t moved = 0;
    // 记录表建不出来（内存或读卡出错）而这段还有有效记录：这轮不删，免得把要留的一起扔掉；同一段连着三次都这样才
    // 当它坏了。/ The record list cannot be built (memory or a read error) while the segment still holds live records:
    // delete nothing this round so what stays is not thrown away; only three failures in a row on the same segment
    // count it as broken.
    bool aborted = !judge || !f || !chunk;
    if (!aborted && !recs && victim.live) {
        if (p->stuck_id != victim.id) { p->stuck_id = victim.id; p->stuck_n = 0; }
        if (++p->stuck_n < 3) aborted = true;
        else LOGW("%s: segment %08X unreadable, dropping", p->cfg.name, (unsigned)victim.id);
    }
    char key[CPACK_KEY_MAX + 1];
    for (uint32_t i = 0; !aborted && i < count; i++) {
        if (recs[i].type != TYPE_PUT) continue;
        lock_take(&p->lock);
        if (s_suspended || !p->loaded || p->gen != gen || generation() != gen) { aborted = true; lock_give(&p->lock); break; }
        const int64_t at = tab_find(p, recs[i].hash);
        if (at < 0 || p->tab[at].seg != victim.id || p->tab[at].off != recs[i].off) { lock_give(&p->lock); continue; }
        rec_t r;
        // 记录头读不出来就是坏了（读不出来一律当没有）。/ An unreadable header is broken (unreadable counts as absent).
        const bool readable = rec_read(f, recs[i].off, victim.size, &r, key) && r.type == TYPE_PUT;
        const cpack_live_t level = readable && p->cfg.live ? p->cfg.live(ctx, key) : CPACK_LIVE_DROP;
        if (readable && (int)level >= thr) {
            uint32_t seg, off;
            const esp_err_t err = append_copy(p, key, r.klen, f, r.len, r.data_crc, chunk, CHUNK, &seg, &off);
            const uint32_t total = REC_HEAD + r.klen + r.len;
            if (err == ESP_OK) {
                if (tab_set(p, recs[i].hash, seg, off, total, r.klen)) moved += total;
                else aborted = true;
            } else if (err == ESP_ERR_INVALID_CRC) {
                tab_del(p, recs[i].hash);   // 数据坏了 / The data is broken
            } else {
                aborted = true;             // 写卡出错：这轮先停 / A write error: stop this round
            }
        } else {
            tab_del(p, recs[i].hash);
        }
        lock_give(&p->lock);
        yield_now();
    }
    if (f) fclose(f);
    mem_free(recs);
    mem_free(chunk);
    if (p->cfg.gc_end && ctx) p->cfg.gc_end(ctx);

    lock_take(&p->lock);
    bool removed = false;
    if (!aborted && p->loaded && p->gen == gen && p->nsegs && p->segs[0].id == victim.id) {
        // 还指着这段的（索引不全时漏掉的）一并去掉，再从表里拿掉这段。/ Drop whatever still points at it (missed by an
        // incomplete index), then take the segment out of the table.
        for (uint32_t i = 0; i < p->tabcap;) {
            if (p->tab[i].hash && p->tab[i].seg == victim.id) tab_remove_at(p, i);
            else i++;
        }
        p->bytes -= p->segs[0].size;
        memmove(&p->segs[0], &p->segs[1], (size_t)(p->nsegs - 1) * sizeof(seg_t));
        p->nsegs--;
        p->stuck_n = 0;
        removed = true;
        // 这段留下的超过四分之三：列表里条目的大图先不留了（正文和缩略图照留），像原来的 strip_images。
        // More than three quarters of this segment stayed: listed items' full images go first (text and thumbnails
        // stay), like the old strip_images.
        if ((uint64_t)moved * 4 >= (uint64_t)victim.size * 3 && p->thr == CPACK_LIVE_ROOM) p->thr = CPACK_LIVE_KEEP;
        // 这次回收搬走的已经超过整个上限还没压下来：要留的本身就放不下，最后什么都不留。/ This collection has moved
        // more than the whole cap without getting under it: what should stay does not fit, so in the end nothing stays.
        p->moved += moved;
        if (p->moved > p->cap_eff && p->thr < CPACK_LIVE_KEEP + 1) { p->thr++; p->moved = 0; }
    }
    p->gc_busy = false;
    lock_give(&p->lock);
    if (removed && remove(path) != 0) LOGW("%s: remove %08X: %d", p->cfg.name, (unsigned)victim.id, errno);
    if (removed) LOGI("%s: collected %08X, kept %.1f of %.1f MB", p->cfg.name, (unsigned)victim.id, moved / 1048576.0,
                      victim.size / 1048576.0);
    return removed;
}

/* ---- 回收站 / Trash ---- */

static void trash_move(const char* dir, const char* tag) {
    static unsigned seq;
    mkdir_all(s_trash);
    char to[PATH_MAX_LEN];
    // 最多试 16 个名字，找不到空名字就下次再说（不在这里一个个试上千次）。/ Try at most 16 names; without a free one,
    // leave it for next time (no thousand stats here).
    for (int tries = 0; tries < 16; tries++) {
        struct stat st;
        snprintf(to, sizeof(to), "%s/%s%03u", s_trash, tag, seq++ % 1000u);
        if (stat(to, &st) == 0) continue;
        if (rename(dir, to) == 0) { LOGI("moved %s to trash", dir); wake_service(); }
        else LOGW("trash %s: %d", dir, errno);
        return;
    }
    LOGW("trash %s: no free name", dir);
}

// 删 path 下的东西，最多 *budget 个文件；删空了返回 true。/ Delete what is under path, at most *budget files; true once
// it is empty.
static bool remove_tree(char* path, size_t cap, int depth, int* budget) {
    if (depth > 6) return false;
    DIR* d = opendir(path);
    if (!d) return remove(path) == 0 || errno == ENOENT;
    const size_t base = strlen(path);
    bool empty = true;
    struct dirent* e;
    while (*budget > 0 && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (base + 1 + strlen(e->d_name) >= cap) { empty = false; continue; }
        snprintf(path + base, cap - base, "/%s", e->d_name);
        struct stat st;
        bool gone;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) gone = remove_tree(path, cap, depth + 1, budget);
        else { gone = remove(path) == 0; (*budget)--; trash_pause(); }
        if (!gone) empty = false;
        path[base] = 0;
        if (s_suspended) { empty = false; break; }
    }
    if (*budget <= 0) empty = false;
    closedir(d);
    path[base] = 0;
    return empty && rmdir(path) == 0;
}

// 清一批回收站：从第一项起，删不动的（只读文件、名字太长）跳到下一项；这一批删掉了东西返回 true。一项都删不动（卡
// 出问题）就停，等下次唤醒。/ Empty one trash batch: from the first entry on, skipping entries that will not go
// (read-only files, overlong names); true when this batch deleted something. Nothing deletable (a card problem) stops
// until the next wake-up.
static bool trash_step(void) {
    if (!s_trash[0] || s_suspended) return false;
    DIR* d = opendir(s_trash);
    if (!d) return false;
    char path[PATH_MAX_LEN + 64];
    bool progress = false;
    struct dirent* e;
    while (!progress && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || strlen(e->d_name) > 100) continue;
        snprintf(path, sizeof(path), "%s/%.100s", s_trash, e->d_name);
        int budget = TRASH_BATCH;
        const bool emptied = remove_tree(path, sizeof(path), 0, &budget);
        progress = emptied || budget < TRASH_BATCH;
    }
    closedir(d);
    return progress;
}

static void service_round(void) {
    // 剩余空间一轮只问一次：问一次可能要扫 FAT，不能在池锁里做。/ Ask for free space once per round: it may scan the
    // FAT and must not happen under a pool lock.
    lock_take(&s_bg_lock);
    const uint64_t free_bytes = !s_suspended && s_free_fn ? s_free_fn() : UINT64_MAX;
    lock_give(&s_bg_lock);
    for (int i = 0; i < s_npools; i++) {
        for (int k = 0; k < 64; k++) {
            lock_take(&s_bg_lock);
            const bool more = !s_suspended && collect_one(&s_pools[i], free_bytes);
            lock_give(&s_bg_lock);
            if (!more) break;
        }
    }
}

// 清一批回收站，拿着后台锁。/ Empty one trash batch under the background lock.
static bool trash_round(void) {
    lock_take(&s_bg_lock);
    const bool more = !s_suspended && trash_step();
    lock_give(&s_bg_lock);
    return more;
}

/* ---- 后台任务 / Background task ---- */

#ifdef ESP_PLATFORM
static TaskHandle_t s_task;

static void service_task(void* arg) {
    (void)arg;
    // 开机先等一会儿，别和开机抢卡。/ Wait a little after boot instead of competing with startup for the card.
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20000));
    // 先把各池的索引读进来（旧目录也在这时挪进回收站），第一次打开页面时就不用在画图时等。/ Load every pool's
    // index first (old folders move to the trash now too), so opening a page the first time does not wait while
    // drawing.
    for (int i = 0; i < s_npools && !s_suspended; i++) {
        lock_take(&s_bg_lock);
        lock_take(&s_pools[i].lock);
        (void)ready(&s_pools[i]);
        lock_give(&s_pools[i].lock);
        lock_give(&s_bg_lock);
    }
    for (;;) {
        if (!s_suspended) {
            service_round();
            while (!s_suspended && trash_round()) vTaskDelay(pdMS_TO_TICKS(20));
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(120000));
    }
}

static void wake_service(void) { if (s_task) xTaskNotifyGive(s_task); }
#else
static void wake_service(void) { }
#endif

void cpack_gc_kick(void) { wake_service(); }

/* ---- 初始化 / Setup ---- */

esp_err_t cpack_init(const char* root, const char* trash) {
    if (s_inited) return ESP_OK;
    if (!root || !trash || strlen(root) >= sizeof(s_root) || strlen(trash) >= sizeof(s_trash)) return ESP_ERR_INVALID_ARG;
    snprintf(s_root, sizeof(s_root), "%s", root);
    snprintf(s_trash, sizeof(s_trash), "%s", trash);
    if (!lock_make(&s_bg_lock)) return ESP_ERR_NO_MEM;
#ifdef ESP_PLATFORM
    // 优先级和主循环一样（1），按时间片轮流，不压界面。/ Same priority as the main loop (1), time-sliced, so it never
    // holds up the UI.
    if (xTaskCreatePinnedToCoreWithCaps(service_task, "cache_pack", 8192, NULL, 1, &s_task, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_task = NULL;
        LOGW("no background task; collection waits for cpack_gc_kick");
    }
#endif
    s_inited = true;
    return ESP_OK;
}

esp_err_t cpack_register(const cpack_pool_cfg_t* cfg) {
    if (!s_inited || !cfg || !cfg->name || !cfg->root || !cfg->prefixes[0] || !cfg->cap_bytes || s_npools >= CPACK_POOLS_MAX)
        return ESP_ERR_INVALID_ARG;
    const size_t name_len = strlen(cfg->name);
    if (!name_len || name_len > 8) return ESP_ERR_INVALID_ARG;
    for (const char* c = cfg->name; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9'))) return ESP_ERR_INVALID_ARG;
    const size_t root_len = strlen(cfg->root);
    for (int k = 0; k < CPACK_PREFIXES_MAX && cfg->prefixes[k]; k++) {
        const size_t n = strlen(cfg->prefixes[k]);
        if (n <= root_len || strncmp(cfg->prefixes[k], cfg->root, root_len) || cfg->prefixes[k][n - 1] != '/')
            return ESP_ERR_INVALID_ARG;
    }
    pool_t* p = &s_pools[s_npools];
    memset(p, 0, sizeof(*p));
    if (!lock_make(&p->lock)) return ESP_ERR_NO_MEM;
    p->cfg = *cfg;
    p->root_len = root_len;
    p->cap_eff = cfg->cap_bytes;
    p->thr = CPACK_LIVE_ROOM;
    const uint64_t min_seg = cfg->cap_bytes / 32;
    p->seg_max = min_seg > s_seg_max ? (uint32_t)(min_seg < 0x40000000u ? min_seg : 0x40000000u) : s_seg_max;
    s_npools++;
    return ESP_OK;
}

void cpack_set_generation_fn(uint32_t (*fn)(void)) { s_gen_fn = fn; }
void cpack_set_free_fn(uint64_t (*fn)(void)) { s_free_fn = fn; }

void cpack_release(void) {
    for (int i = 0; i < s_npools; i++) {
        pool_t* p = &s_pools[i];
        if (!lock_try(&p->lock)) continue;
        if (p->loaded && !p->gc_busy) unload(p);
        lock_give(&p->lock);
    }
}

void cpack_pause(void) {
    if (!s_inited) return;
    s_suspended = true;
    // 等后台手上那一步和前台正在做的读写做完。/ Wait for the background step in hand and any foreground operation.
    lock_take(&s_bg_lock);
    lock_give(&s_bg_lock);
    for (int i = 0; i < s_npools; i++) {
        lock_take(&s_pools[i].lock);
        unload(&s_pools[i]);
        lock_give(&s_pools[i].lock);
    }
}

void cpack_resume(void) {
    if (!s_inited) return;
    s_suspended = false;
    wake_service();
}

#ifdef CPACK_HOST_TEST
void cpack_test_service(void) {
    service_round();
    while (trash_round()) { }
}

void cpack_test_unload(void) {
    for (int i = 0; i < s_npools; i++) unload(&s_pools[i]);
}

void cpack_test_set_segment_max(uint32_t bytes) { s_seg_max = bytes; }

uint64_t cpack_test_written(void) { return s_written; }

void cpack_test_reset(void) {
    for (int i = 0; i < s_npools; i++) { unload(&s_pools[i]); pthread_mutex_destroy(&s_pools[i].lock); }
    if (s_inited) pthread_mutex_destroy(&s_bg_lock);
    memset(s_pools, 0, sizeof(s_pools));
    s_npools = 0;
    s_inited = false;
    s_suspended = false;
    s_gen_fn = NULL;
    s_free_fn = NULL;
    s_seg_max = SEG_MAX_DEFAULT;
}
#endif
