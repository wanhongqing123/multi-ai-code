#pragma once
#include <string>

#include "MaiSessionStore.h"

// 给还没命名的会话起个标题。
//
// 从 MaiTurnRunner 里分出来的理由是职责边界：给会话起名是**会话生命周期**的事，
// 不是"跑一轮对话"的事。它恰好发生在一轮结束时，但那只是时机，不是归属。
// 留在 MaiTurnRunner 里会让那个类越来越像个杂物间——M4 的权限挂起进来之后更明显。
//
// 单独成类还带来两个好处：起名策略以后要换（比如让模型总结一句），只动这里；而且它能被单独测，
// 不用跑一整轮对话。
class MaiSessionTitler {
public:
    struct Options {
        // 标题取用户第一句话的前多少字节。按字节切是因为要控制界面宽度，
        // 但切点会退到 UTF-8 字符边界——硬切会切出半个汉字。
        std::size_t maxBytes = 40;
    };

    MaiSessionTitler();
    explicit MaiSessionTitler(Options options);

    // 返回实际写入的标题；没改动（已有标题、或还没有用户消息）时返回空串。
    std::string apply(MaiSessionStore& store, const std::string& sessionId) const;

    // 暴露出来单独可测：截断逻辑是最容易写错的部分。
    std::string makeTitle(const std::string& text) const;

private:
    Options mOptions;
};
