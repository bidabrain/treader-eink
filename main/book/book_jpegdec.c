/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：基线 JPEG 快速通道。先自己读文件头，只把 JPEGDEC 测过的普通基线图交给它，解成灰度并在解码时缩小；
 * 其余一律返回 false，由 book_image.c 的 tjpgd / stb_image 原路径处理。
 *
 * English: Baseline JPEG fast path. Read the header ourselves and hand only plain baseline images JPEGDEC was tested
 * on to it, decoding to gray and scaling while decoding; everything else returns false and stays on book_image.c's
 * original tjpgd / stb_image path.
 *
 * 冻结：渐进、4:1:1 等非常见采样、CMYK / RGB、非交错扫描、算术编码、16 位量化表、缺表或 DNL 都不进 JPEGDEC
 * （它对彩色渐进灰度输出会越界，对 4:1:1 会除零）；放宽条件前先在主机上用 ASan 和模糊测试验证。
 * Frozen: progressive, 4:1:1 and other unusual sampling, CMYK / RGB, non-interleaved scans, arithmetic coding,
 * 16-bit quant tables, missing tables or DNL never reach JPEGDEC (it overruns on color progressive gray output and
 * divides by zero on 4:1:1); verify on the host with ASan and fuzzing before widening the gate.
 * 每个 MCU 都检查读位置，越过已读入数据就中止：上游遇到坏码流后读指针会一直越界。
 * Check the read position after every MCU and abort once it passes the buffered data: upstream lets the read pointer
 * run past the buffer on a broken stream.
 */
#include "book_jpegdec.h"
#include "esp_heap_caps.h"
#include <string.h>
#include "vendor/jpegdec/JPEGDEC.h"
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#define PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define SIDE_MAX 16384u

static unsigned be16(const uint8_t* p) { return (unsigned)p[0] << 8 | p[1]; }

/* ---- 文件头检查 / Header gate ---- */

