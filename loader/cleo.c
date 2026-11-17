// cleo runner. runs windows .cs scripts on the game's own script interpreter, the one
// that already runs main.scm.
//


// scripts come from GameData/CLEO/*.cs and the CLEO folders of modloader mods.
// each one gets its own CRunningScript linked into the game's active list. the code gets
// copied somewhere less than 4 GB above CTheScripts::ScriptSpace, because the interpreter
// addresses code as a 32-bit offset from ScriptSpace.
//


// one hook on CRunningScript::ProcessOneCommand handles two things, cleo scripts only:
// the jump opcodes (cleo labels are negative offsets from the start of the file, the game
// would read them as mission offsets) and the cleo opcodes from 0x05DC up. everything else
// goes to the game's handler untouched.
//


// cleo scripts get pulled out of the list while the game writes a save so they never end
// up in one, and they restart from the top on a new game or a load, like windows.
//


// memory: scripts use gta-vc.exe 1.0 addresses. cleo_addrs.h maps game variables to
// macVC's symbols by name. peds, cars and objects are handed out as 32-bit stand-ins and
// known struct fields get mapped to macVC's layout. anything else (x86 patches, unknown
// fields) gets logged once and skipped, never written.
#include "internal.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// game layout
// CRunningScript in this macVC build (checked against the game's code in cleo_init)
#define RS_SIZE 0x90
#define RS_IP 0x18
#define RS_STACK 0x1C
#define RS_SP 0x34
#define RS_LOCALS 0x38
#define RS_ACTIVE 0x80
#define RS_COND 0x81
#define RS_NOT 0x8A
#define NLOCALS 18          // 0@..15@ plus the timers 16@ and 17@
#define STACK_DEPTH 6
#define MAX_SCRIPTS_GAME 128
#define SIZE_MAIN 0x370E8   // main.scm part of ScriptSpace
#define SIZE_SPACE 0x3F9A0  // main + mission

static uint8_t *SS;                 // CTheScripts::ScriptSpace
static uint8_t *ScriptsArray;       // CTheScripts::ScriptsArray[128]
static uint8_t **pActive;           // &CTheScripts::pActiveScripts
static int32_t *Params;             // ScriptParams[]
static uint16_t *CommandsExecuted;
static char *CheatString;           // CPad::KeyBoardCheatString, newest key first
static int16_t *OldWeather;

static int8_t (*o_ProcessOneCommand)(uint8_t *);
static void (*o_TheScriptsInit)(void);
static void (*o_TheScriptsProcess)(void);
static void (*o_SaveAllScripts)(uint8_t *, uint32_t *);
static void (*CollectParameters)(uint8_t *, uint32_t *, int16_t);
static void (*StoreParameters)(uint8_t *, uint32_t *, int16_t);
static int32_t *(*GetPointerToScriptVariable)(uint8_t *, uint32_t *, int16_t);
static void (*UpdateCompareFlag)(uint8_t *, _Bool);
static void (*AddScriptToList)(uint8_t *, uint8_t **);
static void (*RemoveScriptFromList)(uint8_t *, uint8_t **);
static void *(*PoolsGetPed)(int), *(*PoolsGetVehicle)(int), *(*PoolsGetObject)(int);
static int (*PoolsGetPedRef)(void *), (*PoolsGetVehicleRef)(void *), (*PoolsGetObjectRef)(void *);
static uint8_t *(*GetPad)(int);
static void (*SetHelpMessage)(uint16_t *, _Bool, _Bool);
static void (*AddMessage)(uint16_t *, uint32_t, uint16_t);
static void (*AddMessageJumpQ)(uint16_t *, uint32_t, uint16_t);
static void (*AddBigMessage)(uint16_t *, uint32_t, uint16_t);
static uint8_t **PedPool, **VehPool, **ObjPool;  // CPools::ms_p*Pool: entries +0, flags +8, size +0x10
static uint8_t *TheCamera;
static int g_widescreen_off = -1;
static uint8_t *(*GetWeaponInfo)(int);
static void (*VehicleCheat)(int);

// scripts
#define MAX_CLEO 128
#define MAX_CALLS 16
typedef struct {
    uint32_t ret_ip;
    int32_t locals[NLOCALS];
} Frame;

typedef struct Cleo {
    uint8_t rs[0xC0];        // the game's CRunningScript; room for locals up to 33@ (see README)
    uint32_t base, size;     // code: SS[base .. base+size)
    char file[96];           // "SEDrunk.cs"
    char path[PATH_MAX];
    char mod[64];            // Mod Loader mod it came from, "" for GameData/CLEO
    int in_list, dead, failed;
    int ncalls;
    Frame calls[MAX_CALLS];
    int find_at[4];          // GET_RANDOM_*_IN_SPHERE_NO_SAVE_RECURSIVE: where the next search starts
    uint32_t last_op_ip;     // for error messages
} Cleo;

static Cleo *g_cleo[MAX_CLEO];
static int g_ncleo;
static int g_ready, g_restart, g_enabled = 1;
static char g_cleo_dir[PATH_MAX];

typedef struct { char path[PATH_MAX]; char mod[64]; } ModScript;
static ModScript g_modscripts[64];
static int g_nmodscripts;

// memory within 4 GB above ScriptSpace: script records, code, label data, ALLOCATE_MEMORY
#define REGION_SIZE (64u << 20)
static uint8_t *R;
static size_t R_used;

static uint8_t *ralloc(size_t n) {
    size_t at = (R_used + 15) & ~(size_t)15;
    if (!R || at + n > REGION_SIZE) return NULL;
    R_used = at + n;
    memset(R + at, 0, n);
    return R + at;
}

// temporary copies of string literals passed as arguments (reused, never grows)
#define SCRATCH_SIZE (64u << 10)
static uint8_t *g_scratch;
static size_t g_scratch_at;
static uint8_t *scratch(size_t n) {
    if (!g_scratch || n > 1024) return NULL;
    if (g_scratch_at + n > SCRATCH_SIZE) g_scratch_at = 0;
    uint8_t *p = g_scratch + g_scratch_at;
    g_scratch_at += (n + 15) & ~(size_t)15;
    return p;
}

static int alloc_region(void) {
    uintptr_t ss = (uintptr_t)SS;
    for (uintptr_t off = 0x10000000; off < 0xE0000000; off += 0x10000000) {
        void *want = (void *)((ss + off) & ~(uintptr_t)0xFFFF);
        void *p = mmap(want, REGION_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p == MAP_FAILED) continue;
        uintptr_t a = (uintptr_t)p;
        if (a > ss && a + REGION_SIZE - ss < 0xC0000000u) { R = p; return 0; }
        munmap(p, REGION_SIZE);
    }
    return -1;
}

static inline uint32_t *IP(Cleo *c) { return (uint32_t *)(c->rs + RS_IP); }
static inline int32_t *LOC(Cleo *c) { return (int32_t *)(c->rs + RS_LOCALS); }
static inline int is_cleo(uint8_t *s) {
    if (s >= ScriptsArray && s < ScriptsArray + MAX_SCRIPTS_GAME * RS_SIZE) return 0;
    for (int i = 0; i < g_ncleo; i++) if ((uint8_t *)g_cleo[i] == s) return 1;
    return 0;
}

// logging, once
static uint64_t g_said[512];
static int g_nsaid;
static int once(uint64_t key) {
    for (int i = 0; i < g_nsaid; i++) if (g_said[i] == key) return 0;
    if (g_nsaid < 512) g_said[g_nsaid++] = key;
    return g_nsaid < 512;
}
static uint64_t skey(Cleo *c, uint32_t what) { return ((uint64_t)(uintptr_t)c << 20) ^ what; }

// address translation
typedef struct { uint32_t win; uint32_t size; const char *sym; const char *name; char kind; } WinVar;
static const WinVar g_vars[] = {
#include "cleo_addrs.h"
};
#define NVARS ((int)(sizeof g_vars / sizeof g_vars[0]))
static uint8_t *g_var_real[NVARS];

#define WIN_TEXT_END 0x67D000   // gta-vc.exe 1.0: code below this, data above
#define WIN_IMAGE_END 0xC00000
#define SLOT_BASE 0xC0000000u
#define NSLOTS 0x4000
enum { K_RAW, K_PED, K_VEH, K_OBJ, K_PINFO, K_SCRIPT };
static const char *const KIND[] = {"memory", "CPed", "CVehicle", "CObject", "CPlayerInfo", "CRunningScript"};
typedef struct { uint8_t *p; int kind; } Slot;
static Slot g_slots[NSLOTS];
static int g_nslots;

static uint32_t to_script(void *p, int kind) {
    if (!p) return 0;
    uintptr_t a = (uintptr_t)p;
    if (kind == K_RAW && a >= (uintptr_t)SS && a - (uintptr_t)SS < SLOT_BASE &&
        (a - (uintptr_t)SS < 0x400000 || a - (uintptr_t)SS >= WIN_IMAGE_END))
        return (uint32_t)(a - (uintptr_t)SS);
    for (int i = 0; i < g_nslots; i++)
        if (g_slots[i].p == p && g_slots[i].kind == kind) return SLOT_BASE + ((uint32_t)i << 16);
    if (g_nslots >= NSLOTS) return 0;
    g_slots[g_nslots] = (Slot){p, kind};
    return SLOT_BASE + ((uint32_t)g_nslots++ << 16);
}

// Windows field offset -> macVC field offset, for the struct kinds above.
// cleo_fields.h only holds fields checked against this build's code.
typedef struct { char kind; uint32_t win, wsize, mac; char ptr; const char *name; } WinField;
static const WinField g_fields[] = {
#include "cleo_fields.h"
};
static const char KIND_CH[] = {0, 'P', 'V', 'O', 'I', 'S'};
static int kind_of(char k) { return k == 'P' ? K_PED : k == 'V' ? K_VEH : k == 'O' ? K_OBJ : k == 'S' ? K_SCRIPT : K_RAW; }

