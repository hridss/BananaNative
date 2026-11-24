// DRAGON BALL FighterZ (0100A250097F0000, main 1.33): the game renders at the picture's shape (Game settings › Aspect ratio: 21:9, 4:3, 10:9, 1:1 or Screen shape).
// Unreal Engine 4.19 sizes its window, its render targets, its cameras' aspect and its UI's anchors from GSystemResolution, which the r.SetRes sink sets
// (patches.py OBS_DBFZ_RES at main+0x1e63a34, right after the sink parsed "<w>x<h>"). The callback makes the width the shape times the height, rounded to a multiple
// of 8 (docked: 1920x1440 at 4:3, 2520x1080 at 21:9). The cameras (OBS_DBFZ_RES | 1 << 8, view_constrain): no letterbox; taller keeps the horizontal reach and sees
// more above and below, wider keeps the 16:9 height of view and sees more of the stage at the sides (its walls are the game's own). The UI (the vertex hook below):
// the battle's bottom HUD moves to the window's bottom edge (taller) or out to its side edges (wider), full-screen 16:9 pictures cover the window.
// The shape: DM_DBFZ_ASPECT=<w>:<h> or a ratio, else DM_VK_PICTURE_ASPECT (the launcher's Aspect ratio, Screen shape included); none or 16:9 leaves the game's size.
// Read once (the sink runs at start and at each operation mode change)
#include "hle.h"
#include "dm.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>

#define DBFZ_MAIN_BUILD "39adb54619956c8e93d3a37777f32ece"
static _Atomic int g_win_w, g_win_h;   // the game window's size as r.SetRes set it last (the UI hook moves only what is drawn into the window itself)

