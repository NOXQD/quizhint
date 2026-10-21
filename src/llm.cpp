#include "llm.hpp"

#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <optional>

namespace llm {

namespace {

constexpr const char* kScreenIntro = "You are given a screenshot of the user's screen.";
constexpr const char* kRegionIntro =
    "You are given a fragment of the screen that the user selected; it contains what needs to be answered.";

constexpr const char* kNumbering =
    "Number the options 1, 2, 3, ... in the order they appear: from top to bottom, and if the options form a "
    "grid, from left to right within each row before moving to the next row. If the options are labelled with "
    "letters (A, B, C, ... or А, Б, В, ...), the first letter is 1, the second is 2, and so on.";

// Настройки полного ответа (длина, пояснение, язык, свои указания) — в текст промпта.
size_t length_limit(const Config& cfg) {
    return cfg.full_length == "long" ? 900 : cfg.full_length == "medium" ? 500 : 250;
}

std::string language_rule(const Config& cfg) {
    if (cfg.answer_language == "ru") return "Always reply in Russian.";
    if (cfg.answer_language == "en") return "Always reply in English.";
    return "Reply in the language of the question.";
}

std::string style_rule(const Config& cfg) {
    const std::string limit = std::to_string(length_limit(cfg));
    if (!cfg.full_explain)
        return "Give only the answer itself, without explanation or reasoning. Keep it under " + limit + " characters.";
    if (cfg.full_length == "short")
        return "Be brief but complete: state the final answer first, then add at most one short sentence of "
               "explanation if it really helps. Keep the whole reply under " + limit + " characters. For a "
               "calculation give the result, not the steps.";
    return "State the final answer first, then explain it briefly: the key reasoning or steps in a few short "
           "sentences. Keep the whole reply under " + limit + " characters.";
}

std::string user_rules(const Config& cfg) {
    if (cfg.instructions.empty()) return {};
    return "\nAdditional instructions from the user (follow them unless they conflict with the rules above): " +
           cfg.instructions;
}

std::string system_prompt(const Config& cfg, Kind kind, bool region) {
    const std::string intro = region ? kRegionIntro : kScreenIntro;
    switch (kind) {
    case Kind::Number:
        if (!cfg.prompt.empty()) return cfg.prompt;
        return intro +
               " It contains a quiz question (in any language) with several answer options. Ignore everything "
               "else: browser UI, other windows, timers, scores.\n"
               "Decide which option is correct and reply with its number. " + kNumbering + "\n"
               "Reply with the number only, without any other words, punctuation or explanation. If several "
               "options are correct (a \"select all that apply\" question), reply with all of their numbers "
               "separated by commas, for example: 1,3\n"
               "If there is no question with answer options in the image, reply 0.";
    case Kind::Full:
        return intro +
               " Find the question, task or problem in it - a quiz or test question, an exercise, a math or logic "
               "problem, a question about code or anything else - and answer it. Ignore everything unrelated: "
               "browser UI, other windows, timers, scores.\n" +
               language_rule(cfg) + " " + style_rule(cfg) + "\n"
               "If the question has answer options, start with the number of the correct option, then \" — \" and "
               "the option text, for example: 2 — Paris. " + kNumbering +
               " If several options are correct, give all of their numbers.\n"
               "Plain text only: no markdown, no LaTeX, no lists.\n"
               "If there is no question or task in the image, reply 0." + user_rules(cfg);
    case Kind::Auto:
        return intro +
               " Find the question, task or problem in it and answer it. Ignore everything unrelated.\n"
               "If the question has answer options, reply with the number of the correct option only, without any "
               "other words. " + kNumbering + " If several options are correct, reply with all of their numbers "
               "separated by commas, for example: 1,3\n"
               "If there are no answer options, reply with a complete answer as plain text without markdown. " +
               language_rule(cfg) + " " + style_rule(cfg) + "\n"
               "If there is no question or task in the image, reply 0." + user_rules(cfg);
    }
    return {};
}

constexpr const char* kNumberUserPrompt = "Which option is correct?";
constexpr const char* kFullUserPrompt = "Answer the question in the image.";

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

Request build_request(const Config& cfg, const std::string& image_base64, const std::string& media_type, Kind kind,
                      bool region) {
    const std::string system = system_prompt(cfg, kind, region);
    const std::string user = kind == Kind::Number ? kNumberUserPrompt : kFullUserPrompt;
    // Для номера хватает пары токенов; текстовому ответу нужно место.
    std::optional<int> max_tokens = cfg.max_tokens;
    if (kind != Kind::Number && max_tokens) max_tokens = std::max(*max_tokens, 512);
    const int full_tokens = cfg.full_length == "long" ? 1000 : cfg.full_length == "medium" ? 700 : 400;
    const int anthropic_max = kind == Kind::Number ? cfg.max_tokens.value_or(16) : std::max(cfg.max_tokens.value_or(0), full_tokens);
    Request r;
    std::string& b = r.body;
    b.reserve(image_base64.size() + 4096);

    if (cfg.provider == "anthropic") {
        r.url = cfg.base_url + "/v1/messages";
        r.headers = {"x-api-key: " + cfg.api_key, "anthropic-version: 2023-06-01", "content-type: application/json"};
        b += "{\"model\":" + json::quote(cfg.model);
        b += ",\"max_tokens\":" + std::to_string(anthropic_max);
        if (cfg.temperature) b += ",\"temperature\":" + format_number(*cfg.temperature);
        b += ",\"system\":" + json::quote(system);
        b += ",\"messages\":[{\"role\":\"user\",\"content\":[";
        b += "{\"type\":\"image\",\"source\":{\"type\":\"base64\",\"media_type\":" + json::quote(media_type);
        b += ",\"data\":\"";
        b += image_base64;
        b += "\"}},{\"type\":\"text\",\"text\":" + json::quote(user) + "}]}]";
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
        b += "\"}},{\"text\":" + json::quote(user) + "}]}]";
        // Рассуждения тоже расходуют maxOutputTokens, поэтому лимит — только если задан явно.
        std::string gen;
        auto add = [&gen](const std::string& field) { gen += (gen.empty() ? "" : ",") + field; };
        if (max_tokens) add("\"maxOutputTokens\":" + std::to_string(*max_tokens));
        if (cfg.temperature) add("\"temperature\":" + format_number(*cfg.temperature));
        if (!cfg.thinking.empty()) add("\"thinkingConfig\":{\"thinkingLevel\":" + json::quote(cfg.thinking) + "}");
        if (!gen.empty()) b += ",\"generationConfig\":{" + gen + "}";
    } else {
        r.url = cfg.base_url + "/chat/completions";
        if (!cfg.api_key.empty()) r.headers.push_back("Authorization: Bearer " + cfg.api_key);
        r.headers.push_back("Content-Type: application/json");
        b += "{\"model\":" + json::quote(cfg.model);
        // У reasoning-моделей лимит съедают рассуждения, поэтому без явной настройки его не шлём.
        if (max_tokens) b += ",\"max_tokens\":" + std::to_string(*max_tokens);
        if (cfg.temperature) b += ",\"temperature\":" + format_number(*cfg.temperature);
        if (!cfg.thinking.empty()) b += ",\"reasoning_effort\":" + json::quote(cfg.thinking);
        b += ",\"messages\":[{\"role\":\"system\",\"content\":" + json::quote(system) + "},";
        b += "{\"role\":\"user\",\"content\":[{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:";
        b += media_type + ";base64,";
        b += image_base64;
        b += "\"}},{\"type\":\"text\",\"text\":" + json::quote(user) + "}]}]";
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

namespace {

// Ответ из одного номера (или списка номеров): «2», «1, 3», «Ответ: Б».
bool is_number_reply(std::string_view text) {
    if (text.find('\n') != std::string_view::npos) return false;
    std::string_view t = trim_view(text, " \t.*)(");
    if (const size_t colon = t.find(':'); colon != std::string_view::npos && colon <= 14) t = trim_view(t.substr(colon + 1), " \t.*)(");
    if (t.empty()) return false;
    if (t.find_first_not_of("0123456789, ") == std::string_view::npos) {
        // Номер варианта — небольшое число: «1945» или «120» — это уже ответ, а не вариант.
        bool any = false;
        for (size_t i = 0; i < t.size();) {
            if (!std::isdigit(static_cast<unsigned char>(t[i]))) {
                ++i;
                continue;
            }
            int value = 0;
            for (; i < t.size() && std::isdigit(static_cast<unsigned char>(t[i])); ++i) value = std::min(value * 10 + (t[i] - '0'), 1000);
            if (value < 1 || value > 20) return false;
            any = true;
        }
        return any;
    }
    if (t.size() == 1) return std::isalpha(static_cast<unsigned char>(t[0]));
    if (t.size() == 2 && static_cast<unsigned char>(t[0]) == 0xD0) return true;  // одна кириллическая буква А…Е
    return false;
}

}  // namespace

std::string clean_text(std::string_view text, size_t limit) {
    std::string out;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        std::string_view line = text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        pos = nl == std::string_view::npos ? text.size() + 1 : nl + 1;

        std::string clean;
        for (size_t i = 0; i < line.size(); ++i) {
            const char c = line[i];
            if (c == '*' || c == '`' || c == '\r') continue;
            if (c == '_' && i + 1 < line.size() && line[i + 1] == '_') { ++i; continue; }
            if (c == '\t') { clean += ' '; continue; }
            if (c == ' ' && !clean.empty() && clean.back() == ' ') continue;
            clean += c;
        }
        std::string_view t = trim_view(clean, " ");
        while (!t.empty() && t.front() == '#') t = trim_view(t.substr(1), " ");  // заголовки markdown
        if (t.empty()) continue;  // пустые строки только занимают место
        if (!out.empty()) out += '\n';
        out += t;
    }
    if (out.size() > limit) {
        size_t n = limit;
        while (n > 0 && (static_cast<unsigned char>(out[n]) & 0xC0) == 0x80) --n;
        out.resize(n);
        out += "…";
    }
    return out;
}

Answer make_answer(std::string_view reply, Kind kind, size_t limit) {
    if (kind == Kind::Number) return {extract_answer(reply), false};
    std::string text = clean_text(reply, limit);
    const std::string_view t = trim_view(text, " .");
    if (t.empty() || t == "0") return {"?", false};  // вопроса на картинке нет
    if (kind == Kind::Auto && is_number_reply(text)) return {extract_answer(text), false};
    return {std::move(text), true};
}

size_t answer_limit(const Config& cfg) {
    // Кириллица — два байта на букву, плюс запас: модель не всегда укладывается в лимит.
    return length_limit(cfg) * 3 + 100;
}

}  // namespace llm
