// SPDX-License-Identifier: MIT
// Runtime C11 SPIR-V rewrite for straight-line vertex shaders with global-memory
// pool-table lookups (raw global memory loads via GMEM_MAP).
#include "vka_spv_gmem_replay.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    SPV_MAGIC = 0x07230203u,
    SPV_V1_4 = 0x00010400u,
    OP_NAME = 5, OP_ENTRY = 15, OP_TVOID = 19, OP_TBOOL = 20, OP_TINT = 21,
    OP_TVEC = 23, OP_TSTRUCT = 30, OP_TPTR = 32, OP_CONST = 43,
    OP_CNULL = 46, OP_SPEC_CONST = 50, OP_FN = 54, OP_PARAM = 55,
    OP_FN_END = 56, OP_CALL = 57, OP_VAR = 59, OP_LOAD = 61, OP_STORE = 62,
    OP_AC = 65, OP_IBAC = 66, OP_PAC = 67, OP_DECORATE = 71,
    OP_COMP_CTOR = 80, OP_COMP_EXT = 81, OP_COPY = 83, OP_CVT_PTR = 120,
    OP_BITCAST = 124, OP_IADD = 128, OP_IMUL = 132, OP_LOR = 166,
    OP_LAND = 167, OP_LNOT = 168, OP_SELECT = 169, OP_IEQ = 170,
    OP_INEQ = 171, OP_ULT = 176, OP_ULE = 178, OP_SHR_L = 194,
    OP_SHL_L = 196, OP_BOR = 197, OP_BAND = 199, OP_DEC_ID = 332,
    OP_PHI = 245, OP_LMERGE = 246, OP_SMERGE = 247, OP_LABEL = 248,
    OP_BR = 249, OP_BRC = 250, OP_SWITCH = 251, OP_RET = 253, OP_RETV = 254,
    OP_DEC_STR = 5632,
    SC_UNI = 2, SC_OUT = 3, SC_PRIV = 6, SC_FUNC = 7,
    SC_SBUF = 12, SC_PHYS = 5349,
    DEC_BIND = 33, DEC_SET = 34, LOOP_DONT_UNROLL = 2,
    GR_MAX_WORDS = 1u << 22, GR_MAX_BOUND = 1u << 20,
    GR_MAX_FUNCS = 128, GR_MAX_CALLS = 4096, GR_MAX_HOPS = 64,
    GR_UCONST_N = 15, GR_ICONST_N = 11,
};

enum { RK_NONE = 0, RK_RUN = 1, RK_WIDE = 2, RK_WORD = 3 };

typedef struct {
    int kind, nwords;
    uint32_t fn_id, priv, pty, idx[4], vptr, vty, tptr, ity, elem;
} fn_kind_t;

typedef struct { uint32_t fn_id; size_t first_ins, last_ins; } fn_span_t;

typedef struct {
    const uint32_t *w;
    size_t nw, ins_n, first_fn, cap;
    uint32_t bound, next_id, entry_fn, table_id, tptr, ity, elem;
    uint32_t void_ty, bty, u32, v4, b4, v2, u64, ppriv, pphys, z4;
    uint32_t u_id[GR_UCONST_N], i_id[GR_ICONST_N];
    uint32_t slots[VKA_GMEM_REPLAY_SLOTS][6];
    uint32_t gaps[VKA_GMEM_REPLAY_GAPS][4];
    uint32_t missed, pend_lo, pend_hi, nxt, gnxt, nruns, seeded;
    size_t *offs;
    uint32_t *def_at, *dset, *dbind, *var_ptr, *var_sc;
    uint32_t *type_at, *cval_at;
    uint8_t *has_set, *has_bind, *gone_id;
    fn_span_t funcs[GR_MAX_FUNCS];
    fn_kind_t kinds[GR_MAX_FUNCS];
    int nfuncs, main_fn_idx, main_calls;
    uint32_t *out;
    size_t out_n, out_cap;
    int err_oom;
} gr_ctx_t;

static const uint32_t GR_UVALS[GR_UCONST_N] = {
    0u, 1u, 2u, 3u, 4u, 7u, 8u, 11u, 12u, 13u, 21u, 256u, 8191u, 0x10000u, 0xffffffffu,
};
static const uint32_t GR_IVALS[GR_ICONST_N] = {
    0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 4096u, 8191u,
};

static int set_err(char *err, size_t errn, const char *msg) {
    if (err && errn) snprintf(err, errn, "%s", msg);
    return 0;
}

static size_t ins_of(const gr_ctx_t *c, size_t idx) {
    assert(idx < c->ins_n);
    return c->offs[idx];
}

static uint32_t new_id(gr_ctx_t *c) {
    return c->next_id++;
}

static void emit_w(gr_ctx_t *c, uint32_t word) {
    if (c->out_n < c->out_cap) c->out[c->out_n++] = word;
    else c->err_oom = 1;
}

static void emit_arr(gr_ctx_t *c, const uint32_t *words, size_t n) {
    for (size_t i = 0; i < n; i++) emit_w(c, words[i]);
}

static void emit_ins(gr_ctx_t *c, uint32_t op, const uint32_t *ops, uint32_t n) {
    emit_w(c, ((n + 1u) << 16) | (op & 0xffffu));
    emit_arr(c, ops, n);
}

static uint32_t emit_val2(gr_ctx_t *c, uint32_t op, uint32_t ty, uint32_t a, uint32_t b) {
    uint32_t r = new_id(c);
    uint32_t ops[4] = {ty, r, a, b};
    emit_ins(c, op, ops, 4);
    return r;
}

static uint32_t emit_val1(gr_ctx_t *c, uint32_t op, uint32_t ty, uint32_t a) {
    uint32_t r = new_id(c);
    uint32_t ops[3] = {ty, r, a};
    emit_ins(c, op, ops, 3);
    return r;
}

static uint32_t emit_val3(gr_ctx_t *c, uint32_t op, uint32_t ty, uint32_t a, uint32_t b, uint32_t d) {
    uint32_t r = new_id(c);
    uint32_t ops[5] = {ty, r, a, b, d};
    emit_ins(c, op, ops, 5);
    return r;
}

static uint32_t emit_load(gr_ctx_t *c, uint32_t ty, uint32_t ptr) {
    return emit_val1(c, OP_LOAD, ty, ptr);
}

static void emit_store(gr_ctx_t *c, uint32_t ptr, uint32_t val) {
    uint32_t ops[2] = {ptr, val};
    emit_ins(c, OP_STORE, ops, 2);
}

static uint32_t uconst(const gr_ctx_t *c, uint32_t v) {
    for (int i = 0; i < GR_UCONST_N; i++) if (GR_UVALS[i] == v) return c->u_id[i];
    assert(0 && "missing u32 constant");
    return 0;
}

static uint32_t iconst(const gr_ctx_t *c, uint32_t v) {
    for (int i = 0; i < GR_ICONST_N; i++) if (GR_IVALS[i] == v) return c->i_id[i];
    assert(0 && "missing i constant");
    return 0;
}

static int no_result_op(uint32_t op) {
    if (op == OP_NAME || op == 6 || op == 7 || op == 8 || op == OP_ENTRY || op == 16 || op == OP_DECORATE || op == 72) return 1;
    if (op == OP_STORE || op == OP_LMERGE || op == OP_SMERGE || op == OP_BR || op == OP_BRC || op == OP_SWITCH || op == 252) return 1;
    if (op == OP_RET || op == OP_RETV || op == OP_FN_END || op == 63 || op == 64 || op == 99 || op == 224 || op == 225) return 1;
    return (op >= 218 && op <= 221) || op == 228 || op == 317 || op == 5294 || op == 5295 || op == 5380;
}

