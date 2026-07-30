#include "logging/AppLog.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTextStream>

namespace AppLog {
namespace {

// 5MB × 4 个文件（当前 + 3 个历史）≈ 20MB 上限。日志的价值在于"出问题那几
// 分钟发生了什么"，留太久没意义，占用用户磁盘反而是负担。
constexpr qint64 kMaxBytes = 5 * 1024 * 1024;
constexpr int kHistoryCount = 3;

QMutex& mutex() {
    static QMutex value;
    return value;
}

QString resolveDirectory() {
    QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (root.isEmpty()) root = QDir::homePath() + QStringLiteral("/.maichat-desktop");
    return QDir(root).filePath(QStringLiteral("logs"));
}

const QString& directory() {
    static const QString value = resolveDirectory();
    return value;
}

QString historyPath(int index) {
    return QDir(directory()).filePath(QStringLiteral("maichat.%1.log").arg(index));
}

QString currentPath() {
    return QDir(directory()).filePath(QStringLiteral("maichat.log"));
}

QFile& logFile() {
    static QFile value;
    return value;
}

qint64& writtenBytes() {
    static qint64 value = 0;
    return value;
}

// 以下几个 *Locked 函数都假定调用方已持有 mutex()。

void openLocked() {
    QFile& file = logFile();
    if (file.isOpen()) return;
    QDir().mkpath(directory());
    file.setFileName(currentPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) return;
    writtenBytes() = file.size();
}

void rotateLocked() {
    QFile& file = logFile();
    file.close();

    // 最老的一个直接删掉，其余依次后移。从后往前改名，否则会自己覆盖自己。
    QFile::remove(historyPath(kHistoryCount));
    for (int index = kHistoryCount - 1; index >= 1; --index) {
        QFile::rename(historyPath(index), historyPath(index + 1));
    }
    QFile::rename(currentPath(), historyPath(1));

    writtenBytes() = 0;
    openLocked();
}

void writeLineLocked(const QString& line) {
    openLocked();
    QFile& file = logFile();
    if (!file.isOpen()) return;

    const QByteArray bytes = (line + QLatin1Char('\n')).toUtf8();
    file.write(bytes);
    file.flush();  // 崩溃前那几行才是最有价值的，不能留在缓冲区里
    writtenBytes() += bytes.size();
    if (writtenBytes() >= kMaxBytes) rotateLocked();
}

char levelTag(QtMsgType type) {
    switch (type) {
        case QtDebugMsg: return 'D';
        case QtInfoMsg: return 'I';
        case QtWarningMsg: return 'W';
        case QtCriticalMsg: return 'E';
        case QtFatalMsg: return 'F';
    }
    return '?';
}

QtMessageHandler& previousHandler() {
    static QtMessageHandler value = nullptr;
    return value;
}

void handler(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    const QString line = QStringLiteral("%1 [%2] %3")
                             .arg(QDateTime::currentDateTime().toString(
                                 QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")))
                             .arg(QLatin1Char(levelTag(type)))
                             .arg(message);

    {
        QMutexLocker locker(&mutex());
        writeLineLocked(line);
    }

    // stderr 保留一份：从终端跑的时候不用再去翻文件。
    // 注意不能在这里用 qDebug 之类——那会直接递归回本函数。
    QTextStream(stderr) << line << '\n';

    // Fatal 必须让原处理器接手，由它触发 abort，否则 Q_ASSERT 失败后程序会
    // 带着已经损坏的状态继续跑下去。
    if (type == QtFatalMsg && previousHandler()) previousHandler()(type, context, message);
}

}  // namespace

void install() {
    static bool installed = false;
    if (installed) return;
    installed = true;

    previousHandler() = qInstallMessageHandler(handler);

    // 开头这一段是给"用户把日志发过来"这个场景准备的：版本、系统、
    // 可执行文件位置，这些每次都得问一遍，不如自己写上。
    // 日志内容一律英文——文件要能在任何编码环境下被原样读出来，也便于直接
    // 贴进 issue 检索。代码注释仍用中文，两者用途不同。
    qInfo().noquote() << QStringLiteral("========== MaiChat started ==========");
    qInfo().noquote() << QStringLiteral("version: %1  qt: %2")
                             .arg(QCoreApplication::applicationVersion().isEmpty()
                                      ? QStringLiteral("<unset>")
                                      : QCoreApplication::applicationVersion())
                             .arg(QLatin1String(qVersion()));
    qInfo().noquote() << QStringLiteral("os: %1  kernel: %2  arch: %3")
                             .arg(QSysInfo::prettyProductName())
                             .arg(QSysInfo::kernelVersion())
                             .arg(QSysInfo::currentCpuArchitecture());
    qInfo().noquote() << QStringLiteral("executable: %1")
                             .arg(QCoreApplication::applicationFilePath());
    qInfo().noquote() << QStringLiteral("log file: %1").arg(currentPath());
}

QString filePath() { return currentPath(); }

QString directoryPath() { return directory(); }

}  // namespace AppLog
