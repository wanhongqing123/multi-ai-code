#include "remote/TrtcEngine.h"

#ifdef MAICHAT_HAVE_TRTC
#include <QCoreApplication>
#include <QMetaObject>

#include "ITRTCCloud.h"
#include "TRTCCloudCallback.h"
#endif

namespace RemoteDesktop {
namespace {

// SDK 不可用时的兜底：所有操作安全失败，调用方无需判空。
class NullTrtcEngine final : public ITrtcEngine {
public:
    void setRemoteVideoCallback(RemoteVideoCallback) override {}
    void setErrorCallback(ErrorCallback) override {}
    void setCustomMessageCallback(CustomMessageCallback) override {}
    bool sendCustomMessage(int, const QByteArray&, bool, bool) override { return false; }
    void bindRemoteView(const QString&, void*) override {}
    QString sdkVersion() const override { return QString(); }
    bool startScreenShare(const TrtcRoomParams&) override { return false; }
    bool startViewing(const TrtcRoomParams&, void*) override { return false; }
    void stop() override {}
    bool isActive() const override { return false; }
};

#ifdef MAICHAT_HAVE_TRTC

class TrtcEngine final : public ITrtcEngine, public liteav::ITRTCCloudCallback {
public:
    TrtcEngine() : cloud_(getTRTCShareInstance()) {
        if (cloud_) cloud_->addCallback(this);
    }

    ~TrtcEngine() override {
        stop();
        if (cloud_) cloud_->removeCallback(this);
        // 单例由 SDK 管理，必须走 destroy 而不是 delete。
        destroyTRTCShareInstance();
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

    bool sendCustomMessage(int cmdId, const QByteArray& payload, bool reliable,
                           bool ordered) override {
        if (!cloud_ || payload.isEmpty()) return false;
        return cloud_->sendCustomCmdMsg(static_cast<uint32_t>(cmdId),
                                        reinterpret_cast<const uint8_t*>(payload.constData()),
                                        static_cast<uint32_t>(payload.size()), reliable, ordered);
    }

    void bindRemoteView(const QString& userId, void* renderWindow) override {
        if (!cloud_ || userId.isEmpty()) return;
        const QByteArray id = userId.toUtf8();
        // 被控端推的是辅路（屏幕共享），这里必须订阅同一路，否则拿不到画面。
        liteav::TRTCRenderParams renderParams;
        renderParams.fillMode = liteav::TRTCVideoFillMode_Fit;
        cloud_->setRemoteRenderParams(id.constData(), liteav::TRTCVideoStreamTypeSub,
                                      renderParams);
        cloud_->startRemoteView(id.constData(), liteav::TRTCVideoStreamTypeSub,
                                static_cast<liteav::TXView>(renderWindow));
    }

    // ---- ITRTCCloudCallback：回调来自 SDK 线程，一律切回主线程再交给上层 ----

    void onError(TXLiteAVError code, const char* message, void*) override {
        const int errorCode = static_cast<int>(code);
        const QString text = message ? QString::fromUtf8(message) : QString();
        postToMainThread([this, errorCode, text] {
            if (errorCallback_) errorCallback_(errorCode, text);
        });
    }

    // 警告不影响会话继续，记录即可，不打扰用户。
    void onWarning(TXLiteAVWarning, const char*, void*) override {}

    void onExitRoom(int) override {}

    void onEnterRoom(int result) override {
        if (result >= 0) return;
        // 负值是错误码：进房失败要让上层知道，否则界面会一直停在"连接中"。
        postToMainThread([this, result] {
            if (errorCallback_) errorCallback_(result, QStringLiteral("进入房间失败"));
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

    QString sdkVersion() const override {
        if (!cloud_) return QString();
        const char* version = cloud_->getSDKVersion();
        return version ? QString::fromUtf8(version) : QString();
    }

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
        renderWindow_ = renderWindow;
        active_ = true;
        return true;
    }

    void stop() override {
        if (!cloud_ || !active_) return;
        cloud_->stopScreenCapture();
        cloud_->exitRoom();
        active_ = false;
        renderWindow_ = nullptr;
    }

    bool isActive() const override { return active_; }

private:
    // 选定「整屏」作为共享源。TRTC 要求 startScreenCapture 前先 select 目标，
    // 否则采集器没有源、不产出画面，对端会一直停在"等待画面"。
    bool selectPrimaryScreen() {
        if (!cloud_) return false;
        liteav::ITRTCScreenCaptureSourceList* sources =
            cloud_->getScreenCaptureSources(liteav::SIZE{0, 0}, liteav::SIZE{0, 0});
        if (!sources) return false;

        bool selected = false;
        int fallbackIndex = -1;
        for (uint32_t i = 0; i < sources->getCount(); ++i) {
            const liteav::TRTCScreenCaptureSourceInfo info = sources->getSourceInfo(i);
            if (info.type != liteav::TRTCScreenCaptureSourceTypeScreen) continue;
            if (fallbackIndex < 0) fallbackIndex = static_cast<int>(i);
            if (!info.isMainScreen) continue;
            // 空 RECT = 采集整个源，不做区域裁剪。
            const liteav::RECT captureRect{0, 0, 0, 0};
            liteav::TRTCScreenCaptureProperty property;
            property.enableCaptureMouse = true;
            // 不给被采集的屏幕加高亮描边：整屏共享时那圈边框只会干扰观看。
            property.enableHighLight = false;
            cloud_->selectScreenCaptureTarget(info, captureRect, property);
            selected = true;
            break;
        }
        if (!selected && fallbackIndex >= 0) {
            const liteav::TRTCScreenCaptureSourceInfo info =
                sources->getSourceInfo(static_cast<uint32_t>(fallbackIndex));
            const liteav::RECT captureRect{0, 0, 0, 0};
            liteav::TRTCScreenCaptureProperty property;
            property.enableCaptureMouse = true;
            property.enableHighLight = false;
            cloud_->selectScreenCaptureTarget(info, captureRect, property);
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
    void* renderWindow_ = nullptr;
    bool active_ = false;
    RemoteVideoCallback remoteVideoCallback_;
    ErrorCallback errorCallback_;
    CustomMessageCallback customMessageCallback_;
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

ITrtcEngine* createTrtcEngine() {
#ifdef MAICHAT_HAVE_TRTC
    return new TrtcEngine();
#else
    return new NullTrtcEngine();
#endif
}

}  // namespace RemoteDesktop
