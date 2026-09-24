#include "agent/DesktopScreenshotTool.h"

#include <QCoreApplication>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMetaObject>
#include <QPixmap>
#include <QScreen>
#include <QStandardPaths>
#include <QThread>
#include <QUuid>

#include <string>
#include <utility>
#include <vector>

namespace {

constexpr int kMaximumScreenshotWidth = 2560;
constexpr int kMaximumScreenshotHeight = 1600;

std::string toUtf8(const QString& text) {
    const QByteArray bytes = text.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

struct CaptureResult {
    QString path;
    QString error;
    int width = 0;
    int height = 0;
};

void captureCurrentDisplay(CaptureResult& result) {
    QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    if (screen == nullptr) screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        result.error = QStringLiteral("no display is available for screen capture");
        return;
    }

    QPixmap screenshot = screen->grabWindow(0);
    if (screenshot.isNull()) {
        result.error = QStringLiteral(
            "screen capture returned no image; check the operating "
            "system screen recording permission");
        return;
    }

    const QSize limit(kMaximumScreenshotWidth, kMaximumScreenshotHeight);
    if (screenshot.width() > limit.width() || screenshot.height() > limit.height()) {
        screenshot = screenshot.scaled(limit, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (root.isEmpty()) root = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    const QString directory = QDir(root).filePath(QStringLiteral("ai-assistant/screenshots"));
    if (!QDir().mkpath(directory)) {
        result.error = QStringLiteral("could not create the screenshot storage directory");
        return;
    }

    const QString fileName =
        QStringLiteral("screenshot-%1-%2.png")
            .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")))
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QString path = QDir(directory).absoluteFilePath(fileName);
    if (!screenshot.save(path, "PNG")) {
        result.error = QStringLiteral("could not save the captured screen as PNG");
        return;
    }

    result.path = QDir::cleanPath(path);
    result.width = screenshot.width();
    result.height = screenshot.height();
}

class DesktopScreenshotTool final : public MaiTool {
public:
    std::string name() const override {
        return "screenshot";
    }

    std::string description() const override {
        return "Capture the display containing the mouse pointer and inspect the "
               "returned image. "
               "Use this when the user asks you to look at the current screen or "
               "diagnose a visible "
               "interface problem.";
    }

    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{},"additionalProperties":false})";
    }

    bool requiresApproval(const std::string&) const override {
        return true;
    }

    MaiToolResult execute(const std::string& argumentsJson,
                          const MaiToolContext& context) override {
        QJsonParseError parseError;
        const QJsonDocument arguments =
            QJsonDocument::fromJson(QByteArray::fromStdString(argumentsJson), &parseError);
        if (parseError.error != QJsonParseError::NoError || !arguments.isObject() ||
            !arguments.object().isEmpty()) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "screenshot expects an empty JSON object");
        }
        if (context.isCanceled()) {
            return MaiToolResult::failure(MaiErrorCode::Canceled, "screen capture was canceled");
        }

        QCoreApplication* application = QCoreApplication::instance();
        if (application == nullptr) {
            return MaiToolResult::failure(MaiErrorCode::NotConfigured,
                                          "screen capture requires a running Qt application");
        }

        CaptureResult capture;
        if (QThread::currentThread() == application->thread()) {
            captureCurrentDisplay(capture);
        } else {
            const bool invoked = QMetaObject::invokeMethod(
                application, [&capture] { captureCurrentDisplay(capture); },
                Qt::BlockingQueuedConnection);
            if (!invoked) {
                return MaiToolResult::failure(MaiErrorCode::Internal,
                                              "could not run screen capture on the GUI thread");
            }
        }

        if (!capture.error.isEmpty()) {
            return MaiToolResult::failure(MaiErrorCode::Internal, toUtf8(capture.error));
        }
        const std::string output = "Captured the current display at " +
                                   std::to_string(capture.width) + "x" +
                                   std::to_string(capture.height) + ".";
        return MaiToolResult::successWithImages(output,
                                                {MaiToolImage{toUtf8(capture.path), "image/png"}});
    }
};

}  // namespace

std::unique_ptr<MaiTool> makeDesktopScreenshotTool() {
    return std::make_unique<DesktopScreenshotTool>();
}
