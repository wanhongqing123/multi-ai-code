#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MaiVideoCircleMarkCancel MaiVideoCircleMarkCancel;

typedef enum MaiVideoCircleMarkStatus {
    MAI_VIDEO_CIRCLE_MARK_OK = 0,
    MAI_VIDEO_CIRCLE_MARK_INVALID_ARGUMENT = 1,
    MAI_VIDEO_CIRCLE_MARK_INVALID_SOURCE = 2,
    MAI_VIDEO_CIRCLE_MARK_UNSUPPORTED_MP4 = 3,
    MAI_VIDEO_CIRCLE_MARK_SIZE_LIMIT = 4,
    MAI_VIDEO_CIRCLE_MARK_DURATION_LIMIT = 5,
    MAI_VIDEO_CIRCLE_MARK_CANCELED = 6,
    MAI_VIDEO_CIRCLE_MARK_IO_ERROR = 7,
    MAI_VIDEO_CIRCLE_MARK_TRANSCODE_ERROR = 8,
    MAI_VIDEO_CIRCLE_MARK_VERIFICATION_ERROR = 9
} MaiVideoCircleMarkStatus;

// Filled by the synchronous preparation call. On success the caller-provided
// target path is the new, complete MP4; no allocated strings need releasing.
// The two verification flags mean that this process checked the output MP4's
// metadata and decoded a video frame containing the badge's blue anchor.
// These checks are consistency evidence, not a cryptographic proof that the
// input was AI-generated. The source task and human moderation provide that
// separate provenance decision. sha256_hex is lowercase and NUL terminated.
typedef struct MaiVideoCircleMarkResult {
    MaiVideoCircleMarkStatus status;
    uint64_t size_bytes;
    int64_t duration_ms;
    int visible_mark_verified;
    int metadata_mark_verified;
    char sha256_hex[65];
    // Bounded English diagnostic that never contains local paths or prompts.
    char detail_utf8[256];
} MaiVideoCircleMarkResult;

MaiVideoCircleMarkCancel* mai_video_circle_mark_cancel_create(void);
void mai_video_circle_mark_cancel_request(MaiVideoCircleMarkCancel* cancel);
// Destroy only after the synchronous preparation call has returned.
void mai_video_circle_mark_cancel_destroy(MaiVideoCircleMarkCancel* cancel);

// Worker-thread operation. Source and target are distinct absolute local MP4
// paths in caller-accessible storage. Target's parent must exist and target
// must not exist. The source file is never modified. The output is transcoded
// with a visible AI badge and MP4 metadata; it is committed atomically only
// after probing duration/metadata and decoding a marked frame. On failure or
// cancellation this call removes only its own temporary files. Output must
// be <= 200,000,000 bytes and 1..60,000 ms, matching the publication service.
// The cancel handle may be null. Other threads may request cancellation, but
// must not destroy the handle until this call returns. The embedded FFmpeg
// command is serialized with Agent FFmpeg tools because it has global state.
void mai_video_circle_prepare_marked_mp4(const char* source_absolute_path_utf8, const char* target_absolute_path_utf8,
                                         const MaiVideoCircleMarkCancel* cancel, MaiVideoCircleMarkResult* result);

#ifdef __cplusplus
}
#endif
