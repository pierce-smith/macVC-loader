// startup. macVC loads this through the fake libmpg123.dylib, same idea as an asi loader
// riding in on a proxy dll. reads macVCLoader.ini, loads every enabled .dylib from
// GameData/plugins and GameData/scripts (and any extra folders), handles safe mode and
// turning off plugins that crash on launch.
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "internal.h"

LoaderConfig g_cfg = {96, 161, 232, 1.0f, LOG_INFO, 1, 1, 1024, 1, 0}; // ` and the ISO section key
char g_game_dir[1024];
int g_safe_mode;
int g_plugins_loaded, g_plugins_failed, g_plugins_disabled;
int g_in_startup_window = 1; // until the first game tick (read by crash.c)

#define AUTO_DISABLE_AFTER 2

// ini
static char g_load_order[64][64];
static int g_nload_order;
static char g_extra_dirs[8][64];
static int g_nextra;
static struct { char name[64]; int enabled; } g_flags[256];
static int g_nflags;

static const char *DEFAULT_INI =
"; macVC Plugin Loader settings\n"
"[Loader]\n"
"; key that opens the console: GRAVE (the ` key), SECTION (the key left of 1 on\n"
"; ISO keyboards), or F1-F12, or a letter\n"
"ConsoleKey=GRAVE\n"
"ConsoleKey2=SECTION\n"
"; console background opacity 0-255, and text size\n"
"ConsoleOpacity=232\n"
"ConsoleScale=1.0\n"
"; info, warn or error - what plugins may write to the log\n"
"LogLevel=info\n"
"; extra plugin folders, relative to GameData, comma separated\n"
"ExtraPluginDirs=\n"
"; plugins loaded first, in this order (file names without .dylib); the rest load alphabetically\n"
"LoadOrder=\n"
"; 1 = if the game ends up with no controller while one is connected, give it that one\n"
"ControllerFallback=1\n"
"; 1 = the game's controller vibration (Options > Controls > Vibration) rumbles the\n"
"; real controller, DualSense and Xbox included (macVC has no rumble of its own)\n"
"GameRumble=1\n"
"; memory for streamed models and textures, in MB (macVC always picks 65, too\n"
"; little for texture packs: things pop in and out). 0 = leave the game's\n"
"StreamingMemory=1024\n"
"; largest texture size (pixels, e.g. 1024) that Mod Loader builds from mods' images\n"
"; (textures/ and name.txd/ folders); bigger images are scaled down. 0 = no limit\n"
"TextureMaxSize=0\n"
"; 1 = play the Rockstar logo and title movies at startup, like on Windows\n"
"; (macVC skips them). Enter, Space, Escape, a click or A / Start skips one.\n"
"PlayMovies=1\n"
"; start with no plugins (or launch with \"Safe Mode.command\")\n"
"SafeMode=0\n"
"\n"
"[Plugins]\n"
"; PluginName=0 disables a plugin. Plugins not listed are enabled.\n"
"\n"
"[Mods]\n"
"; folders in GameData/modloader. ModName=0 disables a mod; ModName=1-100 sets its\n"
"; priority (default 50) - the higher one wins when two mods replace the same file\n";

static void trim(char *s) {
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    char *b = s;
    while (isspace((unsigned char)*b)) b++;
    if (b != s) memmove(s, b, strlen(b) + 1);
}

static int key_from_name(const char *s) {
    if (!strcasecmp(s, "GRAVE") || !strcmp(s, "`") || !strcmp(s, "~")) return 96;
    if (!strcasecmp(s, "SECTION") || !strcasecmp(s, "WORLD1")) return 161;
    if (!strcasecmp(s, "NONE") || !s[0]) return 0;
    if ((s[0] == 'F' || s[0] == 'f') && isdigit((unsigned char)s[1])) {
        int n = atoi(s + 1);
        if (n >= 1 && n <= 12) return 289 + n;
    }
    if (!s[1] && isalnum((unsigned char)s[0])) return toupper((unsigned char)s[0]);
    return -1;
}

static void split_list(char *v, char out[][64], int max, int *n) {
    *n = 0;
    for (char *t = strtok(v, ","); t && *n < max; t = strtok(NULL, ",")) {
        trim(t);
        if (*t) snprintf(out[(*n)++], 64, "%s", t);
    }
}

