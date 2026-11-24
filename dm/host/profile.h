// SPDX-License-Identifier: MIT
// The game profile as data: struct dm_profile holds what game.py's generated game_profile.h holds as DM_GAME_* macros (dm/games/game.py header), one field a macro.
// Two ways to reach it, chosen by dm.h (-DNX_COMMON_PROFILE):
//   profile_ct.h  compile-time (the per-game build, Mac, iOS, unit tests): the DM_GAME_* macros of the generated header stay, DM_T_<TABLE> / DM_N_<TABLE> / DM_HAS_<KEY> sit over them
//   profile_rt.h  common build: the same names over the global `dm_game`, which the game's profile library registers through nxhost_set_profile (android/profile/profile.c)
// Both flavours define the same accessors, so code written against them builds in either. Tables: DM_T_<NAME> (pointer to the first row) and DM_N_<NAME> (row count), for RES_LEVELS,
// RENDER_CFG, RT_POOL, WINDOW_FN, SHAPE_TABLE, DRS_CTL, FPS_STEPS, FPS_HALF_DRAW, MESH_PREPASS_CUT, MESH_VEGETATION_CUT, SUBGROUP_ONE_LANE, UE_SET, DISPLAY_VERSIONS, MODULES (the
// rows' type is dm_<name>_row_t; a table without a row has count 0 and a valid pointer to a dummy row), plus DM_T_RENDER_QUALITY (int[4]). Presence flags DM_HAS_NVN_API,
// DM_HAS_SHARED_FONT_TTF, DM_HAS_UE_SET, DM_HAS_DISPLAY_VERSIONS: 0 or 1, plain C `if`; in `#if` only DM_HAS_NVN_API, _SHARED_FONT_TTF and _UE_SET and only in the compile-time flavour.
// Not usable in a static initialiser in the common build: the values are read at run time
#pragma once
#include <stdint.h>
#include "render_cfg.h"   // rcfg_row_t (render_cfg rows)
#include "rt_pool.h"      // rtpool_row_t (rt_pool rows)

// Row types: the field order is what game.py emits for each table
typedef struct { const char *build; uint64_t table; unsigned n, nl; struct { uint64_t off; unsigned len; } l[12]; } dm_res_levels_row_t;   // main build, size table offset, levels, lists, (offset, length) per list
typedef rcfg_row_t dm_render_cfg_row_t;
typedef rtpool_row_t dm_rt_pool_row_t;
typedef struct { const char *build; uint64_t wfn, hfn; unsigned w, h; } dm_window_fn_row_t;
typedef struct { const char *build, *variant; uint64_t off[7]; } dm_shape_table_row_t;
typedef struct { const char *build; uint64_t params, pick, budget, preset; } dm_drs_ctl_row_t;
typedef struct { const char *build; uint64_t ptr_off; uint32_t mode, count; uint8_t nderef; uint16_t deref[3]; } dm_fps_steps_row_t;
typedef struct { const char *module, *build; uint64_t off; uint8_t mask; } dm_fps_half_draw_row_t;
typedef uint64_t dm_mesh_prepass_cut_row_t[4];   // prepass key, colour pass key, location map (nibbles), has map
typedef uint64_t dm_mesh_vegetation_cut_row_t;   // one mesh shader key (the table lists the keys of all pairs in order; its count is keys, not pairs)
typedef uint64_t dm_subgroup_one_lane_row_t;     // one shader key
typedef struct { const char *name, *value, *env; int ref; } dm_ue_set_row_t;
typedef struct { const char *build, *ver; } dm_display_versions_row_t;
typedef const char *dm_modules_row_t;

// The scalars and strings, by name: X(type, field, KEY) is the macro DM_GAME_<KEY> (the generated header's, in the compile-time flavour). The optional keys of game.py (NVN_API, SHARED_FONT_TTF,
// the fg_* rows) have the defaults profile_ct.h gives them when the profile has no such row
#define DM_PROFILE_STRINGS(X) \
    X(id, ID) X(name, NAME) X(romfs_marker, ROMFS_MARKER) X(args, ARGS) X(env, ENV) X(fastboot_env, FASTBOOT_ENV) X(devtools, DEVTOOLS) X(ue_fps_cvar, UE_FPS_CVAR) \
    X(languages, LANGUAGES) X(tex_copy_staging_build, TEX_COPY_STAGING_BUILD) X(romfs_subst, ROMFS_SUBST) X(turnip_env, TURNIP_ENV) X(vrs_passes, VRS_PASSES) \
    X(render_default_out, RENDER_DEFAULT_OUT)
