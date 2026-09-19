#include "MaiSession.h"

bool MaiSession::isUntitled() const {
    // 用字面量相等来判断"还没命名过"是脆的：用户要是手动把标题就改成 kMaiDefaultSessionTitle，
    // 下一轮结束时会被自动起的标题覆盖掉。真要修得干净需要给 MaiSession 加一个显式标志位，
    // 那会动到线上契约，留给做持久化那一版一起改。
    return title.empty() || title == kMaiDefaultSessionTitle;
}
