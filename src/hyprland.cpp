#include "hyprland.hpp"

#include "json.hpp"

#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace hypr {

namespace {

std::optional<std::string> request(const std::string& command) {
    const char* signature = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!signature || !*signature) return std::nullopt;
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    std::string path = std::string(runtime && *runtime ? runtime : "/tmp") + "/hypr/" + signature + "/.socket.sock";
    if (access(path.c_str(), F_OK) != 0) path = std::string("/tmp/hypr/") + signature + "/.socket.sock";

    sockaddr_un addr{};
    if (path.size() >= sizeof addr.sun_path) return std::nullopt;
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);

    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return std::nullopt;
    const timeval timeout{1, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);

    std::string reply;
    if (connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == 0 &&
        write(fd, command.data(), command.size()) == static_cast<ssize_t>(command.size())) {
        char buf[8192];
        ssize_t n;
        while ((n = read(fd, buf, sizeof buf)) > 0) reply.append(buf, static_cast<size_t>(n));
    }
    close(fd);
    if (reply.empty()) return std::nullopt;
    return reply;
}

}  // namespace

std::vector<Monitor> monitors() {
    std::vector<Monitor> out;
    const auto reply = request("j/monitors");
    if (!reply) return out;
    const auto list = json::parse(*reply);
    if (!list) return out;
    for (const auto& m : list->items()) {
        Monitor mon;
        mon.name = m["name"].str();
        mon.x = static_cast<int>(m["x"].num());
        mon.y = static_cast<int>(m["y"].num());
        mon.width = static_cast<int>(m["width"].num());
        mon.height = static_cast<int>(m["height"].num());
        mon.scale = m["scale"].num(1);
        mon.transform = static_cast<int>(m["transform"].num());
        mon.focused = m["focused"].boolean();
        out.push_back(std::move(mon));
    }
    return out;
}

std::optional<Monitor> focused_monitor() {
    for (auto& m : monitors()) {
        if (m.focused) return m;
    }
    return std::nullopt;
}

std::optional<Rect> active_window() {
    const auto reply = request("j/activewindow");
    if (!reply) return std::nullopt;
    const auto window = json::parse(*reply);
    if (!window) return std::nullopt;
    const auto& at = (*window)["at"];
    const auto& size = (*window)["size"];
    if (at.size() < 2 || size.size() < 2) return std::nullopt;
    Rect r;
    r.x = static_cast<int>(at.at(0).num());
    r.y = static_cast<int>(at.at(1).num());
    r.w = static_cast<int>(size.at(0).num());
    r.h = static_cast<int>(size.at(1).num());
    if (r.w <= 0 || r.h <= 0) return std::nullopt;
    return r;
}

}  // namespace hypr
