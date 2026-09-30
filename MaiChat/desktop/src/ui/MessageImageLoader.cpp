#include "diagnostics/PerformanceLog.h"
#include "ui/MessageImageLoader.h"

#if defined(MAICHAT_HAS_GRAPHICS) && defined(Q_OS_MAC)
#include "MaiGraphicsPresenter.h"
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHash>
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
        QImageReader reader(path_);
        // 失败回退使用 Qt 的解码阶段降采样，不经过 Graphics 回读。
        QSize scaled = reader.size();
        if (scaled.isValid() && !scaled.isEmpty()) {
            scaled.scale(target_, Qt::KeepAspectRatio);
            reader.setScaledSize(scaled);
        }
        QImage image = reader.read();
        const qint64 elapsed = timer.elapsed();
        if (!owner_) return;
        QMetaObject::invokeMethod(owner_, "deliver", Qt::QueuedConnection,
                                  Q_ARG(QString, key_), Q_ARG(QImage, image),
                                  Q_ARG(qint64, elapsed), Q_ARG(QString, context_));
    }

private:
    QPointer<MessageImageLoader> owner_;
    QString path_;
    QSize target_;
    QString key_;
    QString context_;
};

#if defined(MAICHAT_HAS_GRAPHICS) && defined(Q_OS_MAC)
extern "C" void* maiGraphicsCreateMacLayer(QWidget* widget);
extern "C" void maiGraphicsResizeMacLayer(void* layer, QWidget* widget);
extern "C" void maiGraphicsRetainMacLayer(void* layer);
extern "C" void maiGraphicsReleaseMacLayer(void* layer);

class MaiGraphicsImageOverlay;
QHash<uint64_t, QPointer<MaiGraphicsImageOverlay>> graphicsOverlays;

void imagePresented(uint64_t viewId, bool success, void*);

bool startGraphicsPresenter() {
    static bool attempted = false;
    static bool ready = false;
    if (attempted) return ready;
    attempted = true;
    if (QGuiApplication::platformName() != QStringLiteral("cocoa")) return false;
    const QByteArray backendOverride = qgetenv("MAICHAT_GRAPHICS_BACKEND_PATH");
    const QByteArray effectsOverride = qgetenv("MAICHAT_GRAPHICS_EFFECT_DIRECTORY");
    const QString appDirectory = QCoreApplication::applicationDirPath();
    const QString backend = backendOverride.isEmpty()
        ? QDir(appDirectory).filePath(QStringLiteral("../Frameworks/libmaiagent_obs_metal.so"))
        : QFile::decodeName(backendOverride);
    const QString effects = effectsOverride.isEmpty()
        ? QDir(appDirectory).filePath(QStringLiteral("../Resources/MaiAgentGraphics"))
        : QFile::decodeName(effectsOverride);
    if (!QFileInfo::exists(backend) ||
        !QFileInfo::exists(QDir(effects).filePath(QStringLiteral("default.effect"))))
        return false;
    const QByteArray backendBytes = QFile::encodeName(backend);
    const QByteArray effectsBytes = QFile::encodeName(effects);
    ready = maiGraphicsPresenterStart(backendBytes.constData(), effectsBytes.constData(),
                                     imagePresented, nullptr);
    if (ready) {
        QObject::connect(qApp, &QCoreApplication::aboutToQuit, qApp,
                         [] { maiGraphicsPresenterStop(); });
    }
    return ready;
}

class MaiGraphicsImageOverlay final : public QWidget {
public:
    MaiGraphicsImageOverlay(QLabel* label, std::function<void()> onFailure)
        : QWidget(label), label_(label), onFailure_(std::move(onFailure)) {
        setObjectName(QStringLiteral("maiGraphicsImageOverlay"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setGeometry(label->rect());
        label->installEventFilter(this);
        show();
    }

    ~MaiGraphicsImageOverlay() override {
        if (label_) label_->removeEventFilter(this);
        if (viewId_) {
            graphicsOverlays.remove(viewId_);
            maiGraphicsPresenterDetach(viewId_);
        }
        if (nativeLayer_) maiGraphicsReleaseMacLayer(nativeLayer_);
    }

    bool render(const QString& path) {
        nativeLayer_ = maiGraphicsCreateMacLayer(this);
        if (!nativeLayer_) return false;
        const qreal ratio = devicePixelRatioF();
        const uint32_t width = qMax(1, qRound(this->width() * ratio));
        const uint32_t height = qMax(1, qRound(this->height() * ratio));
        viewId_ = maiGraphicsPresenterAttach(nativeLayer_, width, height,
                                             maiGraphicsRetainMacLayer,
                                             maiGraphicsReleaseMacLayer);
        if (!viewId_) return false;
        graphicsOverlays.insert(viewId_, this);
        const QByteArray pathBytes = QFile::encodeName(path);
        return maiGraphicsPresenterShowImage(viewId_, pathBytes.constData(), false);
    }

    void presented(bool success) {
        if (success) {
            setProperty("graphicsPresented", true);
            return;
        }
        if (onFailure_) onFailure_();
        deleteLater();
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == label_ && event->type() == QEvent::Resize) {
            setGeometry(label_->rect());
            updateSurfaceSize();
        }
        return QWidget::eventFilter(watched, event);
    }

    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        updateSurfaceSize();
    }

private:
    void updateSurfaceSize() {
        if (!viewId_ || !nativeLayer_) return;
        maiGraphicsResizeMacLayer(nativeLayer_, this);
        const qreal ratio = devicePixelRatioF();
        maiGraphicsPresenterResize(viewId_, qMax(1, qRound(width() * ratio)),
                                   qMax(1, qRound(height() * ratio)));
    }

    QPointer<QLabel> label_;
    std::function<void()> onFailure_;
    void* nativeLayer_ = nullptr;
    uint64_t viewId_ = 0;
};

void imagePresented(uint64_t viewId, bool success, void*) {
    QMetaObject::invokeMethod(QCoreApplication::instance(), [viewId, success] {
        const auto overlay = graphicsOverlays.value(viewId);
        if (overlay) overlay->presented(success);
    }, Qt::QueuedConnection);
}
#endif

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
#if defined(MAICHAT_HAS_GRAPHICS) && defined(Q_OS_MAC)
    if (QFileInfo(path).isFile() && startGraphicsPresenter()) {
        auto fallback = [this, path, targetPixels, guard, onMissing] {
            if (!guard) return;
            load(path, targetPixels, guard, [guard](const QPixmap& pixmap) {
                if (guard) guard->setPixmap(pixmap);
            }, onMissing);
        };
        auto* overlay = new MaiGraphicsImageOverlay(label, fallback);
        if (overlay->render(path)) return;
        delete overlay;
    }
#endif
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

void MessageImageLoader::deliver(const QString& key, const QImage& image, qint64 elapsedMs, const QString& context) {
    if (context == RemoteDiagnostics::PerformanceLog::shared().context())
        RemoteDiagnostics::PerformanceLog::shared().record("image-decode", elapsedMs);
    const QVector<Pending> targets = waiting_.take(key);
    if (image.isNull()) {
        qWarning().noquote()
            << QStringLiteral("[ui] image decode failed: key=%1 <- file unreadable or unsupported")
                   .arg(key);
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
