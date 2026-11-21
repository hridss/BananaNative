// SPDX-License-Identifier: MIT
// A motion-vector frame generator (DM_FG_ENGINE=mv, the launcher's Native FG), in the frame generation's place of LSFG (vka_framegen.c) and without its DLL. Per game it
// needs a profile (game.tsv fg_* rows, vka_fgprof.c); the DM_FG_MV_* switches override its values. Off by default: with the engine not picked nothing here is created,
// recorded or logged, and LSFG runs as before.
//
// Inputs: the picture without UI of the two game frames A (earlier) and C (vka_fgui.c snapshots), C's UI layer, and per game frame what one compute pass at its DLSS op
// (vka_exec.c dlss_eval, any DM_VK_DLSS mode with a depth and a motion vector texture) keeps: the depth, the 3x3 nearest-depth dilated motion vectors and the distance to
// the nearest depth edge (fgmv/op.comp). Per generated frame at time t: a depth-priority forward splat of C's vectors (one 32-bit atomic max a texel), a bounded hole fill,
// and the display resolve (Catmull-Rom from A and C, per-side occlusion test, the soft band at depth edges, C's UI layer over it) (fgmv/*.comp). A game frame pair without
// that data for both frames (no DLSS op: the map, full-screen menus; an op record of a kind never validated), without the picture without UI, or across a cut
// (DM_FG_CUT=repeat or the profile's cut rule) shows the game frame C itself (under its UI layer): the motion-vector engine and LSFG never run on the same frame. Each such
// cause is a status reason (nxhost_framegen_status) and a log line. The pacer is LSFG's (vka_framegen.c, through its pacing seam).
// The op record's conventions the engine was validated on (NGX flags): depth inverted (8), low-resolution motion vectors (2), not jittered (4); any other combination,
// texture offsets (an atlas) or another motion format: no motion data for that frame (a status reason).
// At its first use a self-test runs 32768 image atomics and checks them; when it fails the frame generation stays with LSFG (logged).
// Switches (all live): DM_FG_ENGINE=mv|lsfg; DM_FG_MV_DEBUG=1 (state: red A only, green C only, blue blend, dim blue disagree) | 2 (vectors) | 3 (C's edge distance) |
// 4 (no UI layer) | 5 (the UI layer the composite uses) | 6 (the replay's static weight dump holds the depth gate); the profile's tuning: DM_FG_MV_SOFT, DM_FG_MV_RATIO,
// DM_FG_MV_TOL, DM_FG_MV_EDGE_THR, DM_FG_MV_FILL (at most 6), DM_FG_MV_UISR; its rules: DM_FG_MV_DITHER (the screen-door dither rule, fgmv/op.comp,
// splat.comp), DM_FG_MV_UIFLOW (world-anchored labels: C's UI layer with its moving tile groups pasted at time t, fgmv/uq4, utile, ugrp, uref.comp; displays up to the
// tile grid's 1920x1088), DM_FG_MV_UIGATE (a group moves only with C's vectors about as large as its flow), DM_FG_MV_STATIC (1 half size, 2 quarter: the static candidate,
// content without its own vectors such as a character's shadow blended unwarped where that explains A and C better, fgmv/stws.comp), DM_FG_MV_STATICZ (not where the
// field's surface is in front of what A and C show there), DM_FG_MV_STFIX (the static candidate also for a screen-fixed additive overlay without vectors: a 3D cursor
// the camera follows), DM_FG_MV_BOTHFAR (both sides occluded: the side whose occluder is farther, not C).
// Diagnostics: DM_VK_FG_GPUTIME=<s> ("fg mv gpu:" per pass, "fg mv op gpu:"), DM_FG_MV_OPSYNC=1 (the op pass's timestamps after an encoder sync: a valid op time, it
// stalls the recording thread each frame), DM_FG_MV_REPLAY=<tag> (a new value runs once): <DM_CTL folder>/fgmv/r<tag>/manifest.txt through the same passes, each pair
// twice (see mv_replay), DM_FG_MV_REPLAY_DUMP=1.
#if defined(__ANDROID__)
#include "vka.h"
#include "vk_android.h"
#include "vka_fgmv_spv.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

uint64_t vka_frame(void); uint64_t now_ns(void);
enum { NO = 8, NG = 3, NKS = 4, NQ = 10, NUQ = 10 };   // NQ: timestamps a frame (fg mv gpu); NUQ: the UI flow's (DM_VK_FG_GPUTIME_SPLIT=1, fg mv ui gpu)
enum { PO, PS, PR, PA, PF2, PQ4, PTI, PGR, PRF, PR2, PSS16, PUC, POT, PSP, NP };   // the pipelines: op, splat, resolve, atomic self-test, fill;
                                                    // UI flow: coverage, tiles, groups, refine; resolve with the extended rules; static weight (two passes, 16x16
                                                    // workgroup tiles); the tiles' classify pass; the op with the dither test from bit masks; the UI flow's split rule
// The UI flow's buffer (words; fgmv/ui.glsl)
enum { UI_O_MV = 375380, UI_O_SH = 377420, UI_O_GF = 369260, UI_O_GVM = 371300, UI_O_GSTAT = 373340, UI_O_GSZ = 314180, UI_O_GM2 = 310100, UI_O_CNT = 399860, UI_O_GE = 399924,
       UI_O_GRP = 401964, UI_O_GSM = 404004, UI_O_SFL = 410124, UI_O_SLB = 414204, UI_O_SG = 420324, UI_O_SCN = 425444, UI_O_SEXS = 425460, UI_O_SCC = 427500, UI_O_SPX = 429540, UI_O_RFL = 757220, UI_O_SX17 = 759260, UI_O_SGX = 759292, UI_O_END = 769532,
       UI_NCNT = 32, UI_NX17 = 8, UI_C_IND = 32, UI_EXMAX = 320, UI_NT = 2040, UI_SGTOT = 960 };
enum { UC_NE, UC_W, UC_CAP, UC_GROUPS, UC_ACTIVE, UC_MOVG, UC_VETO, UC_MOVED, UC_OVER, UC_MAXSRC, UC_ON, UC_SEARCHED, UC_NM, UC_NEEDBIG, UC_BIGS, UC_WBIG, UC_FL,
       UC_VGATE, UC_VCONS, UC_REPICK, UC_SMALL, UC_POVER, UC_PARTU, UC_PARTT, UC_SRCMX, UC_STRIED, UC_SKEPT, UC_STILES, UC_SGUARD, UC_SR2, UC_SOVER, UC_SNOA2 };
_Static_assert(UI_O_SEXS >= UI_O_SCN + 16 && UI_O_SCC >= UI_O_SEXS + UI_NT && UI_O_SPX >= UI_O_SCC + UI_NT && UI_O_RFL >= UI_O_SPX + UI_EXMAX * 1024 && UI_O_SX17 >= UI_O_RFL + UI_NT &&
               UI_O_SGX >= UI_O_SX17 + 32 && UI_O_END >= UI_O_SGX + (UI_SGTOT - 320) * 16,
               "the UI flow's buffer words (fgmv/ui.glsl O_*)");
enum { SG_N, SG_FIRST, SG_WHY, SG_D4, SG_F, SG_KT, SG_GUARD, SG_R2, SG_PASS = 12 };   // a split sub-group's words (ui.glsl SG_*, 16 each; sgb)
enum { X_MORE, X_P2, X_P3, X_IADD, X_IDROP, X_TORN, X_G2P, X_OVR, X_IND };   // the split's extra counters and later passes' indirect arguments (ui.glsl X_*, O_SX17)
static uint32_t sgb(uint32_t g) { return g < 320 ? UI_O_SG + g * 16 : UI_O_SGX + (g - 320) * 16; }
typedef struct { vka_img *im; VkImageView v; } mimg;
typedef struct { mimg z, d, v, e, f; uint32_t w, h, cw, ch; } opset;   // raw depth R32F, dilated depth R32F, dilated vectors RG16F (raw), edge distance R8, dither region R8UI;
                                                                    // w x h in use of the images' cw x ch (the largest render size seen)

static uint32_t g_gen = UINT32_MAX; static int g_want, g_dbg, g_soft;
static int g_state;   // 0 not tried, 1 ready (pipelines + self-test passed), -1 unavailable
static float g_ratio, g_tol, g_thr; static int g_fill, g_dither;
enum { LEV = 1 | 2 | 4 };   // the UI flow shaders' bits (ui.glsl L_*: 1 split over changed-pixel lists, 2 refine over the accepted-tile list, 4 re-pick a workgroup per group)
static int g_uiflow, g_uigate, g_uirules, g_x17, g_npass, g_uicapn, g_uisr, g_static, g_staticz, g_stfix, g_bothfar, g_opsync;
static void keys(void) {
    uint32_t g = atomic_load_explicit(&vka_env_gen, memory_order_relaxed);
    if (g == g_gen) return;
    g_gen = g; const vka_fgprof *P = vka_fgprof_get(); unsigned F = P->features; int env;
    int was = g_want; g_want = vka_fgprof_engine(&env);
    g_dbg = VKA_ENV("DM_FG_MV_DEBUG", 0); g_soft = VKA_ENV("DM_FG_MV_SOFT", P->soft);
    g_ratio = (float)(1.0 + VKA_ENVF("DM_FG_MV_RATIO", P->ratio)); g_tol = (float)VKA_ENVF("DM_FG_MV_TOL", P->tol); g_thr = (float)VKA_ENVF("DM_FG_MV_EDGE_THR", P->edge);
    g_fill = VKA_ENV("DM_FG_MV_FILL", P->fill); if (g_fill < 0) g_fill = 0; if (g_fill > 6) g_fill = 6;
    g_dither = VKA_ENV("DM_FG_MV_DITHER", !!(F & VKA_FGF_DITHER));
    g_uiflow = VKA_ENV("DM_FG_MV_UIFLOW", !!(F & VKA_FGF_UIFLOW)); g_uigate = VKA_ENV("DM_FG_MV_UIGATE", !!(F & VKA_FGF_UIGATE));
    g_uirules = g_uigate ? 1 | (VKA_ENV("DM_FG_MV_UIPICK", !!(F & VKA_FGF_UIPICK)) ? 2 : 0) | (VKA_ENV("DM_FG_MV_UICONS", !!(F & VKA_FGF_UICONS)) ? 4 : 0) |
                           (VKA_ENV("DM_FG_MV_UISMALL", !!(F & VKA_FGF_UISMALL)) ? 8 : 0) | (VKA_ENV("DM_FG_MV_UICAP", !!(F & VKA_FGF_UICAP)) ? 16 : 0) : 0;
    if ((g_uirules & 14) == 14 && VKA_ENV("DM_FG_MV_UISPLIT", !!(F & VKA_FGF_UISPLIT)))   // (the split rule needs the pick, consistency and small-group rules; its guard and rank-2 paste act only with it)
        g_uirules |= 32 | (VKA_ENV("DM_FG_MV_UISGUARD", P->uisguard) ? 64 : 0) | (VKA_ENV("DM_FG_MV_UISPASTE", 1) ? 128 : 0);
    {   // the split rule's options (usplit.comp pc.x17; 0 each: the basic rule): the 5x5 support, object integrity, passes, the reach-limited guard
        int sr = VKA_ENV("DM_FG_MV_UISSUPR", P->uissupr), ig = VKA_ENV("DM_FG_MV_UISINTEG", P->uisinteg), np = VKA_ENV("DM_FG_MV_UISPASS", P->uispass), gd = VKA_ENV("DM_FG_MV_UISGUARD", P->uisguard);
        sr = sr == 2 ? 2 : 1; ig = ig == 2 ? 2 : 0; np = np < 1 ? 1 : np > 3 ? 3 : np;
        g_x17 = (g_uirules & 32) ? (sr == 2 ? 1 : 0) | ig << 1 | (np - 1) << 3 | (gd == 2 && (g_uirules & 64) ? 32 : 0) : 0; g_npass = (g_uirules & 32) ? np : 1;
        static int lx = -1; if (g_x17 != lx) { lx = g_x17; VLOG("fg mv: split rule: support %dx%d, integrity %d, passes %d, guard %d", sr == 2 ? 5 : 3, sr == 2 ? 5 : 3, ig, np,
                                                                (g_uirules & 64) ? (gd == 2 ? 2 : 1) : 0); }
    }
                                                                       // (fgmv/ui.glsl R_*: the rules need the MV gate)
    g_uicapn = VKA_ENV("DM_FG_MV_UICAPN", 384); if (g_uicapn < 1 || g_uicapn > 384) g_uicapn = 384;   // (the tile cap rule's cap: a test hook, fgmv/ui.glsl UICAP)
    g_uisr = VKA_ENV("DM_FG_MV_UISR", P->uisr); if (g_uisr < 1) g_uisr = 1; if (g_uisr > 12) g_uisr = 12;
    g_static = VKA_ENV("DM_FG_MV_STATIC", !!(F & VKA_FGF_STATIC)); if (g_static < 0 || g_static > 2) g_static = 1;
    g_staticz = VKA_ENV("DM_FG_MV_STATICZ", !!(F & VKA_FGF_STATICZ)); g_stfix = VKA_ENV("DM_FG_MV_STFIX", !!(F & VKA_FGF_STFIX)); g_bothfar = VKA_ENV("DM_FG_MV_BOTHFAR", !!(F & VKA_FGF_BOTHFAR));
    g_opsync = VKA_ENV("DM_FG_MV_OPSYNC", 0);
    if (was != g_want) VLOG("fg mv: frame generation engine %s%s", g_want ? "motion vectors" : "LSFG", env ? " (DM_FG_ENGINE)" : " (the launcher's choice)");
    if (g_want && !was && !P->mv) VRED_ONCE("fg mv: this game's profile has no fg_mv row: the engine was not validated on it (dm/games/README.md, the fg_* keys)");
}
int vka_fgmv_wanted(void) { keys(); return g_want; }