static int is_run_breaker(uint32_t op) {
    if (op == 63 || op == 64 || op == 99 || op == 224 || op == 225 || op == 252) return 1;
    if ((op >= 218 && op <= 221) || (op >= 227 && op <= 242)) return 1;
    return op == 4416 || op == 4448 || op == 5294 || op == 5295 || op == 5364 || op == 5365 || op == 5380;
}

static int is_int32_type(const gr_ctx_t *c, uint32_t tid) {
    if (!tid || tid >= c->bound || !c->type_at[tid]) return 0;
    size_t p = c->type_at[tid];
    return (c->w[p] & 0xffffu) == OP_TINT && (c->w[p] >> 16) >= 4 && c->w[p + 2] == 32u;
}

static int ptr_info(const gr_ctx_t *c, uint32_t tid, uint32_t *sc, uint32_t *ptee) {
    if (!tid || tid >= c->bound || !c->type_at[tid]) return 0;
    size_t p = c->type_at[tid];
    if ((c->w[p] & 0xffffu) != OP_TPTR || (c->w[p] >> 16) < 4) return 0;
    *sc = c->w[p + 2];
    *ptee = c->w[p + 3];
    return 1;
}

static int const_u32_val(const gr_ctx_t *c, uint32_t cid, uint32_t *out) {
    if (!cid || cid >= c->bound) return 0;
    if (c->cval_at[cid]) {
        *out = c->w[c->cval_at[cid]];
        return 1;
    }
    if (c->def_at[cid]) {
        size_t p = c->def_at[cid];
        if ((c->w[p] & 0xffffu) == OP_CNULL) { *out = 0; return 1; }
    }
    return 0;
}

static uint32_t find_type2(const gr_ctx_t *c, uint32_t op, uint32_t a, uint32_t b, int nops) {
    for (size_t k = 0; k < c->ins_n && k < c->first_fn; k++) {
        size_t p = ins_of(c, k);
        uint32_t len = c->w[p] >> 16;
        if ((c->w[p] & 0xffffu) != op || len != (uint32_t)(2 + nops)) continue;
        if (nops == 0) return c->w[p + 1];
        if (nops == 1 && c->w[p + 2] == a) return c->w[p + 1];
        if (nops == 2 && c->w[p + 2] == a && c->w[p + 3] == b) return c->w[p + 1];
    }
    return 0;
}

static uint32_t find_const_id(const gr_ctx_t *c, uint32_t ty, uint32_t val) {
    if (!ty) return 0;
    for (size_t k = 0; k < c->ins_n && k < c->first_fn; k++) {
        size_t p = ins_of(c, k);
        if ((c->w[p] & 0xffffu) == OP_CONST && (c->w[p] >> 16) == 4u && c->w[p + 1] == ty && c->w[p + 3] == val) return c->w[p + 2];
    }
    return 0;
}

static uint32_t unwrap_casts(const gr_ctx_t *c, uint32_t v, size_t lo, size_t hi) {
    for (int hop = 0; hop < GR_MAX_HOPS && v && v < c->bound; hop++) {
        size_t p = c->def_at[v];
        if (p <= lo || p >= hi) break;
        uint32_t op = c->w[p] & 0xffffu;
        if (op != OP_BITCAST && op != OP_COPY) break;
        v = c->w[p + 3];
    }
    return v;
}

static int trace_store_sc(const gr_ctx_t *c, uint32_t ptr, uint32_t *out_var, uint32_t *out_sc) {
    uint32_t v = ptr;
    for (int hop = 0; hop < GR_MAX_HOPS && v && v < c->bound; hop++) {
        if (c->var_sc[v]) { *out_var = v; *out_sc = c->var_sc[v] - 1u; return 1; }
        size_t p = c->def_at[v];
        if (!p) break;
        uint32_t op = c->w[p] & 0xffffu;
        if (op == OP_AC || op == OP_IBAC || op == OP_PAC || op == OP_COPY) { v = c->w[p + 3]; continue; }
        break;
    }
    return 0;
}

static int scan_module(gr_ctx_t *c, uint32_t tset, uint32_t tbind, char *err, size_t errn) {
    size_t pos = 5, idx = 0, fn_start = 0, fn_pos = 0;
    int ep_count = 0, ep_model = -1, in_fn = 0, table_hits = 0;
    c->first_fn = c->nw;
    for (size_t s = 0; s < c->nw && pos < c->nw; s++) {
        uint32_t op = c->w[pos] & 0xffffu, len = c->w[pos] >> 16;
        if (!len || pos + len > c->nw) return set_err(err, errn, "malformed instruction");
        c->offs[idx] = pos;
        if (op == OP_ENTRY && len >= 3) { ep_count++; ep_model = (int)c->w[pos + 1]; c->entry_fn = c->w[pos + 2]; }
        else if (op == OP_DECORATE && len >= 4 && c->w[pos + 1] < c->bound) {
            if (c->w[pos + 2] == DEC_SET) { c->dset[c->w[pos + 1]] = c->w[pos + 3]; c->has_set[c->w[pos + 1]] = 1; }
            else if (c->w[pos + 2] == DEC_BIND) { c->dbind[c->w[pos + 1]] = c->w[pos + 3]; c->has_bind[c->w[pos + 1]] = 1; }
        } else if (op >= 19 && op <= 39 && len >= 2 && c->w[pos + 1] < c->bound) {
            c->type_at[c->w[pos + 1]] = (uint32_t)pos;
        } else if ((op == OP_CONST || op == OP_CNULL || op == OP_SPEC_CONST) && len >= 3 && c->w[pos + 2] < c->bound) {
            c->def_at[c->w[pos + 2]] = (uint32_t)pos;
            if (op == OP_CONST && len == 4) c->cval_at[c->w[pos + 2]] = (uint32_t)(pos + 3);
        } else if (op == OP_VAR && len >= 4 && c->w[pos + 2] < c->bound) {
            uint32_t id = c->w[pos + 2];
            c->var_ptr[id] = c->w[pos + 1]; c->var_sc[id] = c->w[pos + 3] + 1u; c->def_at[id] = (uint32_t)pos;
            if (!in_fn && c->has_bind[id] && (c->has_set[id] ? c->dset[id] : 0u) == tset && c->dbind[id] == tbind) {
                c->table_id = id; table_hits++;
            }
        } else if (op == OP_FN && len >= 5) {
            if (c->first_fn == c->nw) c->first_fn = idx;
            if (c->nfuncs >= GR_MAX_FUNCS) return 0;
            fn_start = idx; fn_pos = pos; in_fn = 1; c->def_at[c->w[pos + 2]] = (uint32_t)pos;
        } else if (op == OP_FN_END) {
            if (!in_fn) return set_err(err, errn, "unmatched OpFunctionEnd");
            c->funcs[c->nfuncs].fn_id = c->w[fn_pos + 2];
            c->funcs[c->nfuncs].first_ins = fn_start; c->funcs[c->nfuncs].last_ins = idx;
            if (c->funcs[c->nfuncs].fn_id == c->entry_fn) c->main_fn_idx = c->nfuncs;
            c->nfuncs++; in_fn = 0;
        } else if (in_fn && op == OP_LABEL && len >= 2 && c->w[pos + 1] < c->bound) {
            c->def_at[c->w[pos + 1]] = (uint32_t)pos;
        } else if (in_fn && !no_result_op(op) && len >= 3 && c->w[pos + 2] < c->bound) {
            c->def_at[c->w[pos + 2]] = (uint32_t)pos;
        }
        pos += len; idx++;
    }
    c->ins_n = idx;
    if (in_fn || ep_count != 1 || ep_model != 0 || table_hits != 1 || c->main_fn_idx < 0) return 0;
    return 1;
}

