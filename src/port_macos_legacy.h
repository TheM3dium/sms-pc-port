/* macOS before 10.12 has no clock_gettime: the build force-includes this when
 * CMAKE_OSX_DEPLOYMENT_TARGET is older (see CMakeLists.txt). */
#pragma once
#include <time.h>
#include <sys/time.h>
#include <mach/mach_time.h>

static inline int sms_clock_gettime(clockid_t clock, struct timespec* ts) {
    if (clock == CLOCK_REALTIME) {
        struct timeval tv;
        gettimeofday(&tv, 0);
        ts->tv_sec = tv.tv_sec;
        ts->tv_nsec = (long)tv.tv_usec * 1000;
        return 0;
    }
    static mach_timebase_info_data_t base;
    if (!base.denom) mach_timebase_info(&base);
    unsigned long long ns = mach_absolute_time() * base.numer / base.denom;
    ts->tv_sec = (time_t)(ns / 1000000000ull);
    ts->tv_nsec = (long)(ns % 1000000000ull);
    return 0;
}
#define clock_gettime sms_clock_gettime
