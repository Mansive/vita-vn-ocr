/* pool.h - a small fork-join thread pool for the runtime's parallel hooks (vocr_set_parallel,
 * vocr_textspot6_set_parallel), Windows only (the benchmark, not the console runtime).
 *
 *   pool_start(threads);   then pass pool_run, NULL, pool_ways() to the hooks
 *
 * Workers spin for a short while after each job (a row of the finder is a few microseconds of work), then sleep on
 * WaitOnAddress, so an idle pool costs no CPU. The caller runs share 0 itself.
 */
#ifndef BENCH_POOL_H
#define BENCH_POOL_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <immintrin.h>

#define POOL_MAX 64
#define POOL_SPIN 20000 /* pause loops (~ 50-100 us) before a worker sleeps */

typedef void (*pool_task)(void *ctx, unsigned i);

static struct {
    volatile LONG gen;      /* bumped once per job */
    volatile LONG done;     /* workers finished with the current job */
    volatile LONG sleepers; /* workers in WaitOnAddress */
    pool_task task;
    void *ctx;
    unsigned n, threads;
} pool;

/* WaitOnAddress / WakeByAddressAll (Windows 8+), looked up at start: zig's MinGW has no synchronization import lib */
static BOOL(WINAPI *pool_wait)(volatile VOID *, PVOID, SIZE_T, DWORD);
static void(WINAPI *pool_wake)(PVOID);

static DWORD WINAPI pool_worker(LPVOID arg)
{
    unsigned me = (unsigned)(UINT_PTR)arg;
    LONG seen = 0;
    for (;;) {
        LONG g;
        unsigned spin = 0;
        while ((g = pool.gen) == seen) {
            if (++spin < POOL_SPIN) {
                _mm_pause();
                continue;
            }
            InterlockedIncrement(&pool.sleepers);
            if (pool.gen == seen) pool_wait((volatile VOID *)&pool.gen, &seen, sizeof seen, INFINITE);
            InterlockedDecrement(&pool.sleepers);
            spin = 0;
        }
        seen = g;
        MemoryBarrier(); /* read the job after its gen */
        if (me < pool.n) pool.task(pool.ctx, me);
        InterlockedIncrement(&pool.done);
    }
}

static void pool_start(unsigned threads)
{
    unsigned i;
    HMODULE k = LoadLibraryA("api-ms-win-core-synch-l1-2-0.dll");
    pool_wait = (BOOL(WINAPI *)(volatile VOID *, PVOID, SIZE_T, DWORD))(void (*)(void))GetProcAddress(k, "WaitOnAddress");
    pool_wake = (void(WINAPI *)(PVOID))(void (*)(void))GetProcAddress(k, "WakeByAddressAll");
    if (threads > POOL_MAX) threads = POOL_MAX;
    pool.threads = threads ? threads : 1u;
    for (i = 1; i < pool.threads; i++) CloseHandle(CreateThread(NULL, 0, pool_worker, (LPVOID)(UINT_PTR)i, 0, NULL));
}

static unsigned pool_ways(void) { return pool.threads; }

/* task(ctx, i) for i < n (n <= threads): share 0 here, the rest on the workers */
static void pool_run(void *user, unsigned n, pool_task task, void *ctx)
{
    (void)user;
    pool.task = task;
    pool.ctx = ctx;
    pool.n = n;
    pool.done = 0;
    InterlockedIncrement(&pool.gen);
    if (pool.sleepers) pool_wake((PVOID)&pool.gen);
    task(ctx, 0);
    while (pool.done != (LONG)(pool.threads - 1u)) _mm_pause();
}

#endif
