#include <QCoreApplication>
#include <QFile>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <cstdio>
#include <future>
#include <thread>

#include <json.hpp>

#include "MaiChatTools/DesktopBrowserTools.h"
#include "MaiTool.h"

namespace {

#define CHECK(value) do { if (!(value)) { \
    std::fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__, __LINE__, #value); \
    return 1; } } while (false)

struct FixtureServer {
    std::atomic<bool> stopping{false};
    std::thread worker;
    std::future<quint16> port;

    FixtureServer() {
        std::promise<quint16> ready;
        port = ready.get_future();
        worker = std::thread([this, promise = std::move(ready)]() mutable {
            QTcpServer server;
            if (!server.listen(QHostAddress::LocalHost, 0)) {
                promise.set_value(0);
                return;
            }
            promise.set_value(server.serverPort());
            const QByteArray html = R"(<!doctype html><html><head><meta name="viewport"
                content="width=device-width,initial-scale=1"><title>Agent Browser Fixture</title></head>
                <body style="background:white;font-size:22px"><h1>Agent Browser Fixture</h1>
                <input aria-label="Note" id="note"><input aria-label="Password" type="password">
                <button aria-label="Run" onclick="document.getElementById('result').textContent=
                'clicked:'+document.getElementById('note').value">Run</button>
                <p id="result">idle</p></body></html>)";
            while (!stopping.load()) {
                if (!server.waitForNewConnection(100)) continue;
                QTcpSocket* client = server.nextPendingConnection();
                if (!client) continue;
                client->waitForReadyRead(1000);
                client->readAll();
                const QByteArray header = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                    "Content-Length: " + QByteArray::number(html.size()) +
                    "\r\nConnection: close\r\n\r\n";
                client->write(header);
                client->write(html);
                client->waitForBytesWritten(1000);
                client->disconnectFromHost();
                delete client;
            }
        });
    }
    ~FixtureServer() {
        stopping = true;
        if (worker.joinable()) worker.join();
    }
};

std::string element(const nlohmann::json& snapshot, const std::string& label) {
    for (const auto& value : snapshot["elements"])
        if (value.value("label", "") == label) return value["id"].get<std::string>();
    return {};
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    FixtureServer server;
    const quint16 port = server.port.get();
    CHECK(port != 0);
    const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/page";
    QTemporaryDir workspace;
    CHECK(workspace.isValid());
    auto manager = std::make_shared<DesktopBrowserManager>();
    MaiToolRegistry registry;
    registerDesktopBrowserTools(registry, manager);
    MaiTool* tool = registry.find("desktop_browser");
    CHECK(tool != nullptr);
    CHECK(nlohmann::json::parse(tool->parametersSchema()).is_object());
    CHECK(tool->requiresPerCallApproval(R"({"action":"open"})"));
    CHECK(tool->requiresPerCallApproval(R"({"action":"click"})"));
    CHECK(!tool->requiresApproval(R"({"action":"observe"})"));

    const MaiError opened = manager->open("owner", url);
    if (opened) std::fprintf(stderr, "open failed: %s\n", opened.message().c_str());
    CHECK(!opened);
    CHECK(manager->observe("other").error().code() == MaiErrorCode::NotFound);
    nlohmann::json first;
    for (int attempt = 0; attempt < 60; ++attempt) {
        const auto observed = manager->observe("owner");
        if (observed) {
            const auto value = nlohmann::json::parse(observed.value());
            if (value.value("state", "") == "ready" &&
                value.value("text", "").find("idle") != std::string::npos) {
                first = value;
                break;
            }
        }
        QThread::msleep(100);
    }
    CHECK(first.is_object());
    CHECK(first["url"] == url);
    const std::string note = element(first, "Note");
    const std::string password = element(first, "Password");
    CHECK(!note.empty() && !password.empty());
    CHECK(manager->act("owner", "type", url, first["snapshot_id"], password,
                       "Password", "secret").code() == MaiErrorCode::InvalidInput);
    CHECK(!manager->act("owner", "type", url, first["snapshot_id"], note,
                        "Note", "desktop OK"));
    CHECK(manager->act("owner", "type", url, first["snapshot_id"], note,
                       "Note", "stale").code() == MaiErrorCode::InvalidInput);
    const auto second = manager->observe("owner");
    CHECK(second);
    const auto next = nlohmann::json::parse(second.value());
    const std::string button = element(next, "Run");
    CHECK(!button.empty());
    CHECK(!manager->act("owner", "click", url, next["snapshot_id"],
                        button, "Run", ""));
    bool clicked = false;
    for (int attempt = 0; attempt < 30; ++attempt) {
        const auto observed = manager->observe("owner");
        if (observed && observed.value().find("clicked:desktop OK") != std::string::npos) {
            clicked = true;
            break;
        }
        QThread::msleep(50);
    }
    CHECK(clicked);
    CHECK(manager->screenshot("owner", "https://wrong.example/",
        workspace.filePath(QStringLiteral("wrong.png")).toStdString()).code() ==
        MaiErrorCode::InvalidInput);
    const QString image = workspace.filePath(QStringLiteral("page.png"));
    const MaiError captured = manager->screenshot("owner", url, image.toUtf8().toStdString());
    if (captured) std::fprintf(stderr, "screenshot failed: %s\n", captured.message().c_str());
    CHECK(!captured && QFile::exists(image) && QFileInfo(image).size() > 100);
    const QString copy = qEnvironmentVariable("MAICHAT_BROWSER_TEST_SCREENSHOT");
    if (!copy.isEmpty()) {
        QFile::remove(copy);
        CHECK(QFile::copy(image, copy));
    }
    CHECK(!manager->close("owner"));
    CHECK(manager->observe("owner").error().code() == MaiErrorCode::NotFound);
    return 0;
}
