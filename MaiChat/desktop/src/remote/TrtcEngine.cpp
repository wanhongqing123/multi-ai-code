#include "remote/TrtcEngine.h"

#ifdef MAICHAT_HAVE_TRTC
#include <QCoreApplication>
#include <QDebug>
#include <QMetaObject>

#include "ITRTCCloud.h"
#include "TRTCCloudCallback.h"
#include "remote/TrtcNetworkProxyApi.h"
#endif

namespace RemoteDesktop {
namespace {

// SDK 不可用时的兜底：所有操作安全失败，调用方无需判空。
class NullTrtcEngine final : public ITrtcEngine {
public:
    void setRemoteVideoCallback(RemoteVideoCallback) override {}
    void setErrorCallback(ErrorCallback) override {}
    void setCustomMessageCallback(CustomMessageCallback) override {}
    void setRemoteVideoSizeCallback(RemoteVideoSizeCallback) override {}
    bool sendCustomMessage(int, const QByteArray&, bool, bool) override { return false; }
    void bindRemoteView(const QString&, void*) override {}
    QString sdkVersion() const override { return QString(); }
    bool startScreenShare(const TrtcRoomParams&) override { return false; }
    bool startViewing(const TrtcRoomParams&, void*) override { return false; }
    void stop() override {}
    bool isActive() const override { return false; }
};

#ifdef MAICHAT_HAVE_TRTC

// 屏幕采集接口用到的 SIZE / RECT 在两个平台上住在不同命名空间：
// Windows 版 SDK 直接用 windows.h 的全局 ::SIZE / ::RECT
//（见 ITRTCCloud.h:999 的 getScreenCaptureSources 签名）；
// macOS 没有 windows.h，SDK 自己在 liteav 里补了同名类型。
// 写死任何一边都会让另一边编译不过，所以在这里收敛成一组别名。
#ifdef Q_OS_WIN
using TrtcCaptureSize = ::SIZE;
using TrtcCaptureRect = ::RECT;
#else
using TrtcCaptureSize = liteav::SIZE;
using TrtcCaptureRect = liteav::RECT;
#endif

class TrtcEngine final : public ITrtcEngine, public liteav::ITRTCCloudCallback {
public:
    explicit TrtcEngine(const TrtcNetworkProxyConfig& proxy) {
        initializationError_ = configureNetworkProxy(proxy);
        if (!initializationError_.isEmpty()) {
            // 消息体本身是要显示给用户的中文，这里只补一个英文标签，
            // 便于在日志里定位与检索。
            qWarning().noquote() << QStringLiteral("[trtc] init failed: ") + initializationError_;
            return;
        }
        cloud_ = getTRTCShareInstance();
        if (!cloud_) {
            initializationError_ = QStringLiteral("TRTC SDK 初始化失败");
            return;
        }
        if (cloud_) cloud_->addCallback(this);
    }

    ~TrtcEngine() override {
        stop();
        if (cloud_) {
            cloud_->removeCallback(this);
            // 单例由 SDK 管理，必须走 destroy 而不是 delete。
            destroyTRTCShareInstance();
        }
    }

    void setRemoteVideoCallback(RemoteVideoCallback callback) override {
        remoteVideoCallback_ = std::move(callback);
    }

    void setErrorCallback(ErrorCallback callback) override {
        errorCallback_ = std::move(callback);
    }

    void setCustomMessageCallback(CustomMessageCallback callback) override {
        customMessageCallback_ = std::move(callback);
    }

    void setRemoteVideoSizeCallback(RemoteVideoSizeCallback callback) override {
        remoteVideoSizeCallback_ = std::move(callback);
    }

    bool sendCustomMessage(int cmdId, const QByteArray& payload, bool reliable,
                           bool ordered) override {
        if (!cloud_ || payload.isEmpty()) return false;
        return cloud_->sendCustomCmdMsg(static_cast<uint32_t>(cmdId),
                                        reinterpret_cast<const uint8_t*>(payload.constData()),
                                        static_cast<uint32_t>(payload.size()), reliable, ordered);
    }

    void bindRemoteView(const QString& userId, void* renderWindow) override {
        if (!cloud_ || userId.isEmpty() || renderWindow == nullptr) return;
        desiredRemoteUserId_ = userId;
        desiredRemoteRenderWindow_ = renderWindow;
        applyRemoteViewBinding();
    }

    // ---- ITRTCCloudCallback：回调来自 SDK 线程，一律切回主线程再交给上层 ----

    void onError(TXLiteAVError code, const char* message, void*) override {
        const int errorCode = static_cast<int>(code);
        const QString text = message ? QString::fromUtf8(message) : QString();
        // SDK 的错误此前只走 UI 弹窗，弹完就没了。落盘才追得回来。
        qWarning().noquote()
            << QStringLiteral("[trtc] error %1: %2").arg(errorCode).arg(text);
        postToMainThread([this, errorCode, text] {
            if (errorCallback_) errorCallback_(errorCode, text);
        });
    }

