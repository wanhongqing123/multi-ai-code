#include "MaiChatTools/DesktopBrowserTools.h"

#include <QDir>
#include <QAbstractSocket>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QNetworkProxy>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>
#include <QEventLoop>
#include <QUuid>

#include <chrono>
#include <condition_variable>
#include <initializer_list>
#include <map>
#include <mutex>
#include <thread>

#include <json.hpp>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiTool.h"

namespace {

using Json = nlohmann::json;
constexpr int kCallTimeoutMs = 6000;

std::string utf8(const QString& value) {
    const QByteArray bytes = value.toUtf8();
    return std::string(bytes.constData(), std::size_t(bytes.size()));
}

QString browserProgram() {
#ifdef Q_OS_WIN
    const QString path = QStandardPaths::findExecutable(QStringLiteral("msedge.exe"));
    if (!path.isEmpty()) return path;
    for (const auto& base : {qEnvironmentVariable("ProgramFiles(x86)"),
                             qEnvironmentVariable("ProgramFiles")}) {
        const QString edge = QDir(base).filePath(QStringLiteral("Microsoft/Edge/Application/msedge.exe"));
        if (!base.isEmpty() && QFileInfo::exists(edge)) return edge;
        const QString chrome = QDir(base).filePath(QStringLiteral("Google/Chrome/Application/chrome.exe"));
        if (!base.isEmpty() && QFileInfo::exists(chrome)) return chrome;
    }
    return {};
#else
    for (const auto& program : {
            QStringLiteral("/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"),
            QStringLiteral("/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge")})
        if (QFileInfo::exists(program)) return program;
    return {};
#endif
}

bool allowedUrl(const std::string& raw, QString* host = nullptr) {
    if (raw.empty() || raw.size() > 2048 || raw.find('\\') != std::string::npos ||
        raw.find('\0') != std::string::npos) return false;
    const QUrl url(QString::fromUtf8(raw.c_str(), int(raw.size())));
    if (!url.isValid() || (url.scheme() != QStringLiteral("http") &&
                           url.scheme() != QStringLiteral("https")) ||
        url.host().isEmpty() || !url.userName().isEmpty() || !url.password().isEmpty())
        return false;
    if (host) *host = url.host().toLower();
    return true;
}

struct BrowserSession {
    std::string owner;
    QString program;
    std::unique_ptr<QTemporaryDir> profile;
    mutable std::mutex mutex;
    std::mutex callMutex;
    std::condition_variable changed;
    std::thread worker;
    QString pageSocket;
    QString allowedHost;
    std::string snapshotId;
    std::string observedUrl;
    std::map<std::string, std::string> observedLabels;
    int port = 0;
    bool ready = false;
    bool running = false;
    bool stopRequested = false;
    std::string startError;
};

void runBrowser(const std::shared_ptr<BrowserSession>& session) {
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.setProgram(session->program);
    process.setArguments({QStringLiteral("--no-first-run"),
        QStringLiteral("--no-default-browser-check"),
        QStringLiteral("--disable-sync"),
        QStringLiteral("--remote-debugging-port=0"),
        QStringLiteral("--user-data-dir=") + session->profile->path(),
        QStringLiteral("--new-window"), QStringLiteral("about:blank")});
    process.start();
    bool started = process.waitForStarted(5000);
    int port = 0;
    if (started) {
        const QString activePort = QDir(session->profile->path())
            .filePath(QStringLiteral("DevToolsActivePort"));
        for (int attempt = 0; attempt < 60; ++attempt) {
            QFile file(activePort);
            if (file.open(QIODevice::ReadOnly)) {
                bool parsed = false;
                port = file.readLine().trimmed().toInt(&parsed);
                if (parsed && port > 0) break;
            }
            if (process.state() == QProcess::NotRunning) break;
            process.waitForReadyRead(100);
            process.readAllStandardOutput();
        }
    }
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->port = port;
        session->ready = true;
        session->running = started && port > 0;
        if (!session->running)
            session->startError = started ? "browser debugging endpoint did not start" :
                utf8(process.errorString());
        session->changed.notify_all();
    }
    if (!started) return;
    while (process.state() != QProcess::NotRunning) {
        process.waitForReadyRead(100);
        process.readAllStandardOutput();
        bool stop = false;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            stop = session->stopRequested || port == 0;
        }
        if (stop) {
            process.terminate();
            if (!process.waitForFinished(1500)) {
                process.kill();
                process.waitForFinished(1500);
            }
        }
        process.waitForFinished(0);
    }
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->running = false;
        session->changed.notify_all();
    }
}

