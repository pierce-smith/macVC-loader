// mod loader, works like LINK/2012's modloader on windows.
// mods go in GameData/modloader/<ModName>/ with any folder layout.
//


// file overlay: every file the game opens goes through reVC's casepath(), so one hook on it
// redirects data files, loose txd/dff, audio and the img archives. originals never get touched.
//


// img overlay: .dff/.txd/.col/.ifp that aren't loose game files get packed into
// modloader/.cache/modloader.img (only rebuilt when a mod changes) and registered last.
// the game reads archives last to first and skips names it already has, so the mod wins.
// a folder named gta3.img inside a mod always means inside the archive.
//


// data files that more than one mod changes get merged line by line in modmerge.c.
// gta_vc.dat also picks up data lines from mod readmes, finds new map files by name,
// and drops lines pointing at files that don't exist instead of crashing.
//


// [Mods] in macVCLoader.ini: ModName=0 turns a mod off, 1-100 sets priority (default 50).
// folders starting with . or _ are skipped.
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <pthread.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "internal.h"
#include "gxt_vanilla.h"

#define SECTOR 2048
#define MAX_MODS 128

typedef struct { char name[64]; int priority, enabled, files, img, unused, unsupported; } Mod;
static Mod g_mods[MAX_MODS];
static int g_nmods;

typedef struct { char *key; char *src; int mod, used; } Entry;     // key: lowercase path relative to GameData
static Entry *g_files; static int g_nfiles, g_capfiles;        // file overlay, sorted by key
static Entry *g_img; static int g_nimg, g_capimg;              // IMG overlay, key = lowercase entry name

typedef struct { char *key; int count; char *path; } GameFile; // the game's own files, key = lowercase basename
static GameFile *g_game; static int g_ngame, g_capgame;
static char **g_gamepaths; static int g_ngamepaths, g_capgamepaths; // lowercase relative paths, sorted

// every file of every mod (readmes and new map files are looked up here)
typedef struct { char *rel; const char *base; char *src; int mod; } PoolFile; // rel: lowercase, relative to the mod
static PoolFile *g_pool; static int g_npool, g_cappool;
// every mod's version of each replaced game file, for merging
typedef struct { char *key; char *src; int mod; } Version;
static Version *g_vers; static int g_nvers, g_capvers;
static char g_root[PATH_MAX];
static int g_new_file;
// images in "name.txd/" folders: target is a game path ("models/hud.txd") or, for the archive, "img:name.txd"
typedef struct { char target[256]; char *src; int mod; } TxdImage;
static TxdImage *g_txdimg; static int g_ntxdimg, g_captxdimg;
// texture packs: images in a mod's textures/ folder replace that texture in every TXD that has it
typedef struct { char name[32]; char *src; int mod; } TexImage;
static TexImage *g_tex; static int g_ntex, g_captex;
typedef struct { char tex[32]; char target[64]; } TexLoc;     // texture -> "models/hud.txd" or "img:cuban.txd"
static TexLoc *g_texidx; static int g_ntexidx, g_captexidx, g_texidx_done;
// mods' .gxt files ("text/american.gxt"): diffed against vanilla, never swapped in whole
typedef struct { char lang[24]; char *src; int mod; } GxtMod;
static GxtMod *g_gxt; static int g_ngxt, g_capgxt;

static char g_conflicts[64][200];
static int g_nconflicts;
static int g_ini_n;
static struct { char name[64]; int value; } g_ini[MAX_MODS];
static char g_cache_img[PATH_MAX];
static int g_active;

static char *(*o_casepath)(const char *, _Bool);
static void (*o_LoadCdDirectory)(void);
static void (*f_CdStreamAddImage)(const char *);

void modloader_ini(const char *key, const char *value) {
    if (g_ini_n >= MAX_MODS) return;
    snprintf(g_ini[g_ini_n].name, sizeof g_ini[0].name, "%s", key);
    g_ini[g_ini_n++].value = atoi(value);
}

static void lower(char *s) { for (; *s; s++) *s = (char)tolower((unsigned char)*s); }

static void *grow(void *p, int *cap, int need, size_t sz) {
    if (need <= *cap) return p;
    *cap = *cap ? *cap * 2 : 256;
    if (*cap < need) *cap = need;
    return realloc(p, (size_t)*cap * sz);
}

static int skip_name(const char *n) { return n[0] == '.' || n[0] == '_'; }

