#pragma once

#include <memory>

#include "MaiTool.h"

// file_patch：一次提交多处、多文件的改动，**全有或全无**。
//
// 和 file_edit 的分工：file_edit 一次换一处，中间失败会留下改了一半的文件；
// 跨文件的改动（改接口同时改所有调用方）只有这个能做对。
std::unique_ptr<MaiTool> makeMaiApplyPatchTool();
