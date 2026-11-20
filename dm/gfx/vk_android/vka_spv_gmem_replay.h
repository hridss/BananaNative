// SPDX-License-Identifier: MIT
// Runtime C11 SPIR-V rewrite for straight-line vertex shaders with global-memory
// pool-table lookups (raw global memory loads via GMEM_MAP).
// Pure C11 with zero Vulkan, Android NDK, or host dependencies so it can be
// reused across GPU translators and mobile TBDR drivers (e.g., Turnip ir3).
#pragma once

#include <stddef.h>
#include <stdint.h>

enum {
    VKA_GMEM_REPLAY_SLOTS = 4,   // pool entries cached in Private variables per invocation
    VKA_GMEM_REPLAY_GAPS = 2,    // unmapped gap stretches cached in Private variables
    VKA_GMEM_REPLAY_RUNS = 1 + VKA_GMEM_REPLAY_SLOTS + VKA_GMEM_REPLAY_GAPS,
    VKA_GMEM_TABLE_LAST = 8191,  // last entry index in the 8192-entry pool table
};

// Rewrites SPIR-V (< 1.4) vertex module `w` (`nw` words) so raw global memory helper
// calls read directly through cached Private pool/gap bounds and re-run `main` on a
// miss instead of inlining binary searches at every call site.
//
// Returns a malloc'd word buffer (`*out_nw` words, `*out_sites > 0`) on rewrite.
// Returns NULL with `err[0] == '\0'` and `*out_sites = 0` when the module should
// stay unchanged (non-vertex stage, SPIR-V >= 1.4, unsupported body/callee, or
// already rewritten). Returns NULL with `err` populated on malformed SPIR-V or OOM.
uint32_t *vka_spv_gmem_replay(const uint32_t *w, size_t nw,
                              uint32_t table_set, uint32_t table_binding,
                              size_t *out_nw, int *out_sites,
                              char *err, size_t errn);
