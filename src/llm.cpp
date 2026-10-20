#include "llm.hpp"

#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace llm {

namespace {

constexpr const char* kSystemPrompt =
    "You are given a screenshot that contains a quiz question (in any language) with several answer options. "
    "Ignore everything else on the screen: browser UI, other windows, timers, scores.\n"
    "Decide which option is correct and reply with its number. Number the options 1, 2, 3, ... in the order "
    "they appear on the screen: from top to bottom, and if the options form a grid, from left to right within "
    "each row before moving to the next row. If the options are labelled with letters (A, B, C, ... or "
    "А, Б, В, ...), the first letter is 1, the second is 2, and so on.\n"
    "Reply with the number only, without any other words, punctuation or explanation. If several options are "
    "correct (a \"select all that apply\" question), reply with all of their numbers separated by commas, "
    "for example: 1,3\n"
    "If there is no question with answer options on the screenshot, reply 0.";

constexpr const char* kUserPrompt = "Which option is correct?";

std::string format_number(double d) {
    char buf[32];
    const auto res = std::to_chars(buf, buf + sizeof buf, d);
    return std::string(buf, res.ptr);
}

std::string_view trim_view(std::string_view s, std::string_view chars) {
    const size_t b = s.find_first_not_of(chars);
    if (b == std::string_view::npos) return {};
    const size_t e = s.find_last_not_of(chars);
    return s.substr(b, e - b + 1);
}

// Поля JSON-объекта extra без фигурных скобок (объект уже проверен в конфиге).
std::string extra_members(const std::string& extra) {
    const auto inner = trim_view(extra, " \t\r\n");
    if (inner.size() < 2) return {};
    return std::string(trim_view(inner.substr(1, inner.size() - 2), " \t\r\n"));
}

}  // namespace

Request build_request(const Config& cfg, const std::string& image_base64, const std::string& media_type) {
    const std::string& system = cfg.prompt.empty() ? std::string(kSystemPrompt) : cfg.prompt;
    Request r;
    std::string& b = r.body;
    b.reserve(image_base64.size() + 4096);

    if (cfg.provider == "anthropic") {
        r.url = cfg.base_url + "/v1/messages";
        r.headers = {"x-api-key: " + cfg.api_key, "anthropic-version: 2023-06-01", "content-type: application/json"};
        b += "{\"model\":" + json::quote(cfg.model);
        b += ",\"max_tokens\":" + std::to_string(cfg.max_tokens.value_or(16));
        if (cfg.temperature) b += ",\"temperature\":" + format_number(*cfg.temperature);
        b += ",\"system\":" + json::quote(system);
        b += ",\"messages\":[{\"role\":\"user\",\"content\":[";
        b += "{\"type\":\"image\",\"source\":{\"type\":\"base64\",\"media_type\":" + json::quote(media_type);
        b += ",\"data\":\"";
        b += image_base64;
        b += "\"}},{\"type\":\"text\",\"text\":" + json::quote(kUserPrompt) + "}]}]";
    } else if (cfg.provider == "gemini") {
        // Родной API Gemini (generateContent): модель в адресе, ключ в заголовке.
        std::string model = cfg.model;
        if (model.rfind("models/", 0) == 0) model.erase(0, 7);
        r.url = cfg.base_url + "/models/" + model + ":generateContent";
        r.headers = {"x-goog-api-key: " + cfg.api_key, "Content-Type: application/json"};
        b += "{\"systemInstruction\":{\"parts\":[{\"text\":" + json::quote(system) + "}]}";
        b += ",\"contents\":[{\"role\":\"user\",\"parts\":[";
        b += "{\"inlineData\":{\"mimeType\":" + json::quote(media_type) + ",\"data\":\"";
        b += image_base64;
        b += "\"}},{\"text\":" + json::quote(kUserPrompt) + "}]}]";
        // Рассуждения тоже расходуют maxOutputTokens, поэтому лимит — только если задан явно.
        std::string gen;
        auto add = [&gen](const std::string& field) { gen += (gen.empty() ? "" : ",") + field; };
        if (cfg.max_tokens) add("\"maxOutputTokens\":" + std::to_string(*cfg.max_tokens));
        if (cfg.temperature) add("\"temperature\":" + format_number(*cfg.temperature));
        if (!cfg.thinking.empty()) add("\"thinkingConfig\":{\"thinkingLevel\":" + json::quote(cfg.thinking) + "}");
        if (!gen.empty()) b += ",\"generationConfig\":{" + gen + "}";
    } else {
        r.url = cfg.base_url + "/chat/completions";
        if (!cfg.api_key.empty()) r.headers.push_back("Authorization: Bearer " + cfg.api_key);
        r.headers.push_back("Content-Type: application/json");
        b += "{\"model\":" + json::quote(cfg.model);
        // У reasoning-моделей лимит съедают рассуждения, поэтому без явной настройки его не шлём.
        if (cfg.max_tokens) b += ",\"max_tokens\":" + std::to_string(*cfg.max_tokens);
        if (cfg.temperature) b += ",\"temperature\":" + format_number(*cfg.temperature);
        if (!cfg.thinking.empty()) b += ",\"reasoning_effort\":" + json::quote(cfg.thinking);
        b += ",\"messages\":[{\"role\":\"system\",\"content\":" + json::quote(system) + "},";
        b += "{\"role\":\"user\",\"content\":[{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:";
        b += media_type + ";base64,";
        b += image_base64;
        b += "\"}},{\"type\":\"text\",\"text\":" + json::quote(kUserPrompt) + "}]}]";
    }

    if (const std::string members = extra_members(cfg.extra); !members.empty()) b += "," + members;
    b += "}";
    return r;
}

