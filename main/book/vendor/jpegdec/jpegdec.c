/*
 * SPDX-FileCopyrightText: 2020 BitBank Software, Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：JPEGDEC 1.8.5 的 C 编译单元，只包含未修改的上游 jpeg.inl。
 *
 * English: C translation unit for JPEGDEC 1.8.5; it only includes the unmodified upstream jpeg.inl.
 *
 * 冻结：不改上游文件；只经 book_jpegdec.c 调用。
 * Frozen: do not edit the upstream files; call them only through book_jpegdec.c.
 */
// 上游只在 Linux / macOS / MCUXpresso / Win64 下编出 C 接口（另带几行 stdio 读文件），ESP-IDF 借 MCUXpresso 的开关打开它。
// Upstream only builds its C API (plus a few stdio file readers) for Linux / macOS / MCUXpresso / Win64; ESP-IDF
// borrows the MCUXpresso switch to enable it.
#define __MCUXPRESSO 1
// 上游在不允许非对齐访问的平台上用 (*p)<<24 拼字节，字节 >= 0x80 时有符号左移溢出；它允许预先定义这组宏，这里给无符号版本。
// On platforms without unaligned access upstream builds words with (*p)<<24, a signed overflow for bytes >= 0x80; it
// lets callers predefine these macros, so supply unsigned versions.
#define INTELSHORT(p) ((uint16_t)((uint16_t)(p)[0] | (uint16_t)(p)[1] << 8))
#define INTELLONG(p) ((uint32_t)(p)[0] | (uint32_t)(p)[1] << 8 | (uint32_t)(p)[2] << 16 | (uint32_t)(p)[3] << 24)
#define MOTOSHORT(p) ((uint16_t)((uint16_t)(p)[0] << 8 | (uint16_t)(p)[1]))
#define MOTOLONG(p) ((uint32_t)(p)[0] << 24 | (uint32_t)(p)[1] << 16 | (uint32_t)(p)[2] << 8 | (uint32_t)(p)[3])
#include "jpeg.inl"
