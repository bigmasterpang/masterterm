#pragma once

#include "NativeString.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

// Pure parsing of one remote monitor sample emitted by
// MasterTermSftpWorker --monitor (raw /proc/stat, /proc/meminfo,
// /proc/net/dev, /proc/uptime and `df -P -B1` output).  Malformed lines are
// ignored without affecting the rest of the sample.
namespace NativeMetrics {

struct DiskPartition
{
    std::string mount;
    std::int64_t total = -1;
    std::int64_t used = -1;
    std::int64_t available = -1;
};

struct Sample
{
    std::int64_t cpuTotal = -1;
    std::int64_t cpuIdle = -1;
    std::int64_t memoryTotal = -1;
    std::int64_t memoryAvailable = -1;
    std::int64_t netRx = -1;
    std::int64_t netTx = -1;
    std::int64_t uptime = -1;
    std::string diskMount;
    std::int64_t diskTotal = -1;
    std::int64_t diskUsed = -1;
    std::int64_t diskAvailable = -1;
    std::vector<DiskPartition> partitions;

    // Mirrors the backend gate: a sample without CPU, memory or uptime data
    // is discarded instead of being forwarded to the UI.
    bool usable() const
    {
        return cpuTotal >= 0 || memoryTotal >= 0 || uptime >= 0;
    }
};

namespace detail {

inline std::vector<std::string> splitLines(const std::string &value)
{
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (true) {
        const std::size_t end = value.find('\n', start);
        lines.push_back(value.substr(
            start, end == std::string::npos ? end : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return lines;
}

inline std::vector<std::string> words(const std::string &line)
{
    std::vector<std::string> result;
    std::size_t begin = 0;
    while (begin < line.size()) {
        while (begin < line.size()
               && std::isspace(static_cast<unsigned char>(line[begin]))) ++begin;
        const std::size_t end = begin;
        while (begin < line.size()
               && !std::isspace(static_cast<unsigned char>(line[begin]))) ++begin;
        if (begin > end) result.emplace_back(line.substr(end, begin - end));
    }
    return result;
}

} // namespace detail

inline Sample parseSample(const std::string &sample)
{
    Sample result;
    for (const std::string &rawLine : detail::splitLines(sample)) {
        const std::vector<std::string> fields = detail::words(rawLine);
        if (fields.empty()) continue;
        const NativeString key = NativeString(fields.at(0)).trimmed();
        if (key == NativeString("cpu") && fields.size() >= 6) {
            result.cpuTotal = 0;
            for (std::size_t index = 1; index < fields.size(); ++index)
                result.cpuTotal += NativeString(fields.at(index)).toLongLong();
            result.cpuIdle = NativeString(fields.at(4)).toLongLong()
                + NativeString(fields.at(5)).toLongLong();
        } else if (key == NativeString("MemTotal:") && fields.size() >= 2) {
            result.memoryTotal = NativeString(fields.at(1)).toLongLong() * 1024;
        } else if (key == NativeString("MemAvailable:") && fields.size() >= 2) {
            result.memoryAvailable =
                NativeString(fields.at(1)).toLongLong() * 1024;
        } else if (key.startsWith('/') && fields.size() >= 4) {
            DiskPartition partition;
            partition.mount = key.toStdString();
            partition.total = NativeString(fields.at(1)).toLongLong();
            partition.used = NativeString(fields.at(2)).toLongLong();
            partition.available = NativeString(fields.at(3)).toLongLong();
            if (result.diskMount.empty() || key == NativeString("/")) {
                // Aggregate values must be copied before the partition is
                // moved into the array; a moved-from std::string is empty.
                result.diskMount = key.toStdString();
                result.diskTotal = partition.total;
                result.diskUsed = partition.used;
                result.diskAvailable = partition.available;
            }
            result.partitions.push_back(std::move(partition));
        } else if (rawLine.find(':') != std::string::npos
                   && fields.size() >= 10 && key != NativeString("Inter-|")) {
            if (result.netRx < 0) result.netRx = 0;
            if (result.netTx < 0) result.netTx = 0;
            result.netRx += NativeString(fields.at(1)).toLongLong();
            result.netTx += NativeString(fields.at(9)).toLongLong();
        } else if (fields.size() == 2) {
            char *end = nullptr;
            const double seconds = std::strtod(fields.at(0).c_str(), &end);
            if (end && *end == '\0' && seconds >= 0)
                result.uptime = static_cast<std::int64_t>(seconds);
        }
    }
    return result;
}

} // namespace NativeMetrics
