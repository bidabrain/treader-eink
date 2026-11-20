#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Treader contributors
# SPDX-License-Identifier: Apache-2.0
# 在仓库根运行：用 build/base-tree（改动前的源码，git worktree）画同样的 100% 场景，和当前代码的 100% 图逐字节比对。
# 先跑 tools/page_render/run.sh 生成当前的图。用法：tools/page_render/compare.sh 页名…
# 准备基线：git worktree add --detach build/base-tree <改动前的提交>
# Run from the repository root: draw the same 100% scenes with build/base-tree (the pre-change sources, a git
# worktree) and compare them byte for byte with the current 100% shots. Run tools/page_render/run.sh first.
# Usage: compare.sh page…  Set up the baseline with: git worktree add --detach build/base-tree <pre-change commit>
set -euo pipefail
BASE_TREE=build/base-tree
[ -d "$BASE_TREE/main" ] || { echo "missing $BASE_TREE (git worktree add --detach $BASE_TREE <commit>)"; exit 2; }
status=0
for p in "$@"; do
    SRC_ROOT=$BASE_TREE OUT=build/page-render-base HR_EXTRA_CFLAGS=-DHR_BASELINE \
        bash tools/page_render/run.sh "$p" > "build/page-render-base-$p.log" 2>&1 || { echo "BASE BUILD FAILED $p"; cat "build/page-render-base-$p.log"; status=1; continue; }
    for f in build/page-render-base/"$p"/*_100.png; do
        name=$(basename "$f")
        if cmp -s "$f" "build/page-render/$p/$name"; then echo "same  $p/$name"
        else echo "DIFF  $p/$name"; status=1; fi
    done
done
exit $status
