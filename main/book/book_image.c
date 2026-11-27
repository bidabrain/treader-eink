/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 中文：将常见 EPUB 插图变为适合墨水屏的白底灰度图。
 * English: Convert common EPUB illustrations to white-backed grayscale for the panel.
 * 冻结：调用由内部互斥锁串行（插图预览和封面生成都会解码）；解码器分配总量有界，不在 render 中解码。
 * Frozen: an internal mutex serializes calls (illustration previews and cover building both decode);
 * bound total decoder allocation and never decode inside render.
 */
#include "book_image.h"
#include "book_jpegdec.h"
#include "esp_heap_caps.h"
#include <stdlib.h>
#include <string.h>
#include "vendor/tjpgd.h"
#include "../perf_diag.h"

#ifdef TREADER_PERF_DIAG
// 这次解码最后走到的解码器。/ The decoder this decode last reached.
static const char* s_perf_path = "";
#define PERF_PATH(name) (s_perf_path = (name))
#else
#define PERF_PATH(name) ((void)0)
#endif
#include "miniz.h"
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif

#define IMAGE_HEAP_MIN (4u * 1024u * 1024u)
#define IMAGE_HEAP_TOP (8u * 1024u * 1024u)
#define IMAGE_HEAP_MARGIN (1536u * 1024u)
#define IMAGE_INPUT_MAX (8u * 1024u * 1024u)
#define PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
// stb_image 的分配上限：按当时剩余 PSRAM 留 1.5 MB 余量，最少 4 MB、最多 8 MB。/ stb_image allocation cap: free PSRAM
// minus a 1.5 MB margin, between 4 MB and 8 MB.
static size_t IMAGE_HEAP_MAX = IMAGE_HEAP_MIN;
// 这一次解码的输出框（锁内有效）：默认 648×1000 等比装进；book_image_decode_fit 换成调用方的框和铺满规则。
// This decode's output box (valid under the lock): 648x1000 fit-inside by default; book_image_decode_fit swaps in the
// caller's box and the fill rule.
static unsigned s_box_w = 648, s_box_h = 1000;
static bool s_cover;
typedef union { size_t size; max_align_t align; } allocation_t;
static size_t allocated;

static void* image_alloc(size_t n) {
    if (n > SIZE_MAX - sizeof(allocation_t) || n + sizeof(allocation_t) > IMAGE_HEAP_MAX - allocated) return NULL;
    allocation_t* p = heap_caps_malloc(sizeof(*p) + n, PSRAM_CAPS);
    if (!p) return NULL;
    p->size = n; allocated += n + sizeof(*p);
    return p + 1;
}
static void image_free(void* ptr) {
    if (!ptr) return;
    allocation_t* p = (allocation_t*)ptr - 1;
    allocated -= p->size + sizeof(*p);
    free(p);
}
static void* image_realloc(void* ptr, size_t n) {
    if (!ptr) return image_alloc(n);
    allocation_t* old = (allocation_t*)ptr - 1;
    size_t prior = old->size;
    if (n > SIZE_MAX - sizeof(*old) || n > IMAGE_HEAP_MAX - (allocated - prior)) return NULL;
    allocation_t* p = heap_caps_realloc(old, sizeof(*old) + n, PSRAM_CAPS);
    if (!p) return NULL;
    p->size = n; allocated = allocated - prior + n;
    return p + 1;
}

#define STBI_MALLOC(n) image_alloc(n)
#define STBI_REALLOC(p,n) image_realloc(p,n)
#define STBI_FREE(p) image_free(p)
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_SIMD
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_MAX_DIMENSIONS 4096
#define STB_IMAGE_IMPLEMENTATION
#include "vendor/stb_image.h"