// an ini written by an older loader lacks newer settings. adds each missing
// setting (with its comment) to the end of its section, and missing sections
// to the end of the file; the user's own lines are kept exactly as they are.
static int ini_has(const char *text, const char *needle) { // a line starting with needle, any case
    size_t n = strlen(needle);
    for (const char *l = text; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
        while (*l == ' ' || *l == '\t') l++;
        if (!strncasecmp(l, needle, n)) return 1;
    }
    return 0;
}

static void upgrade_ini(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    static char text[65536];
    size_t len = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    if (len >= sizeof text - 1) return; // unusually big: leave it alone
    text[len] = 0;
    // per section of DEFAULT_INI: the settings the user's file is missing
    char sec_name[8][32] = {{0}}, sec_add[8][2048] = {{0}};
    int sec_new[8] = {0}, nsec = 0, cur = -1, added = 0;
    char pending[1024] = "";
    for (const char *l = DEFAULT_INI; *l;) {
        const char *e = strchr(l, '\n');
        size_t L = e ? (size_t)(e - l) : strlen(l);
        char line[512];
        snprintf(line, sizeof line, "%.*s", (int)(L < sizeof line ? L : sizeof line - 1), l);
        l += L + (e ? 1 : 0);
        if (line[0] == '[' && nsec < 8) {
            cur = nsec++;
            snprintf(sec_name[cur], sizeof sec_name[0], "%s", line);
            sec_new[cur] = !ini_has(text, line);
            pending[0] = 0;
        } else if (line[0] == ';') {
            strlcat(pending, line, sizeof pending);
            strlcat(pending, "\n", sizeof pending);
        } else if (cur >= 0 && strchr(line, '=')) {
            char key[96];
            snprintf(key, sizeof key, "%.*s=", (int)(strchr(line, '=') - line), line);
            if (sec_new[cur] || !ini_has(text, key)) {
                strlcat(sec_add[cur], pending, sizeof sec_add[0]);
                strlcat(sec_add[cur], line, sizeof sec_add[0]);
                strlcat(sec_add[cur], "\n", sizeof sec_add[0]);
                added++;
            }
            pending[0] = 0;
        } else if (cur >= 0 && sec_new[cur] && pending[0]) { // comments of a section with no settings
            strlcat(sec_add[cur], pending, sizeof sec_add[0]);
            pending[0] = 0;
        }
    }
    if (cur >= 0 && sec_new[cur] && pending[0]) strlcat(sec_add[cur], pending, sizeof sec_add[0]);
    int any = 0;
    for (int i = 0; i < nsec; i++) if (sec_new[i] || sec_add[i][0]) any = 1;
    if (!any) return;
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *o = fopen(tmp, "w");
    if (!o) return;
    // copy the user's file, inserting additions just before the next section starts
    int in = -1, blanks = 0; // blank lines are held back so additions go before them
    for (const char *l = text; *l;) {
        const char *e = strchr(l, '\n');
        size_t L = e ? (size_t)(e - l + 1) : strlen(l);
        const char *t = l;
        while (*t == ' ' || *t == '\t' || *t == '\r') t++;
        if (*t == '\n' || !*t) { blanks++; l += L; continue; }
        if (*t == '[' && !memchr(t, '=', L - (size_t)(t - l))) {
            if (in >= 0 && sec_add[in][0] && !sec_new[in]) { fputs(sec_add[in], o); sec_add[in][0] = 0; }
            in = -1;
            for (int i = 0; i < nsec; i++) if (!strncasecmp(t, sec_name[i], strlen(sec_name[i]))) in = i;
        }
        for (; blanks; blanks--) fputc('\n', o);
        fwrite(l, 1, L, o);
        if (!e) fputc('\n', o);
        l += L;
    }
    if (in >= 0 && sec_add[in][0] && !sec_new[in]) { fputs(sec_add[in], o); sec_add[in][0] = 0; }
    for (int i = 0; i < nsec; i++)
        if (sec_new[i]) fprintf(o, "\n%s\n%s", sec_name[i], sec_add[i]);
    if (fclose(o) || rename(tmp, path)) { unlink(tmp); return; }
    mvc_log("[Loader] added %d new setting(s) to macVCLoader.ini", added);
}

