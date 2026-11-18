// Unreal console variables found by their registered names: the texture streaming pool the profile's ue_texture_pool raises, the dynamic resolution's floor DM_UE_DYNRES_MIN sets,
// the static meshes' LOD distance scale the profile's ue_lod_scale raises, and the profile's ue_set rows.
// The rewriter finds each TAutoConsoleVariable the host may write in main (dm/tools/ue_cvar.py: its static constructor's registration, in any Unreal 4 / 5 build) and names it in main's
// fixups (UE_CVAR:<name>); the object holds at +0x10 the pointer to the variable's value, the game thread's copy, then the render thread's. Called on every present (hle_nvn.c)
#include "hle.h"

static int rd(uint64_t a, void *out, size_t n) { return a >= 0x1000 && !plat_mem_read(a, out, n); }

// main's offset of the TAutoConsoleVariable registered under `name`, 0 if the rewriter found none (not an Unreal main, not one of ue_cvar.py's NAMES, or registered at two objects)
uint64_t dm_ue_cvar(const char *name) {
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main"))
        for (int k = 0; k < g_mod[i].n_ue_cvar; k++) if (!strcmp(g_mod[i].ue_cvar[k].name, name)) return g_mod[i].ue_cvar[k].off;
    return 0;
}

// ue_texture_pool: r.Streaming.PoolSize, the megabytes Unreal's texture streamer keeps resident; past it the streamer drops the top mips of the textures in view. The game's
// device profile sets it (DRAGON QUEST VII Reimagined: 200 on the Switch, 1000 on Windows) and the streaming manager reads it again whenever it changes. The host raises a smaller
// value the game sets to the target, whenever the game sets it back; a larger one, or -1 (no pool size: the engine sizes its pool otherwise), stays. The profile key applies on the
// Mac; on iOS only DM_UE_TEXTURE_POOL turns it on (Documents/env.txt), until an iPhone run shows the memory fits. DM_UE_TEXTURE_POOL=0 leaves the game's value
void dm_ue_cvar_tick(void) {
    static int state = -1, target, nlog; static uint64_t obj_off; static const char *src;
    if (!state) return;
    if (state < 0) {
        state = 0;
        const char *e = getenv("DM_UE_TEXTURE_POOL");
        target = e ? atoi(e) : PLAT_IOS ? 0 : DM_GAME_UE_TEXTURE_POOL; src = e ? "DM_UE_TEXTURE_POOL" : "game profile ue_texture_pool";
        if (target <= 0) { if (DM_GAME_UE_TEXTURE_POOL) LOG("main: Unreal r.Streaming.PoolSize left as the game sets it (%s)", e ? src : "iOS without DM_UE_TEXTURE_POOL"); return; }
        obj_off = dm_ue_cvar("r.Streaming.PoolSize");
        if (!obj_off) { LOG("[RED] main: Unreal r.Streaming.PoolSize not found in main by the rewriter (ue_cvar.py; %s %d MB not applied)", src, target); return; }
        LOG("main: Unreal r.Streaming.PoolSize at main+%#llx, raised to %d MB (%s)", (unsigned long long)obj_off, target, src);
        state = 1;
    }
    uint8_t *base = NULL;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) base = g_mod[i].base;
    uint64_t val;
    if (!base || !rd((uint64_t)(uintptr_t)(base + obj_off + 0x10), &val, sizeof val) || !val) return;   // not created yet
    for (int c = 0; c < 2; c++) {
        int32_t cur, *p = (int32_t *)(uintptr_t)(val + 4 * (uint64_t)c);
        if (!rd((uint64_t)(uintptr_t)p, &cur, sizeof cur) || cur <= 0 || cur >= target) continue;
        dm_guest_write_prepare(p, sizeof *p); __atomic_store_n(p, target, __ATOMIC_RELAXED);
        if (nlog < 8) { nlog++; LOG("main: Unreal r.Streaming.PoolSize (%s thread) %d → %d MB (%s)", c ? "render" : "game", cur, target, src); }
    }
}