// ---------- Objects ----------
static VkSampler g_near, g_lin;
static struct { VkDescriptorSetLayout dsl; VkPipelineLayout lay; VkPipeline p; } Pp[NP];
static int img_new(mimg *o, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags use, uint64_t serial) {
    vka_img *im = calloc(1, sizeof *im);
    if (!im) return 0;
    im->serial = serial; im->refs = 1; im->inited = 1; im->gpu_owned = 1;
    im->f = (vka_fmt){fmt, fmt, 0, 0, fmt == VK_FORMAT_R8_UNORM || fmt == VK_FORMAT_R8_UINT ? 1 : fmt == VK_FORMAT_R32G32_UINT ? 8 : 4, 1, 1};
    im->d.target = NVN_TEXTURE_TARGET_2D; im->d.width = w; im->d.height = h;
    im->usage = use; im->aspect = VK_IMAGE_ASPECT_COLOR_BIT; im->w = w; im->h = h; im->depth = im->layers = im->levels = im->samples = 1;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = use};
    VkResult r = vk.vkCreateImage(V.dev, &ci, NULL, &im->img);
    VkMemoryRequirements mr = {0}; if (!r) vk.vkGetImageMemoryRequirements(V.dev, im->img, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0)};
    if (!r && ai.memoryTypeIndex == UINT32_MAX) r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (!r) r = vk.vkAllocateMemory(V.dev, &ai, NULL, &im->m.dm);
    if (!r) r = vk.vkBindImageMemory(V.dev, im->img, im->m.dm, 0);
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (!r) r = vk.vkCreateImageView(V.dev, &vi, NULL, &o->v);
    if (r) { VRED("fg mv: image %ux%u VkFormat %d: %d", w, h, fmt, r); o->v = VK_NULL_HANDLE; vka_tex_drop(im); return 0; }
    o->im = im; return 1;
}
static void img_free(mimg *o) { if (o->v) vk.vkDestroyImageView(V.dev, o->v, NULL); if (o->im) vka_tex_drop(o->im); o->v = VK_NULL_HANDLE; o->im = NULL; }
static void to_general(VkCommandBuffer cb, VkImage img) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}
static const VkImageUsageFlags U_ALL = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
static int opset_new(VkCommandBuffer cb, opset *s, uint32_t w, uint32_t h, uint64_t serial) {
    if (!img_new(&s->z, w, h, VK_FORMAT_R32_SFLOAT, U_ALL, serial) || !img_new(&s->d, w, h, VK_FORMAT_R32_SFLOAT, U_ALL, serial + 1) ||
        !img_new(&s->v, w, h, VK_FORMAT_R16G16_SFLOAT, U_ALL, serial + 2) || !img_new(&s->e, w, h, VK_FORMAT_R8_UNORM, U_ALL, serial + 3) ||
        !img_new(&s->f, w, h, VK_FORMAT_R8_UINT, U_ALL, serial + 0x80)) {
        img_free(&s->z); img_free(&s->d); img_free(&s->v); img_free(&s->e); img_free(&s->f); return 0; }
    to_general(cb, s->z.im->img); to_general(cb, s->d.im->img); to_general(cb, s->v.im->img); to_general(cb, s->e.im->img); to_general(cb, s->f.im->img);
    s->w = s->cw = w; s->h = s->ch = h; return 1;
}
static void opset_free(opset *s) { img_free(&s->z); img_free(&s->d); img_free(&s->v); img_free(&s->e); img_free(&s->f); s->w = s->h = s->cw = s->ch = 0; }

static const VkDescriptorType DS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, DI = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, DB = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
static const VkDescriptorType T_OP[] = {DS, DS, DI, DI, DI, DI, DI}, T_SP[] = {DI, DI, DI, DI}, T_RE[] = {DS, DS, DI, DI, DI, DS, DS, DI, DI, DB}, T_AT[] = {DI},
                              T_F2[] = {DI, DI, DI, DI, DI}, T_Q4[] = {DI, DB, DI}, T_TI[] = {DB}, T_GR[] = {DB, DI}, T_RF[] = {DI, DI, DB}, T_SS[] = {DS, DS, DI, DI, DI, DI, DI, DI, DI},
                              T_R2[] = {DS, DS, DI, DI, DI, DS, DS, DI, DI, DB, DS, DI, DB, DI}, T_SPL[] = {DB, DI, DI, DI};
static const VkDescriptorType *const TY[NP] = {T_OP, T_SP, T_RE, T_AT, T_F2, T_Q4, T_TI, T_GR, T_RF, T_R2, T_SS, T_TI, T_OP, T_SPL};
static const uint32_t NB[NP] = {7, 4, 10, 1, 5, 3, 1, 2, 3, 14, 9, 1, 7, 4};
#define MAXNB 16   // the largest binding count of any pipeline (the stack arrays in pipes() and bind() are sized by it)
_Static_assert(sizeof T_OP / sizeof *T_OP == 7 && sizeof T_RE / sizeof *T_RE == 10 && sizeof T_SS / sizeof *T_SS == 9 && sizeof T_R2 / sizeof *T_R2 == 14 && sizeof T_R2 / sizeof *T_R2 <= MAXNB, "binding type lists and NB disagree");
typedef struct { int iw, ih; float thr, zmin; uint32_t flags; } pc_op;   // flags: 1 dither rule, 2 separable edge distance
typedef struct { int iw, ih; float t, mvx, mvy; uint32_t cshift; int coff, cmax; uint32_t ishift, imask; } pc_sp;   // the key's layout (key_lay)
typedef struct { int iw, ih, iters; uint32_t flags, imask; } pc_f2;   // flags: 1 pick map, 2 field
typedef struct { int W, H, iw, ih; float t, mvx, mvy, ratio, tol, feather, ew, eh; uint32_t flags; } pc_re;   // ew, eh: the op images' size when larger than iw x ih (else 0)   // flags 0x20000 static, 0x40000 bothfar, 0x80000 UI flow (resolve2)
typedef struct { int slot, W, H, store; } pc_q4;
typedef struct { int slot, sr; } pc_ti;
typedef struct { int phase, slot, gate, wcap, big; float t, mvx, mvy, kx, ky, sx, sy; int capn, lev; } pc_gr;   // capn: the tile cap (ui.glsl R_CAP); lev: ui.glsl L_*
typedef struct { int W, H, big, lev; } pc_rf;
typedef struct { int phase, gate, W, H; float mvx, mvy, kx, ky, sx, sy; int noa2, lev, x17; } pc_spl;   // noa2: no A2 layer (the rank-2 paste needs it: no split); lev: ui.glsl L_*;
                                                                                                  // x17: the split rule's options, the pass << 8 (usplit.comp)
typedef struct { int W, H, iw, ih, hw, hh; float t, mvx, mvy, ratio, kx, ky, sx, sy; uint32_t flags; } pc_sw;   // flags: 1 pick, 2 depth gate, 4 quarter, 8 debug, 16 pass 2, 32 screen-fixed overlay
_Static_assert(sizeof(pc_gr) == 56 && sizeof(pc_rf) == 16 && sizeof(pc_spl) == 52 && sizeof(pc_sw) <= 128 && sizeof(pc_re) <= 128,
               "the push constant blocks of fgmv/ugrp.comp, uref.comp, usplit.comp (and every block within the 128-byte minimum limit)");
static const uint32_t PCS[NP] = {sizeof(pc_op), sizeof(pc_sp), sizeof(pc_re), 4, sizeof(pc_f2), sizeof(pc_q4), sizeof(pc_ti), sizeof(pc_gr), sizeof(pc_rf), sizeof(pc_re), sizeof(pc_sw), sizeof(pc_ti), sizeof(pc_op), sizeof(pc_spl)};

static int pipes(void) {
    if (!V.push_desc || !vk.vkCmdPushDescriptorSetKHR) { VRED("fg mv: no push descriptors: the motion-vector engine is off"); return 0; }
    if (!V.null_desc) { VRED("fg mv: no null descriptors: the motion-vector engine is off"); return 0; }
    VkFormat need[6] = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R8_UNORM, VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R8_UINT};
    for (int i = 0; i < 6; i++) { VkFormatProperties fp; vk.vkGetPhysicalDeviceFormatProperties(V.pd, need[i], &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) || (i == 3 && !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT)) || (i == 2 && !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))) {
            VRED("fg mv: VkFormat %d lacks storage%s here: the motion-vector engine is off", need[i], i == 3 ? " atomics" : i == 2 ? " or linear filtering" : ""); return 0; } }
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxAnisotropy = 1};
    VkResult r = vk.vkCreateSampler(V.dev, &si, NULL, &g_near);
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    if (!r) r = vk.vkCreateSampler(V.dev, &si, NULL, &g_lin);
    const uint32_t *code[NP] = {vka_fgmv_op_spv, vka_fgmv_splat_spv, vka_fgmv_resolve_spv, vka_fgmv_atom_spv, vka_fgmv_fill2_spv, vka_fgmv_uq4_spv, vka_fgmv_utile_spv,
                                vka_fgmv_ugrp_spv, vka_fgmv_uref_spv, vka_fgmv_resolve2_spv, vka_fgmv_stws16_spv, vka_fgmv_ucls_spv, vka_fgmv_opt_spv, vka_fgmv_usplit_spv};
    size_t size[NP] = {sizeof vka_fgmv_op_spv, sizeof vka_fgmv_splat_spv, sizeof vka_fgmv_resolve_spv, sizeof vka_fgmv_atom_spv, sizeof vka_fgmv_fill2_spv,
                       sizeof vka_fgmv_uq4_spv, sizeof vka_fgmv_utile_spv, sizeof vka_fgmv_ugrp_spv, sizeof vka_fgmv_uref_spv, sizeof vka_fgmv_resolve2_spv,
                       sizeof vka_fgmv_stws16_spv, sizeof vka_fgmv_ucls_spv, sizeof vka_fgmv_opt_spv, sizeof vka_fgmv_usplit_spv};
    for (int k = 0; k < NP; k++) if (NB[k] > MAXNB) { VRED("fg mv: pipeline %d has %u bindings, over MAXNB %d: the motion-vector engine is off", k, NB[k], MAXNB); return 0; }
    for (int k = 0; k < NP && !r; k++) {
        VkDescriptorSetLayoutBinding b[MAXNB];
        for (uint32_t i = 0; i < NB[k]; i++) b[i] = (VkDescriptorSetLayoutBinding){i, TY[k][i], 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, .bindingCount = NB[k], .pBindings = b};
        VkPushConstantRange pc = {VK_SHADER_STAGE_COMPUTE_BIT, 0, PCS[k]};
        r = vk.vkCreateDescriptorSetLayout(V.dev, &dl, NULL, &Pp[k].dsl);
        VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &Pp[k].dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pc};
        if (!r) r = vk.vkCreatePipelineLayout(V.dev, &li, NULL, &Pp[k].lay);
        VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size[k], .pCode = code[k]}; VkShaderModule m = VK_NULL_HANDLE;
        if (!r) r = vk.vkCreateShaderModule(V.dev, &mi, NULL, &m);
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"}, .layout = Pp[k].lay};
        if (!r) r = vk.vkCreateComputePipelines(V.dev, VK_NULL_HANDLE, 1, &ci, NULL, &Pp[k].p);
        if (m) vk.vkDestroyShaderModule(V.dev, m, NULL);
    }
    if (r) { VRED("fg mv: pipelines %d: the motion-vector engine is off", r); return 0; }
    return 1;
}
// A descriptor: image view (sampler for combined ones) or buffer
typedef struct { VkImageView v; VkSampler s; VkBuffer b; VkDeviceSize off, range; } dsc;
static void bind(VkCommandBuffer cb, int k, const dsc *d, const void *pc) {
    VkWriteDescriptorSet wr[MAXNB]; VkDescriptorImageInfo ii[MAXNB]; VkDescriptorBufferInfo bi[MAXNB];
    for (uint32_t i = 0; i < NB[k]; i++) {
        wr[i] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = i, .descriptorCount = 1, .descriptorType = TY[k][i]};
        if (TY[k][i] == DB) { bi[i] = (VkDescriptorBufferInfo){d[i].b, d[i].off, d[i].range ? d[i].range : VK_WHOLE_SIZE}; wr[i].pBufferInfo = &bi[i]; }
        else { ii[i] = (VkDescriptorImageInfo){TY[k][i] == DS ? d[i].s : VK_NULL_HANDLE, d[i].v, VK_IMAGE_LAYOUT_GENERAL}; wr[i].pImageInfo = &ii[i]; }
    }
    vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, Pp[k].p);
    vk.vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_COMPUTE, Pp[k].lay, 0, NB[k], wr);
    vk.vkCmdPushConstants(cb, Pp[k].lay, VK_SHADER_STAGE_COMPUTE_BIT, 0, PCS[k], pc);
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
    if (r) { VRED("fg mv: buffer of %llu bytes: %d", (unsigned long long)size, r); if (*b) vk.vkDestroyBuffer(V.dev, *b, NULL); if (*m) vk.vkFreeMemory(V.dev, *m, NULL); *b = VK_NULL_HANDLE; *m = VK_NULL_HANDLE; return 0; }
    return 1;
}
static void buf_free(VkBuffer *b, VkDeviceMemory *m) { if (*b) vk.vkDestroyBuffer(V.dev, *b, NULL); if (*m) vk.vkFreeMemory(V.dev, *m, NULL); *b = VK_NULL_HANDLE; *m = VK_NULL_HANDLE; }

// A one-off submission on the queue with its own pool, waited for (the self-test, the replay)
typedef struct { VkCommandPool pool; VkCommandBuffer cb; VkFence fe; } once;
static int once_begin(once *o) {
    VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = V.qf};
    VkResult r = o->pool ? VK_SUCCESS : vk.vkCreateCommandPool(V.dev, &pi, NULL, &o->pool);
    VkCommandBufferAllocateInfo ca = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = o->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    if (!r && !o->cb) r = vk.vkAllocateCommandBuffers(V.dev, &ca, &o->cb);
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (!r && !o->fe) r = vk.vkCreateFence(V.dev, &fci, NULL, &o->fe);
    if (r) return 0;
    vk.vkResetCommandBuffer(o->cb, 0);
    VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vk.vkBeginCommandBuffer(o->cb, &cbi); vka_barrier(o->cb);
    return 1;
}
static int once_end(once *o) {
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vk.vkCmdPipelineBarrier(o->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    vk.vkEndCommandBuffer(o->cb);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &o->cb};
    pthread_mutex_lock(&V.qmu); VkResult r = vk.vkQueueSubmit(V.q, 1, &si, o->fe); pthread_mutex_unlock(&V.qmu);
    if (!r) r = vk.vkWaitForFences(V.dev, 1, &o->fe, VK_TRUE, 5000000000ull);
    vk.vkResetFences(V.dev, 1, &o->fe);
    return r == VK_SUCCESS;
}
static void once_free(once *o) {
    if (o->fe) vk.vkDestroyFence(V.dev, o->fe, NULL);
    if (o->pool) vk.vkDestroyCommandPool(V.dev, o->pool, NULL);
    memset(o, 0, sizeof *o);
}

