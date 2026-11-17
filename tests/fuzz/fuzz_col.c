// merge_col (loader/modmerge.c): collision archives (COLL/COL2/COL3 entries).
// Input: base \x01 version1 [\x01 version2]
#include "common.h"
#include "../../loader/internal.h"
const char *fuzz_name = "col";
#define E1 "COLL\x24\x00\x00\x00" "box_one\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0" "\0\0\0\0"
#define E2 "COLL\x24\x00\x00\x00" "box_two\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0" "\0\0\0\0"
static const uint8_t seed1[] = E1 E2 "\x01" E1 "\x01" E2 E1;
const char *fuzz_seeds_dummy;
static const char *const seeds_[] = {(const char *)seed1, NULL};
const char **fuzz_seeds = (const char **)seeds_;
size_t fuzz_seed_len[] = {sizeof seed1 - 1, 0};
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    const uint8_t *part[3];
    size_t len[3];
    int k = fz_split(d, n, part, len, 3);
    // \x01 also occurs inside binary entries; that's fine, it just moves the split
    if (k < 2) return 0;
    const char *base = fz_file(0, ".col", part[0], len[0]);
    const char *vers[2], *names[2] = {"ModA", "ModB"};
    int prio[2] = {1, 2};
    for (int i = 1; i < k; i++) vers[i - 1] = fz_file(i, ".col", part[i], len[i]);
    const char *out = fz_file(5, ".out", "", 0);
    merge_col(base, vers, prio, names, k - 1, out, "models/coll/generic.col");
    for (int i = 0; i < k; i++) unlink(i ? vers[i - 1] : base);
    unlink(out);
    return 0;
}
