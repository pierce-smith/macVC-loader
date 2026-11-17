// arm64 code patching: inline hooks, mid-function hooks, memory patches, pattern scans,
// and a list of everything that got patched.
//


// a hook only replaces the first instruction with one B into an island within +-128MB,
// so a branch somewhere else in the function can't land in the middle of the patch.
// the instruction that got replaced is relocated (pc-relative ones rewritten) so the
// original is still callable.
#include <dlfcn.h>
#include <libkern/OSCacheControl.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "internal.h"

static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
#define MAX_PATCHES 1024
static PatchRec g_rec[MAX_PATCHES];
static int g_nrec, g_next_id = 1;
static int g_startup = 1;

void hooks_set_startup(int s) { g_startup = s; }
int hook_count(void) { return g_nrec; }
const PatchRec *hook_get(int i) { return (i >= 0 && i < g_nrec) ? &g_rec[i] : NULL; }

const PatchRec *hook_find_island(uintptr_t a) {
    for (int i = 0; i < g_nrec; i++)
        if (g_rec[i].island && a >= g_rec[i].island && a < g_rec[i].island_end) return &g_rec[i];
    return NULL;
}

void owner_name(void *addr, char *out, size_t n) {
    out[0] = 0;
    Dl_info di;
    if (!addr || !dladdr(addr, &di) || !di.dli_fname) return;
    const char *b = strrchr(di.dli_fname, '/');
    b = b ? b + 1 : di.dli_fname;
    if (!strcmp(b, "macVCLoader.dylib")) return;
    snprintf(out, n, "%s", b);
    char *dot = strstr(out, ".dylib");
    if (dot) *dot = 0;
}

static void what_name(void *addr, char *out, size_t n) {
    Dl_info di;
    if (dladdr(addr, &di) && di.dli_sname) {
        uintptr_t off = (uintptr_t)addr - (uintptr_t)di.dli_saddr;
        if (off) snprintf(out, n, "%s+0x%lx", di.dli_sname, (unsigned long)off);
        else snprintf(out, n, "%s", di.dli_sname);
    } else snprintf(out, n, "%p", addr);
}

static int64_t sext(uint64_t v, int bits) {
    uint64_t m = 1ULL << (bits - 1);
    return (int64_t)((v ^ m) - m);
}

// memory access
static int region_prot(uintptr_t addr, vm_prot_t *prot) {
    mach_vm_address_t a = addr;
    mach_vm_size_t sz;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj;
    if (mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS)
        return -1;
    if (a > addr) return -1; // unmapped
    *prot = info.protection;
    return 0;
}

