/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 用固件自己的书源代码（main/book）把测试书的章节表打印出来，给番茄安卓 App 的单元测试当标准答案。
 * 每章一行：序号、原文偏移、标题、UTF-8 字节数、FNV-1a 64 位哈希。用法见 gen_fixtures.sh。
 *
 * Print each test book's chapter table with the firmware's own book source (main/book) as the reference for the
 * Tomato Android app's unit tests. One line per chapter: index, source offset, title, UTF-8 length and FNV-1a 64-bit
 * hash. See gen_fixtures.sh.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "book_source.h"

static uint64_t fnv(const char* s, size_t n) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 1099511628211ULL; }
    return h;
}

int main(int argc, char** argv) {
    for (int a = 1; a < argc; a++) {
        const char* name = strrchr(argv[a], '/');
        name = name ? name + 1 : argv[a];
        if (book_open(argv[a]) != ESP_OK) { printf("FILE\t%s\tERROR\n", name); continue; }
        size_t n = book_chapter_count();
        printf("FILE\t%s\t%" PRIu32 "\t%zu\n", name, book_total_bytes(), n);
        for (size_t i = 0; i < n; i++) {
            char title[256] = "";
            book_chapter_title(i, title, sizeof(title));
            char* text = NULL;
            size_t len = 0;
            if (book_chapter_load(i, &text, &len) != ESP_OK) { printf("%zu\tLOADFAIL\n", i); continue; }
            printf("%zu\t%" PRIu32 "\t%s\t%zu\t%016" PRIx64 "\n", i, book_chapter_byte_offset(i), title, len, fnv(text, len));
            free(text);
            // EPUB 的块表：起点、长度、是不是标题。/ EPUB block table: offset, length, heading flag.
            if (book_kind() == BOOK_KIND_EPUB) {
                html_text_t blocks = {0};
                if (book_chapter_load_blocks(i, &blocks) == ESP_OK) {
                    printf("B\t%zu", i);
                    for (size_t b = 0; b < blocks.count; b++)
                        printf("\t%zu:%zu:%d", blocks.blocks[b].offset, blocks.blocks[b].len, blocks.blocks[b].heading ? 1 : 0);
                    printf("\n");
                    html_text_free(&blocks);
                }
            }
        }
        book_close();
    }
    return 0;
}
