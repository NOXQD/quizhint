#include "json.hpp"

#include <charconv>
#include <cstdint>
#include <cstdio>

namespace json {

namespace {

const Value& null_value() {
    static const Value value;
    return value;
}

void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

}  // namespace

const Value& Value::operator[](std::string_view key) const {
    if (type_ == Type::Object) {
        for (const auto& [k, v] : object_) {
            if (k == key) return v;
        }
    }
    return null_value();
}

const Value& Value::at(size_t index) const {
    if (type_ == Type::Array && index < array_.size()) return array_[index];
    return null_value();
}

size_t Value::size() const {
    if (type_ == Type::Array) return array_.size();
    if (type_ == Type::Object) return object_.size();
    return 0;
}

const std::string& Value::str() const {
    static const std::string empty;
    return type_ == Type::String ? string_ : empty;
}

double Value::num(double fallback) const {
    return type_ == Type::Number ? number_ : fallback;
}

bool Value::boolean(bool fallback) const {
    return type_ == Type::Bool ? bool_ : fallback;
}

class Parser {
public:
    explicit Parser(std::string_view text) : s_(text) {}

    std::optional<Value> run() {
        Value v;
        skip_ws();
        if (!parse_value(v, 0)) return std::nullopt;
        skip_ws();
        if (pos_ != s_.size()) return std::nullopt;
        return v;
    }

private:
    static constexpr int kMaxDepth = 128;

    bool eof() const { return pos_ >= s_.size(); }
    char peek() const { return s_[pos_]; }

    void skip_ws() {
        while (!eof() && (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r')) ++pos_;
    }

    bool eat_char(char c) {
        if (eof() || peek() != c) return false;
        ++pos_;
        return true;
    }

    bool eat_word(std::string_view word) {
        if (s_.substr(pos_, word.size()) != word) return false;
        pos_ += word.size();
        return true;
    }

    bool parse_value(Value& v, int depth) {
        if (eof() || depth > kMaxDepth) return false;
        switch (peek()) {
        case '{':
            return parse_object(v, depth);
        case '[':
            return parse_array(v, depth);
        case '"':
            v.type_ = Value::Type::String;
            return parse_string(v.string_);
        case 't':
            v.type_ = Value::Type::Bool;
            v.bool_ = true;
            return eat_word("true");
        case 'f':
            v.type_ = Value::Type::Bool;
            v.bool_ = false;
            return eat_word("false");
        case 'n':
            v.type_ = Value::Type::Null;
            return eat_word("null");
        default:
            return parse_number(v);
        }
    }

    bool parse_object(Value& v, int depth) {
        v.type_ = Value::Type::Object;
        ++pos_;  // {
        skip_ws();
        if (eat_char('}')) return true;
        for (;;) {
            skip_ws();
            std::string key;
            if (eof() || peek() != '"' || !parse_string(key)) return false;
            skip_ws();
            if (!eat_char(':')) return false;
            skip_ws();
            Value item;
            if (!parse_value(item, depth + 1)) return false;
            v.object_.emplace_back(std::move(key), std::move(item));
            skip_ws();
            if (eat_char(',')) continue;
            return eat_char('}');
        }
    }

    bool parse_array(Value& v, int depth) {
        v.type_ = Value::Type::Array;
        ++pos_;  // [
        skip_ws();
        if (eat_char(']')) return true;
        for (;;) {
            skip_ws();
            Value item;
            if (!parse_value(item, depth + 1)) return false;
            v.array_.push_back(std::move(item));
            skip_ws();
            if (eat_char(',')) continue;
            return eat_char(']');
        }
    }

    bool hex4(uint32_t& cp) {
        if (pos_ + 4 > s_.size()) return false;
        cp = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[pos_++];
            cp <<= 4;
            if (c >= '0' && c <= '9') cp |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') cp |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') cp |= static_cast<uint32_t>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool parse_string(std::string& out) {
        ++pos_;  // открывающая кавычка
        while (!eof()) {
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (eof()) return false;
            switch (s_[pos_++]) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t cp;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    // Суррогатная пара; одиночные половинки заменяем на U+FFFD.
                    const size_t save = pos_;
                    uint32_t low;
                    if (eat_word("\\u") && hex4(low) && low >= 0xDC00 && low <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    } else {
                        pos_ = save;
                        cp = 0xFFFD;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = 0xFFFD;
                }
                append_utf8(out, cp);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    bool parse_number(Value& v) {
        const size_t start = pos_;
        while (!eof()) {
            const char c = peek();
            if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') ++pos_;
            else break;
        }
        if (pos_ == start) return false;
        double d = 0;
        const char* first = s_.data() + start;
        const char* last = s_.data() + pos_;
        const auto [ptr, ec] = std::from_chars(first, last, d);
        if (ec != std::errc() || ptr != last) return false;
        v.type_ = Value::Type::Number;
        v.number_ = d;
        return true;
    }

    std::string_view s_;
    size_t pos_ = 0;
};

std::optional<Value> parse(std::string_view text) {
    return Parser(text).run();
}

std::string quote(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += ch;
            }
        }
    }
    out += '"';
    return out;
}

}  // namespace json
