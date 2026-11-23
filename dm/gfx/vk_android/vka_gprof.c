// GPU time by step (DM_VK_GPUPROF=n, live): one frame in every n (n >= 1) gets a GPU timestamp after each full barrier the replay records (vka_barrier) and at the start of
// each submission and of each part of one (vka_exec.c chunk). Every full barrier waits for all earlier work, so the time between two timestamps is the GPU time of what was
// recorded between them: one render pass, or the clears / copies / uploads of one op. Once the frame's last submission is done the log gets one "gpu prof" block:
// the frame's GPU time by kind (passes, dispatches, transfers, barriers with nothing between them) and the steps that cost most, each with the recording site of the barrier that
// ended it (file:line), the draws in it and the pass's attachments. Gaps between submissions and parts (the GPU waiting for recording) are left out.
// Each step also names what ran in it (vka_gp_note from vka_draw / vka_dispatch): vertex, mesh and compute counts and the most used shaders (geometry key / pixel key,
// with their draws or workgroups); a "by compute shader" line sums the dispatch steps' time by compute shader key (every dispatch ends in a full barrier: its own step).
// It costs a stall per profiled frame (timestamps), nothing when off (one cached switch read per submission).
// DM_VK_GPUPROF_SPLIT=n (live, 0 = off): in the profiled frame only, a render pass is ended after every n draws (vka_exec.c chunk_check) and the next draw opens it again over
// the same targets (loadOp LOAD, as DM_VK_CHUNK_PASS does), so each step holds n draws (one with n = 1) and the frame's draws get a GPU time each. A pass cannot be timed
// inside on a tiler (its draws run bin by bin), so this is the per-draw split. Each split adds the pass's load and store (and on a tiler a binning pass) to its step: the
// report takes the cheapest step of each pass shape as that overhead (the floor) and gives each draw key its time above it ("gpu prof split:" lines, by vertex / pixel
// key pair and by pixel key). The profiled frame then costs far more than the others (thousands of passes); its relative split is what counts
#if defined(__ANDROID__)
#include "vka.h"
#include <stdlib.h>
#include <string.h>

