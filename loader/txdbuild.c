// builds txds for name.txd/ folders in mods. every image in the folder gets added to the
// game's name.txd, or replaces the texture with the same name, like windows modloader.
// images become uncompressed 32-bit textures (d3d8 8888) which vc reads as is.
// tga and bmp get read here, png and jpeg through ImageIO, only loaded if a mod has one.
#include <ctype.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "internal.h"

typedef struct { int w, h, alpha; uint8_t *bgra; } Image; // pixels as stored in the TXD (B, G, R, A)

static uint8_t *read_file(const char *path, size_t *len) {
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

static int load_tga(const uint8_t *d, size_t len, Image *im) {
    if (len < 18) return -1;
    int idlen = d[0], cmap = d[1], type = d[2], w = d[12] | d[13] << 8, h = d[14] | d[15] << 8, bpp = d[16], desc = d[17];
    if (cmap || (type != 2 && type != 10) || (bpp != 24 && bpp != 32) || w <= 0 || h <= 0 || w > 4096 || h > 4096) return -1;
    int px = bpp / 8;
    const uint8_t *p = d + 18 + idlen, *end = d + len;
    im->w = w; im->h = h; im->alpha = 0;
    im->bgra = malloc((size_t)w * h * 4);
    for (int i = 0; i < w * h;) {
        int count = 1, raw = 1;
        if (type == 10) {
            if (p >= end) break;
            count = (*p & 0x7F) + 1;
            raw = !(*p & 0x80);
            p++;
        }
        for (int k = 0; k < count && i < w * h; k++, i++) {
            if (p + px > end) { free(im->bgra); return -1; }
            int x = i % w, y = i / w;
            if (!(desc & 0x20)) y = h - 1 - y; // bottom-up unless bit 5 says top-down
            uint8_t *o = im->bgra + ((size_t)y * w + x) * 4;
            o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = px == 4 ? p[3] : 255;
            if (o[3] != 255) im->alpha = 1;
            if (raw || k == count - 1) p += px;
        }
    }
    return 0;
}

static int load_bmp(const uint8_t *d, size_t len, Image *im) {
    if (len < 54 || d[0] != 'B' || d[1] != 'M') return -1;
    uint32_t off = d[10] | d[11] << 8 | d[12] << 16 | (uint32_t)d[13] << 24;
    int32_t w = (int32_t)(d[18] | d[19] << 8 | d[20] << 16 | (uint32_t)d[21] << 24);
    int32_t h = (int32_t)(d[22] | d[23] << 8 | d[24] << 16 | (uint32_t)d[25] << 24);
    int bpp = d[28] | d[29] << 8, comp = d[30];
    if ((bpp != 24 && bpp != 32) || comp != 0 || w <= 0 || w > 4096 || h == 0) return -1;
    int top_down = h < 0;
    if (h < 0) h = -h;
    if (h > 4096) return -1;
    size_t stride = ((size_t)w * (bpp / 8) + 3) & ~3UL;
    if (off + stride * (size_t)h > len) return -1;
    im->w = w; im->h = h; im->alpha = 0;
    im->bgra = malloc((size_t)w * h * 4);
    for (int y = 0; y < h; y++) {
        const uint8_t *row = d + off + stride * (size_t)(top_down ? y : h - 1 - y);
        for (int x = 0; x < w; x++) {
            const uint8_t *p = row + (size_t)x * (bpp / 8);
            uint8_t *o = im->bgra + ((size_t)y * w + x) * 4;
            o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = bpp == 32 ? p[3] : 255;
        }
    }
    if (bpp == 32) { // a 32-bit BMP with an all-zero alpha channel means "no alpha"
        int any = 0;
        for (int i = 0; i < w * h && !any; i++) any = im->bgra[i * 4 + 3] != 0;
        if (!any) for (int i = 0; i < w * h; i++) im->bgra[i * 4 + 3] = 255;
        else for (int i = 0; i < w * h; i++) if (im->bgra[i * 4 + 3] != 255) { im->alpha = 1; break; }
    }
    return 0;
}

// PNG / JPEG through ImageIO + CoreGraphics, looked up at runtime
typedef struct { double x, y, w, h; } Rect; // CGRect
static int load_imageio(const char *path, Image *im) {
    static void *cg, *io, *cf;
    if (!cg) {
        cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_LAZY);
        cg = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_LAZY);
        io = dlopen("/System/Library/Frameworks/ImageIO.framework/ImageIO", RTLD_LAZY);
    }
    if (!cg || !io || !cf) return -1;
    void *(*URLCreate)(void *, const uint8_t *, long, int) = dlsym(cf, "CFURLCreateFromFileSystemRepresentation");
    void (*CFRelease_)(const void *) = dlsym(cf, "CFRelease");
    void *(*SrcCreate)(void *, void *) = dlsym(io, "CGImageSourceCreateWithURL");
    void *(*ImgAt)(void *, size_t, void *) = dlsym(io, "CGImageSourceCreateImageAtIndex");
    size_t (*W)(void *) = dlsym(cg, "CGImageGetWidth");
    size_t (*H)(void *) = dlsym(cg, "CGImageGetHeight");
    void *(*RGB)(void) = dlsym(cg, "CGColorSpaceCreateDeviceRGB");
    void *(*Ctx)(void *, size_t, size_t, size_t, size_t, void *, uint32_t) = dlsym(cg, "CGBitmapContextCreate");
    // CGContextDrawImage(ctx, CGRect, image): the rect is four doubles passed by value
    void (*DrawRect)(void *, Rect, void *) = dlsym(cg, "CGContextDrawImage");
    if (!URLCreate || !CFRelease_ || !SrcCreate || !ImgAt || !W || !H || !RGB || !Ctx || !DrawRect) return -1;
    void *url = URLCreate(NULL, (const uint8_t *)path, (long)strlen(path), 0);
    void *src = url ? SrcCreate(url, NULL) : NULL;
    void *img = src ? ImgAt(src, 0, NULL) : NULL;
    int rc = -1;
    if (img) {
        size_t w = W(img), h = H(img);
        if (w && h && w <= 4096 && h <= 4096) {
            im->w = (int)w; im->h = (int)h; im->alpha = 0;
            im->bgra = calloc(w * h, 4);
            void *cs = RGB();
            // kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little = B,G,R,A in memory
            void *ctx = Ctx(im->bgra, w, h, 8, w * 4, cs, 2 | (2 << 12));
            if (ctx) {
                DrawRect(ctx, (Rect){0, 0, (double)w, (double)h}, img);
                CFRelease_(ctx);
                for (size_t i = 0; i < w * h; i++) { // un-premultiply
                    uint8_t *p = im->bgra + i * 4;
                    if (p[3] != 255) im->alpha = 1;
                    if (p[3] && p[3] != 255)
                        for (int c = 0; c < 3; c++) { int v = p[c] * 255 / p[3]; p[c] = (uint8_t)(v > 255 ? 255 : v); }
                }
                rc = 0;
            } else { free(im->bgra); im->bgra = NULL; }
            CFRelease_(cs);
        }
        CFRelease_(img);
    }
    if (src) CFRelease_(src);
    if (url) CFRelease_(url);
    return rc;
}

