// esp_new_jpeg（esp_jpeg_dec.h）的 PC 实现。
//
// 板上那份是预编译的 xtensa 静态库，PC 上链不了，所以这里用 LVGL 自带的
// TJpgDec 顶上：接口、缩放语义（scale 是绝对目标尺寸，最多 1/8）、输出格式
// 都按真实组件的行为对齐，相册那条「先探尺寸再按档解码」的流水线因此不用改。
//
// 已知差异：LVGL 那份 TJpgDec 编译时关掉了整档降采样（JD_USE_SCALE 0），而改
// managed_components 会被下次拉依赖冲掉，所以这里一律整图解码再自己做盒式降
// 采样。PC 上多花几毫秒和几 MB 内存，换来构图、尺寸和真机完全一致。
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "esp_jpeg_dec.h"
#include "src/libs/tjpgd/tjpgd.h"

namespace {

struct SimDec {
    jpeg_dec_config_t cfg = {};
    // 输入
    const uint8_t* in = nullptr;
    size_t in_len = 0;
    size_t in_pos = 0;
    // 原图尺寸
    int src_w = 0;
    int src_h = 0;
    // 目标尺寸（考虑 cfg.scale 之后）
    int dst_w = 0;
    int dst_h = 0;
    bool header_ok = false;
};

struct DecodeCtx {
    SimDec* dec = nullptr;
    std::vector<uint8_t>* rgb = nullptr;  // RGB888，按整档解出来的中间图
    int w = 0;
    int h = 0;
};

size_t InFunc(JDEC* jd, uint8_t* buf, size_t len) {
    auto* ctx = static_cast<DecodeCtx*>(jd->device);
    SimDec* d = ctx->dec;
    const size_t remain = d->in_len - d->in_pos;
    const size_t n = len < remain ? len : remain;
    if (buf != nullptr) {
        memcpy(buf, d->in + d->in_pos, n);
    }
    d->in_pos += n;
    return n;
}

int OutFunc(JDEC* jd, void* bitmap, JRECT* rect) {
    auto* ctx = static_cast<DecodeCtx*>(jd->device);
    const auto* src = static_cast<const uint8_t*>(bitmap);
    const int rw = rect->right - rect->left + 1;
    for (int y = rect->top; y <= rect->bottom; ++y) {
        if (y >= ctx->h) {
            break;
        }
        for (int x = rect->left; x <= rect->right; ++x) {
            if (x >= ctx->w) {
                continue;
            }
            const size_t si = (static_cast<size_t>(y - rect->top) * rw + (x - rect->left)) * 3;
            const size_t di = (static_cast<size_t>(y) * ctx->w + x) * 3;
            (*ctx->rgb)[di + 0] = src[si + 0];
            (*ctx->rgb)[di + 1] = src[si + 1];
            (*ctx->rgb)[di + 2] = src[si + 2];
        }
    }
    return 1;
}

int BytesPerPixel(jpeg_pixel_format_t fmt) {
    switch (fmt) {
        case JPEG_PIXEL_FORMAT_RGB565_LE:
        case JPEG_PIXEL_FORMAT_RGB565_BE:
            return 2;
        case JPEG_PIXEL_FORMAT_RGB888:
            return 3;
        case JPEG_PIXEL_FORMAT_RGBA:
            return 4;
        default:
            return 0;
    }
}

void StorePixel(uint8_t* dst, jpeg_pixel_format_t fmt, uint8_t r, uint8_t g, uint8_t b) {
    switch (fmt) {
        case JPEG_PIXEL_FORMAT_RGB565_LE: {
            const uint16_t v = static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) |
                                                     (b >> 3));
            dst[0] = static_cast<uint8_t>(v & 0xFF);
            dst[1] = static_cast<uint8_t>(v >> 8);
            break;
        }
        case JPEG_PIXEL_FORMAT_RGB565_BE: {
            const uint16_t v = static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) |
                                                     (b >> 3));
            dst[0] = static_cast<uint8_t>(v >> 8);
            dst[1] = static_cast<uint8_t>(v & 0xFF);
            break;
        }
        case JPEG_PIXEL_FORMAT_RGBA:
            dst[0] = r;
            dst[1] = g;
            dst[2] = b;
            dst[3] = 255;
            break;
        default:
            dst[0] = r;
            dst[1] = g;
            dst[2] = b;
            break;
    }
}

}  // namespace

