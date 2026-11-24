// The command header layout is taken from the first command list that walks in one, not from the first list generated.
// Mario Kart 8 Deluxe (SDK 4.4.0): the render thread generated a list before the game's first RequestUpdate whose third command was zeroed; taking the layout from it
// left the SDK 6 headers read in the newer layout, no DeviceSink was found and the game was silent. Also check recovery with the other supported headers and sinks.
// Bastion (SDK 5.4.0): its first list, two commands with the SDK 6 header whose first has ID 0x14 and 0x14 bytes, walks in the SDK 8–20 layout too and must decide nothing.
#include "../hle.h"
#include <assert.h>
#include <sys/wait.h>
#include "../hle_audren.c"

// More than one audio renderer: upstream nxnative 18b435ac38 and a334797d12 (the stubs and tests of the two commits; capture and up_3_2 take the renderer)
enum { SAMPLES = 240, NMIX = 8, NCMD = 6 };
// test_renderer_blocks: the renderer each appended block came from; the output holds what it is given and plays none of it
static int blocks_on; static gbuf *blocks[AR_MAX][NBUF]; static int blocks_n[AR_MAX];
static void blocks_exec(void *obj, void *state);
// test_update_clears_event / test_event_paced_driver (upstream nxnative 67e671465): the SDK's Update and the render thread's exec, and the renderer event the game waits on,
// modelled as an auto-clear event (a wait consumes the signal) that each exec signals and the host's clear resets
static int drv_on;
static struct {
    pthread_mutex_t mu; pthread_cond_t cv;
    int signaled, renders, updates, clears, back_to_back, last_update_renders, slow_every, target;
    Result update_rc;
    gbuf *ring[NBUF]; unsigned ring_head, ring_tail;
} drv;
static void drv_exec(void *obj, void *state);
static Result drv_update(void *sys, void *o, size_t on, void *p, size_t pn, const void *in, size_t inn);
void *guest_sym(const char *name) {
    if (blocks_on && strstr(name, "ExecuteAudioRendererRendering")) return (void *)blocks_exec;
    if (drv_on && strstr(name, "ExecuteAudioRendererRendering")) return (void *)drv_exec;
    if (drv_on && !strcmp(name, "_ZN2nn5audio6server17AudioRenderSystem6UpdateEPvmS3_mPKvm")) return (void *)drv_update;
    return NULL;
}
void *objmap_get(const void *key) { (void)key; return NULL; }
void objmap_put(const void *key, void *value) { (void)key; (void)value; }
void *objmap_del(const void *key) { (void)key; return NULL; }
void *dm_guest_thread_spawn(void (*fn)(void *), void *arg, int prio, size_t stack) { (void)fn; (void)arg; (void)prio; (void)stack; return NULL; }
void dm_guest_thread_join(void *tt) { (void)tt; }
uint64_t g_t0_ns; uint64_t now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }   // the render thread's 5 ms clock
module_t *module_of(const void *p) { (void)p; return NULL; }
__thread _Atomic int dm_host_work;   // dm.h's host-work count (hle_thread.c), which the render lock's waiters raise: test_renderer_blocks runs the render thread's loop
void *dm_pcmout_open(int ch) { (void)ch; return NULL; }
void dm_pcmout_close(void *k) { (void)k; }
void dm_pcmout_start(void *k) { (void)k; }
int dm_pcmout_append(void *k, void *b) {
    (void)k;
    if (drv_on) { drv.ring[drv.ring_tail++ % NBUF] = b; return 1; }   // played at once: handed back by the next dm_pcmout_released
    if (!blocks_on) return 0;
    int r = ar_index(t_ar); blocks[r][blocks_n[r]++] = b;
    if (blocks_n[r] == NBUF) atomic_store(&t_ar->open, 0);
    return 1;
}
void *dm_pcmout_released(void *k) { (void)k; return drv_on && drv.ring_head != drv.ring_tail ? drv.ring[drv.ring_head++ % NBUF] : NULL; }
void dm_pcmout_wait(void *k, int ms) { (void)k; (void)ms; }
void hle_bind_system_event(void *ev, int ac) { (void)ev; (void)ac; }
void hle_clear_system_event(void *ev) { (void)ev; pthread_mutex_lock(&drv.mu); drv.clears++; drv.signaled = 0; pthread_mutex_unlock(&drv.mu); }
uint32_t hle_dup_readable_handle(uint32_t h) { return h; }

