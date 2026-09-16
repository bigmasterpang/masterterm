#include "NativeKnownHosts.h"
#include "NativeMetrics.h"
#include "NativeString.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

// CHECK prints every failure and records it instead of relying on the Debug
// CRT assert dialog, so the test can run unattended without popups.
#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::cerr << "FAIL: " #condition " (" << __FILE__ << ":"           \
                      << __LINE__ << ")\n";                                    \
            ++g_failures;                                                      \
        }                                                                      \
    } while (false)

void testMetricsNormalSample()
{
    const std::string normal =
        "cpu  1013 42 1802 897345 4102 90 0 0 0 0\n"
        "cpu0 1013 42 1802 897345 4102 90 0 0 0 0\n"
        "MemTotal:       16384000 kB\n"
        "MemFree:         1234567 kB\n"
        "MemAvailable:   10240000 kB\n"
        "lo: 1000 0 0 0 0 0 0 0 2000 0 0 0 0 0 0 0\n"
        "eth0: 123456 0 0 0 0 0 0 0 654321 0 0 0 0 0 0 0\n"
        "12345.67 8910.11\n"
        "/dev/sda1 1234567890 456789012 678901234 41% /\n"
        "/dev/sda2 987654321 123456789 864197532 13% /boot";
    const NativeMetrics::Sample sample = NativeMetrics::parseSample(normal);
    CHECK(sample.usable());
    CHECK(sample.cpuTotal == 1013 + 42 + 1802 + 897345 + 4102 + 90);
    CHECK(sample.cpuIdle == 897345 + 4102);
    CHECK(sample.memoryTotal == 16384000LL * 1024);
    CHECK(sample.memoryAvailable == 10240000LL * 1024);
    CHECK(sample.netRx == 1000 + 123456);
    CHECK(sample.netTx == 2000 + 654321);
    CHECK(sample.uptime == 12345);
    // The leading filesystem token of a df line is stored as the partition
    // name; the first partition becomes the aggregate disk metric.
    CHECK(sample.diskMount == "/dev/sda1");
    CHECK(sample.diskTotal == 1234567890);
    CHECK(sample.diskUsed == 456789012);
    CHECK(sample.diskAvailable == 678901234);
    CHECK(sample.partitions.size() == 2);
    CHECK(sample.partitions.at(0).mount == "/dev/sda1");
    CHECK(sample.partitions.at(1).mount == "/dev/sda2");
    CHECK(sample.partitions.at(1).total == 987654321);

    // A later device-less root line ("/ ...") still wins as the aggregate.
    const NativeMetrics::Sample rootPreferred = NativeMetrics::parseSample(
        "/dev/sdb1 10 20 30\n"
        "/ 100 200 300 /\n");
    CHECK(rootPreferred.diskMount == "/");
    CHECK(rootPreferred.diskTotal == 100);
    CHECK(rootPreferred.partitions.size() == 2);
    CHECK(rootPreferred.partitions.at(1).mount == "/");
}

void testMetricsAbnormalInput()
{
    // Empty input yields an unusable sample with no partitions.
    const NativeMetrics::Sample empty = NativeMetrics::parseSample("");
    CHECK(!empty.usable());
    CHECK(empty.partitions.empty());

    // Garbage-only samples are discarded instead of being forwarded.
    const NativeMetrics::Sample garbage = NativeMetrics::parseSample(
        std::string("this is not /proc output\r\n\x01\x02\x03\r\n", 31));
    CHECK(!garbage.usable());
    CHECK(garbage.partitions.empty());

    // Truncated cpu lines are ignored while the rest of the sample survives.
    const NativeMetrics::Sample truncatedCpu = NativeMetrics::parseSample(
        "cpu 10 20 30\n"
        "MemTotal: 4096 kB\n");
    CHECK(truncatedCpu.usable());
    CHECK(truncatedCpu.cpuTotal == -1 && truncatedCpu.cpuIdle == -1);
    CHECK(truncatedCpu.memoryTotal == 4096LL * 1024);

    // Non-numeric cpu and memory fields parse to zero instead of crashing.
    const NativeMetrics::Sample nonNumeric = NativeMetrics::parseSample(
        "cpu abc def ghi jkl mno pqr\n"
        "MemTotal: not-a-number kB\n"
        "MemAvailable: 512 kB\n");
    CHECK(nonNumeric.usable());
    CHECK(nonNumeric.cpuTotal == 0);
    CHECK(nonNumeric.cpuIdle == 0);
    CHECK(nonNumeric.memoryTotal == 0);
    CHECK(nonNumeric.memoryAvailable == 512LL * 1024);

    // Disk lines with too few fields are ignored; malformed values stay zero
    // and the first shape-valid line becomes the aggregate disk metric.
    const NativeMetrics::Sample badDisk = NativeMetrics::parseSample(
        "/dev/sda1 only-three\n"
        "/dev/sdb1 nope 2 3\n"
        "/dev/sdc1 10 20 30\n");
    CHECK(!badDisk.usable());
    CHECK(badDisk.partitions.size() == 2);
    CHECK(badDisk.diskMount == "/dev/sdb1");
    CHECK(badDisk.diskTotal == 0);
    CHECK(badDisk.partitions.at(1).total == 10);

    // Short network lines are ignored; valid ones accumulate rx/tx and only
    // appear when at least one interface line exists.
    const NativeMetrics::Sample badNet = NativeMetrics::parseSample(
        "eth0: 1 2 3\n"
        "lo: 5 0 0 0 0 0 0 0 7 0 0 0 0 0 0 0\n");
    CHECK(badNet.netRx == 5 && badNet.netTx == 7);
    const NativeMetrics::Sample noNet = NativeMetrics::parseSample(
        "cpu 1 2 3 4 5 6\n"
        "MemTotal: 100 kB\n");
    CHECK(noNet.netRx == -1 && noNet.netTx == -1);

    // Uptime rejects non-numeric and negative values and requires a second
    // field; fractional input is truncated like the backend does.
    const NativeMetrics::Sample badUptime = NativeMetrics::parseSample(
        "abc 3\n"
        "-5 7\n"
        "123.5\n"
        "123.5 1\n");
    CHECK(badUptime.uptime == 123);

    // CRLF line endings are handled like LF.
    const NativeMetrics::Sample crlf = NativeMetrics::parseSample(
        "cpu 10 20 30 40 50 60\r\n"
        "MemTotal: 2048 kB\r\n"
        "123.0 1.0\r\n");
    CHECK(crlf.cpuTotal == 10 + 20 + 30 + 40 + 50 + 60);
    CHECK(crlf.memoryTotal == 2048LL * 1024);
    CHECK(crlf.uptime == 123);

    // Overflowing numbers are clamped by the string parser to zero.
    const NativeMetrics::Sample overflow = NativeMetrics::parseSample(
        "cpu 99999999999999999999999999 1 2 3 4 5\n"
        "MemTotal: 99999999999999999999999999 kB\n");
    CHECK(overflow.usable());
    CHECK(overflow.cpuTotal == 15);
    CHECK(overflow.cpuIdle == 7);
    CHECK(overflow.memoryTotal == 0);

    // Non-ASCII keys must not disturb parsing or crash.
    const NativeMetrics::Sample utf8 = NativeMetrics::parseSample(
        "cpu 1 2 3 4 5 6\n"
        "\xe4\xb8\xad\xe6\x96\x87 line\n");
    CHECK(utf8.usable());
    CHECK(utf8.cpuTotal == 21);
}

