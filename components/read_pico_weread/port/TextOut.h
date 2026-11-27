/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：TXT 输出：把一章还原出来的内容（纯文本，或出版书的 HTML 片段）去掉标签和图片，按段写进书的 TXT。
 * 每章前面写一行书库认得的标题（“第 N 章 …”），目录靠它识别。
 *
 * English: TXT output: append one chapter's restored content (plain text, or a publisher book's HTML fragment) to the
 * book's TXT, tags and images dropped, one line per paragraph. Each chapter starts with a title line the library
 * recognizes ("第 N 章 …"), which is how its contents are found.
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "HalStorage.h"

namespace TextOut {

/// 书的开头：书名、作者和一个空行。/ The book's opening: title, author and a blank line.
bool writeHeader(HalFile& out, const char* title, const char* author);

/// 把 in 里的一章写到 out 末尾。index 从 0 开始；plain 为真时输入是纯文本，否则是 HTML。buf 是读文件用的工作区。
/// Append one chapter from in to out. index counts from 0; plain means the input is plain text, otherwise HTML. buf is
/// a work area for reading.
bool appendChapter(HalFile& out, HalFile& in, uint32_t index, const char* title, bool plain, uint8_t* buf,
                   size_t cap);

}  // namespace TextOut
