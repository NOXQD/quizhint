#include "overlay.hpp"

#include "log.hpp"
#include "protocols.hpp"

#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <tuple>

namespace {

void set_source(cairo_t* cr, const Rgba& c, double alpha = 1.0) {
    cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a * alpha);
}

void setup_font(cairo_t* cr, const Config& cfg) {
    cairo_select_font_face(cr, cfg.font.c_str(), CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, cfg.font_size);
    cairo_font_options_t* options = cairo_font_options_create();
    cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_GRAY);
    cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_SLIGHT);
    cairo_font_options_set_hint_metrics(options, CAIRO_HINT_METRICS_OFF);
    cairo_set_font_options(cr, options);
    cairo_font_options_destroy(options);
}

// Отступы текста полного ответа от краёв плашки.
double text_pad_x(const Config& cfg) { return std::round(cfg.full_font_size * 0.75); }
double text_pad_y(const Config& cfg) { return std::round(cfg.full_font_size * 0.5); }

// Раскладка текста полного ответа: перенос по словам, не больше full_max_lines
// строк (дальше — многоточие). Метрики без хинтинга, чтобы размер, посчитанный
// заранее, совпадал с отрисовкой при любом масштабе.
PangoLayout* make_text_layout(cairo_t* cr, const Config& cfg, const std::string& text) {
    PangoLayout* layout = pango_cairo_create_layout(cr);
    cairo_font_options_t* options = cairo_font_options_create();
    cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_GRAY);
    cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_SLIGHT);
    cairo_font_options_set_hint_metrics(options, CAIRO_HINT_METRICS_OFF);
    pango_cairo_context_set_font_options(pango_layout_get_context(layout), options);
    cairo_font_options_destroy(options);
    pango_layout_context_changed(layout);

    PangoFontDescription* font = pango_font_description_from_string(cfg.font.c_str());
    pango_font_description_set_absolute_size(font, cfg.full_font_size * PANGO_SCALE);
    pango_font_description_set_weight(font, PANGO_WEIGHT_MEDIUM);
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);

    const double width = std::max(40.0, cfg.full_width - 2 * text_pad_x(cfg));
    pango_layout_set_width(layout, static_cast<int>(width * PANGO_SCALE));
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_height(layout, -std::max(1, cfg.full_max_lines));
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_text(layout, text.c_str(), -1);
    return layout;
}

void rounded_rect(cairo_t* cr, double w, double h, double r) {
    using std::numbers::pi;
    r = std::min({r, w / 2, h / 2});
    cairo_new_sub_path(cr);
    cairo_arc(cr, w - r, r, r, -pi / 2, 0);
    cairo_arc(cr, w - r, h - r, r, 0, pi / 2);
    cairo_arc(cr, r, h - r, r, pi / 2, pi);
    cairo_arc(cr, r, r, r, pi, 3 * pi / 2);
    cairo_close_path(cr);
}

}  // namespace

const zwlr_layer_surface_v1_listener Overlay::kLayerListener = {
    .configure = [](void* data, zwlr_layer_surface_v1*, uint32_t serial, uint32_t width, uint32_t height) {
        static_cast<Overlay*>(data)->on_configure(serial, width, height);
    },
    .closed = [](void* data, zwlr_layer_surface_v1*) {
        log_debug("композитор закрыл оверлей");
        static_cast<Overlay*>(data)->destroy();
    },
};

const wl_surface_listener Overlay::kSurfaceListener = {
    .enter = [](void*, wl_surface*, wl_output*) {},
    .leave = [](void*, wl_surface*, wl_output*) {},
    .preferred_buffer_scale = [](void* data, wl_surface*, int32_t factor) {
        auto* self = static_cast<Overlay*>(data);
        if (factor < 1 || factor == self->buffer_scale_) return;
        self->buffer_scale_ = factor;
        if (!self->viewport_) self->redraw();
    },
    .preferred_buffer_transform = [](void*, wl_surface*, uint32_t) {},
};

const wp_fractional_scale_v1_listener Overlay::kFractionalListener = {
    .preferred_scale = [](void* data, wp_fractional_scale_v1*, uint32_t scale) {
        auto* self = static_cast<Overlay*>(data);
        const double s = scale / 120.0;
        if (s == self->fractional_scale_) return;
        self->fractional_scale_ = s;
        log_debug("масштаб оверлея %.3f", s);
        self->redraw();
    },
};

const wl_buffer_listener Overlay::kBufferListener = {
    .release = [](void* data, wl_buffer* buffer) {
        std::erase_if(static_cast<Overlay*>(data)->buffers_, [buffer](const auto& b) { return b->buffer == buffer; });
    },
};

const wl_callback_listener Overlay::kFrameListener = {
    .done = [](void* data, wl_callback* callback, uint32_t) {
        auto* self = static_cast<Overlay*>(data);
        wl_callback_destroy(callback);
        self->frame_callback_ = nullptr;
        auto done = std::move(self->frame_done_);
        self->frame_done_ = nullptr;
        if (done) done();
    },
};

Overlay::Overlay(Display& display, const Config& cfg) : d_(display), cfg_(cfg) {}

