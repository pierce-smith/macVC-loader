// sdk helpers: player, vehicles, world, 3d drawing, sprites, audio.
// struct offsets get read out of this macVC build's code at startup. if one can't be
// found the helper logs once and does nothing.
#include <ctype.h>
#include <dlfcn.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "internal.h"

typedef MVCVec3 V3;
typedef struct { float left, bottom, right, top; } CRect;

// game functions
static void *(*FindPlayerPed)(void);
static void *(*FindPlayerVehicle)(void);
static V3 (*FindPlayerCoors)(void);
static void (*PlayerSetWanted)(void *, int);
static void (*PedTeleport)(void *, V3);
static void (*AutoTeleport)(void *, V3), (*BikeTeleport)(void *, V3), (*BoatTeleport)(void *, V3);
static int (*GiveWeapon)(void *, int, unsigned, _Bool);
static void *(*GetWeaponInfo)(int);
static void (*RequestModel)(int, int);
static void (*LoadAllRequestedModels)(_Bool);
static void *(*VehicleNew)(unsigned long);
static void (*AutoCtor)(void *, int, uint8_t), (*BikeCtor)(void *, int, uint8_t), (*BoatCtor)(void *, int, uint8_t);
static void (*WorldAdd)(void *);
static void (*ClearSpace)(const V3 *, void *);
static void (*SetHeading)(void *, float);
static void (*PlaceOnRoad)(void *);
static float (*GroundZ)(float, float);
static void (*AutoFix)(void *);
static void (*SetGameClock)(uint8_t, uint8_t);
static void (*ForceWeather)(short);
static uint8_t *ClockHours, *ClockMinutes;
static uint8_t *Players, *PlayerInFocus;
static _Bool (*CalcScreenCoors)(const V3 *, V3 *, float *, float *, _Bool);
static void (*RenderLine)(float, float, float, float, float, float, uint32_t, uint32_t);
static void (*RegisterCorona)(uint32_t, uint8_t, uint8_t, uint8_t, uint8_t, const V3 *, float, float,
                              uint8_t, int8_t, uint8_t, uint8_t, uint8_t, float, _Bool, float);
static void (*PlaceMarkerSet)(uint32_t, uint16_t, V3 *, float, uint8_t, uint8_t, uint8_t, uint8_t, uint16_t, float, int16_t);
static void (*SpriteDraw)(void *, const CRect *, const MVCColor *);
static void (*SpriteSetTexture)(void *, const char *);
static int (*TxdFind)(const char *), (*TxdAdd)(const char *);
static _Bool (*TxdLoad)(int, const char *);
static void (*TxdAddRef)(int), (*TxdPush)(void), (*TxdPop)(void), (*TxdSetCurrent)(int);
static void *(*ReadPNG)(const char *);
static void *(*RasterFromImage)(void *, int);
static void *(*TextureCreate)(void *);
static void (*ImageDestroy)(void *);

static int off_pos = -1;
static int off_money = -1, off_health = -1, off_armour = -1, off_model = -1, size_pinfo = 0x1a0, off_wmodel = -1;

// for the CLEO runner's struct field translation
int sdk_offset(int which) {
    switch (which) {
    case SDK_OFF_POS: return off_pos;
    case SDK_OFF_MODEL: return off_model;
    case SDK_OFF_HEALTH: return off_health;
    case SDK_OFF_ARMOUR: return off_armour;
    case SDK_OFF_MONEY: return off_money;
    case SDK_SIZE_PINFO: return size_pinfo;
    case SDK_OFF_WMODEL: return off_wmodel;
    }
    return -1;
}

static void once(int *flag, const char *what) {
    if (!*flag) { mvc_log_warn("[Loader] %s is not available on this macVC build", what); *flag = 1; }
}

// player
void *mvc_player_ped(void) { return FindPlayerPed ? FindPlayerPed() : NULL; }
void *mvc_player_vehicle(void) { return FindPlayerVehicle ? FindPlayerVehicle() : NULL; }
MVCVec3 mvc_player_pos(void) {
    if (!FindPlayerPed || !FindPlayerPed()) return (V3){0, 0, 0};
    return FindPlayerCoors();
}

