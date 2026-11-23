// SPDX-License-Identifier: MIT
// Frame generation under the game's UI, and a scene cut guard for it; all off by default (nothing is created, recorded or logged while its switches are unset).
//
// A game that draws its HUD into a layer of its own and puts it over the 3D picture with one full-screen draw gives frame generation a picture without UI: the target of
// the pass before that draw, as it is at the pass's end. Two fragment shader keys name the passes, from the game's profile (game.tsv fg_ui_pre, fg_ui_layer; vka_fgprof.c)
// or the switches, which win:
//   DM_FG_UI_PRE=<hex>  the last pass before the UI composite (its colour target 0, RGBA8, is the picture without UI)
//   DM_FG_UI_COMP=<hex> the UI composite draw (its last sampled texture, RGBA8 of the same size, is the UI layer)
// (Fire Emblem Fortune's Weave: the tonemap pass writes the 8-bit picture, then fragment c478e7c70173f0b8 samples the UI layer and draws it over that picture.)
// Snapshots (a copy of each at its pass's end, into images of our own, a ring of NS frames) are taken while DM_FG_UI_SNAP=1 or DM_FG_UI=layer (live), or the
// motion-vector engine runs. A frame's snapshots belong to the window texture its next pass into a presented window texture writes (FEFW copies the picture into its
// 3840x2160 window texture with fragment 262bfc7f8d45e140): the present's fence is one lower than its frame's tonemap submission, and a fence rule took the frame before.
// vka_framegen.c takes them by the window texture it presents.
//
// DM_FG_UI=layer (live): frame generation reads the picture without UI, and each generated frame gets the UI layer of the game frame it follows (frame C) put over it as the
// game does (by transmittance, the only composite: the layer's alpha is what shows through, linear out = L.rgb + base.rgb * L.a in sRGB; measured on FEFW's snapshots
// against its presented picture, 99.98 % of pixels within 1 LSB). A frame without snapshots (no such passes, another size or format) is generated from the window texture
// as before, and a change between the two is a cut.
// DM_FG_CUT=repeat (live; with the motion-vector engine also the profile's fg_feature cut): a generated frame across a scene cut (a camera jump, a menu or map opening) is
// frame C again instead. On 240x135 luma thumbnails of the two game frames A and C, A warped by C's DLSS motion vectors (none without a DLSS op): cut = (res > DM_FG_CUT_RES
// and median tile correlation < DM_FG_CUT_CORR) or mean |mv| > DM_FG_CUT_MV render pixels or (DM_FG_CUT_DLSS=1 and the DLSS op ran in one of A and C only). The
// profile's values by default (fg_tune cut_res, cut_corr, cut_mv as a fraction of the render width); DM_FG_CUT_DLSS 1. DM_FG_CUT_LOG=<n> logs the first n decisions.
// DM_FG_UI_T2T_LOG=<n> (live) logs the first n texture copies of at least 1280 pixels a side (vka_exec.c copy_t2t) and the passes into the presented window texture.
#if defined(__ANDROID__)
#include "vka.h"
#include "vk_android.h"
#include "vka_fgui_spv.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { NS = 6, NW = 4, NK = 4, TW = 240, TH = 135 };
typedef struct { vka_img *im; VkImageView v; } oimg;
static struct { uint64_t fence, frame, n; oimg pre, layer; int has_pre, has_layer; uint64_t pre_serial, layer_serial; } S[NS];
static struct { uint64_t frame, n; oimg mv; uint32_t iw, ih; float s[2]; int ok; } M[NS];   // the DLSS ops' motion vectors (DM_FG_CUT)
static struct { uint64_t serial, pre_q, mv_q, op_q; } Wm[NW]; static unsigned g_wi;   // window texture -> its frame's snapshot (q = ring number + 1; 0 = none)
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_n, g_dn;                 // snapshots / motion vector copies begun (the recording thread writes, the present thread reads under g_mu)
static uint64_t g_kpre, g_kcomp;
static uint64_t g_nlay; static int g_badfmt, g_badfmt_vk;   // layers snapped; the pre-UI target / UI layer refused for their format (1 / 2) and its VkFormat (vka_fgui_nopre)
static uint32_t g_gen = UINT32_MAX; static int g_on, g_lay, g_cut, g_log;
static vka_img *g_pass_c0; static int g_pass_pre, g_pass_comp, g_layer_tid;   // the pass in progress (recording thread)
static _Atomic uint64_t g_win[2];          // the last presented window texture serials (vka_fgui_note_present)

uint64_t vka_exec_cur_fence(void); uint64_t vka_frame(void); uint64_t now_ns(void);
static void keys(void) {
    uint32_t g = atomic_load_explicit(&vka_env_gen, memory_order_relaxed);
    if (g == g_gen) return;
    g_gen = g; char b[64];
    const vka_fgprof *FP = vka_fgprof_get();
    g_kpre = vka_env_str("DM_FG_UI_PRE", b, sizeof b) ? strtoull(b, NULL, 16) : FP->ui_pre;
    g_kcomp = vka_env_str("DM_FG_UI_COMP", b, sizeof b) ? strtoull(b, NULL, 16) : FP->ui_layer;
    int layer = vka_env_str("DM_FG_UI", b, sizeof b) && !strcmp(b, "layer");
    int was = g_on, wl = g_lay, wc = g_cut;
    g_on = (layer || VKA_ENV("DM_FG_UI_SNAP", 0)) && g_kpre && g_kcomp; g_lay = layer && g_on;
    g_cut = vka_env_str("DM_FG_CUT", b, sizeof b) ? !strcmp(b, "repeat") : 0;
    g_log = VKA_ENV("DM_FG_UI_T2T_LOG", 0);
    if (g_on != was) VLOG("fg ui: snapshots %s (pre-UI pass fragment %016llx, UI composite fragment %016llx)", g_on ? "on" : "off", (unsigned long long)g_kpre, (unsigned long long)g_kcomp);
    if (g_lay != wl) VLOG("fg ui: frame generation under the UI %s", g_lay ? "on (DM_FG_UI=layer)" : "off");
    if (g_cut != wc) VLOG("fg cut: scene cut guard %s", g_cut ? "on (DM_FG_CUT=repeat or the profile's cut rule)" : "off");
}
int vka_fgui_on(void) { keys(); return g_on; }
int vka_fgui_layer(void) { keys(); return g_lay; }
int vka_fgui_cut(void) { keys(); return g_cut; }

