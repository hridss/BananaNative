// Unit tests for standalone C11 SPIR-V rewrite dm/gfx/vk_android/vka_spv_gmem_replay.c
#include "../../gfx/vk_android/vka_spv_gmem_replay.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
    HK_WORD = 0, HK_WIDE = 1, HK_RUN = 2,
    SPV_EXT = 10, SPV_MEM = 14, SPV_ENTRY = 15, SPV_EXMODE = 16, SPV_CAP = 17,
    SPV_TVOID = 19, SPV_TINT = 21, SPV_TVEC = 23, SPV_TARR = 28,
    SPV_TSTRUCT = 30, SPV_TPTR = 32, SPV_TFN = 33, SPV_CONST = 43,
    SPV_FN = 54, SPV_PARAM = 55, SPV_FN_END = 56, SPV_CALL = 57, SPV_VAR = 59,
    SPV_LOAD = 61, SPV_STORE = 62, SPV_AC = 65, SPV_DEC = 71, SPV_MDEC = 72,
    SPV_COMP_CTOR = 80, SPV_COMP_EXT = 81, SPV_UCONV = 113, SPV_CVT_PTR = 120,
    SPV_LMERGE = 246, SPV_LABEL = 248, SPV_BR = 249, SPV_RET = 253, SPV_RETV = 254,
};

typedef struct {
    uint32_t w[4096];
    size_t n;
    uint32_t next_id;
    uint32_t t_void, t_u32, t_u64, t_v4, t_tbl, t_arr;
    uint32_t t_fn_main, t_fn_u32, t_fn_void, t_fn_v4;
    uint32_t p_priv_u32, p_priv_v4, p_out_u32, p_sbuf_tbl, p_sbuf_u32, p_phys_u32, p_phys_v4;
    uint32_t c0, c1, c2, c3, c16384;
    uint32_t v_tbl, v_out, v_priv_wide, v_priv_cache;
    uint32_t fn_helper, fn_surv, fn_main;
} spv_bld_t;

typedef struct {
    uint32_t version, exec_model, tbl_set, tbl_bind;
    int helper_kind, multi_block_main, priv_read_first, surviving_caller;
} mod_spec_t;

static void check_cond(int *fail, int cond, int line, const char *msg) {
    if (!cond) {
        (*fail)++;
        printf("FAIL t_android_gmem_replay.c:%d: %s\n", line, msg);
    }
}
#define CHECK(c, msg) check_cond(fail, (c), __LINE__, (msg))

static uint32_t nid(spv_bld_t *b) {
    assert(b->next_id < 4096u);
    return b->next_id++;
}

static void emit(spv_bld_t *b, uint32_t op, const uint32_t *ops, uint32_t nops) {
    assert(b->n + 1u + nops <= 4096u);
    b->w[b->n++] = ((nops + 1u) << 16) | (op & 0xffffu);
    for (uint32_t i = 0; i < nops; i++) b->w[b->n++] = ops[i];
}

static void emit_str(spv_bld_t *b, uint32_t op, const char *s) {
    uint32_t ops[16] = {0};
    size_t len = strlen(s) + 1u;
    size_t nw = (len + 3u) / 4u;
    assert(nw <= 16u);
    memcpy(ops, s, len);
    emit(b, op, ops, (uint32_t)nw);
}

static void init_builder_ids(spv_bld_t *b, const mod_spec_t *sp) {
    memset(b, 0, sizeof *b);
    b->next_id = 1u;
    b->w[b->n++] = 0x07230203u;
    b->w[b->n++] = sp->version ? sp->version : 0x00010300u;
    b->w[b->n++] = 0u;
    b->w[b->n++] = 0u;
    b->w[b->n++] = 0u;
    emit(b, SPV_CAP, (const uint32_t[]){1u}, 1u);
    emit(b, SPV_CAP, (const uint32_t[]){11u}, 1u);
    emit(b, SPV_CAP, (const uint32_t[]){5347u}, 1u);
    emit_str(b, SPV_EXT, "SPV_KHR_physical_storage_buffer");
    emit(b, SPV_MEM, (const uint32_t[]){5348u, 1u}, 2u);
    b->fn_main = nid(b); b->fn_helper = nid(b); b->fn_surv = nid(b);
    b->v_out = nid(b); b->v_tbl = nid(b);
    b->v_priv_wide = nid(b); b->v_priv_cache = nid(b);
    if (sp->exec_model == 0u) {
        emit(b, SPV_ENTRY, (const uint32_t[]){0u, b->fn_main, 0x6e69616du, 0u, b->v_out}, 5u);
    } else {
        emit(b, SPV_ENTRY, (const uint32_t[]){sp->exec_model, b->fn_main, 0x6e69616du, 0u}, 4u);
        emit(b, SPV_EXMODE, (const uint32_t[]){b->fn_main, 17u, 1u, 1u, 1u}, 5u);
    }
}