static void read_ini(void) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/macVCLoader.ini", g_game_dir);
    FILE *f = fopen(path, "r");
    if (!f) {
        f = fopen(path, "w");
        if (f) { fputs(DEFAULT_INI, f); fclose(f); }
        f = fopen(path, "r");
        if (!f) return;
    } else {
        fclose(f);
        upgrade_ini(path);
        f = fopen(path, "r");
        if (!f) return;
    }
    char line[512], section[32] = "";
    while (fgets(line, sizeof line, f)) {
        trim(line);
        if (!line[0] || line[0] == ';' || line[0] == '#') continue;
        // a section header; "[VC]Gasoline=0" (a mod named with brackets) is a setting
        if (line[0] == '[' && !strchr(line, '=')) { snprintf(section, sizeof section, "%s", line); continue; }
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        trim(k); trim(v);
        if (!strcasecmp(section, "[Mods]")) { modloader_ini(k, v); continue; }
        if (!strcasecmp(section, "[Plugins]")) {
            if (g_nflags < 256) {
                snprintf(g_flags[g_nflags].name, 64, "%s", k);
                g_flags[g_nflags++].enabled = atoi(v) != 0;
            }
            continue;
        }
        if (!strcasecmp(k, "ConsoleKey") || !strcasecmp(k, "ConsoleKey2")) {
            int key = key_from_name(v);
            if (key < 0) mvc_log_warn("[Loader] unknown %s \"%s\"", k, v);
            else if (!strcasecmp(k, "ConsoleKey")) g_cfg.console_key = key;
            else g_cfg.console_key2 = key;
        } else if (!strcasecmp(k, "ConsoleOpacity")) {
            int o = atoi(v);
            g_cfg.console_opacity = o < 0 ? 0 : o > 255 ? 255 : o;
        } else if (!strcasecmp(k, "ConsoleScale")) {
            float s = (float)atof(v);
            g_cfg.console_scale = s < 0.5f ? 0.5f : s > 3.f ? 3.f : s;
        } else if (!strcasecmp(k, "LogLevel")) {
            g_cfg.log_level = !strcasecmp(v, "error") ? LOG_ERROR : !strcasecmp(v, "warn") ? LOG_WARN : LOG_INFO;
        } else if (!strcasecmp(k, "ExtraPluginDirs")) {
            split_list(v, g_extra_dirs, 8, &g_nextra);
        } else if (!strcasecmp(k, "LoadOrder")) {
            split_list(v, g_load_order, 64, &g_nload_order);
        } else if (!strcasecmp(k, "PlayMovies")) {
            g_cfg.play_movies = atoi(v) != 0;
        } else if (!strcasecmp(k, "StreamingMemory")) {
            int mb = atoi(v);
            g_cfg.streaming_mb = mb < 0 ? 0 : mb > 4096 ? 4096 : mb;
        } else if (!strcasecmp(k, "TextureMaxSize")) {
            int px = atoi(v);
            g_cfg.texture_max = px <= 0 ? 0 : px < 64 ? 64 : px > 4096 ? 4096 : px;
        } else if (!strcasecmp(k, "GameRumble")) {
            g_cfg.game_rumble = atoi(v) != 0;
        } else if (!strcasecmp(k, "ControllerFallback")) {
            g_cfg.controller_fallback = atoi(v) != 0;
        } else if (!strcasecmp(k, "SafeMode")) {
            if (atoi(v)) g_safe_mode = 1;
        }
    }
    fclose(f);
}

static int plugin_enabled(const char *base) {
    for (int i = 0; i < g_nflags; i++)
        if (!strcasecmp(g_flags[i].name, base)) return g_flags[i].enabled;
    return 1;
}

// crash loop protection
static int startup_crashes(const char *file) {
    FILE *f = fopen(crash_state_path(), "r");
    if (!f) return 0;
    char name[256];
    int c, found = 0;
    while (fscanf(f, "%255s %d", name, &c) == 2)
        if (!strcmp(name, file)) { found = c; break; }
    fclose(f);
    return found;
}

