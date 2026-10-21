#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "config.hpp"

namespace llm {

struct Request {
    std::string url;
    std::vector<std::string> headers;
    std::string body;
};

// Какой ответ нужен от модели.
enum class Kind {
    Number,  // только номер варианта
    Full,    // короткий, но полный ответ текстом
    Auto,    // номер, если есть варианты, иначе короткий ответ (область в режиме «цифра»)
};

// region — на картинке выделенная пользователем часть экрана, а не весь экран.
Request build_request(const Config& cfg, const std::string& image_base64, const std::string& media_type,
                      Kind kind = Kind::Number, bool region = false);

// Ответ, готовый к показу.
struct Answer {
    std::string text;   // "2", "1,3", "?" или текст полного ответа
    bool full = false;  // true — показывать блоком текста
};
// limit — сколько байт полного ответа оставить (длиннее — обрезается с многоточием).
Answer make_answer(std::string_view reply, Kind kind, size_t limit = 700);

// Сколько байт полного ответа показывать при этих настройках.
size_t answer_limit(const Config& cfg);

// Текст полного ответа без markdown и пустых строк, не длиннее limit байт (UTF-8 не рвётся).
std::string clean_text(std::string_view text, size_t limit = 700);

// Достаёт текст ответа модели. При ошибке API — false и описание в error.
bool parse_response(const Config& cfg, long http_status, const std::string& body, std::string& text,
                    std::string& error);

// Что показать на экране: "2", "1,3" или "?" (вопрос не найден / ответ не разобран).
std::string extract_answer(std::string_view text);

// Однострочный обрезанный фрагмент для логов (не рвёт UTF-8).
std::string snippet(std::string_view text, size_t limit = 120);

}  // namespace llm
