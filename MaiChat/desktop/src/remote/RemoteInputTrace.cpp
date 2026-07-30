#include "remote/RemoteInputTrace.h"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QTextStream>

namespace RemoteInput {
namespace {

QString resolveTraceFilePath() {
    QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (root.isEmpty()) root = QDir::homePath() + QStringLiteral("/.maichat-desktop");
    return QDir(root).filePath(QStringLiteral("RemoteDesktop/remote-input-trace.log"));
}

QMutex& traceMutex() {
    static QMutex mutex;
    return mutex;
}

}  // namespace

bool traceEnabled() {
    static const bool enabled = [] {
        const QByteArray raw = qgetenv("MAICHAT_REMOTE_INPUT_TRACE").trimmed().toLower();
        return raw == "1" || raw == "true" || raw == "on";
    }();
    return enabled;
}

QString traceFilePath() {
    if (!traceEnabled()) return QString();
    static const QString path = resolveTraceFilePath();
    return path;
}

void trace(const QString& line) {
    if (!traceEnabled()) return;

    const QString stamped =
        QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")) + QLatin1Char(' ')
        + line;

    // 加锁：SDK 回调虽然都切回了主线程，但这里是诊断设施，不该对调用方的
    // 线程模型有额外要求——写坏一行日志比多一次加锁难查得多。
    QMutexLocker locker(&traceMutex());

    // stderr 一份：开发机上直接就能看到，不用去翻文件。
    // 临时 QTextStream 析构时会自己 flush。
    QTextStream(stderr) << stamped << '\n';

    static bool directoryReady = false;
    const QString path = traceFilePath();
    if (!directoryReady) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        directoryReady = true;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) return;
    QTextStream out(&file);
    out.setCodec("UTF-8");
    out << stamped << '\n';
}

}  // namespace RemoteInput
