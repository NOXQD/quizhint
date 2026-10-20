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

Request build_request(const Config& cfg, const std::string& image_base64, const std::string& media_type);

// Достаёт текст ответа модели. При ошибке API — false и описание в error.
bool parse_response(const Config& cfg, long http_status, const std::string& body, std::string& text,
                    std::string& error);

// Что показать на экране: "2", "1,3" или "?" (вопрос не найден / ответ не разобран).
std::string extract_answer(std::string_view text);

// Однострочный обрезанный фрагмент для логов (не рвёт UTF-8).
std::string snippet(std::string_view text, size_t limit = 120);

}  // namespace llm
