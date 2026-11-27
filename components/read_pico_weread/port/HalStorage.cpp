/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：HalStorage.h 的实现。只有微信读书的后台任务调用，不加锁；FATFS 自己串行化卡访问。
 *
 * English: Implementation of HalStorage.h. Only the WeRead worker calls it, so there is no lock; FATFS serializes
 * card access itself.
 */

#include "HalStorage.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <new>

#include "esp_heap_caps.h"

namespace {

constexpr char kCacheV[] = "/.crosspoint/weread";
constexpr char kBooksV[] = "/WeRead";
// 每个文件的 stdio 缓冲。默认只有 128 字节（CONFIG_FATFS_VFS_FSTAT_BLKSIZE=0），引擎边读边写中间文件，TF 卡小块读写
// 拖慢解码和转换好几倍；8 KB 放 PSRAM，不占内部 RAM。/ Per-file stdio buffer. The default is only 128 bytes
// (CONFIG_FATFS_VFS_FSTAT_BLKSIZE=0) and the engine streams its scratch files, so tiny card reads and writes slowed
// decoding and conversion several times over; 8 KB in PSRAM keeps it off internal RAM.
constexpr size_t kFileBuf = 8192;
// 单个 PSRAM 中间文件最多这么大；再大就写不进去，引擎按写卡失败处理。/ The largest PSRAM scratch file; beyond it writes
// fail and the engine treats it as a card write error.
constexpr size_t kMemMax = 4 * 1024 * 1024;
// PSRAM 最大连续块小于这个就不新建内存文件，照旧写卡，把余量留给界面和别的功能。/ Below this largest PSRAM block no
// new memory file is made and scratch goes to the card, leaving room for the UI and other features.
constexpr size_t kMemHeadroom = 1024 * 1024;
// 放进 PSRAM 的中间文件名（在每本书的缓存目录下，每章用完就删）。/ Scratch names kept in PSRAM (in each book's cache
// directory, deleted after every chapter).
constexpr const char* kMemNames[] = {"shard0.part", "shard1.part", "shard3.part", "encoded.part", "decoded.part"};

void mem_free(MemFile* m) {
    if (!m) return;
    heap_caps_free(m->data);
    delete m;
}

// 让 m 至少能放 need 字节；按 1.5 倍长。/ Make room for need bytes, growing by half.
bool mem_reserve(MemFile* m, size_t need) {
    if (need <= m->cap) return true;
    if (need > kMemMax) return false;
    size_t cap = m->cap ? m->cap + m->cap / 2 : 64 * 1024;
    if (cap < need) cap = need;
    if (cap > kMemMax) cap = kMemMax;
    auto* data = static_cast<uint8_t*>(heap_caps_realloc(m->data, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!data) return false;
    m->data = data, m->cap = cap;
    return true;
}

// path 以 root 开头，后面是结尾或 '/'。/ path starts with root followed by the end or '/'.
bool under(const std::string& path, const char* root, size_t n) {
    return !path.compare(0, n, root) && (path.size() == n || path[n] == '/');
}

}  // namespace

/* ---- HalFile ---- */

HalFile& HalFile::operator=(HalFile&& o) noexcept {
    if (this != &o) {
        close();
        mem_ = o.mem_, mpos_ = o.mpos_, fp_ = o.fp_, dir_ = o.dir_, buf_ = o.buf_, path_ = std::move(o.path_);
        o.mem_ = nullptr, o.fp_ = nullptr, o.dir_ = nullptr, o.buf_ = nullptr;
    }
    return *this;
}

// 接上打开的文件并配缓冲；PSRAM 不够就用默认缓冲，照样能用。/ Take an opened file and give it a buffer; without
// PSRAM it keeps the default buffer and still works.
bool HalFile::attach(FILE* fp) {
    fp_ = fp;
    if (!fp_) return false;
    buf_ = static_cast<char*>(heap_caps_malloc(kFileBuf, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buf_ && setvbuf(fp_, buf_, _IOFBF, kFileBuf) != 0) {
        heap_caps_free(buf_);
        buf_ = nullptr;
    }
    return true;
}

bool HalFile::close() {
    bool ok = true;
    if (fp_) ok = fclose(fp_) == 0;
    if (dir_) closedir(dir_);
    heap_caps_free(buf_);   // fclose 之后才能放 / Only after fclose
    mem_ = nullptr, mpos_ = 0, fp_ = nullptr, dir_ = nullptr, buf_ = nullptr;
    return ok;
}

void HalFile::flush() {
    if (fp_) {
        fflush(fp_);
        fsync(fileno(fp_));
    }
}

int HalFile::read(void* buf, size_t count) {
    if (mem_) {
        const size_t n = mpos_ >= mem_->size ? 0 : std::min(count, mem_->size - mpos_);
        memcpy(buf, mem_->data + mpos_, n);
        mpos_ += n;
        return static_cast<int>(n);
    }
    if (!fp_) return -1;
    const size_t n = fread(buf, 1, count, fp_);
    return n == 0 && ferror(fp_) ? -1 : static_cast<int>(n);
}

int HalFile::read() {
    uint8_t b;
    return read(&b, 1) == 1 ? b : -1;
}

size_t HalFile::write(const void* buf, size_t count) {
    if (mem_ && !mem_reserve(mem_, mpos_ + count)) {
        // PSRAM 不够或这一章特别大：已写的内容转存到卡上，接着写卡，下载不会因此失败。/ PSRAM is short or the chapter
        // is huge: move what is written to the card and keep writing there, so the download does not fail.
        FILE* fp = Storage.spill(path_, mem_, mpos_);
        mem_ = nullptr;
        if (!attach(fp)) return 0;
    }
    if (mem_) {
        memcpy(mem_->data + mpos_, buf, count);
        mpos_ += count;
        if (mpos_ > mem_->size) mem_->size = mpos_;
        return count;
    }
    return fp_ ? fwrite(buf, 1, count, fp_) : 0;
}

bool HalFile::seek64(uint64_t pos) {
    if (mem_) {
        if (pos > mem_->size) return false;
        mpos_ = static_cast<size_t>(pos);
        return true;
    }
    return fp_ && fseek(fp_, static_cast<long>(pos), SEEK_SET) == 0;
}

bool HalFile::seekCur(int64_t offset) {
    if (mem_) return seek64(static_cast<uint64_t>(static_cast<int64_t>(mpos_) + offset));
    return fp_ && fseek(fp_, static_cast<long>(offset), SEEK_CUR) == 0;
}

size_t HalFile::position() const {
    if (mem_) return mpos_;
    const long p = fp_ ? ftell(fp_) : -1;
    return p < 0 ? 0 : static_cast<size_t>(p);
}

uint64_t HalFile::fileSize64() {
    if (mem_) return mem_->size;
    if (!fp_) return 0;
    fflush(fp_);
    struct stat st;
    return fstat(fileno(fp_), &st) == 0 ? static_cast<uint64_t>(st.st_size) : 0;
}

int HalFile::available() const {
    if (mem_) return mpos_ >= mem_->size ? 0 : static_cast<int>(mem_->size - mpos_);
    if (!fp_) return 0;
    struct stat st;
    if (fstat(fileno(fp_), &st) != 0) return 0;
    const long p = ftell(fp_);
    return p < 0 || p >= st.st_size ? 0 : static_cast<int>(st.st_size - p);
}

HalFile HalFile::openNextFile() {
    HalFile out;
    if (!dir_) return out;
    for (struct dirent* e; (e = readdir(dir_));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        const std::string child = path_ + "/" + e->d_name;
        struct stat st;
        if (stat(child.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) out.dir_ = opendir(child.c_str());
        else out.attach(fopen(child.c_str(), "rb"));
        if (!out.isOpen()) continue;
        out.path_ = child;
        break;
    }
    return out;
}

size_t HalFile::getName(char* name, size_t len) {
    if (!name || !len) return 0;
    const size_t slash = path_.find_last_of('/');
    const std::string base = slash == std::string::npos ? path_ : path_.substr(slash + 1);
    const size_t n = base.size() < len - 1 ? base.size() : len - 1;
    memcpy(name, base.data(), n);
    name[n] = '\0';
    return n;
}

/* ---- HalStorage ---- */

HalStorage& HalStorage::getInstance() {
    static HalStorage instance;
    return instance;
}

bool HalStorage::memPath(const std::string& real) const {
    if (cache_.empty() || !under(real, cache_.c_str(), cache_.size())) return false;
    const size_t slash = real.find_last_of('/');
    const char* base = real.c_str() + (slash == std::string::npos ? 0 : slash + 1);
    for (const char* name : kMemNames)
        if (!strcmp(base, name)) return true;
    return false;
}

MemFile* HalStorage::memFind(const std::string& real) {
    const auto it = mem_.find(real);
    return it == mem_.end() ? nullptr : it->second;
}

FILE* HalStorage::spill(const std::string& real, MemFile* m, size_t pos) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    FILE* fp = fopen(real.c_str(), "w+b");
    const bool ok = fp && fwrite(m->data, 1, m->size, fp) == m->size && fseek(fp, static_cast<long>(pos), SEEK_SET) == 0;
    mem_.erase(real);
    mem_free(m);
    if (!ok && fp) {
        fclose(fp);
        fp = nullptr;
    }
    return fp;
}

void HalStorage::dropMemFiles() {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    for (auto& kv : mem_) mem_free(kv.second);
    mem_.clear();
}

void HalStorage::configure(const char* cache_root, const char* books_root) {
    cache_ = cache_root ? cache_root : "";
    books_ = books_root ? books_root : "";
}

std::string HalStorage::map(const std::string& path) const {
    if (path.find("/../") != std::string::npos || (path.size() >= 3 && !path.compare(path.size() - 3, 3, "/.."))) return {};
    if (!cache_.empty() && under(path, kCacheV, sizeof(kCacheV) - 1)) return cache_ + path.substr(sizeof(kCacheV) - 1);
    // 引擎建缓存前先建 /.crosspoint：对应缓存的上一级（/sdcard/weixin）。/ The engine creates /.crosspoint before its
    // cache: that is the cache's parent (/sdcard/weixin).
    if (!cache_.empty() && path == "/.crosspoint") return cache_.substr(0, cache_.find_last_of('/'));
    if (!books_.empty() && under(path, kBooksV, sizeof(kBooksV) - 1)) return books_ + path.substr(sizeof(kBooksV) - 1);
    return {};
}

std::string HalStorage::unmap(const std::string& real) const {
    if (!books_.empty() && under(real, books_.c_str(), books_.size())) return kBooksV + real.substr(books_.size());
    if (!cache_.empty() && under(real, cache_.c_str(), cache_.size())) return kCacheV + real.substr(cache_.size());
    return {};
}

bool HalStorage::exists(const char* path) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    const std::string p = map(path ? path : "");
    if (memFind(p)) return true;
    struct stat st;
    return !p.empty() && stat(p.c_str(), &st) == 0;
}

bool HalStorage::remove(const char* path) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    const std::string p = map(path ? path : "");
    if (MemFile* m = memFind(p)) {
        mem_free(m);
        mem_.erase(p);
        return true;
    }
    return !p.empty() && unlink(p.c_str()) == 0;
}