// *ptr_kind is set when the access is one whole pointer of a pointer field
static int field(int kind, uint32_t off, uint32_t size, int *ptr_kind) {
    *ptr_kind = -1;
    if (kind == K_SCRIPT) {
        // CRunningScript: next/prev pointers (4 bytes on Windows, 8 here), then the same fields
        // (name, IP, stack, locals, flags) 8 bytes further on (checked in cleo_init)
        if (off < 8) { if (off % 4 || size != 4) return -1; *ptr_kind = K_SCRIPT; return (int)off * 2; }
        return off + size <= 0x86 ? (int)off + 8 : -1;
    }
    int pos = sdk_offset(SDK_OFF_POS);
    // CPlaceable's matrix: Windows has a 4-byte vtable pointer before it, macVC an 8-byte one;
    // the matrix itself (right/up/at/pos as float rows) has the same layout
    if (kind != K_PINFO && pos == 0x38 && off >= 0x04 && off + size <= 0x44) return (int)off + 4;
    if (kind != K_PINFO && off == 0x5C && size <= 2 && sdk_offset(SDK_OFF_MODEL) >= 0) return sdk_offset(SDK_OFF_MODEL);
    for (size_t i = 0; i < sizeof g_fields / sizeof g_fields[0]; i++) {
        const WinField *f = &g_fields[i];
        if ((f->kind != KIND_CH[kind] && (f->kind != 'E' || kind == K_PINFO)) || off < f->win || off + size > f->win + f->wsize) continue;
        if (f->ptr == '0') return (int)(f->mac + (off - f->win));
        if ((off - f->win) % 4 || size != 4) return -1; // part of a pointer
        *ptr_kind = kind_of(f->ptr);
        return (int)(f->mac + (off - f->win) / 4 * 8);
    }
    return -1;
}

static const WinVar *win_var(uint32_t a) {
    int lo = 0, hi = NVARS - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_vars[mid].win <= a) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return best >= 0 && a < g_vars[best].win + g_vars[best].size ? &g_vars[best] : NULL;
}

// the real address for a script's `size`-byte access at `a`, or NULL (logged once).
static void *ptr_arg(Cleo *c, uint32_t v);
static int g_ptr_kind; // set by real_addr: the access is a pointer field of this kind, or -1
static uint8_t *real_addr(Cleo *c, uint32_t a, uint32_t size, int write) {
    g_ptr_kind = -1;
    if (a >= SLOT_BASE) {
        uint32_t i = (a - SLOT_BASE) >> 16, off = a & 0xFFFF;
        if (i >= (uint32_t)g_nslots) goto bad;
        Slot *s = &g_slots[i];
        if (s->kind == K_RAW) return s->p + off;
        int m = field(s->kind, off, size, &g_ptr_kind);
        if (m >= 0) return s->p + m;
        if (once(skey(c, a & 0xFFFF) ^ ((uint64_t)s->kind << 40)))
            mvc_log_warn("[CLEO] %s: %s field +0x%X (%u bytes) isn't mapped to macVC yet - %s skipped",
                         c->file, KIND[s->kind], off, size, write ? "write" : "read");
        return NULL;
    }
    if (a >= 0x400000 && a < WIN_IMAGE_END) {
        const WinVar *v = win_var(a);
        if (v && a + size <= v->win + v->size) {
            int k = (int)(v - g_vars);
            if (!g_var_real[k]) g_var_real[k] = mvc_sym(v->sym);
            uint8_t *base = g_var_real[k];
            uint32_t off = a - v->win;
            if (base && v->kind == '0') return base + off;
            if (base && v->kind == 'I') { // CWorld::Players: CPlayerInfo is 0x170 bytes on Windows
                int m = field(K_PINFO, off % 0x170, size, &g_ptr_kind), ps = sdk_offset(SDK_SIZE_PINFO);
                if (m >= 0 && ps > 0) return base + off / 0x170 * (uint32_t)ps + m;
                if (once(skey(c, a)))
                    mvc_log_warn("[CLEO] %s: CPlayerInfo field +0x%X (%u bytes) isn't mapped to macVC yet - %s skipped",
                                 c->file, off % 0x170, size, write ? "write" : "read");
                return NULL;
            }
            if (base && off % 4 == 0 && size == 4) { g_ptr_kind = kind_of(v->kind); return base + off / 4 * 8; } // pointer
        }
        if (once(skey(c, a)))
            mvc_log_warn("[CLEO] %s: %s Windows %s 0x%X (%u bytes)%s%s - skipped", c->file, write ? "writes" : "reads",
                         a < WIN_TEXT_END ? "code at" : "address", a, size, v ? ", inside " : "", v ? v->name : "");
        return NULL;
    }
    // ScriptSpace-relative: global variables, game script structs, our region
    uint8_t *p = SS + a;
    if (a + size <= SIZE_SPACE) return p;
    if (p >= ScriptsArray && p + size <= ScriptsArray + MAX_SCRIPTS_GAME * RS_SIZE) return p;
    if (R && p >= R && p + size <= R + REGION_SIZE) return p;
bad:
    if (once(skey(c, a)))
        mvc_log_warn("[CLEO] %s: %s bad address 0x%X - skipped", c->file, write ? "writes to" : "reads from", a);
    return NULL;
}

// parameters
static int32_t arg(Cleo *c) { CollectParameters(c->rs, IP(c), 1); return Params[0]; }
static float argf(Cleo *c) { int32_t v = arg(c); float f; memcpy(&f, &v, 4); return f; }
static void ret(Cleo *c, int32_t v) { Params[0] = v; StoreParameters(c->rs, IP(c), 1); }
static void retf(Cleo *c, float f) { int32_t v; memcpy(&v, &f, 4); ret(c, v); }
static void cond(Cleo *c, int ok) { UpdateCompareFlag(c->rs, ok != 0); }
static uint8_t peek(Cleo *c) { return SS[*IP(c)]; }

static void skip_param(Cleo *c) {
    uint32_t *ip = IP(c);
    switch (SS[*ip]) {
    case 0: *ip += 1; break;
    case 1: case 6: *ip += 5; break;
    case 2: case 3: case 5: *ip += 3; break;
    case 4: *ip += 2; break;
    case 0x0E: *ip += 2 + SS[*ip + 1]; break;
    default: *ip += 8; break; // VC inline 8-character string
    }
}

// a string argument: a literal, or a variable holding the address of one
static const char *str_arg(Cleo *c, char *out, size_t cap) {
    uint32_t *ip = IP(c);
    uint8_t t = SS[*ip];
    out[0] = 0;
    if (t == 0x0E) {
        size_t n = SS[*ip + 1];
        if (n >= cap) n = cap - 1;
        memcpy(out, SS + *ip + 2, n);
        out[n] = 0;
        *ip += 2 + SS[*ip + 1];
    } else if (t >= 1 && t <= 6) {
        uint32_t a = (uint32_t)arg(c);
        uint8_t *p = real_addr(c, a, 1, 0);
        if (p) { size_t i = 0; for (; i + 1 < cap && p[i]; i++) out[i] = (char)p[i]; out[i] = 0; }
    } else {
        memcpy(out, SS + *ip, 8);
        out[8 < cap ? 8 : cap - 1] = 0;
        *ip += 8;
    }
    return out;
}

// a string output: a variable holding the address to write to
static void str_out(Cleo *c, const char *s) {
    uint8_t t = peek(c);
    if (t < 1 || t > 6) { skip_param(c); return; }
    uint32_t a = (uint32_t)arg(c);
    size_t n = strlen(s) + 1;
    uint8_t *p = real_addr(c, a, (uint32_t)n, 1);
    if (p) memcpy(p, s, n);
}

// one value of an argument list; a string literal becomes its address
static int32_t one_arg(Cleo *c) {
    if (peek(c) != 0x0E) return arg(c);
    char tmp[256];
    str_arg(c, tmp, sizeof tmp);
    uint8_t *copy = scratch(strlen(tmp) + 1);
    if (copy) strcpy((char *)copy, tmp);
    return (int32_t)to_script(copy, K_RAW);
}

// variable-length argument list ended by a 0 byte
static int var_args(Cleo *c, int32_t *out, int max) {
    int n = 0;
    while (peek(c) != 0) {
        int32_t v = one_arg(c);
        if (n < max) out[n++] = v;
    }
    *IP(c) += 1;
    return n;
}
static void skip_var_args(Cleo *c) { while (peek(c) != 0) skip_param(c); *IP(c) += 1; }

// printf with script values: %d %i %u %x %X %o %c, %f %e %g (floats), %s (addresses), %%
static void format(Cleo *c, const char *fmt, const int32_t *a, int n, char *out, size_t cap) {
    size_t o = 0;
    int k = 0;
    for (const char *p = fmt; *p && o + 1 < cap; p++) {
        if (*p != '%') { out[o++] = *p; continue; }
        if (p[1] == '%') { out[o++] = '%'; p++; continue; }
        char spec[32];
        size_t s = 0;
        spec[s++] = '%';
        p++;
        // flags, width, .precision - in that order, so libc only ever sees a valid spec
        while (*p && strchr("-+ #0", *p) && s < 8) spec[s++] = *p++;
        for (int d = 0; isdigit((unsigned char)*p); p++) if (d++ < 3) spec[s++] = *p; // width up to 999
        if (*p == '.') {
            spec[s++] = *p++;
            for (int d = 0; isdigit((unsigned char)*p); p++) if (d++ < 3) spec[s++] = *p;
        }
        while (*p && strchr("hlLqjzt", *p)) p++;
        if (!*p) break;
        spec[s++] = *p;
        spec[s] = 0;
        int32_t v = k < n ? a[k] : 0;
        k++;
        char tmp[512];
        if (strchr("fFeEgGaA", *p)) { float f; memcpy(&f, &v, 4); snprintf(tmp, sizeof tmp, spec, (double)f); }
        else if (*p == 's') {
            uint8_t *sp = real_addr(c, (uint32_t)v, 1, 0);
            snprintf(tmp, sizeof tmp, spec, sp ? (const char *)sp : "");
        } else if (*p == 'c') snprintf(tmp, sizeof tmp, spec, (int)v);
        else if (strchr("uxXo", *p)) snprintf(tmp, sizeof tmp, spec, (unsigned)v);
        else { spec[s - 1] = 'd'; snprintf(tmp, sizeof tmp, spec, (int)v); }
        for (char *t = tmp; *t && o + 1 < cap; t++) out[o++] = *t;
    }
    out[o] = 0;
}

// game text wants UTF-16 that stays alive while it's on screen
static uint16_t *wide(const char *s) {
    static uint16_t ring[16][400];
    static int at;
    uint16_t *w = ring[at++ & 15];
    int i = 0;
    for (; s[i] && i < 399; i++) w[i] = (uint8_t)s[i];
    w[i] = 0;
    return w;
}

