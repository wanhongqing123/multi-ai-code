#include <QApplication>
#include <QCoreApplication>
#include <QTimer>
#include <memory>

#include "app/RemoteIMApplication.h"
#include "im/RemoteIMCredentialDefaults.h"
#include "im/TencentUserSigGenerator.h"
#include "platform/DesktopRemoteIMClientFactory.h"
#include "ui/LoginDialog.h"
#include "ui/MainWindow.h"

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
    QApplication::setApplicationName(QStringLiteral("Multi-AI Code IM"));
    QApplication::setOrganizationName(QStringLiteral("Multi-AI Code"));

    const bool smokeMode = QCoreApplication::arguments().contains(QStringLiteral("--smoke"));

    LoginDialog loginDialog;
    if (smokeMode) {
        loginDialog.setUserId(QStringLiteral("desktop-im"));
        QTimer::singleShot(0, &loginDialog, &QDialog::accept);
    }
    if (loginDialog.exec() != QDialog::Accepted) {
        return 0;
    }

    RemoteIMApplication remoteIM(loginDialog.userId(), createDesktopRemoteIMClient());
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
