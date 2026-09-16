#pragma once

#include <array>
#include <string>
#include <string_view>

namespace NativeBase64 {

inline std::string encode(std::string_view input)
{
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);
    for (std::size_t index = 0; index < input.size(); index += 3) {
        const unsigned int first = static_cast<unsigned char>(input[index]);
        const unsigned int second = index + 1 < input.size()
            ? static_cast<unsigned char>(input[index + 1]) : 0;
        const unsigned int third = index + 2 < input.size()
            ? static_cast<unsigned char>(input[index + 2]) : 0;
        output.push_back(alphabet[first >> 2]);
        output.push_back(alphabet[((first & 0x03) << 4) | (second >> 4)]);
        output.push_back(index + 1 < input.size() ? alphabet[((second & 0x0f) << 2) | (third >> 6)] : '=');
        output.push_back(index + 2 < input.size() ? alphabet[third & 0x3f] : '=');
    }
    return output;
}

inline std::string decode(std::string_view input)
{
    static constexpr unsigned char invalid = 0xff;
    static const auto table = [] {
        std::array<unsigned char, 256> values{};
        values.fill(invalid);
        for (unsigned char index = 0; index < 26; ++index) {
            values['A' + index] = index;
            values['a' + index] = 26 + index;
        }
        for (unsigned char index = 0; index < 10; ++index) values['0' + index] = 52 + index;
        values[static_cast<unsigned char>('+')] = 62;
        values[static_cast<unsigned char>('/')] = 63;
        return values;
    }();
    std::string output;
    unsigned int buffer = 0;
    int bits = 0;
    for (const unsigned char value : input) {
        if (value == '=' || value == ' ' || value == '\r' || value == '\n' || value == '\t') continue;
        if (table[value] == invalid) return {};
        buffer = (buffer << 6) | table[value];
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output.push_back(static_cast<char>((buffer >> bits) & 0xff));
        }
    }
    return output;
}

} // namespace NativeBase64
