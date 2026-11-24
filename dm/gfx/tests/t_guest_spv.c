// The guest Vulkan layer's SPIR-V rewrites (dm/gfx/vk_android/vka_guest_vin.c), on modules assembled here: scaled vertex inputs read as integers and converted, per-invocation
// descriptor array indices decorated NonUniform, dual-source output at location 1 moved to Location 0 Index 1, ArrayStride moved from struct members to the array type.
// Each case checks the rewritten module's instructions; the cases that must stay unchanged check that nothing is returned. make -C dm/gfx/tests t_guest_spv && ./t_guest_spv
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t *vkg_spv_scaled(const uint32_t *w, size_t n, const uint8_t conv[32], size_t *out_n, char *why, size_t wn);
uint32_t *vkg_spv_nonuniform(const uint32_t *w, size_t n, size_t *out_n, unsigned *kinds, char *why, size_t wn);
uint32_t *vkg_spv_dual(const uint32_t *w, size_t n, size_t *out_n, char *why, size_t wn);
uint32_t *vkg_spv_strides(const uint32_t *w, size_t n, size_t *out_n);

static int fail, total;
static void ok(const char *what, int cond) { total++; if (!cond) fail++; printf("%-78s %s\n", what, cond ? "ok" : "FAILED"); }

// A tiny assembler: words in m[], the header first
typedef struct { uint32_t w[512]; size_t n; } mod;
static void hdr(mod *m, uint32_t version, uint32_t bound) { m->n = 0; m->w[m->n++] = 0x07230203u; m->w[m->n++] = version; m->w[m->n++] = 0; m->w[m->n++] = bound; m->w[m->n++] = 0; }
static void ins(mod *m, uint32_t op, int nw, ...) {
    __builtin_va_list a; __builtin_va_start(a, nw);
    m->w[m->n++] = ((uint32_t)(nw + 1) << 16) | op;
    for (int i = 0; i < nw; i++) m->w[m->n++] = __builtin_va_arg(a, uint32_t);
    __builtin_va_end(a);
}
static const uint32_t MAIN0 = 0x6e69616d, MAIN1 = 0;   // "main\0" in two words
// The instructions of a module: count those with opcode op whose operands start with the given words (ANY matches any)
#define ANY 0xffffffffu
static int count(const uint32_t *w, size_t n, uint32_t op, int nw, const uint32_t *pre) {
    int c = 0;
    for (size_t i = 5; i < n && (w[i] >> 16); i += w[i] >> 16) {
        if ((w[i] & 0xffff) != op || (int)(w[i] >> 16) < nw + 1) continue;
        int m = 1; for (int k = 0; k < nw; k++) if (pre[k] != ANY && w[i + 1 + k] != pre[k]) m = 0;
        c += m;
    }
    return c;
}
static uint32_t find_result(const uint32_t *w, size_t n, uint32_t op, int at, int nw, const uint32_t *pre) {   // the word at operand `at` of the first match
    for (size_t i = 5; i < n && (w[i] >> 16); i += w[i] >> 16) {
        if ((w[i] & 0xffff) != op || (int)(w[i] >> 16) < nw + 1) continue;
        int m = 1; for (int k = 0; k < nw; k++) if (pre[k] != ANY && w[i + 1 + k] != pre[k]) m = 0;
        if (m) return w[i + 1 + at];
    }
    return 0;
}
enum { CAP = 17, MEMMODEL = 14, ENTRY = 15, EXECMODE = 16, DECORATE = 71, MEMBER_DECORATE = 72, TVOID = 19, TFN = 33, TINT = 21, TFLOAT = 22, TVEC = 23, TARRAY = 28, TSTRUCT = 30,
       TPTR = 32, CONST = 43, VAR = 59, LOAD = 61, STORE = 62, ACHAIN = 65, FUNC = 54, LABEL = 248, RET = 253, FEND = 56, STOF = 111, UTOF = 112, EXT = 10 };

