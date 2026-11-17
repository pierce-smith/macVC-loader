// limit adjuster, like open limit adjuster or project2dfx's limit raise on windows.
//


// world pools (buildings, dummies, sector list nodes): CPools::Initialise makes them at
// vanilla size, then right after, while they're still empty, they get bigger arrays.
// every loop over a pool uses the pool's own size and these pools aren't saved, so it's safe.
//


// simple objects (3885) and timed objects like neon signs (385) are fixed size stores and
// the game asserts when one fills up. map mods blow past both, so once a store is full
// these hooks hand out entries from an overflow pool. only CModelInfo's own
// Add/Initialise/ShutDown touch the stores directly, everything else goes through
// ms_modelInfoPtrs, so an entry living somewhere else works the same.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "internal.h"

typedef struct {
    const char *name, *add_sym, *store_sym;
    int extra;              // overflow entries
    int limit, size;        // read from the Add function's code
    uint8_t *store;         // CStore<T, N>: int allocated; T store[N]
    uint8_t *pool, *pristine;
    int used;
    void *(*orig)(int);
} Store;

static Store g_simple = {.name = "simple objects", .add_sym = "_ZN10CModelInfo14AddSimpleModelEi", .store_sym = "_ZN10CModelInfo19ms_simpleModelStoreE", .extra = 4000};
static Store g_time = {.name = "timed objects", .add_sym = "_ZN10CModelInfo12AddTimeModelEi", .store_sym = "_ZN10CModelInfo17ms_timeModelStoreE", .extra = 1000};
static void **g_ptrs;
static void (*f_Init)(void *);
static void (*o_Initialise)(void);

// "cmp wN, #limit" ... "mov wN, #size" "smaddl" in the inlined CStore::Alloc
static int decode(Store *s) {
    uint32_t *c = mvc_sym(s->add_sym);
    if (!c) return 0;
    for (int i = 0; i < 48; i++) {
        if ((c[i] & 0xFF80001F) == 0x7100001F && !s->limit) s->limit = (int)((c[i] >> 10) & 0xFFF) << ((c[i] >> 22) & 1 ? 12 : 0);
        if ((c[i] & 0xFFE08000) == 0x9B200000 && i && (c[i - 1] & 0xFF800000) == 0x52800000) {
            s->size = (int)((c[i - 1] >> 5) & 0xFFFF);
            break;
        }
    }
    return s->limit > 0 && s->size > 0;
}

static void *overflow(Store *s, int id) {
    if (s->used >= s->extra) return NULL;
    if (s->used == 0) mvc_log("[Loader] the game's %s limit (%d) is reached - using the loader's extra room for %d more",
                              s->name, s->limit, s->extra);
    uint8_t *e = s->pool + (size_t)s->used++ * (size_t)s->size;
    memcpy(e, s->pristine, (size_t)s->size); // vtable and defaults, as the game's static setup leaves them
    g_ptrs[id] = e;
    f_Init(e);
    return e;
}

static void *h_AddSimple(int id) {
    if (*(int32_t *)g_simple.store < g_simple.limit) return g_simple.orig(id);
    void *e = overflow(&g_simple, id);
    return e ? e : g_simple.orig(id); // out of room: let the game report it
}
static void *h_AddTime(int id) {
    if (*(int32_t *)g_time.store < g_time.limit) return g_time.orig(id);
    void *e = overflow(&g_time, id);
    return e ? e : g_time.orig(id);
}

static void h_Initialise(void) {
    // the store's last entry has never been handed out yet: keep it as the template
    Store *all[] = {&g_simple, &g_time};
    for (int i = 0; i < 2; i++) {
        Store *s = all[i];
        if (s->pristine) memcpy(s->pristine, s->store + 8 + (size_t)(s->limit - 1) * (size_t)s->size, (size_t)s->size);
        s->used = 0;
    }
    o_Initialise();
}

