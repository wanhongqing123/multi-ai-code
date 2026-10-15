// 文件作用：执行受控 SSH 连接与远程命令。
// 对外类型：MaiSshExecRequest、MaiSshExecResult；线程、失败和生命周期以类型附近注释为准。
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "MaiError.h"
#include "MaiTool.h"

// 一次非交互式 SSH 命令。密码是宿主临时输入，不进入模型工具 Schema 或持久化参数。
// 主机密钥是否可信由调用方决定；认证前验证回调会收到标准 SHA256:base64 指纹。
struct MaiSshExecRequest {
    std::string host;
    int port = 22;
    std::string username;
    std::string password;
    std::string command;
    int timeoutSeconds = 30;
};

struct MaiSshExecResult {
    std::string stdoutText;
    std::string stderrText;
    std::string hostFingerprint;
    int exitCode = 0;
    bool hasExitCode = false;
    bool timedOut = false;
    bool truncated = false;
};

using MaiSshPasswordProvider = std::function<MaiResult<std::string>(
    const std::string& host, int port, const std::string& username)>;
using MaiSshHostKeyVerifier = std::function<MaiResult<bool>(const std::string& host, int port,
                                                            const std::string& fingerprint)>;

// iOS、Android、Mac、PC 和 CLI 共用的 C++ 传输入口，通过 libcurl 建立直连 TCP，
// 再交给 libssh2 执行命令。标准输出与错误输出各限 48 KiB；网络等待期间检查取消。
// 超时返回已收到的部分输出并设置 timedOut；连接、认证、主机密钥错误返回 MaiError。
// 不同调用可以并发执行。
MaiResult<MaiSshExecResult> maiExecuteSsh(const MaiSshExecRequest& request,
                                          const MaiSshHostKeyVerifier& verifyHostKey,
                                          const std::atomic<bool>* cancel = nullptr);

// 模型工具封装要求宿主通过安全界面或凭据库提供密码和主机密钥验证回调；缺任一回调即拒绝。
// 同一 Agent 对话中十分钟内的命令可复用已认证连接，但各命令使用独立 SSH 通道，
// 不共享 Shell 状态。连接不跨进程重启持久化；每条远程命令仍须单独审批。
std::unique_ptr<MaiTool> makeMaiSshTool(MaiSshPasswordProvider passwordProvider,
                                        MaiSshHostKeyVerifier verifyHostKey);
