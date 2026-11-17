// mvc-patch, install helper. ships prebuilt so installing needs nothing but macos.
//


//   mvc-patch check  <binary> <dylib path>   exit 0 if the load command is there, 1 if not
//   mvc-patch insert <binary> <dylib path>   adds a weak LC_LOAD_DYLIB if it's missing
//   mvc-patch compat <binary> <dylib>...     checks every game symbol the dylibs use is in
//                                            this macVC build, exit 1 if one's missing
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *len = (size_t)n;
    return b;
}

static struct mach_header_64 *thin(unsigned char *d, size_t len, const char *path) {
    struct mach_header_64 *mh = (void *)d;
    if (!d || len < sizeof *mh || mh->magic != MH_MAGIC_64) {
        fprintf(stderr, "%s is not a thin 64-bit Mach-O\n", path);
        return NULL;
    }
    return mh;
}

// compat
static const char **g_syms;
static size_t g_nsyms;
static int cmp(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

static void load_symbols(unsigned char *d) {
    struct mach_header_64 *mh = (void *)d;
    struct load_command *lc = (void *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++, lc = (void *)((char *)lc + lc->cmdsize)) {
        if (lc->cmd != LC_SYMTAB) continue;
        struct symtab_command *st = (void *)lc;
        struct nlist_64 *nl = (void *)(d + st->symoff);
        g_syms = malloc(st->nsyms * sizeof *g_syms);
        for (uint32_t k = 0; k < st->nsyms; k++)
            if ((nl[k].n_type & N_TYPE) == N_SECT) g_syms[g_nsyms++] = (char *)d + st->stroff + nl[k].n_un.n_strx;
        qsort(g_syms, g_nsyms, sizeof *g_syms, cmp);
    }
}

static int has_symbol(const char *name) {
    char buf[512];
    snprintf(buf, sizeof buf, "_%s", name);
    const char *key = buf;
    return bsearch(&key, g_syms, g_nsyms, sizeof *g_syms, cmp) != NULL;
}

static const char *C_NAMES[] = {"RsGlobal", "TheCamera", "ThePaths", "FrontEndMenuManager", "TheText", "aScreens",
                                "gSelectedJoystickName", "_InputInitialiseJoys", "DMAudio", NULL};

static int looks_like_game_symbol(const char *s) {
    if (s[0] == '_' && s[1] == 'Z' && strlen(s) > 4) {
        for (const char *c = s; *c; c++)
            if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_')) return 0;
        // the plugin's own exported C++ names are not game symbols, but plugins are C; fine
        return 1;
    }
    for (int i = 0; C_NAMES[i]; i++) if (!strcmp(s, C_NAMES[i])) return 1;
    return 0;
}

static int compat_one(const char *path) {
    size_t len;
    unsigned char *d = slurp(path, &len);
    if (!thin(d, len, path)) return 1;
    struct mach_header_64 *mh = (void *)d;
    struct load_command *lc = (void *)(mh + 1);
    int used = 0, missing = 0;
    for (uint32_t i = 0; i < mh->ncmds; i++, lc = (void *)((char *)lc + lc->cmdsize)) {
        if (lc->cmd != LC_SEGMENT_64) continue;
        struct segment_command_64 *sc = (void *)lc;
        struct section_64 *s = (void *)(sc + 1);
        for (uint32_t k = 0; k < sc->nsects; k++) {
            if (strcmp(s[k].sectname, "__cstring")) continue;
            const char *p = (char *)d + s[k].offset, *end = p + s[k].size;
            while (p < end) {
                size_t n = strnlen(p, (size_t)(end - p));
                if (looks_like_game_symbol(p)) {
                    used++;
                    if (!has_symbol(p)) { printf("  missing: %s\n", p); missing++; }
                }
                p += n + 1;
            }
        }
    }
    const char *base = strrchr(path, '/');
    printf("%-24s %d game symbols, %s\n", base ? base + 1 : path, used, missing ? "INCOMPATIBLE" : "ok");
    free(d);
    return missing ? 1 : 0;
}

// main
int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: mvc-patch check|insert <binary> <dylib install path>\n"
                        "       mvc-patch compat <binary> <dylib>...\n");
        return 2;
    }
    const char *cmd = argv[1], *bin = argv[2];
    size_t len;
    unsigned char *d = slurp(bin, &len);
    if (!d) { fprintf(stderr, "cannot read %s\n", bin); return 2; }
    struct mach_header_64 *mh = thin(d, len, bin);
    if (!mh) return 2;

    if (!strcmp(cmd, "compat")) {
        load_symbols(d);
        int bad = 0;
        for (int i = 3; i < argc; i++) bad |= compat_one(argv[i]);
        return bad;
    }
    if (argc != 4 || (strcmp(cmd, "check") && strcmp(cmd, "insert"))) { fprintf(stderr, "bad arguments\n"); return 2; }
    const char *dylib = argv[3];

    size_t off = sizeof *mh, first_data = len;
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        struct load_command *lc = (void *)(d + off);
        if (lc->cmd == LC_LOAD_DYLIB || lc->cmd == LC_LOAD_WEAK_DYLIB) {
            struct dylib_command *dc = (void *)lc;
            if (!strcmp((char *)lc + dc->dylib.name.offset, dylib)) {
                if (!strcmp(cmd, "insert")) printf("already present: %s\n", dylib);
                return 0;
            }
        }
        if (lc->cmd == LC_SEGMENT_64) {
            struct segment_command_64 *sc = (void *)lc;
            struct section_64 *s = (void *)(sc + 1);
            for (uint32_t k = 0; k < sc->nsects; k++)
                if (s[k].offset && s[k].offset < first_data) first_data = s[k].offset;
        }
        off += lc->cmdsize;
    }
    if (!strcmp(cmd, "check")) return 1;

    size_t plen = strlen(dylib) + 1;
    uint32_t size = (uint32_t)((sizeof(struct dylib_command) + plen + 7) & ~7UL);
    size_t end = sizeof *mh + mh->sizeofcmds;
    if (end + size > first_data) { fprintf(stderr, "not enough header padding\n"); return 2; }
    for (size_t i = end; i < end + size; i++)
        if (d[i]) { fprintf(stderr, "header padding is not empty\n"); return 2; }
    struct dylib_command *dc = (void *)(d + end);
    dc->cmd = LC_LOAD_WEAK_DYLIB;
    dc->cmdsize = size;
    dc->dylib.name.offset = sizeof *dc;
    dc->dylib.timestamp = 2;
    dc->dylib.current_version = 0x10000;
    dc->dylib.compatibility_version = 0x10000;
    memcpy((char *)dc + sizeof *dc, dylib, plen);
    mh->ncmds++;
    mh->sizeofcmds += size;

    FILE *f = fopen(bin, "wb");
    if (!f || fwrite(d, 1, len, f) != len) { fprintf(stderr, "cannot write %s\n", bin); return 2; }
    fclose(f);
    printf("added weak load command: %s\n", dylib);
    return 0;
}
