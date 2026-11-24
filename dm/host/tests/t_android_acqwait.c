// The deferred acquire wait's state machine (dm/gfx/nvn/nvn_acq.h; hle_nvn.c nvnWindowAcquireTexture, nvnSyncWait, nvnQueueWaitSync, nvnWindowFinalize) with fake pacing:
// polls and finite timeouts leave the wait pending, a part done stays done, a complete or detached record answers at once, ids of reused records read as complete, and a
// second thread waits for the one waiting it out instead of pacing twice. Super Mario Bros. Wonder – Nintendo Switch 2 Edition is the game behind it (docs/reference/frame-rate.md)
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include "../../gfx/nvn/nvn_acq.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static _Atomic int pace_ok = 0, present_ok = 0, pace_calls, present_calls, pace_block;
static _Atomic int pace_entered;
static uint64_t mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static int f_pace_ready(void *w, uint64_t at) { (void)w; (void)at; return pace_ok; }
static int f_pace_do(void *w, uint64_t at, uint64_t timeout_ns, uint64_t deadline, int *waited) {
    (void)w; (void)at; (void)deadline; pace_calls++; pace_entered = 1;
    while (pace_block) usleep(1000);
    if (pace_ok) return 1;
    if (timeout_ns != UINT64_MAX) return 0;   // a finite wait that the pacing outlasts
    *waited = 1; pace_ok = 1; return 1;       // a blocking wait: the slot's time comes
}
static int f_present_ready(uint64_t tk) { (void)tk; return present_ok; }
static int f_present_wait(uint64_t tk, uint64_t timeout_ns) { (void)tk; present_calls++; if (present_ok) return 1; if (timeout_ns != UINT64_MAX) return 0; present_ok = 1; return 1; }
static const nvn_acq_ops ops = {f_pace_ready, f_pace_do, f_present_ready, f_present_wait, mono};
static nvn_acq_tab tab = NVN_ACQ_TAB_INIT;
static int win;   // a window's address

static uint64_t acq(uint64_t ticket) { pthread_mutex_lock(&tab.mu); uint64_t id = nvn_acq_new_locked(&tab, &win, 5, ticket, 77); pthread_mutex_unlock(&tab.mu); return id; }
static void reset(int p, int q) { pace_ok = p; present_ok = q; pace_calls = present_calls = 0; }

typedef struct { uint64_t id, timeout; int r, waited; } run_arg;
static void *runner(void *a) { run_arg *x = a; x->r = nvn_acq_run(&tab, &ops, x->id, x->timeout, &x->waited, NULL); return NULL; }
static void *detacher(void *a) { nvn_acq_detach(&tab, *(uint64_t *)a); return NULL; }