static int fn_find_table_chain(const gr_ctx_t *c, const fn_span_t *f, fn_kind_t *k) {
    uint32_t sc = 0, elem = 0, ity = 0;
    int seen = 0;
    for (size_t i = f->first_ins; i <= f->last_ins; i++) {
        size_t p = ins_of(c, i);
        uint32_t op = c->w[p] & 0xffffu, len = c->w[p] >> 16;
        if ((op != OP_AC && op != OP_IBAC) || len < 4 || c->w[p + 3] != c->table_id) continue;
        uint32_t c0 = 1;
        if (len != 6 || !const_u32_val(c, c->w[p + 4], &c0) || c0 != 0) return 0;
        if (!ptr_info(c, c->w[p + 1], &sc, &elem) || (sc != SC_UNI && sc != SC_SBUF) || !is_int32_type(c, elem)) return 0;
        if (c->w[p + 4] >= c->bound || !c->def_at[c->w[p + 4]]) return 0;
        ity = c->w[c->def_at[c->w[p + 4]] + 1];
        if (!seen) { k->tptr = c->w[p + 1]; k->elem = elem; k->ity = ity; seen = 1; }
    }
    return seen;
}

static int classify_run_fn(const gr_ctx_t *c, const fn_span_t *f, fn_kind_t *k) {
    size_t p0 = ins_of(c, f->first_ins);
    uint32_t u32 = find_type2(c, OP_TINT, 32, 0, 2);
    uint32_t v4 = u32 ? find_type2(c, OP_TVEC, u32, 4, 2) : 0;
    if (!v4 || c->w[p0 + 1] != v4) return 0;
    int ncu = 0, priv_lds = 0;
    size_t lim = f->first_ins + 40;
    for (size_t i = f->first_ins; i <= f->last_ins; i++) {
        size_t p = ins_of(c, i);
        uint32_t op = c->w[p] & 0xffffu;
        if (op == OP_CVT_PTR) ncu++;
        if (i < lim && op == OP_LOAD && (c->w[p] >> 16) >= 4) {
            uint32_t v = c->w[p + 3];
            if (v < c->bound && c->var_sc[v] == SC_PRIV + 1u) priv_lds++;
        }
    }
    if (ncu < 2 || ncu > 5 || priv_lds < 6) return 0;
    k->kind = RK_RUN;
    k->nwords = ncu - 1;
    return 1;
}

static int classify_wide_fn(const gr_ctx_t *c, const fn_span_t *f, fn_kind_t *k) {
    size_t fa = ins_of(c, f->first_ins), fe = ins_of(c, f->last_ins) + (c->w[ins_of(c, f->last_ins)] >> 16);
    uint32_t cu_ptr = 0, cu_ty = 0, ld_id = 0, sc = 0, pt = 0;
    int ncu = 0, nld = 0;
    for (size_t i = f->first_ins; i <= f->last_ins; i++) {
        size_t p = ins_of(c, i);
        uint32_t op = c->w[p] & 0xffffu;
        if (is_run_breaker(op) || op == OP_CALL) return 0;
        if (op == OP_CVT_PTR) { ncu++; cu_ty = c->w[p + 1]; cu_ptr = c->w[p + 2]; }
    }
    if (ncu != 1 || !ptr_info(c, cu_ty, &sc, &pt) || sc != SC_PHYS) return 0;
    if (!c->type_at[pt] || (c->w[c->type_at[pt]] & 0xffffu) != OP_TVEC || c->w[c->type_at[pt] + 3] != 4u) return 0;
    for (size_t i = f->first_ins; i <= f->last_ins; i++) {
        size_t p = ins_of(c, i);
        if ((c->w[p] & 0xffffu) == OP_LOAD && c->w[p + 3] == cu_ptr) { nld++; ld_id = c->w[p + 2]; }
    }
    if (nld != 1) return 0;
    uint32_t priv = 0, pty = 0, mask = 0;
    for (size_t i = f->first_ins; i <= f->last_ins; i++) {
        size_t p = ins_of(c, i);
        if ((c->w[p] & 0xffffu) != OP_STORE) continue;
        uint32_t ch = c->w[p + 1], ch_p = ch < c->bound ? c->def_at[ch] : 0;
        if (!ch_p || ch_p <= fa || ch_p >= fe) continue;
        uint32_t cop = c->w[ch_p] & 0xffffu;
        if ((cop != OP_AC && cop != OP_IBAC) || (c->w[ch_p] >> 16) != 5u) continue;
        uint32_t val = unwrap_casts(c, c->w[p + 2], fa, fe), vp = val < c->bound ? c->def_at[val] : 0;
        if (!vp || vp <= fa || vp >= fe || (c->w[vp] & 0xffffu) != OP_COMP_EXT || c->w[vp + 3] != ld_id) continue;
        uint32_t lane = c->w[vp + 4], base = c->w[ch_p + 3];
        if (lane < 1 || lane > 3 || base >= c->bound || c->var_sc[base] != SC_PRIV + 1u) continue;
        if (priv && priv != base) return 0;
        priv = base; pty = c->w[ch_p + 1]; k->idx[lane] = c->w[ch_p + 4]; mask |= 1u << lane;
    }
    if (mask != 0xeu) return 0;
    k->kind = RK_WIDE; k->vptr = cu_ty; k->vty = pt; k->priv = priv; k->pty = pty;
    return 1;
}

static int word_load_target(const gr_ctx_t *c, const fn_span_t *f, uint32_t ptr, uint32_t pt, uint32_t *ld_id) {
    uint32_t cur = ptr, cur_ty = pt;
    if (c->type_at[cur_ty] && (c->w[c->type_at[cur_ty]] & 0xffffu) == OP_TSTRUCT) {
        int mem_uses = 0; uint32_t next_p = 0, m_sc = 0, m_ty = 0, c0 = 1;
        for (size_t i = f->first_ins; i <= f->last_ins; i++) {
            size_t p = ins_of(c, i);
            uint32_t op = c->w[p] & 0xffffu, len = c->w[p] >> 16;
            if ((op == OP_AC || op == OP_IBAC) && len == 5u && c->w[p + 3] == cur) {
                mem_uses++; next_p = c->w[p + 2];
                if (!const_u32_val(c, c->w[p + 4], &c0) || c0 != 0 || !ptr_info(c, c->w[p + 1], &m_sc, &m_ty)) return 0;
            } else {
                uint32_t s = no_result_op(op) ? 1u : (op == OP_LABEL || op == OP_VAR || op == OP_PARAM ? len : 3u);
                for (uint32_t k = s; k < len; k++) if (c->w[p + k] == cur) return 0;
            }
        }
        if (mem_uses != 1) return 0;
        cur = next_p; cur_ty = m_ty;
    }
    if (!is_int32_type(c, cur_ty)) return 0;
    int nld = 0;
    for (size_t i = f->first_ins; i <= f->last_ins; i++) {
        size_t p = ins_of(c, i);
        uint32_t op = c->w[p] & 0xffffu, len = c->w[p] >> 16;
        if (op == OP_LOAD && c->w[p + 3] == cur) { nld++; *ld_id = c->w[p + 2]; continue; }
        uint32_t s = no_result_op(op) ? 1u : (op == OP_LABEL || op == OP_VAR || op == OP_PARAM ? len : 3u);
        for (uint32_t k = s; k < len; k++) if (c->w[p + k] == cur) return 0;
    }
    return nld == 1;
}

