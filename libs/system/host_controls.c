#include "ps3emu/host_controls.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef volatile LONG host_atomic_int;
static int host_load(host_atomic_int* p){return (int)InterlockedCompareExchange(p,0,0);}
static void host_store(host_atomic_int* p,int value){InterlockedExchange(p,(LONG)value);}
#else
typedef int host_atomic_int;
static int host_load(host_atomic_int* p){return __atomic_load_n(p,__ATOMIC_ACQUIRE);}
static void host_store(host_atomic_int* p,int value){__atomic_store_n(p,value,__ATOMIC_RELEASE);}
#endif

static host_atomic_int s_volume = 100;
static host_atomic_int s_frame_limit = -1;
static host_atomic_int s_fullscreen = -1;
int ps3_host_set_volume(unsigned value) {
    if(value>100)return 0;
    host_store(&s_volume,(int)value);return 1;
}
unsigned ps3_host_get_volume(void){return (unsigned)host_load(&s_volume);}
int ps3_host_set_frame_limit(unsigned value) {
    if(value!=0&&value!=30)return 0;
    host_store(&s_frame_limit,(int)value);return 1;
}
unsigned ps3_host_get_frame_limit(void){return (unsigned)host_load(&s_frame_limit);}
void ps3_host_set_fullscreen(int value){host_store(&s_fullscreen,value!=0);}
int ps3_host_get_fullscreen(void){return host_load(&s_fullscreen);}
void ps3_host_apply_volume(float* stereo,size_t frames,float* gain) {
    if(!stereo||!gain||!frames)return;
    const float target=(float)ps3_host_get_volume()/100.0f;
    if(*gain==1.0f&&target==1.0f)return; /* bit-preserving legacy/full-volume path */
    const float start=*gain;
    const float step=(target-start)/(float)frames;
    for(size_t i=0;i<frames;++i) {
        const float g=i+1==frames?target:start+step*(float)(i+1);
        stereo[i*2]*=g;stereo[i*2+1]*=g;
    }
    *gain=target;
}
/* One 64-bit word, so a reader never sees half a transition: running, it
 * holds the paused time to subtract; paused, the frozen value with
 * GUEST_FROZEN set. The frozen value sits a millisecond past the pause so a
 * reader that sampled the counter just before it cannot exceed it. */
#define GUEST_FROZEN (1ll<<62)
static host_atomic_int s_paused;
#ifdef _WIN32
static volatile LONG64 s_guest_clock;
static SRWLOCK s_pause_lock=SRWLOCK_INIT;
static int64_t host_counter(void){LARGE_INTEGER q;QueryPerformanceCounter(&q);return q.QuadPart;}
static int64_t host_counter_ms(void){LARGE_INTEGER f;QueryPerformanceFrequency(&f);return f.QuadPart/1000;}
static int64_t guest_clock_load(void){return InterlockedCompareExchange64(&s_guest_clock,0,0);}
static void guest_clock_store(int64_t v){InterlockedExchange64(&s_guest_clock,v);}
#define PAUSE_LOCK() AcquireSRWLockExclusive(&s_pause_lock)
#define PAUSE_UNLOCK() ReleaseSRWLockExclusive(&s_pause_lock)
#else
#include <pthread.h>
#include <time.h>
static int64_t s_guest_clock;
static pthread_mutex_t s_pause_lock=PTHREAD_MUTEX_INITIALIZER;
static int64_t host_counter(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000000000ll+t.tv_nsec;}
static int64_t host_counter_ms(void){return 1000000;}
static int64_t guest_clock_load(void){return __atomic_load_n(&s_guest_clock,__ATOMIC_ACQUIRE);}
static void guest_clock_store(int64_t v){__atomic_store_n(&s_guest_clock,v,__ATOMIC_RELEASE);}
#define PAUSE_LOCK() pthread_mutex_lock(&s_pause_lock)
#define PAUSE_UNLOCK() pthread_mutex_unlock(&s_pause_lock)
#endif
int64_t ps3_host_guest_counter(void) {
    const int64_t state=guest_clock_load(); /* before the counter: see above */
    if(state&GUEST_FROZEN)return state&~GUEST_FROZEN;
    return host_counter()-state;
}
void ps3_host_set_paused(int paused) {
    paused=paused!=0;
    PAUSE_LOCK();
    if(paused!=host_load(&s_paused)) {
        const int64_t state=guest_clock_load();
        if(paused) guest_clock_store(GUEST_FROZEN|(host_counter()+host_counter_ms()-state));
        else {
            const int64_t frozen=state&~GUEST_FROZEN;
            int64_t now=host_counter();
            while(now<frozen)now=host_counter(); /* only within a millisecond of pausing */
            guest_clock_store(now-frozen);
        }
        host_store(&s_paused,paused);
    }
    PAUSE_UNLOCK();
}
int ps3_host_get_paused(void){return host_load(&s_paused);}

uint64_t ps3_host_frame_deadline(ps3_host_pacer* p,uint64_t now,unsigned fps) {
    if(!p)return now;
    if(fps!=30){p->next_ns=0;p->fps=fps;return now;}
    const uint64_t period=1000000000ull/30;
    if(p->fps!=fps||!p->next_ns||now>=p->next_ns) {
        p->fps=fps;p->next_ns=now+period;return now;
    }
    const uint64_t due=p->next_ns;p->next_ns=due+period;return due;
}