// files, paths
// Windows paths are relative to the game folder ("cleo\\Mix Sets.ini"). a script in a
// Mod Loader mod looks next to itself first.
static void resolve_ci(const char *in, char *out, size_t n) {
    // case-insensitive walk from g_game_dir; missing parts are kept as written
    char rel[PATH_MAX];
    snprintf(rel, sizeof rel, "%s", in);
    for (char *p = rel; *p; p++) if (*p == '\\') *p = '/';
    char cur[PATH_MAX];
    snprintf(cur, sizeof cur, "%s", g_game_dir);
    char *save = NULL;
    for (char *part = strtok_r(rel, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (!strcmp(part, ".")) continue;
        char found[256] = "";
        DIR *d = opendir(cur);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) if (!strcasecmp(e->d_name, part)) { snprintf(found, sizeof found, "%s", e->d_name); break; }
            closedir(d);
        }
        size_t L = strlen(cur);
        snprintf(cur + L, sizeof cur - L, "/%s", found[0] ? found : part);
    }
    snprintf(out, n, "%s", cur);
}

static void script_path(Cleo *c, const char *in, char *out, size_t n) {
    if (in[0] == '/') { snprintf(out, n, "%s", in); return; }
    if (c && c->mod[0]) { // a mod's script: "cleo\\x.ini" or "x.ini" next to the script
        char dir[PATH_MAX], rel[PATH_MAX];
        snprintf(dir, sizeof dir, "%s", c->path);
        char *sl = strrchr(dir, '/');
        if (sl) *sl = 0;
        const char *r = in;
        if (!strncasecmp(r, "cleo\\", 5) || !strncasecmp(r, "cleo/", 5)) r += 5;
        snprintf(rel, sizeof rel, "%s/%s", dir, r);
        for (char *p = rel + strlen(dir); *p; p++) if (*p == '\\') *p = '/';
        if (!access(rel, F_OK)) { snprintf(out, n, "%s", rel); return; }
    }
    resolve_ci(in, out, n);
}

#define MAX_FILES 32
static FILE *g_files[MAX_FILES];
static Cleo *g_file_owner[MAX_FILES];

// Windows key codes
static int vk_to_glfw(int vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return vk;
    if (vk >= 0x70 && vk <= 0x87) return 290 + (vk - 0x70);   // F1..F24
    if (vk >= 0x60 && vk <= 0x69) return 320 + (vk - 0x60);   // numpad 0..9
    switch (vk) {
    case 0x08: return 259; case 0x09: return 258; case 0x0D: return 257; case 0x1B: return 256;
    case 0x20: return 32;  case 0x21: return 266; case 0x22: return 267; case 0x23: return 269;
    case 0x24: return 268; case 0x25: return 263; case 0x26: return 265; case 0x27: return 262;
    case 0x28: return 264; case 0x2D: return 260; case 0x2E: return 261; case 0x14: return 280;
    case 0x6A: return 332; case 0x6B: return 334; case 0x6D: return 333; case 0x6E: return 330; case 0x6F: return 331;
    case 0xA0: case 0x10: return 340; case 0xA1: return 344;
    case 0xA2: case 0x11: return 341; case 0xA3: return 345;
    case 0xA4: case 0x12: return 342; case 0xA5: return 346;
    case 0xBA: return 59; case 0xBB: return 61; case 0xBC: return 44; case 0xBD: return 45;
    case 0xBE: return 46; case 0xBF: return 47; case 0xC0: return 96; case 0xDB: return 91;
    case 0xDC: return 92; case 0xDD: return 93; case 0xDE: return 39;
    }
    return -1;
}
static int vk_down(int vk) {
    if (vk == 1) return mvc_mouse_down(0);
    if (vk == 2) return mvc_mouse_down(1);
    if (vk == 4) return mvc_mouse_down(2);
    if (vk == 0x10) return mvc_key_down(340) || mvc_key_down(344);
    if (vk == 0x11) return mvc_key_down(341) || mvc_key_down(345);
    if (vk == 0x12) return mvc_key_down(342) || mvc_key_down(346);
    int k = vk_to_glfw(vk);
    return k >= 0 && mvc_key_down(k);
}

// script lifetime
static Cleo *start_script(const char *path, const char *mod, const int32_t *args, int nargs);

static void kill(Cleo *c) {
    if (c->in_list) RemoveScriptFromList(c->rs, pActive);
    c->in_list = 0;
    c->dead = 1;
    for (int i = 0; i < MAX_FILES; i++)
        if (g_file_owner[i] == c && g_files[i]) { fclose(g_files[i]); g_files[i] = NULL; g_file_owner[i] = NULL; }
}

static void fail(Cleo *c, const char *why) {
    mvc_log_error("[CLEO] %s stopped at offset 0x%X: %s", c->file, c->last_op_ip - c->base, why);
    c->failed = 1;
    kill(c);
}

static int8_t op_terminate(Cleo *c) { kill(c); return 1; }

static uint32_t target(Cleo *c, int32_t v) { return v >= 0 ? (uint32_t)v : c->base + (uint32_t)(-v); }

static int8_t op_cleo_call(Cleo *c) {
    uint32_t to = target(c, arg(c));
    int n = arg(c);
    int32_t a[32];
    int got = 0;
    for (int i = 0; i < n && peek(c) != 0; i++) { int32_t v = arg(c); if (got < 32) a[got++] = v; }
    if (c->ncalls >= MAX_CALLS) { fail(c, "CLEO_CALL nested too deep"); return 1; }
    Frame *f = &c->calls[c->ncalls++];
    f->ret_ip = *IP(c);                       // the return variables follow
    memcpy(f->locals, LOC(c), sizeof f->locals);
    for (int i = 0; i < got && i < NLOCALS; i++) LOC(c)[i] = a[i];
    *IP(c) = to;
    return 0;
}

static int8_t op_cleo_return(Cleo *c) {
    int n = arg(c);
    int32_t r[32];
    int got = 0;
    for (int i = 0; i < n && peek(c) != 0; i++) { int32_t v = arg(c); if (got < 32) r[got++] = v; }
    if (peek(c) == 0) *IP(c) += 1;
    if (!c->ncalls) { fail(c, "CLEO_RETURN without a CLEO_CALL"); return 1; }
    Frame *f = &c->calls[--c->ncalls];
    memcpy(LOC(c), f->locals, sizeof f->locals);
    *IP(c) = f->ret_ip;
    for (int i = 0; i < got && peek(c) != 0; i++) ret(c, r[i]);
    skip_var_args(c);
    return 0;
}

static int8_t op_memory(Cleo *c, int write) {
    uint32_t a = (uint32_t)arg(c), size = (uint32_t)arg(c);
    int32_t value = write ? arg(c) : 0;
    arg(c); // virtual protect: not needed
    if (size == 0 || size > 4) { // a fill, e.g. NOPs over x86 code
        uint8_t *p = write && size ? real_addr(c, a, size, 1) : NULL;
        if (p) memset(p, value & 0xFF, size);
        if (!write) ret(c, 0);
        return 0;
    }
    uint8_t *p = real_addr(c, a, size, write);
    int pk = g_ptr_kind;
    if (p && pk >= 0) { // a pointer field: 64-bit in macVC, a 32-bit stand-in for the script
        if (write) *(void **)p = ptr_arg(c, (uint32_t)value);
        else ret(c, (int32_t)to_script(*(void **)p, pk));
        return 0;
    }
    if (write) { if (p) memcpy(p, &value, size); return 0; }
    int32_t v = 0;
    if (p) memcpy(&v, p, size);
    if (size == 1) v = (uint8_t)v; else if (size == 2) v = (uint16_t)v;
    ret(c, v);
    return 0;
}

// calling game functions by their Windows address (cleo_funcs.h)
typedef struct { uint32_t win; uint8_t method; const char *sym; char ret; const char *args; const char *name; } WinFunc;
static const WinFunc g_funcs[] = {
#include "cleo_funcs.h"
};
#define NFUNCS ((int)(sizeof g_funcs / sizeof g_funcs[0]))
static float g_pop_float;

static const WinFunc *win_func(uint32_t a) {
    int lo = 0, hi = NFUNCS - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_funcs[mid].win == a) return &g_funcs[mid];
        if (g_funcs[mid].win < a) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

// a pointer argument: an object stand-in at offset 0 is the object itself
static void *ptr_arg(Cleo *c, uint32_t v) {
    if (!v) return NULL;
    if (v >= SLOT_BASE && ((v - SLOT_BASE) >> 16) < (uint32_t)g_nslots && !(v & 0xFFFF)) return g_slots[(v - SLOT_BASE) >> 16].p;
    return real_addr(c, v, 1, 0);
}

typedef uint64_t (*FnI)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                        float, float, float, float, float, float, float, float);
typedef float (*FnF)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                     float, float, float, float, float, float, float, float);

static int8_t op_call_function(Cleo *c, int method, int returns) {
    uint32_t fn = (uint32_t)arg(c);
    uint32_t self = method ? (uint32_t)arg(c) : 0;
    int n = arg(c);
    arg(c); // pop: the stack is the callee's business on ARM
    // n arguments, then the return variable (if any), then a 0 byte
    int32_t a[32];
    int got = 0;
    for (int i = 0; i < n && peek(c) != 0; i++) {
        int32_t v = one_arg(c);
        if (got < 32) a[got++] = v;
    }
    const WinFunc *w = win_func(fn);
    void *target_fn = w ? mvc_sym(w->sym) : NULL;
    int want = 0;
    for (const char *k = w ? w->args : ""; *k; k++) want += *k == '3' ? 3 : 1;
    const char *why = !w ? "isn't a known game function" : !target_fn ? "isn't in this macVC build"
                    : want != n || got != n ? "was given the wrong number of arguments"
                    : (w->method != method) ? (method ? "isn't a method" : "is a method") : NULL;
    void *thisp = method ? ptr_arg(c, self) : NULL;
    if (!why && method && !thisp) why = "was called on a null object";
    if (why) {
        if (once(skey(c, fn) ^ 0xCA11))
            mvc_log_warn("[CLEO] %s calls Windows function 0x%X%s%s, which %s - skipped", c->file, fn,
                         w ? " " : "", w ? w->name : "", why);
        if (returns && peek(c) != 0) ret(c, 0);
        skip_var_args(c);
        return 0;
    }
    // CLEO pushes the arguments as listed, so the last one listed is the first argument
    uint64_t x[8] = {0};
    float f[8] = {0};
    int nx = 0, nf = 0, at = n - 1;
    if (method) x[nx++] = (uint64_t)(uintptr_t)thisp;
    for (const char *k = w->args; *k; k++) {
        if (*k == 'f' || *k == '3') {
            for (int r = 0; r < (*k == '3' ? 3 : 1); r++) { memcpy(&f[nf++], &a[at], 4); at--; }
        } else if (*k == 'p') x[nx++] = (uint64_t)(uintptr_t)ptr_arg(c, (uint32_t)a[at--]);
        else x[nx++] = (uint64_t)(int64_t)a[at--];
    }
    int32_t r = 0;
    if (w->ret == 'f') {
        g_pop_float = ((FnF)target_fn)(x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7], f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
        memcpy(&r, &g_pop_float, 4);
    } else {
        uint64_t v = ((FnI)target_fn)(x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7], f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
        switch (w->ret) {
        case 'i': r = (int32_t)v; break;
        case 'P': r = (int32_t)to_script((void *)(uintptr_t)v, K_PED); break;
        case 'V': r = (int32_t)to_script((void *)(uintptr_t)v, K_VEH); break;
        case 'O': r = (int32_t)to_script((void *)(uintptr_t)v, K_OBJ); break;
        case 'p': r = (int32_t)to_script((void *)(uintptr_t)v, K_RAW); break;
        }
    }
    if (returns && peek(c) != 0) ret(c, r);
    skip_var_args(c);
    return 0;
}

