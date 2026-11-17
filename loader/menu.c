// Options > Mods: real pause menu pages, built with reVC's custom frontend option api
// (FrontendScreenAdd / FrontendOptionAdd*) that macVC ships with.
// also the little ini read/write helpers for saving settings.
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "internal.h"

// ini helpers
static void ini_path(const char *ini, char *out, size_t n) {
    if (ini[0] == '/') snprintf(out, n, "%s", ini);
    else snprintf(out, n, "%s/%s", g_game_dir, ini);
}

static void strip(char *s) {
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    char *b = s;
    while (isspace((unsigned char)*b)) b++;
    if (b != s) memmove(s, b, strlen(b) + 1);
}

int mvc_ini_get(const char *ini, const char *section, const char *key, char *out, size_t cap) {
    char path[PATH_MAX], line[1024], sec[128] = "";
    ini_path(ini, path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        strip(line);
        if (line[0] == '[' && !strchr(line, '=')) {
            char *e = strchr(line, ']');
            if (e) *e = 0;
            snprintf(sec, sizeof sec, "%s", line + 1);
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq || line[0] == ';') continue;
        *eq = 0;
        strip(line);
        if (!strcasecmp(sec, section) && !strcasecmp(line, key)) {
            char *v = eq + 1;
            strip(v);
            snprintf(out, cap, "%s", v);
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

int mvc_ini_set(const char *ini, const char *section, const char *key, const char *value) {
    char path[PATH_MAX];
    ini_path(ini, path, sizeof path);
    char **lines = NULL;
    int n = 0, cap = 0;
    FILE *f = fopen(path, "r");
    char buf[1024];
    if (f) {
        while (fgets(buf, sizeof buf, f)) {
            if (n == cap) { cap = cap ? cap * 2 : 64; lines = realloc(lines, (size_t)cap * sizeof *lines); }
            lines[n++] = strdup(buf);
        }
        fclose(f);
    }
    int insec = 0, secEnd = -1, done = 0;
    for (int i = 0; i < n && !done; i++) {
        char t[1024];
        snprintf(t, sizeof t, "%s", lines[i]);
        strip(t);
        if (t[0] == '[' && !strchr(t, '=')) { // "[VC]Gasoline=0" is a setting
            if (insec) { secEnd = i; break; }
            char *e = strchr(t, ']');
            if (e) *e = 0;
            insec = !strcasecmp(t + 1, section);
            continue;
        }
        if (!insec || t[0] == ';') continue;
        char *eq = strchr(t, '=');
        if (!eq) continue;
        *eq = 0;
        strip(t);
        if (!strcasecmp(t, key)) {
            char nl[1024];
            snprintf(nl, sizeof nl, "%s=%s\n", key, value);
            free(lines[i]);
            lines[i] = strdup(nl);
            done = 1;
        }
    }
    f = fopen(path, "w");
    if (!f) return 0;
    if (done) {
        for (int i = 0; i < n; i++) fputs(lines[i], f);
    } else if (insec) {
        int at = secEnd < 0 ? n : secEnd;
        for (int i = 0; i < at; i++) fputs(lines[i], f);
        fprintf(f, "%s=%s\n", key, value);
        for (int i = at; i < n; i++) fputs(lines[i], f);
    } else {
        for (int i = 0; i < n; i++) fputs(lines[i], f);
        size_t last = n ? strlen(lines[n - 1]) : 0; // 0 for a line that starts with a NUL byte
        if (n && (!last || lines[n - 1][last - 1] != '\n')) fputc('\n', f);
        fprintf(f, "\n[%s]\n%s=%s\n", section, key, value);
    }
    fclose(f);
    for (int i = 0; i < n; i++) free(lines[i]);
    free(lines);
    return 1;
}

// Options > Mods (native)
// macVC's menu table is full (45 of 45 pages), and FrontendScreenAdd cannot
// make new pages. page 34 is reVC's leftover Debug page: nothing in the menus
// links to it and no game code switches to it, so the loader takes it over as
// the Mods page. Options gets a real MODS row that opens it. every plugin
// gets a section header followed by its settings as native menu rows, so the
// mouse, controller and Back all work exactly like the game's own pages.
static void (*f_SetCursor)(int, int8_t, _Bool);
static void (*f_AddSelect)(const char *, uint16_t, uint16_t, uint8_t, const char **, int8_t, int8_t *, _Bool,
                           void (*)(int8_t, int8_t), const char *, const char *, _Bool);
static void (*f_AddBuiltin)(const char *, uint16_t, uint16_t, uint8_t, int, int, int);
static void (*o_Populate)(void);
static uint8_t *g_aScreens;

#define SCREEN_STRIDE 0x2F0       // sizeof(CMenuScreen) in macVC (validated at runtime)
#define MAX_SCREENS 45
#define ENTRY_STRIDE 0x28
#define ENTRY_FIRST 0x20          // entries start at screen + 0x20
#define MAX_ROWS 18
#define MODS_PAGE 34              // the unused Debug page (checked: name FED_DBG)
#define ACTION_CHANGEMENU 4
#define ACTION_GOBACK 35

enum { OPT_TOGGLE, OPT_CHOICE, OPT_NUMBER, OPT_KEY };
#define MAX_CHOICES 127           // a select row holds up to 127 values (int8)
typedef struct {
    int kind, page;
    char label_key[8];
    int *ival;
    float *fval;
    float min, step;
    char fmt[16];
    int count;
    char choice_keys[MAX_CHOICES][8];
    const char *choice_ptrs[MAX_CHOICES];
    int values[MAX_CHOICES];      // key codes for key binds
    int8_t shadow;                // the row's current index, written by the game
    char ini[256], sec[64], key[64];
    mvc_menu_changed cb;
} Opt;
typedef struct { char title_key[8]; char title[48]; char owner[64]; int orphan, next; } Page; // orphan: owner was unloaded
                                   // next: its continuation page (+1), when it has more rows than fit
#define PAGE_FIT 10                // settings that fit on screen under the MOD row with Back still visible

#define MAX_OPTS 512               // settings rows across every page (plugins and Mod Loader mods)
#define MAX_PAGES MAX_CHOICES     // every plugin is one value of the MOD row
static Opt g_opts[MAX_OPTS];
static Page g_pages[MAX_PAGES];
static int g_nopts, g_npages, g_keyn, g_built_npages = -1;
static int8_t g_sel_shadow;   // the MOD selector row
static int g_cur_plugin;

static void make_key(char out[8], const char *text) {
    snprintf(out, 8, "MV%05d", g_keyn++);
    char up[96];
    snprintf(up, sizeof up, "%s", text);
    for (char *c = up; *c; c++) *c = (char)toupper((unsigned char)*c);
    mvc_text_set(out, up);
}

int mvc_menu_page(const char *title) {
    if (!title) return -1;
    char owner[64], up[48];
    owner_name(__builtin_return_address(0), owner, sizeof owner);
    snprintf(up, sizeof up, "%s", title);
    for (char *c = up; *c; c++) *c = (char)toupper((unsigned char)*c);
    // a reloaded plugin gets its old page (and its place in the MOD selector) back
    for (int p = 0; p < g_npages; p++)
        if (g_pages[p].orphan && !strcmp(g_pages[p].owner, owner) && !strcmp(g_pages[p].title, up)) {
            g_pages[p].orphan = 0;
            return p;
        }
    if (g_npages >= MAX_PAGES) return -1;
    make_key(g_pages[g_npages].title_key, title);
    snprintf(g_pages[g_npages].title, sizeof g_pages[0].title, "%s", up);
    snprintf(g_pages[g_npages].owner, sizeof g_pages[0].owner, "%s", owner);
    g_pages[g_npages].orphan = 0;
    g_pages[g_npages].next = 0;
    return g_npages++;
}

// a page with PAGE_FIT rows continues on "TITLE (2)", "(3)"..., each its own value of the MOD row
static int fit_page(int page) {
    for (int guard = 0; guard < 16; guard++) {
        int n = 0;
        for (int i = 0; i < g_nopts; i++) if (g_opts[i].page == page) n++;
        if (n < PAGE_FIT) return page;
        if (g_pages[page].next) { page = g_pages[page].next - 1; continue; }
        if (g_npages >= MAX_PAGES) return -1;
        char title[64];
        const char *base = g_pages[page].title;
        int num = 2;
        const char *open = strstr(base, " (");
        if (open && sscanf(open, " (%d)", &num) == 1) num++;
        snprintf(title, sizeof title, "%.*s (%d)", open ? (int)(open - base) : (int)strlen(base), base, num);
        Page *np = &g_pages[g_npages];
        make_key(np->title_key, title);
        snprintf(np->title, sizeof np->title, "%s", title);
        snprintf(np->owner, sizeof np->owner, "%s", g_pages[page].owner);
        np->orphan = 0;
        np->next = 0;
        g_pages[page].next = g_npages + 1;
        page = g_npages++;
    }
    return -1;
}

static Opt *new_opt(int page, int kind, const char *label, const char *ini, const char *sec, const char *key,
                    mvc_menu_changed cb) {
    if (page < 0 || page >= g_npages || g_nopts >= MAX_OPTS || !label) return NULL;
    page = fit_page(page);
    if (page < 0) return NULL;
    Opt *o = &g_opts[g_nopts++];
    memset(o, 0, sizeof *o);
    o->kind = kind;
    o->page = page;
    make_key(o->label_key, label);
    if (ini) snprintf(o->ini, sizeof o->ini, "%s", ini);
    if (sec) snprintf(o->sec, sizeof o->sec, "%s", sec);
    if (key) snprintf(o->key, sizeof o->key, "%s", key);
    o->cb = cb;
    return o;
}

int mvc_menu_toggle(int page, const char *label, int *value, const char *ini, const char *sec, const char *key,
                    mvc_menu_changed cb) {
    Opt *o = new_opt(page, OPT_TOGGLE, label, ini, sec, key, cb);
    if (!o || !value) return -1;
    o->ival = value;
    o->count = 2;
    snprintf(o->choice_keys[0], 8, "FEM_OFF");
    snprintf(o->choice_keys[1], 8, "FEM_ON");
    return (int)(o - g_opts);
}

int mvc_menu_choice(int page, const char *label, const char *const *choices, int count, int *value,
                    const char *ini, const char *sec, const char *key, mvc_menu_changed cb) {
    Opt *o = new_opt(page, OPT_CHOICE, label, ini, sec, key, cb);
    if (!o || !value || count < 1) return -1;
    o->ival = value;
    o->count = count > MAX_CHOICES ? MAX_CHOICES : count;
    for (int i = 0; i < o->count; i++) make_key(o->choice_keys[i], choices[i]);
    return (int)(o - g_opts);
}

// numbers become a select row listing every value from min to max
int mvc_menu_number(int page, const char *label, float *value, float min, float max, float step,
                    const char *fmt, const char *ini, const char *sec, const char *key, mvc_menu_changed cb) {
    Opt *o = new_opt(page, OPT_NUMBER, label, ini, sec, key, cb);
    if (!o || !value || max < min) return -1;
    if (step <= 0) step = 1;
    int n = (int)((max - min) / step + 1.5f);
    while (n > MAX_CHOICES) { step *= 2; n = (int)((max - min) / step + 1.5f); }
    o->fval = value;
    o->min = min;
    o->step = step;
    o->count = n;
    snprintf(o->fmt, sizeof o->fmt, "%s", fmt ? fmt : "%.0f");
    for (int i = 0; i < n; i++) {
        char t[48];
        snprintf(t, sizeof t, o->fmt, min + step * i);
        make_key(o->choice_keys[i], t);
    }
    return (int)(o - g_opts);
}

// key binds become a select row of keys
static const int BINDABLE[] = {
    'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
    '0','1','2','3','4','5','6','7','8','9', 290,291,292,293,294,295,296,297,298,299,300,301,
    32, 258, 340, 341, 342, 344, 345, 346, 260, 261, 268, 269, 266, 267, 59, 39, 44, 46, 47, 91, 93, 45, 61, 92,
    320,321,322,323,324,325,326,327,328,329, 0};

int mvc_menu_keybind(int page, const char *label, int *glfw_key, const char *ini, const char *sec,
                     const char *key, mvc_menu_changed cb) {
    Opt *o = new_opt(page, OPT_KEY, label, ini, sec, key, cb);
    if (!o || !glfw_key) return -1;
    o->ival = glfw_key;
    int n = 0, found = 0;
    for (int i = 0; BINDABLE[i] && n < MAX_CHOICES - 1; i++) {
        o->values[n] = BINDABLE[i];
        if (BINDABLE[i] == *glfw_key) found = 1;
        make_key(o->choice_keys[n++], mvc_key_name(BINDABLE[i]));
    }
    if (!found) { o->values[n] = *glfw_key; make_key(o->choice_keys[n++], mvc_key_name(*glfw_key)); }
    o->count = n;
    return (int)(o - g_opts);
}

static int current_index(Opt *o) {
    switch (o->kind) {
    case OPT_TOGGLE: return *o->ival ? 1 : 0;
    case OPT_CHOICE: return (*o->ival >= 0 && *o->ival < o->count) ? *o->ival : 0;
    case OPT_NUMBER: {
        int k = (int)((*o->fval - o->min) / o->step + 0.5f);
        return k < 0 ? 0 : k >= o->count ? o->count - 1 : k;
    }
    default:
        for (int i = 0; i < o->count; i++) if (o->values[i] == *o->ival) return i;
        return 0;
    }
}

static void apply_index(Opt *o, int k) {
    char v[64];
    switch (o->kind) {
    case OPT_TOGGLE: *o->ival = k ? 1 : 0; snprintf(v, sizeof v, "%d", *o->ival); break;
    case OPT_CHOICE: *o->ival = k; snprintf(v, sizeof v, "%d", k); break;
    case OPT_NUMBER: *o->fval = o->min + o->step * k; snprintf(v, sizeof v, "%g", *o->fval); break;
    default: *o->ival = o->values[k]; snprintf(v, sizeof v, "%s", mvc_key_name(*o->ival)); break;
    }
    if (o->ini[0] && o->sec[0] && o->key[0]) mvc_ini_set(o->ini, o->sec, o->key, v);
    if (o->cb) o->cb();
}

// the game's select callback carries no context, so find the row(s) that changed.
static void select_changed(int8_t before, int8_t after) {
    (void)before; (void)after;
    for (int i = 0; i < g_nopts; i++) {
        Opt *o = &g_opts[i];
        if (o->shadow != current_index(o) && o->shadow >= 0 && o->shadow < o->count) apply_index(o, o->shadow);
    }
}

// keep the rows showing the right value if a plugin changes a setting in code
void menu_frame(void) {
    for (int i = 0; i < g_nopts; i++) g_opts[i].shadow = (int8_t)current_index(&g_opts[i]);
    g_sel_shadow = (int8_t)g_cur_plugin;
}

int menu_overlay_open(void) { return 0; }
int menu_overlay_key(int key, int action) { (void)key; (void)action; return 0; }

// building the page
static uint8_t *screen(int s) { return g_aScreens + (size_t)s * SCREEN_STRIDE; }

static int find_entry(const char *name, int *row) {
    for (int s = 0; s < MAX_SCREENS; s++)
        for (int r = 0; r < MAX_ROWS; r++)
            if (!strncmp((char *)screen(s) + ENTRY_FIRST + r * ENTRY_STRIDE + 4, name, 8)) { *row = r; return s; }
    return -1;
}

static int rows_used(int s) {
    int n = 0;
    for (int r = 0; r < MAX_ROWS; r++)
        if (screen(s)[ENTRY_FIRST + r * ENTRY_STRIDE + 4]) n = r + 1;
    return n;
}

// the Mods page: row 0 is a MOD selector (one value per plugin); the rows
// under it are that plugin's settings, rebuilt whenever the selector changes.
// any number of plugins fit; each gets up to MAX_ROWS - 2 settings.
static const char *g_sel_ptrs[MAX_PAGES];

static void rebuild_rows(void) {
    uint8_t *page = screen(MODS_PAGE);
    memset(page + ENTRY_FIRST + ENTRY_STRIDE, 0, (MAX_ROWS - 1) * ENTRY_STRIDE); // keep row 0
    int row = 1, shown = 0, total = 0;
    for (int i = 0; i < g_nopts; i++) {
        Opt *o = &g_opts[i];
        if (o->page != g_cur_plugin) continue;
        total++;
        if (row >= MAX_ROWS - 1) continue;
        for (int c = 0; c < o->count; c++) o->choice_ptrs[c] = o->choice_keys[c];
        o->shadow = (int8_t)current_index(o);
        f_SetCursor(MODS_PAGE, (int8_t)row++, 0);
        f_AddSelect(o->label_key, 0, 0, 1, (const char **)o->choice_ptrs, (int8_t)o->count, &o->shadow, 0,
                    select_changed, NULL, NULL, 0);
        shown++;
    }
    f_SetCursor(MODS_PAGE, (int8_t)row, 0);
    f_AddBuiltin("FEDS_TB", 0, 0, 1, ACTION_GOBACK, 0, 0);
    if (shown < total)
        mvc_log_warn("[Loader] Mods menu: %d of %d settings fit on the page for this plugin", shown, total);
}

// the page's big header shows the selected mod
static void set_header(void) { mvc_text_set("MVCPAGE", g_pages[g_cur_plugin].title); }

static void selector_changed(int8_t before, int8_t after) {
    (void)before; (void)after;
    if (g_sel_shadow >= 0 && g_sel_shadow < g_npages && g_sel_shadow != g_cur_plugin) {
        g_cur_plugin = g_sel_shadow;
        set_header();
        rebuild_rows();
    }
}

static void build(void) {
    int langRow, langScreen = find_entry("FEL_ENG", &langRow);
    int optRow, optScreen = find_entry("FEO_LAN", &optRow);
    uint8_t *page = screen(MODS_PAGE);
    if (langScreen != 5 || optScreen < 0 || strncmp((char *)page, "FED_DBG", 8)) {
        mvc_log_warn("[Loader] Mods menu: this macVC's menus are laid out differently - menu not added");
        return;
    }
    if (rows_used(optScreen) >= MAX_ROWS - 1) {
        mvc_log_warn("[Loader] Mods menu: the Options page is full - menu not added");
        return;
    }
    // take over the unused Debug page: the header shows the selected mod, back goes to Options
    mvc_text_set("MVCMODS", "MODS");
    mvc_text_set("MVCMOD", "MOD");
    memset(page, 0, 8);
    memcpy(page, "MVCPAGE", 7);
    *(int32_t *)(page + 8) = optScreen;
    *(void **)(page + 0x10) = NULL;   // default layout
    *(void **)(page + 0x18) = NULL;
    memset(page + ENTRY_FIRST, 0, MAX_ROWS * ENTRY_STRIDE);

    if (g_cur_plugin >= g_npages) g_cur_plugin = 0;
    set_header();
    // the MOD row shows "1 / 2"... in load order; the page header names the mod
    static char counter_keys[MAX_PAGES][8];
    for (int p = 0; p < g_npages; p++) {
        char t[16];
        snprintf(counter_keys[p], 8, "MVN%03d", p + 1);
        snprintf(t, sizeof t, "%d / %d", p + 1, g_npages);
        mvc_text_set(counter_keys[p], t);
        g_sel_ptrs[p] = counter_keys[p];
    }
    g_sel_shadow = (int8_t)g_cur_plugin;
    f_SetCursor(MODS_PAGE, 0, 0);
    f_AddSelect("MVCMOD", 0, 0, 1, g_sel_ptrs, (int8_t)g_npages, &g_sel_shadow, 0, selector_changed, NULL, NULL, 0);
    rebuild_rows();

    // the MODS row on the Options page, right under Language
    f_SetCursor(optScreen, (int8_t)(optRow + 1), 0);
    f_AddBuiltin("MVCMODS", 0, 0, 1, ACTION_CHANGEMENU, MODS_PAGE, 0);
    g_built_npages = g_npages;
    mvc_log("[Loader] Options > Mods: %d page(s), %d setting(s)", g_npages, g_nopts);
}

// hot reload
// drops the rows of `owner`'s pages (their value pointers go into its code).
// the pages stay, so a reload slots back into the same place.
int menu_release(const char *owner) {
    int w = 0, n = 0;
    for (int i = 0; i < g_nopts; i++) {
        if (!strcmp(g_pages[g_opts[i].page].owner, owner)) { n++; continue; }
        if (w != i) g_opts[w] = g_opts[i];
        w++;
    }
    g_nopts = w;
    for (int p = 0; p < g_npages; p++)
        if (!strcmp(g_pages[p].owner, owner)) g_pages[p].orphan = 1;
    return n;
}

// re-points the game's rows at the current settings after a reload
void menu_refresh(void) {
    if (g_built_npages < 0) return; // not built yet: it will be, with everything in it
    rebuild_rows();                 // the select records point into g_opts, which moved
    if (g_npages != g_built_npages)
        mvc_log_warn("[Loader] a new Mods menu page was added - it shows up after restarting the game");
}

static void dump_rows(int s) {
    for (int r = 0; r < MAX_ROWS; r++) {
        uint8_t *e = screen(s) + ENTRY_FIRST + r * ENTRY_STRIDE;
        if (!e[4]) continue;
        char hex[160] = "";
        for (int k = 0; k < ENTRY_STRIDE; k += 4) { char t[16]; snprintf(t, sizeof t, "%08x ", *(uint32_t *)(e + k)); strcat(hex, t); }
        mvc_log("[MenuDump] %d.%d %-8.8s %s", s, r, (char *)e + 4, hex);
        int32_t act = *(int32_t *)e;
        if (act == -1 || act == -2) {
            uint8_t *rec = *(uint8_t **)(e + 0x10);
            char rh[300] = "";
            for (int k = 0; k < 0x48; k += 8) { char t[24]; snprintf(t, sizeof t, "%llx ", *(unsigned long long *)(rec + k)); strcat(rh, t); }
            mvc_log("[MenuDump]     record %p: %s", (void *)rec, rh);
        }
    }
}

static void dump_shadows(void) {
    for (int i = 0; i < g_nopts; i++) mvc_log("[MenuDump] opt %d shadow at %p (%d values)", i, (void *)&g_opts[i].shadow, g_opts[i].count);
}

// vibration row
// the PC menus dropped the console versions' Vibration option, but the game
// still keeps the setting (macVC.ini Vibration=) and checks it before every
// shake. a row on Controller Setup (above Back) switches it, using the game's
// own "Vibration" text; the loader turns the shakes into real rumble.
static int8_t *g_vib;
static void (*f_SaveSettings)(void *);

static void find_vibration_pref(void) {
    uint8_t *menu = mvc_sym("FrontEndMenuManager");
    uint32_t *c = mvc_sym("_ZN4CPad10StartShakeEsh");
    if (!menu || !c) return;
    uintptr_t base = 0;
    for (int i = 0; i < 12; i++) {
        uint32_t in = c[i];
        if ((in & 0x9F000000) == 0x90000000) { // ADRP
            uint64_t raw = (((uint64_t)(in >> 5) & 0x7FFFF) << 2) | ((in >> 29) & 3); // 21-bit page count
            int64_t imm = ((int64_t)(raw << 43) >> 43) * 4096;
            base = ((uintptr_t)&c[i] & ~0xFFFUL) + (uintptr_t)imm;
        } else if ((in & 0xFFC00000) == 0x91000000 && base) base += (in >> 10) & 0xFFF; // ADD imm
        else if ((in & 0xFFC00000) == 0x39400000 && base) {                          // LDRB [xN, #imm]
            if (base == (uintptr_t)menu) g_vib = (int8_t *)(menu + ((in >> 10) & 0xFFF));
            return;
        }
    }
}

static void vibration_changed(int8_t before, int8_t after) {
    (void)before; (void)after;
    if (f_SaveSettings) f_SaveSettings(mvc_sym("FrontEndMenuManager"));
    if (*g_vib) haptics_play(game_joystick_name(), 0.6f, 250); // a short buzz to show it works
}

static void add_vibration_row(void) {
    static const char *onoff[2] = {"FEM_OFF", "FEM_ON"};
    int r, s = find_entry("FEC_RED", &r);
    if (s < 0 || !g_vib) return;
    int n = rows_used(s);
    if (n >= MAX_ROWS - 1) return;
    int back = n - 1; // the last row is Back: insert above it
    if (*(int32_t *)(screen(s) + ENTRY_FIRST + back * ENTRY_STRIDE) != ACTION_GOBACK) back = n;
    if (*g_vib != 0 && *g_vib != 1) *g_vib = 0;
    f_SetCursor(s, (int8_t)back, 0);
    f_AddSelect("FEC_VIB", 0, 0, 1, onoff, 2, g_vib, 0, vibration_changed, NULL, NULL, 0);
}

static void h_Populate(void) {
    o_Populate();
    int r;
    if (g_npages && find_entry("MVCMODS", &r) < 0) build();
    if (g_cfg.game_rumble && find_entry("FEC_VIB", &r) < 0) add_vibration_row();
    if (getenv("MVC_MENU_DUMP")) { dump_rows(MODS_PAGE); dump_shadows(); }
}

int menu_init(void) {
    int ok = 1;
#define R(v, n) do { *(void **)&(v) = mvc_sym(n); if (!(v)) { mvc_log_warn("[Loader] Mods menu unavailable: missing %s", n); ok = 0; } } while (0)
    R(f_SetCursor, "_Z23FrontendOptionSetCursoriab");
    R(f_AddSelect, "_Z23FrontendOptionAddSelectPKctthPS0_aPabPFvaaES0_S0_b");
    R(f_AddBuiltin, "_Z30FrontendOptionAddBuiltinActionPKctthiii");
    R(g_aScreens, "aScreens");
#undef R
    if (!ok) return 0;
    *(void **)&f_SaveSettings = mvc_sym("_ZN12CMenuManager12SaveSettingsEv");
    find_vibration_pref();
    if (!g_vib) mvc_log_warn("[Loader] the game's vibration setting wasn't found - no Vibration row");
    return mvc_hook_sym("_Z29CustomFrontendOptionsPopulatev", (void *)h_Populate, (void **)&o_Populate) == 0;
}
