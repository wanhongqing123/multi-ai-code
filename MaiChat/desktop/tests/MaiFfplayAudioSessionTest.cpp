// 文件作用：验证共享 FFplay 纯音频会话的实际播放、暂停、跳转和停止。
#include <QDataStream>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>
#include <cstdlib>

#include "MaiFfplayAudioSession.h"

class MaiFfplayAudioSessionTest : public QObject {
    Q_OBJECT
private slots:
    void audioOnlyPauseHoldsClockAndSeekResumes();
};

void MaiFfplayAudioSessionTest::audioOnlyPauseHoldsClockAndSeekResumes() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QFile file(directory.filePath(QStringLiteral("music.wav")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    constexpr quint32 sampleRate = 8'000;
    constexpr quint32 count = sampleRate * 5;
    stream.writeRawData("RIFF", 4);
    stream << quint32(36 + count * 2);
    stream.writeRawData("WAVEfmt ", 8);
    stream << quint32(16) << quint16(1) << quint16(1) << sampleRate
           << quint32(sampleRate * 2) << quint16(2) << quint16(16);
    stream.writeRawData("data", 4);
    stream << quint32(count * 2);
    for (quint32 index = 0; index < count; ++index) {
        const double phase = index * (440.0 * 6.283185307179586 / sampleRate);
        stream << qint16(std::sin(phase) * 2000);
    }
    file.close();

    const QByteArray path = file.fileName().toUtf8();
    auto* session = maiFfplayAudioStart(path.constData());
    QVERIFY(session);
    MaiFfplayPlaybackStatus state{};
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayAudioStatus(session, &state) == 1 &&
                             state.duration_us >= 4'900'000 && state.position_us > 100'000,
                             3'000);
    QCOMPARE(maiFfplayAudioCommand(session, "pause"), 1);
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayAudioStatus(session, &state) == 1 && state.paused == 1,
                             2'000);
    const int64_t pausePosition = state.position_us;
    QTest::qWait(450);
    QVERIFY(maiFfplayAudioStatus(session, &state) == 1);
    QVERIFY(std::llabs(state.position_us - pausePosition) < 200'000);
    QCOMPARE(maiFfplayAudioCommand(session, "pause"), 1);
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayAudioStatus(session, &state) == 1 && state.paused == 0,
                             2'000);
    QCOMPARE(maiFfplayAudioSeekAbsoluteUs(session, 2'000'000), 1);
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayAudioStatus(session, &state) == 1 &&
                             state.position_us >= 1'800'000, 2'000);
    maiFfplayAudioStop(session);
}

QTEST_GUILESS_MAIN(MaiFfplayAudioSessionTest)
#include "MaiFfplayAudioSessionTest.moc"
