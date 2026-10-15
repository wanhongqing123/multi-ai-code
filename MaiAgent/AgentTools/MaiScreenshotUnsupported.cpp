// 文件作用：为不支持截图的平台提供明确失败实现。
// 直接依赖：MaiScreenshot.h；本文件在这些接口之间完成调用与结果转换。
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