// writes bytes anywhere in the process (code or data), restoring protection.
static int write_raw(uintptr_t addr, const void *src, size_t len) {
    uintptr_t pg = vm_page_size;
    for (uintptr_t page = addr & ~(pg - 1); page < addr + len; page += pg) {
        vm_prot_t prot;
        if (region_prot(page, &prot)) return -1;
        uintptr_t s = page > addr ? page : addr;
        uintptr_t e = page + pg < addr + len ? page + pg : addr + len;
        if (!(prot & VM_PROT_WRITE) || (prot & VM_PROT_EXECUTE)) {
            if (mach_vm_protect(mach_task_self(), page, pg, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS)
                return -1;
            memcpy((void *)s, (const uint8_t *)src + (s - addr), e - s);
            mach_vm_protect(mach_task_self(), page, pg, FALSE, prot);
        } else {
            memcpy((void *)s, (const uint8_t *)src + (s - addr), e - s);
        }
        if (prot & VM_PROT_EXECUTE) sys_icache_invalidate((void *)s, e - s);
    }
    return 0;
}

// islands
// During startup (single-threaded, game not running yet) islands share pages.
// Later, every island gets its own page so flipping protection can never
// affect code another thread might be executing.
typedef struct { uintptr_t base, used, size; } Pool;
static Pool g_pools[16];
static int g_npools;

// a free page as close to `target` as possible (islands must be within the
// +-128MB reach of a B instruction, i keep it inside +-112MB). the unmapped gaps
// of the real memory map are tried nearest first, a few pages into each, so a
// small gap between two mappings is found too. some unmapped space is still
// reserved (__PAGEZERO, the shared cache), which is why allocation can fail
// and the next candidate is tried.
#define ISLAND_REACH 0x7000000ULL
static uintptr_t alloc_page_near(uintptr_t target) {
    uintptr_t pg = vm_page_size;
    uintptr_t lo = target > ISLAND_REACH ? (target - ISLAND_REACH + pg - 1) & ~(pg - 1) : pg;
    uintptr_t hi = (target + ISLAND_REACH) & ~(pg - 1);
    // the 64 candidates nearest the target, kept sorted by distance
    enum { NC = 64 };
    uintptr_t cand[NC], dist[NC];
    int n = 0;
#define ADD_CAND(p) do { uintptr_t c_ = (p), d_ = c_ > target ? c_ - target : target - c_; \
        if (d_ < ISLAND_REACH && (n < NC || d_ < dist[n - 1])) { int i_ = n < NC ? n++ : NC - 1; \
            while (i_ > 0 && dist[i_ - 1] > d_) { cand[i_] = cand[i_ - 1]; dist[i_] = dist[i_ - 1]; i_--; } \
            cand[i_] = c_; dist[i_] = d_; } } while (0)
    mach_vm_address_t a = lo;
    uintptr_t free_from = lo;
    for (;;) {
        mach_vm_size_t sz = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj;
        kern_return_t kr = mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj);
        uintptr_t gap_end = kr == KERN_SUCCESS && a < hi ? (uintptr_t)a : hi; // [free_from, gap_end) is unmapped
        if (gap_end >= free_from + pg) {
            // both ends of the gap, and 1 MB in from each (reserved space often starts at a gap's edge)
            ADD_CAND(gap_end - pg);
            ADD_CAND(free_from);
            if (gap_end - free_from > 0x200000) { ADD_CAND(gap_end - 0x100000); ADD_CAND(free_from + 0x100000); }
        }
        if (kr != KERN_SUCCESS || a >= hi) break;
        free_from = (uintptr_t)(a + sz);
        a += sz;
    }
#undef ADD_CAND
    for (int i = 0; i < n; i++) {
        mach_vm_address_t got = cand[i];
        if (mach_vm_allocate(mach_task_self(), &got, pg, VM_FLAGS_FIXED) != KERN_SUCCESS) continue;
        mach_vm_protect(mach_task_self(), got, pg, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
        return (uintptr_t)got;
    }
    return 0;
}

static int in_range(uintptr_t a, uintptr_t b) {
    int64_t d = (int64_t)(a - b);
    return d > -0x7000000 && d < 0x7000000;
}

static uintptr_t island_alloc(uintptr_t target, size_t bytes) {
    bytes = (bytes + 15) & ~15UL;
    if (g_startup) {
        for (int i = 0; i < g_npools; i++)
            if (in_range(g_pools[i].base, target) && g_pools[i].used + bytes <= g_pools[i].size) {
                uintptr_t a = g_pools[i].base + g_pools[i].used;
                g_pools[i].used += bytes;
                return a;
            }
    }
    uintptr_t p = alloc_page_near(target);
    if (!p) return 0;
    if (g_startup && g_npools < 16) {
        g_pools[g_npools++] = (Pool){p, bytes, vm_page_size};
    }
    return p;
}

// islands live on R-X pages; write them through write_raw.
static int island_write(uintptr_t island, const uint32_t *code, int words) {
    return write_raw(island, code, (size_t)words * 4);
}

// relocation
static int emit_abs_jump(uint32_t *p, uint64_t dest) {
    p[0] = 0x58000050; // LDR X16, #8
    p[1] = 0xD61F0200; // BR  X16
    memcpy(&p[2], &dest, 8);
    return 4;
}

