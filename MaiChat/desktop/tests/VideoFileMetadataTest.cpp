#include <QBuffer>
#include <QByteArray>
#include <QDataStream>
#include <QTemporaryDir>
#include <QTest>

#include "im/VideoFileMetadata.h"

namespace {

void appendU32(QByteArray& out, quint32 value) {
    out.append(static_cast<char>((value >> 24) & 0xFF));
    out.append(static_cast<char>((value >> 16) & 0xFF));
    out.append(static_cast<char>((value >> 8) & 0xFF));
    out.append(static_cast<char>(value & 0xFF));
}

void appendU64(QByteArray& out, quint64 value) {
    appendU32(out, static_cast<quint32>(value >> 32));
    appendU32(out, static_cast<quint32>(value & 0xFFFFFFFFull));
}

QByteArray box(const char* type, const QByteArray& content) {
    QByteArray out;
    appendU32(out, static_cast<quint32>(8 + content.size()));
    out.append(type, 4);
    out.append(content);
    return out;
}

// mvhd v0: version+flags, creation, modification, timescale, duration, 后面的字段解析器不看。
QByteArray movieHeaderV0(quint32 timescale, quint32 duration) {
    QByteArray content;
    appendU32(content, 0);  // version 0 + flags
    appendU32(content, 0);  // creation time
    appendU32(content, 0);  // modification time
    appendU32(content, timescale);
    appendU32(content, duration);
    appendU32(content, 0x00010000);  // rate
    return content;
}

QByteArray movieHeaderV1(quint32 timescale, quint64 duration) {
    QByteArray content;
    appendU32(content, 0x01000000);  // version 1 + flags
    appendU64(content, 0);           // creation time
    appendU64(content, 0);           // modification time
    appendU32(content, timescale);
    appendU64(content, duration);
    appendU32(content, 0x00010000);  // rate
    return content;
}

// tkhd v0，matrix 传单位阵或旋转阵，最后是 16.16 定点的 width/height。
QByteArray trackHeaderV0(quint32 width, quint32 height, bool rotated) {
    QByteArray content;
    appendU32(content, 0);  // version 0 + flags
    appendU32(content, 0);  // creation
    appendU32(content, 0);  // modification
    appendU32(content, 1);  // track id
    appendU32(content, 0);  // reserved
    appendU32(content, 0);  // duration
    appendU64(content, 0);  // reserved
    appendU32(content, 0);  // layer + alternate_group
    appendU32(content, 0);  // volume + reserved
    // matrix: a b u / c d v / x y w
    if (rotated) {
        appendU32(content, 0);           // a
        appendU32(content, 0x00010000);  // b
        appendU32(content, 0);           // u
        appendU32(content, 0xFFFF0000);  // c = -1.0
        appendU32(content, 0);           // d
    } else {
        appendU32(content, 0x00010000);  // a
        appendU32(content, 0);           // b
        appendU32(content, 0);           // u
        appendU32(content, 0);           // c
        appendU32(content, 0x00010000);  // d
    }
    appendU32(content, 0);           // v
    appendU32(content, 0);           // x
    appendU32(content, 0);           // y
    appendU32(content, 0x40000000);  // w
    appendU32(content, width << 16);
    appendU32(content, height << 16);
    return content;
}

QByteArray audioTrackHeader() {
    // 音频轨的 tkhd 宽高都是 0，解析器必须跳过它去找视频轨。
    return trackHeaderV0(0, 0, false);
}

QByteArray fileTypeBox() {
    QByteArray content;
    content.append("isom", 4);
    appendU32(content, 512);
    content.append("isomiso2avc1mp41", 16);
    return box("ftyp", content);
}

VideoFileMetadata parse(const QByteArray& bytes) {
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    return parseIsoBaseMediaMetadata(buffer);
}

}  // namespace

class VideoFileMetadataTest : public QObject {
    Q_OBJECT

private slots:
    void readsDurationAndDimensionsFromMoov();
    void readsVersionOneMovieHeader();
    void findsMoovWhenItTrailsTheMediaData();
    void skipsAudioTrackAndTakesTheVideoTrack();
    void swapsDimensionsForRotatedTracks();
    void treatsUnknownDurationSentinelAsMissing();
    void rejectsNonIsoBaseMediaBytes();
    void survivesTruncatedAndSelfNestedBoxes();
    void acceptsOnlyMp4AndMovExtensions();
    void readsMetadataFromAFileOnDisk();
};

void VideoFileMetadataTest::readsDurationAndDimensionsFromMoov() {
    QByteArray moov;
    moov.append(box("mvhd", movieHeaderV0(600, 600 * 42)));
    moov.append(box("trak", box("tkhd", trackHeaderV0(1920, 1080, false))));

    const VideoFileMetadata metadata = parse(fileTypeBox() + box("moov", moov));

    QVERIFY(metadata.valid);
    QCOMPARE(metadata.durationSeconds, 42);
    QCOMPARE(metadata.width, 1920);
    QCOMPARE(metadata.height, 1080);
}

void VideoFileMetadataTest::readsVersionOneMovieHeader() {
    QByteArray moov;
    moov.append(box("mvhd", movieHeaderV1(90000, quint64(90000) * 7)));
    moov.append(box("trak", box("tkhd", trackHeaderV0(1280, 720, false))));

    const VideoFileMetadata metadata = parse(fileTypeBox() + box("moov", moov));

    QCOMPARE(metadata.durationSeconds, 7);
    QCOMPARE(metadata.width, 1280);
}