static int32_t mix[NMIX * SAMPLES];
_Alignas(8) static uint8_t list[0x20 + NCMD * 0x148];
static uint8_t *put(uint8_t *q, int sdk, int id, int size) {
    memset(q, 0, (size_t)size);
    if (sdk == 21) { q[0] = 1; q[1] = (uint8_t)id; *(int16_t *)(q + 2) = (int16_t)size; }
    else {
        *(uint32_t *)q = 0xcafebabeu;
        if (sdk <= 6) { *(int16_t *)(q + 4) = (int16_t)size; *(int16_t *)(q + 6) = (int16_t)id; q[0x10] = 1; }
        else { q[4] = 1; q[5] = (uint8_t)id; *(int16_t *)(q + 6) = (int16_t)size; }
    }
    return q + size;
}
static size_t build(int sdk, int ncmd) {
    memset(list, 0, sizeof list);
    *(uint32_t *)(list + 4) = (uint32_t)ncmd; *(const int32_t **)(list + 8) = mix; *(uint32_t *)(list + 0x14) = SAMPLES; *(uint32_t *)(list + 0x18) = 48000;
    uint8_t *q = list + 0x20;
    for (int i = 0; i < ncmd - 1; i++) q = put(q, sdk, 6, 0x40);
    int id = sdk <= 5 ? 0xf : sdk == 6 ? 0x10 : sdk == 21 ? 0x14 : 0x13;
    uint8_t *sink = q; q = put(q, sdk, id, sdk == 4 ? 0x130 : sdk == 5 ? 0x138 : sdk == 21 ? 0x140 : 0x148);
    int count = sdk <= 5 ? 0x120 : sdk == 21 ? 0x128 : 0x130;
    *(uint32_t *)(sink + count) = 2;
    if (sdk == 4) { sink[0x124] = 0; sink[0x125] = 1; }
    else { *(int16_t *)(sink + count + 4) = 0; *(int16_t *)(sink + count + 6) = 1; }
    return (size_t)(q - list);
}

static void test_headers(void) {
    const int sdks[] = {4, 5, 6, 16, 21};
    for (size_t i = 0; i < sizeof sdks / sizeof *sdks; i++) {
        _Alignas(8) uint8_t aligned[0x40], wire[0x41]; uint8_t *q = wire + 1;
        int sdk = sdks[i], id = sdk == 21 ? 0x14 : 0x10;
        g_hdr6 = sdk <= 6; g_hdr21 = sdk == 21;
        put(aligned, sdk, id, sizeof aligned); memcpy(q, aligned, sizeof aligned);
        assert(cmd_size(q) == sizeof aligned && cmd_id(q) == id && cmd_on(q));
        assert(cmd_ok(q, q + sizeof aligned));
        // Header and body truncation must fail before any field or next-command read.
        for (size_t n = 0; n < sizeof aligned; n++) assert(!cmd_ok(q, q + n));
        const int16_t invalid[] = {0, -1, 4, 0x41};
        for (size_t n = 0; n < sizeof invalid / sizeof *invalid; n++) {
            memcpy(q + (sdk == 21 ? 2 : sdk <= 6 ? 4 : 6), invalid + n, sizeof *invalid);
            assert(!cmd_ok(q, q + sizeof aligned));
        }
    }
    g_hdr6 = g_hdr21 = 0;
    int16_t out[FRAME_MAX * 2];
    for (size_t n = 0; n < 0x20; n++) assert(capture(&g_ar[0], list, n, out) == 0);
}

