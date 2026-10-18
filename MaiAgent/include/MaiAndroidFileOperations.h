// Android host callback for publishing a completed file through Java file APIs.
#pragma once

#if defined(__ANDROID__)

// The callback receives existing source and absent destination paths as native UTF-8 bytes.
// It may run on any MaiAgent worker thread and must block until publication finishes.
// Return 0 on success or an errno value on failure. A failure must leave the source in place;
// an existing destination must be rejected. The Android Java implementation serializes calls
// within this process and is for App-private paths only. Java Files.move does not guarantee
// no-replace against another process or any writer that bypasses this callback. The host
// registers before creating an agent and keeps the callback valid for the process lifetime.
using MaiAndroidPublishNewFileCallback = int (*)(const char* source, const char* destination);

void maiSetAndroidPublishNewFileCallback(MaiAndroidPublishNewFileCallback callback);

// Invokes the host callback or returns ENOSYS if Android has not installed its Java bridge.
int maiAndroidPublishNewFile(const char* source, const char* destination);

#endif