bool book_jpegdec_eligible(const uint8_t* d, size_t n, unsigned* width, unsigned* height) {
    if (!d || n < 4 || d[0] != 0xff || d[1] != 0xd8) return false;
    size_t i = 2;
    unsigned comps = 0, w = 0, h = 0, dqt = 0, dht = 0;
    uint8_t ids[3] = {0}, tq[3] = {0};
    int adobe = -1;
    bool sof = false;
    while (i + 4 <= n) {
        if (d[i] != 0xff) return false;
        const uint8_t m = d[i + 1];
        if (m == 0xff) { ++i; continue; }  // 填充字节 / Fill byte
        // SOS 之前不该出现无长度标记。/ No length-less markers belong before SOS.
        if (m == 0x00 || m == 0x01 || (m >= 0xd0 && m <= 0xd9)) return false;
        const unsigned len = be16(d + i + 2);
        if (len < 2 || len > n - i - 2) return false;
        const uint8_t* s = d + i + 4;
        const unsigned sl = len - 2;
        if (m == 0xc0) {
            if (sof || sl < 6) return false;
            sof = true;
            h = be16(s + 1); w = be16(s + 3); comps = s[5];
            if (s[0] != 8 || !w || !h || w > SIDE_MAX || h > SIDE_MAX) return false;
            if ((comps != 1 && comps != 3) || sl != 6u + 3u * comps) return false;
            for (unsigned c = 0; c < comps; ++c) {
                const uint8_t hv = s[7 + 3 * c];
                ids[c] = s[6 + 3 * c]; tq[c] = s[8 + 3 * c];
                if (tq[c] > 3) return false;
                // 只收 1x1、2x1、1x2、2x2 的亮度，色度必须 1x1。/ Luma 1x1, 2x1, 1x2 or 2x2 only; chroma must be 1x1.
                if (c == 0 && comps == 3) { if (hv != 0x11 && hv != 0x21 && hv != 0x12 && hv != 0x22) return false; }
                else if (hv != 0x11) return false;
            }
            for (unsigned a = 0; a < comps; ++a) for (unsigned b = a + 1; b < comps; ++b) if (ids[a] == ids[b]) return false;
            // 分量号是 R、G、B 的是 RGB 存储，不是 YCbCr。/ Component ids R, G, B mean RGB storage, not YCbCr.
            if (comps == 3 && ids[0] == 'R' && ids[1] == 'G' && ids[2] == 'B') return false;
        } else if (m >= 0xc1 && m <= 0xcf && m != 0xc4) {
            return false;  // 其他 SOF、DAC、JPG 扩展 / Other SOF, DAC, JPG extensions
        } else if (m == 0xdb) {
            for (unsigned k = 0; k < sl; k += 65) {
                // 只收 8 位量化表。/ 8-bit quant tables only.
                if (sl - k < 65 || (s[k] >> 4) != 0 || (s[k] & 15) > 3) return false;
                dqt |= 1u << (s[k] & 15);
            }
        } else if (m == 0xc4) {
            for (unsigned k = 0; k < sl;) {
                if (sl - k < 17) return false;
                const unsigned tc = s[k] >> 4, th = s[k] & 15;
                unsigned count = 0;
                for (unsigned j = 1; j <= 16; ++j) count += s[k + j];
                if (tc > 1 || th > 1 || !count || count > 256 || sl - k - 17 < count) return false;
                dht |= 1u << (tc * 2 + th);
                k += 17 + count;
            }
        } else if (m == 0xee && sl >= 12 && !memcmp(s, "Adobe", 5)) {
            adobe = s[11];
        } else if (m == 0xdc) {
            return false;  // DNL
        } else if (m == 0xda) {
            if (!sof || sl < 1 || s[0] != comps || sl != 4u + 2u * comps) return false;
            for (unsigned c = 0; c < comps; ++c) {
                // 交错扫描，分量顺序与 SOF 一致，用到的表都已定义。
                // Interleaved scan in SOF order, with every referenced table defined.
                const unsigned td = s[2 + 2 * c] >> 4, ta = s[2 + 2 * c] & 15;
                if (s[1 + 2 * c] != ids[c] || td > 1 || ta > 1) return false;
                if (!(dht & 1u << td) || !(dht & 1u << (2 + ta)) || !(dqt & 1u << tq[c])) return false;
            }
            if (s[1 + 2 * comps] != 0 || s[2 + 2 * comps] != 63 || s[3 + 2 * comps] != 0) return false;
            // Adobe 标记里变换为 0 的三分量图是 RGB。/ A three-component image with Adobe transform 0 is RGB.
            if (comps == 3 && adobe == 0) return false;
            if (width) *width = w;
            if (height) *height = h;
            return true;
        }
        i += 2 + len;
    }
    return false;
}

/* ---- 解码 / Decoding ---- */

typedef struct {
    JPEGIMAGE* jpg;
    uint8_t* out;
    bool overrun;
    unsigned width, height, scaled_width, scaled_height, blocks;
} draw_ctx_t;

// 把一块缩小后的像素按最近邻映射到输出图，规则同 book_image.c 的 jpeg_write。
// Map one block of scaled pixels to the output by nearest neighbor, matching jpeg_write in book_image.c.
static int draw_block(JPEGDRAW* dr) {
    draw_ctx_t* c = dr->pUser;
    if (!c) return 0;
    // 读位置越过已读入的数据说明码流坏了；上游此后不再补数据也不复位，读指针会一路越界，立即停。
    // A read position past the buffered data means a broken stream; upstream then neither refills nor resets, so the
    // read pointer would keep running past the buffer. Stop now.
    if (c->jpg->iVLCOff > c->jpg->iVLCSize || c->jpg->iVLCOff > JPEG_FILE_BUF_SIZE) { c->overrun = true; return 0; }
    if (dr->x < 0 || dr->y < 0 || dr->iWidthUsed <= 0 || dr->iHeight <= 0 || dr->iWidth < dr->iWidthUsed) return 1;
    const unsigned left = (unsigned)dr->x, top = (unsigned)dr->y;
    if (left >= c->scaled_width || top >= c->scaled_height) return 1;
    unsigned right = left + (unsigned)dr->iWidthUsed - 1, bottom = top + (unsigned)dr->iHeight - 1;
    if (right >= c->scaled_width) right = c->scaled_width - 1;
    if (bottom >= c->scaled_height) bottom = c->scaled_height - 1;
    const unsigned x0 = (unsigned)(((uint64_t)left * c->width + c->scaled_width - 1) / c->scaled_width);
    const unsigned x1 = (unsigned)(((uint64_t)(right + 1) * c->width + c->scaled_width - 1) / c->scaled_width);
    const unsigned y0 = (unsigned)(((uint64_t)top * c->height + c->scaled_height - 1) / c->scaled_height);
    const unsigned y1 = (unsigned)(((uint64_t)(bottom + 1) * c->height + c->scaled_height - 1) / c->scaled_height);
    const uint8_t* src = (const uint8_t*)dr->pPixels;
    for (unsigned y = y0; y < y1 && y < c->height; ++y) {
        const unsigned sy = (unsigned)((uint64_t)y * c->scaled_height / c->height) - top;
        if (sy > bottom - top) continue;
        for (unsigned x = x0; x < x1 && x < c->width; ++x) {
            const unsigned sx = (unsigned)((uint64_t)x * c->scaled_width / c->width) - left;
            if (sx > right - left) continue;
            c->out[(size_t)y * c->width + x] = src[(size_t)sy * (unsigned)dr->iWidth + sx];
        }
    }
#ifdef ESP_PLATFORM
    // 每个 MCU 回调一次，每 256 个让出一次 CPU，避免空闲任务看门狗饥饿。
    // One callback per MCU; yield every 256 so idle watchdog tasks can run.
    if (!(++c->blocks % 256)) vTaskDelay(1);
#endif
    return 1;
}

