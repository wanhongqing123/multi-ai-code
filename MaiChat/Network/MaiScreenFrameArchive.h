#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Synchronous, local-only API. Call on a worker thread. workspace must already
// exist and owner_session_id must be a nonempty, bounded UTF-8 owner key. Owner
// keys are hashed into paths; draft:UUID and remote room keys are supported.
//
// request_json.action: begin, append, finish, list, get, discard, retain,
// release, assign_session, release_session.
// append also takes batch_id,width,height,captured_at_ms,elapsed_ms and PNG bytes.
// finish takes batch_id and optional reason. list optionally takes batch_id.
// get takes batch_id,frame_id. retain/release take batch_id,frame_id,reference_id.
// retain optionally takes usage_session_id (defaults to owner_session_id).
// assign_session takes the same exact reference and a required usage_session_id.
// Reference IDs are independent, caller-generated bounded UTF-8 tokens. retain
// is idempotent for the same token and usage; release removes only that token.
// A draft-to-history transition assigns the same token to the actual session;
// it must not release it. References are private and never included in list/get.
// release_session has no other request fields: its owner_session_id is the
// deleted actual session, and only references with that usage_session_id are
// released across the workspace. It is an ordinary lifecycle API, not a tool.
// discard takes batch_id, deleting unreferenced PNGs only. Protected originals
// retain their paths in a hidden retained batch and remain available by get.
// Releasing its last token deletes a PNG only after its batch was discarded.
// Repeated release is harmless, including after its batch has been removed.
// All returned frame paths are relative to workspace, never arbitrary inputs.
//
// Result: {ok:true,output:{...}} or {ok:false,errorCode,error,...}.
// rate_limited is retryable; limit has terminal:true and limit_reason. Accepted
// append can have output.limit_reached:true. Limits: 2 seconds between frames,
// 300 frames/256 MiB per batch, 512 MiB per workspace, 20 MB per PNG. No old
// frames are evicted. finish retains frames. Selection/history retain the
// original PNG without copying it; cancellation/removal releases only its own
// token. Retained PNGs still count toward workspace capacity. A crash between
// retain and attachment persistence conservatively leaves a pin; this API does
// not infer unused references or run a garbage-collection daemon.
// Returns an owned UTF-8 JSON string (NULL only on allocation failure).
char* mai_screen_frame_archive_request(const char* workspace,
    const char* owner_session_id, const char* request_json,
    const void* png, size_t png_size);
void mai_screen_frame_archive_free(char* response_json);

#ifdef __cplusplus
}
#endif
