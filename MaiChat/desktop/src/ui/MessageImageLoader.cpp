#include "diagnostics/PerformanceLog.h"
#include "ui/MessageImageLoader.h"

#if defined(MAICHAT_FFMPEG_IMAGE_DECODE)
#include "MaiImageDecode.h"
#endif

#include <QDebug>
#include <QElapsedTimer>
#include <QEvent>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QPixmap>
#include <QPixmapCache>
#include <QRunnable>

namespace {

constexpr int kCacheLimitKb = 32 * 1024;  // 32MB，与 Android / iOS 取同一个数
constexpr int kMaxDecodeThreads = 2;      // 一屏图片同时解码会把 CPU 抢光，反而更卡
constexpr qint64 kSlowDecodeMs = 50;      // 只有超过这个值才记一条，不逐张刷屏

class DecodeTask : public QRunnable {
public:
    DecodeTask(MessageImageLoader* owner, QString path, QSize target, QString key)
        : owner_(owner), path_(std::move(path)), target_(target), key_(std::move(key)), context_(RemoteDiagnostics::PerformanceLog::shared().context()) {
        setAutoDelete(true);
    }

    void run() override {
        QElapsedTimer timer;
        timer.start();
#if defined(MAICHAT_FFMPEG_IMAGE_DECODE)
        const QByteArray utf8Path = path_.toUtf8();
        const MaiImageDecodeResult decoded = maiImageDecodeFile(
            utf8Path.constData(), qMax(1, target_.width()), qMax(1, target_.height()));
        QImage image;
        const int decodeError = decoded.error_code;
        if (decoded.rgba) {
            image = QImage(decoded.rgba, decoded.width, decoded.height, decoded.stride,
                           QImage::Format_RGBA8888).copy();
            maiImageDecodeFree(decoded.rgba);
        }
#else
        const int decodeError = 0;
        QImageReader reader(path_);
        // Explicit Qt image requests use decoder-stage downsampling.
        QSize scaled = reader.size();
        if (scaled.isValid() && !scaled.isEmpty()) {
            scaled.scale(target_, Qt::KeepAspectRatio);
            reader.setScaledSize(scaled);
        }
        QImage image = reader.read();
#endif
        const qint64 elapsed = timer.elapsed();
        if (!owner_) return;
        QMetaObject::invokeMethod(owner_, "deliver", Qt::QueuedConnection,
                                  Q_ARG(QString, key_), Q_ARG(QImage, image),
                                  Q_ARG(qint64, elapsed), Q_ARG(QString, context_),
                                  Q_ARG(int, decodeError));
    }

private:
    QPointer<MessageImageLoader> owner_;
    QString path_;
    QSize target_;
    QString key_;
    QString context_;
};


}  // namespace

MessageImageLoader& MessageImageLoader::instance() {
    static MessageImageLoader loader;
    return loader;
}

MessageImageLoader::MessageImageLoader(QObject* parent) : QObject(parent) {
    pool_.setMaxThreadCount(kMaxDecodeThreads);
    QPixmapCache::setCacheLimit(kCacheLimitKb);
}

QString MessageImageLoader::cacheKey(const QString& path, const QSize& targetPixels) {
    // 目标尺寸必须进键：气泡缩略图和全屏预览是同一文件的两个解码结果，
    // 共用一个键会让先到的把另一个顶掉。
    // 文件大小与修改时间也进键：同一路径的内容可能变（下载完成后覆盖、
    // 同一条消息重新接收），只按路径缓存会让界面一直贴着旧图，且没人会去清它。
    const QFileInfo info(path);
    return QStringLiteral("%1|%2x%3|%4@%5")
        .arg(path)
        .arg(targetPixels.width())
        .arg(targetPixels.height())
        .arg(info.size())
        .arg(info.lastModified().toMSecsSinceEpoch());
}

void MessageImageLoader::loadInto(const QString& path, const QSize& targetPixels, QLabel* label,
                                  const std::function<void()>& onMissing) {
    if (!label) return;
    QPointer<QLabel> guard(label);
    load(path, targetPixels, label, [guard](const QPixmap& pixmap) {
        if (guard) guard->setPixmap(pixmap);
    }, onMissing);
}

void MessageImageLoader::load(const QString& path, const QSize& targetPixels, QWidget* owner,
                              const std::function<void(const QPixmap&)>& onReady,
                              const std::function<void()>& onMissing) {
    if (!owner) return;
    const QString key = cacheKey(path, targetPixels);
    // 每次请求都先更新身份，包括缓存命中和文件缺失。否则旧的后台任务仍会认为
    // 控件在等旧 key，并在稍后把新缓存图或缺失占位覆盖掉。
    owner->setProperty("pendingImageKey", key);
    const QFileInfo info(path);
    if (path.trimmed().isEmpty() || !info.isFile()) {
        if (onMissing) onMissing();
        return;
    }

    QPixmap cached;
    if (QPixmapCache::find(key, &cached) && !cached.isNull()) {
        if (onReady) onReady(cached);
        return;
    }

    auto& queue = waiting_[key];
    queue.append(Pending{QPointer<QWidget>(owner), onReady, onMissing});
    if (queue.size() > 1) return;  // 已有同键任务在解，等它的结果

    pool_.start(new DecodeTask(this, path, targetPixels, key));
}

void MessageImageLoader::deliver(const QString& key, const QImage& image, qint64 elapsedMs,
                                 const QString& context, int decodeError) {
    if (context == RemoteDiagnostics::PerformanceLog::shared().context())
        RemoteDiagnostics::PerformanceLog::shared().record("image-decode", elapsedMs);
    const QVector<Pending> targets = waiting_.take(key);
    if (image.isNull()) {
        qWarning().noquote()
            << QStringLiteral("[ui] image decode failed: key=%1 ffmpeg_error=%2")
                   .arg(key).arg(decodeError);
        for (const Pending& pending : targets) {
            QWidget* owner = pending.owner.data();
            if (!owner || owner->property("pendingImageKey").toString() != key) continue;
            if (pending.onMissing) pending.onMissing();
        }
        return;
    }
    if (elapsedMs >= kSlowDecodeMs) {
        // 只记慢的那些：逐张记会把日志淹掉，反而看不见真正的问题。
        qInfo().noquote() << QStringLiteral("[ui] slow image decode: %1ms key=%2")
                                 .arg(elapsedMs)
                                 .arg(key);
    }

    const QPixmap pixmap = QPixmap::fromImage(image);
    QPixmapCache::insert(key, pixmap);
    for (const Pending& pending : targets) {
        QWidget* owner = pending.owner.data();
        if (!owner) continue;
        // 身份校验：控件还活着不等于它还在等这张图。
        if (owner->property("pendingImageKey").toString() != key) continue;
        if (pending.onReady) pending.onReady(pixmap);
    }
}