// ---------- The self-test of 32-bit image atomics (fgmv/atom.comp) ----------
static int q_ready(void);
static uint32_t h1(uint32_t n) { n ^= n >> 16; n *= 0x7feb352du; n ^= n >> 15; n *= 0x846ca68bu; n ^= n >> 16; return n; }
static int atom_test(void) {
    mimg im = {0}; VkBuffer b = VK_NULL_HANDLE; VkDeviceMemory m = VK_NULL_HANDLE; uint32_t *map = NULL; once o = {0}; int ok = 0, bad = 0;
    uint64_t t0 = now_ns();
    if (img_new(&im, 64, 64, VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0xF6500000ull) &&
        buf_new(64 * 64 * 4, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &b, &m, (void **)&map) && once_begin(&o)) {
        to_general(o.cb, im.im->img);
        VkClearColorValue zero = {{0}}; VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vk.vkCmdClearColorImage(o.cb, im.im->img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rg); vka_barrier(o.cb);
        dsc d[1] = {{im.v}}; uint32_t pc = 0;
        bind(o.cb, PA, d, &pc); vk.vkCmdDispatch(o.cb, 128, 1, 1); vka_barrier(o.cb);
        VkBufferImageCopy c = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {64, 64, 1}};
        vk.vkCmdCopyImageToBuffer(o.cb, im.im->img, VK_IMAGE_LAYOUT_GENERAL, b, 1, &c);
        if (once_end(&o)) {
            static uint32_t want[4096]; memset(want, 0, sizeof want);
            for (uint32_t n = 0; n < 32768; n++) { uint32_t t = h1(n) & 4095u, v = h1(n ^ 0x9e3779b9u) | 1u; if (v > want[t]) want[t] = v; }
            for (int i = 0; i < 4096; i++) if (map[i] != want[i]) bad++;
            ok = !bad;
        }
    }
    vka_queue_idle(); once_free(&o); buf_free(&b, &m); img_free(&im);
    if (ok) VLOG("fg mv: atomic self-test passed (32768 image atomic max into 4096 texels, %.1f ms)", (now_ns() - t0) / 1e6);
    else VRED("fg mv: atomic self-test FAILED (%d of 4096 texels wrong): the frame generation stays with LSFG (or the game frames only without Lossless.dll)", bad);
    return ok;
}
static int ready(void) {   // the present thread (the self-test waits on the queue)
    if (g_state) return g_state > 0;
    g_state = -1;
    if (!pipes() || !atom_test()) return 0;
    q_ready();
    g_state = 1; return 1;
}
// DM_FG_ENGINE=mv and the engine works (the present thread: its first call runs the self-test)
int vka_fgmv_engine(void) { keys(); return g_want && ready(); }

// ---------- The op side: one pass at each DLSS op (the recording thread) ----------
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { opset s; uint64_t frame, n; float sc[2], jit[2]; uint32_t flags, reset; int ok; } O[NO];   // (flags, jitter, reset: the op record's, kept with its data)
// Op images outgrown by a larger render size: freed by the present thread once no frame can read them (op_reap)
static struct { opset s; uint64_t n; } Rt[2 * NO]; static int g_nrt;
// The last op record's outcome (the status reason of "no motion data"): 0 its pass ran, else OPW_*
enum { OPW_NOTEX = 1, OPW_ATLAS, OPW_FLAGS, OPW_FORMAT, OPW_SIZE, OPW_IMAGES };
static _Atomic int g_opw; static _Atomic uint32_t g_opwf; static char g_opwt[300];   // (g_opwt under g_mu)
static uint64_t g_on;   // op passes begun
static struct { PFN_vkCmdWriteTimestamp wr; PFN_vkCmdResetQueryPool rs; PFN_vkGetQueryPoolResults get; PFN_vkCreateQueryPool create; int st; double tick; } Q;
static VkQueryPool g_oqp; static double g_osum, g_ot0; static uint32_t g_on_t; static int g_ots[NO];
static int q_ready(void) {
    if (Q.st) return Q.st > 0;
    Q.st = -1;
    Q.wr = (PFN_vkCmdWriteTimestamp)vk.vkGetDeviceProcAddr(V.dev, "vkCmdWriteTimestamp"); Q.rs = (PFN_vkCmdResetQueryPool)vk.vkGetDeviceProcAddr(V.dev, "vkCmdResetQueryPool");
    Q.get = (PFN_vkGetQueryPoolResults)vk.vkGetDeviceProcAddr(V.dev, "vkGetQueryPoolResults"); Q.create = (PFN_vkCreateQueryPool)vk.vkGetDeviceProcAddr(V.dev, "vkCreateQueryPool");
    if (!Q.wr || !Q.rs || !Q.get || !Q.create || V.props.limits.timestampPeriod <= 0) return 0;
    Q.tick = V.props.limits.timestampPeriod; Q.st = 1; return 1;
}
static void rec_op(VkCommandBuffer cb, VkImageView zv, VkImageView mvv, opset *s, int iw, int ih) {
    dsc d[7] = {{zv, g_near}, {mvv, g_near}, {s->z.v}, {s->d.v}, {s->v.v}, {s->e.v}, {s->f.v}};
    pc_op pc = {iw, ih, g_thr, 1e-4f, (g_dither ? 1u : 0u) | 2u};   // (POT: with the dither rule; else the plain op)
    bind(cb, g_dither ? POT : PO, d, &pc); vk.vkCmdDispatch(cb, (uint32_t)(iw + 15) / 16, (uint32_t)(ih + 15) / 16, 1);
}
static void op_refuse(int w, uint32_t flags, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void op_refuse(int w, uint32_t flags, const char *fmt, ...) {   // one log line per kind of refusal
    static _Atomic int told[8]; atomic_store(&g_opw, w); atomic_store(&g_opwf, flags);
    char b[300]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    pthread_mutex_lock(&g_mu); memcpy(g_opwt, b, sizeof b); pthread_mutex_unlock(&g_mu);
    if (w > 0 && w < 8 && !atomic_exchange(&told[w], 1)) VRED("fg mv: %s: no motion data", b);
}
// The DLSS op (vka_exec.c dlss_eval, any mode): the whole op record; the pass runs on the conventions it was validated on (depth inverted, low-res unjittered vectors)
void vka_fgmv_op(VkCommandBuffer cb, const p_dlss *o) {
    keys(); if (!g_want || g_state <= 0 || !cb || !o) return;
    if (!o->tex[DLSS_DEPTH] || !o->tex[DLSS_MOTION]) { op_refuse(OPW_NOTEX, o->flags, "the DLSS op has no depth or no motion vector texture"); return; }
    if (o->base[DLSS_DEPTH][0] || o->base[DLSS_DEPTH][1] || o->base[DLSS_MOTION][0] || o->base[DLSS_MOTION][1]) {
        op_refuse(OPW_ATLAS, o->flags, "the DLSS op's depth / motion vectors start at an offset (%d,%d / %d,%d: an atlas)", o->base[DLSS_DEPTH][0], o->base[DLSS_DEPTH][1], o->base[DLSS_MOTION][0], o->base[DLSS_MOTION][1]); return; }
    if ((o->flags & 0xEu) != 0xAu) {
        op_refuse(OPW_FLAGS, o->flags, "the DLSS op's flags %#x are a kind the engine was never validated on (it needs depth inverted 8 and low-res motion vectors 2, not jittered 4)", o->flags); return; }
    vka_img *z = vka_tex_get(o->tex[DLSS_DEPTH], cb);
    vka_img *m = vka_tex_get(o->tex[DLSS_MOTION], cb);
    int iw = o->iw, ih = o->ih; const float *scale = o->mv;
    if (!z || !m || !(z->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) || !(z->usage & VK_IMAGE_USAGE_SAMPLED_BIT) || !(m->usage & VK_IMAGE_USAGE_SAMPLED_BIT) || m->f.vk != VK_FORMAT_R16G16_SFLOAT) {
        op_refuse(OPW_FORMAT, o->flags, "the DLSS op's depth / motion vectors cannot be read (depth %d, motion VkFormat %d)", z ? (int)z->f.vk : -1, m ? (int)m->f.vk : -1); return; }
    uint32_t w = iw > 0 && (uint32_t)iw <= m->w && (uint32_t)iw <= z->w ? (uint32_t)iw : (m->w < z->w ? m->w : z->w), h = ih > 0 && (uint32_t)ih <= m->h && (uint32_t)ih <= z->h ? (uint32_t)ih : (m->h < z->h ? m->h : z->h);
    if (w * h > 1u << 23) { op_refuse(OPW_SIZE, o->flags, "render size %ux%u is over 2^%d texels (the splat key's index)", w, h, 23); return; }
    VkImageView zv = vka_view(z, z->f.vk, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1, NULL), mvv = vka_view(m, m->f.vk, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1, NULL);
    if (!zv || !mvv) return;
    pthread_mutex_lock(&g_mu); unsigned k = (unsigned)(g_on % NO); O[k].ok = 0; pthread_mutex_unlock(&g_mu);
    static uint32_t capw, caph;   // the largest render size seen: every slot's images grow to it and are reused below it (the w x h corner)
    if (w > capw) capw = w;
    if (h > caph) caph = h;
    if (O[k].s.cw < w || O[k].s.ch < h) {   // larger than the slot's images: new ones; the old ones are freed once no frame can read them (op_reap)
        static int told; if (O[k].s.cw && told++ < 4) VLOG("fg mv: render size %ux%u over the op images' %ux%u: new ones at %ux%u", w, h, O[k].s.cw, O[k].s.ch, capw, caph);
        if (O[k].s.cw) {
            pthread_mutex_lock(&g_mu);
            if (g_nrt < 2 * NO) { Rt[g_nrt].s = O[k].s; Rt[g_nrt].n = g_on; g_nrt++; }
            else VRED_ONCE("fg mv: too many outgrown op images waiting: one set is left alive");
            pthread_mutex_unlock(&g_mu);
        }
        memset(&O[k].s, 0, sizeof O[k].s);
        if (!opset_new(cb, &O[k].s, capw, caph, 0xF6600000ull + k * 4)) { op_refuse(OPW_IMAGES, o->flags, "op images of %ux%u could not be made", capw, caph); return; }
    }
    O[k].s.w = w; O[k].s.h = h;
    int ts = VKA_ENVF("DM_VK_FG_GPUTIME", 0) > 0 && q_ready();
    if (ts && !g_oqp) { VkQueryPoolCreateInfo ci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = NO * 2}; if (Q.create(V.dev, &ci, NULL, &g_oqp)) ts = 0; }
    if (g_ots[k]) {   // the slot's last timing (NO frames ago: done, or skipped)
        uint64_t v[2]; g_ots[k] = 0;
        if (!Q.get(V.dev, g_oqp, k * 2, 2, sizeof v, v, 8, VK_QUERY_RESULT_64_BIT)) { g_osum += (double)(v[1] - v[0]) * Q.tick / 1e6; g_on_t++; }
        double now = (double)now_ns() / 1e9; if (!g_ot0) g_ot0 = now;
        if (now - g_ot0 >= VKA_ENVF("DM_VK_FG_GPUTIME", 10) && g_on_t) { VLOG("fg mv op gpu: %u ops, mean %.3f ms (depth, dilation, edge distance, dither region at %ux%u; dither %d; %s)", g_on_t, g_osum / g_on_t, w, h, g_dither, g_opsync ? "synced: valid" : "not synced: not valid"); g_osum = 0; g_on_t = 0; g_ot0 = now; }
    }
    vka_barrier(cb);
    // (the stamps are direct calls, the barrier and the pass go through the encoder thread's ring, vka_enc.c: without DM_FG_MV_OPSYNC both stamps land before them)
    if (ts && g_opsync) vka_enc_sync();
    if (ts) { Q.rs(cb, g_oqp, k * 2, 2); Q.wr(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_oqp, k * 2); }   // (after the barrier: the game's work before it is done)
    rec_op(cb, zv, mvv, &O[k].s, (int)w, (int)h);
    vka_barrier(cb);   // (the end stamp after the pass is done)
    if (ts && g_opsync) vka_enc_sync();
    if (ts) { Q.wr(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_oqp, k * 2 + 1); g_ots[k] = 1; }
    pthread_mutex_lock(&g_mu);
    O[k].frame = vka_frame(); O[k].n = g_on; O[k].sc[0] = scale && scale[0] ? scale[0] : 1; O[k].sc[1] = scale && scale[1] ? scale[1] : 1; O[k].ok = 1; g_on++;
    O[k].jit[0] = o->jitter[0]; O[k].jit[1] = o->jitter[1]; O[k].flags = o->flags; O[k].reset = o->reset;
    pthread_mutex_unlock(&g_mu);
    atomic_store(&g_opw, 0);
    static int told; if (!told++) VLOG("fg mv: first op pass: render %ux%u, depth VkFormat %d, motion scale %.1f %.1f, flags %#x, jitter %.3f %.3f, reset %u", w, h, z->f.vk, O[k].sc[0], O[k].sc[1],
                                      o->flags, o->jitter[0], o->jitter[1], o->reset);
}
// vka_fgui.c win_map (the recording thread): this frame's op slot (ring number + 1), 0 = none
uint64_t vka_fgmv_op_q(uint64_t frame) {
    if (!g_want) return 0;
    uint64_t q = 0;
    pthread_mutex_lock(&g_mu);
    for (uint64_t i = g_on; i > 0 && i + NO > g_on + 1; i--) { unsigned k = (unsigned)((i - 1) % NO); if (O[k].n == i - 1 && O[k].ok && O[k].frame == frame) { q = i; break; } }
    pthread_mutex_unlock(&g_mu);
    return q;
}
static void op_reap(void) {   // the present thread: outgrown op images that no frame can read any more (2 rings of op passes later), after the queue is idle
    opset f[2 * NO]; int n = 0;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_nrt;) { if (g_on > Rt[i].n + 2 * NO) { f[n++] = Rt[i].s; Rt[i] = Rt[--g_nrt]; } else i++; }
    pthread_mutex_unlock(&g_mu);
    if (!n) return;
    vka_queue_idle();
    for (int i = 0; i < n; i++) opset_free(&f[i]);
    VLOG("fg mv: %d outgrown op image set(s) freed", n);
}
static int op_get(uint64_t q, opset **s, float *sc) {   // the present thread: a slot not yet given to a newer frame (not the two oldest: the recording thread runs ahead)
    int ok = 0;
    pthread_mutex_lock(&g_mu);
    if (q && q + NO > g_on + 2) { unsigned k = (unsigned)((q - 1) % NO); if (O[k].n == q - 1 && O[k].ok) { *s = &O[k].s; sc[0] = O[k].sc[0]; sc[1] = O[k].sc[1]; ok = 1; } }
    pthread_mutex_unlock(&g_mu);
    return ok;
}

