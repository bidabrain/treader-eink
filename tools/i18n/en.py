#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Treader contributors
# SPDX-License-Identifier: Apache-2.0
"""英文版固件：编译前把界面上的中文换成英文，仓库源码不改。

  python tools/i18n/en.py extract      扫源码，把新出现的中文字符串加进 i18n/en.tsv（已有的译文保留）
  python tools/i18n/en.py check        查漏译、格式符不一致、内置字体没有的字、表里没人用的条目、网页英文版是否过期
  python tools/i18n/en.py apply OUT    把仓库复制到 OUT 并换掉中文；在 OUT 里 idf.py build 就是英文版

i18n/en.tsv 是 C 源码里的字符串：每条一行“中文<TAB>英文”，两边都是引号中间的原样写法（\\n、\\" 这些转义照写），
相邻的几段字面量算一条。注释和日志（ESP_LOG、printf）不动；传给 strcmp、strstr 这类比较函数的也不动，它们是解析
用的键（“第…章”、服务器给的分区名）。英文写“=”表示这条故意不翻（文件名这类）；英文空着是还没翻，照中文出。
“中文@@文件路径”是只对这个文件的译法（同一个词在不同地方意思不同时用）。以 # 开头的行是注释，extract 会在每条
前面写上它出现的位置。

设备上的网页（传书、验证器导入、服务器设置）整页翻：i18n/html/ 下按原路径放英文版，第一行注释记着照哪一版中文
（sha256）翻的；中文页改了，check 会提醒英文页要跟着改。

English firmware: before building, swap the Chinese UI text for English; the repository's sources stay unchanged.
Commands: extract (add new Chinese strings to i18n/en.tsv, keeping translations), check (missing translations, format
mismatches, characters the built-in font lacks, unused entries, stale English web pages), apply OUT (copy the
repository to OUT with the Chinese swapped; idf.py build in OUT gives the English firmware). i18n/en.tsv holds C
string literals, one "Chinese<TAB>English" entry per line written exactly as between the quotes (escapes kept),
adjacent pieces counting as one. Comments and logs (ESP_LOG, printf) are left alone, as are arguments of comparison
calls such as strcmp and strstr, which are parsing keys. "=" keeps an entry on purpose (file names and the like); an
empty English is untranslated and stays Chinese. "Chinese@@file path" is a translation for that file only. The
device's web pages are translated whole: i18n/html/ holds an English copy at the original path whose first line
records the sha256 of the Chinese page it follows; check warns when the Chinese page changes.
"""
from __future__ import annotations

import hashlib
import re
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TABLE = ROOT / "i18n" / "en.tsv"
HTML_DIR = ROOT / "i18n" / "html"
FONT = ROOT / "main" / "assets" / "builtin.ttf"
# 要翻译的源码目录；生成的表、第三方代码、搜索字表不碰。/ Source trees to translate; generated tables, vendored
# code and the search tables are left alone.
SCAN = ["main", "components/tomato", "components/read_pico_transfer", "components/read_pico_usb",
        "components/read_pico_totp", "components/read_pico"]
SKIP = ("fallback.h", "stb_truetype.h", "gbk_table.h", "/vendor/", "/build/")
HTML = (".html",)
CJK = re.compile("[　-ヿ㐀-鿿＀-￯]")
SPEC = re.compile(r"%[-+ #0]*\d*(?:\.\d+)?(?:hh|h|ll|l|z|j|t|L)?[diouxXeEfgGcsp%]")
LOG_CALLS = {"ESP_LOGE", "ESP_LOGW", "ESP_LOGI", "ESP_LOGD", "ESP_LOGV", "ESP_EARLY_LOGE", "ESP_EARLY_LOGW",
             "ESP_EARLY_LOGI", "ESP_DRAM_LOGE", "ESP_DRAM_LOGW", "printf", "puts"}
DATA_CALLS = {"strcmp", "strncmp", "strcasecmp", "strncasecmp", "strstr", "memcmp", "strchr", "strspn", "strcspn"}
HTML_COMMENT = re.compile(r"<!--.*?-->|/\*.*?\*/|(?<![:\"'\\])//[^\n]*", re.S)
# 英文版专有的设置：在线升级只认英文包。/ English-only settings: online updates accept only English packages.
VARIANT = [
    ("components/tomato/include/tomato.h", '#define TOMATO_OTA_SUFFIX "-upgrade.bin"',
     '#define TOMATO_OTA_SUFFIX "-upgrade-en.bin"'),
]
# 复制仓库时不要的：任何一层的构建产物，和根目录下与固件无关的服务器、App、文档、网站。/ Left out of the copy:
# build output at any level, plus the root-level servers, apps, docs and site that the firmware does not use.
_ANY = {".git", "build", "managed_components", "node_modules", ".gradle"}
_ROOT_ONLY = {"docs", "site", "rednote", "tomato", "weibo", "x", "webtoon", "auth", "i18n"}
# 会让英文版出错的问题（格式符不对会崩、字体缺字会画方框）；只是没翻的不算。/ Problems that break the English
# build (a format mismatch can crash, a missing glyph draws a box); untranslated entries do not count.
DANGER = [0]


