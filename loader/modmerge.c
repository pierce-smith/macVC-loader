// three way line merge for text data files (gta_vc.dat, .ide, handling.cfg...), the way
// windows modloader does it. every mod's changes against the original get kept unless two
// mods change the same lines, then the higher priority one wins.
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "internal.h"

typedef struct { char **line; char **key; int n; } Text;

static void text_free(Text *t) {
    for (int i = 0; i < t->n; i++) { free(t->line[i]); free(t->key[i]); }
    free(t->line); free(t->key);
    memset(t, 0, sizeof *t);
}

// comparison key: no whitespace, lowercase, '/' for '\'. with fuzzy numbers,
// every number is compared rounded to 2 decimals: map editors re-save .ipl files
// with fewer digits, which would otherwise make every line look changed.
static int g_fuzzy;
static char *make_key(const char *s) {
    size_t cap = strlen(s) * 2 + 32, w = 0;
    char *k = malloc(cap);
    while (*s) {
        if (isspace((unsigned char)*s)) { s++; continue; }
        if (g_fuzzy && (isdigit((unsigned char)*s) || ((*s == '-' || *s == '.') && isdigit((unsigned char)s[1])))) {
            char *end;
            double v = strtod(s, &end);
            // only sane magnitudes are rounded; anything else (1e300, nan) is kept as typed
            if (end > s && (!*end || *end == ',' || isspace((unsigned char)*end)) && v > -1e12 && v < 1e12) {
                double r = (double)(long long)(v * 100 + (v < 0 ? -0.5 : 0.5)) / 100;
                if (r == 0) r = 0; // no "-0.00"
                while (w + 32 >= cap) k = realloc(k, cap *= 2);
                w += (size_t)snprintf(k + w, cap - w, "%.2f", r); // at most 17 characters
                s = end;
                continue;
            }
        }
        if (w + 2 >= cap) k = realloc(k, cap *= 2);
        k[w++] = *s == '\\' ? '/' : (char)tolower((unsigned char)*s);
        s++;
    }
    k[w] = 0;
    return k;
}

static int text_read(const char *path, Text *t) {
    memset(t, 0, sizeof *t);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char buf[2048];
    int cap = 0;
    while (fgets(buf, sizeof buf, f)) {
        size_t L = strlen(buf);
        while (L && (buf[L - 1] == '\n' || buf[L - 1] == '\r')) buf[--L] = 0;
        if (t->n == cap) {
            cap = cap ? cap * 2 : 256;
            t->line = realloc(t->line, (size_t)cap * sizeof *t->line);
            t->key = realloc(t->key, (size_t)cap * sizeof *t->key);
        }
        t->line[t->n] = strdup(buf);
        t->key[t->n++] = make_key(buf);
    }
    fclose(f);
    return 0;
}

// a change: base lines [a, b) become lines [c, d) of the mod's version
typedef struct { int a, b, c, d, ver, prio, order, accepted; } Hunk;

static int diff(const Text *A, const Text *B, int ver, int prio, int order, Hunk **out, int *n, int *cap) {
    int n1 = A->n, n2 = B->n;
    if ((long long)(n1 + 1) * (n2 + 1) > 16000000LL) return -1; // too big to align (64 MB)
    int *L = calloc((size_t)(n1 + 1) * (size_t)(n2 + 1), sizeof *L);
    if (!L) return -1;
#define LCS(i, j) L[(size_t)(i) * (size_t)(n2 + 1) + (size_t)(j)]
    for (int i = n1 - 1; i >= 0; i--)
        for (int j = n2 - 1; j >= 0; j--)
            LCS(i, j) = !strcmp(A->key[i], B->key[j]) ? LCS(i + 1, j + 1) + 1
                       : LCS(i + 1, j) > LCS(i, j + 1) ? LCS(i + 1, j) : LCS(i, j + 1);
    int i = 0, j = 0;
    while (i < n1 || j < n2) {
        if (i < n1 && j < n2 && !strcmp(A->key[i], B->key[j])) { i++; j++; continue; }
        int a = i, c = j;
        while ((i < n1 || j < n2) && !(i < n1 && j < n2 && !strcmp(A->key[i], B->key[j]))) {
            if (j < n2 && (i == n1 || LCS(i, j + 1) >= LCS(i + 1, j))) j++;
            else i++;
        }
        if (*n == *cap) { *cap = *cap ? *cap * 2 : 64; *out = realloc(*out, (size_t)*cap * sizeof **out); }
        (*out)[(*n)++] = (Hunk){a, i, c, j, ver, prio, order, 0};
    }
#undef LCS
    free(L);
    return 0;
}