extern "C" jpeg_error_t jpeg_dec_open(jpeg_dec_config_t* config, jpeg_dec_handle_t* jpeg_dec) {
    if (config == nullptr || jpeg_dec == nullptr) {
        return JPEG_ERR_INVALID_PARAM;
    }
    if (config->block_enable) {
        return JPEG_ERR_UNSUPPORT_FMT;  // 仿真不支持分块模式
    }
    if (config->rotate != JPEG_ROTATE_0D) {
        return JPEG_ERR_UNSUPPORT_FMT;
    }
    if (BytesPerPixel(config->output_type) == 0) {
        return JPEG_ERR_UNSUPPORT_FMT;
    }
    auto* d = new SimDec();
    d->cfg = *config;
    *jpeg_dec = d;
    return JPEG_ERR_OK;
}

extern "C" jpeg_error_t jpeg_dec_parse_header(jpeg_dec_handle_t jpeg_dec, jpeg_dec_io_t* io,
                                             jpeg_dec_header_info_t* out_info) {
    auto* d = static_cast<SimDec*>(jpeg_dec);
    if (d == nullptr || io == nullptr || io->inbuf == nullptr || io->inbuf_len <= 0) {
        return JPEG_ERR_INVALID_PARAM;
    }
    d->in = io->inbuf;
    d->in_len = static_cast<size_t>(io->inbuf_len);
    d->in_pos = 0;

    DecodeCtx ctx;
    ctx.dec = d;
    JDEC jd = {};
    std::vector<uint8_t> pool(32 * 1024);
    const JRESULT rc = jd_prepare(&jd, InFunc, pool.data(), pool.size(), &ctx);
    if (rc != JDR_OK) {
        return rc == JDR_INP ? JPEG_ERR_NO_MORE_DATA : JPEG_ERR_BAD_DATA;
    }
    d->src_w = jd.width;
    d->src_h = jd.height;

    d->dst_w = d->cfg.scale.width > 0 ? d->cfg.scale.width : d->src_w;
    d->dst_h = d->cfg.scale.height > 0 ? d->cfg.scale.height : d->src_h;
    if (d->cfg.clipper.width > 0) {
        d->dst_w = d->cfg.clipper.width;
    }
    if (d->cfg.clipper.height > 0) {
        d->dst_h = d->cfg.clipper.height;
    }
    d->header_ok = true;

    if (out_info != nullptr) {
        out_info->width = static_cast<uint16_t>(d->src_w);
        out_info->height = static_cast<uint16_t>(d->src_h);
    }
    io->inbuf_remain = io->inbuf_len;
    return JPEG_ERR_OK;
}

extern "C" jpeg_error_t jpeg_dec_get_outbuf_len(jpeg_dec_handle_t jpeg_dec, int* outbuf_len) {
    auto* d = static_cast<SimDec*>(jpeg_dec);
    if (d == nullptr || outbuf_len == nullptr || !d->header_ok) {
        return JPEG_ERR_INVALID_PARAM;
    }
    *outbuf_len = d->dst_w * d->dst_h * BytesPerPixel(d->cfg.output_type);
    return JPEG_ERR_OK;
}

extern "C" jpeg_error_t jpeg_dec_get_process_count(jpeg_dec_handle_t jpeg_dec,
                                                  int* process_count) {
    if (jpeg_dec == nullptr || process_count == nullptr) {
        return JPEG_ERR_INVALID_PARAM;
    }
    *process_count = 1;
    return JPEG_ERR_OK;
}

