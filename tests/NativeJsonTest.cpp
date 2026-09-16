#include "NativeJson.h"
#include "SftpTransferProtocol.h"
#include "NativeBase64.h"
#include "NativeJsonDom.h"
#include "NativeString.h"

#include <cassert>
#include <iostream>
#include <string>

int main()
{
    assert(NativeJson::isValid(R"({"id":"1","method":"app.getInfo","params":{}})"));
    assert(NativeJson::isValid(R"({"text":"中文\\n\u4e16\u754c","items":[true,false,null,-1.25e+2]})"));

    std::string error;
    assert(!NativeJson::isValid(R"({"id":})", &error));
    assert(!error.empty());
    assert(!NativeJson::isValid(R"([1,])"));
    assert(!NativeJson::isValid(R"({"unterminated":"value})"));
    assert(!NativeJson::isValid("-"));
    assert(NativeBase64::decode(NativeBase64::encode("MasterTerm 中文")) == "MasterTerm 中文");

    const auto singleProgress = parseSftpTransferProgressLine(
        "P\t128\t256\tYXJjaGl2ZS50YXI=\t2\t5\r");
    assert(singleProgress.has_value());
    assert(!singleProgress->directory);
    assert(singleProgress->done == 128 && singleProgress->total == 256);
    assert(singleProgress->hasFileProgress);
    assert(singleProgress->completedFiles == 2
           && singleProgress->totalFiles == 5);
    assert(singleProgress->encodedName == "YXJjaGl2ZS50YXI=");

    const auto directoryProgress = parseSftpTransferProgressLine(
        "D\t4096\t8192\t3\t10\taXEvZmlsZS50eHQ=");
    assert(directoryProgress.has_value() && directoryProgress->directory);
    assert(directoryProgress->completedFiles == 3
           && directoryProgress->totalFiles == 10);
    assert(!parseSftpTransferProgressLine("P\tnot-a-number\t2\tname"));
    assert(!parseSftpTransferProgressLine("D\t1\t2\t1\tname"));
    assert(!parseSftpTransferProgressLine("partial"));
    assert(NativeJson::quote("line\n\"quoted\"") == "\"line\\n\\\"quoted\\\"\"");
    NativeJsonDom::Value document;
    std::string domError;
    const bool domParsed = NativeJsonDom::parse(
        R"({"workspaces":["未分配","开发"],"enabled":true,"count":2})", document, &domError);
    if (!domParsed) std::cerr << domError << '\n';
    assert(domParsed);
    assert(document.isObject());
    assert(document.object().values.at("enabled").boolean());
    assert(document.object().values.at("workspaces").array().values.size() == 2);
    const std::string serialized = NativeJsonDom::stringify(document);
    NativeJsonDom::Value roundTrip;
    assert(NativeJsonDom::parse(serialized, roundTrip));
    assert(roundTrip.object().values.at("count").number() == 2);
    assert(roundTrip.object().values.at("workspaces").array().values.at(1).string()
           == "开发");

    NativeJsonDom::Value escapedUnicode;
    assert(NativeJsonDom::parse(
        "{\"name\":\"\\u9879\\u76ee \\uD83D\\uDE80\"}", escapedUnicode));
    assert(escapedUnicode.object().values.at("name").string() == "项目 🚀");
    assert(!NativeJsonDom::parse("{\"name\":\"\\uD83D\"}", escapedUnicode));
    assert(!NativeJsonDom::parse("{\"name\":\"\\uDE80\"}", escapedUnicode));

    NativeJsonDom::Value progress;
    assert(NativeJsonDom::parse(
        R"({"event":"sftp.transfer","sessionId":"","payload":{"transferId":"","state":"progress","done":4294967296,"total":8589934592,"name":"","completedFiles":0,"totalFiles":1}})",
        progress));
    const NativeJsonDom::Object &progressPayload =
        progress.object().values.at("payload").object();
    assert(progress.object().values.at("sessionId").string().empty());
    assert(progressPayload.values.at("transferId").string().empty());
    assert(progressPayload.values.at("name").string().empty());
    assert(progressPayload.values.at("done").number() == 4294967296.0);
    assert(progressPayload.values.at("total").number() == 8589934592.0);
    NativeJsonDom::Value progressRoundTrip;
    assert(NativeJsonDom::parse(
        NativeJsonDom::stringify(progress), progressRoundTrip));
    assert(progressRoundTrip.object().values.at("payload").object()
               .values.at("total").number() == 8589934592.0);

    NativeJsonDom::Value request;
    assert(NativeJsonDom::parse(
        R"({"id":"request-7","method":"session.resize","params":{"sessionId":"ssh-1","columns":120,"rows":40}})",
        request));
    assert(NativeJsonDom::stringValue(request.object(), "id") == "request-7");
    assert(NativeJsonDom::stringValue(request.object(), "method")
           == "session.resize");
    assert(NativeJsonDom::objectValue(request.object(), "params") != nullptr);
    assert(NativeJsonDom::stringValue(request.object(), "missing").empty());
    assert(NativeJsonDom::objectValue(request.object(), "method") == nullptr);

    NativeJsonDom::Object response;
    response.values.emplace("id", "request-7");
    response.values.emplace("ok", true);
    NativeJsonDom::Object result;
    result.values.emplace("name", "MasterTerm");
    result.values.emplace("transport", "webview2-json");
    response.values.emplace("result", std::move(result));
    NativeJsonDom::Value parsedResponse;
    assert(NativeJsonDom::parse(
        NativeJsonDom::stringify(NativeJsonDom::Value(std::move(response))),
        parsedResponse));
    assert(parsedResponse.object().values.at("ok").boolean());
    assert(parsedResponse.object().values.at("result").object()
               .values.at("name").string() == "MasterTerm");

    NativeJsonDom::Object directoryEntry;
    directoryEntry.values.emplace("name", "Downloads");
    directoryEntry.values.emplace("path", "C:\\Users\\master\\Downloads");
    directoryEntry.values.emplace("directory", true);
    directoryEntry.values.emplace("size", -1.0);
    directoryEntry.values.emplace("modified", 1785200000.0);
    NativeJsonDom::Array directoryEntries;
    directoryEntries.values.emplace_back(std::move(directoryEntry));
    NativeJsonDom::Object directoryResult;
    directoryResult.values.emplace("path", "C:\\Users\\master");
    directoryResult.values.emplace("parent", "C:\\Users");
    directoryResult.values.emplace("entries", std::move(directoryEntries));
    NativeJsonDom::Value directoryRoundTrip;
    assert(NativeJsonDom::parse(
        NativeJsonDom::stringify(
            NativeJsonDom::Value(std::move(directoryResult))),
        directoryRoundTrip));
    const NativeJsonDom::Value &firstEntry =
        directoryRoundTrip.object().values.at("entries").array().values.at(0);
    assert(firstEntry.object().values.at("directory").boolean());
    assert(firstEntry.object().values.at("size").number() == -1.0);

    NativeJsonDom::Object eventMessage;
    eventMessage.values.emplace("event", "session.state");
    eventMessage.values.emplace("sessionId", "ssh-1");
    NativeJsonDom::Object eventPayload;
    eventPayload.values.emplace("state", "connected");
    eventMessage.values.emplace("payload", std::move(eventPayload));
    NativeJsonDom::Value eventRoundTrip;
    assert(NativeJsonDom::parse(
        NativeJsonDom::stringify(
            NativeJsonDom::Value(std::move(eventMessage))),
        eventRoundTrip));
    assert(NativeJsonDom::stringValue(eventRoundTrip.object(), "event")
           == "session.state");

    NativeJsonDom::Object remoteEntry;
    remoteEntry.values.emplace("name", "项目");
    remoteEntry.values.emplace("path", "/home/master/项目");
    remoteEntry.values.emplace("kind", "d");
    remoteEntry.values.emplace("directory", true);
    remoteEntry.values.emplace("size", 0.0);
    remoteEntry.values.emplace("modified", 1785200000.0);
    remoteEntry.values.emplace("permissions", "755");
    remoteEntry.values.emplace("owner", "master");
    NativeJsonDom::Array remoteEntries;
    remoteEntries.values.emplace_back(std::move(remoteEntry));
    NativeJsonDom::Object sftpList;
    sftpList.values.emplace("index", 2.0);
    sftpList.values.emplace("path", "/home/master");
    sftpList.values.emplace("entries", std::move(remoteEntries));
    NativeJsonDom::Value sftpListRoundTrip;
    assert(NativeJsonDom::parse(
        NativeJsonDom::stringify(NativeJsonDom::Value(std::move(sftpList))),
        sftpListRoundTrip));
    assert(sftpListRoundTrip.object().values.at("entries").array()
               .values.at(0).object().values.at("owner").string()
           == "master");

    NativeJsonDom::Object transferProgress;
    transferProgress.values.emplace("transferId", "sftp-9");
    transferProgress.values.emplace("state", "progress");
    transferProgress.values.emplace("done", 4096.0);
    transferProgress.values.emplace("total", 8192.0);
    transferProgress.values.emplace("name", "archive.tar");
    transferProgress.values.emplace("completedFiles", 3.0);
    transferProgress.values.emplace("totalFiles", 10.0);
    NativeJsonDom::Value transferRoundTrip;
    assert(NativeJsonDom::parse(
        NativeJsonDom::stringify(
            NativeJsonDom::Value(std::move(transferProgress))),
        transferRoundTrip));
    assert(transferRoundTrip.object().values.at("completedFiles").number()
           == 3.0);

    NativeJsonDom::Object outputPayload;
    outputPayload.values.emplace(
        "data", NativeBase64::encode("terminal\r\n中文"));
    NativeJsonDom::Object outputEvent;
    outputEvent.values.emplace("event", "session.output");
    outputEvent.values.emplace("sessionId", "ssh-12");
    outputEvent.values.emplace("payload", std::move(outputPayload));
    NativeJsonDom::Value outputRoundTrip;
    assert(NativeJsonDom::parse(
        NativeJsonDom::stringify(
            NativeJsonDom::Value(std::move(outputEvent))),
        outputRoundTrip));
    assert(NativeBase64::decode(
        outputRoundTrip.object().values.at("payload").object()
            .values.at("data").string())
           == "terminal\r\n中文");

    NativeJsonDom::Object authPayload;
    authPayload.values.emplace("index", 1.0);
    authPayload.values.emplace("name", "测试服务器");
    authPayload.values.emplace("address", "127.0.0.1");
    NativeJsonDom::Object authEvent;
    authEvent.values.emplace("event", "auth.required");
    authEvent.values.emplace("sessionId", "");
    authEvent.values.emplace("payload", std::move(authPayload));
    NativeJsonDom::Value authRoundTrip;
    assert(NativeJsonDom::parse(
        NativeJsonDom::stringify(
            NativeJsonDom::Value(std::move(authEvent))),
        authRoundTrip));
    assert(NativeJsonDom::stringValue(authRoundTrip.object(), "sessionId")
           .empty());

    NativeJsonDom::Object nativeParams;
    nativeParams.values.emplace("index", 3.0);
    nativeParams.values.emplace("port", "2222");
    nativeParams.values.emplace("temporary", true);
    nativeParams.values.emplace("wrongNumber", "invalid");
    nativeParams.values.emplace("decimalPort", "22.5");
    NativeJsonDom::Array paths;
    paths.values.emplace_back("C:/Users/test/a.txt");
    nativeParams.values.emplace("paths", std::move(paths));
    assert(NativeJsonDom::integerValue(nativeParams, "index", -1) == 3);
    assert(NativeJsonDom::integerValue(nativeParams, "port", -1) == -1);
    assert(NativeJsonDom::convertedIntegerValue(
               nativeParams, "port", 22) == 2222);
    assert(NativeJsonDom::convertedIntegerValue(
               nativeParams, "wrongNumber", 22) == 22);
    assert(NativeJsonDom::convertedIntegerValue(
               nativeParams, "decimalPort", 22) == 22);
    assert(NativeJsonDom::booleanValue(
        nativeParams, "temporary"));
    assert(!NativeJsonDom::booleanValue(
        nativeParams, "missing"));
    assert(NativeJsonDom::contains(nativeParams, "port"));
    const NativeJsonDom::Array *pathValues =
        NativeJsonDom::arrayValue(nativeParams, "paths");
    assert(pathValues && pathValues->values.size() == 1);
    assert(!NativeJsonDom::arrayValue(nativeParams, "port"));

    NativeString nativeString = NativeString::fromUtf8("Alpha/目录/");
    nativeString.replace('/', '_');
    nativeString.chop(1);
    assert(nativeString == "Alpha_目录");
    assert(nativeString.toLower() == "alpha_目录");
    assert(NativeString("first\nlast").section('\n', -1) == "last");
    const std::wstring nativePath = L"C:\\Users\\用户\\🚀";
    assert(NativeString::fromStdWString(nativePath).toStdWString() == nativePath);
    return 0;
}
