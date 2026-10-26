#include "MaiCurlDownload.h"
#include "MaiCurlDownload.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

std::string read(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void write(const std::string& path, const std::string& content) {
    std::ofstream file(path, std::ios::binary);
    file << content;
}

bool exists(const std::string& path) {
#ifdef _WIN32
    return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat details{};
    return stat(path.c_str(), &details) == 0;
#endif
}

struct ProxyCalls {
    std::vector<std::string> urls;
};

int resolveProxy(const char* url, void* context, MaiDownloadProxy* output) {
    auto* calls = static_cast<ProxyCalls*>(context);
    calls->urls.emplace_back(url);
    *output = {"", nullptr, nullptr};
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    const MaiDownloadOptions invalid{};
    require(mai_chat::downloadFile(invalid).status == MAI_DOWNLOAD_INVALID_ARGUMENT,
            "C++ wrapper failed to return invalid argument status");
    require(argc == 5, "expected base URL, CA PEM files and output directory");
    const std::string base = argv[1];
    const std::string certificate = read(argv[2]);
    const std::string unrelatedCertificate = read(argv[3]);
    const std::string directory = argv[4];
    require(!certificate.empty(), "missing test certificate");
    require(!unrelatedCertificate.empty(), "missing unrelated test certificate");
    const std::string target = directory + "/download.bin";
    MaiDownloadOptions options{};
    options.target_path_utf8 = target.c_str();
    options.max_bytes = 1024;
    options.ca_pem = certificate.data();
    options.ca_pem_length = certificate.size();
    options.proxy_url_utf8 = "";
    MaiDownloadResult result{};

    const std::string signedUrl = base + "/ok?token=a%2Fb&scope=x%20y";
    options.url_utf8 = signedUrl.c_str();
    mai_download_file(&options, nullptr, &result);
    require(result.status == MAI_DOWNLOAD_OK && result.http_status == 200,
            "trusted TLS success failed");
    require(read(target) == "/ok?token=a%2Fb&scope=x%20y", "signed query was changed");
    require(result.bytes == read(target).size(), "incorrect received byte count");

    options.ca_pem = nullptr;
    options.ca_pem_length = 0;
    options.ca_path_utf8 = argv[2];
    mai_download_file(&options, nullptr, &result);
    require(result.status == MAI_DOWNLOAD_OK, "trusted CA path failed");
    options.ca_path_utf8 = nullptr;
    options.ca_pem = certificate.data();
    options.ca_pem_length = certificate.size();

    ProxyCalls proxyCalls;
    const std::string redirectUrl = base + "/redirect";
    options.url_utf8 = redirectUrl.c_str();
    options.proxy_resolver = resolveProxy;
    options.proxy_context = &proxyCalls;
    mai_download_file(&options, nullptr, &result);
    require(result.status == MAI_DOWNLOAD_OK, "HTTPS redirect failed");
    require(read(target) == "/ok?token=a%2Fb&scope=x%20y", "redirect signed query changed");
    require(proxyCalls.urls.size() == 2 && proxyCalls.urls[0] == redirectUrl &&
            proxyCalls.urls[1] == signedUrl, "proxy callback not invoked for each hop");
    options.proxy_resolver = nullptr;
    options.proxy_context = nullptr;

    write(target, "old");
    const std::string badUrl = base + "/bad";
    options.url_utf8 = badUrl.c_str();
    mai_download_file(&options, nullptr, &result);
    require(result.status == MAI_DOWNLOAD_HTTP_ERROR && result.http_status == 404,
            "HTTP error status not reported");
    require(read(target) == "old", "HTTP failure changed existing file");

    const std::string insecureUrl = base + "/insecure";
    options.url_utf8 = insecureUrl.c_str();
    mai_download_file(&options, nullptr, &result);
    require(result.status == MAI_DOWNLOAD_HTTP_ERROR, "HTTP redirect accepted");
    require(read(target) == "old", "insecure redirect changed existing file");

    const std::string oversizedUrl = base + "/large";
    options.url_utf8 = oversizedUrl.c_str();
    options.max_bytes = 32;
    mai_download_file(&options, nullptr, &result);
    require(result.status == MAI_DOWNLOAD_TOO_LARGE, "stream size bound not enforced");
    require(read(target) == "old", "oversized body changed existing file");

    const std::string headerLargeUrl = base + "/header-large";
    options.url_utf8 = headerLargeUrl.c_str();
    mai_download_file(&options, nullptr, &result);
    require(result.status == MAI_DOWNLOAD_TOO_LARGE, "Content-Length bound not enforced");
    require(read(target) == "old", "oversized Content-Length changed existing file");

    options.max_bytes = 1024;
    options.ca_pem = unrelatedCertificate.data();
    options.ca_pem_length = unrelatedCertificate.size();
    options.url_utf8 = signedUrl.c_str();
    mai_download_file(&options, nullptr, &result);
    require(result.status == MAI_DOWNLOAD_NETWORK_ERROR,
            "server certificate signed by another CA was accepted");
    require(read(target) == "old", "TLS failure changed existing file");
    options.ca_pem = certificate.data();
    options.ca_pem_length = certificate.size();

    const std::string emptyUrl = base + "/empty";
    options.url_utf8 = emptyUrl.c_str();
    mai_download_file(&options, nullptr, &result);
    require(result.status == MAI_DOWNLOAD_EMPTY_BODY, "empty attachment accepted");
    require(read(target) == "old", "empty body changed existing file");

    const std::string slowUrl = base + "/slow";
    const std::string marker = directory + "/slow-started";
    options.url_utf8 = slowUrl.c_str();
    MaiDownloadCancel* cancel = mai_download_cancel_create();
    require(cancel != nullptr, "cancellation handle allocation failed");
    std::thread worker([&] { mai_download_file(&options, cancel, &result); });
    for (int i = 0; i < 100 && !exists(marker); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    require(exists(marker), "slow response never started");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    mai_download_cancel_request(cancel);
    worker.join();
    mai_download_cancel_destroy(cancel);
    require(result.status == MAI_DOWNLOAD_CANCELED, "in-flight cancel not reported");
    require(read(target) == "old", "canceled transfer changed existing file");
    std::cout << "local TLS downloader cases passed" << std::endl;
    return 0;
}
