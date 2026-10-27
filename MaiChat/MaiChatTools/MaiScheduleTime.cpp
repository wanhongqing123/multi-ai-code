#include "MaiScheduleTime.h"

#include <ctime>
#include <string>

namespace {

int digits(const std::string& value, std::size_t start, std::size_t count) {
    int parsed = 0;
    for (std::size_t index = start; index < start + count; ++index) {
        const char character = value[index];
        if (character < '0' || character > '9') return -1;
        parsed = parsed * 10 + character - '0';
    }
    return parsed;
}

}  // namespace

MaiResult<std::int64_t> maiParseScheduledTime(const std::string& value) {
    if ((value.size() != 20 && value.size() != 25) ||
        value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
        value[13] != ':' || value[16] != ':')
        return {MaiErrorCode::InvalidInput,
                "time must use YYYY-MM-DDTHH:MM:SS with Z or an explicit offset"};
    const int year = digits(value, 0, 4);
    const int month = digits(value, 5, 2);
    const int day = digits(value, 8, 2);
    const int hour = digits(value, 11, 2);
    const int minute = digits(value, 14, 2);
    const int second = digits(value, 17, 2);
    if (year < 1970 || year > 2100 || month < 1 || month > 12 ||
        day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 ||
        minute > 59 || second < 0 || second > 59)
        return {MaiErrorCode::InvalidInput, "calendar date or time is invalid"};
    int offsetMinutes = 0;
    if (value.size() == 20) {
        if (value[19] != 'Z')
            return {MaiErrorCode::InvalidInput, "UTC time must end in Z"};
    } else {
        if ((value[19] != '+' && value[19] != '-') || value[22] != ':')
            return {MaiErrorCode::InvalidInput, "time zone offset is invalid"};
        const int offsetHours = digits(value, 20, 2);
        const int offsetMins = digits(value, 23, 2);
        if (offsetHours < 0 || offsetHours > 14 || offsetMins < 0 ||
            offsetMins > 59 || (offsetHours == 14 && offsetMins != 0))
            return {MaiErrorCode::InvalidInput, "time zone offset is invalid"};
        offsetMinutes = (offsetHours * 60 + offsetMins) *
            (value[19] == '+' ? 1 : -1);
    }
    std::tm calendar{};
    calendar.tm_year = year - 1900;
    calendar.tm_mon = month - 1;
    calendar.tm_mday = day;
    calendar.tm_hour = hour;
    calendar.tm_min = minute;
    calendar.tm_sec = second;
#if defined(_WIN32)
    const std::time_t seconds = _mkgmtime(&calendar);
#else
    const std::time_t seconds = timegm(&calendar);
#endif
    if (seconds == std::time_t(-1))
        return {MaiErrorCode::InvalidInput, "calendar time is out of range"};
    std::tm checked{};
#if defined(_WIN32)
    if (gmtime_s(&checked, &seconds) != 0)
        return {MaiErrorCode::InvalidInput, "calendar time is out of range"};
#else
    if (!gmtime_r(&seconds, &checked))
        return {MaiErrorCode::InvalidInput, "calendar time is out of range"};
#endif
    if (checked.tm_year != year - 1900 || checked.tm_mon != month - 1 ||
        checked.tm_mday != day || checked.tm_hour != hour ||
        checked.tm_min != minute || checked.tm_sec != second)
        return {MaiErrorCode::InvalidInput, "calendar date is invalid"};
    return std::int64_t(seconds) * 1000 - std::int64_t(offsetMinutes) * 60'000;
}