int main(void) {
    // A new acquire: pending, its fence and its present for a GPU wait
    reset(0, 0);
    uint64_t id = acq(9), f = 0, tk = 0; int waited = 0;
    CHECK(id != 0, "a record");
    nvn_acq_peek(&tab, id, &f, &tk);
    CHECK(f == 77 && tk == 9, "peek: fence %llu ticket %llu", (unsigned long long)f, (unsigned long long)tk);
    // Polls: nothing runs unless every part is ready
    CHECK(!nvn_acq_run(&tab, &ops, id, 0, &waited, &f) && pace_calls == 0 && present_calls == 0 && !waited, "a poll before the slot's time");
    pace_ok = 1;
    CHECK(!nvn_acq_run(&tab, &ops, id, 0, &waited, &f) && pace_calls == 0 && present_calls == 0, "a poll before the present was handed over");
    // A finite wait the pacing outlasts leaves it pending; the blocking one completes it
    reset(0, 0);
    CHECK(!nvn_acq_run(&tab, &ops, id, 1000000, &waited, &f) && pace_calls == 1 && present_calls == 0, "a finite wait that runs out in the pacing");
    f = 0;
    CHECK(nvn_acq_run(&tab, &ops, id, UINT64_MAX, &waited, &f) && pace_calls == 2 && present_calls == 1 && waited && f == 77, "the blocking wait completes it (fence %llu)", (unsigned long long)f);
    waited = 0; reset(0, 0);
    CHECK(nvn_acq_run(&tab, &ops, id, 0, &waited, &f) && !pace_calls && !present_calls && !waited && f == 77, "complete: a poll is signaled at once");
    nvn_acq_peek(&tab, id, &f, &tk);
    CHECK(f == 77 && tk == 0, "complete: a GPU wait has no present left to wait for");
    // A part done stays done: the pacing passed, the present wait ran out
    reset(0, 0); id = acq(10);
    CHECK(!nvn_acq_run(&tab, &ops, id, 0, &waited, NULL), "poll");
    pace_ok = 1;
    CHECK(!nvn_acq_run(&tab, &ops, id, 1000000, &waited, NULL) && pace_calls == 1 && present_calls == 1, "the present wait runs out after the pacing");
    pace_ok = 0; present_ok = 1;
    CHECK(nvn_acq_run(&tab, &ops, id, 0, &waited, NULL) && pace_calls == 1 && present_calls == 2, "the pacing is not waited again (a poll completes it)");
    // No present to wait for (the slot never presented)
    reset(1, 0); id = acq(0);
    CHECK(nvn_acq_run(&tab, &ops, id, 0, &waited, NULL) && present_calls == 0, "no present: only the pacing");
    // WindowFinalize: a pending wait completes without waiting
    reset(0, 0); id = acq(11);
    nvn_acq_detach(&tab, id);
    CHECK(nvn_acq_run(&tab, &ops, id, UINT64_MAX, &waited, NULL) && !pace_calls && !present_calls, "detached");
    // Records: all pending, a further acquire gets none (it waits at once); a completed one is reused, and its old id reads as complete
    uint64_t ids[NVN_ACQ_N]; int got = 0;
    for (;;) { uint64_t x = acq(1); if (!x) break; if (got < NVN_ACQ_N) ids[got] = x; got++; }
    CHECK(got <= NVN_ACQ_N && got >= NVN_ACQ_N - 1, "records: %d free", got);   // (the detached one above is free again)
    reset(1, 1);
    CHECK(nvn_acq_run(&tab, &ops, ids[0], UINT64_MAX, &waited, NULL), "complete one");
    uint64_t again = acq(2);
    CHECK(again && again != ids[0] && again % NVN_ACQ_N == ids[0] % NVN_ACQ_N, "the completed record is reused with a new id");
    reset(0, 0);
    CHECK(nvn_acq_run(&tab, &ops, ids[0], 0, &waited, NULL) && !pace_calls, "the old id of a reused record reads as complete");
    nvn_acq_peek(&tab, ids[0], &f, &tk);
    CHECK(f == 0 && tk == 0, "and has nothing for a GPU wait");
    for (int i = 1; i < got && i < NVN_ACQ_N; i++) nvn_acq_detach(&tab, ids[i]);
    nvn_acq_detach(&tab, again);
    // Two threads: the second waits for the one waiting it out (no second pacing); a poll meanwhile is not ready; WindowFinalize waits for it
    reset(0, 1); id = acq(12); pace_block = 1; pace_entered = 0;
    run_arg a = {id, UINT64_MAX, -1, 0}, b = {id, UINT64_MAX, -1, 0};
    pthread_t ta, tb, td; pthread_create(&ta, NULL, runner, &a);
    while (!pace_entered) usleep(1000);
    waited = 0;
    CHECK(!nvn_acq_run(&tab, &ops, id, 0, &waited, NULL), "a poll while another thread waits it out");
    uint64_t t0 = mono();
    CHECK(!nvn_acq_run(&tab, &ops, id, 20000000, &waited, NULL) && mono() - t0 >= 15000000 && waited, "a finite wait while another thread waits it out runs out");
    pthread_create(&tb, NULL, runner, &b);
    pthread_create(&td, NULL, detacher, &id);
    usleep(20000);
    pace_block = 0;
    pthread_join(ta, NULL); pthread_join(tb, NULL); pthread_join(td, NULL);
    CHECK(a.r == 1 && b.r == 1 && pace_calls == 1, "both complete, paced once (%d, %d, %d pacing calls)", a.r, b.r, (int)pace_calls);
    CHECK(b.waited, "the second thread waited");
    printf("t_android_acqwait: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