// the game's own files
static void walk_game(const char *abs, const char *rel, int depth) {
    DIR *d = opendir(abs);
    if (!d || depth > 6) { if (d) closedir(d); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (depth == 0 && (!strcasecmp(e->d_name, "modloader") || !strcasecmp(e->d_name, "plugins") ||
                           !strcasecmp(e->d_name, "scripts") || !strcasecmp(e->d_name, "crashes"))) continue;
        char a[PATH_MAX], r[PATH_MAX];
        snprintf(a, sizeof a, "%s/%s", abs, e->d_name);
        snprintf(r, sizeof r, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        struct stat st;
        if (stat(a, &st)) continue;
        if (S_ISDIR(st.st_mode)) { walk_game(a, r, depth + 1); continue; }
        lower(r);
        g_gamepaths = grow(g_gamepaths, &g_capgamepaths, g_ngamepaths + 1, sizeof *g_gamepaths);
        g_gamepaths[g_ngamepaths++] = strdup(r);
    }
    closedir(d);
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static int cmp_game(const void *a, const void *b) { return strcmp(((const GameFile *)a)->key, ((const GameFile *)b)->key); }
static int cmp_entry(const void *a, const void *b) { return strcmp(((const Entry *)a)->key, ((const Entry *)b)->key); }

static int game_has_path(const char *rel) {
    return bsearch(&rel, g_gamepaths, (size_t)g_ngamepaths, sizeof *g_gamepaths, cmp_str) != NULL;
}

static GameFile *game_by_name(const char *base) {
    GameFile k = {(char *)base, 0, NULL};
    return bsearch(&k, g_game, (size_t)g_ngame, sizeof *g_game, cmp_game);
}

static void index_game(void) {
    walk_game(g_game_dir, "", 0);
    qsort(g_gamepaths, (size_t)g_ngamepaths, sizeof *g_gamepaths, cmp_str);
    // basename -> how many game files have it (a name used twice needs its folder too)
    for (int i = 0; i < g_ngamepaths; i++) {
        const char *b = strrchr(g_gamepaths[i], '/');
        b = b ? b + 1 : g_gamepaths[i];
        g_game = grow(g_game, &g_capgame, g_ngame + 1, sizeof *g_game);
        g_game[g_ngame++] = (GameFile){(char *)b, 1, g_gamepaths[i]};
    }
    qsort(g_game, (size_t)g_ngame, sizeof *g_game, cmp_game);
    int w = 0;
    for (int i = 0; i < g_ngame; i++) {
        if (w && !strcmp(g_game[w - 1].key, g_game[i].key)) { g_game[w - 1].count++; continue; }
        g_game[w++] = g_game[i];
    }
    g_ngame = w;
}

// mods
static int is_img_type(const char *base) {
    const char *dot = strrchr(base, '.');
    if (!dot || dot - base >= 21 || strlen(base) >= 24) return 0; // the game's limits for archive names
    return !strcasecmp(dot, ".dff") || !strcasecmp(dot, ".txd") || !strcasecmp(dot, ".col") || !strcasecmp(dot, ".ifp");
}

static int is_doc(const char *base) {
    const char *dot = strrchr(base, '.');
    static const char *docs[] = {".txt", ".md", ".pdf", ".rtf", ".url", ".htm", ".html", ".jpg", ".jpeg", ".png", ".ini", ".log", NULL};
    if (!dot) return 0;
    for (int i = 0; docs[i]; i++) if (!strcasecmp(dot, docs[i])) return 1;
    return 0;
}

static int same_content(const char *a, const char *b) {
    struct stat sa, sb;
    if (stat(a, &sa) || stat(b, &sb) || sa.st_size != sb.st_size) return 0;
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    int same = fa && fb;
    static char ba[65536], bb[65536];
    size_t n;
    while (same && (n = fread(ba, 1, sizeof ba, fa)) > 0)
        if (fread(bb, 1, n, fb) != n || memcmp(ba, bb, n)) same = 0;
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    return same;
}

// another mod already claims `key`: the higher priority wins, ties go to the first mod alphabetically.
// two mods shipping the very same file isn't a conflict.
static void add_entry(Entry **arr, int *n, int *cap, const char *key, const char *src, int mod) {
    if (arr == &g_files && !strncmp(key, "text/", 5) && strlen(key) > 9 && !strcmp(key + strlen(key) - 4, ".gxt") &&
        !strchr(key + 5, '/')) {
        g_gxt = grow(g_gxt, &g_capgxt, g_ngxt + 1, sizeof *g_gxt);
        snprintf(g_gxt[g_ngxt].lang, sizeof g_gxt[0].lang, "%s", key + 5);
        g_gxt[g_ngxt].src = strdup(src);
        g_gxt[g_ngxt++].mod = mod;
        return;
    }
    if (arr == &g_files && !g_new_file) {
        g_vers = grow(g_vers, &g_capvers, g_nvers + 1, sizeof *g_vers);
        g_vers[g_nvers++] = (Version){strdup(key), strdup(src), mod};
    }
    for (int i = 0; i < *n; i++) {
        if (strcmp((*arr)[i].key, key)) continue;
        if (same_content((*arr)[i].src, src)) return;
        Mod *old = &g_mods[(*arr)[i].mod], *nw = &g_mods[mod];
        int replace = nw->priority > old->priority;
        if (g_nconflicts < 64)
            snprintf(g_conflicts[g_nconflicts++], sizeof g_conflicts[0], "%s: %s wins over %s", key,
                     replace ? nw->name : old->name, replace ? old->name : nw->name);
        if (replace) { free((*arr)[i].src); (*arr)[i].src = strdup(src); (*arr)[i].mod = mod; }
        return;
    }
    *arr = grow(*arr, cap, *n + 1, sizeof **arr);
    (*arr)[(*n)++] = (Entry){strdup(key), strdup(src), mod, 0};
}

static int ml_ignored_file(const char *rel);
static const char *ext_of(const char *base) { const char *d = strrchr(base, '.'); return d ? d : ""; }

// the part of a mod path from the first game folder on: "my mod/data/maps/x.ipl" -> "data/maps/x.ipl"
static const char *from_game_dir(const char *r) {
    static const char *dirs[] = {"data/", "models/", "anim/", "audio/", "txd/", "text/", "movies/", "mp3/", NULL};
    for (const char *s = r; s; s = strchr(s, '/') ? strchr(s, '/') + 1 : NULL)
        for (int i = 0; dirs[i]; i++)
            if (!strncmp(s, dirs[i], strlen(dirs[i]))) return s;
    return NULL;
}

static void add_mod_file(int mod, const char *abs, const char *rel) {
    char r[PATH_MAX];
    snprintf(r, sizeof r, "%s", rel);
    lower(r);
    const char *base = strrchr(r, '/');
    base = base ? base + 1 : r;
    g_pool = grow(g_pool, &g_cappool, g_npool + 1, sizeof *g_pool);
    g_pool[g_npool].rel = strdup(r);
    g_pool[g_npool].base = strrchr(g_pool[g_npool].rel, '/') ? strrchr(g_pool[g_npool].rel, '/') + 1 : g_pool[g_npool].rel;
    g_pool[g_npool].src = strdup(abs);
    g_pool[g_npool++].mod = mod;
    Mod *m = &g_mods[mod];
    if (ml_ignored_file(r)) return; // modloader.ini IgnoreFiles
    // 1. folders: name.txd/ adds images to a texture dictionary; gta3.img/ means inside
    //    the archive (a .txd folder inside gta3.img/ is still a .txd folder)
    int has_txd_folder = 0;
    for (const char *s = r; s < base; s = strchr(s, '/') + 1) {
        const char *e = strchr(s, '/');
        if (e - s > 4 && !strncmp(e - 4, ".txd", 4)) has_txd_folder = 1;
    }
    // a textures/ folder (any depth, not inside a name.txd/ folder): a texture pack
    if (!has_txd_folder && txd_is_image(base))
        for (const char *s = r; s < base; s = strchr(s, '/') + 1)
            if (!strncmp(s, "textures/", 9)) {
                g_tex = grow(g_tex, &g_captex, g_ntex + 1, sizeof *g_tex);
                TexImage *t = &g_tex[g_ntex++];
                snprintf(t->name, sizeof t->name, "%s", base);
                char *dot = strrchr(t->name, '.');
                if (dot) *dot = 0;
                t->src = strdup(abs);
                t->mod = mod;
                m->files++;
                return;
            }
    for (const char *s = r; s < base; s = strchr(s, '/') + 1) {
        const char *e = strchr(s, '/');
        size_t L = (size_t)(e - s);
        if (L == 8 && !strncmp(s, "gta3.img", 8) && !has_txd_folder) {
            if (is_img_type(base)) { add_entry(&g_img, &g_nimg, &g_capimg, base, abs, mod); m->img++; }
            else { mvc_log("[ModLoader] %s: %s can't go into gta3.img (name too long or not a dff/txd/col/ifp)", m->name, rel); m->unused++; }
            return;
        }
        if (L > 4 && !strncmp(e - 4, ".txd", 4)) { // name.txd/ : images to add to that texture dictionary
            if (!txd_is_image(base)) { m->unused++; return; }
            char folder[256], target[256];
            snprintf(folder, sizeof folder, "%.*s", (int)L, s);
            int in_img = s - r >= 9 && !strncmp(s - 9, "gta3.img/", 9);
            char prefix[PATH_MAX];
            snprintf(prefix, sizeof prefix, "%.*s", (int)(e - r), r);
            const char *gd = from_game_dir(prefix);
            GameFile *g = game_by_name(folder);
            if (!in_img && gd && game_has_path(gd)) snprintf(target, sizeof target, "%s", gd);
            else if (!in_img && g && g->count == 1) snprintf(target, sizeof target, "%s", g->path);
            else snprintf(target, sizeof target, "img:%s", folder);
            g_txdimg = grow(g_txdimg, &g_captxdimg, g_ntxdimg + 1, sizeof *g_txdimg);
            snprintf(g_txdimg[g_ntxdimg].target, sizeof g_txdimg[0].target, "%s", target);
            g_txdimg[g_ntxdimg].src = strdup(abs);
            g_txdimg[g_ntxdimg++].mod = mod;
            m->files++;
            return;
        }
        if (L > 4 && !strncmp(e - 4, ".img", 4) && !(L == 8 && !strncmp(s, "gta3.img", 8))) {
            mvc_log_warn("[ModLoader] %s: %.*s is a folder of files to add to %.*s - not supported yet, skipped", m->name,
                         (int)(e - r), rel, (int)L, s);
            m->unsupported++;
            return;
        }
    }
    // 2. CLEO scripts go to the CLEO runner; Windows code (ASI/.cleo plugins) needs porting
    const char *x = ext_of(base);
    if (!strcmp(x, ".fxt")) { // CLEO text (CLEO/CLEO_TEXT/*.fxt): "KEY text" lines for scripts
        int n = mvc_text_load_fxt(abs);
        mvc_log("[ModLoader] %s: %d text key(s) from %s", m->name, n, rel);
        m->files++;
        return;
    }
    if (!strcmp(x, ".cs") || (!strcmp(x, ".js") && strstr(r, "cleo/"))) {
        cleo_add_mod_script(abs, m->name);
        mvc_log("[ModLoader] %s: %s is a %s script - the CLEO runner starts it", m->name, rel, x[1] == 'c' ? "CLEO" : "JavaScript");
        m->files++;
        return;
    }
    if (!strcmp(x, ".asi") || !strcmp(x, ".dll") || !strcmp(x, ".cleo") || !strcmp(x, ".exe")) {
        mvc_log_warn("[ModLoader] %s: %s is Windows %s - it can't run on macVC (yet)", m->name, rel,
                     !strcmp(x, ".cleo") ? "CLEO plugin code" : "code");
        m->unsupported++;
        return;
    }
    // 3. the same path as a game file, or a trailing part of it ("stuff/data/handling.cfg")
    for (const char *s = r; s != base; s = strchr(s, '/') + 1)
        if (game_has_path(s)) {
            add_entry(&g_files, &g_nfiles, &g_capfiles, s, abs, mod);
            m->files++;
            return;
        }
    // 4. just the name, if only one game file has it
    GameFile *g = game_by_name(base);
    if (g && g->count == 1) {
        add_entry(&g_files, &g_nfiles, &g_capfiles, g->path, abs, mod);
        m->files++;
        return;
    }
    if (g && g->count > 1) {
        // several game files have this name: take the one whose folders best match the mod's
        const char *best = NULL;
        int best_score = 0, tie = 0;
        for (int i = 0; i < g_ngamepaths; i++) {
            const char *gb = strrchr(g_gamepaths[i], '/');
            if (strcmp(gb ? gb + 1 : g_gamepaths[i], base)) continue;
            int score = 0;
            for (const char *s = r; s < base; s = strchr(s, '/') + 1) { // each folder of the mod path
                char part[128];
                snprintf(part, sizeof part, "/%.*s/", (int)(strchr(s, '/') - s), s);
                char gp[PATH_MAX];
                snprintf(gp, sizeof gp, "/%s", g_gamepaths[i]);
                if (strstr(gp, part)) score++;
            }
            if (score > best_score) { best = g_gamepaths[i]; best_score = score; tie = 0; }
            else if (score == best_score && score) tie = 1;
        }
        if (best && !tie) {
            add_entry(&g_files, &g_nfiles, &g_capfiles, best, abs, mod);
            m->files++;
            return;
        }
        mvc_log_warn("[ModLoader] %s: more than one game file is called %s - put it in the same folder as the one "
                     "it replaces (e.g. %s)", m->name, base, g->path);
        m->unused++;
        return;
    }
    const char *gd = from_game_dir(r);
    // 5. models and textures go into the archive
    if (is_img_type(base) && !(gd && !strncmp(gd, "data/", 5))) {
        add_entry(&g_img, &g_nimg, &g_capimg, base, abs, mod);
        m->img++;
        return;
    }
    // 6. a new file in a game folder (a new map's .ide/.ipl/.col): served at that path
    if (gd && !is_doc(base)) {
        g_new_file = 1; // not a replacement of a game file: nothing to merge
        add_entry(&g_files, &g_nfiles, &g_capfiles, gd, abs, mod);
        g_new_file = 0;
        m->files++;
        return;
    }
    if (is_doc(base)) return; // readmes (scanned for data lines later), screenshots
    mvc_log("[ModLoader] %s: %s doesn't match any game file - not used", m->name, rel);
    m->unused++;
}

static void walk_mod(int mod, const char *abs, const char *rel, int depth) {
    DIR *d = opendir(abs);
    if (!d || depth > 8) { if (d) closedir(d); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (skip_name(e->d_name)) continue;
        char a[PATH_MAX], r[PATH_MAX];
        snprintf(a, sizeof a, "%s/%s", abs, e->d_name);
        snprintf(r, sizeof r, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        struct stat st;
        if (stat(a, &st)) continue;
        if (S_ISDIR(st.st_mode)) walk_mod(mod, a, r, depth + 1);
        else if (S_ISREG(st.st_mode)) add_mod_file(mod, a, r);
    }
    closedir(d);
}

static int cmp_mod(const void *a, const void *b) { return strcasecmp(((const Mod *)a)->name, ((const Mod *)b)->name); }

// Windows modloader.ini
// GameData/modloader/modloader.ini, as Windows Mod Loader writes it: the profile
// named in [Folder.Config] Profile= has Config (IgnoreAllMods, ExcludeAllMods),
// priority (Mod=0-100), IgnoreFiles / IgnoreMods / IncludeMods (wildcards) and
// ExclusiveMods (mods loaded only by that profile). macVCLoader.ini [Mods] wins.
#define MAXPAT 128
typedef struct { char pat[128]; int value; } Pat;
static struct {
    int ignore_all, exclude_all;
    Pat prio[MAXPAT]; int nprio;
    Pat ign_files[MAXPAT]; int nign_files;
    Pat ign_mods[MAXPAT]; int nign_mods;
    Pat inc_mods[MAXPAT]; int ninc_mods;
    Pat excl_other[MAXPAT]; int nexcl_other; // exclusive to a different profile
    Pat excl_mine[MAXPAT]; int nexcl_mine;
} g_ml;

static void ml_trim(char *t) {
    char *c = strchr(t, ';');
    if (c) *c = 0;
    char *e = t + strlen(t);
    while (e > t && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    char *b = t;
    while (*b == ' ' || *b == '\t') b++;
    if (b != t) memmove(t, b, strlen(b) + 1);
}

static void ml_add(Pat *arr, int *n, const char *pat, int value) {
    if (*n >= MAXPAT || !*pat) return;
    snprintf(arr[*n].pat, sizeof arr[0].pat, "%s", pat);
    arr[(*n)++].value = value;
}

static void read_ml_ini(const char *root) {
    char path[PATH_MAX], line[512], sec[160] = "", profile[64] = "Default";
    snprintf(path, sizeof path, "%s/modloader.ini", root);
    for (int pass = 0; pass < 2; pass++) { // pass 0 finds the profile, pass 1 reads it
        FILE *f = fopen(path, "r");
        if (!f) return;
        while (fgets(line, sizeof line, f)) {
            ml_trim(line);
            if (!line[0]) continue;
            if (line[0] == '[' && !strchr(line, '=')) {
                char *e = strchr(line, ']');
                if (e) *e = 0;
                snprintf(sec, sizeof sec, "%s", line + 1);
                continue;
            }
            char key[256], val[256] = "";
            char *eq = strchr(line, '=');
            if (eq) { *eq = 0; snprintf(key, sizeof key, "%s", line); snprintf(val, sizeof val, "%s", eq + 1); ml_trim(key); ml_trim(val); }
            else snprintf(key, sizeof key, "%s", line);
            if (pass == 0) {
                if (!strcasecmp(sec, "Folder.Config") && !strcasecmp(key, "Profile") && val[0]) snprintf(profile, sizeof profile, "%s", val);
                continue;
            }
            char want[160];
            if (!strncasecmp(sec, "Profiles.", 9) && strlen(sec) > 21 && !strcasecmp(sec + strlen(sec) - 14, ".ExclusiveMods")) {
                snprintf(want, sizeof want, "Profiles.%s.ExclusiveMods", profile);
                if (!strcasecmp(sec, want)) ml_add(g_ml.excl_mine, &g_ml.nexcl_mine, key, 0);
                else ml_add(g_ml.excl_other, &g_ml.nexcl_other, key, 0);
                continue;
            }
            snprintf(want, sizeof want, "Profiles.%s.", profile);
            if (strncasecmp(sec, want, strlen(want))) continue;
            const char *part = sec + strlen(want);
            int on = !strcasecmp(val, "true") || !strcmp(val, "1");
            if (!strcasecmp(part, "Config")) {
                if (!strcasecmp(key, "IgnoreAllMods")) g_ml.ignore_all = on;
                if (!strcasecmp(key, "ExcludeAllMods")) g_ml.exclude_all = on;
            } else if (!strcasecmp(part, "Priority") && eq) ml_add(g_ml.prio, &g_ml.nprio, key, atoi(val));
            else if (!strcasecmp(part, "IgnoreFiles")) ml_add(g_ml.ign_files, &g_ml.nign_files, key, 0);
            else if (!strcasecmp(part, "IgnoreMods")) ml_add(g_ml.ign_mods, &g_ml.nign_mods, key, 0);
            else if (!strcasecmp(part, "IncludeMods")) ml_add(g_ml.inc_mods, &g_ml.ninc_mods, key, 0);
        }
        fclose(f);
    }
    mvc_log("[ModLoader] read modloader.ini (profile %s)", profile);
}

static int ml_match(const Pat *arr, int n, const char *s) {
    for (int i = 0; i < n; i++) if (!fnmatch(arr[i].pat, s, FNM_CASEFOLD)) return i + 1;
    return 0;
}

// a mod file ignored by IgnoreFiles ("cheetah.dff", "*.dff", "path_in_mod/*.dff")
static int ml_ignored_file(const char *rel) {
    if (!g_ml.nign_files) return 0;
    const char *b = strrchr(rel, '/');
    return ml_match(g_ml.ign_files, g_ml.nign_files, b ? b + 1 : rel) || ml_match(g_ml.ign_files, g_ml.nign_files, rel);
}

// enabled / priority for a mod from modloader.ini (macVCLoader.ini is applied after)
static void ml_apply(Mod *m) {
    if (g_ml.ignore_all) m->enabled = 0;
    if (g_ml.exclude_all && !ml_match(g_ml.inc_mods, g_ml.ninc_mods, m->name) && !ml_match(g_ml.excl_mine, g_ml.nexcl_mine, m->name))
        m->enabled = 0;
    if (ml_match(g_ml.ign_mods, g_ml.nign_mods, m->name)) m->enabled = 0;
    if (ml_match(g_ml.excl_other, g_ml.nexcl_other, m->name) && !ml_match(g_ml.excl_mine, g_ml.nexcl_mine, m->name)) m->enabled = 0;
    int p = ml_match(g_ml.prio, g_ml.nprio, m->name);
    if (p) {
        int v = g_ml.prio[p - 1].value;
        if (v <= 0) m->enabled = 0;
        else m->priority = v > 100 ? 100 : v;
    }
}

static void scan_mods(const char *root) {
    DIR *d = opendir(root);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && g_nmods < MAX_MODS) {
        if (skip_name(e->d_name)) continue;
        char a[PATH_MAX];
        snprintf(a, sizeof a, "%s/%s", root, e->d_name);
        struct stat st;
        if (stat(a, &st) || !S_ISDIR(st.st_mode)) continue;
        Mod *m = &g_mods[g_nmods++];
        memset(m, 0, sizeof *m);
        snprintf(m->name, sizeof m->name, "%s", e->d_name);
        m->enabled = 1;
        m->priority = 50;
        ml_apply(m);
        for (int i = 0; i < g_ini_n; i++)
            if (!strcasecmp(g_ini[i].name, m->name)) {
                if (g_ini[i].value <= 0) m->enabled = 0;
                else { m->enabled = 1; m->priority = g_ini[i].value > 100 ? 100 : g_ini[i].value; }
            }
    }
    closedir(d);
    qsort(g_mods, (size_t)g_nmods, sizeof *g_mods, cmp_mod);
    for (int i = 0; i < g_nmods; i++) {
        if (!g_mods[i].enabled) { mvc_log("[ModLoader] %s is turned off (macVCLoader.ini [Mods] or modloader.ini)", g_mods[i].name); continue; }
        char a[PATH_MAX];
        snprintf(a, sizeof a, "%s/%s", root, g_mods[i].name);
        walk_mod(i, a, "", 0);
    }
}

// data merging
// the real (case-correct) path of a game file from its lowercase relative path
static int game_real_path(const char *rel, char *out, size_t n) {
    char cur[PATH_MAX], part[256];
    snprintf(cur, sizeof cur, "%s", g_game_dir);
    for (const char *s = rel; *s;) {
        const char *e = strchr(s, '/');
        size_t L = e ? (size_t)(e - s) : strlen(s);
        snprintf(part, sizeof part, "%.*s", (int)L, s);
        DIR *d = opendir(cur);
        if (!d) return -1;
        struct dirent *de;
        int found = 0;
        while ((de = readdir(d)))
            if (!strcasecmp(de->d_name, part)) {
                strlcat(cur, "/", sizeof cur);
                strlcat(cur, de->d_name, sizeof cur);
                found = 1;
                break;
            }
        closedir(d);
        if (!found) return -1;
        s += L + (e ? 1 : 0);
    }
    snprintf(out, n, "%s", cur);
    return 0;
}

static Entry *find_file(const char *key) {
    for (int i = 0; i < g_nfiles; i++) if (!strcmp(g_files[i].key, key)) return &g_files[i];
    return NULL;
}

static int mergeable(const char *key) {
    static const char *names[] = {"gta_vc.dat", "handling.cfg", "carcols.dat", "weapon.dat", "object.dat", "pedstats.dat",
                                  "ped.dat", "pedgrp.dat", "surface.dat", "particle.cfg", "fistfite.dat", "water.dat", NULL};
    const char *b = strrchr(key, '/');
    b = b ? b + 1 : key;
    // .ipl too: Vice City's map files are lists of independent placements (LODs are
    // matched by name, not by line), so two map fixes can both apply. Windows Mod
    // Loader only replaces them, which silently dropped one mod's changes.
    if (!strcmp(ext_of(b), ".ide") || !strcmp(ext_of(b), ".ipl") || !strcmp(ext_of(b), ".col")) return 1;
    for (int i = 0; names[i]; i++) if (!strcmp(b, names[i])) return 1;
    return 0;
}

static void cache_name(const char *key, char *out, size_t n) {
    char flat[PATH_MAX];
    snprintf(flat, sizeof flat, "%s", key);
    for (char *c = flat; *c; c++) if (*c == '/') *c = '_';
    snprintf(out, n, "%s/.cache/merged/%s", g_root, flat);
}

// several mods replace the same data file: merge them against the original
static void merge_all(void) {
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/.cache/merged", g_root);
    mkdir(dir, 0755);
    for (int i = 0; i < g_nvers; i++) {
        const char *key = g_vers[i].key;
        int first = 1, n = 0;
        for (int j = 0; j < i; j++) if (!strcmp(g_vers[j].key, key)) first = 0;
        if (!first) continue;
        for (int j = i; j < g_nvers; j++) if (!strcmp(g_vers[j].key, key)) n++;
        if (n < 2 || !mergeable(key)) continue;
        const char **srcs = calloc((size_t)n, sizeof *srcs), **names = calloc((size_t)n, sizeof *names);
        int *prio = calloc((size_t)n, sizeof *prio), k = 0;
        for (int j = i; j < g_nvers; j++)
            if (!strcmp(g_vers[j].key, key)) {
                srcs[k] = g_vers[j].src;
                names[k] = g_mods[g_vers[j].mod].name;
                prio[k++] = g_mods[g_vers[j].mod].priority;
            }
        char base[PATH_MAX], out[PATH_MAX];
        cache_name(key, out, sizeof out);
        int rc = game_real_path(key, base, sizeof base) ? -1
                 : !strcmp(ext_of(key), ".col") ? merge_col(base, (const char *const *)srcs, prio, (const char *const *)names, n, out, key)
                 : merge_text(base, (const char *const *)srcs, prio, (const char *const *)names, n, out, key);
        Entry *e = find_file(key);
        if (rc >= 0 && e) {
            free(e->src);
            e->src = strdup(out);
            mvc_log("[ModLoader] %s: merged the changes of %d mods%s", key, n, rc ? " (some overlapped - type conflicts)" : "");
            // the priority-pick conflict notes for this file are replaced by the merge
            int w = 0;
            for (int c = 0; c < g_nconflicts; c++)
                if (strncmp(g_conflicts[c], key, strlen(key)) || g_conflicts[c][strlen(key)] != ':')
                    memmove(g_conflicts[w++], g_conflicts[c], sizeof g_conflicts[0]);
            g_nconflicts = w;
            if (rc && g_nconflicts < 64)
                snprintf(g_conflicts[g_nconflicts++], sizeof g_conflicts[0], "%s: %d change(s) overlapped while merging - "
                         "see the log", key, rc);
        } else mvc_log_warn("[ModLoader] %s: couldn't merge (too big?) - using the highest priority mod's", key);
        free(srcs); free(names); free(prio);
    }
}

// readme data lines
// like Windows Mod Loader, data lines found in a mod's readme (.txt) files are
// applied: handling.cfg, weapon.dat, carcols.dat car colours, and vehicle / ped
// lines of default.ide. each readme's lines go into a copy of the original file,
// which is then merged with every other mod's version as usual.
typedef struct { char **l; int n, cap; } Lines;
static void lines_add(Lines *t, int at, const char *s) {
    if (t->n == t->cap) { t->cap = t->cap ? t->cap * 2 : 512; t->l = realloc(t->l, (size_t)t->cap * sizeof *t->l); }
    if (at < 0 || at > t->n) at = t->n;
    memmove(&t->l[at + 1], &t->l[at], (size_t)(t->n - at) * sizeof *t->l);
    t->l[at] = strdup(s);
    t->n++;
}
static int lines_read(const char *path, Lines *t) {
    memset(t, 0, sizeof *t);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char b[1024];
    while (fgets(b, sizeof b, f)) { b[strcspn(b, "\r\n")] = 0; lines_add(t, -1, b); }
    fclose(f);
    return 0;
}
static void lines_free(Lines *t) { for (int i = 0; i < t->n; i++) free(t->l[i]); free(t->l); memset(t, 0, sizeof *t); }

// splits on whitespace (and commas when commas=1) into up to 48 tokens
static int tokens(const char *line, char tok[][64], int commas) {
    int n = 0;
    for (const char *s = line; *s && n < 48;) {
        while (*s == ' ' || *s == '\t' || (commas && *s == ',')) s++;
        if (!*s || *s == '#' || *s == ';') break;
        int k = 0;
        while (*s && *s != ' ' && *s != '\t' && !(commas && *s == ',')) { if (k < 63) tok[n][k++] = *s; s++; }
        tok[n++][k] = 0;
    }
    return n;
}
static int numeric(const char *t) {
    char *e;
    strtod(t, &e);
    return e != t && !*e;
}

enum { RD_HANDLING, RD_WEAPON, RD_IDE, RD_CARCOLS, RD_N };
static const char *RD_FILE[RD_N] = {"data/handling.cfg", "data/weapon.dat", "data/default.ide", "data/carcols.dat"};

// what a readme line is, and its key (vehicle name, weapon, model id, model name)
static int readme_kind(const char *line, char *key, size_t kn) {
    char t[48][64];
    int n = tokens(line, t, 0);
    if (n == 33 && isupper((unsigned char)t[0][0])) { // a car's handling line
        int num = 0;
        for (int i = 1; i < n; i++) num += numeric(t[i]);
        if (num >= 28) { snprintf(key, kn, "%s", t[0]); return RD_HANDLING; }
    }
    if ((n == 17 && t[0][0] == '!') || (n == 16 && t[0][0] == '%') || (n == 20 && t[0][0] == '$')) {
        if (!t[0][1] && isupper((unsigned char)t[1][0])) { snprintf(key, kn, "%s %s", t[0], t[1]); return RD_HANDLING; }
    }
    if (n == 26 && isalpha((unsigned char)t[0][0])) { // weapon.dat
        int num = 0;
        for (int i = 2; i < n; i++) num += numeric(t[i]);
        if (num >= 20) { snprintf(key, kn, "%s", t[0]); return RD_WEAPON; }
    }
    n = tokens(line, t, 1);
    static const char *vtypes[] = {"car", "bike", "boat", "plane", "heli", "train", NULL};
    if (n >= 10 && numeric(t[0])) {
        for (int i = 0; vtypes[i]; i++)
            if (!strcasecmp(t[3], vtypes[i])) { snprintf(key, kn, "cars %s", t[0]); return RD_IDE; }
        if (!strncasecmp(t[4], "STAT_", 5)) { snprintf(key, kn, "peds %s", t[0]); return RD_IDE; }
    }
    if (n >= 3 && n % 2 == 1 && isalpha((unsigned char)t[0][0])) { // carcols "name, c1,c2, c3,c4..."
        for (int i = 1; i < n; i++) if (!numeric(t[i])) return -1;
        snprintf(key, kn, "%s", t[0]);
        return RD_CARCOLS;
    }
    return -1;
}

// the key of an existing line in the target file, in the same form readme_kind gives
static int line_key(int kind, const char *line, const char *section, char *key, size_t kn) {
    char t[48][64];
    int n;
    switch (kind) {
    case RD_HANDLING:
        n = tokens(line, t, 0);
        if (n >= 2 && (t[0][0] == '!' || t[0][0] == '%' || t[0][0] == '$') && !t[0][1]) { snprintf(key, kn, "%s %s", t[0], t[1]); return 1; }
        if (n == 33) { snprintf(key, kn, "%s", t[0]); return 1; }
        return 0;
    case RD_WEAPON:
        n = tokens(line, t, 0);
        if (n == 26) { snprintf(key, kn, "%s", t[0]); return 1; }
        return 0;
    case RD_IDE:
        n = tokens(line, t, 1);
        if (n >= 2 && numeric(t[0]) && (!strcmp(section, "cars") || !strcmp(section, "peds"))) { snprintf(key, kn, "%s %s", section, t[0]); return 1; }
        return 0;
    default:
        n = tokens(line, t, 1);
        if (n >= 3 && !strcmp(section, "car") && isalpha((unsigned char)t[0][0])) { snprintf(key, kn, "%s", t[0]); return 1; }
        return 0;
    }
}

static void section_of(int kind, const char *line, char *sec, size_t n) {
    char t[48][64]; // tokens() writes up to 48
    if (kind != RD_IDE && kind != RD_CARCOLS) return;
    if (tokens(line, t, 1) == 1 && isalpha((unsigned char)t[0][0])) {
        if (!strcasecmp(t[0], "end")) sec[0] = 0;
        else if (strlen(t[0]) <= 5) { snprintf(sec, n, "%s", t[0]); lower(sec); }
    }
}

// a vehicle model name defined in default.ide's cars section (the original, or this
// mod's readme lines): colour lines are only added for real vehicles
static Lines g_rd_base_ide, *g_rd_mod_ide;
static int ide_has_car(const Lines *t, const char *name) {
    if (!t) return 0;
    char sec[16] = "", tk[48][64];
    for (int i = 0; i < t->n; i++) {
        section_of(RD_IDE, t->l[i], sec, sizeof sec);
        if (!strcmp(sec, "cars") && tokens(t->l[i], tk, 1) >= 4 && !strcasecmp(tk[1], name)) return 1;
    }
    return 0;
}

// applies one readme line to the file: replaces the line with the same key, or adds it
static int apply_line(int kind, Lines *t, const char *line, const char *key) {
    char sec[16] = "", k[160];
    int last_same_class = -1, sec_end = -1;
    const char *want_sec = kind == RD_IDE ? (strncmp(key, "cars", 4) ? "peds" : "cars") : kind == RD_CARCOLS ? "car" : "";
    char prefix = kind == RD_HANDLING && (key[0] == '!' || key[0] == '%' || key[0] == '$') ? key[0] : 0;
    for (int i = 0; i < t->n; i++) {
        char before[16];
        snprintf(before, sizeof before, "%s", sec);
        section_of(kind, t->l[i], sec, sizeof sec);
        if (before[0] && !sec[0] && !strcmp(before, want_sec)) sec_end = i; // the "end" of the wanted section
        if (!line_key(kind, t->l[i], sec, k, sizeof k)) continue;
        if (!strcasecmp(k, key)) { free(t->l[i]); t->l[i] = strdup(line); return 1; }
        if (kind == RD_HANDLING && ((prefix && k[0] == prefix) || (!prefix && k[0] != '!' && k[0] != '%' && k[0] != '$')))
            last_same_class = i;
    }
    if (kind == RD_WEAPON) return 0; // only the game's weapons can be changed
    if (kind == RD_CARCOLS && !ide_has_car(&g_rd_base_ide, key) && !ide_has_car(g_rd_mod_ide, key)) return 0;
    int at = kind == RD_HANDLING ? (last_same_class >= 0 ? last_same_class + 1 : -1) : sec_end;
    if ((kind == RD_IDE || kind == RD_CARCOLS) && sec_end < 0) return 0; // no such section
    lines_add(t, at, line);
    return 1;
}

static void apply_readmes(void) {
    char dir[PATH_MAX], ide[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/.cache/readme", g_root);
    mkdir(dir, 0755);
    if (!game_real_path("data/default.ide", ide, sizeof ide)) lines_read(ide, &g_rd_base_ide);
    for (int m = 0; m < g_nmods; m++) {
        if (!g_mods[m].enabled) continue;
        Lines out[RD_N] = {{0}};
        int got[RD_N] = {0};
        g_rd_mod_ide = &out[RD_IDE];
        for (int i = 0; i < g_npool; i++) {
            if (g_pool[i].mod != m || strcmp(ext_of(g_pool[i].base), ".txt")) continue;
            FILE *f = fopen(g_pool[i].src, "r");
            if (!f) continue;
            char b[1024], key[160];
            while (fgets(b, sizeof b, f)) {
                b[strcspn(b, "\r\n")] = 0;
                char *s = b;
                while (*s == ' ' || *s == '\t') s++;
                int kind = readme_kind(s, key, sizeof key);
                if (kind < 0) continue;
                if (!got[kind]) {
                    char base[PATH_MAX];
                    if (game_real_path(RD_FILE[kind], base, sizeof base) || lines_read(base, &out[kind])) continue;
                    got[kind] = 1;
                }
                int before = out[kind].n;
                if (!apply_line(kind, &out[kind], s, key)) continue;
                got[kind] = 2;
                mvc_log("[ModLoader] %s: %s line for %s from %s%s", g_mods[m].name, RD_FILE[kind] + 5, key, g_pool[i].base,
                        out[kind].n > before ? " (added)" : "");
            }
            fclose(f);
        }
        for (int k = 0; k < RD_N; k++) {
            if (got[k] == 2) {
                char path[PATH_MAX];
                snprintf(path, sizeof path, "%s/%d_%s", dir, m, RD_FILE[k] + 5);
                FILE *o = fopen(path, "w");
                if (o) {
                    for (int i = 0; i < out[k].n; i++) fprintf(o, "%s\r\n", out[k].l[i]);
                    fclose(o);
                    add_entry(&g_files, &g_nfiles, &g_capfiles, RD_FILE[k], path, m); // a version, merged as usual
                }
            }
            if (got[k]) lines_free(&out[k]);
        }
    }
    g_rd_mod_ide = NULL;
    lines_free(&g_rd_base_ide);
}

// a gta_vc.dat path ("DATA\MAPS\X\X.IPL") exists in the game or a mod; looks for
// a new map file by name anywhere in the mods, like Windows Mod Loader does
static int resolve_data_path(const char *path, int prefer_mod) {
    char rel[PATH_MAX];
    snprintf(rel, sizeof rel, "%s", path);
    for (char *c = rel; *c; c++) *c = *c == '\\' ? '/' : (char)tolower((unsigned char)*c);
    if (game_has_path(rel) || find_file(rel)) return 1;
    const char *b = strrchr(rel, '/');
    b = b ? b + 1 : rel;
    PoolFile *best = NULL;
    for (int i = 0; i < g_npool; i++)
        if (!strcmp(g_pool[i].base, b) && (!best || g_pool[i].mod == prefer_mod)) best = &g_pool[i];
    if (!best) return 0;
    g_new_file = 1;
    add_entry(&g_files, &g_nfiles, &g_capfiles, rel, best->src, best->mod);
    g_new_file = 0;
    return 1;
}

// the path a gta_vc.dat line loads, or NULL ("COLFILE 0 path", "IDE path"...)
static const char *dat_path(const char *line, char *cmd, size_t n) {
    while (isspace((unsigned char)*line)) line++;
    size_t L = strcspn(line, " \t");
    snprintf(cmd, n, "%.*s", (int)L, line);
    static const char *withpath[] = {"IDE", "IPL", "COLFILE", "TEXDICTION", "MODELFILE", "HIERFILE", "IMG", "CDIMAGE", NULL};
    int ok = 0;
    for (int i = 0; withpath[i]; i++) if (!strcasecmp(cmd, withpath[i])) ok = 1;
    if (!ok) return NULL;
    const char *p = line + L;
    while (isspace((unsigned char)*p)) p++;
    if (!strcasecmp(cmd, "COLFILE")) { p += strcspn(p, " \t"); while (isspace((unsigned char)*p)) p++; }
    return *p ? p : NULL;
}

// the final gta_vc.dat: the (merged) mod version or the original, plus data
// lines from readmes, minus lines whose file exists nowhere.
static void build_gta_dat(void) {
    char src[PATH_MAX];
    Entry *e = find_file("data/gta_vc.dat");
    if (e) snprintf(src, sizeof src, "%s", e->src);
    else if (game_real_path("data/gta_vc.dat", src, sizeof src)) return;
    int src_mod = e ? e->mod : -1;
    FILE *f = fopen(src, "r");
    if (!f) return;
    char (*lines)[300] = calloc(2048, sizeof *lines);
    int n = 0, changed = 0;
    char buf[300], cmd[32];
    while (n < 2000 && fgets(buf, sizeof buf, f)) {
        buf[strcspn(buf, "\r\n")] = 0;
        const char *p = dat_path(buf, cmd, sizeof cmd);
        char path[256];
        if (p) {
            snprintf(path, sizeof path, "%.*s", (int)strcspn(p, " \t"), p);
            if (!resolve_data_path(path, src_mod)) {
                mvc_log_warn("[ModLoader] gta_vc.dat: %s isn't in the game or any mod - line \"%s\" left out", path, buf);
                changed = 1;
                continue;
            }
        }
        snprintf(lines[n++], sizeof lines[0], "%s", buf);
    }
    fclose(f);
    // data lines in readmes ("IDE data\maps\neons\neons.ide")
    for (int i = 0; i < g_npool; i++) {
        if (strcmp(ext_of(g_pool[i].base), ".txt")) continue;
        FILE *r = fopen(g_pool[i].src, "r");
        if (!r) continue;
        while (fgets(buf, sizeof buf, r) && n < 2040) {
            buf[strcspn(buf, "\r\n")] = 0;
            const char *p = dat_path(buf, cmd, sizeof cmd);
            if (!p || (strcasecmp(cmd, "IDE") && strcasecmp(cmd, "IPL") && strcasecmp(cmd, "COLFILE"))) continue;
            char path[256], line[300];
            snprintf(path, sizeof path, "%.*s", (int)strcspn(p, " \t"), p);
            const char *t = buf;
            while (isspace((unsigned char)*t)) t++;
            snprintf(line, sizeof line, "%s", t);
            int dup = 0, last = -1;
            for (int k = 0; k < n; k++) {
                char c2[32];
                const char *p2 = dat_path(lines[k], c2, sizeof c2);
                if (p2 && !strcasecmp(c2, cmd)) {
                    last = k;
                    char a[256];
                    snprintf(a, sizeof a, "%.*s", (int)strcspn(p2, " \t"), p2);
                    for (char *c = a; *c; c++) if (*c == '\\') *c = '/';
                    char bb[256];
                    snprintf(bb, sizeof bb, "%s", path);
                    for (char *c = bb; *c; c++) if (*c == '\\') *c = '/';
                    if (!strcasecmp(a, bb)) dup = 1;
                }
            }
            if (dup || !resolve_data_path(path, g_pool[i].mod)) continue;
            int at = last >= 0 ? last + 1 : n; // after the last line of the same kind
            memmove(&lines[at + 1], &lines[at], (size_t)(n - at) * sizeof *lines);
            snprintf(lines[at], sizeof lines[0], "%s", line);
            n++;
            changed = 1;
            mvc_log("[ModLoader] gta_vc.dat: added \"%s\" from %s's readme", line, g_mods[g_pool[i].mod].name);
        }
        fclose(r);
    }
    if (changed) {
        char out[PATH_MAX];
        snprintf(out, sizeof out, "%s/.cache/merged/data_gta_vc.dat", g_root);
        FILE *o = fopen(out, "w");
        if (o) {
            for (int k = 0; k < n; k++) fprintf(o, "%s\r\n", lines[k]);
            fclose(o);
            if (e) { free(e->src); e->src = strdup(out); }
            else {
                g_new_file = 1;
                add_entry(&g_files, &g_nfiles, &g_capfiles, "data/gta_vc.dat", out, g_pool[0].mod);
                g_new_file = 0;
            }
        }
    }
    free(lines);
}

// texture folders
// reads an entry of the game's own gta3.img (for a name.txd/ folder on an archive texture)
static uint8_t *read_from_gta3(const char *name, size_t *len) {
    char dir[PATH_MAX], img[PATH_MAX];
    *len = 0;
    if (game_real_path("models/gta3.dir", dir, sizeof dir) || game_real_path("models/gta3.img", img, sizeof img)) return NULL;
    FILE *f = fopen(dir, "rb");
    if (!f) return NULL;
    struct { uint32_t off, size; char name[24]; } e;
    uint8_t *data = NULL;
    while (fread(&e, sizeof e, 1, f) == 1) {
        e.name[23] = 0;
        if (strcasecmp(e.name, name)) continue;
        FILE *g = fopen(img, "rb");
        if (g) {
            data = malloc((size_t)e.size * SECTOR);
            fseeko(g, (off_t)e.off * SECTOR, SEEK_SET);
            *len = fread(data, 1, (size_t)e.size * SECTOR, g);
            fclose(g);
        }
        break;
    }
    fclose(f);
    return data;
}

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    *len = 0;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = n > 0 ? malloc((size_t)n) : NULL;
    if (d) *len = fread(d, 1, (size_t)n, f);
    fclose(f);
    return d;
}

static uint32_t fnv32(uint32_t h, const void *p, size_t n);

// texture packs
// index of which texture dictionaries hold which texture names: every loose game
// .txd and every .txd inside gta3.img. Reading them all takes a few seconds, so
// the index is cached in modloader/.cache/textures.idx until the game files change.
static int cmp_texloc(const void *a, const void *b) {
    const TexLoc *x = a, *y = b;
    int c = strcmp(x->tex, y->tex);
    return c ? c : strcmp(x->target, y->target);
}

static void texidx_add(const char *tex, const char *target) {
    g_texidx = grow(g_texidx, &g_captexidx, g_ntexidx + 1, sizeof *g_texidx);
    snprintf(g_texidx[g_ntexidx].tex, sizeof g_texidx[0].tex, "%s", tex);
    snprintf(g_texidx[g_ntexidx++].target, sizeof g_texidx[0].target, "%s", target);
}

static void texidx_add_txd(const uint8_t *d, size_t len, const char *target) {
    static char names[1024][32];
    int n = txd_texture_names(d, len, names, 1024);
    for (int i = 0; i < n; i++) texidx_add(names[i], target);
}

static void texidx_build(void) {
    if (g_texidx_done) return;
    g_texidx_done = 1;
    char dir[PATH_MAX], img[PATH_MAX], cache[PATH_MAX], sig[512];
    struct stat sd = {0}, si = {0};
    int have_img = !game_real_path("models/gta3.dir", dir, sizeof dir) && !game_real_path("models/gta3.img", img, sizeof img) &&
                   !stat(dir, &sd) && !stat(img, &si);
    long long loose = 0;
    int nloose = 0;
    for (int i = 0; i < g_ngamepaths; i++) {
        size_t L = strlen(g_gamepaths[i]);
        if (L < 4 || strcmp(g_gamepaths[i] + L - 4, ".txd")) continue;
        char real[PATH_MAX];
        struct stat st;
        if (!game_real_path(g_gamepaths[i], real, sizeof real) && !stat(real, &st)) { loose += st.st_size + st.st_mtime; nloose++; }
    }
    snprintf(sig, sizeof sig, "texidx 1 %lld %ld %lld %ld %d %lld", (long long)sd.st_size, (long)sd.st_mtime,
             (long long)si.st_size, (long)si.st_mtime, nloose, loose);
    snprintf(cache, sizeof cache, "%s/.cache/textures.idx", g_root);
    FILE *f = fopen(cache, "r");
    if (f) {
        char line[256];
        int ok = fgets(line, sizeof line, f) && !strncmp(line, sig, strlen(sig)) && line[strlen(sig)] == '\n';
        while (ok && fgets(line, sizeof line, f)) {
            char tex[32], target[64];
            if (sscanf(line, "%31s %63s", tex, target) == 2) texidx_add(tex, target);
        }
        fclose(f);
        if (ok && g_ntexidx) { qsort(g_texidx, (size_t)g_ntexidx, sizeof *g_texidx, cmp_texloc); return; }
        g_ntexidx = 0;
    }
    mvc_log("[ModLoader] indexing the game's textures for texture packs (once)...");
    for (int i = 0; i < g_ngamepaths; i++) {
        size_t L = strlen(g_gamepaths[i]);
        if (L < 4 || strcmp(g_gamepaths[i] + L - 4, ".txd")) continue;
        char real[PATH_MAX];
        size_t len;
        uint8_t *d = game_real_path(g_gamepaths[i], real, sizeof real) ? NULL : slurp(real, &len);
        if (d) texidx_add_txd(d, len, g_gamepaths[i]);
        free(d);
    }
    if (have_img) {
        FILE *fd = fopen(dir, "rb"), *fi = fopen(img, "rb");
        struct { uint32_t off, size; char name[24]; } e;
        size_t cap = 0;
        uint8_t *buf = NULL;
        while (fd && fi && fread(&e, sizeof e, 1, fd) == 1) {
            e.name[23] = 0;
            size_t L = strlen(e.name);
            if (L < 5 || strcasecmp(e.name + L - 4, ".txd") || !e.size || e.size > 16384) continue; // up to 32 MB
            size_t want = (size_t)e.size * SECTOR;
            if (want > cap) { free(buf); buf = malloc(cap = want); }
            if (!buf) break;
            if (fseeko(fi, (off_t)e.off * SECTOR, SEEK_SET)) continue;
            size_t got = fread(buf, 1, want, fi);
            char target[64];
            snprintf(target, sizeof target, "img:%s", e.name);
            lower(target);
            texidx_add_txd(buf, got, target);
        }
        free(buf);
        if (fd) fclose(fd);
        if (fi) fclose(fi);
    }
    qsort(g_texidx, (size_t)g_ntexidx, sizeof *g_texidx, cmp_texloc);
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s.new", cache);
    f = fopen(tmp, "w");
    if (f) {
        fprintf(f, "%s\n", sig);
        for (int i = 0; i < g_ntexidx; i++) fprintf(f, "%s %s\n", g_texidx[i].tex, g_texidx[i].target);
        if (fclose(f) || rename(tmp, cache)) unlink(tmp);
    }
    mvc_log("[ModLoader] texture index: %d texture(s) in the game's dictionaries", g_ntexidx);
}

// every texture-pack image becomes an image for each dictionary that has a texture of its name
static void apply_texture_packs(void) {
    if (!g_ntex) return;
    texidx_build();
    // mods' own archive dictionaries can hold textures the game's don't
    int base = g_ntexidx;
    for (int i = 0; i < g_nimg; i++) {
        size_t L = strlen(g_img[i].key), len;
        if (L < 5 || strcmp(g_img[i].key + L - 4, ".txd")) continue;
        uint8_t *d = slurp(g_img[i].src, &len);
        char target[64];
        snprintf(target, sizeof target, "img:%s", g_img[i].key);
        if (d) texidx_add_txd(d, len, target);
        free(d);
    }
    if (g_ntexidx > base) qsort(g_texidx, (size_t)g_ntexidx, sizeof *g_texidx, cmp_texloc);
    int used = 0, places = 0, unmatched = 0;
    for (int i = 0; i < g_ntex; i++) {
        TexImage *t = &g_tex[i];
        char key[32];
        snprintf(key, sizeof key, "%s", t->name);
        lower(key);
        size_t lo = 0, hi = (size_t)g_ntexidx; // first entry >= key
        while (lo < hi) { size_t mid = (lo + hi) / 2; if (strcmp(g_texidx[mid].tex, key) < 0) lo = mid + 1; else hi = mid; }
        int hits = 0;
        for (size_t k = lo; k < (size_t)g_ntexidx && !strcmp(g_texidx[k].tex, key); k++) {
            if (k > lo && !strcmp(g_texidx[k].target, g_texidx[k - 1].target)) continue;
            g_txdimg = grow(g_txdimg, &g_captxdimg, g_ntxdimg + 1, sizeof *g_txdimg);
            snprintf(g_txdimg[g_ntxdimg].target, sizeof g_txdimg[0].target, "%s", g_texidx[k].target);
            g_txdimg[g_ntxdimg].src = strdup(t->src);
            g_txdimg[g_ntxdimg++].mod = t->mod;
            hits++;
        }
        if (hits) { used++; places += hits; }
        else {
            unmatched++;
            g_mods[t->mod].unused++;
            if (unmatched <= 10) mvc_log("[ModLoader] %s: textures/%s matches no texture in the game - not used", g_mods[t->mod].name, t->name);
        }
    }
    if (unmatched > 10) mvc_log("[ModLoader] ... and %d more texture-pack image(s) that match nothing", unmatched - 10);
    mvc_log("[ModLoader] texture packs: %d image(s) replace %d texture(s) across the game's dictionaries", used, places);
}

// `texfind <name>`: which dictionaries hold a texture
static void cmd_texfind(int argc, const char **argv) {
    if (argc < 2) { mvc_log("usage: texfind <texture name>   (where a texture lives, for texture packs)"); return; }
    texidx_build();
    char key[32];
    snprintf(key, sizeof key, "%s", argv[1]);
    lower(key);
    int n = 0;
    for (int i = 0; i < g_ntexidx; i++)
        if (strstr(g_texidx[i].tex, key) && n++ < 40) mvc_log("  %-24s %s", g_texidx[i].tex, g_texidx[i].target);
    mvc_log("%d match(es)%s", n, n > 40 ? " (first 40 shown)" : "");
}

static void build_txd_folders(void) {
    if (!g_ntxdimg) return;
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/.cache/txd", g_root);
    mkdir(dir, 0755);
    char imgdir[PATH_MAX];
    snprintf(imgdir, sizeof imgdir, "%s/img", dir);
    mkdir(imgdir, 0755);
    for (int i = 0; i < g_ntxdimg; i++) {
        const char *target = g_txdimg[i].target;
        int first = 1;
        for (int j = 0; j < i; j++) if (!strcmp(g_txdimg[j].target, target)) first = 0;
        if (!first) continue;
        // images for this dictionary, highest priority first; a name already taken is skipped
        const char *srcs[256];
        int n = 0;
        for (int pass = 100; pass >= 0 && n < 256; pass--)
            for (int j = i; j < g_ntxdimg && n < 256; j++) {
                if (strcmp(g_txdimg[j].target, target) || g_mods[g_txdimg[j].mod].priority != pass) continue;
                const char *b = strrchr(g_txdimg[j].src, '/');
                char nm[64];
                snprintf(nm, sizeof nm, "%s", b ? b + 1 : g_txdimg[j].src);
                char *dot = strrchr(nm, '.');
                if (dot) *dot = 0;
                int dup = 0;
                for (int k = 0; k < n; k++) {
                    const char *b2 = strrchr(srcs[k], '/');
                    char n2[64];
                    snprintf(n2, sizeof n2, "%s", b2 ? b2 + 1 : srcs[k]);
                    char *d2 = strrchr(n2, '.');
                    if (d2) *d2 = 0;
                    if (!strcasecmp(n2, nm)) dup = 1;
                }
                if (!dup) srcs[n++] = g_txdimg[j].src;
            }
        int img = !strncmp(target, "img:", 4);
        const char *name = img ? target + 4 : (strrchr(target, '/') ? strrchr(target, '/') + 1 : target);
        // the dictionary the images go on top of: another mod's, the loose game file, or gta3.img's
        size_t blen = 0;
        uint8_t *base = NULL;
        Entry *e = NULL;
        if (img) {
            for (int k = 0; k < g_nimg; k++) if (!strcmp(g_img[k].key, name)) e = &g_img[k];
            base = e ? slurp(e->src, &blen) : read_from_gta3(name, &blen);
        } else {
            e = find_file(target);
            char real[PATH_MAX];
            if (e) base = slurp(e->src, &blen);
            else if (!game_real_path(target, real, sizeof real)) base = slurp(real, &blen);
        }
        char out[PATH_MAX], flat[256];
        snprintf(flat, sizeof flat, "%s", img ? target + 4 : target);
        for (char *c = flat; *c; c++) if (*c == '/') *c = '_';
        // archive entries are named after their file, so those keep the plain name, in their own folder
        snprintf(out, sizeof out, "%s/%s%s", dir, img ? "img/" : "", flat);
        // the same images on the same dictionary as last time: reuse the built file
        // (decoding a big texture pack's images on every launch would be slow)
        size_t scap = (size_t)n * (PATH_MAX + 48) + 128;
        char sigpath[PATH_MAX], *sig = malloc(scap);
        size_t sl = (size_t)snprintf(sig, scap, "txd 1 %d %zu %08x\n", g_cfg.texture_max, blen, base ? fnv32(0x811c9dc5, base, blen) : 0);
        for (int k = 0; k < n; k++) {
            struct stat st;
            if (stat(srcs[k], &st)) st.st_size = 0, st.st_mtime = 0;
            sl += (size_t)snprintf(sig + sl, scap - sl, "%lld %ld %s\n", (long long)st.st_size, (long)st.st_mtime, srcs[k]);
        }
        snprintf(sigpath, sizeof sigpath, "%s.sig", out);
        size_t ol;
        uint8_t *old = slurp(sigpath, &ol);
        int rc;
        if (old && ol == sl && !memcmp(old, sig, sl) && !access(out, R_OK)) rc = n;
        else {
            unlink(sigpath);
            rc = txd_build(base, blen, srcs, n, out, img ? name : target);
            FILE *sf = rc > 0 ? fopen(sigpath, "wb") : NULL;
            if (sf) { fwrite(sig, 1, sl, sf); fclose(sf); }
        }
        free(old);
        free(sig);
        free(base);
        if (rc <= 0) continue;
        int mod = g_txdimg[i].mod;
        if (e) { free(e->src); e->src = strdup(out); }
        else if (img) add_entry(&g_img, &g_nimg, &g_capimg, name, out, mod);
        else { g_new_file = 1; add_entry(&g_files, &g_nfiles, &g_capfiles, target, out, mod); g_new_file = 0; }
    }
}

// game text
// a mod's .gxt is a whole copy of the game's text, and macVC's own text has
// strings the 2003 files don't (its graphics menu, controller names...). so the
// mod's file isn't swapped in: only the strings it changed compared to vanilla
// 1.0 (gxt_vanilla.h holds fingerprints of the original strings) are applied on
// top of macVC's text, for the language that file is.
static uint32_t fnv32(uint32_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x01000193; }
    return h;
}
static int cmp_gxthash(const void *a, const void *b) {
    uint32_t x = ((const GxtHash *)a)->key, y = ((const GxtHash *)b)->key;
    return x < y ? -1 : x > y;
}

// calls fn for every string of a VC .gxt (TABL / TKEY / TDAT)
static int gxt_each(const uint8_t *d, size_t len, void (*fn)(const char *table, const char *key, const uint16_t *s, size_t n, void *u), void *u) {
    if (len < 8 || memcmp(d, "TABL", 4)) return -1;
    uint32_t tn;
    memcpy(&tn, d + 4, 4);
    int count = 0;
    for (uint32_t t = 0; t + 12 <= tn && 8 + t + 12 <= len; t += 12) {
        char table[9] = {0};
        memcpy(table, d + 8 + t, 8);
        uint32_t off;
        memcpy(&off, d + 8 + t + 8, 4);
        size_t o = off + (strcmp(table, "MAIN") ? 8 : 0);
        if (o + 8 > len || memcmp(d + o, "TKEY", 4)) continue;
        uint32_t kn;
        memcpy(&kn, d + o + 4, 4);
        size_t ks = o + 8, td = ks + kn;
        if (td + 8 > len || memcmp(d + td, "TDAT", 4)) continue;
        uint32_t dn;
        memcpy(&dn, d + td + 4, 4);
        size_t ds = td + 8, de = ds + dn > len ? len : ds + dn;
        for (uint32_t j = 0; j + 12 <= kn; j += 12) {
            uint32_t eo;
            memcpy(&eo, d + ks + j, 4);
            char key[9] = {0};
            memcpy(key, d + ks + j + 4, 8);
            size_t s = ds + eo, e = s;
            while (e + 1 < de && (d[e] || d[e + 1])) e += 2;
            if (s >= de) continue;
            size_t n = (e - s) / 2;
            uint16_t *w = malloc((n + 1) * 2);
            memcpy(w, d + s, n * 2);
            w[n] = 0;
            fn(table, key, w, n, u);
            free(w);
            count++;
        }
    }
    return count;
}

typedef struct { const GxtHash *van; int nvan; const char *lang; int mod; int applied, conflicts;
                 char (*taken)[8]; int ntaken, captaken; } GxtCtx;

static void gxt_string(const char *table, const char *key, const uint16_t *s, size_t n, void *u) {
    GxtCtx *c = u;
    char tk[32];
    int tl = snprintf(tk, sizeof tk, "%s:%s", table, key);
    GxtHash k = {fnv32(0x811c9dc5, tk, (size_t)tl), 0};
    if (c->van) {
        const GxtHash *v = bsearch(&k, c->van, (size_t)c->nvan, sizeof *c->van, cmp_gxthash);
        if (v && v->value == fnv32(0x811c9dc5, s, n * 2)) return; // the original string: not changed by the mod
    }
    for (int i = 0; i < c->ntaken; i++) if (!strncmp(c->taken[i], key, 7)) { c->conflicts++; return; } // a higher priority mod set it
    if (c->ntaken == c->captaken) { c->captaken = c->captaken ? c->captaken * 2 : 64; c->taken = realloc(c->taken, (size_t)c->captaken * 8); }
    snprintf(c->taken[c->ntaken++], 8, "%.7s", key);
    text_mod_set(c->lang, key, s);
    c->applied++;
}

static void apply_gxt(void) {
    text_mod_clear();
    for (int i = 0; i < g_ngxt; i++) {
        int first = 1;
        for (int j = 0; j < i; j++) if (!strcasecmp(g_gxt[j].lang, g_gxt[i].lang)) first = 0;
        if (!first) continue;
        GxtCtx c = {0};
        c.lang = g_gxt[i].lang;
        for (size_t v = 0; v < sizeof GXT_VANILLA / sizeof *GXT_VANILLA; v++)
            if (!strcasecmp(GXT_VANILLA[v].file, c.lang)) { c.van = GXT_VANILLA[v].tab; c.nvan = GXT_VANILLA[v].n; }
        // highest priority first; a string already set by one wins over later ones
        for (int pass = 100; pass >= 1; pass--)
            for (int j = i; j < g_ngxt; j++) {
                if (strcasecmp(g_gxt[j].lang, c.lang) || g_mods[g_gxt[j].mod].priority != pass) continue;
                size_t len;
                uint8_t *d = slurp(g_gxt[j].src, &len);
                if (!d) continue;
                c.mod = g_gxt[j].mod;
                int before = c.applied;
                if (gxt_each(d, len, gxt_string, &c) < 0)
                    mvc_log_warn("[ModLoader] %s: %s isn't a Vice City text file - skipped", g_mods[c.mod].name, c.lang);
                else
                    mvc_log("[ModLoader] %s: %d string(s) from %s, on top of macVC's text%s", g_mods[c.mod].name,
                            c.applied - before, c.lang, c.van ? "" : " (no original to compare with: every string used)");
                free(d);
            }
        free(c.taken);
    }
}

// the IMG overlay archive
// packs every IMG entry into modloader/.cache/modloader.img + .dir. a manifest of
// sizes and dates means it is only rewritten when a mod changes.
static int build_cache(const char *root) {
    char dir[PATH_MAX], dirfile[PATH_MAX], manifest[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/.cache", root);
    mkdir(dir, 0755);
    snprintf(g_cache_img, sizeof g_cache_img, "%s/modloader.img", dir);
    snprintf(dirfile, sizeof dirfile, "%s/modloader.dir", dir);
    snprintf(manifest, sizeof manifest, "%s/manifest.txt", dir);

    size_t cap = (size_t)g_nimg * 300 + 64, len = 0;
    char *want = malloc(cap);
    len += (size_t)snprintf(want + len, cap - len, "macVC modloader cache 1\n");
    for (int i = 0; i < g_nimg; i++) {
        struct stat st;
        if (stat(g_img[i].src, &st)) st.st_size = 0, st.st_mtime = 0;
        len += (size_t)snprintf(want + len, cap - len, "%lld %ld %s\n", (long long)st.st_size, (long)st.st_mtime, g_img[i].src);
    }
    FILE *f = fopen(manifest, "r");
    if (f) {
        char *have = malloc(len + 2);
        size_t n = fread(have, 1, len + 1, f);
        fclose(f);
        int same = n == len && !memcmp(have, want, len) && access(g_cache_img, R_OK) == 0 && access(dirfile, R_OK) == 0;
        free(have);
        if (same) { free(want); mvc_log("[ModLoader] archive cache is up to date (%d entries)", g_nimg); return 0; }
    }
    unlink(manifest); // a half-written cache must never look valid
    FILE *img = fopen(g_cache_img, "wb"), *dirf = fopen(dirfile, "wb");
    if (!img || !dirf) {
        if (img) fclose(img);
        if (dirf) fclose(dirf);
        free(want);
        mvc_log_error("[ModLoader] cannot write %s: %s", dir, strerror(errno));
        return -1;
    }
    static char buf[1 << 16];
    uint32_t sector = 0;
    int ok = 1;
    for (int i = 0; i < g_nimg && ok; i++) {
        FILE *in = fopen(g_img[i].src, "rb");
        if (!in) { mvc_log_error("[ModLoader] cannot read %s", g_img[i].src); continue; }
        uint64_t bytes = 0;
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
            if (fwrite(buf, 1, n, img) != n) { ok = 0; break; }
            bytes += n;
        }
        fclose(in);
        uint32_t sectors = (uint32_t)((bytes + SECTOR - 1) / SECTOR);
        if (!sectors) continue;
        size_t pad = (size_t)sectors * SECTOR - bytes;
        memset(buf, 0, pad);
        if (pad && fwrite(buf, 1, pad, img) != pad) ok = 0;
        struct { uint32_t offset, size; char name[24]; } de = {sector, sectors, {0}};
        const char *b = strrchr(g_img[i].src, '/');
        snprintf(de.name, sizeof de.name, "%s", b ? b + 1 : g_img[i].src);
        if (fwrite(&de, sizeof de, 1, dirf) != 1) ok = 0;
        sector += sectors;
    }
    if (fclose(img)) ok = 0;
    if (fclose(dirf)) ok = 0;
    if (!ok) { free(want); mvc_log_error("[ModLoader] writing the archive cache failed (disk full?)"); return -1; }
    f = fopen(manifest, "w");
    if (f) { fwrite(want, 1, len, f); fclose(f); }
    free(want);
    mvc_log("[ModLoader] built the archive cache: %d entries, %.1f MB", g_nimg, sector * (double)SECTOR / 1048576.0);
    return 0;
}

// hooks
// the game's path -> lowercase path relative to GameData, or NULL if outside it.
static const char *game_relative(const char *path, char *out, size_t n) {
    char p[PATH_MAX], cwd[PATH_MAX];
    if (path[0] == '/' || path[0] == '\\') snprintf(p, sizeof p, "%s", path);
    else if (getcwd(cwd, sizeof cwd)) snprintf(p, sizeof p, "%s/%s", cwd, path);
    else return NULL;
    // backslashes, "//", "/./" and "dir/../"
    char norm[PATH_MAX];
    size_t w = 0;
    for (const char *s = p; *s && w < sizeof norm - 1;) {
        if (*s == '/' || *s == '\\') {
            while (*s == '/' || *s == '\\') s++;
            if (s[0] == '.' && (!s[1] || s[1] == '/' || s[1] == '\\')) { s++; continue; }
            if (s[0] == '.' && s[1] == '.' && (!s[2] || s[2] == '/' || s[2] == '\\')) {
                while (w > 0 && norm[--w] != '/') {}
                s += 2;
                continue;
            }
            norm[w++] = '/';
        } else norm[w++] = *s++;
    }
    // paths from gta_vc.dat lines end in a space (the game turns the line break
    // into one); casepath ignores it, so must we
    while (w > 0 && (norm[w - 1] == ' ' || norm[w - 1] == '\t' || norm[w - 1] == '\r' || norm[w - 1] == '\n')) w--;
    norm[w] = 0;
    size_t L = strlen(g_game_dir);
    if (strncasecmp(norm, g_game_dir, L) || norm[L] != '/') return NULL;
    snprintf(out, n, "%s", norm + L + 1);
    lower(out);
    return out;
}

// the file list is rebuilt by a refresh while other threads (audio) may be opening files
static pthread_rwlock_t g_files_lock = PTHREAD_RWLOCK_INITIALIZER;

static char *h_casepath(const char *path, _Bool check_first) {
    char rel[PATH_MAX];
    if (path && game_relative(path, rel, sizeof rel)) {
        size_t rl = strlen(rel);
        // the game opening a language's text: mods' strings for that language apply.
        // a language whose file was removed falls back to English instead of crashing
        // (macVC only probes text/russian.gxt to decide whether to offer Russian).
        if (!strncmp(rel, "text/", 5) && rl > 9 && !strcmp(rel + rl - 4, ".gxt")) {
            char real[PATH_MAX];
            if (strcmp(rel, "text/russian.gxt") && game_real_path(rel, real, sizeof real) &&
                !game_real_path("text/american.gxt", real, sizeof real)) {
                static int told;
                if (!told++) mvc_log("[Loader] %s isn't installed - using English text", rel + 5);
                text_mod_language("american.gxt");
                return strdup(real);
            }
            text_mod_language(rel + 5);
        }
        if (g_nfiles) {
            char *found = NULL;
            pthread_rwlock_rdlock(&g_files_lock);
            Entry k = {rel, NULL, 0, 0};
            Entry *e = bsearch(&k, g_files, (size_t)g_nfiles, sizeof *g_files, cmp_entry);
            if (e) {
                if (!e->used) { e->used = 1; mvc_log("[ModLoader] %s: using %s", e->key, g_mods[e->mod].name); }
                found = strdup(e->src); // callers free() it, like casepath's own result
            }
            pthread_rwlock_unlock(&g_files_lock);
            if (found) return found;
        }
    }
    return o_casepath(path, check_first);
}

static void h_LoadCdDirectory(void) {
    // registered last = read first, so these entries win over gta3.img's
    if (g_nimg && g_cache_img[0] && f_CdStreamAddImage) {
        f_CdStreamAddImage(g_cache_img);
        mvc_log("[ModLoader] archive overlay registered (%d entries)", g_nimg);
    }
    o_LoadCdDirectory();
}

// console
static void cmd_mods(int argc, const char **argv) {
    (void)argc; (void)argv;
    if (!g_nmods) { mvc_log("no mods - put mod folders in GameData/modloader/"); return; }
    for (int i = 0; i < g_nmods; i++) {
        Mod *m = &g_mods[i];
        if (!m->enabled) mvc_log("  %-28s disabled", m->name);
        else mvc_log("  %-28s priority %-3d %d file(s), %d archive entr%s%s%s", m->name, m->priority, m->files, m->img,
                     m->img == 1 ? "y" : "ies", m->unused ? ", some files unused" : "",
                     m->unsupported ? ", has Windows-only parts (see the log)" : "");
    }
}
static void cmd_conflicts(int argc, const char **argv) {
    (void)argc; (void)argv;
    if (!g_nconflicts) { mvc_log("no conflicts between mods"); return; }
    for (int i = 0; i < g_nconflicts; i++) mvc_log("  %s", g_conflicts[i]);
    mvc_log("set ModName=<1-100> under [Mods] in macVCLoader.ini to change who wins (higher wins)");
}
static void cmd_modfile(int argc, const char **argv) {
    if (argc < 2) { mvc_log("usage: modfile <game path>   e.g. modfile data/handling.cfg"); return; }
    char rel[PATH_MAX], p[PATH_MAX];
    snprintf(p, sizeof p, "%s/%s", g_game_dir, argv[1]);
    if (!game_relative(p, rel, sizeof rel)) return;
    Entry k = {rel, NULL, 0, 0};
    Entry *e = bsearch(&k, g_files, (size_t)g_nfiles, sizeof *g_files, cmp_entry);
    const char *b = strrchr(rel, '/');
    Entry k2 = {(char *)(b ? b + 1 : rel), NULL, 0, 0}, *e2 = NULL;
    for (int i = 0; i < g_nimg && !e; i++) if (!strcmp(g_img[i].key, k2.key)) e2 = &g_img[i];
    if (e) mvc_log("%s -> %s (%s)", rel, e->src, g_mods[e->mod].name);
    else if (e2) mvc_log("%s (in gta3.img) -> %s (%s)", k2.key, e2->src, g_mods[e2->mod].name);
    else mvc_log("%s: not replaced by any mod", rel);
}

int modloader_active(void) { return g_active; }

// in-game menu
// Options > Mods: every mod gets its own page in the MOD selector (next to the
// plugins' pages), with ENABLED and PRIORITY rows saved to macVCLoader.ini [Mods].
// like Windows Mod Loader's menu, changes apply on the next launch: models and
// map data are only read while the game starts.
#define MENU_MODS 120
static int g_menu_on[MENU_MODS], g_menu_was_on[MENU_MODS], g_nmenu;
static char g_menu_name[MENU_MODS][64]; // mod indices change when a refresh finds new mods
static float g_menu_prio[MENU_MODS], g_menu_was_prio[MENU_MODS];

static void menu_changed(void) {
    for (int i = 0; i < g_nmenu; i++) {
        if (g_menu_on[i] == g_menu_was_on[i] && g_menu_prio[i] == g_menu_was_prio[i]) continue;
        char v[16];
        snprintf(v, sizeof v, "%d", g_menu_on[i] ? (int)(g_menu_prio[i] + 0.5f) : 0);
        mvc_ini_set("macVCLoader.ini", "Mods", g_menu_name[i], v);
        mvc_log("[ModLoader] %s: %s, priority %d - applied when you leave the menu (models, textures and maps after a "
                "restart)", g_menu_name[i], g_menu_on[i] ? "on" : "off", (int)(g_menu_prio[i] + 0.5f));
        g_menu_was_on[i] = g_menu_on[i];
        g_menu_was_prio[i] = g_menu_prio[i];
    }
}

void modloader_menu(void) { // called after the plugins loaded, so their pages come first
    for (int i = 0; i < g_nmods && i < MENU_MODS; i++) {
        int page = mvc_menu_page(g_mods[i].name);
        if (page < 0) { mvc_log("[ModLoader] the Mods menu is full - set the other mods in macVCLoader.ini [Mods]"); break; }
        snprintf(g_menu_name[i], sizeof g_menu_name[0], "%s", g_mods[i].name);
        g_menu_on[i] = g_menu_was_on[i] = g_mods[i].enabled;
        g_menu_prio[i] = g_menu_was_prio[i] = (float)g_mods[i].priority;
        mvc_menu_toggle(page, "Enabled", &g_menu_on[i], NULL, NULL, NULL, menu_changed);
        mvc_menu_number(page, "Priority", &g_menu_prio[i], 1, 100, 1, "%.0f", NULL, NULL, NULL, menu_changed);
        g_nmenu = i + 1;
    }
}

// refresh
// like Windows Mod Loader's refresh: when you leave the pause menu (or type
// "mods refresh") and a mod file changed, the mods are scanned again. replaced
// files and merged data apply from then on; handling, weapons and the time
// cycle are reloaded on the spot. models, textures and map files are only read
// while the game starts, so those changes are reported as needing a restart.
static uint64_t hash_bytes(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}
static uint64_t hash_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint64_t h = 14695981039346656037ULL;
    static uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) h = hash_bytes(h, buf, n);
    fclose(f);
    return h;
}