static int32_t *money_ptr(void) {
    if (off_money < 0 || !Players || !PlayerInFocus) return NULL;
    return (int32_t *)(Players + (size_t)*PlayerInFocus * size_pinfo + off_money);
}
int mvc_player_money(void) { int32_t *m = money_ptr(); return m ? *m : 0; }
void mvc_player_set_money(int d) { static int w; int32_t *m = money_ptr(); if (m) *m = d; else once(&w, "money"); }

static float *ped_float(int off) {
    void *p = mvc_player_ped();
    return (p && off >= 0) ? (float *)((uint8_t *)p + off) : NULL;
}
float mvc_player_health(void) { float *f = ped_float(off_health); return f ? *f : 0; }
void mvc_player_set_health(float hp) { static int w; float *f = ped_float(off_health); if (f) *f = hp; else once(&w, "health"); }
float mvc_player_armour(void) { float *f = ped_float(off_armour); return f ? *f : 0; }
void mvc_player_set_armour(float a) { static int w; float *f = ped_float(off_armour); if (f) *f = a; else once(&w, "armour"); }
void mvc_player_set_wanted(int stars) {
    void *p = mvc_player_ped();
    if (p && PlayerSetWanted) PlayerSetWanted(p, stars < 0 ? 0 : stars > 6 ? 6 : stars);
}

// vehicle classes from data/default.ide ("cars" section, 4th column)
enum { VT_UNKNOWN, VT_CAR, VT_BIKE, VT_BOAT, VT_OTHER };
static uint8_t g_vtype[1024];
static void load_vehicle_types(void) {
    char path[PATH_MAX], line[512];
    snprintf(path, sizeof path, "%s/data/default.ide", g_game_dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    int incars = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (!strncasecmp(p, "cars", 4)) { incars = 1; continue; }
        if (!strncasecmp(p, "end", 3)) { incars = 0; continue; }
        if (!incars || !isdigit((unsigned char)*p)) continue;
        int id = atoi(p);
        char *c = p;
        for (int k = 0; k < 3 && c; k++) { c = strchr(c, ','); if (c) c++; }
        if (!c || id < 0 || id >= 1024) continue;
        while (isspace((unsigned char)*c)) c++;
        g_vtype[id] = !strncasecmp(c, "car", 3) ? VT_CAR : !strncasecmp(c, "bike", 4) ? VT_BIKE
                    : !strncasecmp(c, "boat", 4) ? VT_BOAT : VT_OTHER;
    }
    fclose(f);
}

MVCVec3 mvc_entity_pos(void *e) {
    if (!e || off_pos < 0) return (V3){0, 0, 0};
    return *(V3 *)((uint8_t *)e + off_pos);
}

int mvc_vehicle_model(void *v) { return (v && off_model >= 0) ? *(uint16_t *)((uint8_t *)v + off_model) : -1; }

static void teleport_vehicle(void *v, V3 pos) {
    int t = g_vtype[mvc_vehicle_model(v) & 1023];
    if (t == VT_BIKE && BikeTeleport) BikeTeleport(v, pos);
    else if (t == VT_BOAT && BoatTeleport) BoatTeleport(v, pos);
    else if (t == VT_CAR && AutoTeleport) AutoTeleport(v, pos);
    else mvc_log_warn("[Loader] can't teleport this kind of vehicle");
}

void mvc_player_teleport(MVCVec3 pos) {
    if (pos.z <= -99.f && GroundZ) pos.z = GroundZ(pos.x, pos.y) + 1.0f;
    void *v = mvc_player_vehicle();
    if (v) teleport_vehicle(v, pos);
    else if (mvc_player_ped() && PedTeleport) PedTeleport(mvc_player_ped(), pos);
}