// An image of our own with a view (as vka_framegen.c out_new); 0 on failure
static int oimg_new(oimg *o, uint32_t w, uint32_t h, VkFormat fmt, uint64_t serial) {
    vka_img *im = calloc(1, sizeof *im);
    if (!im) return 0;
    im->serial = serial; im->refs = 1; im->inited = 1; im->gpu_owned = 1;
    im->f = (vka_fmt){fmt, fmt, 0, 0, 4, 1, 1};
    im->d.target = NVN_TEXTURE_TARGET_2D; im->d.width = w; im->d.height = h;
    im->usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    im->aspect = VK_IMAGE_ASPECT_COLOR_BIT; im->w = w; im->h = h; im->depth = im->layers = im->levels = im->samples = 1;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = im->usage};
    VkResult r = vk.vkCreateImage(V.dev, &ci, NULL, &im->img);
    VkMemoryRequirements mr = {0}; if (!r) vk.vkGetImageMemoryRequirements(V.dev, im->img, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0)};
    if (!r && ai.memoryTypeIndex == UINT32_MAX) r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (!r) r = vk.vkAllocateMemory(V.dev, &ai, NULL, &im->m.dm);
    if (!r) r = vk.vkBindImageMemory(V.dev, im->img, im->m.dm, 0);
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (!r) r = vk.vkCreateImageView(V.dev, &vi, NULL, &o->v);
    if (r) { VRED("fg ui: image %ux%u VkFormat %d: %d", w, h, fmt, r); o->v = VK_NULL_HANDLE; vka_tex_drop(im); return 0; }
    o->im = im; return 1;
}
static void oimg_free(oimg *o) { if (o->v) vk.vkDestroyImageView(V.dev, o->v, NULL); if (o->im) vka_tex_drop(o->im); o->v = VK_NULL_HANDLE; o->im = NULL; }
static void general(VkCommandBuffer cb, VkImage img) {   // a new image's layout
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

// The copy of a game image's level 0 / layer 0 into ours (32-bit texels on both sides: a bit copy, also from an sRGB format)
static int snap(VkCommandBuffer cb, const vka_img *src, oimg *dst, VkFormat fmt, uint64_t serial) {
    if (!src || src->aspect != VK_IMAGE_ASPECT_COLOR_BIT || src->f.bpb != 4 || src->f.bw > 1 || src->samples != 1 || !src->img) return 0;
    uint32_t lv = 0; if (!vka_img_lv(src, &lv) || lv) return 0;
    if (dst->im && (dst->im->w != src->w || dst->im->h != src->h)) oimg_free(dst);   // (a size change: the GPU's earlier uses are behind the frames' fences)
    if (!dst->im) { if (!oimg_new(dst, src->w, src->h, fmt, serial)) return 0; general(cb, dst->im->img); }
    VkImageCopy c = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {src->w, src->h, 1}};
    vk.vkCmdCopyImage(cb, src->img, VK_IMAGE_LAYOUT_GENERAL, dst->im->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    return 1;
}

static int rgba8(const vka_img *im) { return im->f.vk == VK_FORMAT_R8G8B8A8_UNORM || im->f.vk == VK_FORMAT_R8G8B8A8_SRGB; }   // the pre-UI and layer formats the composite reads
// The recording thread: each draw (vka_draw.c), each texture binding of a fragment shader (vka_draw.c stage_writes), each pass end (vka_draw.c vka_rp_end)
void vka_fgui_draw(uint64_t fkey, vka_img *c0) {
    keys(); if (!g_on || !fkey) return;
    if (fkey == g_kpre) { g_pass_pre = 1; g_pass_c0 = c0; }
    else if (fkey == g_kcomp) g_pass_comp = 1;
}
void vka_fgui_tex(uint64_t key, int tid) { if (g_on && key == g_kcomp && tid) g_layer_tid = tid; }
void vka_fgui_pass_end(VkCommandBuffer cb) {
    if (!g_pass_pre && !g_pass_comp) return;
    int pre = g_pass_pre, comp = g_pass_comp; vka_img *c0 = g_pass_c0; g_pass_pre = g_pass_comp = 0; g_pass_c0 = NULL;
    if (!g_on || !cb) return;
    uint64_t fr = vka_frame(), fe = vka_exec_cur_fence();
    if (pre && c0 && !rgba8(c0)) {   // (a 32-bit target of another format would be read as RGBA8: no snapshot)
        if (!g_badfmt) VRED("fg ui: the pre-UI target (serial %llu) is VkFormat %d, not RGBA8: no picture without UI", (unsigned long long)c0->serial, c0->f.vk);
        g_badfmt = 1; g_badfmt_vk = c0->f.vk; pre = 0;
    }
    if (pre && c0) {
        pthread_mutex_lock(&g_mu); unsigned k = (unsigned)(g_n % NS); S[k].has_pre = S[k].has_layer = 0; pthread_mutex_unlock(&g_mu);   // (taken as not ready while it is rewritten)
        vka_barrier(cb);
        int ok = snap(cb, c0, &S[k].pre, VK_FORMAT_R8G8B8A8_UNORM, 0xF6100000ull + k);
        vka_barrier(cb);
        pthread_mutex_lock(&g_mu); S[k].fence = fe; S[k].frame = fr; S[k].n = g_n; S[k].has_pre = ok; S[k].pre_serial = c0->serial; g_n++; pthread_mutex_unlock(&g_mu);
        static int told; if (!told && ok) { told = 1; VLOG("fg ui: first pre-UI snapshot: serial %llu %ux%u VkFormat %d, frame %llu fence %llu", (unsigned long long)c0->serial, c0->w, c0->h, c0->f.vk, (unsigned long long)fr, (unsigned long long)fe); }
        if (!ok) { static int bad; if (!bad++) VRED("fg ui: the pre-UI target (serial %llu, VkFormat %d, %u bytes a texel) cannot be copied: no snapshot", (unsigned long long)c0->serial, c0->f.vk, c0->f.bpb); }
    }
    if (comp) {
        pthread_mutex_lock(&g_mu); int have = g_n > 0; unsigned k = (unsigned)((g_n + NS - 1) % NS); int same = have && S[k].frame == fr && S[k].has_pre; pthread_mutex_unlock(&g_mu);
        if (!same) return;
        vka_img *L = g_layer_tid ? vka_tex_id_img(g_layer_tid, cb) : NULL;
        vka_barrier(cb);
        if (L && !rgba8(L)) { if (g_badfmt != 2) VRED("fg ui: the UI layer (serial %llu) is VkFormat %d, not RGBA8: no picture without UI", (unsigned long long)L->serial, L->f.vk); g_badfmt = 2; g_badfmt_vk = L->f.vk; return; }
        int ok = L && L->w == S[k].pre.im->w && L->h == S[k].pre.im->h && snap(cb, L, &S[k].layer, VK_FORMAT_R8G8B8A8_UNORM, 0xF6200000ull + k);
        if (ok) g_nlay++;
        vka_barrier(cb);
        pthread_mutex_lock(&g_mu); S[k].has_layer = ok; S[k].layer_serial = L ? L->serial : 0; pthread_mutex_unlock(&g_mu);
        static int told; if (!told && ok) { told = 1; VLOG("fg ui: first UI layer snapshot: serial %llu %ux%u VkFormat %d (texture ID %d), frame %llu fence %llu", (unsigned long long)L->serial, L->w, L->h, L->f.vk, g_layer_tid, (unsigned long long)fr, (unsigned long long)fe); }
        if (!ok) { static int bad; if (bad++ < 4) VRED("fg ui: UI layer (texture ID %d, %s) not snapped", g_layer_tid, L ? "other size or format" : "no image"); }
    }
}

// The DLSS op (vka_exec.c, FSR 3.1 path): its motion vectors (RG16F, raw * scale = render pixels) for the cut guard
void vka_fgui_dlss(VkCommandBuffer cb, vka_img *m, int iw, int ih, const float *scale) {
    keys(); if (!g_cut || !m || !cb) return;
    if (m->f.vk != VK_FORMAT_R16G16_SFLOAT) { VRED_ONCE("fg cut: motion vectors in VkFormat %d, not RG16F: the cut guard runs without them", m->f.vk); return; }
    uint64_t fr = vka_frame();
    pthread_mutex_lock(&g_mu); unsigned k = (unsigned)(g_dn % NS); M[k].ok = 0; pthread_mutex_unlock(&g_mu);
    vka_barrier(cb);
    int ok = snap(cb, m, &M[k].mv, VK_FORMAT_R16G16_SFLOAT, 0xF6300000ull + k);
    vka_barrier(cb);
    pthread_mutex_lock(&g_mu);
    M[k].n = g_dn; M[k].frame = fr; M[k].ok = ok; g_dn++;
    M[k].iw = iw > 0 && (uint32_t)iw <= m->w ? (uint32_t)iw : m->w; M[k].ih = ih > 0 && (uint32_t)ih <= m->h ? (uint32_t)ih : m->h;
    M[k].s[0] = scale && scale[0] ? scale[0] : 1; M[k].s[1] = scale && scale[1] ? scale[1] : 1;
    pthread_mutex_unlock(&g_mu);
}

// A pass into a presented window texture (vka_draw.c): that texture shows this frame, so this frame's snapshots go with it
static void win_map(uint64_t serial) {
    uint64_t fr = vka_frame(), pq = 0, mq = 0;
    pthread_mutex_lock(&g_mu);
    for (uint64_t q = g_n; q > 0 && q + NS > g_n + 1; q--) { unsigned k = (unsigned)((q - 1) % NS); if (S[k].n == q - 1 && S[k].has_pre && S[k].frame == fr) { pq = q; break; } }
    for (uint64_t q = g_dn; q > 0 && q + NS > g_dn + 1; q--) { unsigned k = (unsigned)((q - 1) % NS); if (M[k].n == q - 1 && M[k].ok && M[k].frame == fr) { mq = q; break; } }
    unsigned i = 0; while (i < NW && Wm[i].serial != serial) i++;
    if (i == NW) i = g_wi++ % NW;
    Wm[i].serial = serial; Wm[i].pre_q = pq; Wm[i].mv_q = mq;
    pthread_mutex_unlock(&g_mu);
    static int told; if (!told && (pq || mq)) { told = 1; VLOG("fg ui: window texture serial %llu written in frame %llu: snapshot %llu, motion vectors %llu", (unsigned long long)serial, (unsigned long long)fr, (unsigned long long)pq, (unsigned long long)mq); }
}

// The present thread: the snapshots of the frame that wrote this window texture. Returns whether there is any
int vka_fgui_take(uint64_t serial, vka_fgui_frame *u) {
    memset(u, 0, sizeof *u);
    keys(); if (!g_on && !g_cut) return 0;
    pthread_mutex_lock(&g_mu);
    for (unsigned i = 0; i < NW; i++) {
        if (Wm[i].serial != serial || !serial) continue;
        uint64_t q = Wm[i].pre_q;
        if (q && q + NS > g_n + 1) {   // (not the oldest slot: the recording thread may be rewriting it)
            unsigned k = (unsigned)((q - 1) % NS);
            if (S[k].n == q - 1 && S[k].has_pre && S[k].has_layer) {
                u->pre = S[k].pre.im->img; u->layer = S[k].layer.im->img; u->pre_v = S[k].pre.v; u->layer_v = S[k].layer.v; u->w = S[k].pre.im->w; u->h = S[k].pre.im->h;
                u->sfence = S[k].fence; u->pre_serial = S[k].pre_serial; u->layer_serial = S[k].layer_serial; u->has_ui = 1;
            }
        }
        u->op_q = Wm[i].op_q;
        q = Wm[i].mv_q;
        if (q && q + NS > g_dn + 1) {
            unsigned k = (unsigned)((q - 1) % NS);
            if (M[k].n == q - 1 && M[k].ok) { u->mv_v = M[k].mv.v; u->mw = M[k].iw; u->mh = M[k].ih; u->mvs[0] = M[k].s[0]; u->mvs[1] = M[k].s[1]; u->has_mv = 1; }
        }
        break;
    }
    pthread_mutex_unlock(&g_mu);
    return u->has_ui || u->has_mv || u->op_q;
}

// The present thread (vka_fgmv.c): the ring number (n + 1) of the pre-UI snapshot in image pre while it and its UI layer are readable with margin slots before the
// recording thread rewrites it (margin 1: vka_fgui_take's test); 0: gone or being rewritten
uint64_t vka_fgui_snap_q(VkImage pre, unsigned margin) {
    uint64_t q = 0;
    pthread_mutex_lock(&g_mu);
    for (unsigned k = 0; k < NS; k++)
        if (pre && S[k].pre.im && S[k].pre.im->img == pre && S[k].has_pre && S[k].has_layer && S[k].n + 1 + NS > g_n + margin) q = S[k].n + 1;
    pthread_mutex_unlock(&g_mu);
    return q;
}

// Why this frame has no picture without UI for the motion-vector engine (its status reason; the present thread): w x h the window crop it would replace
void vka_fgui_nopre(const vka_fgui_frame *u, uint32_t w, uint32_t h, char *b, size_t n) {
    keys();
    if (!g_kpre || !g_kcomp) { snprintf(b, n, "%s", "the game's profile names no pre-UI pass and UI layer (fg_ui_pre, fg_ui_layer; DM_FG_UI_PRE, DM_FG_UI_COMP)"); return; }
    pthread_mutex_lock(&g_mu); uint64_t np = g_n, nl = g_nlay; pthread_mutex_unlock(&g_mu);
    unsigned long long fr = (unsigned long long)vka_frame();
    if (g_badfmt) snprintf(b, n, "the %s is VkFormat %d, not RGBA8", g_badfmt == 1 ? "pre-UI target" : "UI layer", g_badfmt_vk);
    else if (!np) snprintf(b, n, "the pre-UI pass key %016llx never matched in %llu frames", (unsigned long long)g_kpre, fr);
    else if (!nl) snprintf(b, n, "the UI composite key %016llx never matched in %llu frames", (unsigned long long)g_kcomp, fr);
    else if (u && u->has_ui && (u->w != w || u->h != h)) snprintf(b, n, "it is %ux%u, the window's picture %ux%u", u->w, u->h, w, h);
    else snprintf(b, n, "%s", u && u->has_ui ? "the window's picture is cropped at an offset" : "no snapshot for this frame's window texture");
}

// ---------- The cut guard and the UI over the generated frames (compute, after the chain's generation; vka_framegen.c) ----------
enum { OFF_TILE = 259328, OFF_RES = 264704, BUF_SIZE = 264768 };   // thumbnails 2 x 240 x 135 floats, tiles 144 x 9, result 6 (256-aligned)
static struct {
    int state, mv_ok; VkDescriptorSetLayout dsl; VkPipelineLayout lay; VkPipeline p[4];   // thumb, tile, decide, out
    struct { VkBuffer b, hb; VkDeviceMemory m, hm; float *map; int used, flags, ts; uint64_t count; } k[NK];   // b: device local (the passes), hb: the result for the CPU
    PFN_vkCmdWriteTimestamp wr; PFN_vkCmdResetQueryPool rs; PFN_vkGetQueryPoolResults get; VkQueryPool qp; int tq; double tick, tsum[4], tt0; uint32_t tn;   // DM_VK_FG_GPUTIME: the passes' GPU time
    struct { VkImage img; VkImageView v; } iv[4]; unsigned ivn;
    oimg dmv; uint64_t n, cuts, forced, logged;
    VkDescriptorSetLayout tdsl; VkPipelineLayout tlay; VkPipeline tp; VkSampler ls;   // thumb2.comp (16 bilinear taps a thumb pixel)
    int th_ok, th_k; uint64_t th_count; uint32_t th_w, th_h;   // C's thumbnail of the last cut check: the next pair's A (slot th_k, count th_count, picture size)
} P;
typedef struct { uint32_t W, H, off, bw, bh, st; } pct;   // st: thumb2.comp's tap step (2: 16 taps a block, 4: 4)
typedef struct { uint32_t W, H, iw, ih; float mvx, mvy, t_res, t_corr, t_mv; uint32_t flags; } pcon;

static int post_ready(void) {
    if (P.state) return P.state > 0;
    P.state = -1;
    if (!V.push_desc || !vk.vkCmdPushDescriptorSetKHR) { VRED("fg ui: no push descriptors: frame generation under the UI and the cut guard are off"); return 0; }
    VkDescriptorSetLayoutBinding b[8];
    for (uint32_t i = 0; i < 8; i++) b[i] = (VkDescriptorSetLayoutBinding){i, i == 2 || i == 4 || i == 6 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, .bindingCount = 8, .pBindings = b};
    VkPushConstantRange pc = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pcon)};
    VkResult r = vk.vkCreateDescriptorSetLayout(V.dev, &dl, NULL, &P.dsl);
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &P.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pc};
    if (!r) r = vk.vkCreatePipelineLayout(V.dev, &li, NULL, &P.lay);
    const uint32_t *code[4] = {vka_fgui_thumb_spv, vka_fgui_tile_spv, vka_fgui_decide_spv, vka_fgui_out_spv};
    size_t size[4] = {sizeof vka_fgui_thumb_spv, sizeof vka_fgui_tile_spv, sizeof vka_fgui_decide_spv, sizeof vka_fgui_out_spv};
    for (int k = 0; k < 4 && !r; k++) {
        VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size[k], .pCode = code[k]}; VkShaderModule m = VK_NULL_HANDLE;
        r = vk.vkCreateShaderModule(V.dev, &mi, NULL, &m);
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"}, .layout = P.lay};
        if (!r) r = vk.vkCreateComputePipelines(V.dev, VK_NULL_HANDLE, 1, &ci, NULL, &P.p[k]);
        if (m) vk.vkDestroyShaderModule(V.dev, m, NULL);
    }
    {   // the thumbnail of one picture by bilinear taps (thumb2.comp)
        VkDescriptorSetLayoutBinding tb[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}, {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
        VkDescriptorSetLayoutCreateInfo tl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, .bindingCount = 2, .pBindings = tb};
        VkPushConstantRange tpc = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pct)};
        if (!r) r = vk.vkCreateDescriptorSetLayout(V.dev, &tl, NULL, &P.tdsl);
        VkPipelineLayoutCreateInfo tli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &P.tdsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &tpc};
        if (!r) r = vk.vkCreatePipelineLayout(V.dev, &tli, NULL, &P.tlay);
        VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof vka_fgui_thumb2_spv, .pCode = vka_fgui_thumb2_spv}; VkShaderModule m = VK_NULL_HANDLE;
        if (!r) r = vk.vkCreateShaderModule(V.dev, &mi, NULL, &m);
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"}, .layout = P.tlay};
        if (!r) r = vk.vkCreateComputePipelines(V.dev, VK_NULL_HANDLE, 1, &ci, NULL, &P.tp);
        if (m) vk.vkDestroyShaderModule(V.dev, m, NULL);
        VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxAnisotropy = 1};
        if (!r) r = vk.vkCreateSampler(V.dev, &si, NULL, &P.ls);
    }
    if (r) { VRED("fg ui: pipelines %d: frame generation under the UI and the cut guard are off", r); return 0; }
    VkFormatProperties fp; vk.vkGetPhysicalDeviceFormatProperties(V.pd, VK_FORMAT_R16G16_SFLOAT, &fp);
    P.mv_ok = !!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
    vk.vkGetPhysicalDeviceFormatProperties(V.pd, VK_FORMAT_R8G8B8A8_UNORM, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) { VRED("fg ui: RGBA8 is no storage image here: off"); return 0; }
    VLOG("fg ui: UI layer / cut guard pipelines ready%s", P.mv_ok ? "" : " (RG16F is no storage image here: the cut guard runs without motion vectors)");
    P.state = 1; return 1;
}
static int buf_new(VkDeviceSize size, VkMemoryPropertyFlags want, VkBufferUsageFlags use, VkBuffer *b, VkDeviceMemory *m, void **map) {
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = use};
    VkResult r = vk.vkCreateBuffer(V.dev, &bi, NULL, b);
    VkMemoryRequirements mr = {0}; if (!r) vk.vkGetBufferMemoryRequirements(V.dev, *b, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = vka_mem_type(mr.memoryTypeBits, want, 0)};
    if (!r && ai.memoryTypeIndex == UINT32_MAX) r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (!r) r = vk.vkAllocateMemory(V.dev, &ai, NULL, m);
    if (!r) r = vk.vkBindBufferMemory(V.dev, *b, *m, 0);
    if (!r && map) r = vk.vkMapMemory(V.dev, *m, 0, VK_WHOLE_SIZE, 0, map);
    if (r) { VRED_ONCE("fg ui: buffer: %d", r); return 0; }
    return 1;
}
// The passes' buffer in device memory (with a host-visible one the cut guard took 3.6 ms a frame on the test tablet), the result copied into a small host-visible one for the log
static int buf_ready(unsigned k, VkCommandBuffer cb) {
    if (P.k[k].map) return 1;
    void *p = NULL;
    if (!P.k[k].b && !buf_new(BUF_SIZE, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, &P.k[k].b, &P.k[k].m, NULL)) return 0;
    if (!buf_new(256, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &P.k[k].hb, &P.k[k].hm, &p)) return 0;
    memset(p, 0, 256); P.k[k].map = p;
    vk.vkCmdFillBuffer(cb, P.k[k].b, 0, BUF_SIZE, 0); vka_barrier(cb);
    return 1;
}
static int ts_ready(void) {   // the queue's timestamp bits, once
    if (P.tq) return P.tq > 0;
    P.tq = -1;
    P.wr = (PFN_vkCmdWriteTimestamp)vk.vkGetDeviceProcAddr(V.dev, "vkCmdWriteTimestamp"); P.rs = (PFN_vkCmdResetQueryPool)vk.vkGetDeviceProcAddr(V.dev, "vkCmdResetQueryPool");
    P.get = (PFN_vkGetQueryPoolResults)vk.vkGetDeviceProcAddr(V.dev, "vkGetQueryPoolResults");
    PFN_vkCreateQueryPool create = (PFN_vkCreateQueryPool)vk.vkGetDeviceProcAddr(V.dev, "vkCreateQueryPool");
    VkQueryPoolCreateInfo ci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = NK * 5};
    if (!P.wr || !P.rs || !P.get || !create || create(V.dev, &ci, NULL, &P.qp) || V.props.limits.timestampPeriod <= 0) return 0;
    P.tick = V.props.limits.timestampPeriod; P.tq = 1; return 1;
}
static void ts(VkCommandBuffer cb, unsigned k, int i) { if (P.k[k].ts) P.wr(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, P.qp, k * 5 + (uint32_t)i); }   // (each after a barrier: the work before it is done)
static VkImageView view_of(VkImage img) {   // the chain's inputs (RGBA8 UNORM, storage); a pre-UI snapshot read in place (vka_fgmv.c vka_fgmv_direct): its own view
    VkImageView sv = VK_NULL_HANDLE;
    pthread_mutex_lock(&g_mu); for (unsigned k = 0; k < NS; k++) if (S[k].pre.im && S[k].pre.im->img == img) sv = S[k].pre.v; pthread_mutex_unlock(&g_mu);
    if (sv) return sv;
    for (unsigned i = 0; i < P.ivn; i++) if (P.iv[i].img == img) return P.iv[i].v;
    if (P.ivn == 4) return VK_NULL_HANDLE;
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView v = VK_NULL_HANDLE;
    if (vk.vkCreateImageView(V.dev, &vi, NULL, &v)) return VK_NULL_HANDLE;
    P.iv[P.ivn].img = img; P.iv[P.ivn].v = v; P.ivn++;
    return v;
}
// The chain is gone (vka_framegen.c teardown, after the queue went idle): the views of its inputs
void vka_fgui_post_reset(void) { for (unsigned i = 0; i < P.ivn; i++) vk.vkDestroyImageView(V.dev, P.iv[i].v, NULL); P.ivn = 0; P.th_ok = 0; }
VkImageView vka_fgui_view(VkImage img) { return img ? view_of(img) : VK_NULL_HANDLE; }   // a view of an input (storage and sampled; vka_fgmv.c)