static int classify_word_fn(const gr_ctx_t *c, const fn_span_t *f, fn_kind_t *k) {
    size_t fa = ins_of(c, f->first_ins), fe = ins_of(c, f->last_ins) + (c->w[ins_of(c, f->last_ins)] >> 16);
    if (!is_int32_type(c, c->w[fa + 1])) return 0;
    uint32_t cu_ptr = 0, cu_ty = 0, sc = 0, pt = 0, ld_id = 0, dummy_v = 0, s_sc = 0;
    int ncu = 0, has_ld_ret = 0;
    for (size_t i = f->first_ins; i <= f->last_ins; i++) {
        size_t p = ins_of(c, i);
        uint32_t op = c->w[p] & 0xffffu;
        if (op == OP_PARAM && !is_int32_type(c, c->w[p + 1])) return 0;
        if (is_run_breaker(op) || op == OP_CALL) return 0;
        if (op == OP_STORE && (!trace_store_sc(c, c->w[p + 1], &dummy_v, &s_sc) || (s_sc != SC_FUNC && s_sc != SC_PRIV))) return 0;
        if (op == OP_CVT_PTR) { ncu++; cu_ty = c->w[p + 1]; cu_ptr = c->w[p + 2]; }
    }
    if (ncu != 1 || !ptr_info(c, cu_ty, &sc, &pt) || sc != SC_PHYS || !word_load_target(c, f, cu_ptr, pt, &ld_id)) return 0;
    for (size_t i = f->first_ins; i <= f->last_ins; i++) {
        size_t p = ins_of(c, i);
        if ((c->w[p] & 0xffffu) != OP_RETV) continue;
        uint32_t rv = unwrap_casts(c, c->w[p + 1], fa, fe), cz = 1;
        if (rv == ld_id) has_ld_ret = 1;
        else if (!const_u32_val(c, rv, &cz) || cz != 0) return 0;
    }
    if (!has_ld_ret) return 0;
    k->kind = RK_WORD;
    return 1;
}

static int classify_fn(const gr_ctx_t *c, int fn_idx, fn_kind_t *k) {
    const fn_span_t *f = &c->funcs[fn_idx];
    memset(k, 0, sizeof *k);
    k->fn_id = f->fn_id;
    if (f->fn_id == c->entry_fn || f->last_ins - f->first_ins < 4u) return 0;
    if ((c->w[ins_of(c, f->first_ins + 1)] & 0xffffu) != OP_PARAM) return 0;
    if ((c->w[ins_of(c, f->first_ins + 2)] & 0xffffu) != OP_PARAM) return 0;
    if ((c->w[ins_of(c, f->first_ins + 3)] & 0xffffu) == OP_PARAM) return 0;
    if (!fn_find_table_chain(c, f, k)) return 0;
    if (classify_run_fn(c, f, k) || classify_wide_fn(c, f, k) || classify_word_fn(c, f, k)) return 1;
    return 0;
}

static int find_func_index(const gr_ctx_t *c, uint32_t fid) {
    for (int i = 0; i < c->nfuncs; i++) if (c->funcs[i].fn_id == fid) return i;
    return -1;
}

static int check_main_body(gr_ctx_t *c, uint8_t *written) {
    const fn_span_t *mf = &c->funcs[c->main_fn_idx];
    int labels = 0, saw_nonvar = 0;
    size_t last = mf->last_ins;
    if (last <= mf->first_ins + 2u || (c->w[ins_of(c, last - 1u)] & 0xffffu) != OP_RET) return 0;
    for (size_t i = mf->first_ins + 1u; i < last; i++) {
        size_t p = ins_of(c, i);
        uint32_t op = c->w[p] & 0xffffu, len = c->w[p] >> 16;
        if (op == OP_LABEL) { if (++labels > 1) return 0; continue; }
        if (op == OP_VAR) { if (saw_nonvar || len < 4u || c->w[p + 3] != SC_FUNC) return 0; continue; }
        saw_nonvar = 1;
        if (is_run_breaker(op)) return 0;
        if (op == OP_STORE) {
            uint32_t var = 0, sc = 0;
            if (!trace_store_sc(c, c->w[p + 1], &var, &sc) || (sc != SC_FUNC && sc != SC_PRIV && sc != SC_OUT)) return 0;
            if (sc == SC_PRIV) written[var] = 1;
        } else if (op == OP_LOAD) {
            uint32_t var = 0, sc = 0;
            if (trace_store_sc(c, c->w[p + 3], &var, &sc) && sc == SC_PRIV && !written[var]) return 0;
        } else if (op == OP_CALL) {
            int fi = len == 6u ? find_func_index(c, c->w[p + 3]) : -1;
            if (fi < 0) return 0;
            if (c->kinds[fi].kind == RK_NONE && !classify_fn(c, fi, &c->kinds[fi])) return 0;
            if (c->kinds[fi].kind == RK_WIDE) written[c->kinds[fi].priv] = 1;
            c->main_calls++;
        }
    }
    return labels == 1 && c->main_calls > 0 && c->main_calls <= GR_MAX_CALLS;
}

static int verify_no_surviving_calls(const gr_ctx_t *c) {
    for (int f = 0; f < c->nfuncs; f++) {
        if (f == c->main_fn_idx || c->kinds[f].kind != RK_NONE) continue;
        for (size_t i = c->funcs[f].first_ins; i <= c->funcs[f].last_ins; i++) {
            size_t p = ins_of(c, i);
            if ((c->w[p] & 0xffffu) != OP_CALL) continue;
            int ti = find_func_index(c, c->w[p + 3]);
            if (ti >= 0 && c->kinds[ti].kind != RK_NONE) return 0;
        }
    }
    return 1;
}

static void mark_gone_ids(gr_ctx_t *c) {
    for (int f = 0; f < c->nfuncs; f++) {
        if (c->kinds[f].kind == RK_NONE) continue;
        for (size_t i = c->funcs[f].first_ins; i <= c->funcs[f].last_ins; i++) {
            size_t p = ins_of(c, i);
            uint32_t op = c->w[p] & 0xffffu, len = c->w[p] >> 16;
            uint32_t id = (op == OP_LABEL) ? c->w[p + 1] : (!no_result_op(op) && len >= 3u) ? c->w[p + 2] : 0u;
            if (id && id < c->bound) c->gone_id[id] = 1;
        }
    }
}

static void plan_types_and_vars(gr_ctx_t *c) {
    c->next_id = c->bound;
    for (int f = 0; f < c->nfuncs; f++) if (c->kinds[f].kind != RK_NONE) {
        c->tptr = c->kinds[f].tptr; c->ity = c->kinds[f].ity; c->elem = c->kinds[f].elem; break;
    }
    c->void_ty = find_type2(c, OP_TVOID, 0, 0, 0);
    c->bty = find_type2(c, OP_TBOOL, 0, 0, 0); if (!c->bty) c->bty = new_id(c);
    c->u32 = find_type2(c, OP_TINT, 32, 0, 2); if (!c->u32) c->u32 = new_id(c);
    c->v4 = find_type2(c, OP_TVEC, c->u32, 4, 2); if (!c->v4) c->v4 = new_id(c);
    c->b4 = find_type2(c, OP_TVEC, c->bty, 4, 2); if (!c->b4) c->b4 = new_id(c);
    c->v2 = find_type2(c, OP_TVEC, c->u32, 2, 2); if (!c->v2) c->v2 = new_id(c);
    c->u64 = find_type2(c, OP_TINT, 64, 0, 2); if (!c->u64) c->u64 = new_id(c);
    c->ppriv = find_type2(c, OP_TPTR, SC_PRIV, c->u32, 2); if (!c->ppriv) c->ppriv = new_id(c);
    c->pphys = find_type2(c, OP_TPTR, SC_PHYS, c->u32, 2); if (!c->pphys) c->pphys = new_id(c);
    c->z4 = new_id(c);
    for (int i = 0; i < GR_UCONST_N; i++) { uint32_t x = find_const_id(c, c->u32, GR_UVALS[i]); c->u_id[i] = x ? x : new_id(c); }
    for (int i = 0; i < GR_ICONST_N; i++) {
        uint32_t x = find_const_id(c, c->ity, GR_IVALS[i]);
        if (!x && c->ity == c->u32) {
            for (int j = 0; j < GR_UCONST_N; j++) if (GR_UVALS[j] == GR_IVALS[i]) { x = c->u_id[j]; break; }
        }
        c->i_id[i] = x ? x : new_id(c);
    }
    for (int s = 0; s < VKA_GMEM_REPLAY_SLOTS; s++) for (int w = 0; w < 6; w++) c->slots[s][w] = new_id(c);
    for (int g = 0; g < VKA_GMEM_REPLAY_GAPS; g++) for (int w = 0; w < 4; w++) c->gaps[g][w] = new_id(c);
    c->missed = new_id(c); c->pend_lo = new_id(c); c->pend_hi = new_id(c);
    c->nxt = new_id(c); c->gnxt = new_id(c); c->nruns = new_id(c); c->seeded = new_id(c);
}