static void t_scaled(void) {
    // vertex shader: in vec4 a (location 0, fed SSCALED), in vec2 b (location 1, unsigned scaled, read through a component access chain), in vec3 c (location 2, stays)
    // ids: 1 void 2 fn 3 float 4 vec4 5 ptr vec4 6 a 7 main 8 label 9 load a 10 vec2 11 ptr vec2 12 b 13 vec3 14 ptr vec3 15 c 16 load c 17 ptr float 18 uint 19 const 0 20 ac b.x 21 load b.x
    mod m; hdr(&m, 0x10000, 22);
    ins(&m, CAP, 1, 1); ins(&m, MEMMODEL, 2, 0, 1); ins(&m, ENTRY, 7, 0, 7, MAIN0, MAIN1, 6, 12, 15);
    ins(&m, DECORATE, 3, 6, 30, 0); ins(&m, DECORATE, 3, 12, 30, 1); ins(&m, DECORATE, 3, 15, 30, 2);
    ins(&m, TVOID, 1, 1); ins(&m, TFN, 2, 2, 1); ins(&m, TFLOAT, 2, 3, 32); ins(&m, TVEC, 3, 4, 3, 4); ins(&m, TPTR, 3, 5, 1, 4);
    ins(&m, TVEC, 3, 10, 3, 2); ins(&m, TPTR, 3, 11, 1, 10); ins(&m, TVEC, 3, 13, 3, 3); ins(&m, TPTR, 3, 14, 1, 13); ins(&m, TPTR, 3, 17, 1, 3);
    ins(&m, TINT, 3, 18, 32, 0); ins(&m, CONST, 3, 18, 19, 0);
    ins(&m, VAR, 3, 5, 6, 1); ins(&m, VAR, 3, 11, 12, 1); ins(&m, VAR, 3, 14, 15, 1);
    ins(&m, FUNC, 4, 1, 7, 0, 2); ins(&m, LABEL, 1, 8); ins(&m, LOAD, 3, 4, 9, 6); ins(&m, LOAD, 3, 13, 16, 15); ins(&m, ACHAIN, 4, 17, 20, 12, 19); ins(&m, LOAD, 3, 3, 21, 20);
    ins(&m, RET, 0); ins(&m, FEND, 0);
    uint8_t conv[32] = {0}; conv[0] = 2; conv[1] = 1; char why[128]; size_t on = 0;
    uint32_t *o = vkg_spv_scaled(m.w, m.n, conv, &on, why, sizeof why);
    ok("scaled: the module is rewritten", o != NULL);
    if (!o) { printf("  why: %s\n", why); return; }
    uint32_t sint = find_result(o, on, TINT, 0, 3, (uint32_t[]){ANY, 32, 1});
    ok("scaled: a signed 32-bit integer type is declared", sint != 0);
    ok("scaled: the existing unsigned integer type is reused, not declared again", count(o, on, TINT, 2, (uint32_t[]){ANY, 32}) == 2 && count(o, on, TINT, 3, (uint32_t[]){18, 32, 0}) == 1);
    uint32_t ivec4 = find_result(o, on, TVEC, 0, 3, (uint32_t[]){ANY, sint, 4});
    ok("scaled: an ivec4 type for location 0", ivec4 != 0);
    uint32_t pv = find_result(o, on, TPTR, 0, 3, (uint32_t[]){ANY, 1, ivec4});
    ok("scaled: location 0's variable is an Input ivec4", count(o, on, VAR, 3, (uint32_t[]){pv, 6, 1}) == 1);
    uint32_t tmp = find_result(o, on, LOAD, 1, 3, (uint32_t[]){ivec4, ANY, 6});
    ok("scaled: location 0 is loaded as ivec4 and converted with OpConvertSToF into the load's id", tmp && count(o, on, STOF, 3, (uint32_t[]){4, 9, tmp}) == 1);
    uint32_t uvec2 = find_result(o, on, TVEC, 0, 3, (uint32_t[]){ANY, 18, 2}), pu = find_result(o, on, TPTR, 0, 3, (uint32_t[]){ANY, 1, uvec2});
    ok("scaled: location 1's variable is an Input uvec2", uvec2 && count(o, on, VAR, 3, (uint32_t[]){pu, 12, 1}) == 1);
    uint32_t pus = find_result(o, on, TPTR, 0, 3, (uint32_t[]){ANY, 1, 18});
    ok("scaled: its component access chain points at an unsigned integer", pus && count(o, on, ACHAIN, 4, (uint32_t[]){pus, 20, 12, 19}) == 1);
    uint32_t t2 = find_result(o, on, LOAD, 1, 3, (uint32_t[]){18, ANY, 20});
    ok("scaled: the component is loaded as uint and converted with OpConvertUToF", t2 && count(o, on, UTOF, 3, (uint32_t[]){3, 21, t2}) == 1);
    ok("scaled: location 2 stays a float input, loaded as before", count(o, on, VAR, 3, (uint32_t[]){14, 15, 1}) == 1 && count(o, on, LOAD, 3, (uint32_t[]){13, 16, 15}) == 1);
    ok("scaled: the id bound covers the new ids", o[3] > 22 && o[3] <= 22 + 8);
    free(o);
    uint8_t none[32] = {0};
    ok("scaled: nothing to convert gives no module", !vkg_spv_scaled(m.w, m.n, none, &on, why, sizeof why) && !why[0]);
}