namespace {

// Ошибка API простыми словами: что случилось и что делать, а в скобках — исходный
// текст сервиса (первая строка, обрезанная).
std::string describe_http_error(long status, const std::string& message, const json::Value* error) {
    std::string head;
    if (status == 401 || status == 403 || message.find("API key") != std::string::npos) head = "ключ API не подходит";
    else if (status == 404) head = "модель не найдена — проверьте её имя";
    else if (status == 429) head = "лимит запросов исчерпан";
    else if (status == 500 || status == 502 || status == 503 || status == 504 || status == 529)
        head = "сервис перегружен, попробуйте ещё раз или выберите другую модель";

    // Google подсказывает, через сколько повторить: details[].retryDelay = "36.5s".
    if (status == 429 && error) {
        for (const auto& detail : (*error)["details"].items()) {
            const std::string& delay = detail["retryDelay"].str();
            if (delay.empty()) continue;
            double seconds = 0;
            std::from_chars(delay.data(), delay.data() + delay.size(), seconds);
            if (seconds > 0) head += ", повторить через " + std::to_string(static_cast<int>(seconds + 0.999)) + " с";
        }
    }

    const std::string detail =
        "HTTP " + std::to_string(status) + (message.empty() ? "" : ": " + snippet(message.substr(0, message.find('\n')), 160));
    return head.empty() ? detail : head + " (" + detail + ")";
}

}  // namespace