static int emit_abs_cond(uint32_t *p, uint32_t rewritten, uint64_t dest) {
    p[0] = rewritten;   // conditional branch, now targeting p+2
    p[1] = 0x14000005;  // B +20 -> past the absolute jump
    emit_abs_jump(&p[2], dest);
    return 6;
}

// relocates one instruction from `pc` into `p`. returns words written (-1 if
// impossible); sets *ends when the instruction never falls through (B).
static int relocate(uint32_t *p, uint32_t i, uint64_t pc, int *ends) {
    *ends = 0;
    if ((i & 0x9F000000) == 0x90000000 || (i & 0x9F000000) == 0x10000000) { // ADRP / ADR
        int rd = i & 31;
        int64_t imm = sext((((i >> 5) & 0x7FFFF) << 2) | ((i >> 29) & 3), 21);
        uint64_t val = (i & 0x80000000) ? (pc & ~0xFFFULL) + (imm << 12) : pc + imm;
        p[0] = 0x58000040 | rd; // LDR Xd, #8
        p[1] = 0x14000003;      // B #12
        memcpy(&p[2], &val, 8);
        return 4;
    }
    if ((i & 0xFC000000) == 0x14000000) { // B
        *ends = 1;
        return emit_abs_jump(p, pc + sext(i & 0x3FFFFFF, 26) * 4);
    }
    if ((i & 0xFC000000) == 0x94000000) { // BL
        uint64_t dest = pc + sext(i & 0x3FFFFFF, 26) * 4;
        p[0] = 0x58000070; // LDR X16, #12
        p[1] = 0xD63F0200; // BLR X16
        p[2] = 0x14000003; // B #12
        memcpy(&p[3], &dest, 8);
        return 5;
    }
    if ((i & 0xFF000010) == 0x54000000) // B.cond
        return emit_abs_cond(p, (i & 0xFF00001F) | (2 << 5), pc + sext((i >> 5) & 0x7FFFF, 19) * 4);
    if ((i & 0x7E000000) == 0x34000000) // CBZ / CBNZ
        return emit_abs_cond(p, (i & 0xFF00001F) | (2 << 5), pc + sext((i >> 5) & 0x7FFFF, 19) * 4);
    if ((i & 0x7E000000) == 0x36000000) // TBZ / TBNZ
        return emit_abs_cond(p, (i & 0xFFF8001F) | (2 << 5), pc + sext((i >> 5) & 0x3FFF, 14) * 4);
    if ((i & 0x3B000000) == 0x18000000) { // LDR (literal)
        uint64_t addr = pc + sext((i >> 5) & 0x7FFFF, 19) * 4;
        int rt = i & 31, opc = i >> 30, v = (i >> 26) & 1;
        static const uint32_t gpr[] = {0xB9400220, 0xF9400220, 0xB9800220, 0};
        static const uint32_t fpr[] = {0xBD400220, 0xFD400220, 0x3DC00220, 0};
        if (!v && opc == 3) { p[0] = 0xD503201F; return 1; } // PRFM (literal): a hint, drop it
        uint32_t ld = v ? fpr[opc] : gpr[opc];
        if (!ld) return -1;
        p[0] = 0x58000051; // LDR X17, #8
        p[1] = 0x14000003; // B #12
        memcpy(&p[2], &addr, 8);
        p[4] = ld | rt;    // LDR <t>, [X17]
        return 5;
    }
    p[0] = i;
    return 1;
}

// registry
static PatchRec *rec_new(uintptr_t target, uint32_t len, int kind, void *owner_addr) {
    if (g_nrec >= MAX_PATCHES) return NULL;
    PatchRec *r = &g_rec[g_nrec++];
    memset(r, 0, sizeof *r);
    r->target = target;
    r->len = len;
    r->kind = kind;
    r->id = g_next_id++;
    r->active = 1;
    memcpy(r->orig, (void *)target, len > sizeof r->orig ? sizeof r->orig : len);
    owner_name(owner_addr, r->owner, sizeof r->owner);
    if (!r->owner[0]) snprintf(r->owner, sizeof r->owner, "Loader");
    what_name((void *)target, r->what, sizeof r->what);
    return r;
}

