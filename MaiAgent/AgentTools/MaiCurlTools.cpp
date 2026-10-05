#include "MaiCurlTools.h"

#include <utility>

#include "MaiCurlCliTool.h"

void registerMaiCurlTools(MaiToolRegistry& registry, std::string caBundlePath) {
    registry.add(makeMaiCurlCliTool(std::move(caBundlePath)));
}
