#include "SftpTransferProtocol.h"

#include <charconv>
#include <string_view>
#include <vector>

namespace {

std::vector<std::string_view> splitFields(std::string_view line)
{
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (start <= line.size()) {
        const std::size_t end = line.find('\t', start);
        fields.emplace_back(line.substr(start, end == std::string_view::npos
                                             ? std::string_view::npos
                                             : end - start));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return fields;
}

bool parseInteger(std::string_view value, std::int64_t &result)
{
    if (value.empty()) return false;
    const char *first = value.data();
    const char *last = first + value.size();
    const auto parsed = std::from_chars(first, last, result);
    return parsed.ec == std::errc() && parsed.ptr == last;
}

}

std::optional<SftpTransferProgress> parseSftpTransferProgressLine(
    const std::string &line)
{
    std::string_view text(line);
    if (!text.empty() && text.back() == '\r') text.remove_suffix(1);
    const auto fields = splitFields(text);
    if (fields.size() < 4 || (fields[0] != "P" && fields[0] != "D"))
        return std::nullopt;

    SftpTransferProgress progress;
    progress.directory = fields[0] == "D";
    if (!parseInteger(fields[1], progress.done)
        || !parseInteger(fields[2], progress.total))
        return std::nullopt;

    if (progress.directory) {
        if (fields.size() < 6
            || !parseInteger(fields[3], progress.completedFiles)
            || !parseInteger(fields[4], progress.totalFiles))
            return std::nullopt;
        progress.encodedName = std::string(fields[5]);
        progress.hasFileProgress = true;
    } else {
        progress.encodedName = std::string(fields[3]);
        if (fields.size() >= 6) {
            if (!parseInteger(fields[4], progress.completedFiles)
                || !parseInteger(fields[5], progress.totalFiles))
                return std::nullopt;
            progress.hasFileProgress = true;
        }
    }
    if (fields.size() >= 8 && parseInteger(fields[6], progress.fileDone)) {
        progress.hasFileBytes = parseInteger(fields[7], progress.fileTotal)
            && progress.fileTotal > 0;
    }
    return progress;
}
