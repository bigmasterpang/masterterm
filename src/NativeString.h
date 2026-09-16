#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cwchar>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace NativeText {

inline std::wstring utf8ToWide(const std::string &value)
{
#ifdef _WIN32
    if (value.empty()) return {};
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) {
        count = MultiByteToWideChar(CP_UTF8, 0,
            value.data(), static_cast<int>(value.size()), nullptr, 0);
        if (count <= 0) return {};
        std::wstring result(static_cast<std::size_t>(count), L'\0');
        return MultiByteToWideChar(CP_UTF8, 0, value.data(),
            static_cast<int>(value.size()), result.data(), count) == count ? result : std::wstring();
    }
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), count) == count ? result : std::wstring();
#else
    return std::wstring(value.begin(), value.end());
#endif
}

inline std::string wideToUtf8(const std::wstring &value)
{
#ifdef _WIN32
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<std::size_t>(count), '\0');
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), count, nullptr, nullptr) == count
        ? result : std::string();
#else
    return std::string(value.begin(), value.end());
#endif
}

} // namespace NativeText

class NativeString final {
public:
    NativeString() = default;
    NativeString(const char *value) : value_(value ? value : ""), null_(value == nullptr) {}
    NativeString(const std::string &value) : value_(value) {}

    bool isEmpty() const { return value_.empty(); }
    bool isNull() const { return null_; }
    int size() const { return static_cast<int>(value_.size()); }
    const std::string &toStdString() const { return value_; }
    std::wstring toStdWString() const
    {
        return NativeText::utf8ToWide(value_);
    }
    std::string toUtf8() const { return value_; }

    NativeString trimmed() const
    {
        const auto first = value_.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return {};
        const auto last = value_.find_last_not_of(" \t\r\n");
        return value_.substr(first, last - first + 1);
    }
    NativeString left(int count) const { return value_.substr(0, (std::max)(0, count)); }
    NativeString mid(int start, int count = -1) const
    {
        if (start < 0) start = 0;
        if (start >= size()) return {};
        return count < 0 ? value_.substr(static_cast<std::size_t>(start))
                         : value_.substr(static_cast<std::size_t>(start), static_cast<std::size_t>(count));
    }
    int indexOf(char character, int from = 0) const
    {
        const auto position = value_.find(character, static_cast<std::size_t>((std::max)(0, from)));
        return position == std::string::npos ? -1 : static_cast<int>(position);
    }
    int indexOf(const NativeString &value, int from = 0) const
    {
        const auto position = value_.find(value.value_, static_cast<std::size_t>((std::max)(0, from)));
        return position == std::string::npos ? -1 : static_cast<int>(position);
    }
    int lastIndexOf(char character) const
    {
        const auto position = value_.rfind(character);
        return position == std::string::npos ? -1 : static_cast<int>(position);
    }
    bool contains(char character) const { return value_.find(character) != std::string::npos; }
    bool contains(const NativeString &value) const { return value_.find(value.value_) != std::string::npos; }
    bool startsWith(char character) const { return !value_.empty() && value_.front() == character; }
    bool startsWith(const NativeString &value) const { return value_.compare(0, value.value_.size(), value.value_) == 0; }
    bool endsWith(char character) const { return !value_.empty() && value_.back() == character; }
    bool endsWith(const NativeString &value) const { return value_.size() >= value.value_.size() && value_.compare(value_.size() - value.value_.size(), value.value_.size(), value.value_) == 0; }

    NativeString toLower() const
    {
        NativeString result = *this;
        std::transform(result.value_.begin(), result.value_.end(),
                       result.value_.begin(), [](unsigned char value) {
                           return static_cast<char>(std::tolower(value));
                       });
        return result;
    }
    NativeString section(char separator, int index) const
    {
        if (index >= 0) {
            std::size_t start = 0;
            for (int current = 0; current < index; ++current) {
                start = value_.find(separator, start);
                if (start == std::string::npos) return {};
                ++start;
            }
            const std::size_t end = value_.find(separator, start);
            return value_.substr(start, end == std::string::npos
                ? std::string::npos : end - start);
        }
        const std::size_t start = value_.rfind(separator);
        return start == std::string::npos ? *this : value_.substr(start + 1);
    }

