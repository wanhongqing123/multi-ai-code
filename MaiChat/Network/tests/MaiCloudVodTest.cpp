#include "MaiCloudVod.h"

#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr size_t kChunkBytes = 1024 * 1024;
constexpr size_t kCacheBytes = 8 * kChunkBytes;

void require(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

size_t collect(char* data, size_t size, size_t count, void* context) {
    const size_t length = size * count;
    static_cast<std::string*>(context)->append(data, length);
    return length;
}

struct Metrics {
    unsigned ranges = 0;
    unsigned long long bytes = 0;
    unsigned auth = 0;
    unsigned long long highestRange = 0;
    unsigned renewAuth = 0;
    unsigned ossConnections = 0;
    unsigned ossAuthHeaders = 0;
};

Metrics metrics(const std::string& base, const char* caPath) {
    CURL* curl = curl_easy_init();
    require(curl != nullptr, "metric CURL initialization failed");
    const std::string url = base + "/metrics";
    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CAINFO, caPath);
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    const CURLcode status = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    require(status == CURLE_OK, "metric request failed");
    Metrics result;
    require(std::sscanf(body.c_str(), "%u %llu %u %llu %u %u %u",
                        &result.ranges, &result.bytes, &result.auth,
                        &result.highestRange, &result.renewAuth,
                        &result.ossConnections, &result.ossAuthHeaders) == 7,
            "metric response invalid");
    return result;
}

MaiCloudVodOptions optionsFor(const std::string& url, const char* caPath,
                              uint64_t expected) {
    MaiCloudVodOptions options{};
    options.signed_url_utf8 = url.c_str();
    options.signed_host_suffix_utf8 = "localhost";
    options.expected_size_bytes = expected;
    options.ca_path_utf8 = caPath;
    options.proxy_url_utf8 = "";
    return options;
}

void verifyBytes(MaiCloudVod* source, uint64_t offset, size_t length) {
    std::vector<unsigned char> output(length);
    size_t received = 0;
    MaiCloudVodResult result{};
    require(mai_cloud_vod_read_at(source, offset, output.data(), length,
                                  &received, &result) == 1 &&
            result.status == MAI_CLOUD_VOD_OK && received == length,
            "random range read failed");
    for (size_t index = 0; index < length; ++index)
        require(output[index] == (offset + index) % 251,
                "random range bytes differed from source");
}

struct Refresh {
    std::string url;
    unsigned calls = 0;
};

int refresh(void* context, char* output, size_t capacity) {
    auto* state = static_cast<Refresh*>(context);
    ++state->calls;
    if (state->url.size() + 1 > capacity) return 0;
    std::memcpy(output, state->url.c_str(), state->url.size() + 1);
    return 1;
}

struct ProxySwitch {
    unsigned calls = 0;
};

int rotatingProxy(const char*, void* context, MaiDownloadProxy* selected) {
    auto* state = static_cast<ProxySwitch*>(context);
    ++state->calls;
    selected->url_utf8 = ""; // Direct TLS in this fixture.
    selected->username_utf8 = state->calls == 1 ? "fixture-user" : nullptr;
    selected->password_utf8 = state->calls == 1 ? "fixture-secret" : nullptr;
    return 1;
}

void testCore(const std::string& base, const char* caPath, uint64_t size) {
    const Metrics before = metrics(base, caPath);
    const std::string url = base + "/oss/play?sig=valid";
    MaiCloudVodOptions options = optionsFor(url, caPath, size);
    MaiCloudVodResult result{};
    MaiCloudVod* source = mai_cloud_vod_open(&options, &result);
    require(source && result.status == MAI_CLOUD_VOD_OK,
            "first Range open failed");
    require(mai_cloud_vod_size(source) == size, "source size mismatch");
    const Metrics afterOpen = metrics(base, caPath);
    require(afterOpen.ranges == before.ranges + 1 &&
            afterOpen.bytes - before.bytes == kChunkBytes,
            "open fetched more than the first chunk");
    verifyBytes(source, 0, 80);
    verifyBytes(source, kChunkBytes - 20, 80);
    verifyBytes(source, size - 128, 128);
    for (unsigned index = 0; index < 10; ++index)
        verifyBytes(source, uint64_t(index) * kChunkBytes + 25, 33);
    require(metrics(base, caPath).ossConnections == before.ossConnections + 1,
            "successive Range reads opened more than one TLS connection");
    require(mai_cloud_vod_cached_bytes(source) <= kCacheBytes,
            "LRU cache exceeded its 8 MiB bound");
    unsigned char byte = 0;
    size_t received = 100;
    require(mai_cloud_vod_read_at(source, size, &byte, 1,
                                  &received, &result) == 1 && received == 0,
            "EOF read failed");
    mai_cloud_vod_close(source);
}

