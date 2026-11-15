// The texture cache's limit and eviction policy (dm/gfx/vk_android/vka_texlimit.h): the limit follows RAM and the heap budget, the override-free floor holds, images used in
// the current or the previous frame are never candidates, and a simulated working set larger than the limit is not uploaded again frame after frame (the thrash the old
// array-order, age-1 policy caused: Mario Kart World at a 1,536 MiB limit, Ocarina of Time at 512 MiB); a 12 GB device keeps an eighth of RAM, and memory pressure
// (PSI) stops the cache's growth, then shrinks it; while the system reports no stall an image used in the last VKTL_CALM_FRAMES frames stays, so a scene that
// comes back to its textures (a race's laps) does not make them again once free memory is at the reserve; under pressure the cache holds no more than before
#include <stdio.h>
#include <stdlib.h>
#include "../../gfx/vk_android/vka_texlimit.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define MIB VKTL_MIB
#define GIB (1024 * MIB)

// A cache of n images of one size, a frame using `use` of them (ids 0..use-1, in a rotating order), evicting by the policy whenever over the limit, as vka_tex.c does on a miss
typedef struct { uint64_t last; int live, gpu; } img;
static int cmp(const void *a, const void *b) { const img *x = *(img *const *)a, *y = *(img *const *)b; return vktl_order(x->gpu, x->last, y->gpu, y->last); }
static int run(int n, int use, uint64_t each, uint64_t limit, int frames, int *uploads_last) {
    img *im = calloc((size_t)n, sizeof *im); img **c = malloc((size_t)n * sizeof *c); uint64_t bytes = 0; int uploads = 0;
    for (uint64_t f = 1; f <= (uint64_t)frames; f++) {
        int up = 0;
        for (int k = 0; k < use; k++) {
            int id = (int)((k + f * 7) % (uint64_t)use);
            if (!im[id].live) {   // a miss: prune, then make it
                if (bytes > limit) {
                    int nc = 0;
                    for (int i = 0; i < n; i++) if (im[i].live && vktl_evictable(f, im[i].last, 0, im[i].gpu, 0, 0, 0)) c[nc++] = &im[i];
                    qsort(c, (size_t)nc, sizeof *c, cmp);
                    for (int i = 0; i < nc && bytes > vktl_target(limit); i++) { c[i]->live = 0; bytes -= each; }
                }
                im[id].live = 1; bytes += each; up++;
            }
            im[id].last = f;
        }
        uploads += up; if (f == (uint64_t)frames) *uploads_last = up;
    }
    free(im); free(c); return uploads;
}

// A lap: n images of one size, each frame uses a window of `use` of them that moves on by one image a frame (so an image comes back every n frames, unused for n - use
// of them). Free memory is at the reserve throughout, as on a 12 GB device in a race: the limit, measured every 60 frames, is what the cache holds. The uploads of the
// last lap, with the keep that pressure `psi` gives
static int lap(int n, int use, uint64_t each, double psi, int laps, uint64_t *bytes_end) {
    img *im = calloc((size_t)n, sizeof *im); img **c = malloc((size_t)n * sizeof *c); uint64_t bytes = 0, limit = 0; int last_lap = 0;
    vktl_in in = {.mem_total = 11 * GIB, .mem_avail = 11 * GIB / 8, .psi_some10 = psi};
    for (uint64_t f = 1; f <= (uint64_t)n * (uint64_t)laps; f++) {
        if (f % 60 == 1) { in.img_bytes = bytes; limit = vktl_limit(&in); }
        uint64_t keep = vktl_keep_frames(psi, VKTL_CALM_FRAMES);
        for (int k = 0; k < use; k++) {
            int id = (int)((f + (uint64_t)k) % (uint64_t)n);
            if (!im[id].live) {
                if (bytes > limit) {
                    int cand = 0;
                    for (int i = 0; i < n; i++) if (im[i].live && vktl_evictable_at(f, im[i].last, 0, 0, 0, 0, 0, keep)) c[cand++] = &im[i];
                    qsort(c, (size_t)cand, sizeof *c, cmp);
                    for (int i = 0; i < cand && bytes > vktl_target(limit); i++) { c[i]->live = 0; bytes -= each; }
                }
                im[id].live = 1; bytes += each;
                if (f > (uint64_t)n * (uint64_t)(laps - 1)) last_lap++;
            }
            im[id].last = f;
        }
    }
    *bytes_end = bytes; free(im); free(c); return last_lap;
}

