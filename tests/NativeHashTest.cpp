#include "NativeHash.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

int main(int argc, char **argv)
{
    const std::string abcExpected =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    if (NativeHash::sha256Hex("abc") != abcExpected) {
        std::cerr << "NATIVE_HASH_ABC_FAILED\n";
        return 1;
    }
    if (argc < 2) {
        std::cout << "NATIVE_HASH_TEST_OK\n";
        return 0;
    }
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) {
        std::cerr << "NATIVE_HASH_FILE_OPEN_FAILED\n";
        return 2;
    }
    const std::string bytes{
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    std::cout << NativeHash::sha256Hex(bytes) << "\n";
    return 0;
}
