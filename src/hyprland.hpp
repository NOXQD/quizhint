#pragma once

#include <optional>
#include <string>
#include <vector>

// Запросы к Hyprland через его IPC-сокет (без запуска hyprctl).
// Вне Hyprland функции возвращают nullopt.
namespace hypr {

struct Monitor {
    std::string name;           // имя коннектора, например eDP-1
    int x = 0, y = 0;           // положение в логических координатах
    int width = 0, height = 0;  // режим в физических пикселях
    double scale = 1;
    int transform = 0;          // wl_output.transform
    bool focused = false;
};

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;  // глобальные логические координаты
};

std::vector<Monitor> monitors();
std::optional<Monitor> focused_monitor();
std::optional<Rect> active_window();

}  // namespace hypr
