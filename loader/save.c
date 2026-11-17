// plugin save data, kept next to each save and never inside it:
//   ~/Library/Application Support/macVC/GTAVCsf<N>.b.mvc
// so taking out a plugin (or the whole loader) can't break a save.
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "internal.h"

typedef struct { char key[64]; uint8_t *data; uint32_t len; } Item;
static Item g_items[256];
static int g_nitems;
static int g_slot;

int mvc_save_slot(void) { return g_slot; }
void save_set_slot(int slot) { g_slot = slot; }

void save_clear(void) {
    for (int i = 0; i < g_nitems; i++) free(g_items[i].data);
    g_nitems = 0;
}

int mvc_save_set(const char *key, const void *data, size_t len) {
    if (!key || !key[0] || (len && !data) || len > (1u << 24)) return 0;
    Item *it = NULL;
    for (int i = 0; i < g_nitems; i++) if (!strcmp(g_items[i].key, key)) { it = &g_items[i]; break; }
    if (!it) {
        if (g_nitems >= 256) { mvc_log_error("[Loader] save data: too many keys"); return 0; }
        it = &g_items[g_nitems++];
        snprintf(it->key, sizeof it->key, "%s", key);
        it->data = NULL;
    }
    free(it->data);
    it->data = malloc(len ? len : 1);
    memcpy(it->data, data, len);
    it->len = (uint32_t)len;
    return 1;
}

int mvc_save_get(const char *key, void *out, size_t cap) {
    for (int i = 0; i < g_nitems; i++)
        if (!strcmp(g_items[i].key, key)) {
            size_t n = g_items[i].len < cap ? g_items[i].len : cap;
            if (out && n) memcpy(out, g_items[i].data, n);
            return (int)g_items[i].len;
        }
    return -1;
}

static void sidecar(int slot, char *out, size_t n) {
    const char *home = getenv("HOME");
    snprintf(out, n, "%s/Library/Application Support/macVC/GTAVCsf%d.b.mvc", home ? home : "", slot);
}

int save_write(int slot) {
    if (slot < 1) return 0;
    char path[PATH_MAX], tmp[PATH_MAX];
    sidecar(slot, path, sizeof path);
    if (!g_nitems) { unlink(path); return 1; }
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { mvc_log_error("[Loader] cannot write %s", path); return 0; }
    uint32_t n = (uint32_t)g_nitems;
    fwrite("MVCS\1\0\0\0", 1, 8, f);
    fwrite(&n, 4, 1, f);
    for (int i = 0; i < g_nitems; i++) {
        uint16_t kl = (uint16_t)strlen(g_items[i].key);
        fwrite(&kl, 2, 1, f);
        fwrite(g_items[i].key, 1, kl, f);
        fwrite(&g_items[i].len, 4, 1, f);
        fwrite(g_items[i].data, 1, g_items[i].len, f);
    }
    int ok = fclose(f) == 0;
    if (ok) rename(tmp, path);
    mvc_log("[Loader] plugin save data: %d item(s) -> slot %d", g_nitems, slot);
    return ok;
}

int save_read(int slot) {
    save_clear();
    if (slot < 1) return 0;
    char path[PATH_MAX];
    sidecar(slot, path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char magic[8];
    uint32_t n = 0;
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "MVCS", 4) || fread(&n, 4, 1, f) != 1 || n > 256) { fclose(f); return 0; }
    for (uint32_t i = 0; i < n; i++) {
        uint16_t kl;
        uint32_t len;
        char key[64] = {0};
        if (fread(&kl, 2, 1, f) != 1 || kl >= sizeof key || fread(key, 1, kl, f) != kl) break;
        if (fread(&len, 4, 1, f) != 1 || len > (1u << 24)) break;
        uint8_t *d = malloc(len ? len : 1);
        if (fread(d, 1, len, f) != len) { free(d); break; }
        mvc_save_set(key, d, len);
        free(d);
    }
    fclose(f);
    mvc_log("[Loader] plugin save data: %d item(s) <- slot %d", g_nitems, slot);
    return 1;
}
