// offline test of Mod Loader texture packs against a real game folder.
//   texpack_test <GameData copy whose modloader/ we may write>
// picks a texture from a loose .txd and one that lives in several gta3.img
// dictionaries, drops TGA images with those names in modloader/TestPack/textures/,
// runs Mod Loader's build steps and checks every dictionary got the new texture.
#include "../fuzz/common.h"
#include "../../loader/modloader.c"
const char *fuzz_name = "texpack";

static void tga(const char *path, int w, int h) {
    FILE *f = fopen(path, "wb");
    uint8_t hd[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, (uint8_t)w, (uint8_t)(w >> 8), (uint8_t)h, (uint8_t)(h >> 8), 32, 8};
    fwrite(hd, 1, 18, f);
    for (int i = 0; i < w * h; i++) { uint8_t px[4] = {0, 0, 255, 255}; fwrite(px, 1, 4, f); }
    fclose(f);
}

static int check_txd(const char *path, const char *tex, int want_w) {
    size_t len;
    uint8_t *d = slurp(path, &len);
    if (!d) { printf("FAIL: %s not built\n", path); return 1; }
    char names[1024][32];
    int n = txd_texture_names(d, len, names, 1024), found = 0;
    for (size_t i = 24 + *(uint32_t *)(d + 16); i + 12 <= len;) { // find the texture, check its size
        uint32_t t = *(uint32_t *)(d + i), s = *(uint32_t *)(d + i + 4);
        if (t == 0x15 && !strcasecmp((char *)d + i + 32, tex)) { found = *(uint16_t *)(d + i + 32 + 64 + 8); break; }
        i += 12 + s;
    }
    free(d);
    if (found != want_w) { printf("FAIL: %s: %s width %d, want %d (%d textures)\n", path, tex, found, want_w, n); return 1; }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    snprintf(g_game_dir, sizeof g_game_dir, "%s", argv[1]);
    snprintf(g_root, sizeof g_root, "%s/modloader", g_game_dir);
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/.cache", g_root);
    mkdir(p, 0755);
    index_game();
    texidx_build();
    if (!g_ntexidx) { printf("FAIL: empty texture index\n"); return 1; }
    // a texture only in models/hud.txd, and the img texture found in the most dictionaries
    const char *loose = NULL, *multi = NULL;
    int best = 0;
    for (int i = 0; i < g_ntexidx; i++) {
        if (!loose && !strcmp(g_texidx[i].target, "models/hud.txd")) loose = g_texidx[i].tex;
        int n = 1;
        while (i + n < g_ntexidx && !strcmp(g_texidx[i + n].tex, g_texidx[i].tex)) n++;
        if (n > best && n < 20 && !strncmp(g_texidx[i].target, "img:", 4)) { best = n; multi = g_texidx[i].tex; }
    }
    printf("index: %d entries; loose test texture %s, archive test texture %s (in %d dictionaries)\n", g_ntexidx, loose, multi, best);
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/TestPack/textures/sub", g_root);
    snprintf(p, sizeof p, "%s/%s.tga", dir, loose); tga(p, 64, 32);
    snprintf(p, sizeof p, "%s/%s.tga", dir, multi); tga(p, 256, 256);
    snprintf(p, sizeof p, "%s/no_such_texture_xyz.tga", dir); tga(p, 8, 8);
    g_cfg.texture_max = 128; // the 256 image must come out 128 wide
    scan_mods(g_root);
    apply_texture_packs();
    int t0 = g_ntxdimg;
    build_txd_folders();
    int fail = 0;
    char out[PATH_MAX];
    snprintf(out, sizeof out, "%s/.cache/txd/models_hud.txd", g_root);
    fail += check_txd(out, loose, 64);
    int img = 0;
    for (int i = 0; i < g_ntexidx; i++)
        if (!strcmp(g_texidx[i].tex, multi)) {
            snprintf(out, sizeof out, "%s/.cache/txd/img/%s", g_root, g_texidx[i].target + 4);
            fail += check_txd(out, multi, 128);
            img++;
        }
    // the archive overlay must name its entries after the dictionaries
    int named = 0;
    for (int i = 0; i < g_nimg; i++) {
        const char *b = strrchr(g_img[i].src, '/');
        if (b && !strcmp(b + 1, g_img[i].key)) named++;
    }
    if (named != g_nimg) { printf("FAIL: %d of %d archive entries have the wrong file name\n", g_nimg - named, g_nimg); fail++; }
    if (g_nimg != img) { printf("FAIL: %d archive overrides, want %d\n", g_nimg, img); fail++; }
    // a second run reuses the built files (no rebuild)
    struct stat a, b;
    snprintf(out, sizeof out, "%s/.cache/txd/models_hud.txd", g_root);
    stat(out, &a);
    sleep(1);
    build_txd_folders();
    stat(out, &b);
    if (a.st_mtime != b.st_mtime) { printf("FAIL: unchanged texture pack was rebuilt\n"); fail++; }
    printf("%d texture-pack image placements, %d archive dictionaries checked: %s\n", t0, img, fail ? "FAILED" : "passed");
    return fail != 0;
}