static int load_image(const char *path, Image *im) {
    const char *dot = strrchr(path, '.');
    if (!dot) return -1;
    if (!strcasecmp(dot, ".png") || !strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg")) return load_imageio(path, im);
    size_t len;
    uint8_t *d = read_file(path, &len);
    if (!d) return -1;
    int rc = !strcasecmp(dot, ".tga") ? load_tga(d, len, im) : !strcasecmp(dot, ".bmp") ? load_bmp(d, len, im) : -1;
    free(d);
    return rc;
}

int txd_is_image(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".tga") || !strcasecmp(dot, ".bmp") || !strcasecmp(dot, ".png") ||
                   !strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg"));
}

// TXD writing
typedef struct { uint8_t *d; size_t n, cap; } Buf;
static void put(Buf *b, const void *p, size_t n) {
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 4096; b->d = realloc(b->d, b->cap); }
    memcpy(b->d + b->n, p, n);
    b->n += n;
}
static void put32(Buf *b, uint32_t v) { put(b, &v, 4); }
static void chunk(Buf *b, uint32_t type, uint32_t size, uint32_t ver) { put32(b, type); put32(b, size); put32(b, ver); }

static void texture_native(Buf *b, const char *name, const Image *im, uint32_t ver) {
    uint32_t pixels = (uint32_t)im->w * (uint32_t)im->h * 4, st = 88 + 4 + pixels;
    chunk(b, 0x15, 12 + st + 12, ver);
    chunk(b, 0x01, st, ver);
    put32(b, 8);           // platform: D3D8
    put32(b, 0x00001102);  // linear filtering, wrap u/v
    char nm[32] = {0}, mask[32] = {0};
    snprintf(nm, sizeof nm, "%s", name);
    put(b, nm, 32);
    put(b, mask, 32);
    put32(b, 0x0500);      // raster format 8888
    put32(b, im->alpha ? 1 : 0);
    uint16_t wh[2] = {(uint16_t)im->w, (uint16_t)im->h};
    put(b, wh, 4);
    uint8_t info[4] = {32, 1, 4, 0}; // depth, mip levels, raster type (texture), no compression
    put(b, info, 4);
    put32(b, pixels);
    put(b, im->bgra, pixels);
    chunk(b, 0x03, 0, ver); // empty extension
}

