/* Lock exclusion/publication/progress test, plus a bounded TAS comparison.
 * No guest data, GPU or game binaries required. */
#define _GNU_SOURCE
#include "../spu_coherency.c"
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#if defined(__ANDROID__)
#include <sched.h>
#endif

void spu_ch_wake(spu_context* ctx) { (void)ctx; }
static atomic_flag baseline = ATOMIC_FLAG_INIT;
static atomic_uint ready, go, failed;
static unsigned mode, rounds;
static struct { uint64_t sequence; uint64_t words[15]; } shared;
static void take(void) {
    if (!mode) while (atomic_flag_test_and_set_explicit(&baseline, memory_order_acquire)) {}
    else spu_lockline_lock();
}
static void drop(void) {
    if (!mode) atomic_flag_clear_explicit(&baseline, memory_order_release);
    else spu_lockline_unlock();
}
static void* worker(void* unused) {
    (void)unused;
    atomic_fetch_add_explicit(&ready, 1, memory_order_release);
    while (!atomic_load_explicit(&go, memory_order_acquire)) sched_yield();
    for (unsigned i = 0; i < rounds; ++i) {
        take();
        const uint64_t before = shared.sequence;
        for (unsigned j = 0; j < 15; ++j) if (shared.words[j] != before + j)
            atomic_store_explicit(&failed, 1, memory_order_relaxed);
        shared.sequence = before + 1;
        for (unsigned j = 0; j < 15; ++j) shared.words[j] = before + 1 + j;
        drop();
    }
    return NULL;
}
static double seconds(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
int main(int argc, char** argv) {
    alarm(12);
    unsigned threads = argc > 2 ? (unsigned)atoi(argv[2]) : 4;
    mode = argc > 1 ? (unsigned)atoi(argv[1]) : 1;
    if (threads < 1 || threads > 8 || mode > 1) return 2;
#if defined(__ANDROID__)
    cpu_set_t allowed; CPU_ZERO(&allowed);
    CPU_SET(0, &allowed); CPU_SET(1, &allowed); CPU_SET(2, &allowed);
    if (sched_setaffinity(0, sizeof allowed, &allowed)) { perror("affinity"); return 2; }
#endif
    rounds = 20000;
    for (unsigned j = 0; j < 15; ++j) shared.words[j] = j;
    pthread_t workers[8];
    for (unsigned i = 0; i < threads; ++i) if (pthread_create(&workers[i], NULL, worker, NULL)) return 2;
    while (atomic_load_explicit(&ready, memory_order_acquire) != threads) sched_yield();
    const double begin = seconds();
    atomic_store_explicit(&go, 1, memory_order_release);
    for (unsigned i = 0; i < threads; ++i) pthread_join(workers[i], NULL);
    const double elapsed = seconds() - begin;
    if (atomic_load(&failed) || shared.sequence != (uint64_t)threads * rounds) return 1;
    printf("PASS lockline mode=%s threads=%u transactions=%llu wall_ms=%.2f\n",
           mode ? "TTAS" : "old-TAS", threads, (unsigned long long)shared.sequence, elapsed * 1000);
    return 0;
}
