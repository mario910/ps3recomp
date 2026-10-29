#ifndef CELL_CAMERA_PATTERN_H
#define CELL_CAMERA_PATTERN_H
#include <stdint.h>
#include <string.h>
/* Same RAW8/GRBG image as the original pixel loop. Cache only the immutable
 * background, never the caller-visible frame. TLS avoids initialization races. */
static inline void camera_pattern_fill(unsigned char* dst,uint32_t frame)
{
    static _Thread_local unsigned char background[640*480];
    static _Thread_local int ready;
    if(!ready){
        for(unsigned y=0;y<480;++y)for(unsigned x=0;x<640;++x){
            unsigned r=x*255/640,g=y*255/480,b=255-r;
            background[y*640+x]=(unsigned char)((y&1)?((x&1)?g:b):((x&1)?r:g));
        }
        ready=1;
    }
    memcpy(dst,background,sizeof background);
    unsigned bx=(frame*5u)%(640-96),by=(frame*3u)%(480-96);
    for(unsigned y=by;y<by+96;++y)memset(dst+y*640+bx,250,96);
}
#endif
