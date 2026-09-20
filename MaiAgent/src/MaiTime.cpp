#include "MaiTime.h"

#include <chrono>
#include <cstdio>
#include <ctime>

MaiMillis MaiTime::getCurrentTime() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

std::string MaiTime::formatIso8601Utc(MaiMillis millis) {
    const std::time_t seconds = static_cast<std::time_t>(millis / 1000);
    std::tm broken{};
    // gmtime 返回的是一块**共享的静态缓冲**，两个线程同时调会互相踩。
    // 两个平台的线程安全版本名字和参数顺序都不一样，所以这里分开写。
#if defined(_WIN32)
    ::gmtime_s(&broken, &seconds);
#else
    ::gmtime_r(&seconds, &broken);
#endif
    char text[32] = {0};
    std::snprintf(text, sizeof(text), "%04d-%02d-%02dT%02d:%02d:%02dZ", broken.tm_year + 1900,
                  broken.tm_mon + 1, broken.tm_mday, broken.tm_hour, broken.tm_min, broken.tm_sec);
    return std::string(text);
}
