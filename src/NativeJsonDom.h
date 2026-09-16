#pragma once

#include <cctype>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "NativeJson.h"

namespace NativeJsonDom {

struct Array;
struct Object;

struct Value {
    using Storage = std::variant<std::nullptr_t, bool, double, std::string,
                                 std::shared_ptr<Array>, std::shared_ptr<Object>>;

    Storage storage = nullptr;

    Value() = default;
    Value(std::nullptr_t) : storage(nullptr) {}
    Value(bool value) : storage(value) {}
    Value(double value) : storage(value) {}
    Value(const char *value) : storage(std::string(value ? value : "")) {}
    Value(std::string value) : storage(std::move(value)) {}
    Value(Array value);
    Value(Object value);

    bool isNull() const { return std::holds_alternative<std::nullptr_t>(storage); }
    bool isBool() const { return std::holds_alternative<bool>(storage); }
    bool isNumber() const { return std::holds_alternative<double>(storage); }
    bool isString() const { return std::holds_alternative<std::string>(storage); }
    bool isArray() const { return std::holds_alternative<std::shared_ptr<Array>>(storage); }
    bool isObject() const { return std::holds_alternative<std::shared_ptr<Object>>(storage); }
    const std::string &string() const { return std::get<std::string>(storage); }
    double number() const { return std::get<double>(storage); }
    bool boolean() const { return std::get<bool>(storage); }
    const Array &array() const { return *std::get<std::shared_ptr<Array>>(storage); }
    const Object &object() const { return *std::get<std::shared_ptr<Object>>(storage); }
};

struct Array { std::vector<Value> values; };
struct Object { std::map<std::string, Value> values; };

inline Value::Value(Array value) : storage(std::make_shared<Array>(std::move(value))) {}
inline Value::Value(Object value) : storage(std::make_shared<Object>(std::move(value))) {}

class Parser final {
public:
    bool parse(std::string_view input, Value &result, std::string *error = nullptr)
    {
        input_ = input;
        position_ = 0;
        error_ = error;
        skipWhitespace();
        if (!parseValue(result)) return fail("invalid JSON value");
        skipWhitespace();
        return position_ == input_.size() || fail("trailing JSON data");
    }

private:
    bool parseValue(Value &result)
    {
        skipWhitespace();
        if (position_ >= input_.size()) return false;
        switch (input_[position_]) {
        case '{': return parseObject(result);
        case '[': return parseArray(result);
        case '"': { std::string value; if (!parseString(value)) return false; result = Value(std::move(value)); return true; }
        case 't': return parseLiteral("true", Value(true), result);
        case 'f': return parseLiteral("false", Value(false), result);
        case 'n': return parseLiteral("null", Value(nullptr), result);
        default: return parseNumber(result);
        }
    }

    bool parseObject(Value &result)
    {
        ++position_;
        Object object;
        skipWhitespace();
        if (consume('}')) { result = Value(std::move(object)); return true; }
        while (position_ < input_.size()) {
            std::string key;
            if (!parseString(key)) return false;
            skipWhitespace();
            if (!consume(':')) return false;
            Value value;
            if (!parseValue(value)) return false;
            object.values.emplace(std::move(key), std::move(value));
            skipWhitespace();
            if (consume('}')) { result = Value(std::move(object)); return true; }
            if (!consume(',')) return false;
            skipWhitespace();
        }
        return false;
    }

    bool parseArray(Value &result)
    {
        ++position_;
        Array array;
        skipWhitespace();
        if (consume(']')) { result = Value(std::move(array)); return true; }
        while (position_ < input_.size()) {
            Value value;
            if (!parseValue(value)) return false;
            array.values.push_back(std::move(value));
            skipWhitespace();
            if (consume(']')) { result = Value(std::move(array)); return true; }
            if (!consume(',')) return false;
            skipWhitespace();
        }
        return false;
    }

    bool parseString(std::string &result)
    {
        if (!consume('"')) return false;
        while (position_ < input_.size()) {
            const unsigned char value = static_cast<unsigned char>(input_[position_++]);
            if (value == '"') return true;
            if (value < 0x20) return false;
            if (value != '\\') { result.push_back(static_cast<char>(value)); continue; }
            if (position_ >= input_.size()) return false;
            const char escape = input_[position_++];
            switch (escape) {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u': {
                unsigned int codepoint = 0;
                if (!parseUnicodeEscape(codepoint)) return false;
                if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                    if (position_ + 2 > input_.size()
                        || input_[position_] != '\\' || input_[position_ + 1] != 'u') {
                        return false;
                    }
                    position_ += 2;
                    unsigned int lowSurrogate = 0;
                    if (!parseUnicodeEscape(lowSurrogate)
                        || lowSurrogate < 0xdc00 || lowSurrogate > 0xdfff) {
                        return false;
                    }
                    codepoint = 0x10000
                        + ((codepoint - 0xd800) << 10)
                        + (lowSurrogate - 0xdc00);
                } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
                    return false;
                }
                appendUtf8(result, codepoint);
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool parseNumber(Value &result)
    {
        const std::size_t start = position_;
        if (position_ < input_.size() && input_[position_] == '-') ++position_;
        if (position_ >= input_.size()) return false;
        if (input_[position_] == '0') ++position_;
        else if (!digits()) return false;
        if (position_ < input_.size() && input_[position_] == '.') {
            ++position_;
            if (!digits()) return false;
        }
        if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) ++position_;
            if (!digits()) return false;
        }
        const std::string token(input_.substr(start, position_ - start));
        char *end = nullptr;
        const double number = std::strtod(token.c_str(), &end);
        if (!end || *end != '\0') return false;
        result = Value(number);
        return true;
    }