// A float variable held at a target, both copies: dir < 0 only lowers it (a lower value the game sets stays), dir > 0 only raises it. The target and its source are
// read once (target <= 0: nothing done); written again whenever the game sets its own, as its device profile or settings do
typedef struct { const char *name, *src; float target; int dir, state, nlog; uint64_t off; } ue_float;
static void float_tick(ue_float *v) {
    if (!v->state) return;
    if (v->state < 0) {
        v->state = 0;
        if (!(v->target > 0)) return;
        if (!(v->off = dm_ue_cvar(v->name))) { LOG("[RED] main: Unreal %s not found in main by the rewriter (ue_cvar.py; %s %g not applied)", v->name, v->src, v->target); return; }
        LOG("main: Unreal %s at main+%#llx, set to %g (%s)", v->name, (unsigned long long)v->off, v->target, v->src);
        v->state = 1;
    }
    uint8_t *base = NULL;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) base = g_mod[i].base;
    uint64_t val;
    if (!base || !rd((uint64_t)(uintptr_t)(base + v->off + 0x10), &val, sizeof val) || !val) return;   // not created yet
    for (int c = 0; c < 2; c++) {
        float cur, *p = (float *)(uintptr_t)(val + 4 * (uint64_t)c);
        if (!rd((uint64_t)(uintptr_t)p, &cur, sizeof cur) || cur == v->target || (v->dir < 0 ? cur < v->target : cur > v->target)) continue;
        dm_guest_write_prepare(p, sizeof *p); *p = v->target;
        if (v->nlog < 8) { v->nlog++; LOG("main: Unreal %s (%s thread) %g → %g (%s)", v->name, c ? "render" : "game", cur, v->target, v->src); }
    }
}

// DM_UE_DYNRES_MIN=<percent>: r.DynamicRes.MinScreenPercentage, the lowest screen percentage Unreal's dynamic resolution may pick (it scales between it and
// r.DynamicRes.MaxScreenPercentage from the GPU time it reads; the game's device profile sets it at boot). Lowered to it whenever the game sets a higher one (Orbitals: 83.3,
// and 5 at times, which stays):
// at 50 its split-screen gameplay went from 12.0 to 13.0-13.6 fps on the AYN Thor, the engine picking the scale. Unset: the game's own
void dm_ue_dynres_tick(void) {
    static ue_float v = {"r.DynamicRes.MinScreenPercentage", "DM_UE_DYNRES_MIN", 0, -1, -1, 0, 0};
    if (v.state < 0) {
        const char *e = getenv("DM_UE_DYNRES_MIN"); float t = e && *e ? strtof(e, NULL) : 0;
        if (e && *e && !(t > 0 && t <= 100)) { LOG("[RED] main: DM_UE_DYNRES_MIN=%s is not a percentage in (0, 100]: not applied", e); t = 0; }
        v.target = t;
    }
    float_tick(&v);
}

// ue_lod_scale / DM_UE_LOD_SCALE=<scale>: r.StaticMeshLODDistanceScale, by which Unreal multiplies a static mesh's distance when it picks its LOD (above 1: the coarser
// LODs nearer), raised to it whenever the game sets a lower one. A geometry-bound frame sends fewer triangles: Orbitals' split-screen gameplay on the AYN Thor, 2 against
// the game's 1, 7.06 → 5.54 M primitives a frame, GPU 71.8 → 68.7 ms, screenshots within the noise of two at the same scale. The engine reads it every frame (written live,
// it took effect at once). DM_UE_LOD_SCALE overrides the profile, 0 leaves the game's
void dm_ue_lod_tick(void) {
    static ue_float v = {"r.StaticMeshLODDistanceScale", "game profile ue_lod_scale", 0, 1, -1, 0, 0};
    if (v.state < 0) {
        const char *e = getenv("DM_UE_LOD_SCALE"); float t = e && *e ? strtof(e, NULL) : DM_GAME_UE_LOD_SCALE;
        if (e && *e) v.src = "DM_UE_LOD_SCALE";
        if (e && *e && !(t >= 0 && t <= 16)) { LOG("[RED] main: DM_UE_LOD_SCALE=%s is not a scale in [0, 16]: not applied", e); t = 0; }
        v.target = t;
    }
    float_tick(&v);
}

