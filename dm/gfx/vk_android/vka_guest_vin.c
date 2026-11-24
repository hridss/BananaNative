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

// Descriptor arrays indexed per invocation (docs/reference/rendering.md "Descriptor arrays indexed per vertex without NonUniform"): Hagi's GX vertex shaders pick a
// transform-matrix block per vertex (position_matrix[4], normal_matrix[2]: arrays of uniform blocks, indexed by in_MatIdx). Vulkan defines such an access only when the
// index is dynamically uniform, or the access is decorated NonUniform; NVIDIA's hardware reads any element either way, Adreno does not (skinned models drawn as spikes).
// Every access chain into a descriptor array (a Uniform, StorageBuffer or UniformConstant variable of array type) whose first index is not a constant, and every load
// through it, is decorated NonUniform, with the capabilities that need (ShaderNonUniform and the array kind's) and SPV_EXT_descriptor_indexing below SPIR-V 1.5.
// kinds (out): bit 0 a uniform buffer array, 1 storage buffer, 2 sampled image / sampler, 3 storage image was so indexed. NULL: nothing to change (why "") or not done
uint32_t *vkg_spv_nonuniform(const uint32_t *w, size_t n, size_t *out_n, unsigned *kinds, char *why, size_t wn) {
    enum { OP_CAPABILITY = 17, OP_EXTENSION = 10, OP_MEMORY_MODEL = 14, OP_TYPE_IMAGE = 25, OP_TYPE_SAMPLER = 26, OP_TYPE_SAMPLED_IMAGE = 27, OP_TYPE_ARRAY = 28, OP_TYPE_RTARRAY = 29,
           OP_TYPE_STRUCT = 30, OP_CONSTANT = 43, OP_SPEC_CONSTANT = 50, OP_MEMBER_DECORATE = 72, DEC_NON_UNIFORM = 5300, DEC_BLOCK = 2, DEC_BUFFER_BLOCK = 3,
           CAP_SHADER_NON_UNIFORM = 5301, CAP_UBO_NU = 5306, CAP_SAMPLED_NU = 5307, CAP_SSBO_NU = 5308, CAP_STORAGE_IMAGE_NU = 5309, SC_UNIFORM_CONSTANT = 0, SC_UNIFORM = 2, SC_STORAGE_BUFFER = 12 };
    why[0] = 0; *kinds = 0;
    if (n < 5 || w[0] != 0x07230203u) { snprintf(why, wn, "not SPIR-V"); return NULL; }
    uint32_t bound = w[3];
    if (!bound || bound > (1u << 22)) { snprintf(why, wn, "id bound %u", bound); return NULL; }
    uint8_t *cst = calloc(bound, 1), *kind = calloc(bound, 1), *dec = calloc(bound, 1);   // cst: a constant; kind: array variable kind + 1 / type kind; dec: to decorate (1), already decorated (2)
    uint32_t *ptype = calloc(bound, 4), *elem = calloc(bound, 4);   // ptype: a pointer's pointee; elem: an array's element type
    uint8_t *tkind = calloc(bound, 1), *block = calloc(bound, 1);   // tkind: 1 struct, 2 image, 3 sampler / sampled image, 4 array; block: a struct with Block / BufferBlock
    uint32_t *res = NULL; size_t ndec = 0, ann_end = 0, cap_end = 0; int have_ext = 0, have_cap[6] = {0};
    if (!cst || !kind || !dec || !ptype || !elem || !tkind || !block) { snprintf(why, wn, "out of memory"); goto out; }
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; const uint32_t *x = w + i;
        if (!len || i + len > n) { snprintf(why, wn, "bad instruction length"); goto out; }
        if (op == OP_CAPABILITY && len >= 2) { cap_end = i + len; uint32_t c = x[1];
            if (c == CAP_SHADER_NON_UNIFORM) have_cap[0] = 1; else if (c == CAP_UBO_NU) have_cap[1] = 1; else if (c == CAP_SSBO_NU) have_cap[2] = 1; else if (c == CAP_SAMPLED_NU) have_cap[3] = 1; else if (c == CAP_STORAGE_IMAGE_NU) have_cap[4] = 1; }
        else if (op == OP_EXTENSION && len >= 2 && !strncmp((const char *)(x + 1), "SPV_EXT_descriptor_indexing", (len - 1) * 4)) have_ext = 1;
        else if (op == OP_DECORATE && len >= 3 && x[1] < bound) { if (x[2] == DEC_NON_UNIFORM) dec[x[1]] = 2; if (x[2] == DEC_BLOCK || x[2] == DEC_BUFFER_BLOCK) block[x[1]] = 1; }
        else if ((op == OP_CONSTANT || op == OP_SPEC_CONSTANT) && len >= 3 && x[2] < bound) cst[x[2]] = 1;
        else if (op == OP_TYPE_STRUCT && len >= 2 && x[1] < bound) tkind[x[1]] = 1;
        else if (op == OP_TYPE_IMAGE && len >= 9 && x[1] < bound) tkind[x[1]] = x[7] == 2 ? 5 : 2;   // (Sampled 2: a storage image)
        else if ((op == OP_TYPE_SAMPLER || op == OP_TYPE_SAMPLED_IMAGE) && len >= 2 && x[1] < bound) tkind[x[1]] = 3;
        else if ((op == OP_TYPE_ARRAY || op == OP_TYPE_RTARRAY) && len >= 3 && x[1] < bound) { tkind[x[1]] = 4; elem[x[1]] = x[2]; }
        else if (op == OP_TYPE_POINTER && len >= 4 && x[1] < bound) ptype[x[1]] = x[3];
        else if (op == OP_VARIABLE && len >= 4 && x[2] < bound && x[1] < bound) {
            uint32_t t = ptype[x[1]], sc = x[3];
            if (t < bound && tkind[t] == 4 && elem[t] < bound) {
                uint32_t e = elem[t];
                if ((sc == SC_UNIFORM || sc == SC_STORAGE_BUFFER) && tkind[e] == 1) kind[x[2]] = sc == SC_STORAGE_BUFFER ? 2 : 1;   // (a Uniform BufferBlock array is a storage buffer array)
                else if (sc == SC_UNIFORM_CONSTANT && tkind[e] == 3) kind[x[2]] = 3;
                else if (sc == SC_UNIFORM_CONSTANT && tkind[e] == 2) kind[x[2]] = 3;   // separate sampled images
                else if (sc == SC_UNIFORM_CONSTANT && tkind[e] == 5) kind[x[2]] = 4;   // storage images
            }
        }
        if (!ann_end && (op == OP_TYPE_INT || op == OP_TYPE_FLOAT || op == OP_TYPE_VECTOR || op == OP_TYPE_POINTER || op == OP_TYPE_STRUCT || op == OP_TYPE_ARRAY || op == OP_TYPE_IMAGE ||
                         op == OP_TYPE_SAMPLER || op == OP_TYPE_SAMPLED_IMAGE || op == OP_TYPE_RTARRAY || op == OP_CONSTANT || (op >= 19 && op <= 39))) ann_end = i;
        i += len;
    }
    for (size_t i = 5; i < n; i += w[i] >> 16) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; const uint32_t *x = w + i;
        if ((op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN) && len >= 5 && x[3] < bound && kind[x[3]] && !(kind[x[3]] & 0x80) && x[4] < bound && !cst[x[4]]) {
            if (x[2] < bound && !dec[x[2]]) { dec[x[2]] = 1; ndec++; }
            *kinds |= 1u << (kind[x[3]] - 1);
            kind[x[2]] = 0x80 | kind[x[3]];   // (loads through it are decorated too)
        } else if (op == OP_LOAD && len >= 4 && x[3] < bound && (kind[x[3]] & 0x80) && x[2] < bound && !dec[x[2]]) { dec[x[2]] = 1; ndec++; }
    }
    if (!ndec) goto out;
    if (!ann_end) { snprintf(why, wn, "no type section"); goto out; }
    int ver15 = (w[1] >> 16 & 0xff) > 1 || (w[1] >> 8 & 0xff) >= 5;
    uint32_t caps[5]; int nc = 0;
    if (!have_cap[0]) caps[nc++] = CAP_SHADER_NON_UNIFORM;
    if ((*kinds & 1) && !have_cap[1]) caps[nc++] = CAP_UBO_NU;
    if ((*kinds & 2) && !have_cap[2]) caps[nc++] = CAP_SSBO_NU;
    if ((*kinds & 4) && !have_cap[3]) caps[nc++] = CAP_SAMPLED_NU;
    if ((*kinds & 8) && !have_cap[4]) caps[nc++] = CAP_STORAGE_IMAGE_NU;
    static const char extname[] = "SPV_EXT_descriptor_indexing";   // 27 characters + NUL: 7 words
    int addext = !ver15 && !have_ext;
    res = malloc((n + nc * 2 + (addext ? 8 : 0) + ndec * 3) * 4);
    if (!res) { snprintf(why, wn, "out of memory"); goto out; }
    if (!cap_end) cap_end = 5;
    size_t o = 0;
    memcpy(res, w, cap_end * 4); o = cap_end;
    for (int k = 0; k < nc; k++) { res[o++] = (2u << 16) | OP_CAPABILITY; res[o++] = caps[k]; }
    if (addext) { res[o++] = (8u << 16) | OP_EXTENSION; memset(res + o, 0, 28); memcpy(res + o, extname, sizeof extname); o += 7; }
    memcpy(res + o, w + cap_end, (ann_end - cap_end) * 4); o += ann_end - cap_end;
    for (uint32_t id = 1; id < bound; id++) if (dec[id] == 1) { res[o++] = (3u << 16) | OP_DECORATE; res[o++] = id; res[o++] = DEC_NON_UNIFORM; }
    memcpy(res + o, w + ann_end, (n - ann_end) * 4); o += n - ann_end;
    *out_n = o;
