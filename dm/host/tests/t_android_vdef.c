// What a vertex input reads when no NVN attribute feeds it (vka_vdef.h; vka_draw.c vin_build): an absent attribute (format 0, or past the bound count) reads NVN's default
// (0, 0, 0, 1.0) as raw words for float, uint and sint inputs alike (W = 0x3f800000, upstream nxnative's t_default_attrib); a format the replay does not map reads zeros.
// The fetch is a 4 x 32-bit format of the input's type from the block at stride 0, so every vertex reads the 16 bytes at the attribute's offset unchanged
#include <stdio.h>
#include "../../gfx/vk_android/vka_vdef.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// R32G32B32A32_{SFLOAT,UINT,SINT} at binding stride 0: vertex v reads offset + v * 0, the words as they are
static void fetch(const uint8_t *blk, uint32_t off, uint32_t vertex, uint32_t out[4]) { (void)vertex; memcpy(out, blk + off + 0u * vertex, 16); }

int main(void) {
    uint8_t blk[VKA_VDEF_SIZE + 16]; memset(blk, 0xEE, sizeof blk);
    vka_vdef_fill(blk);
    CHECK(blk[VKA_VDEF_SIZE] == 0xEE, "the fill wrote past the block");
    CHECK(VKA_VDEF_W1 + 16 <= VKA_VDEF_SIZE && VKA_VDEF_ZERO + 16 <= VKA_VDEF_SIZE, "a vector past the bound size (robust buffer access would read zeros)");
    CHECK(VKA_VDEF_W1 % 4 == 0 && VKA_VDEF_ZERO % 4 == 0, "a vector offset not 4-byte aligned");
    static const uint32_t w1[4] = {0, 0, 0, 0x3f800000u}, z[4] = {0, 0, 0, 0};
    const char *type[3] = {"float", "sint", "uint"};
    // absent: format 0 (an explicit attribute of format 0, or one past the bound count: vin_build passes 0)
    for (int t = 0; t < 3; t++) for (uint32_t v = 0; v < 3; v++) {
        uint32_t got[4]; fetch(blk, vka_vdef_offset(0), v, got);
        CHECK(!memcmp(got, w1, 16), "absent %s input, vertex %u: %08x %08x %08x %08x, want 0 0 0 3f800000", type[t], v, got[0], got[1], got[2], got[3]);
    }
    uint32_t got[4]; fetch(blk, vka_vdef_offset(0), 0, got);
    float f[4]; memcpy(f, got, 16);
    CHECK(f[0] == 0.0f && f[1] == 0.0f && f[2] == 0.0f && f[3] == 1.0f, "absent float input: %g %g %g %g, want 0 0 0 1", f[0], f[1], f[2], f[3]);
    int32_t s[4]; memcpy(s, got, 16);
    CHECK(s[3] == 0x3f800000 && s[3] != 1, "absent sint input: W %d, want the float bits 0x3f800000 (not the integer 1 a narrower format would give)", s[3]);
    // a format the replay does not map (non-zero): zeros, as upstream's unknown non-zero format
    for (uint32_t fmt = 1; fmt < 256; fmt += 37) {
        fetch(blk, vka_vdef_offset(fmt), 0, got);
        CHECK(!memcmp(got, z, 16), "unmapped format %#x: %08x %08x %08x %08x, want zeros", fmt, got[0], got[1], got[2], got[3]);
    }
    printf("t_android_vdef: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
