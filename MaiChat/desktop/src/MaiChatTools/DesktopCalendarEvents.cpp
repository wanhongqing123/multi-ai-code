#include "MaiChatTools/DesktopCalendarEvents.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QSaveFile>
#include <QThread>
#include <QUuid>

namespace {

QString escaped(QString value) {
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    value.replace(QLatin1Char(';'), QStringLiteral("\\;"));
    value.replace(QLatin1Char(','), QStringLiteral("\\,"));
    value.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    value.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    value.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    return value;
}

QByteArray folded(const QByteArray& line) {
    QByteArray output;
    int offset = 0;
    while (offset < line.size()) {
        int count = qMin(offset == 0 ? 75 : 74, line.size() - offset);
        while (offset + count < line.size() && count > 1 &&
               (static_cast<unsigned char>(line[offset + count]) & 0xC0) == 0x80)
            --count;
        if (offset) output.append(' ');
        output.append(line.constData() + offset, count);
        output.append("\r\n");
        offset += count;
    }
    return output;
}

QByteArray calendarBytes(const MaiCalendarEventInput& event) {
    const auto utc = [](qint64 milliseconds) {
        return QDateTime::fromMSecsSinceEpoch(milliseconds, Qt::UTC)
            .toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'"));
    };
    QByteArray bytes("BEGIN:VCALENDAR\r\nVERSION:2.0\r\n"
                     "PRODID:-//MaiChat//Session Calendar//EN\r\n"
                     "CALSCALE:GREGORIAN\r\nBEGIN:VEVENT\r\n");
    const QString uid = QUuid::createUuid().toString(QUuid::WithoutBraces) +
        QStringLiteral("@maichat.local");
    bytes += folded((QStringLiteral("UID:") + uid).toUtf8());
    bytes += folded((QStringLiteral("DTSTAMP:") +
        utc(QDateTime::currentMSecsSinceEpoch())).toUtf8());
    bytes += folded((QStringLiteral("DTSTART:") + utc(event.startsAtUtcMs)).toUtf8());
    bytes += folded((QStringLiteral("DTEND:") + utc(event.endsAtUtcMs)).toUtf8());
    bytes += folded((QStringLiteral("SUMMARY:") +
        escaped(QString::fromUtf8(event.title.data(), int(event.title.size())))).toUtf8());
    if (!event.notes.empty())
        bytes += folded((QStringLiteral("DESCRIPTION:") +
            escaped(QString::fromUtf8(event.notes.data(), int(event.notes.size())))).toUtf8());
    bytes += "BEGIN:VALARM\r\nACTION:DISPLAY\r\nTRIGGER:PT0M\r\n"
             "DESCRIPTION:MaiChat calendar reminder\r\nEND:VALARM\r\n"
             "END:VEVENT\r\nEND:VCALENDAR\r\n";
    return bytes;
}

}  // namespace

MaiToolResult prepareDesktopCalendarImport(const MaiCalendarEventInput& event,
    const MaiToolContext& context, std::function<bool(const QUrl&)> open) {
    const MaiError validation = maiValidateCalendarEvent(event,
        QDateTime::currentMSecsSinceEpoch());
    if (validation.hasError())
        return MaiToolResult::failure(validation.code(), validation.message());
    const std::string relative = "calendar/" +
        QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString() + ".ics";
    const std::string resolved = context.resolvePath(relative);
    if (resolved.empty())
        return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                      "calendar import path is outside the Agent workspace");
    const QString path = QString::fromUtf8(resolved.c_str());
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return MaiToolResult::failure(MaiErrorCode::Internal,
                                      "cannot prepare calendar import directory");
    QSaveFile file(path);
    const QByteArray contents = calendarBytes(event);
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size() ||
        !file.commit())
        return MaiToolResult::failure(MaiErrorCode::Internal,
                                      "cannot save calendar import file");
    const QUrl url = QUrl::fromLocalFile(path);
    bool opened = false;
    if (open) opened = open(url);
    else if (auto* app = QCoreApplication::instance()) {
        const auto invoke = [&] { opened = QDesktopServices::openUrl(url); };
        if (QThread::currentThread() == app->thread()) invoke();
        else QMetaObject::invokeMethod(app, invoke, Qt::BlockingQueuedConnection);
    }
    const QJsonObject result{{QStringLiteral("state"), QStringLiteral("import_required")},
        {QStringLiteral("file_path"), QDir::fromNativeSeparators(path)},
        {QStringLiteral("system_calendar_opened"), opened},
        {QStringLiteral("saved_to_calendar"), false}};
    const QByteArray json = QJsonDocument(result).toJson(QJsonDocument::Compact);
    return MaiToolResult::success(std::string(json.constData(), std::size_t(json.size())));
}
