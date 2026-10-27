#pragma once

#include <memory>
#include <string>

#include "MaiError.h"

class MaiToolRegistry;

// Owns an isolated, visible Chrome/Edge profile for each local Desktop Session.
// Ordinary App code and the Agent tool share these calls; no model-provided JS runs.
class DesktopBrowserManager {
public:
    DesktopBrowserManager();
    ~DesktopBrowserManager();
    DesktopBrowserManager(const DesktopBrowserManager&) = delete;
    DesktopBrowserManager& operator=(const DesktopBrowserManager&) = delete;

    MaiError open(const std::string& sessionId, const std::string& url);
    MaiResult<std::string> observe(const std::string& sessionId);
    MaiError act(const std::string& sessionId, const std::string& action,
                 const std::string& expectedUrl, const std::string& snapshotId,
                 const std::string& elementId, const std::string& expectedLabel,
                 const std::string& text);
    MaiError screenshot(const std::string& sessionId, const std::string& expectedUrl,
                        const std::string& outputPath);
    MaiError close(const std::string& sessionId);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void registerDesktopBrowserTools(MaiToolRegistry& registry,
    std::shared_ptr<DesktopBrowserManager> manager);