static void push(VkCommandBuffer cb, int pi, unsigned k, VkImageView a, VkImageView c, VkImageView mv, VkImageView gen, VkImageView layer, const pcon *pc) {
    VkDescriptorImageInfo im[5] = {{VK_NULL_HANDLE, a, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, c, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, mv, VK_IMAGE_LAYOUT_GENERAL},
                                   {VK_NULL_HANDLE, gen, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, layer, VK_IMAGE_LAYOUT_GENERAL}};
    VkDescriptorBufferInfo bu[3] = {{P.k[k].b, 0, OFF_TILE}, {P.k[k].b, OFF_TILE, OFF_RES - OFF_TILE}, {P.k[k].b, OFF_RES, BUF_SIZE - OFF_RES}};
    VkWriteDescriptorSet wr[8]; static const int imi[8] = {0, 1, -1, 2, -1, 3, -1, 4}, bui[8] = {-1, -1, 0, -1, 1, -1, 2, -1};
    for (uint32_t i = 0; i < 8; i++) {
        wr[i] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = i, .descriptorCount = 1,
            .descriptorType = bui[i] >= 0 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
        if (bui[i] >= 0) wr[i].pBufferInfo = &bu[bui[i]]; else wr[i].pImageInfo = &im[imi[i]];
    }
    vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, P.p[pi]);
    vk.vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_COMPUTE, P.lay, 0, 8, wr);
    vk.vkCmdPushConstants(cb, P.lay, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof *pc, pc);
}