enum { NQ = 8192, DESC = 96 };   // NQ: room for DM_VK_GPUPROF_SPLIT=1 over a frame of ~4,000 draws
enum { NK = 4 };
typedef struct { char k; uint64_t key, pkey; uint32_t n; uint64_t work; } gp_key;   // k: 'v' vertex draw, 'm' mesh draw, 'c' dispatch
typedef struct { uint32_t nv, nm, nc, nk; gp_key key[NK]; } gp_what;
typedef struct { const char *file; int line; int kind; uint64_t draws, passes, ops; char desc[DESC]; gp_what w; } gp_mark;   // kind: 0 barrier, 1 submission start, 2 part start, 3 commit
static gp_mark g_m[NQ]; static int g_n;
static VkQueryPool g_qp; static int g_ok = -1; static double g_tick_ns;
static PFN_vkCmdWriteTimestamp g_write; static PFN_vkCmdResetQueryPool g_reset; static PFN_vkGetQueryPoolResults g_get;
static int g_state;                     // 0 idle, 1 recording the profiled frame, 2 waiting for its last fence
static uint64_t g_frame, g_last_frame, g_fence; static VkCommandBuffer g_cb;
static char g_pass[DESC]; static int g_pass_new;   // the last pass opened (vka_gp_pass), taken by the next mark
static uint64_t g_sub_frame; static int g_sub_frame_set;
int vka_gp_active;                      // marks are taken (read by vka_barrier)
static int g_split;                     // DM_VK_GPUPROF_SPLIT of the profiled frame
int vka_gp_split(void) { return vka_gp_active ? g_split : 0; }
static gp_what g_w;                     // what the open step recorded so far
// DM_VK_GPUPROF_PSTATS=1 (live, with DM_VK_GPUPROF): each draw call of the profiled frame inside a pipeline statistics query (input-assembly primitives, vertex shader
// invocations, primitives into and out of clipping, fragment shader invocations), so the report can say where primitives go: one into clipping and not out is trivially
// rejected (wholly outside the view volume). "gpu prof pstats" lines: the frame's totals, then the shader pairs and the passes that lose the most primitives that way.
// Each query waits for the command encoder (vka_enc_sync), so the profiled frame gets slower still
enum { PSN = 8192, PSV = 5 };
static VkQueryPool g_ps; static int g_ps_ok = -1, g_ps_on, g_psn;
static struct { uint64_t vs, fs, pass; uint32_t w, h; uint64_t ib; uint32_t cnt; int32_t bv; float vx; } g_psd[PSN];
static PFN_vkCmdBeginQuery g_qb; static PFN_vkCmdEndQuery g_qe;
static int ps_init(void) {
    if (g_ps_ok >= 0) return g_ps_ok;
    g_ps_ok = 0;
    PFN_vkCreateQueryPool create = (PFN_vkCreateQueryPool)vk.vkGetDeviceProcAddr(V.dev, "vkCreateQueryPool");
    g_qb = (PFN_vkCmdBeginQuery)vk.vkGetDeviceProcAddr(V.dev, "vkCmdBeginQuery"); g_qe = (PFN_vkCmdEndQuery)vk.vkGetDeviceProcAddr(V.dev, "vkCmdEndQuery");
    VkQueryPoolCreateInfo ci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS, .queryCount = PSN,
        .pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT | VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
                              VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT | VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT | VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT};
    if (!create || !g_qb || !g_qe || create(V.dev, &ci, NULL, &g_ps)) { VRED("gpu prof pstats: no pipeline statistics query pool (the device's pipelineStatisticsQuery?)"); return 0; }
    g_ps_ok = 1; return 1;
}
int vka_gp_draw_begin(VkCommandBuffer cb, uint64_t vs, uint64_t fs, uint32_t w, uint32_t h, uint64_t ib, uint32_t cnt, int32_t bv, float vx) {
    if (!vka_gp_active || !g_ps_on || cb != g_cb || g_psn >= PSN) return -1;
    extern uint64_t vka_n_rp;
    vka_enc_sync(); g_qb(cb, g_ps, (uint32_t)g_psn, 0);
    g_psd[g_psn].vs = vs; g_psd[g_psn].fs = fs; g_psd[g_psn].pass = vka_n_rp; g_psd[g_psn].w = w; g_psd[g_psn].h = h; g_psd[g_psn].ib = ib; g_psd[g_psn].cnt = cnt; g_psd[g_psn].bv = bv; g_psd[g_psn].vx = vx;
    return g_psn++;
}
void vka_gp_draw_end(VkCommandBuffer cb, int q) { if (q >= 0) { vka_enc_sync(); g_qe(cb, g_ps, (uint32_t)q); } }
typedef struct { uint64_t a, b; uint32_t n, w, h; uint64_t v[PSV]; } ps_agg;
static int64_t ps_rej(const ps_agg *g) { return (int64_t)g->v[2] - (int64_t)g->v[3]; }
static int ps_by_rej(const void *x, const void *y) { int64_t a = ps_rej(x), b = ps_rej(y); return b > a ? 1 : b < a ? -1 : 0; }
static void ps_report(void) {
    if (!g_ps_on || !g_psn) return;
    static uint64_t r[PSN][PSV];
    if (g_get(V.dev, g_ps, 0, (uint32_t)g_psn, sizeof r, r, PSV * 8, VK_QUERY_RESULT_64_BIT)) { VLOG("gpu prof pstats: results not ready"); return; }
    uint64_t t[PSV] = {0}; for (int i = 0; i < g_psn; i++) for (int v = 0; v < PSV; v++) t[v] += r[i][v];
    VLOG("gpu prof pstats: frame %llu, %d draws: %llu input primitives, %llu vertex shader invocations, %llu primitives into clipping, %llu out (%.1f %% rejected there), %llu fragment shader invocations",
         (unsigned long long)g_frame, g_psn, (unsigned long long)t[0], (unsigned long long)t[1], (unsigned long long)t[2], (unsigned long long)t[3], t[2] ? 100.0 * ((double)t[2] - (double)t[3]) / (double)t[2] : 0.0, (unsigned long long)t[4]);
    {   // draws wholly outside the view volume (no primitive out of clipping) against those partly in it: object culling missed the first, the second straddle the view
        uint64_t nw = 0, pw = 0, np = 0, pp = 0, nh = 0, ph = 0;
        for (int i = 0; i < g_psn; i++) { uint64_t in = r[i][2], out = r[i][3]; if (!in) continue;
            if (!out) { nw++; pw += in; } else if (out * 2 < in) { nh++; ph += in; } else { np++; pp += in; } }
        VLOG("gpu prof pstats: draws wholly outside the view %llu (%llu primitives), more than half outside %llu (%llu), at least half inside %llu (%llu)",
             (unsigned long long)nw, (unsigned long long)pw, (unsigned long long)nh, (unsigned long long)ph, (unsigned long long)np, (unsigned long long)pp);
    }
    {   // the wholly-outside draws by shader pair and pass, and their place in the frame (draw index, pass, viewport)
        enum { NW = 64 }; struct { uint64_t vs, fs; uint32_t n; uint64_t prims; uint64_t pmin, pmax; } w[NW]; int nwk = 0;
        for (int i = 0; i < g_psn; i++) { if (!r[i][2] || r[i][3]) continue;
            int j = 0; while (j < nwk && !(w[j].vs == g_psd[i].vs && w[j].fs == g_psd[i].fs)) j++;
            if (j == nwk) { if (nwk == NW) continue; w[j].vs = g_psd[i].vs; w[j].fs = g_psd[i].fs; w[j].n = 0; w[j].prims = 0; w[j].pmin = UINT64_MAX; w[j].pmax = 0; nwk++; }
            w[j].n++; w[j].prims += r[i][2]; if (g_psd[i].pass < w[j].pmin) w[j].pmin = g_psd[i].pass; if (g_psd[i].pass > w[j].pmax) w[j].pmax = g_psd[i].pass; }
        for (int x = 0; x < nwk; x++) for (int y = x + 1; y < nwk; y++) if (w[y].prims > w[x].prims) { typeof(w[0]) t = w[x]; w[x] = w[y]; w[y] = t; }
        for (int x = 0; x < nwk && x < 12; x++) VLOG("gpu prof pstats:  wholly outside: v %016llx/%016llx %u draws, %llu primitives, passes %llu-%llu", (unsigned long long)w[x].vs,
                                                     (unsigned long long)w[x].fs, w[x].n, (unsigned long long)w[x].prims, (unsigned long long)w[x].pmin, (unsigned long long)w[x].pmax);
        {   // split screen: is a wholly-outside draw the same mesh (index buffer, count, base vertex) as a draw of the frame at another viewport x that is visible?
            uint64_t same_vis = 0, same_out = 0, alone = 0, p_vis = 0;
            for (int i = 0; i < g_psn; i++) { if (!r[i][2] || r[i][3]) continue;
                int found = 0;
                for (int k = 0; k < g_psn && found < 2; k++) if (k != i && g_psd[k].ib == g_psd[i].ib && g_psd[k].cnt == g_psd[i].cnt && g_psd[k].bv == g_psd[i].bv && g_psd[k].vx != g_psd[i].vx)
                    found = r[k][3] ? 2 : 1;
                if (found == 2) { same_vis++; p_vis += r[i][2]; } else if (found == 1) same_out++; else alone++; }
            VLOG("gpu prof pstats: wholly-outside draws whose mesh is drawn visible at the other view's viewport %llu (%llu primitives), drawn there but outside too %llu, in one view only %llu",
                 (unsigned long long)same_vis, (unsigned long long)p_vis, (unsigned long long)same_out, (unsigned long long)alone);
        }
        if (VKA_ENV("DM_VK_GPUPROF_PSTATS", 0) >= 2)   // =2: each wholly-outside draw's index in the profiled frame's draws, for DM_VK_RT_LOG's lines
            for (int i = 0, k = 0; i < g_psn && k < 200; i++) if (r[i][2] && !r[i][3]) { k++; VLOG("gpu prof pstats:   outside draw %d: pass %llu, v %016llx/%016llx, %llu primitives",
                                                                                           i, (unsigned long long)g_psd[i].pass, (unsigned long long)g_psd[i].vs, (unsigned long long)g_psd[i].fs, (unsigned long long)r[i][2]); }
    }
    enum { NA = 1024 }; static ps_agg agg[2][NA]; int na[2] = {0, 0};   // [0] by shader pair, [1] by pass
    for (int i = 0; i < g_psn; i++) for (int m = 0; m < 2; m++) {
        uint64_t a = m ? g_psd[i].pass : g_psd[i].vs, b = m ? 0 : g_psd[i].fs; int j = 0;
        while (j < na[m] && !(agg[m][j].a == a && agg[m][j].b == b)) j++;
        if (j == na[m]) { if (na[m] == NA) continue; memset(&agg[m][j], 0, sizeof agg[m][j]); agg[m][j].a = a; agg[m][j].b = b; agg[m][j].w = g_psd[i].w; agg[m][j].h = g_psd[i].h; na[m]++; }
        agg[m][j].n++; for (int v = 0; v < PSV; v++) agg[m][j].v[v] += r[i][v];
    }
    for (int m = 0; m < 2; m++) {
        qsort(agg[m], (size_t)na[m], sizeof agg[m][0], ps_by_rej);
        for (int x = 0; x < na[m] && x < 16; x++) {
            const ps_agg *g = &agg[m][x]; double rej = g->v[2] ? 100.0 * (double)ps_rej(g) / (double)g->v[2] : 0;
            if (m) VLOG("gpu prof pstats:  pass %llu (viewport %ux%u), %u draws: %llu primitives, %llu into clipping, %llu out (%.1f %% rejected), %llu vertex invocations, %llu fragment",
                        (unsigned long long)g->a, g->w, g->h, g->n, (unsigned long long)g->v[0], (unsigned long long)g->v[2], (unsigned long long)g->v[3], rej, (unsigned long long)g->v[1], (unsigned long long)g->v[4]);
            else VLOG("gpu prof pstats:  v %016llx/%016llx (viewport %ux%u), %u draws: %llu primitives, %llu into clipping, %llu out (%.1f %% rejected), %llu vertex invocations, %llu fragment",
                      (unsigned long long)g->a, (unsigned long long)g->b, g->w, g->h, g->n, (unsigned long long)g->v[0], (unsigned long long)g->v[2], (unsigned long long)g->v[3], rej, (unsigned long long)g->v[1], (unsigned long long)g->v[4]);
        }
    }
}
void vka_gp_note(char k, uint64_t key, uint64_t pkey, uint64_t work) {
    if (!vka_gp_active) return;
    if (k == 'm') g_w.nm++; else if (k == 'c') g_w.nc++; else g_w.nv++;
    uint32_t i = 0; while (i < g_w.nk && !(g_w.key[i].k == k && g_w.key[i].key == key && g_w.key[i].pkey == pkey)) i++;
    if (i == g_w.nk) { if (g_w.nk == NK) return; g_w.key[g_w.nk++] = (gp_key){k, key, pkey, 0, 0}; }
    g_w.key[i].n++; g_w.key[i].work += work;
}
static void what_str(const gp_what *w, char *o, size_t on) {
    size_t n = (size_t)snprintf(o, on, "; %uv %um %uc", w->nv, w->nm, w->nc);
    gp_key k[NK]; memcpy(k, w->key, sizeof k);
    for (uint32_t a = 0; a < w->nk; a++) for (uint32_t b = a + 1; b < w->nk; b++) if (k[b].n > k[a].n) { gp_key t = k[a]; k[a] = k[b]; k[b] = t; }
    for (uint32_t a = 0; a < w->nk && a < 2 && n < on; a++)
        n += (size_t)snprintf(o + n, on - n, " %c %016llx/%016llx x%u (%llu %s)", k[a].k, (unsigned long long)k[a].key, (unsigned long long)k[a].pkey, k[a].n,
                              (unsigned long long)k[a].work, k[a].k == 'v' ? "vertices" : "workgroups");
    if (w->nk == NK && n < on) snprintf(o + n, on - n, " +more");
}