static void t_nonuniform(void) {
    // ids: 1 void 2 fn 3 float 4 vec4 5 uint 6 c0 7 c4 8 block 9 block[4] 10 ptr Uniform arr 11 var 12 ptr Uniform vec4 13 ptr Input uint 14 idx var 15 main 16 label
    //      17 load idx 18 ac (non-uniform) 19 load 20 ac (constant) 21 load
    mod m; hdr(&m, 0x10000, 22);
    ins(&m, CAP, 1, 1); ins(&m, MEMMODEL, 2, 0, 1); ins(&m, ENTRY, 5, 0, 15, MAIN0, MAIN1, 14);
    ins(&m, DECORATE, 2, 8, 2); ins(&m, DECORATE, 3, 14, 30, 0);
    ins(&m, TVOID, 1, 1); ins(&m, TFN, 2, 2, 1); ins(&m, TFLOAT, 2, 3, 32); ins(&m, TVEC, 3, 4, 3, 4); ins(&m, TINT, 3, 5, 32, 0); ins(&m, CONST, 3, 5, 6, 0); ins(&m, CONST, 3, 5, 7, 4);
    ins(&m, TSTRUCT, 2, 8, 4); ins(&m, TARRAY, 3, 9, 8, 7); ins(&m, TPTR, 3, 10, 2, 9); ins(&m, VAR, 3, 10, 11, 2); ins(&m, TPTR, 3, 12, 2, 4); ins(&m, TPTR, 3, 13, 1, 5); ins(&m, VAR, 3, 13, 14, 1);
    ins(&m, FUNC, 4, 1, 15, 0, 2); ins(&m, LABEL, 1, 16); ins(&m, LOAD, 3, 5, 17, 14);
    ins(&m, ACHAIN, 5, 12, 18, 11, 17, 6); ins(&m, LOAD, 3, 4, 19, 18); ins(&m, ACHAIN, 5, 12, 20, 11, 7, 6); ins(&m, LOAD, 3, 4, 21, 20);
    ins(&m, RET, 0); ins(&m, FEND, 0);
    char why[128]; size_t on = 0; unsigned kinds = 0;
    uint32_t *o = vkg_spv_nonuniform(m.w, m.n, &on, &kinds, why, sizeof why);
    ok("nonuniform: the module is rewritten", o != NULL);
    if (!o) { printf("  why: %s\n", why); return; }
    ok("nonuniform: the kind is a uniform buffer array", kinds == 1);
    ok("nonuniform: the access chain with the loaded index is decorated NonUniform", count(o, on, DECORATE, 2, (uint32_t[]){18, 5300}) == 1);
    ok("nonuniform: the load through it is decorated too", count(o, on, DECORATE, 2, (uint32_t[]){19, 5300}) == 1);
    ok("nonuniform: the constant-index access chain is left alone", !count(o, on, DECORATE, 2, (uint32_t[]){20, 5300}) && !count(o, on, DECORATE, 2, (uint32_t[]){21, 5300}));
    ok("nonuniform: capabilities ShaderNonUniform and UniformBufferArrayNonUniformIndexing", count(o, on, CAP, 1, (uint32_t[]){5301}) == 1 && count(o, on, CAP, 1, (uint32_t[]){5306}) == 1);
    ok("nonuniform: SPV_EXT_descriptor_indexing below SPIR-V 1.5", count(o, on, EXT, 1, (uint32_t[]){0x5f565053}) == 1);
    ok("nonuniform: two decorations added, no other", count(o, on, DECORATE, 0, NULL) == 4);
    free(o);
    m.w[1] = 0x10500;   // SPIR-V 1.5: no extension
    o = vkg_spv_nonuniform(m.w, m.n, &on, &kinds, why, sizeof why);
    ok("nonuniform: SPIR-V 1.5 takes no extension", o && !count(o, on, EXT, 0, NULL));
    free(o);
}

