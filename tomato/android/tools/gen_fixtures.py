#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Treader contributors
# SPDX-License-Identifier: Apache-2.0
# 生成番茄安卓 App 书源测试用的 TXT：各种编码、换行、章节标题和回退分段的边界情况。全部是原创文字。
# Generate TXT fixtures for the Tomato Android app's book-source tests: encodings, line endings, chapter titles and
# fallback parts at their edges. All text is original.
from pathlib import Path
import sys

out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
para = "河边的风吹过石桥，小舟慢慢经过。我们沿着小路走，把看到的景色写在纸上。"


def body(n):
    return "\n".join(f"{para}{i}" for i in range(n))


def write(name, text, enc="utf-8", bom=False, crlf=False):
    if crlf:
        text = text.replace("\n", "\r\n")
    data = text.encode(enc)
    if bom:
        data = b"\xef\xbb\xbf" + data
    (out / name).write_bytes(data)


nums = "一二三四五六七八九十"
chapters = "".join(f"第{nums[i % 10]}章 标题{i}\n{body(20)}\n" for i in range(12))
write("utf8_chapters.txt", "前言\n" + body(5) + "\n" + chapters)
write("utf8_bom_crlf.txt", chapters, bom=True, crlf=True)
write("gbk_chapters.txt", chapters, enc="gbk")
write("digits_units.txt", "".join(f"第{i}{u} 标题\n{body(8)}\n" for i, u in zip(range(1, 15), "章节卷回集部篇章节卷回集部篇")))
write("english.txt", "".join(f"Chapter {i}: The river\n" + "The boat drifts under the bridge.\n" * 30 for i in range(1, 9)))
write("english_space.txt", "".join(f"Chapter\t {i}\nline\n" * 1 for i in range(1, 6)) + "Chapter x\nnot a title\n")
# 不算标题：超过 40 字节、“第”后没有数字、数字后不是章节字。/ Not titles: over 40 bytes, no number, wrong unit.
write("not_titles.txt", "第一章" + "很长" * 20 + "\n正文\n第章 没有数字\n第一天 不是章\n第十二章 真的\n内容\n第十三章 也是\n内容\n")
# 没有章节：按 48 KB 后的第一个换行切段。/ No chapters: parts at the first newline after 48 KB.
write("no_chapters.txt", body(3000))
# 没有换行的超长文本：在 96 KB 上限前按字符切。/ One huge line: cut by character before the 96 KB cap.
write("one_line.txt", para * 3000)
# 一章超过 96 KB：整本回退分段。/ One chapter over 96 KB: the whole book falls back to parts.
write("huge_chapter.txt", "第一章 开始\n" + body(2500) + "\n第二章 结束\n" + body(5) + "\n")
# 只有一个标题：少于两章也回退。/ A single title: fewer than two chapters also falls back.
write("single_title.txt", "第一章 唯一\n" + body(10) + "\n")
# 无效 UTF-8 字节混在里面：判成 GBK。/ Invalid UTF-8 bytes inside: detected as GBK.
(out / "mixed_bytes.txt").write_bytes(("第一章 甲\n" + body(3) + "\n").encode("utf-8") + b"\xff\xfe\x80abc\n" +
                                      ("第二章 乙\n" + body(3) + "\n").encode("utf-8"))
# GBK 里的无效双字节和孤立高字节。/ Invalid GBK pairs and a lone high byte.
(out / "gbk_invalid.txt").write_bytes(("第一章 甲\n" + body(3) + "\n").encode("gbk") + b"\x81\x7f\xa1\n\xfe" +
                                      ("\n第二章 乙\n" + body(3) + "\n").encode("gbk"))
# 超过 2048 个标题：回退分段。/ Over 2048 titles: fall back to parts.
write("too_many.txt", "".join(f"第{i}章\n短\n" for i in range(1, 2100)))
write("empty.txt", "")
write("cr_only.txt", "第一章 甲\r正文\r第二章 乙\r正文\r")
print("ok")

# ---- EPUB ----
import zipfile

