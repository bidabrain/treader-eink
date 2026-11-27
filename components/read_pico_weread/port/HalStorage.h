/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：引擎的文件接口（CrossMux HalStorage / HalFile 的子集），用 stdio 和 dirent 实现在 TF 卡上。
 * 引擎写的是虚拟路径：/.crosspoint/… 是缓存（登录信息、书架、下载中间文件），/WeRead/… 是成书；configure 把两者
 * 换成真实目录。别的路径一律拒绝，引擎碰不到书库以外的文件。
 *
 * English: The engine's file API (a subset of CrossMux's HalStorage / HalFile) on the TF card through stdio and dirent.
 * The engine writes virtual paths: /.crosspoint/... is the cache (sign-in, shelf, download scratch) and /WeRead/... the
 * finished books; configure maps both to real directories. Any other path is refused, so the engine cannot reach
 * files outside them.
 *
 * 中文：每章用完就删的中间文件（分片、拼接、解码结果）放在 PSRAM 里，不落 TF 卡：引擎每章要建、写、读、删五六个这样的文件，
 * 在卡上做是下载慢的主因。分不到 PSRAM 时这些文件照样写卡。
 *
 * English: Per-chapter scratch files (shards, the joined and decoded data) live in PSRAM instead of on the card: the
 * engine creates, writes, reads and deletes five or six of them per chapter, which on the card was the main cost of a
 * download. Without PSRAM they still go to the card.
 */

#pragma once

#include <dirent.h>
#include <fcntl.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>

#include "Arduino.h"

/// PSRAM 里的一个中间文件。/ One scratch file in PSRAM.
struct MemFile {
    uint8_t* data = nullptr;
    size_t size = 0, cap = 0;
};

class HalFile {
    friend class HalStorage;
    MemFile* mem_ = nullptr;   // 不为空就是 PSRAM 文件 / Non-null for a PSRAM file
    size_t mpos_ = 0;
    FILE* fp_ = nullptr;
    DIR* dir_ = nullptr;
    char* buf_ = nullptr;   // stdio 缓冲（PSRAM）/ stdio buffer (PSRAM)
    std::string path_;   // 真实路径 / Real path
    bool attach(FILE* fp);

 public:
    HalFile() = default;
    ~HalFile() { close(); }
    HalFile(HalFile&& o) noexcept { *this = std::move(o); }
    HalFile& operator=(HalFile&& o) noexcept;
    HalFile(const HalFile&) = delete;
    HalFile& operator=(const HalFile&) = delete;

    bool isOpen() const { return fp_ || dir_ || mem_; }
    explicit operator bool() const { return isOpen(); }
    bool isDirectory() const { return dir_ != nullptr; }
    bool close();
    void flush();
    int read(void* buf, size_t count);
    int read();
    size_t write(const void* buf, size_t count);
    size_t write(const uint8_t* buf, size_t count) { return write(static_cast<const void*>(buf), count); }
    size_t write(uint8_t b) { return write(&b, 1); }
    bool seek(size_t pos) { return seek64(pos); }
    bool seekSet(size_t pos) { return seek64(pos); }
    bool seek64(uint64_t pos);
    bool seekCur(int64_t offset);
    size_t position() const;
    uint64_t fileSize64();
    size_t fileSize() { return static_cast<size_t>(fileSize64()); }
    size_t size() { return fileSize(); }
    int available() const;
    /// 目录里的下一项（跳过 . 和 ..）。/ The next directory entry (skipping . and ..).
    HalFile openNextFile();
    size_t getName(char* name, size_t len);
};

class HalStorage {
    // 并行下载时两个任务同时用：建、删、改名和内存文件表都在这把锁里；已打开文件的读写不加锁（各管各的文件）。
    // Two workers use it at once during parallel downloads: creating, deleting, renaming and the memory-file table
    // take this lock; reads and writes on open files do not (each worker owns its files).
    std::recursive_mutex mu_;
    std::string cache_, books_;
    std::map<std::string, MemFile*> mem_;   // 真实路径 → PSRAM 文件 / Real path to PSRAM file
    bool memPath(const std::string& real) const;
    MemFile* memFind(const std::string& real);
    friend class HalFile;
    /// 内存文件写不下时转存到卡上，返回打开的卡文件（位置在 pos）。/ Spill a memory file that cannot grow to the card;
    /// returns the open card file positioned at pos.
    FILE* spill(const std::string& real, MemFile* m, size_t pos);

 public:
    static HalStorage& getInstance();

    /// 缓存目录和成书目录（真实路径）。/ Cache and finished-book directories (real paths).
    void configure(const char* cache_root, const char* books_root);
    /// 虚拟路径换真实路径；不认识的返回空串。/ Virtual to real path; empty for unknown roots.
    std::string map(const std::string& path) const;
    /// 真实路径换回虚拟路径；不在两个目录下返回空串。/ Real back to virtual; empty outside both roots.
    std::string unmap(const std::string& real) const;

    bool exists(const char* path);
    bool remove(const char* path);
    bool rename(const char* from, const char* to);
    bool ensureDirectoryExists(const char* path);
    bool removeDir(const char* path);
    HalFile open(const char* path, int oflag = O_RDONLY);
    bool openFileForRead(const char* module, const char* path, HalFile& file);
    bool openFileForRead(const char* module, const std::string& path, HalFile& file) {
        return openFileForRead(module, path.c_str(), file);
    }
    bool openFileForWrite(const char* module, const char* path, HalFile& file);
    bool openFileForWrite(const char* module, const std::string& path, HalFile& file) {
        return openFileForWrite(module, path.c_str(), file);
    }
    String readFile(const char* path);
    bool writeFile(const char* path, const String& content);
    /// 放掉所有 PSRAM 中间文件（任务结束时）。/ Free every PSRAM scratch file (at the end of a job).
    void dropMemFiles();
};

#define Storage HalStorage::getInstance()
