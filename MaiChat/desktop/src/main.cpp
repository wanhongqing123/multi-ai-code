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
#include "logging/AppLog.h"
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
    if (root.isEmpty()) root = QDir::homePath() + QStringLiteral("/.maichat-desktop");
    return QDir(root).filePath(QStringLiteral("RemoteIMHistory/") + userId + QStringLiteral("/messages.db"));
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

#ifdef Q_OS_WIN
    // 强制 QMediaPlayer 走 Windows Media Foundation。
    //
    // Qt5 在 Windows 上同时带 dsengine（DirectShow）和 wmfengine（WMF）两个后端，
    // 默认可能挑中 DirectShow——而 DirectShow 没有内置 H.264/AAC 解码器，播放
    // 手机录的 mp4 会失败：日志里是 DirectShowPlayerService::doRender 报
    // 0x80040266（VFW_E_UNSUPPORTED_STREAM），界面上只有一块黑屏。
    // 必须在构造 QApplication 之前设置，插件选择发生在那之前。
    if (qEnvironmentVariableIsEmpty("QT_MULTIMEDIA_PREFERRED_PLUGINS")) {
        qputenv("QT_MULTIMEDIA_PREFERRED_PLUGINS", "windowsmediafoundation");
    }
#endif

    QApplication app(argc, argv);
    // 独立应用身份：数据落 %APPDATA%\MaiChat\Desktop IM（macOS 同理在 Application Support 下），
    // 与其它应用的数据树分开。
    QApplication::setApplicationName(QStringLiteral("Desktop IM"));
    QApplication::setOrganizationName(QStringLiteral("MaiChat"));
#ifdef MAICHAT_VERSION
    QApplication::setApplicationVersion(QStringLiteral(MAICHAT_VERSION));
#endif

    // 必须在上面三行之后：日志目录是按组织名/应用名推导的。放在这里也保证了
    // 后续所有 qInfo/qWarning（含 Qt 自身的）都能落盘——GUI 子系统下 stderr
    // 没有去处，不接管的话双击启动等于完全没有日志。
    AppLog::install();

    // 接收图片/文件需要走 HTTPS 下载（QNetworkAccessManager），Qt 5.15 依赖 OpenSSL 1.1。
    // 未随包携带 libssl/libcrypto 时 supportsSsl() 为假，图片/文件下载会静默失败——
    // 表现为“文字能收、图片收不到”。此处给出明确告警便于定位。
    qInfo().noquote() << QStringLiteral("[im] OpenSSL supportsSsl=%1 build=%2")
                             .arg(QSslSocket::supportsSsl() ? QStringLiteral("true") : QStringLiteral("false"))
                             .arg(QSslSocket::sslLibraryBuildVersionString());
    if (!QSslSocket::supportsSsl()) {
        qWarning().noquote() << QStringLiteral(
            "[im] OpenSSL unavailable: received images/files cannot be downloaded. "
            "Ship libssl-1_1-x64.dll and libcrypto-1_1-x64.dll alongside the app.");
    }

#ifdef Q_OS_WIN
    // 对齐 Electron 端（--mac-font-sans：Inter/Segoe UI + Noto Sans SC/微软雅黑）：
    // Qt 在中文 Windows 上默认落到宋体（衬线观感），与远程 IM 抽屉的无衬线风格
    // 不一致。用 Segoe UI + 微软雅黑组合，像素字号与全局 QSS 的 px 体系保持一致。
    QFont appFont;
    appFont.setFamilies({QStringLiteral("Segoe UI"),
                         QStringLiteral("Microsoft YaHei UI"),
                         QStringLiteral("Microsoft YaHei")});
    // 登录窗保持设计尺寸（不随缩放倍率变化）：这里用基准 13px，
    // 进入主界面时才由 MainWindow 把全局字体切到倍率值。
    appFont.setPixelSize(13);
    app.setFont(appFont);
#endif

    const QStringList arguments = QCoreApplication::arguments();
    const bool smokeMode = arguments.contains(QStringLiteral("--smoke"));
    // --login <userId>：跳过登录页直接以该账号进入。远程桌面等功能需要同机
    // 双开两个账号联调，而 GUI 自动化填表在焦点竞争下并不可靠。
    QString autoLoginUserId;
    const int loginIndex = arguments.indexOf(QStringLiteral("--login"));
    if (loginIndex >= 0 && loginIndex + 1 < arguments.size()) {
        autoLoginUserId = arguments.at(loginIndex + 1).trimmed();
    }

    LoginDialog loginDialog;
    if (smokeMode) {
        loginDialog.setUserId(QStringLiteral("desktop-im"));
        QTimer::singleShot(0, &loginDialog, &QDialog::accept);
    } else if (!autoLoginUserId.isEmpty()) {
        loginDialog.setUserId(autoLoginUserId);
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
