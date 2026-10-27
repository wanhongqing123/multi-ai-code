#pragma once

#include <functional>
#include <string>

#include "MaiTool.h"

// The handler borrows arguments only for the duration of execute(). It must
// route to the account-private MaiCloudDriveClient path used by the regular UI;
// it must not return account tokens, signed URLs, or local checkpoint paths.
// Upload/resume return accepted before completion; inspect list/pending later.
// Pause may return while cancellation is in progress.
using MaiCloudDriveToolHandler = std::function<MaiToolResult(const std::string& name, const std::string& argumentsJson,
                                                             const MaiToolContext& context)>;

// Registers the same Agent-facing contract on iOS, Android, and Desktop.
// Upload, resume, abort, and delete require approval on every call. Pause
// preserves confirmed parts and needs no approval. Registration performs no
// network call. Session attachment open is read-only and binds the current
// Agent Session ID before calling the platform's ordinary download core.
void registerMaiCloudDriveTools(MaiToolRegistry& tools, MaiCloudDriveToolHandler handler);
