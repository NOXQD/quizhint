#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "display.hpp"
#include "image.hpp"

struct ext_image_capture_source_v1;
struct ext_image_copy_capture_session_v1;
struct ext_image_copy_capture_session_v1_listener;
struct ext_image_copy_capture_frame_v1;
struct ext_image_copy_capture_frame_v1_listener;
struct zwlr_screencopy_frame_v1;
struct zwlr_screencopy_frame_v1_listener;

// Снимок монитора без внешних утилит: ext-image-copy-capture-v1, а если его
// нет или он не сработал — wlr-screencopy-unstable-v1. Курсор на снимок не попадает.
class Capturer {
public:
    // frame == nullptr — ошибка (описание в error). Данные кадра действительны
    // только во время вызова.
    using Callback = std::function<void(const RawFrame* frame, const std::string& error)>;

    Capturer(Display& display, std::string protocol);  // auto | ext | wlr
    ~Capturer();
    Capturer(const Capturer&) = delete;
    Capturer& operator=(const Capturer&) = delete;

    void start(Output* output, Callback done);
    void cancel();
    bool busy() const { return active_; }
    void output_removed(Output* output);
    const char* backend_name() const;

private:
    enum class Backend { None, Ext, Wlr };

    static const ext_image_copy_capture_session_v1_listener kExtSessionListener;
    static const ext_image_copy_capture_frame_v1_listener kExtFrameListener;
    static const zwlr_screencopy_frame_v1_listener kWlrFrameListener;

    void start_ext();
    void start_wlr();
    void ext_constraints_done();
    void wlr_copy();
    void ready();
    void fail(const std::string& message);
    void finish(const RawFrame* frame, const std::string& error);
    void release();

    Display& d_;
    std::string protocol_;
    Callback done_;
    Output* output_ = nullptr;
    bool active_ = false;
    bool ext_failed_ = false;  // ext однажды не сработал — дальше сразу wlr
    Backend backend_ = Backend::None;
    Backend last_backend_ = Backend::None;
    std::unique_ptr<ShmBuffer> buffer_;
    uint32_t format_ = 0, width_ = 0, height_ = 0, stride_ = 0;
    bool y_invert_ = false;

    std::vector<uint32_t> ext_formats_;
    ext_image_capture_source_v1* ext_source_ = nullptr;
    ext_image_copy_capture_session_v1* ext_session_ = nullptr;
    ext_image_copy_capture_frame_v1* ext_frame_ = nullptr;

    zwlr_screencopy_frame_v1* wlr_frame_ = nullptr;
    bool wlr_has_shm_ = false;
    bool wlr_copying_ = false;
};