static int8_t op_print(Cleo *c, int kind, int formatted) {
    char fmt[512], text[512];
    str_arg(c, fmt, sizeof fmt);
    int32_t time = 0, style = 0, a[32];
    if (kind != 0) time = arg(c);       // 0 = help box
    if (kind == 3) style = arg(c);       // big
    int n = formatted ? var_args(c, a, 32) : 0;
    if (formatted) format(c, fmt, a, n, text, sizeof text); else snprintf(text, sizeof text, "%s", fmt);
    if (kind == 0 && SetHelpMessage) SetHelpMessage(wide(text), 0, 0);
    else if (kind == 1 && AddMessage) AddMessage(wide(text), (uint32_t)time, 0);
    else if (kind == 2 && AddMessageJumpQ) AddMessageJumpQ(wide(text), (uint32_t)time, 0);
    else if (kind == 3 && AddBigMessage) AddBigMessage(wide(text), (uint32_t)time, (uint16_t)style);
    return 0;
}

static int8_t op_ini(Cleo *c, int op) {
    int write = op & 1, type = (op - 0x0AF0) / 2; // 0 int, 1 float, 2 string
    char val[256] = "", path[PATH_MAX], file[256], sec[128], key[128];
    int32_t iv = 0;
    if (write) { if (type == 2) str_arg(c, val, sizeof val); else iv = arg(c); }
    script_path(c, str_arg(c, file, sizeof file), path, sizeof path);
    str_arg(c, sec, sizeof sec);
    str_arg(c, key, sizeof key);
    if (write) {
        if (type == 0) snprintf(val, sizeof val, "%d", iv);
        else if (type == 1) { float f; memcpy(&f, &iv, 4); snprintf(val, sizeof val, "%g", (double)f); }
        cond(c, mvc_ini_set(path, sec, key, val));
        return 0;
    }
    int ok = mvc_ini_get(path, sec, key, val, sizeof val);
    if (type == 0) { if (ok) ret(c, (int32_t)strtol(val, NULL, 0)); else skip_param(c); }
    else if (type == 1) { if (ok) retf(c, strtof(val, NULL)); else skip_param(c); }
    else { if (ok) str_out(c, val); else skip_param(c); }
    cond(c, ok);
    return 0;
}

// sscanf with script outputs; returns how many were stored
static int scan_into(Cleo *c, const char *src, const char *fmt) {
    int stored = 0;
    const char *s = src;
    for (const char *f = fmt; *f && peek(c) != 0; f++) {
        if (isspace((unsigned char)*f)) { while (isspace((unsigned char)*s)) s++; continue; }
        if (*f != '%') { if (*s != *f) break; s++; continue; }
        f++;
        while (*f && strchr("0123456789hl", *f)) f++;
        char *end = NULL;
        while (isspace((unsigned char)*s) && *f != 'c') s++;
        if (*f == 'd' || *f == 'i' || *f == 'u' || *f == 'x' || *f == 'X') {
            long v = strtol(s, &end, *f == 'x' || *f == 'X' ? 16 : 10);
            if (end == s) break;
            ret(c, (int32_t)v);
        } else if (strchr("fFeEgG", *f)) {
            float v = strtof(s, &end);
            if (end == s) break;
            retf(c, v);
        } else if (*f == 's') {
            char word[256];
            size_t n = 0;
            while (*s && !isspace((unsigned char)*s) && n < 255) word[n++] = *s++;
            word[n] = 0;
            if (!n) break;
            str_out(c, word);
            end = (char *)s;
        } else break;
        s = end;
        stored++;
    }
    skip_var_args(c);
    return stored;
}

static int8_t op_file(Cleo *c, int op) {
    char a1[PATH_MAX], a2[PATH_MAX], p1[PATH_MAX], p2[PATH_MAX];
    switch (op) {
    case 0x0A99: skip_param(c); return 0; // SET_CURRENT_DIRECTORY: paths are always game-relative here
    case 0x0A9A: {
        script_path(c, str_arg(c, a1, sizeof a1), p1, sizeof p1);
        int32_t mode = arg(c);
        // mode: a short string packed into the number ('r' = 114, "wb"...), or a string address
        char m[8] = "rb";
        int literal = mode != 0;
        for (int i = 0; i < 4; i++) {
            char ch = (char)((uint32_t)mode >> (8 * i));
            if (ch && !strchr("rwabt+", ch)) literal = 0;
        }
        if (literal) { memcpy(m, &mode, 4); m[4] = 0; }
        else { uint8_t *mp = real_addr(c, (uint32_t)mode, 1, 0); if (mp && mp[0]) snprintf(m, sizeof m, "%.3s", (char *)mp); }
        if (!strchr(m, 'b') && strlen(m) < 3) strcat(m, "b"); // Windows text mode isn't a thing here
        int slot = -1;
        for (int i = 0; i < MAX_FILES; i++) if (!g_files[i]) { slot = i; break; }
        FILE *f = slot >= 0 ? fopen(p1, m) : NULL;
        if (f) { g_files[slot] = f; g_file_owner[slot] = c; }
        ret(c, f ? slot + 1 : 0);
        cond(c, f != NULL);
        return 0;
    }
    }
    if (op == 0x0AAB || op == 0x0AE4 || op == 0x0AE5 || op == 0x0B00 || op == 0x0B01) {
        script_path(c, str_arg(c, a1, sizeof a1), p1, sizeof p1);
        struct stat st;
        int ok = 0;
        if (op == 0x0AAB) ok = !stat(p1, &st);
        else if (op == 0x0AE4) ok = !stat(p1, &st) && S_ISDIR(st.st_mode);
        else if (op == 0x0AE5) ok = !mkdir(p1, 0755) || errno == EEXIST;
        else if (op == 0x0B00) ok = !unlink(p1);
        else { arg(c); ok = !rmdir(p1); }
        cond(c, ok);
        return 0;
    }
    if (op >= 0x0B02 && op <= 0x0B05) {
        script_path(c, str_arg(c, a1, sizeof a1), p1, sizeof p1);
        script_path(c, str_arg(c, a2, sizeof a2), p2, sizeof p2);
        int ok = 0;
        if (op <= 0x0B03) ok = !rename(p1, p2);
        else if (op == 0x0B04) {
            FILE *in = fopen(p1, "rb"), *o = in ? fopen(p2, "wb") : NULL;
            if (o) { char b[8192]; size_t n; while ((n = fread(b, 1, sizeof b, in)) > 0) fwrite(b, 1, n, o); fclose(o); ok = 1; }
            if (in) fclose(in);
        }
        cond(c, ok);
        return 0;
    }
    // operations on an open file: the first argument is the handle
    int32_t h = arg(c);
    FILE *f = h >= 1 && h <= MAX_FILES ? g_files[h - 1] : NULL;
    switch (op) {
    case 0x0A9B: if (f) { fclose(f); g_files[h - 1] = NULL; g_file_owner[h - 1] = NULL; } return 0;
    case 0x0A9C: {
        long sz = 0;
        if (f) { long at = ftell(f); fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, at, SEEK_SET); }
        ret(c, (int32_t)sz);
        return 0;
    }
    case 0x0A9D: { // READ_FROM_FILE size -> variable
        int32_t n = arg(c), v = 0;
        if (f && n > 0 && n <= 4) fread(&v, 1, (size_t)n, f);
        ret(c, v);
        return 0;
    }
    case 0x0A9E: { int32_t n = arg(c), v = arg(c); if (f && n > 0 && n <= 4) fwrite(&v, 1, (size_t)n, f); return 0; }
    case 0x0AD5: { int32_t off = arg(c), wh = arg(c); cond(c, f && !fseek(f, off, wh)); return 0; }
    case 0x0AD6: cond(c, !f || feof(f) || (ungetc(fgetc(f), f) == EOF)); return 0;
    case 0x0AD7: { // READ_STRING_FROM_FILE dest max
        uint32_t dest = (uint32_t)arg(c);
        int32_t max = arg(c);
        uint8_t *p = max > 0 ? real_addr(c, dest, (uint32_t)max, 1) : NULL;
        int ok = p && f && fgets((char *)p, max, f);
        cond(c, ok);
        return 0;
    }
    case 0x0AD8: str_arg(c, a1, sizeof a1); cond(c, f && fputs(a1, f) >= 0); return 0;
    case 0x0AD9: {
        int32_t a[32];
        str_arg(c, a1, sizeof a1);
        int n = var_args(c, a, 32);
        format(c, a1, a, n, a2, sizeof a2);
        if (f) fputs(a2, f);
        return 0;
    }
    case 0x0ADA: { // SCAN_FILE format -> count, values...
        str_arg(c, a1, sizeof a1);
        char line[1024] = "";
        int ok = f && fgets(line, sizeof line, f);
        uint32_t count_ip = *IP(c);
        skip_param(c);                        // the count output, written below
        int n = ok ? scan_into(c, line, a1) : (skip_var_args(c), 0);
        uint32_t after = *IP(c);
        *IP(c) = count_ip;
        ret(c, n);
        *IP(c) = after;
        cond(c, ok);
        return 0;
    }
    }
    return 0;
}

// an opcode the runner knows by name but doesn't implement: step over it
typedef struct { uint16_t op; uint8_t n, variadic, is_cond; const char *name; } OpInfo;
static const OpInfo g_ops[] = {
#include "cleo_ops.h"
};
static const OpInfo *op_info(int op) {
    for (size_t i = 0; i < sizeof g_ops / sizeof g_ops[0]; i++) if (g_ops[i].op == op) return &g_ops[i];
    return NULL;
}
static int8_t op_unsupported(Cleo *c, const OpInfo *in) {
    for (int i = 0; i < in->n; i++) {
        if (in->variadic && i == in->n - 1) { skip_var_args(c); break; }
        skip_param(c);
    }
    if (in->is_cond) cond(c, 0);
    if (once(skey(c, in->op) ^ 0x0BC0DE))
        mvc_log_warn("[CLEO] %s uses %04X %s, which isn't supported on Mac yet - skipped", c->file, in->op, in->name);
    return 0;
}