static void warn_overlap(uintptr_t a, uint32_t len, const char *owner, int kind) {
    for (int i = 0; i < g_nrec; i++) {
        PatchRec *r = &g_rec[i];
        if (!r->active || a >= r->target + r->len || r->target >= a + len) continue;
        if (!strcmp(r->owner, owner)) continue;
        // two inline hooks on one function entry chain fine; anything else conflicts
        if (kind == PATCH_HOOK && r->kind == PATCH_HOOK && r->target == a) {
            mvc_log("[Loader] note: %s and %s both hook %s (chained)", r->owner, owner, r->what);
            continue;
        }
        mvc_log_warn("[Loader] CONFLICT: %s patches %s, which %s already patched", owner, r->what, r->owner);
    }
}

// inline hooks
static int hook_impl(void *target, void *replacement, void **original, void *caller) {
    if (!target || !replacement) {
        mvc_log_error("[Loader] hook failed: null target or replacement");
        return -1;
    }
    pthread_mutex_lock(&g_mx);
    uint64_t pc = (uint64_t)target;
    uint32_t insn0 = *(uint32_t *)target;
    uintptr_t island = island_alloc(pc, 4 * 32);
    if (!island) {
        pthread_mutex_unlock(&g_mx);
        mvc_log_error("[Loader] hook failed at %p: no memory in branch range", target);
        return -1;
    }
    uint32_t code[32] = {0};
    emit_abs_jump(code, (uint64_t)replacement);
    uint32_t *tramp = code + 8;
    int ends;
    int n = relocate(tramp, insn0, pc, &ends);
    // relocated code computes addresses as if it lived in the island; fine for
    // everything we emit, because every form above is position-independent now
    if (n < 0) {
        pthread_mutex_unlock(&g_mx);
        mvc_log_error("[Loader] hook failed at %p: cannot relocate instruction %08x", target, insn0);
        return -1;
    }
    if (!ends) emit_abs_jump(tramp + n, pc + 4);
    if (island_write(island, code, 32)) {
        pthread_mutex_unlock(&g_mx);
        mvc_log_error("[Loader] hook failed at %p: island not writable", target);
        return -1;
    }
    char owner[64];
    owner_name(caller, owner, sizeof owner);
    if (!owner[0]) snprintf(owner, sizeof owner, "Loader");
    warn_overlap(pc, 4, owner, PATCH_HOOK);
    PatchRec *r = rec_new(pc, 4, PATCH_HOOK, caller);
    if (r) { r->island = island; r->island_end = island + 4 * 32; }

    int64_t off = (int64_t)(island - pc);
    uint32_t b = 0x14000000 | (uint32_t)((off >> 2) & 0x3FFFFFF);
    if (original) *original = (void *)(island + 8 * 4);
    int rc = write_raw(pc, &b, 4);
    pthread_mutex_unlock(&g_mx);
    if (rc) { mvc_log_error("[Loader] hook failed at %p: code not writable", target); return -1; }
    return 0;
}

int mvc_hook(void *target, void *replacement, void **original) {
    return hook_impl(target, replacement, original, __builtin_return_address(0));
}

int mvc_hook_sym(const char *name, void *replacement, void **original) {
    void *t = mvc_sym(name);
    if (!t) {
        mvc_log_error("[Loader] hook failed: symbol %s not found", name);
        return -1;
    }
    return hook_impl(t, replacement, original, __builtin_return_address(0));
}

