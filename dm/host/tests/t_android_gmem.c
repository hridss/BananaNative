// Raw global memory's pool table (vka_gmem.h; vka_draw.c gmem_map, vka_mem.c vka_mem_sink) read as the translator's helper reads it (GlobalToStorage.cs
// TryGenerateRawStorageOp: the 2 MiB chunk's hint in word 6 of entry chunk & 8191, else the 13-step binary search, then the inPool check). The hints never touch
// entry 0, whose words 6 and 7 are the sink (c = 8192, 16384, ... overwrote its low word), an 8 KiB-aligned sink never reads as a hint, an unused entry's word 6 is
// no hint, and every address of 600 pools (past the 64 a 6-step search found) finds its own pool, by hint or by search. Word 7's spans (vka_gmem_span) and a raw
// load's check of its last entry with them (PR #1833) choose the lookup's entry, and only because entry 0, whose word 7 is the sink's high word, is never reused
#include <stdio.h>
#include <stdlib.h>
#include "../../gfx/vk_android/vka_gmem.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define N DMSH_GMEM_MAP_ENTRIES
#define POOLS 600

static uint8_t table[(size_t)N * DMSH_GMEM_MAP_ENTRY];
static uint32_t w(uint32_t e, int word) { uint32_t v; memcpy(&v, table + (size_t)e * DMSH_GMEM_MAP_ENTRY + word * 4, 4); return v; }
static uint64_t start(uint32_t e) { return (uint64_t)w(e, 1) << 32 | w(e, 0); }
static uint64_t end(uint32_t e) { return (uint64_t)w(e, 3) << 32 | w(e, 2); }

// The helper's lookup (without the entry chosen last): → the entry, 0 when the address is in no pool; *by_hint whether the hint chose it
static uint32_t lookup(uint64_t a, int *by_hint) {
    uint32_t lo = (uint32_t)a, hi = (uint32_t)(a >> 32), chunk = hi << (32 - DMSH_GMEM_HINT_SHIFT) | lo >> DMSH_GMEM_HINT_SHIFT;
    uint32_t hint = w(chunk & (N - 1), VKA_GMEM_HINT_WORD), hidx = hint & (N - 1), sel;
    *by_hint = hidx != 0 && hint >> VKA_GMEM_INDEX_BITS == chunk && hi < (1u << (DMSH_GMEM_HINT_SHIFT - VKA_GMEM_INDEX_BITS));
    if (*by_hint) sel = hidx;
    else {
        sel = 0;
        for (uint32_t step = N / 2; step; step >>= 1) if (start(sel + step) <= a) sel += step;
    }
    return a < end(sel) ? sel : 0;
}

static void make(const uint64_t *lo, const uint64_t *hi, int k, uint64_t sink) {
    memset(table, 0xff, sizeof table); memset(table, 0, DMSH_GMEM_MAP_ENTRY);
    memcpy(table + 24, &sink, 8);
    for (int i = 0; i < k; i++) { uint64_t e[4] = {lo[i], hi[i], 0x1000, (uint64_t)vka_gmem_span(lo[i], hi[i]) << 32}; memcpy(table + (size_t)(i + 1) * DMSH_GMEM_MAP_ENTRY, e, sizeof e); }
    vka_gmem_hints(table, lo, hi, k);
}

// A raw load's check of the entry it chose last (GlobalToStorage.cs, PR #1833): never entry 0 (guard), the start's high word, the offset below word 7's span
static int last_holds(uint32_t last, uint64_t a, int guard) {
    uint32_t lo = (uint32_t)a, hi = (uint32_t)(a >> 32);
    return (!guard || last != 0) && hi == w(last, 1) && lo - w(last, 0) < w(last, VKA_GMEM_SPAN_WORD);
}
// An invocation's run of loads: each takes its last entry when the check holds, else the lookup (which it keeps, a miss's entry 0 included). A held entry goes
// straight to the access with its delta, with no in-pool check after it, so a load held by entry 0 reads its own address as a device address instead of the sink.
// → the number of loads that read elsewhere than the lookup would have them read
static int walk(const uint64_t *seq, int n, int guard, int *reused) {
    uint32_t last = 0; int bad = 0; *reused = 0;
    for (int s = 0; s < n; s++) {
        int h; uint32_t want = lookup(seq[s], &h);
        if (last_holds(last, seq[s], guard)) { (*reused)++; bad += last != want || last == 0; }
        else last = want;
    }
    return bad;
}

