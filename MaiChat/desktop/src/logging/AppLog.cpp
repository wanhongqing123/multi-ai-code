#include "logging/AppLog.h"

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTextStream>

namespace AppLog {
namespace {

// 单个文件 5MB，保留 7 天。日志的价值在于"出问题那天/那几分钟发生了什么"，
// 再久没有意义，占用用户磁盘反而是负担。
constexpr qint64 kMaxBytesPerFile = 5 * 1024 * 1024;
constexpr int kRetentionDays = 7;

constexpr char kFilePrefix[] = "maichat-";
constexpr char kFileSuffix[] = ".log";
// maichat-20260731.log / maichat-20260731.1.log / maichat-20260731-p1234.log
constexpr char kDateFormat[] = "yyyyMMdd";

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

// 同机双开是这个产品的正常用法（--login 就是为远程桌面同机联调加的）。
// 两个进程往同一个文件里追加，日志会交错，轮转更会互相把对方的文件改名——
// 排障时看到一份两个账号混在一起的日志，比没有日志更误导人。
// 所以第一个实例拿到锁用干净的文件名，之后的实例各自带 -p<pid> 后缀。
const QString& instanceSuffix() {
    static const QString value = [] {
        QDir().mkpath(directory());
        // 静态持有：锁必须活到进程退出，析构时才释放。
        // 不改 staleLockTime——默认值下 Qt 会检查锁里记的 pid 是否还活着，
        // 上次崩溃残留的锁能被自动回收；设成 0 反而会让残留锁永远生效，
        // 之后每次启动都白白带上 pid 后缀。
        static QLockFile lock(QDir(directory()).filePath(QStringLiteral(".instance.lock")));
        if (lock.tryLock(0)) return QString();
        return QStringLiteral("-p%1").arg(QCoreApplication::applicationPid());
    }();
    return value;
}

QString fileNameFor(const QDate& date) {
    return QLatin1String(kFilePrefix) + date.toString(QLatin1String(kDateFormat))
           + instanceSuffix() + QLatin1String(kFileSuffix);
}

// 同一天写满 5MB 时，当前文件改名成带序号的，序号按时间递增：
// .1 是当天最早的一段，不带序号的永远是正在写的那个。
QString rolledNameFor(const QDate& date, int index) {
    return QLatin1String(kFilePrefix) + date.toString(QLatin1String(kDateFormat))
           + instanceSuffix() + QStringLiteral(".%1").arg(index) + QLatin1String(kFileSuffix);
}

QString pathFor(const QString& fileName) { return QDir(directory()).filePath(fileName); }

QFile& logFile() {
    static QFile value;
    return value;
}

qint64& writtenBytes() {
    static qint64 value = 0;
    return value;
}

QDate& openedDate() {
    static QDate value;
    return value;
}

// 以下 *Locked 函数都假定调用方已持有 mutex()。

// 超过保留期的文件直接删掉。只认自己的命名规则，绝不碰目录里的其它文件——
// 日志目录理论上是我们独占的，但删除是不可逆操作，宁可保守。
void purgeExpiredLocked() {
    const QDate oldestKept = QDate::currentDate().addDays(-(kRetentionDays - 1));
    QDir dir(directory());
    const auto entries = dir.entryInfoList(
        {QLatin1String(kFilePrefix) + QStringLiteral("*") + QLatin1String(kFileSuffix)},
        QDir::Files);
    for (const QFileInfo& entry : entries) {
        // 从 maichat-20260731[...] 里取出那 8 位日期。取不出来的说明不是我们
        // 写的，跳过。
        const QString stem = entry.fileName().mid(static_cast<int>(qstrlen(kFilePrefix)));
        const QDate date = QDate::fromString(stem.left(8), QLatin1String(kDateFormat));
        if (!date.isValid()) continue;
        if (date < oldestKept) QFile::remove(entry.absoluteFilePath());
    }
}

void openLocked() {
    QFile& file = logFile();
    if (file.isOpen()) return;
    QDir().mkpath(directory());
    openedDate() = QDate::currentDate();
    file.setFileName(pathFor(fileNameFor(openedDate())));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) return;
    writtenBytes() = file.size();
}

void rollBySizeLocked() {
    QFile& file = logFile();
    const QDate date = openedDate();
    file.close();

    // 找当天第一个没被占用的序号。不复用序号，避免把已有的一段覆盖掉。
    int index = 1;
    while (QFile::exists(pathFor(rolledNameFor(date, index)))) ++index;
    QFile::rename(pathFor(fileNameFor(date)), pathFor(rolledNameFor(date, index)));

    writtenBytes() = 0;
    openLocked();
}

void writeLineLocked(const QString& line) {
    openLocked();
    QFile& file = logFile();
    if (!file.isOpen()) return;

    // 跨零点要换文件，否则跑了通宵的进程会把两天的记录混在前一天里。
    if (openedDate() != QDate::currentDate()) {
        file.close();
        openLocked();
        purgeExpiredLocked();
        if (!file.isOpen()) return;
    }

    const QByteArray bytes = (line + QLatin1Char('\n')).toUtf8();
    file.write(bytes);
    file.flush();  // 崩溃前那几行才是最有价值的，不能留在缓冲区里
    writtenBytes() += bytes.size();
    if (writtenBytes() >= kMaxBytesPerFile) rollBySizeLocked();
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

    {
        QMutexLocker locker(&mutex());
        openLocked();
        purgeExpiredLocked();
    }

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
    qInfo().noquote() << QStringLiteral("pid: %1").arg(QCoreApplication::applicationPid());
    qInfo().noquote() << QStringLiteral("log file: %1").arg(filePath());
}

QString filePath() {
    QMutexLocker locker(&mutex());
    const QDate date = openedDate().isValid() ? openedDate() : QDate::currentDate();
    return pathFor(fileNameFor(date));
}

QString directoryPath() { return directory(); }

}  // namespace AppLog
