// SPDX-FileCopyrightText: Copyright 2026 Banana Native
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Render-condition state walked ahead by vka_exec.c memu_walk (DM_VK_MESH_EMU_COND), free of Vulkan for dm/host/tests/t_android_memu.c.
#ifndef VKA_MEMU_COND_H
#define VKA_MEMU_COND_H
#include <assert.h>
#include <stdint.h>
#include <string.h>

enum { VKA_MRC_MAX_REP = 64, VKA_MRC_TS = 1, VKA_MRC_CONST = 2, VKA_MRC_OQ = 3 };

typedef struct {
    int rc_skip;
    int oq_count;
    int nrep;
    uint64_t addr[VKA_MRC_MAX_REP];
    uint32_t kind[VKA_MRC_MAX_REP];
} vka_mrc_walk;

static inline void vka_mrc_init(vka_mrc_walk *w, int rc_skip, int oq_count) {
    assert(w != NULL);
    memset(w, 0, sizeof *w);
    w->rc_skip = rc_skip != 0;
    w->oq_count = oq_count != 0;
}

static inline int vka_mrc_note_report(vka_mrc_walk *w, uint64_t addr, uint32_t kind) {
    assert(w != NULL);
    assert(w->nrep >= 0 && w->nrep <= VKA_MRC_MAX_REP);
    if (!addr) return 1;
    for (int i = 0; i < w->nrep && i < VKA_MRC_MAX_REP; i++) {
        if (w->addr[i] == addr) { w->kind[i] = kind; return 1; }
    }
    if (w->nrep >= VKA_MRC_MAX_REP) return 0;
    w->addr[w->nrep] = addr;
    w->kind[w->nrep] = kind;
    w->nrep++;
    return 1;
}

// >= 0: answered by a report met during the walk (1 = known *val, 0 = unknown); -1 = not met in walk (use rc_known); -2 = met non-TS with qmem (read guest RAM)
static inline int vka_mrc_walk_known(const vka_mrc_walk *w, int qmem, uint64_t addr, uint64_t *val) {
    assert(w != NULL);
    assert(val != NULL);
    assert(w->nrep >= 0 && w->nrep <= VKA_MRC_MAX_REP);
    if (!addr) return -1;
    for (int i = 0; i < w->nrep && i < VKA_MRC_MAX_REP; i++) {
        if (w->addr[i] != addr) continue;
        if (w->kind[i] == VKA_MRC_TS) { *val = 0; return 1; }
        return qmem ? -2 : 0;
    }
    return -1;
}

static inline int vka_mrc_eval(int32_t mode, int k0, uint64_t v0, int k1, uint64_t v1) {
    if (mode >= 2) return mode == 3;
    if (!k0 || !k1) return 0;
    return mode != 0 ? v0 == v1 : v0 != v1;
}
#endif