void vka_gp_sub_frame(uint64_t f) { g_sub_frame = f; g_sub_frame_set = 1; }   // the replay worker: the game frame of the submission it replays next
static int init(void) {
    if (g_ok >= 0) return g_ok;
    g_ok = 0;
    g_write = (PFN_vkCmdWriteTimestamp)vk.vkGetDeviceProcAddr(V.dev, "vkCmdWriteTimestamp"); g_reset = (PFN_vkCmdResetQueryPool)vk.vkGetDeviceProcAddr(V.dev, "vkCmdResetQueryPool");
    g_get = (PFN_vkGetQueryPoolResults)vk.vkGetDeviceProcAddr(V.dev, "vkGetQueryPoolResults");
    PFN_vkCreateQueryPool create = (PFN_vkCreateQueryPool)vk.vkGetDeviceProcAddr(V.dev, "vkCreateQueryPool");
    VkQueryPoolCreateInfo ci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = NQ};
    if (!g_write || !g_reset || !g_get || !create || create(V.dev, &ci, NULL, &g_qp) || V.props.limits.timestampPeriod <= 0) { VRED("gpu prof: no timestamp query pool"); return 0; }
    g_tick_ns = V.props.limits.timestampPeriod; g_ok = 1;
    return 1;
}
static void mark(VkCommandBuffer cb, int kind, const char *file, int line) {
    if (g_n >= NQ) return;
    gp_mark *m = &g_m[g_n]; m->file = file; m->line = line; m->kind = kind;
    uint64_t sk, di, pi; vka_draw_stats(&m->draws, &sk, &di, &pi); (void)sk; (void)di; (void)pi;
    extern uint64_t vka_n_rp; m->passes = vka_n_rp; uint64_t subs, rec; vka_exec_counts(&m->ops, &subs, &rec);
    if (g_pass_new) { memcpy(m->desc, g_pass, DESC); g_pass_new = 0; } else m->desc[0] = 0;
    m->w = g_w; memset(&g_w, 0, sizeof g_w);
    vka_enc_sync();
    g_write(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_qp, (uint32_t)g_n);
    g_n++;
}
void vka_gp_mark(VkCommandBuffer cb, const char *file, int line) { if (cb == g_cb) mark(cb, 0, file, line); }
void vka_gp_pass(const char *desc) { if (vka_gp_active) { snprintf(g_pass, DESC, "%s", desc); g_pass_new = 1; } }
void vka_gp_part(VkCommandBuffer cb) { if (!vka_gp_active) return; g_cb = cb; mark(cb, 2, "part", 0); }