#define DM_PROFILE_NUMS(X) \
    X(uint64_t, program_id, PROGRAM_ID) X(uint64_t, mem_reserve, MEM_RESERVE) X(uint64_t, mem_alias, MEM_ALIAS) X(uint64_t, mem_stack, MEM_STACK) X(uint64_t, app_pool, APP_POOL) \
    X(uint64_t, mem_used, MEM_USED) X(uint64_t, sys_resource, SYS_RESOURCE) X(uint64_t, stdalloc_cap, STDALLOC_CAP) X(uint64_t, shader_key_skip_off, SHADER_KEY_SKIP_OFF) \
    X(uint64_t, shader_key_skip_len, SHADER_KEY_SKIP_LEN) X(uint64_t, fg_ui_pre, FG_UI_PRE) X(uint64_t, fg_ui_layer, FG_UI_LAYER) X(float, ue_lod_scale, UE_LOD_SCALE) \
    X(int, mem_map_limit, MEM_MAP_LIMIT) X(int, present_display, PRESENT_DISPLAY) X(int, core_preempt, CORE_PREEMPT) X(int, alloc_big_align, ALLOC_BIG_ALIGN) \
    X(int, fps_target, FPS_TARGET) X(int, fps_hold, FPS_HOLD) X(int, fps_limit, FPS_LIMIT) X(int, ue_texture_pool, UE_TEXTURE_POOL) X(int, present_snap, PRESENT_SNAP) \
    X(int, pso_mtl4, PSO_MTL4) X(int, enc_thread, ENC_THREAD) X(int, pad_layout, PAD_LAYOUT) X(int, opmode, OPMODE) X(int, perf_boost, PERF_BOOST) \
    X(int, tex_flush_upload, TEX_FLUSH_UPLOAD) X(int, tex_copy_staging_pc, TEX_COPY_STAGING_PC) X(int, render_cond, RENDER_COND) X(int, tex_dirty_upload, TEX_DIRTY_UPLOAD) \
    X(int, tex_dyn_slots, TEX_DYN_SLOTS) X(int, tex_soft, TEX_SOFT) X(int, ram_saver_soft, RAM_SAVER_SOFT) X(int, occlusion_split_pass, OCCLUSION_SPLIT_PASS) \
    X(int, nonuniform_texture_arrays, NONUNIFORM_TEXTURE_ARRAYS) X(int, event_wait, EVENT_WAIT) X(int, gpu_ts_segment, GPU_TS_SEGMENT) X(int, draw_barriers_game, DRAW_BARRIERS_GAME) \
    X(int, vconv_range, VCONV_RANGE) X(int, av1_dav1d, AV1_DAV1D) X(int, place_coalesce_pct, PLACE_COALESCE_PCT) X(int, low_ram_place_part_mb, LOW_RAM_PLACE_PART_MB) \
    X(int, low_ram_img_cache_max_mb, LOW_RAM_IMG_CACHE_MAX_MB) X(int, low_ram_app_pool_mb, LOW_RAM_APP_POOL_MB) X(int, shared_font_ttf, SHARED_FONT_TTF) \
    X(int, nvn_api_major, NVN_API_MAJOR) X(int, nvn_api_minor, NVN_API_MINOR) X(int, fg_mv, FG_MV) X(unsigned, fg_features, FG_FEATURES) \
    X(double, fg_tune_ratio, FG_TUNE_RATIO) X(double, fg_tune_tol, FG_TUNE_TOL) X(double, fg_tune_edge, FG_TUNE_EDGE) X(double, fg_tune_cut_res, FG_TUNE_CUT_RES) \
    X(double, fg_tune_cut_corr, FG_TUNE_CUT_CORR) X(double, fg_tune_cut_mv, FG_TUNE_CUT_MV) X(int, fg_tune_fill, FG_TUNE_FILL) X(int, fg_tune_soft, FG_TUNE_SOFT) \
    X(int, fg_tune_uisr, FG_TUNE_UISR) X(int, fg_tune_uissupr, FG_TUNE_UISSUPR) X(int, fg_tune_uisinteg, FG_TUNE_UISINTEG) X(int, fg_tune_uispass, FG_TUNE_UISPASS) \
    X(int, fg_tune_uisguard, FG_TUNE_UISGUARD)
// The tables: X(name, NAME): a pointer to the rows (dm_<name>_row_t) and the count, n_<name>
#define DM_PROFILE_TABLES(X) \
    X(res_levels, RES_LEVELS) X(render_cfg, RENDER_CFG) X(rt_pool, RT_POOL) X(window_fn, WINDOW_FN) X(shape_table, SHAPE_TABLE) X(drs_ctl, DRS_CTL) X(fps_steps, FPS_STEPS) \
    X(fps_half_draw, FPS_HALF_DRAW) X(mesh_prepass_cut, MESH_PREPASS_CUT) X(mesh_vegetation_cut, MESH_VEGETATION_CUT) X(subgroup_one_lane, SUBGROUP_ONE_LANE) X(ue_set, UE_SET) \
    X(display_versions, DISPLAY_VERSIONS) X(modules, MODULES)

struct dm_profile {
#define DM_PROFILE_F_STR(f, K) const char *f;
#define DM_PROFILE_F_NUM(t, f, K) t f;
#define DM_PROFILE_F_TAB(f, K) const dm_##f##_row_t *f; int n_##f;
    DM_PROFILE_STRINGS(DM_PROFILE_F_STR)
    DM_PROFILE_NUMS(DM_PROFILE_F_NUM)
    DM_PROFILE_TABLES(DM_PROFILE_F_TAB)
#undef DM_PROFILE_F_STR
#undef DM_PROFILE_F_NUM
#undef DM_PROFILE_F_TAB
    int render_quality[4];     // DM_GAME_RENDER_QUALITY: performance mode and smallest level of Quality, then of Performance
    int has_nvn_api;           // the profile has a device_api row (nvn_api_major / _minor are -1 without)
    int has_shared_font_ttf;   // the profile has a shared_font row (shared_font_ttf is 0 without)
};

// The profile of the running game. Compile-time flavour: only the game's profile library (android/profile/profile.c) defines it. Common build: the host's copy
extern const struct dm_profile dm_game;
// Called once by the game's profile library when it loads (a constructor), before any use of the profile; the common host keeps the copy
void nxhost_set_profile(const struct dm_profile *p);
