#pragma once

#include <cstdint>
#include <optional>
#include <string>

struct SftpTransferProgress {
    bool directory = false;
    std::int64_t done = 0;
    std::int64_t total = 0;
    std::int64_t completedFiles = 0;
    std::int64_t totalFiles = 0;
    bool hasFileProgress = false;
    std::int64_t fileDone = 0;
    std::int64_t fileTotal = 0;
    bool hasFileBytes = false;
    std::string encodedName;
};

// Parses one complete line emitted by MasterTermSftpWorker. The worker uses
// P for a single file and D for a directory tree; malformed or partial lines
// are rejected so the caller can wait for more stdout data.
std::optional<SftpTransferProgress> parseSftpTransferProgressLine(
    const std::string &line);