// GET_RANDOM_CHAR/CAR/OBJECT_IN_SPHERE_NO_SAVE_RECURSIVE
static int8_t op_sphere(Cleo *c, int kind, int has_skip) {
    float x = argf(c), y = argf(c), z = argf(c), r = argf(c);
    int next = arg(c), skip = has_skip ? arg(c) : 0;
    uint8_t **pp = kind == K_PED ? PedPool : kind == K_VEH ? VehPool : ObjPool;
    void *(*get)(int) = kind == K_PED ? PoolsGetPed : kind == K_VEH ? PoolsGetVehicle : PoolsGetObject;
    uint8_t *pool = pp ? *pp : NULL;
    int found = -1;
    if (pool && get) {
        uint8_t *flags = *(uint8_t **)(pool + 8);
        int size = *(int32_t *)(pool + 0x10);
        int pk, health = field(kind, kind == K_PED ? 0x354 : 0x204, 4, &pk);
        for (int i = next ? c->find_at[kind] : 0; i < size && found < 0; i++) {
            if (flags[i] & 0x80) continue; // free slot
            int h = (i << 8) | flags[i];
            uint8_t *e = get(h);
            if (!e || (kind == K_PED && e == mvc_player_ped())) continue;
            if (skip && kind != K_OBJ && health >= 0 && *(float *)(e + health) <= 0) continue; // dead / wrecked
            MVCVec3 p = mvc_entity_pos(e);
            float dx = p.x - x, dy = p.y - y, dz = p.z - z;
            if (dx * dx + dy * dy + dz * dz > r * r) continue;
            found = h;
            c->find_at[kind] = i + 1;
        }
    }
    if (found < 0) c->find_at[kind] = 0;
    ret(c, found);
    cond(c, found >= 0);
    return 0;
}

// DISPLAY_TEXT_STRING / FORMATTED: drawn on the next HUD frame, like the game's own
typedef struct { float x, y; char s[160]; } ScreenText;
static ScreenText g_stext[48];
static int g_nstext;
static void draw_texts(void) {
    float sx = mvc_screen_w() / 640.f, sy = mvc_screen_h() / 448.f;
    for (int i = 0; i < g_nstext; i++)
        mvc_text(g_stext[i].x * sx, g_stext[i].y * sy, g_stext[i].s, 0.48f * sx, 1.12f * sy,
                 (MVCColor){225, 225, 225, 255}, 0, 1, 2);
    g_nstext = 0;
}

static int g_stream; // PLAY_AUDIO_STREAM: one stream, like CLEO's 1-channel player

static void clipboard(int write, uint8_t *p, int n) {
    FILE *f = popen(write ? "/usr/bin/pbcopy" : "/usr/bin/pbpaste", write ? "w" : "r");
    if (!f) return;
    if (write) fwrite(p, 1, strnlen((char *)p, (size_t)n), f);
    else { size_t got = fread(p, 1, (size_t)n - 1, f); p[got] = 0; }
    pclose(f);
}

static int g_shared[1024];

static void *find_script_named(const char *name) {
    for (uint8_t *s = *pActive; s; s = *(uint8_t **)s)
        if (!strncasecmp((char *)s + 0x10, name, 8)) return s;
    return NULL;
}

// vehicle state the CLEO car commands read: offsets confirmed in this build's code at startup
static int g_veh_flags = -1, g_veh_gear = -1, g_hnd_gears = -1, g_veh_handling = -1, g_mi_gamename = -1;
static void **ModelInfoPtrs;
static void *(*BlendAnimation)(void *clump, int group, int anim, float delta);

// the first instruction in func[0..max) matching (insn & mask) == want, or NULL
static const uint32_t *find_insn(const void *func, int max, uint32_t mask, uint32_t want) {
    const uint32_t *p = func;
    for (int i = 0; p && i < max; i++) if ((p[i] & mask) == want) return &p[i];
    return NULL;
}
#define IMM12(v) ((uint32_t)(v) << 10)
static void find_vehicle_offsets(void) {
    const void *ctor = mvc_sym("_ZN8CVehicleC2Eh"), *gears = mvc_sym("_ZN13cTransmission14InitGearRatiosEv");
    const void *hload = mvc_sym("_ZN16cHandlingDataMgr16LoadHandlingDataEv"), *vload = mvc_sym("_ZN11CFileLoader17LoadVehicleObjectEPKc");
    // CVehicle(): add xN, x0, #flags (the bit fields: engine on = bit 4, lights on = bit 6); strb wN, [x0, #gear]
    if (find_insn(ctor, 64, 0xFFFFFFE0, 0x91000000 | IMM12(0x295))) g_veh_flags = 0x295;
    if (find_insn(ctor, 64, 0xFFFFFC00, 0x39000000 | IMM12(0x2A4))) g_veh_gear = 0x2A4;
    // cTransmission::InitGearRatios: ldrsb w8, [x0, #nNumberOfGears]; handling data holds it at +0x34 + that
    const uint32_t *g = find_insn(gears, 16, 0xFFC00000, 0x39C00000); // LDRSB Wt, [Xn, #imm]
    int toff = g ? (int)((*g >> 10) & 0xFFF) : -1;
    if (toff > 0 && find_insn(hload, 1200, 0xFFFFFC00, 0x39000000 | IMM12(0x34 + toff))) g_hnd_gears = 0x34 + toff; // STRB
    g_veh_handling = 0x160; // CVehicle::m_pHandlingData (cleo_fields.h; read throughout CAutomobile::ProcessControl)
    // LoadVehicleObject: strcpy(mi->m_gameName, ...) is the first "add x0, x19, #imm" after SetTexDictionary
    const uint32_t *t = find_insn(vload, 64, 0xFFC003FF, 0x91000000 | (19 << 5)); // add x0, x19, #imm
    if (t) { int off = (int)((*t >> 10) & 0xFFF); if (off >= 0x20 && off < 0x100) g_mi_gamename = off; }
    ModelInfoPtrs = mvc_sym("_ZN10CModelInfo16ms_modelInfoPtrsE");
    *(void **)&BlendAnimation = mvc_sym("_ZN12CAnimManager14BlendAnimationEPN2rw5ClumpE12AssocGroupId11AnimationIdf");
    if (g_veh_flags < 0 || g_veh_gear < 0 || g_hnd_gears < 0 || g_mi_gamename < 0)
        mvc_log_warn("[CLEO] car state offsets not all found in this macVC build (flags %d gear %d gears %d name %d) - "
                     "those car commands return 0", g_veh_flags, g_veh_gear, g_hnd_gears, g_mi_gamename);
}