MaiResult<Json> httpPages(int port) {
    QNetworkAccessManager network;
    network.setProxy(QNetworkProxy::NoProxy);
    const QUrl url(QStringLiteral("http://127.0.0.1:%1/json/list").arg(port));
    QNetworkReply* reply = network.get(QNetworkRequest(url));
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(kCallTimeoutMs);
    if (!reply->isFinished()) loop.exec();
    if (!reply->isFinished()) {
        reply->abort();
        return {MaiErrorCode::Network, "browser endpoint timed out"};
    }
    if (reply->error() != QNetworkReply::NoError)
        return {MaiErrorCode::Network, utf8(reply->errorString())};
    const QByteArray bytes = reply->readAll();
    Json parsed = Json::parse(bytes.constData(), nullptr, false);
    if (!parsed.is_array()) return {MaiErrorCode::Protocol, "browser page list is invalid"};
    return parsed;
}

MaiResult<Json> cdp(const QString& socketUrl, const std::string& method,
                    const Json& parameters = Json::object()) {
    if (socketUrl.isEmpty()) return {MaiErrorCode::NotConfigured, "browser page is not ready"};
    QWebSocket socket;
    socket.setProxy(QNetworkProxy::NoProxy);
    QEventLoop connectLoop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&socket, &QWebSocket::connected, &connectLoop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &connectLoop, &QEventLoop::quit);
    timer.start(kCallTimeoutMs);
    socket.open(QUrl(socketUrl));
    if (socket.state() != QAbstractSocket::ConnectedState) connectLoop.exec();
    if (socket.state() != QAbstractSocket::ConnectedState)
        return {MaiErrorCode::Network, "browser page WebSocket did not connect"};

    QEventLoop resultLoop;
    QByteArray response;
    QObject::connect(&socket, &QWebSocket::textMessageReceived,
                     &resultLoop, [&](const QString& message) {
        const QByteArray bytes = message.toUtf8();
        const Json parsed = Json::parse(bytes.constData(), nullptr, false);
        if (parsed.is_object() && parsed.value("id", Json{}) == 1) {
            response = bytes;
            resultLoop.quit();
        }
    });
    QObject::connect(&timer, &QTimer::timeout, &resultLoop, &QEventLoop::quit);
    timer.start(kCallTimeoutMs);
    socket.sendTextMessage(QString::fromStdString(
        Json{{"id", 1}, {"method", method}, {"params", parameters}}.dump()));
    if (response.isEmpty()) resultLoop.exec();
    socket.close();
    if (response.isEmpty()) return {MaiErrorCode::Network, "browser command timed out"};
    Json result = Json::parse(response.constData(), nullptr, false);
    if (!result.is_object()) return {MaiErrorCode::Protocol, "browser command response is invalid"};
    if (result.contains("error"))
        return {MaiErrorCode::Protocol, result["error"].dump()};
    return result.value("result", Json::object());
}

MaiResult<std::string> evaluate(const QString& socket, const std::string& expression) {
    const auto result = cdp(socket, "Runtime.evaluate",
        Json{{"expression", expression}, {"returnByValue", true},
             {"awaitPromise", false}});
    if (!result) return result.error();
    if (!result.value().value("result", Json{}).is_object())
        return {MaiErrorCode::Protocol, "browser JavaScript result is missing"};
    const Json value = result.value()["result"].value("value", Json{});
    if (!value.is_string())
        return {MaiErrorCode::Protocol, "browser JavaScript did not return text"};
    return value.get<std::string>();
}

std::string snapshotScript(const std::string& nonce) {
    return "(() => {"
        "const visible=e=>{const r=e.getBoundingClientRect(),s=getComputedStyle(e);"
        "return r.width>0&&r.height>0&&s.visibility!=='hidden'&&s.display!=='none'};"
        "const label=e=>(e.getAttribute('aria-label')||e.innerText||"
        "e.getAttribute('placeholder')||e.getAttribute('name')||"
        "e.getAttribute('title')||e.tagName.toLowerCase()).trim().slice(0,160);"
        "const candidates=Array.from(document.querySelectorAll("
        "'a[href],button,input,textarea,select,[role=\"button\"]')).filter(visible).slice(0,80);"
        "const nonce=" + Json(nonce).dump() + ";"
        "const elements=candidates.map((e,i)=>{const id='mai_'+nonce+'_'+i;"
        "e.setAttribute('data-maichat-agent-id',id);return {id,label:label(e),"
        "tag:e.tagName.toLowerCase(),type:(e.getAttribute('type')||'').toLowerCase()}});"
        "return JSON.stringify({url:location.href,title:document.title,"
        "state:document.readyState,visibility:document.visibilityState,"
        "text:(document.body?.innerText||'').slice(0,20000),elements})})()";
}

