#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Synchronous, local-only API. Call on a worker thread. workspace must already
// exist and owner_session_id must be a nonempty, bounded UTF-8 owner key. Owner
// keys are hashed into paths; draft:UUID and remote room keys are supported.
//
// request_json.action: begin, append, finish, list, get, discard.
// append also takes batch_id,width,height,captured_at_ms,elapsed_ms and PNG bytes.
// finish takes batch_id and optional reason. list optionally takes batch_id.
// get takes batch_id,frame_id. discard takes batch_id and is the only deletion.
// All returned frame paths are relative to workspace, never arbitrary inputs.
//
// Result: {ok:true,output:{...}} or {ok:false,errorCode,error,...}.
// rate_limited is retryable; limit has terminal:true and limit_reason. Accepted
// append can have output.limit_reached:true. Limits: 2 seconds between frames,
// 300 frames/256 MiB per batch, 512 MiB per workspace, 20 MB per PNG. No old
// frames are evicted. finish retains frames. A selected frame must be copied
// to the session screenshots area before it becomes a persistent attachment.
// Returns an owned UTF-8 JSON string (NULL only on allocation failure).
char* mai_screen_frame_archive_request(const char* workspace,
    const char* owner_session_id, const char* request_json,
    const void* png, size_t png_size);
void mai_screen_frame_archive_free(char* response_json);

#ifdef __cplusplus
}
#endif