void mvc_player_give_weapon(int type, int ammo) {
    void *p = mvc_player_ped();
    if (!p || !GiveWeapon) return;
    if (GetWeaponInfo && off_wmodel >= 0 && RequestModel && LoadAllRequestedModels) {
        uint8_t *info = GetWeaponInfo(type);
        int model = info ? *(int32_t *)(info + off_wmodel) : -1;
        if (model > 0) { RequestModel(model, 1); LoadAllRequestedModels(0); }
    }
    GiveWeapon(p, type, (unsigned)(ammo < 0 ? 0 : ammo), 0);
}

// vehicles
void *mvc_vehicle_spawn(int model, MVCVec3 pos, float heading) {
    if (model < 0 || model >= 1024 || !VehicleNew || !WorldAdd || !RequestModel) return NULL;
    int t = g_vtype[model];
    void (*ctor)(void *, int, uint8_t) = t == VT_CAR ? AutoCtor : t == VT_BIKE ? BikeCtor : t == VT_BOAT ? BoatCtor : NULL;
    void (*tp)(void *, V3) = t == VT_CAR ? AutoTeleport : t == VT_BIKE ? BikeTeleport : t == VT_BOAT ? BoatTeleport : NULL;
    if (!ctor || !tp) { mvc_log_warn("[Loader] model %d is not a car, bike or boat", model); return NULL; }
    RequestModel(model, 1);
    LoadAllRequestedModels(0);
    void *v = VehicleNew(0x5a0);
    if (!v) { mvc_log_warn("[Loader] vehicle pool is full"); return NULL; }
    ctor(v, model, 1 /* random vehicle: the game cleans it up when you leave it */);
    if (pos.z <= -99.f && GroundZ) pos.z = GroundZ(pos.x, pos.y) + 1.0f;
    tp(v, pos);
    if (SetHeading) SetHeading(v, heading * (float)M_PI / 180.f);
    if (t == VT_CAR && PlaceOnRoad) PlaceOnRoad(v);
    if (ClearSpace) ClearSpace(&pos, v);
    WorldAdd(v);
    return v;
}

void mvc_vehicle_fix(void *v) {
    if (v && AutoFix && g_vtype[mvc_vehicle_model(v) & 1023] == VT_CAR) AutoFix(v);
}

// world
void mvc_world_time(int *h, int *m) {
    if (h) *h = ClockHours ? *ClockHours : 0;
    if (m) *m = ClockMinutes ? *ClockMinutes : 0;
}
void mvc_world_set_time(int h, int m) { if (SetGameClock) SetGameClock((uint8_t)((h % 24 + 24) % 24), (uint8_t)((m % 60 + 60) % 60)); }
void mvc_world_set_weather(int w) { if (ForceWeather && w >= 0 && w <= 5) ForceWeather((short)w); }

// 3D
int mvc_world_to_screen(MVCVec3 world, float *sx, float *sy) {
    if (!CalcScreenCoors) return 0;
    V3 out;
    float w, h;
    if (!CalcScreenCoors(&world, &out, &w, &h, 1)) return 0;
    if (sx) *sx = out.x;
    if (sy) *sy = out.y;
    return 1;
}

void mvc_line3d(MVCVec3 a, MVCVec3 b, MVCColor ca, MVCColor cb) {
    if (!RenderLine) return;
    uint32_t c1 = (uint32_t)ca.r << 24 | (uint32_t)ca.g << 16 | (uint32_t)ca.b << 8 | ca.a;
    uint32_t c2 = (uint32_t)cb.r << 24 | (uint32_t)cb.g << 16 | (uint32_t)cb.b << 8 | cb.a;
    RenderLine(a.x, a.y, a.z, b.x, b.y, b.z, c1, c2);
}

void mvc_text3d(MVCVec3 world, const char *s, float scale, MVCColor c) {
    float x, y;
    if (!mvc_world_to_screen(world, &x, &y)) return;
    float u = mvc_screen_h() / 448.f;
    mvc_text(x, y, s, 0.35f * u * scale, 0.75f * u * scale, c, MVC_ALIGN_CENTER, 1, 1);
}