// changes when any mod file or either ini changes (names, sizes, dates)
static void fp_walk(const char *dir, int depth, uint64_t *h) {
    DIR *d = opendir(dir);
    if (!d || depth > 10) { if (d) closedir(d); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st)) continue;
        *h = hash_bytes(*h, p, strlen(p));
        if (S_ISDIR(st.st_mode)) { fp_walk(p, depth + 1, h); continue; }
        *h = hash_bytes(*h, &st.st_size, sizeof st.st_size);
        *h = hash_bytes(*h, &st.st_mtimespec, sizeof st.st_mtimespec);
    }
    closedir(d);
}
static uint64_t fingerprint(void) {
    uint64_t h = 14695981039346656037ULL;
    fp_walk(g_root, 0, &h);
    char p[PATH_MAX];
    struct stat st;
    snprintf(p, sizeof p, "%s/macVCLoader.ini", g_game_dir);
    if (!stat(p, &st)) h = hash_bytes(h, &st.st_mtimespec, sizeof st.st_mtimespec);
    return h;
}
static uint64_t g_fingerprint;

// [Mods] of macVCLoader.ini again (the menu may have changed it)
static void reload_mods_ini(void) {
    g_ini_n = 0;
    char p[PATH_MAX], line[512], sec[64] = "";
    snprintf(p, sizeof p, "%s/macVCLoader.ini", g_game_dir);
    FILE *f = fopen(p, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        ml_trim(line);
        if (!line[0]) continue;
        if (line[0] == '[' && !strchr(line, '=')) { snprintf(sec, sizeof sec, "%s", line); continue; }
        char *eq = strchr(line, '=');
        if (!eq || strcasecmp(sec, "[Mods]")) continue;
        *eq = 0;
        char k[256], v[64];
        snprintf(k, sizeof k, "%s", line);
        snprintf(v, sizeof v, "%s", eq + 1);
        ml_trim(k); ml_trim(v);
        modloader_ini(k, v);
    }
    fclose(f);
}