// ---------- The present side ----------
static struct {
    uint32_t w, h, rw, rh;
    mimg in[2], key, field, pick; vka_img *out[NG]; VkImageView outv[NG];
    VkBuffer zb; VkDeviceMemory zm;   // a zero "no cut" result
    VkQueryPool qp; int ts[NKS]; double tsum[NQ - 1], tt0; uint32_t tn;
    uint64_t gen, rep_nodata, rep_nopre, rep_force, rep_size, nall;
    // the UI flow's buffer and the two frames' UI layers (A's is C's of the frame before), the static weight (half, quarter size; s1: the two-pass form's
    // first pass, half size, the quarter in its corner), the counters' host copies
    mimg uil[3], swh, swq, s1; VkBuffer ub, hb; VkDeviceMemory um, hm; uint32_t *hmap; uint64_t uq[3]; int uis[NKS], ukl[3];   // per game frame count % 3 (A2: count - 2);
                                                                     // uq: the frame's layer is readable for the pair count + 1 (+ 1 as A2); ukl: copied into uil
    // a game frame's input read in place from its pre-UI snapshot (vka_fgui.c ring; image, sampled view, its UI layer, ring number), per count % 3
    struct { VkImage im; VkImageView v, lv; uint64_t q, n; } dr[3];
    uint64_t un, umv, usum, umax, ugr, uover, uveto, ucap, unb, uvg, uvc, urp, usm, upo, upu, upt, usx, ust, usk, uss, usg, us2, uso, usn, ux[UI_NX17]; double ut0;
    VkQueryPool uqp; int uts[NKS]; double utsum[NUQ - 1], utt0; uint32_t utn;   // the UI flow's timestamps (DM_VK_FG_GPUTIME_SPLIT=1)
} M;
static vka_img *out_img(uint32_t w, uint32_t h, int i, VkImageView *view) {
    mimg o = {0};
    if (!img_new(&o, w, h, VK_FORMAT_R8G8B8A8_UNORM, U_ALL, 0xF6700000ull + (uint64_t)i)) return NULL;
    *view = o.v; return o.im;
}
void vka_fgmv_teardown(void) {
    if (!M.w && !M.rw) return;
    vka_queue_idle();
    for (int i = 0; i < 2; i++) img_free(&M.in[i]);
    img_free(&M.key); img_free(&M.field); img_free(&M.pick);
    for (int i = 0; i < NG; i++) { if (M.outv[i]) vk.vkDestroyImageView(V.dev, M.outv[i], NULL); if (M.out[i]) vka_tex_drop(M.out[i]); M.out[i] = NULL; M.outv[i] = VK_NULL_HANDLE; }
    for (int i = 0; i < 3; i++) { img_free(&M.uil[i]); M.uq[i] = 0; M.ukl[i] = 0; }
    memset(M.dr, 0, sizeof M.dr);
    img_free(&M.swh); img_free(&M.swq); img_free(&M.s1);
    M.w = M.h = M.rw = M.rh = 0;
    vka_fgui_post_reset();
    VLOG("fg mv: present-side images freed");
}
// The inputs and outputs at display size w x h (the present thread); 0 on failure
int vka_fgmv_setup(uint32_t w, uint32_t h) {
    if (M.w == w && M.h == h) return 1;
    vka_fgmv_teardown();
    for (int i = 0; i < 2; i++) if (!img_new(&M.in[i], w, h, VK_FORMAT_R8G8B8A8_UNORM, U_ALL, 0xF6800000ull + (uint64_t)i)) goto fail;
    for (int i = 0; i < NG; i++) if (!(M.out[i] = out_img(w, h, i, &M.outv[i]))) goto fail;
    if (!M.zb && !buf_new(256, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, &M.zb, &M.zm, NULL)) goto fail;
    for (int i = 0; i < 3; i++) if (!img_new(&M.uil[i], w, h, VK_FORMAT_R8G8B8A8_UNORM, U_ALL, 0xF6800010ull + (uint64_t)i)) goto fail;
    if (!img_new(&M.swh, (w + 1) / 2, (h + 1) / 2, VK_FORMAT_R8_UNORM, U_ALL, 0xF6800020ull) || !img_new(&M.swq, (w + 3) / 4, (h + 3) / 4, VK_FORMAT_R8_UNORM, U_ALL, 0xF6800021ull) ||
        !img_new(&M.s1, (w + 1) / 2, (h + 1) / 2, VK_FORMAT_R32_SFLOAT, U_ALL, 0xF6800022ull)) goto fail;
    if (!M.ub && !buf_new((VkDeviceSize)UI_O_END * 4, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                           &M.ub, &M.um, NULL)) goto fail;
    if (!M.hb && !buf_new((VkDeviceSize)NKS * (UI_NCNT + UI_NX17) * 4, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &M.hb, &M.hm, (void **)&M.hmap)) goto fail;
    M.w = w; M.h = h;
    { once o = {0}; if (once_begin(&o)) {   // layouts and the zero result
          for (int i = 0; i < 2; i++) to_general(o.cb, M.in[i].im->img);
          for (int i = 0; i < 3; i++) to_general(o.cb, M.uil[i].im->img);
          for (int i = 0; i < NG; i++) to_general(o.cb, M.out[i]->img);
          to_general(o.cb, M.swh.im->img); to_general(o.cb, M.swq.im->img); to_general(o.cb, M.s1.im->img);
          vk.vkCmdFillBuffer(o.cb, M.zb, 0, 256, 0); vk.vkCmdFillBuffer(o.cb, M.ub, 0, VK_WHOLE_SIZE, 0);
          once_end(&o); } once_free(&o); }
    VLOG("fg mv: present side ready at %ux%u", w, h);
    return 1;
fail:
    M.w = w; M.h = h; vka_fgmv_teardown(); return 0;
}
VkImage vka_fgmv_input(uint64_t count) { return M.in[count % 2].im ? M.in[count % 2].im->img : VK_NULL_HANDLE; }
// vka_fgui.c: the ring number of the pre-UI snapshot in image pre while it and its UI layer stay readable, margin slots before the recording thread rewrites it (0: gone)
uint64_t vka_fgui_snap_q(VkImage pre, unsigned margin);
// The present thread, before the input copy (vka_framegen.c): the engine reads this frame's pre-UI snapshot in place, and the next
// pair reads its UI layer there as A's (returned: no copy needed). NULL: copy into vka_fgmv_input(count)
VkImage vka_fgmv_direct(uint64_t count, const vka_fgui_frame *u, int use_pre, uint32_t cw, uint32_t ch) {
    keys(); unsigned i = (unsigned)(count % 3); memset(&M.dr[i], 0, sizeof M.dr[i]);
    if (!use_pre || !u || !u->has_ui || !u->pre || !u->pre_v || !u->layer_v || !M.w || u->w != M.w || u->h != M.h || cw != M.w || ch != M.h) return VK_NULL_HANDLE;
    uint64_t q = vka_fgui_snap_q(u->pre, 1); if (!q) return VK_NULL_HANDLE;
    M.dr[i].im = u->pre; M.dr[i].v = u->pre_v; M.dr[i].lv = u->layer_v; M.dr[i].q = q; M.dr[i].n = count; return u->pre;
}
static int in_direct(uint64_t count) { unsigned i = (unsigned)(count % 3); return M.dr[i].im && M.dr[i].n == count; }   // count: the game frame's own (A: count - 1)
// A game frame's input and its view: its snapshot read in place (NULL: the ring has moved on, margin as above) or the copy
static VkImage in_of(uint64_t count, unsigned margin, VkImageView *v) {
    unsigned i = (unsigned)(count % 3);
    if (in_direct(count)) { int ok = vka_fgui_snap_q(M.dr[i].im, margin) == M.dr[i].q; *v = ok ? M.dr[i].v : VK_NULL_HANDLE; return ok ? M.dr[i].im : VK_NULL_HANDLE; }
    VkImage im = vka_fgmv_input(count); *v = vka_fgui_view(im); return im;
}
vka_img *vka_fgmv_out(int g) { return g >= 0 && g < NG ? M.out[g] : NULL; }
VkImageView *vka_fgmv_out_views(void) { return M.outv; }