void mvc_corona(uint32_t id, MVCVec3 pos, float size, MVCColor c, float dist) {
    if (RegisterCorona) RegisterCorona(id, c.r, c.g, c.b, c.a, &pos, size, dist, 0, 0, 0, 1, 0, 0.f, 0, 1.5f);
}

void mvc_marker(uint32_t id, MVCVec3 pos, float radius, MVCColor c) {
    if (PlaceMarkerSet) PlaceMarkerSet(id, 4, &pos, radius, c.r, c.g, c.b, c.a, 2048, 0.1f, 0);
}

// sprites
#define MAX_SPRITES 256
static void *g_sprites[MAX_SPRITES]; // each is a CSprite2d { RwTexture * }
static int g_nsprites;

int mvc_sprite_load(const char *spec) {
    if (!spec || g_nsprites >= MAX_SPRITES - 1) return 0;
    void **spr = calloc(1, sizeof(void *));
    const char *colon = strchr(spec, ':');
    char full[PATH_MAX];
    if (colon) { // "models/hud.txd:texture"
        char file[PATH_MAX], name[64], tex[64];
        snprintf(file, sizeof file, "%.*s", (int)(colon - spec), spec);
        snprintf(tex, sizeof tex, "%s", colon + 1);
        const char *base = strrchr(file, '/');
        snprintf(name, sizeof name, "%s", base ? base + 1 : file);
        char *dot = strrchr(name, '.');
        if (dot) *dot = 0;
        if (!TxdFind || !TxdPush || !SpriteSetTexture) { free(spr); return 0; }
        int slot = TxdFind(name);
        if (slot < 0) {
            snprintf(full, sizeof full, "%s/%s", g_game_dir, file);
            slot = TxdAdd(name);
            if (slot < 0 || !TxdLoad(slot, full)) { mvc_log_error("[Loader] cannot load %s", file); free(spr); return 0; }
            TxdAddRef(slot);
        }
        TxdPush();
        TxdSetCurrent(slot);
        SpriteSetTexture(spr, tex);
        TxdPop();
    } else { // PNG
        if (!ReadPNG || !RasterFromImage || !TextureCreate) { free(spr); return 0; }
        snprintf(full, sizeof full, "%s/%s", g_game_dir, spec);
        void *img = ReadPNG(full);
        if (!img) { mvc_log_error("[Loader] cannot read %s", spec); free(spr); return 0; }
        void *ras = RasterFromImage(img, 0);
        if (ImageDestroy) ImageDestroy(img);
        if (!ras) { free(spr); return 0; }
        spr[0] = TextureCreate(ras);
    }
    if (!spr[0]) { mvc_log_error("[Loader] texture not found: %s", spec); free(spr); return 0; }
    g_sprites[++g_nsprites] = spr;
    return g_nsprites;
}

void mvc_sprite_draw(int id, float l, float t, float r, float b, MVCColor tint) {
    if (id <= 0 || id > g_nsprites || !SpriteDraw) return;
    CRect rc = {l, b, r, t};
    SpriteDraw(g_sprites[id], &rc, &tint);
}

// audio (OpenAL)
static void (*alGenBuffers_)(int, unsigned *), (*alGenSources_)(int, unsigned *);
static void (*alBufferData_)(unsigned, int, const void *, int, int);
static void (*alSourcei_)(unsigned, int, int), (*alSourcef_)(unsigned, int, float);
static void (*alSource3f_)(unsigned, int, float, float, float);
static void (*alSourcePlay_)(unsigned), (*alSourceStop_)(unsigned);
static void (*alGetSourcei_)(unsigned, int, int *);
static void (*alDeleteSources_)(int, const unsigned *), (*alDeleteBuffers_)(int, const unsigned *);
static int (*mpg_init)(void), (*mpg_open)(void *, const char *), (*mpg_getformat)(void *, long *, int *, int *);
static int (*mpg_format_none)(void *), (*mpg_format)(void *, long, int, int), (*mpg_read)(void *, void *, size_t, size_t *);
static void *(*mpg_new)(const char *, int *);
static int (*mpg_close)(void *);
static void (*mpg_delete)(void *);

