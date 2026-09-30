#include "util/platform.h"

#include <time.h>

uint64_t os_gettime_ns(void)
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}
