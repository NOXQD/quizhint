#include "display.hpp"

#include "protocols.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>

std::unique_ptr<ShmBuffer> ShmBuffer::create(wl_shm* shm, int width, int height, int stride, uint32_t format) {
    const size_t size = static_cast<size_t>(stride) * static_cast<size_t>(height);
    if (!shm || width <= 0 || height <= 0 || stride <= 0 || size > INT32_MAX) return nullptr;
    const int fd = memfd_create("quizhint", MFD_CLOEXEC);
    if (fd < 0) return nullptr;
    if (ftruncate(fd, static_cast<off_t>(size)) < 0) {
        close(fd);
        return nullptr;
    }
    void* data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return nullptr;
    }
    // libwayland дублирует fd при отправке запроса, свой можно закрыть сразу.
    wl_shm_pool* pool = wl_shm_create_pool(shm, fd, static_cast<int32_t>(size));
    close(fd);

    auto buf = std::make_unique<ShmBuffer>();
    buf->buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, format);
    wl_shm_pool_destroy(pool);
    buf->data = static_cast<uint8_t*>(data);
    buf->size = size;
    buf->width = width;
    buf->height = height;
    buf->stride = stride;
    buf->format = format;
    return buf;
}

ShmBuffer::~ShmBuffer() {
    if (buffer) wl_buffer_destroy(buffer);
    if (data) munmap(data, size);
}

const wl_registry_listener Display::kRegistryListener = {
    .global = [](void* data, wl_registry*, uint32_t name, const char* interface, uint32_t version) {
        static_cast<Display*>(data)->on_global(name, interface, version);
    },
    .global_remove = [](void* data, wl_registry*, uint32_t name) {
        static_cast<Display*>(data)->on_global_remove(name);
    },
};

const wl_output_listener Display::kOutputListener = {
    .geometry = [](void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*, const char*,
                   int32_t) {},
    .mode = [](void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) {},
    .done = [](void*, wl_output*) {},
    .scale = [](void* data, wl_output*, int32_t factor) { static_cast<Output*>(data)->scale = factor; },
    .name = [](void* data, wl_output*, const char* name) { static_cast<Output*>(data)->name = name; },
    .description = [](void*, wl_output*, const char*) {},
};

namespace {

template <typename T>
T* bind(wl_registry* registry, uint32_t name, const wl_interface* interface, uint32_t version) {
    return static_cast<T*>(wl_registry_bind(registry, name, interface, version));
}

}  // namespace

void Display::on_global(uint32_t name, const char* interface, uint32_t version) {
    const std::string_view iface(interface);
    if (iface == wl_compositor_interface.name) {
        compositor_version = std::min(version, 6u);
        compositor = bind<wl_compositor>(registry_, name, &wl_compositor_interface, compositor_version);
    } else if (iface == wl_shm_interface.name) {
        shm = bind<wl_shm>(registry_, name, &wl_shm_interface, 1);
    } else if (iface == wl_output_interface.name) {
        auto out = std::make_unique<Output>();
        out->global = name;
        out->wl = bind<wl_output>(registry_, name, &wl_output_interface, std::min(version, 4u));
        wl_output_add_listener(out->wl, &kOutputListener, out.get());
        outputs_.push_back(std::move(out));
    } else if (iface == zwlr_layer_shell_v1_interface.name) {
        layer_shell = bind<zwlr_layer_shell_v1>(registry_, name, &zwlr_layer_shell_v1_interface, std::min(version, 4u));
    } else if (iface == zwlr_screencopy_manager_v1_interface.name) {
        wlr_screencopy = bind<zwlr_screencopy_manager_v1>(registry_, name, &zwlr_screencopy_manager_v1_interface,
                                                          std::min(version, 3u));
    } else if (iface == ext_image_copy_capture_manager_v1_interface.name) {
        ext_copy_capture = bind<ext_image_copy_capture_manager_v1>(registry_, name,
                                                                   &ext_image_copy_capture_manager_v1_interface, 1);
    } else if (iface == ext_output_image_capture_source_manager_v1_interface.name) {
        ext_output_source = bind<ext_output_image_capture_source_manager_v1>(
            registry_, name, &ext_output_image_capture_source_manager_v1_interface, 1);
    } else if (iface == wp_fractional_scale_manager_v1_interface.name) {
        fractional_scale = bind<wp_fractional_scale_manager_v1>(registry_, name,
                                                                &wp_fractional_scale_manager_v1_interface, 1);
    } else if (iface == wp_viewporter_interface.name) {
        viewporter = bind<wp_viewporter>(registry_, name, &wp_viewporter_interface, 1);
    }
}