// The thumbnails. C's always (16 bilinear taps a thumb pixel when the picture is a whole number of even blocks, else thumb.comp's box;
// 4 taps a block when its sides are multiples of 4);
// A's is C's of the check before when that was the frame before at the same size (copied from its slot), else made again
static void thumbs(VkCommandBuffer cb, unsigned k, VkImageView av, VkImageView cv, VkImageView mvv, VkImageView gv, VkImageView lv, const pcon *pc, uint64_t count) {
    uint32_t bw = pc->W / TW, bh = pc->H / TH;
    int taps = P.tp && bw >= 2 && bh >= 2 && !(bw & 1) && !(bh & 1) && bw * TW == pc->W && bh * TH == pc->H;
    uint32_t st = !(bw & 3) && !(bh & 3) ? 4 : 2;
    int cached = taps && P.th_ok && P.th_count + 1 == count && (unsigned)P.th_k != k && P.th_w == pc->W && P.th_h == pc->H;
    if (!taps) { push(cb, 0, k, av, cv, mvv, gv, lv, pc); vk.vkCmdDispatch(cb, TW, TH, 1); }
    else {
        if (cached) { VkBufferCopy bc = {TW * TH * 4, 0, TW * TH * 4}; vk.vkCmdCopyBuffer(cb, P.k[P.th_k].b, P.k[k].b, 1, &bc); }
        for (int side = cached ? 1 : 0; side < 2; side++) {
            VkDescriptorImageInfo ii = {P.ls, side ? cv : av, VK_IMAGE_LAYOUT_GENERAL}; VkDescriptorBufferInfo bi = {P.k[k].b, 0, OFF_TILE};
            VkWriteDescriptorSet w2[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
                                          {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi}};
            pct tc = {pc->W, pc->H, side ? TW * TH : 0, bw, bh, st};
            vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, P.tp);
            vk.vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_COMPUTE, P.tlay, 0, 2, w2);
            vk.vkCmdPushConstants(cb, P.tlay, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof tc, &tc);
            vk.vkCmdDispatch(cb, (TW + 15) / 16, (TH + 15) / 16, 1);
        }
    }
    P.th_ok = 1; P.th_k = (int)k; P.th_count = count; P.th_w = pc->W; P.th_h = pc->H;
}
static void cut_passes(VkCommandBuffer cb, unsigned k, VkImageView av, VkImageView cv, VkImageView mvv, VkImageView gv, VkImageView lv, const pcon *pc, uint64_t count) {
    thumbs(cb, k, av, cv, mvv, gv, lv, pc, count); vka_barrier(cb); ts(cb, k, 1);
    push(cb, 1, k, av, cv, mvv, gv, lv, pc); vk.vkCmdDispatch(cb, 16, 9, 1); vka_barrier(cb); ts(cb, k, 2);
    push(cb, 2, k, av, cv, mvv, gv, lv, pc); vk.vkCmdDispatch(cb, 1, 1, 1); vka_barrier(cb); ts(cb, k, 3);
    VkBufferCopy bc = {OFF_RES, 0, 32}; vk.vkCmdCopyBuffer(cb, P.k[k].b, P.k[k].hb, 1, &bc);
}
// The motion vector stand-in and the push constants (both callers)
static int prep(VkCommandBuffer cb, const vka_fgui_frame *u, uint32_t w, uint32_t h, int lay, int force, int dlss_change, int *cut, VkImageView *mvv, pcon *pc) {
    *cut = g_cut && P.mv_ok; int have_mv = *cut && u && u->has_mv;
    if (g_cut && !P.mv_ok) { VRED_ONCE("fg cut: no RG16F storage image: the cut guard is off"); }
    *mvv = have_mv ? u->mv_v : VK_NULL_HANDLE;
    if (*cut && !have_mv) {   // a 1x1 stand-in (not read without the flag)
        if (!P.dmv.im) { if (!oimg_new(&P.dmv, 1, 1, VK_FORMAT_R16G16_SFLOAT, 0xF6400000ull)) return 0; general(cb, P.dmv.im->img); }
        *mvv = P.dmv.v;
    }
    const vka_fgprof *FP = vka_fgprof_get(); double cmv = VKA_ENVF("DM_FG_CUT_MV", -1);   // DM_FG_CUT_MV in render px; else the profile's fraction of the render width
    *pc = (pcon){w, h, have_mv ? u->mw : 1, have_mv ? u->mh : 1, have_mv ? u->mvs[0] : 1, have_mv ? u->mvs[1] : 1,
               (float)VKA_ENVF("DM_FG_CUT_RES", FP->cut_res), (float)VKA_ENVF("DM_FG_CUT_CORR", FP->cut_corr), cmv >= 0 ? (float)cmv : (float)(FP->cut_mv * (have_mv ? u->mw : 1)),
               (have_mv ? 1u : 0u) | (dlss_change ? 2u : 0u) | (*cut ? 4u : 0u) | (lay ? 8u : 0u) | (VKA_ENV("DM_FG_CUT_DLSS", 1) ? 16u : 0u) | (force ? 32u : 0u)};
    return 1;
}