void startup_finished(void) {
    g_in_startup_window = 0;
    // the game made it to gameplay: forget startup crashes of plugins that loaded fine
    FILE *f = fopen(crash_state_path(), "r");
    if (!f) return;
    char keep[64][300];
    int n = 0;
    char name[256];
    int c;
    while (n < 64 && fscanf(f, "%255s %d", name, &c) == 2)
        if (c >= AUTO_DISABLE_AFTER) snprintf(keep[n++], 300, "%s %d\n", name, c); // still disabled
    fclose(f);
    if (!n) { unlink(crash_state_path()); return; }
    f = fopen(crash_state_path(), "w");
    if (!f) return;
    for (int i = 0; i < n; i++) fputs(keep[i], f);
    fclose(f);
}

// loading
typedef struct { char file[96]; char desc[160]; char dir[64]; char name[256]; void *handle; int reloads; char trust[96]; } Loaded;
static Loaded g_loaded[256];
static int g_nloaded;

void plugins_print(void) {
    mvc_log("[Loader] %d plugin(s) loaded:", g_nloaded);
    for (int i = 0; i < g_nloaded; i++)
        mvc_log("[Loader]   %-28s %s%s  [%s]", g_loaded[i].file, g_loaded[i].desc, g_loaded[i].reloads ? " (reloaded)" : "",
                g_loaded[i].trust);
}
typedef struct { char dir[64]; char file[256]; } Candidate;
static Candidate g_cand[512];
static int g_ncand;

static void collect(const char *sub) {
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/%s", g_game_dir, sub);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && g_ncand < 512) {
        size_t L = strlen(e->d_name);
        if (L > 6 && !strcasecmp(e->d_name + L - 6, ".dylib") && e->d_name[0] != '.') {
            snprintf(g_cand[g_ncand].dir, 64, "%s", sub);
            snprintf(g_cand[g_ncand].file, 256, "%s", e->d_name);
            g_ncand++;
        }
    }
    closedir(d);
}

static int order_rank(const Candidate *c) {
    char base[256];
    snprintf(base, sizeof base, "%s", c->file);
    base[strlen(base) - 6] = 0;
    for (int i = 0; i < g_nload_order; i++)
        if (!strcasecmp(g_load_order[i], base)) return i;
    return 1000;
}

static int cand_cmp(const void *a, const void *b) {
    const Candidate *x = a, *y = b;
    int rx = order_rank(x), ry = order_rank(y);
    if (rx != ry) return rx - ry;
    return strcasecmp(x->file, y->file);
}

static int start_plugin(const char *dir, const char *file, const char *path, Loaded *slot);

static void load_one(const Candidate *c) {
    char base[256], path[PATH_MAX];
    snprintf(base, sizeof base, "%s", c->file);
    base[strlen(base) - 6] = 0;
    if (!plugin_enabled(base)) {
        mvc_log("[Loader] %s/%s is disabled in macVCLoader.ini", c->dir, c->file);
        g_plugins_disabled++;
        return;
    }
    int crashes = startup_crashes(c->file);
    if (crashes >= AUTO_DISABLE_AFTER) {
        mvc_log_error("[Loader] %s crashed the game during startup %d times and was disabled. "
                      "Delete GameData/macVCLoader.state to try it again.", c->file, crashes);
        g_plugins_disabled++;
        return;
    }
    snprintf(path, sizeof path, "%s/%s/%s", g_game_dir, c->dir, c->file);
    if (start_plugin(c->dir, c->file, path, NULL)) g_plugins_failed++;
    else g_plugins_loaded++;
}

