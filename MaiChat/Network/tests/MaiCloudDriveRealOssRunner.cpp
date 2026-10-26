#include "MaiCloudDriveClient.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

constexpr uint64_t kPartBytes = 8 * 1024 * 1024;

const char* required(const char* name) {
    const char* value = std::getenv(name);
    if (!value || !*value) {
        std::cerr << "Cloud acceptance runner is missing a setting\n";
        std::exit(2);
    }
    return value;
}

int stopAfterFirstPart(uint64_t uploaded, uint64_t total, void*) {
    if (total != 2'000'000'000) return 1;
    // The second PUT has begun, so the first part has completed at OSS.
    return uploaded > kPartBytes + 1024 ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2 || (std::string(argv[1]) != "pause" &&
                      std::string(argv[1]) != "resume")) {
        std::cerr << "Expected pause or resume phase\n";
        return 2;
    }
    const char* base = required("MAICHAT_CLOUD_ACCEPT_AUTH_BASE");
    const char* token = required("MAICHAT_CLOUD_ACCEPT_TOKEN");
    const char* ca = required("MAICHAT_CLOUD_ACCEPT_CA_BUNDLE");
    const char* source = required("MAICHAT_CLOUD_ACCEPT_SOURCE");
    const char* checkpoint = required("MAICHAT_CLOUD_ACCEPT_CHECKPOINT");
    MaiCloudDriveOptions options{};
    options.auth_base_url_utf8 = base;
    options.account_token_utf8 = token;
    options.ca_path_utf8 = ca;
    options.proxy_url_utf8 = "";
    options.connect_timeout_ms = 10'000;
    MaiCloudDriveResult result{};
    const bool pausing = std::string(argv[1]) == "pause";
    mai_cloud_drive_upload_resumable(&options, source, checkpoint,
        "acceptance-2gb.mp4", "video/mp4", nullptr,
        pausing ? stopAfterFirstPart : nullptr, nullptr, &result);
    const bool success = pausing ?
        result.status == MAI_CLOUD_DRIVE_CANCELED :
        result.status == MAI_CLOUD_DRIVE_OK && result.http_status == 200;
    // Never print a response body: it can contain a signed URL or token.
    std::cout << "Cloud client " << argv[1] << ": status=" << result.status
              << " http=" << result.http_status << " curl=" << result.curl_code
              << (success ? " PASS\n" : " FAIL\n");
    mai_cloud_drive_result_free(&result);
    return success ? 0 : 1;
}