bool parse_response(const Config& cfg, long status, const std::string& body, std::string& text,
                    std::string& error) {
    const auto v = json::parse(body);
    if (!v) {
        error = describe_http_error(status, body.empty() ? "пустой ответ" : snippet(body, 300), nullptr);
        return false;
    }
    if (status < 200 || status >= 300 || !(*v)["error"].is_null()) {
        const auto& e = (*v)["error"];
        std::string message = e.is_string() ? e.str() : e["message"].str();
        if (message.empty()) message = snippet(body, 300);
        error = describe_http_error(status, message, e.is_object() ? &e : nullptr);
        return false;
    }

    text.clear();
    std::string reason;  // почему ответ мог оказаться пустым
    if (cfg.provider == "anthropic") {
        for (const auto& block : (*v)["content"].items()) {
            if (block["type"].str() == "text") text += block["text"].str();
        }
        reason = (*v)["stop_reason"].str();
    } else if (cfg.provider == "gemini") {
        const auto& candidate = (*v)["candidates"].at(0);
        for (const auto& part : candidate["content"]["parts"].items()) {
            if (!part["thought"].boolean()) text += part["text"].str();  // пересказ рассуждений пропускаем
        }
        reason = candidate["finishReason"].str();
        if (const auto& blocked = (*v)["promptFeedback"]["blockReason"].str(); !blocked.empty())
            reason = "запрос заблокирован: " + blocked;
    } else {
        const auto& content = (*v)["choices"].at(0)["message"]["content"];
        if (content.is_string()) {
            text = content.str();
        } else {
            for (const auto& part : content.items()) text += part["text"].str();
        }
        reason = (*v)["choices"].at(0)["finish_reason"].str();
    }
    if (trim_view(text, " \t\r\n").empty()) {
        error = "модель вернула пустой ответ" + (reason.empty() ? std::string() : " (" + reason + ")");
        return false;
    }
    return true;
}

std::string extract_answer(std::string_view text) {
    // Берём первую непустую строку: модель иногда дописывает пояснение ниже.
    std::string_view line;
    for (size_t pos = 0; pos <= text.size();) {
        const size_t nl = text.find('\n', pos);
        const auto candidate = text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        if (!trim_view(candidate, " \t\r").empty()) {
            line = candidate;
            break;
        }
        if (nl == std::string_view::npos) break;
        pos = nl + 1;
    }

    std::vector<int> numbers;
    bool zero = false;
    for (size_t i = 0; i < line.size();) {
        if (!std::isdigit(static_cast<unsigned char>(line[i]))) {
            ++i;
            continue;
        }
        long value = 0;
        for (; i < line.size() && std::isdigit(static_cast<unsigned char>(line[i])); ++i) {
            if (value < 1000) value = value * 10 + (line[i] - '0');
        }
        if (value == 0) zero = true;
        else if (value <= 20 && numbers.size() < 4 && std::find(numbers.begin(), numbers.end(), value) == numbers.end())
            numbers.push_back(static_cast<int>(value));
    }
    if (!numbers.empty()) {
        std::string out;
        for (const int n : numbers) {
            if (!out.empty()) out += ',';
            out += std::to_string(n);
        }
        return out;
    }
    if (zero) return "?";

    // Модель ответила буквой: «B», «Ответ: Б».
    constexpr std::string_view kPunct = " \t\r.,:;!?)(*\"'`[]";
    auto token = trim_view(line, kPunct);
    if (const size_t sp = token.find_last_of(" \t"); sp != std::string_view::npos) token = token.substr(sp + 1);
    token = trim_view(token, kPunct);
    if (token.size() == 1) {
        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(token[0])));
        if (c >= 'a' && c <= 'h') return std::to_string(c - 'a' + 1);
    }
    if (token.size() == 2 && static_cast<unsigned char>(token[0]) == 0xD0) {
        const auto c = static_cast<unsigned char>(token[1]);
        if (c >= 0x90 && c <= 0x95) return std::to_string(c - 0x90 + 1);  // А…Е
        if (c >= 0xB0 && c <= 0xB5) return std::to_string(c - 0xB0 + 1);  // а…е
    }
    return "?";
}

std::string snippet(std::string_view text, size_t limit) {
    text = trim_view(text, " \t\r\n");
    bool cut = false;
    if (text.size() > limit) {
        size_t n = limit;
        while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80) --n;
        text = text.substr(0, n);
        cut = true;
    }
    std::string out(text);
    for (char& c : out) {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    }
    if (cut) out += "…";
    return out;
}

}  // namespace llm