static void emit_new_decls(gr_ctx_t *c) {
    if (c->bty >= c->bound) { uint32_t a[1] = {c->bty}; emit_ins(c, OP_TBOOL, a, 1); }
    if (c->u32 >= c->bound) { uint32_t a[3] = {c->u32, 32, 0}; emit_ins(c, OP_TINT, a, 3); }
    if (c->v4 >= c->bound) { uint32_t a[3] = {c->v4, c->u32, 4}; emit_ins(c, OP_TVEC, a, 3); }
    if (c->b4 >= c->bound) { uint32_t a[3] = {c->b4, c->bty, 4}; emit_ins(c, OP_TVEC, a, 3); }
    if (c->v2 >= c->bound) { uint32_t a[3] = {c->v2, c->u32, 2}; emit_ins(c, OP_TVEC, a, 3); }
    if (c->u64 >= c->bound) { uint32_t a[3] = {c->u64, 64, 0}; emit_ins(c, OP_TINT, a, 3); }
    if (c->ppriv >= c->bound) { uint32_t a[3] = {c->ppriv, SC_PRIV, c->u32}; emit_ins(c, OP_TPTR, a, 3); }
    if (c->pphys >= c->bound) { uint32_t a[3] = {c->pphys, SC_PHYS, c->u32}; emit_ins(c, OP_TPTR, a, 3); }
    { uint32_t a[2] = {c->v4, c->z4}; emit_ins(c, OP_CNULL, a, 2); }
    for (int i = 0; i < GR_UCONST_N; i++) if (c->u_id[i] >= c->bound) {
        uint32_t a[3] = {c->u32, c->u_id[i], GR_UVALS[i]}; emit_ins(c, OP_CONST, a, 3);
    }
    for (int i = 0; i < GR_ICONST_N; i++) if (c->i_id[i] >= c->bound && c->ity != c->u32) {
        uint32_t a[3] = {c->ity, c->i_id[i], GR_IVALS[i]}; emit_ins(c, OP_CONST, a, 3);
    } else if (c->i_id[i] >= c->bound && c->ity == c->u32) {
        int dup = 0; for (int j = 0; j < GR_UCONST_N; j++) if (c->u_id[j] == c->i_id[i]) dup = 1;
        if (!dup) { uint32_t a[3] = {c->ity, c->i_id[i], GR_IVALS[i]}; emit_ins(c, OP_CONST, a, 3); }
    }
    uint32_t ff = uconst(c, 0xffffffffu), z0 = uconst(c, 0u);
    for (int s = 0; s < VKA_GMEM_REPLAY_SLOTS; s++) for (int w = 0; w < 6; w++) {
        uint32_t a[4] = {c->ppriv, c->slots[s][w], SC_PRIV, w < 2 ? ff : z0}; emit_ins(c, OP_VAR, a, 4);
    }
    for (int g = 0; g < VKA_GMEM_REPLAY_GAPS; g++) for (int w = 0; w < 4; w++) {
        uint32_t a[4] = {c->ppriv, c->gaps[g][w], SC_PRIV, w < 2 ? ff : z0}; emit_ins(c, OP_VAR, a, 4);
    }
    uint32_t pv[7] = {c->missed, c->pend_lo, c->pend_hi, c->nxt, c->gnxt, c->nruns, c->seeded};
    for (int i = 0; i < 7; i++) { uint32_t a[4] = {c->ppriv, pv[i], SC_PRIV, z0}; emit_ins(c, OP_VAR, a, 4); }
}

static uint32_t emit_lt64(gr_ctx_t *c, uint32_t ahi, uint32_t alo, uint32_t bhi, uint32_t blo, int eq) {
    uint32_t lt_hi = emit_val2(c, OP_ULT, c->bty, ahi, bhi);
    uint32_t eq_hi = emit_val2(c, OP_IEQ, c->bty, ahi, bhi);
    uint32_t lo_cmp = emit_val2(c, eq ? OP_ULE : OP_ULT, c->bty, alo, blo);
    return emit_val2(c, OP_LOR, c->bty, lt_hi, emit_val2(c, OP_LAND, c->bty, eq_hi, lo_cmp));
}

static uint32_t emit_within(gr_ctx_t *c, uint32_t lu, uint32_t hu, const uint32_t b[4]) {
    return emit_val2(c, OP_LAND, c->bty, emit_lt64(c, b[1], b[0], hu, lu, 1), emit_lt64(c, hu, lu, b[3], b[2], 0));
}

static void emit_add64(gr_ctx_t *c, uint32_t alo, uint32_t ahi, uint32_t blo, uint32_t bhi, uint32_t *ol, uint32_t *oh) {
    uint32_t l = emit_val2(c, OP_IADD, c->u32, alo, blo);
    uint32_t cy = emit_val3(c, OP_SELECT, c->u32, emit_val2(c, OP_ULT, c->bty, l, alo), uconst(c, 1u), uconst(c, 0u));
    *ol = l;
    *oh = emit_val2(c, OP_IADD, c->u32, emit_val2(c, OP_IADD, c->u32, ahi, bhi), cy);
}

static void emit_check(gr_ctx_t *c, uint32_t lu, uint32_t hu, uint32_t *ohit, uint32_t *odl, uint32_t *odh) {
    uint32_t hit = 0, dl = 0, dh = 0;
    for (int s = 0; s < VKA_GMEM_REPLAY_SLOTS; s++) {
        uint32_t sv[6];
        for (int w = 0; w < 6; w++) sv[w] = emit_load(c, c->u32, c->slots[s][w]);
        uint32_t hh = emit_within(c, lu, hu, sv);
        if (s == 0) { hit = hh; dl = sv[4]; dh = sv[5]; }
        else {
            hit = emit_val2(c, OP_LOR, c->bty, hit, hh);
            dl = emit_val3(c, OP_SELECT, c->u32, hh, sv[4], dl);
            dh = emit_val3(c, OP_SELECT, c->u32, hh, sv[5], dh);
        }
    }
    *ohit = hit; *odl = dl; *odh = dh;
}

static void emit_note_miss(gr_ctx_t *c, uint32_t hit, uint32_t lu, uint32_t hu) {
    uint32_t lo_page = emit_val2(c, OP_LAND, c->bty, emit_val2(c, OP_IEQ, c->bty, hu, uconst(c, 0u)),
                                 emit_val2(c, OP_ULT, c->bty, lu, uconst(c, 0x10000u)));
    uint32_t known = lo_page;
    for (int g = 0; g < VKA_GMEM_REPLAY_GAPS; g++) {
        uint32_t gv[4];
        for (int w = 0; w < 4; w++) gv[w] = emit_load(c, c->u32, c->gaps[g][w]);
        known = emit_val2(c, OP_LOR, c->bty, known, emit_within(c, lu, hu, gv));
    }
    uint32_t miss = emit_val2(c, OP_LAND, c->bty, emit_val1(c, OP_LNOT, c->bty, hit), emit_val1(c, OP_LNOT, c->bty, known));
    uint32_t mo = emit_load(c, c->u32, c->missed);
    uint32_t first = emit_val2(c, OP_LAND, c->bty, miss, emit_val2(c, OP_IEQ, c->bty, mo, uconst(c, 0u)));
    emit_store(c, c->pend_lo, emit_val3(c, OP_SELECT, c->u32, first, lu, emit_load(c, c->u32, c->pend_lo)));
    emit_store(c, c->pend_hi, emit_val3(c, OP_SELECT, c->u32, first, hu, emit_load(c, c->u32, c->pend_hi)));
    emit_store(c, c->missed, emit_val3(c, OP_SELECT, c->u32, miss, uconst(c, 1u), mo));
}

