#ifndef PS3_TIMER_SHORT_SPIN_H
#define PS3_TIMER_SHORT_SPIN_H
#include <stdint.h>
#include <limits.h>
/* Opt-in, bounded micro-sleep tail. A zero/invalid threshold preserves the
 * existing scheduler-yield path. Never shorten or skip the requested delay. */
static inline int64_t ps3_short_sleep_ticks(uint64_t usec,uint64_t frequency,unsigned threshold)
{
    if(!threshold || threshold>100 || !usec || usec>threshold || !frequency || frequency>1000000000000ull)return 0;
    return (int64_t)((usec*frequency+999999u)/1000000u);
}
#ifdef _WIN32
#include <windows.h>
static inline int ps3_short_sleep_spin(uint64_t usec,int64_t frequency,unsigned threshold)
{
    int64_t ticks=frequency>0?ps3_short_sleep_ticks(usec,(uint64_t)frequency,threshold):0;
    if(!ticks)return 0;
    LARGE_INTEGER begin,now;
    if(!QueryPerformanceCounter(&begin) || begin.QuadPart<0 || begin.QuadPart>INT64_MAX-ticks)return 0;
    const int64_t deadline=begin.QuadPart+ticks;
    do {YieldProcessor();if(!QueryPerformanceCounter(&now))return 0;}while(now.QuadPart<deadline);
    return 1;
}
#endif
#endif