def copy_ignore(directory: str, names):
    top = Path(directory).resolve() == ROOT
    return {n for n in names
            if n in _ANY or n.startswith("build-") or n.endswith(".apk") or (top and n in _ROOT_ONLY)}


def sources(root: Path):
    for d in SCAN:
        for p in sorted((root / d).rglob("*")):
            rel = p.relative_to(root).as_posix()
            if p.is_file() and p.suffix in (".c", ".h", *HTML) and not any(s in rel for s in SKIP):
                yield p


def enclosing_calls(src: str, pos: int):
    """pos 外面一层层没闭合的函数调用名，到这句话开头为止。/ Names of the unclosed calls around pos, out to the start
    of the statement."""
    names, depth, i = [], 0, pos - 1
    while i >= 0 and pos - i < 2000:   # 大的初始化表里不会有比较调用 / No comparison call spans a big initializer
        c = src[i]
        if c == ")":
            depth += 1
        elif c == "(":
            if depth:
                depth -= 1
            else:
                m = re.search(r"([A-Za-z_]\w*)\s*$", src[max(0, i - 64):i])
                names.append(m.group(1) if m else "")
        elif c in ";{}" and depth == 0:
            break
        i -= 1
    return names


def literal_runs(src: str):
    """C 源码里相邻字符串字面量的段：(开始, 结束, 引号里原样拼起来的文字, 要不要跳过)。跳过的是日志和比较函数的
    参数。/ Runs of adjacent string literals in C source: (start, end, raw text joined, skip). Log and comparison
    arguments are skipped."""
    i, n = 0, len(src)
    runs = []
    while i < n:
        c = src[i]
        if src.startswith("//", i):
            i = src.find("\n", i)
            i = n if i < 0 else i
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
        elif c == "'":
            j = i + 1
            while j < n and src[j] != "'":
                j += 2 if src[j] == "\\" else 1
            i = j + 1
        elif c == '"':
            start, parts, j = i, [], i
            while True:
                k = j + 1
                while k < n and src[k] != '"':
                    k += 2 if src[k] == "\\" else 1
                parts.append(src[j + 1:k])
                j = k + 1
                m = j
                while m < n and src[m] in " \t\r\n":
                    m += 1
                if m < n and src[m] == '"':
                    j = m
                    continue
                break
            text = "".join(parts)
            skip = False
            if CJK.search(text):
                calls = enclosing_calls(src, start)
                skip = any(c in LOG_CALLS or c in DATA_CALLS for c in calls)
            runs.append((start, j, text, skip))
            i = j
        else:
            i += 1
    return runs


def read_table(path: Path):
    table, order = {}, []
    if not path.exists():
        return table, order
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        zh, _, en = line.partition("\t")
        if zh not in table:
            order.append(zh)
        table[zh] = en
    return table, order


def collect_c(root: Path):
    """C 源码里要翻的中文字符串 → 出现位置。/ Chinese C strings to translate → where they occur."""
    found: dict[str, list[str]] = {}
    for p in sources(root):
        if p.suffix in HTML:
            continue
        rel = p.relative_to(root).as_posix()
        src = p.read_text(encoding="utf-8", errors="replace")
        for start, _, text, skip in literal_runs(src):
            if not skip and CJK.search(text):
                found.setdefault(text, []).append(f"{rel}:{src.count(chr(10), 0, start) + 1}")
    return found