static int rsize(VkCommandBuffer cb, uint32_t rw, uint32_t rh) {   // the splat key and the field at render size: at the largest seen, reused below it (the rw x rh corner)
    if (M.rw >= rw && M.rh >= rh) return 1;
    if (rw < M.rw) rw = M.rw;
    if (rh < M.rh) rh = M.rh;
    if (M.rw) { vka_queue_idle(); img_free(&M.key); img_free(&M.field); img_free(&M.pick); }
    M.rw = M.rh = 0;
    if (!img_new(&M.key, rw, rh, VK_FORMAT_R32_UINT, U_ALL, 0xF6900000ull) || !img_new(&M.field, rw, rh, VK_FORMAT_R32G32_UINT, U_ALL, 0xF6900001ull) ||
        !img_new(&M.pick, rw, rh, VK_FORMAT_R32G32_UINT, U_ALL, 0xF6900002ull)) { img_free(&M.key); img_free(&M.field); img_free(&M.pick); return 0; }
    to_general(cb, M.key.im->img); to_general(cb, M.field.im->img); to_general(cb, M.pick.im->img);
    M.rw = rw; M.rh = rh; return 1;
}
// The inputs of a generated frame for the extended rules (NULL: none of the UI flow, static candidate and both-occluded rules, the base passes only): A's UI layer (0: no UI flow), C's slot of the UI flow's buffer, the first generated frame of the pair
// (the UI flow's per-pair stages run there), whether C's coverage pyramid is made for the next pair (store), the counters' copy (hslot >= 0) and where C's layer is
// copied for the next pair (kv; 0: none, the next pair reads it in its snapshot)
typedef struct { VkImageView la; int slot, first, store, hslot; VkImageView kv, a2; } gx;   // a2: the A2 layer (the split rule's rank-2 paste; NULL: none)
// The UI flow's tile grid (fgmv/ui.glsl): 60 x 34 tiles of 32 display px, so displays up to 1920x1088; its coverage pass runs a workgroup per 64x64 px
enum { UI_TX = 60, UI_TY = 34, UI_MAXW = UI_TX * 32, UI_MAXH = UI_TY * 32 };
// Render px per display px and back, as floats of the two sizes (a float quotient of two whole numbers is the correctly rounded one: the same as the double's, rounded once)
static void ui_k(const opset *C, uint32_t w, uint32_t h, float *k) { k[0] = (float)C->w / (float)w; k[1] = (float)C->h / (float)h; k[2] = (float)w / (float)C->w; k[3] = (float)h / (float)C->h; }
typedef struct { const opset *A, *C; const float *sc; VkImageView aview, cview; uint32_t w, h; float t; int pick, fieldf, stb, dbg; const gx *x; } rgs;
static void gen_pass(VkCommandBuffer cb, int i, const rgs *r);
#define ui_bar(cb) vka_barrier(cb)
static void ui_q4(VkCommandBuffer cb, VkImageView lv, int slot, uint32_t w, uint32_t h, VkImageView keep) {   // a layer's coverage pyramid into slot (and a copy into keep)
    dsc d[3] = {{lv}, {.b = M.ub}, {keep ? keep : lv}}; pc_q4 pq = {slot, (int)w, (int)h, keep ? 1 : 0};   // (no copy: binding 2 is not written)
    bind(cb, PQ4, d, &pq); vk.vkCmdDispatch(cb, UI_TX / 2, (UI_TY + 1) / 2, 1); ui_bar(cb);
}
// C's coverage pyramid into slot, and its UI layer copied into kv as the next pair's A (one pass)
static void ui_store(VkCommandBuffer cb, VkImageView lv, int slot, uint32_t w, uint32_t h, VkImageView kv) { ui_q4(cb, lv, slot, w, h, kv); }
// The UI flow's timestamps: the slot stamped by this pair (-1 none; each stamp after a barrier). 0 before the coverage, 1 coverage, 2 tiles, 3 labels and work items,
// 4 candidate SADs, 5 pick (and the MV-consistent re-pick), 6 refine, 7 final accept and gate, 8 the big pass, 9 the paste lists
static int g_uk = -1;
static void ui_ts(VkCommandBuffer cb, int i) { if (g_uk >= 0) Q.wr(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, M.uqp, (uint32_t)g_uk * NUQ + (uint32_t)i); }
// The UI flow's per-pair stages (tiles, groups, refine; ugrp.comp's passes) on C's vectors. The refine and the big pass (groups of more than 128 tiles) are dispatched
// indirectly from the arguments the passes before write (ui.glsl C_IND: no workgroups when nothing is accepted / no big pass is needed); phase 5 runs inside phase 1
static void ui_pair(VkCommandBuffer cb, const opset *C, const float *sc, VkImageView la, VkImageView lc, int slot, uint32_t w, uint32_t h, int noa2) {
    float kk[4]; ui_k(C, w, h, kk);
    vk.vkCmdFillBuffer(cb, M.ub, (VkDeviceSize)UI_O_CNT * 4, 64 * 4, 0); ui_bar(cb);
    VkDeviceSize ia = (VkDeviceSize)(UI_O_CNT + UI_C_IND) * 4;   // + 4 * (0 refine, 5 big refine, 10 big one-workgroup phases, 13 big phase 3, 16 phase 3, 19 tile search,
                                                                   // 22 / 25 the split's, 28 phase 7; all inside the 64 counter words cleared here)
    _Static_assert(UI_C_IND + 28 + 3 <= 64, "the indirect arguments are cleared with the counters");
    dsc dt[1] = {{.b = M.ub}}; pc_ti pt = {slot, g_uisr};
    // the classify pass (4 tiles a workgroup), then the search on the tiles it listed (indirect)
    bind(cb, PUC, dt, &pt); vk.vkCmdDispatch(cb, UI_TX / 4, UI_TY, 1); ui_bar(cb);
    pt.sr = g_uisr | 256 | 512; bind(cb, PTI, dt, &pt); vk.vkCmdDispatchIndirect(cb, M.ub, ia + 19 * 4);
    ui_bar(cb); ui_ts(cb, 2);
    dsc dg[2] = {{.b = M.ub}, {C->v.v}}, dr[3] = {{la}, {lc}, {.b = M.ub}};
    pc_gr pg = {0, slot, g_uirules, 1 << 20, 0, 0.5f, sc[0], sc[1], kk[0], kk[1], kk[2], kk[3], g_uicapn, LEV};
    pc_rf pr = {(int)w, (int)h, 0, LEV};
    bind(cb, PGR, dg, &pg); vk.vkCmdDispatch(cb, 1, 1, 1); ui_bar(cb); ui_ts(cb, 3);
    pg.phase = 3; bind(cb, PGR, dg, &pg); vk.vkCmdDispatchIndirect(cb, M.ub, ia + 16 * 4); ui_bar(cb); ui_ts(cb, 4);
    pg.phase = 4; bind(cb, PGR, dg, &pg); vk.vkCmdDispatch(cb, 1, 1, 1); ui_bar(cb);
    if (g_uirules & 2) {
        pg.phase = 6; bind(cb, PGR, dg, &pg); vk.vkCmdDispatch(cb, 1, 1, 1); ui_bar(cb);
        // phase 6 lists the groups to re-pick; phase 7 a workgroup each, indirect; phase 8 the accept
        pg.phase = 7; bind(cb, PGR, dg, &pg); vk.vkCmdDispatchIndirect(cb, M.ub, ia + 28 * 4); ui_bar(cb);
        pg.phase = 8; bind(cb, PGR, dg, &pg); vk.vkCmdDispatch(cb, 1, 1, 1); ui_bar(cb);
    }
    ui_ts(cb, 5);
    bind(cb, PRF, dr, &pr); vk.vkCmdDispatchIndirect(cb, M.ub, ia); ui_bar(cb); ui_ts(cb, 6);
    pg.phase = 1; bind(cb, PGR, dg, &pg); vk.vkCmdDispatch(cb, 1, 1, 1); ui_bar(cb); ui_ts(cb, 7);
    // the big pass (groups of more than 128 tiles). With the tile cap rule its indirect arguments are always 0 (ugrp.comp phase 1 writes + 10 as 0 when C_NEEDBIG is 0,
    // which only a pass without the rule sets; + 13 and + 5 are written only inside the big pass; all three cleared above): not recorded then
    if (!(g_uirules & 16)) {
    pg.big = 1; pr.big = 1;
    pg.phase = 0; bind(cb, PGR, dg, &pg); vk.vkCmdDispatchIndirect(cb, M.ub, ia + 10 * 4); ui_bar(cb);
    pg.phase = 3; bind(cb, PGR, dg, &pg); vk.vkCmdDispatchIndirect(cb, M.ub, ia + 13 * 4); ui_bar(cb);
    pg.phase = 4; bind(cb, PGR, dg, &pg); vk.vkCmdDispatchIndirect(cb, M.ub, ia + 10 * 4); ui_bar(cb);
    bind(cb, PRF, dr, &pr); vk.vkCmdDispatchIndirect(cb, M.ub, ia + 5 * 4); ui_bar(cb);
    pg.phase = 1; bind(cb, PGR, dg, &pg); vk.vkCmdDispatchIndirect(cb, M.ub, ia + 10 * 4); ui_bar(cb);
    }
    if (g_uirules & 32) {   // the split rule (usplit.comp): pool and examined tiles, the changed-pixel pass (indirect), sub-groups, their accepts (indirect); then phase 5
        dsc ds[4] = {{.b = M.ub}, {C->v.v}, {la}, {lc}}; pc_spl ps = {0, g_uirules, (int)w, (int)h, sc[0], sc[1], kk[0], kk[1], kk[2], kk[3], (g_uirules & 128) && noa2, 1, g_x17};   // (lev 1: L_SFAST)
        bind(cb, PSP, ds, &ps); vk.vkCmdDispatch(cb, 1, 1, 1); ui_bar(cb);
        ps.phase = 1; bind(cb, PSP, ds, &ps); vk.vkCmdDispatchIndirect(cb, M.ub, ia + 22 * 4); ui_bar(cb);
        ps.phase = 2; bind(cb, PSP, ds, &ps); vk.vkCmdDispatch(cb, 1, 1, 1); ui_bar(cb);
        ps.phase = 3; bind(cb, PSP, ds, &ps); vk.vkCmdDispatchIndirect(cb, M.ub, ia + 25 * 4); ui_bar(cb);
        for (int q = 1; q < g_npass; q++) {   // (DM_FG_MV_UISPASS) the next pass's sub-groups (phase 4), then their accepts (indirect)
            ps.phase = 4; ps.x17 = g_x17 | (q - 1) << 8; bind(cb, PSP, ds, &ps); vk.vkCmdDispatch(cb, 1, 1, 1); ui_bar(cb);
            ps.phase = 3; ps.x17 = g_x17 | q << 8; bind(cb, PSP, ds, &ps); vk.vkCmdDispatchIndirect(cb, M.ub, (VkDeviceSize)(UI_O_SX17 + X_IND + 3 * (q - 1)) * 4); ui_bar(cb);
        }
        pg.phase = 5; pg.big = 0; bind(cb, PGR, dg, &pg); vk.vkCmdDispatch(cb, 1, 1, 1); ui_bar(cb);
    }
    ui_ts(cb, 8);
}
// The splat key's layout for a render size (fgmv/key.glsl; computed once per size): code shift, offset, max, index bits, index mask. Index bits max(20, bitlen(n - 1))
// (the op refuses above 2^23 texels), the depth code the rest: up to 2^20 texels the 12-bit code
static const uint32_t *key_lay(uint32_t w, uint32_t h) {
    static uint32_t n0, L[5];
    uint32_t n = w * h, ib = 20;
    if (n == n0) return L;
    while (ib < 23 && ((n - 1) >> ib)) ib++;
    uint32_t db = 32 - ib, k = 12 - db;
    L[0] = 16 + k; L[1] = 12288u >> k; L[2] = (1u << db) - 1; L[3] = ib; L[4] = (1u << ib) - 1; n0 = n; return L;
}
// rec_gen's main passes: 0 the key clear, 1 splat, 2 fill, 3 the static weight's first pass, 4 the static weight (its second pass)
static void gen_pass(VkCommandBuffer cb, int i, const rgs *r) {
    const opset *A = r->A, *C = r->C; const float *sc = r->sc; int pick = r->pick;
    if (i == 0) { VkClearColorValue zero = {{0}}; VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}; vk.vkCmdClearColorImage(cb, M.key.im->img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rg); return; }
    const uint32_t *kl = key_lay(C->w, C->h);
    if (i == 1) {
        dsc ds[4] = {{C->d.v}, {C->v.v}, {M.key.v}, {C->f.v}};
        pc_sp ps = {(int)C->w, (int)C->h, r->t, sc[0], sc[1], kl[0], (int)kl[1], (int)kl[2], kl[3], kl[4]};
        bind(cb, PS, ds, &ps); vk.vkCmdDispatch(cb, (C->w + 15) / 16, (C->h + 15) / 16, 1); return;
    }
    if (i == 2) {
        dsc df[5] = {{M.key.v}, {C->d.v}, {C->v.v}, {M.pick.v}, {M.field.v}};
        pc_f2 pf = {(int)C->w, (int)C->h, g_fill, (pick ? 1u : 0u) | (!pick || r->fieldf ? 2u : 0u), kl[4]};
        bind(cb, PF2, df, &pf);
        vk.vkCmdDispatch(cb, (C->w + 15) / 16, (C->h + 15) / 16, 1); return;
    }
    uint32_t w = r->w, h = r->h; float t = r->t; mimg *sw = g_static == 2 ? &M.swq : &M.swh;
    dsc dw[9] = {{r->aview, g_lin}, {r->cview, g_lin}, {pick ? M.pick.v : M.field.v}, {A->z.v}, {C->z.v}, {A->d.v}, {C->d.v}, {sw->v}, {M.s1.v}};
    pc_sw pw = {(int)w, (int)h, (int)C->w, (int)C->h, (int)sw->im->w, (int)sw->im->h, t, sc[0], sc[1], g_ratio, (float)C->w / (float)w, (float)C->h / (float)h,
                (float)w / (float)C->w, (float)h / (float)C->h, (pick ? 1u : 0u) | (g_staticz ? 2u : 0u) | (g_static == 2 ? 4u : 0u) | (r->dbg == 6 ? 8u : 0u) | (g_stfix ? 32u : 0u)};
    int tw = 16, sp = PSS16; uint32_t nx = (sw->im->w + tw - 1) / tw, ny = (sw->im->h + tw - 1) / tw;
    if (i == 3) { bind(cb, sp, dw, &pw); vk.vkCmdDispatch(cb, (sw->im->w + 13) / 14, (sw->im->h + 13) / 14, 1); return; }   // (pass 1: 14 x 14 tiles)
    pw.flags |= 16u;
    bind(cb, sp, dw, &pw); vk.vkCmdDispatch(cb, nx, ny, 1);
}
// One generated frame: splat, fill, resolve (+ UI). A / C: their opsets (NULL: repeat C); aview / cview: sampled views of the display inputs; layer: C's UI layer (0: none)
static void rec_gen(VkCommandBuffer cb, const opset *A, const opset *C, const float *sc, VkImageView aview, VkImageView cview, VkImageView layer, VkImageView out,
                    uint32_t w, uint32_t h, float t, VkBuffer rbuf, VkDeviceSize roff, int cutf, int dbg, int fieldf, void (*mark)(VkCommandBuffer, int), const gx *x) {
    int rep = !A || !C, pick = !rep;   // fieldf: also store the plain field (for the replay dump)
    int ui = x && !rep && x->la && layer && g_uiflow, stb = x && !rep && g_static, b4 = x && !rep && (ui || g_static || g_bothfar);
    rgs r = {A, C, sc, aview, cview, w, h, t, pick, fieldf, stb, dbg, x};
    if (!rep) {
        gen_pass(cb, 0, &r); vka_barrier(cb); if (mark) mark(cb, 3);
        gen_pass(cb, 1, &r); vka_barrier(cb); if (mark) mark(cb, 4);
        gen_pass(cb, 2, &r); vka_barrier(cb); if (mark) mark(cb, 5);
    } else if (mark) { mark(cb, 3); mark(cb, 4); mark(cb, 5); }
    g_uk = ui && x->first && x->hslot >= 0 && M.uts[x->hslot] ? x->hslot : -1; ui_ts(cb, 0);
    if (x && x->first && x->hslot >= 0 && g_uk < 0) M.uts[x->hslot] = 0;   // (no UI flow in this pair: nothing to read)
    if (x && x->store && x->first) ui_store(cb, layer, x->slot, w, h, x->kv);
    ui_ts(cb, 1);
    if (ui) {
        if (x->first) ui_pair(cb, C, sc, x->la, layer, x->slot, w, h, !x->a2);
        float kk[4]; ui_k(C, w, h, kk);
        dsc dg[2] = {{.b = M.ub}, {C->v.v}}; pc_gr pg = {2, x->slot, g_uirules, 1 << 20, 0, t, sc[0], sc[1], kk[0], kk[1], kk[2], kk[3], g_uicapn};
        bind(cb, PGR, dg, &pg); vk.vkCmdDispatch(cb, 8, 1, 1); ui_bar(cb);   // (8 workgroups: 256 destination tiles each)
        ui_ts(cb, 9); g_uk = -1;
    }
    if (mark) mark(cb, 6);
    mimg *sw = g_static == 2 ? &M.swq : &M.swh;
    if (stb) {
        gen_pass(cb, 3, &r); vka_barrier(cb);
        gen_pass(cb, 4, &r); vka_barrier(cb);
    }
    if (mark) mark(cb, 7);
    dsc dr[14] = {{aview, g_lin}, {cview, g_lin}, {rep ? VK_NULL_HANDLE : pick ? M.pick.v : M.field.v}, {rep ? VK_NULL_HANDLE : A->z.v}, {rep ? VK_NULL_HANDLE : C->z.v},
                  {rep ? VK_NULL_HANDLE : A->e.v, g_lin}, {rep ? VK_NULL_HANDLE : C->e.v, g_lin}, {layer}, {out}, {.b = rbuf ? rbuf : M.zb, .off = rbuf ? roff : 0, .range = 32},
                  {stb ? sw->v : VK_NULL_HANDLE, g_lin}, {ui ? x->la : VK_NULL_HANDLE}, {.b = M.ub}, {ui && (g_uirules & 160) == 160 ? x->a2 : VK_NULL_HANDLE}};
    int big = !rep && (C->cw != C->w || C->ch != C->h);   // (op images larger than the size in use: the edge distance is sampled in their corner)
    pc_re pr = {(int)w, (int)h, rep ? 1 : (int)C->w, rep ? 1 : (int)C->h, t, sc ? sc[0] : 1, sc ? sc[1] : 1, g_ratio, g_tol, 0.0f, big ? (float)C->cw : 0, big ? (float)C->ch : 0,
                (layer ? 1u : 0u) | (rep ? 2u : 0u) | (cutf ? 4u : 0u) | (g_soft ? 8u : 0u) | ((uint32_t)(dbg & 15) << 8) | (pick ? 0x10000u : 0u) |
                (stb ? 0x20000u : 0u) | (b4 && g_bothfar ? 0x40000u : 0u) | (ui ? 0x80000u : 0u) | (b4 ? 0x100000u : 0u) |
                (ui && (g_uirules & 160) == 160 && x->a2 ? 0x200000u : 0u)};
    bind(cb, b4 ? PR2 : PR, dr, &pr); vk.vkCmdDispatch(cb, (w + 15) / 16, (h + 15) / 16, 1);
    if (x && x->hslot >= 0 && ui && x->first) {   // the counters (vka_fgmv_done)
        vka_barrier(cb);
        VkBufferCopy bc[2] = {{(VkDeviceSize)UI_O_CNT * 4, (VkDeviceSize)x->hslot * UI_NCNT * 4, UI_NCNT * 4},   // (+ the split's extra counters after the NKS slots)
                              {(VkDeviceSize)UI_O_SX17 * 4, (VkDeviceSize)(NKS * UI_NCNT + x->hslot * UI_NX17) * 4, UI_NX17 * 4}};
        vk.vkCmdCopyBuffer(cb, M.ub, M.hb, 2, bc);
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        M.uis[x->hslot] = 1;
    }
}
static unsigned g_mk;
static void mark_ts(VkCommandBuffer cb, int i) { if (M.ts[g_mk]) Q.wr(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, M.qp, g_mk * NQ + (uint32_t)i); }   // (each after a barrier)