// dlopens `path` and runs its MVCPluginInit. `slot` is the plugin's existing
// entry when it is being reloaded, NULL for a new one.
static int start_plugin(const char *dir, const char *file, const char *path, Loaded *slot) {
    char base[256];
    snprintf(base, sizeof base, "%s", file);
    base[strlen(base) - 6] = 0;
    Candidate cc;
    snprintf(cc.dir, sizeof cc.dir, "%s", dir);
    snprintf(cc.file, sizeof cc.file, "%s", file);
    const Candidate *c = &cc;
    char trust[96];
    int tf = plugin_trust(path, trust, sizeof trust);
    if (tf & TRUST_QUARANTINED)
        mvc_log_warn("[Loader] %s/%s is quarantined (downloaded from the internet). Plugins run with the game's full "
                     "access - only keep it if you trust where it came from.", c->dir, c->file);
    crash_set_loading(c->file);
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        crash_set_loading(NULL);
        mvc_log_error("[Loader] FAILED to load %s/%s: %s", c->dir, c->file, dlerror());
        if (tf & (TRUST_UNSIGNED | TRUST_BAD_SIGNATURE))
            mvc_log_error("[Loader]   %s is %s. Apple Silicon only loads signed code: re-sign it with "
                          "codesign -f -s - \"%s\" (only if you trust it)", c->file,
                          tf & TRUST_UNSIGNED ? "unsigned" : "signed but modified since", c->file);
        return -1;
    }
    const MVCPluginInfo *info = dlsym(h, "mvc_plugin_info");
    if (info && info->api_version > MVC_API_VERSION) {
        mvc_log_error("[Loader] %s needs a newer loader (plugin API %d, this loader has %d) - not started",
                      c->file, info->api_version, MVC_API_VERSION);
        crash_set_loading(NULL);
        return -1;
    }
    if (info)
        mvc_log("[Loader] loaded %s %s by %s (%s/%s)", info->name ? info->name : base, info->version ? info->version : "",
                info->author ? info->author : "unknown", c->dir, c->file);
    else
        mvc_log_warn("[Loader] loaded %s/%s with no manifest (MVC_PLUGIN) - its author and version are unknown",
                     c->dir, c->file);
    Loaded *L = slot ? slot : g_nloaded < 256 ? &g_loaded[g_nloaded++] : NULL;
    if (L) {
        snprintf(L->file, sizeof L->file, "%s/%s", c->dir, c->file);
        snprintf(L->dir, sizeof L->dir, "%s", c->dir);
        snprintf(L->name, sizeof L->name, "%s", base);
        L->handle = h;
        snprintf(L->trust, sizeof L->trust, "%s", trust);
        if (info) snprintf(L->desc, sizeof L->desc, "%s %s - %s", info->name ? info->name : "",
                           info->version ? info->version : "", info->description ? info->description : "");
        else snprintf(L->desc, sizeof L->desc, "(no manifest)");
    }
    void (*init)(void) = (void (*)(void))dlsym(h, "MVCPluginInit");
    int first_handler = event_handler_count();
    if (init) init();
    events_catch_up(first_handler); // nothing to catch up on during startup
    crash_set_loading(NULL);
    return 0;
}

// hot reload
// `reload <plugin>` swaps in a rebuilt .dylib without restarting the game:
//  1. the plugin's optional MVCPluginUnload() runs (stash state with mvc_save_set)
//  2. its hooks and patches are undone, its callbacks, console commands and
//     Mods menu rows are dropped
//  3. a fresh copy of the file is loaded and initialised, and gets the events
//     it missed (game init, new session, save loaded)
// the old code is never unmapped: anything that still points into it (a game
// pointer, a chained hook, a frame on the stack) keeps working.
// requests are queued and run at the start of the next frame, outside every
// plugin callback.
static char g_pending[8][96];
static int g_npending;

static Loaded *find_loaded(const char *name) {
    for (int i = 0; i < g_nloaded; i++)
        if (!strcasecmp(g_loaded[i].name, name) || !strcasecmp(g_loaded[i].file, name)) return &g_loaded[i];
    return NULL;
}

int plugin_loaded(const char *name) { Loaded *L = find_loaded(name); return L && L->handle; }

static void release(Loaded *L) {
    void (*unload)(void) = L->handle ? (void (*)(void))dlsym(L->handle, "MVCPluginUnload") : NULL;
    if (unload) unload();
    int h = hooks_release(L->name), cb = callbacks_release(L->name);
    int cmd = commands_release(L->name), rows = menu_release(L->name);
    mvc_log("[Loader]   released %d hook(s)/patch(es), %d callback(s), %d command(s), %d menu row(s)", h, cb, cmd, rows);
}

static int copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb");
    if (!in) return -1;
    FILE *o = fopen(to, "wb");
    if (!o) { fclose(in); return -1; }
    char buf[65536];
    size_t n;
    int rc = 0;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, o) != n) { rc = -1; break; }
    fclose(in);
    if (fclose(o)) rc = -1;
    return rc;
}