out:
    free(cst); free(kind); free(dec); free(ptype); free(elem); free(tkind); free(block);
    return res;
}
// Dual-source blending from output location 1 (docs/reference/rendering.md "Dual-source blending from output location 1"): Hagi's GX fragment shaders write the
// second blend source as an ordinary output at location 1 (no Index decoration) and their pipelines blend with SRC1 factors. NVIDIA's hardware feeds colour target 1 to
// source 1; Vulkan takes source 1 only from Location 0 Index 1, and leaves it undefined otherwise (Qualcomm's driver drew Peach's dress see-through, Turnip happened to
// read target 1). For a pipeline that blends with SRC1, its fragment module's output at location 1 becomes Location 0 Index 1, when the module has outputs at
// locations 0 and 1 only. NULL: nothing to change (why "") or not done
uint32_t *vkg_spv_dual(const uint32_t *w, size_t n, size_t *out_n, char *why, size_t wn) {
    enum { DEC_INDEX = 32, SC_OUTPUT = 3 };
    why[0] = 0;
    if (n < 5 || w[0] != 0x07230203u) { snprintf(why, wn, "not SPIR-V"); return NULL; }
    uint32_t bound = w[3];
    if (!bound || bound > (1u << 22)) { snprintf(why, wn, "id bound %u", bound); return NULL; }
    uint8_t *out = calloc(bound, 1); int32_t *loc = malloc(bound * sizeof *loc); uint8_t *idx = calloc(bound, 1);
    uint32_t *res = NULL; size_t at1 = 0; uint32_t v1 = 0; int frag = 0, bad = 0;
    if (!out || !loc || !idx) { snprintf(why, wn, "out of memory"); goto out; }
    for (uint32_t i = 0; i < bound; i++) loc[i] = -1;
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; const uint32_t *x = w + i;
        if (!len || i + len > n) { snprintf(why, wn, "bad instruction length"); goto out; }
        if (op == OP_ENTRY_POINT && len >= 3 && x[1] == 4) frag = 1;
        else if (op == OP_DECORATE && len >= 4 && x[1] < bound && x[2] == DEC_LOCATION) loc[x[1]] = (int32_t)x[3];
        else if (op == OP_DECORATE && len >= 3 && x[1] < bound && x[2] == DEC_INDEX) idx[x[1]] = 1;
        else if (op == OP_VARIABLE && len >= 4 && x[2] < bound && x[3] == SC_OUTPUT) out[x[2]] = 1;
        i += len;
    }
    if (!frag) goto out;
    int have0 = 0;
    for (uint32_t id = 1; id < bound; id++) {
        if (!out[id] || loc[id] < 0) continue;
        if (idx[id]) goto out;   // (the module says it already)
        if (loc[id] == 0) have0 = 1; else if (loc[id] == 1) v1 = id; else bad = 1;
    }
    if (!v1 || !have0) goto out;
    if (bad) { snprintf(why, wn, "outputs at locations past 1"); goto out; }
    for (size_t i = 5; i < n; i += w[i] >> 16) if ((w[i] & 0xffff) == OP_DECORATE && (w[i] >> 16) >= 4 && w[i + 1] == v1 && w[i + 2] == DEC_LOCATION) { at1 = i; break; }
    res = malloc((n + 4) * 4);
    if (!res) { snprintf(why, wn, "out of memory"); goto out; }
    memcpy(res, w, at1 * 4);
    res[at1] = w[at1]; res[at1 + 1] = v1; res[at1 + 2] = DEC_LOCATION; res[at1 + 3] = 0;   // Location 0
    res[at1 + 4] = (4u << 16) | OP_DECORATE; res[at1 + 5] = v1; res[at1 + 6] = DEC_INDEX; res[at1 + 7] = 1;   // Index 1
    memcpy(res + at1 + 8, w + at1 + 4, (n - at1 - 4) * 4);
    *out_n = n + 4;
