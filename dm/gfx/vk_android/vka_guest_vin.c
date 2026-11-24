// Scaled vertex formats on a device without them (vka_guest.c; docs/reference/rendering.md "Scaled vertex formats on a device without them"): VK_FORMAT_*_USCALED /
// *_SSCALED feed a float shader input the integer's value unnormalized, and their vertex fetch is optional in Vulkan. NVIDIA's driver and Turnip have it; Qualcomm's
// driver on the Adreno 740 has none of them, and the GX layers' positions, colours and texture coordinates come in them (Super Mario 3D All-Stars' Sunshine, Super Mario
// Galaxy): every such draw read nothing. The attribute is fed as the matching UINT / SINT format instead, and the vertex shader reads it as an integer and converts it
// (OpConvertUToF / OpConvertSToF after each load), which is what the scaled format does in the fixed-function fetch.
// This file is the SPIR-V rewrite only (no Vulkan), so a host unit test can run it.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { OP_ENTRY_POINT = 15, OP_TYPE_INT = 21, OP_TYPE_FLOAT = 22, OP_TYPE_VECTOR = 23, OP_TYPE_POINTER = 32, OP_FUNCTION = 54, OP_FUNCTION_CALL = 57, OP_VARIABLE = 59,
       OP_LOAD = 61, OP_STORE = 62, OP_COPY_MEMORY = 63, OP_ACCESS_CHAIN = 65, OP_IN_BOUNDS_ACCESS_CHAIN = 66, OP_PTR_ACCESS_CHAIN = 67, OP_DECORATE = 71,
       OP_COPY_OBJECT = 83, OP_CONVERT_S_TO_F = 111, OP_CONVERT_U_TO_F = 112, DEC_LOCATION = 30, SC_INPUT = 1 };