extern "C" jpeg_error_t jpeg_dec_process(jpeg_dec_handle_t jpeg_dec, jpeg_dec_io_t* io) {
    auto* d = static_cast<SimDec*>(jpeg_dec);
    if (d == nullptr || io == nullptr || io->outbuf == nullptr || !d->header_ok) {
        return JPEG_ERR_INVALID_PARAM;
    }

    const int mid_w = d->src_w;
    const int mid_h = d->src_h;
    std::vector<uint8_t> rgb(static_cast<size_t>(mid_w) * mid_h * 3, 0);
    DecodeCtx ctx;
    ctx.dec = d;
    ctx.rgb = &rgb;
    ctx.w = mid_w;
    ctx.h = mid_h;

    d->in_pos = 0;
    JDEC jd = {};
    std::vector<uint8_t> pool(32 * 1024);
    if (jd_prepare(&jd, InFunc, pool.data(), pool.size(), &ctx) != JDR_OK) {
        return JPEG_ERR_BAD_DATA;
    }
    const JRESULT rc = jd_decomp(&jd, OutFunc, 0);
    if (rc != JDR_OK) {
        return JPEG_ERR_BAD_DATA;
    }

    // 整图 -> 目标尺寸：每个目标像素取源图对应小块的平均值（盒式），
    // 和真机 DCT 降采样出来的观感接近，不会有最近邻那种锯齿。
    const int bpp = BytesPerPixel(d->cfg.output_type);
    for (int y = 0; y < d->dst_h; ++y) {
        const int sy0 = static_cast<int>(static_cast<int64_t>(y) * mid_h / d->dst_h);
        int sy1 = static_cast<int>(static_cast<int64_t>(y + 1) * mid_h / d->dst_h);
        if (sy1 <= sy0) {
            sy1 = sy0 + 1;
        }
        for (int x = 0; x < d->dst_w; ++x) {
            const int sx0 = static_cast<int>(static_cast<int64_t>(x) * mid_w / d->dst_w);
            int sx1 = static_cast<int>(static_cast<int64_t>(x + 1) * mid_w / d->dst_w);
            if (sx1 <= sx0) {
                sx1 = sx0 + 1;
            }
            uint32_t r = 0;
            uint32_t g = 0;
            uint32_t b = 0;
            uint32_t n = 0;
            for (int sy = sy0; sy < sy1 && sy < mid_h; ++sy) {
                const uint8_t* row = &rgb[(static_cast<size_t>(sy) * mid_w) * 3];
                for (int sx = sx0; sx < sx1 && sx < mid_w; ++sx) {
                    r += row[sx * 3 + 0];
                    g += row[sx * 3 + 1];
                    b += row[sx * 3 + 2];
                    ++n;
                }
            }
            if (n == 0) {
                n = 1;
            }
            uint8_t* dst = io->outbuf + (static_cast<size_t>(y) * d->dst_w + x) * bpp;
            StorePixel(dst, d->cfg.output_type, static_cast<uint8_t>(r / n),
                       static_cast<uint8_t>(g / n), static_cast<uint8_t>(b / n));
        }
    }

    io->out_size = d->dst_w * d->dst_h * bpp;
    io->inbuf_remain = 0;
    return JPEG_ERR_OK;
}

extern "C" jpeg_error_t jpeg_dec_close(jpeg_dec_handle_t jpeg_dec) {
    delete static_cast<SimDec*>(jpeg_dec);
    return JPEG_ERR_OK;
}

extern "C" void* jpeg_calloc_align(size_t size, int aligned) {
    void* p = nullptr;
    const size_t a = aligned > 0 ? static_cast<size_t>(aligned) : sizeof(void*);
    const size_t rounded = (size + a - 1) / a * a;
    if (posix_memalign(&p, a, rounded) != 0) {
        return nullptr;
    }
    memset(p, 0, rounded);
    return p;
}

extern "C" void jpeg_free_align(void* data) {
    free(data);
}
