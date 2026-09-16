#include "NativeString.h"

#include <cassert>
#include <filesystem>
#include <string>

int main()
{
    const NativeString original = NativeString::fromUtf8(
        "项目 🚀 / 空格 (括号) / 深层目录 / 文件名.txt");
    assert(!original.isEmpty());
    assert(NativeString::fromStdWString(original.toStdWString()) == original);

    const std::string longComponent(180, 'x');
    const NativeString longPath = NativeString::fromUtf8(
        ("C:/MasterTerm/" + longComponent + "/中文 Emoji 🚀 (1).txt").c_str());
    assert(NativeString::fromStdWString(longPath.toStdWString()) == longPath);

    const std::filesystem::path widePath(longPath.toStdWString());
    assert(NativeString::fromStdWString(widePath.filename().wstring())
           == NativeString::fromUtf8("中文 Emoji 🚀 (1).txt"));
    return 0;
}
