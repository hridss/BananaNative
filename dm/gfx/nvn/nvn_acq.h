// Deferred acquire waits (hle_nvn.c nvnWindowAcquireTexture with a sync object). On the console nvnWindowAcquireTexture returns the texture index at once and the wait for
// the texture's release (its slot's display time, the present that last showed it) is the returned NVNsync's: a CPU nvnSyncWait or a GPU nvnQueueWaitSync. The host blocked
// inside AcquireTexture instead, and a game that measures its CPU time across AcquireTexture and its draw recording before it waits on the sync (Super Mario Bros. Wonder –
// Nintendo Switch 2 Edition, sead::GameFrameworkNx) counted the vsync wait as CPU time and never stepped back from present interval 2 to 1.
//
// An acquire with a sync object takes a record here: the slot's earliest release time (avail), the ticket of the slot's previous present and the GPU fence the sync stands
// for, all taken at the acquire, before a parallel presentation thread can present the slot again. The sync holds only the record's id (sync_t fenced 4), so a sync the game
// fences again, initializes again or forgets leaks nothing; the window holds the id too and waits for a record nobody waited for at its next acquire (one pacing wait a
// frame for games that wait only on the GPU). A record is reused only once complete, and an id no longer in the table is a wait that completed.
//
// Kept apart from hle_nvn.c with the pacing behind nvn_acq_ops so the state machine can be tested on its own (dm/host/tests/t_android_acqwait.c).
#pragma once
#include <pthread.h>
#include <stdint.h>
#include <time.h>

#define NVN_ACQ_N 16   // records: one pending a window at a time, so more than enough; when none is free the acquire waits at once, as before
typedef struct {
    uint64_t id;       // 0: never used
    void *win;         // the window (NULL once complete or detached by WindowFinalize)
    uint64_t avail;    // the slot's earliest release time when acquired (ns; win_t.avail)
    uint64_t ticket;   // the slot's previous present (win_t.ticket; 0: none)
    uint64_t fence;    // the GPU fence the acquire sync stands for (mtl_acquire_fence)
    int paced;         // the pacing part has completed (a finite wait that ran out after it keeps it)
    int done;          // 0: pending, 1: a thread is waiting it out, 2: complete or detached
} nvn_acq_rec;
typedef struct {
    int (*pace_ready)(void *win, uint64_t avail);                                                    // the pacing wait would not block now
    int (*pace_do)(void *win, uint64_t avail, uint64_t timeout_ns, uint64_t deadline, int *waited);   // the pacing wait (0: ran out at the deadline)
    int (*present_ready)(uint64_t ticket);                                                           // that present has been handed to the GPU
    int (*present_wait)(uint64_t ticket, uint64_t timeout_ns);                                       // wait for it (UINT64_MAX: no limit; 0: ran out)
    uint64_t (*now)(void);                                                                           // monotonic ns
} nvn_acq_ops;
typedef struct { pthread_mutex_t mu; pthread_cond_t cv; nvn_acq_rec rec[NVN_ACQ_N]; uint64_t gen; } nvn_acq_tab;
#define NVN_ACQ_TAB_INIT {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, {{0}}, 0}

