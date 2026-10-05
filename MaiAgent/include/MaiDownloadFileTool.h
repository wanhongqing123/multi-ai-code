#pragma once

#include <memory>
#include <string>

#include "MaiTool.h"

// Downloads HTTP(S) bytes into a new file below the Agent workspace. The tool streams to a
// temporary file, enforces a byte limit during transfer, and publishes the file only after a
// complete successful response. Calls run on the Agent's blocking tool worker, not a UI thread.
// Errors are returned as JSON text with a stable code; no downloaded content is executed.
std::unique_ptr<MaiTool> makeMaiDownloadFileTool(std::string caBundlePath = {});