static uint64_t winner_hash(const char *key) {
    Entry *e = find_file(key);
    char real[PATH_MAX];
    if (e) return hash_file(e->src);
    return game_real_path(key, real, sizeof real) ? 0 : hash_file(real);
}
static uint64_t hash_where(int (*want)(const char *)) {
    uint64_t h = 14695981039346656037ULL;
    for (int i = 0; i < g_nfiles; i++)
        if (want(g_files[i].key)) { h = hash_bytes(h, g_files[i].key, strlen(g_files[i].key)); uint64_t c = hash_file(g_files[i].src); h = hash_bytes(h, &c, 8); }
    return h;
}
static int is_map_file(const char *k) {
    const char *x = ext_of(k);
    return !strcmp(k, "data/gta_vc.dat") || !strcmp(x, ".ide") || !strcmp(x, ".ipl") || !strcmp(x, ".col") || !strcmp(k, "data/carcols.dat");
}
static uint64_t hash_img(void) {
    uint64_t h = 14695981039346656037ULL;
    for (int i = 0; i < g_nimg; i++) {
        struct stat st;
        h = hash_bytes(h, g_img[i].key, strlen(g_img[i].key));
        h = hash_bytes(h, g_img[i].src, strlen(g_img[i].src));
        if (!stat(g_img[i].src, &st)) { h = hash_bytes(h, &st.st_size, sizeof st.st_size); h = hash_bytes(h, &st.st_mtimespec, sizeof st.st_mtimespec); }
    }
    return h;
}

