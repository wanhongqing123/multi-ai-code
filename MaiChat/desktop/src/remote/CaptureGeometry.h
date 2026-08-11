#pragma once

#include <QRect>
#include <QSize>
#include <QString>

namespace RemoteDesktop {

// 采集内容进入编码帧时的布局方式。当前桌面共享只使用 Fit：保持采集区域
// 宽高比，编码画布比例不同时在短边补黑。Unknown 只用于兼容没有几何信息的
// 老信令，绝不能据此猜测黑边。
enum class CaptureContentMode {
    Unknown,
    Fit,
};

// 被控端采集坐标系。captureRect 使用 sourceSize 内的像素坐标；远端输入最终
// 必须归一化到 sourceSize，而不是编码帧，否则区域采集或编码补边都会产生偏移。
struct CaptureGeometry {
    QSize sourceSize;
    QRect captureRect;
    CaptureContentMode contentMode = CaptureContentMode::Unknown;
    quint64 revision = 0;

    bool isValid() const;
};

// 限制来自 IM 的不可信几何，既避免 QRect 整数溢出，也避免把明显不可能的
// 超大尺寸带进后续浮点换算。单边 65535 仍远高于常见多屏桌面。
constexpr int kMaxCaptureGeometryDimension = 65535;
constexpr quint64 kMaxCaptureGeometryRevision = 2147483647ULL;

QString captureContentModeName(CaptureContentMode mode);
QString describeCaptureGeometry(const CaptureGeometry& geometry);

}  // namespace RemoteDesktop