bool HalStorage::rename(const char* from, const char* to) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    const std::string a = map(from ? from : ""), b = map(to ? to : "");
    // 中间文件只在 PSRAM 里改名；不会改名到卡上的正式文件。/ Scratch files are only renamed within PSRAM; they never
    // become files on the card.
    if (MemFile* m = memFind(a)) {
        if (!memPath(b)) return false;
        if (MemFile* old = memFind(b)) mem_free(old);
        mem_.erase(a);
        mem_[b] = m;
        return true;
    }
    return !a.empty() && !b.empty() && ::rename(a.c_str(), b.c_str()) == 0;
}

bool HalStorage::ensureDirectoryExists(const char* path) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    const std::string p = map(path ? path : "");
    if (p.empty()) return false;
    // 逐级建目录；已经存在不算错（FAT 对挂载点 mkdir 报 EINVAL，所以先 stat）。/ Create level by level; existing
    // is fine (FAT returns EINVAL for mkdir on a mount point, so stat first).
    for (size_t i = 1; i <= p.size(); i++) {
        if (i < p.size() && p[i] != '/') continue;
        const std::string part = p.substr(0, i);
        struct stat st;
        if (stat(part.c_str(), &st) == 0) {
            if (!S_ISDIR(st.st_mode)) return false;
            continue;
        }
        if (mkdir(part.c_str(), 0777) != 0 && errno != EEXIST) return false;
    }
    return true;
}