static void test(int sdk, int incomplete) {
    // render_once starts with ID 0x14 for SDK 20+ (the Span version of GenerateCommand), 0x13 otherwise.
    g_sink_id = sdk == 21 ? 0x14 : 0x13;
    for (int s = 0; s < SAMPLES; s++) { mix[s] = 1000; mix[SAMPLES + s] = -1000; }
    int16_t out[FRAME_MAX * 2];

    // An empty startup list must not choose a layout either.
    size_t n = build(sdk, NCMD);
    *(uint32_t *)(list + 4) = 0;
    assert(capture(&g_ar[0], list, 0x20, out) == 0);
    if (incomplete) {
        // The third command is zeroed, as Mario Kart 8 Deluxe's first list was. Several such frames must leave detection open.
        n = build(sdk, NCMD);
        memset(list + 0x20 + 2 * 0x40, 0, 0x40);
        for (int i = 0; i < 3; i++) assert(capture(&g_ar[0], list, n, out) == 0);
        assert(!g_hdr6 && !g_hdr21 && !g_sink4);
    }
    if (sdk == 5) {
        // Bastion's first list: a 0x14-byte command of ID 0x14, then a 0x28-byte one of ID 0xa: in the SDK 8–20 layout the IDs read as sizes, and both still walk
        memset(list, 0, sizeof list);
        *(uint32_t *)(list + 4) = 2; *(const int32_t **)(list + 8) = mix; *(uint32_t *)(list + 0x14) = SAMPLES; *(uint32_t *)(list + 0x18) = 48000;
        uint8_t *q = put(list + 0x20, 5, 0x14, 0x14); q = put(q, 5, 0xa, 0x28);
        assert(capture(&g_ar[0], list, (size_t)(q - list), out) == 0);
        assert(!g_hdr6 && !g_hdr21 && !g_sink4);
    }

    // SDK 21 starts with short valid lists: detect its header before the first full list's sink scan.
    n = build(sdk, sdk == 21 ? 2 : NCMD);
    int got = capture(&g_ar[0], list, n, out);
    assert(g_hdr6 == (sdk <= 6) && g_hdr21 == (sdk == 21) && g_sink4 == (sdk <= 5) && g_sink5 == (sdk == 5));
    assert(g_sink_id == (sdk <= 5 ? 0xf : sdk == 6 ? 0x10 : sdk == 21 ? 0x14 : 0x13));
    assert(got == SAMPLES);
    for (int s = 0; s < SAMPLES; s++) assert(out[2 * s] == 1000 && out[2 * s + 1] == -1000);
    n = build(sdk, NCMD);
    assert(capture(&g_ar[0], list, n, out) == SAMPLES);   // detection stays valid for later full frames
}

// Malformed lists after layout selection must leave the histogram/sink scan pending
// and must never produce PCM, even when a valid sink precedes the malformed tail.
static void test_malformed(int sdk, int bad) {
    g_sink_id = sdk == 21 ? 0x14 : 0x13;
    int16_t out[FRAME_MAX * 2];
    size_t n = build(sdk, NCMD);
    *(uint32_t *)(list + 4) = 1;
    assert(capture(&g_ar[0], list, 0x20 + 0x40, out) == 0);   // select the layout without a sink or histogram
    int sink_id = g_sink_id;
    n = build(sdk, NCMD);
    if (bad < 2) memset(list + 0x20 + (size_t)bad * 0x40, 0, 0x40);
    else { *(uint32_t *)(list + 4) = NCMD + 1; memset(list + n, 0, 0x40); n += 0x40; }
    for (size_t i = 0; i < sizeof out / sizeof *out; i++) out[i] = 1234;

    FILE *log = tmpfile(); assert(log);
    fflush(stderr); int saved = dup(STDERR_FILENO); assert(saved >= 0);
    assert(dup2(fileno(log), STDERR_FILENO) >= 0);
    int got = capture(&g_ar[0], list, n, out);
    fflush(stderr); assert(dup2(saved, STDERR_FILENO) >= 0); close(saved);
    char text[2048]; assert(fseek(log, 0, SEEK_SET) == 0); size_t bytes = fread(text, 1, sizeof text - 1, log); text[bytes] = 0; fclose(log);
    assert(got == 0);
    assert(!strstr(text, "by command ID:"));
    assert(strstr(text, "malformed"));
    assert(g_sink_id == sink_id && !g_sink4 && !g_sink5);
    for (size_t i = 0; i < sizeof out / sizeof *out; i++) assert(out[i] == 1234);

    // A subsequent valid full list still discovers the SDK 4/5 sink and yields stereo.
    n = build(sdk, NCMD);
    for (int i = 0; i < SAMPLES; i++) { mix[i] = 700; mix[SAMPLES + i] = -900; }
    assert(capture(&g_ar[0], list, n, out) == SAMPLES);
    for (int i = 0; i < SAMPLES; i++) assert(out[2 * i] == 700 && out[2 * i + 1] == -900);
    // Validation also applies after the histogram has already been logged.
    *(uint32_t *)(list + 4) = NCMD + 1; memset(list + n, 0, 0x40);
    assert(capture(&g_ar[0], list, n + 0x40, out) == 0);
}

