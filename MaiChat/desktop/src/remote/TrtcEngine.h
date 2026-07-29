#pragma once

#include <QByteArray>
#include <QString>
#include <functional>

// TRTC 的薄封装。
//
// 存在的意义有两个：
//  1. 把 SDK 细节挡在一层接口后面，controller 与状态机可以用 fake 完整单测，
//     不需要真实网络/房间/摄像头；
//  2. 未 vendoring SDK 的平台（当前是 macOS/Linux）编译时自动落到空实现，
//     其余功能不受影响。
namespace RemoteDesktop {

// 进房参数。UserSig 与 IM 共用同一套生成逻辑（TencentUserSigGenerator）。
struct TrtcRoomParams {
    int sdkAppId = 0;
    QString userId;
    QString userSig;
    QString roomId;   // 字符串房间号，形如 mc-<发起方>-<随机>
};

class ITrtcEngine {
public:
    // 远端画面可用性变化。SDK 回调来自其内部线程，实现方负责切回主线程，
    // 调用方可以直接碰 UI。
    using RemoteVideoCallback = std::function<void(const QString& userId, bool available)>;
    // 进房/推流失败。空实现平台不会触发。
    using ErrorCallback = std::function<void(int code, const QString& message)>;
    // 收到对端自定义消息（远程输入）。同样已切回主线程。
    using CustomMessageCallback =
        std::function<void(const QString& userId, int cmdId, const QByteArray& payload)>;

    virtual ~ITrtcEngine() = default;

    virtual void setRemoteVideoCallback(RemoteVideoCallback callback) = 0;
    virtual void setErrorCallback(ErrorCallback callback) = 0;
    virtual void setCustomMessageCallback(CustomMessageCallback callback) = 0;

    // 发送自定义消息（远程输入）。返回 false 表示 SDK 拒发——多半是撞了
    // 配额，调用方自己已按更严的滑动窗口约束过，正常不该出现。
    //
    // ⚠️ 必须固定在同一线程调用：SDK 的限流计数器是裸成员、无任何同步
    //（trtc_message_sender.h:45），多线程调进去是数据竞争。统一走主线程。
    virtual bool sendCustomMessage(int cmdId, const QByteArray& payload, bool reliable,
                                   bool ordered) = 0;

    // 把远端画面绑定到原生窗口句柄。收到 RemoteVideoCallback(available=true)
    // 之后调用；渲染窗口可能晚于进房才创建，故与 startViewing 分开。
    virtual void bindRemoteView(const QString& userId, void* renderWindow) = 0;

    // SDK 版本号；取不到返回空串。用于启动自检与问题上报。
    virtual QString sdkVersion() const = 0;

    // 被控端：进房并开始推屏。windowHandle 传 nullptr 表示采集整个主屏。
    virtual bool startScreenShare(const TrtcRoomParams& params) = 0;

    // 控制端：进房并订阅远端屏幕流，渲染到给定原生窗口句柄。
    virtual bool startViewing(const TrtcRoomParams& params, void* renderWindow) = 0;

    // 两端通用：离房并停止一切采集/渲染。可重复调用。
    virtual void stop() = 0;

    virtual bool isActive() const = 0;
};

// 是否编译进了真实 TRTC 实现。false 时 createTrtcEngine 返回空实现，
// UI 应据此隐藏或禁用远程桌面入口。
bool isTrtcAvailable();

// 创建引擎实例。SDK 不可用时返回一个所有操作都失败的空实现，
// 而不是 nullptr——调用方不必到处判空。
ITrtcEngine* createTrtcEngine();

}  // namespace RemoteDesktop