    int toInt(bool *ok = nullptr) const { return parse<int>(ok); }
    long long toLongLong(bool *ok = nullptr) const { return parse<long long>(ok); }
    unsigned int toUInt(bool *ok = nullptr, int base = 10) const
    {
        try {
            std::size_t used = 0;
            const unsigned long value = std::stoul(value_, &used, base);
            if (ok) *ok = used == value_.size();
            return static_cast<unsigned int>(value);
        } catch (...) {
            if (ok) *ok = false;
            return 0;
        }
    }

    NativeString &prepend(char character) { value_.insert(value_.begin(), character); return *this; }
    void chop(int count)
    {
        if (count <= 0) return;
        value_.resize(value_.size() > static_cast<std::size_t>(count)
            ? value_.size() - static_cast<std::size_t>(count) : 0);
    }
    NativeString &operator+=(const NativeString &value) { value_ += value.value_; return *this; }
    NativeString &operator+=(char value) { value_ += value; return *this; }
    void clear() { value_.clear(); null_ = false; }
    NativeString &replace(const NativeString &before, const NativeString &after)
    {
        std::size_t position = 0;
        while ((position = value_.find(before.value_, position)) != std::string::npos) {
            value_.replace(position, before.value_.size(), after.value_);
            position += after.value_.size();
        }
        return *this;
    }
    NativeString &replace(char before, char after)
    {
        std::replace(value_.begin(), value_.end(), before, after);
        return *this;
    }

    template<typename... Args>
    NativeString arg(const Args &...args) const
    {
        NativeString result = *this;
        (result.replaceNext(format(args)), ...);
        return result;
    }

    static NativeString fromStdString(const std::string &value) { return value; }
    static NativeString fromStdWString(const std::wstring &value)
    { return NativeText::wideToUtf8(value); }
    static NativeString fromUtf8(const char *value, int count = -1)
    { return value ? std::string(value, count < 0 ? std::strlen(value) : static_cast<std::size_t>(count)) : NativeString(); }
    static NativeString fromLocal8Bit(const char *value) { return value ? value : ""; }
    static NativeString fromLatin1(const char *value, int count = -1) { return fromUtf8(value, count); }
    static NativeString fromWCharArray(const wchar_t *value, int count = -1)
    { return value ? fromStdWString(std::wstring(value, count < 0 ? std::wcslen(value) : static_cast<std::size_t>(count))) : NativeString(); }
    template<typename T> static NativeString number(T value, int base = 10)
    {
        if (base == 10) return std::to_string(value);
        std::ostringstream stream;
        stream << std::setbase(base) << value;
        return stream.str();
    }

private:
    template<typename T> T parse(bool *ok) const
    {
        try {
            std::size_t used = 0;
            long long value = std::stoll(value_, &used, 0);
            if (ok) *ok = used == value_.size();
            return static_cast<T>(value);
        } catch (...) {
            if (ok) *ok = false;
            return {};
        }
    }
    static NativeString format(const NativeString &value) { return value; }
    static NativeString format(const char *value) { return value ? value : ""; }
    template<typename T> static NativeString format(const T &value) { return number(value); }
    void replaceNext(const NativeString &value)
    {
        const auto position = value_.find('%');
        if (position == std::string::npos || position + 1 >= value_.size()) return;
        if (!std::isdigit(static_cast<unsigned char>(value_[position + 1]))) return;
        value_.replace(position, 2, value.value_);
    }
    std::string value_;
    bool null_ = false;
};

inline NativeString operator+(const NativeString &left, const NativeString &right)
{ return left.toStdString() + right.toStdString(); }
inline NativeString operator+(const NativeString &left, const char *right)
{ return left.toStdString() + NativeString(right).toStdString(); }
inline NativeString operator+(const char *left, const NativeString &right)
{ return NativeString(left).toStdString() + right.toStdString(); }
inline NativeString operator+(const NativeString &left, char right)
{ return left.toStdString() + std::string(1, right); }
inline NativeString operator+(char left, const NativeString &right)
{ return std::string(1, left) + right.toStdString(); }
inline bool operator==(const NativeString &left, const NativeString &right)
{ return left.toStdString() == right.toStdString(); }
inline bool operator!=(const NativeString &left, const NativeString &right)
{ return !(left == right); }
inline bool operator<(const NativeString &left, const NativeString &right)
{ return left.toStdString() < right.toStdString(); }

template<typename T> T nativeMax(T left, T right) { return left > right ? left : right; }
template<typename T> T nativeMin(T left, T right) { return left < right ? left : right; }
template<typename T> T nativeBound(T minimum, T value, T maximum)
{ return nativeMax(minimum, nativeMin(value, maximum)); }
