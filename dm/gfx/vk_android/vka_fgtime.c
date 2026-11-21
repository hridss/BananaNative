// SPDX-License-Identifier: MIT
// GPU time of frame generation (vka_framegen.c) and of the presents (vka_present.c), off by default: nothing is created, recorded or logged while DM_VK_FG_GPUTIME is unset.
// DM_VK_FG_GPUTIME=<s> (live; 0 = off): GPU timestamps around the frame generation's command buffer (vka_framegen.c: after its leading full barrier and after its
// trailing one) and around every present command buffer (vka_present.c: after a full barrier at its start and at its end), read back without waiting when the slot is
// reused; one "fg gpu:" line every <s> seconds with mean / p50 / p95 ms by kind and the GPU ms a second. DM_VK_FG_GPUTIME_SPLIT=1 (live) adds timestamps after the
// input copy and after the shared passes (copy / shared / generate split; each adds a drain of the queue).
#if defined(__ANDROID__)
#include "vka.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double fgt_mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

enum { T_FG, T_COPY, T_SHARED, T_GEN, T_PGAME, T_PGEN, T_N, TS_RING = 8192, Q_FG = 4, NFGS = 8, NPRS = 8, Q_PR0 = NFGS * Q_FG, NQ = Q_PR0 + NPRS * 2 };
static const char *const g_tn[T_N] = {"frame generation", "copy", "shared", "generate", "present game", "present generated"};
static struct {
    int ok; VkQueryPool qp; double tick; PFN_vkCmdWriteTimestamp wr; PFN_vkCmdResetQueryPool rs; PFN_vkGetQueryPoolResults get;
    int fg_on[NFGS], fg_split[NFGS], fg_ng[NFGS], pr_on[NPRS], pr_gen[NPRS];
    float s[T_N][TS_RING]; uint32_t ns[T_N]; uint32_t ngen; double t0, sum[T_N];
} T;
static int t_init(void) {
    if (T.ok) return T.ok > 0;
    T.ok = -1;
    T.wr = (PFN_vkCmdWriteTimestamp)vk.vkGetDeviceProcAddr(V.dev, "vkCmdWriteTimestamp"); T.rs = (PFN_vkCmdResetQueryPool)vk.vkGetDeviceProcAddr(V.dev, "vkCmdResetQueryPool");
    T.get = (PFN_vkGetQueryPoolResults)vk.vkGetDeviceProcAddr(V.dev, "vkGetQueryPoolResults");
    PFN_vkCreateQueryPool create = (PFN_vkCreateQueryPool)vk.vkGetDeviceProcAddr(V.dev, "vkCreateQueryPool");
    VkQueryPoolCreateInfo ci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = NQ};
    if (!T.wr || !T.rs || !T.get || !create || create(V.dev, &ci, NULL, &T.qp) || V.props.limits.timestampPeriod <= 0) { VRED("fg gpu: no timestamp query pool"); return 0; }
    T.tick = V.props.limits.timestampPeriod; T.ok = 1; T.t0 = fgt_mono();
    VLOG("fg gpu: timestamps on (tick %.3f ns)", T.tick);
    return 1;
}
static double t_iv(void) { return VKA_ENVF("DM_VK_FG_GPUTIME", 0); }
static void t_add(int k, double ms) { if (ms < 0 || ms > 1000) return; T.s[k][T.ns[k] % TS_RING] = (float)ms; T.ns[k]++; T.sum[k] += ms; }
static int fcmp(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return x < y ? -1 : x > y; }
static void t_report(void) {
    double iv = t_iv(), now = fgt_mono();
    if (iv <= 0 || now - T.t0 < iv) return;
    char b[1200]; size_t o = (size_t)snprintf(b, sizeof b, "fg gpu: %.1f s, %u frame generation cbs (%u generating):", now - T.t0, T.ns[T_FG], T.ngen);
    static float tmp[TS_RING];
    for (int k = 0; k < T_N; k++) {
        uint32_t n = T.ns[k] < TS_RING ? T.ns[k] : TS_RING; if (!n) continue;
        memcpy(tmp, T.s[k], n * sizeof(float)); qsort(tmp, n, sizeof(float), fcmp);
        o += (size_t)snprintf(b + o, o < sizeof b ? sizeof b - o : 0, " %s mean/p50/p95 %.2f/%.2f/%.2f ms (%u);", g_tn[k], T.sum[k] / T.ns[k], tmp[n / 2], tmp[(n * 95) / 100 < n ? (n * 95) / 100 : n - 1], T.ns[k]);
    }
    double el = now - T.t0;
    if (o < sizeof b) snprintf(b + o, sizeof b - o, " GPU ms a second: frame generation %.1f, presents %.1f (game %.1f, generated %.1f)", T.sum[T_FG] / el, (T.sum[T_PGAME] + T.sum[T_PGEN]) / el, T.sum[T_PGAME] / el, T.sum[T_PGEN] / el);
    VLOG("%s", b);
    memset(T.ns, 0, sizeof T.ns); memset(T.sum, 0, sizeof T.sum); T.ngen = 0; T.t0 = now;
}
static void t_ts(VkCommandBuffer cb, VkPipelineStageFlagBits st, uint32_t q) { T.wr(cb, st, T.qp, q); }
void vka_fgt_fg_begin(VkCommandBuffer cb, unsigned k) {
    k %= NFGS; T.fg_on[k] = 0;
    if (t_iv() <= 0 || !t_init()) return;
    T.rs(cb, T.qp, k * Q_FG, Q_FG); t_ts(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, k * Q_FG);
    T.fg_on[k] = 1; T.fg_split[k] = VKA_ENV("DM_VK_FG_GPUTIME_SPLIT", 0) != 0;
}
void vka_fgt_fg_mark(VkCommandBuffer cb, unsigned k, int which) { k %= NFGS; if (T.fg_on[k] && T.fg_split[k]) t_ts(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, k * Q_FG + (uint32_t)which); }
void vka_fgt_fg_end(VkCommandBuffer cb, unsigned k, int ng) { k %= NFGS; if (T.fg_on[k]) { t_ts(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, k * Q_FG + 3); T.fg_ng[k] = ng; } }
void vka_fgt_fg_done(unsigned k) {   // after the slot's fence: the last use's values
    k %= NFGS; if (!T.fg_on[k]) return;
    T.fg_on[k] = 0; uint64_t v[4];
    if (T.fg_split[k]) { if (T.get(V.dev, T.qp, k * Q_FG, 4, sizeof v, v, 8, VK_QUERY_RESULT_64_BIT)) return; }
    else { uint64_t a[1], c[1]; if (T.get(V.dev, T.qp, k * Q_FG, 1, 8, a, 8, VK_QUERY_RESULT_64_BIT) || T.get(V.dev, T.qp, k * Q_FG + 3, 1, 8, c, 8, VK_QUERY_RESULT_64_BIT)) return; v[0] = a[0]; v[3] = c[0]; }
    double ms = T.tick / 1e6;
    t_add(T_FG, (double)(v[3] - v[0]) * ms); if (T.fg_ng[k]) T.ngen++;
    if (T.fg_split[k]) { t_add(T_COPY, (double)(v[1] - v[0]) * ms); t_add(T_SHARED, (double)(v[2] - v[1]) * ms); t_add(T_GEN, (double)(v[3] - v[2]) * ms); }
    t_report();
}
void vka_fgt_pr_begin(VkCommandBuffer cb, unsigned k, int gen) {
    k %= NPRS; T.pr_on[k] = 0;
    if (t_iv() <= 0 || !t_init()) return;
    T.rs(cb, T.qp, Q_PR0 + k * 2, 2); vka_barrier(cb); t_ts(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, Q_PR0 + k * 2);
    T.pr_on[k] = 1; T.pr_gen[k] = gen;
}
void vka_fgt_pr_end(VkCommandBuffer cb, unsigned k) { k %= NPRS; if (T.pr_on[k]) { vka_barrier(cb); t_ts(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, Q_PR0 + k * 2 + 1); } }
void vka_fgt_pr_done(unsigned k) {
    k %= NPRS; if (!T.pr_on[k]) return;
    T.pr_on[k] = 0; uint64_t v[2];
    if (T.get(V.dev, T.qp, Q_PR0 + k * 2, 2, sizeof v, v, 8, VK_QUERY_RESULT_64_BIT)) return;
    t_add(T.pr_gen[k] ? T_PGEN : T_PGAME, (double)(v[1] - v[0]) * T.tick / 1e6);
    if (t_iv() > 0 && !T.ns[T_FG]) t_report();   // (frame generation off: the presents alone)
}
#endif