    bool digits()
    {
        const std::size_t start = position_;
        while (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_]))) ++position_;
        return position_ != start;
    }

    bool parseLiteral(std::string_view literal, Value value, Value &result)
    {
        if (input_.substr(position_, literal.size()) != literal) return false;
        position_ += literal.size();
        result = std::move(value);
        return true;
    }

    bool consume(char value)
    {
        if (position_ >= input_.size() || input_[position_] != value) return false;
        ++position_;
        return true;
    }

    void skipWhitespace()
    {
        while (position_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[position_]))) ++position_;
    }

    bool fail(const char *message)
    {
        if (error_) *error_ = std::string(message) + " at offset " + std::to_string(position_);
        return false;
    }

    static int hex(char value)
    {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    }

    bool parseUnicodeEscape(unsigned int &result)
    {
        result = 0;
        for (int index = 0; index < 4; ++index) {
            if (position_ >= input_.size()) return false;
            const int digit = hex(input_[position_++]);
            if (digit < 0) return false;
            result = (result << 4) | static_cast<unsigned int>(digit);
        }
        return true;
    }

    static void appendUtf8(std::string &output, unsigned int value)
    {
        if (value <= 0x7f) output.push_back(static_cast<char>(value));
        else if (value <= 0x7ff) {
            output.push_back(static_cast<char>(0xc0 | (value >> 6)));
            output.push_back(static_cast<char>(0x80 | (value & 0x3f)));
        } else if (value <= 0xffff) {
            output.push_back(static_cast<char>(0xe0 | (value >> 12)));
            output.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (value & 0x3f)));
        } else {
            output.push_back(static_cast<char>(0xf0 | (value >> 18)));
            output.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (value & 0x3f)));
        }
    }

    std::string_view input_;
    std::size_t position_ = 0;
    std::string *error_ = nullptr;
};

inline bool parse(std::string_view input, Value &result, std::string *error = nullptr)
{
    return Parser().parse(input, result, error);
}

inline const Value *find(const Object &object, std::string_view key)
{
    const auto found = object.values.find(std::string(key));
    return found == object.values.end() ? nullptr : &found->second;
}

inline std::string stringValue(
    const Object &object, std::string_view key,
    std::string fallback = {})
{
    const Value *value = find(object, key);
    return value && value->isString() ? value->string() : fallback;
}

inline const Object *objectValue(
    const Object &object, std::string_view key)
{
    const Value *value = find(object, key);
    return value && value->isObject() ? &value->object() : nullptr;
}

inline const Array *arrayValue(
    const Object &object, std::string_view key)
{
    const Value *value = find(object, key);
    return value && value->isArray() ? &value->array() : nullptr;
}

inline bool contains(const Object &object, std::string_view key)
{
    return find(object, key) != nullptr;
}

inline double numberValue(
    const Object &object, std::string_view key, double fallback = 0)
{
    const Value *value = find(object, key);
    return value && value->isNumber() ? value->number() : fallback;
}

inline int integerValue(
    const Object &object, std::string_view key, int fallback = 0)
{
    const Value *value = find(object, key);
    if (!value || !value->isNumber())
        return fallback;
    const double number = value->number();
    if (number < std::numeric_limits<int>::min()
        || number > std::numeric_limits<int>::max())
        return fallback;
    return static_cast<int>(number);
}

inline int convertedIntegerValue(
    const Object &object, std::string_view key, int fallback = 0)
{
    const Value *value = find(object, key);
    if (!value)
        return fallback;
    if (value->isNumber())
        return integerValue(object, key, fallback);
    if (value->isString()) {
        char *end = nullptr;
        const long result = std::strtol(value->string().c_str(), &end, 10);
        if (end != value->string().c_str() && end && *end == '\0'
            && result >= std::numeric_limits<int>::min()
            && result <= std::numeric_limits<int>::max())
            return static_cast<int>(result);
    }
    return fallback;
}

inline bool booleanValue(
    const Object &object, std::string_view key, bool fallback = false)
{
    const Value *value = find(object, key);
    return value && value->isBool() ? value->boolean() : fallback;
}

inline std::string stringify(const Value &value)
{
    if (value.isNull()) return "null";
    if (value.isBool()) return value.boolean() ? "true" : "false";
    if (value.isNumber()) return std::to_string(value.number());
    if (value.isString()) return NativeJson::quote(value.string());
    if (value.isArray()) {
        std::string output = "[";
        bool first = true;
        for (const Value &item : value.array().values) {
            if (!first) output.push_back(',');
            first = false;
            output += stringify(item);
        }
        output.push_back(']');
        return output;
    }
    std::string output = "{";
    bool first = true;
    for (const auto &entry : value.object().values) {
        if (!first) output.push_back(',');
        first = false;
        output += NativeJson::quote(entry.first);
        output.push_back(':');
        output += stringify(entry.second);
    }
    output.push_back('}');
    return output;
}

} // namespace NativeJsonDom
