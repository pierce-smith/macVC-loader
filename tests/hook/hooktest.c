// tests for the hook engine (loader/hook.c, midhook.S). every instruction form the
// relocator rewrites gets hooked, and calling through the trampoline has to give the same
// result as the unhooked function. run with tests/run.sh
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../../include/macvc.h"

int hooks_release(const char *owner);
void hooks_set_startup(int in_startup);
void *mvc_sym(const char *name) { (void)name; return NULL; } // the game isn't here

extern uint64_t thunk(void *fn, uint64_t arg);
extern char g_data;
#define F(n) extern char n;
F(t_plain) F(t_adrp) F(t_adr) F(t_b) F(t_bl) F(t_bcond) F(t_cbz) F(t_cbnz_w) F(t_tbz) F(t_tbnz_hi)
F(t_ldr_x) F(t_ldr_w) F(t_ldrsw) F(t_ldr_s) F(t_ldr_d) F(t_ldr_q)
F(t_mid) F(t_mid_at) F(t_mid_flags) F(t_mid_flags_at) F(t_mid_cbz) F(t_mid_cbz_at)
F(t_mid_regs) F(t_mid_regs_at) F(t_patch) F(t_marker)

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// one replacement per test that counts calls and forwards to the trampoline.
static void *g_orig;
static int g_calls;
static uint64_t repl(uint64_t a) { g_calls++; return thunk(g_orig, a); }

static void test_form(const char *name, void *fn, const uint64_t *args, int nargs) {
    uint64_t want[8];
    for (int i = 0; i < nargs; i++) want[i] = thunk(fn, args[i]);
    g_calls = 0;
    int rc = mvc_hook(fn, (void *)repl, &g_orig);
    CHECK(rc == 0, "%s: hook failed", name);
    if (rc) return;
    for (int i = 0; i < nargs; i++) {
        uint64_t got = thunk(fn, args[i]);
        CHECK(got == want[i], "%s(%#llx): got %#llx want %#llx", name, args[i], got, want[i]);
    }
    CHECK(g_calls == nargs, "%s: replacement ran %d times, want %d", name, g_calls, nargs);
    CHECK(mvc_unhook(fn) == 0, "%s: unhook failed", name);
    g_calls = 0;
    for (int i = 0; i < nargs; i++) CHECK(thunk(fn, args[i]) == want[i], "%s: wrong after unhook", name);
    CHECK(g_calls == 0, "%s: replacement still runs after unhook", name);
}

// chaining: two hooks on one function
static void *g_o1, *g_o2;
static uint64_t r1(uint64_t a) { return thunk(g_o1, a) * 10; }
static uint64_t r2(uint64_t a) { return thunk(g_o2, a) + 1000; }

// midhooks
static void mid_add100(MVCContext *c, void *u) { c->x[0] += 100; (*(int *)u)++; }
static void mid_clobber(MVCContext *c, void *u) {
    (void)c; (void)u;
    // a C handler is free to use caller-saved registers; the stub must restore them
    volatile double d = 3.0; (void)d;
}
static void mid_zero_x1(MVCContext *c, void *u) { (void)u; c->x[1] = 0; }
static void mid_check_sp(MVCContext *c, void *u) { *(uint64_t *)u = c->sp; }

