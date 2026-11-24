// nvn_obj.h nvn_snap_relocate: a copied record (CopyCommands) names its own texture snapshots, so recording over the source does not change what the copy draws into.
// Synthetic records as hle_nvn.c writes them (REC_SNAP: the payload, then the snapshots, each pointer to its snapshot inside the record); no GPU, no game files
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "../nvn/nvn_obj.h"

static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)
#define SNAP_OFF(T) ((sizeof(T) + 7) & ~(size_t)7)

static _Alignas(16) uint8_t src[8192], dst[8192];
static tex_t outside;   // an object kept by its address (not valid when recorded): never moved
static uint32_t rec(uint8_t *at, uint32_t op, size_t payload, int nsnap) {   // header, zeroed payload and snapshots, as REC_SNAP sizes them
    uint32_t sz = (uint32_t)((8 + payload + (size_t)nsnap * sizeof(tex_t) + 7) & ~(size_t)7); op_hdr h = {op, sz};
    memset(at, 0, sz); memcpy(at, &h, 8); return sz;
}
static const tex_t *snap(uint8_t *r, size_t off, int k, uint64_t serial) {   // as tex_snap: the snapshot at payload + off + k, its serial set
    tex_t *s = (tex_t *)(r + 8 + off) + k; s->magic = 1; s->serial = serial; return s;
}

int main(void) {
    // SET_RT: two colour targets and a depth target snapped, one colour target an outside object
    uint32_t sz = rec(src, OP_SET_RT, SNAP_OFF(p_set_rt), 3 + 1);
    p_set_rt *r = (p_set_rt *)(src + 8); r->n = 3;
    r->color[0] = snap(src, SNAP_OFF(p_set_rt), 0, 100); r->color[1] = &outside; r->color[2] = snap(src, SNAP_OFF(p_set_rt), 2, 102);
    r->depth = snap(src, SNAP_OFF(p_set_rt), 3, 103);
    memcpy(dst, src, sz);
    CHECK(nvn_snap_relocate(dst, src, sz, (uint64_t)(uintptr_t)dst) == 3, "SET_RT: three snapshot pointers moved");
    const p_set_rt *c = (const p_set_rt *)(dst + 8);
    CHECK((const uint8_t *)c->color[0] == dst + ((const uint8_t *)r->color[0] - src), "SET_RT colour 0 names the copy's snapshot");
    CHECK(c->color[1] == &outside, "SET_RT: an outside object stays");
    CHECK((const uint8_t *)c->color[2] == dst + ((const uint8_t *)r->color[2] - src) && (const uint8_t *)c->depth == dst + ((const uint8_t *)r->depth - src), "SET_RT colour 2 and depth moved");
    memset(src, 0xa5, sz);   // the game records over the source
    CHECK(c->color[0]->serial == 100 && c->color[2]->serial == 102 && c->depth->serial == 103, "SET_RT: the copy's snapshots survive the source being recorded over");
    // A record whose count is out of range moves nothing; a pointer past the record's end stays
    sz = rec(src, OP_SET_RT, SNAP_OFF(p_set_rt), 1); r = (p_set_rt *)(src + 8); r->n = 9; r->depth = snap(src, SNAP_OFF(p_set_rt), 0, 1);
    memcpy(dst, src, sz); CHECK(nvn_snap_relocate(dst, src, sz, (uint64_t)(uintptr_t)dst) == 0, "SET_RT with 9 colour targets: nothing moved");
    sz = rec(src, OP_SET_RT, SNAP_OFF(p_set_rt), 1); r = (p_set_rt *)(src + 8); r->n = 0; r->depth = (const tex_t *)(src + sz);   // the next record's bytes
    memcpy(dst, src, sz); CHECK(nvn_snap_relocate(dst, src, sz, (uint64_t)(uintptr_t)dst) == 0 && ((const p_set_rt *)(dst + 8))->depth == (const tex_t *)(src + sz), "a pointer past the record stays");
    // COPY_T2T (src, dst), COPY_B2T and COPY_T2B (tex), CLEAR_TEX (tex), DLSS (each texture)
    sz = rec(src, OP_COPY_T2T, SNAP_OFF(p_copy_t2t), 2); p_copy_t2t *t2t = (p_copy_t2t *)(src + 8);
    t2t->src = snap(src, SNAP_OFF(p_copy_t2t), 0, 7); t2t->dst = snap(src, SNAP_OFF(p_copy_t2t), 1, 8);
    memcpy(dst, src, sz); CHECK(nvn_snap_relocate(dst, src, sz, (uint64_t)(uintptr_t)dst) == 2, "COPY_T2T: both moved");
    memset(src, 0, sz); CHECK(((p_copy_t2t *)(dst + 8))->src->serial == 7 && ((p_copy_t2t *)(dst + 8))->dst->serial == 8, "COPY_T2T reads the copy");
    for (int op = 0; op < 2; op++) {
        sz = rec(src, op ? OP_COPY_T2B : OP_COPY_B2T, SNAP_OFF(p_copy_b2t), 1); p_copy_b2t *b = (p_copy_b2t *)(src + 8); b->src = 0x40001000; b->tex = snap(src, SNAP_OFF(p_copy_b2t), 0, 9);
        memcpy(dst, src, sz); CHECK(nvn_snap_relocate(dst, src, sz, (uint64_t)(uintptr_t)dst) == 1 && ((p_copy_b2t *)(dst + 8))->src == 0x40001000, op ? "COPY_T2B: tex moved, address kept" : "COPY_B2T: tex moved, address kept");
    }
    sz = rec(src, OP_CLEAR_TEX, SNAP_OFF(p_clear_tex), 1); ((p_clear_tex *)(src + 8))->tex = snap(src, SNAP_OFF(p_clear_tex), 0, 10);
    memcpy(dst, src, sz); CHECK(nvn_snap_relocate(dst, src, sz, (uint64_t)(uintptr_t)dst) == 1, "CLEAR_TEX: moved");
    sz = rec(src, OP_DLSS, SNAP_OFF(p_dlss), DLSS_TEXTURES); p_dlss *d = (p_dlss *)(src + 8);
    for (int k = 0; k < DLSS_TEXTURES; k++) d->tex[k] = k == DLSS_BIAS ? NULL : snap(src, SNAP_OFF(p_dlss), k, 20 + (uint64_t)k);
    memcpy(dst, src, sz); CHECK(nvn_snap_relocate(dst, src, sz, (uint64_t)(uintptr_t)dst) == DLSS_TEXTURES - 1, "DLSS: every snapped texture moved, a null one stays");
    // Records without snapshots are left alone
    sz = rec(src, OP_DRAW, sizeof(p_draw), 0); memcpy(dst, src, sz);
    CHECK(nvn_snap_relocate(dst, src, sz, (uint64_t)(uintptr_t)dst) == 0 && !memcmp(dst, src, sz), "DRAW: unchanged");
    printf("t_snap_reloc: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
