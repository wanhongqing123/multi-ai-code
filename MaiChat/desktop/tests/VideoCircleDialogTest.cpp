#include "ui/VideoCircleDialog.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTabWidget>
#include <QThread>
#include <QTimer>

#include <curl/curl.h>

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

bool containsLabel(QWidget& widget, const QString& value) {
    for (QLabel* label : widget.findChildren<QLabel*>()) {
        if (label->text().contains(value)) return true;
    }
    return false;
}

bool waitFor(QWidget& widget, const QString& label, int timeoutMs = 6000) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < timeoutMs) {
        QApplication::processEvents();
        for (QLabel* candidate : widget.findChildren<QLabel*>()) {
            if (candidate->text().contains(label) && candidate->isVisible() &&
                candidate->size().width() > 0 && candidate->size().height() > 0) return true;
        }
        QThread::msleep(20);
    }
    return false;
}

bool waitForIdle(VideoCircleDialog& dialog) {
    auto* status = dialog.findChild<QLabel*>(QStringLiteral("videoCircleStatus"));
    if (!status) return false;
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < 8000) {
        QApplication::processEvents();
        if (status->text().isEmpty()) return true;
        QThread::msleep(20);
    }
    return false;
}

bool controlFixture(const QString& base, const QString& certificate, const char* action) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    const QByteArray url = (base + QStringLiteral("/test/") + QString::fromLatin1(action)).toUtf8();
    const QByteArray ca = certificate.toUtf8();
    curl_easy_setopt(curl, CURLOPT_URL, url.constData());
    curl_easy_setopt(curl, CURLOPT_CAINFO, ca.constData());
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
        +[](char*, size_t size, size_t count, void*) { return size * count; });
    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    return code == CURLE_OK && status == 200;
}

} // namespace

