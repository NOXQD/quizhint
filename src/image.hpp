#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Кадр в разделяемой памяти в формате wl_shm.
struct RawFrame {
    const uint8_t* data = nullptr;
    uint32_t format = 0;
    int width = 0, height = 0, stride = 0;
    bool y_invert = false;
};

struct RgbImage {
    int width = 0, height = 0;
    std::vector<uint8_t> pixels;  // RGB, 3 байта на пиксель
};

struct PixelRect {
    int x = 0, y = 0, w = 0, h = 0;
};

// Байт на пиксель для поддерживаемых форматов wl_shm, 0 — формат не поддерживается.
int shm_format_bpp(uint32_t format);

// Переводит кадр в RGB, при необходимости вырезая прямоугольник (в пикселях кадра).
bool frame_to_rgb(const RawFrame& frame, std::optional<PixelRect> crop, RgbImage& out, std::string& error);

// Уменьшает усреднением по площади, чтобы длинная сторона была не больше max_side.
void downscale(RgbImage& image, int max_side);

bool encode_jpeg(const RgbImage& image, int quality, std::vector<uint8_t>& out, std::string& error);

std::string base64(const uint8_t* data, size_t size);

// MIME-тип по сигнатуре: image/jpeg, image/png, image/gif, image/webp или "".
std::string sniff_image_type(const std::vector<uint8_t>& data);