static void emit_decls(spv_bld_t *b, const mod_spec_t *sp) {
    b->t_void = nid(b); b->t_u32 = nid(b); b->t_u64 = nid(b);
    b->t_v4 = nid(b); b->t_arr = nid(b); b->t_tbl = nid(b);
    if (sp->exec_model == 0u) emit(b, SPV_DEC, (const uint32_t[]){b->v_out, 30u, 0u}, 3u);
    emit(b, SPV_DEC, (const uint32_t[]){b->t_arr, 6u, 4u}, 3u);
    emit(b, SPV_DEC, (const uint32_t[]){b->t_tbl, 2u}, 2u);
    emit(b, SPV_MDEC, (const uint32_t[]){b->t_tbl, 0u, 35u, 0u}, 4u);
    emit(b, SPV_DEC, (const uint32_t[]){b->v_tbl, 34u, sp->tbl_set}, 3u);
    emit(b, SPV_DEC, (const uint32_t[]){b->v_tbl, 33u, sp->tbl_bind}, 3u);
    emit(b, SPV_DEC, (const uint32_t[]){b->v_tbl, 24u}, 2u);
    emit(b, SPV_TVOID, &b->t_void, 1u);
    emit(b, SPV_TINT, (const uint32_t[]){b->t_u32, 32u, 0u}, 3u);
    emit(b, SPV_TINT, (const uint32_t[]){b->t_u64, 64u, 0u}, 3u);
    emit(b, SPV_TVEC, (const uint32_t[]){b->t_v4, b->t_u32, 4u}, 3u);
    b->c0 = nid(b); emit(b, SPV_CONST, (const uint32_t[]){b->t_u32, b->c0, 0u}, 3u);
    b->c1 = nid(b); emit(b, SPV_CONST, (const uint32_t[]){b->t_u32, b->c1, 1u}, 3u);
    b->c2 = nid(b); emit(b, SPV_CONST, (const uint32_t[]){b->t_u32, b->c2, 2u}, 3u);
    b->c3 = nid(b); emit(b, SPV_CONST, (const uint32_t[]){b->t_u32, b->c3, 3u}, 3u);
    b->c16384 = nid(b); emit(b, SPV_CONST, (const uint32_t[]){b->t_u32, b->c16384, 16384u}, 3u);
    emit(b, SPV_TARR, (const uint32_t[]){b->t_arr, b->t_u32, b->c16384}, 3u);
    emit(b, SPV_TSTRUCT, (const uint32_t[]){b->t_tbl, b->t_arr}, 2u);
    b->p_sbuf_tbl = nid(b); emit(b, SPV_TPTR, (const uint32_t[]){b->p_sbuf_tbl, 12u, b->t_tbl}, 3u);
    b->p_sbuf_u32 = nid(b); emit(b, SPV_TPTR, (const uint32_t[]){b->p_sbuf_u32, 12u, b->t_u32}, 3u);
    b->p_priv_u32 = nid(b); emit(b, SPV_TPTR, (const uint32_t[]){b->p_priv_u32, 6u, b->t_u32}, 3u);
    b->p_priv_v4 = nid(b); emit(b, SPV_TPTR, (const uint32_t[]){b->p_priv_v4, 6u, b->t_v4}, 3u);
    b->p_out_u32 = nid(b); emit(b, SPV_TPTR, (const uint32_t[]){b->p_out_u32, 3u, b->t_u32}, 3u);
    b->p_phys_u32 = nid(b); emit(b, SPV_TPTR, (const uint32_t[]){b->p_phys_u32, 5349u, b->t_u32}, 3u);
    b->p_phys_v4 = nid(b); emit(b, SPV_TPTR, (const uint32_t[]){b->p_phys_v4, 5349u, b->t_v4}, 3u);
    b->t_fn_main = nid(b); emit(b, SPV_TFN, (const uint32_t[]){b->t_fn_main, b->t_void}, 2u);
    b->t_fn_u32 = nid(b); emit(b, SPV_TFN, (const uint32_t[]){b->t_fn_u32, b->t_u32, b->t_u32, b->t_u32}, 4u);
    b->t_fn_void = nid(b); emit(b, SPV_TFN, (const uint32_t[]){b->t_fn_void, b->t_void, b->t_u32, b->t_u32}, 4u);
    b->t_fn_v4 = nid(b); emit(b, SPV_TFN, (const uint32_t[]){b->t_fn_v4, b->t_v4, b->t_u32, b->t_u32}, 4u);
    emit(b, SPV_VAR, (const uint32_t[]){b->p_sbuf_tbl, b->v_tbl, 12u}, 3u);
    if (sp->exec_model == 0u) emit(b, SPV_VAR, (const uint32_t[]){b->p_out_u32, b->v_out, 3u}, 3u);
    emit(b, SPV_VAR, (const uint32_t[]){b->p_priv_v4, b->v_priv_wide, 6u}, 3u);
    emit(b, SPV_VAR, (const uint32_t[]){b->p_priv_u32, b->v_priv_cache, 6u, b->c0}, 4u);
}