static void test_histogram(void) {
    // 64 IDs x 1000 commands need 566 bytes of histogram text, more than its 512-byte buffer.
    // Disabled commands keep this a formatter test without dereferencing sink payloads.
    enum { COUNT = 64000, SIZE = 8 };
    size_t len = 0x20 + COUNT * SIZE;
    uint8_t *cl = calloc(1, len); assert(cl);
    *(uint32_t *)(cl + 4) = COUNT;
    for (int i = 0; i < COUNT; i++) { uint8_t *q = cl + 0x20 + i * SIZE; put(q, 16, i / 1000, SIZE); q[4] = 0; }
    FILE *log = tmpfile(); assert(log);
    fflush(stderr); int saved = dup(STDERR_FILENO); assert(saved >= 0);
    assert(dup2(fileno(log), STDERR_FILENO) >= 0);
    int16_t out[FRAME_MAX * 2]; assert(capture(&g_ar[0], cl, len, out) == 0);
    fflush(stderr); assert(dup2(saved, STDERR_FILENO) >= 0); close(saved);
    char text[2048]; assert(fseek(log, 0, SEEK_SET) == 0); size_t bytes = fread(text, 1, sizeof text - 1, log); text[bytes] = 0; fclose(log);
    const char *hist = strstr(text, "by command ID:"); assert(hist);
    hist += strlen("by command ID:");
    const char *end = strchr(hist, '\n'); assert(end && end - hist <= 512);
    assert(strstr(hist, " 0×1000 1×1000"));
    free(cl);
}

// Upstream nxnative 18b435ac38. Two renderers open at once: an SDK call that names only the system finds its own renderer, and a system not seen yet is the one open renderer whose system is not known
static void test_two_renderers(void) {
    uint8_t x, y, z; memset(g_ar, 0, sizeof g_ar);
    g_ar[0].handle[0] = &x; g_ar[1].handle[0] = &y;
    assert(ar_of_sys(&z) == NULL);   // two unknown: not a guess
    g_ar[0].sys = &x;
    assert(ar_of_sys(&x) == &g_ar[0]);
    assert(ar_of_sys(&y) == &g_ar[1] && g_ar[1].sys == &y);
    assert(ar_of_sys(&y) == &g_ar[1]);
    assert(ar_of_sys(&z) == NULL);   // no renderer left to be it
    // The 3:2 upsampler's history is each renderer's own: frames of one do not reach the other's output
    int32_t quiet[2 * 160] = {0}, loud[2 * 160], out[2 * 240];
    for (int i = 0; i < 2 * 160; i++) loud[i] = 20000;
    memset(g_ar, 0, sizeof g_ar);
    assert(up_3_2(&g_ar[0], loud, 160, out) == 240);
    assert(up_3_2(&g_ar[1], quiet, 160, out) == 240);
    for (int i = 0; i < 2 * 240; i++) assert(out[i] == 0);
    memset(g_ar, 0, sizeof g_ar);
}