void testProxyOptionReset(const std::string& base, const char* caPath,
                          uint64_t size) {
    const std::string url = base + "/oss/play?sig=valid";
    MaiCloudVodOptions options = optionsFor(url, caPath, size);
    ProxySwitch route;
    options.proxy_url_utf8 = nullptr;
    options.proxy_resolver = rotatingProxy;
    options.proxy_context = &route;
    const Metrics before = metrics(base, caPath);
    MaiCloudVodResult result{};
    MaiCloudVod* source = mai_cloud_vod_open(&options, &result);
    require(source != nullptr, "proxy-option fixture failed to open");
    verifyBytes(source, kChunkBytes + 5, 40);
    const Metrics after = metrics(base, caPath);
    require(route.calls >= 2 && after.ossAuthHeaders == before.ossAuthHeaders,
            "proxy credentials leaked to the signed OSS origin");
    mai_cloud_vod_close(source);
}

void testInvalidResponses(const std::string& base, const char* caPath,
                          uint64_t size) {
    MaiCloudVodResult result{};
    for (const char* route : {"ignore-range", "oversize"}) {
        const std::string url = base + "/oss/" + route + "?sig=valid";
        MaiCloudVodOptions options = optionsFor(url, caPath, size);
        require(mai_cloud_vod_open(&options, &result) == nullptr,
                "invalid first response was accepted");
        require(result.status == MAI_CLOUD_VOD_HTTP_ERROR ||
                result.status == MAI_CLOUD_VOD_TOO_LARGE,
                "invalid first response produced wrong error");
    }
    const std::string driftUrl = base + "/oss/etag-drift?sig=valid";
    MaiCloudVodOptions options = optionsFor(driftUrl, caPath, size);
    MaiCloudVod* source = mai_cloud_vod_open(&options, &result);
    require(source != nullptr, "ETag drift fixture failed to open");
    unsigned char byte = 0;
    size_t received = 0;
    require(mai_cloud_vod_read_at(source, kChunkBytes, &byte, 1,
                                  &received, &result) == 0 &&
            result.status == MAI_CLOUD_VOD_INVALID_RESPONSE,
            "ETag drift was accepted");
    mai_cloud_vod_close(source);
    const std::string badHost = "https://other.example.com/oss/play?sig=valid";
    options = optionsFor(badHost, caPath, size);
    require(mai_cloud_vod_open(&options, &result) == nullptr &&
            result.status == MAI_CLOUD_VOD_INVALID_ARGUMENT,
            "non-OSS host was accepted");
}

void testRefresh(const std::string& base, const char* caPath, uint64_t size) {
    MaiCloudVodResult result{};
    Refresh state{base + "/oss/play?sig=fresh", 0};
    const std::string expired = base + "/oss/expire-after-first?sig=old";
    MaiCloudVodOptions options = optionsFor(expired, caPath, size);
    options.refresh_url = refresh;
    options.refresh_context = &state;
    MaiCloudVod* source = mai_cloud_vod_open(&options, &result);
    require(source != nullptr, "expired-after-first fixture failed to open");
    verifyBytes(source, kChunkBytes + 5, 50);
    require(state.calls == 1, "signed URL not renewed exactly once on 403");
    mai_cloud_vod_close(source);
}

