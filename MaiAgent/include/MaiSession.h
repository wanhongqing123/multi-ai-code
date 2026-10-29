// 文件作用：定义 Agent 会话及其持久化状态。
// 对外类型：MaiSession；线程、失败和生命周期以类型附近注释为准。
#pragma once

#include <string>

#include "MaiError.h"
#include "MaiTime.h"

// 新建会话在被真正命名之前用的占位标题。
//
// 这里放英文而不是"新会话"：核心是个要被链进各种壳的库，本地化是界面的事。把中文写死在库里，
// 等于所有壳都被迫说中文。
//
// 收成常量是因为 isUntitled() 要拿它做相等比较，两处各写一份字面量的话，
// 改其中一处就会让"还没命名"这个判断静默失效。
inline constexpr const char* kMaiDefaultSessionTitle = "New session";

// 显式会话名称与现有跨设备 presence 标题边界一致：最多 120 个 Unicode 标量、480 个 UTF-8 字节。
// 不做截断或 trim；拒绝空/全 Unicode 空白、坏 UTF-8、C0/C1 控制字符和 Unicode 换行。
// 纯校验可在任何线程并发调用，不访问会话、文件或存储；失败返回 InvalidInput。
// MaiUpdateSession.title 的空串仍表示“不改”，普通入口应只对非空更新调用本校验。
MaiError maiValidateSessionTitle(const std::string& title);

// 一个会话。纯数据：它不知道自己怎么被存、怎么被跑。
struct MaiSession {
    std::string id;  // ses_...
    std::string title;
    std::string directory;
    std::string model;  // 空表示用 MaiAgent 的默认模型
    std::string agent = "build";
    // 把这个会话起起来的那个会话。空表示它是用户自己开的（根会话）。
    //
    // 子 Agent 就是一个会话，不是别的什么东西——它照样有自己的消息、自己的一轮、
    // 自己的工作线程。父子关系只是一个字段。这是 codex 的做法：
    // 那边一个 sub-agent 也就是一个 thread。
    std::string parentId;
    // 离根会话有多远。根是 0，它的孩子是 1，以此类推。
    //
    // 单独存一个数而不是每次顺着 parentId 往上数：数的话每次都要查几次库，
    // 而这个值在创建之后永远不会变。**上限判断靠它**，别让它和 parentId 不一致。
    int depth = 0;

    MaiMillis created = 0;
    MaiMillis updated = 0;

    // 显式改名与占位字符串分开保存；用户也可以真的把会话叫作 New session。
    // 由普通 MaiUpdateSession 的非空标题更新置位，清空会话历史并重置标题时复位。
    // 必须持久化，否则同轮收尾或 App 重启后的自动起名会覆盖用户指定的有效名称。
    bool titleEdited = false;

    // 标题是不是还没被真正命名过。第一轮结束后会用用户那句话填上。
    bool isUntitled() const;

    // 是不是用户自己开的那种会话。子 Agent 不该出现在会话列表里——
    // 用户没开过它们，列出来只会让人以为自己漏了什么。
    bool isRoot() const;
};
