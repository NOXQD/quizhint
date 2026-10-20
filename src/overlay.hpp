#pragma once

#include <cairo.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "config.hpp"
#include "display.hpp"

struct zwlr_layer_surface_v1;
struct zwlr_layer_surface_v1_listener;
struct wp_fractional_scale_v1;
struct wp_fractional_scale_v1_listener;
struct wp_viewport;

// Маленькая плашка с текстом в левом верхнем углу монитора: слой overlay
// (поверх всех окон, включая полноэкранные), без фокуса клавиатуры, клики
// проходят насквозь. Поверхность живёт всё время и в спрятанном виде просто
// прозрачна — так нет анимаций появления, и смена цифры мгновенная.
class Overlay {
public:
    static constexpr const char* kBusy = "\x01";  // индикатор «модель думает»

    Overlay(Display& display, const Config& cfg);
    ~Overlay();
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    // Создаёт поверхность на мониторе или переносит её туда.
    void move_to(Output* output);
    Output* output() const { return output_; }
    void output_removed(Output* output);

    // Показать текст; пустая строка — спрятать.
    void set(const std::string& text);
    // Спрятать и вызвать presented, когда композитор отрисует кадр без плашки.
    void hide(std::function<void()> presented);
    bool shown() const { return !text_.empty(); }

private:
    static const zwlr_layer_surface_v1_listener kLayerListener;
    static const wl_surface_listener kSurfaceListener;
    static const wp_fractional_scale_v1_listener kFractionalListener;
    static const wl_buffer_listener kBufferListener;
    static const wl_callback_listener kFrameListener;

    void create();
    void destroy();
    void redraw();
    void paint(cairo_t* cr) const;
    std::pair<uint32_t, uint32_t> surface_size() const;
    void on_configure(uint32_t serial, uint32_t width, uint32_t height);

    Display& d_;
    const Config& cfg_;
    Output* output_ = nullptr;
    wl_surface* surface_ = nullptr;
    zwlr_layer_surface_v1* layer_ = nullptr;
    wp_fractional_scale_v1* fractional_ = nullptr;
    wp_viewport* viewport_ = nullptr;
    wl_callback* frame_callback_ = nullptr;
    std::function<void()> frame_done_;
    bool configured_ = false;
    uint32_t width_ = 0, height_ = 0;  // логический размер поверхности
    int32_t buffer_scale_ = 1;         // целый масштаб (без wp_viewporter)
    double fractional_scale_ = 0;      // дробный масштаб из wp_fractional_scale, 0 — ещё не пришёл
    std::string text_;
    std::vector<std::unique_ptr<ShmBuffer>> buffers_;  // ещё не отпущенные композитором
};
