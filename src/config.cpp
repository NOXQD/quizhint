#include "config.hpp"

#include "json.hpp"
#include "log.hpp"

#include <cctype>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

// Текст config.example, встраивается при сборке (см. Makefile).
extern const char kConfigExample[];

namespace {

std::string trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

// "строка в кавычках" → строка; внутри понимаются \" \\ \n \t.
std::string unquote(const std::string& v) {
    if (v.size() < 2 || v.front() != '"' || v.back() != '"') return v;
    std::string out;
    for (size_t i = 1; i + 1 < v.size(); ++i) {
        if (v[i] == '\\' && i + 2 < v.size()) {
            const char c = v[++i];
            out += c == 'n' ? '\n' : c == 't' ? '\t' : c;
        } else {
            out += v[i];
        }
    }
    return out;
}

// Обратное к unquote: кавычки только там, где без них значение прочитается иначе.
std::string format_value(const std::string& v) {
    const bool quoted = !v.empty() && (std::isspace(static_cast<unsigned char>(v.front())) ||
                                       std::isspace(static_cast<unsigned char>(v.back())) || v.front() == '"' ||
                                       v.find_first_of("\n\t") != std::string::npos);
    if (!quoted) return v;
    std::string out = "\"";
    for (const char c : v) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\t') {
            out += "\\t";
        } else {
            out += c;
        }
    }
    return out + "\"";
}

template <typename T>
bool parse_number(const std::string& v, T& out) {
    const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
    return ec == std::errc() && ptr == v.data() + v.size();
}

std::string format_number(double d) {
    char buf[32];
    const auto res = std::to_chars(buf, buf + sizeof buf, d);
    return std::string(buf, res.ptr);
}

bool parse_bool(const std::string& v, bool& out) {
    if (v == "true" || v == "yes" || v == "on" || v == "1") {
        out = true;
        return true;
    }
    if (v == "false" || v == "no" || v == "off" || v == "0") {
        out = false;
        return true;
    }
    return false;
}

// #rrggbb или #rrggbbaa
bool parse_color(const std::string& v, Rgba& out) {
    std::string hex = v;
    if (!hex.empty() && hex[0] == '#') hex.erase(0, 1);
    if (hex.size() != 6 && hex.size() != 8) return false;
    uint32_t value = 0;
    const auto [ptr, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), value, 16);
    if (ec != std::errc() || ptr != hex.data() + hex.size()) return false;
    if (hex.size() == 6) value = (value << 8) | 0xff;
    out.r = ((value >> 24) & 0xff) / 255.0;
    out.g = ((value >> 16) & 0xff) / 255.0;
    out.b = ((value >> 8) & 0xff) / 255.0;
    out.a = (value & 0xff) / 255.0;
    return true;
}

std::string format_color(const Rgba& c) {
    char buf[16];
    auto byte = [](double v) { return static_cast<unsigned>(v * 255.0 + 0.5); };
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x%02x", byte(c.r), byte(c.g), byte(c.b), byte(c.a));
    return buf;
}

// Сочетание клавиш попадает в Lua-строку hyprctl eval, поэтому только безопасные символы.
bool valid_hotkey(const std::string& v) {
    return v.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_+: ") ==
           std::string::npos;
}

