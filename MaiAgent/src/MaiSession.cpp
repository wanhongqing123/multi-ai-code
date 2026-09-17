#include "MaiSession.h"

bool MaiSession::isUntitled() const {
    return title.empty() || title == "新会话";
}
