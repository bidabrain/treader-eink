/*
 * SPDX-FileCopyrightText: 2026 XH Lab, Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 离线拼音输入法引擎：从 TF 卡载入 PICOIME1 词库到 PSRAM，按全拼或九键生成候选。
 * 不画界面、不碰触摸；键盘页只把编码串交给这里，再把候选画出来。
 *
 * Offline pinyin engine: loads a PICOIME1 lexicon from TF into PSRAM and produces
 * candidates for full-pinyin or T9 input. No drawing, no touch; the keyboard page
 * passes the composition string in and paints the candidates it gets back.
 *
 * 冻结：词库只读、最多 2 MiB；逐键查询只读 RAM，不打开文件。
 * Frozen: read-only lexicon, at most 2 MiB; keystroke queries touch RAM only, never files.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// 默认词库路径。/ Default lexicon path.
#define PINYIN_IME_DEFAULT_PATH "/sdcard/ime/pinyin-v1.bin"
/// 编码串最大长度（不含分词符）。/ Max composition length, separators excluded.
#define PINYIN_IME_MAX_INPUT 39
/// 单个候选最大 UTF-8 字节数（含结尾 0）。/ Max candidate UTF-8 bytes incl. NUL.
#define PINYIN_IME_CAND_TEXT 64

typedef enum {
    PINYIN_IME_QWERTY = 0, ///< 26 键全拼，编码为 a-z / Full pinyin, a-z
    PINYIN_IME_T9,         ///< 九键，编码为 2-9 / T9, digits 2-9
} pinyin_ime_mode_t;

typedef struct {
    char text[PINYIN_IME_CAND_TEXT]; ///< 候选文字 / Candidate text
    uint8_t consumed;                ///< 选中后从编码串头部消耗的字节数 / Composition bytes consumed on pick
    int16_t score;                   ///< 越小越靠前 / Lower ranks first
} pinyin_ime_cand_t;

/// 载入并校验词库；已载入同一文件时直接返回 true。失败时保留之前已载入的词库。
/// Load and validate a lexicon; returns true at once if already loaded. On failure the previous lexicon stays.
bool pinyin_ime_load(const char* path);
/// 释放词库内存。/ Free the lexicon.
void pinyin_ime_unload(void);
/// 词库是否可用。/ Whether a lexicon is loaded.
bool pinyin_ime_ready(void);
/// 词条数，未载入为 0。/ Entry count, 0 when not loaded.
uint32_t pinyin_ime_entry_count(void);

/// 按编码串生成候选，按 score 升序写入 out，返回个数。编码串可含 ' 作分词符。
/// Build candidates for a composition (may contain ' as a separator), sorted by score; returns the count.
int pinyin_ime_query(const char* composition, pinyin_ime_mode_t mode, pinyin_ime_cand_t* out, int max);

/// 字母对应的九键数字。/ T9 digit for a letter.
char pinyin_ime_t9_digit(char letter);

#ifdef __cplusplus
}
#endif
