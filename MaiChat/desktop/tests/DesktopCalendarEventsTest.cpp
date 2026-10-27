#include "MaiChatTools/DesktopCalendarEvents.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

#include "MaiTime.h"

namespace {

void require(bool value, const char* reason) {
    if (!value) {
        std::cerr << reason << '\n';
        std::exit(1);
    }
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid(), "cannot make calendar test workspace");
    MaiToolContext context;
    context.root = directory.path().toUtf8().toStdString();
    context.sessionId = "ses_calendar";
    const std::int64_t start = MaiTime::getCurrentTime() + 60 * 60 * 1000;
    MaiCalendarEventInput event{"开会,路线", "第一行\n第二行", context.sessionId,
                                 start, start + 30 * 60 * 1000};
    bool opened = false;
    const MaiToolResult result = prepareDesktopCalendarImport(event, context,
        [&](const QUrl& url) {
            opened = url.isLocalFile();
            return opened;
        });
    require(!result.hasError() && opened,
            "OS calendar importer was not invoked with a local file");
    const auto payload = QJsonDocument::fromJson(
        QByteArray::fromStdString(result.output())).object();
    require(payload.value(QStringLiteral("state")) == QStringLiteral("import_required") &&
            !payload.value(QStringLiteral("saved_to_calendar")).toBool(),
            "calendar import falsely claimed an event was saved");
    QFile file(payload.value(QStringLiteral("file_path")).toString());
    require(file.open(QIODevice::ReadOnly), "calendar invitation was not saved");
    const QByteArray bytes = file.readAll();
    require(bytes.contains("BEGIN:VCALENDAR\r\n") &&
            bytes.contains("BEGIN:VEVENT\r\n") &&
            bytes.contains("SUMMARY:") && bytes.contains("\\,") &&
            bytes.contains("\\n") && bytes.contains("DTSTART:") &&
            bytes.contains("DTEND:") && bytes.contains("BEGIN:VALARM\r\n") &&
            bytes.contains("TRIGGER:PT0M\r\n") &&
            bytes.contains("END:VCALENDAR\r\n"),
            "calendar invitation lacks required or escaped fields");

    MaiToolRegistry tools;
    registerMaiCalendarTools(tools, [&](const MaiCalendarEventInput& input,
                                         const MaiToolContext& owner) {
        return prepareDesktopCalendarImport(input, owner,
            [](const QUrl&) { return true; });
    });
    MaiTool* tool = tools.findForModel("maichat_calendar_create_event");
    require(tool && tool->requiresPerCallApproval("{}"),
            "calendar write tool lacks per-call approval");
    const auto utc = [](qint64 value) {
        return QDateTime::fromMSecsSinceEpoch(value, Qt::UTC)
            .toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss'Z'"));
    };
    const QJsonObject valid{{QStringLiteral("title"), QStringLiteral("计划检查")},
        {QStringLiteral("starts_at"), utc(start)},
        {QStringLiteral("ends_at"), utc(start + 30 * 60 * 1000)}};
    const QByteArray arguments = QJsonDocument(valid).toJson(QJsonDocument::Compact);
    const MaiToolResult created = tool->execute(
        std::string(arguments.constData(), std::size_t(arguments.size())), context);
    require(!created.hasError() && created.output().find("import_required") !=
            std::string::npos, "Agent calendar tool did not use the ordinary importer");
    require(tool->execute(R"({"title":"bad","starts_at":"1970-01-01T00:00:01Z","ends_at":"1970-01-01T00:00:02Z"})",
                          context).hasError(),
            "past calendar event was created");
    require(tool->execute(R"({"title":"bad","starts_at":"1970-01-01T00:00:01Z","ends_at":"1970-01-01T00:00:02Z","shell":"x"})",
                          context).hasError(),
            "calendar tool accepted an unknown action");
}
