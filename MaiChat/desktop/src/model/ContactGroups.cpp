// 文件作用：桌面端界面使用的数据模型；ContactGroups 负责联系人分组的数据和编辑流程，本文件给出其实现。
// 实现边界：与 ContactGroups 的公开声明配套，调用方通过相应接口进入。
#include "model/ContactGroups.h"

namespace ContactGroups {

QString normalize(const QString& raw) {
    return raw.trimmed();
}

bool isAcceptableName(const QString& normalized) {
    return !normalized.isEmpty();
}

}  // namespace ContactGroups