static int8_t run_cleo_op(Cleo *c, int op) {
    char s1[PATH_MAX], s2[512], path[PATH_MAX];
    switch (op) {
    // control flow (the game's versions would read CLEO labels as mission offsets)
    case 0x0002: *IP(c) = target(c, arg(c)); return 0;
    case 0x004C: { uint32_t t = target(c, arg(c)); if (c->rs[RS_COND]) *IP(c) = t; return 0; }
    case 0x004D: { uint32_t t = target(c, arg(c)); if (!c->rs[RS_COND]) *IP(c) = t; return 0; }
    case 0x0AA0: case 0x0050: {
        uint32_t t = target(c, arg(c));
        if (op == 0x0AA0 && c->rs[RS_COND]) return 0;
        uint16_t *sp = (uint16_t *)(c->rs + RS_SP);
        if (*sp >= STACK_DEPTH) { fail(c, "GOSUB nested too deep"); return 1; }
        ((uint32_t *)(c->rs + RS_STACK))[(*sp)++] = *IP(c);
        *IP(c) = t;
        return 0;
    }
    case 0x0AA1: {
        if (c->rs[RS_COND]) return 0;
        uint16_t *sp = (uint16_t *)(c->rs + RS_SP);
        if (*sp) *IP(c) = ((uint32_t *)(c->rs + RS_STACK))[--(*sp)];
        return 0;
    }
    case 0x004E: case 0x05DC: case 0x0A93: return op_terminate(c);
    case 0x004F: { // START_NEW_SCRIPT with a label inside this file: another script on the same code
        int32_t lab = arg(c);
        int32_t a[NLOCALS];
        int n = var_args(c, a, NLOCALS);
        if (lab >= 0) { fail(c, "START_NEW_SCRIPT into main.scm"); return 1; }
        Cleo *k = start_script(NULL, c->mod, a, n);
        if (k) {
            k->base = c->base; k->size = c->size;
            snprintf(k->file, sizeof k->file, "%s", c->file);
            snprintf(k->path, sizeof k->path, "%s", c->path);
            *IP(k) = target(c, lab);
        }
        return 0;
    }
    case 0x05DD: case 0x0ABA: { // TERMINATE_ALL_CUSTOM_SCRIPTS_WITH_THIS_NAME
        str_arg(c, s1, sizeof s1);
        int self = 0;
        for (int i = 0; i < g_ncleo; i++)
            if (!g_cleo[i]->dead && !strncasecmp((char *)g_cleo[i]->rs + 0x10, s1, 8)) {
                if (g_cleo[i] == c) self = 1; else kill(g_cleo[i]);
            }
        if (self) return op_terminate(c);
        return 0;
    }
    case 0x05DE: case 0x0600: case 0x0A92: { // START_CUSTOM_SCRIPT file args...
        str_arg(c, s1, sizeof s1);
        int32_t a[NLOCALS];
        int n = var_args(c, a, NLOCALS);
        char dir[PATH_MAX];
        snprintf(dir, sizeof dir, "%s", c->path);
        char *sl = strrchr(dir, '/');
        if (sl) *sl = 0;
        snprintf(path, sizeof path, "%s/%s", dir, s1);
        for (char *p = path; *p; p++) if (*p == '\\') *p = '/';
        if (access(path, R_OK)) script_path(c, s1, path, sizeof path);
        if (!start_script(path, c->mod, a, n)) mvc_log_warn("[CLEO] %s: couldn't start %s", c->file, s1);
        return 0;
    }
    case 0x05F5: case 0x0AB1: return op_cleo_call(c);
    case 0x05F6: case 0x0AB2: return op_cleo_return(c);
    case 0x0AA9: cond(c, 1); return 0;      // IS_GAME_VERSION_ORIGINAL: addresses are 1.0's
    case 0x05E5: ret(c, 0); return 0;       // GET_GAME_VERSION: 1.0
    // memory
    case 0x05DF: case 0x0A8C: return op_memory(c, 1);
    case 0x05E0: case 0x0A8D: return op_memory(c, 0);
    case 0x05E1: case 0x0AA5: return op_call_function(c, 0, 0);
    case 0x05E2: case 0x0AA7: return op_call_function(c, 0, 1);
    case 0x05E3: case 0x0AA6: return op_call_function(c, 1, 0);
    case 0x05E4: case 0x0AA8: return op_call_function(c, 1, 1);
    case 0x05E6: case 0x0A96: ret(c, (int32_t)to_script(PoolsGetPed ? PoolsGetPed(arg(c)) : (arg(c), NULL), K_PED)); return 0;
    case 0x05E7: case 0x0A97: ret(c, (int32_t)to_script(PoolsGetVehicle ? PoolsGetVehicle(arg(c)) : (arg(c), NULL), K_VEH)); return 0;
    case 0x05E8: case 0x0A98: ret(c, (int32_t)to_script(PoolsGetObject ? PoolsGetObject(arg(c)) : (arg(c), NULL), K_OBJ)); return 0;
    case 0x05E9: case 0x0AEA: case 0x05EA: case 0x0AEB: case 0x05EB: case 0x0AEC: {
        uint32_t a = (uint32_t)arg(c);
        int want = (op == 0x05E9 || op == 0x0AEA) ? K_PED : (op == 0x05EA || op == 0x0AEB) ? K_VEH : K_OBJ;
        void *p = NULL;
        if (a >= SLOT_BASE && ((a - SLOT_BASE) >> 16) < (uint32_t)g_nslots && g_slots[(a - SLOT_BASE) >> 16].kind == want)
            p = g_slots[(a - SLOT_BASE) >> 16].p;
        int h = -1;
        if (p) h = want == K_PED ? PoolsGetPedRef(p) : want == K_VEH ? PoolsGetVehicleRef(p) : PoolsGetObjectRef(p);
        ret(c, h);
        return 0;
    }
    case 0x05EC: case 0x0A9F: ret(c, (int32_t)to_script(c->rs, K_RAW)); return 0;
    case 0x05ED: case 0x0AAA: str_arg(c, s1, sizeof s1); ret(c, (int32_t)to_script(find_script_named(s1), K_RAW)); return 0;
    case 0x05F7: case 0x0AC6: ret(c, (int32_t)to_script(SS + target(c, arg(c)), K_RAW)); return 0;
    case 0x05F8: case 0x0AC7: {
        uint8_t t = peek(c);
        int32_t *v = (t == 2 || t == 3) ? GetPointerToScriptVariable(c->rs, IP(c), t == 2 ? 2 : 1) : NULL;
        if (!v) skip_param(c);
        ret(c, (int32_t)to_script(v, K_RAW));
        return 0;
    }
    case 0x0AC8: { int32_t n = arg(c); uint8_t *p = n > 0 ? ralloc((size_t)n) : NULL; ret(c, (int32_t)to_script(p, K_RAW)); cond(c, p != NULL); return 0; }
    case 0x0AC9: arg(c); return 0;          // FREE_MEMORY: the region is reset when scripts restart
    case 0x0606: arg(c); arg(c); arg(c); return 0;
    // maths
    case 0x05F3: case 0x0AEE: { float a = argf(c), b = argf(c); retf(c, powf(a, b)); return 0; }
    case 0x05F4: case 0x0AEF: { float a = argf(c), b = argf(c); retf(c, logf(a) / logf(b)); return 0; }
    case 0x05F2: case 0x0AE9: retf(c, g_pop_float); return 0; // POP_FLOAT: a float-returning call's result
    case 0x0A8E: { int32_t a = arg(c), b = arg(c); ret(c, a + b); return 0; }
    case 0x0A8F: { int32_t a = arg(c), b = arg(c); ret(c, a - b); return 0; }
    case 0x0A90: { int32_t a = arg(c), b = arg(c); ret(c, a * b); return 0; }
    case 0x0A91: { int32_t a = arg(c), b = arg(c); ret(c, b ? a / b : 0); return 0; }
    case 0x05F9: case 0x0B10: { int32_t a = arg(c), b = arg(c); ret(c, a & b); return 0; }
    case 0x05FA: case 0x0B11: { int32_t a = arg(c), b = arg(c); ret(c, a | b); return 0; }
    case 0x05FB: case 0x0B12: { int32_t a = arg(c), b = arg(c); ret(c, a ^ b); return 0; }
    case 0x05FC: case 0x0B13: { int32_t a = arg(c); ret(c, ~a); return 0; }
    case 0x05FD: case 0x0B14: { int32_t a = arg(c), b = arg(c); ret(c, b ? a % b : 0); return 0; }
    case 0x05FE: case 0x0B15: { int32_t a = arg(c), b = arg(c); ret(c, (int32_t)((uint32_t)a >> (b & 31))); return 0; }
    case 0x05FF: case 0x0B16: { int32_t a = arg(c), b = arg(c); ret(c, (int32_t)((uint32_t)a << (b & 31))); return 0; }
    case 0x0B17: case 0x0B18: case 0x0B19: case 0x0B1B: case 0x0B1C: case 0x0B1D: case 0x0B1A: {
        uint8_t t = peek(c);
        int32_t *v = (t == 2 || t == 3) ? GetPointerToScriptVariable(c->rs, IP(c), t == 2 ? 2 : 1) : NULL;
        if (!v) { skip_param(c); if (op != 0x0B1A) skip_param(c); return 0; }
        if (op == 0x0B1A) { *v = ~*v; return 0; }
        int32_t b = arg(c);
        if (op == 0x0B17) *v &= b; else if (op == 0x0B18) *v |= b; else if (op == 0x0B19) *v ^= b;
        else if (op == 0x0B1B) *v = b ? *v % b : 0;
        else if (op == 0x0B1C) *v = (int32_t)((uint32_t)*v >> (b & 31));
        else *v = (int32_t)((uint32_t)*v << (b & 31));
        return 0;
    }
    // input
    case 0x05EE: case 0x0AB0: cond(c, vk_down(arg(c))); return 0;
    case 0x0601: case 0x0602: {
        int32_t b = arg(c), v = arg(c);
        int16_t *st = GetPad && b >= 0 && b < 20 ? (int16_t *)GetPad(0) : NULL; // NewState, one short per button
        if (op == 0x0602) { if (st) st[b] = (int16_t)v; }
        else cond(c, st && st[b] == v);
        return 0;
    }
    case 0x0ADC: { // TEST_CHEAT
        str_arg(c, s1, sizeof s1);
        size_t n = strlen(s1);
        int ok = CheatString && n > 0 && n < 30;
        for (size_t i = 0; ok && i < n; i++) ok = toupper((unsigned char)CheatString[i]) == toupper((unsigned char)s1[n - 1 - i]);
        if (ok) CheatString[0] = 0;
        cond(c, ok);
        return 0;
    }
    case 0x0607: ret(c, OldWeather ? *OldWeather : 0); return 0;
    // text
    case 0x0ACA: return op_print(c, 0, 0);
    case 0x0ACE: return op_print(c, 0, 1);
    case 0x0ACC: return op_print(c, 1, 0);
    case 0x0AD0: return op_print(c, 1, 1);
    case 0x0ACD: return op_print(c, 2, 0);
    case 0x0AD1: return op_print(c, 2, 1);
    case 0x0ACB: return op_print(c, 3, 0);
    case 0x0ACF: return op_print(c, 3, 1);
    case 0x0AD3: { // STRING_FORMAT dest fmt args
        uint32_t dest_ip = *IP(c);
        skip_param(c);
        str_arg(c, s1, sizeof s1);
        int32_t a[32];
        int n = var_args(c, a, 32);
        format(c, s1, a, n, s2, sizeof s2);
        uint32_t after = *IP(c);
        *IP(c) = dest_ip;
        str_out(c, s2);
        *IP(c) = after;
        return 0;
    }
    case 0x0AD4: { // SCAN_STRING src fmt -> count values...
        str_arg(c, s1, sizeof s1);
        str_arg(c, s2, sizeof s2);
        uint32_t count_ip = *IP(c);
        skip_param(c);
        int n = scan_into(c, s1, s2);
        uint32_t after = *IP(c);
        *IP(c) = count_ip;
        ret(c, n);
        *IP(c) = after;
        cond(c, n > 0);
        return 0;
    }
    case 0x0ADE: { // GET_TEXT_LABEL_STRING key -> string
        str_arg(c, s1, sizeof s1);
        const uint16_t *w = mvc_text_get(s1);
        size_t i = 0;
        for (; w && w[i] && i < sizeof s2 - 1; i++) s2[i] = w[i] < 256 ? (char)w[i] : '?';
        s2[i] = 0;
        str_out(c, s2);
        return 0;
    }
    case 0x0ADF: str_arg(c, s1, 64); str_arg(c, s2, sizeof s2); mvc_text_set(s1, s2); return 0;
    // cars: gears, engine, lights, model name
    case 0x0AB7: case 0x0AB8: case 0x0ABD: case 0x0ABE: case 0x0ABF: {
        int32_t h = arg(c);
        uint8_t *v = PoolsGetVehicle ? PoolsGetVehicle(h) : NULL;
        uint8_t *fl = v && g_veh_flags >= 0 ? v + g_veh_flags : NULL;
        if (op == 0x0ABF) { int on = arg(c); if (fl) *fl = on ? (*fl | 0x10) : (*fl & ~0x10); return 0; } // SET_CAR_ENGINE_ON
        if (op == 0x0ABD) { cond(c, fl && (*fl & 0x40)); return 0; }                                    // IS_CAR_LIGHTS_ON
        if (op == 0x0ABE) { cond(c, fl && (*fl & 0x10)); return 0; }                                    // IS_CAR_ENGINE_ON
        if (op == 0x0AB8) { ret(c, v && g_veh_gear >= 0 ? (int8_t)v[g_veh_gear] : 0); return 0; }       // GET_CAR_CURRENT_GEAR
        uint8_t *hd = v && g_hnd_gears >= 0 ? *(uint8_t **)(v + g_veh_handling) : NULL;                 // GET_CAR_NUMBER_OF_GEARS
        ret(c, hd ? (int8_t)hd[g_hnd_gears] : 0);
        return 0;
    }
    case 0x0ADB: { // GET_NAME_OF_VEHICLE_MODEL: the GXT key of the car's name ("INFERNU")
        int32_t m = arg(c);
        uint8_t *mi = ModelInfoPtrs && m >= 0 && m < 6500 && g_mi_gamename >= 0 ? ModelInfoPtrs[m] : NULL;
        char name[9] = "";
        if (mi) { memcpy(name, mi + g_mi_gamename, 8); name[8] = 0; }
        str_out(c, name);
        return 0;
    }
    case 0x0673: { // PLAY_ANIMATION char group anim blend
        int32_t h = arg(c), group = arg(c), anim = arg(c);
        float delta = argf(c);
        uint8_t *ped = PoolsGetPed ? PoolsGetPed(h) : NULL;
        void *clump = ped ? *(void **)(ped + 0x58) : NULL; // CEntity::m_rwObject
        if (clump && BlendAnimation && group >= 0 && anim >= 0) BlendAnimation(clump, group, anim, delta);
        return 0;
    }
    case 0x0AE0: str_arg(c, s1, sizeof s1); return 0;
    // ini, files
    case 0x0AF0: case 0x0AF1: case 0x0AF2: case 0x0AF3: case 0x0AF4: case 0x0AF5: return op_ini(c, op);
    case 0x0A99: case 0x0A9A: case 0x0A9B: case 0x0A9C: case 0x0A9D: case 0x0A9E: case 0x0AAB: case 0x0AD5:
    case 0x0AD6: case 0x0AD7: case 0x0AD8: case 0x0AD9: case 0x0ADA: case 0x0AE4: case 0x0AE5:
    case 0x0B00: case 0x0B01: case 0x0B02: case 0x0B03: case 0x0B04: case 0x0B05:
        return op_file(c, op);
    // world
    case 0x05EF: case 0x0AE1: return op_sphere(c, K_PED, 1);
    case 0x05F0: case 0x0AE2: return op_sphere(c, K_VEH, 1);
    case 0x05F1: case 0x0AE3: return op_sphere(c, K_OBJ, 0);
    case 0x0603: cond(c, TheCamera && g_widescreen_off >= 0 && TheCamera[g_widescreen_off]); return 0;
    case 0x0604: { // GET_WEAPONTYPE_MODEL
        int32_t t = arg(c), off = sdk_offset(SDK_OFF_WMODEL);
        uint8_t *info = GetWeaponInfo && t >= 0 && t < 37 && off >= 0 ? GetWeaponInfo(t) : NULL;
        ret(c, info ? *(int32_t *)(info + off) : -1);
        return 0;
    }
    case 0x0605: { // GET_WEAPONTYPE_FOR_MODEL
        int32_t m = arg(c), off = sdk_offset(SDK_OFF_WMODEL), t = -1;
        for (int i = 0; GetWeaponInfo && off >= 0 && i < 37 && t < 0; i++) {
            uint8_t *info = GetWeaponInfo(i);
            if (info && *(int32_t *)(info + off) == m) t = i;
        }
        ret(c, t);
        return 0;
    }
    case 0x0608: case 0x0609: {
        float x = argf(c), y = argf(c);
        str_arg(c, s1, sizeof s1);
        if (op == 0x0609) { int32_t a[32]; int n = var_args(c, a, 32); format(c, s1, a, n, s2, sizeof s2); }
        else snprintf(s2, sizeof s2, "%s", s1);
        if (g_nstext < 48) { g_stext[g_nstext].x = x; g_stext[g_nstext].y = y; snprintf(g_stext[g_nstext].s, 160, "%s", s2); g_nstext++; }
        return 0;
    }
    case 0x0ADD: { int32_t m = arg(c); if (VehicleCheat) VehicleCheat(m); return 0; } // SPAWN_VEHICLE_BY_CHEATING
    case 0x0AAC: { // PLAY_AUDIO_STREAM_1CHANNEL file loop volume
        str_arg(c, s1, sizeof s1);
        int32_t loop = arg(c);
        float vol = argf(c);
        script_path(c, s1, path, sizeof path);
        if (g_stream) mvc_sound_stop(g_stream);
        g_stream = mvc_sound_play(path, vol, loop);
        return 0;
    }
    case 0x0AAD: if (g_stream) mvc_sound_stop(g_stream); g_stream = 0; return 0;
    case 0x0B20: case 0x0B21: { // READ/WRITE_CLIPBOARD_DATA address size
        uint32_t a = (uint32_t)arg(c);
        int32_t n = arg(c);
        uint8_t *p = n > 1 && n < 65536 ? real_addr(c, a, (uint32_t)n, op == 0x0B20) : NULL;
        if (p) clipboard(op == 0x0B21, p, n);
        return 0;
    }
    // shared variables
    case 0x0AB3: { int32_t i = arg(c), v = arg(c); if (i >= 0 && i < 1024) g_shared[i] = v; return 0; }
    case 0x0AB4: { int32_t i = arg(c); ret(c, i >= 0 && i < 1024 ? g_shared[i] : 0); return 0; }
    }
    const OpInfo *in = op_info(op);
    if (in) return op_unsupported(c, in);
    char why[96];
    snprintf(why, sizeof why, "unknown opcode %04X (from a Windows .cleo plugin?)", op);
    fail(c, why);
    return 1;
}

