// Depth and stencil planes in the render pass feedback rule (dm/gfx/vk_android/vka_dsplanes.h): a stencil write while depth is sampled keeps the pass, the same plane
// written and sampled ends it, KEEP operations write no stencil
#include <stdio.h>
#include "../../gfx/vk_android/vka_dsplanes.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    enum { KEEP = 1, REPLACE = 3 };
    const unsigned keep6[6] = {KEEP, KEEP, KEEP, KEEP, KEEP, KEEP}, mask6[6] = {KEEP, KEEP, REPLACE, KEEP, KEEP, KEEP};
    CHECK(vkds_stencil_writes(1, 0xff, 0, mask6, KEEP) == VKDS_STENCIL, "a pass operation that replaces writes stencil");
    CHECK(!vkds_stencil_writes(1, 0xff, 0xff, keep6, KEEP), "KEEP everywhere writes nothing");
    CHECK(!vkds_stencil_writes(1, 0, 0, mask6, KEEP), "a zero write mask writes nothing");
    CHECK(!vkds_stencil_writes(0, 0xff, 0xff, mask6, KEEP), "the test off writes nothing");

    // Unreal's lights: the stencil mask written, then shading that samples depth after a continuing SET_RT
    CHECK(!vkds_end(1, VKDS_DEPTH, 0, VKDS_STENCIL, 0), "depth sampled after a stencil write: the pass goes on");
    CHECK(!vkds_end(1, 0, VKDS_STENCIL, 0, VKDS_DEPTH), "stencil written after a depth sample: the pass goes on");
    CHECK(vkds_end(1, VKDS_DEPTH, 0, VKDS_DEPTH, 0), "depth sampled after a depth write: ends");
    CHECK(vkds_end(1, VKDS_STENCIL, 0, VKDS_STENCIL | VKDS_DEPTH, 0), "a stencil view sampled after a stencil write: ends");
    CHECK(vkds_end(0, 0, VKDS_DEPTH, 0, VKDS_DEPTH), "depth written after a depth sample: ends, with or without a SET_RT");
    CHECK(!vkds_end(0, VKDS_DEPTH, 0, VKDS_DEPTH, 0), "without a continuing SET_RT a sample after a write is the barrier rule's");
    CHECK(vkds_end(1, VKDS_BOTH, 0, VKDS_STENCIL, 0), "a view of unknown plane counts as both");
    if (!fails) printf("t_android_dsplanes: ok\n");
    return fails != 0;
}
