#include "image.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <turbojpeg.h>
#include <wayland-client.h>

namespace {

// Расположение каналов в памяти (x86_64, little-endian).
enum class Layout { Bgrx, Rgbx, Xrgb2101010, Xbgr2101010, Bgr, Rgb };

bool layout_of(uint32_t format, Layout& layout) {
    switch (format) {
    case WL_SHM_FORMAT_ARGB8888:
    case WL_SHM_FORMAT_XRGB8888: layout = Layout::Bgrx; return true;
    case WL_SHM_FORMAT_ABGR8888:
    case WL_SHM_FORMAT_XBGR8888: layout = Layout::Rgbx; return true;
    case WL_SHM_FORMAT_ARGB2101010:
    case WL_SHM_FORMAT_XRGB2101010: layout = Layout::Xrgb2101010; return true;
    case WL_SHM_FORMAT_ABGR2101010:
    case WL_SHM_FORMAT_XBGR2101010: layout = Layout::Xbgr2101010; return true;
    case WL_SHM_FORMAT_RGB888: layout = Layout::Bgr; return true;
    case WL_SHM_FORMAT_BGR888: layout = Layout::Rgb; return true;
    default: return false;
    }
}

void convert_row(Layout layout, const uint8_t* src, uint8_t* dst, int count) {
    switch (layout) {
    case Layout::Bgrx:
        for (int x = 0; x < count; ++x, src += 4, dst += 3) {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
        }
        break;
    case Layout::Rgbx:
        for (int x = 0; x < count; ++x, src += 4, dst += 3) {
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
        }
        break;
    case Layout::Xrgb2101010:
        for (int x = 0; x < count; ++x, src += 4, dst += 3) {
            uint32_t w;
            std::memcpy(&w, src, 4);
            dst[0] = static_cast<uint8_t>(w >> 22);
            dst[1] = static_cast<uint8_t>(w >> 12);
            dst[2] = static_cast<uint8_t>(w >> 2);
        }
        break;
    case Layout::Xbgr2101010:
        for (int x = 0; x < count; ++x, src += 4, dst += 3) {
            uint32_t w;
            std::memcpy(&w, src, 4);
            dst[0] = static_cast<uint8_t>(w >> 2);
            dst[1] = static_cast<uint8_t>(w >> 12);
            dst[2] = static_cast<uint8_t>(w >> 22);
        }
        break;
    case Layout::Bgr:
        for (int x = 0; x < count; ++x, src += 3, dst += 3) {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
        }
        break;
    case Layout::Rgb:
        std::memcpy(dst, src, static_cast<size_t>(count) * 3);
        break;
    }
}

}  // namespace

int shm_format_bpp(uint32_t format) {
    Layout layout;
    if (!layout_of(format, layout)) return 0;
    return layout == Layout::Bgr || layout == Layout::Rgb ? 3 : 4;
}

bool frame_to_rgb(const RawFrame& frame, std::optional<PixelRect> crop, RgbImage& out, std::string& error) {
    Layout layout;
    if (!layout_of(frame.format, layout)) {
        char buf[128];
        std::snprintf(buf, sizeof buf, "неподдерживаемый формат пикселей 0x%08x", frame.format);
        error = buf;
        return false;
    }
    PixelRect r{0, 0, frame.width, frame.height};
    if (crop) {
        const int x0 = std::clamp(crop->x, 0, frame.width);
        const int y0 = std::clamp(crop->y, 0, frame.height);
        const int x1 = std::clamp(crop->x + crop->w, 0, frame.width);
        const int y1 = std::clamp(crop->y + crop->h, 0, frame.height);
        if (x1 > x0 && y1 > y0) r = {x0, y0, x1 - x0, y1 - y0};
    }
    const int bpp = shm_format_bpp(frame.format);
    out.width = r.w;
    out.height = r.h;
    out.pixels.resize(static_cast<size_t>(r.w) * r.h * 3);
    for (int y = 0; y < r.h; ++y) {
        int sy = r.y + y;
        if (frame.y_invert) sy = frame.height - 1 - sy;
        const uint8_t* src = frame.data + static_cast<size_t>(sy) * frame.stride + static_cast<size_t>(r.x) * bpp;
        convert_row(layout, src, &out.pixels[static_cast<size_t>(y) * r.w * 3], r.w);
    }
    return true;
}