// conv[l]: 0 the input at location l stays, 1 it becomes an unsigned integer, 2 a signed one. Returns the new module (malloc'd, *out_n words) or NULL with why: the module
// has no vertex input to change (why "") or a use of the input the rewrite does not follow
uint32_t *vkg_spv_scaled(const uint32_t *w, size_t n, const uint8_t conv[32], size_t *out_n, char *why, size_t wn) {
    why[0] = 0;
    if (n < 5 || w[0] != 0x07230203u) { snprintf(why, wn, "not SPIR-V"); return NULL; }
    uint32_t bound = w[3];
    if (!bound || bound > (1u << 22)) { snprintf(why, wn, "id bound %u", bound); return NULL; }
    // Pass 1: what each id is
    int32_t *loc = malloc(bound * sizeof *loc); uint32_t *pos = calloc(bound, sizeof *pos);   // pos: word index of the declaring instruction + 1
    uint8_t *role = calloc(bound, 1);   // 1 float32 type, 2 float32 vector, 3 int32 unsigned, 4 int32 signed, 5 int vector, 6 pointer, 7 variable
    uint32_t *a0 = calloc(bound, 4), *a1 = calloc(bound, 4);   // type operands: vector (component, count); pointer (storage, pointee); variable (type, storage)
    uint8_t *mark = calloc(bound, 1);   // 1 unsigned / 2 signed: a target variable or a pointer derived from one
    uint32_t *res = NULL; size_t first_target = 0;
    if (!loc || !pos || !role || !a0 || !a1 || !mark) { snprintf(why, wn, "out of memory"); goto out; }
    for (uint32_t i = 0; i < bound; i++) loc[i] = -1;
    int vertex = 0;
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) { snprintf(why, wn, "bad instruction length at word %zu", i); goto out; }
        const uint32_t *x = w + i;
        if (op == OP_ENTRY_POINT && len >= 3 && x[1] == 0) vertex = 1;
        else if (op == OP_DECORATE && len >= 4 && x[2] == DEC_LOCATION && x[1] < bound) loc[x[1]] = (int32_t)x[3];
        else if (op == OP_TYPE_FLOAT && len >= 3 && x[1] < bound && x[2] == 32) { role[x[1]] = 1; pos[x[1]] = (uint32_t)i + 1; }
        else if (op == OP_TYPE_INT && len >= 4 && x[1] < bound && x[2] == 32) { role[x[1]] = x[3] ? 4 : 3; pos[x[1]] = (uint32_t)i + 1; }
        else if (op == OP_TYPE_VECTOR && len >= 4 && x[1] < bound && x[2] < bound) {
            a0[x[1]] = x[2]; a1[x[1]] = x[3]; pos[x[1]] = (uint32_t)i + 1;
            role[x[1]] = role[x[2]] == 1 ? 2 : role[x[2]] == 3 || role[x[2]] == 4 ? 5 : 0;
        } else if (op == OP_TYPE_POINTER && len >= 4 && x[1] < bound) { role[x[1]] = 6; a0[x[1]] = x[2]; a1[x[1]] = x[3]; pos[x[1]] = (uint32_t)i + 1; }
        else if (op == OP_VARIABLE && len >= 4 && x[2] < bound) { role[x[2]] = 7; a0[x[2]] = x[1]; a1[x[2]] = x[3]; pos[x[2]] = (uint32_t)i + 1; }
        i += len;
    }
    if (!vertex) goto out;   // (no vertex entry point: nothing to change)
    // The targets: Input variables at a converted location whose type is a float32 scalar or vector
    int any = 0;
    for (uint32_t id = 1; id < bound; id++) {
        if (role[id] != 7 || a1[id] != SC_INPUT || loc[id] < 0 || loc[id] >= 32 || !conv[loc[id]]) continue;
        uint32_t pt = a0[id]; if (pt >= bound || role[pt] != 6) continue;
        uint32_t t = a1[pt]; if (t >= bound || (role[t] != 1 && role[t] != 2)) { snprintf(why, wn, "the input at location %d is not a float", loc[id]); goto out; }
        mark[id] = conv[loc[id]]; any = 1;
        if (!first_target || pos[id] - 1 < first_target) first_target = pos[id] - 1;
    }
    if (!any) goto out;
    // Pointers derived from them (access chains into a vector input), and uses the rewrite does not follow
    for (size_t i = 5; i < n; i += w[i] >> 16) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; const uint32_t *x = w + i;
        if ((op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN) && len >= 4 && x[3] < bound && mark[x[3]] && x[2] < bound) mark[x[2]] = mark[x[3]];
        else if (op == OP_PTR_ACCESS_CHAIN || op == OP_COPY_OBJECT || op == OP_COPY_MEMORY || op == OP_FUNCTION_CALL || op == OP_STORE) {
            for (uint32_t k = 1; k < len; k++) if (x[k] < bound && mark[x[k]] && !(op == OP_COPY_OBJECT && k == 2)) { snprintf(why, wn, "opcode %u uses a converted input", op); goto out; }
        }
    }
    // The integer types: existing ones are reused, missing ones declared before the first target variable (and after every type they take)
    uint32_t next = bound;
    uint32_t ity[2] = {0, 0}, ivec[2][5] = {{0}}, iptr_s[2] = {0, 0}, iptr_v[2][5] = {{0}};   // [signed][count]
    int need_s[2] = {0, 0}, need_v[2][5] = {{0}}, need_ps[2] = {0, 0};
    for (uint32_t id = 1; id < bound; id++) if (mark[id]) {
        int s = mark[id] - 1;
        if (role[id] == 7) { uint32_t t = a1[a0[id]]; if (role[t] == 2) { if (a1[t] < 2 || a1[t] > 4) { snprintf(why, wn, "a %u-component input", a1[t]); goto out; } need_v[s][a1[t]] = 1; } else need_ps[s] = 1; need_s[s] = 1; }
        else need_ps[s] = need_s[s] = 1;
    }
    size_t at = first_target;   // the insertion point: before the first target variable, after what it reuses
    for (uint32_t id = 1; id < bound; id++) {
        if (role[id] == 3 || role[id] == 4) { int s = role[id] == 4; if (!ity[s]) { ity[s] = id; } }
    }
    for (int s = 0; s < 2; s++) if (ity[s] && pos[ity[s]] - 1 > at) { snprintf(why, wn, "the 32-bit integer type is declared after the inputs"); goto out; }
    for (uint32_t id = 1; id < bound; id++) {
        if (role[id] == 5) for (int s = 0; s < 2; s++) if (ity[s] && a0[id] == ity[s] && a1[id] >= 2 && a1[id] <= 4 && !ivec[s][a1[id]] && pos[id] - 1 < at) ivec[s][a1[id]] = id;
    }
    for (uint32_t id = 1; id < bound; id++) {
        if (role[id] != 6 || a0[id] != SC_INPUT || pos[id] - 1 >= at) continue;
        for (int s = 0; s < 2; s++) {
            if (ity[s] && a1[id] == ity[s] && !iptr_s[s]) iptr_s[s] = id;
            for (int c = 2; c <= 4; c++) if (ivec[s][c] && a1[id] == ivec[s][c] && !iptr_v[s][c]) iptr_v[s][c] = id;
        }
    }
    uint32_t add[64]; size_t nadd = 0;
    for (int s = 0; s < 2; s++) {
        if (!need_s[s]) continue;
        if (!ity[s]) { ity[s] = next++; add[nadd++] = (4u << 16) | OP_TYPE_INT; add[nadd++] = ity[s]; add[nadd++] = 32; add[nadd++] = (uint32_t)s; }
        for (int c = 2; c <= 4; c++) if (need_v[s][c]) {
            if (!ivec[s][c]) { ivec[s][c] = next++; add[nadd++] = (4u << 16) | OP_TYPE_VECTOR; add[nadd++] = ivec[s][c]; add[nadd++] = ity[s]; add[nadd++] = (uint32_t)c; }
            if (!iptr_v[s][c]) { iptr_v[s][c] = next++; add[nadd++] = (4u << 16) | OP_TYPE_POINTER; add[nadd++] = iptr_v[s][c]; add[nadd++] = SC_INPUT; add[nadd++] = ivec[s][c]; }
        }
        if (!iptr_s[s] && need_ps[s]) { iptr_s[s] = next++; add[nadd++] = (4u << 16) | OP_TYPE_POINTER; add[nadd++] = iptr_s[s]; add[nadd++] = SC_INPUT; add[nadd++] = ity[s]; }
    }
    // Pass 2: the new module. Each load of a converted input gets a fresh id of the integer type and a conversion into the load's own id
    size_t nload = 0;
    for (size_t i = 5; i < n; i += w[i] >> 16) if ((w[i] & 0xffff) == OP_LOAD && (w[i] >> 16) >= 4 && w[i + 3] < bound && mark[w[i + 3]]) nload++;
    res = malloc((n + nadd + nload * 4) * 4);
    if (!res) { snprintf(why, wn, "out of memory"); goto out; }
    size_t o = 0;
    memcpy(res, w, 5 * 4); o = 5;
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; const uint32_t *x = w + i;
        if (i == at) { memcpy(res + o, add, nadd * 4); o += nadd; }
        if (op == OP_VARIABLE && len >= 4 && x[2] < bound && mark[x[2]]) {
            int s = mark[x[2]] - 1; uint32_t t = a1[a0[x[2]]];
            memcpy(res + o, x, len * 4); res[o + 1] = role[t] == 2 ? iptr_v[s][a1[t]] : iptr_s[s]; o += len;
        } else if ((op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN) && len >= 4 && x[2] < bound && mark[x[2]]) {
            memcpy(res + o, x, len * 4); res[o + 1] = iptr_s[mark[x[2]] - 1]; o += len;
        } else if (op == OP_LOAD && len >= 4 && x[3] < bound && mark[x[3]]) {
            int s = mark[x[3]] - 1; uint32_t rt = x[1], rid = x[2], tmp = next++;
            uint32_t it = role[rt] == 2 ? ivec[s][a1[rt]] : ity[s];
            memcpy(res + o, x, len * 4); res[o + 1] = it; res[o + 2] = tmp; o += len;
            res[o++] = (4u << 16) | (s ? OP_CONVERT_S_TO_F : OP_CONVERT_U_TO_F); res[o++] = rt; res[o++] = rid; res[o++] = tmp;
        } else { memcpy(res + o, x, len * 4); o += len; }
        i += len;
    }
    res[3] = next;
    *out_n = o;
out:
    free(loc); free(pos); free(role); free(a0); free(a1); free(mark);
    return res;
}
