# Shared attachment downloads

`MaiCurlDownload.cpp` is the single C++ transfer implementation for MaiChat IM
attachments. Desktop, iOS, and Android supply their own media URL, cache path,
certificate bundle, proxy selection, and account lifetime; they call the same
blocking C ABI in `MaiCurlDownload.h` from a background thread. The optional
`MaiCurlDownload.hpp` gives C++ callers RAII cancellation.

The caller chooses `max_bytes` by media type (the current product limits are
32 MiB for images, 64 MiB for voice, 512 MiB for files, 1 GiB for video, and
16 MiB for covers). The core accepts only HTTPS URLs, checks every redirect,
verifies the server certificate and host name, reapplies the platform proxy
resolver at each hop, limits the streamed body, and writes a private sibling
temp file. A complete HTTP 200 response atomically replaces the target file;
errors and cancellation leave an existing target untouched.

`mai_download_file` is synchronous. A `MaiDownloadCancel` may be shared across
concurrent downloads for one account or operation. The caller must wait for all
those calls to return before destroying it. Account switch logic must also
check its own account generation before presenting a successful file.

Local TLS regression (certificate and key generated only inside the test run):

```sh
cmake -S MaiChat/Network -B /tmp/maichat-network-test -DMAICHAT_NETWORK_BUILD_TESTS=ON
cmake --build /tmp/maichat-network-test
ctest --test-dir /tmp/maichat-network-test --output-on-failure
```

## AI video circle client

`MaiVideoCircleClient.h` is one C ABI over the shared libcurl transport for
Desktop, iOS, and Android. Host code supplies its account token, the account
and media service HTTPS base URLs, CA certificate or system trust store, and
its proxy resolver. Calls are synchronous and belong on a worker thread.
`mai_video_circle_result_free` releases every response. A cancellation handle
can interrupt a transfer without changing the source file.

The fixed actions cover signed upload metadata, submitting a completed AI
video card, the public feed and author's statuses, a short playback link,
withdrawal, reports, blocking, the blocked-author list, and unblocking. The
pure `mai_video_circle_inspect_card` helper decides whether a completed card
can show the publish entry in all three UIs. The publish action accepts the
**completed `agent_send_media` card** again and extracts
`source_task_id` and `source_tool` from it. It sends neither local paths nor
prompts to the account service. Old cards without verified source fields cannot
be published. The client does not manufacture an AI provenance claim for an
arbitrary MP4; the reviewer still checks the marked file.

For a new publication, prepare a separate, visibly and metadata-marked MP4
without altering the generated original; ask `SIGN_UPLOAD` for a private OSS
URL; call `mai_video_circle_upload_file` with the exact signed JSON, marked
file, and byte size; then call `PUBLISH_DRAFT` with the completed card, title,
object key, size, and duration. The uploader validates the HTTPS host and
object path, preserves the signed query, sends only the signed OSS headers,
refuses redirects, and caps files at 200 MB. It never sends an account Bearer
to OSS or routes video bytes through the account server. For playback, request
`PLAY_LINK` and immediately download the 60-second signed URL through
`mai_download_file` to a bounded temporary cache before opening the existing
local video player. A hidden or withdrawn post no longer receives new links.

`MAICHAT_VIDEO_CIRCLE_ENABLED` remains off in production. The local TLS fixture
uses two disposable account tokens, a fake OSS endpoint, and an admin review
step; it does not use production credentials or upload real media.
