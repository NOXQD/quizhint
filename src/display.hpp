#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <wayland-client.h>

struct zwlr_layer_shell_v1;
struct zwlr_screencopy_manager_v1;
struct ext_image_copy_capture_manager_v1;
struct ext_output_image_capture_source_manager_v1;
struct wp_fractional_scale_manager_v1;
struct wp_viewporter;

struct Output {
    wl_output* wl = nullptr;
    uint32_t global = 0;
    std::string name;   // имя коннектора, например eDP-1
    int32_t scale = 1;  // целый масштаб из wl_output
};

// wl_buffer поверх memfd.
struct ShmBuffer {
    wl_buffer* buffer = nullptr;
    uint8_t* data = nullptr;
    size_t size = 0;
    int width = 0, height = 0, stride = 0;
    uint32_t format = 0;

    static std::unique_ptr<ShmBuffer> create(wl_shm* shm, int width, int height, int stride, uint32_t format);

    ShmBuffer() = default;
    ~ShmBuffer();
    ShmBuffer(const ShmBuffer&) = delete;
    ShmBuffer& operator=(const ShmBuffer&) = delete;
};

// Подключение к композитору и глобальные объекты.
class Display {
public:
    Display() = default;
    ~Display();
    Display(const Display&) = delete;
    Display& operator=(const Display&) = delete;

    bool connect(std::string& error);
    int fd() const { return wl_display_get_fd(display); }

    // Вызываются вокруг poll(): prepare_poll разбирает очередь и отправляет
    // запросы, finish_poll читает события (если сокет готов) и обрабатывает их.
    bool prepare_poll();
    bool finish_poll(bool readable);
    void cancel_poll() { wl_display_cancel_read(display); }

    Output* output_by_name(const std::string& name) const;
    Output* first_output() const { return outputs_.empty() ? nullptr : outputs_.front().get(); }
    const std::vector<std::unique_ptr<Output>>& outputs() const { return outputs_; }

    // Монитор отключён; вызывается до уничтожения Output.
    std::function<void(Output*)> on_output_removed;

    wl_display* display = nullptr;
    wl_compositor* compositor = nullptr;
    uint32_t compositor_version = 0;
    wl_shm* shm = nullptr;
    zwlr_layer_shell_v1* layer_shell = nullptr;
    zwlr_screencopy_manager_v1* wlr_screencopy = nullptr;
    ext_image_copy_capture_manager_v1* ext_copy_capture = nullptr;
    ext_output_image_capture_source_manager_v1* ext_output_source = nullptr;
    wp_fractional_scale_manager_v1* fractional_scale = nullptr;
    wp_viewporter* viewporter = nullptr;

private:
    static const wl_registry_listener kRegistryListener;
    static const wl_output_listener kOutputListener;

    void on_global(uint32_t name, const char* interface, uint32_t version);
    void on_global_remove(uint32_t name);

    wl_registry* registry_ = nullptr;
    std::vector<std::unique_ptr<Output>> outputs_;
};
