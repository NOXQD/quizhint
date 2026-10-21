#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

struct Rgba {
    double r = 1, g = 1, b = 1, a = 1;
};

struct Config {
    // Модель
    std::string provider = "anthropic";  // anthropic | gemini | openai (любой OpenAI-совместимый API)
    std::string model;                   // пусто — значение по умолчанию для провайдера
    std::string api_key;                 // общий ключ; после finalize_config — ключ текущего провайдера
    std::string anthropic_api_key;       // ключи для каждого провайдера: при переключении
    std::string gemini_api_key;          // не нужно вводить ключ заново
    std::string openai_api_key;
    std::string base_url;                // пусто — значение по умолчанию для провайдера
    std::optional<int> max_tokens;
    std::optional<double> temperature;
    std::string thinking;                // уровень рассуждений: minimal | low | medium | high, пусто — по умолчанию
    std::string extra;                   // JSON-объект, его поля добавляются в тело запроса
    std::string prompt;                  // системный промпт режима «цифра»; пусто — встроенный
    std::string answer_mode = "number";  // number — номер варианта, full — короткий полный ответ
    std::string full_length = "short";   // полный ответ: short | medium | long
    bool full_explain = true;            // добавлять короткое пояснение к ответу
    std::string answer_language = "auto";  // auto — язык вопроса, ru, en
    std::string instructions;            // свои указания модели (добавляются к промпту)
    int timeout = 30;                    // секунды на весь запрос

    // Снимок экрана
    std::string capture = "monitor";        // monitor | window
    std::string capture_protocol = "auto";  // auto | ext | wlr
    int max_size = 1568;                    // длинная сторона картинки, 0 — не уменьшать
    int jpeg_quality = 80;

    // Цифра на экране
    std::string font = "sans-serif";
    double font_size = 15;
    Rgba color{1, 1, 1, 1};
    Rgba background{0, 0, 0, 0.8};
    std::string position = "top-left";  // угол экрана: top-left | top-right | bottom-left | bottom-right
    int margin_x = 6;                   // отступ от угла по горизонтали
    int margin_y = 6;                   // и по вертикали
    bool over_panels = false;  // true — в самом углу поверх панели, false — под ней
    double hide_after = 5;     // секунды, 0 — не прятать до следующего запроса
    bool busy_indicator = true;

    // Полный ответ
    double full_font_size = 13;
    int full_width = 440;          // ширина плашки с текстом
    int full_max_lines = 10;       // дальше — многоточие
    double hide_after_full = 20;   // секунды, 0 — не прятать до следующего запроса

    // Для плагина Omarchy; сам quizhint их не использует.
    std::string hotkey = "F1";
    std::string region_hotkey = "F3";  // выделить область экрана
    bool autostart = true;

    // Откуда взят ключ: "config", имя переменной окружения или "" (ключа нет).
    std::string api_key_source;
};

using ConfigValues = std::vector<std::pair<std::string, std::string>>;

// $XDG_CONFIG_HOME/quizhint/config или ~/.config/quizhint/config.
std::string default_config_path();

// Разбирает текст конфига; path нужен только для сообщений об ошибках.
bool parse_config(const std::string& text, const std::string& path, Config& cfg, std::string& error);

// Читает файл поверх значений по умолчанию. Отсутствие файла по пути
// по умолчанию — не ошибка; явно указанный файл обязан существовать.
bool load_config(const std::string& path, bool explicit_path, Config& cfg, std::string& error);

// Подставляет model/base_url/api_key по умолчанию и проверяет значения.
// need_api = false, когда модель не нужна (--show, правка и вывод конфига).
bool finalize_config(Config& cfg, bool need_api, std::string& error);

// Меняет значения в файле, сохраняя остальные строки и комментарии. Если файла
// нет, он создаётся из config.example. Пишется атомарно с правами 0600.
bool update_config_file(const std::string& path, const ConfigValues& values, std::string& error);

// Настройки в JSON для плагина Omarchy. Сам ключ API не выводится.
std::string config_json(const Config& cfg, const std::string& path, bool exists, const std::string& error);