    // 警告不影响会话继续，记录即可，不打扰用户。
    void onWarning(TXLiteAVWarning code, const char* message, void*) override {
        qWarning().noquote()
            << QStringLiteral("[trtc] warning %1: %2")
                   .arg(static_cast<int>(code))
                   .arg(message ? QString::fromUtf8(message) : QString());
    }

    void onExitRoom(int) override {}

    void onEnterRoom(int result) override {
        postToMainThread([this, result] {
            if (result < 0) {
                // 负值是错误码：进房失败要让上层知道，否则界面会一直停在"连接中"。
                if (errorCallback_) errorCallback_(result, QStringLiteral("进入房间失败"));
                return;
            }
            enteredRoom_ = true;
            applyRemoteViewBinding();
        });
    }

    // 屏幕共享推的是辅路（Sub），可用性通知走 onUserSubStreamAvailable；
    // onUserVideoAvailable 只管主路摄像头，监听它永远等不到画面。
    void onUserSubStreamAvailable(const char* userId, bool available) override {
        const QString id = userId ? QString::fromUtf8(userId) : QString();
        postToMainThread([this, id, available] {
            if (remoteVideoCallback_) remoteVideoCallback_(id, available);
        });
    }

    // 首帧带着真实分辨率，是最早能拿到尺寸的时机——远程控制的坐标映射就等
    // 这个值，越早给越好，否则开控制的头几秒鼠标是偏的。
    void onFirstVideoFrame(const char* userId, const liteav::TRTCVideoStreamType streamType,
                           const int width, const int height) override {
        reportRemoteVideoSize(userId, streamType, width, height);
    }

    // 编码参数变化后尺寸会变（比如被控端换了分辨率），必须跟着更新，
    // 否则映射会一直用旧的宽高比。
    void onUserVideoSizeChanged(const char* userId, liteav::TRTCVideoStreamType streamType,
                                int newWidth, int newHeight) override {
        reportRemoteVideoSize(userId, streamType, newWidth, newHeight);
    }

    QString sdkVersion() const override {
        if (!cloud_) return QString();
        const char* version = cloud_->getSDKVersion();
        return version ? QString::fromUtf8(version) : QString();
    }

    QString initializationError() const override { return initializationError_; }

    bool startScreenShare(const TrtcRoomParams& params) override {
        if (!enterRoom(params)) return false;
        // 屏幕共享编码参数：分辨率跟随桌面，优先保清晰度（文字要能看清），
        // 帧率压到 10 fps 以省带宽——远程办公看的是静态界面，不是视频。
        liteav::TRTCVideoEncParam encParam;
        encParam.videoResolution = liteav::TRTCVideoResolution_1920_1080;
        encParam.resMode = liteav::TRTCVideoResolutionModeLandscape;
        encParam.videoFps = 10;
        encParam.videoBitrate = 1600;
        encParam.enableAdjustRes = true;

        // 必须先选定采集目标，否则 startScreenCapture 不会产出任何画面
        // （对端会一直停在"等待画面"）。这里选第一个「整屏」类型的源。
        if (!selectPrimaryScreen()) return false;

        cloud_->startScreenCapture(nullptr, liteav::TRTCVideoStreamTypeSub, &encParam);
        active_ = true;
        return true;
    }

    bool startViewing(const TrtcRoomParams& params, void* renderWindow) override {
        if (!enterRoom(params)) return false;
        Q_UNUSED(renderWindow);
        active_ = true;
        return true;
    }

    void stop() override {
        if (!cloud_) return;
        if (!subscribedRemoteUserId_.isEmpty()) {
            const QByteArray id = subscribedRemoteUserId_.toUtf8();
            cloud_->stopRemoteView(id.constData(), liteav::TRTCVideoStreamTypeSub);
        }
        if (active_) {
            cloud_->stopScreenCapture();
            cloud_->exitRoom();
        }
        active_ = false;
        enteredRoom_ = false;
        desiredRemoteUserId_.clear();
        desiredRemoteRenderWindow_ = nullptr;
        subscribedRemoteUserId_.clear();
        subscribedRemoteRenderWindow_ = nullptr;
    }