typedef struct { unsigned src, buf; float vol; int active; } Voice;
static Voice g_voices[32];

static float sfx_volume(void) {
    static uint32_t last;
    static float v = 0.75f;
    uint32_t now = mvc_time_ms();
    if (now - last < 2000 && last) return v;
    last = now;
    char path[PATH_MAX], val[32];
    snprintf(path, sizeof path, "%s/Library/Application Support/macVC/macVC.ini", getenv("HOME") ? getenv("HOME") : "");
    if (mvc_ini_get(path, "Audio", "SfxVolume", val, sizeof val)) {
        float f = (float)atof(val) / 64.f;
        v = f < 0 ? 0 : f > 1 ? 1 : f;
    }
    return v;
}

static int decode_wav(const char *path, void **pcm, int *bytes, int *fmt, int *rate) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) { fclose(f); return 0; }
    int ch = 0, bits = 0, ok = 0;
    for (;;) {
        uint8_t c[8];
        if (fread(c, 1, 8, f) != 8) break;
        uint32_t sz = c[4] | c[5] << 8 | c[6] << 16 | (uint32_t)c[7] << 24;
        if (!memcmp(c, "fmt ", 4)) {
            uint8_t fm[16];
            if (sz < 16 || fread(fm, 1, 16, f) != 16) break;
            if ((fm[0] | fm[1] << 8) != 1) break; // PCM only
            ch = fm[2] | fm[3] << 8;
            *rate = (int)(fm[4] | fm[5] << 8 | fm[6] << 16 | (uint32_t)fm[7] << 24);
            bits = fm[14] | fm[15] << 8;
            fseek(f, (long)(sz - 16 + (sz & 1)), SEEK_CUR);
        } else if (!memcmp(c, "data", 4)) {
            *pcm = malloc(sz);
            *bytes = (int)fread(*pcm, 1, sz, f);
            ok = 1;
            break;
        } else fseek(f, (long)(sz + (sz & 1)), SEEK_CUR);
    }
    fclose(f);
    if (!ok || !ch || (bits != 8 && bits != 16) || ch > 2) { if (ok) free(*pcm); return 0; }
    *fmt = ch == 1 ? (bits == 8 ? 0x1100 : 0x1101) : (bits == 8 ? 0x1102 : 0x1103);
    return 1;
}

static int decode_mp3(const char *path, void **pcm, int *bytes, int *fmt, int *rate) {
    if (!mpg_new) return 0;
    static int inited;
    if (!inited) { mpg_init(); inited = 1; }
    int err;
    void *h = mpg_new(NULL, &err);
    if (!h) return 0;
    if (mpg_open(h, path) != 0) { mpg_delete(h); return 0; }
    long r; int ch, enc;
    mpg_getformat(h, &r, &ch, &enc);
    mpg_format_none(h);
    mpg_format(h, r, ch, 0xD0 /* MPG123_ENC_SIGNED_16 */);
    size_t cap = 1 << 20, len = 0, got;
    uint8_t *buf = malloc(cap);
    for (;;) {
        if (cap - len < 65536) { cap *= 2; buf = realloc(buf, cap); }
        int rc = mpg_read(h, buf + len, cap - len, &got);
        len += got;
        if (rc != 0 && rc != -10 /* NEW_FORMAT */) break;
        if (!got && rc != 0) break;
    }
    mpg_close(h);
    mpg_delete(h);
    if (!len) { free(buf); return 0; }
    *pcm = buf; *bytes = (int)len; *rate = (int)r;
    *fmt = ch == 1 ? 0x1101 : 0x1103;
    return 1;
}

