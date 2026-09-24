#include "MaiScreenshot.h"

bool maiIsScreenshotSupported() {
    return false;
}

MaiError maiCaptureScreenshot(const MaiScreenshotRequest&, MaiScreenshot& screenshot) {
    screenshot = MaiScreenshot{};
    return MaiError::make(MaiErrorCode::NotSupported,
                          "native screen capture is not supported on this platform");
}

MaiError maiListCaptureWindows(std::vector<std::string>& titles) {
    titles.clear();
    return MaiError::make(MaiErrorCode::NotSupported,
                          "native window enumeration is not supported on this platform");
}