void testLargeSeek(const std::string& base, const char* caPath) {
    constexpr uint64_t size = 1'900'000'001;
    const std::string url = base + "/oss/large-virtual?sig=valid";
    MaiCloudVodOptions options = optionsFor(url, caPath, size);
    MaiCloudVodResult result{};
    MaiCloudVod* source = mai_cloud_vod_open(&options, &result);
    require(source && mai_cloud_vod_size(source) == size,
            "64-bit virtual source failed to open");
    verifyBytes(source, size - 97, 97);
    mai_cloud_vod_close(source);
}

void testSmallStrictRange(const std::string& base, const char* caPath) {
    constexpr uint64_t size = 1683;
    const std::string url = base + "/oss/reject-beyond?sig=valid";
    for (uint64_t expected : {size, uint64_t(0)}) {
        const Metrics before = metrics(base, caPath);
        MaiCloudVodOptions options = optionsFor(url, caPath, expected);
        MaiCloudVodResult result{};
        MaiCloudVod* source = mai_cloud_vod_open(&options, &result);
        require(source && result.status == MAI_CLOUD_VOD_OK &&
                mai_cloud_vod_size(source) == size,
                "small OSS object rejected an out-of-bounds first Range");
        verifyBytes(source, 0, size);
        const Metrics after = metrics(base, caPath);
        require(after.ranges - before.ranges == (expected ? 1u : 2u) &&
                after.bytes - before.bytes == size + (expected ? 0u : 1u),
                "small object requested extra bytes beyond EOF");
        mai_cloud_vod_close(source);
    }
}

void testCloudDrive(const std::string& base, const char* caPath,
                    uint64_t size) {
    const std::string auth = base + "/auth";
    MaiCloudDriveOptions account{};
    account.auth_base_url_utf8 = auth.c_str();
    account.account_token_utf8 = "local-token";
    account.ca_path_utf8 = caPath;
    account.proxy_url_utf8 = "";
    account.signed_upload_host_suffix_utf8 = "localhost";
    MaiCloudVodResult result{};
    MaiCloudVod* source = mai_cloud_vod_open_cloud_drive(
        &account, "123e4567-e89b-42d3-a456-426614174000", nullptr,
        nullptr, &result);
    require(source && result.status == MAI_CLOUD_VOD_OK &&
            mai_cloud_vod_size(source) == size,
            "shared cloud-drive play-link helper failed");
    verifyBytes(source, size - 17, 17);
    require(metrics(base, caPath).auth == 2,
            "shared helper did not renew the expired play link");
    mai_cloud_vod_close(source);
}

