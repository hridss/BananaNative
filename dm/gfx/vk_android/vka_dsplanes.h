// Depth and stencil as two planes in a render pass's feedback rule (vka_draw.c). A depth / stencil image keeps its stencil apart from its depth (a plane of its own on the
// Adreno; Vulkan views one aspect), so a draw that writes stencil while the pass samples the image's depth is no feedback: Unreal's deferred lights write each light's stencil
// mask, then shade reading depth, and with any depth / stencil write counted Orbitals ended its lighting pass about 128 times a frame. Only a plane written and sampled in one
// pass is. After nxnative a739027a (the Metal path's plane rule). Free of Vulkan, so the unit test builds it on any host (dm/host/tests/t_android_dsplanes.c)
#pragma once

enum { VKDS_DEPTH = 1, VKDS_STENCIL = 2, VKDS_BOTH = 3 };

// The stencil plane is written when the test is on, a face's write mask is not 0 and one of the six operations (fail, depth fail, pass; front then back) is not KEEP:
// KEEP everywhere writes nothing, whatever the mask. keep: the API's KEEP value
static inline unsigned vkds_stencil_writes(int test, unsigned wmask_front, unsigned wmask_back, const unsigned ops[6], unsigned keep) {
    if (!test || !(wmask_front || wmask_back)) return 0;
    for (int i = 0; i < 6; i++) if (ops[i] != keep) return VKDS_STENCIL;
    return 0;
}

// Whether the pass ends before a draw. cont: a SET_RT of the same targets continued the pass; sampled: the planes the draw samples of the depth / stencil attachment;
// writes: the planes it writes; written: the planes earlier draws of the pass wrote; read: the planes earlier draws sampled. A sample after a write of its plane needs
// the write stored first (only after a continuing SET_RT: without one, the game's barrier rule decides), and a write after a sample of its plane must not reach it
static inline int vkds_end(int cont, unsigned sampled, unsigned writes, unsigned written, unsigned read) {
    return (cont && (sampled & written)) || (writes & read);
}