// After the generated frames are written (and a barrier): the cut guard on A / C and the UI layer of C over each generated frame. a / c: the chain's inputs of the
// two game frames; layer: put C's UI layer over them; force: repeat C (the input changed between pre-UI and post-UI); dlss_change: DLSS ran in one of A, C only
int vka_fgui_post(VkCommandBuffer cb, unsigned k, VkImage a, VkImage c, const VkImageView *gen, size_t ng, uint32_t w, uint32_t h, const vka_fgui_frame *u, int layer, int force, int dlss_change, uint64_t count) {
    keys();
    if (!ng || !post_ready()) return 0;
    k %= NK; if (!buf_ready(k, cb)) return 0;
    VkImageView av = view_of(a), cv = view_of(c);
    if (!av || !cv) { VRED_ONCE("fg ui: views of the chain's inputs: off"); return 0; }
    int lay = layer && u && u->has_ui, cut; VkImageView mvv; pcon pc;
    if (!prep(cb, u, w, h, lay, force, dlss_change, &cut, &mvv, &pc)) return 0;
    VkImageView lv = lay ? u->layer_v : cv;
    if (!cut) mvv = lv;
    P.k[k].ts = VKA_ENVF("DM_VK_FG_GPUTIME", 0) > 0 && ts_ready();
    if (P.k[k].ts) { P.rs(cb, P.qp, k * 5, 5); ts(cb, k, 0); }   // (no cut passes: binding 3 is not read; any valid view of a storage image does not do for RG16F, so the out pass gets one of its own kind)
    if (cut) cut_passes(cb, k, av, cv, mvv, gen[0], lv, &pc, count);
    else if (P.k[k].ts) { ts(cb, k, 1); ts(cb, k, 2); ts(cb, k, 3); }
    for (size_t g = 0; g < ng; g++) { push(cb, 3, k, av, cv, mvv, gen[g], lv, &pc); vk.vkCmdDispatch(cb, (w + 15) / 16, (h + 15) / 16, 1); }
    vka_barrier(cb); ts(cb, k, 4);
    P.k[k].used = 1; P.k[k].flags = (int)pc.flags; P.k[k].count = count;
    return 1;
}
// The cut guard alone (DM_FG_CUT=repeat) for vka_fgmv.c: its decision stays in the slot's buffer (*rb at *roff: rs[0] > 0.5 = cut) for the resolve.
// 0: no check
int vka_fgui_cutpass(VkCommandBuffer cb, unsigned k, VkImage a, VkImage c, uint32_t w, uint32_t h, const vka_fgui_frame *u, int dlss_change, uint64_t count, VkBuffer *rb, VkDeviceSize *roff) {
    keys();
    if (!g_cut || !post_ready()) return 0;
    k %= NK; if (!buf_ready(k, cb)) return 0;
    VkImageView av = view_of(a), cv = view_of(c);
    if (!av || !cv) { VRED_ONCE("fg cut: views of the inputs: off"); return 0; }
    int cut; VkImageView mvv; pcon pc;
    if (!prep(cb, u, w, h, 0, 0, dlss_change, &cut, &mvv, &pc) || !cut) return 0;
    P.k[k].ts = VKA_ENVF("DM_VK_FG_GPUTIME", 0) > 0 && ts_ready();
    if (P.k[k].ts) { P.rs(cb, P.qp, k * 5, 5); vka_barrier(cb); ts(cb, k, 0); }
    cut_passes(cb, k, av, cv, mvv, cv, cv, &pc, count);
    ts(cb, k, 4);
    P.k[k].used = 1; P.k[k].flags = (int)pc.flags; P.k[k].count = count;
    *rb = P.k[k].b; *roff = OFF_RES;
    return 1;
}
// Its submission is done (vka_framegen.c, after its fence): the decision, logged (DM_FG_CUT_LOG) and counted
void vka_fgui_post_done(unsigned k) {
    k %= NK; if (!P.k[k].used) return;
    P.k[k].used = 0;
    if (P.k[k].ts) {   // DM_VK_FG_GPUTIME: thumbnails, tiles, decision, output (the UI and the repeat), one "fg ui gpu:" line every 10 s
        uint64_t v[5]; P.k[k].ts = 0;
        if (!P.get(V.dev, P.qp, k * 5, 5, sizeof v, v, 8, VK_QUERY_RESULT_64_BIT)) { for (int i = 0; i < 4; i++) P.tsum[i] += (double)(v[i + 1] - v[i]) * P.tick / 1e6; P.tn++; }
        double now = (double)now_ns() / 1e9; if (!P.tt0) P.tt0 = now;
        if (now - P.tt0 >= 10 && P.tn) { VLOG("fg ui gpu: %u frames, mean ms: thumbnails %.3f, tiles %.3f, decision %.3f, output %.3f (total %.3f)", P.tn, P.tsum[0] / P.tn, P.tsum[1] / P.tn, P.tsum[2] / P.tn, P.tsum[3] / P.tn,
                                         (P.tsum[0] + P.tsum[1] + P.tsum[2] + P.tsum[3]) / P.tn); memset(P.tsum, 0, sizeof P.tsum); P.tn = 0; P.tt0 = now; }
    }
    int f = P.k[k].flags; if (!(f & 36)) return;
    const float *rs = P.k[k].map;
    int cut = (f & 32) || ((f & 4) && rs[0] > 0.5f);
    P.n++; if (cut) P.cuts++; if (f & 32) P.forced++;
    int nlog = VKA_ENV("DM_FG_CUT_LOG", 0);
    if ((f & 4) && P.logged < (uint64_t)nlog) { P.logged++;
        VLOG("fg cut: frame %llu %s res %.2f corr %.3f mvmean %.1f tiles %.0f mv %d dlss change %d ui %d", (unsigned long long)P.k[k].count, cut ? (f & 32 ? "REPEAT (forced)" : "REPEAT") : "generated",
             rs[1], rs[2], rs[3], rs[4], f & 1, (f >> 1) & 1, (f >> 3) & 1); }
    if (P.n % 900 == 0) VLOG("fg cut: %llu frames checked, %llu repeated (%llu forced by a change between pre-UI and post-UI input)", (unsigned long long)P.n, (unsigned long long)P.cuts, (unsigned long long)P.forced);
}