// Why a pair shows the game frame C again: the status reason (vka_fgmv_status, nxhost_framegen_status) and a log line for each cause at its 1st, 300th and every 9000th pair
static struct { int why; char txt[300]; uint64_t run[5], lines[5]; } Wy;   // run: the cause's pairs in a row
static void why_note(int why, const vka_fgui_frame *u, int use_pre, int haveA, int haveC) {
    char t[300] = "";
    if (why == 1) {
        if (atomic_load(&g_opw)) { pthread_mutex_lock(&g_mu); snprintf(t, sizeof t, "no motion data: %s", g_opwt); pthread_mutex_unlock(&g_mu); }
        else snprintf(t, sizeof t, "no motion data: %s", !haveC ? "no DLSS op in this game frame (a map, a full-screen menu)" : "no DLSS op in the frame before");
    } else if (why == 2) {
        char b[240]; if (!use_pre) vka_fgui_nopre(u, M.w, M.h, b, sizeof b); else snprintf(b, sizeof b, "%s", "the frame before had none");
        snprintf(t, sizeof t, "no picture without UI: %s", b);
    } else if (why == 3) snprintf(t, sizeof t, "%s", "the input changed between the picture without UI and the window texture");
    else if (why == 4) snprintf(t, sizeof t, "%s", "the render size changed between the two game frames");
    for (int i = 1; i < 5; i++) if (i != why) Wy.run[i] = 0;
    if (why) { uint64_t r = ++Wy.run[why]; if ((r == 1 || r == 300 || r % 9000 == 0) && Wy.lines[why]++ < 24) VLOG("fg mv: game frame shown again: %s (%llu pairs in a row)", t, (unsigned long long)r); }
    pthread_mutex_lock(&g_mu); Wy.why = why; snprintf(Wy.txt, sizeof Wy.txt, "%s", t); pthread_mutex_unlock(&g_mu);
}
// The last pair's cause (0: generated) and its reason (vka_framegen.c, the status)
int vka_fgmv_status(char *b, size_t n) { pthread_mutex_lock(&g_mu); int w = Wy.why; if (b && n) snprintf(b, n, "%s", Wy.txt); pthread_mutex_unlock(&g_mu); return w; }

// The present thread, inside the frame generation's command buffer after the input copy (vka_framegen.c): the ng generated frames between A (count-1) and C (count).
// u: C's snapshots (and op slot); opq_a / pre_a: A's op slot and whether A's input is its picture without UI; force: the input kind changed (repeat); dch: DLSS ran in one only
int vka_fgmv_gen(VkCommandBuffer cb, unsigned k, uint64_t count, size_t ng, const vka_fgui_frame *u, int use_pre, uint64_t opq_a, int pre_a, int force, int dch) {
    k %= NKS;
    opset *A = NULL, *C = NULL; float sa[2], sc[2] = {1, 1};
    op_reap();
    int haveA = op_get(opq_a, &A, sa), haveC = u && op_get(u->op_q, &C, sc);
    VkImageView av = VK_NULL_HANDLE, cv = VK_NULL_HANDLE;
    VkImage ai = in_of(count - 1, 2, &av), ci = in_of(count, 1, &cv);   // (A is game frame count - 1; its snapshot: one more free slot than C's, which vka_fgui_take checked just now)
    int why = !haveA || !haveC ? 1 : !use_pre || !pre_a || !ai ? 2 : force ? 3 : A->w != C->w || A->h != C->h || A->cw != C->cw || A->ch != C->ch ? 4 : 0;
    M.nall++;
    if (why == 1) M.rep_nodata++; else if (why == 2) M.rep_nopre++; else if (why == 3) M.rep_force++; else if (why == 4) M.rep_size++; else M.gen++;
    why_note(why, u, use_pre, haveA, haveC);
    if (M.nall % 900 == 0) VLOG("fg mv: %llu frame pairs: %llu generated, game frame shown again: %llu without motion data (no DLSS op), %llu without the picture without UI, %llu input change, %llu size change",
                                (unsigned long long)M.nall, (unsigned long long)M.gen, (unsigned long long)M.rep_nodata, (unsigned long long)M.rep_nopre, (unsigned long long)M.rep_force, (unsigned long long)M.rep_size);
    if (why) { A = C = NULL; }
    if (C && !rsize(cb, C->w, C->h)) { A = C = NULL; }
    if (!ai) { ai = ci; av = cv; }   // (A's snapshot gone: C again, no pass reads A)
    int ts = VKA_ENVF("DM_VK_FG_GPUTIME", 0) > 0 && q_ready();
    if (ts && !M.qp) { VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = NKS * NQ}; if (Q.create(V.dev, &qi, NULL, &M.qp)) ts = 0; }
    M.ts[k] = ts; g_mk = k;
    int uts = ts && VKA_ENV("DM_VK_FG_GPUTIME_SPLIT", 0) != 0;
    if (uts && !M.uqp) { VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = NKS * NUQ}; if (Q.create(V.dev, &qi, NULL, &M.uqp)) uts = 0; }
    M.uts[k] = uts;
    if (uts) Q.rs(cb, M.uqp, k * NUQ, NUQ);
    // Stamps: 0 here (the input copy before it may still run), 1 the copy done, 2 the cut guard, 3 the key clear, 4 splat, 5 fill, 6 UI flow, 7 static weight, 8 resolve+UI,
    // 9 the more generated frames
    if (ts) { Q.rs(cb, M.qp, k * NQ, NQ); Q.wr(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, M.qp, k * NQ); vka_barrier(cb); mark_ts(cb, 1); }
    // The cut guard (DM_FG_CUT=repeat): thumbnails, tiles and the decision into its buffer, which the resolve reads
    VkBuffer rb = VK_NULL_HANDLE; VkDeviceSize ro = 0;
    int cutf = A && vka_fgui_cutpass(cb, k, ai, ci, M.w, M.h, u, dch, count, &rb, &ro);
    if (ts) vka_barrier(cb);
    mark_ts(cb, 2);
    if (!av || !cv) { VRED_ONCE("fg mv: views of the inputs: no generated frames"); return 0; }
    VkImageView lv = use_pre && u && u->has_ui ? u->layer_v : VK_NULL_HANDLE;
    // the UI flow: C's UI layer is kept for the next pair (the UI flow's A); A's is the one the pair before kept
    int uiok = g_uiflow && lv && M.w <= UI_MAXW && M.h <= UI_MAXH, slot = (int)(count & 1), l0 = (int)(count % 3), l1 = (int)((count + 2) % 3), l2 = (int)((count + 1) % 3);
    if (g_uiflow && lv && !uiok) VRED_ONCE("fg mv: the UI flow's tile grid holds displays up to %dx%d: off at %ux%u", UI_MAXW, UI_MAXH, M.w, M.h);
    // A's layer: the copy the pair before made, or A's snapshot (read in place: its input is too, checked above)
    VkImageView la = !uiok || M.uq[l1] != count ? VK_NULL_HANDLE : M.ukl[l1] ? M.uil[l1].v : in_direct(count - 1) ? M.dr[l1].lv : VK_NULL_HANDLE;
    // A2's (the split rule's rank-2 paste): the frame before A's, kept the same way (read in place: while the ring still holds its snapshot)
    VkImageView a2 = !la || M.uq[l2] != count - 1 ? VK_NULL_HANDLE : M.ukl[l2] ? M.uil[l2].v :
                     in_direct(count - 2) && vka_fgui_snap_q(M.dr[l2].im, 3) == M.dr[l2].q ? M.dr[l2].lv : VK_NULL_HANDLE;
    int cdir = in_direct(count) && M.dr[l0].lv == lv;
    gx x = {la, slot, 1, uiok, (int)k, cdir ? VK_NULL_HANDLE : M.uil[l0].v, a2};
    M.uis[k] = 0;
    for (size_t g = 0; g < ng && g < NG; g++) {
        x.first = g == 0;
        rec_gen(cb, A, C, sc, av, cv, lv, M.outv[g], M.w, M.h, (float)(g + 1) / (float)(ng + 1), rb, ro, cutf, g_dbg, 0, g == 0 ? mark_ts : NULL, &x);
        vka_barrier(cb); if (g == 0) mark_ts(cb, 8);
    }
    M.uq[l0] = uiok ? count + 1 : 0; M.ukl[l0] = uiok && !cdir;
    vka_barrier(cb); mark_ts(cb, 9);
    return 1;
}
// After the slot's fence (vka_framegen.c): its timing
void vka_fgmv_done(unsigned k) {
    k %= NKS;
    if (M.uis[k] && M.hmap) {   // the UI flow's counters of that pair
        const uint32_t *c = M.hmap + k * UI_NCNT; M.uis[k] = 0;
        uint32_t mv = c[UC_ON] ? c[UC_NM] : 0; M.un++; M.umv += mv > 0; M.usum += mv; if (mv > M.umax) M.umax = mv; M.ugr += c[UC_ON] ? c[UC_MOVG] : 0;
        M.uover += c[UC_OVER] || c[UC_MAXSRC] || c[UC_CAP]; M.uveto += c[UC_VETO]; M.ucap += c[UC_CAP]; M.unb += c[UC_BIGS] && !c[UC_NEEDBIG];
        M.uvg += c[UC_VGATE]; M.uvc += c[UC_VCONS]; M.urp += c[UC_REPICK]; M.usm += c[UC_SMALL]; M.upo += c[UC_POVER];
        M.upu += c[UC_PARTU]; M.upt += c[UC_PARTT]; if (c[UC_SRCMX] > M.usx) M.usx = c[UC_SRCMX];
        M.ust += c[UC_STRIED]; M.usk += c[UC_SKEPT]; M.uss += c[UC_STILES]; M.usg += c[UC_SGUARD]; M.us2 += c[UC_SR2]; M.uso += c[UC_SOVER]; M.usn += c[UC_SNOA2];
        { const uint32_t *x = M.hmap + NKS * UI_NCNT + k * UI_NX17; for (int i = 0; i < UI_NX17; i++) M.ux[i] += x[i]; }
        double now = (double)now_ns() / 1e9; if (!M.ut0) M.ut0 = now;
        if (now - M.ut0 >= 10.0) {
            char po[480] = ""; int pn = 0; if (M.upo) pn = snprintf(po, sizeof po, ", pick_over %llu", (unsigned long long)M.upo);
            if (g_uirules & 16) pn += snprintf(po + pn, sizeof po - (size_t)pn, ", partial %llu (tiles %llu), srcmax %llu", (unsigned long long)M.upu, (unsigned long long)M.upt, (unsigned long long)M.usx);
            if (g_uirules & 32) {   // (the split rule)
                pn += snprintf(po + pn, sizeof po - (size_t)pn, ", split tried %llu kept %llu tiles %llu guard %llu r2 %llu", (unsigned long long)M.ust, (unsigned long long)M.usk,
                               (unsigned long long)M.uss, (unsigned long long)M.usg, (unsigned long long)M.us2);
                if (M.uso) pn += snprintf(po + pn, sizeof po - (size_t)pn, " over %llu", (unsigned long long)M.uso);
                if (M.usn) pn += snprintf(po + pn, sizeof po - (size_t)pn, " noA2 %llu", (unsigned long long)M.usn);
                const uint64_t *x = M.ux;   // (window sums; overrun must stay 0)
                snprintf(po + pn, sizeof po - (size_t)pn, ", v17 more %llu integ_add %llu integ_drop %llu torn %llu guard2_pass %llu integ_overrun %llu [x17 %d]", (unsigned long long)x[X_MORE],
                         (unsigned long long)x[X_IADD], (unsigned long long)x[X_IDROP], (unsigned long long)x[X_TORN], (unsigned long long)x[X_G2P], (unsigned long long)x[X_OVR], g_x17);
            }
            VLOG("fg mv ui: %llu pairs, %llu with tiles moved, tiles moved sum %llu max %llu, moving groups sum %llu, overflow %llu, vetoed gate %llu cons %llu, repicked %llu, small %llu%s"
                 " (work cap %llu; big groups left %llu; rules %d)", (unsigned long long)M.un, (unsigned long long)M.umv, (unsigned long long)M.usum, (unsigned long long)M.umax,
                 (unsigned long long)M.ugr, (unsigned long long)M.uover, (unsigned long long)M.uvg, (unsigned long long)M.uvc, (unsigned long long)M.urp, (unsigned long long)M.usm, po,
                 (unsigned long long)M.ucap, (unsigned long long)M.unb, g_uirules);
            M.un = M.umv = M.usum = M.umax = M.ugr = M.uover = M.uveto = M.ucap = M.unb = M.uvg = M.uvc = M.urp = M.usm = M.upo = M.upu = M.upt = M.usx = 0;
            M.ust = M.usk = M.uss = M.usg = M.us2 = M.uso = M.usn = 0; memset(M.ux, 0, sizeof M.ux); M.ut0 = now; }
    }
    if (M.uts[k]) {   // the UI flow's sub-pass times (DM_VK_FG_GPUTIME_SPLIT=1; only pairs that ran it)
        uint64_t v[NUQ]; M.uts[k] = 0;
        if (!Q.get(V.dev, M.uqp, k * NUQ, NUQ, sizeof v, v, 8, VK_QUERY_RESULT_64_BIT)) { for (int i = 0; i < NUQ - 1; i++) M.utsum[i] += (double)(v[i + 1] - v[i]) * Q.tick / 1e6; M.utn++; }
        double now = (double)now_ns() / 1e9; if (!M.utt0) M.utt0 = now;
        if (now - M.utt0 >= VKA_ENVF("DM_VK_FG_GPUTIME", 10) && M.utn) {
            double s = 0, *a = M.utsum; for (int i = 0; i < NUQ - 1; i++) s += a[i];
            VLOG("fg mv ui gpu: %u pairs, mean ms: coverage %.3f, tiles %.3f, labels %.3f, candidate SADs %.3f, pick %.3f, refine %.3f, final %.3f, big pass %.3f, paste lists %.3f"
                 " (total %.3f) [rules %d]", M.utn, a[0] / M.utn, a[1] / M.utn, a[2] / M.utn, a[3] / M.utn, a[4] / M.utn, a[5] / M.utn, a[6] / M.utn, a[7] / M.utn, a[8] / M.utn, s / M.utn, g_uirules);
            memset(M.utsum, 0, sizeof M.utsum); M.utn = 0; M.utt0 = now; }
    }
    if (!M.ts[k]) return;
    uint64_t v[NQ]; M.ts[k] = 0;
    if (!Q.get(V.dev, M.qp, k * NQ, NQ, sizeof v, v, 8, VK_QUERY_RESULT_64_BIT)) { for (int i = 0; i < NQ - 1; i++) M.tsum[i] += (double)(v[i + 1] - v[i]) * Q.tick / 1e6; M.tn++; }
    double now = (double)now_ns() / 1e9; if (!M.tt0) M.tt0 = now;
    if (now - M.tt0 >= VKA_ENVF("DM_VK_FG_GPUTIME", 10) && M.tn) {
        double s = 0; for (int i = 1; i < NQ - 1; i++) s += M.tsum[i];
        VLOG("fg mv gpu: %u frames, mean ms: input copy tail %.3f, cut guard %.3f, key clear %.3f, splat %.3f, fill %.3f, ui flow %.3f, static %.3f, resolve+UI %.3f, more generated %.3f"
             " (total %.3f; after the copy %.3f) [dither %d uiflow %d uigate %d static %d staticz %d bothfar %d]", M.tn, M.tsum[0] / M.tn,
             M.tsum[1] / M.tn, M.tsum[2] / M.tn, M.tsum[3] / M.tn, M.tsum[4] / M.tn, M.tsum[5] / M.tn, M.tsum[6] / M.tn, M.tsum[7] / M.tn, M.tsum[8] / M.tn,
             (s + M.tsum[0]) / M.tn, s / M.tn, g_dither, g_uiflow, g_uigate, g_static, g_staticz, g_bothfar);
        memset(M.tsum, 0, sizeof M.tsum); M.tn = 0; M.tt0 = now; }
}