static uint32_t emit_tbl_word(gr_ctx_t *c, uint32_t idx_i, uint32_t word_off) {
    uint32_t mul = emit_val2(c, OP_IMUL, c->ity, idx_i, iconst(c, 8u));
    uint32_t off = emit_val2(c, OP_IADD, c->ity, mul, iconst(c, word_off));
    uint32_t ptr = emit_val3(c, OP_AC, c->tptr, c->table_id, iconst(c, 0u), off);
    uint32_t ld = emit_load(c, c->elem, ptr);
    return c->elem == c->u32 ? ld : emit_val1(c, OP_BITCAST, c->u32, ld);
}

static uint32_t emit_phys_load(gr_ctx_t *c, uint32_t vty, uint32_t vptr, uint32_t ptr_u64) {
    uint32_t p = emit_val1(c, OP_CVT_PTR, vptr, ptr_u64);
    uint32_t r = new_id(c);
    uint32_t ops[5] = {vty, r, p, 2u, 4u};
    emit_ins(c, OP_LOAD, ops, 5);
    return r;
}

static uint32_t as_u32(gr_ctx_t *c, uint32_t v) {
    if (v < c->bound && c->def_at[v]) {
        uint32_t op = c->w[c->def_at[v]] & 0xffffu;
        if (op != OP_LABEL && op != OP_FN && c->w[c->def_at[v] + 1] == c->u32) return v;
    }
    return emit_val1(c, OP_BITCAST, c->u32, v);
}

static void emit_site(gr_ctx_t *c, const fn_kind_t *k, uint32_t ret_ty, uint32_t res_id, uint32_t arg0, uint32_t arg1) {
    uint32_t lu = as_u32(c, arg0), hu = as_u32(c, arg1);
    uint32_t sk0 = emit_tbl_word(c, iconst(c, 0u), 6u), sk1 = emit_tbl_word(c, iconst(c, 0u), 7u);
    uint32_t v = 0;
    if (k->kind == RK_WIDE) {
        uint32_t hit = 0, dl = 0, dh = 0, pl = 0, ph = 0;
        emit_check(c, lu, hu, &hit, &dl, &dh);
        emit_add64(c, lu, hu, dl, dh, &pl, &ph);
        uint32_t sl = emit_val3(c, OP_SELECT, c->u32, hit, pl, sk0), sh = emit_val3(c, OP_SELECT, c->u32, hit, ph, sk1);
        uint32_t vec = emit_val2(c, OP_COMP_CTOR, c->v2, sl, sh), u64a = emit_val1(c, OP_BITCAST, c->u64, vec);
        uint32_t lv = emit_phys_load(c, k->vty, k->vptr, u64a);
        if (k->vty != c->v4) lv = emit_val1(c, OP_BITCAST, c->v4, lv);
        uint32_t hb = new_id(c);
        uint32_t hb_ops[6] = {c->b4, hb, hit, hit, hit, hit};
        emit_ins(c, OP_COMP_CTOR, hb_ops, 6);
        v = emit_val3(c, OP_SELECT, c->v4, hb, lv, c->z4);
        emit_note_miss(c, hit, lu, hu);
    } else {
        int n = (k->kind == RK_RUN) ? k->nwords : 1;
        uint32_t ws[4] = {uconst(c, 0u), uconst(c, 0u), uconst(c, 0u), uconst(c, 0u)};
        for (int j = 0; j < n; j++) {
            uint32_t al = lu, ah = hu, hit = 0, dl = 0, dh = 0, pl = 0, ph = 0;
            if (j > 0) emit_add64(c, lu, hu, uconst(c, (uint32_t)(4 * j)), uconst(c, 0u), &al, &ah);
            emit_check(c, al, ah, &hit, &dl, &dh);
            emit_add64(c, al, ah, dl, dh, &pl, &ph);
            uint32_t sl = emit_val3(c, OP_SELECT, c->u32, hit, pl, sk0), sh = emit_val3(c, OP_SELECT, c->u32, hit, ph, sk1);
            uint32_t u64a = emit_val1(c, OP_BITCAST, c->u64, emit_val2(c, OP_COMP_CTOR, c->v2, sl, sh));
            uint32_t wv = emit_phys_load(c, c->u32, c->pphys, u64a);
            ws[j] = emit_val3(c, OP_SELECT, c->u32, hit, wv, uconst(c, 0u));
            emit_note_miss(c, hit, al, ah);
        }
        uint32_t ctor_ops[6] = {c->v4, 0, ws[0], ws[1], ws[2], ws[3]};
        v = new_id(c); ctor_ops[1] = v;
        emit_ins(c, OP_COMP_CTOR, ctor_ops, 6);
    }
    if (k->kind == RK_RUN) { uint32_t co[3] = {ret_ty, res_id, v}; emit_ins(c, OP_COPY, co, 3); return; }
    if (ret_ty != c->void_ty) {
        uint32_t e0 = emit_val2(c, OP_COMP_EXT, c->u32, v, 0u);
        uint32_t co[3] = {ret_ty, res_id, e0};
        emit_ins(c, ret_ty == c->u32 ? OP_COPY : OP_BITCAST, co, 3);
    }
    if (k->kind == RK_WIDE) {
        for (uint32_t j = 1; j <= 3; j++) {
            uint32_t ek = emit_val2(c, OP_COMP_EXT, c->u32, v, j);
            uint32_t ap = emit_val2(c, OP_AC, k->pty, k->priv, k->idx[j]);
            emit_store(c, ap, ek);
        }
    }
}

static uint32_t emit_learn_hint(gr_ctx_t *c, uint32_t lu, uint32_t hu, uint32_t *ohidx_i) {
    uint32_t sh = emit_val2(c, OP_SHL_L, c->u32, hu, uconst(c, 11u));
    uint32_t lo_s = emit_val2(c, OP_SHR_L, c->u32, lu, uconst(c, 21u));
    uint32_t chunk = emit_val2(c, OP_BOR, c->u32, sh, lo_s);
    uint32_t c_idx = emit_val2(c, OP_BAND, c->u32, chunk, uconst(c, 8191u));
    if (c->ity != c->u32) c_idx = emit_val1(c, OP_BITCAST, c->ity, c_idx);
    uint32_t hw = emit_tbl_word(c, c_idx, 6u);
    uint32_t hidx = emit_val2(c, OP_BAND, c->u32, hw, uconst(c, 8191u));
    uint32_t hidx_i = (c->ity == c->u32) ? hidx : emit_val1(c, OP_BITCAST, c->ity, hidx);
    uint32_t hok = emit_val2(c, OP_LAND, c->bty, emit_val2(c, OP_INEQ, c->bty, hidx, uconst(c, 0u)),
                             emit_val2(c, OP_IEQ, c->bty, emit_val2(c, OP_SHR_L, c->u32, hw, uconst(c, 13u)), chunk));
    hok = emit_val2(c, OP_LAND, c->bty, hok, emit_val2(c, OP_ULT, c->bty, hu, uconst(c, 256u)));
    uint32_t hb[4];
    for (uint32_t w = 0; w < 4; w++) hb[w] = emit_tbl_word(c, hidx_i, w);
    *ohidx_i = hidx_i;
    return emit_val2(c, OP_LAND, c->bty, hok, emit_within(c, lu, hu, hb));
}