int mvc_unhook(void *target) {
    pthread_mutex_lock(&g_mx);
    PatchRec *mine = NULL;
    for (int i = g_nrec - 1; i >= 0; i--)
        if (g_rec[i].active && g_rec[i].target == (uintptr_t)target && g_rec[i].kind != PATCH_WRITE) { mine = &g_rec[i]; break; }
    if (!mine) { pthread_mutex_unlock(&g_mx); return -1; }
    // only the most recent hook on a function can be removed safely
    uint32_t now = *(uint32_t *)target;
    int64_t off = (int64_t)(mine->island - (uintptr_t)target);
    uint32_t expect = 0x14000000 | (uint32_t)((off >> 2) & 0x3FFFFFF);
    if (now != expect) {
        pthread_mutex_unlock(&g_mx);
        mvc_log_warn("[Loader] cannot unhook %s: another hook was installed after it", mine->what);
        return -1;
    }
    int rc = write_raw((uintptr_t)target, mine->orig, 4);
    if (!rc) mine->active = 0;
    pthread_mutex_unlock(&g_mx);
    return rc;
}

// mid-function hooks
extern void mvc_midhook_stub(void); // midhook.S
typedef struct { mvc_midhook_fn fn; void *user; } MidInfo;

int mvc_midhook(void *at, mvc_midhook_fn fn, void *user) {
    if (!at || !fn) return -1;
    pthread_mutex_lock(&g_mx);
    uint64_t pc = (uint64_t)at;
    uint32_t insn = *(uint32_t *)at;
    MidInfo *info = malloc(sizeof *info);
    info->fn = fn;
    info->user = user;
    uintptr_t island = island_alloc(pc, 4 * 40);
    if (!island) { pthread_mutex_unlock(&g_mx); mvc_log_error("[Loader] midhook failed at %p: no memory", at); return -1; }

    uint32_t c[40] = {0};
    int k = 0;
    c[k++] = 0xD10203FF; // sub sp, sp, #128        (step over the red zone)
    c[k++] = 0xA9BF47F0; // stp x16, x17, [sp, #-16]!
    c[k++] = 0xA9BF7FFE; // stp x30, xzr, [sp, #-16]!
    int lit_info = k++;  // ldr x16, =info
    int lit_stub = k++;  // ldr x17, =stub
    c[k++] = 0xD63F0220; // blr x17
    c[k++] = 0xA8C17FFE; // ldp x30, xzr, [sp], #16
    c[k++] = 0xA8C147F0; // ldp x16, x17, [sp], #16
    c[k++] = 0x910203FF; // add sp, sp, #128
    int ends;
    int n = relocate(&c[k], insn, pc, &ends);
    if (n < 0) { pthread_mutex_unlock(&g_mx); mvc_log_error("[Loader] midhook failed at %p: cannot relocate %08x", at, insn); return -1; }
    k += n;
    if (!ends) {
        int64_t back = (int64_t)(pc + 4) - (int64_t)(island + (uintptr_t)k * 4);
        c[k] = 0x14000000 | (uint32_t)((back >> 2) & 0x3FFFFFF); // B back (no register clobbered)
        k++;
    }
    if (k & 1) k++; // align literals to 8 bytes
    uint64_t vinfo = (uint64_t)info, vstub = (uint64_t)mvc_midhook_stub;
    int li = k;
    memcpy(&c[k], &vinfo, 8); k += 2;
    memcpy(&c[k], &vstub, 8); k += 2;
    c[lit_info] = 0x58000000 | (uint32_t)(((li - lit_info)) << 5) | 16;
    c[lit_stub] = 0x58000000 | (uint32_t)(((li + 2 - lit_stub)) << 5) | 17;
    if (island_write(island, c, k)) { pthread_mutex_unlock(&g_mx); return -1; }

    char owner[64];
    owner_name(__builtin_return_address(0), owner, sizeof owner);
    if (!owner[0]) snprintf(owner, sizeof owner, "Loader");
    warn_overlap(pc, 4, owner, PATCH_MIDHOOK);
    PatchRec *r = rec_new(pc, 4, PATCH_MIDHOOK, __builtin_return_address(0));
    if (r) { r->island = island; r->island_end = island + (uintptr_t)k * 4; }
    int64_t off = (int64_t)(island - pc);
    uint32_t b = 0x14000000 | (uint32_t)((off >> 2) & 0x3FFFFFF);
    int rc = write_raw(pc, &b, 4);
    pthread_mutex_unlock(&g_mx);
    return rc;
}

