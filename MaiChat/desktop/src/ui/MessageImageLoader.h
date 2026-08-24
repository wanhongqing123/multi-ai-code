#pragma once

#include <functional>

#include <QHash>
#include <QLabel>
#include <QPixmap>
#include <QWidget>
#include <QObject>
#include <QPointer>
#include <QSize>
#include <QString>
#include <QThreadPool>
#include <QVector>

/**
 * 消息图片 / 视频封面的加载。
 *
 * 为什么需要它：原先是在 createMessageBubble 里直接 QPixmap(path) 解全尺寸原图。
 * 实测 12MP 照片解码后再缩到 560x400 要 46.8ms，而按目标尺寸降采样只要 9.6ms；
 * 且没有缓存，界面每重建一次就重解一遍——十张图的会话切进去约 470ms 主线程停顿。
 *
 * 三端（Qt / Android / iOS）约定一致：按显示尺寸降采样、缓存键含目标尺寸与文件
 * 指纹、按字节限容 32MB、后台解码主线程贴图、同键并发只解一次、贴图前校验目标身份。
 */
class MessageImageLoader : public QObject {
    Q_OBJECT

public:
    static MessageImageLoader& instance();

    /**
     * 把 path 解成不超过 targetPixels 的图，回调交给调用方使用（GUI 线程）。
     * 已缓存则同步回调；否则后台解码，期间保持调用方摆好的占位。
     *
     * owner 既是生命周期守卫，也是身份标记：解码结果回来时会核对 owner 是否还在
     * 等这张图——增量渲染会替换气泡，迟到的结果不能贴到已经换成别条消息的控件上。
     */
    void load(const QString& path, const QSize& targetPixels, QWidget* owner,
              const std::function<void(const QPixmap&)>& onReady,
              const std::function<void()>& onMissing = {});

    /** 贴到 QLabel 的常见情形。 */
    void loadInto(const QString& path, const QSize& targetPixels, QLabel* label,
                  const std::function<void()>& onMissing = {});

    /** 缓存键：路径 + 目标像素 + 文件大小 + 修改时间。见 .cpp 里的说明。 */
    static QString cacheKey(const QString& path, const QSize& targetPixels);

private slots:
    void deliver(const QString& key, const QImage& image, qint64 elapsedMs);

private:
    explicit MessageImageLoader(QObject* parent = nullptr);

    struct Pending {
        QPointer<QWidget> owner;
        std::function<void(const QPixmap&)> onReady;
        std::function<void()> onMissing;
    };

    QThreadPool pool_;
    // 同一个键正在解码时，后来的请求排队等结果，不重复解。
    QHash<QString, QVector<Pending>> waiting_;
};
