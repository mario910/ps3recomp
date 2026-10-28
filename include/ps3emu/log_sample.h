#ifndef PS3EMU_LOG_SAMPLE_H
#define PS3EMU_LOG_SAMPLE_H
/* Concurrent diagnostic sampling. Once saturated it is only a relaxed load;
 * no counter wrap, data race, or per-message FILE lock on the quiet path. */
static inline int ps3_log_sample(unsigned* counter,unsigned limit)
{
    unsigned value=__atomic_load_n(counter,__ATOMIC_RELAXED);
    while(value<limit)
        if(__atomic_compare_exchange_n(counter,&value,value+1,1,__ATOMIC_RELAXED,__ATOMIC_RELAXED))return 1;
    return 0;
}
#endif