// memory patches
static int patch_impl(void *addr, const void *bytes, size_t len, void *caller) {
    if (!addr || !bytes || !len || len > 64) {
        mvc_log_error("[Loader] patch rejected at %p (length %zu, max 64)", addr, len);
        return -1;
    }
    pthread_mutex_lock(&g_mx);
    char owner[64];
    owner_name(caller, owner, sizeof owner);
    if (!owner[0]) snprintf(owner, sizeof owner, "Loader");
    warn_overlap((uintptr_t)addr, (uint32_t)len, owner, PATCH_WRITE);
    PatchRec *r = rec_new((uintptr_t)addr, (uint32_t)len, PATCH_WRITE, caller);
    int rc = write_raw((uintptr_t)addr, bytes, len);
    if (rc && r) { r->active = 0; }
    int id = (!rc && r) ? r->id : -1;
    pthread_mutex_unlock(&g_mx);
    if (rc) mvc_log_error("[Loader] patch failed at %p", addr);
    return id;
}

int mvc_write(void *addr, const void *bytes, size_t len) {
    return patch_impl(addr, bytes, len, __builtin_return_address(0));
}

int mvc_nop(void *addr, int count) {
    uint32_t nops[16];
    if (count < 1 || count > 16) return -1;
    for (int i = 0; i < count; i++) nops[i] = 0xD503201F;
    return patch_impl(addr, nops, (size_t)count * 4, __builtin_return_address(0));
}

int mvc_patch_branch(void *at, void *dest, int link) {
    int64_t off = (int64_t)((uintptr_t)dest - (uintptr_t)at);
    if (off <= -0x8000000 || off >= 0x8000000) {
        mvc_log_error("[Loader] branch patch at %p: destination out of range", at);
        return -1;
    }
    uint32_t b = (link ? 0x94000000 : 0x14000000) | (uint32_t)((off >> 2) & 0x3FFFFFF);
    return patch_impl(at, &b, 4, __builtin_return_address(0));
}

int mvc_unpatch(int id) {
    pthread_mutex_lock(&g_mx);
    int rc = -1;
    for (int i = 0; i < g_nrec; i++) {
        PatchRec *r = &g_rec[i];
        if (r->id == id && r->active && r->kind == PATCH_WRITE) {
            rc = write_raw(r->target, r->orig, r->len);
            if (!rc) r->active = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_mx);
    return rc;
}

// pattern scan
void *mvc_find_pattern(const char *pat) {
    uint8_t bytes[256], mask[256];
    int n = 0;
    for (const char *p = pat; *p && n < 256;) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (p[0] == '?') {
            mask[n] = 0; bytes[n++] = 0;
            p += (p[1] == '?') ? 2 : 1;
            continue;
        }
        char hex[3] = {p[0], p[1], 0};
        char *end;
        long v = strtol(hex, &end, 16);
        if (end != hex + 2) { mvc_log_error("[Loader] bad pattern near \"%s\"", p); return NULL; }
        bytes[n] = (uint8_t)v; mask[n++] = 1;
        p += 2;
    }
    if (!n) return NULL;
    const struct mach_header_64 *mh = NULL;
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const struct mach_header_64 *h = (const void *)_dyld_get_image_header(i);
        if (h && h->filetype == MH_EXECUTE) { mh = h; break; }
    }
    if (!mh) return NULL;
    unsigned long size = 0;
    uint8_t *text = getsectiondata(mh, "__TEXT", "__text", &size);
    if (!text) return NULL;
    for (unsigned long i = 0; i + (unsigned long)n <= size; i++) {
        int ok = 1;
        for (int j = 0; j < n; j++)
            if (mask[j] && text[i + j] != bytes[j]) { ok = 0; break; }
        if (ok) return text + i;
    }
    return NULL;
}

