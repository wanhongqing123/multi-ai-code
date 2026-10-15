// 文件作用：桌面端远程桌面连接与控制；CaptureGeometry 负责几何尺寸和坐标转换，本文件给出其实现。
// 主要入口：isValid。
// 实现边界：与 CaptureGeometry 的公开声明配套，调用方通过相应接口进入。
#include "remote/CaptureGeometry.h"

namespace RemoteDesktop {

bool CaptureGeometry::isValid() const {
    const int sourceWidth = sourceSize.width();
    const int sourceHeight = sourceSize.height();
    const int captureX = captureRect.x();
    const int captureY = captureRect.y();
    const int captureWidth = captureRect.width();
    const int captureHeight = captureRect.height();

    if (sourceWidth <= 0 || sourceHeight <= 0
        || sourceWidth > kMaxCaptureGeometryDimension
        || sourceHeight > kMaxCaptureGeometryDimension) {
        return false;
    }
    if (captureX < 0 || captureY < 0 || captureWidth <= 0 || captureHeight <= 0
        || captureWidth > kMaxCaptureGeometryDimension
        || captureHeight > kMaxCaptureGeometryDimension) {
        return false;
    }
    // 用 64 位相加，避免恶意 JSON 在 x + width 处绕回。
    if (static_cast<qint64>(captureX) + captureWidth > sourceWidth
        || static_cast<qint64>(captureY) + captureHeight > sourceHeight) {
        return false;
    }
    return contentMode == CaptureContentMode::Fit && revision > 0
        && revision <= kMaxCaptureGeometryRevision;
}

QString captureContentModeName(CaptureContentMode mode) {
    switch (mode) {
        case CaptureContentMode::Fit: return QStringLiteral("fit");
        case CaptureContentMode::Unknown: break;
    }
    return QStringLiteral("unknown");
}

QString describeCaptureGeometry(const CaptureGeometry& geometry) {
    return QStringLiteral("source=%1x%2 capture=(%3,%4 %5x%6) mode=%7 revision=%8")
        .arg(geometry.sourceSize.width())
        .arg(geometry.sourceSize.height())
        .arg(geometry.captureRect.x())
        .arg(geometry.captureRect.y())
        .arg(geometry.captureRect.width())
        .arg(geometry.captureRect.height())
        .arg(captureContentModeName(geometry.contentMode))
        .arg(geometry.revision);
}

}  // namespace RemoteDesktop
