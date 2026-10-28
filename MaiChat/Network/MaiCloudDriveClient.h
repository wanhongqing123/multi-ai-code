#pragma once

#include "MaiCurlDownload.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MaiCloudDriveCancel MaiCloudDriveCancel;

typedef enum MaiCloudDriveAction {
    MAI_CLOUD_DRIVE_LIST_VIDEOS = 1,
    MAI_CLOUD_DRIVE_PLAY_LINK = 2,
    MAI_CLOUD_DRIVE_DELETE_VIDEO = 3,
    MAI_CLOUD_DRIVE_ABORT_UPLOAD = 4,
    MAI_CLOUD_DRIVE_THUMBNAIL_LINK = 5,
    MAI_SESSION_ATTACHMENT_CLEANUP_ROOM = 6,
    MAI_CLOUD_DRIVE_LIST_FOLDERS = 7,
    MAI_CLOUD_DRIVE_CREATE_FOLDER = 8,
    MAI_CLOUD_DRIVE_UPDATE_FOLDER = 9,
    MAI_CLOUD_DRIVE_DELETE_FOLDER = 10,
    MAI_CLOUD_DRIVE_MOVE_MEDIA = 11,
    // The account API retains its /videos transport path and video_id field.
    // These names also cover images without changing existing callers.
    MAI_CLOUD_DRIVE_LIST_MEDIA = MAI_CLOUD_DRIVE_LIST_VIDEOS,
    MAI_CLOUD_DRIVE_READ_LINK = MAI_CLOUD_DRIVE_PLAY_LINK,
    MAI_CLOUD_DRIVE_DELETE_MEDIA = MAI_CLOUD_DRIVE_DELETE_VIDEO
} MaiCloudDriveAction;

typedef struct MaiCloudDriveMediaInfo {
    // Both strings have static lifetime. The caller must not free them.
    const char* content_type_utf8;
    const char* media_type_utf8; // "image" or "video".
    uint64_t max_size_bytes;
} MaiCloudDriveMediaInfo;

// Classifies a display filename using the same extension and MIME rules as
// upload_resumable. Returns 1 on success, 0 for an unsupported/unsafe name.
// This common mapping avoids separate format tables in the platform hosts.
int mai_cloud_drive_media_info(const char* display_name_utf8,
                               MaiCloudDriveMediaInfo* output);
// Validates UTF-8, controls, leading/trailing Unicode whitespace and portable
// name restrictions for a logical folder (not an on-disk path). The account
// service performs NFC normalization and checks the normalized byte length.
int mai_cloud_drive_folder_name_valid(const char* name_utf8);
// Session attachments use the same multipart core but a separate API, quota
// and OSS object prefix. Files with a safe extension use octet-stream.
int mai_session_attachment_media_info(const char* display_name_utf8,
                                      MaiCloudDriveMediaInfo* output);

typedef enum MaiCloudDriveStatus {
    MAI_CLOUD_DRIVE_OK = 0,
    MAI_CLOUD_DRIVE_INVALID_ARGUMENT = 1,
    MAI_CLOUD_DRIVE_NETWORK_ERROR = 2,
    MAI_CLOUD_DRIVE_HTTP_ERROR = 3,
    MAI_CLOUD_DRIVE_RESPONSE_TOO_LARGE = 4,
    MAI_CLOUD_DRIVE_FILE_ERROR = 5,
    MAI_CLOUD_DRIVE_CANCELED = 6,
    MAI_CLOUD_DRIVE_INVALID_RESPONSE = 7,
    MAI_CLOUD_DRIVE_CHECKPOINT_MISMATCH = 8,
    MAI_CLOUD_DRIVE_PART_MISMATCH = 9,
    MAI_CLOUD_DRIVE_SOURCE_NOT_SEEKABLE = 10,
    MAI_CLOUD_DRIVE_INSUFFICIENT_SPACE = 11
} MaiCloudDriveStatus;

