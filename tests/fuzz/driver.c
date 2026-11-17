// small mutation fuzzer for the parsers. apple's clang has no libFuzzer runtime, so this
// drives the same LLVMFuzzerTestOneInput targets itself: seeds (built in plus any files
// you give it) get byte flips, inserts, deletes, splices and edge values, under asan+ubsan.
//   fuzz_<target> [-n iterations] [-s seed] [seed files...]
//   fuzz_<target> -r crash.bin        replay one input
// the input gets written to fuzz-<target>.last before each run so a crash leaves it behind.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
extern const char *fuzz_name;
extern const char **fuzz_seeds;      // built-in seeds, NULL-terminated; "\x01" separates parts
extern size_t fuzz_seed_len[];

static uint64_t g_rng = 88172645463325252ULL;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (uint32_t)g_rng; }

typedef struct { uint8_t *d; size_t n; } Buf;
static Buf g_corpus[256];
static int g_ncorpus;

static void add(const uint8_t *d, size_t n) {
    if (g_ncorpus >= 256) return;
    g_corpus[g_ncorpus].d = malloc(n + 1);
    memcpy(g_corpus[g_ncorpus].d, d, n);
    g_corpus[g_ncorpus++].n = n;
}

static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long L = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)L + 1);
    *n = fread(d, 1, (size_t)L, f);
    fclose(f);
    return d;
}

#define MAXLEN 65536
static size_t mutate(uint8_t *d, size_t n) {
    int rounds = 1 + (int)(rnd() % 8);
    static const int32_t interesting[] = {0, 1, -1, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, (int32_t)0x80000000, 12, 8, 64};
    for (int r = 0; r < rounds; r++) {
        uint32_t k = rnd() % 9;
        size_t at = n ? rnd() % n : 0;
        switch (k) {
        case 0: if (n) d[at] ^= (uint8_t)(1u << (rnd() % 8)); break;
        case 1: if (n) d[at] = (uint8_t)rnd(); break;
        case 2: if (n + 1 < MAXLEN) { memmove(d + at + 1, d + at, n - at); d[at] = (uint8_t)rnd(); n++; } break;
        case 3: if (n) { size_t L = 1 + rnd() % (n - at); memmove(d + at, d + at + L, n - at - L); n -= L; } break;
        case 4: if (n >= 4 && at + 4 <= n) { int32_t v = interesting[rnd() % (sizeof interesting / 4)]; memcpy(d + at, &v, 4); } break;
        case 5: { // splice from another corpus entry
            Buf *o = &g_corpus[rnd() % g_ncorpus];
            if (!o->n) break;
            size_t from = rnd() % o->n, L = 1 + rnd() % (o->n - from);
            if (n + L >= MAXLEN) break;
            memmove(d + at + L, d + at, n - at);
            memcpy(d + at, o->d + from, L);
            n += L;
            break;
        }
        case 6: if (n) { size_t L = 1 + rnd() % (n - at); if (n + L < MAXLEN) { memmove(d + at + L, d + at, n - at); n += L; } } break; // duplicate a range
        case 7: { static const char *tok[] = {"\n", "\r\n", "=", "[", "]", "%", "%s", "%d", "%f", " ", "\t", ",", "#", ";", "\x01", "end\n", "cars\n", "TABL", "TKEY", "TDAT", "COLL", "COL3"};
                  const char *t = tok[rnd() % (sizeof tok / sizeof *tok)]; size_t L = strlen(t);
                  if (n + L < MAXLEN) { memmove(d + at + L, d + at, n - at); memcpy(d + at, t, L); n += L; } break; }
        case 8: if (n > 1) n = at + 1; break; // truncate
        }
    }
    return n;
}

int main(int argc, char **argv) {
    long iters = 20000;
    char last[256];
    snprintf(last, sizeof last, "fuzz-%s.last", fuzz_name);
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) iters = atol(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) g_rng = (uint64_t)atoll(argv[++i]) * 2654435761ULL + 1;
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) {
            size_t n;
            uint8_t *d = slurp(argv[++i], &n);
            if (!d) { perror(argv[i]); return 2; }
            LLVMFuzzerTestOneInput(d, n);
            printf("%s: replay finished without a crash\n", fuzz_name);
            return 0;
        }
    }
    for (int s = 0; fuzz_seeds[s]; s++) add((const uint8_t *)fuzz_seeds[s], fuzz_seed_len[s]);
    for (; i < argc; i++) { size_t n; uint8_t *d = slurp(argv[i], &n); if (d) { if (n < MAXLEN) add(d, n); free(d); } }
    for (int s = 0; s < g_ncorpus; s++) LLVMFuzzerTestOneInput(g_corpus[s].d, g_corpus[s].n);
    uint8_t *buf = malloc(MAXLEN + 1);
    time_t t0 = time(NULL);
    for (long it = 0; it < iters; it++) {
        Buf *src = &g_corpus[rnd() % g_ncorpus];
        memcpy(buf, src->d, src->n);
        size_t n = mutate(buf, src->n);
        FILE *f = fopen(last, "wb");
        if (f) { fwrite(buf, 1, n, f); fclose(f); }
        LLVMFuzzerTestOneInput(buf, n);
        if (it % 64 == 0 && g_ncorpus < 256) add(buf, n); // keep some variety
    }
    remove(last);
    printf("%s: %ld runs, no crashes (%lds)\n", fuzz_name, iters, (long)(time(NULL) - t0));
    return 0;
}