void Display::on_global_remove(uint32_t name) {
    const auto it = std::find_if(outputs_.begin(), outputs_.end(), [name](const auto& o) { return o->global == name; });
    if (it == outputs_.end()) return;
    if (on_output_removed) on_output_removed(it->get());
    if (wl_output_get_version((*it)->wl) >= WL_OUTPUT_RELEASE_SINCE_VERSION) wl_output_release((*it)->wl);
    else wl_output_destroy((*it)->wl);
    outputs_.erase(it);
}

bool Display::connect(std::string& error) {
    display = wl_display_connect(nullptr);
    if (!display) {
        error = "не удалось подключиться к Wayland-композитору (WAYLAND_DISPLAY не задан?)";
        return false;
    }
    registry_ = wl_display_get_registry(display);
    wl_registry_add_listener(registry_, &kRegistryListener, this);
    // Первый roundtrip — глобальные объекты, второй — их начальные события (имена мониторов).
    if (wl_display_roundtrip(display) < 0 || wl_display_roundtrip(display) < 0) {
        error = "ошибка Wayland при инициализации";
        return false;
    }
    if (!compositor || !shm) {
        error = "композитор не предоставляет wl_compositor или wl_shm";
        return false;
    }
    if (!layer_shell) {
        error = "композитор не поддерживает wlr-layer-shell (нужен Hyprland, Sway, niri или другой с layer-shell)";
        return false;
    }
    return true;
}

Display::~Display() {
    if (!display) return;
    for (auto& o : outputs_) wl_output_destroy(o->wl);
    outputs_.clear();
    if (viewporter) wp_viewporter_destroy(viewporter);
    if (fractional_scale) wp_fractional_scale_manager_v1_destroy(fractional_scale);
    if (ext_output_source) ext_output_image_capture_source_manager_v1_destroy(ext_output_source);
    if (ext_copy_capture) ext_image_copy_capture_manager_v1_destroy(ext_copy_capture);
    if (wlr_screencopy) zwlr_screencopy_manager_v1_destroy(wlr_screencopy);
    if (layer_shell) {
        if (zwlr_layer_shell_v1_get_version(layer_shell) >= ZWLR_LAYER_SHELL_V1_DESTROY_SINCE_VERSION)
            zwlr_layer_shell_v1_destroy(layer_shell);
        else
            wl_proxy_destroy(reinterpret_cast<wl_proxy*>(layer_shell));
    }
    if (shm) wl_shm_destroy(shm);
    if (compositor) wl_compositor_destroy(compositor);
    if (registry_) wl_registry_destroy(registry_);
    wl_display_flush(display);
    wl_display_disconnect(display);
}

bool Display::prepare_poll() {
    while (wl_display_prepare_read(display) != 0) {
        if (wl_display_dispatch_pending(display) < 0) return false;
    }
    if (wl_display_flush(display) < 0 && errno != EAGAIN) {
        wl_display_cancel_read(display);
        return false;
    }
    return true;
}

bool Display::finish_poll(bool readable) {
    if (readable) {
        if (wl_display_read_events(display) < 0) return false;
    } else {
        wl_display_cancel_read(display);
    }
    return wl_display_dispatch_pending(display) >= 0;
}

Output* Display::output_by_name(const std::string& name) const {
    for (const auto& o : outputs_) {
        if (!name.empty() && o->name == name) return o.get();
    }
    return nullptr;
}
