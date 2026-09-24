#pragma once

#include <memory>

#include "MaiTool.h"

// Creates the desktop-only screen capture tool. Each execution requires user
// approval, captures the display under the pointer on the Qt GUI thread, and
// returns a PNG attachment to the model.
std::unique_ptr<MaiTool> makeDesktopScreenshotTool();
