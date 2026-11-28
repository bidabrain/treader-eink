/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：TF 卡上的缓存包。把成千上万个能重新下载的小缓存文件（帖子 JSON、图片、灰度缓存）按原来的路径当 key 存进
 * 几个只追加的段文件，避开 FAT 上建、删、改名小文件的目录开销。每个池有总大小上限，后台按“最旧的段先回收、列表里
 * 还用着的搬到新段”做垃圾回收，不会越用越大。本组件不碰 SD 挂载、NVS 和界面，主机上可以直接测试。
 *
 * English: The TF-card cache pack. Thousands of re-downloadable small cache files (post JSON, images, gray caches) are
 * stored under their old paths as keys in a few append-only segment files, avoiding FAT's directory cost of
 * creating, deleting and renaming small files. Every pool has a total size cap and a background collector reclaims
 * the oldest segment first, moving what the lists still use into a new segment, so it never keeps growing. This
 * component touches no SD mounting, NVS or UI and runs in host tests.
 *
 * 冻结：只放能重建的缓存，读不出来一律当“没有”，从不尝试修复；段文件格式改动要升版本号（旧版本的段整段丢弃）。
 * Frozen: only rebuildable caches go in, anything unreadable counts as absent and is never repaired; a segment format
 * change bumps the version (segments of an older version are dropped whole).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CPACK_POOLS_MAX 4        ///< 最多几个池 / Most pools
#define CPACK_PREFIXES_MAX 3     ///< 每个池最多几个路径前缀 / Most path prefixes per pool
#define CPACK_KEY_MAX 160        ///< key（去掉池根目录的路径）最长字节 / Longest key (the path below the pool root)
#define CPACK_RECORD_MAX (4u * 1024u * 1024u)  ///< 单条记录数据上限 / Largest record payload

/// 垃圾回收时一条记录还值不值得留。/ How much a record is still worth keeping during collection.
typedef enum {
    CPACK_LIVE_DROP = 0,   ///< 列表里已经没有，可以扔 / No longer listed, may go
    CPACK_LIVE_ROOM = 1,   ///< 列表里有，有空间就留（大图）/ Listed, kept while there is room (full images)
    CPACK_LIVE_KEEP = 2,   ///< 尽量留（正文、缩略图、正在看的）/ Kept as long as possible (text, thumbnails, open item)
} cpack_live_t;

/// 一个池的配置。所有字符串和数组在池的整个生命期内必须有效（通常是静态常量）。
/// A pool's configuration. Every string and array must outlive the pool (normally static constants).
typedef struct {
    /// 池名，也是段文件所在子目录名（小写字母数字，最多 8 个字符）。/ Pool name, also the segment subfolder (lowercase
    /// letters and digits, at most 8 characters).
    const char* name;
    /// 归这个池管的路径前缀，以 '/' 结尾，NULL 结束。/ Path prefixes this pool owns, each ending in '/', NULL-terminated.
    const char* prefixes[CPACK_PREFIXES_MAX + 1];
    /// key 是路径去掉这一段之后的部分，必须是每个前缀的开头。/ The key is the path with this removed; it must start
    /// every prefix.
    const char* root;
    /// 总大小上限（字节，含段头和段尾索引）。/ Total size cap in bytes (segment headers and footers included).
    uint64_t cap_bytes;
    /// 升级前的旧缓存目录，第一次用这个池时整个挪进回收站（只改名，不删，用户可在电脑上删），NULL 结束；可以为 NULL。
    /// Pre-upgrade cache folders, moved whole into the trash the first time the pool is used (a rename only, never
    /// deleted; the user may delete it on a computer), NULL-terminated; may be NULL.
    const char* const* old_dirs;
    /// 一轮回收开始时调用，返回传给 live 的上下文；返回 NULL 表示这轮不能判断（例如列表读坏了），这轮不回收。
    /// Called when a collection round starts; returns the context passed to live; NULL means this round cannot judge
    /// (a list failed to read, say) and nothing is collected.
    void* (*gc_begin)(void* user);
    /// 判断一条记录还值不值得留；key 见 root。/ Judge whether a record is still worth keeping; key as in root.
    cpack_live_t (*live)(void* ctx, const char* key);
    /// 一轮回收结束时调用。/ Called when a collection round ends.
    void (*gc_end)(void* ctx);
    /// 传给 gc_begin 的参数。/ Argument passed to gc_begin.
    void* user;
} cpack_pool_cfg_t;

/// 池的统计。/ Pool statistics.
typedef struct {
    bool loaded;            ///< 索引在内存里 / Index is in memory
    uint32_t segments;      ///< 段文件个数 / Segment files
    uint32_t records;       ///< 有效记录条数 / Live records
    uint64_t bytes;         ///< 段文件总字节 / Total segment bytes
    uint64_t live_bytes;    ///< 有效记录占的字节 / Bytes held by live records
    uint64_t cap_bytes;     ///< 当前生效的上限 / Cap in effect
} cpack_stats_t;

/// 初始化，root 是放段文件的目录（例如 "/sdcard/.treader/cache"），trash 是回收站目录；可重复调用，只有第一次生效。
/// 设备上会起一个低优先级后台任务做垃圾回收。回收站不自动清空：在 FAT 上逐个删上万个小文件要占卡好几个小时，界面会卡。
/// Initialize with root as the segment directory (e.g. "/sdcard/.treader/cache") and trash as the trash directory;
/// repeat calls are ignored. On the device it starts a low-priority background task for collection. The trash is not
/// emptied automatically: deleting tens of thousands of small files one by one on FAT holds the card for hours and
/// stalls the UI.
esp_err_t cpack_init(const char* root, const char* trash);

