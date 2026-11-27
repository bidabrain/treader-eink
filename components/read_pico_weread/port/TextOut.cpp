/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：TextOut.h 的实现。HTML 只做最简单的事：块级标签换段，script / style / head / title 里的字不要，图片忽略，
 * 实体还原；源码里的换行不算空格（中文正文里不该冒出空格）。
 *
 * English: Implementation of TextOut.h. HTML handling is minimal: block tags break paragraphs, text inside script /
 * style / head / title is dropped, images are ignored and entities restored; source line breaks are not spaces
 * (Chinese text should not gain stray spaces).
 */

#include "TextOut.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

#include "Epub/htmlEntities.h"

namespace {

// 和书库 book_txt.c 的 heading() 同一条规则。/ The same rule as heading() in the library's book_txt.c.
bool isHeading(const char* s) {
    while (*s == ' ' || *s == '\t') ++s;
    if (!strncmp(s, "Chapter", 7)) {
        s += 7;
        while (*s == ' ' || *s == '\t') ++s;
        return isdigit(static_cast<unsigned char>(*s)) != 0;
    }
    if (strncmp(s, "第", 3)) return false;
    s += 3;
    bool number = false;
    while (*s) {
        if (*s >= '0' && *s <= '9') {
            number = true;
            ++s;
            continue;
        }
        const unsigned char c = static_cast<unsigned char>(*s);
        const size_t n = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
        char token[5] = {};
        memcpy(token, s, n < 4 ? n : 4);
        if (n == 3 && strstr("零一二三四五六七八九十百千万两", token)) {
            number = true;
            s += n;
            continue;
        }
        return number && n == 3 && strstr("章节卷回集部篇", token);
    }
    return false;
}

void appendUtf8(std::string& s, uint32_t cp) {
    if (cp < 0x80) {
        s += static_cast<char>(cp);
    } else if (cp < 0x800) {
        s += static_cast<char>(0xC0 | cp >> 6), s += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += static_cast<char>(0xE0 | cp >> 12), s += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x110000) {
        s += static_cast<char>(0xF0 | cp >> 18), s += static_cast<char>(0x80 | (cp >> 12 & 0x3F));
        s += static_cast<char>(0x80 | (cp >> 6 & 0x3F)), s += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// 去掉首尾的半角空格、Tab 和全角空格。/ Trim ASCII spaces, tabs and ideographic spaces at both ends.
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    for (;;) {
        if (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
        else if (b - a >= 3 && !s.compare(a, 3, "　")) a += 3;
        else break;
    }
    for (;;) {
        if (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
        else if (b - a >= 3 && !s.compare(b - 3, 3, "　")) b -= 3;
        else break;
    }
    return s.substr(a, b - a);
}

// 一章的输出：攒一段，段落结束时写出；第一段和章名一样就不重复写。/ One chapter's output: gather a paragraph and
// write it when it ends; a first paragraph equal to the chapter name is not written twice.
struct Writer {
    HalFile& out;
    std::string title;
    std::string line;
    bool first = true;
    bool ok = true;

    Writer(HalFile& o, std::string t) : out(o), title(std::move(t)) {}

    void put(char c) { line += c; }
    void put(const char* s) { line += s; }
    void space() {
        if (!line.empty() && line.back() != ' ') line += ' ';
    }
    void paragraph() {
        const std::string t = trim(line);
        line.clear();
        if (t.empty()) return;
        if (first) {
            first = false;
            if (t == title) return;
        }
        ok = ok && out.write(t.data(), t.size()) == t.size() && out.write("\n", 1) == 1;
    }
};

enum class Mode { Text, Tag, Entity };

struct Html {
    Writer& w;
    Mode mode = Mode::Text;
    std::string tag, entity, skip;
    char quote = 0;

    explicit Html(Writer& writer) : w(writer) {}

    void endTag() {
        size_t i = 0;
        const bool closing = i < tag.size() && tag[i] == '/';
        if (closing) ++i;
        std::string name;
        while (i < tag.size() && (isalnum(static_cast<unsigned char>(tag[i])))) {
            name += static_cast<char>(tolower(static_cast<unsigned char>(tag[i])));
            ++i;
        }
        if (!skip.empty()) {
            if (closing && name == skip) skip.clear();
            return;
        }
        if (!closing && (name == "script" || name == "style" || name == "head" || name == "title")) {
            const bool selfClosing = !tag.empty() && tag.back() == '/';
            if (!selfClosing) skip = name;
            return;
        }
        static const char* const kBlock[] = {"p", "div", "br", "h1", "h2", "h3", "h4", "h5", "h6", "li", "tr",
                                             "blockquote", "section", "article", "header", "footer", "dt", "dd",
                                             "pre", "hr", "table", "ul", "ol", "figure", "aside"};
        for (const char* b : kBlock)
            if (name == b) {
                w.paragraph();
                return;
            }
    }

    void endEntity() {
        // entity 含开头的 &，不含 ;。/ entity holds the leading & but not the ;.
        if (skip.empty()) {
            uint32_t cp = 0;
            bool numeric = entity.size() > 2 && entity[1] == '#';
            if (numeric) {
                const bool hex = entity[2] == 'x' || entity[2] == 'X';
                cp = static_cast<uint32_t>(strtoul(entity.c_str() + (hex ? 3 : 2), nullptr, hex ? 16 : 10));
            }
            if (numeric && cp) {
                if (cp == 0xA0) w.space();
                else appendUtf8(w.line, cp);
            } else {
                const std::string full = entity + ";";
                const char* v = lookupHtmlEntity(full.c_str(), full.size());
                if (v && !strcmp(full.c_str(), "&nbsp;")) w.space();
                else if (v) w.put(v);
                else w.put(full.c_str());
            }
        }
        entity.clear();
        mode = Mode::Text;
    }

    void feed(char c) {
        switch (mode) {
        case Mode::Tag:
            if (quote) {
                if (c == quote) quote = 0;
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '>') {
                endTag();
                tag.clear();
                mode = Mode::Text;
            } else if (tag.size() < 64) {
                tag += c;
            }
            return;
        case Mode::Entity:
            if (c == ';') {
                endEntity();
                return;
            }
            if (entity.size() < 12 && (isalnum(static_cast<unsigned char>(c)) || c == '#')) {
                entity += c;
                return;
            }
            // 不是实体：原样当文字。/ Not an entity: keep it as text.
            if (skip.empty()) w.put(entity.c_str());
            entity.clear();
            mode = Mode::Text;
            break;
        case Mode::Text:
            break;
        }
        if (c == '<') {
            mode = Mode::Tag;
            quote = 0;
            return;
        }
        if (c == '&') {
            mode = Mode::Entity;
            entity = "&";
            return;
        }
        if (!skip.empty() || c == '\r' || c == '\n') return;
        if (c == ' ' || c == '\t') w.space();
        else w.put(c);
    }
};

}  // namespace

namespace TextOut {

bool writeHeader(HalFile& out, const char* title, const char* author) {
    std::string head = title ? title : "";
    head += "\n";
    if (author && author[0]) head += std::string("作者：") + author + "\n";
    head += "\n";
    return out.write(head.data(), head.size()) == head.size();
}

bool appendChapter(HalFile& out, HalFile& in, uint32_t index, const char* title, bool plain, uint8_t* buf,
                   size_t cap) {
    Writer w(out, trim(title ? title : ""));
    // 章名行：书库按“第 N 章”认目录，不像的就在前面补上序号。/ The title line: the library finds contents by
    // "第 N 章", so a title that does not look like one gets the number in front.
    std::string head = "\n";
    if (w.title.empty()) {
        char num[32];
        snprintf(num, sizeof(num), "第%u章", static_cast<unsigned>(index + 1));
        head += num;
    } else if (isHeading(w.title.c_str())) {
        head += w.title;
    } else {
        char num[32];
        snprintf(num, sizeof(num), "第%u章 ", static_cast<unsigned>(index + 1));
        head += num + w.title;
    }
    head += "\n";
    if (out.write(head.data(), head.size()) != head.size()) return false;
    Html html(w);
    for (;;) {
        const int got = in.read(buf, cap);
        if (got < 0) return false;
        if (got == 0) break;
        for (int i = 0; i < got; i++) {
            const char c = static_cast<char>(buf[i]);
            if (!plain) {
                html.feed(c);
            } else if (c == '\n') {
                w.paragraph();
            } else if (c != '\r') {
                w.put(c);
            }
        }
        if (!w.ok) return false;
    }
    w.paragraph();
    return w.ok;
}

}  // namespace TextOut