// Upstream nxnative a334797d12 (its issue #1336). AudioOut queues a block's pointer and copies its samples when the sound card asks, so a block is the renderer's own until it comes back released.
// Renderer 0 queues all its blocks and none plays; renderer 1 then renders (blocks_exec marks each frame with its renderer) and renderer 0's blocks must still hold its samples.
// The render thread here is paced by the clock (now_ns stands still, so each of its frames waits for its 5 ms slot: about 0.3 s for the two renderers)
static void blocks_exec(void *obj, void *state) {
    (void)obj; (void)state;
    for (int i = 0; i < SAMPLES * 2; i++) t_ar->frame[i] = (int16_t)(ar_index(t_ar) + 1);
    t_ar->frame_n = SAMPLES;
}
static void test_renderer_blocks(void) {
    blocks_on = 1;
    for (int r = 0; r < 2; r++) { pthread_mutex_init(&g_ar[r].mu, NULL); atomic_store(&g_ar[r].open, 1); render_main(&g_ar[r]); pthread_mutex_destroy(&g_ar[r].mu); }
    blocks_on = 0;
    for (int r = 0; r < 2; r++) {
        assert(blocks_n[r] == NBUF);
        for (int i = 0; i < NBUF; i++) {
            const int16_t *s = blocks[r][i]->ptr;
            assert(blocks[r][i]->size == SAMPLES * 4);
            for (int j = 0; j < SAMPLES * 2; j++) assert(s[j] == r + 1);
        }
    }
    memset(g_ar, 0, sizeof g_ar);
}

