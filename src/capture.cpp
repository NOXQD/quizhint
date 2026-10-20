#include "capture.hpp"

#include "log.hpp"
#include "protocols.hpp"

#include <algorithm>
#include <cstdio>

namespace {

// Предпочтительные форматы буфера, если композитор предлагает несколько.
constexpr uint32_t kPreferredFormats[] = {
    WL_SHM_FORMAT_XRGB8888,    WL_SHM_FORMAT_ARGB8888,    WL_SHM_FORMAT_XBGR8888,    WL_SHM_FORMAT_ABGR8888,
    WL_SHM_FORMAT_XRGB2101010, WL_SHM_FORMAT_ARGB2101010, WL_SHM_FORMAT_XBGR2101010, WL_SHM_FORMAT_ABGR2101010,
    WL_SHM_FORMAT_RGB888,      WL_SHM_FORMAT_BGR888,
};

}  // namespace

const ext_image_copy_capture_session_v1_listener Capturer::kExtSessionListener = {
    .buffer_size = [](void* data, ext_image_copy_capture_session_v1*, uint32_t width, uint32_t height) {
        auto* self = static_cast<Capturer*>(data);
        self->width_ = width;
        self->height_ = height;
    },
    .shm_format = [](void* data, ext_image_copy_capture_session_v1*, uint32_t format) {
        static_cast<Capturer*>(data)->ext_formats_.push_back(format);
    },
    .dmabuf_device = [](void*, ext_image_copy_capture_session_v1*, wl_array*) {},
    .dmabuf_format = [](void*, ext_image_copy_capture_session_v1*, uint32_t, wl_array*) {},
    .done = [](void* data, ext_image_copy_capture_session_v1*) { static_cast<Capturer*>(data)->ext_constraints_done(); },
    .stopped = [](void* data, ext_image_copy_capture_session_v1*) {
        static_cast<Capturer*>(data)->fail("сессия захвата остановлена композитором");
    },
};

const ext_image_copy_capture_frame_v1_listener Capturer::kExtFrameListener = {
    .transform = [](void*, ext_image_copy_capture_frame_v1*, uint32_t) {},
    .damage = [](void*, ext_image_copy_capture_frame_v1*, int32_t, int32_t, int32_t, int32_t) {},
    .presentation_time = [](void*, ext_image_copy_capture_frame_v1*, uint32_t, uint32_t, uint32_t) {},
    .ready = [](void* data, ext_image_copy_capture_frame_v1*) { static_cast<Capturer*>(data)->ready(); },
    .failed = [](void* data, ext_image_copy_capture_frame_v1*, uint32_t reason) {
        static_cast<Capturer*>(data)->fail("кадр не снят, причина " + std::to_string(reason));
    },
};

const zwlr_screencopy_frame_v1_listener Capturer::kWlrFrameListener = {
    .buffer = [](void* data, zwlr_screencopy_frame_v1* frame, uint32_t format, uint32_t width, uint32_t height,
                 uint32_t stride) {
        auto* self = static_cast<Capturer*>(data);
        self->format_ = format;
        self->width_ = width;
        self->height_ = height;
        self->stride_ = stride;
        self->wlr_has_shm_ = true;
        // До версии 3 нет события buffer_done: копируем сразу.
        if (zwlr_screencopy_frame_v1_get_version(frame) < 3) self->wlr_copy();
    },
    .flags = [](void* data, zwlr_screencopy_frame_v1*, uint32_t flags) {
        static_cast<Capturer*>(data)->y_invert_ = flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
    },
    .ready = [](void* data, zwlr_screencopy_frame_v1*, uint32_t, uint32_t, uint32_t) {
        static_cast<Capturer*>(data)->ready();
    },
    .failed = [](void* data, zwlr_screencopy_frame_v1*) { static_cast<Capturer*>(data)->fail("кадр не снят"); },
    .damage = [](void*, zwlr_screencopy_frame_v1*, uint32_t, uint32_t, uint32_t, uint32_t) {},
    .linux_dmabuf = [](void*, zwlr_screencopy_frame_v1*, uint32_t, uint32_t, uint32_t) {},
    .buffer_done = [](void* data, zwlr_screencopy_frame_v1*) { static_cast<Capturer*>(data)->wlr_copy(); },
};

Capturer::Capturer(Display& display, std::string protocol) : d_(display), protocol_(std::move(protocol)) {}

Capturer::~Capturer() {
    cancel();
}

const char* Capturer::backend_name() const {
    switch (backend_ != Backend::None ? backend_ : last_backend_) {
    case Backend::Ext: return "ext-image-copy-capture";
    case Backend::Wlr: return "wlr-screencopy";
    default: return "—";
    }
}

void Capturer::start(Output* output, Callback done) {
    cancel();
    done_ = std::move(done);
    output_ = output;
    active_ = true;
    if (!output_) {
        finish(nullptr, "нет монитора для снимка");
        return;
    }
    const bool ext = d_.ext_copy_capture && d_.ext_output_source && protocol_ != "wlr" && !ext_failed_;
    const bool wlr = d_.wlr_screencopy && protocol_ != "ext";
    if (ext) start_ext();
    else if (wlr) start_wlr();
    else finish(nullptr, "композитор не поддерживает ни ext-image-copy-capture-v1, ни wlr-screencopy");
}