static int hunk_cmp_prio(const void *x, const void *y) {
    const Hunk *a = x, *b = y;
    if (a->prio != b->prio) return b->prio - a->prio;  // higher priority first
    return a->order - b->order;                        // then the first mod alphabetically
}
static int hunk_cmp_pos(const void *x, const void *y) {
    const Hunk *a = x, *b = y;
    if (a->a != b->a) return a->a - b->a;
    if ((a->b == a->a) != (b->b == b->a)) return (a->b == a->a) ? -1 : 1; // insertions before a replacement
    return b->prio != a->prio ? a->prio - b->prio : a->order - b->order;
}

static int same_change(const Hunk *x, const Text *vx, const Hunk *y, const Text *vy) {
    if (x->a != y->a || x->b != y->b || x->d - x->c != y->d - y->c) return 0;
    for (int k = 0; k < x->d - x->c; k++) if (strcmp(vx->key[x->c + k], vy->key[y->c + k])) return 0;
    return 1;
}

// merges versions[0..n) of `base` into `out`. names[] are used in messages.
// returns the number of conflicting changes that were dropped, or -1.
int merge_text(const char *base, const char *const *versions, const int *prio, const char *const *names, int n,
               const char *out, const char *what) {
    size_t wl = strlen(what);
    g_fuzzy = wl > 4 && !strcasecmp(what + wl - 4, ".ipl");
    Text B, *V = calloc((size_t)n, sizeof *V);
    if (text_read(base, &B)) { free(V); return -1; }
    Hunk *h = NULL;
    int nh = 0, caph = 0, rc = 0;
    for (int v = 0; v < n; v++) {
        if (text_read(versions[v], &V[v]) || diff(&B, &V[v], v, prio[v], v, &h, &nh, &caph)) { rc = -1; goto done; }
    }
    // accept changes from the highest priority down; skip ones that overlap an accepted one
    qsort(h, (size_t)nh, sizeof *h, hunk_cmp_prio);
    int dropped = 0;
    for (int x = 0; x < nh; x++) {
        int ok = 1, dup = 0;
        for (int y = 0; y < x && ok; y++) {
            if (!h[y].accepted) continue;
            if (same_change(&h[x], &V[h[x].ver], &h[y], &V[h[y].ver])) { dup = 1; break; }
            int xa = h[x].a, xb = h[x].b, ya = h[y].a, yb = h[y].b;
            int overlap = (xb > xa && yb > ya) ? (xa < yb && ya < xb)          // two replacements
                        : (xb == xa && yb > ya) ? (xa > ya && xa < yb)          // insertion inside a replacement
                        : (yb == ya && xb > xa) ? (ya > xa && ya < xb) : 0;
            if (overlap) {
                ok = 0;
                mvc_log_warn("[ModLoader] %s: %s and %s change the same lines (%d-%d) - using %s's", what, names[h[y].ver],
                             names[h[x].ver], xa + 1, xb > xa ? xb : xa + 1, names[h[y].ver]);
            }
        }
        if (dup) continue;
        if (ok) h[x].accepted = 1; else dropped++;
    }
    qsort(h, (size_t)nh, sizeof *h, hunk_cmp_pos);
    FILE *f = fopen(out, "w");
    if (!f) { rc = -1; goto done; }
    int k = 0;
    for (int i = 0; i <= B.n;) {
        int next = i + 1, replaced = 0;
        for (; k < nh && h[k].a == i; k++) {
            if (!h[k].accepted) continue;
            for (int l = h[k].c; l < h[k].d; l++) fprintf(f, "%s\n", V[h[k].ver].line[l]);
            if (h[k].b > h[k].a) { next = h[k].b; replaced = 1; k++; break; } // skip the base lines it replaced
        }
        if (!replaced && i < B.n) fprintf(f, "%s\n", B.line[i]);
        while (k < nh && h[k].a < next) k++; // dropped changes inside the replaced range
        i = next;
    }
    if (fclose(f)) rc = -1;
    else rc = dropped;
done:
    text_free(&B);
    for (int v = 0; v < n; v++) text_free(&V[v]);
    free(V);
    free(h);
    return rc;
}

