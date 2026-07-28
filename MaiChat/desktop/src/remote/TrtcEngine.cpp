#include "remote/TrtcEngine.h"

#ifdef MAICHAT_HAVE_TRTC
#include "ITRTCCloud.h"
#include "TRTCCloudDef.h"
#endif

namespace RemoteDesktop {
namespace {

// SDK 不可用时的兜底：所有操作安全失败，调用方无需判空。
class NullTrtcEngine final : public ITrtcEngine {
public:
    QString sdkVersion() const override { return QString(); }
    bool startScreenShare(const TrtcRoomParams&) override { return false; }
    bool startViewing(const TrtcRoomParams&, void*) override { return false; }
    void stop() override {}
    bool isActive() const override { return false; }
};

#ifdef MAICHAT_HAVE_TRTC

class TrtcEngine final : public ITrtcEngine {
public:
    TrtcEngine() : cloud_(getTRTCShareInstance()) {}

    ~TrtcEngine() override {
        stop();
        // 单例由 SDK 管理，必须走 destroy 而不是 delete。
        destroyTRTCShareInstance();
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
        // TXView 传 nullptr：采集整个主屏（一期不做窗口选择）。
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

    liteav::ITRTCCloud* cloud_ = nullptr;
    void* renderWindow_ = nullptr;
    bool active_ = false;
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
