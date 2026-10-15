// 文件作用：将 OpenSSL 命令行能力封装为受控工具。
// 对外类型：MaiOpenSslCommandResult；线程、失败和生命周期以类型附近注释为准。
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "MaiTool.h"

// 执行已审计的内嵌 OpenSSL 命令行只读子集。上游程序使用进程全局 BIO 和选项状态，
// 因此调用必须串行；参数不包含前置的 "openssl"。
// 输入文件须在工具上下文内解析且不超过单命令上限。
// 结果包含真实退出码与有界标准输出／错误输出；不支持的子命令和选项在进入上游前拒绝。
struct MaiOpenSslCommandResult {
    int exitCode = 0;
    std::string standardOutput;
    std::string errorOutput;
    bool truncated = false;
};

MaiResult<MaiOpenSslCommandResult> maiRunOpenSslCommand(const std::vector<std::string>& arguments,
                                                        const MaiToolContext& context);

std::unique_ptr<MaiTool> makeMaiOpenSslCliTool();