bool apply(Config& c, const std::string& key, const std::string& value, std::string& error) {
    auto bad = [&] {
        error = "неверное значение " + key + " = " + value;
        return false;
    };
    int i = 0;
    double d = 0;
    bool b = false;

    if (key == "provider") c.provider = value;
    else if (key == "model") c.model = value;
    else if (key == "api_key") c.api_key = value;
    else if (key == "anthropic_api_key") c.anthropic_api_key = value;
    else if (key == "gemini_api_key") c.gemini_api_key = value;
    else if (key == "openai_api_key") c.openai_api_key = value;
    else if (key == "base_url") c.base_url = value;
    else if (key == "thinking") {
        if (!value.empty() && value != "minimal" && value != "low" && value != "medium" && value != "high") return bad();
        c.thinking = value;
    }
    else if (key == "extra") c.extra = value;
    else if (key == "prompt") c.prompt = value;
    else if (key == "capture") c.capture = value;
    else if (key == "capture_protocol") c.capture_protocol = value;
    else if (key == "font") c.font = value;
    else if (key == "max_tokens") {
        if (!parse_number(value, i) || i <= 0) return bad();
        c.max_tokens = i;
    } else if (key == "temperature") {
        if (value.empty()) c.temperature.reset();
        else if (!parse_number(value, d)) return bad();
        else c.temperature = d;
    } else if (key == "timeout") {
        if (!parse_number(value, c.timeout)) return bad();
    } else if (key == "max_size") {
        if (!parse_number(value, c.max_size)) return bad();
    } else if (key == "jpeg_quality") {
        if (!parse_number(value, c.jpeg_quality)) return bad();
    } else if (key == "font_size") {
        if (!parse_number(value, c.font_size)) return bad();
    } else if (key == "color") {
        if (!parse_color(value, c.color)) return bad();
    } else if (key == "background") {
        if (!parse_color(value, c.background)) return bad();
    } else if (key == "margin_top") {
        if (!parse_number(value, c.margin_top)) return bad();
    } else if (key == "margin_left") {
        if (!parse_number(value, c.margin_left)) return bad();
    } else if (key == "over_panels") {
        if (!parse_bool(value, b)) return bad();
        c.over_panels = b;
    } else if (key == "hide_after") {
        if (!parse_number(value, c.hide_after)) return bad();
    } else if (key == "busy_indicator") {
        if (!parse_bool(value, b)) return bad();
        c.busy_indicator = b;
    } else if (key == "hotkey") {
        if (!valid_hotkey(value)) return bad();
        c.hotkey = value;
    } else if (key == "autostart") {
        if (!parse_bool(value, b)) return bad();
        c.autostart = b;
    } else {
        error = "неизвестный параметр " + key;
        return false;
    }
    return true;
}

// Ключ строки «key = value». commented — искать в закомментированной строке
// («#key = value» из примера). Строки-пояснения ключом не считаются.
std::string line_key(const std::string& line, bool commented) {
    std::string t = trim(line);
    if (t.empty()) return {};
    if (commented) {
        if (t[0] != '#') return {};
        const size_t start = t.find_first_not_of('#');
        if (start == std::string::npos) return {};
        t = trim(std::string_view(t).substr(start));
    } else if (t[0] == '#' || t[0] == ';') {
        return {};
    }
    const size_t eq = t.find('=');
    if (eq == std::string::npos) return {};
    std::string key = trim(std::string_view(t).substr(0, eq));
    if (key.empty() || key.find_first_not_of("abcdefghijklmnopqrstuvwxyz_") != std::string::npos) return {};
    return key;
}

bool read_text(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool make_dirs(const std::string& dir) {
    size_t pos = 0;
    do {
        pos = dir.find('/', pos + 1);
        const std::string part = dir.substr(0, pos);
        if (mkdir(part.c_str(), 0700) != 0 && errno != EEXIST) return false;
    } while (pos != std::string::npos);
    return true;
}

// Запись через временный файл и rename: файл никогда не остаётся записанным наполовину.
bool write_atomic(const std::string& path, const std::string& content, std::string& error) {
    const size_t slash = path.rfind('/');
    if (slash != std::string::npos && slash > 0 && !make_dirs(path.substr(0, slash))) {
        error = "не удалось создать каталог для " + path + ": " + std::strerror(errno);
        return false;
    }
    std::string tmp = path + ".XXXXXX";
    const int fd = mkstemp(tmp.data());  // права 0600
    if (fd < 0) {
        error = "не удалось записать " + path + ": " + std::strerror(errno);
        return false;
    }
    size_t done = 0;
    while (done < content.size()) {
        const ssize_t n = write(fd, content.data() + done, content.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += static_cast<size_t>(n);
    }
    const bool ok = done == content.size() && fsync(fd) == 0;
    close(fd);
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
        error = "не удалось записать " + path + ": " + std::strerror(errno);
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

}  // namespace

std::string default_config_path() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) return std::string(xdg) + "/quizhint/config";
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.config/quizhint/config";
}

