// 文件作用：用无声 WAV 验证本地音乐选择、播放与关闭后的资源释放。
#include <QApplication>
#include <QDataStream>
#include <QFile>
#include <QMediaPlayer>
#include <QPointer>
#include <QPushButton>
#include <QTemporaryDir>
#include <QtTest>

#include "ui/DesktopMusicPlayerDialog.h"

class DesktopMusicPlayerDialogTest : public QObject {
    Q_OBJECT
private slots:
    void playsLocalFileAndStopsOnClose();
};

void DesktopMusicPlayerDialogTest::playsLocalFileAndStopsOnClose() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString error;
    QVERIFY(!DesktopMusicPlayerDialog::openFile(nullptr,
        directory.filePath(QStringLiteral("missing.mp3")), &error));
    QVERIFY(!error.isEmpty());

    const QString path = directory.filePath(QStringLiteral("silence.wav"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    constexpr quint32 samples = 8'000;
    constexpr quint32 bytes = samples * 2;
    stream.writeRawData("RIFF", 4);
    stream << quint32(36 + bytes);
    stream.writeRawData("WAVEfmt ", 8);
    stream << quint32(16) << quint16(1) << quint16(1) << quint32(8'000)
           << quint32(16'000) << quint16(2) << quint16(16);
    stream.writeRawData("data", 4);
    stream << bytes;
    QCOMPARE(file.write(QByteArray(bytes, '\0')), qint64(bytes));
    file.close();

    QVERIFY(DesktopMusicPlayerDialog::openFile(nullptr, path, &error));
    QVERIFY(error.isEmpty());
    QPointer<QWidget> dialog;
    for (QWidget* widget : QApplication::topLevelWidgets())
        if (widget->objectName() == QStringLiteral("desktopMusicPlayer")) dialog = widget;
    QVERIFY(dialog);
    auto* player = dialog->findChild<QMediaPlayer*>();
    QVERIFY(player);
    QTRY_VERIFY_WITH_TIMEOUT(player->duration() >= 900, 3000);
    auto* toggle = dialog->findChild<QPushButton*>(QStringLiteral("desktopMusicPlayPause"));
    QVERIFY(toggle);
    toggle->click();
    QTRY_COMPARE(player->state(), QMediaPlayer::PausedState);
    toggle->click();
    QTRY_COMPARE(player->state(), QMediaPlayer::PlayingState);
    dialog->close();
    QTRY_VERIFY(dialog.isNull());
}

QTEST_MAIN(DesktopMusicPlayerDialogTest)
#include "DesktopMusicPlayerDialogTest.moc"
