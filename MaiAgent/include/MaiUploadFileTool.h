#pragma once

#include <memory>
#include <string>

#include "MaiTool.h"

// Stream a bounded workspace file as the raw body of an HTTP(S) PUT or POST request. The file
// remains unchanged. Each call needs user approval because it sends local bytes to a remote host.
// Multipart forms and provider credentials are intentionally outside this tool's contract.
std::unique_ptr<MaiTool> makeMaiUploadFileTool(std::string caBundlePath = {});
