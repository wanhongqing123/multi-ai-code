#pragma once

class MaiToolRegistry;

// Register desktop image-analysis tools backed by the shared OpenCV C API.
// Models are loaded from Qt resources once per Agent instance.
void registerDesktopVisionTools(MaiToolRegistry& registry);
