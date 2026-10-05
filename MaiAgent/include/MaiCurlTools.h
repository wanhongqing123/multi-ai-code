#pragma once

#include <string>

#include "MaiTool.h"

// Register one model-facing curl command backed by the embedded upstream CLI. Specialized
// libcurl API helpers remain available for internal provider use but are not registered tools.
void registerMaiCurlTools(MaiToolRegistry& registry, std::string caBundlePath = {});
