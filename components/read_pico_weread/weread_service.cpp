/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：read_pico_weread.h 的实现。引擎的 Operation 放 PSRAM，在 tomato 后台任务里一步步跑；进度写进快照，
 * 页面用临界区复制。下载完的书名记进 /sdcard/weixin/books.txt，书库据此认出微信读书的书。
 *
 * English: Implementation of read_pico_weread.h. The engine Operation lives in PSRAM and steps on the tomato
 * background task; progress goes into a snapshot pages copy under a critical section. Finished book names go into
 * /sdcard/weixin/books.txt so the library can tell WeRead books apart.
 *
 * 冻结：取消一律在下一步之前生效，不写半本书；后台在跑时不退出登录、不改目录。
 * Frozen: a cancel always takes effect before the next step, never leaving half a book; no sign-out or directory
 * change while a job runs.
 */

#include "read_pico_weread.h"

#include <dirent.h>
#include <sys/stat.h>

#include <cstdio>
#include <atomic>
#include <cstring>
#include <functional>
#include <mutex>
#include <new>
#include <vector>

#include "HalStorage.h"
#include "ObfuscationUtils.h"
#include "WeReadBrowse.h"
#include "WeReadClient.h"
#include "WeReadStore.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tomato.h"

#define TAG "weread"
// 清单在缓存目录外：退出登录清缓存时它要留着，已下载的书照样认得出。/ The list sits outside the cache so it survives
// the cache wipe on sign-out and downloaded books are still recognized.
#define LIST_PATH WEREAD_DIR "/books.txt"
#define LIST_MAX 2048   // 清单最大字节，书名只记文件名 / List size cap; only file names are kept