static inline nvn_acq_rec *nvn_acq_find(nvn_acq_tab *t, uint64_t id) {   // under t->mu
    nvn_acq_rec *r = id ? &t->rec[id % NVN_ACQ_N] : NULL;
    return r && r->id == id ? r : NULL;
}
// Under t->mu: a record for a new acquire, its id (0: none free)
static inline uint64_t nvn_acq_new_locked(nvn_acq_tab *t, void *win, uint64_t avail, uint64_t ticket, uint64_t fence) {
    for (int k = 0; k < NVN_ACQ_N; k++) {
        nvn_acq_rec *r = &t->rec[k];
        if (r->id && r->done != 2) continue;
        uint64_t id = ++t->gen * NVN_ACQ_N + (uint64_t)k;
        *r = (nvn_acq_rec){id, win, avail, ticket, fence, 0, 0};
        return id;
    }
    return 0;
}
// What a GPU wait on the acquire needs: its fence, and the present still to wait for (0 once the record completed: its wait has had that present done)
static inline void nvn_acq_peek(nvn_acq_tab *t, uint64_t id, uint64_t *fence, uint64_t *ticket) {
    pthread_mutex_lock(&t->mu);
    const nvn_acq_rec *r = nvn_acq_find(t, id);
    *fence = r ? r->fence : 0; *ticket = r && r->done != 2 ? r->ticket : 0;
    pthread_mutex_unlock(&t->mu);
}
static inline void nvn_acq_cv_wait(nvn_acq_tab *t, uint64_t left_ns) {   // under t->mu; UINT64_MAX: no limit
    if (left_ns == UINT64_MAX) { pthread_cond_wait(&t->cv, &t->mu); return; }
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t ns = (uint64_t)ts.tv_nsec + left_ns % 1000000000ull;
    ts.tv_sec += (time_t)(left_ns / 1000000000ull + ns / 1000000000ull); ts.tv_nsec = (long)(ns % 1000000000ull);
    pthread_cond_timedwait(&t->cv, &t->mu, &ts);
}
// Wait out an acquire: its pacing, then its slot's previous present. timeout 0 polls (nothing runs unless all of it is ready), a finite one leaves an unfinished wait pending
// (the part done stays done). Returns 1 when complete (*fence_out: the GPU fence still to wait for), 0 when the time ran out. *waited: it blocked at all
static inline int nvn_acq_run(nvn_acq_tab *t, const nvn_acq_ops *op, uint64_t id, uint64_t timeout_ns, int *waited, uint64_t *fence_out) {
    uint64_t start = op->now(), deadline = timeout_ns == UINT64_MAX || timeout_ns > UINT64_MAX - start ? UINT64_MAX : start + timeout_ns;
    pthread_mutex_lock(&t->mu);
    nvn_acq_rec *r;
    for (;;) {
        r = nvn_acq_find(t, id);
        if (!r || r->done == 2) { if (fence_out) *fence_out = r ? r->fence : 0; pthread_mutex_unlock(&t->mu); return 1; }
        if (r->done == 0) break;
        if (!timeout_ns) { pthread_mutex_unlock(&t->mu); return 0; }   // another thread is waiting it out
        *waited = 1;
        uint64_t now = op->now();
        if (deadline != UINT64_MAX && now >= deadline) { pthread_mutex_unlock(&t->mu); return 0; }
        nvn_acq_cv_wait(t, deadline == UINT64_MAX ? UINT64_MAX : deadline - now);
    }
    r->done = 1;   // the record is not reused or detached while this thread has it
    void *win = r->win; uint64_t at = r->avail, tk = r->ticket, gf = r->fence; int paced = r->paced;
    pthread_mutex_unlock(&t->mu);
    if (fence_out) *fence_out = gf;
    int ok = 1;
    if (!timeout_ns && ((!paced && !op->pace_ready(win, at)) || (tk && !op->present_ready(tk)))) ok = 0;
    else {
        if (!paced) {
            uint64_t now = op->now(), rem = deadline == UINT64_MAX ? UINT64_MAX : now >= deadline ? 0 : deadline - now;
            ok = op->pace_do(win, at, rem, deadline, waited);
            if (ok) { pthread_mutex_lock(&t->mu); r->paced = 1; pthread_mutex_unlock(&t->mu); }
        }
        if (ok && tk) {
            if (!op->present_ready(tk)) *waited = 1;
            uint64_t now = op->now(), rem = deadline == UINT64_MAX ? UINT64_MAX : now >= deadline ? 0 : deadline - now;
            ok = op->present_wait(tk, rem);
        }
    }
    pthread_mutex_lock(&t->mu);
    if (ok) { r->done = 2; r->win = NULL; } else r->done = 0;
    pthread_cond_broadcast(&t->cv);
    pthread_mutex_unlock(&t->mu);
    return ok;
}
// WindowFinalize: a pending acquire of the window completes without waiting (a thread waiting it out finishes first: it holds the window)
static inline void nvn_acq_detach(nvn_acq_tab *t, uint64_t id) {
    pthread_mutex_lock(&t->mu);
    nvn_acq_rec *r;
    while ((r = nvn_acq_find(t, id)) && r->done == 1) pthread_cond_wait(&t->cv, &t->mu);
    if (r && r->done == 0) { r->done = 2; r->win = NULL; pthread_cond_broadcast(&t->cv); }
    pthread_mutex_unlock(&t->mu);
}