bool parse_config(const std::string& text, const std::string& path, Config& cfg, std::string& error) {
    std::istringstream in(text);
    std::string line;
    for (int n = 1; std::getline(in, line); ++n) {
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;
        const size_t eq = t.find('=');
        if (eq == std::string::npos) {
            error = path + ":" + std::to_string(n) + ": ожидается «ключ = значение»";
            return false;
        }
        const std::string key = trim(std::string_view(t).substr(0, eq));
        const std::string value = unquote(trim(std::string_view(t).substr(eq + 1)));
        std::string e;
        if (!apply(cfg, key, value, e)) {
            error = path + ":" + std::to_string(n) + ": " + e;
            return false;
        }
    }
    return true;
}

bool load_config(const std::string& path, bool explicit_path, Config& cfg, std::string& error) {
    if (access(path.c_str(), F_OK) != 0) {
        if (!explicit_path) return true;
        error = "файл не найден: " + path;
        return false;
    }
    std::string text;
    if (!read_text(path, text)) {
        error = "не удалось прочитать " + path;
        return false;
    }
    if (!parse_config(text, path, cfg, error)) return false;
    struct stat st{};
    const bool has_key = !cfg.api_key.empty() || !cfg.anthropic_api_key.empty() || !cfg.gemini_api_key.empty() ||
                         !cfg.openai_api_key.empty();
    if (has_key && stat(path.c_str(), &st) == 0 && (st.st_mode & 077)) {
        log_info("внимание: в %s лежит ключ API, а файл доступен другим пользователям — chmod 600", path.c_str());
    }
    return true;
}

bool finalize_config(Config& c, bool need_api, std::string& error) {
    auto fail = [&](const std::string& message) {
        error = message;
        return false;
    };
    if (c.provider != "anthropic" && c.provider != "gemini" && c.provider != "openai")
        return fail("provider: anthropic, gemini или openai");
    if (c.capture != "monitor" && c.capture != "window") return fail("capture: monitor или window");
    if (c.capture_protocol != "auto" && c.capture_protocol != "ext" && c.capture_protocol != "wlr")
        return fail("capture_protocol: auto, ext или wlr");
    if (c.jpeg_quality < 1 || c.jpeg_quality > 100) return fail("jpeg_quality: от 1 до 100");
    if (c.max_size != 0 && c.max_size < 64) return fail("max_size: 0 или не меньше 64");
    if (c.font_size < 4 || c.font_size > 200) return fail("font_size: от 4 до 200");
    if (c.timeout < 1 || c.timeout > 600) return fail("timeout: от 1 до 600 секунд");
    if (c.hide_after < 0) return fail("hide_after не может быть отрицательным");
    if (!c.extra.empty()) {
        const auto v = json::parse(c.extra);
        if (!v || !v->is_object()) return fail("extra должен быть JSON-объектом, например {\"top_p\": 1}");
    }

    const bool anthropic = c.provider == "anthropic";
    const bool gemini = c.provider == "gemini";
    if (c.base_url.empty()) {
        c.base_url = anthropic ? "https://api.anthropic.com"
                     : gemini  ? "https://generativelanguage.googleapis.com/v1beta"
                               : "https://api.openai.com/v1";
    }
    while (!c.base_url.empty() && c.base_url.back() == '/') c.base_url.pop_back();
    if (c.model.empty()) {
        if (anthropic) c.model = "claude-haiku-5-5";
        // Псевдоним свежей Flash-Lite: отвечает за 1–2 с. Старшие Flash заметно медленнее,
        // а под нагрузкой могут не ответить и за минуту.
        else if (gemini) c.model = "gemini-flash-lite-latest";
        else if (need_api) return fail("для provider = openai укажите model");
    }
    // Рассуждения Gemini — это секунды ожидания. Flash-Lite по умолчанию думает
    // минимально; старшим моделям minimal не всегда доступен, им — low.
    if (gemini && c.thinking.empty()) c.thinking = c.model.find("lite") != std::string::npos ? "minimal" : "low";

    // Ключ: свой для провайдера, иначе общий api_key, иначе переменные окружения.
    const std::string& own = anthropic ? c.anthropic_api_key : gemini ? c.gemini_api_key : c.openai_api_key;
    if (!own.empty()) c.api_key = own;
    c.api_key_source = c.api_key.empty() ? "" : "config";
    const char* main_env = anthropic ? "ANTHROPIC_API_KEY" : gemini ? "GEMINI_API_KEY" : "OPENAI_API_KEY";
    if (c.api_key.empty()) {
        for (const char* name : {"QUIZHINT_API_KEY", main_env, gemini ? "GOOGLE_API_KEY" : ""}) {
            const char* env = *name ? std::getenv(name) : nullptr;
            if (env && *env) {
                c.api_key = env;
                c.api_key_source = name;
                break;
            }
        }
    }
    if (need_api && (anthropic || gemini) && c.api_key.empty())
        return fail("нет ключа API: впишите api_key в " + default_config_path() + " или задайте " + main_env);
    return true;
}

