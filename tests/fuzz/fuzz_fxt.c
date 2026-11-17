// mvc_text_load_fxt (loader/text.c): CLEO .fxt text files.
#include "common.h"
#include "../../include/macvc.h"
const char *fuzz_name = "fxt";
SEEDS("GAS_1 Press ~k~~PED_ANSWER_PHONE~ to refuel\n# comment\nLONGKEY1 too long\n\n// c\nK2\tTab ~r~red\r\n", "A\nB \nC  x\n")
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    const char *p = fz_file(0, ".fxt", d, n);
    mvc_text_load_fxt(p);
    const uint16_t *w = mvc_text_get("GAS_1");
    if (w) { size_t L = 0; while (w[L]) L++; (void)L; }
    unlink(p);
    return 0;
}