    bool isActive() const override { return active_; }

private:
    // 采集源的几何是排查坐标偏移的起点：被采集屏幕的宽高比决定了对端画面的
    // 宽高比，而注入又是按主屏像素算的。三者只要有一处对不上，鼠标就偏。
    static void logCaptureSource(const liteav::TRTCScreenCaptureSourceInfo& info,
                                 const QString& how) {
        qInfo().noquote()
            << QStringLiteral("[remote-input] host capture source: %1 name=\"%2\" "
                              "origin=(%3,%4) size=%5x%6 isMainScreen=%7")
                   .arg(how)
                   .arg(info.sourceName ? QString::fromUtf8(info.sourceName)
                                        : QStringLiteral("<null>"))
                   .arg(info.x)
                   .arg(info.y)
                   .arg(info.width)
                   .arg(info.height)
                   .arg(info.isMainScreen ? QStringLiteral("true") : QStringLiteral("false"));
    }

    // 只认辅路：被控端推的是屏幕共享（Sub），主路是摄像头，尺寸完全不相干，
    // 拿它去算黑边会把映射彻底带偏。
    void reportRemoteVideoSize(const char* userId, liteav::TRTCVideoStreamType streamType,
                               int width, int height) {
        if (streamType != liteav::TRTCVideoStreamTypeSub) return;
        if (width <= 0 || height <= 0) return;
        const QString id = userId ? QString::fromUtf8(userId) : QString();
        postToMainThread([this, id, width, height] {
            if (remoteVideoSizeCallback_) remoteVideoSizeCallback_(id, width, height);
        });
    }

    static QString configureNetworkProxy(const TrtcNetworkProxyConfig& config) {
        if (!config.enabled) return QString();

        const QString host = config.host.trimmed();
        if (host.isEmpty() || config.port == 0) {
            return QStringLiteral("TRTC SOCKS5 代理地址或端口无效");
        }

        ITXNetworkProxy* proxy = createTXNetworkProxy();
        if (!proxy) return QStringLiteral("TRTC SOCKS5 代理接口初始化失败");

        TRTCSocks5ProxyConfig capabilities;
        capabilities.support_https = true;
        capabilities.support_tcp = true;
        capabilities.support_udp = config.supportUdp;
        const QByteArray hostUtf8 = host.toUtf8();
        // 无认证代理必须传 nullptr。空字符串仍是非空指针，部分 SDK 版本会据此
        // 进入用户名/密码认证分支，和只接受 NO AUTH 的本地代理握手失败。
        const int result = proxy->setSocks5Proxy(hostUtf8.constData(), config.port,
                                                 nullptr, nullptr, &capabilities);
        destroyTXNetworkProxy(&proxy);
        if (result != 0) {
            return QStringLiteral("TRTC SOCKS5 代理设置失败（%1）").arg(result);
        }
        return QString();
    }

    void applyRemoteViewBinding() {
        if (!cloud_ || !enteredRoom_ || desiredRemoteUserId_.isEmpty()
            || desiredRemoteRenderWindow_ == nullptr) {
            return;
        }

        const QByteArray id = desiredRemoteUserId_.toUtf8();
        if (subscribedRemoteUserId_ == desiredRemoteUserId_) {
            if (subscribedRemoteRenderWindow_ != desiredRemoteRenderWindow_) {
                cloud_->updateRemoteView(
                    id.constData(), liteav::TRTCVideoStreamTypeSub,
                    static_cast<liteav::TXView>(desiredRemoteRenderWindow_));
                subscribedRemoteRenderWindow_ = desiredRemoteRenderWindow_;
            }
            return;
        }

        if (!subscribedRemoteUserId_.isEmpty()) {
            const QByteArray previousId = subscribedRemoteUserId_.toUtf8();
            cloud_->stopRemoteView(previousId.constData(), liteav::TRTCVideoStreamTypeSub);
        }

        // 被控端推的是辅路（屏幕共享），这里必须订阅同一路，否则拿不到画面。
        liteav::TRTCRenderParams renderParams;
        renderParams.fillMode = liteav::TRTCVideoFillMode_Fit;
        cloud_->setRemoteRenderParams(id.constData(), liteav::TRTCVideoStreamTypeSub,
                                      renderParams);
        cloud_->startRemoteView(id.constData(), liteav::TRTCVideoStreamTypeSub,
                                static_cast<liteav::TXView>(desiredRemoteRenderWindow_));
        subscribedRemoteUserId_ = desiredRemoteUserId_;
        subscribedRemoteRenderWindow_ = desiredRemoteRenderWindow_;
    }