void testKnownHostsNormal()
{
    const std::vector<NativeKnownHosts::Entry> entries =
        NativeKnownHosts::parseText(
            "# comment\n"
            "\n"
            "example.com ssh-rsa AAAAB3NzaC1yc2EAAAADAQAB\n"
            "   spaced.example.com   ssh-ed25519   AAAAC3NzaC1lZDI1NTE5  \n"
            "[10.0.0.1]:2222 ecdsa-sha2-nistp256 AAAAE2VjZHNh\n"
            "plainhost\n"
            "host-with-type ssh-rsa\n");
    CHECK(entries.size() == 5);
    CHECK(entries.at(0).host == "example.com");
    CHECK(entries.at(0).keyType == "ssh-rsa");
    CHECK(entries.at(0).key == "AAAAB3NzaC1yc2EAAAADAQAB");
    CHECK(entries.at(1).host == "spaced.example.com");
    CHECK(entries.at(1).keyType == "ssh-ed25519");
    CHECK(entries.at(1).key == "AAAAC3NzaC1lZDI1NTE5");
    CHECK(entries.at(2).host == "[10.0.0.1]:2222");
    CHECK(entries.at(2).keyType == "ecdsa-sha2-nistp256");
    CHECK(entries.at(2).key == "AAAAE2VjZHNh");
    CHECK(entries.at(3).host == "plainhost");
    CHECK(entries.at(3).keyType.empty());
    CHECK(entries.at(3).key.empty());
    CHECK(entries.at(4).host == "host-with-type");
    CHECK(entries.at(4).keyType == "ssh-rsa");
    CHECK(entries.at(4).key.empty());
}

void testKnownHostsAbnormalInput()
{
    // CRLF, tabs and cert-authority/revoked marker lines are handled; marker
    // lines are not treated as hosts.
    const std::vector<NativeKnownHosts::Entry> crlf =
        NativeKnownHosts::parseText(
            "example.com ssh-rsa AAA\r\n"
            "@cert-authority *.example.com ssh-rsa BBB\r\n"
            "@revoked host ssh-ed25519 CCC\r\n"
            "\thost\ttab-separated\tDDD\t\n");
    CHECK(crlf.size() == 2);
    CHECK(crlf.at(0).host == "example.com");
    CHECK(crlf.at(1).host == "host");
    CHECK(crlf.at(1).keyType == "tab-separated");
    CHECK(crlf.at(1).key == "DDD");

    // Binary garbage and unterminated lines must not crash.
    const std::string garbage(
        "\xff\xfe\x00 line\r\n host ssh-rsa \xff\xfe key\n", 31);
    const std::vector<NativeKnownHosts::Entry> garbageEntries =
        NativeKnownHosts::parseText(garbage);
    CHECK(garbageEntries.size() == 2);
    CHECK(garbageEntries.at(0).host == std::string("\xff\xfe\x00", 3));
    CHECK(garbageEntries.at(1).key == "\xff\xfe key");

    // Empty, whitespace-only and comment-only content yields no entries.
    CHECK(NativeKnownHosts::parseText("").empty());
    CHECK(NativeKnownHosts::parseText(" \t\r\n\n   \n").empty());
    CHECK(NativeKnownHosts::parseText(
        "# only comments\n# another\n").empty());
}

} // namespace

int main()
{
    testMetricsNormalSample();
    testMetricsAbnormalInput();
    testKnownHostsNormal();
    testKnownHostsAbnormalInput();
    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
