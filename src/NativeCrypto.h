#pragma once

// DPAPI-based secret protection for cloud sync (task T2-2).
//
// Passwords, key passphrases, proxy passwords and private key contents are
// encrypted with Windows DPAPI (current user scope) before leaving the
// machine, so the sync server only ever sees ciphertext.  Encryption is
// per-device by design: another machine receives the encrypted blob but
// cannot decrypt it and the import path falls back to prompting for
// credentials (the field is skipped and counted instead of failing).

#include "NativeBase64.h"

#include <windows.h>
#include <dpapi.h>

#include <string>
#include <string_view>

namespace NativeCrypto {

inline constexpr wchar_t CloudSyncEntropy[] =
    L"MasterTerm.CloudSync.v1";

inline DATA_BLOB cloudEntropyBlob()
{
    DATA_BLOB blob{};
    blob.pbData = reinterpret_cast<BYTE *>(
        const_cast<wchar_t *>(CloudSyncEntropy));
    blob.cbData = static_cast<DWORD>(
        (wcslen(CloudSyncEntropy) + 1) * sizeof(wchar_t));
    return blob;
}

// The "enc:" prefix marks DPAPI-encrypted fields inside the sync payload so
// legacy plaintext values stay importable (schema evolution, T2-1).
inline constexpr char EncryptedPrefix[] = "enc:";

inline bool isEncrypted(std::string_view value)
{
    return value.rfind(EncryptedPrefix, 0) == 0;
}

inline std::string protectBase64(std::string_view plain, std::string &error)
{
    if (plain.empty())
        return {};
    DATA_BLOB input{};
    input.pbData = const_cast<BYTE *>(
        reinterpret_cast<const BYTE *>(plain.data()));
    input.cbData = static_cast<DWORD>(plain.size());
    DATA_BLOB output{};
    DATA_BLOB entropy = cloudEntropyBlob();
    if (!CryptProtectData(
            &input, L"MasterTerm 云同步凭据", &entropy, nullptr, nullptr,
            CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        error = "无法加密同步凭据（Windows 错误 "
            + std::to_string(GetLastError()) + "）";
        return {};
    }
    const std::string result = NativeBase64::encode(std::string(
        reinterpret_cast<const char *>(output.pbData), output.cbData));
    LocalFree(output.pbData);
    return result;
}

inline std::string unprotectBase64(std::string_view encoded, std::string &error)
{
    const std::string raw = NativeBase64::decode(encoded);
    if (raw.empty()) {
        error = "同步凭据密文无效";
        return {};
    }
    DATA_BLOB input{};
    input.pbData = const_cast<BYTE *>(
        reinterpret_cast<const BYTE *>(raw.data()));
    input.cbData = static_cast<DWORD>(raw.size());
    DATA_BLOB output{};
    DATA_BLOB entropy = cloudEntropyBlob();
    if (!CryptUnprotectData(
            &input, nullptr, &entropy, nullptr, nullptr,
            CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        error = "无法解密同步凭据（可能来自其他设备或用户）";
        return {};
    }
    const std::string result(
        reinterpret_cast<const char *>(output.pbData), output.cbData);
    LocalFree(output.pbData);
    return result;
}

} // namespace NativeCrypto
