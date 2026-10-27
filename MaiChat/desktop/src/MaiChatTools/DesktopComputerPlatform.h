#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "MaiError.h"

struct DesktopNativeWindow {
    std::uint64_t id = 0;
    std::uint64_t processId = 0;
    std::string title;
    std::string application;
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
};

struct DesktopNativeElement {
    std::string role;
    std::string label;
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
};

MaiResult<std::vector<DesktopNativeWindow>> maiDesktopWindows();
MaiResult<std::vector<DesktopNativeElement>> maiDesktopElements(
    const DesktopNativeWindow& window);
MaiError maiDesktopActivateWindow(const DesktopNativeWindow& window);
MaiError maiDesktopClick(double x, double y);
MaiError maiDesktopType(const std::string& text);
MaiError maiDesktopKey(const std::string& key);
MaiError maiDesktopScroll(double x, double y, int lines);
MaiResult<bool> maiDesktopFocusedInputIsSecure();
