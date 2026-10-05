#pragma once

#include <memory>
#include <string>

#include "MaiTool.h"

// Sends one bounded HTTP(S) request and returns status, selected response headers, timing,
// and a bounded body. This is a model tool, not an HTTP server or a replacement for curl_fetch.
// Every call requires explicit approval because methods, headers, and bodies may transmit
// workspace data. The CA bundle is a host-provided path used only for TLS verification; empty
// selects libcurl's platform default. The tool is safe to share across worker threads.
std::unique_ptr<MaiTool> makeMaiHttpRequestTool(std::string caBundlePath = {});
