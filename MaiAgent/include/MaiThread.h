#pragma once

#include <string>

// 线程工具。
//
// 形状参考 chromium 的 base::PlatformThread
// （E:\OpenSource\chromium\src\base\threading\platform_thread.h），
// 只取这个项目现在真正需要的那一点。
class MaiThread {
public:
    // 给**当前**线程起名字。
    //
    // 这不是锦上添花：agent 是多会话并发的，一个进程里同时跑着好几个轮次
    // 的工作线程、SSE 连接线程、httplib 的接受线程。出问题时抓一个 dump，
    // 没有名字的话调试器里只有一串线程 ID，得靠调用栈一个个认。
    //
    // 各平台的落点：
    //   Windows  SetThreadDescription（Win10 1607+，动态取，老系统上自动
    //            降级成只在挂着调试器时有效的那套异常约定）
    //   Linux    prctl(PR_SET_NAME)，**上限 15 字节**，超了会被截断
    //   macOS    pthread_setname_np，上限 63 字节
    //
    // 所以名字要短。约定是 "mai-" 开头加一个用途，例如 "mai-turn"。
    static void setCurrentName(const std::string& name);

    // 取回刚才设的名字。拿不到就返回空串——
    // 这个只是给日志和测试用的，不要拿它做逻辑判断。
    static std::string currentName();
};
