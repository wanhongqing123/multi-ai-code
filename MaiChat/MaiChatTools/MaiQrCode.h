#pragma once

#include <string>

#include "MaiError.h"
#include "MaiFilePath.h"
#include "MaiTool.h"

// Encode UTF-8 text as a scannable PNG with a four-module quiet zone.
// The ordinary entry point and the Agent tool use this same implementation.
MaiResult<std::string> maiEncodeQrCodePng(const std::string& text);
MaiError maiGenerateQrCodePng(const std::string& text, const MaiFilePath& output);
void registerMaiQrCodeTools(MaiToolRegistry& tools);
