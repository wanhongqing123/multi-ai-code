#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cstdio>

#include <json.hpp>

#include "MaiChatTools/DesktopComputerPlatform.h"
#include "MaiChatTools/DesktopComputerTools.h"
#include "MaiTool.h"

namespace {

#define CHECK(value) do { if (!(value)) { \
    std::fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__, __LINE__, #value); \
    return 1; } } while (false)

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    MaiToolRegistry registry;
    auto controller = std::make_shared<DesktopComputerController>();
    registerDesktopComputerTools(registry, controller);
    MaiTool* tool = registry.find("desktop_computer");
    CHECK(tool && nlohmann::json::parse(tool->parametersSchema()).is_object());
    CHECK(tool->requiresPerCallApproval(R"({"action":"observe"})"));
    CHECK(tool->requiresPerCallApproval(R"({"action":"click"})"));
    CHECK(!tool->requiresPerCallApproval(R"({"action":"clear"})"));
    MaiToolContext absent;
    CHECK(tool->execute(R"({"action":"observe","window_title":"test"})", absent)
              .error().code() == MaiErrorCode::NotConfigured);
    CHECK(controller->act("no-observation", "click", "unknown", 1,
                          0.5, 0.5, "", "", 0).code() == MaiErrorCode::NotFound);
    QWidget window;
    window.setWindowTitle(QStringLiteral("MaiChat Computer Tool Fixture"));
    auto* layout = new QVBoxLayout(&window);
    auto* result = new QLabel(QStringLiteral("Waiting"), &window);
    auto* input = new QLineEdit(&window);
    input->setAccessibleName(QStringLiteral("Fixture input"));
    auto* button = new QPushButton(QStringLiteral("Confirm"), &window);
    QObject::connect(button, &QPushButton::clicked, &window,
                     [result, input] { result->setText(input->text()); });
    layout->addWidget(result);
    layout->addWidget(input);
    layout->addWidget(button);
    window.resize(500, 320);
    window.show();
    app.processEvents();
    QThread::msleep(300);
    app.processEvents();
    const auto windows = maiDesktopWindows();
    if (!windows) {
        std::fprintf(stderr, "computer test unavailable: %s\n",
                     windows.error().message().c_str());
        CHECK(windows.error().code() == MaiErrorCode::NotConfigured);
        return 0;
    }
    DesktopNativeWindow native;
    int count = 0;
    for (const auto& candidate : windows.value()) {
        if (candidate.title == "MaiChat Computer Tool Fixture") {
            native = candidate;
            ++count;
        }
    }
    CHECK(count == 1);
    QTemporaryDir workspace;
    CHECK(workspace.isValid());
    const auto observed = controller->observe("owner", native.title,
        workspace.path().toUtf8().toStdString(), true);
    if (!observed) {
        std::fprintf(stderr, "computer observe unavailable: %s\n",
                     observed.error().message().c_str());
        CHECK(observed.error().code() == MaiErrorCode::NotConfigured ||
              observed.error().code() == MaiErrorCode::NotSupported);
        return 0;
    }
    const auto snapshot = nlohmann::json::parse(observed.value().json);
    CHECK(snapshot["window_id"] == std::to_string(native.id));
    CHECK(QFileInfo(QString::fromStdString(observed.value().imagePath)).size() > 100);
    const QString copy = qEnvironmentVariable("MAICHAT_COMPUTER_TEST_SCREENSHOT");
    if (!copy.isEmpty()) {
        QFile::remove(copy);
        CHECK(QFile::copy(QString::fromStdString(observed.value().imagePath), copy));
    }
    CHECK(controller->act("other", "click", snapshot["snapshot_id"],
        native.id, 0.5, 0.5, "", "", 0).code() == MaiErrorCode::NotFound);
    CHECK(controller->act("owner", "click", "stale", native.id,
        0.5, 0.5, "", "", 0).code() == MaiErrorCode::InvalidInput);
    const QPoint inputCenter = input->mapToGlobal(input->rect().center());
    const double inputX = (inputCenter.x() - native.x) / native.width;
    const double inputY = (inputCenter.y() - native.y) / native.height;
    const MaiError clicked = controller->act("owner", "click",
        snapshot["snapshot_id"], native.id, inputX, inputY, "", "", 0);
    if (clicked) {
        std::fprintf(stderr, "computer click unavailable: %s\n", clicked.message().c_str());
        CHECK(clicked.code() == MaiErrorCode::NotConfigured);
        return 0;
    }
    QThread::msleep(150);
    app.processEvents();
    const auto selected = controller->observe("owner", native.title,
        workspace.path().toUtf8().toStdString(), false);
    CHECK(selected);
    const auto second = nlohmann::json::parse(selected.value().json);
    const MaiError typed = controller->act("owner", "type",
        second["snapshot_id"], native.id, 0, 0, "computer OK", "", 0);
    if (typed) {
        std::fprintf(stderr, "computer type unavailable: %s\n", typed.message().c_str());
        CHECK(typed.code() == MaiErrorCode::NotConfigured);
        return 0;
    }
    QThread::msleep(150);
    app.processEvents();
    CHECK(input->text() == QStringLiteral("computer OK"));
    CHECK(!controller->clear("owner"));
    return 0;
}