static uint32_t helper_type_for(const spv_bld_t *b, int kind, uint32_t *ret_ty) {
    if (kind == HK_WORD || kind == HK_WIDE) { *ret_ty = b->t_u32; return b->t_fn_u32; }
    *ret_ty = b->t_v4;
    return b->t_fn_v4;
}

static void emit_helper_body(spv_bld_t *b, int kind, uint32_t lo, uint32_t ac) {
    if (kind == HK_WORD) {
        uint32_t addr = nid(b), ptr = nid(b), ld = nid(b);
        emit(b, SPV_UCONV, (const uint32_t[]){b->t_u64, addr, lo}, 3u);
        emit(b, SPV_CVT_PTR, (const uint32_t[]){b->p_phys_u32, ptr, addr}, 3u);
        emit(b, SPV_LOAD, (const uint32_t[]){b->t_u32, ld, ptr, 2u, 4u}, 5u);
        emit(b, SPV_RETV, &ld, 1u);
    } else if (kind == HK_WIDE) {
        uint32_t addr = nid(b), ptr = nid(b), ld = nid(b), e0 = nid(b);
        uint32_t idx[3] = {b->c1, b->c2, b->c3};
        emit(b, SPV_UCONV, (const uint32_t[]){b->t_u64, addr, lo}, 3u);
        emit(b, SPV_CVT_PTR, (const uint32_t[]){b->p_phys_v4, ptr, addr}, 3u);
        emit(b, SPV_LOAD, (const uint32_t[]){b->t_v4, ld, ptr, 2u, 16u}, 5u);
        emit(b, SPV_COMP_EXT, (const uint32_t[]){b->t_u32, e0, ld, 0u}, 4u);
        for (uint32_t lane = 0; lane < 3u; lane++) {
            uint32_t ex = nid(b), pp = nid(b);
            emit(b, SPV_COMP_EXT, (const uint32_t[]){b->t_u32, ex, ld, lane + 1u}, 4u);
            emit(b, SPV_AC, (const uint32_t[]){b->p_priv_u32, pp, b->v_priv_wide, idx[lane]}, 4u);
            emit(b, SPV_STORE, (const uint32_t[]){pp, ex}, 2u);
        }
        emit(b, SPV_RETV, &e0, 1u);
    } else {
        for (int i = 0; i < 6; i++) {
            uint32_t pl = nid(b);
            emit(b, SPV_LOAD, (const uint32_t[]){b->t_u32, pl, b->v_priv_cache}, 3u);
        }
        uint32_t a0 = nid(b), p0 = nid(b), l0 = nid(b), a1 = nid(b), p1 = nid(b), l1 = nid(b), t0 = nid(b), v = nid(b);
        emit(b, SPV_LOAD, (const uint32_t[]){b->t_u32, t0, ac}, 3u);
        emit(b, SPV_UCONV, (const uint32_t[]){b->t_u64, a0, lo}, 3u);
        emit(b, SPV_CVT_PTR, (const uint32_t[]){b->p_phys_u32, p0, a0}, 3u);
        emit(b, SPV_LOAD, (const uint32_t[]){b->t_u32, l0, p0, 2u, 4u}, 5u);
        emit(b, SPV_UCONV, (const uint32_t[]){b->t_u64, a1, t0}, 3u);
        emit(b, SPV_CVT_PTR, (const uint32_t[]){b->p_phys_u32, p1, a1}, 3u);
        emit(b, SPV_LOAD, (const uint32_t[]){b->t_u32, l1, p1, 2u, 4u}, 5u);
        emit(b, SPV_COMP_CTOR, (const uint32_t[]){b->t_v4, v, l0, l1, b->c0, t0}, 6u);
        emit(b, SPV_RETV, &v, 1u);
    }
}

