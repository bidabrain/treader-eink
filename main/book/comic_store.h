/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 漫画书架的数据：扫描 TF 卡 comics/（一层系列文件夹或单个 CBZ）、按自然顺序排话、阅读进度与翻页方向、删除。
 * 进度存在 comics/.treader/progress.txt，和书库的进度分开。
 *
 * Comics shelf data: scan comics/ on the TF card (one level of series folders or single CBZ files), natural episode
 * order, reading progress and page direction, deletion. Progress lives in comics/.treader/progress.txt, apart from
 * the library's.
 *
 * 冻结：只认 CBZ，只扫一层文件夹，不碰 comics/ 以外的文件。
 * Frozen: CBZ only, one folder level, never touch files outside comics/.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define COMIC_ROOT "/sdcard/comics"
#define COMIC_META_DIR COMIC_ROOT "/.treader"
#define COMIC_NAME_MAX 128
#define COMIC_PATH_MAX 288
#define COMIC_SHELF_MAX 300
#define COMIC_VOLUME_MAX 500

/// 书架上的一项：系列文件夹或单本 CBZ。/ One shelf item: a series folder or a single CBZ.
typedef struct {
    char name[COMIC_NAME_MAX];   ///< 显示名（文件夹名或去掉 .cbz 的文件名）/ Display name
    char path[COMIC_PATH_MAX];   ///< 绝对路径 / Absolute path
    bool series;                 ///< 是系列文件夹 / Is a series folder
    uint16_t volumes;            ///< 系列的话数（单本为 1）/ Episodes in the series (1 for a single)
    uint16_t done;               ///< 看完的话数 / Finished episodes
    uint32_t stamp;              ///< 最近阅读的顺序号，0 为没看过 / Recent-read counter, 0 if never read
    uint8_t pct;                 ///< 单本的进度百分比 / Single-book progress percent
    bool webtoon;                ///< 系列文件夹里有 WEBTOON 订阅标记 / The series folder has a WEBTOON subscription marker
    uint16_t fresh;              ///< WEBTOON：上次打开后新下载的话数 / WEBTOON: episodes downloaded since last opened
    char wt_id[16];              ///< WEBTOON 作品编号 / WEBTOON title id
} comic_entry_t;

/// 一话（一个 CBZ）的进度。/ Progress of one episode (one CBZ).
typedef struct {
    uint16_t page;    ///< 当前页（从 0 起）/ Current page, from 0
    uint16_t pages;   ///< 总页数 / Page count
    uint32_t stamp;   ///< 最近阅读顺序号 / Recent-read counter
    bool done;        ///< 看完 / Finished
} comic_progress_t;

/// 自然排序比较（数字按数值比）。/ Natural comparison (digit runs compare by value).
int comic_name_cmp(const char* a, const char* b);
/// 扫描书架；返回项数，没卡或没有 comics/ 返回 0。有 WEBTOON 订阅标记的系列即使还没下载到话也列出。
/// Scan the shelf; returns the count, 0 without a card or folder. Series with a WEBTOON marker are listed even before
/// any episode is downloaded.
int comic_store_scan(comic_entry_t* out, int max);
/// 列出系列里的 CBZ 文件名，按自然顺序。/ List a series' CBZ file names in natural order.
int comic_store_volumes(const char* series_path, char (*names)[COMIC_NAME_MAX], int max);

/// 读一话的进度；没有记录返回 false。/ Read an episode's progress; false when there is none.
bool comic_progress_get(const char* path, comic_progress_t* out);
/// 记一话的进度（只改内存，flush 才写卡）；stamp 为 0 时自动取新顺序号。
/// Record an episode's progress in memory (flush writes it); a zero stamp takes a fresh counter.
void comic_progress_set(const char* path, const comic_progress_t* p);
/// 清掉 path 本身及其下所有话的进度。/ Forget progress for path and everything under it.
void comic_progress_forget(const char* path);
/// 最近看的那一话；没有返回 false。/ The most recently read episode; false if none.
bool comic_progress_last(char* path, size_t cap);
/// 翻页方向：1 从右往左，0 从左往右，-1 没设过。key 是系列文件夹或单本路径。
/// Page direction: 1 right-to-left, 0 left-to-right, -1 unset. key is the series folder or single path.
int comic_direction_get(const char* key);
void comic_direction_set(const char* key, bool rtl);
/// 有改动就写回卡上。/ Write changes back to the card.
esp_err_t comic_progress_flush(void);
/// 丢掉内存里的进度表（拔卡时用，不写卡）。/ Drop the in-memory table without writing (on media loss).
void comic_progress_drop(void);

/// 删除单本 CBZ 或整个系列文件夹（只删里面的 CBZ 和文件夹本身），并清进度。
/// Delete a single CBZ or a whole series folder (its CBZ files and the folder) and forget the progress.
esp_err_t comic_store_delete(const char* path);
