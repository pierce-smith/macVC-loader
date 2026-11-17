// txd_build (loader/txdbuild.c): base .txd + one image (TGA/BMP/PNG/JPEG).
// Input: base txd \x01 image; the image's first byte picks its extension.
#include "common.h"
#include "../../loader/internal.h"
const char *fuzz_name = "txd";
// a 2x2 32-bit uncompressed TGA and a 2x2 24-bit BMP
#define TGA "\0\0\x02\0\0\0\0\0\0\0\0\0\x02\0\x02\0\x20\x08" "\xff\0\0\xff\0\xff\0\xff\0\0\xff\xff\xff\xff\xff\xff"
#define BMP "BM\x46\0\0\0\0\0\0\0\x36\0\0\0\x28\0\0\0\x02\0\0\0\x02\0\0\0\x01\0\x18\0\0\0\0\0\x10\0\0\0\x13\x0b\0\0\x13\x0b\0\0\0\0\0\0\0\0\0\0" "\xff\0\0\0\xff\0\0\0\0\0\xff\xff\xff\xff\0\0"
static const uint8_t s1[] = "\x01" TGA, s2[] = "\x01" BMP, s3[] = "\x16\0\0\0\x04\0\0\0\xff\xff\x03\x18" "\x01" TGA;
static const char *const seeds_[] = {(const char *)s1, (const char *)s2, (const char *)s3, NULL};
const char **fuzz_seeds = (const char **)seeds_;
size_t fuzz_seed_len[] = {sizeof s1 - 1, sizeof s2 - 1, sizeof s3 - 1, 0};
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    const uint8_t *part[2];
    size_t len[2];
    if (fz_split(d, n, part, len, 2) < 2) return 0;
    static const char *ext[] = {".tga", ".bmp", ".png", ".jpg"};
    const char *e = ext[len[1] ? part[1][0] % 4 : 0];
    if (len[1] && (part[1][0] == 'B' || part[1][0] == 0)) e = part[1][0] == 'B' ? ".bmp" : ".tga";
    const char *img = fz_file(1, e, part[1], len[1]);
    const char *imgs[1] = {img};
    const char *out = fz_file(5, ".txd", "", 0);
    txd_build(part[0], len[0], imgs, 1, out, "fuzz.txd");
    unlink(img);
    unlink(out);
    return 0;
}
