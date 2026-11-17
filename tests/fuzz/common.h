// helpers shared by the fuzz targets.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// splits data at \x01 bytes into up to `max` parts.
static int fz_split(const uint8_t *d, size_t n, const uint8_t **part, size_t *len, int max) {
    int k = 0;
    size_t start = 0;
    for (size_t i = 0; i <= n && k < max; i++)
        if (i == n || d[i] == 1) { part[k] = d + start; len[k++] = i - start; start = i + 1; }
    return k;
}

// writes bytes to a per-process temp file and returns its path (static buffer per slot).
static const char *fz_file(int slot, const char *ext, const void *d, size_t n) {
    static char paths[8][256];
    snprintf(paths[slot], sizeof paths[slot], "/tmp/mvcfuzz-%d-%d%s", (int)getpid(), slot, ext);
    FILE *f = fopen(paths[slot], "wb");
    if (f) { fwrite(d, 1, n, f); fclose(f); }
    return paths[slot];
}

#define SEEDS(...) static const char *const seeds_[] = {__VA_ARGS__, NULL}; \
    const char **fuzz_seeds = (const char **)seeds_; size_t fuzz_seed_len[sizeof seeds_ / sizeof *seeds_]; \
    __attribute__((constructor)) static void seed_lens_(void) { for (int i = 0; seeds_[i]; i++) fuzz_seed_len[i] = strlen(seeds_[i]); }