    // 选定「整屏」作为共享源。TRTC 要求 startScreenCapture 前先 select 目标，
    // 否则采集器没有源、不产出画面，对端会一直停在"等待画面"。
    bool selectPrimaryScreen() {
        if (!cloud_) return false;
        liteav::ITRTCScreenCaptureSourceList* sources =
            cloud_->getScreenCaptureSources(TrtcCaptureSize{0, 0}, TrtcCaptureSize{0, 0});
        if (!sources) return false;

        bool selected = false;
        int fallbackIndex = -1;
        for (uint32_t i = 0; i < sources->getCount(); ++i) {
            const liteav::TRTCScreenCaptureSourceInfo info = sources->getSourceInfo(i);
            if (info.type != liteav::TRTCScreenCaptureSourceTypeScreen) continue;
            if (fallbackIndex < 0) fallbackIndex = static_cast<int>(i);
            if (!info.isMainScreen) continue;
            // 空 RECT = 采集整个源，不做区域裁剪。
            const TrtcCaptureRect captureRect{0, 0, 0, 0};
            liteav::TRTCScreenCaptureProperty property;
            property.enableCaptureMouse = true;
            // 不给被采集的屏幕加高亮描边：整屏共享时那圈边框只会干扰观看。
            property.enableHighLight = false;
            cloud_->selectScreenCaptureTarget(info, captureRect, property);
            logCaptureSource(info, QStringLiteral("main-screen"));
            selected = true;
            break;
        }
        if (!selected && fallbackIndex >= 0) {
            const liteav::TRTCScreenCaptureSourceInfo info =
                sources->getSourceInfo(static_cast<uint32_t>(fallbackIndex));
            const TrtcCaptureRect captureRect{0, 0, 0, 0};
            liteav::TRTCScreenCaptureProperty property;
            property.enableCaptureMouse = true;
            property.enableHighLight = false;
            cloud_->selectScreenCaptureTarget(info, captureRect, property);
            // 没找到主屏而退到第一个屏：注入是按主屏坐标算的，这两者不一致
            // 就必然偏，必须在日志里显式点出来。
            logCaptureSource(info, QStringLiteral("FALLBACK-not-main-screen"));
            selected = true;
        }
        sources->release();
        return selected;
    }

    bool enterRoom(const TrtcRoomParams& params) {
        if (!cloud_ || params.sdkAppId <= 0 || params.userId.isEmpty()
            || params.userSig.isEmpty() || params.roomId.isEmpty()) {
            return false;
        }
        const QByteArray userId = params.userId.toUtf8();
        const QByteArray userSig = params.userSig.toUtf8();
        const QByteArray roomId = params.roomId.toUtf8();

        liteav::TRTCParams trtcParams;
        trtcParams.sdkAppId = params.sdkAppId;
        trtcParams.userId = userId.constData();
        trtcParams.userSig = userSig.constData();
        trtcParams.strRoomId = roomId.constData();
        // 远程桌面是 1v1 且画面即内容，用视频通话场景（低延迟优先）。
        enteredRoom_ = false;
        cloud_->enterRoom(trtcParams, liteav::TRTCAppSceneVideoCall);
        return true;
    }

    void onRecvCustomCmdMsg(const char* userId, int32_t cmdId, uint32_t seq,
                            const uint8_t* message, uint32_t messageSize) override {
        Q_UNUSED(seq);  // 我们在自己的协议里带了序号，SDK 的序号不参与判定。
        if (message == nullptr || messageSize == 0) return;
        const QString id = userId ? QString::fromUtf8(userId) : QString();
        const QByteArray payload(reinterpret_cast<const char*>(message),
                                 static_cast<int>(messageSize));
        postToMainThread([this, id, cmdId, payload] {
            if (customMessageCallback_) customMessageCallback_(id, cmdId, payload);
        });
    }

    // SDK 回调在其内部线程触发，直接碰 Qt 控件会崩。统一投递到主线程。
    static void postToMainThread(std::function<void()> work) {
        QMetaObject::invokeMethod(qApp, [work = std::move(work)] { work(); },
                                  Qt::QueuedConnection);
    }

    liteav::ITRTCCloud* cloud_ = nullptr;
    bool active_ = false;
    bool enteredRoom_ = false;
    QString desiredRemoteUserId_;
    void* desiredRemoteRenderWindow_ = nullptr;
    QString subscribedRemoteUserId_;
    void* subscribedRemoteRenderWindow_ = nullptr;
    RemoteVideoCallback remoteVideoCallback_;
    ErrorCallback errorCallback_;
    CustomMessageCallback customMessageCallback_;
    RemoteVideoSizeCallback remoteVideoSizeCallback_;
    QString initializationError_;
};

#endif  // MAICHAT_HAVE_TRTC

}  // namespace

bool isTrtcAvailable() {
#ifdef MAICHAT_HAVE_TRTC
    return true;
#else
    return false;
#endif
}

ITrtcEngine* createTrtcEngine(const TrtcNetworkProxyConfig& proxy) {
#ifdef MAICHAT_HAVE_TRTC
    return new TrtcEngine(proxy);
#else
    Q_UNUSED(proxy);
    return new NullTrtcEngine();
#endif
}

}  // namespace RemoteDesktop