// Upstream nxnative 67e671465. The render thread's exec renders a frame and signals the renderer event, holding the renderer's lock as render_main does
static void drv_exec(void *obj, void *state) {
    (void)obj; (void)state;
    for (int i = 0; i < SAMPLES * 2; i++) t_ar->frame[i] = 1;
    t_ar->frame_n = SAMPLES;
    pthread_mutex_lock(&drv.mu);
    drv.renders++; drv.signaled = 1;
    pthread_cond_broadcast(&drv.cv);
    pthread_mutex_unlock(&drv.mu);
}
// The SDK's Update: counts updates that came with no render since the update before (what left a light limiter's initialization unsent on Super Mario Bros. Wonder)
static Result drv_update(void *sys, void *o, size_t on, void *p, size_t pn, const void *in, size_t inn) {
    (void)sys; (void)o; (void)on; (void)p; (void)pn; (void)in; (void)inn;
    if (drv.update_rc) return drv.update_rc;
    pthread_mutex_lock(&drv.mu);
    if (drv.updates++ && drv.renders == drv.last_update_renders) drv.back_to_back++;
    drv.last_update_renders = drv.renders;
    pthread_mutex_unlock(&drv.mu);
    return RES_OK;
}
static uint8_t drv_sys[8], drv_ev;
static void drv_setup(void) {
    memset(g_ar, 0, sizeof g_ar); memset(&drv, 0, sizeof drv);
    pthread_mutex_init(&drv.mu, NULL); pthread_cond_init(&drv.cv, NULL);
    ar_t *a = &g_ar[0];
    pthread_mutex_init(&a->mu, NULL);
    a->handle[0] = drv_sys; a->sys = drv_sys; a->ev = &drv_ev; a->ev_kernel = 1;
    atomic_store(&a->open, 1);
    drv_on = 1;
}
static void drv_teardown(void) {
    drv_on = 0;
    pthread_mutex_destroy(&g_ar[0].mu); pthread_cond_destroy(&drv.cv); pthread_mutex_destroy(&drv.mu);
    memset(g_ar, 0, sizeof g_ar);
}
// An update clears a stale signal of the renderer event on success, keeps it on failure or when the renderer has no event, and neither renders nor waits: a sound driver
// that polls (Bink, Batman: Arkham Asylum upstream) returns at once however often it updates
static void test_update_clears_event(void) {
    drv_setup();
    for (int i = 0; i < 12; i++) {
        drv.signaled = 1;
        assert(h_ars_Update(drv_sys, NULL, 0, NULL, 0, NULL, 0) == RES_OK && !drv.signaled);
    }
    assert(drv.updates == 12 && drv.clears == 12 && drv.renders == 0 && atomic_load(&g_ar[0].updates) == 12);
    drv.signaled = 1; drv.update_rc = RES_FAIL;
    assert(h_ars_Update(drv_sys, NULL, 0, NULL, 0, NULL, 0) == RES_FAIL && drv.signaled && drv.clears == 12);
    drv.update_rc = RES_OK; g_ar[0].ev = NULL;
    assert(h_ars_Update(drv_sys, NULL, 0, NULL, 0, NULL, 0) == RES_OK && drv.signaled && drv.clears == 12 && drv.renders == 0);
    drv_teardown();
}
// An event-paced sound thread (Super Mario Bros. Wonder, DRAGON QUEST XI S's CoreAudioOut once no wave buffer comes back): update, wait on the renderer event, work, and
// again. It starts with a stale signal (the render thread's first frame came before its first update), and every third round its work outlasts a 5 ms frame, so a render
// signals between its wait and its next update (adding a light limiter). The update's clear drops both: every update has a render of its own before the next one
static void *drv_driver(void *arg) {
    (void)arg;
    for (int round = 0; round < drv.target; round++) {
        assert(h_ars_Update(drv_sys, NULL, 0, NULL, 0, NULL, 0) == RES_OK);
        pthread_mutex_lock(&drv.mu);
        while (!drv.signaled) pthread_cond_wait(&drv.cv, &drv.mu);
        drv.signaled = 0;   // auto-clear: the wait consumes the signal
        pthread_mutex_unlock(&drv.mu);
        if (round % drv.slow_every == drv.slow_every - 1) usleep(7000);
    }
    atomic_store(&g_ar[0].open, 0);
    return NULL;
}
static void test_event_paced_driver(void) {
    drv_setup();
    drv.target = 40; drv.slow_every = 3;
    drv.signaled = 1;   // stale
    pthread_t th; assert(!pthread_create(&th, NULL, drv_driver, NULL));
    render_main(&g_ar[0]);   // until the driver's last round closes the renderer
    assert(!pthread_join(th, NULL));
    assert(drv.updates == drv.target && drv.clears == drv.target && drv.back_to_back == 0 && drv.renders >= drv.target);
    printf("audren event-paced driver: %d updates, %d renders, every update cleared the event, no two updates without a render between them\n", drv.updates, drv.renders);
    drv_teardown();
}

int main(int argc, char **argv) {
    test_two_renderers();
    test_renderer_blocks();
    test_update_clears_event();
    test_event_paced_driver();
    if (argc == 2 && !strcmp(argv[1], "--histogram")) { test_histogram(); return 0; }
    if (argc == 2 && !strcmp(argv[1], "--malformed")) { test_malformed(16, 0); return 0; }
    test_headers();
    const int sdks[] = {4, 5, 6, 16, 21};
    for (size_t i = 0; i < sizeof sdks / sizeof *sdks; i++) for (int scenario = 0; scenario <= 4; scenario++) {
        // Each renderer format starts in a fresh process, including capture's one-time layout and sink detection.
        pid_t pid = fork(); assert(pid >= 0);
        if (!pid) {
            if (scenario <= 1) test(sdks[i], scenario);
            else test_malformed(sdks[i], scenario - 2);
            _exit(0);
        }
        int status; assert(waitpid(pid, &status, 0) == pid);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    test_histogram();
    puts("audren layout: two renderers, renderers' own blocks, the renderer event cleared by each update, an event-paced driver, bounded histograms, malformed-list rejection, unaligned and truncated headers, SDK 4/5/6/16/21 stereo PCM and empty/incomplete startup recovery passed");
    return 0;
}
