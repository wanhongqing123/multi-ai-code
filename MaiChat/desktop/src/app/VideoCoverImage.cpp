#include "app/VideoCoverImage.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QPainter>
#include <QPainterPath>
#include <QtGlobal>
#include <QDebug>


#ifdef Q_OS_WIN
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#endif

namespace {

// 封面最长边。IM 各端气泡里只显示一张小图，1080 够用又不会让上传变慢。
constexpr int kCoverMaxEdge = 1080;
// 视频没给出画面尺寸时的兜底比例（16:9）。
constexpr int kFallbackCoverWidth = 640;
constexpr int kFallbackCoverHeight = 360;

QSize coverSizeFor(const VideoFileMetadata& metadata) {
    int width = metadata.width;
    int height = metadata.height;
    if (width <= 0 || height <= 0) {
        width = kFallbackCoverWidth;
        height = kFallbackCoverHeight;
    }
    const int longest = qMax(width, height);
    if (longest > kCoverMaxEdge) {
        const double scale = static_cast<double>(kCoverMaxEdge) / static_cast<double>(longest);
        width = qMax(1, static_cast<int>(width * scale));
        height = qMax(1, static_cast<int>(height * scale));
    }
    return QSize(width, height);
}

#ifdef Q_OS_WIN

// Windows 的缩略图是 HBITMAP。Qt5WinExtras 没有链进来，这里自己走 GetDIBits 拷一份
// 32 位 BGRA 出来。注意 DIB 默认是自底向上的，height 取负号才是自顶向下。
QImage imageFromHBitmap(HBITMAP bitmap) {
    if (!bitmap) return QImage();
    BITMAP info{};
    if (GetObjectW(bitmap, sizeof(info), &info) == 0) return QImage();
    if (info.bmWidth <= 0 || info.bmHeight <= 0) return QImage();

    BITMAPINFO header{};
    header.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    header.bmiHeader.biWidth = info.bmWidth;
    header.bmiHeader.biHeight = -info.bmHeight;
    header.bmiHeader.biPlanes = 1;
    header.bmiHeader.biBitCount = 32;
    header.bmiHeader.biCompression = BI_RGB;

    QImage image(info.bmWidth, info.bmHeight, QImage::Format_ARGB32);
    if (image.isNull()) return QImage();

    const HDC screen = GetDC(nullptr);
    if (!screen) return QImage();
    const int copied = GetDIBits(screen, bitmap, 0, static_cast<UINT>(info.bmHeight),
                                 image.bits(), &header, DIB_RGB_COLORS);
    ReleaseDC(nullptr, screen);
    if (copied == 0) return QImage();

    // 缩略图提供程序返回的 alpha 常常整片是 0（它们只填了 RGB）。直接用会得到一张
    // 全透明的图，落成 PNG 就是空白。这里统一按不透明处理。
    return image.convertToFormat(QImage::Format_RGB32);
}

class ComScope {
public:
    ComScope() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        // RPC_E_CHANGED_MODE 表示本线程已经按别的模型初始化过（TRTC 等 SDK 会做），
        // 那种情况下 COM 可用但不该由我们来反初始化。
        initialized_ = SUCCEEDED(hr);
    }
    ~ComScope() {
        if (initialized_) CoUninitialize();
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

private:
    bool initialized_ = false;
};

#endif  // Q_OS_WIN

}  // namespace

QImage loadSystemVideoThumbnail(const QString& videoPath, int maxEdgePixels) {
#ifdef Q_OS_WIN
    const QString nativePath = QDir::toNativeSeparators(QFileInfo(videoPath).absoluteFilePath());
    if (nativePath.isEmpty()) return QImage();

    ComScope com;
    IShellItem* item = nullptr;
    HRESULT hr = SHCreateItemFromParsingName(reinterpret_cast<const wchar_t*>(nativePath.utf16()),
                                             nullptr, IID_PPV_ARGS(&item));
    if (FAILED(hr) || !item) return QImage();

    IShellItemImageFactory* factory = nullptr;
    hr = item->QueryInterface(IID_PPV_ARGS(&factory));
    item->Release();
    if (FAILED(hr) || !factory) return QImage();

    HBITMAP bitmap = nullptr;
    const SIZE requested{maxEdgePixels, maxEdgePixels};
    // SIIGBF_THUMBNAILONLY 很关键：不加的话拿不到缩略图时系统会回退成一枚通用的
    // 文件类型图标，我们会把「一张 mp4 图标」当成首帧发出去。
    hr = factory->GetImage(requested, SIIGBF_BIGGERSIZEOK | SIIGBF_THUMBNAILONLY, &bitmap);
    factory->Release();
    if (FAILED(hr) || !bitmap) return QImage();

    const QImage image = imageFromHBitmap(bitmap);
    DeleteObject(bitmap);
    return image;
#else
    // macOS 上要走 QuickLook/AVFoundation，目前还没接；调用方会退到占位封面。
    Q_UNUSED(videoPath);
    Q_UNUSED(maxEdgePixels);
    return QImage();
#endif
}