int mvc_sound_play(const char *path, float volume, int loop) {
    if (!alGenSources_ || !path) return 0;
    char full[PATH_MAX];
    if (path[0] == '/') snprintf(full, sizeof full, "%s", path);
    else snprintf(full, sizeof full, "%s/%s", g_game_dir, path);
    void *pcm = NULL;
    int bytes = 0, fmt = 0, rate = 0;
    size_t L = strlen(full);
    int ok = (L > 4 && !strcasecmp(full + L - 4, ".mp3")) ? decode_mp3(full, &pcm, &bytes, &fmt, &rate)
                                                          : decode_wav(full, &pcm, &bytes, &fmt, &rate);
    if (!ok) { mvc_log_error("[Loader] cannot play %s (WAV PCM or MP3 only)", path); return 0; }
    int slot = -1;
    for (int i = 0; i < 32; i++) if (!g_voices[i].active) { slot = i; break; }
    if (slot < 0) { free(pcm); return 0; }
    Voice *v = &g_voices[slot];
    alGenBuffers_(1, &v->buf);
    alBufferData_(v->buf, fmt, pcm, bytes, rate);
    free(pcm);
    alGenSources_(1, &v->src);
    alSourcei_(v->src, 0x1009 /* AL_BUFFER */, (int)v->buf);
    alSourcei_(v->src, 0x202 /* AL_SOURCE_RELATIVE */, 1);
    alSource3f_(v->src, 0x1004 /* AL_POSITION */, 0, 0, 0);
    alSourcei_(v->src, 0x1007 /* AL_LOOPING */, loop ? 1 : 0);
    v->vol = volume;
    alSourcef_(v->src, 0x100A /* AL_GAIN */, volume * sfx_volume());
    alSourcePlay_(v->src);
    v->active = 1;
    return slot + 1;
}

void mvc_sound_stop(int h) {
    if (h < 1 || h > 32 || !g_voices[h - 1].active) return;
    alSourceStop_(g_voices[h - 1].src);
}

void sdk_frame(void) {
    if (!alGetSourcei_) return;
    float sv = sfx_volume();
    for (int i = 0; i < 32; i++) {
        Voice *v = &g_voices[i];
        if (!v->active) continue;
        int st = 0;
        alGetSourcei_(v->src, 0x1010 /* AL_SOURCE_STATE */, &st);
        if (st == 0x1014 /* AL_STOPPED */) {
            alDeleteSources_(1, &v->src);
            alDeleteBuffers_(1, &v->buf);
            v->active = 0;
        } else {
            alSourcef_(v->src, 0x100A, v->vol * sv);
        }
    }
}

// init
// first load of `size` with base register x0 after a BL to `callee` inside `func`
static int load_after_call(void *func, void *callee, int max, int size) {
    if (!func || !callee) return -1;
    const uint32_t *p = func;
    for (int k = 0; k < max; k++) {
        uint32_t i = p[k];
        if ((i & 0xFC000000) != 0x94000000) continue;
        int64_t off = (int64_t)((uint64_t)(i & 0x3FFFFFF) << 38) >> 36;
        if ((uintptr_t)&p[k] + off != (uintptr_t)callee) continue;
        for (int j = k + 1; j < k + 8 && j < max; j++) {
            uint32_t q = p[j];
            uint32_t top = q & 0xFFC00000;
            int s = top == 0xB9400000 ? 4 : top == 0x79400000 ? 2 : top == 0x79800000 ? 2 : 0;
            if (s == size && ((q >> 5) & 31) == 0) return (int)((q >> 10) & 0xFFF) * s;
        }
        return -1;
    }
    return -1;
}

#define S(v, n) (*(void **)&(v) = mvc_sym(n))

