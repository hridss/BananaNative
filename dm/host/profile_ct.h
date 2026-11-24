// SPDX-License-Identifier: MIT
// Compile-time flavour of the profile accessors (profile.h): over the generated game_profile.h of the build's game, which must be included before this header. The scalars and
// strings are the header's DM_GAME_* macros as they are; here are the defaults of the optional keys (a key the profile lacks), the tables as static arrays (a table sized by the
// header's count macro, folded to a constant), and DM_T_* / DM_N_* / DM_HAS_*. A table nobody uses costs nothing: the arrays are static const and unused
#pragma once
#include "profile.h"

#ifdef DM_GAME_NVN_API_MAJOR
#define DM_HAS_NVN_API 1
#else
#define DM_HAS_NVN_API 0
#define DM_GAME_NVN_API_MAJOR -1
#define DM_GAME_NVN_API_MINOR -1
#endif
#ifdef DM_GAME_SHARED_FONT_TTF
#define DM_HAS_SHARED_FONT_TTF 1
#else
#define DM_HAS_SHARED_FONT_TTF 0
#define DM_GAME_SHARED_FONT_TTF 0
#endif
// The core defaults of the frame generator's rows (vka_fgprof.c), for a profile without fg_* rows
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
#define DM_GAME_FG_TUNE_RATIO 0.05
#endif
#ifndef DM_GAME_FG_TUNE_TOL
#define DM_GAME_FG_TUNE_TOL 40
#endif
#ifndef DM_GAME_FG_TUNE_EDGE
#define DM_GAME_FG_TUNE_EDGE 1.15
#endif
#ifndef DM_GAME_FG_TUNE_FILL
#define DM_GAME_FG_TUNE_FILL 6
#endif
#ifndef DM_GAME_FG_TUNE_SOFT
#define DM_GAME_FG_TUNE_SOFT 1
#endif
#ifndef DM_GAME_FG_TUNE_UISR
#define DM_GAME_FG_TUNE_UISR 12
#endif
#ifndef DM_GAME_FG_TUNE_UISSUPR
#define DM_GAME_FG_TUNE_UISSUPR 1
#endif
#ifndef DM_GAME_FG_TUNE_UISINTEG
#define DM_GAME_FG_TUNE_UISINTEG 0
#endif
#ifndef DM_GAME_FG_TUNE_UISPASS
#define DM_GAME_FG_TUNE_UISPASS 1
#endif
#ifndef DM_GAME_FG_TUNE_UISGUARD
#define DM_GAME_FG_TUNE_UISGUARD 1
#endif
#ifndef DM_GAME_FG_TUNE_CUT_RES
#define DM_GAME_FG_TUNE_CUT_RES 12
#endif
#ifndef DM_GAME_FG_TUNE_CUT_CORR
#define DM_GAME_FG_TUNE_CUT_CORR 0.5
#endif
#ifndef DM_GAME_FG_TUNE_CUT_MV
#define DM_GAME_FG_TUNE_CUT_MV 0.220848056537
#endif

// A table macro the header lacks (a test fixture's partial stub game_profile.h): an empty table with the dummy row game.py gives an empty one
#ifndef DM_GAME_RES_LEVELS
#define DM_GAME_RES_LEVELS {{"", 0, 0, 0, {{0, 0}}}}
#define DM_GAME_NRES_LEVELS 0
#endif
#ifndef DM_GAME_RENDER_CFG
#define DM_GAME_RENDER_CFG {{"", 0, 0, 0, 0, 0, {{0, 0, 0, 0}}, 0, 0, 0, 0, 0}}
#define DM_GAME_NRENDER_CFG 0
#endif
#ifndef DM_GAME_RT_POOL
#define DM_GAME_RT_POOL {{"", "", 0, 0, 0, 0, {0}, {0}, {0}, 0}}
#define DM_GAME_NRT_POOL 0
#endif
#ifndef DM_GAME_WINDOW_FN
#define DM_GAME_WINDOW_FN {{"", 0, 0, 0, 0}}
#define DM_GAME_NWINDOW_FN 0
#endif
#ifndef DM_GAME_SHAPE_TABLE
#define DM_GAME_SHAPE_TABLE {{"", "", {0}}}
#define DM_GAME_NSHAPE_TABLE 0
#endif
#ifndef DM_GAME_DRS_CTL
#define DM_GAME_DRS_CTL {{"", 0, 0, 0, 0}}
#define DM_GAME_NDRS_CTL 0
#endif
#ifndef DM_GAME_FPS_STEPS
#define DM_GAME_FPS_STEPS {{"", 0, 0, 0, 0, {0}}}
#define DM_GAME_NFPS_STEPS 0
#endif
#ifndef DM_GAME_FPS_HALF_DRAW
#define DM_GAME_FPS_HALF_DRAW {{"", "", 0, 0}}
#define DM_GAME_NFPS_HALF_DRAW 0
#endif
#ifndef DM_GAME_MESH_PREPASS_CUT
#define DM_GAME_MESH_PREPASS_CUT {{0, 0, 0, 0}}
#define DM_GAME_NMESH_PREPASS_CUT 0
#endif
#ifndef DM_GAME_MESH_VEGETATION_CUT
#define DM_GAME_MESH_VEGETATION_CUT {0}
#define DM_GAME_NMESH_VEGETATION_CUT 0
#endif
#ifndef DM_GAME_SUBGROUP_ONE_LANE
#define DM_GAME_SUBGROUP_ONE_LANE {0}
#define DM_GAME_NSUBGROUP_ONE_LANE 0
#endif
#ifndef DM_GAME_DISPLAY_VERSIONS
#define DM_GAME_DISPLAY_VERSIONS
#endif
#ifndef DM_GAME_MODULES
#define DM_GAME_MODULES {"main"}
#ifndef DM_GAME_NMOD
#define DM_GAME_NMOD 1
#endif
#endif
#ifndef DM_GAME_RENDER_QUALITY
#define DM_GAME_RENDER_QUALITY {0, 0, 0, 0}
#endif

