#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Treader contributors
# SPDX-License-Identifier: Apache-2.0
# 在仓库根运行：用固件的界面代码在电脑上画几屏公共控件，写到 build/ui-preview/*.png。
# Run from the repository root: draw the shared controls with the firmware's UI code into build/ui-preview/*.png.
set -euo pipefail
mkdir -p build/ui-preview
# 内置字体和固件一样嵌进程序。/ Embed the built-in font as the firmware does.
(cd main/assets && ld -r -b binary -o ../../build/ui-preview/builtin_ttf.o builtin.ttf)
gcc -std=gnu11 -O1 -w -Itools/ui_render_stubs -Imain/ui -Imain/font -Imain \
    tools/ui_render_host.c main/ui/tr_ui.c main/ui/ui_kit.c main/ui/datamatrix.c main/font/ttf_font.c \
    build/ui-preview/builtin_ttf.o -lz -lm -o build/ui-preview/render
build/ui-preview/render sdcard/fonts/ChillDuanSansVF.ttf build/ui-preview
