#include "MaiTime.h"

#include <chrono>

MaiMillis MaiTime::getCurrentTime() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}