// ue_set rows (game.tsv, game.py DM_GAME_UE_SET): a variable held at a value whenever the game sets another, both copies of a TAutoConsoleVariable or the global of one
// registered by reference (ue_cvar.py REFS: the row's ref). A row with an environment variable applies only while it is 1: the launcher's Game options toggles (Orbitals:
// "Particle light volume off" holds r.TranslucentLightingVolume at 0, GPU about 3.8 ms a frame less in its split-screen gameplay on the AYN Thor). A value with a decimal
// point is written as a float, else as an int. The toggles are read on every present, so a live option (the in-game panel's "env <switch>=0|1" lines, vka_ctl.c) applies
// from the next frame: of the rows on one variable the last one on is held, and with none on any more the variable gets back the value the game last set: the one before
// the first hold, or the one the game wrote over the hold since (a hold from the first frame came before the game's device profile: Orbitals' floor 67 % held from the
// start gave back the engine's start value, far under the game's 83.3, when turned off). The engine reads these variables every frame
typedef struct { const char *name, *value, *env; int ref; } ue_set_row;
typedef struct { int state, nlog, saved; const ue_set_row *held; uint32_t bits, orig[2], wrote[2]; uint64_t off; } ue_set_st;   // state: 0 not looked up yet, 1 found, -1 not found; wrote: what the hold last left in each copy
static int ue_set_on(const ue_set_row *r) { const char *e = *r->env ? getenv(r->env) : "1"; return e && !strcmp(e, "1"); }
// One variable: `on` is its row to hold (the last row on), NULL to give the game's value back
static void ue_set_apply(const ue_set_row *r0, const ue_set_row *on, ue_set_st *s) {
    if (s->state < 0 || (!on && !s->saved)) return;
    const ue_set_row *by = on ? on : s->held;
    const char *src = by && *by->env ? by->env : "game profile ue_set";
    if (!s->state) {
        s->state = -1;
        if (!(s->off = dm_ue_cvar(r0->name))) { LOG("[RED] main: Unreal %s not found in main by the rewriter (ue_cvar.py NAMES / REFS; %s %s not applied)", r0->name, src, on->value); return; }
        LOG("main: Unreal %s at main+%#llx (%s)", r0->name, (unsigned long long)s->off, r0->ref ? "a global registered by reference" : "both copies");
        s->state = 1;
    }
    if (on != s->held && on) {
        if (strchr(on->value, '.')) { float f = strtof(on->value, NULL); memcpy(&s->bits, &f, 4); } else { int32_t v = (int32_t)strtol(on->value, NULL, 10); memcpy(&s->bits, &v, 4); }
        LOG("main: Unreal %s held at %s (%s)", r0->name, on->value, src); s->nlog = 0;
    }
    s->held = on;
    uint8_t *base = NULL;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) base = g_mod[i].base;
    if (!base) return;
    uint64_t val = (uint64_t)(uintptr_t)(base + s->off);
    if (!r0->ref && (!rd((uint64_t)(uintptr_t)(base + s->off + 0x10), &val, sizeof val) || !val)) return;   // not created yet
    int n = r0->ref ? 1 : 2;
    uint32_t *p[2] = {(uint32_t *)(uintptr_t)val, (uint32_t *)(uintptr_t)(val + 4)};
    if (on && !s->saved) { for (int c = 0; c < n; c++) { if (!rd((uint64_t)(uintptr_t)p[c], &s->orig[c], 4)) return; s->wrote[c] = s->orig[c]; } s->saved = 1; }
    for (int c = 0; c < n; c++) {
        uint32_t cur;
        if (!rd((uint64_t)(uintptr_t)p[c], &cur, sizeof cur)) continue;
        if (cur != s->wrote[c]) s->orig[c] = cur;   // the game set its own value over the hold: the one to give back (or to leave, at the release)
        uint32_t want = on ? s->bits : s->orig[c];
        if (on) s->wrote[c] = want;
        if (cur == want) continue;
        dm_guest_write_prepare(p[c], sizeof *p[c]); __atomic_store_n(p[c], want, __ATOMIC_RELAXED);
        if (on && s->nlog < 4) { s->nlog++; LOG("main: Unreal %s%s → %s (%s)", r0->name, r0->ref ? "" : c ? " (render thread)" : " (game thread)", on->value, src); }
    }
    if (!on) { s->saved = 0; LOG("main: Unreal %s back to the game's value (%s off)", r0->name, src); }
}
void dm_ue_set_tick(void) {
#ifdef DM_GAME_UE_SET
    static const ue_set_row rows[] = DM_GAME_UE_SET;
    enum { N = sizeof rows / sizeof rows[0] };
    static ue_set_st st[N];
    for (size_t i = 0; i < N; i++) {
        size_t k = 0; while (strcmp(rows[k].name, rows[i].name)) k++;
        if (k != i) continue;   // a variable's rows are handled at its first
        const ue_set_row *on = NULL;
        for (size_t j = i; j < N; j++) if (!strcmp(rows[j].name, rows[i].name) && ue_set_on(&rows[j])) on = &rows[j];
        ue_set_apply(&rows[i], on, &st[i]);
    }
#endif
}
