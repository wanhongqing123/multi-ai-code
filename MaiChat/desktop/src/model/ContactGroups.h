#pragma once

#include <QString>

// 联系人分组的命名规则。
//
// 单独拎出来是因为这套规则三端必须一致（Qt / iOS / Android），而且它有实际后果：
// 规则不一致会让同一个人在两个端上看到不同的分组结构。集中在一处也便于单测。
//
// 界面上没有「未分组」这一节：没有分组的联系人就直接列在分组的同一层，
// 不套任何标题。因此这里也不需要保留名——分组名只要非空就行。
namespace ContactGroups {

// 去掉首尾空白。用户在输入框里多打一个空格不该产生两个看起来一样的分组。
QString normalize(const QString& raw);

// 归一化之后能否作为分组名。重名检查不在这里——那要看现有分组列表，是调用方的事。
bool isAcceptableName(const QString& normalized);

}  // namespace ContactGroups
