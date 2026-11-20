# stb_image

`stb_image.h` v2.30 is an unmodified upstream file from
[nothings/stb](https://github.com/nothings/stb/blob/master/stb_image.h).
Its MIT/public-domain dual-license notice is retained at the end of the header.
`book_image.c` enables JPEG/PNG only and supplies a bounded PSRAM allocator.

`stb_image.h` v2.30 为未修改的上游文件，文件末尾保留 MIT/公共领域双许可声明。
`book_image.c` 仅启用 JPEG/PNG，并提供有总量限制的 PSRAM 分配器。

`tjpgd.c` and `tjpgd.h` are unmodified TJpgDec R0.03 sources from
[Espressif esp_jpeg](https://github.com/espressif/idf-extra-components/tree/master/esp_jpeg/tjpgd).
ChaN's original redistribution notice is retained in `tjpgd.c`.
`tjpgdcnf.h` is the local configuration: RGB output, scaling enabled,
512-byte input buffer, optimization level 1, no default Huffman tables.

`tjpgd.c` 与 `tjpgd.h` 保留上游原文和 ChaN 的再分发声明。
`tjpgdcnf.h` 是本地配置：RGB输出、启用缩小、512字节输入缓冲、一级优化、不补缺失的霍夫曼表。

The caller converts saturated RGB blocks to grayscale. This avoids luminance
wraparound in the upstream grayscale path with optimization level 1.
调用方将已饱和的RGB块转灰度，避免上游一级优化灰度路径中的亮度溢出回绕。

`book_image.c` (which includes `stb_image.h`), `tjpgd.c` and `jpegdec/jpegdec.c` are built with `-fwrapv`:
corrupt images can overflow signed integers in their IDCT and dequantization, and `-fwrapv` defines that as
wraparound, which is what GCC already produced.
`book_image.c`（含 `stb_image.h`）、`tjpgd.c` 与 `jpegdec/jpegdec.c` 用 `-fwrapv` 编译：损坏的图会让反变换和反量化里的有符号整数溢出，
`-fwrapv` 把它定义为回绕，与 GCC 原来生成的结果一致。

# JPEGDEC

`jpegdec/JPEGDEC.h`, `jpegdec/jpeg.inl` and `jpegdec/LICENSE` come from
[bitbank2/JPEGDEC](https://github.com/bitbank2/JPEGDEC) 1.8.5 (Apache-2.0).
`jpegdec/jpegdec.c` is the local C translation unit. It enables the upstream C API, which upstream only builds for
desktop and MCUXpresso targets, and predefines unsigned byte-order macros instead of upstream's signed `(*p)<<24`.
`book_jpegdec.c` only passes plain baseline images that pass its header check.
Local change to `jpeg.inl`: the four Huffman table builders check `iBitNum` before shifting by `iBitNum - N`, so no
shift count is negative. Each line is marked `Treader`.

`jpegdec/` 下的 `JPEGDEC.h`、`jpeg.inl`、`LICENSE` 来自 bitbank2/JPEGDEC 1.8.5（Apache-2.0）。
`jpegdec/jpegdec.c` 是本地编译单元：打开上游只给桌面和 MCUXpresso 编的 C 接口，并预先定义无符号的字节序宏，替代上游有符号的 `(*p)<<24`。
`book_jpegdec.c` 只把通过文件头检查的普通基线图交给它。
本地改动：`jpeg.inl` 的四处霍夫曼建表在按 `iBitNum - N` 移位前先判断 `iBitNum`，不再出现负的移位量；每行都标了 `Treader`。