QImage renderPlaceholderVideoCover(const VideoFileMetadata& metadata) {
    const QSize size = coverSizeFor(metadata);
    QImage image(size, QImage::Format_RGB32);
    if (image.isNull()) return image;
    image.fill(QColor(0x11, 0x18, 0x27));

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const double edge = qMin(size.width(), size.height()) * 0.28;
    const QPointF center(size.width() / 2.0, size.height() / 2.0);
    const double radius = edge * 0.95;

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 255, 255, 38));
    painter.drawEllipse(center, radius, radius);

    QPainterPath triangle;
    const double half = edge * 0.5;
    // 三角形按视觉重心右移一点，正对着圆心看着才是居中的。
    const double shift = half * 0.18;
    triangle.moveTo(center.x() - half * 0.55 + shift, center.y() - half);
    triangle.lineTo(center.x() - half * 0.55 + shift, center.y() + half);
    triangle.lineTo(center.x() + half * 0.85 + shift, center.y());
    triangle.closeSubpath();
    painter.setBrush(QColor(255, 255, 255, 220));
    painter.drawPath(triangle);
    painter.end();

    return image;
}

VideoCoverImage writeVideoCoverImage(const QImage& image, const QString& outputPathWithoutSuffix) {
    VideoCoverImage cover;
    if (image.isNull()) return cover;
    QDir().mkpath(QFileInfo(outputPathWithoutSuffix).absolutePath());

    // JPEG 体积小得多，优先用。Qt 的 JPEG 支持是插件，缺插件时 save 会失败——
    // 那就退回 PNG（内建，永远可用），并把 type 一并改掉，别让 SDK 收到对不上的后缀。
    const QString jpegPath = outputPathWithoutSuffix + QStringLiteral(".jpg");
    if (image.save(jpegPath, "JPEG", 85)) {
        cover.path = jpegPath;
        cover.type = QStringLiteral("jpg");
    } else {
        const QString pngPath = outputPathWithoutSuffix + QStringLiteral(".png");
        if (!image.save(pngPath, "PNG")) return cover;
        cover.path = pngPath;
        cover.type = QStringLiteral("png");
    }

    const QFileInfo info(cover.path);
    cover.sizeBytes = info.size();
    if (cover.sizeBytes <= 0) return cover;
    cover.width = image.width();
    cover.height = image.height();
    cover.valid = true;
    return cover;
}

VideoCoverImage createVideoCoverImage(const QString& videoPath,
                                      const VideoFileMetadata& metadata,
                                      const QString& outputDirectory) {
    const QString stem =
        QStringLiteral("cover-%1").arg(QDateTime::currentMSecsSinceEpoch());
    const QString outputPathWithoutSuffix = QDir(outputDirectory).filePath(stem);

    QImage image = loadSystemVideoThumbnail(videoPath, kCoverMaxEdge);
    bool fromSystem = !image.isNull();
    if (!fromSystem) image = renderPlaceholderVideoCover(metadata);

    const VideoCoverImage cover = writeVideoCoverImage(image, outputPathWithoutSuffix);
    // 封面是这条链路上最容易"没报错但结果不对"的一环（系统给不出缩略图、JPEG 插件
    // 缺失），把选到的来源和落盘结果都记下来，排障时不用猜。
    qInfo().noquote() << QStringLiteral("video cover: source=%1 size=%2x%3 type=%4 bytes=%5 ok=%6")
                             .arg(fromSystem ? QStringLiteral("system-thumbnail")
                                             : QStringLiteral("placeholder"))
                             .arg(cover.width)
                             .arg(cover.height)
                             .arg(cover.type)
                             .arg(cover.sizeBytes)
                             .arg(cover.valid ? 1 : 0);
    return cover;
}
