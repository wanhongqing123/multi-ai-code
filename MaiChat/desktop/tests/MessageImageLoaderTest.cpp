#include <QtTest>

#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QTemporaryDir>

#include "ui/MessageImageLoader.h"

class MessageImageLoaderTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void decodesDownToTheRequestedSizeInsteadOfFullResolution();
    void secondRequestForTheSameSizeIsServedFromCache();
    void differentTargetSizesDoNotShareOneCacheEntry();
    void reportsMissingFileInsteadOfHangingOnThePlaceholder();
    void cacheKeyChangesWhenTheFileContentChanges();
    void cachedReplacementRejectsOlderAsyncResult();

private:
    QTemporaryDir dir_;
    QString largePath_;
};

void MessageImageLoaderTest::initTestCase() {
    QVERIFY(dir_.isValid());
    // 造一张远大于气泡的图：整图解码和降采样解码的区别只有在这种尺寸下才显得出来。
    QImage source(2400, 1800, QImage::Format_RGB32);
    QPainter painter(&source);
    for (int y = 0; y < source.height(); y += 30) {
        painter.fillRect(0, y, source.width(), 15, QColor(y % 255, 90, 160));
    }
    painter.end();
    largePath_ = dir_.filePath(QStringLiteral("large.jpg"));
    QVERIFY(source.save(largePath_, "JPG", 88));
}

// 这条是本次改动的要害：结果必须是按目标尺寸解出来的，而不是解完整图再缩。
void MessageImageLoaderTest::decodesDownToTheRequestedSizeInsteadOfFullResolution() {
    QLabel label;
    const QSize target(240, 180);
    MessageImageLoader::instance().loadInto(largePath_, target, &label);
    QTRY_VERIFY(label.pixmap() != nullptr && !label.pixmap()->isNull());
    QVERIFY2(label.pixmap()->width() <= target.width(),
             "解出来的图不该比请求的目标还宽——那说明没有按目标尺寸降采样");
    QVERIFY2(label.pixmap()->height() <= target.height(),
             "解出来的图不该比请求的目标还高");
    // 也不能降过头：整体尺寸应当仍接近目标，否则贴上去是糊的。
    QVERIFY(label.pixmap()->width() >= target.width() / 2);
}

// 缓存被去掉的话这条会挂：第二次请求就不再是同步命中了。
void MessageImageLoaderTest::secondRequestForTheSameSizeIsServedFromCache() {
    const QSize target(200, 150);
    QLabel warmUp;
    MessageImageLoader::instance().loadInto(largePath_, target, &warmUp);
    QTRY_VERIFY(warmUp.pixmap() != nullptr && !warmUp.pixmap()->isNull());

    QLabel second;
    MessageImageLoader::instance().loadInto(largePath_, target, &second);
    // 不跑事件循环：命中缓存时必须当场贴好。
    QVERIFY2(second.pixmap() != nullptr && !second.pixmap()->isNull(),
             "同尺寸的第二次请求应当直接命中缓存并同步贴图");
}

// 气泡缩略图和全屏预览是同一文件的两个解码结果，共用一个键会互相顶掉。
void MessageImageLoaderTest::differentTargetSizesDoNotShareOneCacheEntry() {
    QLabel small;
    QLabel big;
    MessageImageLoader::instance().loadInto(largePath_, QSize(120, 90), &small);
    QTRY_VERIFY(small.pixmap() != nullptr && !small.pixmap()->isNull());
    MessageImageLoader::instance().loadInto(largePath_, QSize(800, 600), &big);
    QTRY_VERIFY(big.pixmap() != nullptr && !big.pixmap()->isNull());

    QVERIFY2(big.pixmap()->width() > small.pixmap()->width(),
             "大尺寸请求拿到的不该是小缩略图——说明缓存键没带目标尺寸");
}

void MessageImageLoaderTest::reportsMissingFileInsteadOfHangingOnThePlaceholder() {
    QLabel label;
    bool missing = false;
    MessageImageLoader::instance().loadInto(
        dir_.filePath(QStringLiteral("not-there.jpg")), QSize(200, 150), &label,
        [&missing] { missing = true; });
    QVERIFY2(missing, "文件不存在时必须回调，否则界面会一直停在占位上");
}

// 同一路径的内容可能变（下载完成后覆盖、同一条消息重新接收）。
// 键里不带文件指纹的话，界面会一直贴着那张旧图。
void MessageImageLoaderTest::cacheKeyChangesWhenTheFileContentChanges() {
    const QString path = dir_.filePath(QStringLiteral("changing.jpg"));
    QImage first(100, 100, QImage::Format_RGB32);
    first.fill(Qt::red);
    QVERIFY(first.save(path, "JPG", 90));
    const QString before = MessageImageLoader::cacheKey(path, QSize(50, 50));

    QTest::qWait(1100);  // 让修改时间确实变化（文件系统的时间精度可能是秒级）
    QImage second(400, 400, QImage::Format_RGB32);
    second.fill(Qt::blue);
    QVERIFY(second.save(path, "JPG", 90));
    const QString after = MessageImageLoader::cacheKey(path, QSize(50, 50));

    QVERIFY2(before != after, "同一路径换了内容之后，缓存键必须跟着变");
}

// 先发一个大图异步请求，随即用已经缓存的小图替换同一个控件。缓存命中也必须更新
// pendingImageKey，否则大图稍后回来会把已经贴好的新图覆盖掉。
void MessageImageLoaderTest::cachedReplacementRejectsOlderAsyncResult() {
    const QSize replacementSize(91, 67);
    QLabel warmUp;
    MessageImageLoader::instance().loadInto(largePath_, replacementSize, &warmUp);
    QTRY_VERIFY(warmUp.pixmap() != nullptr && !warmUp.pixmap()->isNull());

    QLabel target;
    MessageImageLoader::instance().loadInto(largePath_, QSize(777, 583), &target);
    MessageImageLoader::instance().loadInto(largePath_, replacementSize, &target);
    QVERIFY(target.pixmap() != nullptr && !target.pixmap()->isNull());
    QVERIFY(target.pixmap()->width() <= replacementSize.width());

    QTest::qWait(500);
    QVERIFY2(target.pixmap() != nullptr && target.pixmap()->width() <= replacementSize.width(),
             "较早的异步结果不能覆盖后来同步命中的缓存图");
}

QTEST_MAIN(MessageImageLoaderTest)
#include "MessageImageLoaderTest.moc"