void VideoFileMetadataTest::findsMoovWhenItTrailsTheMediaData() {
    // 没做 faststart 的文件 moov 在 mdat 后面。这是最常见的真实布局，
    // 也是「必须能 seek」这条约束的由来。
    QByteArray moov;
    moov.append(box("mvhd", movieHeaderV0(1000, 3500)));
    moov.append(box("trak", box("tkhd", trackHeaderV0(640, 360, false))));
    const QByteArray mdat = box("mdat", QByteArray(4096, '\x00'));

    const VideoFileMetadata metadata = parse(fileTypeBox() + mdat + box("moov", moov));

    QVERIFY(metadata.valid);
    QCOMPARE(metadata.durationSeconds, 4);  // 3.5s 四舍五入
    QCOMPARE(metadata.width, 640);
}

void VideoFileMetadataTest::skipsAudioTrackAndTakesTheVideoTrack() {
    QByteArray moov;
    moov.append(box("mvhd", movieHeaderV0(1000, 5000)));
    moov.append(box("trak", box("tkhd", audioTrackHeader())));
    moov.append(box("trak", box("tkhd", trackHeaderV0(1080, 1920, false))));

    const VideoFileMetadata metadata = parse(fileTypeBox() + box("moov", moov));

    QCOMPARE(metadata.width, 1080);
    QCOMPARE(metadata.height, 1920);
}

void VideoFileMetadataTest::swapsDimensionsForRotatedTracks() {
    // 手机竖屏录像常常是「1920x1080 存储 + 90 度旋转矩阵」。封面必须按 1080x1920 画，
    // 否则对端看到的是一张躺倒比例的封面。
    QByteArray moov;
    moov.append(box("mvhd", movieHeaderV0(1000, 1000)));
    moov.append(box("trak", box("tkhd", trackHeaderV0(1920, 1080, true))));

    const VideoFileMetadata metadata = parse(fileTypeBox() + box("moov", moov));

    QCOMPARE(metadata.width, 1080);
    QCOMPARE(metadata.height, 1920);
}

void VideoFileMetadataTest::treatsUnknownDurationSentinelAsMissing() {
    // mvhd v0 里 0xFFFFFFFF 是「时长未知」，不是 49710 天的视频。
    QByteArray moov;
    moov.append(box("mvhd", movieHeaderV0(1000, 0xFFFFFFFFu)));
    moov.append(box("trak", box("tkhd", trackHeaderV0(640, 360, false))));

    const VideoFileMetadata metadata = parse(fileTypeBox() + box("moov", moov));

    QCOMPARE(metadata.durationSeconds, 0);
    QVERIFY(metadata.valid);  // 尺寸还在，仍算解析到了东西
}

void VideoFileMetadataTest::rejectsNonIsoBaseMediaBytes() {
    const VideoFileMetadata metadata = parse(QByteArray("not a video file at all, just text"));
    QVERIFY(!metadata.valid);
    QCOMPARE(metadata.durationSeconds, 0);
    QCOMPARE(metadata.width, 0);
}

void VideoFileMetadataTest::survivesTruncatedAndSelfNestedBoxes() {
    // 盒子声明的长度超出文件：不能越界读，也不能死循环。
    QByteArray truncated;
    appendU32(truncated, 4096);
    truncated.append("moov", 4);
    truncated.append(QByteArray(16, '\x00'));
    QVERIFY(!parse(truncated).valid);

    // size 小于头长度的畸形盒子：解析必须停下来，而不是原地打转。
    QByteArray degenerate = fileTypeBox();
    appendU32(degenerate, 0);
    degenerate.append("moov", 4);
    QVERIFY(!parse(degenerate).valid);

    // moov 自嵌套 moov：深度上限兜住，不能一路递归到栈溢出。
    QByteArray nested = box("mvhd", movieHeaderV0(1000, 1000));
    for (int i = 0; i < 64; ++i) nested = box("moov", nested);
    parse(fileTypeBox() + nested);  // 只要不崩就算过
}

void VideoFileMetadataTest::acceptsOnlyMp4AndMovExtensions() {
    QVERIFY(isSupportedVideoFile(QStringLiteral("/tmp/clip.mp4")));
    QVERIFY(isSupportedVideoFile(QStringLiteral("/tmp/clip.MOV")));
    QVERIFY(!isSupportedVideoFile(QStringLiteral("/tmp/clip.mkv")));
    QVERIFY(!isSupportedVideoFile(QStringLiteral("/tmp/clip.webm")));
    QVERIFY(!isSupportedVideoFile(QStringLiteral("/tmp/clip")));
    QCOMPARE(videoContainerTypeForPath(QStringLiteral("/tmp/clip.MP4")), QStringLiteral("mp4"));
    QCOMPARE(videoContainerTypeForPath(QStringLiteral("/tmp/clip.avi")), QString());
}

void VideoFileMetadataTest::readsMetadataFromAFileOnDisk() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("clip.mp4"));

    QByteArray moov;
    moov.append(box("mvhd", movieHeaderV0(600, 600 * 12)));
    moov.append(box("trak", box("tkhd", trackHeaderV0(854, 480, false))));
    const QByteArray bytes = fileTypeBox() + box("moov", moov);

    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(bytes), qint64(bytes.size()));
    file.close();

    const VideoFileMetadata metadata = readVideoFileMetadata(path);

    QVERIFY(metadata.valid);
    QCOMPARE(metadata.containerType, QStringLiteral("mp4"));
    QCOMPARE(metadata.sizeBytes, qint64(bytes.size()));
    QCOMPARE(metadata.durationSeconds, 12);
    QCOMPARE(metadata.width, 854);
    QCOMPARE(metadata.height, 480);
}

QTEST_MAIN(VideoFileMetadataTest)
#include "VideoFileMetadataTest.moc"