void testCancellation(const std::string& base, const char* caPath,
                      uint64_t size) {
    const std::string url = base + "/oss/slow?sig=valid";
    MaiCloudVodOptions options = optionsFor(url, caPath, size);
    MaiCloudVodResult result{};
    MaiCloudVod* source = mai_cloud_vod_open(&options, &result);
    require(source != nullptr, "slow fixture failed to open");
    MaiCloudVodResult readResult{};
    int completed = 1;
    std::thread worker([&] {
        unsigned char byte = 0;
        size_t received = 0;
        completed = mai_cloud_vod_read_at(source, kChunkBytes,
                                          &byte, 1, &received, &readResult);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    mai_cloud_vod_cancel(source);
    worker.join();
    require(completed == 0 && readResult.status == MAI_CLOUD_VOD_CANCELED,
            "active Range request did not cancel");
    mai_cloud_vod_close(source);
}

void testRestartableInterrupt(const std::string& base, const char* caPath,
                              uint64_t size) {
    const std::string url = base + "/oss/slow-once?sig=valid";
    MaiCloudVodOptions options = optionsFor(url, caPath, size);
    MaiCloudVodResult result{};
    MaiCloudVod* source = mai_cloud_vod_open(&options, &result);
    require(source != nullptr, "restartable fixture failed to open");
    const unsigned before = metrics(base, caPath).ranges;
    MaiCloudVodResult readResult{};
    int completed = 1;
    std::thread worker([&] {
        unsigned char byte = 0;
        size_t received = 0;
        completed = mai_cloud_vod_read_at(source, kChunkBytes,
                                          &byte, 1, &received, &readResult);
    });
    const auto waitUntil = std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    while (metrics(base, caPath).ranges == before &&
           std::chrono::steady_clock::now() < waitUntil)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    require(metrics(base, caPath).ranges > before,
            "slow range did not begin before interruption");
    const auto interruptedAt = std::chrono::steady_clock::now();
    mai_cloud_vod_interrupt_read(source);
    worker.join();
    const auto elapsed = std::chrono::steady_clock::now() - interruptedAt;
    if (completed != 0 || readResult.status != MAI_CLOUD_VOD_INTERRUPTED ||
        elapsed >= std::chrono::seconds(1))
        std::cerr << "range interrupt completed=" << completed
                  << " status=" << readResult.status << " elapsed_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
                  << '\n';
    require(completed == 0 && readResult.status == MAI_CLOUD_VOD_INTERRUPTED &&
            elapsed < std::chrono::seconds(1),
            "read interruption did not return promptly");
    verifyBytes(source, kChunkBytes + 5, 50);
    require(mai_cloud_vod_cached_bytes(source) <= kCacheBytes,
            "restart retained an unbounded partial chunk");
    mai_cloud_vod_close(source);
}

void testRenewalInterrupt(const std::string& base, const char* caPath,
                          uint64_t size) {
    const std::string auth = base + "/auth";
    MaiCloudDriveOptions account{};
    account.auth_base_url_utf8 = auth.c_str();
    account.account_token_utf8 = "local-token";
    account.ca_path_utf8 = caPath;
    account.proxy_url_utf8 = "";
    account.signed_upload_host_suffix_utf8 = "localhost";
    MaiCloudVodResult result{};
    MaiCloudVod* source = mai_cloud_vod_open_cloud_drive(
        &account, "223e4567-e89b-42d3-a456-426614174000", nullptr,
        nullptr, &result);
    require(source != nullptr, "renewal-interrupt fixture failed to open");
    MaiCloudVodResult readResult{};
    int completed = 1;
    std::thread worker([&] {
        unsigned char byte = 0;
        size_t received = 0;
        completed = mai_cloud_vod_read_at(source, kChunkBytes,
                                          &byte, 1, &received, &readResult);
    });
    const auto waitUntil = std::chrono::steady_clock::now() +
        std::chrono::seconds(3);
    while (metrics(base, caPath).renewAuth < 2 &&
           std::chrono::steady_clock::now() < waitUntil)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    require(metrics(base, caPath).renewAuth == 2,
            "renewal request did not begin before interruption");
    const auto interruptedAt = std::chrono::steady_clock::now();
    mai_cloud_vod_interrupt_read(source);
    worker.join();
    const auto elapsed = std::chrono::steady_clock::now() - interruptedAt;
    require(completed == 0 && readResult.status == MAI_CLOUD_VOD_INTERRUPTED &&
            elapsed < std::chrono::seconds(2),
            "signed-link renewal did not interrupt promptly");
    verifyBytes(source, kChunkBytes + 5, 50);
    require(metrics(base, caPath).renewAuth == 3,
            "next playback did not start a fresh renewal request");
    mai_cloud_vod_close(source);
}

}  // namespace

int main(int argc, char** argv) {
    require(argc == 4, "expected base, CA path and content size");
    const std::string base = argv[1];
    const uint64_t size = std::strtoull(argv[3], nullptr, 10);
    require(size > 10 * kChunkBytes, "fixture content too short");
    testCore(base, argv[2], size);
    testProxyOptionReset(base, argv[2], size);
    testInvalidResponses(base, argv[2], size);
    testRefresh(base, argv[2], size);
    testLargeSeek(base, argv[2]);
    testSmallStrictRange(base, argv[2]);
    testCloudDrive(base, argv[2], size);
    testCancellation(base, argv[2], size);
    testRestartableInterrupt(base, argv[2], size);
    testRenewalInterrupt(base, argv[2], size);
    std::cout << "VOD Range/seek/cache/renewal/cancel passed\n";
}