// world pools
typedef struct { void *entries; uint8_t *flags; int32_t size, alloc; } CPool; // reVC's CPool<T>
typedef struct { const char *sym, *name; int want; uintptr_t addr; int size, elem; } Pool;
static Pool g_pools[] = {
    {.sym = "_ZN6CPools15ms_pPtrNodePoolE", .name = "sector-list nodes", .want = 200000},
    {.sym = "_ZN6CPools16ms_pBuildingPoolE", .name = "buildings", .want = 40000},
    {.sym = "_ZN6CPools13ms_pDummyPoolE", .name = "dummies", .want = 10000},
    {.sym = "_ZN6CPools16ms_pColModelPoolE", .name = "collision models", .want = 12000}, // map + ped hit models
};
#define NPOOLS (int)(sizeof g_pools / sizeof *g_pools)
static void (*o_PoolsInit)(void);

// reads each pool's element size from CPools::Initialise: per pool it calls
// new(0x18) for the CPool, new[](size * elem), new[](size), then stores the
// CPool* into its global ("str x19, [xN, #off]").
static int decode_pools(void) {
    uint32_t *c = mvc_sym("_ZN6CPools10InitialiseEv");
    if (!c) return 0;
    uint64_t w0 = 0, page[32] = {0}, calls[8];
    int ncalls = 0, found = 0;
    for (int i = 0; i < 700; i++) {
        uint32_t in = c[i];
        if (in == 0xD65F03C0) break; // ret
        if ((in & 0xFF80001F) == 0x52800000) w0 = (uint64_t)((in >> 5) & 0xFFFF) << (((in >> 21) & 3) * 16);   // movz w0
        else if ((in & 0xFF80001F) == 0x72800000) {                                                               // movk w0
            int sh = (int)((in >> 21) & 3) * 16;
            w0 = (w0 & ~(0xFFFFULL << sh)) | ((uint64_t)((in >> 5) & 0xFFFF) << sh);
        } else if ((in & 0xFC000000) == 0x94000000) { if (ncalls < 8) calls[ncalls++] = w0; }                    // bl
        else if ((in & 0x9F000000) == 0x90000000) {                                                               // adrp
            uint64_t raw = (((uint64_t)(in >> 5) & 0x7FFFF) << 2) | ((in >> 29) & 3);
            page[in & 31] = ((uintptr_t)&c[i] & ~0xFFFULL) + (uint64_t)(((int64_t)(raw << 43) >> 43) * 4096);
        } else if ((in & 0xFFC0001F) == 0xF9000013 && ncalls >= 3) {                                              // str x19, [xn, #imm]
            uintptr_t at = page[(in >> 5) & 31] + ((in >> 10) & 0xFFF) * 8;
            uint64_t bytes = calls[ncalls - 2], count = calls[ncalls - 1];
            for (int p = 0; p < NPOOLS; p++)
                if (g_pools[p].addr == at && count && bytes % count == 0) {
                    g_pools[p].size = (int)count;
                    g_pools[p].elem = (int)(bytes / count);
                    found++;
                }
            ncalls = 0;
        }
    }
    return found;
}

static void h_PoolsInit(void) {
    o_PoolsInit();
    for (int p = 0; p < NPOOLS; p++) {
        Pool *P = &g_pools[p];
        CPool *pool = *(CPool **)P->addr;
        if (!pool || !P->elem || pool->size != P->size || P->want <= P->size) continue;
        void *e = calloc((size_t)P->want, (size_t)P->elem);
        uint8_t *f = malloc((size_t)P->want);
        if (!e || !f) { free(e); free(f); continue; }
        memset(f, 0x80, (size_t)P->want); // every slot free, as the game's own loop leaves them
        pool->entries = e;                // (the vanilla arrays are left alone, unused)
        pool->flags = f;
        pool->size = P->want;
    }
}

// how full everything is once the map has loaded
void limits_report(void) {
    char line[400] = "[Loader] pools after loading the map:";
    for (int p = 0; p < NPOOLS; p++) {
        CPool *pool = g_pools[p].addr ? *(CPool **)g_pools[p].addr : NULL;
        if (!pool) continue;
        int used = 0;
        for (int i = 0; i < pool->size; i++) used += !(pool->flags[i] & 0x80);
        char t[80];
        snprintf(t, sizeof t, " %s %d/%d,", g_pools[p].name, used, pool->size);
        strlcat(line, t, sizeof line);
    }
    CPool **txd = mvc_sym("_ZN9CTxdStore11ms_pTxdPoolE");
    if (txd && *txd) {
        int used = 0;
        for (int i = 0; i < (*txd)->size; i++) used += !((*txd)->flags[i] & 0x80);
        char t[80];
        snprintf(t, sizeof t, " texture dictionaries %d/%d,", used, (*txd)->size);
        strlcat(line, t, sizeof line);
    }
    char t[120];
    snprintf(t, sizeof t, " simple objects %d+%d/%d+%d, timed objects %d+%d/%d+%d", *(int32_t *)g_simple.store, g_simple.used,
             g_simple.limit, g_simple.extra, *(int32_t *)g_time.store, g_time.used, g_time.limit, g_time.extra);
    if (g_simple.store && g_time.store) strlcat(line, t, sizeof line);
    mvc_log("%s", line);
}

