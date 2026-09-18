#pragma once
#include <memory>
#include <string>

#include "MaiAgent.h"

struct MaiHttpAdapterOptions {
    std::string host = "127.0.0.1";
    int port = 0;  // 0 = 让系统选一个空闲端口，启动后用 port() 取实际值
};

// 把核心包成 REST + SSE，喂现有的 Electron 界面。
//
// **名字里是 Adapter 不是 Server，这是有意的。** 叫 MaiHttpServer 时
// 几乎每个人第一眼都以为"这个 agent 是个服务端"——不是。产物是
// maiagent 这个库，HTTP 只是为了让说不了 C++ 的调用方（现在的 Electron
// 界面是 JS 写的）能连上来而临时贴的一层壳。
//
// Qt 桌面 / iOS / Android / 嵌入式全都直接链 maiagent，一行 HTTP 都不过。
// Qt 界面就位之后这整个目录可以删掉，核心一行都不用改。
//
// 顺带注意别和 libcurl 搞混：核心里的 libcurl 是 HTTP **客户端**
// （去调大模型，摘不掉）；这里是 HTTP **服务端**（给界面连，可摘）。
// 两者方向相反。
//
// 这一层也是**唯一**把核心结构体转成 JSON 的地方。核心里全是原生结构体，
// 进来的请求在这里解析成 MaiOperation，出去的事件在这里序列化成 SSE。
class MaiHttpAdapter {
public:
    MaiHttpAdapter(MaiAgent& agent, MaiHttpAdapterOptions opts);
    ~MaiHttpAdapter();
    MaiHttpAdapter(const MaiHttpAdapter&) = delete;
    MaiHttpAdapter& operator=(const MaiHttpAdapter&) = delete;

    // bind 和阻塞循环必须分开：port=0 时端口是系统分配的，
    // 调用方要在开始阻塞之前就能拿到它（打印出来、写进文件给 UI 用）。
    bool bind();   // 成功后 port() 即可用
    bool serve();  // 阻塞直到 stop()
    void stop();

    int port() const;  // 实际监听到的端口
    std::string baseUrl() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
