// Unreal's heap on the host keeps each thread's recently freed small blocks intact: a game can read a small block it has just freed (TMap::Add(Key, *Found) copies from its own
// storage after growing it, nxnative #2345), and Unreal's binned allocator leaves such a block intact until it is reused (hle_uemalloc.c FREED_RING)
#include "../hle_uemalloc.c"
#include <assert.h>
#include <pthread.h>

module_t *g_mod;
int g_nmod;
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
int dm_map_source_overlap(uint64_t addr, uint64_t size, uint64_t *s, uint64_t *n) { (void)addr; (void)size; (void)s; (void)n; return 0; }
int gate_obs_present(int id) { (void)id; return 0; }
void gate_obs_set(int id, void (*callback)(uint64_t *, int)) { (void)id; (void)callback; }

// Apple's zones count their blocks; the process heap elsewhere (and a sanitizer's zones, which report the whole heap) cannot say which blocks are this heap's
#if defined(__APPLE__) && !__has_feature(address_sanitizer) && !__has_feature(thread_sanitizer)
#define COUNTS_ZONE_BLOCKS 1
static size_t blocks_in_zone(void) { malloc_statistics_t s; malloc_zone_statistics((malloc_zone_t *)g_zone, &s); return s.blocks_in_use; }
#else
#define COUNTS_ZONE_BLOCKS 0
static size_t blocks_in_zone(void) { return 0; }
#endif
static void assert_filled(const uint8_t *p, size_t n, uint8_t v) { for (size_t i = 0; i < n; i++) assert(p[i] == v); }
static void *free_small_blocks(void *arg) {
    uintptr_t seed = (uintptr_t)arg;
    for (unsigned i = 0; i < 20000; i++) {
        size_t n = 16 + ((seed + i) * 37) % 2048;
        uint8_t *p = m_Malloc(NULL, n, 0);
        memset(p, (int)(seed + 1), n);
        p = m_Realloc(NULL, p, n + 64, 0);
        assert_filled(p, n, (uint8_t)(seed + 1));
        m_Free(NULL, p);
    }
    return NULL;
}

int main(void) {
    g_zone = plat_zone_create("guest Unreal heap");
    enum { kept = 64, grown = 4096, limit = FREED_MAX, more = FREED_RING * 4 };
    uint8_t *freed = m_Malloc(NULL, kept, 0);
    memset(freed, 0xa5, kept);
    m_Free(NULL, freed);
    assert_filled(freed, kept, 0xa5);
    // A growing Realloc moves the block and leaves the old one readable, as the map's copy expects
    uint8_t *old = m_Malloc(NULL, kept, 0);
    memset(old, 0x3c, kept);
    uint8_t *moved = m_Realloc(NULL, old, grown, 0);
    assert(moved != old);
    assert_filled(old, kept, 0x3c);
    assert_filled(moved, kept, 0x3c);
    // A size the block still holds stays put, and a shrink past half gives the larger block back while leaving it readable
    size_t usable = plat_malloc_size(moved);
    assert(m_Realloc(NULL, moved, usable - 1, 0) == moved);
    uint8_t *less = m_Realloc(NULL, moved, kept, 0);
    assert(less != moved);
    assert_filled(moved, kept, 0x3c);
    assert_filled(less, kept, 0x3c);
    m_Free(NULL, less);
    // Shrinking a minimum-size block that cannot get smaller stays in place rather than copying into the same size class
    uint8_t *tiny = m_Malloc(NULL, 16, 0);
    if (plat_malloc_good_size(8) >= plat_malloc_size(tiny)) assert(m_Realloc(NULL, tiny, 8, 0) == tiny);
    // Realloc to 0 also keeps the small block intact briefly
    memset(tiny, 0x6d, 8);
    assert(m_Realloc(NULL, tiny, 0, 0) == NULL);
    assert_filled(tiny, 8, 0x6d);
    // The per-thread ring holds a bounded number of blocks: the oldest is really freed
    for (unsigned i = 0; i < more; i++) m_Free(NULL, m_Malloc(NULL, 128, 0));
    assert(!COUNTS_ZONE_BLOCKS || blocks_in_zone() <= FREED_RING);
    // A block over the limit is not kept
    size_t before = blocks_in_zone();
    m_Free(NULL, m_Malloc(NULL, limit + 4096, 0));
    assert(!COUNTS_ZONE_BLOCKS || blocks_in_zone() == before);
    // Frees from other threads neither evict this thread's just-freed block nor leak after those threads exit
    uint8_t *pinned = m_Malloc(NULL, kept, 0);
    memset(pinned, 0x9b, kept);
    m_Free(NULL, pinned);
    pthread_t threads[4];
    for (uintptr_t i = 0; i < 4; i++) assert(!pthread_create(&threads[i], NULL, free_small_blocks, (void *)i));
    for (unsigned i = 0; i < 4; i++) assert(!pthread_join(threads[i], NULL));
    assert_filled(pinned, kept, 0x9b);
    assert(!COUNTS_ZONE_BLOCKS || blocks_in_zone() <= FREED_RING);
    // Trim drains the calling thread's ring back to the zone
    m_Trim(NULL);
    assert(!COUNTS_ZONE_BLOCKS || blocks_in_zone() == 0);
    assert(atomic_load(&g_used) == 0);
    puts("Unreal heap: recently freed small blocks stay intact, the ring is bounded and drains: OK");
    return 0;
}