static void emit_helper_fn(spv_bld_t *b, const mod_spec_t *sp) {
    uint32_t ret_ty = 0, fn_ty = helper_type_for(b, sp->helper_kind, &ret_ty);
    uint32_t lo = nid(b), hi = nid(b), lbl = nid(b), ac = nid(b);
    emit(b, SPV_FN, (const uint32_t[]){ret_ty, b->fn_helper, 0u, fn_ty}, 4u);
    emit(b, SPV_PARAM, (const uint32_t[]){b->t_u32, lo}, 2u);
    emit(b, SPV_PARAM, (const uint32_t[]){b->t_u32, hi}, 2u);
    emit(b, SPV_LABEL, &lbl, 1u);
    emit(b, SPV_AC, (const uint32_t[]){b->p_sbuf_u32, ac, b->v_tbl, b->c0, b->c0}, 5u);
    emit_helper_body(b, sp->helper_kind, lo, ac);
    emit(b, SPV_FN_END, NULL, 0u);
    if (sp->surviving_caller) {
        uint32_t sl = nid(b), sh = nid(b), slbl = nid(b), call_r = nid(b);
        emit(b, SPV_FN, (const uint32_t[]){ret_ty, b->fn_surv, 0u, fn_ty}, 4u);
        emit(b, SPV_PARAM, (const uint32_t[]){b->t_u32, sl}, 2u);
        emit(b, SPV_PARAM, (const uint32_t[]){b->t_u32, sh}, 2u);
        emit(b, SPV_LABEL, &slbl, 1u);
        emit(b, SPV_CALL, (const uint32_t[]){ret_ty, call_r, b->fn_helper, sl, sh}, 5u);
        if (ret_ty == b->t_void) emit(b, SPV_RET, NULL, 0u);
        else emit(b, SPV_RETV, &call_r, 1u);
        emit(b, SPV_FN_END, NULL, 0u);
    }
}

static void build_module(spv_bld_t *b, const mod_spec_t *sp) {
    init_builder_ids(b, sp);
    emit_decls(b, sp);
    emit_helper_fn(b, sp);
    uint32_t ret_ty = 0;
    helper_type_for(b, sp->helper_kind, &ret_ty);
    uint32_t m_lbl = nid(b), r0 = nid(b), r1 = nid(b), callee = sp->surviving_caller ? b->fn_surv : b->fn_helper;
    emit(b, SPV_FN, (const uint32_t[]){b->t_void, b->fn_main, 0u, b->t_fn_main}, 4u);
    emit(b, SPV_LABEL, &m_lbl, 1u);
    if (sp->priv_read_first) {
        uint32_t pl = nid(b);
        emit(b, SPV_LOAD, (const uint32_t[]){b->t_u32, pl, b->v_priv_cache}, 3u);
    }
    emit(b, SPV_CALL, (const uint32_t[]){ret_ty, r0, callee, b->c1, b->c0}, 5u);
    emit(b, SPV_CALL, (const uint32_t[]){ret_ty, r1, callee, b->c2, b->c0}, 5u);
    if (sp->exec_model == 0u) {
        uint32_t out_v = b->c0;
        if (sp->helper_kind == HK_WORD) out_v = r1;
        else if (sp->helper_kind == HK_WIDE) {
            uint32_t pp = nid(b); out_v = nid(b);
            emit(b, SPV_AC, (const uint32_t[]){b->p_priv_u32, pp, b->v_priv_wide, b->c1}, 4u);
            emit(b, SPV_LOAD, (const uint32_t[]){b->t_u32, out_v, pp}, 3u);
        } else {
            out_v = nid(b);
            emit(b, SPV_COMP_EXT, (const uint32_t[]){b->t_u32, out_v, r0, 0u}, 4u);
        }
        emit(b, SPV_STORE, (const uint32_t[]){b->v_out, out_v}, 2u);
    }
    if (sp->multi_block_main) {
        uint32_t lbl2 = nid(b);
        emit(b, SPV_BR, &lbl2, 1u);
        emit(b, SPV_LABEL, &lbl2, 1u);
    }
    emit(b, SPV_RET, NULL, 0u);
    emit(b, SPV_FN_END, NULL, 0u);
    b->w[3] = b->next_id;
}

