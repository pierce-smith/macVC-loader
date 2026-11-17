// merge_text (loader/modmerge.c): three-way line merge of data files.
// Input: base \x01 version1 \x01 version2 [\x01 version3]; the first byte of
// the base picks the file type (.ipl gets fuzzy number matching).
#include "common.h"
#include "../../loader/internal.h"
const char *fuzz_name = "merge";
SEEDS("cars\n90, landstal, landstal\n91, idaho, idaho\nend\n\x01" "cars\n90, landstal, landstal2\n91, idaho, idaho\nend\n\x01" "cars\n90, landstal, landstal\n91, idaho, idaho\n92, new, new\nend\n",
      "inst\n1, a, 0, 1.000, 2.000, 3.000\nend\n\x01inst\n1, a, 0, 1.0001, 2.000, 3.000\nend\n\x01inst\nend\n",
      "; handling\nLANDSTAL 1700.0 5.0\n\x01LANDSTAL 1800.0 5.0\n\x01LANDSTAL 1700.0 6.0\nNEWCAR 1 2\n")
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    const uint8_t *part[4];
    size_t len[4];
    int k = fz_split(d, n, part, len, 4);
    if (k < 2) return 0;
    const char *what = n && (d[0] & 1) ? "data/maps/test.ipl" : "data/default.ide";
    const char *base = fz_file(0, ".txt", part[0], len[0]);
    const char *vers[3];
    const char *names[3] = {"ModA", "ModB", "ModC"};
    int prio[3] = {1, 2, 3};
    for (int i = 1; i < k; i++) vers[i - 1] = fz_file(i, ".txt", part[i], len[i]);
    const char *out = fz_file(5, ".out", "", 0);
    merge_text(base, vers, prio, names, k - 1, out, what);
    for (int i = 0; i < k; i++) unlink(i ? vers[i - 1] : base);
    unlink(out);
    return 0;
}
