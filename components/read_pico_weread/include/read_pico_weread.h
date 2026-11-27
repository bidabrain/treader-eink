/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：微信读书的设备侧接口。协议引擎来自 CrossMux（MIT，见 LICENSE-CrossMux.txt），这里把它接到 Treader：
 * 联网在 tomato 的后台任务里跑（tomato_sync_start_ext），页面读快照和书架。扫码登录、同步书架、整本下载成 EPUB 写进书库。
 *
 * English: The device side of WeRead. The protocol engine comes from CrossMux (MIT, see LICENSE-CrossMux.txt); this
 * wires it into Treader: networking runs on the tomato background task (tomato_sync_start_ext) and pages read a
 * snapshot and the shelf. QR sign-in, shelf sync, whole-book download to EPUB in the library.
 *
 * 冻结：只读云端，不上传进度和笔记；只有完整、校验过的 EPUB 才出现在书库；登录信息用本机钥匙加密存在 TF 卡。
 * Frozen: read-only against the cloud, never uploading progress or notes; only complete, verified EPUBs appear in the
 * library; the sign-in is stored on the TF card encrypted under a device key.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/// 成书直接放进书库目录（书库不扫子目录）；哪些是微信读书下的记在缓存目录的清单里。/ Finished books go straight
/// into the library directory (the library does not scan subdirectories); a list in the cache records which came from
/// WeRead.
#define WEREAD_BOOKS_DIR "/sdcard/books"
/// 微信读书自己的目录，和番茄的 /sdcard/tomato 同级。/ WeRead's own directory, beside Tomato's /sdcard/tomato.
#define WEREAD_DIR "/sdcard/weixin"
/// 缓存（登录信息、书架、封面、下载中间文件），退出登录时清空。/ Cache (sign-in, shelf, covers, download scratch),
/// emptied on sign-out.
#define WEREAD_CACHE_DIR WEREAD_DIR "/cache"
/// 下成 TXT 的书的封面（文件名是“书的文件名.img”），书库照番茄 TXT 的做法显示；退出登录也留着。/ Covers of books
/// downloaded as TXT ("<book file name>.img"), shown by the library like Tomato TXT covers; kept on sign-out.
#define WEREAD_COVER_DIR WEREAD_DIR "/covers"
#define WEREAD_PATH_MAX 288

typedef enum {
    WEREAD_STAGE_NONE = 0,
    WEREAD_STAGE_QR,          ///< 等手机扫码 / Waiting for the phone to scan
    WEREAD_STAGE_SHELF,       ///< 取书架 / Fetching the shelf
    WEREAD_STAGE_COVERS,      ///< 取书架封面 / Fetching shelf covers
    WEREAD_STAGE_CHAPTERS,    ///< 下载章节 / Downloading chapters
    WEREAD_STAGE_IMAGES,      ///< 下载插图 / Downloading illustrations
    WEREAD_STAGE_PACKAGING,   ///< 打包 EPUB / Packing the EPUB
} weread_stage_t;

/// 后台任务的进度，页面轮询。/ Background progress the page polls.
typedef struct {
    weread_stage_t stage;
    char qr[320];          ///< 扫码登录的网址（stage 为 QR 时）/ Sign-in URL to show as a QR (stage QR)
    uint32_t done, total;  ///< 本阶段进度 / Progress in this stage
    char title[192];       ///< 正在下载的书名 / Title being downloaded
    char output[WEREAD_PATH_MAX];   ///< 最近下载完成的文件 / Last finished file
    uint32_t finished;     ///< 下载完成的次数，变了说明书库多了书 / Finished downloads; a change means a new book
    uint32_t revision;     ///< 每次变化加一 / Bumped on every change
} weread_snapshot_t;

/// 书架上的一本。/ One shelf book.
typedef struct {
    char id[64];
    char title[192];
    char author[96];
    char local[WEREAD_PATH_MAX];   ///< 已下载的 TXT 或 EPUB，没有为空 / Downloaded TXT or EPUB; empty when none
    char cover[WEREAD_PATH_MAX];   ///< 封面原图，没有为空 / Cover image; empty when none
} weread_book_t;

/// 设好目录（TF 卡挂载后、后台空闲时调用；重复调用无害）。/ Set the directories (after the TF card mounts, while
/// idle; repeat calls are harmless).
bool weread_configure(void);

/// 用户确认过“非官方接口”的说明。/ Whether the user accepted the unofficial-interface notice.
bool weread_disclaimer_accepted(void);
bool weread_accept_disclaimer(void);

/// 本机有登录信息（不联网验证）。/ Whether a local sign-in exists (not verified online).
bool weread_logged_in(void);

/// 后台登录（需要时扫码）并同步书架和封面；浏览类，前台可打断。/ Sign in in the background (QR when needed) and sync
/// the shelf and covers; browsing, so the foreground may preempt it.
esp_err_t weread_start_sync(void);
/// 后台整本下载；下载类，锁屏不睡。epub 为真时出带插图的 EPUB（慢），否则只出文字 TXT（快）。/ Download one whole
/// book in the background; a download, so lock holds. With epub an EPUB with illustrations (slower), otherwise a
/// text-only TXT (faster).
esp_err_t weread_start_download(const char* book_id, bool epub);
/// 是不是微信读书的任务在跑。/ Whether a WeRead job is running.
bool weread_busy(void);
void weread_snapshot(weread_snapshot_t* out);

/// 书架本数；没同步过为 0。/ Shelf size; 0 before the first sync.
int weread_shelf_count(void);
/// 读第 index 本。/ Read book index.
bool weread_shelf_get(int index, weread_book_t* out);
/// 删掉没下完的书留下的章节和插图缓存（取消、失败后留着的续传数据）；后台在跑时不动。离开微信读书页时调用。
/// Delete chapter and image caches left by unfinished downloads (resume data kept after a cancel or failure); does
/// nothing while a job runs. Called when leaving the WeRead page.
void weread_drop_partial(void);
/// 书库删了这本书：删掉它的 TXT 封面。/ The library deleted this book: delete its TXT cover.
void weread_forget_book(const char* path);
/// 书库里这本书是不是从微信读书下的。/ Whether this library file came from WeRead.
bool weread_is_book(const char* path);

/// 退出登录：删登录信息、书架和缓存；已下载的书留着。后台在跑时失败。/ Sign out: delete the sign-in, shelf and cache;
/// downloaded books stay. Fails while a job runs.
esp_err_t weread_logout(void);

#ifdef __cplusplus
}
#endif