// ---------- DM_FG_MV_REPLAY=<tag>: inputs from files through the same passes (parity and determinism) ----------
// <DM_CTL folder>/fgmv/r<tag>/manifest.txt:
//   size <W> <H> <RW> <RH>
//   pair <name> <t> <mv scale x> <mv scale y> <A.raw> <C.raw> <C layer.raw | -> <depth A.raw> <depth C.raw> <motion C.raw>
// (A / C / layer: RGBA8 W*H*4, the sRGB bits; depth: float32 RW*RH*4 (the DLSS op's D32 depth); motion: RG16F RW*RH*4, raw), then optionally <A layer.raw | -> (the UI
// flow's A), then optionally <A2 layer.raw | -> (the frame before A's: the split rule's rank-2 paste). Each pair runs twice: <name>.raw (RGBA8),
// with DM_FG_MV_REPLAY_DUMP=1 also <name>_key.raw (R32UI), <name>_field.raw (RG32UI), <name>_{a,c}_{z,d,v,e}.raw (the op passes' outputs); replay.txt: per pair
// the FNV-1a 64 of both runs and whether they are the same; with the dump also <name>_flow.txt (the UI flow's table, ui_flow_txt) and <name>_stw.raw (the static
// weight, R8 at its size; DM_FG_MV_DEBUG=6: the depth gate). The present stalls meanwhile
static uint64_t fnv(const uint8_t *p, size_t n) { uint64_t h = 1469598103934665603ull; for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; } return h; }
static int rd(const char *dir, const char *name, void *dst, size_t n) { char p[1200]; snprintf(p, sizeof p, "%s/%s", dir, name); FILE *f = fopen(p, "rb"); if (!f) return 0; size_t r = fread(dst, 1, n, f); fclose(f); return r == n; }
static int wr(const char *dir, const char *name, const void *src, size_t n) { char p[1200]; snprintf(p, sizeof p, "%s/%s", dir, name); FILE *f = fopen(p, "wb"); if (!f) return 0; size_t r = fwrite(src, 1, n, f); fclose(f); return r == n; }
// The UI flow's table from its buffer (the counters, the groups that reached the MV gate, the moving tiles), in the reference model's text layout; this one when a rule
// of the gate is on (uipick, uicons, uismall), with their counters, and ui_flow_txt below without them
static void ui_flow15_txt(FILE *f, const uint32_t *u, float t) {
    const uint32_t *c = u + UI_O_CNT;
    fprintf(f, "# device UI flow table with the gate's rules, t = %g. active %u groups %u moving_groups %u vetoed_groups %u vetoed_gate %u vetoed_cons %u repicked %u small_accepted %u moved %u overflow %u",
            t, c[UC_ACTIVE], c[UC_GROUPS], c[UC_MOVG], c[UC_VETO], c[UC_VGATE], c[UC_VCONS], c[UC_REPICK], c[UC_SMALL], c[UC_MOVED], c[UC_OVER]);
    if (g_uirules & 16) fprintf(f, " big %u partial_units %u partial_tiles %u srcmax %u", c[UC_BIGS], c[UC_PARTU], c[UC_PARTT], c[UC_SRCMX]);   // (the tile cap rule)
    fprintf(f, "\n");
    int spl = (g_uirules & 32) != 0;
    const uint32_t *sn = u + UI_O_SCN; uint32_t nsg = c[UC_SOVER] || c[UC_SNOA2] ? 0 : sn[1];
    if (spl) fprintf(f, "# v1.6 split examined %u over %u noA2 %u tried %u kept %u guard %u split_tiles %u r2_tiles %u", c[UC_SNOA2] ? 0 : sn[0], c[UC_SOVER], c[UC_SNOA2], c[UC_STRIED],
                     c[UC_SKEPT], c[UC_SGUARD], c[UC_STILES], c[UC_SR2]);
    const uint32_t *x = u + UI_O_SX17; int v17 = spl && g_x17 != 0;   // (a split option on: the extended table)
    if (v17) fprintf(f, " more %u pass2 %u pass3 %u integ_add %u integ_drop %u torn %u guard2_pass %u", x[X_MORE], x[X_P2], x[X_P3], x[X_IADD], x[X_IDROP], x[X_TORN], x[X_G2P]);
    if (spl) fprintf(f, "\n");
    for (int g = 0; g < 2040; g++) {
        uint32_t st = u[UI_O_GSTAT + g], rp = u[UI_O_GRP + g], r = u[UI_O_GM2 + g] % 9409u; if (!st) continue;
        int dy = (int)(r / 97) - 48, dx = (int)(r % 97) - 48; char pf[80];
        if (rp >> 31) snprintf(pf, sizeof pf, "repick (%d %d) -> (%d %d)", (int)((rp >> 8) & 255) - 64, (int)(rp & 255) - 64, dy, dx);
        else if (rp >> 30 && st > 3) snprintf(pf, sizeof pf, "pick (%d %d) pick_over %u", dy, dx, rp & 0x3FFFFFFFu);
        else snprintf(pf, sizeof pf, "pick (%d %d)", dy, dx);
        fprintf(f, "# group tiles %u first (%d %d) %s", u[UI_O_GSZ + g], g / 60, g % 60, pf);
        if (st <= 3 || st == 7) {   // (7: kept, then dropped by the tile cap rule's partial keep)
            uint32_t gf = u[UI_O_GF + g]; int fy = (int)((gf >> 10) & 1023) - 512, fx = (int)(gf & 1023) - 512, fm = abs(fy) > abs(fx) ? abs(fy) : abs(fx); float vm, e; memcpy(&vm, &u[UI_O_GVM + g], 4); memcpy(&e, &u[UI_O_GE + g], 4);
            fprintf(f, " f (%d %d) vmax %.6g e %.6g lim %g gate 4*vmax %s max|f| %d%s -> %s\n", fy, fx, vm, e, fm > 16 ? 0.5 * fm : 8.0, st == 1 ? "<" : ">=", fm, u[UI_O_GSM + g] ? " small" : "",
                    st == 1 ? "VETO_GATE" : st == 2 ? "keep" : st == 7 ? "partial" : "VETO_CONS");
        } else fprintf(f, " -> %s\n", st == 4 ? "q4rej" : st == 5 ? "fullrej" : "fmin");
    }
    for (uint32_t g = 0; spl && g < nsg && g < UI_SGTOT; g++) {   // the tried sub-groups (usplit.comp; in the order evaluated: pass 1, 2, 3)
        static const char *const wn[10] = {"keep", "nocand", "tilerej", "fullrej", "massrej", "fmin", "veto_gate", "veto_cons", "guard", "partial"};
        const uint32_t *r = u + sgb(g); uint32_t why = r[SG_WHY] < 10 ? r[SG_WHY] : 1;
        fprintf(f, "# split tiles %u first (%u %u) ", r[SG_N], r[SG_FIRST] / 60, r[SG_FIRST] % 60);
        if (why == 1) { fprintf(f, v17 ? "no candidate -> nocand pass %u\n" : "no candidate -> nocand\n", r[SG_PASS]); continue; }
        fprintf(f, "d4 (%d %d) f (%d %d) kept_tiles %u -> %s", (int)((r[SG_D4] >> 8) & 255) - 64, (int)(r[SG_D4] & 255) - 64, (int)((r[SG_F] >> 10) & 1023) - 512, (int)(r[SG_F] & 1023) - 512,
                r[SG_KT], wn[why]);
        if (why == 8) fprintf(f, " (%u %u)", r[SG_GUARD] / 60, r[SG_GUARD] % 60);
        if (why == 0 || why == 9) fprintf(f, " r2 %u", r[SG_R2]);
        if (v17) fprintf(f, " pass %u", r[SG_PASS]);
        fprintf(f, "\n");
    }
    fprintf(f, "# ty tx fy fx sy sx   (tile row/col; full-res C->A flow; paste shift s = floor((1 - t) f + 0.5))%s\n", spl ? "; S = split tile, R2 = rank-2 paste tile" : "");
    for (int i = 0; i < 2040; i++) {
        uint32_t m = u[UI_O_MV + i], sh = u[UI_O_SH + i], sf = spl ? u[UI_O_SFL + i] : 0; if (!m) continue;
        fprintf(f, "%d %d %d %d %d %d%s%s\n", i / 60, i % 60, (int)((m >> 10) & 1023) - 512, (int)(m & 1023) - 512, (int)((sh >> 10) & 1023) - 512, (int)(sh & 1023) - 512,
                sf & 16 ? " S" : "", (sf & 16) && (sf & 32) ? " R2" : "");
    }
}
static void ui_flow_txt(FILE *f, const uint32_t *u) {
    const uint32_t *c = u + UI_O_CNT;
    fprintf(f, "# device UI flow table: active %u groups %u moving_groups %u vetoed_groups %u moved %u overflow %u (maxsrc %u cap %u searched %u work %u big groups %u needbig %u workbig %u)\n",
            c[UC_ACTIVE], c[UC_GROUPS], c[UC_MOVG], c[UC_VETO], c[UC_MOVED], c[UC_OVER], c[UC_MAXSRC], c[UC_CAP], c[UC_SEARCHED], c[UC_W], c[UC_BIGS], c[UC_NEEDBIG], c[UC_WBIG]);
    if (g_uigate) for (int g = 0; g < 2040; g++) {
        uint32_t st = u[UI_O_GSTAT + g], gf = u[UI_O_GF + g]; if (!st) continue; float vm; memcpy(&vm, &u[UI_O_GVM + g], 4);
        fprintf(f, "# group tiles %u first (%d %d) f (%d %d) vmax %.6g -> %s\n", u[UI_O_GSZ + g], g / 60, g % 60, (int)((gf >> 10) & 1023) - 512, (int)(gf & 1023) - 512, vm, st == 2 ? "keep" : "VETO");
    }
    for (int i = 0; i < 2040; i++) {
        uint32_t m = u[UI_O_MV + i], sh = u[UI_O_SH + i]; if (!m) continue;
        fprintf(f, "%d %d %d %d %d %d\n", i / 60, i % 60, (int)((m >> 10) & 1023) - 512, (int)(m & 1023) - 512, (int)((sh >> 10) & 1023) - 512, (int)(sh & 1023) - 512);
    }
}
static double mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void mv_replay(int tag) {
    char dir[1024], line[2048], path[1200];
    snprintf(dir, sizeof dir, "%s/fgmv/r%d", vka_ctl_dir(), tag); snprintf(path, sizeof path, "%s/manifest.txt", dir);
    FILE *mf = fopen(path, "r"); if (!mf) { VRED("fg mv replay: no %s", path); return; }
    unsigned W = 0, H = 0, RW = 0, RH = 0; int dump = VKA_ENV("DM_FG_MV_REPLAY_DUMP", 0);
    double t0 = mono();
    mimg in[5] = {{0}}, out = {0}, zi[2] = {{0}}, mi[2] = {{0}}; opset os[2] = {{{0}}}; VkBuffer b = VK_NULL_HANDLE; VkDeviceMemory m = VK_NULL_HANDLE; uint8_t *map = NULL; once o = {0};
    size_t one = 0, rone = 0; VkDeviceSize off[22] = {0}; int setup = 0, pairs = 0, same = 0, differ = 0, failed = 0;
    FILE *rf = NULL;
    while (fgets(line, sizeof line, mf)) {
        char *tok[16], *sv = NULL; int nt = 0; for (char *t = strtok_r(line, " \t\r\n", &sv); t && nt < 16; t = strtok_r(NULL, " \t\r\n", &sv)) tok[nt++] = t;
        if (nt >= 5 && !strcmp(tok[0], "size") && !setup) {
            W = (unsigned)atoi(tok[1]); H = (unsigned)atoi(tok[2]); RW = (unsigned)atoi(tok[3]); RH = (unsigned)atoi(tok[4]);
            if (!W || !H || !RW || !RH || RW * RH > 1u << 23) { VRED("fg mv replay: r%d: bad size line", tag); break; }
            one = (size_t)W * H * 4; rone = (size_t)RW * RH * 4;
            // buffer: A, C, layer, out (display) | zA, zC, mvA (zero), mvC, key, field (2x) | 2 x op (z, d, v, e)
            VkDeviceSize sz[22] = {one, one, one, one, rone, rone, rone, rone, rone, rone * 2, rone, rone, rone, rone / 4 + 256, rone, rone, rone / 4 + 256, rone * 2,
                                   one, (VkDeviceSize)UI_O_END * 4, (VkDeviceSize)(W + 1) / 2 * ((H + 1) / 2) + 256, one}, tot = 0;   // + A's layer, the UI flow's buffer, the static weight, A2's layer
            for (int i = 0; i < 22; i++) { off[i] = tot; tot += (sz[i] + 255) & ~(VkDeviceSize)255; }
            tot += rone * 2 + 512;   // the second op set's v and e (offsets 16, 17 below)
            int ok = buf_new(tot, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, &b, &m, (void **)&map);
            ok = ok && vka_fgmv_setup(W, H);   // (the present side's images and the UI flow's buffer; the live pair after this has no UI flow A, below)
            for (int i = 0; i < 5 && ok; i++) ok = img_new(&in[i], W, H, VK_FORMAT_R8G8B8A8_UNORM, U_ALL, i == 4 ? 0xF6A00004ull : 0xF6A00000ull + (uint64_t)i);
            ok = ok && img_new(&out, W, H, VK_FORMAT_R8G8B8A8_UNORM, U_ALL, 0xF6A00003ull);
            for (int i = 0; i < 2 && ok; i++) ok = img_new(&zi[i], RW, RH, VK_FORMAT_R32_SFLOAT, U_ALL, 0xF6A00010ull + (uint64_t)i) && img_new(&mi[i], RW, RH, VK_FORMAT_R16G16_SFLOAT, U_ALL, 0xF6A00020ull + (uint64_t)i);
            ok = ok && once_begin(&o);
            if (ok) {
                for (int i = 0; i < 5; i++) to_general(o.cb, in[i].im->img);
                to_general(o.cb, out.im->img); for (int i = 0; i < 2; i++) { to_general(o.cb, zi[i].im->img); to_general(o.cb, mi[i].im->img); }
                ok = opset_new(o.cb, &os[0], RW, RH, 0xF6A00030ull) && opset_new(o.cb, &os[1], RW, RH, 0xF6A00040ull) && rsize(o.cb, RW, RH);
                ok = once_end(&o) && ok;
            }
            if (!ok) { VRED("fg mv replay: r%d: setup failed", tag); break; }
            memset(map + off[6], 0, rone);   // mvA: zero (A's op pass gives its depth and edges only)
            snprintf(path, sizeof path, "%s/replay.txt", dir); rf = fopen(path, "w");
            if (rf) fprintf(rf, "# fg mv replay r%d: %ux%u render %ux%u, ratio %.3f tol %.1f thr %.3f fill %d soft %d dither %d debug %d"
                            " uiflow %d uigate %d uisr %d static %d staticz %d bothfar %d; per pair: name t hash1 hash2 same|DIFFERS\n",
                            tag, W, H, RW, RH, g_ratio, g_tol, g_thr, g_fill, g_soft, g_dither, g_dbg, g_uiflow, g_uigate, g_uisr, g_static, g_staticz, g_bothfar);
            setup = 1; continue;
        }
        if (!setup || nt < 11 || strcmp(tok[0], "pair")) continue;
        float t = strtof(tok[2], NULL), sc[2] = {strtof(tok[3], NULL), strtof(tok[4], NULL)}; int lay = strcmp(tok[7], "-") != 0, laya = lay && nt >= 12 && strcmp(tok[11], "-") != 0, laya2 = laya && nt >= 13 && strcmp(tok[12], "-") != 0;
        if (!rd(dir, tok[5], map + off[0], one) || !rd(dir, tok[6], map + off[1], one) || (lay && !rd(dir, tok[7], map + off[2], one)) || (laya && !rd(dir, tok[11], map + off[18], one)) || (laya2 && !rd(dir, tok[12], map + off[21], one)) ||
            !rd(dir, tok[8], map + off[4], rone) || !rd(dir, tok[9], map + off[5], rone) || !rd(dir, tok[10], map + off[7], rone)) {
            VRED("fg mv replay: r%d: pair %s: an input file is missing or short", tag, tok[1]); failed++; continue; }
        uint64_t hh[2] = {0, 0}; int okp = 1;
        for (int run = 0; run < 2 && okp; run++) {
            if (!once_begin(&o)) { okp = 0; break; }
            VkCommandBuffer cb = o.cb;
            VkBufferImageCopy c = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {W, H, 1}};
            for (int i = 0; i < 5; i++) { if ((i == 2 && !lay) || (i == 3 && !laya) || (i == 4 && !laya2)) continue; c.bufferOffset = i == 4 ? off[21] : i == 3 ? off[18] : off[i]; vk.vkCmdCopyBufferToImage(cb, b, in[i].im->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c); }
            c.imageExtent = (VkExtent3D){RW, RH, 1};
            c.bufferOffset = off[4]; vk.vkCmdCopyBufferToImage(cb, b, zi[0].im->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
            c.bufferOffset = off[5]; vk.vkCmdCopyBufferToImage(cb, b, zi[1].im->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
            c.bufferOffset = off[6]; vk.vkCmdCopyBufferToImage(cb, b, mi[0].im->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
            c.bufferOffset = off[7]; vk.vkCmdCopyBufferToImage(cb, b, mi[1].im->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
            vka_barrier(cb);
            rec_op(cb, zi[0].v, mi[0].v, &os[0], (int)RW, (int)RH); rec_op(cb, zi[1].v, mi[1].v, &os[1], (int)RW, (int)RH); vka_barrier(cb);
            int uir = laya && g_uiflow && W <= UI_MAXW && H <= UI_MAXH;
            if (uir) { ui_q4(cb, in[3].v, 1, W, H, VK_NULL_HANDLE); ui_q4(cb, in[2].v, 0, W, H, VK_NULL_HANDLE); }   // (A's pyramid in slot 1, C's in 0)
            gx x = {uir ? in[3].v : VK_NULL_HANDLE, 0, 1, 0, -1, VK_NULL_HANDLE, uir && laya2 ? in[4].v : VK_NULL_HANDLE};
            rec_gen(cb, &os[0], &os[1], sc, in[0].v, in[1].v, lay ? in[2].v : VK_NULL_HANDLE, out.v, W, H, t, VK_NULL_HANDLE, 0, 0, g_dbg, dump, NULL, &x);
            vka_barrier(cb);
            c.imageExtent = (VkExtent3D){W, H, 1}; c.bufferOffset = off[3]; vk.vkCmdCopyImageToBuffer(cb, out.im->img, VK_IMAGE_LAYOUT_GENERAL, b, 1, &c);
            if (dump && run == 0) {
                c.imageExtent = (VkExtent3D){RW, RH, 1};
                c.bufferOffset = off[8]; vk.vkCmdCopyImageToBuffer(cb, M.key.im->img, VK_IMAGE_LAYOUT_GENERAL, b, 1, &c);
                c.bufferOffset = off[9]; vk.vkCmdCopyImageToBuffer(cb, M.field.im->img, VK_IMAGE_LAYOUT_GENERAL, b, 1, &c);
                c.bufferOffset = off[10]; vk.vkCmdCopyImageToBuffer(cb, os[0].z.im->img, VK_IMAGE_LAYOUT_GENERAL, b, 1, &c);
                c.bufferOffset = off[11]; vk.vkCmdCopyImageToBuffer(cb, os[1].z.im->img, VK_IMAGE_LAYOUT_GENERAL, b, 1, &c);
                c.bufferOffset = off[12]; vk.vkCmdCopyImageToBuffer(cb, os[1].d.im->img, VK_IMAGE_LAYOUT_GENERAL, b, 1, &c);
                c.bufferOffset = off[13]; vk.vkCmdCopyImageToBuffer(cb, os[0].e.im->img, VK_IMAGE_LAYOUT_GENERAL, b, 1, &c);
                c.bufferOffset = off[14]; vk.vkCmdCopyImageToBuffer(cb, os[1].v.im->img, VK_IMAGE_LAYOUT_GENERAL, b, 1, &c);
                c.bufferOffset = off[15]; vk.vkCmdCopyImageToBuffer(cb, os[1].e.im->img, VK_IMAGE_LAYOUT_GENERAL, b, 1, &c);
                c.bufferOffset = off[16]; vk.vkCmdCopyImageToBuffer(cb, os[1].f.im->img, VK_IMAGE_LAYOUT_GENERAL, b, 1, &c);
                c.bufferOffset = off[17]; vk.vkCmdCopyImageToBuffer(cb, M.pick.im->img, VK_IMAGE_LAYOUT_GENERAL, b, 1, &c);
                VkBufferCopy bc = {0, off[19], (VkDeviceSize)UI_O_END * 4}; vk.vkCmdCopyBuffer(cb, M.ub, b, 1, &bc);
                mimg *sw = g_static == 2 ? &M.swq : &M.swh; c.imageExtent = (VkExtent3D){sw->im->w, sw->im->h, 1}; c.bufferOffset = off[20];
                vk.vkCmdCopyImageToBuffer(cb, sw->im->img, VK_IMAGE_LAYOUT_GENERAL, b, 1, &c);
            }
            if (!once_end(&o)) { okp = 0; break; }
            hh[run] = fnv(map + off[3], one);
            if (run == 0) {
                char nm[200]; snprintf(nm, sizeof nm, "%s.raw", tok[1]); wr(dir, nm, map + off[3], one);
                if (dump) {
                    static const char *dn[10] = {"key", "field", "a_z", "c_z", "c_d", "a_e", "c_v", "c_e", "c_f", "pick"};
                    size_t dz[10] = {rone, rone * 2, rone, rone, rone, rone / 4, rone, rone / 4, rone / 4, rone * 2};
                    for (int i = 0; i < 10; i++) { snprintf(nm, sizeof nm, "%s_%s.raw", tok[1], dn[i]); wr(dir, nm, map + off[8 + i], dz[i]); }
                    if (g_static) { mimg *sw = g_static == 2 ? &M.swq : &M.swh; snprintf(nm, sizeof nm, "%s_stw.raw", tok[1]); wr(dir, nm, map + off[20], (size_t)sw->im->w * sw->im->h); }
                    if (laya && g_uiflow) { snprintf(nm, sizeof nm, "%s/%s_flow.txt", dir, tok[1]); FILE *ff = fopen(nm, "w"); if (ff) { if (g_uirules > 1) ui_flow15_txt(ff, (const uint32_t *)(map + off[19]), t); else ui_flow_txt(ff, (const uint32_t *)(map + off[19])); fclose(ff); } }
                }
            }
        }
        if (!okp) { VRED("fg mv replay: r%d: pair %s: submission failed", tag, tok[1]); failed++; continue; }
        pairs++; if (hh[0] == hh[1]) same++; else differ++;
        if (rf) fprintf(rf, "%s %.4f %016llx %016llx %s\n", tok[1], t, (unsigned long long)hh[0], (unsigned long long)hh[1], hh[0] == hh[1] ? "same" : "DIFFERS");
    }
    fclose(mf); if (rf) fclose(rf);
    vka_queue_idle(); once_free(&o); M.uq[0] = M.uq[1] = M.uq[2] = 0;   // (the replay used both pyramid slots: the next live pair has no A)
    for (int i = 0; i < 5; i++) img_free(&in[i]);
    img_free(&out); for (int i = 0; i < 2; i++) { img_free(&zi[i]); img_free(&mi[i]); opset_free(&os[i]); }
    if (map) vk.vkUnmapMemory(V.dev, m);
    buf_free(&b, &m);
    VLOG("fg mv replay: r%d: %d pairs in %.1f s, repeat same %d, differs %d, failed %d (%s/replay.txt)", tag, pairs, mono() - t0, same, differ, failed, dir);
}
// The present thread, each game frame (vka_framegen.c)
void vka_fgmv_replay_poll(void) {
    static int last; int rp = VKA_ENV("DM_FG_MV_REPLAY", 0);
    if (rp == last) return;
    last = rp; if (!rp) return;
    keys(); if (!ready()) { VRED("fg mv replay: the engine is unavailable (see the [RED] lines)"); return; }
    vka_gate_host_stall(1); mv_replay(rp); vka_gate_host_stall(0);
}
#endif