int main(void) {
    // the limit: images plus what RAM can still give past the reserve, the budget's share past other usage, the floor
    vktl_in a = {.mem_total = 15 * GIB, .mem_avail = 4 * GIB, .img_bytes = 1 * GIB};
    CHECK(vktl_reserve(15 * GIB) == 512 * MIB, "a 15 GiB phone keeps 512 MiB");
    CHECK(vktl_limit(&a) == 1 * GIB + 4 * GIB - 512 * MIB, "RAM: images + MemAvailable - reserve (%llu MiB)", (unsigned long long)(vktl_limit(&a) >> 20));
    CHECK(vktl_reserve(64 * GIB) == 2 * GIB, "a 64 GiB machine keeps a 32nd");
    vktl_in b = a; b.heap_budget = 6 * GIB; b.heap_usage = 4 * GIB;   // 3 GiB of other use: 6 - 6/16 - 3 = 2.625 GiB
    CHECK(vktl_limit(&b) == 6 * GIB - 6 * GIB / 16 - 3 * GIB, "the budget's share is the smaller (%llu MiB)", (unsigned long long)(vktl_limit(&b) >> 20));
    vktl_in h = {.heap_size = 12 * GIB}; CHECK(vktl_limit(&h) == 6 * GIB, "no budget, no RAM figures: half the heap");
    vktl_in z = {0}; CHECK(vktl_limit(&z) == VKTL_FLOOR, "nothing known: the floor");
    vktl_in lo = {.mem_total = 4 * GIB, .mem_avail = 300 * MIB, .img_bytes = 100 * MIB}; CHECK(vktl_limit(&lo) == VKTL_FLOOR, "a tight phone: not under the floor");
    vktl_in full = {.mem_total = 15 * GIB, .mem_avail = 700 * MIB, .img_bytes = 3 * GIB}; CHECK(vktl_limit(&full) == 3 * GIB + 188 * MIB, "Mario Kart World's measured state: about its images");

    // a 12 GB device (MemTotal 11 GiB) keeps an eighth; 16 GB devices keep the old rule
    CHECK(vktl_reserve(11 * GIB) == 11 * GIB / 8, "an 11 GiB device keeps an eighth (%llu MiB)", (unsigned long long)(vktl_reserve(11 * GIB) >> 20));
    CHECK(vktl_reserve(12 * GIB) == 12 * GIB / 8 && vktl_reserve(12 * GIB + MIB) == 512 * MIB, "12 GiB is the edge");
    // the Portal's Zelda run: 6.8 GiB available at the start, so the old reserve let the images take nearly all of it
    vktl_in portal = {.mem_total = 11 * GIB, .mem_avail = 6800 * MIB, .img_bytes = 0};
    CHECK(vktl_limit(&portal) == 6800 * MIB - 11 * GIB / 8, "a fresh start on the Portal: MemAvailable less an eighth (%llu MiB)", (unsigned long long)(vktl_limit(&portal) >> 20));
    // memory pressure: below the hold mark nothing changes; at it the limit is what the cache holds; past the shrink mark an eighth less; never under the floor
    vktl_in p0 = {.mem_total = 11 * GIB, .mem_avail = 4 * GIB, .img_bytes = 2 * GIB, .psi_some10 = 1.0};
    CHECK(vktl_limit(&p0) == 2 * GIB + 4 * GIB - 11 * GIB / 8, "light pressure: the RAM rule (%llu MiB)", (unsigned long long)(vktl_limit(&p0) >> 20));
    vktl_in p1 = p0; p1.psi_some10 = VKTL_PSI_HOLD;
    CHECK(vktl_limit(&p1) == 2 * GIB, "stalling: no growth past what it holds (%llu MiB)", (unsigned long long)(vktl_limit(&p1) >> 20));
    vktl_in p2 = p0; p2.psi_some10 = 40.0;
    CHECK(vktl_limit(&p2) == 2 * GIB - 2 * GIB / 8, "stalling hard: an eighth back (%llu MiB)", (unsigned long long)(vktl_limit(&p2) >> 20));
    vktl_in p3 = {.mem_total = 11 * GIB, .mem_avail = 4 * GIB, .img_bytes = 300 * MIB, .psi_some10 = 40.0};
    CHECK(vktl_limit(&p3) == VKTL_FLOOR, "pressure never goes under the floor");
    vktl_in p4 = p0; p4.psi_some10 = -1;
    CHECK(vktl_limit(&p4) == vktl_limit(&p0), "pressure not known: the RAM rule");
    vktl_in p5 = {.mem_total = 11 * GIB, .mem_avail = 700 * MIB, .img_bytes = 2 * GIB, .psi_some10 = VKTL_PSI_HOLD};
    CHECK(vktl_limit(&p5) == 2 * GIB, "less available than the reserve: the RAM rule holds the images where they are, it never takes any back (%llu MiB)", (unsigned long long)(vktl_limit(&p5) >> 20));
    p5.psi_some10 = VKTL_PSI_SHRINK; CHECK(vktl_limit(&p5) == 2 * GIB - 2 * GIB / 8, "only pressure takes images back");

    // candidates
    CHECK(!vktl_evictable(100, 100, 0, 0, 0, 0, 0), "used this frame: kept");
    CHECK(!vktl_evictable(100, 99, 0, 0, 0, 0, 0), "used the previous frame: kept");
    CHECK(vktl_evictable(100, 98, 0, 0, 0, 0, 0), "two frames unused: a candidate");
    CHECK(!vktl_evictable(100, 50, 1, 0, 0, 0, 0), "held: kept");
    CHECK(!vktl_evictable(100, 50, 0, 0, 1, 0, 0), "a playing video's: kept");
    CHECK(!vktl_evictable(100, 50, 0, 0, 0, 1, 0), "its submission not done: kept");
    CHECK(!vktl_evictable(100, 0, 0, 0, 0, 0, 0), "never stamped: kept");
    CHECK(!vktl_evictable(100, 50, 0, 1, 0, 0, 0) && !vktl_evictable(200, 50, 0, 1, 0, 0, 0) && !vktl_evictable(100000, 50, 0, 1, 0, 0, 0), "GPU content: never a candidate, however old (its storage cannot give it back; #215)");
    CHECK(!vktl_evictable(100, 50, 0, 1, 0, 0, 1) && vktl_evictable(200, 50, 0, 1, 0, 0, 1) && !vktl_evictable(200, 50, 1, 1, 0, 0, 1) && !vktl_evictable(200, 50, 0, 1, 0, 1, 1),
          "GPU content under hard memory pressure: after VKTL_GPU_FRAMES unused frames, never held or in flight");
    CHECK(vktl_order(0, 90, 1, 10) < 0 && vktl_order(0, 10, 0, 90) < 0 && vktl_order(1, 10, 1, 90) < 0, "uploadable before GPU-only, oldest first");

    // A working set (1,800 MiB, 900 images of 2 MiB) over a 1,536 MiB limit: uploaded once, then kept over the limit, not again every frame
    int last = -1, up = run(1200, 900, 2 * MIB, 1536 * MIB, 120, &last);
    CHECK(up == 900 && last == 0, "a working set over the limit: %d uploads, %d in the last frame", up, last);
    // Under the limit: one upload each
    up = run(1200, 600, 2 * MIB, 1536 * MIB, 120, &last);
    CHECK(up == 600 && last == 0, "under the limit: one upload each (%d, last frame %d)", up, last);
    // A cache with 100 uploadable and 100 GPU-written images over the limit, all long unused: only the uploadable ones go, the GPU-written ones stay (over the limit, as logged)
    { enum { N = 200 }; img im[N]; img *c[N]; uint64_t bytes = N * 8 * MIB, limit = 700 * MIB; int nc = 0, evicted_gpu = 0;
      for (int i = 0; i < N; i++) { im[i] = (img){.last = (uint64_t)(10 + i), .live = 1, .gpu = i % 2}; }
      for (int i = 0; i < N; i++) if (vktl_evictable(5000, im[i].last, 0, im[i].gpu, 0, 0, 0)) c[nc++] = &im[i];
      qsort(c, (size_t)nc, sizeof *c, cmp);
      for (int i = 0; i < nc && bytes > vktl_target(limit); i++) { evicted_gpu += c[i]->gpu; c[i]->live = 0; bytes -= 8 * MIB; }
      CHECK(nc == N / 2 && evicted_gpu == 0 && bytes == N / 2 * 8 * MIB && bytes > limit, "GPU-written images stay over the limit: %d candidates, %d GPU-written evicted, %llu MiB left", nc, evicted_gpu, (unsigned long long)(bytes >> 20)); }

    // how long unused an image must be: VKTL_CALM_FRAMES while the system reports no stall, two frames under pressure and when the pressure is not known
    CHECK(vktl_keep_frames(0.0, VKTL_CALM_FRAMES) == VKTL_CALM_FRAMES && vktl_keep_frames(VKTL_PSI_HOLD - 0.1, VKTL_CALM_FRAMES) == VKTL_CALM_FRAMES, "no stall: the calm keep");
    CHECK(vktl_keep_frames(VKTL_PSI_HOLD, VKTL_CALM_FRAMES) == VKTL_KEEP_FRAMES && vktl_keep_frames(VKTL_PSI_SHRINK, VKTL_CALM_FRAMES) == VKTL_KEEP_FRAMES &&
          vktl_keep_frames(40.0, VKTL_CALM_FRAMES) == VKTL_KEEP_FRAMES, "under pressure, from the hold mark on: two frames, as before");
    CHECK(vktl_keep_frames(-1, VKTL_CALM_FRAMES) == VKTL_KEEP_FRAMES, "pressure not known: two frames (nothing says memory is not short)");
    CHECK(vktl_keep_frames(0.0, 2) == VKTL_KEEP_FRAMES && vktl_keep_frames(0.0, 0) == VKTL_KEEP_FRAMES && vktl_keep_frames(0.0, 1800) == 1800, "the switch: never under two frames");
    CHECK(!vktl_evictable_at(1000, 401, 0, 0, 0, 0, 0, 600) && vktl_evictable_at(1000, 400, 0, 0, 0, 0, 0, 600), "calm: a candidate only after the keep's frames unused");
    CHECK(!vktl_evictable_at(1000, 100, 1, 0, 0, 0, 0, 600) && !vktl_evictable_at(1000, 100, 0, 1, 0, 0, 0, 600) && !vktl_evictable_at(1000, 900, 0, 1, 0, 0, 1, 600),
          "calm: held and GPU-written images as before");
    // A lap of 400 frames over 3,200 MiB of textures, 960 MiB of them in a frame, free memory at the reserve. With the two-frame keep the limit follows the cache
    // down to what a frame uses and every texture is made again every lap; with the calm keep the lap stays and nothing is uploaded after the first lap
    { uint64_t held = 0; int up = lap(400, 120, 8 * MIB, 0.5, 6, &held);
      CHECK(up == 0 && held == 400 * 8 * MIB, "a lap with no stall: %d uploads in the last lap, %llu MiB held", up, (unsigned long long)(held >> 20));
      up = lap(400, 120, 8 * MIB, VKTL_PSI_HOLD + 1.5, 6, &held);
      CHECK(up == 400 && held < 400 * 8 * MIB / 2, "the same lap while the limit holds on pressure: as before, no more than a frame uses is held (%d uploads in the last lap, %llu MiB held)", up, (unsigned long long)(held >> 20));
      up = lap(400, 120, 8 * MIB, VKTL_PSI_SHRINK, 6, &held);
      CHECK(up == 400 && held < 400 * 8 * MIB / 2, "the same lap under hard pressure: the cache gives back what the frame does not use (%d uploads in the last lap, %llu MiB held)", up, (unsigned long long)(held >> 20));
      up = lap(400, 120, 8 * MIB, -1, 6, &held);
      CHECK(up == 400 && held < 400 * 8 * MIB / 2, "the same lap with the pressure not known: as before (%d uploads in the last lap, %llu MiB held)", up, (unsigned long long)(held >> 20));
      // a lap longer than the keep: what was unused for the keep's frames goes, the rest stays
      up = lap(2000, 120, 2 * MIB, 0.5, 4, &held);
      CHECK(up == 2000 && held >= (uint64_t)VKTL_CALM_FRAMES * 2 * MIB && held <= (uint64_t)(VKTL_CALM_FRAMES + 120 + 130) * 2 * MIB,
            "a lap longer than the keep: %d uploads in the last lap, %llu MiB held", up, (unsigned long long)(held >> 20)); }
    if (!fails) printf("t_android_texlimit: ok\n");
    return fails != 0;
}
