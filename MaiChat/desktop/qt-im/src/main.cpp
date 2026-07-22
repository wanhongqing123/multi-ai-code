#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QSslSocket>
#include <QStandardPaths>
#include <QTimer>
#include <memory>

#include "app/RemoteIMApplication.h"
#include "im/RemoteIMCredentialDefaults.h"
#include "im/TencentUserSigGenerator.h"
#include "platform/DesktopRemoteIMClientFactory.h"
#include "storage/LocalMessageDatabase.h"
#include "ui/LoginDialog.h"
#include "ui/MainWindow.h"

namespace {

// 每账号一个本地消息库：登录后先从这里恢复全部历史（SDK 漫游只有几条，
// 降级为补充源）。目录约定与 TimSdk 缓存一致（AppDataLocation）。
QString messageDatabasePath(const QString& userId) {
    QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (root.isEmpty()) root = QDir::homePath() + QStringLiteral("/.multi-ai-im-desktop");
    return QDir(root).filePath(QStringLiteral("RemoteIMHistory/") + userId + QStringLiteral("/messages.db"));
}

// 早期版本以 "Multi-AI Code/Multi-AI Code IM" 身份存数据——与 Electron 主程序
//（productName "Multi-AI Code"）的品牌目录混在一起。两者是独立应用（Electron
// 内置 IM 以后也要有自己的消息存储），Desktop IM 迁到独立的
// "Multi-AI IM/Desktop IM"，老目录整体搬迁一次（含 SDK 缓存与消息库）。
// 跨平台：基准目录取自当前 AppDataLocation 的上两级（Windows=%APPDATA%，
// macOS=~/Library/Application Support），两端老数据都能搬。
void migrateLegacyAppData() {
    const QString current = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (current.isEmpty() || QDir(current).exists()) return;
    const QString orgDir = QFileInfo(current).absolutePath();          // <base>/Multi-AI IM
    const QString base = QFileInfo(orgDir).absolutePath();             // <base>
    const QString legacy = base + QStringLiteral("/Multi-AI Code/Multi-AI Code IM");
    if (!QDir(legacy).exists()) return;
    QDir().mkpath(orgDir);
    QDir().rename(legacy, current);
    // 老的组织目录若因此清空则顺手移除；他人（如 Electron）在用则 rmdir 失败，无副作用。
    QDir(base).rmdir(QStringLiteral("Multi-AI Code"));
}

}  // namespace

int main(int argc, char* argv[]) {
    // High-DPI 支持：必须在构造 QApplication 之前设置，否则平台插件初始化时读不到。
    // 不开启时，Qt5 在 Windows 上以逻辑分辨率绘制、再由系统按位图放大，
    // 在 125%/150%/200% 缩放的显示器上文字与边框会发虚（低分辨率观感）。
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    // PassThrough 保留 1.5 这类分数缩放因子，避免被取整成 1x/2x 造成布局突变或再次模糊。
    QApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#endif

    QApplication app(argc, argv);
    // 独立应用身份：数据树与 Electron 主程序（multi-ai-code / "Multi-AI Code"）
    // 分开，落在 %APPDATA%\Multi-AI IM\Desktop IM。
    QApplication::setApplicationName(QStringLiteral("Desktop IM"));
    QApplication::setOrganizationName(QStringLiteral("Multi-AI IM"));
    migrateLegacyAppData();

    // 接收图片/文件需要走 HTTPS 下载（QNetworkAccessManager），Qt 5.15 依赖 OpenSSL 1.1。
    // 未随包携带 libssl/libcrypto 时 supportsSsl() 为假，图片/文件下载会静默失败——
    // 表现为“文字能收、图片收不到”。此处给出明确告警便于定位。
    qInfo().noquote() << QStringLiteral("[remote-im] OpenSSL supportsSsl=%1 build=%2")
                             .arg(QSslSocket::supportsSsl() ? QStringLiteral("true") : QStringLiteral("false"))
                             .arg(QSslSocket::sslLibraryBuildVersionString());
    if (!QSslSocket::supportsSsl()) {
        qWarning().noquote() << QStringLiteral(
            "[remote-im] OpenSSL 不可用：接收到的图片/文件将无法下载显示，请随应用携带 "
            "libssl-1_1-x64.dll 与 libcrypto-1_1-x64.dll。");
    }

#ifdef Q_OS_WIN
    // 对齐 Electron 端（--mac-font-sans：Inter/Segoe UI + Noto Sans SC/微软雅黑）：
    // Qt 在中文 Windows 上默认落到宋体（衬线观感），与远程 IM 抽屉的无衬线风格
    // 不一致。用 Segoe UI + 微软雅黑组合，像素字号与全局 QSS 的 px 体系保持一致。
    QFont appFont;
    appFont.setFamilies({QStringLiteral("Segoe UI"),
                         QStringLiteral("Microsoft YaHei UI"),
                         QStringLiteral("Microsoft YaHei")});
    appFont.setPixelSize(13);
    app.setFont(appFont);
#endif

    const bool smokeMode = QCoreApplication::arguments().contains(QStringLiteral("--smoke"));

    LoginDialog loginDialog;
    if (smokeMode) {
        loginDialog.setUserId(QStringLiteral("desktop-im"));
        QTimer::singleShot(0, &loginDialog, &QDialog::accept);
    }
    if (loginDialog.exec() != QDialog::Accepted) {
        return 0;
    }

    RemoteIMApplication remoteIM(loginDialog.userId(),
                                 createDesktopRemoteIMClient(),
                                 std::make_unique<LocalMessageDatabase>(messageDatabasePath(loginDialog.userId())));
    MainWindow window(remoteIM);
    window.show();
    const QString userSig = TencentUserSigGenerator::generate(
        RemoteIMCredentialDefaults::sdkAppId,
        loginDialog.userId(),
        RemoteIMCredentialDefaults::secretKey()
    );
    remoteIM.connectToService(RemoteIMCredentialDefaults::sdkAppId, userSig);

    if (smokeMode) {
        QTimer::singleShot(300, &app, &QCoreApplication::quit);
    }

    return app.exec();
}