typedef struct step_ { int i; double ms; } step;
// DM_VK_GPUPROF_SPLIT: the draw steps' time by draw key, above each pass shape's floor (its cheapest step with draws: the split's own load / store)
typedef struct { char k; uint64_t key, pkey; uint32_t n; uint64_t work; double ms, over; } sp_key;
static int sp_by_over(const void *a, const void *b) { double d = ((const sp_key *)b)->over - ((const sp_key *)a)->over; return d > 0 ? 1 : d < 0 ? -1 : 0; }
static void split_report(const struct step_ *s, int ns);
static int by_ms(const void *a, const void *b) { double d = ((const step *)b)->ms - ((const step *)a)->ms; return d > 0 ? 1 : d < 0 ? -1 : 0; }
static void report(void) {
    static uint64_t t[NQ];
    if (g_get(V.dev, g_qp, 0, (uint32_t)g_n, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT)) { VLOG("gpu prof: frame %llu: results not ready", (unsigned long long)g_frame); return; }
    static step s[NQ]; int ns = 0; double tot = 0, pass = 0, xfer = 0, disp = 0, bare = 0, first = 0; int npass = 0, ndisp = 0;
    double vka_ts_tick_ns(void); double tick = vka_ts_tick_ns(); if (!(tick > 0)) tick = g_tick_ns;   // (the tick measured against the host clock where the driver's period is off: vka_exec.c tick_sample)
    for (int i = 1; i < g_n; i++) {
        if (g_m[i].kind == 1 || g_m[i].kind == 2) continue;   // the gap before a submission or part: the GPU waited for recording
        double ms = t[i] > t[i - 1] ? (t[i] - t[i - 1]) * tick / 1e6 : 0;
        tot += ms; s[ns++] = (step){i, ms};
        // a dispatch counts neither as a draw nor as an op: its step is told by what it recorded (vka_gp_note), or it would be taken for a bare barrier
        if (g_m[i].passes > g_m[i - 1].passes) { pass += ms; npass++; } else if (g_m[i].w.nc) { disp += ms; ndisp++; }
        else if (g_m[i].ops > g_m[i - 1].ops || g_m[i].draws > g_m[i - 1].draws) xfer += ms; else bare += ms;
        if (g_m[i - 1].kind == 1) first += ms;
    }
    VLOG("gpu prof: frame %llu: %d timestamps, %.2f ms GPU: passes %.2f ms (%d), dispatches %.2f ms (%d), clears / copies %.2f ms, steps with nothing recorded (barriers, uploads, writebacks) %.2f ms",
         (unsigned long long)g_frame, g_n, tot, pass, npass, disp, ndisp, xfer, bare);
    if (VKA_ENV("DM_VK_GPUPROF_LIST", 0))   // every step in order
        for (int k = 0; k < ns; k++) { const gp_mark *a = &g_m[s[k].i - 1], *b = &g_m[s[k].i];
            char ws[200]; what_str(&b->w, ws, sizeof ws);
            VLOG("gpu prof: step #%d %.3f ms: %s:%d, %llu draws, %llu ops%s%s%s", s[k].i, s[k].ms, b->file, b->line, (unsigned long long)(b->draws - a->draws),
                 (unsigned long long)(b->ops - a->ops), b->desc[0] ? "; pass " : "", b->desc, ws); }
    if (g_split) split_report(s, ns);   // the steps still in GPU order
    qsort(s, (size_t)ns, sizeof *s, by_ms);
    for (int k = 0; k < ns && k < 16; k++) {
        const gp_mark *a = &g_m[s[k].i - 1], *b = &g_m[s[k].i];
        char ws[200]; what_str(&b->w, ws, sizeof ws);
        VLOG("gpu prof:  #%d %.3f ms: %s:%d, %llu draws, %llu passes, %llu ops%s%s%s", s[k].i, s[k].ms, b->file, b->line, (unsigned long long)(b->draws - a->draws),
             (unsigned long long)(b->passes - a->passes), (unsigned long long)(b->ops - a->ops), b->desc[0] ? "; pass " : "", b->desc, ws);
    }
    {   // the dispatch steps by compute shader, and the draw steps by kind (vertex only, with mesh draws)
        struct { uint64_t key; int n; double ms; uint64_t wg; } cs[48]; int ncs = 0; double vms = 0, mms = 0, cms = 0; int nvs = 0, nms = 0;
        for (int k = 0; k < ns; k++) { const gp_what *w = &g_m[s[k].i].w;
            if (w->nm) { mms += s[k].ms; nms++; } else if (w->nv) { vms += s[k].ms; nvs++; }
            if (w->nc == 1 && !w->nv && !w->nm) { int j = 0; cms += s[k].ms; while (j < ncs && cs[j].key != w->key[0].key) j++;
                if (j == ncs) { if (ncs == 48) continue; cs[ncs++] = (typeof(cs[0])){w->key[0].key, 0, 0, 0}; }
                cs[j].n++; cs[j].ms += s[k].ms; cs[j].wg += w->key[0].work; } }
        for (int a = 0; a < ncs; a++) for (int b = a + 1; b < ncs; b++) if (cs[b].ms > cs[a].ms) { typeof(cs[0]) t = cs[a]; cs[a] = cs[b]; cs[b] = t; }
        char line[1400]; size_t ln = 0; line[0] = 0;
        for (int j = 0; j < ncs && j < 16 && ln < sizeof line; j++) ln += (size_t)snprintf(line + ln, sizeof line - ln, " %016llx %dx %.2fms (%llu wg);", (unsigned long long)cs[j].key, cs[j].n, cs[j].ms, (unsigned long long)cs[j].wg);
        VLOG("gpu prof: steps with mesh draws %.2f ms (%d), vertex draws only %.2f ms (%d), single dispatches %.2f ms; by compute shader:%s", mms, nms, vms, nvs, cms, line);
    }
    // by barrier site: count and time
    struct { const char *f; int l; int n; double ms; } site[64]; int nsite = 0;
    for (int k = 0; k < ns; k++) { const gp_mark *b = &g_m[s[k].i]; int j = 0;
        while (j < nsite && !(site[j].l == b->line && site[j].f == b->file)) j++;
        if (j == nsite) { if (nsite == 64) continue; site[nsite++] = (typeof(site[0])){b->file, b->line, 0, 0}; }
        site[j].n++; site[j].ms += s[k].ms; }
    char line[1200]; size_t ln = 0; line[0] = 0;
    for (int j = 0; j < nsite && ln < sizeof line; j++) ln += (size_t)snprintf(line + ln, sizeof line - ln, " %s:%d %dx %.2fms;", site[j].f, site[j].l, site[j].n, site[j].ms);
    VLOG("gpu prof: by site:%s", line);
    ps_report();
}
// mtl_sub_begin, after the command buffer begins: arms the profile at a new frame, reports a finished one
void vka_gp_sub_begin(VkCommandBuffer cb, uint64_t frame) {
    if (g_sub_frame_set) frame = g_sub_frame;
    if (g_state == 2 && vka_done() >= g_fence) { report(); g_state = 0; }
    if (g_state == 1 && frame != g_frame) { g_state = 2; vka_gp_active = 0; g_cb = VK_NULL_HANDLE; }
    if (g_state == 2 && vka_done() >= g_fence) { report(); g_state = 0; }
    if (g_state == 0) {
        int every = VKA_ENV("DM_VK_GPUPROF", 0);
        if (every <= 0 || frame == g_frame || (g_last_frame && frame < g_last_frame + (uint64_t)every) || !init()) return;
        g_state = 1; g_frame = g_last_frame = frame; g_n = 0; vka_gp_active = 1; g_pass_new = 0; memset(&g_w, 0, sizeof g_w);
        g_split = VKA_ENV("DM_VK_GPUPROF_SPLIT", 0); if (g_split < 0) g_split = 0;
        g_reset(cb, g_qp, 0, NQ);
        g_psn = 0; g_ps_on = VKA_ENV("DM_VK_GPUPROF_PSTATS", 0) && ps_init(); if (g_ps_on) g_reset(cb, g_ps, 0, PSN);
    }
    if (g_state == 1) { g_cb = cb; mark(cb, 1, "submission", 0); }
}
void vka_gp_commit(VkCommandBuffer cb, uint64_t fence) { if (g_state == 1 && cb == g_cb) { mark(cb, 3, "commit", 0); g_fence = fence; } }
static void split_report(const step *s, int ns) {
    enum { NSH = 128, NSK = 1024 };
    static char shape[NSH][DESC]; static double floor_ms[NSH], shape_ms[NSH]; static int shape_n[NSH], shape_d[NSH]; int nsh = 0;
    static int sh_of[NQ];
    const char *cur = "";   // the pass shape a step belongs to: the last pass desc seen (a split pass opens again with the same desc)
    for (int k = 0; k < ns; k++) {
        const gp_mark *b = &g_m[s[k].i]; sh_of[k] = -1;
        if (b->desc[0]) cur = b->desc;
        if (!(b->w.nv + b->w.nm) || !cur[0]) continue;
        int j = 0; while (j < nsh && strcmp(shape[j], cur)) j++;
        if (j == nsh) { if (nsh == NSH) continue; snprintf(shape[nsh], DESC, "%s", cur); floor_ms[nsh] = 1e9; shape_ms[nsh] = 0; shape_n[nsh] = shape_d[nsh] = 0; nsh++; }
        sh_of[k] = j; shape_n[j]++; shape_d[j] += (int)(b->w.nv + b->w.nm); shape_ms[j] += s[k].ms; if (s[k].ms < floor_ms[j]) floor_ms[j] = s[k].ms;
    }
    static sp_key kk[NSK]; int nk = 0; double tot = 0, over = 0;
    for (int k = 0; k < ns; k++) {
        if (sh_of[k] < 0) continue;
        const gp_what *w = &g_m[s[k].i].w; uint32_t nd = w->nv + w->nm, kn = 0;
        for (uint32_t a = 0; a < w->nk; a++) kn += w->key[a].n;
        if (!kn) continue;
        double ov = s[k].ms - floor_ms[sh_of[k]]; if (ov < 0) ov = 0;
        tot += s[k].ms; over += ov; (void)nd;
        for (uint32_t a = 0; a < w->nk; a++) {   // a step of several keys: its time shared by their draws
            const gp_key *g = &w->key[a]; double f = (double)g->n / kn; int j = 0;
            while (j < nk && !(kk[j].k == g->k && kk[j].key == g->key && kk[j].pkey == g->pkey)) j++;
            if (j == nk) { if (nk == NSK) continue; kk[nk++] = (sp_key){g->k, g->key, g->pkey, 0, 0, 0, 0}; }
            kk[j].n += g->n; kk[j].work += g->work; kk[j].ms += s[k].ms * f; kk[j].over += ov * f;
        }
    }
    VLOG("gpu prof split: frame %llu, %d draws a step at most: draw steps %.2f ms, %.2f ms above the floors; %d pass shapes:", (unsigned long long)g_frame, g_split, tot, over, nsh);
    for (int j = 0; j < nsh; j++)
        VLOG("gpu prof split:  shape %s: %d steps, %d draws, %.2f ms, floor %.3f ms a step, %.2f ms above it", shape[j], shape_n[j], shape_d[j], shape_ms[j], floor_ms[j],
             shape_ms[j] - floor_ms[j] * shape_n[j]);
    qsort(kk, (size_t)nk, sizeof *kk, sp_by_over);
    for (int j = 0; j < nk && j < 40; j++)
        VLOG("gpu prof split:  %c %016llx/%016llx x%u (%llu %s): %.3f ms, %.3f above the floor (%.1f %%)", kk[j].k, (unsigned long long)kk[j].key, (unsigned long long)kk[j].pkey,
             kk[j].n, (unsigned long long)kk[j].work, kk[j].k == 'v' ? "vertices" : "workgroups", kk[j].ms, kk[j].over, over > 0 ? 100 * kk[j].over / over : 0);
    // by pixel key alone
    static sp_key pk[NSK]; int np = 0;
    for (int j = 0; j < nk; j++) { int i = 0; while (i < np && pk[i].pkey != kk[j].pkey) i++;
        if (i == np) pk[np++] = (sp_key){'p', 0, kk[j].pkey, 0, 0, 0, 0};
        pk[i].n += kk[j].n; pk[i].ms += kk[j].ms; pk[i].over += kk[j].over; }
    qsort(pk, (size_t)np, sizeof *pk, sp_by_over);
    char line[1400]; size_t ln = 0; line[0] = 0;
    for (int j = 0; j < np && j < 24 && ln < sizeof line; j++) ln += (size_t)snprintf(line + ln, sizeof line - ln, " %016llx x%u %.2f/%.2fms;", (unsigned long long)pk[j].pkey, pk[j].n, pk[j].over, pk[j].ms);
    VLOG("gpu prof split: by pixel shader (above the floor / all):%s", line);
}
#endif