namespace {

struct Job {
    bool download;
    bool epub;
    char id[64];
};

portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
weread_snapshot_t s_snap;
Job s_job;
volatile bool s_running;
bool s_configured;
const volatile bool* s_cancel;

// 改快照；内容真的变了才加版本号，页面只在有变化时重画。/ Update the snapshot; the revision moves only on a real
// change, so pages redraw only when something changed.
void snap_update(void (*fn)(weread_snapshot_t*, const void*), const void* arg) {
    portENTER_CRITICAL(&s_lock);
    const weread_snapshot_t before = s_snap;
    fn(&s_snap, arg);
    s_snap.revision = before.revision;
    if (memcmp(&before, &s_snap, sizeof(before))) s_snap.revision++;
    portEXIT_CRITICAL(&s_lock);
}

weread_stage_t stage_of(const WeReadClient::Operation& op, bool download) {
    using S = WeReadClient::Operation::ProgressStage;
    switch (op.progressStage()) {
    case S::Chapters: return download ? WEREAD_STAGE_CHAPTERS : WEREAD_STAGE_SHELF;
    case S::Images: return download ? WEREAD_STAGE_IMAGES : WEREAD_STAGE_COVERS;
    case S::Packaging: return download ? WEREAD_STAGE_PACKAGING : WEREAD_STAGE_COVERS;
    case S::Preparing: default: return download ? WEREAD_STAGE_CHAPTERS : WEREAD_STAGE_SHELF;
    }
}

const char* error_text(WeReadClient::Error e) {
    using E = WeReadClient::Error;
    switch (e) {
    case E::Network: return "网络连接失败";
    case E::SessionExpired: return "登录已过期，请重新扫码";
    case E::LoginFailed: return "登录失败";
    case E::SdCard: return "TF 卡写入失败";
    case E::Integrity: return "下载内容校验失败";
    case E::Unavailable: return "这本书暂时无法下载";
    case E::Clock: return "时间不对，请先校时";
    case E::OutOfMemory: return "内存不足";
    case E::WholeBookOnly: return "这本书只能下成 EPUB，在设置里把下载格式改成 EPUB";
    case E::Cancelled: return "已取消";
    default: return "微信读书返回了无法识别的内容";
    }
}

// 清单读进内存（PSRAM）一次，书库扫描时逐本查不再开文件；写清单后作废重读。
// The list is read into PSRAM once so a library scan checks each book without opening files; a write invalidates it.
std::recursive_mutex s_list_mutex;   // 后台写、书库读 / The worker writes, the library reads
char* s_list;          // "\n名字\n名字\n" / "\nname\nname\n"
bool s_list_loaded;

void list_load(void) {
    if (s_list_loaded) return;
    s_list_loaded = true;
    heap_caps_free(s_list);
    s_list = nullptr;
    FILE* f = fopen(LIST_PATH, "r");
    if (!f) return;
    struct stat st;
    if (fstat(fileno(f), &st) == 0 && st.st_size > 0 && st.st_size <= 4 * LIST_MAX) {
        s_list = static_cast<char*>(heap_caps_malloc(st.st_size + 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (s_list) {
            s_list[0] = '\n';
            const size_t n = fread(s_list + 1, 1, st.st_size, f);
            s_list[1 + n] = '\n';
            s_list[2 + n] = 0;
            for (char* p = s_list; *p; p++) if (*p == '\r') *p = '\n';
        }
    }
    fclose(f);
}

// 文件名在清单里（每行一个）。/ Whether the file name is in the list (one per line).
bool list_has(const char* name) {
    std::lock_guard<std::recursive_mutex> guard(s_list_mutex);
    list_load();
    if (!s_list || !name[0]) return false;
    const size_t n = strlen(name);
    for (const char* p = strstr(s_list, name); p; p = strstr(p + 1, name))
        if (p[-1] == '\n' && p[n] == '\n') return true;
    return false;
}

void list_add(const char* name) {
    std::lock_guard<std::recursive_mutex> guard(s_list_mutex);
    if (!name[0] || list_has(name)) return;
    struct stat st;
    // 太大就重写，只留还在书库里的。/ Rewrite when too big, keeping only books still in the library.
    if (stat(LIST_PATH, &st) == 0 && st.st_size > LIST_MAX) {
        FILE* in = fopen(LIST_PATH, "r");
        FILE* out = fopen(LIST_PATH ".part", "w");
        char line[WEREAD_PATH_MAX], path[WEREAD_PATH_MAX + 16];
        while (in && out && fgets(line, sizeof(line), in)) {
            line[strcspn(line, "\r\n")] = 0;
            snprintf(path, sizeof(path), WEREAD_BOOKS_DIR "/%s", line);
            if (line[0] && stat(path, &st) == 0) fprintf(out, "%s\n", line);
        }
        if (in) fclose(in);
        if (out && fclose(out) == 0) {
            remove(LIST_PATH);
            rename(LIST_PATH ".part", LIST_PATH);
        }
        s_list_loaded = false;
    }
    FILE* f = fopen(LIST_PATH, "a");
    if (!f) return;
    fprintf(f, "%s\n", name);
    fclose(f);
    s_list_loaded = false;
}

const char* base_name(const char* path) {
    const char* s = strrchr(path, '/');
    return s ? s + 1 : path;
}

bool cancelled(void*) { return s_cancel && *s_cancel; }

void progress_cb(void* raw) {
    auto* op = static_cast<WeReadClient::Operation*>(raw);
    if (cancelled(nullptr)) op->cancel();
}

/* ---- 缓存清理 / Cache cleanup ---- */

// 这张卡上删一个文件可能要将近 1 秒：先把要删的目录改名挪进 .trash（一次目录操作，马上完成），再由低优先级的
// 后台任务慢慢删，界面和下载都不等它。/ Deleting one file on this card can take close to a second: directories are
// first renamed into .trash (one directory operation, instant) and a low-priority task deletes them later, so neither
// the UI nor a download waits.
#define TRASH_DIR WEREAD_DIR "/.trash"   // 在缓存外面，退出登录能把整个缓存挪进来 / Outside the cache so sign-out can move the whole cache in
volatile bool s_trash_busy;
uint32_t s_trash_seq;

bool remove_tree(const std::string& p) {
    DIR* d = opendir(p.c_str());
    if (!d) return unlink(p.c_str()) == 0;
    bool ok = true;
    for (struct dirent* e; (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        ok = remove_tree(p + "/" + e->d_name) && ok;
        vTaskDelay(1);   // 让出 CPU 和卡 / Yield the CPU and the card
    }
    closedir(d);
    return rmdir(p.c_str()) == 0 && ok;
}

void trash_task(void*) {
    remove_tree(TRASH_DIR);
    s_trash_busy = false;
    vTaskDelete(nullptr);
}

// 后台删 .trash；已经在删就不重复开。/ Delete .trash in the background unless that is already running.
void empty_trash(void) {
    struct stat st;
    if (s_trash_busy || stat(TRASH_DIR, &st) != 0) return;
    s_trash_busy = true;
    if (xTaskCreate(trash_task, "weread_trash", 4096, nullptr, 1, nullptr) != pdPASS) s_trash_busy = false;
}

// .trash 下一个没用过的名字（重启后序号从头来，可能撞上还没删完的）。/ An unused name under .trash (the counter
// restarts after a reboot and may hit one not yet deleted).
bool trash_name(char* out, size_t cap) {
    struct stat st;
    if (stat(TRASH_DIR, &st) != 0 && mkdir(TRASH_DIR, 0777) != 0) return false;
    for (int tries = 0; tries < 1000; tries++) {
        snprintf(out, cap, TRASH_DIR "/%u", (unsigned)++s_trash_seq);
        if (stat(out, &st) != 0) return true;
    }
    return false;
}

// 把一本书（真实目录）的章节和插图缓存挪进 .trash。/ Move one book's (real directory) chapter and image cache into
// .trash.
void trash_chapters(const std::string& real_dir) {
    struct stat st;
    for (const char* sub : {"/chapters", "/images"}) {
        const std::string from = real_dir + sub;
        if (stat(from.c_str(), &st) != 0) continue;
        char to[96];
        if (!trash_name(to, sizeof(to)) || rename(from.c_str(), to) != 0) ESP_LOGW(TAG, "trash rename failed");
    }
}

// 删掉一本书的章节和插图缓存。/ Drop one book's chapter and image cache.
void drop_chapters(const std::string& dir) {
    trash_chapters(Storage.map(dir));
    empty_trash();
}

// 清掉别的书没下完留下的章节缓存（keep_id 为空就全清）。/ Clear chapter caches unfinished books left behind (all
// of them when keep_id is empty).
void drop_other_chapters(const char* keep_id) {
    const std::string keep = keep_id[0] ? Storage.map(WeReadStore::bookDirectory(keep_id)) : std::string();
    DIR* d = opendir(WEREAD_CACHE_DIR);
    if (!d) return;
    for (struct dirent* e; (e = readdir(d));) {
        if (e->d_name[0] == '.') continue;
        const std::string dir = std::string(WEREAD_CACHE_DIR "/") + e->d_name;
        if (dir != keep) trash_chapters(dir);
    }
    closedir(d);
    empty_trash();
}

bool find_record(const char* id, WeReadStore::ShelfRecord& rec) {
    HalFile shelf;
    uint32_t total = 0;
    if (!WeReadStore::openShelf(shelf, total)) return false;
    for (uint32_t i = 0; i < total; i++) {
        if (!WeReadStore::readShelfRecord(shelf, i, rec)) return false;
        if (memchr(rec.bookId, 0, sizeof(rec.bookId)) && !strcmp(rec.bookId, id)) return true;
    }
    return false;
}

/* ---- 跑引擎 / Driving the engine ---- */

using Op = WeReadClient::Operation;
using Ev = WeReadClient::Operation::Event;

// 在 PSRAM 里建一个引擎操作；用完 free_op。/ Make an engine operation in PSRAM; release it with free_op.
Op* new_op(void) {
    void* mem = heap_caps_malloc(sizeof(Op), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return mem ? new (mem) Op() : nullptr;
}

void free_op(Op* op) {
    if (!op) return;
    op->reset();
    op->~Op();
    heap_caps_free(op);
}

// 把一个操作跑到结束，进度和二维码写进快照。range 在目录到手（ChapterRangeReady）时设章节范围；total 指向的数
// 不为 0 时，进度按“本操作完成数 + extra()”对它算（并行下载合起来算）。/ Run an operation to its end, writing progress
// and the QR into the snapshot. range sets the chapter range once the contents are in (ChapterRangeReady); while
// *total is non-zero, progress counts this operation's chapters plus extra() against it (parallel parts added up).
struct Drive {
    Op* op;
    bool download;
    void (*progress)(int, int);
    std::function<bool()> range;
    std::function<uint32_t()> extra;
    const uint32_t* total = nullptr;
};

Ev drive(const Drive& d) {
    Op* op = d.op;
    while (op->active()) {
        if (cancelled(nullptr)) op->cancel();
        const Ev ev = op->step(progress_cb, op, cancelled, nullptr);
        if (ev == Ev::ChapterRangeReady && (!d.range || !d.range())) op->cancel();
        struct Prog { weread_stage_t stage; uint32_t done, total; const char* qr; } p = {
            stage_of(*op, d.download), op->progressCompleted(), op->progressTotal(),
            ev == Ev::QrReady ? op->qrUrl() : nullptr};
        if (d.total && *d.total && op->progressStage() == Op::ProgressStage::Chapters) {
            p.done += d.extra ? d.extra() : 0;
            p.total = *d.total;
        }
        snap_update([](weread_snapshot_t* s, const void* a) {
            const Prog* p = static_cast<const Prog*>(a);
            if (p->qr) {
                snprintf(s->qr, sizeof(s->qr), "%s", p->qr);
                s->stage = WEREAD_STAGE_QR;
            } else if (s->stage != WEREAD_STAGE_QR) {
                s->stage = p->stage;
            }
            s->done = p->done, s->total = p->total;
        }, &p);
        if (ev == Ev::Authenticated) {
            snap_update([](weread_snapshot_t* s, const void*) { s->qr[0] = 0, s->stage = WEREAD_STAGE_SHELF; }, nullptr);
        }
        if (d.progress && p.total) d.progress(static_cast<int>(p.done), static_cast<int>(p.total));
        if (ev == Ev::Complete || ev == Ev::Failed || ev == Ev::Cancelled) return ev;
        // 只让出一个调度周期：引擎等扫码、重试退避时空转也不占满 CPU，正常下载不白等。/ Yield one tick only: the
        // engine idling on a scan or retry backoff does not hog the CPU, and normal downloading does not wait.
        if (ev == Ev::None) vTaskDelay(1);
    }
    return op->error() == WeReadClient::Error::Ok ? Ev::Complete : Ev::Failed;
}

/* ---- TXT 并行下载 / Parallel TXT download ---- */

// 章节数不少于这个才分两路并行；太短的书一路下更省事。/ Books with at least this many chapters are split across two
// workers; shorter ones are not worth it.
#define PARALLEL_MIN 8
#define HELPER_STACK 14336

// 第二路：在自己的任务里下后半本。只有主路能出二维码，第二路要登录就放弃，由主路补下。/ The second worker downloads
// the back half on its own task. Only the main worker may show a QR, so the second gives up when it needs a sign-in
// and the main worker redoes its half.
struct Helper {
    Op* op = nullptr;
    uint32_t first = 0, last = 0;
    std::atomic<uint32_t> done{0};
    std::atomic<bool> abort{false};
    bool ok = false;
    SemaphoreHandle_t joined = nullptr;
};

bool helper_cancelled(void* raw) { return cancelled(nullptr) || static_cast<Helper*>(raw)->abort.load(); }

void helper_task(void* raw) {
    auto* h = static_cast<Helper*>(raw);
    Op* op = h->op;
    bool ranged = false;
    while (op->active()) {
        if (helper_cancelled(h)) op->cancel();
        const Ev ev = op->step(nullptr, nullptr, helper_cancelled, h);
        if (ev == Ev::ChapterRangeReady) ranged = op->setChapterRange(h->first, h->last);
        if (ev == Ev::ChapterRangeReady && !ranged) op->cancel();
        if (ev == Ev::QrReady) {
            ESP_LOGW(TAG, "helper needs sign-in, main worker will redo its half");
            op->cancel();
        }
        if (ranged && op->progressStage() == Op::ProgressStage::Chapters) h->done = op->progressCompleted();
        if (ev == Ev::Complete) {
            h->ok = true;
            break;
        }
        if (ev == Ev::Failed || ev == Ev::Cancelled) break;
        if (ev == Ev::None) vTaskDelay(1);
    }
    ESP_LOGI(TAG, "helper %s at %u/%u", h->ok ? "done" : "stopped", (unsigned)h->done.load(),
             (unsigned)(h->last - h->first + 1));
    xSemaphoreGive(h->joined);
    vTaskDelete(nullptr);
}

// 把一个真实目录挪进 .trash，交给后台删。/ Move a real directory into .trash for background deletion.
void trash_dir(const std::string& real) {
    struct stat st;
    char to[96];
    if (stat(real.c_str(), &st) == 0 && (!trash_name(to, sizeof(to)) || rename(real.c_str(), to) != 0))
        ESP_LOGW(TAG, "trash rename failed");
}

// 把书架同步时下好的封面原图复制成书库认的 TXT 封面。没有封面就算了。/ Copy the cover fetched during shelf sync to
// where the library looks for a TXT cover. No cover, no copy.
void copy_txt_cover(const WeReadStore::ShelfRecord& rec, const std::string& book_real) {
    const std::string dir = Storage.map(WeReadStore::bookDirectory(rec.bookId));
    std::string src;
    struct stat st;
    for (const char* name : {"/cover.source.jpg", "/cover.source.png"})
        if (stat((dir + name).c_str(), &st) == 0 && st.st_size > 0) {
            src = dir + name;
            break;
        }
    if (src.empty() || (stat(WEREAD_COVER_DIR, &st) != 0 && mkdir(WEREAD_COVER_DIR, 0777) != 0)) return;
    const std::string dest = std::string(WEREAD_COVER_DIR "/") + base_name(book_real.c_str()) + ".img";
    FILE* in = fopen(src.c_str(), "rb");
    FILE* out = in ? fopen(dest.c_str(), "wb") : nullptr;
    auto* buf = static_cast<uint8_t*>(heap_caps_malloc(16384, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    bool ok = in && out && buf;
    for (size_t n; ok && (n = fread(buf, 1, 16384, in)) > 0;) ok = fwrite(buf, 1, n, out) == n;
    heap_caps_free(buf);
    if (in) fclose(in);
    if (out && fclose(out) != 0) ok = false;
    if (!ok) remove(dest.c_str());
}

// 同步后给已经下成 TXT、还没封面的书补上封面（这个功能加进来以前下的书）。/ After a sync, give a cover to books
// already downloaded as TXT that lack one (books from before this existed).
void backfill_txt_covers(void) {
    HalFile shelf;
    uint32_t total = 0;
    if (!WeReadStore::openShelf(shelf, total)) return;
    WeReadStore::ShelfRecord rec;
    for (uint32_t i = 0; i < total && !cancelled(nullptr); i++) {
        if (!WeReadStore::readShelfRecord(shelf, i, rec) || !memchr(rec.bookId, 0, sizeof(rec.bookId)) ||
            !memchr(rec.title, 0, sizeof(rec.title)))
            continue;
        std::string txt = WeReadStore::finalBookPath(rec);
        txt.replace(txt.size() - 5, 5, ".txt");
        const std::string real = Storage.map(txt);
        struct stat st;
        if (stat(real.c_str(), &st) != 0) continue;
        const std::string cover = std::string(WEREAD_COVER_DIR "/") + base_name(real.c_str()) + ".img";
        if (stat(cover.c_str(), &st) != 0) copy_txt_cover(rec, real);
    }
}

// 后面的分段接到第一段末尾。/ Append the later parts to the first.
bool join_parts(const std::vector<std::string>& parts) {
    FILE* out = fopen(Storage.map(parts[0]).c_str(), "ab");
    auto* buf = static_cast<uint8_t*>(heap_caps_malloc(16384, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    bool ok = out && buf;
    for (size_t i = 1; ok && i < parts.size(); i++) {
        FILE* in = fopen(Storage.map(parts[i]).c_str(), "rb");
        ok = in != nullptr;
        for (size_t n; ok && (n = fread(buf, 1, 16384, in)) > 0;) ok = fwrite(buf, 1, n, out) == n;
        if (in) fclose(in);
    }
    heap_caps_free(buf);
    if (out && fclose(out) != 0) ok = false;
    return ok;
}

// TXT 下载：章节多就分两路同时下，最后拼起来。返回最终文件（虚拟路径），失败返回空串，msg 写原因。
// TXT download: with many chapters two workers run at once and the parts are joined. Returns the final file (virtual
// path), or empty on failure with msg set.
std::string download_txt(Op* op, const WeReadStore::ShelfRecord& rec, void (*progress)(int, int), char* msg,
                         size_t cap, bool& oom) {
    const std::string dir = WeReadStore::bookDirectory(rec.bookId);
    const std::string part1 = dir + "/part1.txt", part2 = dir + "/part2.txt", part3 = dir + "/part3.txt";
    for (const std::string* p : {&part1, &part2, &part3})
        if (Storage.exists(p->c_str())) Storage.remove(p->c_str());
    std::string final_path = WeReadStore::finalBookPath(rec);
    final_path.replace(final_path.size() - 5, 5, ".txt");

    WeReadClient::DownloadOptions opt;
    opt.imagePolicy = WeReadStore::ImagePolicy::Exclude;
    opt.plainTextOutput = true;
    opt.chapterScope = WeReadClient::DownloadOptions::ChapterScope::SelectRange;
    opt.textFinalize = false;
    opt.textPartPath = part1.c_str();

    Helper* h = nullptr;
    uint32_t n = 0, mid = 0;
    std::vector<std::string> parts{part1};
    Ev ev = Ev::Failed;
    bool fail_oom = false;
    std::string result;
    if (!op->begin(Op::Kind::Download, &rec, opt)) {
        fail_oom = op->error() == WeReadClient::Error::OutOfMemory;
        snprintf(msg, cap, "%s", error_text(op->error()));
        goto out;
    }
    {
        // 目录到手：章节够多就把后半本交给第二路。/ Contents in: with enough chapters the back half goes to the
        // second worker.
        Drive d{op, true, progress, nullptr, nullptr, &n};
        d.range = [&]() {
            n = op->chapterCount();
            mid = n;
            if (n >= PARALLEL_MIN && heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= 512 * 1024) {
                h = new (std::nothrow) Helper();
                Op* hop = h ? new_op() : nullptr;
                WeReadClient::DownloadOptions o2 = opt;
                o2.workSuffix = "/w2";
                o2.reuseToc = true;
                o2.textPartPath = part2.c_str();
                if (hop && (h->joined = xSemaphoreCreateBinary()) && hop->begin(Op::Kind::Download, &rec, o2)) {
                    h->op = hop;
                    h->first = n / 2;
                    h->last = n - 1;
                    if (xTaskCreate(helper_task, "weread_w2", HELPER_STACK, h, 4, nullptr) == pdPASS) {
                        mid = n / 2;
                        ESP_LOGI(TAG, "parallel: %u + %u chapters", (unsigned)mid, (unsigned)(n - mid));
                    } else {
                        h->op = nullptr;
                    }
                }
                if (!h || !h->op) {
                    // 开不了第二路：一路下完整本。/ No second worker: one worker does the whole book.
                    free_op(hop);
                    if (h && h->joined) vSemaphoreDelete(h->joined);
                    delete h;
                    h = nullptr;
                }
            }
            return op->setChapterRange(0, mid - 1);
        };
        // 进度按整本算：主路完成数加第二路完成数。/ Progress counts the whole book: both workers' chapters added.
        d.extra = [&]() { return h ? h->done.load() : 0u; };
        ev = drive(d);
    }
    if (h) {
        // 主路停了：失败或取消时叫第二路也停，然后等它退出，才能放它的内存。/ The main worker stopped: on failure or
        // cancel tell the second to stop too, then wait for it to exit before freeing its memory.
        if (ev != Ev::Complete) h->abort = true;
        xSemaphoreTake(h->joined, portMAX_DELAY);
    }
    if (ev != Ev::Complete) {
        fail_oom = op->error() == WeReadClient::Error::OutOfMemory;
        snprintf(msg, cap, "%s", error_text(ev == Ev::Cancelled ? WeReadClient::Error::Cancelled : op->error()));
        goto out;
    }
    if (h && h->ok) {
        parts.push_back(part2);
    } else if (h) {
        // 第二路没下完：主路把后半本整个补下。/ The second worker did not finish: the main worker redoes the back half.
        op->reset();
        WeReadClient::DownloadOptions o3 = opt;
        o3.reuseToc = true;
        o3.textPartPath = part3.c_str();
        const uint32_t first = mid;
        Drive d{op, true, progress, nullptr, nullptr, &n};
        d.range = [&]() { return op->setChapterRange(first, n - 1); };
        d.extra = [&]() { return first; };
        if (!op->begin(Op::Kind::Download, &rec, o3) || (ev = drive(d)) != Ev::Complete) {
            fail_oom = op->error() == WeReadClient::Error::OutOfMemory;
            snprintf(msg, cap, "%s", error_text(cancelled(nullptr) ? WeReadClient::Error::Cancelled : op->error()));
            goto out;
        }
        parts.push_back(part3);
    }
    snap_update([](weread_snapshot_t* s, const void*) { s->stage = WEREAD_STAGE_PACKAGING; }, nullptr);
    if (!join_parts(parts) || !WeReadStore::atomicReplace(part1, final_path)) {
        snprintf(msg, cap, "%s", error_text(WeReadClient::Error::SdCard));
        goto out;
    }
    result = final_path;
    copy_txt_cover(rec, Storage.map(final_path));
out:
    if (h) {
        free_op(h->op);
        vSemaphoreDelete(h->joined);
        delete h;
    }
    for (const std::string* p : {&part1, &part2, &part3})
        if (Storage.exists(p->c_str())) Storage.remove(p->c_str());
    trash_dir(Storage.map(dir + "/w2"));
    empty_trash();
    oom = fail_oom;
    return result;
}

// 在后台任务里跑一次引擎操作。/ Run one engine operation on the background task.
esp_err_t run_job(void*, const volatile bool* cancel, void (*progress)(int, int), char* msg, size_t cap) {
    s_cancel = cancel;
    const Job job = s_job;
    Op* op = new_op();
    WeReadStore::ShelfRecord rec;
    bool ok = false;
    bool oom = false;   // 引擎报内存不足：页面据此放缓存 / The engine ran out of memory; the page drops caches on it
    std::string final_path;
    if (!op) {
        snprintf(msg, cap, "内存不足");
        oom = true;
        goto done;
    }
    if (job.download) {
        if (!find_record(job.id, rec)) {
            snprintf(msg, cap, "书架上找不到这本书，请先同步");
            goto done;
        }
        snap_update([](weread_snapshot_t* s, const void* a) {
            snprintf(s->title, sizeof(s->title), "%s", static_cast<const WeReadStore::ShelfRecord*>(a)->title);
        }, &rec);
        drop_other_chapters(rec.bookId);
        if (!job.epub) {
            final_path = download_txt(op, rec, progress, msg, cap, oom);
            ok = !final_path.empty();
        } else {
            WeReadClient::DownloadOptions opt;
            opt.imagePolicy = WeReadStore::ImagePolicy::Embed;
            if (!op->begin(Op::Kind::Download, &rec, opt)) {
                oom = op->error() == WeReadClient::Error::OutOfMemory;
                snprintf(msg, cap, "%s", error_text(op->error()));
                goto done;
            }
            const Ev ev = drive(Drive{op, true, progress, nullptr, nullptr, 0});
            ok = ev == Ev::Complete;
            if (ok) final_path = op->finalPath();
            else {
                oom = op->error() == WeReadClient::Error::OutOfMemory;
                snprintf(msg, cap, "%s", error_text(ev == Ev::Cancelled ? WeReadClient::Error::Cancelled : op->error()));
            }
        }
    } else {
        if (!op->begin(Op::Kind::Sync, nullptr, {}, Op::ShelfCoverScope::All)) {
            snprintf(msg, cap, "%s", error_text(op->error()));
            goto done;
        }
        const Ev ev = drive(Drive{op, false, progress, nullptr, nullptr, 0});
        ok = ev == Ev::Complete;
        if (!ok) snprintf(msg, cap, "%s", error_text(ev == Ev::Cancelled ? WeReadClient::Error::Cancelled : op->error()));
    }
    if (ok && job.download) {
        // 书已经在书库：删掉这本的章节和插图缓存，TF 卡不留两份。/ The book is in the library: drop its chapter and image
        // cache so the card holds one copy.
        drop_chapters(WeReadStore::bookDirectory(rec.bookId));
        const std::string real = Storage.map(final_path);
        list_add(base_name(real.c_str()));
        snap_update([](weread_snapshot_t* s, const void* a) {
            snprintf(s->output, sizeof(s->output), "%s", static_cast<const char*>(a));
            s->finished++;
        }, real.c_str());
        snprintf(msg, cap, "下载完成");
    } else if (ok) {
        backfill_txt_covers();
        snprintf(msg, cap, "书架已同步，共 %d 本", weread_shelf_count());
    }
done:
    free_op(op);
    Storage.dropMemFiles();   // 取消或失败留下的内存中间文件 / Memory scratch left by a cancel or failure
    snap_update([](weread_snapshot_t* s, const void*) { s->qr[0] = 0, s->stage = WEREAD_STAGE_NONE; }, nullptr);
    s_cancel = nullptr;
    s_running = false;
    return ok ? ESP_OK : cancelled(nullptr) ? ESP_ERR_INVALID_STATE : oom ? ESP_ERR_NO_MEM : ESP_FAIL;
}

esp_err_t start(const Job& job, const char* step) {
    if (!weread_configure() || !Storage.ensureDirectoryExists("/.crosspoint/weread") ||
        !Storage.ensureDirectoryExists("/WeRead"))
        return ESP_ERR_INVALID_STATE;
    if (s_running || tomato_sync_busy()) return ESP_ERR_INVALID_STATE;
    empty_trash();   // 上次没删完的接着删 / Carry on deleting what an earlier run left
    s_job = job;
    s_running = true;
    snap_update([](weread_snapshot_t* s, const void*) {
        s->stage = WEREAD_STAGE_NONE, s->qr[0] = 0, s->done = s->total = 0, s->title[0] = 0;
    }, nullptr);
    const tomato_ext_job_t ext = {.run = run_job, .ctx = nullptr, .download = job.download, .step = step};
    const esp_err_t err = tomato_sync_start_ext(&ext);
    if (err != ESP_OK) s_running = false;
    return err;
}

}  // namespace

extern "C" {

// 只设路径映射，不建目录：画页面时也会调到。目录在开始任务时建。/ Only sets the path mapping and creates nothing, as
// page drawing calls it too. Directories are made when a job starts.
bool weread_configure(void) {
    if (s_running) return s_configured;
    struct stat st;
    s_configured = stat("/sdcard", &st) == 0;
    if (s_configured) Storage.configure(WEREAD_CACHE_DIR, WEREAD_BOOKS_DIR);
    return s_configured;
}

bool weread_disclaimer_accepted(void) { return weread_configure() && WeReadStore::hasAcceptedDisclaimer(); }

bool weread_accept_disclaimer(void) { return weread_configure() && WeReadStore::acceptDisclaimer(); }

// 只看登录文件在不在，不解密：画页面时会调到。过期与否联网时才知道。/ Only checks that the sign-in file exists,
// without decrypting, since page drawing calls it. Whether it expired is known only online.
bool weread_logged_in(void) { return weread_configure() && Storage.exists(WeReadStore::kSessionPath); }

esp_err_t weread_start_sync(void) { return start(Job{false, false, {}}, "正在同步微信读书"); }

esp_err_t weread_start_download(const char* book_id, bool epub) {
    if (!book_id || !book_id[0] || strlen(book_id) >= sizeof(Job::id)) return ESP_ERR_INVALID_ARG;
    Job job{true, epub, {}};
    snprintf(job.id, sizeof(job.id), "%s", book_id);
    return start(job, "正在下载");
}

bool weread_busy(void) { return s_running; }

void weread_snapshot(weread_snapshot_t* out) {
    if (!out) return;
    portENTER_CRITICAL(&s_lock);
    *out = s_snap;
    portEXIT_CRITICAL(&s_lock);
}

int weread_shelf_count(void) {
    if (!weread_configure()) return 0;
    HalFile shelf;
    uint32_t total = 0;
    return WeReadStore::openShelf(shelf, total) ? static_cast<int>(total) : 0;
}

bool weread_shelf_get(int index, weread_book_t* out) {
    if (!out || index < 0 || !weread_configure()) return false;
    memset(out, 0, sizeof(*out));
    HalFile shelf;
    uint32_t total = 0;
    WeReadStore::ShelfRecord rec;
    if (!WeReadStore::openShelf(shelf, total) || static_cast<uint32_t>(index) >= total ||
        !WeReadStore::readShelfRecord(shelf, static_cast<uint32_t>(index), rec) ||
        !memchr(rec.bookId, 0, sizeof(rec.bookId)) || !memchr(rec.title, 0, sizeof(rec.title)) ||
        !memchr(rec.author, 0, sizeof(rec.author)))
        return false;
    snprintf(out->id, sizeof(out->id), "%s", rec.bookId);
    snprintf(out->title, sizeof(out->title), "%s", rec.title);
    snprintf(out->author, sizeof(out->author), "%s", rec.author);
    // TXT 和 EPUB 同名不同扩展名，哪个在就算哪个（TXT 优先）。/ TXT and EPUB share a name; whichever exists counts
    // (TXT first).
    std::string book = WeReadStore::finalBookPath(rec);
    std::string txt = book;
    if (txt.size() > 5) txt.replace(txt.size() - 5, 5, ".txt");
    for (const std::string& p : {txt, book}) {
        if (!Storage.exists(p.c_str())) continue;
        snprintf(out->local, sizeof(out->local), "%s", Storage.map(p).c_str());
        break;
    }
    const std::string dir = WeReadStore::bookDirectory(rec.bookId);
    for (const char* name : {"/cover.source.jpg", "/cover.source.png"}) {
        if (Storage.exists((dir + name).c_str())) {
            snprintf(out->cover, sizeof(out->cover), "%s", Storage.map(dir + name).c_str());
            break;
        }
    }
    return true;
}

void weread_forget_book(const char* path) {
    if (!path || strncmp(path, WEREAD_BOOKS_DIR "/", sizeof(WEREAD_BOOKS_DIR))) return;
    const std::string cover = std::string(WEREAD_COVER_DIR "/") + base_name(path) + ".img";
    remove(cover.c_str());
}

void weread_drop_partial(void) {
    if (s_running || !weread_configure()) return;
    drop_other_chapters("");
}

bool weread_is_book(const char* path) {
    if (!path || strncmp(path, WEREAD_BOOKS_DIR "/", sizeof(WEREAD_BOOKS_DIR))) return false;
    return list_has(base_name(path));
}

esp_err_t weread_logout(void) {
    if (s_running || tomato_sync_busy()) return ESP_ERR_INVALID_STATE;
    if (!weread_configure()) return ESP_ERR_NOT_FOUND;
    // 整个缓存（登录信息、书架、每本书的封面和目录）改名挪进 .trash，马上生效；文件由后台慢慢删，界面不卡。
    // 留着的“同意说明”标记挪回来。/ Move the whole cache (sign-in, shelf, each book's cover and contents) into .trash,
    // effective at once; a background task deletes the files so the UI never stalls. The notice acceptance comes back.
    obfuscation::forgetKey();
    const bool accepted = WeReadStore::hasAcceptedDisclaimer();
    struct stat st;
    char to[96];
    if (stat(WEREAD_CACHE_DIR, &st) == 0 && (!trash_name(to, sizeof(to)) || rename(WEREAD_CACHE_DIR, to) != 0))
        return ESP_FAIL;
    empty_trash();
    if (accepted) WeReadStore::acceptDisclaimer();
    return ESP_OK;
}

}  // extern "C"