static void do_reload(const char *name) {
    Loaded *L = find_loaded(name);
    if (!L) { mvc_log_warn("[Loader] reload: no loaded plugin called \"%s\" (type plugins)", name); return; }
    char src[PATH_MAX], dir[PATH_MAX], copy[PATH_MAX];
    snprintf(src, sizeof src, "%s/%s", g_game_dir, L->file);
    if (access(src, R_OK)) { mvc_log_error("[Loader] reload: %s is gone - not reloaded", L->file); return; }
    // load a copy: dyld won't load the same path twice, and the copy keeps the
    // original file free to be rebuilt again while the game runs
    const char *file = strrchr(L->file, '/') + 1;
    snprintf(dir, sizeof dir, "%s/.reload", g_game_dir);
    mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%s/.reload/%d-%d", g_game_dir, getpid(), ++L->reloads);
    mkdir(dir, 0755);
    snprintf(copy, sizeof copy, "%s/%s", dir, file);
    if (copy_file(src, copy)) { mvc_log_error("[Loader] reload: could not copy %s", L->file); return; }
    mvc_log("[Loader] reloading %s", L->file);
    release(L);
    L->handle = NULL; // set again if the new copy starts
    char d[64], f[256];
    snprintf(d, sizeof d, "%s", L->dir);
    snprintf(f, sizeof f, "%s", file);
    int rc = start_plugin(d, f, copy, L);
    unlink(copy); // the loaded image stays valid
    rmdir(dir);
    menu_refresh();
    if (rc) mvc_log_error("[Loader] reload of %s failed - it stays unloaded until fixed and reloaded", L->file);
    else mvc_log("[Loader] reloaded %s (%d hooks/patches active in total)", L->file, hook_count());
}

static void do_unload(const char *name) {
    Loaded *L = find_loaded(name);
    if (!L) { mvc_log_warn("[Loader] unload: no loaded plugin called \"%s\"", name); return; }
    mvc_log("[Loader] unloading %s", L->file);
    release(L);
    menu_refresh();
    int i = (int)(L - g_loaded);
    memmove(&g_loaded[i], &g_loaded[i + 1], (size_t)(g_nloaded - i - 1) * sizeof *g_loaded);
    g_nloaded--;
    mvc_log("[Loader] unloaded %s (its code stays in memory, inactive; load it again with load)", name);
}

static void do_load(const char *name) {
    if (find_loaded(name)) { mvc_log_warn("[Loader] load: %s is already loaded - use reload", name); return; }
    const char *dirs[10] = {"plugins", "scripts"};
    int nd = 2;
    for (int i = 0; i < g_nextra && nd < 10; i++) dirs[nd++] = g_extra_dirs[i];
    for (int i = 0; i < nd; i++) {
        char file[256], path[PATH_MAX];
        snprintf(file, sizeof file, "%s.dylib", name);
        snprintf(path, sizeof path, "%s/%s/%s", g_game_dir, dirs[i], file);
        if (access(path, R_OK)) continue;
        // a plugin unloaded earlier this session can't come back from the same
        // path (dyld keeps it), so go through a copy like reload does
        char dir[PATH_MAX], copy[PATH_MAX];
        snprintf(dir, sizeof dir, "%s/.reload", g_game_dir);
        mkdir(dir, 0755);
        static int n;
        snprintf(dir, sizeof dir, "%s/.reload/%d-load%d", g_game_dir, getpid(), ++n);
        mkdir(dir, 0755);
        snprintf(copy, sizeof copy, "%s/%s", dir, file);
        if (copy_file(path, copy)) { mvc_log_error("[Loader] load: could not copy %s", path); return; }
        int rc = start_plugin(dirs[i], file, copy, NULL);
        unlink(copy);
        rmdir(dir);
        menu_refresh();
        if (!rc) mvc_log("[Loader] loaded %s/%s", dirs[i], file);
        return;
    }
    mvc_log_warn("[Loader] load: no %s.dylib in plugins/, scripts/ or the extra plugin folders", name);
}

void plugins_run_pending(void) {
    for (int i = 0; i < g_npending; i++) {
        char *verb = g_pending[i], *arg = strchr(verb, ' ');
        if (!arg) continue;
        *arg++ = 0;
        if (!strcmp(verb, "reload")) do_reload(arg);
        else if (!strcmp(verb, "unload")) do_unload(arg);
        else do_load(arg);
    }
    g_npending = 0;
}