std::string actionScript(const std::string& action, const std::string& url,
                         const std::string& id, const std::string& label,
                         const std::string& text) {
    return "(() => {if(document.visibilityState!=='visible')return 'hidden';"
        "if(location.href!==" + Json(url).dump() + ")return 'stale';"
        "const id=" + Json(id).dump() + ",expected=" + Json(label).dump() +
        ",action=" + Json(action).dump() + ";"
        "const e=Array.from(document.querySelectorAll('[data-maichat-agent-id]'))"
        ".find(item=>item.getAttribute('data-maichat-agent-id')===id);"
        "if(!e)return 'stale';const actual=(e.getAttribute('aria-label')||"
        "e.innerText||e.getAttribute('placeholder')||e.getAttribute('name')||"
        "e.getAttribute('title')||e.tagName.toLowerCase()).trim().slice(0,160);"
        "if(actual!==expected)return 'stale';"
        "if(action==='click'){setTimeout(()=>e.click(),0);return 'dispatched'}"
        "const type=(e.getAttribute('type')||'text').toLowerCase();"
        "if((e.tagName!=='INPUT'&&e.tagName!=='TEXTAREA')||"
        "['password','hidden','file','checkbox','radio','submit'].includes(type)||"
        "['current-password','new-password','one-time-code'].includes("
        "(e.getAttribute('autocomplete')||'').toLowerCase()))return 'unsupported';"
        "e.value=" + Json(text).dump() + ";"
        "e.dispatchEvent(new Event('input',{bubbles:true}));"
        "e.dispatchEvent(new Event('change',{bubbles:true}));return 'dispatched'})()";
}

} // namespace

struct DesktopBrowserManager::Impl {
    mutable std::mutex mutex;
    std::map<std::string, std::shared_ptr<BrowserSession>> sessions;

    std::shared_ptr<BrowserSession> find(const std::string& owner) const {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = sessions.find(owner);
        return found == sessions.end() ? nullptr : found->second;
    }
};

DesktopBrowserManager::DesktopBrowserManager() : impl_(std::make_unique<Impl>()) {}

DesktopBrowserManager::~DesktopBrowserManager() {
    std::map<std::string, std::shared_ptr<BrowserSession>> sessions;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        sessions.swap(impl_->sessions);
    }
    for (const auto& item : sessions) {
        std::lock_guard<std::mutex> lock(item.second->mutex);
        item.second->stopRequested = true;
    }
    for (const auto& item : sessions)
        if (item.second->worker.joinable()) item.second->worker.join();
}

MaiError DesktopBrowserManager::open(const std::string& sessionId,
                                     const std::string& rawUrl) {
    QString host;
    if (sessionId.empty() || !allowedUrl(rawUrl, &host))
        return {MaiErrorCode::InvalidInput, "open needs a Session and HTTP(S) URL without credentials"};
    const QString program = browserProgram();
    if (program.isEmpty())
        return {MaiErrorCode::NotConfigured, "Chrome or Edge is not installed for Desktop browser control"};
    const MaiError closed = close(sessionId);
    if (closed && closed.code() != MaiErrorCode::NotFound) return closed;
    auto session = std::make_shared<BrowserSession>();
    session->owner = sessionId;
    session->program = program;
    session->allowedHost = host;
    session->profile = std::make_unique<QTemporaryDir>(
        QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
            .filePath(QStringLiteral("MaiChatAgentBrowser-XXXXXX")));
    if (!session->profile->isValid())
        return {MaiErrorCode::Internal, "could not create an isolated browser profile"};
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->sessions.size() >= 2)
            return {MaiErrorCode::Busy, "close another Desktop Agent browser before opening a new one"};
        impl_->sessions.emplace(sessionId, session);
    }
    session->worker = std::thread([session] { runBrowser(session); });
    {
        std::unique_lock<std::mutex> lock(session->mutex);
        session->changed.wait_for(lock, std::chrono::seconds(8),
                                  [&session] { return session->ready; });
        if (!session->running) {
            const std::string error = session->startError.empty() ?
                "browser did not start" : session->startError;
            lock.unlock();
            close(sessionId);
            return {MaiErrorCode::NotConfigured, error};
        }
    }

    MaiResult<Json> pages(Json::array());
    for (int attempt = 0; attempt < 20; ++attempt) {
        pages = httpPages(session->port);
        if (pages && !pages.value().empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!pages) {
        const MaiError error = pages.error();
        close(sessionId);
        return error;
    }
    QString socket;
    for (const Json& page : pages.value()) {
        if (page.is_object() && page.value("type", "") == "page" &&
            page.value("webSocketDebuggerUrl", Json{}).is_string()) {
            socket = QString::fromStdString(page["webSocketDebuggerUrl"].get<std::string>());
            break;
        }
    }
    if (socket.isEmpty()) {
        close(sessionId);
        return {MaiErrorCode::Protocol, "browser has no controllable page"};
    }
    MaiError navigationError;
    {
        std::lock_guard<std::mutex> lock(session->callMutex);
        session->pageSocket = socket;
        const auto navigated = cdp(socket, "Page.navigate", Json{{"url", rawUrl}});
        if (!navigated || navigated.value().contains("errorText")) {
            navigationError = !navigated ? navigated.error() :
                MaiError(MaiErrorCode::Network, navigated.value()["errorText"].get<std::string>());
        } else if (navigated.value().value("isDownload", false)) {
            navigationError = {MaiErrorCode::NotSupported,
                               "browser downloads use the separate download_file tool"};
        }
    }
    if (navigationError) {
        close(sessionId);
        return navigationError;
    }
    return {};
}