static double parse_shape(const char *e) {
    if (!e || !*e) return 0;
    double w = 0, h = 0;
    if (sscanf(e, "%lf:%lf", &w, &h) == 2 && w > 0 && h > 0) return w / h;
    w = atof(e); return w > 0 ? w : 0;
}
static double shape(void) {
    static double s = -1;
    if (s >= 0) return s;
    const char *src = "DM_DBFZ_ASPECT", *e = getenv(src);
    if (!e || !*e) { src = "DM_VK_PICTURE_ASPECT"; e = getenv(src); }
    s = parse_shape(e);
    if (s && (s < 0.4 || s > 4.0)) { LOG("[RED] dbfz aspect: %s=%s is out of range (0.4 to 4): the game's own size", src, e); s = 0; }
    if (s && fabs(s - 16.0 / 9.0) < 0.01) s = 0;
    if (s) LOG("dbfz aspect: the game renders at %.4f (%s=%s)", s, src, e);
    else LOG("dbfz aspect: the game's own 16:9 size (no picture shape)");
    return s;
}
// Param 1: FMinimalViewInfo::CalculateProjectionMatrixGivenView about to read bConstrainAspectRatio (bit 0 of [x20+0x2c]); x1 the aspect axis constraint (a byte:
// 0 MaintainYFOV, 1 MaintainXFOV, 2 MajorAxisFOV). At a picture shape the game's cameras ask for 16:9 with the constraint on, which letterboxes the view inside
// the window; cleared, the projection follows the window, and MaintainXFOV keeps the fighters' horizontal reach (DM_DBFZ_AXIS=0/1/2 picks another, -1 leaves it)
// DM_DBFZ_CAM_LOG=<n> (live, DM_CTL env): logs the next n views (FMinimalViewInfo: location +0, rotation +0xc, FOV +0x18, aspect ratio +0x28) with the frame number
extern uint64_t vka_frame(void);
static void cam_log(const uint8_t *vi, int axis) {
    static unsigned n; static char last[16]; static int left;
    if (!(n++ & 63)) { const char *e = getenv("DM_DBFZ_CAM_LOG"); if (!e) e = ""; if (strncmp(e, last, sizeof last - 1)) { snprintf(last, sizeof last, "%s", e); left = atoi(e); } }
    if (left <= 0) return;
    left--; const float *v = (const float *)vi;
    LOG("dbfz cam f%llu loc %.1f %.1f %.1f rot %.2f %.2f %.2f fov %.3f aspect %.4f axis %d", (unsigned long long)vka_frame(), v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[10], axis);
}
static void view_constrain(uint64_t *r) {
    uint8_t *vi = (uint8_t *)r[20];
    if (!shape()) { cam_log(vi, r[1] ? *(uint8_t *)r[1] : 255); return; }
    if (vi[0x2c] & 1) vi[0x2c] &= (uint8_t)~1;
    static int axis = -2; if (axis == -2) { const char *e = getenv("DM_DBFZ_AXIS"); axis = e && *e ? atoi(e) : 1; LOG("dbfz aspect: camera letterbox off, aspect axis %d (DM_DBFZ_AXIS)", axis); }
    if (axis >= 0 && axis <= 2 && r[1]) { uint8_t *ax = (uint8_t *)r[1]; if (*ax != axis) *ax = (uint8_t)axis; }
    cam_log(vi, r[1] ? *(uint8_t *)r[1] : 255);
    // Character select: its backdrop is a flat 16:9 card filling the fixed menu camera's view (loc 0 800 0, yaw -90, FOV 54) far behind the fighters, so another shape
    // sees black past the card (above and below it when taller, beside it when wider). Zooming until the card covers would cut the team's third fighter at the side
    // (taller) or make the fighters too big for the roster (wider), so the camera dollies: it backs off along its axis (FSceneViewProjectionData::ViewOrigin, x3) and
    // narrows its field of view by the same factor at the fighters' depth d1, which keeps the fighters' size and place while the card, at depth d2, grows until it
    // covers. The fighters keep a 1.15 zoom against 16:9 when taller (DM_DBFZ_COVER, live; the third fighter just inside), their 16:9 framing when wider. What the
    // dolly cannot cover (d2 too near) is left at the bottom: the view shifts up with the view info's off-centre projection offset (+0x550) until the card's top meets
    // the window's top. Depths measured at 21:9 (the card's width at 100, 200 and 300 back: d2 3644, 3646, 3700; the fighters' drift: 550 for a team's back member to
    // 720 for the front ones, d1 680 between). The back member then drifts at most 2 % of the width toward the middle. Live: DM_DBFZ_CS_D1 / _D2 (depths), _BACK
    // (a fixed dolly, for measuring), _SHIFT. The view info is a copy made for each projection, so its
    // fields are set afresh every time (the cam log shows the game's own values, never these)
    static unsigned cn; static float zoom = 1.15f, d1 = 680.0f, d2 = 3650.0f, back = -1.0f, shift = -1.0f;
    if (!(cn++ & 63)) { const char *e = getenv("DM_DBFZ_COVER"); zoom = e && *e ? strtof(e, NULL) : 1.15f;
                        e = getenv("DM_DBFZ_CS_D1"); d1 = e && *e ? strtof(e, NULL) : 680.0f; e = getenv("DM_DBFZ_CS_D2"); d2 = e && *e ? strtof(e, NULL) : 3650.0f;
                        e = getenv("DM_DBFZ_CS_BACK"); back = e && *e ? strtof(e, NULL) : -1.0f; e = getenv("DM_DBFZ_CS_SHIFT"); shift = e && *e ? strtof(e, NULL) : -1.0f; }
    float *v = (float *)vi, *off = (float *)(vi + 0x550);
    int w = g_win_w, h = g_win_h; double win = w > 0 && h > 0 ? (double)w / h : shape(), fw = win / (16.0 / 9.0);   // fw > 1: wider than 16:9
    int pose = v[0] == 0.0f && v[1] == 800.0f && v[2] == 0.0f && v[3] == 0.0f && v[4] == -90.0f && v[5] == 0.0f;
    if (pose && v[6] == 54.0f && win > 0 && fabs(fw - 1.0) > 0.005 && zoom >= 1.0f) {
        double z = fw < 1.0 ? zoom : 1.0 / fw, need = fw < 1.0 ? 1.0 / (fw * zoom) : fw;   // the fighters' zoom against 16:9; the card's growth that covers
        double dz = back >= 0 ? back : d2 > need * d1 * 1.001 ? d1 * d2 * (need - 1.0) / (d2 - need * d1) : 0.0;
        double got = dz > 0 && d2 > 0 ? (d1 + dz) * d2 / (d1 * (d2 + dz)) : 1.0;
        float *org = (float *)r[3];
        if (dz > 0 && org) org[1] += (float)dz;   // (yaw -90: the camera looks down -Y; backing off is +Y)
        double k = dz > 0 && org ? d1 / (d1 + dz) : 1.0;
        v[6] = (float)(2.0 * atan(tan(27.0 * M_PI / 180.0) / z * k) * 180.0 / M_PI);
        double half = fw < 1.0 ? fw * zoom * got : 1.0, sh = shift >= 0 ? shift : half < 1.0 ? 1.0 - half : 0.0;   // (taller: the card's half-height in NDC)
        if (sh > 0) off[1] = (float)-sh;
        static float said; if (said != (float)(fw + dz + sh)) { said = (float)(fw + dz + sh);
            LOG("dbfz aspect: character select camera at %.3f: fighters zoom %.3f, %.0f back (card %.3f of %.3f needed), FOV 54 → %.2f, shifted up %.3f (DM_DBFZ_COVER, DM_DBFZ_CS_*)", fw, z, dz, got, need, v[6], sh); }
    }
    // Wider than 16:9: the game's cameras keep their 16:9 height of view (MaintainXFOV with the horizontal FOV widened by the shape's factor), so the fight is framed as
    // at 16:9 and more of the stage shows at the sides; the walls and the camera's limits are the game's own
    else if (fw > 1.005 && !pose && v[6] > 1.0f && v[6] < 170.0f)
        v[6] = (float)(2.0 * atan(tan(v[6] * M_PI / 360.0) * fw) * 180.0 / M_PI);
}
// x29 = sp + 0x30 in the sink: the parsed width at [sp+0xc], the height at [sp+8]
static void ui_attach_once(void);
static void res_parsed(uint64_t *r, int param) {
    if (strcmp(dm_main_build(), DBFZ_MAIN_BUILD)) return;
    ui_attach_once();
    if (param == 1) { view_constrain(r); return; }
    double s = shape(); if (!s) return;
    int32_t *pw = (int32_t *)(r[29] - 0x24), *ph = (int32_t *)(r[29] - 0x28);
    int32_t w = *pw, h = *ph;
    if (w <= 0 || h <= 0 || fabs((double)w / h - 16.0 / 9.0) > 0.02) return;   // only the game's 16:9 sizes
    // Docked, the game renders 1440 rows instead of 1080 at a picture shape (1920x1440 at 4:3, downsampled to the screen): the Retroid Pocket Nova's Adreno 740 draws a
    // battle in 7-8 ms there (5 ms at 1440x1080), 60 fps held. DM_DBFZ_HEIGHT=<rows> picks another height (1080: the game's own)
    static int want_h = -1;
    if (want_h < 0) { const char *e = getenv("DM_DBFZ_HEIGHT"); want_h = e && *e ? atoi(e) : 1440; if (want_h < 360 || want_h > 2160) want_h = 1080;
                      LOG("dbfz aspect: docked height %d (DM_DBFZ_HEIGHT)", want_h); }
    if (want_h && h == 1080 && s < 16.0 / 9.0) { h = want_h; *ph = h; }   // (wider: the game's 1080 rows, already more pixels than its 1920x1080)
    int32_t nw = (int32_t)lround(h * s / 8.0) * 8;
    if (nw < 64) return;
    *pw = nw; g_win_w = nw; g_win_h = h;
    static int32_t last; if (last != nw * 4096 + h) { last = nw * 4096 + h; LOG("dbfz aspect: r.SetRes %dx%d → %dx%d", w, h, nw, h); }
}