int sdk_init(void) {
    S(FindPlayerPed, "_Z13FindPlayerPedv");
    S(FindPlayerVehicle, "_Z17FindPlayerVehiclev");
    S(FindPlayerCoors, "_Z15FindPlayerCoorsv");
    S(PlayerSetWanted, "_ZN10CPlayerPed14SetWantedLevelEi");
    S(PedTeleport, "_ZN4CPed8TeleportE7CVector");
    S(AutoTeleport, "_ZN11CAutomobile8TeleportE7CVector");
    S(BikeTeleport, "_ZN5CBike8TeleportE7CVector");
    S(BoatTeleport, "_ZN5CBoat8TeleportE7CVector");
    S(GiveWeapon, "_ZN4CPed10GiveWeaponE11eWeaponTypejb");
    S(GetWeaponInfo, "_ZN11CWeaponInfo13GetWeaponInfoE11eWeaponType");
    S(RequestModel, "_ZN10CStreaming12RequestModelEii");
    S(LoadAllRequestedModels, "_ZN10CStreaming22LoadAllRequestedModelsEb");
    S(VehicleNew, "_ZN8CVehiclenwEm");
    S(AutoCtor, "_ZN11CAutomobileC1Eih");
    S(BikeCtor, "_ZN5CBikeC1Eih");
    S(BoatCtor, "_ZN5CBoatC1Eih");
    S(WorldAdd, "_ZN6CWorld3AddEP7CEntity");
    S(ClearSpace, "_ZN11CTheScripts26ClearSpaceForMissionEntityERK7CVectorP7CEntity");
    S(SetHeading, "_ZN10CPlaceable10SetHeadingEf");
    S(PlaceOnRoad, "_ZN11CAutomobile19PlaceOnRoadProperlyEv");
    S(GroundZ, "_ZN6CWorld19FindGroundZForCoordEff");
    S(AutoFix, "_ZN11CAutomobile3FixEv");
    S(SetGameClock, "_ZN6CClock12SetGameClockEhh");
    S(ForceWeather, "_ZN8CWeather15ForceWeatherNowEs");
    S(ClockHours, "_ZN6CClock18ms_nGameClockHoursE");
    S(ClockMinutes, "_ZN6CClock20ms_nGameClockMinutesE");
    S(Players, "_ZN6CWorld7PlayersE");
    S(PlayerInFocus, "_ZN6CWorld13PlayerInFocusE");
    S(CalcScreenCoors, "_ZN7CSprite15CalcScreenCoorsERKN2rw3V3dEPS1_PfS5_b");
    S(RenderLine, "_ZN6CLines22RenderLineWithClippingEffffffjj");
    S(RegisterCorona, "_ZN8CCoronas14RegisterCoronaEjhhhhRK7CVectorffhahhhfbf");
    S(PlaceMarkerSet, "_ZN10C3dMarkers14PlaceMarkerSetEjtR7CVectorfhhhhtfs");
    S(SpriteDraw, "_ZN9CSprite2d4DrawERK5CRectRK5CRGBA");
    S(SpriteSetTexture, "_ZN9CSprite2d10SetTextureEPKc");
    S(TxdFind, "_ZN9CTxdStore11FindTxdSlotEPKc");
    S(TxdAdd, "_ZN9CTxdStore10AddTxdSlotEPKc");
    S(TxdLoad, "_ZN9CTxdStore7LoadTxdEiPKc");
    S(TxdAddRef, "_ZN9CTxdStore6AddRefEi");
    S(TxdPush, "_ZN9CTxdStore14PushCurrentTxdEv");
    S(TxdPop, "_ZN9CTxdStore13PopCurrentTxdEv");
    S(TxdSetCurrent, "_ZN9CTxdStore13SetCurrentTxdEi");
    S(ReadPNG, "_ZN2rw7readPNGEPKc");
    S(RasterFromImage, "_ZN2rw6Raster15createFromImageEPNS_5ImageEi");
    S(TextureCreate, "_ZN2rw7Texture6createEPNS_6RasterE");
    S(ImageDestroy, "_ZN2rw5Image7destroyEv");

    // offsets, read from this build's code
    uint32_t *spi = mvc_sym("_ZN11CPlayerInfo14SavePlayerInfoEPhPj");
    if (spi && (spi[0] & 0xFF800000) == 0x52800000) size_pinfo = (spi[0] >> 5) & 0xFFFF;
    off_money = mvc_first_load_imm(spi, 12, 4);
    off_model = mvc_first_load_imm(mvc_sym("_ZN8CVehicle9UsesSirenEv"), 2, 2);
    // CHud::Draw reads the player's health then armour as floats
    const uint32_t *hud = mvc_sym("_ZN4CHud4DrawEv");
    for (int k = 0; hud && k < 3000 && off_health < 0; k++) {
        uint32_t i = hud[k];
        if ((i & 0xFFC00000) == 0xBD400000) {
            int imm = (int)((i >> 10) & 0xFFF) * 4;
            if (imm >= 0x300 && imm < 0x800) { off_health = imm; off_armour = imm + 4; }
        }
    }
    // FindPlayerCoors returns the entity matrix position: ADD Xd, Xn, #pos
    const uint32_t *fpc = mvc_sym("_Z15FindPlayerCoorsv");
    // (the first one is TheCamera's in replay mode; the vehicle/ped ones come last)
    for (int k = 0; fpc && k < 40; k++) {
        if (fpc[k] == 0xD65F03C0 && k > 30) break; // ret near the end
        if ((fpc[k] & 0xFFC00000) == 0x91000000) {
            int imm = (int)((fpc[k] >> 10) & 0xFFF);
            if (imm >= 0x20 && imm <= 0x80) off_pos = imm;
        }
    }
    off_wmodel = load_after_call(mvc_sym("_ZN4CPed16SetCurrentWeaponEi"), (void *)GetWeaponInfo, 80, 4);
    mvc_log("[Loader] SDK offsets: position +0x%x, money +0x%x, health +0x%x, armour +0x%x, model +0x%x, weapon model +0x%x",
            off_pos, off_money, off_health, off_armour, off_model, off_wmodel);
    load_vehicle_types();

    void *al = dlopen("@executable_path/../Frameworks/libopenal.1.dylib", RTLD_NOW | RTLD_NOLOAD);
    if (!al) al = RTLD_DEFAULT;
    *(void **)&alGenBuffers_ = dlsym(al, "alGenBuffers");
    *(void **)&alGenSources_ = dlsym(al, "alGenSources");
    *(void **)&alBufferData_ = dlsym(al, "alBufferData");
    *(void **)&alSourcei_ = dlsym(al, "alSourcei");
    *(void **)&alSourcef_ = dlsym(al, "alSourcef");
    *(void **)&alSource3f_ = dlsym(al, "alSource3f");
    *(void **)&alSourcePlay_ = dlsym(al, "alSourcePlay");
    *(void **)&alSourceStop_ = dlsym(al, "alSourceStop");
    *(void **)&alGetSourcei_ = dlsym(al, "alGetSourcei");
    *(void **)&alDeleteSources_ = dlsym(al, "alDeleteSources");
    *(void **)&alDeleteBuffers_ = dlsym(al, "alDeleteBuffers");
    void *mp = dlopen("@executable_path/../Frameworks/libmpg123.dylib", RTLD_NOW | RTLD_NOLOAD);
    if (!mp) mp = RTLD_DEFAULT;
    *(void **)&mpg_init = dlsym(mp, "mpg123_init");
    *(void **)&mpg_new = dlsym(mp, "mpg123_new");
    *(void **)&mpg_open = dlsym(mp, "mpg123_open");
    *(void **)&mpg_getformat = dlsym(mp, "mpg123_getformat");
    *(void **)&mpg_format_none = dlsym(mp, "mpg123_format_none");
    *(void **)&mpg_format = dlsym(mp, "mpg123_format");
    *(void **)&mpg_read = dlsym(mp, "mpg123_read");
    *(void **)&mpg_close = dlsym(mp, "mpg123_close");
    *(void **)&mpg_delete = dlsym(mp, "mpg123_delete");
    if (!alGenSources_) mvc_log_warn("[Loader] OpenAL not found - mvc_sound_play disabled");
    return 1;
}
