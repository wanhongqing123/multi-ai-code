#include "model/ContactGroups.h"

namespace ContactGroups {

QString normalize(const QString& raw) {
    return raw.trimmed();
}

bool isAcceptableName(const QString& normalized) {
    return !normalized.isEmpty();
}

}  // namespace ContactGroups
