/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：PSRAM 不够时统一放掉能重建的缓存。各缓存登记一个释放函数，大块分配失败的地方调 mem_reclaim() 再试一次。
 *
 * English: Free rebuildable caches together when PSRAM runs short. Each cache registers a release function; a place
 * whose large allocation failed calls mem_reclaim() and tries once more.
 *
 * 冻结：只在主任务调用；释放函数只能放内存里有备份（TF 卡缓存、文件）的东西，不画屏、不碰正在显示的缓冲。
 * Frozen: main task only; release functions free only what is backed elsewhere (card caches, files), never draw and
 * never touch a buffer on screen.
 */
#pragma once

#include <stddef.h>

/// 释放函数：放掉自己的可重建缓存。/ A release function: drop its own rebuildable cache.
typedef void (*mem_reclaim_fn)(void);

/// 登记释放函数（重复登记只算一次，最多 12 个）。页面级的缓存在 on_enter 登记、on_exit 注销。
/// Register a release function (a repeat counts once, up to 12). Page caches register in on_enter and unregister in
/// on_exit.
void mem_reclaim_register(mem_reclaim_fn fn);
/// 注销释放函数。/ Unregister a release function.
void mem_reclaim_unregister(mem_reclaim_fn fn);
/// 调用所有释放函数，日志写原因和最大连续块的前后变化；返回之后的最大连续块。
/// Call every release function, logging the reason and the largest free block before and after; returns the block
/// after.
size_t mem_reclaim(const char* why);