static int is_cleo_op(int op) {
    return op >= 0x05DC || op == 0x0002 || op == 0x004C || op == 0x004D || op == 0x004E || op == 0x004F || op == 0x0050;
}

static int8_t h_ProcessOneCommand(uint8_t *s) {
    if (!is_cleo(s)) return o_ProcessOneCommand(s);
    Cleo *c = (Cleo *)s;
    uint32_t ip = *IP(c);
    uint16_t raw = (uint16_t)(SS[ip] | SS[ip + 1] << 8);
    int op = raw & 0x7FFF;
    if (!is_cleo_op(op)) return o_ProcessOneCommand(s);
    c->last_op_ip = ip;
    (*CommandsExecuted)++;
    *IP(c) = ip + 2;
    c->rs[RS_NOT] = raw >> 15;
    return run_cleo_op(c, op);
}

// JavaScript (cleo_js.c)
// a JS script's commands run through the same interpreter path as a CLEO script's:
// each one is assembled into the script's own small code buffer and executed once.
#define JS_CODE 4096
Cleo *cleo_js_record(const char *path, const char *mod) {
    if (g_ncleo >= MAX_CLEO) return NULL;
    Cleo *c = (Cleo *)ralloc(sizeof(Cleo));
    uint8_t *code = c ? ralloc(JS_CODE) : NULL;
    if (!code) return NULL;
    c->base = (uint32_t)(code - SS);
    c->size = JS_CODE;
    snprintf(c->path, sizeof c->path, "%s", path);
    const char *b = strrchr(path, '/');
    snprintf(c->file, sizeof c->file, "%s", b ? b + 1 : path);
    snprintf(c->mod, sizeof c->mod, "%s", mod ? mod : "");
    memcpy(c->rs + 0x10, "jsscript", 8);
    c->rs[RS_ACTIVE] = 1;
    g_cleo[g_ncleo++] = c; // never in the game's list: the game doesn't run it, the JS thread does
    return c;
}

int cleo_js_exec(Cleo *c, const uint8_t *code, size_t len, int32_t *out, int nout) {
    if (len + 2 > JS_CODE || c->dead) return 0;
    memcpy(SS + c->base, code, len);
    SS[c->base + len] = SS[c->base + len + 1] = 0;
    *IP(c) = c->base;
    c->rs[RS_COND] = 0;
    c->rs[RS_NOT] = 0;
    *(uint16_t *)(c->rs + 0x88) = 0; // AND/OR state
    memset(LOC(c), 0, NLOCALS * 4);
    h_ProcessOneCommand(c->rs);
    for (int i = 0; i < nout && i < NLOCALS; i++) out[i] = LOC(c)[i];
    return c->rs[RS_COND];
}

uint8_t *cleo_js_addr(Cleo *c, uint32_t a, uint32_t size, int write) { return real_addr(c, a, size, write); }
uint32_t cleo_js_ptr(void *p) { return to_script(p, K_RAW); }
const char *cleo_js_failed(Cleo *c) { return c->failed ? "stopped" : NULL; }

// loading
static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = n > 0 && n < (16 << 20) ? malloc((size_t)n) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *len = (size_t)n;
    return b;
}

// path == NULL: a record only (START_NEW_SCRIPT fills in the code)
static Cleo *start_script(const char *path, const char *mod, const int32_t *args, int nargs) {
    if (g_ncleo >= MAX_CLEO) return NULL;
    Cleo *c = (Cleo *)ralloc(sizeof(Cleo));
    if (!c) return NULL;
    if (path) {
        size_t len;
        uint8_t *code = slurp(path, &len);
        uint8_t *dst = code ? ralloc(len + 16) : NULL;
        if (!dst) { free(code); return NULL; }
        memcpy(dst, code, len);
        free(code);
        c->base = (uint32_t)(dst - SS);
        c->size = (uint32_t)len;
        snprintf(c->path, sizeof c->path, "%s", path);
        const char *b = strrchr(path, '/');
        snprintf(c->file, sizeof c->file, "%s", b ? b + 1 : path);
    }
    snprintf(c->mod, sizeof c->mod, "%s", mod ? mod : "");
    // CRunningScript::Init: name "noname", wake-time 0, deatharrest on
    memcpy(c->rs + 0x10, "noname\0", 8);
    c->rs[0x8B] = 1;
    *IP(c) = c->base;
    for (int i = 0; i < nargs && i < NLOCALS; i++) LOC(c)[i] = args[i];
    c->rs[RS_ACTIVE] = 1;
    AddScriptToList(c->rs, pActive);
    c->in_list = 1;
    g_cleo[g_ncleo++] = c;
    return c;
}

static int script_enabled(const char *file) {
    char v[32];
    if (mvc_ini_get("macVCLoader.ini", "CLEO", file, v, sizeof v) && atoi(v) <= 0) return 0;
    return 1;
}

// scripts that a native plugin already replaces
static const char *replaced_by(const char *file) {
    static const struct { const char *script, *plugin; } R2[] = {{"[VC]Gasoline.cs", "Gasoline"}, {"SESlices.cs", "SeveredLimbs"}};
    for (size_t i = 0; i < sizeof R2 / sizeof R2[0]; i++)
        if (!strcasecmp(file, R2[i].script) && plugin_loaded(R2[i].plugin)) return R2[i].plugin;
    return NULL;
}

static int cmp_name(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }

static void start_all(void) {
    // every script restarts from scratch: drop the old records, code and handles
    js_stop_all(); // JS threads use records in the region that's about to be reset
    for (int i = 0; i < g_ncleo; i++) kill(g_cleo[i]);
    g_ncleo = 0;
    R_used = 0;
    g_nslots = 0;
    g_scratch = ralloc(SCRATCH_SIZE);
    g_scratch_at = 0;
    int started = 0, skipped = 0;
    char *names[256];
    int n = 0;
    DIR *d = g_cleo_dir[0] ? opendir(g_cleo_dir) : NULL;
    struct dirent *e;
    while (d && (e = readdir(d)) && n < 256) {
        size_t L = strlen(e->d_name);
        if (e->d_name[0] == '.') continue;
        if ((L > 3 && !strcasecmp(e->d_name + L - 3, ".cs")) || (L > 3 && !strcasecmp(e->d_name + L - 3, ".js")))
            names[n++] = strdup(e->d_name);
    }
    if (d) closedir(d);
    qsort(names, (size_t)n, sizeof *names, cmp_name);
    for (int i = 0; i < n + g_nmodscripts; i++) {
        char path[PATH_MAX];
        const char *mod = "";
        if (i < n) snprintf(path, sizeof path, "%s/%s", g_cleo_dir, names[i]);
        else { snprintf(path, sizeof path, "%s", g_modscripts[i - n].path); mod = g_modscripts[i - n].mod; }
        const char *file = strrchr(path, '/') + 1;
        const char *by = replaced_by(file);
        if (!script_enabled(file) || by) {
            if (!g_ready) mvc_log("[CLEO] %s %s", file, by ? "is replaced by a native plugin - not started" : "is turned off in macVCLoader.ini [CLEO]");
            skipped++;
            continue;
        }
        size_t L = strlen(file);
        if (L > 3 && !strcasecmp(file + L - 3, ".js")) { if (js_start(path, mod)) started++; continue; }
        if (start_script(path, mod, NULL, 0)) started++;
        else mvc_log_error("[CLEO] couldn't load %s", path);
    }
    for (int i = 0; i < n; i++) free(names[i]);
    mvc_log("[CLEO] started %d script(s)%s", started, skipped ? " (some skipped, see above)" : "");
}

void cleo_add_mod_script(const char *path, const char *mod) {
    if (g_nmodscripts >= 64) return;
    snprintf(g_modscripts[g_nmodscripts].path, PATH_MAX, "%s", path);
    snprintf(g_modscripts[g_nmodscripts].mod, 64, "%s", mod);
    g_nmodscripts++;
}

// game hooks
static void h_TheScriptsInit(void) {
    o_TheScriptsInit();
    // the game just emptied its script lists, ours included
    for (int i = 0; i < g_ncleo; i++) g_cleo[i]->in_list = 0;
    g_restart = 1;
}

static void h_TheScriptsProcess(void) {
    o_TheScriptsProcess();
    // start (or restart) after the game's own scripts have run: the first tick happens while
    // the game is still loading, and main.scm creates the player in it. a CLEO script's first
    // command may need the player (SEDrunk starts with GET_PLAYER_CHAR), as on Windows.
    if (g_restart && g_enabled) { g_restart = 0; start_all(); g_ready = 1; return; }
    js_tick(); // JavaScript scripts run right after the game's and CLEO's scripts
}

static void h_SaveAllScripts(uint8_t *buf, uint32_t *size) {
    // the save stores each active script as an index into ScriptsArray: keep ours out
    for (int i = 0; i < g_ncleo; i++) if (g_cleo[i]->in_list) RemoveScriptFromList(g_cleo[i]->rs, pActive);
    o_SaveAllScripts(buf, size);
    for (int i = g_ncleo - 1; i >= 0; i--) if (g_cleo[i]->in_list) AddScriptToList(g_cleo[i]->rs, pActive);
}

// console
static void cmd_cleo(int argc, const char **argv) {
    if (argc >= 2 && !strcasecmp(argv[1], "reload")) { g_restart = 1; mvc_log("[CLEO] scripts restart on the next game tick"); return; }
    int live = 0;
    for (int i = 0; i < g_ncleo; i++) if (!g_cleo[i]->dead) live++;
    mvc_log("[CLEO] %d script(s), %d running (folder %s):", g_ncleo, live, g_cleo_dir[0] ? g_cleo_dir : "none");
    for (int i = 0; i < g_ncleo; i++) {
        Cleo *c = g_cleo[i];
        char name[9];
        memcpy(name, c->rs + 0x10, 8);
        name[8] = 0;
        mvc_log("[CLEO]   %-28s %-8s %s at 0x%X%s%s", c->file, name, c->failed ? "FAILED" : c->dead ? "ended" : "running",
                *IP(c) - c->base, c->mod[0] ? " - mod " : "", c->mod);
    }
}

void cleo_init(void) {
    SS = mvc_sym("_ZN11CTheScripts11ScriptSpaceE");
    ScriptsArray = mvc_sym("_ZN11CTheScripts12ScriptsArrayE");
    pActive = mvc_sym("_ZN11CTheScripts14pActiveScriptsE");
    Params = mvc_sym("ScriptParams");
    CommandsExecuted = mvc_sym("_ZN11CTheScripts16CommandsExecutedE");
    CheatString = mvc_sym("_ZN4CPad19KeyBoardCheatStringE");
    OldWeather = mvc_sym("_ZN8CWeather14OldWeatherTypeE");
    CollectParameters = mvc_sym("_ZN14CRunningScript17CollectParametersEPjs");
    StoreParameters = mvc_sym("_ZN14CRunningScript15StoreParametersEPjs");
    GetPointerToScriptVariable = mvc_sym("_ZN14CRunningScript26GetPointerToScriptVariableEPjs");
    UpdateCompareFlag = mvc_sym("_ZN14CRunningScript17UpdateCompareFlagEb");
    AddScriptToList = mvc_sym("_ZN14CRunningScript15AddScriptToListEPPS_");
    RemoveScriptFromList = mvc_sym("_ZN14CRunningScript20RemoveScriptFromListEPPS_");
    PoolsGetPed = mvc_sym("_ZN6CPools6GetPedEi");
    PoolsGetVehicle = mvc_sym("_ZN6CPools10GetVehicleEi");
    PoolsGetObject = mvc_sym("_ZN6CPools9GetObjectEi");
    PoolsGetPedRef = mvc_sym("_ZN6CPools9GetPedRefEP4CPed");
    PoolsGetVehicleRef = mvc_sym("_ZN6CPools13GetVehicleRefEP8CVehicle");
    PoolsGetObjectRef = mvc_sym("_ZN6CPools12GetObjectRefEP7CObject");
    GetPad = mvc_sym("_ZN4CPad6GetPadEi");
    SetHelpMessage = mvc_sym("_ZN4CHud14SetHelpMessageEPtbb");
    AddMessage = mvc_sym("_ZN9CMessages10AddMessageEPtjt");
    AddMessageJumpQ = mvc_sym("_ZN9CMessages15AddMessageJumpQEPtjt");
    AddBigMessage = mvc_sym("_ZN9CMessages13AddBigMessageEPtjt");
    PedPool = mvc_sym("_ZN6CPools11ms_pPedPoolE");
    VehPool = mvc_sym("_ZN6CPools15ms_pVehiclePoolE");
    ObjPool = mvc_sym("_ZN6CPools14ms_pObjectPoolE");
    TheCamera = mvc_sym("TheCamera");
    GetWeaponInfo = mvc_sym("_ZN11CWeaponInfo13GetWeaponInfoE11eWeaponType");
    VehicleCheat = mvc_sym("_Z12VehicleCheati");
    find_vehicle_offsets();
    const uint32_t *ws = mvc_sym("_ZN7CCamera15SetWideScreenOnEv"); // mov w8,#1; strb w8,[x0,#off]
    for (int i = 0; ws && i < 4 && g_widescreen_off < 0; i++)
        if ((ws[i] & 0xFFC003E0) == 0x39000000) g_widescreen_off = (int)((ws[i] >> 10) & 0xFFF);
    mvc_on_hud(draw_texts);
    mvc_console_command("cleo", "list CLEO scripts; 'cleo reload' restarts them", cmd_cleo);

    // the folder: GameData/CLEO (any case)
    DIR *d = opendir(g_game_dir);
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (!strcasecmp(e->d_name, "cleo")) snprintf(g_cleo_dir, sizeof g_cleo_dir, "%s/%s", g_game_dir, e->d_name);
    if (d) closedir(d);
    if (!g_cleo_dir[0] && !g_nmodscripts) { mvc_log("[CLEO] no GameData/CLEO folder - CLEO runner idle"); g_enabled = 0; return; }

    void *poc = mvc_sym("_ZN14CRunningScript17ProcessOneCommandEv");
    int ok = SS && ScriptsArray && pActive && Params && CommandsExecuted && CollectParameters && StoreParameters &&
             GetPointerToScriptVariable && UpdateCompareFlag && AddScriptToList && RemoveScriptFromList && poc;
    // this build's CRunningScript layout: ProcessOneCommand reads the IP at +0x18,
    // and the game's 128 scripts fill ScriptsArray up to IntroTextLines
    uint8_t *after = mvc_sym("_ZN11CTheScripts14IntroTextLinesE");
    if (ok && (mvc_first_load_imm(poc, 16, 4) != RS_IP || (after && after - ScriptsArray != MAX_SCRIPTS_GAME * RS_SIZE))) {
        mvc_log_error("[CLEO] this macVC build's script engine has a different layout - CLEO runner disabled");
        g_enabled = 0;
        return;
    }
    if (!ok || alloc_region()) { mvc_log_error("[CLEO] script engine not found - CLEO runner disabled"); g_enabled = 0; return; }
    if (mvc_hook(poc, (void *)h_ProcessOneCommand, (void **)&o_ProcessOneCommand) ||
        mvc_hook_sym("_ZN11CTheScripts4InitEv", (void *)h_TheScriptsInit, (void **)&o_TheScriptsInit) ||
        mvc_hook_sym("_ZN11CTheScripts7ProcessEv", (void *)h_TheScriptsProcess, (void **)&o_TheScriptsProcess) ||
        mvc_hook_sym("_ZN11CTheScripts14SaveAllScriptsEPhPj", (void *)h_SaveAllScripts, (void **)&o_SaveAllScripts)) {
        mvc_log_error("[CLEO] couldn't hook the script engine - CLEO runner disabled");
        g_enabled = 0;
        return;
    }
    mvc_log("[CLEO] runner ready: %s, %d mod script(s); Windows mapping: %d variables, %d functions, %d struct fields; "
            "code at ScriptSpace+0x%llX", g_cleo_dir[0] ? g_cleo_dir : "no CLEO folder", g_nmodscripts, NVARS, NFUNCS,
            (int)(sizeof g_fields / sizeof g_fields[0]), (unsigned long long)(R - SS));
}