// collision archives
// a .col file is a list of named entries ("COLL", size, name[22], data). Mods'
// added or changed entries are combined; when two mods change the same entry
// differently, the higher priority (then the first alphabetically) wins.
typedef struct { char name[24]; const uint8_t *blob; uint32_t len; int owner; } ColEntry;

static uint8_t *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = n > 0 ? malloc((size_t)n) : NULL;
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; }
    fclose(f);
    *len = d ? (size_t)n : 0;
    return d;
}

static int col_parse(const uint8_t *d, size_t len, ColEntry **out, int owner) {
    int n = 0, cap = 0;
    for (size_t i = 0; i + 30 <= len;) {
        if (memcmp(d + i, "COL", 3)) break;
        uint32_t size;
        memcpy(&size, d + i + 4, 4);
        if (i + 8 + size > len) break;
        if (n == cap) { cap = cap ? cap * 2 : 128; *out = realloc(*out, (size_t)cap * sizeof **out); }
        ColEntry *e = &(*out)[n++];
        memset(e->name, 0, sizeof e->name);
        for (int k = 0; k < 22 && d[i + 8 + k]; k++) e->name[k] = (char)tolower(d[i + 8 + k]);
        e->blob = d + i;
        e->len = 8 + size;
        e->owner = owner;
        i += 8 + size;
    }
    return n;
}

static int col_find(ColEntry *a, int n, const char *name) {
    for (int i = 0; i < n; i++) if (!strcmp(a[i].name, name)) return i;
    return -1;
}

int merge_col(const char *base, const char *const *versions, const int *prio, const char *const *names, int n,
              const char *out, const char *what) {
    size_t blen;
    uint8_t *bd = read_all(base, &blen);
    if (!bd) return -1;
    ColEntry *B = NULL, *R = NULL;
    int nb = col_parse(bd, blen, &B, -1);
    // result starts as the original
    R = malloc(sizeof *R * (size_t)(nb ? nb : 1));
    memcpy(R, B, sizeof *R * (size_t)nb);
    int nr = nb, dropped = 0;
    uint8_t **vd = calloc((size_t)n, sizeof *vd);
    int *order = malloc(sizeof *order * (size_t)n);
    for (int v = 0; v < n; v++) order[v] = v;
    for (int a = 0; a < n; a++) // highest priority first, then the first mod alphabetically
        for (int b = a + 1; b < n; b++)
            if (prio[order[b]] > prio[order[a]]) { int t = order[a]; order[a] = order[b]; order[b] = t; }
    for (int k = 0; k < n; k++) {
        int v = order[k];
        size_t vl;
        vd[v] = read_all(versions[v], &vl);
        if (!vd[v]) continue;
        ColEntry *V = NULL;
        int nv = col_parse(vd[v], vl, &V, v);
        for (int i = 0; i < nv; i++) {
            int bi = col_find(B, nb, V[i].name);
            if (bi >= 0 && B[bi].len == V[i].len && !memcmp(B[bi].blob, V[i].blob, V[i].len)) continue; // unchanged
            int ri = col_find(R, nr, V[i].name);
            if (ri >= 0 && R[ri].owner >= 0) { // a higher priority mod already changed it
                if (R[ri].len != V[i].len || memcmp(R[ri].blob, V[i].blob, V[i].len)) {
                    mvc_log_warn("[ModLoader] %s: %s and %s both change collision %s - using %s's", what, names[R[ri].owner],
                                 names[v], V[i].name, names[R[ri].owner]);
                    dropped++;
                }
                continue;
            }
            if (ri >= 0) R[ri] = V[i];
            else { R = realloc(R, sizeof *R * (size_t)(nr + 1)); R[nr++] = V[i]; }
        }
        free(V);
    }
    FILE *f = fopen(out, "wb");
    int rc = f ? dropped : -1;
    for (int i = 0; f && i < nr; i++) if (fwrite(R[i].blob, 1, R[i].len, f) != R[i].len) rc = -1;
    if (f && fclose(f)) rc = -1;
    for (int v = 0; v < n; v++) free(vd[v]);
    free(vd); free(order); free(B); free(R); free(bd);
    return rc;
}