static void queue(const char *verb, int argc, const char **argv) {
    if (argc < 2) { mvc_log("usage: %s <plugin name>   (type plugins for the list)", verb); return; }
    if (g_npending >= 8) return;
    snprintf(g_pending[g_npending++], sizeof g_pending[0], "%s %s", verb, argv[1]);
}
static void cmd_reload(int argc, const char **argv) {
    if (argc >= 2 && !strcasecmp(argv[1], "all")) {
        for (int i = 0; i < g_nloaded && g_npending < 8; i++)
            snprintf(g_pending[g_npending++], sizeof g_pending[0], "reload %s", g_loaded[i].name);
        return;
    }
    queue("reload", argc, argv);
}
static void cmd_unload(int argc, const char **argv) { queue("unload", argc, argv); }
static void cmd_load(int argc, const char **argv) { queue("load", argc, argv); }

static void remove_stale_copies(void) {
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/.reload", g_game_dir);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char sub[PATH_MAX];
        snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name);
        DIR *s = opendir(sub);
        struct dirent *f;
        while (s && (f = readdir(s))) {
            if (f->d_name[0] == '.') continue;
            char p[PATH_MAX];
            snprintf(p, sizeof p, "%s/%s", sub, f->d_name);
            unlink(p);
        }
        if (s) closedir(s);
        rmdir(sub);
    }
    closedir(d);
    rmdir(dir);
}

__attribute__((constructor)) static void loader_main(void) {
    char exe[PATH_MAX];
    uint32_t sz = sizeof exe;
    if (_NSGetExecutablePath(exe, &sz) != 0) return;
    char real[PATH_MAX];
    if (!realpath(exe, real)) snprintf(real, sizeof real, "%s", exe);
    char *slash = strrchr(real, '/'); // .../Contents/MacOS/macVC
    if (slash) *slash = 0;
    snprintf(g_game_dir, sizeof g_game_dir, "%s/../Resources/GameData", real);
    char tmp[PATH_MAX];
    if (realpath(g_game_dir, tmp)) snprintf(g_game_dir, sizeof g_game_dir, "%s", tmp);

    log_open(g_game_dir);
    log_capture_stdio();
    crash_init(g_game_dir);
    read_ini();

    time_t now = time(NULL);
    char ts[64];
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&now));
    mvc_log("[Loader] macVC Plugin Loader %s (plugin API %d) - %s", MVC_LOADER_VERSION, MVC_API_VERSION, ts);
    mvc_log("[Loader] game directory: %s", g_game_dir);

    const char *env = getenv("MVC_SAFE_MODE");
    // (no keyboard check here: talking to the window server before the game
    //  creates its window stops macOS from hiding the mouse cursor)
    char marker[PATH_MAX];
    snprintf(marker, sizeof marker, "%s/SAFE_MODE", g_game_dir);
    if ((env && atoi(env)) || access(marker, F_OK) == 0) g_safe_mode = 1;

    if (!game_init()) {
        mvc_log_error("[Loader] this macVC build is not supported - plugins not loaded");
        hooks_set_startup(0);
        return;
    }
    mvc_log("[Loader] core hooks installed. Console key: %d", g_cfg.console_key);
    remove_stale_copies();
    mvc_console_command("reload", "reload <plugin>|all - swap in a rebuilt plugin without restarting", cmd_reload);
    mvc_console_command("unload", "unload <plugin> - undo a plugin's hooks and stop it", cmd_unload);
    mvc_console_command("load", "load <plugin> - load a plugin from plugins/ or scripts/", cmd_load);

    limits_init();
    movies_init();
    if (!g_safe_mode) modloader_init();
    if (!g_safe_mode) cleo_init();
    if (g_safe_mode) {
        mvc_log_warn("[Loader] SAFE MODE: no plugins loaded (Safe Mode.command, MVC_SAFE_MODE, a GameData/SAFE_MODE file, or SafeMode=1)");
    } else {
        collect("plugins");
        collect("scripts");
        for (int i = 0; i < g_nextra; i++) collect(g_extra_dirs[i]);
        qsort(g_cand, (size_t)g_ncand, sizeof *g_cand, cand_cmp);
        for (int i = 0; i < g_ncand; i++) load_one(&g_cand[i]);
    }
    if (!g_safe_mode) modloader_menu();
    g_min_log_level = g_cfg.log_level;
    hooks_set_startup(0);
    mvc_log("[Loader] %d plugin(s) loaded, %d disabled, %d failed, %d patches/hooks active",
            g_plugins_loaded, g_plugins_disabled, g_plugins_failed, hook_count());
}