out:
    free(out); free(loc); free(idx);
    return res;
}
// ArrayStride on a struct member (docs/reference/rendering.md "Vulkan shaders generated at run time"): Hagi's generated GX shaders (Super Mario Galaxy, Sunshine) put
// it on the member (OpMemberDecorate) instead of the array type, which NVIDIA's driver takes and Vulkan does not allow. Each such stride is copied onto the member's array
// type (upstream's vk_shaders.py fix_strides) and the member decoration is dropped, so the module is valid. NULL: unchanged
uint32_t *vkg_spv_strides(const uint32_t *w, size_t n, size_t *out_n) {
    enum { OP_TYPE_ARRAY = 28, OP_TYPE_RTARRAY = 29, OP_TYPE_STRUCT = 30, OP_MEMBER_DECORATE = 72, DEC_ARRAY_STRIDE = 6 };
    if (n < 5 || w[0] != 0x07230203u) return NULL;
    size_t at = 0, nmem = 0;
    for (size_t i = 5; i < n;) { uint32_t op = w[i] & 0xffff, len = w[i] >> 16; if (!len || i + len > n) return NULL;
        if (op == OP_MEMBER_DECORATE && len >= 5 && w[i + 3] == DEC_ARRAY_STRIDE) { at = i + len; nmem++; }
        i += len; }
    if (!nmem) return NULL;
    uint32_t bound = w[3]; if (!bound || bound > (1u << 22)) return NULL;
    uint8_t *is_arr = calloc(bound, 1), *has = calloc(bound, 1); uint32_t *add = calloc(bound, 4); const uint32_t **st = calloc(bound, sizeof *st); uint32_t *stn = calloc(bound, 4);
    uint32_t *res = NULL; size_t nadd = 0;
    if (!is_arr || !has || !add || !st || !stn) goto out;
    for (size_t i = 5; i < n; i += w[i] >> 16) { uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == OP_DECORATE && len >= 4 && w[i + 2] == DEC_ARRAY_STRIDE && w[i + 1] < bound) has[w[i + 1]] = 1;
        else if ((op == OP_TYPE_ARRAY || op == OP_TYPE_RTARRAY) && len >= 3 && w[i + 1] < bound) is_arr[w[i + 1]] = 1;
        else if (op == OP_TYPE_STRUCT && len >= 2 && w[i + 1] < bound) { st[w[i + 1]] = w + i + 2; stn[w[i + 1]] = len - 2; } }
    for (size_t i = 5; i < n; i += w[i] >> 16) { uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op != OP_MEMBER_DECORATE || len < 5 || w[i + 3] != DEC_ARRAY_STRIDE || w[i + 1] >= bound) continue;
        uint32_t s = w[i + 1], m = w[i + 2]; if (!st[s] || m >= stn[s]) continue;
        uint32_t t = st[s][m]; if (t < bound && is_arr[t] && !has[t] && !add[t]) { add[t] = w[i + 4]; nadd++; } }
    res = malloc((n + nadd * 4) * 4); if (!res) goto out;
    size_t o = 0; memcpy(res, w, 5 * 4); o = 5;
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (i == at) for (uint32_t t = 0; t < bound; t++) if (add[t]) { res[o++] = (4u << 16) | OP_DECORATE; res[o++] = t; res[o++] = DEC_ARRAY_STRIDE; res[o++] = add[t]; }
        if (!(op == OP_MEMBER_DECORATE && len >= 5 && w[i + 3] == DEC_ARRAY_STRIDE)) { memcpy(res + o, w + i, len * 4); o += len; }   // (the member decoration dropped)
        i += len;
    }
    if (at >= n) for (uint32_t t = 0; t < bound; t++) if (add[t]) { res[o++] = (4u << 16) | OP_DECORATE; res[o++] = t; res[o++] = DEC_ARRAY_STRIDE; res[o++] = add[t]; }
    *out_n = o;
out:
    free(is_arr); free(has); free(add); free(st); free(stn);
    return res;
}