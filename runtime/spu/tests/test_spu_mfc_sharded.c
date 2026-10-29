/* Production engine ownership plus explicit cross-bucket concurrency.
 * The control and sharded variants use the same engines and lookup code. */
#define main previous_registry_main
#include "test_spu_mfc_slots.c"
#undef main

typedef struct {spu_context* ctx;volatile LONG done;unsigned loops;int bench;} ShardRequest;
static volatile LONG bench_arrived,bench_go;
static void* shard_worker(void* opaque)
{
    ShardRequest* r=opaque;
    if(r->bench){InterlockedIncrement(&bench_arrived);while(!bench_go)YieldProcessor();}
    for(unsigned n=0;n<r->loops;++n){
        mfc_engine* engine=mfc_for(r->ctx);
        if(engine->queue_count || engine->tag_completed!=UINT32_MAX)abort();
        engine->queue_count=7;engine->tag_completed=n;
        spu_mfc_release(r->ctx);
    }
    InterlockedExchange(&r->done,1);return NULL;
}
static int shard_wait(volatile LONG* done,unsigned ms)
{
    ULONGLONG begin=GetTickCount64();
    while(!*done){if(GetTickCount64()-begin>ms)return 0;Sleep(0);}return 1;
}
static void independent_bucket_case(int sharded)
{
    CHECK(fixture_mfc_nodes()==0);s_mfc_sharded=sharded;
    spu_context* contexts=calloc(64,sizeof(*contexts));CHECK(contexts!=NULL);
    spu_mfc_bucket* locked=mfc_bucket_for(&contexts[0]);
    unsigned other=1;
    if(sharded)while(other<64 && mfc_bucket_for(&contexts[other])==locked)++other;
    CHECK(other<64);
    ShardRequest req={.ctx=&contexts[other],.loops=1};pthread_t worker;
    AcquireSRWLockExclusive(&locked->state.lock);
    CHECK(pthread_create(&worker,NULL,shard_worker,&req)==0);
    /* Unrelated buckets must make progress while one writer is stopped.
     * Legacy deliberately blocks, proving the test detects a global lock. */
    CHECK(shard_wait(&req.done,sharded?3000:50)==sharded);
    ReleaseSRWLockExclusive(&locked->state.lock);
    CHECK(pthread_join(worker,NULL)==0);CHECK(req.done==1);
    CHECK(fixture_mfc_nodes()==0);free(contexts);
}
static void lifetime_bench(int sharded)
{
    enum {WORKERS=4,LOOPS=200000};
    s_mfc_sharded=sharded;
    ShardRequest req[WORKERS]={0};pthread_t threads[WORKERS];
    bench_arrived=bench_go=0;
    unsigned buckets=0;spu_mfc_bucket* seen[WORKERS];
    for(unsigned n=0;n<WORKERS;++n){
        req[n].ctx=calloc(1,sizeof(spu_context));if(!req[n].ctx)abort();
        req[n].loops=LOOPS;req[n].bench=1;
        spu_mfc_bucket* bucket=mfc_bucket_for(req[n].ctx);unsigned i=0;
        while(i<buckets && seen[i]!=bucket)++i;if(i==buckets)seen[buckets++]=bucket;
        if(pthread_create(&threads[n],NULL,shard_worker,&req[n]))abort();
    }
    ULONGLONG deadline=GetTickCount64();
    while(bench_arrived<WORKERS){if(GetTickCount64()-deadline>3000)abort();YieldProcessor();}
    LARGE_INTEGER begin,end,frequency;QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&begin);
    InterlockedExchange(&bench_go,1);
    for(unsigned n=0;n<WORKERS;++n){pthread_join(threads[n],NULL);free(req[n].ctx);}
    QueryPerformanceCounter(&end);
    printf("MFC lifetime benchmark: sharded=%d buckets=%u workers=%d lifetimes=%d wall_ms=%.3f\n",
           sharded,buckets,WORKERS,WORKERS*LOOPS,1000.0*(end.QuadPart-begin.QuadPart)/frequency.QuadPart);
    if(fixture_mfc_nodes())abort();
}
int main(int argc,char** argv)
{
    if(argc==2){for(unsigned n=0;n<3;++n)lifetime_bench(atoi(argv[1])!=0);return 0;}
    for(int mode=0;mode<2;++mode){s_mfc_sharded=mode;if(previous_registry_main())return 1;independent_bucket_case(mode);}
    printf("MFC sharded ownership: %d passed, %d failed\n",g_pass,g_fail);
    return g_fail?1:0;
}