/// 登记一个池；返回 ESP_OK 或参数错误。初始化后、第一次读写前调用。/ Register a pool; returns ESP_OK or an argument
/// error. Call after init and before the first access.
esp_err_t cpack_register(const cpack_pool_cfg_t* cfg);

/// 给出“卡的挂载代数”的函数：挂载换了（拔插、换卡、格式化）返回值就变，包会丢掉内存里的索引重新读。可以不设。
/// Supply a function returning the card's mount generation: it changes whenever the mount changes (removal, swap,
/// format) and the pack then drops its in-memory index and reads it again. Optional.
void cpack_set_generation_fn(uint32_t (*fn)(void));

/// 给出“卡上还剩多少字节”的函数，剩余空间不够时自动压低上限。可以不设。/ Supply a function returning the card's free
/// bytes; the cap shrinks on its own when space runs low. Optional.
void cpack_set_free_fn(uint64_t (*fn)(void));

/// 这个路径归不归某个池管。/ Whether some pool owns this path.
bool cpack_owns(const char* path);

/// 存一条：同一路径再存就覆盖。超过上限太多（后台还没回收完）时返回 ESP_ERR_NO_MEM，调用方当缓存失败处理。
/// Store one record; storing the same path again replaces it. Returns ESP_ERR_NO_MEM when far over the cap (the
/// background has not caught up), which callers treat as a cache failure.
esp_err_t cpack_put(const char* path, const void* data, size_t len);
/// 同 cpack_put，数据分两段给（a 在前 b 在后），省得调用方先拼成一块。/ Like cpack_put with the data in two parts (a
/// then b), so callers need not join them first.
esp_err_t cpack_put2(const char* path, const void* a, size_t alen, const void* b, size_t blen);

/// 读一条：*out 是 PSRAM 里新分配的 len+1 字节（末尾补 0，方便当字符串），用 cpack_free 放掉。数据比 max 大、
/// 没有、校验不对都返回错误且 *out 为 NULL。
/// Read one record: *out is a fresh PSRAM buffer of len+1 bytes (a trailing 0 for string use), freed with cpack_free.
/// Larger than max, absent or failing its checksum returns an error with *out NULL.
esp_err_t cpack_get(const char* path, size_t max, uint8_t** out, size_t* len);

/// 读开头最多 cap 字节到 buf（不校验整条），*got 是读到的字节数。/ Read at most cap leading bytes into buf (no whole
/// record check); *got receives the byte count.
esp_err_t cpack_read_head(const char* path, void* buf, size_t cap, size_t* got);

/// 有没有这条，有的话 *len 是数据长度（只查内存索引，不读卡）。/ Whether the record exists; *len receives its length
/// (looks at the in-memory index only, no card access).
bool cpack_size(const char* path, size_t* len);

/// 删一条；没有也返回 ESP_OK。/ Delete one record; ESP_OK when it was absent too.
esp_err_t cpack_remove(const char* path);

/// 放掉 cpack_get 给的缓冲。/ Free a buffer from cpack_get.
void cpack_free(void* p);

/// 提醒后台看看要不要回收（存满时自己也会提醒）。/ Nudge the background to check for collection (a full pool does so
/// on its own too).
void cpack_gc_kick(void);

/// 内存紧张时调用：没在用的池放掉内存索引，下次用时再从卡上读。/ Call when memory is short: pools not in use drop their
/// in-memory index and read it from the card again next time.
void cpack_release(void);

/// 卡要交出去（USB 传书）、格式化或重新挂载之前调用：等前台的读写和后台手上那一步做完，之后一律拒绝、后台不再碰卡，
/// 直到 cpack_resume。/ Call before the card is handed away (USB transfer), formatted or remounted: waits for foreground
/// operations and the background step in hand, then refuses everything and keeps the background off the card until
/// cpack_resume.
void cpack_pause(void);
/// 恢复（交卡失败、格式化或重新挂载做完之后）；索引下次用时从卡上重读。/ Resume (after a failed handover, or once a
/// format or remount is done); the index is read from the card again on next use.
void cpack_resume(void);

/// 取一个池的统计；name 不存在返回 false。/ Get a pool's statistics; false when no pool has that name.
bool cpack_stats(const char* name, cpack_stats_t* out);

#ifdef CPACK_HOST_TEST
/// 主机测试用：同步做完一轮回收。/ Host tests: run one collection round synchronously.
void cpack_test_service(void);
/// 主机测试用：放掉所有池的内存状态（模拟重启），配置保留。/ Host tests: drop every pool's memory state (a simulated
/// restart), keeping the configuration.
void cpack_test_unload(void);
/// 主机测试用：段文件大小上限。/ Host tests: the segment size limit.
void cpack_test_set_segment_max(uint32_t bytes);
/// 主机测试用：至今写进段文件的记录字节。/ Host tests: record bytes written into segments so far.
uint64_t cpack_test_written(void);
/// 主机测试用：清空所有登记（池和函数），回到未初始化。/ Host tests: forget every registration and return to
/// uninitialized.
void cpack_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif
