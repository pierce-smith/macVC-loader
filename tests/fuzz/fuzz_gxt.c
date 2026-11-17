// gxt_each (loader/modloader.c): Vice City .gxt files (TABL/TKEY/TDAT).
#include "common.h"
#include "../../loader/modloader.c"
const char *fuzz_name = "gxt";
// TABL with MAIN -> TKEY (one key) -> TDAT ("Hi")
static const uint8_t g1[] = "TABL\x0c\0\0\0MAIN\0\0\0\0\x14\0\0\0"
                            "TKEY\x0c\0\0\0" "\0\0\0\0KEY1\0\0\0\0"
                            "TDAT\x06\0\0\0" "H\0i\0\0\0";
static const char *const seeds_[] = {(const char *)g1, NULL};
const char **fuzz_seeds = (const char **)seeds_;
size_t fuzz_seed_len[] = {sizeof g1 - 1, 0};
static void each(const char *t, const char *k, const uint16_t *s, size_t n, void *u) {
    (void)t; (void)k; (void)u;
    volatile uint16_t x = 0;
    for (size_t i = 0; i < n; i++) x ^= s[i];
}
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    uint8_t *copy = malloc(n ? n : 1); // exact-size copy so ASan sees overreads
    memcpy(copy, d, n);
    gxt_each(copy, n, each, NULL);
    free(copy);
    return 0;
}
