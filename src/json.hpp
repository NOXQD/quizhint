#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Минимальный JSON: разбор ответов API и Hyprland, экранирование строк для запросов.
namespace json {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    // Отсутствующий ключ или индекс дают null, поэтому цепочки безопасны:
    // v["choices"].at(0)["message"]["content"].str()
    const Value& operator[](std::string_view key) const;
    const Value& at(size_t index) const;
    size_t size() const;

    const std::string& str() const;  // "" если не строка
    double num(double fallback = 0) const;
    bool boolean(bool fallback = false) const;
    const std::vector<Value>& items() const { return array_; }

private:
    friend class Parser;

    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0;
    std::string string_;
    std::vector<Value> array_;
    std::vector<std::pair<std::string, Value>> object_;
};

std::optional<Value> parse(std::string_view text);

// Строка в кавычках с экранированием по RFC 8259.
std::string quote(std::string_view s);

}  // namespace json