static uint32_t png_u32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static unsigned png_u16(const uint8_t* p) { return (unsigned)p[0] << 8 | p[1]; }
static uint8_t png_paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return (uint8_t)(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

// 非交错 PNG 边解压边还原：每次只留两行，按目标尺寸取样，大图也只占几十 KB；交错或 16 位的交给 stb_image。
// Stream non-interlaced PNG: inflate and unfilter one row at a time, sampling straight into the target size, so even
// large images need only tens of KB; interlaced or 16-bit images fall back to stb_image.
// 等比装进 bw×bh，至少 1×1。/ Fit inside bw x bh keeping the aspect, at least 1x1.
static void contain(uint64_t w, uint64_t h, unsigned bw, unsigned bh, unsigned* ow, unsigned* oh) {
    if (w > bw) { h = h * bw / w; w = bw; }
    if (h > bh) { w = w * bh / h; h = bh; }
    *ow = w ? (unsigned)w : 1;
    *oh = h ? (unsigned)h : 1;
}

void book_image_out_size(unsigned iw, unsigned ih, size_t budget, unsigned* ow, unsigned* oh) {
    if (s_cover && iw && ih) {
        uint64_t w = iw, h = ih;
        // 两边都比框大才缩到刚好铺满；宽对齐时高够就按宽，否则按高。/ Shrink to just fill only when both sides exceed
        // the box: match the width when the height still covers, else match the height.
        if (w > s_box_w && h > s_box_h) {
            const uint64_t at_w = h * s_box_w / w;
            if (at_w >= s_box_h) { h = at_w; w = s_box_w; }
            else { w = w * s_box_h / h; h = s_box_h; }
        }
        if (w * h <= budget) {
            *ow = w ? (unsigned)w : 1;
            *oh = h ? (unsigned)h : 1;
            return;
        }
    }
    contain(iw, ih, s_box_w, s_box_h, ow, oh);
}

typedef struct {
    unsigned w, h, type, bits, channels, dw, dh, dy, y;
    size_t row_n, filled, bpp;
    uint8_t *cur, *prev, *out;
    const uint8_t *palette, *trans;
    size_t palette_n, trans_n;
    bool bad;
} png_rows_t;

static unsigned png_sample(const png_rows_t* s, const uint8_t* row, unsigned x, unsigned c) {
    if (s->bits == 8) return row[(size_t)x * s->channels + c];
    const unsigned per = 8 / s->bits, mask = (1u << s->bits) - 1;
    const uint8_t byte = row[x / per];
    return (byte >> (8 - s->bits - (x % per) * s->bits)) & mask;
}

// 一行还原好以后，落到输出图的对应行。/ Once a row is unfiltered, write it into its output row.
static void png_emit_row(png_rows_t* s, const uint8_t* row) {
    if (s->dy >= s->dh || (uint64_t)s->dy * s->h / s->dh != s->y) return;
    const unsigned mask = (1u << s->bits) - 1;
    for (unsigned x = 0; x < s->dw; ++x) {
        const unsigned sx = (unsigned)((uint64_t)x * s->w / s->dw);
        unsigned gray = 0, alpha = 255;
        if (s->type == 0 || s->type == 4) {
            const unsigned v = png_sample(s, row, sx, 0);
            gray = s->bits == 8 ? v : v * 255u / mask;
            if (s->type == 4) alpha = png_sample(s, row, sx, 1);
            else if (s->trans && v == png_u16(s->trans)) alpha = 0;
        } else if (s->type == 3) {
            const unsigned index = png_sample(s, row, sx, 0);
            if (index >= s->palette_n) { s->bad = true; return; }
            const uint8_t* color = s->palette + index * 3;
            gray = (77u * color[0] + 150u * color[1] + 29u * color[2]) >> 8;
            if (index < s->trans_n) alpha = s->trans[index];
        } else {
            const uint8_t* p = row + (size_t)sx * s->channels;
            gray = (77u * p[0] + 150u * p[1] + 29u * p[2]) >> 8;
            if (s->type == 6) alpha = p[3];
            else if (s->trans && p[0] == png_u16(s->trans) && p[1] == png_u16(s->trans + 2) && p[2] == png_u16(s->trans + 4))
                alpha = 0;
        }
        s->out[(size_t)s->dy * s->dw + x] = (uint8_t)((gray * alpha + 255u * (255u - alpha) + 127u) / 255u);
    }
    ++s->dy;
}

// 收下解压出来的一段字节，凑满一行就还原。/ Take a run of inflated bytes and unfilter each completed row.
static void png_feed(png_rows_t* s, const uint8_t* data, size_t n) {
    while (n && !s->bad && s->y < s->h) {
        const size_t want = s->row_n + 1 - s->filled, take = n < want ? n : want;
        memcpy(s->cur + s->filled, data, take);
        s->filled += take;
        data += take;
        n -= take;
        if (s->filled < s->row_n + 1) break;
        const unsigned filter = s->cur[0];
        uint8_t* row = s->cur + 1;
        const uint8_t* prev = s->y ? s->prev + 1 : NULL;
        if (filter > 4) { s->bad = true; return; }
        for (size_t x = 0; x < s->row_n; ++x) {
            const unsigned a = x >= s->bpp ? row[x - s->bpp] : 0, b = prev ? prev[x] : 0;
            const unsigned c = prev && x >= s->bpp ? prev[x - s->bpp] : 0;
            const unsigned add = filter == 0 ? 0 : filter == 1 ? a : filter == 2 ? b : filter == 3 ? (a + b) / 2 : png_paeth(a, b, c);
            row[x] = (uint8_t)(row[x] + add);
        }
        png_emit_row(s, row);
        uint8_t* t = s->prev;
        s->prev = s->cur;
        s->cur = t;
        s->filled = 0;
        ++s->y;
#ifdef ESP_PLATFORM
        if (!(s->y % 32)) vTaskDelay(1);
#endif
    }
}

static bool png_decode(const uint8_t* data, size_t len, size_t budget,
                       uint8_t** pixels, uint16_t* width, uint16_t* height) {
    if (len < 33 || memcmp(data, "\x89PNG\r\n\x1a\n", 8) || png_u32(data + 8) != 13 || memcmp(data + 12, "IHDR", 4)) return false;
    png_rows_t s = {.w = png_u32(data + 16), .h = png_u32(data + 20), .type = data[25], .bits = data[24]};
    s.channels = s.type == 0 ? 1 : s.type == 2 ? 3 : s.type == 3 ? 1 : s.type == 4 ? 2 : s.type == 6 ? 4 : 0;
    // 8 位都支持；1、2、4 位只用于灰度和调色板。/ 8-bit for every type; 1, 2 and 4 bits only for gray and palette.
    const bool bits_ok = s.bits == 8 || ((s.type == 0 || s.type == 3) && (s.bits == 1 || s.bits == 2 || s.bits == 4));
    if (!s.w || !s.h || s.w > 16384 || s.h > 16384 || !s.channels || !bits_ok || data[26] || data[27] || data[28]) return false;
    bool idat = false, ended = false;
    for (size_t at = 33; at < len;) {
        if (len - at < 12) return false;
        size_t n = png_u32(data + at);
        if (n > len - at - 12) return false;
        const uint8_t* tag = data + at + 4;
        if (!memcmp(tag, "IDAT", 4)) idat = true;
        else if (!memcmp(tag, "PLTE", 4)) {
            if (idat || s.palette || !n || n % 3 || n > 768) return false;
            s.palette = data + at + 8;
            s.palette_n = n / 3;
        } else if (!memcmp(tag, "tRNS", 4)) {
            if (idat || s.trans || (s.type == 0 ? n != 2 : s.type == 2 ? n != 6 : s.type == 3 ? !n || n > 256 : true)) return false;
            s.trans = data + at + 8;
            s.trans_n = n;
        } else if (!memcmp(tag, "IEND", 4)) { if (n) return false; ended = true; break; }
        else if (!(tag[0] & 32)) return false;
        at += n + 12;
    }
    if (!ended || !idat || (s.type == 3 && (!s.palette || s.trans_n > s.palette_n))) return false;
    book_image_out_size(s.w, s.h, budget, &s.dw, &s.dh);
    if ((size_t)s.dw * s.dh > budget) return false;
    s.row_n = ((size_t)s.w * s.channels * s.bits + 7) / 8;
    s.bpp = s.channels * s.bits / 8 ? s.channels * s.bits / 8 : 1;
    s.cur = heap_caps_malloc(s.row_n + 1, PSRAM_CAPS);
    s.prev = heap_caps_malloc(s.row_n + 1, PSRAM_CAPS);
    uint8_t* dict = heap_caps_malloc(TINFL_LZ_DICT_SIZE, PSRAM_CAPS);
    tinfl_decompressor* inf = heap_caps_malloc(sizeof(*inf), PSRAM_CAPS);
    s.out = heap_caps_malloc((size_t)s.dw * s.dh, PSRAM_CAPS);
    bool ok = false, done = false;
    if (!s.cur || !s.prev || !dict || !inf || !s.out) goto out;
    tinfl_init(inf);
    size_t dict_ofs = 0;
    for (size_t at = 33; at + 12 <= len && !done && !s.bad;) {
        size_t n = png_u32(data + at);
        const uint8_t* tag = data + at + 4;
        if (!memcmp(tag, "IEND", 4)) break;
        if (memcmp(tag, "IDAT", 4)) { at += n + 12; continue; }
        const uint8_t* in = data + at + 8;
        at += n + 12;
        // 后面还有 IDAT 就告诉解压器还有输入。/ Tell the inflater more input follows when another IDAT does.
        bool more = false;
        for (size_t k = at; k + 12 <= len; k += png_u32(data + k) + 12) {
            if (!memcmp(data + k + 4, "IDAT", 4)) { more = true; break; }
            if (!memcmp(data + k + 4, "IEND", 4)) break;
        }
        for (;;) {
            size_t in_n = n, out_n = TINFL_LZ_DICT_SIZE - dict_ofs;
            const tinfl_status st = tinfl_decompress(inf, in, &in_n, dict, dict + dict_ofs, &out_n,
                                                     TINFL_FLAG_PARSE_ZLIB_HEADER | (more ? TINFL_FLAG_HAS_MORE_INPUT : 0));
            in += in_n;
            n -= in_n;
            png_feed(&s, dict + dict_ofs, out_n);
            dict_ofs = (dict_ofs + out_n) & (TINFL_LZ_DICT_SIZE - 1);
            if (s.bad || st < TINFL_STATUS_DONE) { s.bad = true; break; }
            if (st == TINFL_STATUS_DONE) { done = true; break; }
            if (st == TINFL_STATUS_NEEDS_MORE_INPUT) { if (!more) s.bad = true; break; }
        }
    }
    if (!s.bad && s.y == s.h && s.dy == s.dh) {
        *pixels = s.out;
        *width = (uint16_t)s.dw;
        *height = (uint16_t)s.dh;
        s.out = NULL;
        ok = true;
    }
out:
    heap_caps_free(s.cur);
    heap_caps_free(s.prev);
    heap_caps_free(dict);
    heap_caps_free(inf);
    free(s.out);
    return ok;
}

typedef struct {
    const uint8_t* data;
    size_t len, at;
    uint8_t* out;
    unsigned width, height, scaled_width, scaled_height, blocks;
} jpeg_io_t;

static size_t jpeg_read(JDEC* jd, uint8_t* out, size_t count) {
    jpeg_io_t* io = jd->device;
    if (count > io->len - io->at) count = io->len - io->at;
    if (out) memcpy(out, io->data + io->at, count);
    io->at += count;
    return count;
}
static int jpeg_write(JDEC* jd, void* bitmap, JRECT* r) {
    jpeg_io_t* io = jd->device;
    if (r->right >= io->scaled_width || r->bottom >= io->scaled_height ||
        r->left > r->right || r->top > r->bottom) return 0;
    unsigned x0 = ((unsigned)r->left * io->width + io->scaled_width - 1) / io->scaled_width;
    unsigned x1 = ((unsigned)(r->right + 1) * io->width + io->scaled_width - 1) / io->scaled_width;
    unsigned y0 = ((unsigned)r->top * io->height + io->scaled_height - 1) / io->scaled_height;
    unsigned y1 = ((unsigned)(r->bottom + 1) * io->height + io->scaled_height - 1) / io->scaled_height;
    const uint8_t* rgb = bitmap;
    for (unsigned y = y0; y < y1; ++y) for (unsigned x = x0; x < x1; ++x) {
        unsigned sx = x * io->scaled_width / io->width - r->left;
        unsigned sy = y * io->scaled_height / io->height - r->top;
        const uint8_t* color = rgb + (sy * (r->right - r->left + 1) + sx) * 3;
        io->out[(size_t)y * io->width + x] = (uint8_t)((77u*color[0]+150u*color[1]+29u*color[2])>>8);
    }
#ifdef ESP_PLATFORM
    // 大图也定期让出 CPU，避免空闲任务看门狗饥饿。/ Yield during large images so idle watchdog tasks can run.
    if (!(++io->blocks % 32)) vTaskDelay(1);
#endif
    return 1;
}
static bool jpeg_decode(const uint8_t* data, size_t len, size_t budget,
                        uint8_t** pixels, uint16_t* width, uint16_t* height) {
    void* work = heap_caps_malloc(8192, PSRAM_CAPS);
    if (!work) return false;
    JDEC jd;
    jpeg_io_t io = {.data=data, .len=len};
    bool ok = false;
    if (jd_prepare(&jd, jpeg_read, work, 8192, &io) != JDR_OK || !jd.width || !jd.height ||
        jd.width > 8192 || jd.height > 8192 || (uint64_t)jd.width * jd.height > 16u * 1024u * 1024u) goto done;
    book_image_out_size(jd.width, jd.height, budget, &io.width, &io.height);
    if ((size_t)io.width * io.height > budget) goto done;
    unsigned scale = 0;
    // 阅读插图允许至多两倍放大，优先在 JPEG 解码阶段降采样。/ Allow at most 2x enlargement to reduce work during JPEG decoding.
    while (scale < 3 && ((unsigned)jd.width >> (scale + 1)) >= (io.width + 1) / 2 && ((unsigned)jd.height >> (scale + 1)) >= (io.height + 1) / 2) ++scale;
    io.scaled_width = jd.width >> scale; io.scaled_height = jd.height >> scale;
    io.out = heap_caps_malloc((size_t)io.width * io.height, PSRAM_CAPS);
    if (!io.out) goto done;
    memset(io.out, 255, (size_t)io.width * io.height);
    ok = jd_decomp(&jd, jpeg_write, (uint8_t)scale) == JDR_OK;
    if (ok) { *pixels = io.out; *width = (uint16_t)io.width; *height = (uint16_t)io.height; }
    else free(io.out);
done:
    free(work);
    return ok;
}

static bool decode_locked(const uint8_t* data, size_t len, size_t budget,
                          uint8_t** pixels, uint16_t* width, uint16_t* height);

// 插图和封面共用全局分配计数和输出框，必须一次只解一张；加锁后从哪个任务调用都安全。
// Illustrations and covers share the global allocation count and output box, so decode one at a time; the lock makes
// any caller task safe.
static bool decode_boxed(const uint8_t* data, size_t len, size_t budget, unsigned box_w, unsigned box_h, bool cover,
                         uint8_t** pixels, uint16_t* width, uint16_t* height) {
#ifdef ESP_PLATFORM
    static StaticSemaphore_t storage;
    static SemaphoreHandle_t lock;
    static portMUX_TYPE init_lock = portMUX_INITIALIZER_UNLOCKED;
    portENTER_CRITICAL(&init_lock);
    if (!lock) lock = xSemaphoreCreateMutexStatic(&storage);
    portEXIT_CRITICAL(&init_lock);
    xSemaphoreTake(lock, portMAX_DELAY);
#endif
    PERF_T0(t_decode);
    s_box_w = box_w;
    s_box_h = box_h;
    s_cover = cover;
    bool ok = decode_locked(data, len, budget, pixels, width, height);
    PERF_LOG("decode %s %u B -> %ux%u: %ld ms%s", s_perf_path, (unsigned)len, ok ? (unsigned)*width : 0u,
             ok ? (unsigned)*height : 0u, PERF_US(t_decode) / 1000, ok ? "" : " FAILED");
    s_box_w = 648;
    s_box_h = 1000;
    s_cover = false;
#ifdef ESP_PLATFORM
    xSemaphoreGive(lock);
#endif
    return ok;
}

bool book_image_decode(const uint8_t* data, size_t len, size_t budget,
                       uint8_t** pixels, uint16_t* width, uint16_t* height) {
    return decode_boxed(data, len, budget, 648, 1000, false, pixels, width, height);
}

bool book_image_decode_fit(const uint8_t* data, size_t len, unsigned box_w, unsigned box_h, bool cover,
                           uint8_t** pixels, uint16_t* width, uint16_t* height) {
    if (!box_w || !box_h || box_w > 16384 || box_h > 16384) {
        if (pixels) *pixels = NULL;
        if (width) *width = 0;
        if (height) *height = 0;
        return false;
    }
    const size_t area = (size_t)box_w * box_h;
    return decode_boxed(data, len, area > 648u * 1000u ? area : 648u * 1000u, box_w, box_h, cover, pixels, width,
                        height);
}

static bool decode_locked(const uint8_t* data, size_t len, size_t budget,
                          uint8_t** pixels, uint16_t* width, uint16_t* height) {
    if (!pixels || !width || !height) return false;
    *pixels = NULL; *width = *height = 0;
    if (!data || !len || len > IMAGE_INPUT_MAX || allocated) return false;
    PERF_PATH("png");
    if (png_decode(data,len,budget,pixels,width,height)) return true;
    // 普通基线 JPEG 先走 JPEGDEC 快速通道；不合格或失败再用 tjpgd 按 MCU 缩小；小型渐进 JPEG 仍可走有界完整解码。
    // Plain baseline JPEG tries the JPEGDEC fast path first; ineligible or failed ones fall back to MCU-scaled tjpgd;
    // bounded full decoding still handles small progressive JPEGs.
    PERF_PATH("jpegdec");
    if (len >= 2 && data[0] == 0xff && data[1] == 0xd8 &&
        (book_jpegdec_decode(data, len, budget, pixels, width, height) ||
         (PERF_PATH("tjpgd"), jpeg_decode(data, len, budget, pixels, width, height)))) return true;
    PERF_PATH("stb");
    int w, h, channels;
#ifdef ESP_PLATFORM
    const size_t free_psram = heap_caps_get_free_size(PSRAM_CAPS);
    IMAGE_HEAP_MAX = free_psram > IMAGE_HEAP_MIN + IMAGE_HEAP_MARGIN ? free_psram - IMAGE_HEAP_MARGIN : IMAGE_HEAP_MIN;
    if (IMAGE_HEAP_MAX > IMAGE_HEAP_TOP) IMAGE_HEAP_MAX = IMAGE_HEAP_TOP;
#endif
    // 渐进 JPEG、交错或 16 位 PNG 要整张解：每像素约 5 字节，按上限算能解多大。
    // Progressive JPEG and interlaced or 16-bit PNG decode whole: about 5 bytes per pixel, so size them to the cap.
    if (!stbi_info_from_memory(data, (int)len, &w, &h, &channels) ||
        w <= 0 || h <= 0 || (uint64_t)w * h * 5u > IMAGE_HEAP_MAX) return false;
    unsigned uw, uh;
    book_image_out_size((unsigned)w, (unsigned)h, budget, &uw, &uh);
    const int dw = (int)uw, dh = (int)uh;
    size_t size = (size_t)dw * dh;
    if (size > budget) return false;
    uint8_t* gray = stbi_load_from_memory(data, (int)len, &w, &h, &channels, 2);
    if (!gray) return false;
    uint8_t* out = heap_caps_malloc(size, PSRAM_CAPS);
    if (out) {
        for (int y = 0; y < dh; ++y) for (int x = 0; x < dw; ++x) {
            size_t at = ((size_t)((int64_t)y * h / dh) * w + (size_t)((int64_t)x * w / dw)) * 2;
            unsigned alpha = gray[at + 1];
            out[(size_t)y * dw + x] = (uint8_t)((gray[at] * alpha + 255u * (255u - alpha) + 127u) / 255u);
        }
    }
    stbi_image_free(gray);
    if (!out) return false;
    *pixels = out; *width = (uint16_t)dw; *height = (uint16_t)dh;
    return true;
}