MaiResult<std::string> DesktopBrowserManager::observe(const std::string& sessionId) {
    const auto session = impl_->find(sessionId);
    if (!session) return {MaiErrorCode::NotFound, "this Session has no Agent browser"};
    std::lock_guard<std::mutex> call(session->callMutex);
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (!session->running) return {MaiErrorCode::Busy, "browser has stopped"};
    }
    const std::string nonce = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    const auto result = evaluate(session->pageSocket, snapshotScript(nonce));
    if (!result) return result.error();
    Json snapshot = Json::parse(result.value(), nullptr, false);
    if (!snapshot.is_object() || !snapshot.value("url", Json{}).is_string() ||
        !snapshot.value("elements", Json{}).is_array())
        return {MaiErrorCode::Protocol, "browser snapshot is invalid"};
    const std::string url = snapshot["url"].get<std::string>();
    if (url == "about:blank" || snapshot.value("state", "") == "loading")
        return Json{{"state", "loading"}, {"url", url}}.dump();
    if (snapshot.value("visibility", "") != "visible")
        return {MaiErrorCode::Busy, "Agent browser tab is not visible; show it before continuing"};
    QString pageHost;
    if (!allowedUrl(url, &pageHost) || pageHost != session->allowedHost)
        return {MaiErrorCode::NotSupported,
                "page left the selected host; open its current URL before acting"};
    session->snapshotId = nonce;
    session->observedUrl = url;
    session->observedLabels.clear();
    for (const Json& element : snapshot["elements"]) {
        if (element.is_object() && element.value("id", Json{}).is_string() &&
            element.value("label", Json{}).is_string())
            session->observedLabels[element["id"].get<std::string>()] =
                element["label"].get<std::string>();
    }
    snapshot["state"] = "ready";
    snapshot["snapshot_id"] = nonce;
    return snapshot.dump();
}

MaiError DesktopBrowserManager::act(const std::string& sessionId,
    const std::string& action, const std::string& expectedUrl,
    const std::string& snapshotId, const std::string& elementId,
    const std::string& expectedLabel, const std::string& text) {
    if ((action != "click" && action != "type") || snapshotId.empty() ||
        elementId.empty() || expectedLabel.empty() || expectedUrl.empty() ||
        text.size() > 4000)
        return {MaiErrorCode::InvalidInput, "use an observed URL, snapshot and element label"};
    const auto session = impl_->find(sessionId);
    if (!session) return {MaiErrorCode::NotFound, "this Session has no Agent browser"};
    std::lock_guard<std::mutex> call(session->callMutex);
    const auto known = session->observedLabels.find(elementId);
    if (session->snapshotId != snapshotId || session->observedUrl != expectedUrl ||
        known == session->observedLabels.end() || known->second != expectedLabel)
        return {MaiErrorCode::InvalidInput, "browser snapshot changed; observe again"};
    const auto result = evaluate(session->pageSocket,
        actionScript(action, expectedUrl, elementId, expectedLabel, text));
    if (!result) return result.error();
    if (result.value() != "dispatched")
        return {MaiErrorCode::InvalidInput,
                "element changed or is a protected input; observe again"};
    session->snapshotId.clear();
    session->observedLabels.clear();
    return {};
}

