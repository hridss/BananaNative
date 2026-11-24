// The profile as data equals the generated header: android/profile/profile.c (linked in) fills struct dm_profile from the game's DM_GAME_* macros and registers it through
// nxhost_set_profile at load. Every field and every table row must equal the macro or table of game_profile.h (profile_ct.h's DM_T_* / DM_N_* / DM_HAS_*), and the common build's
// accessors (profile_rt.h, compiled as the second translation unit of this test with -DT_RT_TU, over the same dm_game) must give back the same fields.
// Build: t_profile_equiv.c (compile-time side, below) + t_profile_equiv.c -DT_RT_TU + ../../../android/profile/profile.c, with the game's profile header (make GAME=<id>)
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef T_RT_TU
// Run-time side: no game_profile.h here. Copy every value out through the accessor names
#include "../profile_rt.h"
void t_rt_fill(struct dm_profile *o, int has[4]) {
    memset(o, 0, sizeof *o);
#define F_STR(f, K) o->f = DM_GAME_##K;
#define F_NUM(t, f, K) o->f = DM_GAME_##K;
#define F_TAB(f, K) o->f = DM_T_##K; o->n_##f = DM_N_##K;
    DM_PROFILE_STRINGS(F_STR)
    DM_PROFILE_NUMS(F_NUM)
    DM_PROFILE_TABLES(F_TAB)
    for (int i = 0; i < 4; i++) o->render_quality[i] = DM_T_RENDER_QUALITY[i];
    o->has_nvn_api = DM_HAS_NVN_API; o->has_shared_font_ttf = DM_HAS_SHARED_FONT_TTF;
    has[0] = DM_HAS_NVN_API; has[1] = DM_HAS_SHARED_FONT_TTF; has[2] = DM_HAS_UE_SET; has[3] = DM_HAS_DISPLAY_VERSIONS;
    (void)DM_GAME_NMOD;
}
#else
#include "game_profile.h"
#include "../profile_ct.h"

static const struct dm_profile *g_reg;
static int g_nreg, g_fail;
void nxhost_set_profile(const struct dm_profile *p) { g_reg = p; g_nreg++; }
void nxhost_set_prewarm_seed(const unsigned char *p, size_t n) { (void)p; (void)n; }   // profile.c's constructor
void t_rt_fill(struct dm_profile *o, int has[4]);

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static void ser_res_levels(char *b, size_t n, const dm_res_levels_row_t *r) {
    int k = snprintf(b, n, "%s %llx %u %u", r->build, (unsigned long long)r->table, r->n, r->nl);
    for (int i = 0; i < 12; i++) k += snprintf(b + k, n - (size_t)k, " %llx:%u", (unsigned long long)r->l[i].off, r->l[i].len);
}
static void ser_render_cfg(char *b, size_t n, const dm_render_cfg_row_t *r) {
    int k = snprintf(b, n, "%s %llx %u %u %u %u", r->build, (unsigned long long)r->table, r->n, r->stride, r->out_off, r->ren_off);
    for (int i = 0; i < RCFG_MAX; i++) k += snprintf(b + k, n - (size_t)k, " %u,%u,%u,%u", r->sz[i][0], r->sz[i][1], r->sz[i][2], r->sz[i][3]);
    snprintf(b + k, n - (size_t)k, " %u %u %u %u %u", r->orig_w, r->orig_h, r->max_w, r->max_h, r->min_h);
}
static void ser_rt_pool(char *b, size_t n, const dm_rt_pool_row_t *r) {
    int k = snprintf(b, n, "%s %s %llx %x %u %u", r->build, r->module, (unsigned long long)r->off, r->word, r->base_mb, r->nt);
    for (int i = 0; i < RTP_TIERS; i++) k += snprintf(b + k, n - (size_t)k, " %u,%u,%u", r->mb[i], r->max_w[i], r->max_h[i]);
    snprintf(b + k, n - (size_t)k, " %u", r->out_h);
}
static void ser_window_fn(char *b, size_t n, const dm_window_fn_row_t *r) { snprintf(b, n, "%s %llx %llx %u %u", r->build, (unsigned long long)r->wfn, (unsigned long long)r->hfn, r->w, r->h); }
static void ser_shape_table(char *b, size_t n, const dm_shape_table_row_t *r) {
    int k = snprintf(b, n, "%s %s", r->build, r->variant);
    for (int i = 0; i < 7; i++) k += snprintf(b + k, n - (size_t)k, " %llx", (unsigned long long)r->off[i]);
}
static void ser_drs_ctl(char *b, size_t n, const dm_drs_ctl_row_t *r) {
    snprintf(b, n, "%s %llx %llx %llx %llx", r->build, (unsigned long long)r->params, (unsigned long long)r->pick, (unsigned long long)r->budget, (unsigned long long)r->preset);
}
static void ser_fps_steps(char *b, size_t n, const dm_fps_steps_row_t *r) {
    snprintf(b, n, "%s %llx %x %x %u %x %x %x", r->build, (unsigned long long)r->ptr_off, r->mode, r->count, r->nderef, r->deref[0], r->deref[1], r->deref[2]);
}
static void ser_fps_half_draw(char *b, size_t n, const dm_fps_half_draw_row_t *r) { snprintf(b, n, "%s %s %llx %x", r->module, r->build, (unsigned long long)r->off, r->mask); }
static void ser_mesh_prepass_cut(char *b, size_t n, const dm_mesh_prepass_cut_row_t *rp) { const uint64_t *r = *rp; snprintf(b, n, "%llx %llx %llx %llx", (unsigned long long)r[0], (unsigned long long)r[1], (unsigned long long)r[2], (unsigned long long)r[3]); }
static void ser_mesh_vegetation_cut(char *b, size_t n, const dm_mesh_vegetation_cut_row_t *r) { snprintf(b, n, "%llx", (unsigned long long)*r); }
static void ser_subgroup_one_lane(char *b, size_t n, const dm_subgroup_one_lane_row_t *r) { snprintf(b, n, "%llx", (unsigned long long)*r); }
static void ser_ue_set(char *b, size_t n, const dm_ue_set_row_t *r) { snprintf(b, n, "%s %s %s %d", r->name, r->value, r->env, r->ref); }
static void ser_display_versions(char *b, size_t n, const dm_display_versions_row_t *r) { snprintf(b, n, "%s %s", r->build, r->ver); }
static void ser_modules(char *b, size_t n, const dm_modules_row_t *r) { snprintf(b, n, "%s", *r); }