bool book_jpegdec_decode(const uint8_t* data, size_t len, size_t budget,
                         uint8_t** pixels, uint16_t* width, uint16_t* height) {
    unsigned iw = 0, ih = 0;
    if (!pixels || !width || !height || len > INT32_MAX || !book_jpegdec_eligible(data, len, &iw, &ih)) return false;
    unsigned w, h;
    book_image_out_size(iw, ih, budget, &w, &h);
    if ((size_t)w * h > budget) return false;
    unsigned scale = 0;
    // 至多两倍放大，与 tjpgd 路径一致。/ At most 2x enlargement, as on the tjpgd path.
    while (scale < 3 && (iw >> (scale + 1)) >= (w + 1) / 2 && (ih >> (scale + 1)) >= (h + 1) / 2) ++scale;
    static const int OPTIONS[4] = {0, JPEG_SCALE_HALF, JPEG_SCALE_QUARTER, JPEG_SCALE_EIGHTH};
    draw_ctx_t ctx = {
        .overrun = false,
        .width = w, .height = h,
        .scaled_width = (iw + (1u << scale) - 1) >> scale,
        .scaled_height = (ih + (1u << scale) - 1) >> scale,
    };
    JPEGIMAGE* jpg = heap_caps_calloc(1, sizeof(*jpg), PSRAM_CAPS);
    ctx.jpg = jpg;
    ctx.out = heap_caps_malloc((size_t)w * h, PSRAM_CAPS);
    bool ok = false;
    if (!jpg || !ctx.out) goto done;
    memset(ctx.out, 255, (size_t)w * h);
    if (!JPEG_openRAM(jpg, (uint8_t*)data, (int)len, draw_block)) goto done;
    // 两边读到的尺寸不一致就不信它。/ Distrust a size that disagrees with our header read.
    if (JPEG_getWidth(jpg) != (int)iw || JPEG_getHeight(jpg) != (int)ih) goto close;
    JPEG_setPixelType(jpg, EIGHT_BIT_GRAYSCALE);
    // 每解完一个 MCU 就回调，让越界检查最多晚一个 MCU（单个 MCU 至多读约 1.5 KB，落在结构体内后面的表里）。
    // Call back after every MCU so the overrun check lags by at most one MCU (one MCU reads at most about 1.5 KB,
    // which lands in the tables that follow inside the struct).
    JPEG_setMaxOutputSize(jpg, 1);
    jpg->pUser = &ctx;
    ok = JPEG_decode(jpg, 0, 0, OPTIONS[scale]) == 1 && jpg->iError == JPEG_SUCCESS && !ctx.overrun;
close:
    JPEG_close(jpg);
done:
    free(jpg);
    if (ok) { *pixels = ctx.out; *width = (uint16_t)w; *height = (uint16_t)h; }
    else free(ctx.out);
    return ok;
}
