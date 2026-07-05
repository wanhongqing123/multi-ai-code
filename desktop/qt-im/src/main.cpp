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