static bool remove_tree(const std::string& p) {
    DIR* d = opendir(p.c_str());
    if (!d) return unlink(p.c_str()) == 0;
    bool ok = true;
    for (struct dirent* e; (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        ok = remove_tree(p + "/" + e->d_name) && ok;
    }
    closedir(d);
    return rmdir(p.c_str()) == 0 && ok;
}

bool HalStorage::removeDir(const char* path) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    const std::string p = map(path ? path : "");
    struct stat st;
    return !p.empty() && stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && remove_tree(p);
}

HalFile HalStorage::open(const char* path, int oflag) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    HalFile f;
    const std::string p = map(path ? path : "");
    if (p.empty()) return f;
    if (MemFile* m = memFind(p)) {
        f.mem_ = m;
        f.path_ = p;
        return f;
    }
    struct stat st;
    if ((oflag & O_ACCMODE) == O_RDONLY && stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        f.dir_ = opendir(p.c_str());
    } else if ((oflag & O_ACCMODE) == O_RDWR) {
        FILE* fp = fopen(p.c_str(), "r+b");
        if (!fp && (oflag & O_CREAT)) fp = fopen(p.c_str(), "w+b");
        f.attach(fp);
    } else {
        f.attach(fopen(p.c_str(), "rb"));
    }
    if (f.isOpen()) f.path_ = p;
    return f;
}

