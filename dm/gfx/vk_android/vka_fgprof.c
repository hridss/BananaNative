// SPDX-License-Identifier: MIT
// The motion-vector frame generator's per-game profile (vka_fgmv.c, vka_fgui.c): the game.tsv fg_* rows as game.py compiles them (DM_GAME_FG_*, dm/games/README.md),
// over the core defaults below, and the engine choice. Each DM_FG_* switch the environment, env.txt or a live control line sets wins over its row; a call site passes the
// profile's value as its default (VKA_ENV(key, vka_fgprof_get()->...)). Without fg_* rows: no pre-UI or UI layer keys (the engine shows the game frames only), every rule off,
// the tuning values below.
// The engine: DM_FG_ENGINE=mv|lsfg when set; else the launcher's choice (Frame generation: Off, Native FG, LSFG), DM_FG_MENU=mv|lsfg at start and nxhost_framegen_engine
// while the game runs, where Native FG is the engine only on a game with the fg_mv row (else LSFG); else LSFG.
#if defined(__ANDROID__)
#include "vka.h"
#include "vk_android.h"
#include "game_profile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The core defaults (a game without the row)
#ifndef DM_GAME_FG_MV
#define DM_GAME_FG_MV 0
#endif
#ifndef DM_GAME_FG_UI_PRE
#define DM_GAME_FG_UI_PRE 0ull
#endif
#ifndef DM_GAME_FG_UI_LAYER
#define DM_GAME_FG_UI_LAYER 0ull
#endif
#ifndef DM_GAME_FG_FEATURES
#define DM_GAME_FG_FEATURES 0
#endif
#ifndef DM_GAME_FG_TUNE_RATIO
#define DM_GAME_FG_TUNE_RATIO 0.05      // depth ratio of the occlusion test (passed as 1 + ratio)
#endif
#ifndef DM_GAME_FG_TUNE_TOL
#define DM_GAME_FG_TUNE_TOL 40          // luma difference (0..255) under which A and C agree
#endif
#ifndef DM_GAME_FG_TUNE_EDGE
#define DM_GAME_FG_TUNE_EDGE 1.15       // the 3x3 depth ratio of a depth edge
#endif
#ifndef DM_GAME_FG_TUNE_FILL
#define DM_GAME_FG_TUNE_FILL 6          // hole fill iterations (0..6)
#endif
#ifndef DM_GAME_FG_TUNE_SOFT
#define DM_GAME_FG_TUNE_SOFT 1          // the soft band at depth edges
#endif
#ifndef DM_GAME_FG_TUNE_UISR
#define DM_GAME_FG_TUNE_UISR 12         // the UI flow's coarse search radius in 16-px cells (1..12)
#endif
#ifndef DM_GAME_FG_TUNE_UISSUPR
#define DM_GAME_FG_TUNE_UISSUPR 1       // the split rule's tile support window: 1 (3x3) or 2 (5x5 for the tiles 3x3 does not support)
#endif
#ifndef DM_GAME_FG_TUNE_UISINTEG
#define DM_GAME_FG_TUNE_UISINTEG 0      // the split rule's object integrity: 0 off, 2 add and drop
#endif
#ifndef DM_GAME_FG_TUNE_UISPASS
#define DM_GAME_FG_TUNE_UISPASS 1       // the split rule's passes (1..3)
#endif
#ifndef DM_GAME_FG_TUNE_UISGUARD
#define DM_GAME_FG_TUNE_UISGUARD 1      // the split rule's guard: 0 off, 1 any changed pixel next to the part, 2 only what the paste can reach and the flow does not explain
#endif
#ifndef DM_GAME_FG_TUNE_CUT_RES
#define DM_GAME_FG_TUNE_CUT_RES 12      // the cut guard's warp residual
#endif
#ifndef DM_GAME_FG_TUNE_CUT_CORR
#define DM_GAME_FG_TUNE_CUT_CORR 0.5    // the cut guard's median tile correlation
#endif
#ifndef DM_GAME_FG_TUNE_CUT_MV
#define DM_GAME_FG_TUNE_CUT_MV 0.220848056537   // the cut guard's mean motion, a fraction of the render width (250 px at 1132)
#endif