// offset helpers
static int load_size_imm(uint32_t i, int *size, int *rn) {
    uint32_t top = i & 0xFFC00000;
    int s;
    if (top == 0x39400000) s = 1;
    else if (top == 0x79400000) s = 2;
    else if (top == 0xB9400000) s = 4;
    else if (top == 0xF9400000) s = 8;
    else return -1;
    *size = s;
    *rn = (i >> 5) & 31;
    return (int)((i >> 10) & 0xFFF) * s;
}

int mvc_first_load_imm(void *func, int max_insns, int size) {
    if (!func) return -1;
    const uint32_t *p = (const uint32_t *)func;
    for (int k = 0; k < max_insns; k++) {
        int s, rn, imm = load_size_imm(p[k], &s, &rn);
        if (imm >= 0 && s == size) return imm;
    }
    return -1;
}

int mvc_load_imm_from_base(void *func, int max_insns, void *base_addr, int size) {
    if (!func) return -1;
    const uint32_t *p = (const uint32_t *)func;
    uint64_t regs[32] = {0};
    for (int k = 0; k < max_insns; k++) {
        uint32_t i = p[k];
        uint64_t pc = (uint64_t)&p[k];
        if ((i & 0x9F000000) == 0x90000000) {
            int64_t imm = sext((((i >> 5) & 0x7FFFF) << 2) | ((i >> 29) & 3), 21);
            regs[i & 31] = (pc & ~0xFFFULL) + (imm << 12);
            continue;
        }
        if ((i & 0xFF800000) == 0x91000000) {
            int rd = i & 31, rn = (i >> 5) & 31;
            uint64_t imm = ((i >> 10) & 0xFFF) << (((i >> 22) & 1) * 12);
            regs[rd] = regs[rn] ? regs[rn] + imm : 0;
            continue;
        }
        int s, rn, imm = load_size_imm(i, &s, &rn);
        if (imm >= 0) {
            if (s == size && regs[rn] == (uint64_t)base_addr) return imm;
            regs[i & 31] = 0;
        }
    }
    return -1;
}

// hot reload
// takes back everything `owner` patched, newest first. a hook that is still
// the outermost one on its function is removed outright; one that another
// hook was chained on top of is turned into a pass-through, by making its
// island skip straight to the trampoline. islands are never freed, so
// nothing that still branches into them can crash.
int hooks_release(const char *owner) {
    pthread_mutex_lock(&g_mx);
    int n = 0;
    for (int i = g_nrec - 1; i >= 0; i--) {
        PatchRec *r = &g_rec[i];
        if (!r->active || strcmp(r->owner, owner)) continue;
        if (r->kind == PATCH_WRITE) {
            for (int j = i + 1; j < g_nrec; j++)
                if (g_rec[j].active && g_rec[j].target < r->target + r->len && r->target < g_rec[j].target + g_rec[j].len)
                    mvc_log_warn("[Loader] %s: undoing a patch on %s that %s patched again later", owner, r->what, g_rec[j].owner);
            if (write_raw(r->target, r->orig, r->len)) { mvc_log_error("[Loader] %s: could not undo the patch on %s", owner, r->what); continue; }
        } else {
            int64_t off = (int64_t)(r->island - r->target);
            uint32_t expect = 0x14000000 | (uint32_t)((off >> 2) & 0x3FFFFFF);
            if (*(uint32_t *)r->target == expect) {
                if (write_raw(r->target, r->orig, 4)) continue;
            } else {
                // hooks: [abs jump to replacement (8 words)][trampoline]; midhooks: [9-word call to the stub][relocated insn]
                uint32_t skip = r->kind == PATCH_HOOK ? 0x14000008 : 0x14000009;
                if (island_write(r->island, &skip, 1)) continue;
            }
        }
        r->active = 0;
        n++;
    }
    pthread_mutex_unlock(&g_mx);
    return n;
}