MaiError DesktopBrowserManager::screenshot(const std::string& sessionId,
    const std::string& expectedUrl, const std::string& outputPath) {
    const auto session = impl_->find(sessionId);
    if (!session) return {MaiErrorCode::NotFound, "this Session has no Agent browser"};
    std::lock_guard<std::mutex> call(session->callMutex);
    if (expectedUrl.empty() || session->observedUrl != expectedUrl)
        return {MaiErrorCode::InvalidInput, "observe the current URL before a screenshot"};
    const auto actual = evaluate(session->pageSocket, "location.href");
    if (!actual) return actual.error();
    if (actual.value() != expectedUrl)
        return {MaiErrorCode::InvalidInput, "page URL changed; observe again"};
    const auto visibility = evaluate(session->pageSocket, "document.visibilityState");
    if (!visibility) return visibility.error();
    if (visibility.value() != "visible")
        return {MaiErrorCode::Busy, "Agent browser tab is not visible"};
    const auto captured = cdp(session->pageSocket, "Page.captureScreenshot",
        Json{{"format", "png"}, {"captureBeyondViewport", false}});
    if (!captured) return captured.error();
    if (!captured.value().value("data", Json{}).is_string())
        return {MaiErrorCode::Protocol, "browser screenshot data is missing"};
    const QByteArray encoded = QByteArray::fromStdString(
        captured.value()["data"].get<std::string>());
    const QByteArray png = QByteArray::fromBase64(encoded);
    if (png.size() < 8 || png.size() > 20 * 1024 * 1024 ||
        png.left(8) != QByteArray("\x89PNG\r\n\x1a\n", 8))
        return {MaiErrorCode::Protocol, "browser screenshot is not a bounded PNG"};
    QSaveFile file(QString::fromUtf8(outputPath.c_str(), int(outputPath.size())));
    if (!file.open(QIODevice::WriteOnly) || file.write(png) != png.size() ||
        !file.commit())
        return {MaiErrorCode::Internal, "could not save browser screenshot"};
    return {};
}

MaiError DesktopBrowserManager::close(const std::string& sessionId) {
    std::shared_ptr<BrowserSession> session;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto found = impl_->sessions.find(sessionId);
        if (found == impl_->sessions.end())
            return {MaiErrorCode::NotFound, "this Session has no Agent browser"};
        session = found->second;
        impl_->sessions.erase(found);
    }
    {
        std::lock_guard<std::mutex> call(session->callMutex);
        std::lock_guard<std::mutex> lock(session->mutex);
        session->stopRequested = true;
    }
    if (session->worker.joinable()) session->worker.join();
    return {};
}