static void pools_init(void) {
    for (int p = 0; p < NPOOLS; p++) g_pools[p].addr = (uintptr_t)mvc_sym(g_pools[p].sym);
    if (decode_pools() != NPOOLS) { mvc_log_warn("[Loader] world pools: layout not recognised - pool sizes not raised"); return; }
    if (mvc_hook_sym("_ZN6CPools10InitialiseEv", (void *)h_PoolsInit, (void **)&o_PoolsInit)) return;
    char line[300] = "[Loader] world pools:";
    for (int p = 0; p < NPOOLS; p++) {
        char t[80];
        snprintf(t, sizeof t, " %s %d -> %d,", g_pools[p].name, g_pools[p].size, g_pools[p].want);
        strlcat(line, t, sizeof line);
    }
    line[strlen(line) - 1] = 0;
    mvc_log("%s", line);
}

// streaming memory
// CStreaming::Init2 sets the budget for streamed models and textures to
// max((physical memory - 40 MB) / 2, 65 MB). macVC reports a tiny physical
// memory size, so it's always 65 MB: with high-res texture packs the game keeps
// throwing models out and streaming them back in (popping, foliage fading in
// over and over). Windows limit adjusters raise it the same way.
static void (*o_StreamInit2)(void);
static size_t *g_stream_mem;

static void h_StreamInit2(void) {
    o_StreamInit2();
    size_t want = (size_t)g_cfg.streaming_mb << 20;
    if (g_stream_mem && want > *g_stream_mem) {
        mvc_log("[Loader] streaming memory: %zu MB -> %d MB", *g_stream_mem >> 20, g_cfg.streaming_mb);
        *g_stream_mem = want;
    }
}

static void streaming_init(void) {
    if (g_cfg.streaming_mb <= 0) return;
    g_stream_mem = mvc_sym("_ZN10CStreaming18ms_memoryAvailableE");
    if (!g_stream_mem || mvc_hook_sym("_ZN10CStreaming5Init2Ev", (void *)h_StreamInit2, (void **)&o_StreamInit2))
        mvc_log_warn("[Loader] streaming memory can't be raised on this macVC build");
}

void limits_init(void) {
    streaming_init();
    pools_init();
    g_ptrs = mvc_sym("_ZN10CModelInfo16ms_modelInfoPtrsE");
    *(void **)&f_Init = mvc_sym("_ZN16CSimpleModelInfo4InitEv");
    Store *all[] = {&g_simple, &g_time};
    int ok = g_ptrs && f_Init;
    for (int i = 0; i < 2 && ok; i++) {
        Store *s = all[i];
        s->store = mvc_sym(s->store_sym);
        if (!s->store || !decode(s)) { ok = 0; break; }
        s->pool = calloc((size_t)s->extra, (size_t)s->size);
        s->pristine = calloc(1, (size_t)s->size);
        if (!s->pool || !s->pristine) ok = 0;
    }
    if (!ok) { mvc_log_warn("[Loader] object limits: this macVC build is laid out differently - limits not raised"); return; }
    if (mvc_hook_sym("_ZN10CModelInfo10InitialiseEv", (void *)h_Initialise, (void **)&o_Initialise) ||
        mvc_hook_sym(g_simple.add_sym, (void *)h_AddSimple, (void **)&g_simple.orig) ||
        mvc_hook_sym(g_time.add_sym, (void *)h_AddTime, (void **)&g_time.orig)) {
        mvc_log_error("[Loader] object limits: hooks failed");
        return;
    }
    mvc_log("[Loader] object limits: simple %d + %d, timed %d + %d (entries of %d / %d bytes)", g_simple.limit,
            g_simple.extra, g_time.limit, g_time.extra, g_simple.size, g_time.size);
}
