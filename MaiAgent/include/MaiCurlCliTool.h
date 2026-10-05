#pragma once

#include <memory>
#include <string>

#include "MaiTool.h"

// Execute a bounded, non-interactive subset of the vendored curl CLI in the Agent worker thread.
// The tool validates all file paths and options before it calls the upstream argc/argv entry.
// Every invocation requires separate user approval because it may send local data externally.
std::unique_ptr<MaiTool> makeMaiCurlCliTool(std::string caBundlePath = {});
