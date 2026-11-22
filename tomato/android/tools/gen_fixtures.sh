#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Treader contributors
# SPDX-License-Identifier: Apache-2.0
# 重新生成书源测试的标准答案：在 gcc 容器里用固件的 main/book 代码打印测试书的章节表。在仓库根目录运行。
# Regenerate the book-source reference: print the test books' chapter tables with the firmware's main/book code in a
# gcc container. Run from the repository root.
set -euo pipefail
res=tomato/android/app/src/test/resources
python tomato/android/tools/gen_fixtures.py "$res/books"
docker run --rm -v "$PWD":/src -w /src gcc:14 bash -c '
  apt-get update -qq >/dev/null && apt-get install -y -qq zlib1g-dev >/dev/null
  gcc -std=c11 -D_DEFAULT_SOURCE -O1 -Imain/book -Itools/book_epub_stubs -Itools/zip_host_stubs \
    tomato/android/tools/firmware_dump.c main/book/book_source.c main/book/book_txt.c main/book/gbk.c \
    main/book/book_epub.c main/book/zip_reader.c main/book/html_text.c main/book/book_image.c tools/book_epub_stubs/book_jpegdec_host.c main/book/vendor/tjpgd.c \
    -lz -o /tmp/dump
  /tmp/dump '"$res"'/books/* > '"$res"'/firmware_chapters.tsv'
echo "wrote $res/firmware_chapters.tsv"