int main(int argc, char** argv) {
    require(argc == 5, "expected local TLS URL, two certificates and temp directory");
    QApplication app(argc, argv);
    QFile cert(QString::fromUtf8(argv[2]));
    require(cert.open(QIODevice::ReadOnly), "could not read fixture CA");
    const QString base = QString::fromUtf8(argv[1]);
    const auto defaultBases = desktopVideoCircleEndpointBases(
        QUrl(QStringLiteral("https://ichat.life/api/v1/")));
    require(defaultBases.first == QStringLiteral("https://ichat.life/api") &&
            defaultBases.second == QStringLiteral("https://ichat.life/maichat"),
            "default auth/media endpoint derivation changed");
    const auto proxyBases = desktopVideoCircleEndpointBases(
        QUrl(QStringLiteral("https://example.test/reverse/api/v1/")));
    require(proxyBases.first == QStringLiteral("https://example.test/reverse/api") &&
            proxyBases.second == QStringLiteral("https://example.test/reverse/maichat"),
            "reverse-proxy path prefix was lost");
    DesktopVideoCircleAccess viewer{base + QStringLiteral("/auth"),
        base + QStringLiteral("/media"), QStringLiteral("viewer-token"), cert.readAll(),
        QStringLiteral("localhost"), QStringLiteral("")};
    viewer.accountId = QStringLiteral("viewer");
    DesktopVideoCircleAccess author = viewer;
    author.accountToken = QStringLiteral("author-token");
    author.accountId = QStringLiteral("author");
    DesktopVideoCircleAccess expired = viewer;
    expired.accountToken = QStringLiteral("invalid-token");
    DesktopVideoCircleAccess disabled = viewer;
    disabled.authBaseUrl = base + QStringLiteral("/disabled");
    require(VideoCircleDialog::isAvailable(viewer), "viewer could not open the enabled feed");
    require(VideoCircleDialog::isAvailable(author), "author could not open the enabled feed");
    require(!VideoCircleDialog::isAvailable(expired), "invalid account could open the feed");
    require(!VideoCircleDialog::isAvailable(disabled), "404 feature-disabled feed was shown");
    const QString screenshots = qEnvironmentVariableIsSet("MAICHAT_VIDEO_CIRCLE_SCREENSHOT_DIR")
        ? qEnvironmentVariable("MAICHAT_VIDEO_CIRCLE_SCREENSHOT_DIR")
        : QString::fromUtf8(argv[4]);
    require(QDir().mkpath(screenshots), "could not create screenshot directory");

    int playbackCount = 0;
    QString playedPath;
    VideoCircleDialog dialog([viewer] { return viewer; }, [] { return QStringLiteral("viewer"); },
        [&playbackCount, &playedPath](const QString& path) {
            ++playbackCount;
            playedPath = path;
        });
    dialog.openBrowse();
    require(waitFor(dialog, QStringLiteral("还没有审核通过的视频")),
            "empty-feed screen was not rendered");
    QThread::msleep(80);
    QApplication::processEvents();
    for (QLabel* label : dialog.findChildren<QLabel*>()) {
        if (label->text().contains(QStringLiteral("还没有审核通过")))
            require(label->grab().save(QDir(screenshots).filePath(
                QStringLiteral("desktop-video-circle-empty-label.png"))),
                "could not save offscreen empty label");
    }
    QPixmap browse = dialog.grab();
    require(browse.save(QDir(screenshots).filePath(
        QStringLiteral("desktop-video-circle-empty.png"))), "could not save offscreen browse view");

    const QString fakeVideo = QDir(QString::fromUtf8(argv[4])).filePath(QStringLiteral("source.mp4"));
    QFile file(fakeVideo);
    require(file.open(QIODevice::WriteOnly), "could not create local preview file");
    require(file.write("fake video fixture") > 0, "could not write local preview file");
    file.close();
    const QString missingSource = QStringLiteral(
        R"({"tool":"agent_send_media","state":"completed","output":{"delivery":"current_ai_session","type":"video","mime_type":"video/mp4","path":"source.mp4","source_tool":"wan_video"}})");
    dialog.openPublish(missingSource, fakeVideo);
    require(dialog.findChild<QLabel*>(QStringLiteral("videoCircleStatus"))->text().contains(
                QStringLiteral("可验证")), "unsourced video was offered for publication");
    const QString completeCard = QStringLiteral(
        R"({"tool":"agent_send_media","state":"completed","output":{"delivery":"current_ai_session","type":"video","mime_type":"video/mp4","path":"source.mp4","source_tool":"wan_video","source_task_id":"task-1"}})");
    dialog.openPublish(completeCard, fakeVideo);
    QApplication::processEvents();
    require(dialog.findChild<QWidget*>(QStringLiteral("videoCirclePublishConfirm"))->isVisible(),
            "source-verified video did not open publication preview");
    require(containsLabel(dialog, QStringLiteral("发布者：viewer")),
            "publication preview omitted the account identity");
    QPixmap publishShot = dialog.grab();
    require(publishShot.save(QDir(screenshots).filePath(
        QStringLiteral("desktop-video-circle-publish.png"))), "could not save offscreen publish view");
    require(playbackCount == 0, "preview started playback without a click");
    dialog.close();

    const QString generated = QDir(QString::fromUtf8(argv[4])).filePath(QStringLiteral("generated.mp4"));
    require(QFile::copy(QStringLiteral(MAICHAT_VIDEO_CIRCLE_FIXTURE), generated),
            "could not copy generated-video fixture");
    VideoCircleDialog publisher([author] { return author; },
        [] { return QStringLiteral("author"); }, [](const QString&) {});
    publisher.openPublish(completeCard, generated);
    auto* titleInput = publisher.findChild<QLineEdit*>(QStringLiteral("videoCircleTitle"));
    require(titleInput != nullptr, "publication title input missing");
    titleInput->setText(QStringLiteral("本地测试片段"));
    QTimer::singleShot(0, [] {
        QWidget* modal = QApplication::activeModalWidget();
        require(modal != nullptr, "publication confirmation did not open");
        auto* confirm = modal->findChild<QPushButton*>(QStringLiteral("appMessageConfirm"));
        require(confirm != nullptr, "publication confirmation action missing");
        confirm->click();
    });
    auto* publish = publisher.findChild<QPushButton*>(QStringLiteral("videoCirclePublishConfirm"));
    require(publish != nullptr, "publication action missing");
    publish->click();
    const bool pendingShown = waitFor(publisher, QStringLiteral("本地测试片段"), 20000);
    if (!pendingShown) {
        auto* state = publisher.findChild<QLabel*>(QStringLiteral("videoCircleStatus"));
        std::cerr << "publish status: " << (state ? state->text().toStdString() : "missing") << '\n';
    }
    require(pendingShown,
            "marked video was not submitted into the author's pending list");
    require(containsLabel(publisher, QStringLiteral("待审核")),
            "publisher did not see the pending review state");
    require(publisher.findChild<QLabel*>(QStringLiteral("videoCircleReviewReason")) == nullptr,
            "null review reason was rendered in the pending state");
    require(publisher.grab().save(QDir(screenshots).filePath(
        QStringLiteral("desktop-video-circle-pending.png"))),
        "could not save offscreen pending view");
    publisher.close();

    dialog.openBrowse();
    require(waitForIdle(dialog) && containsLabel(dialog, QStringLiteral("还没有审核通过的视频")),
            "pending publication leaked into viewer feed");
    require(controlFixture(base, QString::fromUtf8(argv[2]), "review"),
            "fake moderator could not approve the pending video");
    dialog.openBrowse();
    require(waitFor(dialog, QStringLiteral("本地测试片段")),
            "approved video did not appear in the viewer feed");
    bool compactApprovedRow = false;
    for (QFrame* row : dialog.findChildren<QFrame*>(QStringLiteral("videoCircleFeedRow"))) {
        if (row->isVisible() && row->height() == 112) compactApprovedRow = true;
    }
    require(compactApprovedRow, "single approved video row stretched vertically");
    require(dialog.grab().save(QDir(screenshots).filePath(
        QStringLiteral("desktop-video-circle-approved.png"))),
        "could not save offscreen approved view");
    QPushButton* play = nullptr;
    for (QPushButton* candidate : dialog.findChildren<QPushButton*>()) {
        if (candidate->text() == QStringLiteral("播放") && candidate->isVisible()) {
            play = candidate;
            break;
        }
    }
    require(play != nullptr, "feed playback action missing");
    play->click();
    QElapsedTimer playbackTimer;
    playbackTimer.start();
    while (playbackTimer.elapsed() < 8000 && playbackCount == 0) {
        QApplication::processEvents();
        QThread::msleep(20);
    }
    require(playbackCount == 1 && QFileInfo(playedPath).isFile(),
            "viewer did not download via shared libcurl and open local playback");
    require(QFile::remove(playedPath), "test playback file was not cleaned up");
    require(controlFixture(base, QString::fromUtf8(argv[2]), "hide"),
            "fake moderator could not hide the approved post");
    publisher.openBrowse();
    require(waitFor(publisher, QStringLiteral("已下架")),
            "moderator-hidden video was mislabeled as withdrawn");
    require(containsLabel(publisher, QStringLiteral("运营下架")),
            "moderator review reason was not shown");
    require(publisher.grab().save(QDir(screenshots).filePath(
        QStringLiteral("desktop-video-circle-hidden.png"))),
        "could not save offscreen hidden view");
    require(controlFixture(base, QString::fromUtf8(argv[2]), "withdraw-cleanup-pending"),
            "could not configure cleanup-pending fixture");
    QPushButton* withdraw = nullptr;
    for (QPushButton* candidate : publisher.findChildren<QPushButton*>()) {
        if (candidate->text() == QStringLiteral("撤回") && candidate->isVisible()) {
            withdraw = candidate;
            break;
        }
    }
    require(withdraw != nullptr, "hidden author video could not be withdrawn");
    QTimer::singleShot(0, [] {
        QWidget* modal = QApplication::activeModalWidget();
        require(modal != nullptr, "withdrawal confirmation did not open");
        auto* confirm = modal->findChild<QPushButton*>(QStringLiteral("appMessageDanger"));
        require(confirm != nullptr, "withdrawal confirmation action missing");
        confirm->click();
    });
    withdraw->click();
    require(waitFor(publisher, QStringLiteral("文件清理中")),
            "202 cleanup-pending withdrawal was described as complete deletion");
    require(containsLabel(publisher, QStringLiteral("已撤回")),
            "withdrawn status was not reflected in My videos");
    publisher.close();

    require(controlFixture(base, QString::fromUtf8(argv[2]), "seed-many"),
            "could not seed a long feed");
    dialog.openBrowse();
    require(waitFor(dialog, QStringLiteral("第 35 个视频")),
            "long video feed did not render all rows");
    auto* tabs = dialog.findChild<QTabWidget*>(QStringLiteral("videoCircleTabs"));
    auto* feedArea = tabs ? qobject_cast<QScrollArea*>(tabs->widget(0)) : nullptr;
    require(feedArea && feedArea->verticalScrollBar()->maximum() > 0,
            "long feed did not become scrollable");
    QList<QFrame*> rows = dialog.findChildren<QFrame*>(QStringLiteral("videoCircleFeedRow"));
    require(rows.size() >= 35, "long feed rows were clipped or lost");
    for (QFrame* row : rows) {
        if (row->isVisible()) require(row->height() == 112, "long-feed row height differed from single row");
    }
    feedArea->ensureWidgetVisible(rows.last());
    QApplication::processEvents();
    require(dialog.grab().save(QDir(screenshots).filePath(
        QStringLiteral("desktop-video-circle-long-feed.png"))),
        "could not save offscreen long-feed view");
    publisher.openBrowse();
    require(publisher.isVisible(), "author dialog did not reopen");
    require(waitForIdle(publisher), "author's first My page did not load");
    auto* publisherTabs = publisher.findChild<QTabWidget*>(QStringLiteral("videoCircleTabs"));
    require(publisherTabs && publisherTabs->currentIndex() == 1,
            "author did not return to My videos");
    QPushButton* loadMine = nullptr;
    for (QPushButton* candidate : publisher.findChildren<QPushButton*>(
             QStringLiteral("videoCircleMineLoadMore"))) {
        if (candidate->isVisible()) { loadMine = candidate; break; }
    }
    if (!loadMine) {
        int videoRows = 0;
        int allRows = 0;
        for (QFrame* candidate : publisher.findChildren<QFrame*>(
                 QStringLiteral("videoCircleMineRow"))) {
            ++allRows;
            if (candidate->isVisible()) ++videoRows;
        }
        auto* stack = publisher.findChild<QStackedWidget*>();
        std::cerr << "mine rows=" << allRows << " visible=" << videoRows
                  << " tabs visible=" << publisherTabs->isVisible()
                  << " stack index=" << (stack ? stack->currentIndex() : -1)
                  << " status="
                  << publisher.findChild<QLabel*>(QStringLiteral("videoCircleStatus"))
                         ->text().toStdString() << '\n';
    }
    require(loadMine != nullptr, "older My videos had no pagination action");
    loadMine->click();
    require(waitFor(publisher, QStringLiteral("第 1 个视频")),
            "older My videos could not be loaded by cursor");
    publisherTabs->setCurrentIndex(0);
    require(waitForIdle(publisher), "author's public feed did not load");
    for (QPushButton* candidate : publisher.findChildren<QPushButton*>()) {
        require(!candidate->isVisible() || (candidate->text() != QStringLiteral("举报") &&
                candidate->text() != QStringLiteral("屏蔽作者")),
                "author was offered report or block on own post");
    }
    publisher.close();
    dialog.close();
    std::cout << "VideoCircleDialog local TLS/offscreen PASS\n";
    return 0;
}
