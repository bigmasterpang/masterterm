#pragma once

#include <cstddef>
#include <string>
#include <string_view>

// Small dependency-free JSON syntax validator used at the WebView2 boundary.
// It deliberately does not build a second object model yet; the existing
// backend object model is migrated in a later step without changing the wire
// protocol first.
namespace NativeJson {

inline std::string quote(std::string_view value)
{
    std::string output;
    output.reserve(value.size() + 2);
    output.push_back('"');
    static constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (character < 0x20) {
                output += "\\u00";
                output.push_back(hex[character >> 4]);
                output.push_back(hex[character & 0x0f]);
            } else {
                output.push_back(static_cast<char>(character));
            }
        }
    }
    output.push_back('"');
    return output;
}

class Validator final {
public:
    bool validate(std::string_view input, std::string *error = nullptr)
    {
        input_ = input;
        position_ = 0;
        error_ = error;
        skipWhitespace();
        if (!parseValue() || !atEnd()) {
            fail("invalid JSON value");
            return false;
        }
        return true;
    }

private:
    void skipWhitespace()
    {
        while (position_ < input_.size()) {
            const char value = input_[position_];
            if (value != ' ' && value != '\t' && value != '\r' && value != '\n') break;
            ++position_;
        }
    }

    bool atEnd() const { return position_ == input_.size(); }

    bool parseValue()
    {
        skipWhitespace();
        if (atEnd()) return false;
        switch (input_[position_]) {
        case '{': return parseObject();
        case '[': return parseArray();
        case '"': return parseString();
        case 't': return parseLiteral("true");
        case 'f': return parseLiteral("false");
        case 'n': return parseLiteral("null");
        default: return parseNumber();
        }
    }

    bool parseObject()
    {
        ++position_;
        skipWhitespace();
        if (consume('}')) return true;
        while (!atEnd()) {
            if (!parseString()) return false;
            skipWhitespace();
            if (!consume(':')) return false;
            if (!parseValue()) return false;
            skipWhitespace();
            if (consume('}')) return true;
            if (!consume(',')) return false;
            skipWhitespace();
        }
        return false;
    }

    bool parseArray()
    {
        ++position_;
        skipWhitespace();
        if (consume(']')) return true;
        while (!atEnd()) {
            if (!parseValue()) return false;
            skipWhitespace();
            if (consume(']')) return true;
            if (!consume(',')) return false;
            skipWhitespace();
        }
        return false;
    }

    bool parseString()
    {
        if (!consume('"')) return false;
        while (!atEnd()) {
            const unsigned char value = static_cast<unsigned char>(input_[position_++]);
            if (value == '"') return true;
            if (value < 0x20) return false;
            if (value != '\\') continue;
            if (atEnd()) return false;
            const char escape = input_[position_++];
            if (escape == 'u') {
                for (int index = 0; index < 4; ++index) {
                    if (atEnd() || !isHex(input_[position_++])) return false;
                }
            } else if (escape != '"' && escape != '\\' && escape != '/'
                       && escape != 'b' && escape != 'f' && escape != 'n'
                       && escape != 'r' && escape != 't') {
                return false;
            }
        }
        return false;
    }

    bool parseLiteral(std::string_view literal)
    {
        if (input_.substr(position_, literal.size()) != literal) return false;
        position_ += literal.size();
        return true;
    }

    bool parseNumber()
    {
        const std::size_t start = position_;
        if (consume('-')) { }
        if (!consume('0') && !consumeDigits()) {
            return false;
        }
        if (consume('.')) {
            if (!consumeDigits()) return false;
        }
        if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) ++position_;
            if (!consumeDigits()) return false;
        }
        return position_ > start;
    }

    bool consumeDigits()
    {
        const std::size_t start = position_;
        while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
        return position_ != start;
    }

    bool consume(char expected)
    {
        if (position_ >= input_.size() || input_[position_] != expected) return false;
        ++position_;
        return true;
    }

    static bool isHex(char value)
    {
        return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f')
            || (value >= 'A' && value <= 'F');
    }

    void fail(const char *message)
    {
        if (error_) *error_ = std::string(message) + " at offset " + std::to_string(position_);
    }

    std::string_view input_;
    std::size_t position_ = 0;
    std::string *error_ = nullptr;
};

inline bool isValid(std::string_view input, std::string *error = nullptr)
{
    return Validator().validate(input, error);
}

} // namespace NativeJson
