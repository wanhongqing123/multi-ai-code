#pragma once

#include <functional>
#include <memory>
#include <string>

#include "MaiTool.h"

// 密码只由平台安全输入页交给普通鉴权 API；参数、返回和 Agent 历史都不能承载密码。
// handler 在调用线程同步返回无凭据的 UI 请求状态，不等待用户输入，不得持有 context。
// open 必须绑定当前账号；status 只能查询同一账号的 request_id。页面实际出现后才能
// 返回 awaiting_user；仅普通 API 验证 password_set=true 后才能返回 changed。
using MaiChangePasswordToolHandler =
    std::function<MaiToolResult(const std::string& argumentsJson, const MaiToolContext& context)>;

// 三端共用输入校验、无密码 schema 和结果白名单。打开输入页本身不修改密码，故不另加审批；
// 用户仍须在原生安全表单输入并提交。未接宿主、取消或协议错误均不能误报已修改。
std::unique_ptr<MaiTool> makeMaiChangePasswordTool(MaiChangePasswordToolHandler handler);
