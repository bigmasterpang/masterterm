// DPAPI cloud-sync crypto unit test (task T2-2).  Verifies the protect /
// unprotect roundtrip used for synced secrets, the encrypted-field marker
// and the failure modes (wrong base64, wrong entropy, empty input).
#include "NativeCrypto.h"
#include "NativeBase64.h"

#include <iostream>
#include <string>

namespace {

int g_failures = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::cerr << "FAIL: " #condition " (" << __FILE__ << ":"           \
                      << __LINE__ << ")\n";                                    \
            ++g_failures;                                                      \
        }                                                                      \
    } while (false)

void testRoundtrip()
{
    std::string error;
    const std::string encrypted = NativeCrypto::protectBase64(
        "s3cret-password-测试-1234", error);
    CHECK(!encrypted.empty());
    CHECK(error.empty());
    CHECK(NativeCrypto::isEncrypted(
        std::string(NativeCrypto::EncryptedPrefix) + encrypted));

    const std::string plain = NativeCrypto::unprotectBase64(encrypted, error);
    CHECK(error.empty());
    CHECK(plain == "s3cret-password-测试-1234");

    // Ciphertext must not contain the plaintext.
    CHECK(encrypted.find("s3cret-password") == std::string::npos);
}

void testBinaryRoundtrip()
{
    // Private key material is arbitrary bytes (base64-encoded before DPAPI).
    std::string binary;
    for (int index = 0; index < 512; ++index)
        binary.push_back(static_cast<char>((index * 7) % 256));
    std::string error;
    const std::string encrypted = NativeCrypto::protectBase64(binary, error);
    CHECK(!encrypted.empty() && error.empty());
    const std::string plain = NativeCrypto::unprotectBase64(encrypted, error);
    CHECK(error.empty());
    CHECK(plain == binary);
}

void testEmptyInput()
{
    std::string error;
    CHECK(NativeCrypto::protectBase64("", error).empty());
    CHECK(error.empty());
    // Empty ciphertext maps to "skip this field", not a hard failure.
    CHECK(NativeCrypto::unprotectBase64("", error).empty());
    CHECK(!error.empty());
}

void testInvalidCiphertext()
{
    std::string error;
    const std::string plain =
        NativeCrypto::unprotectBase64("bm90LWRwYXBp", error);
    CHECK(plain.empty());
    CHECK(!error.empty());
}

void testMarker()
{
    CHECK(NativeCrypto::isEncrypted("enc:abc123"));
    CHECK(NativeCrypto::isEncrypted("enc:"));
    CHECK(!NativeCrypto::isEncrypted("plaintext"));
    CHECK(!NativeCrypto::isEncrypted(""));
    CHECK(!NativeCrypto::isEncrypted("enc"));
}

} // namespace

int main()
{
    testRoundtrip();
    testBinaryRoundtrip();
    testEmptyInput();
    testInvalidCiphertext();
    testMarker();
    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "NATIVE_CRYPTO_TEST_OK\n";
    return 0;
}