// All pointers are borrowed for each synchronous call. Run calls on a worker
// thread. Base URL may include a reverse-proxy prefix but must use HTTPS.
// Supply the account token without "Bearer ". Null proxy means environment;
// empty proxy forces direct. The host suffix defaults to aliyuncs.com; only
// local TLS tests should set it to localhost.
typedef struct MaiCloudDriveOptions {
    const char* auth_base_url_utf8;
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
    int transfer_timeout_ms; // <= 0 selects 30 seconds metadata, 200 seconds
                             // for upload completion, 10 minutes per part.
    const char* signed_upload_host_suffix_utf8;
    // iOS-owned staged media needs a durable completion receipt before its
    // source is removed. Zero preserves the normal delete-on-success behavior.
    int retain_completed_checkpoint;
    // Set all three for the independent Session attachment space. Null values
    // keep existing private cloud-drive behavior.
    const char* owner_device_slot_utf8;
    const char* owner_login_session_id_utf8;
    const char* session_id_utf8;
} MaiCloudDriveOptions;

typedef struct MaiCloudDriveResult {
    MaiCloudDriveStatus status;
    long http_status;
    int curl_code;
    // Owned UTF-8 JSON for successful API calls; bounded error body otherwise.
    char* response_json_utf8;
    size_t response_bytes;
} MaiCloudDriveResult;

// Called on the worker thread. Return nonzero to cancel. After a restart the
// first call includes bytes already confirmed by server ListParts.
typedef int (*MaiCloudDriveProgress)(uint64_t uploaded_bytes,
                                     uint64_t total_bytes,
                                     void* context);

// Must fill all requested bytes at the given offset and return 1. Return -1
// when the source cannot seek, 0 for other read errors. Android SAF can use a
// persistently permitted content URI as stable_id_utf8 and pread on a reopened
// ParcelFileDescriptor. The source stays open until the synchronous call ends.
// A non-seekable provider must be staged to a durable private file by the host.
typedef int (*MaiCloudDriveReadAt)(void* context, uint64_t offset,
                                   unsigned char* output, size_t requested);

typedef struct MaiCloudDriveSource {
    const char* stable_id_utf8; // Durable host identity, not sent to server.
    uint64_t size_bytes;
    MaiCloudDriveReadAt read_at;
    void* context;
} MaiCloudDriveSource;

// For a non-seekable platform provider only: fill up to capacity bytes, set
// bytes_read (zero at EOF), and return 1. Return 0 for a read error. The host
// supplies an already authorized provider stream; this core handles durable
// atomic staging, size bounds, free-space check, and cancellation.
typedef int (*MaiCloudDriveReadNext)(void* context, unsigned char* output,
                                     size_t capacity, size_t* bytes_read);

MaiCloudDriveCancel* mai_cloud_drive_cancel_create(void);
void mai_cloud_drive_cancel_request(MaiCloudDriveCancel* cancel);
// Destroy only after every call using the handle has returned.
void mai_cloud_drive_cancel_destroy(MaiCloudDriveCancel* cancel);

// LIST_MEDIA accepts optional {"before":"cursor","folder_id":"uuid"}.
// Omit folder_id for all media; an empty folder_id selects the root directory.
// LIST_FOLDERS returns the account's entire folder tree. CREATE_FOLDER accepts
// {"name":...,"parent_id":...}; UPDATE_FOLDER accepts folder_id and name and/or
// parent_id. Empty parent_id means root. DELETE_FOLDER accepts folder_id and
// refuses nonempty folders. MOVE_MEDIA accepts {"media_id":...,"folder_id":...}
// (empty folder_id moves to root). Folder actions never alter OSS object keys.
// The remaining media actions need
// {"video_id":"uuid"}. THUMBNAIL_LINK returns a short-lived signed HTTPS
// thumbnail_url. ABORT_UPLOAD discards an unfinished upload session;
// canceling upload_resumable instead keeps its checkpoint for a later resume.
void mai_cloud_drive_request(const MaiCloudDriveOptions* options,
                             MaiCloudDriveAction action,
                             const char* payload_json_utf8,
                             const MaiCloudDriveCancel* cancel,
                             MaiCloudDriveResult* result);

