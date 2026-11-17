// mvc_ini_get / mvc_ini_set (loader/menu.c) on arbitrary ini files.
#include "common.h"
#include "../../include/macvc.h"
const char *fuzz_name = "ini";
SEEDS("[Main]\nConsoleKey=GRAVE\n; c\n[Mods]\nGasoline=1\n", "[a\nk=v\n[b]=x\n=\n[]\nk = v \r\n", "\xEF\xBB\xBF[Main]\r\nLogLevel = 2\r\n")
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    const char *p = fz_file(0, ".ini", d, n);
    char out[64];
    mvc_ini_get(p, "Main", "ConsoleKey", out, sizeof out);
    mvc_ini_get(p, "", "k", out, sizeof out);
    mvc_ini_get(p, "Mods", "Gasoline", out, 1);
    mvc_ini_set(p, "Mods", "Gasoline", "0");
    mvc_ini_set(p, "New Section", "Key", "a value that is long enough to matter");
    if (!mvc_ini_get(p, "New Section", "Key", out, sizeof out) || strcmp(out, "a value that is long enough to matter")) {
        // a value just written must read back (unless the file couldn't be written)
        FILE *f = fopen(p, "r");
        if (f) { fclose(f); fprintf(stderr, "ini: written value did not read back\n"); abort(); }
    }
    unlink(p);
    return 0;
}