static void emit_learn_update(gr_ctx_t *c, uint32_t idx, uint32_t lu, uint32_t hu) {
    uint32_t ws[6];
    for (uint32_t w = 0; w < 6; w++) ws[w] = emit_tbl_word(c, idx, w);
    uint32_t below = emit_lt64(c, ws[1], ws[0], hu, lu, 1);
    uint32_t found = emit_val2(c, OP_LAND, c->bty, below, emit_lt64(c, hu, lu, ws[3], ws[2], 0));
    uint32_t n0 = emit_load(c, c->u32, c->nxt);
    for (uint32_t si = 0; si < VKA_GMEM_REPLAY_SLOTS; si++) {
        uint32_t here = emit_val2(c, OP_LAND, c->bty, found, emit_val2(c, OP_IEQ, c->bty, n0, uconst(c, si)));
        for (int w = 0; w < 6; w++) emit_store(c, c->slots[si][w], emit_val3(c, OP_SELECT, c->u32, here, ws[w], emit_load(c, c->u32, c->slots[si][w])));
    }
    uint32_t n1 = emit_val2(c, OP_IADD, c->u32, n0, uconst(c, 1u));
    uint32_t nwrap = emit_val3(c, OP_SELECT, c->u32, emit_val2(c, OP_IEQ, c->bty, n1, uconst(c, VKA_GMEM_REPLAY_SLOTS)), uconst(c, 0u), n1);
    emit_store(c, c->nxt, emit_val3(c, OP_SELECT, c->u32, found, nwrap, n0));
    uint32_t last = emit_val2(c, OP_IEQ, c->bty, idx, iconst(c, VKA_GMEM_TABLE_LAST));
    uint32_t nidx = emit_val3(c, OP_SELECT, c->ity, last, idx, emit_val2(c, OP_IADD, c->ity, idx, iconst(c, 1u)));
    uint32_t ff = uconst(c, 0xffffffffu), z0 = uconst(c, 0u);
    uint32_t ns0 = emit_val3(c, OP_SELECT, c->u32, last, ff, emit_tbl_word(c, nidx, 0u));
    uint32_t ns1 = emit_val3(c, OP_SELECT, c->u32, last, ff, emit_tbl_word(c, nidx, 1u));
    uint32_t gap[4] = {emit_val3(c, OP_SELECT, c->u32, below, ws[2], z0), emit_val3(c, OP_SELECT, c->u32, below, ws[3], z0),
                       emit_val3(c, OP_SELECT, c->u32, below, ns0, ws[0]), emit_val3(c, OP_SELECT, c->u32, below, ns1, ws[1])};
    uint32_t nf = emit_val1(c, OP_LNOT, c->bty, found), g0 = emit_load(c, c->u32, c->gnxt);
    for (uint32_t gi = 0; gi < VKA_GMEM_REPLAY_GAPS; gi++) {
        uint32_t here = emit_val2(c, OP_LAND, c->bty, nf, emit_val2(c, OP_IEQ, c->bty, g0, uconst(c, gi)));
        for (int w = 0; w < 4; w++) emit_store(c, c->gaps[gi][w], emit_val3(c, OP_SELECT, c->u32, here, gap[w], emit_load(c, c->u32, c->gaps[gi][w])));
    }
    uint32_t g1 = emit_val2(c, OP_IADD, c->u32, g0, uconst(c, 1u));
    uint32_t gwrap = emit_val3(c, OP_SELECT, c->u32, emit_val2(c, OP_IEQ, c->bty, g1, uconst(c, VKA_GMEM_REPLAY_GAPS)), z0, g1);
    emit_store(c, c->gnxt, emit_val3(c, OP_SELECT, c->u32, nf, gwrap, g0));
}

static void emit_learn(gr_ctx_t *c, uint32_t cur_lbl, uint32_t lu, uint32_t hu) {
    uint32_t hidx_i = 0, hok = emit_learn_hint(c, lu, hu, &hidx_i);
    uint32_t sb = new_id(c), sh = new_id(c), sbb = new_id(c), sc = new_id(c), sm = new_id(c), smm = new_id(c);
    uint32_t x0 = new_id(c), s0 = new_id(c), x1 = new_id(c), s1 = new_id(c), idx = new_id(c);
    { uint32_t a[2] = {smm, 0}; emit_ins(c, OP_SMERGE, a, 2); }
    { uint32_t a[3] = {hok, smm, sb}; emit_ins(c, OP_BRC, a, 3); }
    emit_ins(c, OP_LABEL, &sb, 1); emit_ins(c, OP_BR, &sh, 1);
    emit_ins(c, OP_LABEL, &sh, 1);
    { uint32_t a[6] = {c->ity, x0, iconst(c, 0u), sb, x1, sc}; emit_ins(c, OP_PHI, a, 6); }
    { uint32_t a[6] = {c->ity, s0, iconst(c, 4096u), sb, s1, sc}; emit_ins(c, OP_PHI, a, 6); }
    { uint32_t a[3] = {sm, sc, LOOP_DONT_UNROLL}; emit_ins(c, OP_LMERGE, a, 3); }
    emit_ins(c, OP_BR, &sbb, 1);
    emit_ins(c, OP_LABEL, &sbb, 1);
    uint32_t nx = emit_val2(c, OP_IADD, c->ity, x0, s0);
    uint32_t le = emit_lt64(c, emit_tbl_word(c, nx, 1u), emit_tbl_word(c, nx, 0u), hu, lu, 1);
    { uint32_t a[5] = {c->ity, x1, le, nx, x0}; emit_ins(c, OP_SELECT, a, 5); }
    { uint32_t a[4] = {c->ity, s1, s0, iconst(c, 1u)}; emit_ins(c, OP_SHR_L, a, 4); }
    uint32_t more = emit_val2(c, OP_INEQ, c->bty, s1, iconst(c, 0u));
    emit_ins(c, OP_BR, &sc, 1);
    emit_ins(c, OP_LABEL, &sc, 1);
    { uint32_t a[3] = {more, sh, sm}; emit_ins(c, OP_BRC, a, 3); }
    emit_ins(c, OP_LABEL, &sm, 1); emit_ins(c, OP_BR, &smm, 1);
    emit_ins(c, OP_LABEL, &smm, 1);
    { uint32_t a[6] = {c->ity, idx, hidx_i, cur_lbl, x1, sm}; emit_ins(c, OP_PHI, a, 6); }
    emit_learn_update(c, idx, lu, hu);
}