int main(void) {
    static uint64_t lo[POOLS], hi[POOLS];
    // pools of 6 MiB every 16 MiB from 1 GiB (to about 10 GiB), the last one at 16 GiB
    for (int i = 0; i < POOLS; i++) { lo[i] = (1ull << 30) + (uint64_t)i * (16ull << 20); hi[i] = lo[i] + (6ull << 20); }
    lo[POOLS - 1] = 8192ull << DMSH_GMEM_HINT_SHIFT; hi[POOLS - 1] = lo[POOLS - 1] + (8ull << 20);   // one holding chunk 8192 whole (entry 0's slot)
    CHECK(lo[POOLS - 2] < lo[POOLS - 1], "sorted by start");
    uint64_t sink = vka_gmem_sink_at(0x7f3a12345000ull);
    CHECK(sink % VKA_GMEM_SINK_ALIGN == 0 && sink >= 0x7f3a12345000ull && sink + 16 <= 0x7f3a12345000ull + 2 * VKA_GMEM_SINK_ALIGN, "the sink inside its 16 KiB, 8 KiB aligned (%llx)", (unsigned long long)sink);
    make(lo, hi, POOLS, sink);
    uint64_t got; memcpy(&got, table + 24, 8);
    CHECK(got == sink, "entry 0's words 6 and 7 are still the sink (%llx, not %llx): no hint was written there", (unsigned long long)got, (unsigned long long)sink);
    CHECK(vka_gmem_hint_entry(8192) == 0 && vka_gmem_hint_entry(16384) == 0 && vka_gmem_hint_entry(8193) == 1, "chunks 8192, 16384 map to entry 0");
    // every address of every pool: its own pool; just past each: none
    int hinted = 0, searched = 0;
    for (int i = 0; i < POOLS; i++) {
        for (uint64_t a = lo[i]; a < hi[i]; a += 0x7ff01) {
            int h; uint32_t e = lookup(a, &h);
            CHECK(e == (uint32_t)i + 1, "address %llx of pool %d found entry %u", (unsigned long long)a, i, e);
            h ? hinted++ : searched++;
        }
        int h; CHECK(lookup(hi[i], &h) == 0, "the end of pool %d is in no pool", i);
    }
    CHECK(hinted > 0 && searched > 0, "both paths taken (%d by hint, %d by search)", hinted, searched);
    { int h; uint32_t e = lookup(lo[POOLS - 1] + 4096, &h); CHECK(e == POOLS && !h, "chunk 8192: no hint (the sink's word), found by the search (%u)", e); }
    { int h; CHECK(lookup(0xFFFFE00000ull, &h) == 0 && !h, "chunk 2^19 - 1 (an unused entry's word 6 was ~0): no hint, no pool"); }
    CHECK(lookup(0, &(int){0}) == 0, "address 0: entry 0, which holds nothing");
    // a sink that is not aligned would read as a hint for chunk 8192 when its tag matches; an aligned one never names an entry
    for (uint64_t raw = 0x100000000ull; raw < 0x100000000ull + (1ull << 26); raw += 4096 * 7) {
        uint32_t s = (uint32_t)vka_gmem_sink_at(raw);
        CHECK((s & (N - 1)) == 0, "sink %llx: its low word names entry %u", (unsigned long long)vka_gmem_sink_at(raw), s & (N - 1));
    }
    // spans: a range inside one 4 GiB window gets its length, one straddling a window or 4 GiB long gets 0 (the full lookup), and entry 0 keeps the sink's high word
    CHECK(vka_gmem_span(0x100000000ull, 0x100600000ull) == 0x600000, "a 6 MiB range: its span");
    CHECK(vka_gmem_span(0xFFFFF000ull, 0x100001000ull) == 0, "a range across the 4 GiB boundary: no span");
    CHECK(vka_gmem_span(0x100000000ull, 0x200000000ull) == 0, "a 4 GiB range: no span (its length does not fit 32 bits)");
    CHECK(vka_gmem_span(0x1FFFFF000ull, 0x200000000ull) == 0x1000, "a range ending at the window's end: its span");
    CHECK(vka_gmem_span(0x5000, 0x5000) == 0, "an empty range: no span");
    CHECK(w(0, VKA_GMEM_SPAN_WORD) == (uint32_t)(sink >> 32) && w(0, VKA_GMEM_SPAN_WORD) != 0, "entry 0's word 7 is the sink's high word (%x), not a span", w(0, 7));
    // runs of loads like a shader's: words of one pool, the next pool, back, a gap, low addresses in the first window, a pool above 4 GiB, its end
    {
        static uint64_t seq[64]; int n = 0;
        for (int k = 0; k < 6; k++) seq[n++] = lo[3] + 4 * k;
        seq[n++] = lo[4]; seq[n++] = lo[4] + 64; seq[n++] = lo[3] + 0x100;
        seq[n++] = hi[3] + 4;   // the gap after pool 3: no pool
        for (uint64_t a = 0; a < 0x40; a += 4) seq[n++] = a;   // the first window's low addresses: in no pool
        seq[n++] = lo[3] + 8;
        seq[n++] = lo[300]; seq[n++] = lo[300] + 4; seq[n++] = hi[300] - 4; seq[n++] = hi[300];
        seq[n++] = lo[300] - 4; seq[n++] = (lo[300] & 0xFFFFFFFFull) + 4;   // just below the start; the same low word one window down
        int reused, bad = walk(seq, n, 1, &reused);
        CHECK(bad == 0, "the span check with entry 0 excluded picks the lookup's entry for every load (%d of %d differ)", bad, n);
        CHECK(reused >= 8, "loads in the same pool reuse their entry (%d of %d)", reused, n);
        int r0, bad0 = walk(seq, n, 0, &r0);
        CHECK(bad0 > 0, "without excluding entry 0 its sink word makes it hold the first window's low addresses (%d wrong)", bad0);
    }
    if (fails) { printf("t_android_gmem: %d failures\n", fails); return 1; }
    printf("t_android_gmem: ok (%d addresses by hint, %d by search, %d pools)\n", hinted, searched, POOLS);
    return 0;
}
