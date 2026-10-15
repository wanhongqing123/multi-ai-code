// 文件作用：提供应用私有存储的查询与文件操作工具。
// 对外类型：MaiAppStoragePaths；线程、失败和生命周期以类型附近注释为准。
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "MaiTool.h"

// 路径必须由宿主提供，且属于 App 自有目录；`workspace` 只报告，不参与清理。
// 只扫描普通文件，不跟随目录符号链接。清理须有近期预览 ID，
// 并重新校验每个文件的大小和修改时间；每次调用都须重新审批。
// 一个管理实例为每个 Agent 只保留一份预览，绝不删除目录。
struct MaiAppStoragePaths {
    std::string temporary;
    std::string cache;
    std::string workspace;
};

std::unique_ptr<MaiTool> makeMaiAppStorageTool(MaiAppStoragePaths paths);