// halves an image (2x2 average) until both sides fit `max`.
static void fit_size(Image *im, int max) {
    while (max > 0 && (im->w > max || im->h > max) && im->w > 1 && im->h > 1) {
        int w = im->w / 2, h = im->h / 2;
        uint8_t *o = malloc((size_t)w * h * 4);
        if (!o) return;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                for (int c = 0; c < 4; c++) {
                    const uint8_t *p = im->bgra + ((size_t)(2 * y) * im->w + 2 * x) * 4 + c;
                    o[((size_t)y * w + x) * 4 + c] = (uint8_t)((p[0] + p[4] + p[im->w * 4] + p[im->w * 4 + 4] + 2) / 4);
                }
        free(im->bgra);
        im->bgra = o;
        im->w = w;
        im->h = h;
    }
}

int txd_texture_names(const uint8_t *d, size_t len, char (*names)[32], int max) {
    if (len < 28 || *(const uint32_t *)d != 0x16) return -1;
    size_t end = 12 + (size_t)*(const uint32_t *)(d + 4), i = 24 + (size_t)*(const uint32_t *)(d + 16);
    if (end > len) end = len;
    int n = 0;
    while (i + 12 <= end && n < max) {
        uint32_t t = *(const uint32_t *)(d + i), s = *(const uint32_t *)(d + i + 4);
        if (t == 0x15 && i + 24 + 8 + 32 <= end) {
            memcpy(names[n], d + i + 32, 31);
            names[n][31] = 0;
            for (char *c = names[n]; *c; c++) *c = (char)tolower((unsigned char)*c);
            if (names[n][0]) n++;
        }
        if (i + 12 + (size_t)s > end) break;
        i += 12 + (size_t)s;
    }
    return n;
}

// base (may be NULL) + images -> out. returns the number of textures added or replaced, -1 on error.
int txd_build(const uint8_t *base, size_t blen, const char *const *images, int n, const char *out, const char *what) {
    uint32_t ver = 0x1003FFFF; // RenderWare 3.4.0.3, as Vice City writes
    char names[256][32];
    Image im[256];
    int ni = 0;
    for (int i = 0; i < n && ni < 256; i++) {
        memset(&im[ni], 0, sizeof im[ni]);
        if (load_image(images[i], &im[ni])) {
            mvc_log_warn("[ModLoader] %s: can't read %s (TGA, BMP, PNG or JPEG, up to 4096x4096)", what, images[i]);
            continue;
        }
        fit_size(&im[ni], g_cfg.texture_max);
        const char *b = strrchr(images[i], '/');
        snprintf(names[ni], 32, "%s", b ? b + 1 : images[i]);
        char *dot = strrchr(names[ni], '.');
        if (dot) *dot = 0;
        ni++;
    }
    if (!ni) return -1;
    Buf body = {0};
    int count = 0, replaced = 0;
    // the original's textures, minus the ones replaced by an image of the same name
    if (base && blen >= 28 && *(uint32_t *)base == 0x16) {
        ver = *(uint32_t *)(base + 8);
        size_t end = 12 + *(uint32_t *)(base + 4), i = 12 + 12 + *(uint32_t *)(base + 16); // skip the struct
        if (end > blen) end = blen;
        while (i + 12 <= end) {
            uint32_t t = *(uint32_t *)(base + i), s = *(uint32_t *)(base + i + 4);
            if (i + 12 + s > end) break;
            if (t == 0x15 && s >= 24 + 8 + 32) { // the struct header, platform, filter, name
                char nm[33] = {0};
                memcpy(nm, base + i + 24 + 8, 32);
                int skip = 0;
                for (int k = 0; k < ni; k++) if (!strcasecmp(nm, names[k])) skip = 1;
                if (skip) replaced++;
                else { put(&body, base + i, 12 + s); count++; }
            }
            i += 12 + s;
        }
    }
    for (int k = 0; k < ni; k++) { texture_native(&body, names[k], &im[k], ver); count++; free(im[k].bgra); }
    Buf txd = {0};
    chunk(&txd, 0x16, (uint32_t)(12 + 4 + body.n + 12), ver);
    chunk(&txd, 0x01, 4, ver);
    uint16_t cd[2] = {(uint16_t)count, 0};
    put(&txd, cd, 4);
    put(&txd, body.d, body.n);
    chunk(&txd, 0x03, 0, ver);
    free(body.d);
    // only rewrite when something changed, so the archive cache isn't rebuilt every launch
    size_t olen;
    uint8_t *old = read_file(out, &olen);
    int same = old && olen == txd.n && !memcmp(old, txd.d, olen);
    free(old);
    int rc = ni;
    if (!same) {
        FILE *f = fopen(out, "wb");
        if (!f || fwrite(txd.d, 1, txd.n, f) != txd.n) rc = -1;
        if (f && fclose(f)) rc = -1;
    }
    free(txd.d);
    if (rc > 0) mvc_log("[ModLoader] %s: %d texture(s) from the folder (%d replaced, %d added)", what, ni, replaced, ni - replaced);
    return rc;
}
