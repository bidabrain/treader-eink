#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Treader contributors
# SPDX-License-Identifier: Apache-2.0
# 在仓库根运行：用真实页面代码在电脑上画正常 / 大 / 特大三档，写到 build/page-render/<页>/*.png 并打印检查结果。
# 用法：tools/page_render/run.sh [页名…]（默认全部 harness）。在 gcc 容器里跑：
#   docker run --rm -v "$PWD:/w" -w /w gcc:14 bash tools/page_render/run.sh all
# SRC_ROOT 指向另一份源码时（compare.sh 用）画那份代码；OUT 改输出目录。每个页面用自己的目标文件目录，可以同时跑。
# Run from the repository root: draw real pages at the normal / large / largest sizes into
# build/page-render/<page>/*.png and print the checks. Usage: tools/page_render/run.sh [page…] (all harnesses by
# default), inside a gcc container as shown above. SRC_ROOT points at another source tree (compare.sh uses it); OUT
# changes the output directory. Each page builds in its own object directory, so pages can run at the same time.
set -euo pipefail
SRC=${SRC_ROOT:-.}
OUT=${OUT:-build/page-render}
CJSON=$(dirname "$(find managed_components -name cJSON.h | head -1)")
INC="-Itools/page_render/stubs -Itools/page_render -I$CJSON -I$SRC/main -I$SRC/main/app -I$SRC/main/ui -I$SRC/main/font \
-I$SRC/main/apps -I$SRC/main/book -I$SRC/main/factory -I$SRC/components/epdiy/src -I$SRC/components/epdiy/include"
for d in "$SRC"/components/*/include managed_components/*/include; do INC="$INC -I$d"; done
CFLAGS="-std=gnu11 -O1 -g -w -fno-pie -no-pie -DPAGE_RENDER_HOST ${HR_EXTRA_CFLAGS:-}"
COMMON="tools/page_render/page_render.c $SRC/main/ui/tr_ui.c $SRC/main/ui/ui_kit.c $SRC/main/ui/datamatrix.c \
$SRC/main/font/ttf_font.c $SRC/main/settings.c"
pages=("$@")
if [ ${#pages[@]} -eq 0 ]; then
    for h in tools/page_render/harness_*.c; do pages+=("$(basename "${h%.c}" | sed 's/^harness_//')"); done
fi
for p in "${pages[@]}"; do
    OBJ="$OUT/obj-$p"
    mkdir -p "$OBJ"
    (cd main/assets && ld -r -b binary -o "../../$OBJ/builtin_ttf.o" builtin.ttf)
    OBJS="$OBJ/builtin_ttf.o"
    for f in $COMMON; do
        o="$OBJ/$(basename "${f%.c}").o"
        gcc $CFLAGS $INC -c "$f" -o "$o"
        OBJS="$OBJS $o"
    done
    extra=""
    [ -f "tools/page_render/extra_$p.txt" ] && extra=$(sed "s#@SRC@#$SRC#g" "tools/page_render/extra_$p.txt")
    gcc $CFLAGS $INC "tools/page_render/harness_$p.c" $extra $OBJS -lz -lm \
        -Wl,--unresolved-symbols=ignore-all -Wl,--wrap=tr_tab_bar -Wl,--wrap=tr_fit -Wl,--wrap=ttf_draw_text_px \
        -Wl,--wrap=ttf_draw_text_px_bw -Wl,--wrap=tr_icon -Wl,--wrap=tr_icon_on -Wl,--wrap=time -Wl,--wrap=tr_pager -o "$OBJ/render"
    "$OBJ/render" sdcard/fonts/ChillDuanSansVF.ttf "$OUT"
done