int main(void) {
    CHECK(g_nreg == 1);               // the constructor registered once ...
    CHECK(g_reg == &dm_game);         // ... the library's own struct
    const struct dm_profile *P = g_reg;
    if (!P) return 1;
#define C_STR(f, K) CHECK(P->f && !strcmp(P->f, DM_GAME_##K));
#define C_NUM(t, f, K) CHECK(P->f == (t)DM_GAME_##K);
    DM_PROFILE_STRINGS(C_STR)
    DM_PROFILE_NUMS(C_NUM)
    // tables: the count and every row (as text: the row types hold strings)
#define C_TAB(f, K) do { \
        CHECK(P->n_##f == (int)DM_N_##K); CHECK(P->f != NULL); \
        for (int i = 0; i < P->n_##f && i < (int)DM_N_##K; i++) { char a[1024], b[1024]; ser_##f(a, sizeof a, &P->f[i]); ser_##f(b, sizeof b, &DM_T_##K[i]); \
            if (strcmp(a, b)) { printf("FAIL table %s row %d: '%s' vs '%s'\n", #f, i, a, b); g_fail++; } } \
        /* the row after the last (the dummy row of an empty table, or the end) is never read: the pointer stays inside its array */ \
    } while (0);
    DM_PROFILE_TABLES(C_TAB)
    CHECK(P->n_modules == DM_GAME_NMOD);
    for (int i = 0; i < 4; i++) CHECK(P->render_quality[i] == DM_T_RENDER_QUALITY[i]);
    CHECK(P->has_nvn_api == DM_HAS_NVN_API && P->has_shared_font_ttf == DM_HAS_SHARED_FONT_TTF);
    CHECK(DM_HAS_UE_SET == (P->n_ue_set > 0) && DM_HAS_DISPLAY_VERSIONS == (P->n_display_versions > 0));
    CHECK(DM_HAS_NVN_API == (P->nvn_api_major >= 0) && (DM_HAS_NVN_API || (P->nvn_api_major == -1 && P->nvn_api_minor == -1)));
    CHECK(DM_HAS_SHARED_FONT_TTF || P->shared_font_ttf == 0);
#ifdef DM_GAME_UE_SET
    CHECK(DM_HAS_UE_SET == 1 && P->n_ue_set > 0);
#else
    CHECK(DM_HAS_UE_SET == 0 && P->n_ue_set == 0);
#endif
    // the common build's accessors over dm_game give back the same values
    struct dm_profile o; int has[4];
    t_rt_fill(&o, has);
#define R_STR(f, K) CHECK(o.f == P->f);
#define R_NUM(t, f, K) CHECK(o.f == P->f);
#define R_TAB(f, K) CHECK(o.f == P->f && o.n_##f == P->n_##f);
    DM_PROFILE_STRINGS(R_STR)
    DM_PROFILE_NUMS(R_NUM)
    DM_PROFILE_TABLES(R_TAB)
    CHECK(!memcmp(o.render_quality, P->render_quality, sizeof o.render_quality));
    CHECK(o.has_nvn_api == P->has_nvn_api && o.has_shared_font_ttf == P->has_shared_font_ttf);
    CHECK(has[0] == DM_HAS_NVN_API && has[1] == DM_HAS_SHARED_FONT_TTF && has[2] == DM_HAS_UE_SET && has[3] == DM_HAS_DISPLAY_VERSIONS);
    if (g_fail) { printf("t_profile_equiv: %d failures (%s)\n", g_fail, DM_GAME_ID); return 1; }
    printf("t_profile_equiv ok (%s)\n", DM_GAME_ID);
    return 0;
}
#endif