// Uploads MP4/MOV up to 2,000,000,000 bytes, or JPEG/PNG/WebP/HEIC up to
// 20,000,000 bytes, through the same OSS multipart transport.
// The client hashes the whole file, stores an atomic checkpoint at the
// caller-provided private path, asks the account server for authoritative
// ListParts, and PUTs only missing parts with a fresh signed URL and MD5.
// Cancel/network failure keeps the checkpoint. A successful complete removes
// it unless retain_completed_checkpoint is set; then it atomically records
// completed=true in the checkpoint before returning success, and the host
// removes it after cleaning its owned source and task record. The caller must
// serialize calls sharing a checkpoint path and keep that
// path in app-private storage. Neither video bytes nor OSS credentials reach
// the account server. Use a stable <task-id>.maicloud.json path under an
// app-private, per-account directory. This path convenience wrapper opens a
// regular file; use the source variant below for a seekable SAF/Photos handle.
// Completion JSON is returned in result.
void mai_cloud_drive_upload_resumable(const MaiCloudDriveOptions* options,
                                      const char* file_path_utf8,
                                      const char* checkpoint_path_utf8,
                                      const char* display_name_utf8,
                                      const char* content_type_utf8,
                                      const MaiCloudDriveCancel* cancel,
                                      MaiCloudDriveProgress progress,
                                      void* progress_context,
                                      MaiCloudDriveResult* result);

// Same implementation with a seekable host source. On relaunch enumerate the
// checkpoint, reopen the source from stable_id_utf8, and supply the same ID.
// This avoids copying a 2 GB Android SAF video when its provider supports
// random reads and the URI permission is persisted by the host.
void mai_cloud_drive_upload_resumable_source(
    const MaiCloudDriveOptions* options,
    const MaiCloudDriveSource* source,
    const char* checkpoint_path_utf8,
    const char* display_name_utf8,
    const char* content_type_utf8,
    const MaiCloudDriveCancel* cancel,
    MaiCloudDriveProgress progress,
    void* progress_context,
    MaiCloudDriveResult* result);

// Fallback for non-seekable SAF/Photos streams. target_path_utf8 must be a new
// absolute path in app-private storage, with an existing parent directory.
// Requires expected bytes plus 64 MiB available before starting; successful
// exact-size copy is fsynced and atomically committed. Failure removes only
// its temporary file. Call upload_resumable on the resulting private path;
// that call applies the MIME-specific limit to staged images.
void mai_cloud_drive_stage_stream(const char* target_path_utf8,
                                  uint64_t expected_bytes,
                                  MaiCloudDriveReadNext read_next,
                                  void* read_context,
                                  const MaiCloudDriveCancel* cancel,
                                  MaiCloudDriveProgress progress,
                                  void* progress_context,
                                  MaiCloudDriveResult* result);

// Enumerates only *.maicloud.json files immediately inside the caller's
// per-account private directory. Returns {"uploads":[{"checkpoint_path":...,
// "source_id":...,"video_id":...,"name":...,"content_type":...,
// "size_bytes":...}],"invalid_count":N}. No bearer, signature, or signed
// URL is ever stored in the checkpoint. The host chooses the directory and
// may reopen a persisted URI/bookmark or present a missing source for selection.
void mai_cloud_drive_list_local_checkpoints(const char* directory_utf8,
                                            MaiCloudDriveResult* result);

// Explicitly discards an unfinished upload: DELETE the account-owned upload
// session, then remove its local checkpoint after a successful server reply.
// Use cancel for a pause that should remain resumable.
void mai_cloud_drive_abort_resumable(const MaiCloudDriveOptions* options,
                                     const char* checkpoint_path_utf8,
                                     const MaiCloudDriveCancel* cancel,
                                     MaiCloudDriveResult* result);

void mai_cloud_drive_result_free(MaiCloudDriveResult* result);

#ifdef __cplusplus
}
#endif
