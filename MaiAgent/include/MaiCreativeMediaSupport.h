#pragma once

#include <optional>
#include <string>

#include "MaiTool.h"

// Internal transport helpers for model-backed media tools. Hosts must call these only on a worker
// thread. The bearer key stays inside the HTTP request and is never placed in returned text.
struct MaiCreativeHttpResult {
    std::string body;
    std::optional<MaiToolResult> error;
};

MaiToolResult maiCreativeFailure(MaiErrorCode error, const char* code, const std::string& message);
MaiToolResult maiCreativeInvalid(const std::string& message);
bool maiCreativeValidId(const std::string& id);

// A null body sends GET; a non-null body sends JSON POST. Responses are bounded to 2 MB.
MaiCreativeHttpResult maiCreativeRequestJson(const std::string& url, const std::string& key,
                                             const std::string& caBundle, const std::string* body,
                                             const MaiToolContext& context);

// Upload an in-memory file as multipart/form-data. The caller validates the workspace path and
// file size before reading it. This helper does not persist or expose the bearer key.
MaiCreativeHttpResult maiCreativeUploadFile(const std::string& url, const std::string& key,
                                            const std::string& caBundle,
                                            const std::string& filename, const std::string& bytes,
                                            const std::string& purpose,
                                            const MaiToolContext& context);

// Legacy APIs accept PNG/JPEG raw Base64 or data URLs. The candidate must resolve inside the
// Agent workspace and remain under 5 MB. The original file is never modified.
std::optional<MaiToolResult> maiCreativeReadImage(const std::string& candidate,
                                                  const MaiToolContext& context, bool dataUrl,
                                                  std::string& encoded);

// Download to a new workspace path, validate its media signature, and publish atomically.
MaiToolResult maiCreativeDownloadMedia(const std::string& url, bool video,
                                       const std::string& provider, const MaiToolContext& context,
                                       const std::string& caBundle);
