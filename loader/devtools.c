// console dev tools: sym, addr, mem, poke, watch, perf.
//


// address expressions, no spaces, numbers are hex with or without 0x:
//   player+44C            the player's CPed + 0x44C (health)
//   vehicle+0x5A0         the car you're in
//   TheCamera+75          a game variable by name
//   CHud::Draw            a function by demangled name (or mangled, _ZN4CHud4DrawEv)
//   [RsGlobal+20]+10      [...] reads the pointer at that address
#include <ctype.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "internal.h"

static void out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void out(const char *fmt, ...) {
    char b[512];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    __builtin_va_end(ap);
    log_push(b, LOG_INFO);
}

// memory access
// reads through the kernel, so a bad address returns an error instead of crashing.
static int safe_read(uintptr_t a, void *buf, size_t n) {
    mach_vm_size_t got = 0;
    if (!a) return 0;
    return mach_vm_read_overwrite(mach_task_self(), a, n, (mach_vm_address_t)(uintptr_t)buf, &got) == KERN_SUCCESS && got == n;
}

static int region_writable(uintptr_t a, size_t n) {
    mach_vm_address_t addr = a;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj;
    if (mach_vm_region(mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS)
        return 0;
    return addr <= a && a + n <= addr + size && (info.protection & VM_PROT_WRITE);
}

// demangled names
static char *(*cxa_demangle)(const char *, char *, size_t *, int *);
static char **g_dem; // demangled name per game symbol (or NULL when not a C++ name)

static const char *sym_pretty(int i) {
    const GameSym *s;
    int n = game_symbols(&s);
    if (i < 0 || i >= n) return "";
    if (!g_dem) {
        g_dem = calloc((size_t)n, sizeof *g_dem);
        if (!cxa_demangle) *(void **)&cxa_demangle = dlsym(RTLD_DEFAULT, "__cxa_demangle");
        if (g_dem && cxa_demangle)
            for (int k = 0; k < n; k++) {
                int st = 0;
                if (s[k].name[0] == '_' && s[k].name[1] == 'Z') g_dem[k] = cxa_demangle(s[k].name, NULL, NULL, &st);
            }
    }
    return (g_dem && g_dem[i]) ? g_dem[i] : s[i].name;
}

static int sym_index(const GameSym *g) {
    const GameSym *s;
    game_symbols(&s);
    return g ? (int)(g - s) : -1;
}

// "CHud::Draw" or "CHud::Draw()" -> the symbol with that demangled name
static void *sym_by_pretty(const char *want) {
    const GameSym *s;
    int n = game_symbols(&s);
    size_t L = strlen(want);
    for (int i = 0; i < n; i++) {
        const char *p = sym_pretty(i);
        if (!strncmp(p, want, L) && (p[L] == 0 || p[L] == '(')) return (void *)s[i].addr;
    }
    return NULL;
}

static uintptr_t g_game_lo, g_game_hi;
static int in_game(uintptr_t a) {
    if (!g_game_hi) {
        const GameSym *s;
        int n = game_symbols(&s);
        if (n) { g_game_lo = s[0].addr; g_game_hi = s[n - 1].addr + 0x100000; }
    }
    return a >= g_game_lo && a < g_game_hi;
}

// "CHud::Draw()+0x10" for addresses in the game, "libfoo.dylib: sym+0x4" elsewhere, "" if unknown
static void describe(uintptr_t a, char *o, size_t n) {
    o[0] = 0;
    const PatchRec *isl = hook_find_island(a);
    if (isl) { snprintf(o, n, "hook island for %s (%s)", isl->what, isl->owner[0] ? isl->owner : "loader"); return; }
    if (in_game(a)) {
        const GameSym *g = game_sym_at(a);
        if (g && a - g->addr < 0x40000) {
            if (a == g->addr) snprintf(o, n, "%s", sym_pretty(sym_index(g)));
            else snprintf(o, n, "%s+0x%lx", sym_pretty(sym_index(g)), (unsigned long)(a - g->addr));
        }
        return;
    }
    Dl_info di;
    if (dladdr((void *)a, &di) && di.dli_fname) {
        const char *b = strrchr(di.dli_fname, '/');
        b = b ? b + 1 : di.dli_fname;
        if (di.dli_sname) snprintf(o, n, "%s: %s+0x%lx", b, di.dli_sname, (unsigned long)(a - (uintptr_t)di.dli_saddr));
        else snprintf(o, n, "%s+0x%lx", b, (unsigned long)(a - (uintptr_t)di.dli_fbase));
    }
}

// address expressions
static char g_err[128];

static int all_hex(const char *t) {
    if (t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) t += 2;
    if (!*t) return 0;
    for (; *t; t++) if (!isxdigit((unsigned char)*t)) return 0;
    return 1;
}

static int parse_expr(const char **pp, uintptr_t *v);

static int parse_term(const char **pp, uintptr_t *v) {
    const char *p = *pp;
    if (*p == '[') {
        p++;
        uintptr_t a;
        if (!parse_expr(&p, &a)) return 0;
        if (*p != ']') { snprintf(g_err, sizeof g_err, "missing ]"); return 0; }
        p++;
        if (!safe_read(a, v, sizeof *v)) { snprintf(g_err, sizeof g_err, "can't read memory at 0x%lx", (unsigned long)a); return 0; }
        *pp = p;
        return 1;
    }
    char tok[256];
    size_t n = 0;
    // a C++ name may contain "operator-", but a term never starts with + or -
    while (p[n] && p[n] != '+' && p[n] != ']' && !(p[n] == '-' && n) && n < sizeof tok - 1) { tok[n] = p[n]; n++; }
    tok[n] = 0;
    if (!n) { snprintf(g_err, sizeof g_err, "expected a value"); return 0; }
    *pp = p + n;
    if (all_hex(tok)) { *v = (uintptr_t)strtoull(tok, NULL, 16); return 1; }
    if (!strcasecmp(tok, "player") || !strcasecmp(tok, "ped")) {
        *v = (uintptr_t)mvc_player_ped();
        if (!*v) { snprintf(g_err, sizeof g_err, "no player right now"); return 0; }
        return 1;
    }
    if (!strcasecmp(tok, "vehicle") || !strcasecmp(tok, "car")) {
        *v = (uintptr_t)mvc_player_vehicle();
        if (!*v) { snprintf(g_err, sizeof g_err, "you're not in a vehicle"); return 0; }
        return 1;
    }
    void *s = mvc_sym(tok);
    if (!s) s = sym_by_pretty(tok);
    if (!s) { snprintf(g_err, sizeof g_err, "unknown name \"%s\" (try: sym %s)", tok, tok); return 0; }
    *v = (uintptr_t)s;
    return 1;
}

static int parse_expr(const char **pp, uintptr_t *v) {
    if (!parse_term(pp, v)) return 0;
    while (**pp == '+' || **pp == '-') {
        char op = *(*pp)++;
        uintptr_t r;
        if (!parse_term(pp, &r)) return 0;
        *v = op == '+' ? *v + r : *v - r;
    }
    return 1;
}

static int eval(const char *s, uintptr_t *v) {
    g_err[0] = 0;
    const char *p = s;
    if (!parse_expr(&p, v)) return 0;
    if (*p) { snprintf(g_err, sizeof g_err, "unexpected \"%s\"", p); return 0; }
    return 1;
}

// value types
enum { T_U8, T_I8, T_U16, T_I16, T_U32, T_I32, T_F32, T_F64, T_U64, T_PTR, T_VEC3, T_STR };
static const struct { const char *name; int size; } g_types[] = {
    {"u8", 1}, {"i8", 1}, {"u16", 2}, {"i16", 2}, {"u32", 4}, {"i32", 4}, {"f32", 4}, {"f64", 8},
    {"u64", 8}, {"ptr", 8}, {"vec3", 12}, {"str", 32}};
#define NTYPES (int)(sizeof g_types / sizeof *g_types)

static int type_from(const char *s) {
    if (!s) return -1;
    for (int i = 0; i < NTYPES; i++) if (!strcasecmp(g_types[i].name, s)) return i;
    if (!strcasecmp(s, "byte") || !strcasecmp(s, "bool")) return T_U8;
    if (!strcasecmp(s, "int")) return T_I32;
    if (!strcasecmp(s, "float") || !strcasecmp(s, "f")) return T_F32;
    if (!strcasecmp(s, "short")) return T_I16;
    if (!strcasecmp(s, "double")) return T_F64;
    if (!strcasecmp(s, "p")) return T_PTR;
    return -1;
}

static int fmt_value(uintptr_t a, int t, char *o, size_t n) {
    uint8_t b[32] = {0};
    if (!safe_read(a, b, (size_t)g_types[t].size)) { snprintf(o, n, "(unreadable)"); return 0; }
    switch (t) {
    case T_U8:  snprintf(o, n, "%u", b[0]); break;
    case T_I8:  snprintf(o, n, "%d", (int8_t)b[0]); break;
    case T_U16: { uint16_t x; memcpy(&x, b, 2); snprintf(o, n, "%u", x); break; }
    case T_I16: { int16_t x; memcpy(&x, b, 2); snprintf(o, n, "%d", x); break; }
    case T_U32: { uint32_t x; memcpy(&x, b, 4); snprintf(o, n, "%u (0x%x)", x, x); break; }
    case T_I32: { int32_t x; memcpy(&x, b, 4); snprintf(o, n, "%d", x); break; }
    case T_F32: { float x; memcpy(&x, b, 4); snprintf(o, n, "%.3f", x); break; }
    case T_F64: { double x; memcpy(&x, b, 8); snprintf(o, n, "%.4f", x); break; }
    case T_U64: { uint64_t x; memcpy(&x, b, 8); snprintf(o, n, "%llu", (unsigned long long)x); break; }
    case T_PTR: {
        uint64_t x; memcpy(&x, b, 8);
        char d[160];
        describe((uintptr_t)x, d, sizeof d);
        snprintf(o, n, d[0] ? "0x%llx  %s" : "0x%llx", (unsigned long long)x, d);
        break;
    }
    case T_VEC3: { float f[3]; memcpy(f, b, 12); snprintf(o, n, "%.2f, %.2f, %.2f", f[0], f[1], f[2]); break; }
    case T_STR: {
        int k = 0;
        char s[33];
        for (; k < 32 && b[k]; k++) s[k] = isprint(b[k]) ? (char)b[k] : '.';
        s[k] = 0;
        snprintf(o, n, "\"%s\"", s);
        break;
    }
    }
    return 1;
}

// sym / addr
static int ci_contains(const char *hay, const char *needle) {
    size_t L = strlen(needle);
    for (; *hay; hay++) if (!strncasecmp(hay, needle, L)) return 1;
    return 0;
}

static void cmd_sym(int argc, const char **argv) {
    if (argc < 2) {
        out("usage: sym <part of a name> [more words]   e.g. sym CHud::Draw, sym radar blip");
        out("       every word must appear in the name; searches demangled and mangled names");
        return;
    }
    const GameSym *s;
    int n = game_symbols(&s);
    if (n <= 0) { out("the game's symbol table is not readable"); return; }
    intptr_t slide = game_slide();
    int found = 0, shown = 0;
    const int MAXSHOW = 40;
    for (int i = 0; i < n; i++) {
        const char *pretty = sym_pretty(i);
        int ok = 1;
        for (int w = 1; w < argc && ok; w++) ok = ci_contains(pretty, argv[w]) || ci_contains(s[i].name, argv[w]);
        if (!ok) continue;
        found++;
        if (shown >= MAXSHOW) continue;
        shown++;
        if (pretty != s[i].name) out("  0x%09lx  %s   %s", (unsigned long)(s[i].addr - slide), pretty, s[i].name);
        else out("  0x%09lx  %s", (unsigned long)(s[i].addr - slide), pretty);
    }
    if (found > shown) out("%d matches, first %d shown - add another word to narrow it down", found, shown);
    else out("%d match%s (addresses as in Hopper/Ghidra; runtime = address + 0x%lx)", found, found == 1 ? "" : "es", (unsigned long)slide);
}

static void cmd_addr(int argc, const char **argv) {
    if (argc < 2) { out("usage: addr <address or expression>   e.g. addr 1000a2f30, addr player"); return; }
    uintptr_t a;
    if (!eval(argv[1], &a)) { out("%s", g_err); return; }
    char d[200];
    describe(a, d, sizeof d);
    if (in_game(a)) out("0x%lx (file 0x%lx) = %s", (unsigned long)a, (unsigned long)(a - game_slide()), d[0] ? d : "no symbol");
    else out("0x%lx = %s", (unsigned long)a, d[0] ? d : "not in the game or a loaded library (heap/stack?)");
}

// mem / poke
static void cmd_mem(int argc, const char **argv) {
    if (argc < 2) {
        out("usage: mem <address> [bytes] [x|f|i|p]   hex (default), floats, ints or pointers");
        out("       mem player 80, mem vehicle+0x5A0 40 f, mem [RsGlobal+20] 40 p");
        return;
    }
    uintptr_t a;
    if (!eval(argv[1], &a)) { out("%s", g_err); return; }
    int len = argc > 2 ? (int)strtol(argv[2], NULL, 16) : 0x40;
    if (len <= 0) len = 0x40;
    if (len > 0x200) len = 0x200;
    char mode = argc > 3 ? (char)tolower((unsigned char)argv[3][0]) : 'x';
    static uint8_t buf[0x200];
    if (!safe_read(a, buf, (size_t)len)) {
        // the end of the range may be unmapped; show what can be read
        while (len > 16 && !safe_read(a, buf, (size_t)len)) len -= 16;
        if (!safe_read(a, buf, (size_t)len)) { out("can't read memory at 0x%lx", (unsigned long)a); return; }
    }
    char d[160];
    describe(a, d, sizeof d);
    out("0x%lx%s%s, 0x%x bytes (offsets and lengths are hex)", (unsigned long)a, d[0] ? " = " : "", d, len);
    for (int off = 0; off < len;) {
        char line[200];
        int p = snprintf(line, sizeof line, "  +%03x ", off);
        if (mode == 'f' || mode == 'i') {
            for (int k = 0; k < 4 && off + 4 <= len; k++, off += 4) {
                if (mode == 'f') { float f; memcpy(&f, buf + off, 4); p += snprintf(line + p, sizeof line - p, " %12.3f", f); }
                else { int32_t x; memcpy(&x, buf + off, 4); p += snprintf(line + p, sizeof line - p, " %11d", x); }
            }
            if (off + 4 > len) off = len;
        } else if (mode == 'p') {
            if (off + 8 > len) break;
            uint64_t x; memcpy(&x, buf + off, 8);
            describe((uintptr_t)x, d, sizeof d);
            snprintf(line + p, sizeof line - p, " 0x%016llx  %s", (unsigned long long)x, d);
            off += 8;
        } else {
            char asc[17];
            int k = 0;
            for (; k < 16 && off + k < len; k++) {
                p += snprintf(line + p, sizeof line - p, "%s%02x", k == 8 ? "  " : " ", buf[off + k]);
                asc[k] = isprint(buf[off + k]) ? (char)buf[off + k] : '.';
            }
            asc[k] = 0;
            for (; k < 16; k++) p += snprintf(line + p, sizeof line - p, k == 8 ? "    " : "   ");
            snprintf(line + p, sizeof line - p, "  %s", asc);
            off += 16;
        }
        out("%s", line);
    }
}

static void cmd_poke(int argc, const char **argv) {
    if (argc < 4) { out("usage: poke <address> <type> <value>   types: u8 i8 u16 i16 u32 i32 f32 f64 u64"); return; }
    uintptr_t a;
    if (!eval(argv[1], &a)) { out("%s", g_err); return; }
    int t = type_from(argv[2]);
    if (t < 0 || t >= T_PTR) { out("type must be one of u8 i8 u16 i16 u32 i32 f32 f64 u64"); return; }
    uint8_t b[8];
    int sz = g_types[t].size;
    if (t == T_F32) { float f = (float)atof(argv[3]); memcpy(b, &f, 4); }
    else if (t == T_F64) { double f = atof(argv[3]); memcpy(b, &f, 8); }
    else { long long x = strtoll(argv[3], NULL, 0); memcpy(b, &x, (size_t)sz); }
    char before[96], after[96];
    if (!fmt_value(a, t, before, sizeof before)) { out("can't read memory at 0x%lx", (unsigned long)a); return; }
    if (region_writable(a, (size_t)sz)) memcpy((void *)a, b, (size_t)sz);
    else {
        int id = mvc_write((void *)a, b, (size_t)sz); // read-only (game code): goes through the patch registry
        if (id < 0) { out("write failed"); return; }
        out("0x%lx is read-only (code) - written as patch #%d, listed in `hooks`", (unsigned long)a, id);
    }
    fmt_value(a, t, after, sizeof after);
    out("0x%lx: %s -> %s", (unsigned long)a, before, after);
}

// watch overlay
#define MAXWATCH 12
static struct { char expr[96]; int type; } g_watch[MAXWATCH];
static int g_nwatch;

static void cmd_watch(int argc, const char **argv) {
    if (argc < 2) {
        if (!g_nwatch) {
            out("usage: watch <address> [type]   shows a value on screen, updated every frame");
            out("       types: u8 i8 u16 i16 u32 i32 (default) f32 f64 u64 ptr vec3 str");
            out("       watch pos, watch player+44C f32, watch vehicle+0x5A0 f32");
            return;
        }
        for (int i = 0; i < g_nwatch; i++) out("  %d: %s %s", i + 1, g_watch[i].expr, g_types[g_watch[i].type].name);
        out("unwatch <number> or unwatch all");
        return;
    }
    int t = argc > 2 ? type_from(argv[2]) : T_I32;
    if (t < 0) { out("unknown type \"%s\"", argv[2]); return; }
    if (!strcasecmp(argv[1], "pos")) t = T_VEC3;
    else {
        uintptr_t a;
        if (!eval(argv[1], &a)) out("note: %s (the watch will show it once it resolves)", g_err);
    }
    if (g_nwatch >= MAXWATCH) { out("at most %d watches - unwatch one first", MAXWATCH); return; }
    snprintf(g_watch[g_nwatch].expr, sizeof g_watch[0].expr, "%s", argv[1]);
    g_watch[g_nwatch].type = t;
    g_nwatch++;
    out("watching %s as %s", argv[1], g_types[t].name);
}

static void cmd_unwatch(int argc, const char **argv) {
    if (argc < 2 || !strcasecmp(argv[1], "all")) { g_nwatch = 0; out("watches cleared"); return; }
    int i = atoi(argv[1]) - 1;
    if (i < 0 || i >= g_nwatch) { out("no watch %s - type watch to list them", argv[1]); return; }
    memmove(&g_watch[i], &g_watch[i + 1], sizeof g_watch[0] * (size_t)(g_nwatch - i - 1));
    g_nwatch--;
}

// MVC_EXEC="cmd1;cmd2" runs console commands ~2 seconds after the first frame
// (for scripted tests that only launch the game and read the log)
static void run_env_exec(void) {
    static int frames;
    if (frames > 120 || ++frames != 120) return;
    const char *e = getenv("MVC_EXEC");
    if (!e || !*e) return;
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", e);
    char *save = NULL;
    for (char *c = strtok_r(buf, ";", &save); c; c = strtok_r(NULL, ";", &save)) {
        while (*c == ' ') c++;
        if (*c) console_exec(c);
    }
}

void devtools_draw(void) {
    run_env_exec();
    if (!g_nwatch) return;
    float W = mvc_screen_w(), H = mvc_screen_h();
    float u = H / 448.f * g_cfg.console_scale;
    float sx = 0.30f * u, sy = 0.62f * u, lh = 11.f * u, pad = 6 * u;
    float y = H * 0.32f;
    mvc_flush_text();
    for (int i = 0; i < g_nwatch; i++) {
        char v[200], line[260];
        if (!strcasecmp(g_watch[i].expr, "pos")) {
            MVCVec3 p = mvc_player_pos();
            snprintf(v, sizeof v, "%.2f, %.2f, %.2f", p.x, p.y, p.z);
        } else {
            uintptr_t a;
            if (eval(g_watch[i].expr, &a)) fmt_value(a, g_watch[i].type, v, sizeof v);
            else snprintf(v, sizeof v, "-");
        }
        snprintf(line, sizeof line, "%s  %s", g_watch[i].expr, v);
        mvc_text(W - pad, y, line, sx, sy, (MVCColor){255, 150, 225, 255}, MVC_ALIGN_RIGHT, 1, 1);
        y += lh;
    }
    mvc_flush_text();
}

// reVC debug menu
// macVC ships reVC's debug menu, fully wired up: CGame::Process calls
// DebugMenuProcess every frame and Ctrl+M opens it. its on/off flag is a static
// int; DebugMenuProcess flips it with "ldr w8,[xN,#off]; cmp w8,#0; cset w8,eq;
// str w8,[xN,#off]", and the page comes from the ADRP before that.
static int *debug_menu_flag(void) {
    static int *flag, looked;
    if (looked) return flag;
    looked = 1;
    const uint32_t *p = mvc_sym("_Z16DebugMenuProcessv");
    uint64_t page[32] = {0};
    for (int i = 0; p && i < 64; i++) {
        uint32_t in = p[i];
        if ((in & 0x9F000000) == 0x90000000) { // ADRP
            int64_t imm = (int64_t)((uint64_t)((((in >> 5) & 0x7FFFF) << 2) | ((in >> 29) & 3)) << 43) >> 31; // sign-extend, << 12
            page[in & 31] = ((uint64_t)&p[i] & ~0xFFFULL) + (uint64_t)imm;
            continue;
        }
        // LDR Wt, [Xn, #imm] ; CMP Wt, #0 ; CSET Wt, EQ ; STR Wt, [Xn, #imm]
        if ((in & 0xFFC00000) == 0xB9400000 && i + 3 < 64 && (p[i + 1] & 0xFFFFFC1F) == 0x7100001F &&
            (p[i + 2] & 0xFFFFFFE0) == 0x1A9F17E0 && (p[i + 3] & 0xFFC00000) == 0xB9000000 &&
            ((p[i + 3] >> 5) & 31) == ((in >> 5) & 31) && ((p[i + 3] >> 10) & 0xFFF) == ((in >> 10) & 0xFFF)) {
            uint64_t base = page[(in >> 5) & 31];
            if (base) flag = (int *)(base + ((in >> 10) & 0xFFF) * 4);
            break;
        }
    }
    return flag;
}

static void cmd_debugmenu(int argc, const char **argv) {
    (void)argc; (void)argv;
    int *f = debug_menu_flag();
    if (!f) { out("reVC's debug menu wasn't found in this macVC build"); return; }
    *f = !*f; // DebugMenuProcess sets it up on the next frame
    out("debug menu %s (Ctrl+M also toggles it; arrows/Enter or the mouse drive it)", *f ? "on" : "off");
}

// perf
static void cmd_perf(int argc, const char **argv) {
    if (argc > 1 && !strcasecmp(argv[1], "reset")) { perf_reset(); out("perf counters reset"); return; }
    perf_report(argc > 1 && !strcasecmp(argv[1], "all"));
}

void devtools_register(void) {
    mvc_console_command("sym", "sym <words> - search the game's function and variable names", cmd_sym);
    mvc_console_command("addr", "addr <address> - what's at an address", cmd_addr);
    mvc_console_command("mem", "mem <address> [bytes] [x|f|i|p] - memory viewer", cmd_mem);
    mvc_console_command("poke", "poke <address> <type> <value> - write memory", cmd_poke);
    mvc_console_command("watch", "watch <address> [type] - show a value on screen", cmd_watch);
    mvc_console_command("unwatch", "unwatch <n|all> - remove watches", cmd_unwatch);
    mvc_console_command("perf", "perf [all|reset] - time spent in each plugin", cmd_perf);
    mvc_console_command("debugmenu", "toggle reVC's built-in debug menu (also Ctrl+M)", cmd_debugmenu);
}
