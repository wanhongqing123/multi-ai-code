#pragma once

#include <memory>
#include <string>

#include "MaiError.h"

class MaiToolRegistry;

struct DesktopComputerObservation {
    std::string json;
    std::string imagePath;
};

// Session-owned observed window state. A later action must match the same
// window, title, geometry and snapshot token; the next observation verifies it.
class DesktopComputerController {
public:
    DesktopComputerController();
    ~DesktopComputerController();
    DesktopComputerController(const DesktopComputerController&) = delete;
    DesktopComputerController& operator=(const DesktopComputerController&) = delete;

    MaiResult<DesktopComputerObservation> observe(const std::string& sessionId,
        const std::string& windowTitle, const std::string& workspace,
        bool includeElements);
    MaiError act(const std::string& sessionId, const std::string& action,
        const std::string& snapshotId, std::uint64_t windowId,
        double xRatio, double yRatio, const std::string& text,
        const std::string& key, int scrollLines);
    MaiError clear(const std::string& sessionId);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void registerDesktopComputerTools(MaiToolRegistry& registry,
    std::shared_ptr<DesktopComputerController> controller);