#define DM_CT_TAB_(f, TYPE, MACRO) static const TYPE dm_ct_##f[] __attribute__((unused)) = MACRO;
DM_CT_TAB_(res_levels, dm_res_levels_row_t, DM_GAME_RES_LEVELS)
DM_CT_TAB_(render_cfg, dm_render_cfg_row_t, DM_GAME_RENDER_CFG)
DM_CT_TAB_(rt_pool, dm_rt_pool_row_t, DM_GAME_RT_POOL)
DM_CT_TAB_(window_fn, dm_window_fn_row_t, DM_GAME_WINDOW_FN)
DM_CT_TAB_(shape_table, dm_shape_table_row_t, DM_GAME_SHAPE_TABLE)
DM_CT_TAB_(drs_ctl, dm_drs_ctl_row_t, DM_GAME_DRS_CTL)
DM_CT_TAB_(fps_steps, dm_fps_steps_row_t, DM_GAME_FPS_STEPS)
DM_CT_TAB_(fps_half_draw, dm_fps_half_draw_row_t, DM_GAME_FPS_HALF_DRAW)
DM_CT_TAB_(mesh_prepass_cut, dm_mesh_prepass_cut_row_t, DM_GAME_MESH_PREPASS_CUT)
DM_CT_TAB_(mesh_vegetation_cut, dm_mesh_vegetation_cut_row_t, DM_GAME_MESH_VEGETATION_CUT)
DM_CT_TAB_(subgroup_one_lane, dm_subgroup_one_lane_row_t, DM_GAME_SUBGROUP_ONE_LANE)
#ifdef DM_GAME_UE_SET   // emitted only for a profile with ue_set rows: without, one dummy row and the count 0 (DM_GAME_UE_SET stays undefined)
#define DM_HAS_UE_SET 1
DM_CT_TAB_(ue_set, dm_ue_set_row_t, DM_GAME_UE_SET)
#define DM_N_UE_SET ((int)(sizeof dm_ct_ue_set / sizeof dm_ct_ue_set[0]))
#else
#define DM_HAS_UE_SET 0
static const dm_ue_set_row_t dm_ct_ue_set[] __attribute__((unused)) = {{"", "", "", 0}};
#define DM_N_UE_SET 0
#endif
// the display versions game.py can emit empty (nothing after the macro): a first dummy row keeps the array non-empty, the table starts after it
static const dm_display_versions_row_t dm_ct_display_versions[] __attribute__((unused)) = {{"", ""}, DM_GAME_DISPLAY_VERSIONS};
DM_CT_TAB_(modules, dm_modules_row_t, DM_GAME_MODULES)   // never empty (main at least)
static const int dm_ct_render_quality[4] __attribute__((unused)) = DM_GAME_RENDER_QUALITY;

#define DM_T_RES_LEVELS dm_ct_res_levels
#define DM_N_RES_LEVELS DM_GAME_NRES_LEVELS
#define DM_T_RENDER_CFG dm_ct_render_cfg
#define DM_N_RENDER_CFG DM_GAME_NRENDER_CFG
#define DM_T_RT_POOL dm_ct_rt_pool
#define DM_N_RT_POOL DM_GAME_NRT_POOL
#define DM_T_WINDOW_FN dm_ct_window_fn
#define DM_N_WINDOW_FN DM_GAME_NWINDOW_FN
#define DM_T_SHAPE_TABLE dm_ct_shape_table
#define DM_N_SHAPE_TABLE DM_GAME_NSHAPE_TABLE
#define DM_T_DRS_CTL dm_ct_drs_ctl
#define DM_N_DRS_CTL DM_GAME_NDRS_CTL
#define DM_T_FPS_STEPS dm_ct_fps_steps
#define DM_N_FPS_STEPS DM_GAME_NFPS_STEPS
#define DM_T_FPS_HALF_DRAW dm_ct_fps_half_draw
#define DM_N_FPS_HALF_DRAW DM_GAME_NFPS_HALF_DRAW
#define DM_T_MESH_PREPASS_CUT dm_ct_mesh_prepass_cut
#define DM_N_MESH_PREPASS_CUT DM_GAME_NMESH_PREPASS_CUT
#define DM_T_MESH_VEGETATION_CUT dm_ct_mesh_vegetation_cut
#define DM_N_MESH_VEGETATION_CUT DM_GAME_NMESH_VEGETATION_CUT
#define DM_T_SUBGROUP_ONE_LANE dm_ct_subgroup_one_lane
#define DM_N_SUBGROUP_ONE_LANE DM_GAME_NSUBGROUP_ONE_LANE
#define DM_T_UE_SET dm_ct_ue_set
#define DM_T_DISPLAY_VERSIONS (dm_ct_display_versions + 1)
#define DM_N_DISPLAY_VERSIONS ((int)(sizeof dm_ct_display_versions / sizeof dm_ct_display_versions[0]) - 1)
#define DM_HAS_DISPLAY_VERSIONS (DM_N_DISPLAY_VERSIONS > 0)
#define DM_T_MODULES dm_ct_modules
#define DM_N_MODULES DM_GAME_NMOD
#define DM_T_RENDER_QUALITY dm_ct_render_quality