static void free_entries(Entry *a, int n) { for (int i = 0; i < n; i++) { free(a[i].key); free(a[i].src); } }

static void refresh_mods(int forced) {
    if (!g_root[0]) return;
    uint64_t fp = fingerprint();
    if (!forced && fp == g_fingerprint) return;
    g_fingerprint = fp;
    static const struct { const char *key, *sym, *what; int member; } live[] = {
        {"data/handling.cfg", "_ZN16cHandlingDataMgr16LoadHandlingDataEv", "handling", 1},
        {"data/weapon.dat", "_ZN11CWeaponInfo14LoadWeaponDataEv", "weapons", 0},
        {"data/timecyc.dat", "_ZN10CTimeCycle10InitialiseEv", "time cycle", 0},
    };
    enum { NLIVE = sizeof live / sizeof *live };
    uint64_t before[NLIVE];
    for (int i = 0; i < NLIVE; i++) before[i] = winner_hash(live[i].key);
    uint64_t map_before = hash_where(is_map_file), img_before = hash_img();
    int files_before = g_nfiles;

    pthread_rwlock_wrlock(&g_files_lock);
    free_entries(g_files, g_nfiles);
    g_nfiles = 0;
    Entry *old_img = g_img; int old_nimg = g_nimg, old_capimg = g_capimg;
    g_img = NULL; g_nimg = 0; g_capimg = 0;
    for (int i = 0; i < g_npool; i++) { free(g_pool[i].rel); free(g_pool[i].src); }
    g_npool = 0;
    for (int i = 0; i < g_nvers; i++) { free(g_vers[i].key); free(g_vers[i].src); }
    g_nvers = 0;
    for (int i = 0; i < g_ntxdimg; i++) free(g_txdimg[i].src);
    g_ntxdimg = 0;
    for (int i = 0; i < g_ntex; i++) free(g_tex[i].src);
    g_ntex = 0;
    for (int i = 0; i < g_ngxt; i++) free(g_gxt[i].src);
    g_ngxt = 0;
    g_nconflicts = 0;
    g_nmods = 0;
    memset(&g_ml, 0, sizeof g_ml);
    reload_mods_ini();
    read_ml_ini(g_root);
    scan_mods(g_root);
    apply_readmes();
    merge_all();
    if (g_npool) build_gta_dat();
    apply_texture_packs();
    build_txd_folders();
    apply_gxt();
    qsort(g_files, (size_t)g_nfiles, sizeof *g_files, cmp_entry);
    uint64_t img_after = hash_img();
    free_entries(g_img, g_nimg);
    free(g_img);
    g_img = old_img; g_nimg = old_nimg; g_capimg = old_capimg; // the registered archive stays as it is
    pthread_rwlock_unlock(&g_files_lock);

    mvc_log("[ModLoader] refreshed: %d mod(s), %d file(s) replaced (was %d)", g_nmods, g_nfiles, files_before);
    for (int i = 0; i < NLIVE; i++) {
        if (winner_hash(live[i].key) == before[i]) continue;
        void *fn = mvc_sym(live[i].sym);
        void *self = live[i].member ? mvc_sym("mod_HandlingManager") : NULL;
        if (!fn || (live[i].member && !self)) continue;
        if (live[i].member) ((void (*)(void *))fn)(self);
        else ((void (*)(void))fn)();
        mvc_log("[ModLoader] reloaded %s (%s)", live[i].what, live[i].key);
    }
    if (hash_where(is_map_file) != map_before)
        mvc_log_warn("[ModLoader] map or vehicle-colour files changed - restart the game to see those changes");
    if (img_after != img_before)
        mvc_log_warn("[ModLoader] models or textures changed - restart the game to see them (the archive is rebuilt then)");
}