// Logs (DM_FG_UI_T2T_LOG): what writes the presented window texture
void vka_fgui_note_present(uint64_t serial) { if (atomic_load(&g_win[0]) != serial) { atomic_store(&g_win[1], atomic_load(&g_win[0])); atomic_store(&g_win[0], serial); } }
static _Atomic int g_logged;
static int log_room(void) { keys(); int n = atomic_load(&g_logged); while (n < g_log) if (atomic_compare_exchange_weak(&g_logged, &n, n + 1)) return 1; return 0; }
void vka_fgui_t2t(const vka_img *s, const vka_img *d, int sx, int sy, int dx, int dy, int w, int h, uint32_t flags, const char *path) {
    if (!g_log || !s || !d || (s->w < 1280 && d->w < 1280) || !log_room()) return;
    uint64_t w0 = atomic_load(&g_win[0]), w1 = atomic_load(&g_win[1]);
    VLOG("fg ui t2t: frame %llu serial %llu (%ux%u VkFormat %d) %d,%d -> serial %llu (%ux%u VkFormat %d) %d,%d size %dx%d flags %#x %s%s", (unsigned long long)vka_frame(), (unsigned long long)s->serial, s->w, s->h, s->f.vk, sx, sy,
         (unsigned long long)d->serial, d->w, d->h, d->f.vk, dx, dy, w, h, flags, path, d->serial == w0 || d->serial == w1 ? " [into a presented window texture]" : "");
}
void vka_fgui_pass_begin(uint64_t c0_serial, uint32_t w, uint32_t h) {
    if (!c0_serial) return;
    keys(); if (!g_log && !g_on && !g_cut) return;
    uint64_t w0 = atomic_load(&g_win[0]), w1 = atomic_load(&g_win[1]);
    if (c0_serial != w0 && c0_serial != w1) return;
    if (g_on || g_cut) win_map(c0_serial);
    if (g_log && log_room()) VLOG("fg ui pass: frame %llu pass %ux%u into presented window texture serial %llu", (unsigned long long)vka_frame(), w, h, (unsigned long long)c0_serial);
}
void vka_fgui_draw_log(uint64_t vkey, uint64_t fkey, uint64_t c0_serial) {
    if (!g_log || !c0_serial) return;
    uint64_t w0 = atomic_load(&g_win[0]), w1 = atomic_load(&g_win[1]);
    if ((c0_serial == w0 || c0_serial == w1) && log_room()) VLOG("fg ui draw: frame %llu into presented window texture serial %llu: vertex %016llx fragment %016llx", (unsigned long long)vka_frame(), (unsigned long long)c0_serial, (unsigned long long)vkey, (unsigned long long)fkey);
}
#endif