Overlay::~Overlay() {
    destroy();
}

void Overlay::move_to(Output* output) {
    if (layer_ && output == output_) return;
    destroy();
    output_ = output;
    create();
}

void Overlay::output_removed(Output* output) {
    if (output_ == output) destroy();
}

void Overlay::create() {
    surface_ = wl_compositor_create_surface(d_.compositor);
    wl_surface_add_listener(surface_, &kSurfaceListener, this);

    // Пустая область ввода: клики проходят сквозь плашку.
    wl_region* region = wl_compositor_create_region(d_.compositor);
    wl_surface_set_input_region(surface_, region);
    wl_region_destroy(region);

    if (d_.fractional_scale && d_.viewporter) {
        fractional_ = wp_fractional_scale_manager_v1_get_fractional_scale(d_.fractional_scale, surface_);
        wp_fractional_scale_v1_add_listener(fractional_, &kFractionalListener, this);
        viewport_ = wp_viewporter_get_viewport(d_.viewporter, surface_);
    }
    buffer_scale_ = output_ ? std::max(1, output_->scale) : 1;
    fractional_scale_ = 0;

    layer_ = zwlr_layer_shell_v1_get_layer_surface(d_.layer_shell, surface_, output_ ? output_->wl : nullptr,
                                                   ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "quizhint");
    zwlr_layer_surface_v1_add_listener(layer_, &kLayerListener, this);
    // Угол экрана и отступы от него.
    const bool bottom = cfg_.position.rfind("bottom", 0) == 0;
    const bool right = cfg_.position.size() > 5 && cfg_.position.compare(cfg_.position.size() - 5, 5, "right") == 0;
    zwlr_layer_surface_v1_set_anchor(layer_, (bottom ? ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM : ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) |
                                                 (right ? ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT : ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT));
    zwlr_layer_surface_v1_set_margin(layer_, bottom ? 0 : cfg_.margin_y, right ? cfg_.margin_x : 0,
                                     bottom ? cfg_.margin_y : 0, right ? 0 : cfg_.margin_x);
    zwlr_layer_surface_v1_set_exclusive_zone(layer_, cfg_.over_panels ? -1 : 0);
    zwlr_layer_surface_v1_set_keyboard_interactivity(layer_, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    req_w_ = req_h_ = 0;
    std::tie(req_w_, req_h_) = desired_size();
    zwlr_layer_surface_v1_set_size(layer_, req_w_, req_h_);
    wl_surface_commit(surface_);
}

void Overlay::destroy() {
    if (frame_callback_) {
        wl_callback_destroy(frame_callback_);
        frame_callback_ = nullptr;
    }
    frame_done_ = nullptr;
    if (layer_) {
        zwlr_layer_surface_v1_destroy(layer_);
        layer_ = nullptr;
    }
    if (viewport_) {
        wp_viewport_destroy(viewport_);
        viewport_ = nullptr;
    }
    if (fractional_) {
        wp_fractional_scale_v1_destroy(fractional_);
        fractional_ = nullptr;
    }
    if (surface_) {
        wl_surface_destroy(surface_);
        surface_ = nullptr;
    }
    buffers_.clear();
    configured_ = false;
    width_ = height_ = 0;
    req_w_ = req_h_ = 0;
    output_ = nullptr;
}

// Размер плашки для цифр постоянный, с запасом под «8,8,8,8»: так его не нужно
// менять при каждой новой цифре. Лишнее место прозрачно и пропускает клики.
std::pair<uint32_t, uint32_t> Overlay::badge_size() const {
    cairo_surface_t* scratch = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t* cr = cairo_create(scratch);
    setup_font(cr, cfg_);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, "8,8,8,8", &ext);
    cairo_destroy(cr);
    cairo_surface_destroy(scratch);
    const auto h = static_cast<uint32_t>(std::ceil(cfg_.font_size * 1.4));
    const auto w = std::max(h, static_cast<uint32_t>(std::ceil(ext.width + cfg_.font_size * 0.8)));
    return {w, h};
}

// Плашка полного ответа — точно по размеру текста.
std::pair<uint32_t, uint32_t> Overlay::text_size() const {
    cairo_surface_t* scratch = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t* cr = cairo_create(scratch);
    PangoLayout* layout = make_text_layout(cr, cfg_, text_);
    PangoRectangle logical{};
    pango_layout_get_extents(layout, nullptr, &logical);
    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(scratch);
    const double tw = static_cast<double>(logical.width) / PANGO_SCALE;
    const double th = static_cast<double>(logical.height) / PANGO_SCALE;
    return {static_cast<uint32_t>(std::ceil(tw + 2 * text_pad_x(cfg_))),
            static_cast<uint32_t>(std::ceil(th + 2 * text_pad_y(cfg_)))};
}

std::pair<uint32_t, uint32_t> Overlay::desired_size() const {
    if (text_.empty()) return req_w_ ? std::pair{req_w_, req_h_} : badge_size();  // спрятано — размер не трогаем
    return kind_ == Kind::Text ? text_size() : badge_size();
}