static const vka_fgprof g_prof = {DM_GAME_FG_MV, DM_GAME_FG_UI_PRE, DM_GAME_FG_UI_LAYER, DM_GAME_FG_FEATURES, DM_GAME_FG_TUNE_RATIO, DM_GAME_FG_TUNE_TOL, DM_GAME_FG_TUNE_EDGE,
                                  DM_GAME_FG_TUNE_CUT_RES, DM_GAME_FG_TUNE_CUT_CORR, DM_GAME_FG_TUNE_CUT_MV, DM_GAME_FG_TUNE_FILL, DM_GAME_FG_TUNE_SOFT, DM_GAME_FG_TUNE_UISR,
                                  DM_GAME_FG_TUNE_UISSUPR, DM_GAME_FG_TUNE_UISINTEG, DM_GAME_FG_TUNE_UISPASS, DM_GAME_FG_TUNE_UISGUARD};
static _Atomic int g_told;
const vka_fgprof *vka_fgprof_get(void) {
    if (!atomic_exchange(&g_told, 1)) {
        static const char *const fn[] = {"uiflow", "uigate", "static", "staticz", "bothfar", "dither", "cut", "uipick", "uicons", "uismall", "uicap", "uisplit", "stfix"};
        char f[128] = ""; size_t n = 0;
        for (int i = 0; i < 13; i++) if (g_prof.features & (1u << i)) n += (size_t)snprintf(f + n, sizeof f - n, " %s", fn[i]);
        VLOG("[profile] fg: engine %s for this game (fg_mv), pre-UI pass %016llx, UI layer %016llx, rules%s; ratio %g tol %g edge %g fill %d soft %d uisr %d uissupr %d uisinteg %d uispass %d uisguard %d, cut res %g corr %g mv %g of the render width "
             "(a DM_FG_* switch that is set wins)", g_prof.mv ? "validated" : "not validated", (unsigned long long)g_prof.ui_pre, (unsigned long long)g_prof.ui_layer, n ? f : " none",
             g_prof.ratio, g_prof.tol, g_prof.edge, g_prof.fill, g_prof.soft, g_prof.uisr, g_prof.uissupr, g_prof.uisinteg, g_prof.uispass, g_prof.uisguard, g_prof.cut_res, g_prof.cut_corr, g_prof.cut_mv);
    }
    return &g_prof;
}

// The launcher's live choice (the in-game panel): -1 none yet (DM_FG_MENU), 0 LSFG, 1 the motion-vector engine
static _Atomic int g_menu = -1;
__attribute__((visibility("default"))) void nxhost_framegen_engine(int mv) {
    if (atomic_exchange(&g_menu, mv ? 1 : 0) == (mv ? 1 : 0)) return;
    VLOG("frame generation: the launcher picks %s", mv ? "Native FG (the motion-vector engine)" : "LSFG");
    atomic_fetch_add(&vka_env_gen, 1);   // (every switch's call site reads again: vka_fgmv.c keys)
}
// Native FG on a game whose profile has no fg_mv row is LSFG (with the launcher's LSFG settings; off without Lossless.dll, vka_framegen.c): decided on the profile at
// each choice (start, a live switch), never per frame. DM_FG_ENGINE=mv runs the engine on any game
static _Atomic int g_fallback = -1;
int vka_fgprof_fallback(void) { return atomic_load(&g_fallback) > 0; }
int vka_fgprof_engine(int *from_env) {
    char b[32]; int m, fb;
    *from_env = 0;
    if (vka_env_str("DM_FG_ENGINE", b, sizeof b)) { *from_env = 1; atomic_store(&g_fallback, 0); return !strcmp(b, "mv"); }
    if ((m = atomic_load(&g_menu)) < 0) m = vka_env_str("DM_FG_MENU", b, sizeof b) && !strcmp(b, "mv");
    fb = m && !vka_fgprof_get()->mv;
    if (atomic_exchange(&g_fallback, fb) != fb && fb) VLOG("frame generation: Native FG is not available for this game (its profile has no fg_mv row): using LSFG with the launcher's LSFG settings");
    return m && !fb;
}
#endif