def write_table(path: Path, found):
    table, order = read_table(path)
    keys = order + [k for k in found if k not in table]
    path.parent.mkdir(exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as f:
        f.write("# 英文版界面文字，见 tools/i18n/en.py。中文<TAB>英文；“=”不翻，空着是还没翻。\n")
        f.write("# English UI text, see tools/i18n/en.py. Chinese<TAB>English; \"=\" keeps it, empty is untranslated.\n")
        for k in keys:
            where = found.get(k.partition("@@")[0])
            f.write("# " + ((" ".join(where[:4]) + (" …" if len(where) > 4 else "")) if where else "(unused)") + "\n")
            f.write(f"{k}\t{table.get(k, '')}\n")
    new = sum(1 for k in found if k not in table)
    print(f"{path.name}: {len(keys)} entries, {new} new, {sum(1 for k in keys if not table.get(k))} untranslated")


def font_chars():
    try:
        from fontTools.ttLib import TTFont
    except ImportError:
        print("(fontTools not installed: skipping the font check)")
        return None
    return set(TTFont(str(FONT)).getBestCmap())


def specs(s: str):
    return [m.group(0) for m in SPEC.finditer(s)]


def check_table(found, cmap) -> int:
    table, order = read_table(TABLE)
    bad = 0
    for key in order:
        en = table[key]
        k, _, scope = key.partition("@@")
        if scope and not (ROOT / scope).is_file():
            print(f"override for a missing file: {key}")
            bad += 1
        if k not in found:
            print(f"unused: {key}")
        if not en:
            print(f"missing: {key}")
            bad += 1
            continue
        if en == "=":
            continue
        if specs(k) != specs(en):
            print(f"format: {k} -> {en}  ({specs(k)} vs {specs(en)})")
            bad += 1
            DANGER[0] += 1
        if CJK.search(en):
            print(f"chinese left: {k} -> {en}")
            bad += 1
            DANGER[0] += 1
        if cmap is not None:
            miss = sorted({ch for ch in en if ord(ch) > 0x7e and ord(ch) not in cmap})
            if miss:
                print(f"font lacks {''.join(miss)}: {en}")
                bad += 1
                DANGER[0] += 1
    for k in found:
        if k not in table:
            print(f"not in table: {k}  ({found[k][0]})")
            bad += 1
    return bad


def sha256(path: Path) -> str:
    # 换行统一成 LF 再算，Windows（CRLF）和 Linux 检出的结果一样。/ Hash with LF line endings so Windows (CRLF) and
    # Linux checkouts agree.
    return hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest()


def check_html() -> int:
    """英文网页齐不齐、跟不跟得上中文页。/ Whether the English pages exist and follow the current Chinese pages."""
    bad = 0
    for p in sources(ROOT):
        if p.suffix not in HTML:
            continue
        rel = p.relative_to(ROOT).as_posix()
        en = HTML_DIR / rel
        shown = en.relative_to(ROOT).as_posix()
        if not en.is_file():
            print(f"web page has no English copy: {rel} (expected {shown})")
            bad += 1
            continue
        text = en.read_text(encoding="utf-8")
        m = re.search(r"sha256:([0-9a-f]{64})", text.split("\n", 1)[0])
        if not m or m.group(1) != sha256(p):
            print(f"web page changed since its English copy: {rel} (update {shown}, then its first line to "
                  f"sha256:{sha256(p)})")
            bad += 1
        if CJK.search(HTML_COMMENT.sub("", text)):
            print(f"chinese left in {shown}")
            bad += 1
    return bad


def translate_c(src: str, table, rel: str = "") -> str:
    out, last = [], 0
    for start, end, text, skip in literal_runs(src):
        en = table.get(f"{text}@@{rel}") or table.get(text)
        if skip or not en or en == "=":
            continue
        out.append(src[last:start])
        out.append('"' + en + '"')
        last = end
    out.append(src[last:])
    return "".join(out)


def cmd_extract():
    write_table(TABLE, collect_c(ROOT))


def cmd_check() -> int:
    bad = check_table(collect_c(ROOT), font_chars()) + check_html()
    print(f"{bad} problem(s), {DANGER[0]} that break the English build")
    # 2：有会出错的问题；1：只是有没翻的或过期的网页；0：都好。/ 2: something breaks; 1: only untranslated text or
    # stale web pages; 0: clean.
    return 2 if DANGER[0] else 1 if bad else 0


def cmd_apply(out: str):
    dst = Path(out).resolve()
    if dst.exists():
        shutil.rmtree(dst)
    shutil.copytree(ROOT, dst, ignore=copy_ignore)
    table, _ = read_table(TABLE)
    n = 0
    for p in sources(dst):
        rel = p.relative_to(dst).as_posix()
        if p.suffix in HTML:
            # 网页整页换成英文版，去掉第一行的 sha256 注释。/ Swap the whole page for the English copy, minus its
            # sha256 comment line.
            en = HTML_DIR / rel
            if en.is_file():
                p.write_text(en.read_text(encoding="utf-8").split("\n", 1)[1], encoding="utf-8", newline="")
                n += 1
            continue
        src = p.read_text(encoding="utf-8", errors="surrogateescape")
        new = translate_c(src, table, rel)
        if new != src:
            p.write_text(new, encoding="utf-8", errors="surrogateescape", newline="")
            n += 1
    for rel, old, new in VARIANT:
        p = dst / rel
        src = p.read_text(encoding="utf-8")
        if old not in src:
            sys.exit(f"variant patch not found in {rel}: {old}")
        p.write_text(src.replace(old, new), encoding="utf-8", newline="")
    print(f"wrote {dst} ({n} files translated)")


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "extract":
        cmd_extract()
    elif cmd == "check":
        sys.exit(cmd_check())
    elif cmd == "apply" and len(sys.argv) > 2:
        cmd_apply(sys.argv[2])
    else:
        print(__doc__)
        sys.exit(2)
