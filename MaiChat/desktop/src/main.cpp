#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
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

#ifdef Q_OS_WIN
#include <shobjidl.h>
#endif
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

    // 文字渲染走 DirectWrite，替换默认的 GDI 字体引擎。GDI 引擎只有 8 级
    // 灰度抗锯齿，且在 150% 这类分数缩放下按浮点位置摆放字形（横向无
    // hinting），黑字明显发灰发糊；DirectWrite 的栅格化质量对齐 Electron/
    // 原生控件。仍然不是 ClearType 次像素渲染（Qt5 raster 管线做不到），
    // 但已是 Qt5 下能拿到的最好结果。注意：QSS 里的 font-weight 也别超过
    // 700——微软雅黑/Segoe UI 的真字面只到 Bold(700)，更高会触发合成假粗体，
    // 笔画交汇处断裂、粗细不均。必须与上面一样在 QApplication 之前设置。
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "windows:fontengine=directwrite");
    }
#endif

#ifdef Q_OS_WIN
    // 必须在 QApplication 之前：Windows 只在进程还没创建过窗口时接受这个设置。
    //
    // 没有它，系统托盘的通知在 Windows 10/11 上会被静默丢弃——代码照跑、日志照打、
    // 屏幕上什么都不出现。Windows 需要靠这个 id 把通知归属到某个"应用"，
    // 而这个 id 必须和开始菜单快捷方式上写的那一个完全一致（见 windows-installer.nsi）。
    SetCurrentProcessExplicitAppUserModelID(L"com.kongshang.maichat");
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

    // 字体栈对齐 Electron 端（MaiChatBuddy，maichatbuddy.css 的 --mcb-font-sans：
    // "Inter", "Noto Sans SC", "PingFang SC", "Microsoft YaHei"）。
    //
    // Inter/Noto/JetBrains Mono 都随包打进资源（:/maichat/fonts/，SIL OFL 1.1），
    // 不依赖系统装没装：Inter 拉丁字形的现代感是 Electron 观感的主要来源，
    // Noto Sans SC 补上中文的 Medium(500)——雅黑只有 400/700 两档，500 会触发
    // 假粗体；Noto 有真 Medium/Bold，QSS 与代码里设的字重都落在真实字面上。
    // Inter/JetBrains Mono 是 latin 子集，缺的字形（箭头、全角标点等）由列表
    // 后面的中文字体接住；雅黑/苹方垫底，字体注册万一失败也不至于开天窗。
    const QString bundledFonts[] = {
        QStringLiteral(":/maichat/fonts/Inter-latin-400.ttf"),
        QStringLiteral(":/maichat/fonts/Inter-latin-500.ttf"),
        QStringLiteral(":/maichat/fonts/Inter-latin-600.ttf"),
        QStringLiteral(":/maichat/fonts/Inter-latin-700.ttf"),
        QStringLiteral(":/maichat/fonts/NotoSansSC-Regular.otf"),
        QStringLiteral(":/maichat/fonts/NotoSansSC-Medium.otf"),
        QStringLiteral(":/maichat/fonts/NotoSansSC-Bold.otf"),
        QStringLiteral(":/maichat/fonts/JetBrainsMono-latin-400.ttf"),
        QStringLiteral(":/maichat/fonts/JetBrainsMono-latin-700.ttf"),
    };
    for (const QString& path : bundledFonts) {
        if (QFontDatabase::addApplicationFont(path) < 0) {
            qWarning().noquote() << QStringLiteral("[font] bundled font failed to load: %1").arg(path);
        }
    }

    QFont appFont;
    // 全局关闭字形提示（PreferNoHinting），这是和字体栈绑定的一条决策：
    //
    // Noto Sans SC 的 OTF 是 CFF 轮廓、**没有任何手工 hinting 指令**，DirectWrite
    // 对它只能自动 hinting——小字号粗体上会产生「断墨」伪影：横画中段墨色变浅、
    // 笔画宽度周期性波动，一眼看去笔画像断了。关掉 hinting 走纯抗锯齿后笔画
    // 连续均匀（边缘略软，macOS 一贯就是这种风格，可接受）。
    //
    // 注意这条的前史：GDI 引擎 + 雅黑时代这里曾试过 PreferVerticalHinting 修拉丁
    // 字距，结果全界面发虚，撤了；后来 markdown 里那条也在换 DirectWrite 后删了
    // （见 MarkdownLayout.cpp）。**hinting 的取舍跟着「引擎 × 字体」组合走**：
    //   GDI  + 雅黑（TrueType 手工 hinting） → 默认全提示最好
    //   DW   + 雅黑                        → 默认即可
    //   DW   + Noto CFF（无 hinting 指令）  → NoHinting 才不坏
    // 换字体栈或引擎时，重验三件事：字距、粗体笔画连续性、小字锐度。
    appFont.setHintingPreference(QFont::PreferNoHinting);
    appFont.setFamilies({QStringLiteral("Inter"),
                         QStringLiteral("Noto Sans SC"),
                         QStringLiteral("Microsoft YaHei UI"),
                         QStringLiteral("Microsoft YaHei"),
                         QStringLiteral("PingFang SC")});
    // 登录窗保持设计尺寸（不随缩放倍率变化）：这里用基准 13px，
    // 进入主界面时才由 MainWindow 把全局字体切到倍率值。
    appFont.setPixelSize(13);
    app.setFont(appFont);

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