void Overlay::on_configure(uint32_t serial, uint32_t width, uint32_t height) {
    zwlr_layer_surface_v1_ack_configure(layer_, serial);
    if (width && height) {
        width_ = width;
        height_ = height;
    } else {
        width_ = req_w_;
        height_ = req_h_;
    }
    configured_ = true;
    redraw();
}

void Overlay::set(const std::string& text, Kind kind) {
    text_ = text;
    kind_ = text == kBusy ? Kind::Badge : kind;
    if (!layer_) {
        if (!text_.empty()) create();  // нарисуется после configure
        return;
    }
    if (!text_.empty()) {
        const auto [w, h] = desired_size();
        if (w != req_w_ || h != req_h_) {
            // Новый размер: просим его у композитора и рисуем, когда придёт configure.
            req_w_ = w;
            req_h_ = h;
            zwlr_layer_surface_v1_set_size(layer_, w, h);
            wl_surface_commit(surface_);
            if (configured_) return;
        }
    }
    redraw();
}

void Overlay::hide(std::function<void()> presented) {
    text_.clear();
    if (!layer_ || !configured_) {
        if (presented) presented();
        return;
    }
    if (frame_callback_) wl_callback_destroy(frame_callback_);
    frame_callback_ = wl_surface_frame(surface_);  // запрос до commit в redraw()
    wl_callback_add_listener(frame_callback_, &kFrameListener, this);
    frame_done_ = std::move(presented);
    redraw();
}

void Overlay::redraw() {
    if (!layer_ || !configured_ || !width_ || !height_) return;

    const bool fractional = viewport_ != nullptr;
    const double scale = fractional && fractional_scale_ > 0 ? fractional_scale_ : buffer_scale_;
    const int bw = fractional ? static_cast<int>(std::lround(width_ * scale)) : static_cast<int>(width_) * buffer_scale_;
    const int bh = fractional ? static_cast<int>(std::lround(height_ * scale)) : static_cast<int>(height_) * buffer_scale_;
    const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, bw);
    auto buf = ShmBuffer::create(d_.shm, bw, bh, stride, WL_SHM_FORMAT_ARGB8888);
    if (!buf) {
        log_info("не удалось выделить буфер для оверлея");
        return;
    }

    cairo_surface_t* cs = cairo_image_surface_create_for_data(buf->data, CAIRO_FORMAT_ARGB32, bw, bh, stride);
    cairo_t* cr = cairo_create(cs);
    cairo_scale(cr, static_cast<double>(bw) / width_, static_cast<double>(bh) / height_);
    paint(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(cs);

    wl_buffer_add_listener(buf->buffer, &kBufferListener, this);
    wl_surface_attach(surface_, buf->buffer, 0, 0);
    if (fractional) wp_viewport_set_destination(viewport_, static_cast<int32_t>(width_), static_cast<int32_t>(height_));
    else wl_surface_set_buffer_scale(surface_, buffer_scale_);
    if (d_.compositor_version >= WL_SURFACE_DAMAGE_BUFFER_SINCE_VERSION) wl_surface_damage_buffer(surface_, 0, 0, bw, bh);
    else wl_surface_damage(surface_, 0, 0, static_cast<int32_t>(width_), static_cast<int32_t>(height_));
    wl_surface_commit(surface_);
    buffers_.push_back(std::move(buf));
}

void Overlay::paint(cairo_t* cr) const {
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_restore(cr);
    if (text_.empty()) return;
    if (kind_ == Kind::Text) {
        paint_text(cr);
        return;
    }

    const double h = height_;
    const double fs = cfg_.font_size;
    const bool busy = text_ == kBusy;
    setup_font(cr, cfg_);
    cairo_text_extents_t ext{};
    double w = h;
    if (!busy) {
        cairo_text_extents(cr, text_.c_str(), &ext);
        w = std::min(std::max(h, std::ceil(ext.width + fs * 0.8)), static_cast<double>(width_));
    }

    rounded_rect(cr, w, h, h * 0.3);
    set_source(cr, cfg_.background);
    cairo_fill(cr);

    if (busy) {
        cairo_arc(cr, w / 2, h / 2, fs * 0.16, 0, 2 * std::numbers::pi);
        set_source(cr, cfg_.color, 0.8);
        cairo_fill(cr);
        return;
    }
    // По вертикали центрируем по высоте цифры, а не всей строки: запятая уходит ниже базовой линии.
    cairo_text_extents_t digit;
    cairo_text_extents(cr, "8", &digit);
    cairo_move_to(cr, (w - ext.width) / 2 - ext.x_bearing, (h - digit.height) / 2 - digit.y_bearing);
    set_source(cr, cfg_.color);
    cairo_show_text(cr, text_.c_str());
}

void Overlay::paint_text(cairo_t* cr) const {
    const double w = width_, h = height_;
    rounded_rect(cr, w, h, std::round(cfg_.full_font_size * 0.5));
    set_source(cr, cfg_.background);
    cairo_fill(cr);

    PangoLayout* layout = make_text_layout(cr, cfg_, text_);
    cairo_move_to(cr, text_pad_x(cfg_), text_pad_y(cfg_));
    set_source(cr, cfg_.color);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}