namespace {

class DesktopBrowserTool final : public MaiTool {
public:
    explicit DesktopBrowserTool(std::shared_ptr<DesktopBrowserManager> manager)
        : manager_(std::move(manager)) {}
    std::string name() const override { return "desktop_browser"; }
    std::string description() const override {
        return "Operate one visible, isolated Chrome/Edge page owned by the current Desktop "
               "Session. open starts a fresh browser profile; observe returns bounded page text "
               "and elements; screenshot returns a PNG. For click/type, use the exact URL, "
               "snapshot_id, element_id and label from observe, then observe again to verify. "
               "Passwords/OTP inputs are excluded. Only the approved host can be observed or "
               "acted on. This does not control the user's existing browser profile. open, "
               "screenshot, click and type follow the Agent approval policy.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{)"
               R"("action":{"type":"string","enum":["open","observe","screenshot","click","type","close"]},)"
               R"("url":{"type":"string","maxLength":2048},)"
               R"("expected_url":{"type":"string","maxLength":2048},)"
               R"("snapshot_id":{"type":"string"},"element_id":{"type":"string"},)"
               R"("expected_label":{"type":"string"},)"
               R"("text":{"type":"string","maxLength":4000}},)"
               R"("required":["action"],"additionalProperties":false})";
    }
    bool requiresApproval(const std::string& raw) const override {
        return requiresPerCallApproval(raw);
    }
    bool requiresPerCallApproval(const std::string& raw) const override {
        const Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object() || !args.value("action", Json{}).is_string()) return true;
        const std::string action = args["action"].get<std::string>();
        return action != "observe" && action != "close";
    }
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "browser operation canceled");
        if (context.sessionId.empty())
            return MaiToolResult::failure(MaiErrorCode::NotConfigured,
                                          "a Desktop Session is required");
        const Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object() || !args.value("action", Json{}).is_string())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "browser action is required");
        auto exact = [&args](std::initializer_list<const char*> allowed) {
            for (auto item = args.begin(); item != args.end(); ++item) {
                bool found = false;
                for (const char* field : allowed) found |= item.key() == field;
                if (!found) return false;
            }
            return true;
        };
        const std::string action = args["action"].get<std::string>();
        if (action == "open") {
            if (!exact({"action", "url"}) || !args.value("url", Json{}).is_string())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "open needs a URL");
            const std::string url = args["url"].get<std::string>();
            const MaiError opened = manager_->open(context.sessionId, url);
            if (opened) return MaiToolResult::failure(opened.code(), opened.message());
            return MaiToolResult::success(Json{{"state", "loading"}, {"url", url},
                {"next_step", "Call observe after the page loads."}}.dump());
        }
        if (action == "observe") {
            if (!exact({"action"}))
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "observe has no options");
            const auto observed = manager_->observe(context.sessionId);
            if (!observed) return MaiToolResult::failure(observed.error().code(),
                                                          observed.error().message());
            return MaiToolResult::success(observed.value());
        }
        if (action == "close") {
            if (!exact({"action"}))
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "close has no options");
            const MaiError closed = manager_->close(context.sessionId);
            if (closed) return MaiToolResult::failure(closed.code(), closed.message());
            return MaiToolResult::success(R"({"state":"closed"})");
        }
        if (!args.value("expected_url", Json{}).is_string() ||
            args["expected_url"].get<std::string>().empty())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "use the URL from a current observation");
        const std::string expectedUrl = args["expected_url"].get<std::string>();
        if (action == "screenshot") {
            if (!exact({"action", "expected_url"}) || context.root.empty())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "screenshot needs the Session workspace");
            if (context.model == "glm-5.3")
                return MaiToolResult::failure(MaiErrorCode::NotSupported,
                                              "the current model cannot inspect screenshots");
            MaiFilePath directory = MaiFilePath::fromUtf8(context.root)
                .append(MaiFilePath::fromUtf8("browser"));
            const MaiError created = MaiFileSystem::createDirectories(directory);
            if (created) return MaiToolResult::failure(created.code(), created.message());
            const MaiFilePath path = directory.append(MaiFilePath::fromUtf8(
                MaiIdGenerator::generate("browser_") + ".png"));
            const MaiError saved = manager_->screenshot(context.sessionId,
                                                       expectedUrl, path.toUtf8());
            if (saved) return MaiToolResult::failure(saved.code(), saved.message());
            return MaiToolResult::successWithImages(
                Json{{"state", "captured"}, {"url", expectedUrl}}.dump(),
                {MaiToolImage{path.toUtf8(), "image/png"}});
        }
        if (action == "click" || action == "type") {
            if (!exact(action == "click" ?
                       std::initializer_list<const char*>{"action", "expected_url",
                                                          "snapshot_id", "element_id", "expected_label"} :
                       std::initializer_list<const char*>{"action", "expected_url",
                                                          "snapshot_id", "element_id", "expected_label", "text"}) ||
                !args.value("snapshot_id", Json{}).is_string() ||
                !args.value("element_id", Json{}).is_string() ||
                !args.value("expected_label", Json{}).is_string() ||
                (action == "type" && !args.value("text", Json{}).is_string()))
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "use an observed element and exact label");
            const MaiError acted = manager_->act(context.sessionId, action, expectedUrl,
                args["snapshot_id"].get<std::string>(),
                args["element_id"].get<std::string>(),
                args["expected_label"].get<std::string>(),
                action == "type" ? args["text"].get<std::string>() : std::string());
            if (acted) return MaiToolResult::failure(acted.code(), acted.message());
            return MaiToolResult::success(Json{{"state", "dispatched"}, {"action", action},
                {"verified", false}, {"next_step", "Call observe to verify the page state."}}.dump());
        }
        return MaiToolResult::failure(MaiErrorCode::InvalidInput, "unknown browser action");
    }

private:
    std::shared_ptr<DesktopBrowserManager> manager_;
};

} // namespace

void registerDesktopBrowserTools(MaiToolRegistry& registry,
    std::shared_ptr<DesktopBrowserManager> manager) {
    registry.add(std::make_unique<DesktopBrowserTool>(std::move(manager)));
}
