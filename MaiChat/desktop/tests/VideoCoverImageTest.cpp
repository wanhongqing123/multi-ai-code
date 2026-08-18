#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>
#include <QTest>

#include "app/VideoCoverImage.h"

class VideoCoverImageTest : public QObject {
    Q_OBJECT

private slots:
    void placeholderKeepsTheVideoAspectRatio();
    void placeholderFallsBackToSixteenByNineWithoutDimensions();
    void placeholderClampsOversizedVideos();
    void placeholderIsNotABlankFrame();
    void writesACoverWithEverySdkRequiredField();
    void createReturnsAUsableCoverEvenWithoutASystemThumbnail();
};

void VideoCoverImageTest::placeholderKeepsTheVideoAspectRatio() {
    VideoFileMetadata metadata;
    metadata.width = 1080;
    metadata.height = 1920;

    const QImage cover = renderPlaceholderVideoCover(metadata);

    // 最长边收到 1080：1080 * (1080/1920) = 607.5，向下取整 607。
    QCOMPARE(cover.height(), 1080);
    QCOMPARE(cover.width(), 607);
}

void VideoCoverImageTest::placeholderFallsBackToSixteenByNineWithoutDimensions() {
    // 容器解不出画面尺寸时也必须给出一张封面：SDK 的封面宽高是必填项，
    // 留 0 会让整条视频消息发不出去。
    const QImage cover = renderPlaceholderVideoCover(VideoFileMetadata{});

    QCOMPARE(cover.width(), 640);
    QCOMPARE(cover.height(), 360);
}

void VideoCoverImageTest::placeholderClampsOversizedVideos() {
    VideoFileMetadata metadata;
    metadata.width = 3840;
    metadata.height = 2160;

    const QImage cover = renderPlaceholderVideoCover(metadata);

    QCOMPARE(cover.width(), 1080);
    QCOMPARE(cover.height(), 607);
}

void VideoCoverImageTest::placeholderIsNotABlankFrame() {
    VideoFileMetadata metadata;
    metadata.width = 640;
    metadata.height = 360;

    const QImage cover = renderPlaceholderVideoCover(metadata);

    QVERIFY(!cover.isNull());
    // 中心画着播放三角，必须比四角的底色亮——否则等于发了一张纯色图出去。
    const QRgb center = cover.pixel(cover.width() / 2, cover.height() / 2);
    const QRgb corner = cover.pixel(2, 2);
    QVERIFY(qGray(center) > qGray(corner) + 40);
}

void VideoCoverImageTest::writesACoverWithEverySdkRequiredField() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    VideoFileMetadata metadata;
    metadata.width = 960;
    metadata.height = 540;
    const QImage image = renderPlaceholderVideoCover(metadata);

    const VideoCoverImage cover = writeVideoCoverImage(image, dir.filePath(QStringLiteral("cover")));

    QVERIFY(cover.valid);
    QVERIFY(QFileInfo::exists(cover.path));
    QVERIFY(cover.type == QStringLiteral("jpg") || cover.type == QStringLiteral("png"));
    // 落盘后缀必须与上报给 SDK 的 type 一致，不然对端按错的类型去解封面。
    QVERIFY(cover.path.endsWith(QStringLiteral(".") + cover.type));
    QCOMPARE(cover.sizeBytes, QFileInfo(cover.path).size());
    // 上报的宽高必须是真正落盘那张图的宽高，而不是原视频的——封面会被收敛到 1080
    // 最长边，两者并不总是相等。
    QCOMPARE(cover.width, image.width());
    QCOMPARE(cover.height, image.height());
    QCOMPARE(cover.width, 960);
}

void VideoCoverImageTest::createReturnsAUsableCoverEvenWithoutASystemThumbnail() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // 指向一个根本不存在的视频：系统缩略图必然拿不到，此时仍要退到占位封面，
    // 而不是返回一个 invalid 结果把整条消息拦下来。
    VideoFileMetadata metadata;
    metadata.width = 800;
    metadata.height = 600;

    const VideoCoverImage cover = createVideoCoverImage(
        dir.filePath(QStringLiteral("missing.mp4")), metadata, dir.path());

    QVERIFY(cover.valid);
    QVERIFY(QFileInfo::exists(cover.path));
    QCOMPARE(cover.width, 800);
    QCOMPARE(cover.height, 600);
    QVERIFY(cover.sizeBytes > 0);
}

QTEST_MAIN(VideoCoverImageTest)
#include "VideoCoverImageTest.moc"