static void t_dual(void) {
    // fragment shader: outputs a (location 0), b (location 1). ids: 1 void 2 fn 3 float 4 vec4 5 ptr Output vec4 6 a 7 b 8 main 9 label 10 c (location 2, second module)
    mod m; hdr(&m, 0x10000, 11);
    ins(&m, CAP, 1, 1); ins(&m, MEMMODEL, 2, 0, 1); ins(&m, ENTRY, 6, 4, 8, MAIN0, MAIN1, 6, 7); ins(&m, EXECMODE, 2, 8, 7);
    ins(&m, DECORATE, 3, 6, 30, 0); ins(&m, DECORATE, 3, 7, 30, 1);
    ins(&m, TVOID, 1, 1); ins(&m, TFN, 2, 2, 1); ins(&m, TFLOAT, 2, 3, 32); ins(&m, TVEC, 3, 4, 3, 4); ins(&m, TPTR, 3, 5, 3, 4); ins(&m, VAR, 3, 5, 6, 3); ins(&m, VAR, 3, 5, 7, 3);
    ins(&m, FUNC, 4, 1, 8, 0, 2); ins(&m, LABEL, 1, 9); ins(&m, RET, 0); ins(&m, FEND, 0);
    char why[128]; size_t on = 0;
    uint32_t *o = vkg_spv_dual(m.w, m.n, &on, why, sizeof why);
    ok("dual: the module is rewritten", o != NULL);
    if (o) {
        ok("dual: location 1's output is at Location 0 Index 1", count(o, on, DECORATE, 3, (uint32_t[]){7, 30, 0}) == 1 && count(o, on, DECORATE, 3, (uint32_t[]){7, 32, 1}) == 1);
        ok("dual: no output is left at location 1", !count(o, on, DECORATE, 3, (uint32_t[]){ANY, 30, 1}));
        ok("dual: location 0's output keeps its decoration, without an index", count(o, on, DECORATE, 3, (uint32_t[]){6, 30, 0}) == 1 && !count(o, on, DECORATE, 3, (uint32_t[]){6, 32, ANY}));
        free(o);
    }
    mod m3; hdr(&m3, 0x10000, 11);   // with a third output at location 2: left as made
    ins(&m3, CAP, 1, 1); ins(&m3, MEMMODEL, 2, 0, 1); ins(&m3, ENTRY, 7, 4, 8, MAIN0, MAIN1, 6, 7, 10); ins(&m3, EXECMODE, 2, 8, 7);
    ins(&m3, DECORATE, 3, 6, 30, 0); ins(&m3, DECORATE, 3, 7, 30, 1); ins(&m3, DECORATE, 3, 10, 30, 2);
    ins(&m3, TVOID, 1, 1); ins(&m3, TFN, 2, 2, 1); ins(&m3, TFLOAT, 2, 3, 32); ins(&m3, TVEC, 3, 4, 3, 4); ins(&m3, TPTR, 3, 5, 3, 4); ins(&m3, VAR, 3, 5, 6, 3); ins(&m3, VAR, 3, 5, 7, 3); ins(&m3, VAR, 3, 5, 10, 3);
    ins(&m3, FUNC, 4, 1, 8, 0, 2); ins(&m3, LABEL, 1, 9); ins(&m3, RET, 0); ins(&m3, FEND, 0);
    ok("dual: outputs past location 1 leave the module as made, with the reason", !vkg_spv_dual(m3.w, m3.n, &on, why, sizeof why) && why[0]);
}

static void t_strides(void) {
    // ids: 1 float 2 uint 3 c4 4 float[4] 5 struct { float[4] }; the stride on member 0 of 5
    mod m; hdr(&m, 0x10000, 6);
    ins(&m, CAP, 1, 1); ins(&m, MEMMODEL, 2, 0, 1); ins(&m, MEMBER_DECORATE, 4, 5, 0, 6, 16); ins(&m, MEMBER_DECORATE, 4, 5, 0, 35, 0);
    ins(&m, TFLOAT, 2, 1, 32); ins(&m, TINT, 3, 2, 32, 0); ins(&m, CONST, 3, 2, 3, 4); ins(&m, TARRAY, 3, 4, 1, 3); ins(&m, TSTRUCT, 2, 5, 4);
    size_t on = 0; uint32_t *o = vkg_spv_strides(m.w, m.n, &on);
    ok("strides: the module is rewritten", o != NULL);
    if (!o) return;
    ok("strides: the array type is decorated ArrayStride 16", count(o, on, DECORATE, 3, (uint32_t[]){4, 6, 16}) == 1);
    ok("strides: the member's ArrayStride is dropped, its Offset kept", !count(o, on, MEMBER_DECORATE, 3, (uint32_t[]){5, 0, 6}) && count(o, on, MEMBER_DECORATE, 3, (uint32_t[]){5, 0, 35}) == 1);
    ok("strides: one word less (OpDecorate in place of OpMemberDecorate)", on == m.n - 1);
    free(o);
    mod v; hdr(&v, 0x10000, 6);   // already valid: unchanged
    ins(&v, CAP, 1, 1); ins(&v, MEMMODEL, 2, 0, 1); ins(&v, DECORATE, 3, 4, 6, 16); ins(&v, TFLOAT, 2, 1, 32); ins(&v, TINT, 3, 2, 32, 0); ins(&v, CONST, 3, 2, 3, 4); ins(&v, TARRAY, 3, 4, 1, 3);
    ok("strides: a valid module gives no new one", !vkg_spv_strides(v.w, v.n, &on));
}

int main(void) {
    t_scaled(); t_nonuniform(); t_dual(); t_strides();
    printf("%d of %d checks passed\n", total - fail, total);
    return fail != 0;
}