bool update_config_file(const std::string& path, const ConfigValues& values, std::string& error) {
    Config scratch;
    for (const auto& [key, value] : values) {
        if (value.find('\r') != std::string::npos) {
            error = key + ": недопустимый символ в значении";
            return false;
        }
        if (!apply(scratch, key, value, error)) return false;
    }

    // Если конфиг — символическая ссылка (dotfiles), правим файл, на который она указывает.
    std::string target = path;
    char resolved[PATH_MAX];
    if (realpath(path.c_str(), resolved)) target = resolved;

    std::string text;
    if (!read_text(target, text)) text = kConfigExample;
    std::vector<std::string> lines;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) lines.push_back(line);

    for (const auto& [key, value] : values) {
        // Действует последнее присваивание, поэтому меняем именно его. Если ключ
        // задан только закомментированным примером, раскомментируем пример.
        int active = -1, commented = -1;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (line_key(lines[i], false) == key) active = static_cast<int>(i);
            else if (commented < 0 && line_key(lines[i], true) == key) commented = static_cast<int>(i);
        }
        const std::string line = key + " = " + format_value(value);
        if (active >= 0) lines[static_cast<size_t>(active)] = line;
        else if (commented >= 0) lines[static_cast<size_t>(commented)] = line;
        else lines.push_back(line);
    }

    std::string out;
    for (const auto& line : lines) out += line + "\n";
    Config check;
    if (!parse_config(out, target, check, error) || !finalize_config(check, false, error)) return false;
    return write_atomic(target, out, error);
}

std::string config_json(const Config& c, const std::string& path, bool exists, const std::string& error) {
    std::string j = "{";
    auto raw = [&](const char* key, const std::string& value) {
        if (j.size() > 1) j += ',';
        j += json::quote(key);
        j += ':';
        j += value;
    };
    auto str = [&](const char* key, const std::string& value) { raw(key, json::quote(value)); };
    auto num = [&](const char* key, double value) { raw(key, format_number(value)); };
    auto flag = [&](const char* key, bool value) { raw(key, value ? "true" : "false"); };

    flag("ok", error.empty());
    if (!error.empty()) str("error", error);
    str("path", path);
    flag("exists", exists);
    str("provider", c.provider);
    str("model", c.model);
    str("base_url", c.base_url);
    flag("api_key_set", !c.api_key.empty());
    str("api_key_source", c.api_key_source);
    // Последние символы ключа: видно, какой ключ сохранён, и плагин замечает его замену.
    str("api_key_hint", c.api_key.size() >= 12 ? c.api_key.substr(c.api_key.size() - 4) : "");
    if (c.max_tokens) num("max_tokens", *c.max_tokens);
    if (c.temperature) num("temperature", *c.temperature);
    str("thinking", c.thinking);
    flag("custom_prompt", !c.prompt.empty());
    flag("extra_set", !c.extra.empty());
    num("timeout", c.timeout);
    str("capture", c.capture);
    str("capture_protocol", c.capture_protocol);
    num("max_size", c.max_size);
    num("jpeg_quality", c.jpeg_quality);
    str("font", c.font);
    num("font_size", c.font_size);
    str("color", format_color(c.color));
    str("background", format_color(c.background));
    num("margin_top", c.margin_top);
    num("margin_left", c.margin_left);
    flag("over_panels", c.over_panels);
    num("hide_after", c.hide_after);
    flag("busy_indicator", c.busy_indicator);
    str("hotkey", c.hotkey);
    flag("autostart", c.autostart);
    return j + "}";
}
