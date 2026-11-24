// What a vertex shader input reads when no NVN attribute feeds it (vka_draw.c vin_build; t_android_vdef): NVN / Maxwell's default attribute is the float vector
// (0, 0, 0, 1.0) as raw register words, W = 0x3f800000 whatever type the input is declared with, so an integer input reads 0x3f800000 in W, not 1 (upstream nxnative's
// Metal replay, PR #3237: dmsh_pull.metal dm_fetch and nvn_mtl.m vertex_constants, its t_default_attrib). An attribute whose format the replay does not map keeps four zero
// words, as upstream does for an unknown non-zero format. The replay binds this block at stride 0 (every vertex reads the same 16 bytes) and points each such input at
// one of its two vectors with the attribute's offset and a 4 x 32-bit format of the input's own type (R32G32B32A32_SFLOAT / _UINT / _SINT), which passes the words
// unchanged. A narrower format would not do: Vulkan fills a missing W of an integer format with the integer 1.
#ifndef VKA_VDEF_H
#define VKA_VDEF_H
#include <stdint.h>
#include <string.h>

#define VKA_VDEF_SIZE 32u   // bytes of the block: the zero vector, then the default vector
enum { VKA_VDEF_ZERO = 0, VKA_VDEF_W1 = 16 };   // offsets of the two vectors

static inline void vka_vdef_fill(void *p) {
    static const uint32_t w[VKA_VDEF_SIZE / 4] = {0, 0, 0, 0, 0, 0, 0, 0x3f800000u};
    memcpy(p, w, sizeof w);
}
// The vector an input reads that no mapped attribute feeds: fmt is its NVN attribute format, 0 when the attribute is absent (past the bound count, or format 0)
static inline uint32_t vka_vdef_offset(uint32_t fmt) { return fmt ? VKA_VDEF_ZERO : VKA_VDEF_W1; }
#endif
