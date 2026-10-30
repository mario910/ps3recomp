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
uint64_t ps3_host_frame_deadline(ps3_host_pacer* p,uint64_t now,unsigned fps) {
    if(!p)return now;
    if(fps!=30){p->next_ns=0;p->fps=fps;return now;}
    const uint64_t period=1000000000ull/30;
    if(p->fps!=fps||!p->next_ns||now>=p->next_ns) {
        p->fps=fps;p->next_ns=now+period;return now;
    }
    const uint64_t due=p->next_ns;p->next_ns=due+period;return due;
}