static void cmd_mods_refresh(int argc, const char **argv) {
    if (argc >= 2 && !strcasecmp(argv[1], "refresh")) { refresh_mods(1); return; }
    cmd_mods(argc, argv);
}
static void on_event(int ev, void *arg) {
    (void)arg;
    if (ev == MVC_EV_RESUMED) refresh_mods(0); // edit mods while paused, unpause to apply
}

void modloader_init(void) {
    mvc_console_command("mods", "mods in GameData/modloader;  mods refresh - apply changed mod files", cmd_mods_refresh);
    mvc_console_command("conflicts", "files two mods both replace, and which one wins", cmd_conflicts);
    mvc_console_command("modfile", "modfile <game path> - which mod replaces a file", cmd_modfile);
    mvc_console_command("texfind", "texfind <name> - which texture dictionaries hold a texture", cmd_texfind);
    char root[PATH_MAX];
    snprintf(root, sizeof root, "%s/modloader", g_game_dir);
    struct stat st;
    if (stat(root, &st)) mkdir(root, 0755); // ready for the first mod
    snprintf(g_root, sizeof g_root, "%s", root);
    char cache[PATH_MAX];
    snprintf(cache, sizeof cache, "%s/.cache", root);
    mkdir(cache, 0755);
    index_game();
    read_ml_ini(root);
    scan_mods(root);
    // the hooks go in even with no mods yet, so mods added while playing work after a refresh
    g_fingerprint = fingerprint();
    mvc_on_event(on_event);
    apply_readmes();
    merge_all();
    if (g_npool) build_gta_dat();
    apply_texture_packs();
    build_txd_folders();
    apply_gxt();
    qsort(g_files, (size_t)g_nfiles, sizeof *g_files, cmp_entry);
    if (g_nimg && build_cache(root)) g_nimg = 0;
    if (mvc_hook_sym("_Z8casepathPKcb", (void *)h_casepath, (void **)&o_casepath)) { g_nfiles = 0; g_root[0] = 0; }
    if (g_nimg) {
        *(void **)&f_CdStreamAddImage = mvc_sym("CdStreamAddImage");
        if (!f_CdStreamAddImage || mvc_hook_sym("_ZN10CStreaming15LoadCdDirectoryEv", (void *)h_LoadCdDirectory,
                                                (void **)&o_LoadCdDirectory)) {
            mvc_log_error("[ModLoader] this macVC build can't take archive mods - .dff/.txd replacements not used");
            g_nimg = 0;
        }
    }
    int enabled = 0;
    for (int i = 0; i < g_nmods; i++) enabled += g_mods[i].enabled;
    g_active = g_nfiles || g_nimg;
    mvc_log("[ModLoader] %d mod(s) (%d enabled): %d file(s) replaced, %d archive entr%s, %d conflict(s)%s", g_nmods, enabled,
            g_nfiles, g_nimg, g_nimg == 1 ? "y" : "ies", g_nconflicts, g_nconflicts ? " - type conflicts" : "");
}