// ---------- The UI at the picture's shape ----------
// (Measured at 1440x1080, 4:3 at the game's own height; the numbers scale with the window.) The game's UMG layout there is already uniform (0.75 of its 1080p sizes, x and y alike: the timer's digit 52x85 at 1920x1080, 38x63 here) and its top-anchored
// widgets sit at the window's top edge. Its bottom-anchored widgets (the meter bars, the assist and Ki counters) stop at the bottom of a centred 16:9 band (y 945 of 1080,
// the camera's 16:9 rect), leaving 135 rows below them. Slate draws them with a plain ortho of the window (2/1440, -2/1080; the vertex shaders below; FSlateVertex: 40 bytes,
// the position at +24). The hook (vka_draw.c vka_vertex_hook) moves each Slate element whose centre lies in the lower part of the window down by H(1 - f)/2, f = shape/(16/9)
// (135 at 4:3), before the GPU reads it: the bottom HUD then sits at the window's bottom edge as at 16:9. An element covering most of the height (backgrounds, fades) stays.
// An element is a run of triangles over a contiguous range of vertices (Slate writes each element's vertices and indices together: a box's 9-slice grid, a glyph).
// Slate rebuilds its vertices every frame; the y written last for an address is remembered, so a vertex is never moved twice (a batch drawn again, a buffer kept).
// DM_DBFZ_UI=0 (live) leaves the UI as the game draws it; DM_DBFZ_UI_LOG=<n> (live) logs the next n elements' boxes;
// DM_DBFZ_UI_VS adds vertex shader keys (hex, comma-separated)
extern void (*vka_vertex_hook)(uint64_t vs, uint64_t vb, uint32_t stride, uint64_t ib, int itype, int32_t bv, int32_t first, int32_t count, float vpw, float vph);
static uint64_t g_ui_vs[16] = {0x7856974beed8b7acull, 0x90e62c0c44b5e135ull}; static int g_ui_nvs = 2;
static int g_ui_match = 1;   // live DM_DBFZ_UI_MATCH: 1 boxes (a member's box last frame overlaps, or it touches an element moved this frame), 0 rows only (v18)
static int g_ui_mode = 1, g_ui_logleft;   // live (DM_CTL env): DM_DBFZ_UI 0 / 1, DM_DBFZ_UI_LOG=<n> logs the next n elements (their boxes before the move)
static void ui_live(void) {
    static unsigned n; static char last[16];
    if (n++ & 255) return;
    const char *e = getenv("DM_DBFZ_UI"); g_ui_mode = !(e && *e == '0');
    e = getenv("DM_DBFZ_UI_MATCH"); g_ui_match = !(e && *e == '0');
    e = getenv("DM_DBFZ_UI_LOG"); if (!e) e = "";
    if (strncmp(e, last, sizeof last - 1)) { snprintf(last, sizeof last, "%s", e); if (atoi(e) > 0) g_ui_logleft = atoi(e); }
}
enum { UI_SEEN = 1 << 16 };
static _Atomic uint64_t g_ui_seen[UI_SEEN];   // address >> 2 << 32 | the y bits written there
static int ui_is_slate(uint64_t vs) { for (int i = 0; i < g_ui_nvs; i++) if (g_ui_vs[i] == vs) return 1; return 0; }
static int ui_done(uint64_t a, uint32_t ybits) {
    uint64_t e = atomic_load_explicit(&g_ui_seen[(a >> 3) & (UI_SEEN - 1)], memory_order_relaxed);
    return (uint32_t)(e >> 32) == (uint32_t)(a >> 2) && (uint32_t)e == ybits;
}
static void ui_mark(uint64_t a, uint32_t ybits) { atomic_store_explicit(&g_ui_seen[(a >> 3) & (UI_SEEN - 1)], (uint64_t)(uint32_t)(a >> 2) << 32 | ybits, memory_order_relaxed); }
// The bottom group: the elements the game anchored at the bottom of its 16:9 band, found on the previous frame's boxes (the UI barely changes from one frame to the next;
// draws are recorded one by one, so the frame's own boxes are not all known when its first element is moved). Seeds: elements ending in the band's last rows (y1 >= ui_seed_y: 0.8 H at 4:3);
// the group takes in every element touching a member (within 6 pixels in x and in y) that is not taller than half the window (a centred 16:9 background would join
// everything). Only members move: a dialogue box with its name tab, the battle's meters with their counters; a block above a gap stays (the character select's portrait
// grid above its help bar, the reception's menu above its dialogue). Members are known by their rows (rounded), matched in the next frame; a seed moves on its own frame
enum { UI_NB = 4096, UI_SET = 4096, UI_RING = 16384 };
typedef struct { float x0, x1, y0, y1; } ui_box;
// Every element's box (before a move) goes into a ring with its pass's start time; the group is found on the distinct boxes of the last 50 ms, once a frame (8 ms
// apart at most). Not the previous pass alone: a screen can draw its UI over several vertex buffers, each starting at vertex 0, and the pass before may then be a
// couple of elements of another one (the reception's dialogue: its name tab and the first glyphs of its title stayed while the rest moved, v29)
static ui_box g_ring[UI_RING]; static uint64_t g_ring_t[UI_RING]; static uint32_t g_ring_n; static uint64_t g_pass_t, g_clu_t, g_box_frame;
static uint64_t ui_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
static void ui_box_add(const ui_box *e) { g_ring[g_ring_n & (UI_RING - 1)] = *e; g_ring_t[g_ring_n & (UI_RING - 1)] = g_pass_t; g_ring_n++; }
static uint32_t g_member[UI_SET];
static ui_box g_mbox[UI_NB], g_cbox[UI_NB]; static int g_nmbox, g_ncbox; static float g_mtop;
static signed char g_mside[UI_NB], g_cside[UI_NB];   // wider than 16:9: the side a member's cluster moves to (-1 left, 1 right, 0 stays: one that spans the middle)   // the previous frame's members, this frame's moved elements (boxes before the move)
static int g_full_seen;   // this frame drew an element covering the whole window already: a later 16:9 picture is an overlay (the title's logo), not a background   // hashes of the previous frame's members (0: empty)
extern uint64_t vka_frame(void);
static uint32_t ui_key(const ui_box *b) { uint32_t k = (uint32_t)lroundf(b->y0 * 0.5f) * 2654435761u ^ (uint32_t)lroundf(b->y1 * 0.5f) * 40503u; return k ? k : 1; }   // rows only: a meter's fill changes its width every frame
static void set_add(uint32_t k) { for (uint32_t i = k & (UI_SET - 1), n = 0; n < UI_SET; i = (i + 1) & (UI_SET - 1), n++) { if (!g_member[i]) { g_member[i] = k; return; } if (g_member[i] == k) return; } }
static int set_has(uint32_t k) { for (uint32_t i = k & (UI_SET - 1), n = 0; n < UI_SET; i = (i + 1) & (UI_SET - 1), n++) { if (!g_member[i]) return 0; if (g_member[i] == k) return 1; } return 0; }
static int touch(const ui_box *a, const ui_box *b) { return a->x0 <= b->x1 + 6 && a->x1 >= b->x0 - 6 && a->y0 <= b->y1 + 6 && a->y1 >= b->y0 - 6; }
// A UI pass: Slate writes each frame's elements into its own vertex buffer (three in turn) from vertex 0. The renderer's frame counter is no clock for it: one of its
// frames can hold two UI passes or none, and a pass can straddle it (v18: the title's background lost its cover scale for a frame, a member was missed: a flicker)
static uint64_t g_pass_vb; static int64_t g_pass_hi;   // (a buffer used again for the next pass: it starts again from vertex 0)
// A seed's bottom row: within a tenth of the 16:9 band's height above the band's bottom ((1 + f)/2 of the window): 0.8 H at 4:3, 0.725 H at 1:1 (where 0.8 H lies below the band)
static float ui_seed_y(float H, float f) { return H * ((1.0f + f) * 0.5f - 0.1f * f); }
static void ui_frame(float W, float H, float f, uint64_t vb, int64_t v0) {
    if (v0 > 8 || (vb == g_pass_vb && g_pass_hi < 64)) { if (vb == g_pass_vb && v0 > g_pass_hi) g_pass_hi = v0; return; }
    g_pass_vb = vb; g_pass_hi = v0; g_box_frame++; g_full_seen = 0; g_ncbox = 0;
    uint64_t now = ui_now(); g_pass_t = now;
    if (now - g_clu_t < 8000000ull) return;   // (the group of this frame is known already)
    g_clu_t = now;
    static ui_box bx[UI_NB]; static uint64_t hk[2 * UI_NB]; int n = 0;   // the distinct boxes of the last 50 ms
    memset(hk, 0, sizeof hk);
    uint32_t cnt = g_ring_n < UI_RING ? g_ring_n : UI_RING;
    for (uint32_t k = 1; k <= cnt && n < UI_NB; k++) {
        uint32_t i = (g_ring_n - k) & (UI_RING - 1);
        if (now - g_ring_t[i] > 50000000ull) break;
        const ui_box *e = &g_ring[i];
        uint64_t h = ((uint64_t)(uint32_t)lroundf(e->x0) * 0x9e3779b1u ^ (uint64_t)(uint32_t)lroundf(e->x1) << 20 ^ (uint64_t)(uint32_t)lroundf(e->y0) << 40 ^ (uint64_t)(uint32_t)lroundf(e->y1) * 0x85ebca6bu) | 1;
        uint32_t s = (uint32_t)(h * 0x9e3779b97f4a7c15ull >> 51) & (2 * UI_NB - 1); int dup = 0;
        while (hk[s]) { if (hk[s] == h) { dup = 1; break; } s = (s + 1) & (2 * UI_NB - 1); }
        if (dup) continue;
        hk[s] = h; bx[n++] = *e;
    }
    static unsigned char in[UI_NB]; static int mem[UI_NB]; int nm = 0;
    for (int i = 0; i < n; i++) { const ui_box *e = &bx[i]; in[i] = e->y1 - e->y0 <= 0.5f * H && e->y1 >= ui_seed_y(H, f); if (in[i]) mem[nm++] = i; }
    for (int grew = 1; grew; ) {   // members take in what touches them, until nothing more does
        grew = 0;
        for (int i = 0; i < n; i++) {
            if (in[i] || bx[i].y1 - bx[i].y0 > 0.5f * H) continue;
            for (int j = 0; j < nm; j++) if (touch(&bx[i], &bx[mem[j]])) { in[i] = 1; mem[nm++] = i; grew = 1; break; }
        }
    }
    memset(g_member, 0, sizeof g_member);
    for (int j = 0; j < nm; j++) set_add(ui_key(&bx[mem[j]]));
    g_nmbox = 0; g_mtop = 1e30f;
    for (int j = 0; j < nm; j++) { g_mbox[g_nmbox++] = bx[mem[j]]; if (bx[mem[j]].y0 < g_mtop) g_mtop = bx[mem[j]].y0; }
    // Clusters (members touching each other, transitively: a meter with its frame and counter, a line of text): each moves to the side its bounds lie on, as one piece
    static int par[UI_NB];
    for (int j = 0; j < nm; j++) par[j] = j;
    for (int j = 0; j < nm; j++) for (int k = j + 1; k < nm; k++) if (touch(&g_mbox[j], &g_mbox[k])) {
        int a = j, b = k; while (par[a] != a) a = par[a]; while (par[b] != b) b = par[b]; if (a != b) par[b] = a; }
    static ui_box cb[UI_NB];
    for (int j = 0; j < nm; j++) cb[j] = (ui_box){1e30f, -1e30f, 1e30f, -1e30f};
    for (int j = 0; j < nm; j++) { int a = j; while (par[a] != a) a = par[a]; ui_box *c = &cb[a], *m = &g_mbox[j];
        if (m->x0 < c->x0) c->x0 = m->x0; if (m->x1 > c->x1) c->x1 = m->x1; if (m->y0 < c->y0) c->y0 = m->y0; if (m->y1 > c->y1) c->y1 = m->y1; }
    for (int j = 0; j < nm; j++) { int a = j; while (par[a] != a) a = par[a]; const ui_box *c = &cb[a];
        g_mside[j] = c->x1 < 0.5f * W ? -1 : c->x0 > 0.5f * W ? 1 : 0; }
}
// A member of the bottom group: its box overlaps one of last frame's members (an element that moves a little, a glyph that bobs, a fill that grows), or it touches
// an element already moved this frame (one that appears now beside its panel; Slate draws a panel before what sits on it)
static int ui_member_side(const ui_box *e, int *side) {   // *side: the side of the member it matched (2: none, a seed of this pass)
    *side = 2;
    if (!g_ui_match) return set_has(ui_key(e));
    if (e->y1 >= g_mtop - 2) for (int i = 0; i < g_nmbox; i++) { const ui_box *m = &g_mbox[i];
        if (e->x0 <= m->x1 + 2 && e->x1 >= m->x0 - 2 && e->y0 <= m->y1 + 2 && e->y1 >= m->y0 - 2) { *side = g_mside[i]; return 1; } }
    for (int i = 0; i < g_ncbox; i++) if (touch(e, &g_cbox[i])) { *side = g_cside[i]; return 1; }
    return 0;
}
static int ui_member(const ui_box *e) { int sd; return ui_member_side(e, &sd); }
static void ui_element(uint64_t vb, int64_t lo, int64_t hi, float W, float H, float f, float g) {
    if (lo < 0 || hi < lo || hi - lo > 4096) return;
    float *y0p = (float *)(vb + (uint64_t)lo * 40 + 28);
    uint32_t b0; memcpy(&b0, y0p, 4);
    float *x0p = y0p - 1; uint32_t xb0; memcpy(&xb0, x0p, 4);
    if (ui_done((uint64_t)y0p, b0) || ui_done((uint64_t)x0p, xb0)) { if (g_ui_logleft > 0) { g_ui_logleft--; LOG("dbfz ui el f%llu D %.0fx%.0f %#llx v%lld+%lld (moved already)", (unsigned long long)g_box_frame, W, H, (unsigned long long)vb, (long long)lo, (long long)(hi - lo + 1)); } return; }
    ui_box e = {1e30f, -1e30f, 1e30f, -1e30f};
    for (int64_t v = lo; v <= hi; v++) { float y = *(float *)(vb + (uint64_t)v * 40 + 28), x = *(float *)(vb + (uint64_t)v * 40 + 24);
                                         if (!(y == y) || !(x == x) || y < -4 * H || y > 5 * H) return;
                                         if (y < e.y0) e.y0 = y; if (y > e.y1) e.y1 = y; if (x < e.x0) e.x0 = x; if (x > e.x1) e.x1 = x; }
    int lg = g_ui_logleft > 0; if (lg) g_ui_logleft--;
#define UI_LOG(d) do { if (lg) LOG("dbfz ui el f%llu %c %.0fx%.0f %#llx v%lld+%lld x %.1f-%.1f y %.1f-%.1f", (unsigned long long)g_box_frame, d, W, H, (unsigned long long)vb, (long long)lo, (long long)(hi - lo + 1), e.x0, e.x1, e.y0, e.y1); } while (0)
    if (!g_ui_mode || (f >= 1.0f && g <= 1.0f)) { UI_LOG('-'); return; }
    ui_box_add(&e);
    float h = e.y1 - e.y0;
    if (e.x1 - e.x0 >= 0.98f * W && h >= 0.95f * H) { g_full_seen = 1; UI_LOG('F'); return; }
    if (g > 1.0f) {   // wider than 16:9 (g = shape / (16/9)): the game lays its UI out for the window, but its full-screen art stays a centred 16:9 and the battle's bottom
                      // HUD stays in the 16:9 band's width (its meters 400 columns in at 21:9): the art is scaled to cover, the bottom group's clusters move out to the sides
        float bw = H * 16.0f / 9.0f, cx = W * 0.5f;
        if (!g_full_seen && h >= 0.95f * H && e.x1 - e.x0 >= 0.9f * bw && e.x1 - e.x0 <= 1.2f * bw && e.x1 - e.x0 < 0.98f * W && fabsf((e.x0 + e.x1) * 0.5f - cx) < 0.03f * W) {   // (the title's art and the menus' dim panel are drawn a little larger than 16:9: 2041 wide at 1080 rows)
            float cy = H * 0.5f;
            for (int64_t v = lo; v <= hi; v++) {
                float *xp = (float *)(vb + (uint64_t)v * 40 + 24), *yp = xp + 1, y = (*yp - cy) * g + cy; uint32_t b; memcpy(&b, &y, 4);
                *xp = (*xp - cx) * g + cx; *yp = y; ui_mark((uint64_t)yp, b);
            }
            UI_LOG('C'); return;
        }
        // (Menus keep the game's centred 16:9 layout, as Banana Native's other wide games keep theirs: a dialogue box is drawn in pieces of several widths, and stretching
        // the full-width ones only (v31) split it)
        int side = 2; float sy = ui_seed_y(H, 1.0f);
        if (h > 0.5f * H || !(ui_member_side(&e, &side) || e.y1 >= sy)) { UI_LOG('S'); return; }
        if (g_ncbox < UI_NB) { g_cside[g_ncbox] = (signed char)(side == 2 ? 0 : side); g_cbox[g_ncbox++] = e; }
        if (side == 2 || side == 0) { UI_LOG('s'); return; }   // (a member that spans the middle, or a seed seen first now: its cluster is known next pass)
        UI_LOG(side < 0 ? 'L' : 'R');
        float dx = side * (W - W / g) * 0.5f;
        for (int64_t v = lo; v <= hi; v++) { float *xp = (float *)(vb + (uint64_t)v * 40 + 24), x = *xp + dx; uint32_t b; memcpy(&b, &x, 4); *xp = x; ui_mark((uint64_t)xp, b); }   // (x marked: the y stays what Slate wrote)
        return;
    }
    if (!g_full_seen && e.x1 - e.x0 >= 0.98f * W && h >= 0.9f * f * H && h <= 1.05f * f * H) {   // a full-width 16:9 picture (a menu's background art): covers the window, its sides cut
        float cx = W * 0.5f, cy = (e.y0 + e.y1) * 0.5f, k = 1.0f / f, ty = H * 0.5f - cy;
        for (int64_t v = lo; v <= hi; v++) {
            float *xp = (float *)(vb + (uint64_t)v * 40 + 24), *yp = xp + 1, y = (*yp - cy) * k + cy + ty; uint32_t b; memcpy(&b, &y, 4);
            *xp = (*xp - cx) * k + cx; *yp = y; ui_mark((uint64_t)yp, b);
        }
        UI_LOG('C'); return;
    }
    float sy = ui_seed_y(H, f);
    if (h > 0.5f * H || (e.y1 < sy && !ui_member(&e))) { UI_LOG('S'); return; }   // not in the bottom group (a seed now, or a member last frame): stays where the game puts it
    UI_LOG(e.y1 >= sy ? 'B' : 'M');
    if (g_ncbox < UI_NB) g_cbox[g_ncbox++] = e;
    float off = H * (1 - f) * 0.5f;
    for (int64_t v = lo; v <= hi; v++) {
        float *yp = (float *)(vb + (uint64_t)v * 40 + 28), y = *yp + off; uint32_t b; memcpy(&b, &y, 4);
        *yp = y; ui_mark((uint64_t)yp, b);
    }
}
static void ui_hook(uint64_t vs, uint64_t vb, uint32_t stride, uint64_t ib, int itype, int32_t bv, int32_t first, int32_t count, float vpw, float vph) {
    if (stride != 40 || !vb || count < 3) return;
    if (!ui_is_slate(vs)) {   // another shader on 40-byte vertices: named once while the element log runs (a Slate shader to add to the list)
        static uint64_t told[32]; static int nt;
        if (g_ui_logleft > 0 && nt < 32) { int k = 0; for (; k < nt && told[k] != vs; k++) ; if (k == nt) { told[nt++] = vs; LOG("dbfz ui: vertex shader %016llx draws 40-byte vertices into %.0fx%.0f (not hooked)", (unsigned long long)vs, vpw, vph); } }
        return;
    }
    ui_live();
    double s = shape(); if ((!s || fabs(s - 16.0 / 9.0) < 0.01) && !g_ui_logleft) return;   // (16:9: only the element log)
    float fs = s ? (float)(s / (16.0 / 9.0)) : 1.0f, f = fs < 1.0f ? fs : 1.0f, g = fs > 1.005f ? fs : 1.0f, H = vph; if (H < 64) return;
    int off_target = fabsf(vpw - (float)g_win_w) > 1.5f || fabsf(vph - (float)g_win_h) > 1.5f;
    if ((f < 1.0f || g > 1.0f) && off_target && !g_ui_logleft) return;   // an offscreen target (the character select's 16:9 picture): its own layout
    if (off_target) f = g = 1.0f;   // (logged, not moved)
    {   // the draw's first vertex
        int64_t v0 = first;
        if (ib) v0 = (int64_t)bv + (itype == 1 ? ((const uint16_t *)ib)[0] : itype == 2 ? (int64_t)((const uint32_t *)ib)[0] : ((const uint8_t *)ib)[0]);
        ui_frame(vpw, H, f, vb, v0);
    }
    int64_t lo = -1, hi = -1;
    for (int32_t t = 0; t + 2 < count; t += 3) {
        int64_t v[3];
        for (int k = 0; k < 3; k++) {
            if (!ib) v[k] = (int64_t)first + t + k;
            else if (itype == 1) v[k] = (int64_t)bv + ((const uint16_t *)ib)[t + k];
            else if (itype == 2) v[k] = (int64_t)bv + ((const uint32_t *)ib)[t + k];
            else v[k] = (int64_t)bv + ((const uint8_t *)ib)[t + k];
        }
        int64_t tl = v[0] < v[1] ? (v[0] < v[2] ? v[0] : v[2]) : (v[1] < v[2] ? v[1] : v[2]), th = v[0] > v[1] ? (v[0] > v[2] ? v[0] : v[2]) : (v[1] > v[2] ? v[1] : v[2]);
        if (lo >= 0 && tl <= hi + 0 && th >= lo) { if (tl < lo) lo = tl; if (th > hi) hi = th; continue; }   // shares the run's range: the same element
        if (lo >= 0) ui_element(vb, lo, hi, vpw, H, f, g);
        lo = tl; hi = th;
    }
    if (lo >= 0) ui_element(vb, lo, hi, vpw, H, f, g);
}
static void ui_init(void) {
    const char *e = getenv("DM_DBFZ_UI"); if (e && *e == '0') { LOG("dbfz aspect: UI left as the game draws it (DM_DBFZ_UI=0)"); return; }
    if ((e = getenv("DM_DBFZ_UI_VS")) && *e) for (const char *p = e; *p && g_ui_nvs < 16; ) { char *q; uint64_t k = strtoull(p, &q, 16); if (q == p) break; g_ui_vs[g_ui_nvs++] = k; p = *q == ',' ? q + 1 : q; }
    vka_vertex_hook = ui_hook;
}

__attribute__((constructor)) static void dbfz_aspect_attach(void) { gate_obs_set(OBS_DBFZ_RES, res_parsed); }
// The UI hook is attached once the game is known (the constructor runs in every game's host): at the first r.SetRes
static void ui_attach_once(void) { static int done; if (!done) { done = 1; ui_init(); } }