void Capturer::cancel() {
    done_ = nullptr;
    release();
    active_ = false;
}

void Capturer::output_removed(Output* output) {
    if (active_ && output_ == output) finish(nullptr, "монитор отключён");
}

void Capturer::start_ext() {
    backend_ = Backend::Ext;
    width_ = height_ = 0;
    ext_formats_.clear();
    ext_source_ = ext_output_image_capture_source_manager_v1_create_source(d_.ext_output_source, output_->wl);
    ext_session_ = ext_image_copy_capture_manager_v1_create_session(d_.ext_copy_capture, ext_source_, 0);
    ext_image_copy_capture_session_v1_add_listener(ext_session_, &kExtSessionListener, this);
}

// Композитор прислал ограничения на буфер — выделяем его и снимаем кадр.
// Повторный done (ограничения изменились) во время съёмки игнорируем.
void Capturer::ext_constraints_done() {
    if (!active_ || backend_ != Backend::Ext || ext_frame_) return;
    const uint32_t* format = nullptr;
    for (const uint32_t& f : kPreferredFormats) {
        if (std::find(ext_formats_.begin(), ext_formats_.end(), f) != ext_formats_.end()) {
            format = &f;
            break;
        }
    }
    if (!format || !width_ || !height_) {
        fail("композитор не предложил подходящий формат буфера");
        return;
    }
    const int bpp = shm_format_bpp(*format);
    buffer_ = ShmBuffer::create(d_.shm, static_cast<int>(width_), static_cast<int>(height_),
                                static_cast<int>(width_) * bpp, *format);
    if (!buffer_) {
        fail("не удалось выделить память под кадр");
        return;
    }
    y_invert_ = false;
    ext_frame_ = ext_image_copy_capture_session_v1_create_frame(ext_session_);
    ext_image_copy_capture_frame_v1_add_listener(ext_frame_, &kExtFrameListener, this);
    ext_image_copy_capture_frame_v1_attach_buffer(ext_frame_, buffer_->buffer);
    ext_image_copy_capture_frame_v1_damage_buffer(ext_frame_, 0, 0, static_cast<int32_t>(width_),
                                                  static_cast<int32_t>(height_));
    ext_image_copy_capture_frame_v1_capture(ext_frame_);
}

void Capturer::start_wlr() {
    backend_ = Backend::Wlr;
    wlr_has_shm_ = false;
    wlr_copying_ = false;
    y_invert_ = false;
    wlr_frame_ = zwlr_screencopy_manager_v1_capture_output(d_.wlr_screencopy, 0, output_->wl);
    zwlr_screencopy_frame_v1_add_listener(wlr_frame_, &kWlrFrameListener, this);
}

void Capturer::wlr_copy() {
    if (!active_ || backend_ != Backend::Wlr || wlr_copying_) return;
    if (!wlr_has_shm_) {
        fail("wlr-screencopy не предлагает буфер в разделяемой памяти");
        return;
    }
    if (!shm_format_bpp(format_)) {
        char buf[128];
        std::snprintf(buf, sizeof buf, "неподдерживаемый формат пикселей 0x%08x", format_);
        fail(buf);
        return;
    }
    buffer_ = ShmBuffer::create(d_.shm, static_cast<int>(width_), static_cast<int>(height_),
                                static_cast<int>(stride_), format_);
    if (!buffer_) {
        fail("не удалось выделить память под кадр");
        return;
    }
    wlr_copying_ = true;
    zwlr_screencopy_frame_v1_copy(wlr_frame_, buffer_->buffer);
}

void Capturer::ready() {
    if (!active_ || !buffer_) return;
    RawFrame frame;
    frame.data = buffer_->data;
    frame.format = buffer_->format;
    frame.width = buffer_->width;
    frame.height = buffer_->height;
    frame.stride = buffer_->stride;
    frame.y_invert = y_invert_;
    finish(&frame, {});
}

void Capturer::fail(const std::string& message) {
    if (!active_) return;
    if (backend_ == Backend::Ext && d_.wlr_screencopy && protocol_ == "auto") {
        log_info("ext-image-copy-capture: %s — пробую wlr-screencopy", message.c_str());
        ext_failed_ = true;
        release();
        start_wlr();
        return;
    }
    finish(nullptr, message);
}

void Capturer::finish(const RawFrame* frame, const std::string& error) {
    auto buffer = std::move(buffer_);  // данные кадра нужны до конца вызова done
    auto done = std::move(done_);
    done_ = nullptr;
    release();
    active_ = false;
    if (done) done(frame, error);
}

void Capturer::release() {
    if (ext_frame_) {
        ext_image_copy_capture_frame_v1_destroy(ext_frame_);
        ext_frame_ = nullptr;
    }
    if (ext_session_) {
        ext_image_copy_capture_session_v1_destroy(ext_session_);
        ext_session_ = nullptr;
    }
    if (ext_source_) {
        ext_image_capture_source_v1_destroy(ext_source_);
        ext_source_ = nullptr;
    }
    if (wlr_frame_) {
        zwlr_screencopy_frame_v1_destroy(wlr_frame_);
        wlr_frame_ = nullptr;
    }
    buffer_.reset();
    if (backend_ != Backend::None) last_backend_ = backend_;
    backend_ = Backend::None;
}
