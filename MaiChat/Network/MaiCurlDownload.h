#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MaiDownloadCancel MaiDownloadCancel;

typedef enum MaiDownloadStatus {
    MAI_DOWNLOAD_OK = 0,
    MAI_DOWNLOAD_INVALID_ARGUMENT = 1,
    MAI_DOWNLOAD_NETWORK_ERROR = 2,
    MAI_DOWNLOAD_HTTP_ERROR = 3,
    MAI_DOWNLOAD_TOO_LARGE = 4,
    MAI_DOWNLOAD_WRITE_ERROR = 5,
    MAI_DOWNLOAD_CANCELED = 6,
    MAI_DOWNLOAD_EMPTY_BODY = 7
} MaiDownloadStatus;

typedef struct MaiDownloadProxy {
    const char* url_utf8;       // Null: environment proxy; empty: direct.
    const char* username_utf8;
    const char* password_utf8;
} MaiDownloadProxy;

// Return 1 and populate output, or 0 to fail the transfer. Output strings
// must remain valid until the next callback invocation, when they may be
// replaced. Called for the original URL
// and again for every HTTPS redirect, so platform PAC rules can be reapplied.
typedef int (*MaiDownloadProxyResolver)(const char* url_utf8,
                                        void* context,
                                        MaiDownloadProxy* output);

// All strings are UTF-8 and borrowed for the duration of mai_download_file.
// Pass either ca_pem/ca_pem_length or ca_path_utf8, or neither for libcurl's
// platform trust store. A null proxy_url_utf8 uses libcurl's environment proxy;
// an empty string forces a direct connection. The optional proxy credentials
// are separate from the proxy URL so signed media URLs are never rewritten.
typedef struct MaiDownloadOptions {
    const char* url_utf8;
    const char* target_path_utf8;
    uint64_t max_bytes;
    const char* ca_pem;
    size_t ca_pem_length;
    const char* ca_path_utf8;
    const char* proxy_url_utf8;
    const char* proxy_username_utf8;
    const char* proxy_password_utf8;
    int connect_timeout_ms;   // <= 0 selects 10 seconds.
    int transfer_timeout_ms;  // <= 0 selects 2 or 10 minutes by size.
    MaiDownloadProxyResolver proxy_resolver;
    void* proxy_context;
} MaiDownloadOptions;

typedef struct MaiDownloadResult {
    MaiDownloadStatus status;
    long http_status;
    int curl_code;
    uint64_t bytes;
} MaiDownloadResult;

MaiDownloadCancel* mai_download_cancel_create(void);
void mai_download_cancel_request(MaiDownloadCancel* cancel);
// One handle may cancel several concurrent transfers. Destroy it only after
// every mai_download_file call using it has returned.
void mai_download_cancel_destroy(MaiDownloadCancel* cancel);

// Synchronous. Run on a background thread. A successful call atomically
// publishes target_path_utf8; every failure removes only its private temp file.
// The final file is never truncated or replaced by a failed download.
void mai_download_file(const MaiDownloadOptions* options,
                       const MaiDownloadCancel* cancel,
                       MaiDownloadResult* result);

#ifdef __cplusplus
}
#endif