bool HalStorage::openFileForRead(const char*, const char* path, HalFile& file) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    file = HalFile();
    const std::string p = map(path ? path : "");
    if (MemFile* m = memFind(p)) {
        file.mem_ = m;
        file.path_ = p;
        return true;
    }
    if (p.empty() || !file.attach(fopen(p.c_str(), "rb"))) return false;
    file.path_ = p;
    return true;
}

bool HalStorage::openFileForWrite(const char*, const char* path, HalFile& file) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    file = HalFile();
    const std::string p = map(path ? path : "");
    if (memPath(p)) {
        // 写一个中间文件：放进 PSRAM（同名的旧内容清空）；分不到就退回写卡。/ Writing a scratch file: keep it in
        // PSRAM (truncating any old one); fall back to the card when PSRAM is short.
        MemFile* m = memFind(p);
        if (!m && heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= kMemHeadroom) {
            m = new (std::nothrow) MemFile();
            if (m && !mem_reserve(m, 64 * 1024)) {
                delete m;
                m = nullptr;
            }
            if (m) mem_[p] = m;
        }
        if (m) {
            m->size = 0;
            file.mem_ = m;
            file.path_ = p;
            return true;
        }
    }
    if (p.empty() || !file.attach(fopen(p.c_str(), "w+b"))) return false;
    file.path_ = p;
    return true;
}

String HalStorage::readFile(const char* path) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    HalFile f;
    if (!openFileForRead("WR", path, f)) return String();
    std::string out(f.fileSize(), '\0');
    const int n = f.read(out.data(), out.size());
    out.resize(n > 0 ? static_cast<size_t>(n) : 0);
    return String(std::move(out));
}

bool HalStorage::writeFile(const char* path, const String& content) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    HalFile f;
    if (!openFileForWrite("WR", path, f)) return false;
    const bool ok = f.write(content.data(), content.size()) == content.size();
    f.flush();
    return f.close() && ok;
}