int main(void) {
    hooks_set_startup(1);
    const uint64_t a1[] = {0, 1, 41};
    test_form("plain", &t_plain, a1, 3);
    test_form("adrp", &t_adrp, a1, 1);
    CHECK(thunk(&t_adrp, 0) == (uint64_t)&g_data, "adrp: wrong address");
    test_form("adr", &t_adr, a1, 1);
    test_form("b", &t_b, a1, 1);
    test_form("bl", &t_bl, a1, 3);
    const uint64_t a2[] = {0, 9, 10, 11, 1000};
    test_form("b.cond", &t_bcond, a2, 5);
    const uint64_t a3[] = {0, 1, 0x100000000ULL};
    test_form("cbz", &t_cbz, a3, 3);
    test_form("cbnz w", &t_cbnz_w, a3, 3);
    const uint64_t a4[] = {0, 8, 7, 0x10000000000ULL, 0x10000000008ULL};
    test_form("tbz", &t_tbz, a4, 5);
    test_form("tbnz hi", &t_tbnz_hi, a4, 5);
    test_form("ldr x", &t_ldr_x, a1, 1);
    CHECK(thunk(&t_ldr_x, 0) == 0x1122334455667788ULL, "ldr x value");
    test_form("ldr w", &t_ldr_w, a1, 1);
    CHECK(thunk(&t_ldr_w, 0) == 0xFFFFFFF0ULL, "ldr w zero-extends");
    test_form("ldrsw", &t_ldrsw, a1, 1);
    CHECK(thunk(&t_ldrsw, 0) == 0xFFFFFFFFFFFFFFF0ULL, "ldrsw sign-extends");
    test_form("ldr s", &t_ldr_s, a1, 1);
    test_form("ldr d", &t_ldr_d, a1, 1);
    test_form("ldr q", &t_ldr_q, a1, 1);
    CHECK(thunk(&t_ldr_q, 0) == 0x1122334455667788ULL, "ldr q high half");

    // chaining and unhook order
    CHECK(mvc_hook(&t_plain, (void *)r1, &g_o1) == 0, "chain hook 1");
    CHECK(mvc_hook(&t_plain, (void *)r2, &g_o2) == 0, "chain hook 2");
    CHECK(thunk(&t_plain, 4) == 1050, "chain: got %llu want 1050", thunk(&t_plain, 4));
    CHECK(mvc_unhook(&t_plain) == 0, "unhook the outer hook");
    CHECK(thunk(&t_plain, 4) == 50, "after removing outer: got %llu", thunk(&t_plain, 4));
    CHECK(mvc_unhook(&t_plain) == 0, "unhook the inner hook");
    CHECK(thunk(&t_plain, 4) == 5, "fully unhooked");
    CHECK(mvc_unhook(&t_plain) != 0, "unhook with nothing hooked must fail");

    // midhook changes a register; runs once per call
    int count = 0;
    CHECK(mvc_midhook(&t_mid_at, mid_add100, &count) == 0, "midhook");
    CHECK(thunk(&t_mid, 1) == 104, "midhook result %llu", thunk(&t_mid, 1));
    CHECK(count == 1, "midhook ran %d times", count);
    // flags survive the stub
    CHECK(mvc_midhook(&t_mid_flags_at, mid_clobber, NULL) == 0, "midhook flags");
    CHECK(thunk(&t_mid_flags, 3) == 200 && thunk(&t_mid_flags, 30) == 100, "flags not preserved");
    // midhook on a relocated CBZ sees the handler's register change
    CHECK(thunk(&t_mid_cbz, 5) == 1, "cbz before");
    CHECK(mvc_midhook(&t_mid_cbz_at, mid_zero_x1, NULL) == 0, "midhook cbz");
    CHECK(thunk(&t_mid_cbz, 5) == 2, "midhook on cbz: handler change not seen");
    // callee-saved and NEON registers survive
    CHECK(mvc_midhook(&t_mid_regs_at, mid_clobber, NULL) == 0, "midhook regs");
    uint64_t regs[12] = {0};
    thunk(&t_mid_regs, (uint64_t)regs);
    for (int i = 0; i < 10; i++) CHECK(regs[i] == (uint64_t)(19 + i), "x%d = %llu", 19 + i, regs[i]);
    double d8, d9;
    memcpy(&d8, &regs[10], 8); memcpy(&d9, &regs[11], 8);
    CHECK(d8 == 1.0 && d9 == 2.0, "d8/d9 = %f %f", d8, d9);
    // ctx->sp is the hooked code's sp
    uint64_t seen_sp = 0;
    CHECK(mvc_midhook(&t_mid, mid_check_sp, &seen_sp) == 0, "midhook sp");
    thunk(&t_mid, 0);
    CHECK(seen_sp != 0 && (seen_sp & 15) == 0, "ctx->sp %#llx", seen_sp);

    // patches
    uint32_t mov42 = 0xD2800540; // mov x0, #42
    int id = mvc_write(&t_patch, &mov42, 4);
    CHECK(id > 0 && thunk(&t_patch, 0) == 42, "mvc_write");
    CHECK(mvc_unpatch(id) == 0 && thunk(&t_patch, 0) == 5, "mvc_unpatch");
    id = mvc_nop(&t_patch, 1);
    CHECK(id > 0 && thunk(&t_patch, 77) == 77, "mvc_nop");
    mvc_unpatch(id);
    id = mvc_patch_branch(&t_patch, &t_b, 0);
    CHECK(id > 0 && thunk(&t_patch, 0) == 2, "mvc_patch_branch");
    mvc_unpatch(id);
    CHECK(thunk(&t_patch, 0) == 5, "branch unpatched");
    CHECK(mvc_write(&t_patch, &mov42, 65) < 0, "oversized patch must be rejected");

    // pattern scan over this executable's __text
    CHECK(mvc_find_pattern("E0 DD 97 D2 C0 1B B8 F2") == &t_marker, "find_pattern exact");
    CHECK(mvc_find_pattern("E0 ?? 97 D2 ?? 1B B8 F2") == &t_marker, "find_pattern wildcards");
    CHECK(mvc_find_pattern("E0 DD 97 D2 C0 1B B8 F3 F3 F3") == NULL, "find_pattern miss");
    CHECK(mvc_find_pattern("ZZ") == NULL, "find_pattern bad input");

    // offset helpers
    static const uint32_t fn[] = {0xF9400421 /* ldr x1,[x1,#8] */, 0xB9401802 /* ldr w2,[x0,#24] */, 0x39402003 /* ldrb w3,[x0,#8] */};
    CHECK(mvc_first_load_imm((void *)fn, 3, 8) == 8, "first_load_imm x");
    CHECK(mvc_first_load_imm((void *)fn, 3, 4) == 24, "first_load_imm w");
    CHECK(mvc_first_load_imm((void *)fn, 3, 1) == 8, "first_load_imm b");
    CHECK(mvc_first_load_imm((void *)fn, 3, 2) == -1, "first_load_imm miss");

    // hot reload: hooks_release takes back everything this "plugin" did
    mvc_hook(&t_plain, (void *)r1, &g_o1);
    int pid = mvc_write(&t_patch, &mov42, 4);
    (void)pid;
    int released = hooks_release(getprogname()); // the owner of code in this executable
    CHECK(released >= 2, "hooks_release released %d", released);
    CHECK(thunk(&t_plain, 4) == 5 && thunk(&t_patch, 0) == 5, "hooks_release left code patched");
    CHECK(thunk(&t_mid, 1) == 4, "midhooks released: got %llu", thunk(&t_mid, 1));

    // hooks made after startup get their own pages
    hooks_set_startup(0);
    test_form("plain (runtime island)", &t_plain, a1, 3);
    // every runtime hook takes a page within branch range: make sure hundreds fit
    int ok = 0;
    for (int i = 0; i < 300; i++) if (!mvc_hook(&t_plain, (void *)repl, &g_orig) && !mvc_unhook(&t_plain)) ok++;
#if defined(__has_feature) && __has_feature(address_sanitizer)
    CHECK(ok >= 100, "only %d of 300 runtime hooks found island space", ok); // ASan reserves most of the nearby space
#else
    CHECK(ok == 300, "only %d of 300 runtime hooks found island space", ok);
#endif

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail != 0;
}