static void emit_main_and_body(gr_ctx_t *c) {
    const fn_span_t *mf = &c->funcs[c->main_fn_idx];
    size_t p_fn = ins_of(c, mf->first_ins);
    uint32_t void_ty = c->w[p_fn + 1], fn_ctrl = c->w[p_fn + 3], fn_ty = c->w[p_fn + 4];
    uint32_t body_id = new_id(c);
    uint32_t e = new_id(c), h = new_id(c), bd = new_id(c), kk = new_id(c), s = new_id(c), cont = new_id(c), m = new_id(c);
    { uint32_t a[4] = {void_ty, c->entry_fn, fn_ctrl, fn_ty}; emit_ins(c, OP_FN, a, 4); }
    emit_ins(c, OP_LABEL, &e, 1); emit_ins(c, OP_BR, &h, 1);
    emit_ins(c, OP_LABEL, &h, 1);
    { uint32_t a[3] = {m, cont, 0u}; emit_ins(c, OP_LMERGE, a, 3); }
    emit_ins(c, OP_BR, &bd, 1); emit_ins(c, OP_LABEL, &bd, 1);
    emit_store(c, c->missed, uconst(c, 0u));
    { uint32_t a[3] = {void_ty, new_id(c), body_id}; emit_ins(c, OP_CALL, a, 3); }
    uint32_t again = emit_val2(c, OP_INEQ, c->bty, emit_load(c, c->u32, c->missed), uconst(c, 0u));
    { uint32_t a[2] = {s, 0u}; emit_ins(c, OP_SMERGE, a, 2); }
    { uint32_t a[3] = {again, kk, s}; emit_ins(c, OP_BRC, a, 3); }
    emit_ins(c, OP_LABEL, &kk, 1);
    emit_learn(c, kk, emit_load(c, c->u32, c->pend_lo), emit_load(c, c->u32, c->pend_hi));
    emit_ins(c, OP_BR, &s, 1); emit_ins(c, OP_LABEL, &s, 1); emit_ins(c, OP_BR, &cont, 1);
    emit_ins(c, OP_LABEL, &cont, 1);
    uint32_t r1 = emit_val2(c, OP_IADD, c->u32, emit_load(c, c->u32, c->nruns), uconst(c, 1u));
    emit_store(c, c->nruns, r1);
    uint32_t more = emit_val2(c, OP_LAND, c->bty, again, emit_val2(c, OP_ULT, c->bty, r1, uconst(c, VKA_GMEM_REPLAY_RUNS)));
    { uint32_t a[3] = {more, h, m}; emit_ins(c, OP_BRC, a, 3); }
    emit_ins(c, OP_LABEL, &m, 1); emit_ins(c, OP_RET, NULL, 0); emit_ins(c, OP_FN_END, NULL, 0);
    { uint32_t a[4] = {void_ty, body_id, 0u, fn_ty}; emit_ins(c, OP_FN, a, 4); }
    int first = 1;
    for (size_t i = mf->first_ins + 1u; i <= mf->last_ins; i++) {
        size_t p = ins_of(c, i);
        uint32_t op = c->w[p] & 0xffffu, len = c->w[p] >> 16;
        if (op == OP_CALL) {
            int fi = find_func_index(c, c->w[p + 3]);
            if (first) {
                uint32_t ss = new_id(c), sj = new_id(c);
                uint32_t lu = as_u32(c, c->w[p + 4]), hu = as_u32(c, c->w[p + 5]);
                uint32_t need = emit_val2(c, OP_IEQ, c->bty, emit_load(c, c->u32, c->seeded), uconst(c, 0u));
                { uint32_t a[2] = {sj, 0u}; emit_ins(c, OP_SMERGE, a, 2); }
                { uint32_t a[3] = {need, ss, sj}; emit_ins(c, OP_BRC, a, 3); }
                emit_ins(c, OP_LABEL, &ss, 1); emit_store(c, c->seeded, uconst(c, 1u));
                emit_learn(c, ss, lu, hu);
                emit_ins(c, OP_BR, &sj, 1); emit_ins(c, OP_LABEL, &sj, 1);
                first = 0;
            }
            emit_site(c, &c->kinds[fi], c->w[p + 1], c->w[p + 2], c->w[p + 4], c->w[p + 5]);
        } else {
            emit_arr(c, &c->w[p], len);
        }
    }
}

static void emit_module(gr_ctx_t *c) {
    emit_arr(c, c->w, 5);
    for (size_t k = 0; k < c->first_fn; k++) {
        size_t p = ins_of(c, k);
        uint32_t op = c->w[p] & 0xffffu, len = c->w[p] >> 16;
        if ((op == OP_NAME || op == OP_DECORATE || op == OP_DEC_ID || op == OP_DEC_STR) && c->w[p + 1] < c->bound && c->gone_id[c->w[p + 1]]) continue;
        emit_arr(c, &c->w[p], len);
    }
    emit_new_decls(c);
    for (int f = 0; f < c->nfuncs; f++) {
        if (f == c->main_fn_idx) emit_main_and_body(c);
        else if (c->kinds[f].kind == RK_NONE) {
            for (size_t i = c->funcs[f].first_ins; i <= c->funcs[f].last_ins; i++) {
                size_t p = ins_of(c, i);
                emit_arr(c, &c->w[p], c->w[p] >> 16);
            }
        }
    }
    c->out[3] = c->next_id;
}

static gr_ctx_t *ctx_alloc(const uint32_t *w, size_t nw, uint32_t bound) {
    gr_ctx_t *c = calloc(1, sizeof *c);
    uint8_t *blk = NULL;
    if (!c) return NULL;
    size_t b = (size_t)bound;
    size_t need = nw * sizeof(size_t) + b * (7u * sizeof(uint32_t) + 3u);
    c->offs = calloc(1, need);
    if (!c->offs) { free(c); return NULL; }
    blk = (uint8_t *)(c->offs + nw);
    c->def_at = (uint32_t *)blk; blk += b * sizeof(uint32_t);
    c->dset = (uint32_t *)blk; blk += b * sizeof(uint32_t);
    c->dbind = (uint32_t *)blk; blk += b * sizeof(uint32_t);
    c->var_ptr = (uint32_t *)blk; blk += b * sizeof(uint32_t);
    c->var_sc = (uint32_t *)blk; blk += b * sizeof(uint32_t);
    c->type_at = (uint32_t *)blk; blk += b * sizeof(uint32_t);
    c->cval_at = (uint32_t *)blk; blk += b * sizeof(uint32_t);
    c->has_set = blk; blk += b; c->has_bind = blk; blk += b; c->gone_id = blk;
    c->w = w; c->nw = nw; c->bound = bound; c->main_fn_idx = -1;
    return c;
}

static void ctx_free(gr_ctx_t *c, uint8_t *written) {
    free(written);
    if (c) { free(c->offs); free(c); }
}

uint32_t *vka_spv_gmem_replay(const uint32_t *w, size_t nw, uint32_t table_set, uint32_t table_binding,
                              size_t *out_nw, int *out_sites, char *err, size_t errn) {
    assert(out_nw != NULL && out_sites != NULL);
    *out_nw = 0; *out_sites = 0;
    if (err && errn) err[0] = '\0';
    if (!w || nw < 5u || nw > GR_MAX_WORDS || w[0] != SPV_MAGIC) { set_err(err, errn, "not SPIR-V"); return NULL; }
    if (w[1] >= SPV_V1_4 || !w[3] || w[3] > GR_MAX_BOUND) return NULL;
    gr_ctx_t *c = ctx_alloc(w, nw, w[3]);
    uint8_t *written = calloc(w[3], 1);
    if (!c || !written) { set_err(err, errn, "out of memory"); ctx_free(c, written); return NULL; }
    if (!scan_module(c, table_set, table_binding, err, errn) || !check_main_body(c, written) || !verify_no_surviving_calls(c)) {
        ctx_free(c, written); return NULL;
    }
    mark_gone_ids(c);
    plan_types_and_vars(c);
    c->out_cap = nw + 4096u + (size_t)c->main_calls * 2560u;
    c->out = malloc(c->out_cap * sizeof(uint32_t));
    if (!c->out) { set_err(err, errn, "out of memory"); ctx_free(c, written); return NULL; }
    emit_module(c);
    if (c->err_oom) { set_err(err, errn, "output buffer overflow"); free(c->out); ctx_free(c, written); return NULL; }
    assert(c->out_n >= 5u && c->out[0] == SPV_MAGIC && c->out[3] == c->next_id);
    uint32_t *shrunk = realloc(c->out, c->out_n * sizeof(uint32_t));
    if (shrunk) c->out = shrunk;
    *out_nw = c->out_n; *out_sites = c->main_calls;
    uint32_t *res = c->out;
    ctx_free(c, written);
    return res;
}