static int count_op(const uint32_t *w, size_t n, uint32_t op) {
    int k = 0;
    for (size_t i = 5; i < n;) {
        uint32_t len = w[i] >> 16;
        if (!len || i + len > n) return -1;
        if ((w[i] & 0xffffu) == op) k++;
        i += len;
    }
    return k;
}

static int have_val(void) {
    return system("spirv-val --version >/dev/null 2>&1") == 0;
}

static int validate_spv(const uint32_t *w, size_t n, const char *tag) {
    char path[256];
    snprintf(path, sizeof path, "/tmp/t_gmem_replay_%d.spv", (int)getpid());
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fwrite(w, 4, n, f);
    fclose(f);
    char cmd[400];
    snprintf(cmd, sizeof cmd, "spirv-val --target-env vulkan1.3 %s", path);
    int r = system(cmd);
    remove(path);
    if (r) printf("  spirv-val failed on %s\n", tag);
    return r == 0;
}

static void test_positive(int *fail, int val, int kind, const char *tag) {
    spv_bld_t b;
    mod_spec_t sp = {0x00010300u, 0u, 0u, 0u, kind, 0, 0, 0};
    build_module(&b, &sp);
    size_t on = 0; int sites = -1; char err[160] = "";
    uint32_t *out = vka_spv_gmem_replay(b.w, b.n, 0u, 0u, &on, &sites, err, sizeof err);
    CHECK(out != NULL, tag);
    CHECK(err[0] == '\0' && sites == 2 && on > b.n, "replay stats & no err");
    if (out) {
        CHECK(out[0] == 0x07230203u && count_op(out, on, SPV_LMERGE) >= 2, "valid header & loop merges");
        CHECK(count_op(out, on, SPV_CALL) == 1, "only wrapper body call remains");
        if (val) CHECK(validate_spv(out, on, tag), "spirv-val rewrite");
        size_t on2 = 0; int s2 = -1; char e2[160] = "";
        uint32_t *idem = vka_spv_gmem_replay(out, on, 0u, 0u, &on2, &s2, e2, sizeof e2);
        CHECK(idem == NULL && e2[0] == '\0' && s2 == 0, "idempotent second pass");
        free(idem);
        free(out);
    }
}

static void test_negative(int *fail, mod_spec_t sp, const char *tag) {
    spv_bld_t b;
    build_module(&b, &sp);
    size_t on = 99u; int sites = 99; char err[160] = "x";
    uint32_t *out = vka_spv_gmem_replay(b.w, b.n, 0u, 0u, &on, &sites, err, sizeof err);
    CHECK(out == NULL && err[0] == '\0' && sites == 0 && on == 0u, tag);
    free(out);
}

int main(void) {
    int fails = 0;
    int *fail = &fails;
    int val = have_val();
    test_positive(fail, val, HK_WORD, "RK_WORD");
    test_positive(fail, val, HK_WIDE, "RK_WIDE");
    test_positive(fail, val, HK_RUN, "RK_RUN");
    test_negative(fail, (mod_spec_t){0x00010300u, 5u, 0u, 0u, HK_WORD, 0, 0, 0}, "reject GLCompute");
    test_negative(fail, (mod_spec_t){0x00010300u, 0u, 1u, 0u, HK_WORD, 0, 0, 0}, "reject wrong set");
    test_negative(fail, (mod_spec_t){0x00010300u, 0u, 0u, 0u, HK_WORD, 1, 0, 0}, "reject multi-block main");
    test_negative(fail, (mod_spec_t){0x00010300u, 0u, 0u, 0u, HK_WORD, 0, 1, 0}, "reject priv read before write");
    test_negative(fail, (mod_spec_t){0x00010300u, 0u, 0u, 0u, HK_WORD, 0, 0, 1}, "reject surviving helper call");
    test_negative(fail, (mod_spec_t){0x00010400u, 0u, 0u, 0u, HK_WORD, 0, 0, 0}, "reject SPIR-V 1.4");
    uint32_t bad[8] = {0x12345678u, 0x00010300u, 0u, 10u, 0u, 0x00010000u, 0u, 0u};
    size_t on = 0; int sites = 0; char err[160] = "";
    uint32_t *out = vka_spv_gmem_replay(bad, 8u, 0u, 0u, &on, &sites, err, sizeof err);
    CHECK(out == NULL && err[0] != '\0', "malformed SPIR-V sets err");
    free(out);
    printf("t_android_gmem_replay: %s%s\n", fails ? "FAILED" : "ok", val ? "" : " (no spirv-val: structure only)");
    return fails != 0;
}