CONTAINER = '<?xml version="1.0"?><container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>'


def epub(name, files, opf, extra=None):
    with zipfile.ZipFile(out / name, "w") as z:
        z.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip")
        z.writestr("META-INF/container.xml", CONTAINER, zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/content.opf", opf, zipfile.ZIP_DEFLATED)
        for path, text in files.items():
            z.writestr(path, text, zipfile.ZIP_DEFLATED)


def xhtml(title, body):
    return ('<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE html>\n<html xmlns="http://www.w3.org/1999/xhtml"><head><title>'
            + title + '</title><style>p{margin:0}</style><script>var a="<p>no</p>";</script></head><body>' + body + '</body></html>')


chap = lambda i: xhtml(f"c{i}", f"<h1>第{i}章 标题</h1>" + "".join(
    f"<p>{para}&amp;&lt;{i}&gt;&nbsp;&#x4E2D;&#20013;  空白\n  折叠 <b>粗</b><br/>换行</p>" for _ in range(6))
    + '<!-- 注释 <p>不要</p> --><div><img src="../Images/a%20b.jpg" alt="x"/></div><p>&unknown; &#0; &#xD800;</p>')
files = {f"OEBPS/Text/c{i}.xhtml": chap(i) for i in range(1, 6)}
files["OEBPS/toc.ncx"] = ('<?xml version="1.0"?><ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1"><navMap>'
    '<navPoint id="n1"><navLabel><text>第一章  开头&amp;</text></navLabel><content src="Text/c1.xhtml"/>'
    '<navPoint id="n1a"><navLabel><text><![CDATA[小节 <一>]]></text></navLabel><content src="Text/c2.xhtml#frag"/></navPoint></navPoint>'
    '<navPoint id="n3"><navLabel><text>   第三章\n  跨行   </text></navLabel><content src="Text/c3.xhtml"/></navPoint>'
    '</navMap></ncx>')
files["OEBPS/nav.xhtml"] = ('<?xml version="1.0"?><html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><head><title>nav</title></head><body>'
    '<nav epub:type="toc"><ol><li><a href="Text/c1.xhtml">不该覆盖</a></li><li><a href="Text/c4.xhtml"><span>第四</span>章</a></li></ol></nav></body></html>')
manifest = "".join(f'<item id="c{i}" href="Text/c{i}.xhtml" media-type="application/xhtml+xml"/>' for i in range(1, 6))
opf = ('<?xml version="1.0"?><package xmlns="http://www.idpf.org/2007/opf" version="3.0"><metadata/><manifest>' + manifest
       + '<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>'
       '<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/></manifest>'
       '<spine toc="ncx">' + "".join(f'<itemref idref="c{i}"/>' for i in range(1, 6)) + '<itemref idref="c2"/></spine></package>')
epub("epub_mixed.epub", files, opf)

# 没有目录：标题都用“第 N 节”。/ No navigation: every title falls back to "第 N 节".
files2 = {f"OEBPS/c{i}.html": xhtml(f"c{i}", "<h2>节</h2>" + f"<p>{para * 20}</p>") for i in range(1, 4)}
opf2 = ('<?xml version="1.0"?><package xmlns="http://www.idpf.org/2007/opf" version="2.0"><manifest>'
        + "".join(f'<item id="c{i}" href="c{i}.html" media-type="text/html"/>' for i in range(1, 4))
        + '</manifest><spine>' + "".join(f'<itemref idref="c{i}"/>' for i in range(1, 4)) + '</spine></package>')
epub("epub_notoc.epub", files2, opf2)

# 目录坏了（标签没闭合）：不留半截标题。/ Broken NCX (unclosed tag): no partial titles remain.
files3 = dict(files2)
files3["OEBPS/toc.ncx"] = '<ncx><navMap><navPoint><navLabel><text>甲</text></navLabel><content src="c1.html"/></navPoint><navPoint>'
opf3 = opf2.replace('</manifest><spine>', '<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/></manifest><spine toc="ncx">')
epub("epub_badtoc.epub", files3, opf3)
print("epub ok")
