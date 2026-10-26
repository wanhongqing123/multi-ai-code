#pragma once

#include "MaiCurlDownload.h"
#include "MaiCloudDriveClient.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MaiCloudVod MaiCloudVod;
typedef struct MaiCloudVodCancel MaiCloudVodCancel;

typedef enum MaiCloudVodStatus {
    MAI_CLOUD_VOD_OK = 0,
    MAI_CLOUD_VOD_INVALID_ARGUMENT = 1,
    MAI_CLOUD_VOD_NETWORK_ERROR = 2,
    MAI_CLOUD_VOD_HTTP_ERROR = 3,
    MAI_CLOUD_VOD_INVALID_RESPONSE = 4,
    MAI_CLOUD_VOD_TOO_LARGE = 5,
    MAI_CLOUD_VOD_CANCELED = 6,
    MAI_CLOUD_VOD_INTERRUPTED = 7
} MaiCloudVodStatus;

typedef struct MaiCloudVodResult {
    MaiCloudVodStatus status;
    long http_status;
    int curl_code;
} MaiCloudVodResult;

// Called from the playback worker after an expired signed URL receives 403.
// Write a new null-terminated HTTPS URL into output, at most capacity bytes
// including the terminator, and return 1. Return 0 to report renewal failure.
// The host may make its account-authenticated play-link request here. Never
// pass the account token or signed URL to the media decoder or its argv.
typedef int (*MaiCloudVodRefreshUrl)(void* context, char* output,
                                     size_t capacity);

typedef struct MaiCloudVodOptions {
    const char* signed_url_utf8;
    // Defaults to aliyuncs.com. Only local TLS fixtures should override this
    // with localhost. Redirects and renewed URLs obey the same host rule.
    const char* signed_host_suffix_utf8;
    // 0 discovers the size from the first Content-Range. A nonzero value must
    // match the response and must not exceed 2,000,000,000 bytes.
    uint64_t expected_size_bytes;
    const char* ca_pem;
    size_t ca_pem_length;
    const char* ca_path_utf8;
    const char* proxy_url_utf8;
    const char* proxy_username_utf8;
    const char* proxy_password_utf8;
    MaiDownloadProxyResolver proxy_resolver;
    void* proxy_context;
    MaiCloudVodRefreshUrl refresh_url;
    void* refresh_context;
    // Borrowed until close. Allows cancellation while open is still fetching.
    const MaiCloudVodCancel* cancel;
    int connect_timeout_ms;  // <= 0 selects 10 seconds.
    int transfer_timeout_ms; // <= 0 selects 30 seconds per range.
} MaiCloudVodOptions;

MaiCloudVodCancel* mai_cloud_vod_cancel_create(void);
void mai_cloud_vod_cancel_request(MaiCloudVodCancel* cancel);
void mai_cloud_vod_cancel_destroy(MaiCloudVodCancel* cancel);

// Opens a seekable, read-only HTTP Range source. With a known size, the first
// range fetches up to 1 MiB; with unknown size, a 0-0 size probe comes first.
// Every range validates 206, Content-Range, object size and TLS.
// Data remains only in a bounded 8 MiB process-memory cache; no media file is
// written to disk. Run open and read_at on a playback worker, never the UI
// thread. The caller owns the returned session and closes it after reads stop.
MaiCloudVod* mai_cloud_vod_open(const MaiCloudVodOptions* options,
                               MaiCloudVodResult* result);

// Cloud-drive convenience entry point. Gets the account-owned play link,
// validates its video type and exact size, then opens the Range source.
// On signed-URL expiry/403 it fetches a fresh play link using the same account
// API; platform hosts need no separate renewal code. Strings in account_options
// are copied for the lifetime of the source. proxy_context and vod_cancel
// remain borrowed until close; account_cancel is borrowed only during open.
// Null cancel handles are allowed. Calling cancel(source) also interrupts an
// in-flight play-link renewal.
MaiCloudVod* mai_cloud_vod_open_cloud_drive(
    const MaiCloudDriveOptions* account_options,
    const char* video_id_utf8,
    const MaiCloudDriveCancel* account_cancel,
    const MaiCloudVodCancel* vod_cancel,
    MaiCloudVodResult* result);

// One playback thread may read a source at a time. Success returns 1 and sets
// bytes_read to up to capacity, or zero at EOF. Failure returns 0, clears
// bytes_read and fills result. Random reads support FFmpeg demuxer seeks.
int mai_cloud_vod_read_at(MaiCloudVod* source, uint64_t offset,
                          unsigned char* output, size_t capacity,
                          size_t* bytes_read, MaiCloudVodResult* result);

uint64_t mai_cloud_vod_size(const MaiCloudVod* source);
uint64_t mai_cloud_vod_cached_bytes(const MaiCloudVod* source);

// Interrupts the currently blocked Range read or signed-link renewal without
// closing the source or discarding cached chunks. A later read starts with a
// new generation and may reuse the source for replay or a speed change.
// Call before synchronously joining a playback worker; unlike cancel, this
// does not prevent future reads.
void mai_cloud_vod_interrupt_read(MaiCloudVod* source);

// May be called concurrently with open/read_at, including while libcurl is
// waiting for bytes. A source is destroyed only after its last read returns.
void mai_cloud_vod_cancel(MaiCloudVod* source);
void mai_cloud_vod_close(MaiCloudVod* source);

#ifdef __cplusplus
}
#endif