void downscale(RgbImage& img, int max_side) {
    const int sw = img.width, sh = img.height;
    if (max_side <= 0 || std::max(sw, sh) <= max_side) return;
    const double k = static_cast<double>(max_side) / std::max(sw, sh);
    const int dw = std::max(1, static_cast<int>(std::lround(sw * k)));
    const int dh = std::max(1, static_cast<int>(std::lround(sh * k)));

    // Каждый пиксель результата — среднее прямоугольника исходных пикселей.
    std::vector<int> x0(dw), x1(dw);
    for (int dx = 0; dx < dw; ++dx) {
        x0[dx] = static_cast<int>(static_cast<int64_t>(dx) * sw / dw);
        x1[dx] = std::max(x0[dx] + 1, static_cast<int>(static_cast<int64_t>(dx + 1) * sw / dw));
    }
    std::vector<uint8_t> out(static_cast<size_t>(dw) * dh * 3);
    std::vector<uint32_t> acc(static_cast<size_t>(dw) * 3);
    for (int dy = 0; dy < dh; ++dy) {
        const int y0 = static_cast<int>(static_cast<int64_t>(dy) * sh / dh);
        const int y1 = std::max(y0 + 1, static_cast<int>(static_cast<int64_t>(dy + 1) * sh / dh));
        std::fill(acc.begin(), acc.end(), 0);
        for (int sy = y0; sy < y1; ++sy) {
            const uint8_t* row = &img.pixels[static_cast<size_t>(sy) * sw * 3];
            for (int dx = 0; dx < dw; ++dx) {
                uint32_t r = 0, g = 0, b = 0;
                for (int sx = x0[dx]; sx < x1[dx]; ++sx) {
                    r += row[sx * 3];
                    g += row[sx * 3 + 1];
                    b += row[sx * 3 + 2];
                }
                acc[dx * 3] += r;
                acc[dx * 3 + 1] += g;
                acc[dx * 3 + 2] += b;
            }
        }
        for (int dx = 0; dx < dw; ++dx) {
            const uint32_t n = static_cast<uint32_t>(y1 - y0) * static_cast<uint32_t>(x1[dx] - x0[dx]);
            uint8_t* d = &out[(static_cast<size_t>(dy) * dw + dx) * 3];
            d[0] = static_cast<uint8_t>((acc[dx * 3] + n / 2) / n);
            d[1] = static_cast<uint8_t>((acc[dx * 3 + 1] + n / 2) / n);
            d[2] = static_cast<uint8_t>((acc[dx * 3 + 2] + n / 2) / n);
        }
    }
    img.pixels = std::move(out);
    img.width = dw;
    img.height = dh;
}

bool encode_jpeg(const RgbImage& img, int quality, std::vector<uint8_t>& out, std::string& error) {
    tjhandle tj = tj3Init(TJINIT_COMPRESS);
    if (!tj) {
        error = "не удалось инициализировать libjpeg-turbo";
        return false;
    }
    tj3Set(tj, TJPARAM_QUALITY, quality);
    tj3Set(tj, TJPARAM_SUBSAMP, TJSAMP_420);
    tj3Set(tj, TJPARAM_FASTDCT, 1);
    unsigned char* jpeg = nullptr;
    size_t size = 0;
    const int rc = tj3Compress8(tj, img.pixels.data(), img.width, 0, img.height, TJPF_RGB, &jpeg, &size);
    if (rc == 0) out.assign(jpeg, jpeg + size);
    else error = tj3GetErrorStr(tj);
    tj3Free(jpeg);
    tj3Destroy(tj);
    return rc == 0;
}

std::string base64(const uint8_t* data, size_t size) {
    static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < size; i += 3) {
        const uint32_t v = (uint32_t{data[i]} << 16) | (uint32_t{data[i + 1]} << 8) | data[i + 2];
        out += kTable[v >> 18];
        out += kTable[(v >> 12) & 63];
        out += kTable[(v >> 6) & 63];
        out += kTable[v & 63];
    }
    if (i < size) {
        uint32_t v = uint32_t{data[i]} << 16;
        if (i + 1 < size) v |= uint32_t{data[i + 1]} << 8;
        out += kTable[v >> 18];
        out += kTable[(v >> 12) & 63];
        out += i + 1 < size ? kTable[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::string sniff_image_type(const std::vector<uint8_t>& d) {
    auto starts = [&](std::initializer_list<uint8_t> sig, size_t offset = 0) {
        if (d.size() < offset + sig.size()) return false;
        return std::equal(sig.begin(), sig.end(), d.begin() + static_cast<std::ptrdiff_t>(offset));
    };
    if (starts({0xFF, 0xD8, 0xFF})) return "image/jpeg";
    if (starts({0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A})) return "image/png";
    if (starts({'G', 'I', 'F', '8'})) return "image/gif";
    if (starts({'R', 'I', 'F', 'F'}) && starts({'W', 'E', 'B', 'P'}, 8)) return "image/webp";
    return {};
}
