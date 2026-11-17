// custom game text, plugin keys and fxt files, served through a CText::Get hook so the
// game's own help boxes, menus and mission text can use them.
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "internal.h"

typedef struct { char key[8]; uint16_t *text; } Entry;
static Entry *g_tab;
static int g_n, g_cap;
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static uint16_t *(*o_TextGet)(void *self, const char *key);
static void *g_TheText;

static uint16_t *widen(const char *s) {
    size_t n = strlen(s);
    uint16_t *w = malloc((n + 1) * sizeof *w);
    for (size_t i = 0; i < n; i++) w[i] = (unsigned char)s[i];
    w[n] = 0;
    return w;
}

static Entry *find(const char *key) {
    for (int i = 0; i < g_n; i++)
        if (!strncmp(g_tab[i].key, key, 7)) return &g_tab[i];
    return NULL;
}

void mvc_text_set(const char *key, const char *ascii) {
    if (!key || !key[0] || !ascii) return;
    pthread_mutex_lock(&g_mx);
    Entry *e = find(key);
    if (!e) {
        if (g_n == g_cap) {
            g_cap = g_cap ? g_cap * 2 : 256;
            g_tab = realloc(g_tab, (size_t)g_cap * sizeof *g_tab);
        }
        e = &g_tab[g_n++];
        memset(e->key, 0, sizeof e->key);
        strncpy(e->key, key, 7);
        e->text = NULL;
    }
    // keep the old buffer alive: the game may still hold a pointer to it
    e->text = widen(ascii);
    pthread_mutex_unlock(&g_mx);
}

// Mod Loader text: strings a mod's .gxt changes, per language file, applied on
// top of macVC's own text instead of replacing the whole file.
typedef struct { char lang[24]; char key[8]; uint16_t *text; } ModText;
static ModText *g_mt;
static int g_nmt, g_capmt;
static char g_lang[24] = "american.gxt";

void text_mod_clear(void) { // buffers are kept: the game may still hold a pointer
    pthread_mutex_lock(&g_mx);
    g_nmt = 0;
    pthread_mutex_unlock(&g_mx);
}
void text_mod_language(const char *gxt_file) {
    pthread_mutex_lock(&g_mx);
    snprintf(g_lang, sizeof g_lang, "%s", gxt_file);
    pthread_mutex_unlock(&g_mx);
}
void text_mod_set(const char *lang, const char *key, const uint16_t *w) {
    size_t n = 0;
    while (w[n]) n++;
    uint16_t *copy = malloc((n + 1) * sizeof *copy);
    memcpy(copy, w, (n + 1) * sizeof *copy);
    pthread_mutex_lock(&g_mx);
    if (g_nmt == g_capmt) { g_capmt = g_capmt ? g_capmt * 2 : 256; g_mt = realloc(g_mt, (size_t)g_capmt * sizeof *g_mt); }
    ModText *m = &g_mt[g_nmt++];
    snprintf(m->lang, sizeof m->lang, "%s", lang);
    memset(m->key, 0, sizeof m->key);
    strncpy(m->key, key, 7);
    m->text = copy;
    pthread_mutex_unlock(&g_mx);
}

static uint16_t *h_TextGet(void *self, const char *key) {
    if (key) {
        pthread_mutex_lock(&g_mx);
        Entry *e = find(key);
        uint16_t *t = e ? e->text : NULL;
        for (int i = 0; !t && i < g_nmt; i++)
            if (!strncmp(g_mt[i].key, key, 7) && !strcasecmp(g_mt[i].lang, g_lang)) t = g_mt[i].text;
        pthread_mutex_unlock(&g_mx);
        if (t) return t;
    }
    return o_TextGet(self, key);
}

const uint16_t *mvc_text_get(const char *key) {
    if (!o_TextGet || !g_TheText) return NULL;
    return h_TextGet(g_TheText, key);
}

int mvc_text_load_fxt(const char *path) {
    char full[PATH_MAX];
    if (path[0] == '/') snprintf(full, sizeof full, "%s", path);
    else snprintf(full, sizeof full, "%s/%s", g_game_dir, path);
    FILE *f = fopen(full, "r");
    if (!f) return 0;
    char line[1024];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (!*p || *p == '#' || *p == ';' || (p[0] == '/' && p[1] == '/')) continue;
        char key[16] = {0};
        int k = 0;
        while (*p && !isspace((unsigned char)*p) && k < 15) key[k++] = *p++;
        while (*p == ' ' || *p == '\t') p++;
        char *e = p + strlen(p);
        while (e > p && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
        if (k > 7) { mvc_log_warn("[Loader] %s: key \"%s\" is longer than 7 characters - skipped", path, key); continue; }
        mvc_text_set(key, p);
        n++;
    }
    fclose(f);
    return n;
}

static void load_fxt_dir(const char *sub) {
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/%s", g_game_dir, sub);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t L = strlen(e->d_name);
        if (L > 4 && !strcasecmp(e->d_name + L - 4, ".fxt")) {
            char rel[PATH_MAX];
            snprintf(rel, sizeof rel, "%s/%s", sub, e->d_name);
            int n = mvc_text_load_fxt(rel);
            mvc_log("[Loader] text: %d keys from %s", n, rel);
        }
    }
    closedir(d);
}

int text_init(void) {
    g_TheText = mvc_sym("TheText");
    if (mvc_hook_sym("_ZN5CText3GetEPKc", (void *)h_TextGet, (void **)&o_TextGet)) return 0;
    const char *dirs[] = {"text", "plugins", "scripts", "CLEO/CLEO_TEXT"};
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) load_fxt_dir(dirs[i]);
    return 1;
}
