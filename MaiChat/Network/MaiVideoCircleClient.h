#pragma once

#include "MaiCurlDownload.h"
#include "MaiFirstPartyRoute.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MaiVideoCircleCancel MaiVideoCircleCancel;

typedef enum MaiVideoCircleAction {
    MAI_VIDEO_CIRCLE_SIGN_UPLOAD = 1,
    MAI_VIDEO_CIRCLE_PUBLISH_DRAFT = 2,
    MAI_VIDEO_CIRCLE_FEED = 3,
    MAI_VIDEO_CIRCLE_MINE = 4,
    MAI_VIDEO_CIRCLE_PLAY_LINK = 5,
    MAI_VIDEO_CIRCLE_WITHDRAW = 6,
    MAI_VIDEO_CIRCLE_REPORT = 7,
    MAI_VIDEO_CIRCLE_BLOCK = 8,
    MAI_VIDEO_CIRCLE_UNBLOCK = 9,
    MAI_VIDEO_CIRCLE_BLOCKS_LIST = 10
} MaiVideoCircleAction;

typedef enum MaiVideoCircleStatus {
    MAI_VIDEO_CIRCLE_OK = 0,
    MAI_VIDEO_CIRCLE_INVALID_ARGUMENT = 1,
    MAI_VIDEO_CIRCLE_NETWORK_ERROR = 2,
    MAI_VIDEO_CIRCLE_HTTP_ERROR = 3,
    MAI_VIDEO_CIRCLE_RESPONSE_TOO_LARGE = 4,
    MAI_VIDEO_CIRCLE_FILE_ERROR = 5,
    MAI_VIDEO_CIRCLE_CANCELED = 6,
    MAI_VIDEO_CIRCLE_INVALID_RESPONSE = 7
} MaiVideoCircleStatus;

// All pointers are borrowed until the synchronous call returns. Base URLs may
// contain a reverse-proxy path prefix (for example /api or /maichat) but must
// use HTTPS. Supply the account token without "Bearer ". Null proxy URL uses
// environment proxy settings; empty string forces a direct connection.
typedef struct MaiVideoCircleOptions {
    const char* auth_base_url_utf8;
    const char* media_base_url_utf8;
    const char* account_token_utf8;
    const char* ca_pem;
    size_t ca_pem_length;
    const char* ca_path_utf8;
    const char* proxy_url_utf8;
    const char* proxy_username_utf8;
    const char* proxy_password_utf8;
    MaiDownloadProxyResolver proxy_resolver;
    void* proxy_context;
    int connect_timeout_ms;  // <= 0 selects 10 seconds.
    int transfer_timeout_ms; // <= 0 selects 30 seconds for metadata.
    // Null selects aliyuncs.com. Local TLS tests may set localhost.
    const char* signed_upload_host_suffix_utf8;
    // Optional route selection for account control requests only; media PUTs
    // to signed OSS URLs retain their independently selected system proxy.
    MaiFirstPartyRouteSelector first_party_route_selector;
    MaiFirstPartyRouteInvalidator first_party_route_invalidator;
    void* first_party_route_context;
} MaiVideoCircleOptions;

typedef struct MaiVideoCircleResult {
    MaiVideoCircleStatus status;
    long http_status;
    int curl_code;
    // Owned UTF-8 JSON; may contain a bounded server error body on HTTP_ERROR.
    char* response_json_utf8;
    size_t response_bytes;
} MaiVideoCircleResult;

MaiVideoCircleCancel* mai_video_circle_cancel_create(void);
void mai_video_circle_cancel_request(MaiVideoCircleCancel* cancel);
// Destroy only after every request using the handle has returned.
void mai_video_circle_cancel_destroy(MaiVideoCircleCancel* cancel);

// Pure local eligibility check for a complete tool part, not just its raw
// output: {"tool":"agent_send_media","state":"completed","output":{...}}
// The output may also be the original JSON string from the tool part. On
// success result.response_json_utf8 contains only path, source_tool and
// source_task_id. It is intended for button visibility; PUBLISH_DRAFT checks
// the same card again before creating a network request.
void mai_video_circle_inspect_card(const char* completed_card_json_utf8,
                                   MaiVideoCircleResult* result);

// Fixed routes and methods only. payload_json_utf8 is a JSON object (or null
// for FEED/MINE). Both FEED and MINE accept an optional {"before":"cursor"}
// payload for the next page and return {"posts":[...],"next_cursor":...}.
// PUBLISH_DRAFT requires a completed agent_send_media part:
// {"completed_card":{"tool":"agent_send_media","state":"completed",
//                    "output":{"delivery":"current_ai_session","type":"video",
//                              "mime_type":"video/mp4","path":"relative.mp4",
//                              "source_tool":"wan_video","source_task_id":"..."}},
//  "title":"...","object_key":"...","size_bytes":123,"duration_ms":8000}.
// The client derives source_tool/source_task_id from the completed card; they
// cannot be supplied as independent publishing fields. Run on a worker thread.
void mai_video_circle_request(const MaiVideoCircleOptions* options,
                              MaiVideoCircleAction action,
                              const char* payload_json_utf8,
                              const MaiVideoCircleCancel* cancel,
                              MaiVideoCircleResult* result);

// PUTs an already prepared MP4 directly to the exact signed HTTPS OSS URL.
// signed_upload_json_utf8 is the successful SIGN_UPLOAD response. No Bearer or
// cookie is sent, redirects are refused, and the source file is never changed.
// expected_bytes must equal the file size and be at most 200 MB.
void mai_video_circle_upload_file(const MaiVideoCircleOptions* options,
                                  const char* signed_upload_json_utf8,
                                  const char* file_path_utf8,
                                  uint64_t expected_bytes,
                                  const MaiVideoCircleCancel* cancel,
                                  MaiVideoCircleResult* result);

void mai_video_circle_result_free(MaiVideoCircleResult* result);

#ifdef __cplusplus
}
#endif
