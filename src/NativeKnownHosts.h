#pragma once

#include <string>
#include <vector>

// Pure parsing of the OpenSSH known_hosts text file used by the known-host
// management UI.  Comment, blank and marker lines (cert-authority/revoked)
// are skipped; the remaining lines are split into host / key type / key.
namespace NativeKnownHosts {

struct Entry
{
    std::string line;
    std::string host;
    std::string keyType;
    std::string key;
};

inline bool isHostEntry(const std::string &trimmed)
{
    return !trimmed.empty() && trimmed.front() != '#'
        && trimmed.front() != '@';
}

inline std::vector<Entry> parseText(const std::string &content)
{
    std::vector<Entry> result;
    std::size_t start = 0;
    while (true) {
        const std::size_t end = content.find('\n', start);
        std::string line = content.substr(
            start, end == std::string::npos ? end : end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos) {
            if (end == std::string::npos) break;
            start = end + 1;
            continue;
        }
        const std::size_t last = line.find_last_not_of(" \t");
        const std::string trimmed = line.substr(first, last - first + 1);
        if (isHostEntry(trimmed)) {
            Entry entry;
            entry.line = trimmed;
            const std::size_t separator = trimmed.find_first_of(" \t");
            entry.host = separator == std::string::npos
                ? trimmed : trimmed.substr(0, separator);
            const std::size_t typeBegin = separator == std::string::npos
                ? std::string::npos
                : trimmed.find_first_not_of(" \t", separator);
            const std::size_t typeEnd = typeBegin == std::string::npos
                ? std::string::npos
                : trimmed.find_first_of(" \t", typeBegin);
            if (typeBegin != std::string::npos)
                entry.keyType = trimmed.substr(typeBegin,
                    typeEnd == std::string::npos
                        ? std::string::npos : typeEnd - typeBegin);
            const std::size_t keyBegin = typeEnd == std::string::npos
                ? std::string::npos
                : trimmed.find_first_not_of(" \t", typeEnd);
            if (keyBegin != std::string::npos)
                entry.key = trimmed.substr(keyBegin);
            result.push_back(std::move(entry));
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return result;
}

} // namespace NativeKnownHosts
