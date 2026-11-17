// gasoline, fuel for macVC.
// port of Gasoline v1.1 (the cleo fuel mod in Big Cojones) with the original 7 stations,
// pump zones, N to refuel, prices and burn rate. fuel is in gallons.
//


// every vehicle has its own tank and burns fuel with speed. empty tank, no power.
// pull into a pump zone and hold N (or the pad button) to buy fuel.
// settings: GameData/plugins/Gasoline.ini
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "../../include/macvc.h"

MVC_PLUGIN("Gasoline", "1.0", "Big Cojones (macVC port)", "",
           "Fuel, the 7 original gas stations and paid refueling - port of the Gasoline v1.1 CLEO mod");

typedef struct { float x, y, z; } V3;
typedef struct { float x, y; } V2;

// game API
static void *(*FindPlayerVehicle)(void);
static void *(*FindPlayerPed)(void);
static V3 (*FindPlayerCoors)(void);
static V3 *(*FindPlayerSpeed)(void);
static void *(*CPad_GetPad)(int);
static int16_t (*o_GetAccelerate)(void *);
static int16_t (*o_GetBrake)(void *);
static void (*SetHelpMessage)(uint16_t *, _Bool, _Bool);
static void (*AddMessageJumpQ)(uint16_t *, uint32_t, uint16_t);
static void (*RadarWorldToRadar)(V2 *, const V2 *);
static float (*RadarLimit)(V2 *);
static void (*RadarToScreen)(V2 *, const V2 *);
static void (*ShowRadarTrace)(float, float, uint32_t, uint8_t, uint8_t, uint8_t, uint8_t);
static void (*DrawRadarSprite)(uint16_t, float, float, uint8_t);
static void (*PlaceMarkerSet)(uint32_t id, uint16_t type, V3 *pos, float size, uint8_t r, uint8_t g, uint8_t b,
                              uint8_t a, uint16_t pulsePeriod, float pulseFraction, int16_t rotateRate);
static _Bool (*IsPlayerOnAMission)(void);
static uint8_t *Players, *PlayerInFocus;
static int off_model = 0x6a, off_money = 0xc8, size_playerinfo = 0x1a0;

// settings
typedef struct { char name[48]; V3 pos; V3 half; } Station; // half = zone half-size (x, y, z)
#define MAX_STATIONS 128
static Station st[MAX_STATIONS];
static int nst;

enum { HUD_MODERN, HUD_ORIGINAL };
static struct {
    float tank, startMin, startMax, mpg, refuelGps, pricePerGallon, lowFuel, emptyBelow;
    int enabled, refuelKey, refuelPad, positionKey, requireStop;
    char refuelKeyName[24];
    int drainOnMissions, showRadar, radarIcon, showMarkers, hudStyle;
    float hudRight, hudTop;
    int exclude[32], nexclude;
} cfg;

static const char *DEFAULT_INI =
"; Gasoline for macVC - port of the Big Cojones Gasoline v1.1 CLEO mod\n"
"; Defaults reproduce the original: 7 stations, hold N to refuel, $5 + 0.4 units per frame.\n"
"[Gasoline]\n"
"; 0 = fuel system off (no fuel use, no HUD). Also in Options > Mods > Gasoline\n"
"Enabled=1\n"
"; tank size in gallons (original: 65 units)\n"
"TankGallons=17.2\n"
"; fuel in vehicles you get into, random between these (original: 3-20 units)\n"
"StartGallonsMin=0.8\n"
"StartGallonsMax=5.3\n"
"; fuel economy - fuel burns with distance driven (original: speed / 13360 per frame)\n"
"MilesPerGallon=1.05\n"
"; refuel speed and price (original: 0.4 units for $5 every frame at 30 fps)\n"
"RefuelGallonsPerSecond=3.17\n"
"PricePerGallon=47.30\n"
"; low-fuel warning, in gallons\n"
"LowFuelGallons=2.5\n"
"; refuel controls. Keys: A-Z, 0-9, F1-F12, SPACE, ENTER, TAB\n"
"; Pad buttons: A B X Y LB RB BACK START LTHUMB RTHUMB DPAD_UP DPAD_DOWN DPAD_LEFT DPAD_RIGHT\n"
"RefuelKey=N\n"
"RefuelPadButton=DPAD_RIGHT\n"
"; logs your position to the ~ console so you can add stations\n"
"PositionKey=F9\n"
"; 1 = you must come to a stop at the pump before you can refuel\n"
"RequireStop=1\n"
"; 0 = fuel does not drain during missions\n"
"DrainOnMissions=1\n"
"; radar icon for stations (original: 33). 0 = small orange square instead\n"
"ShowStationsOnRadar=1\n"
"RadarIcon=33\n"
"; ring marker around each pump zone, like mission markers (not in the original)\n"
"ShowPumpMarkers=1\n"
"; Modern = gauge under the wanted stars, Original = the CLEO mod's bar under the radar\n"
"HudStyle=Modern\n"
"; Modern gauge position, in 640x448 HUD units from the top-right corner\n"
"HudRight=103\n"
"HudTop=110\n"
"; vehicle model IDs that never use fuel (RC Bandit, RC Baron, RC Raider, RC Goblin)\n"
"ExcludeModels=171,194,195,231\n"
"\n"
"[Stations]\n"
"; Name, X, Y, Z, zone half-size X, Y, Z  (original zones: 3, 3, 1.5)\n"
"Station=Ocean Beach,61.64,-1073.61,9.98,3,3,1.5\n"
"Station=Downtown,-928.62,1153.44,10.81,3,3,1.5\n"
"Station=Downtown,-599.90,620.08,11.28,3,3,1.5\n"
"Station=Little Havana,-1007.04,-850.03,6.68,3,3,1.5\n"
"Station=Viceport,-890.42,-1245.43,11.04,3,3,1.5\n"
"Station=Vice Point,328.40,455.07,10.04,3,3,1.5\n"
"Station=Washington Beach,432.22,-569.07,10.07,3,3,1.5\n";

static int parse_key(const char *s) {
    if (!s[0]) return -1;
    if (!s[1] && isalpha((unsigned char)s[0])) return toupper((unsigned char)s[0]);
    if (!s[1] && isdigit((unsigned char)s[0])) return s[0];
    if ((s[0] == 'F' || s[0] == 'f') && isdigit((unsigned char)s[1])) {
        int n = atoi(s + 1);
        if (n >= 1 && n <= 12) return 289 + n;
    }
    if (!strcasecmp(s, "SPACE")) return 32;
    if (!strcasecmp(s, "ENTER")) return 257;
    if (!strcasecmp(s, "TAB")) return 258;
    return -1;
}

static int parse_pad(const char *s) {
    static const char *n[] = {"A", "B", "X", "Y", "LB", "RB", "BACK", "START", "GUIDE",
                              "LTHUMB", "RTHUMB", "DPAD_UP", "DPAD_RIGHT", "DPAD_DOWN", "DPAD_LEFT"};
    for (int i = 0; i < 15; i++) if (!strcasecmp(s, n[i])) return i;
    return -1;
}

static const char *pad_label(void) {
    static const char *n[] = {"A", "B", "X", "Y", "LB", "RB", "Back", "Start", "Guide",
                              "L3", "R3", "D-Pad Up", "D-Pad Right", "D-Pad Down", "D-Pad Left"};
    return cfg.refuelPad >= 0 ? n[cfg.refuelPad] : "";
}

static void trim(char *s) {
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    char *b = s;
    while (isspace((unsigned char)*b)) b++;
    if (b != s) memmove(s, b, strlen(b) + 1);
}

static void load_config(void) {
    cfg.tank = 17.2f; cfg.startMin = 0.8f; cfg.startMax = 5.3f; cfg.mpg = 1.05f;
    cfg.refuelGps = 3.17f; cfg.pricePerGallon = 47.3f; cfg.lowFuel = 2.5f;
    cfg.refuelKey = 'N'; cfg.refuelPad = 12; cfg.positionKey = 298;
    strcpy(cfg.refuelKeyName, "N");
    cfg.requireStop = 1;
    cfg.enabled = 1;
    cfg.drainOnMissions = 1; cfg.showRadar = 1; cfg.radarIcon = 33; cfg.showMarkers = 1;
    cfg.hudStyle = HUD_MODERN; cfg.hudRight = 103; cfg.hudTop = 110;
    cfg.exclude[0] = 171; cfg.exclude[1] = 194; cfg.exclude[2] = 195; cfg.exclude[3] = 231; cfg.nexclude = 4;

    char path[1024];
    snprintf(path, sizeof path, "%s/plugins/Gasoline.ini", mvc_game_dir());
    FILE *f = fopen(path, "r");
    if (f) { // replace the old (v1) ini that had the wrong stations
        char probe[256] = {0};
        fread(probe, 1, sizeof probe - 1, f);
        if (!strstr(probe, "Gasoline v1.1")) {
            fclose(f);
            char old[1100];
            snprintf(old, sizeof old, "%s.old", path);
            rename(path, old);
            mvc_log("old Gasoline.ini saved as Gasoline.ini.old");
            f = NULL;
        } else rewind(f);
    }
    if (!f) {
        f = fopen(path, "w");
        if (f) { fputs(DEFAULT_INI, f); fclose(f); mvc_log("created default plugins/Gasoline.ini"); }
        f = fopen(path, "r");
        if (!f) { mvc_log_error("cannot read plugins/Gasoline.ini"); return; }
    }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        trim(line);
        if (!line[0] || line[0] == ';' || line[0] == '[') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        trim(k); trim(v);
        float fv = (float)atof(v);
        if (!strcasecmp(k, "TankGallons")) cfg.tank = fmaxf(0.5f, fv);
        else if (!strcasecmp(k, "StartGallonsMin")) cfg.startMin = fv;
        else if (!strcasecmp(k, "StartGallonsMax")) cfg.startMax = fv;
        else if (!strcasecmp(k, "MilesPerGallon")) cfg.mpg = fmaxf(0.01f, fv);
        else if (!strcasecmp(k, "RefuelGallonsPerSecond")) cfg.refuelGps = fmaxf(0.01f, fv);
        else if (!strcasecmp(k, "PricePerGallon")) cfg.pricePerGallon = fmaxf(0, fv);
        else if (!strcasecmp(k, "LowFuelGallons")) cfg.lowFuel = fv;
        else if (!strcasecmp(k, "Enabled")) cfg.enabled = atoi(v) != 0;
        else if (!strcasecmp(k, "RefuelKey")) { int x = parse_key(v); if (x < 0) x = mvc_key_from_name(v); if (x >= 0) { cfg.refuelKey = x; snprintf(cfg.refuelKeyName, 24, "%s", v); } else mvc_log_error("unknown RefuelKey %s", v); }
        else if (!strcasecmp(k, "RefuelPadButton")) { cfg.refuelPad = parse_pad(v); if (cfg.refuelPad < 0 && strcasecmp(v, "NONE")) mvc_log_error("unknown RefuelPadButton %s", v); }
        else if (!strcasecmp(k, "PositionKey")) { cfg.positionKey = parse_key(v); if (cfg.positionKey < 0) cfg.positionKey = mvc_key_from_name(v); }
        else if (!strcasecmp(k, "DrainOnMissions")) cfg.drainOnMissions = atoi(v);
        else if (!strcasecmp(k, "RequireStop")) cfg.requireStop = atoi(v);
        else if (!strcasecmp(k, "ShowStationsOnRadar")) cfg.showRadar = atoi(v);
        else if (!strcasecmp(k, "RadarIcon")) cfg.radarIcon = atoi(v);
        else if (!strcasecmp(k, "ShowPumpMarkers")) cfg.showMarkers = atoi(v);
        else if (!strcasecmp(k, "HudStyle")) cfg.hudStyle = (!strcasecmp(v, "Original") || !strcmp(v, "1")) ? HUD_ORIGINAL : HUD_MODERN;
        else if (!strcasecmp(k, "HudRight")) cfg.hudRight = fv;
        else if (!strcasecmp(k, "HudTop")) cfg.hudTop = fv;
        else if (!strcasecmp(k, "ExcludeModels")) {
            cfg.nexclude = 0;
            for (char *t = strtok(v, ","); t && cfg.nexclude < 32; t = strtok(NULL, ","))
                cfg.exclude[cfg.nexclude++] = atoi(t);
        } else if (!strcasecmp(k, "Station") && nst < MAX_STATIONS) {
            Station *s = &st[nst];
            char *c = strchr(v, ',');
            if (!c) continue;
            *c = 0;
            snprintf(s->name, sizeof s->name, "%s", v);
            trim(s->name);
            int n = sscanf(c + 1, "%f , %f , %f , %f , %f , %f", &s->pos.x, &s->pos.y, &s->pos.z,
                           &s->half.x, &s->half.y, &s->half.z);
            if (n == 4) { s->half.y = s->half.x; s->half.z = 3; n = 6; }
            if (n == 6) nst++;
            else mvc_log_error("bad Station line for %s", s->name);
        }
    }
    fclose(f);
    if (cfg.startMax < cfg.startMin) cfg.startMax = cfg.startMin;
    if (cfg.startMax > cfg.tank) cfg.startMax = cfg.tank;
    cfg.emptyBelow = cfg.tank * (0.1f / 65.f); // original: engine dies at 0.1 of 65 units
    mvc_log("%d gas stations, %.1f gal tank, %.2f mpg, $%.2f/gal, refuel with %s / %s",
            nst, cfg.tank, cfg.mpg, cfg.pricePerGallon, mvc_key_name(cfg.refuelKey), pad_label());
}

// fuel state
typedef struct { void *veh; uint16_t model; float fuel; uint32_t seen; } Tank;
static Tank tanks[96];
static Tank *cur;              // tank of the vehicle the player is in
static int empty;              // current vehicle has no fuel
static int atStation, refueling, warnedLow, warnedEmpty;
static float costAcc, sessionCost, sessionFuel;
static int nearestIdx = -1;
static float nearestDist;
static void *pad0;

static int excluded(uint16_t m) {
    for (int i = 0; i < cfg.nexclude; i++) if (cfg.exclude[i] == m) return 1;
    return 0;
}

// fuel of parked cars, restored after loading a save (matched by model + position)
typedef struct { uint16_t model; float x, y, z, fuel; } SavedTank;
static SavedTank saved[192];
static int nsaved;

// remembers a tank whose vehicle is leaving the world. Garages store a parked
// car and delete it when you walk away, then rebuild it on the same spot when
// you come back (or after loading a save), so it gets its fuel back from here.
static void remember(uint16_t model, MVCVec3 p, float fuel) {
    for (int i = 0; i < nsaved; i++) {
        float dx = saved[i].x - p.x, dy = saved[i].y - p.y, dz = saved[i].z - p.z;
        if (saved[i].model == model && dx * dx + dy * dy + dz * dz < 16.f) { saved[i].fuel = fuel; return; }
    }
    if (nsaved == 192) memmove(saved, saved + 1, (size_t)--nsaved * sizeof *saved); // forget the oldest
    saved[nsaved++] = (SavedTank){model, p.x, p.y, p.z, fuel};
}

static Tank *tank_for(void *veh, uint16_t model, uint32_t now) {
    Tank *free_slot = NULL, *oldest = &tanks[0];
    for (int i = 0; i < 96; i++) {
        Tank *t = &tanks[i];
        if (t->veh == veh && t->model == model) { t->seen = now; return t; }
        if (!t->veh && !free_slot) free_slot = t;
        if (t->seen < oldest->seen) oldest = t;
    }
    Tank *t = free_slot ? free_slot : oldest;
    t->veh = veh;
    t->model = model;
    t->seen = now;
    t->fuel = cfg.startMin + (cfg.startMax - cfg.startMin) * (float)arc4random_uniform(1001) / 1000.f;
    MVCVec3 p = mvc_entity_pos(veh);
    for (int i = 0; i < nsaved; i++) {
        float dx = saved[i].x - p.x, dy = saved[i].y - p.y, dz = saved[i].z - p.z;
        if (saved[i].model == model && dx * dx + dy * dy + dz * dz < 16.f) {
            t->fuel = saved[i].fuel;
            saved[i] = saved[--nsaved];
            break;
        }
    }
    return t;
}

static int32_t *money_ptr(void) {
    if (!Players || !PlayerInFocus) return NULL;
    return (int32_t *)(Players + (size_t)*PlayerInFocus * size_playerinfo + off_money);
}

static float player_speed(void) { // game units per 1/50 s
    V3 *s = FindPlayerSpeed ? FindPlayerSpeed() : NULL;
    return s ? sqrtf(s->x * s->x + s->y * s->y + s->z * s->z) : 0.f;
}

static void to_game_text(uint16_t *dst, int max, const char *ascii) {
    int n = 0;
    for (; ascii[n] && n < max - 1; n++) dst[n] = (unsigned char)ascii[n];
    dst[n] = 0;
}

static void help(const char *ascii) {
    static uint16_t buf[256];
    to_game_text(buf, 256, ascii);
    if (SetHelpMessage) SetHelpMessage(buf, 0, 0);
}

static void on_event(int ev, void *arg) {
    if (ev == MVC_EV_SAVE_WRITING) {
        // cars in the world, plus cars a garage is holding right now
        SavedTank out[96 + 192];
        int n = 0;
        for (int i = 0; i < 96; i++) {
            if (!tanks[i].veh) continue;
            MVCVec3 p = mvc_entity_pos(tanks[i].veh);
            out[n++] = (SavedTank){tanks[i].model, p.x, p.y, p.z, tanks[i].fuel};
        }
        for (int i = 0; i < nsaved; i++) out[n++] = saved[i];
        mvc_save_set("gasoline.tanks", out, (size_t)n * sizeof *out);
        return;
    }
    if (ev == MVC_EV_SAVE_LOADED) {
        int len = mvc_save_get("gasoline.tanks", saved, sizeof saved);
        nsaved = len > 0 ? len / (int)sizeof *saved : 0;
        if (nsaved) mvc_log("restored fuel for %d parked vehicle(s)", nsaved);
        return;
    }
    if (ev == MVC_EV_NEW_SESSION && mvc_save_slot() == 0) nsaved = 0; // new game
    if (ev == MVC_EV_VEHICLE_DESTROYED) {
        for (int i = 0; i < 96; i++)
            if (tanks[i].veh == arg) {
                remember(tanks[i].model, mvc_entity_pos(arg), tanks[i].fuel);
                tanks[i].veh = NULL;
            }
        if (cur && cur->veh == NULL) { cur = NULL; empty = 0; }
    } else if (ev == MVC_EV_NEW_SESSION) {
        memset(tanks, 0, sizeof tanks);
        cur = NULL; empty = 0; refueling = 0;
    }
}

// input hooks
static int16_t h_GetAccelerate(void *pad) {
    int16_t r = o_GetAccelerate(pad);
    if (pad == pad0 && cur && empty) return 0;
    return r;
}

static int16_t h_GetBrake(void *pad) {
    int16_t r = o_GetBrake(pad);
    // with no fuel the brake still stops the car, but cannot drive it in reverse
    if (pad == pad0 && cur && empty && player_speed() < 0.03f) return 0;
    return r;
}

// per frame
static uint32_t lastMs;

static void tick(void) {
    uint32_t now = mvc_time_ms();
    float dt = (now - lastMs) / 1000.f;
    if (now < lastMs || dt > 0.25f) dt = 0;
    lastMs = now;

    if (cfg.positionKey > 0 && mvc_key_pressed(cfg.positionKey) && FindPlayerPed && FindPlayerPed()) {
        V3 p = FindPlayerCoors();
        mvc_log("position: Station=New Station,%.2f,%.2f,%.2f,3,3,1.5", p.x, p.y, p.z);
        help("Gasoline: position written to the console (tilde key).");
    }

    if (!cfg.enabled) { cur = NULL; empty = 0; atStation = refueling = 0; return; }
    void *veh = FindPlayerVehicle ? FindPlayerVehicle() : NULL;
    uint16_t model = veh ? *(uint16_t *)((uint8_t *)veh + off_model) : 0;
    if (!veh || excluded(model)) {
        cur = NULL; empty = 0; atStation = refueling = 0;
        return;
    }
    if (!cur || cur->veh != veh) { warnedLow = 0; warnedEmpty = 0; refueling = 0; }
    cur = tank_for(veh, model, now);

    // burn fuel by distance travelled
    int drain = cfg.drainOnMissions || !(IsPlayerOnAMission && IsPlayerOnAMission());
    if (drain && dt > 0) {
        float metresPerSec = player_speed() * 50.f;
        cur->fuel -= metresPerSec * dt / 1609.344f / cfg.mpg;
        if (cur->fuel < 0) cur->fuel = 0;
    }
    empty = cur->fuel <= cfg.emptyBelow;

    // pump zones (axis-aligned boxes, like the original's locate check)
    V3 p = FindPlayerCoors();
    nearestIdx = -1;
    nearestDist = 1e9f;
    atStation = 0;
    for (int i = 0; i < nst; i++) {
        float dx = st[i].pos.x - p.x, dy = st[i].pos.y - p.y, dz = st[i].pos.z - p.z;
        float d = sqrtf(dx * dx + dy * dy);
        if (d < nearestDist) { nearestDist = d; nearestIdx = i; }
        if (fabsf(dx) <= st[i].half.x && fabsf(dy) <= st[i].half.y && fabsf(dz) <= st[i].half.z) atStation = 1;
    }

    if (cfg.showMarkers && PlaceMarkerSet) {
        // the game's own mission-marker cylinder (type 4, as CTheScripts::DrawScriptSpheres uses)
        for (int i = 0; i < nst; i++) {
            float dx = st[i].pos.x - p.x, dy = st[i].pos.y - p.y;
            if (dx * dx + dy * dy > 150.f * 150.f) continue;
            V3 c = {st[i].pos.x, st[i].pos.y, st[i].pos.z - 1.0f};
            float r = fminf(st[i].half.x, st[i].half.y);
            PlaceMarkerSet(0x6A510000u + (uint32_t)i, 4, &c, r, 255, 150, 40, 228, 2048, 0.1f, 0);
        }
    }

    // ~0.5 mph: the vehicle has to be standing at the pump
    int stopped = !cfg.requireStop || player_speed() < 0.005f;

    // original prompt: "~g~Press 'N' to refuel." while in the pump zone
    if (atStation && !refueling && cur->fuel < cfg.tank && AddMessageJumpQ) {
        static uint16_t msg[128];
        char m[128];
        if (!stopped) snprintf(m, sizeof m, "~y~Stop at the pump to refuel.");
        else if (cfg.refuelPad >= 0) snprintf(m, sizeof m, "~g~Press '%s' (%s) to refuel.", mvc_key_name(cfg.refuelKey), pad_label());
        else snprintf(m, sizeof m, "~g~Press '%s' to refuel.", mvc_key_name(cfg.refuelKey));
        to_game_text(msg, 128, m);
        AddMessageJumpQ(msg, 150, 0);
    }

    int held = mvc_key_down(cfg.refuelKey) || (cfg.refuelPad >= 0 && mvc_pad_down(cfg.refuelPad));
    if (atStation && stopped && held && cur->fuel < cfg.tank && dt > 0) {
        int32_t *money = money_ptr();
        if (!refueling) { refueling = 1; sessionCost = 0; sessionFuel = 0; costAcc = 0; }
        float add = fminf(cfg.refuelGps * dt, cfg.tank - cur->fuel);
        if (money && cfg.pricePerGallon > 0) {
            costAcc += add * cfg.pricePerGallon;
            int take = (int)costAcc;
            if (take > 0 && *money < take) {
                help("You can't afford any more gas.");
                refueling = 0;
                add = 0;
            } else if (take > 0) {
                *money -= take;
                costAcc -= take;
                sessionCost += take;
            }
        }
        cur->fuel += add;
        sessionFuel += add;
        if (cur->fuel >= cfg.tank - 0.001f) cur->fuel = cfg.tank;
        empty = cur->fuel <= cfg.emptyBelow;
        if (cur->fuel >= cfg.tank && refueling) {
            char m[160];
            snprintf(m, sizeof m, "Tank full. %.1f gallons for $%.0f.", sessionFuel, sessionCost);
            help(m);
            refueling = 0;
        }
    } else if (refueling) {
        refueling = 0;
        if (sessionFuel >= 0.05f) {
            char m[160];
            snprintf(m, sizeof m, "Pumped %.1f gallons for $%.0f. Tank: %.1f of %.1f gal.",
                     sessionFuel, sessionCost, cur->fuel, cfg.tank);
            help(m);
        }
    }

    if (cur->fuel <= cfg.lowFuel && !empty && !warnedLow) {
        warnedLow = 1;
        help("Fuel is low. Gas stations are marked on the radar.");
    }
    if (cur->fuel > cfg.lowFuel) warnedLow = 0;
    if (empty && !warnedEmpty) {
        warnedEmpty = 1;
        help("Out of gas! Find a gas station - or another ride.");
    }
    if (!empty) warnedEmpty = 0;
}

// HUD
static void hud_original(void) {
    // the CLEO mod's bar: drawn in 640x448 screen space, left edge at x=7.5,
    // centred on y=440: black 67x5 frame, grey 65x2 track, orange fill.
    float W = mvc_screen_w(), H = mvc_screen_h();
    float sx = W / 640.f, sy = H / 448.f;
    float frac = cur->fuel / cfg.tank;
#define BOX(l, w, h, col) mvc_rect((l) * sx, (440.f - (h) / 2) * sy, ((l) + (w)) * sx, (440.f + (h) / 2) * sy, col)
    BOX(7.5f, 67.f, 5.f, ((MVCColor){0, 0, 0, 255}));
    BOX(7.5f, 65.f, 2.f, ((MVCColor){100, 100, 100, 255}));
    if (frac > 0) BOX(7.5f, 65.f * frac, 2.f, ((MVCColor){200, 100, 0, 255}));
#undef BOX
}

static void hud(void) {
    if (!cur) return;
    if (cfg.hudStyle == HUD_ORIGINAL) { hud_original(); return; }

    float W = mvc_screen_w(), H = mvc_screen_h();
    float u = H / 448.f;
    float right = W - cfg.hudRight * u, top = cfg.hudTop * u;
    float bw = 78 * u, bh = 7 * u;
    float left = right - bw;
    float f = cur->fuel / cfg.tank;
    int low = cur->fuel <= cfg.lowFuel;
    int blink = low && ((mvc_time_ms() / 300) & 1);

    MVCColor fill = low ? (MVCColor){230, 60, 50, 255} : (MVCColor){255, 170, 20, 255};
    MVCColor back = low ? (MVCColor){80, 20, 18, 220} : (MVCColor){90, 55, 5, 220};
    if (refueling) fill = (MVCColor){120, 235, 120, 255};

    mvc_rect(left, top, right + 3 * u, top + bh + 3 * u, (MVCColor){0, 0, 0, 140}); // drop shadow
    mvc_rect(left - 1.5f * u, top - 1.5f * u, right + 1.5f * u, top + bh + 1.5f * u, (MVCColor){0, 0, 0, 255});
    mvc_rect(left, top, right, top + bh, back);
    if (f > 0 && !blink) mvc_rect(left, top, left + bw * f, top + bh, fill);
    for (int q = 1; q < 4; q++) {
        float x = left + bw * q / 4.f;
        mvc_rect(x - 0.5f * u, top, x + 0.5f * u, top + bh * 0.45f, (MVCColor){0, 0, 0, 200});
    }

    char amt[24];
    snprintf(amt, sizeof amt, "%.1f GAL", cur->fuel);
    MVCColor label = low ? (MVCColor){255, 90, 80, 255} : (MVCColor){255, 190, 60, 255};
    mvc_text(left - 4 * u, top - 3.2f * u, "FUEL", 0.42f * u, 0.85f * u, label, MVC_ALIGN_RIGHT, 2, (int)(1 * u));
    mvc_text(right, top + bh + 3 * u, amt, 0.30f * u, 0.62f * u, (MVCColor){230, 230, 230, 255}, MVC_ALIGN_RIGHT, 1, (int)(1 * u));

    char s[96];
    s[0] = 0;
    MVCColor sc = {255, 255, 255, 255};
    if (refueling) { snprintf(s, sizeof s, "REFUELING"); sc = (MVCColor){120, 235, 120, 255}; }
    else if (empty) { snprintf(s, sizeof s, "OUT OF GAS"); sc = (MVCColor){255, 90, 80, 255}; }
    else if (low && nearestIdx >= 0 && !atStation) {
        snprintf(s, sizeof s, "GAS: %s  %.1f MI", st[nearestIdx].name, nearestDist / 1609.344f);
        for (char *c = s; *c; c++) *c = (char)toupper((unsigned char)*c);
        sc = (MVCColor){255, 190, 60, 255};
    }
    // second line, under the gallons, so the two never overlap
    float row2 = top + bh + 12.5f * u;
    if (refueling) {
        // green label on the left, money spent in red on the right
        char cost[32];
        snprintf(cost, sizeof cost, "-$%.0f", sessionCost);
        mvc_text(left, row2, s, 0.30f * u, 0.62f * u, sc, MVC_ALIGN_LEFT, 1, (int)(1 * u));
        mvc_text(right, row2, cost, 0.30f * u, 0.62f * u, (MVCColor){235, 60, 60, 255}, MVC_ALIGN_RIGHT, 1, (int)(1 * u));
    } else if (s[0]) {
        mvc_text(right, row2, s, 0.30f * u, 0.62f * u, sc, MVC_ALIGN_RIGHT, 1, (int)(1 * u));
    }
}

// radar
static void radar(void) {
    if (!cfg.enabled) return;
    if (!cfg.showRadar || !RadarWorldToRadar || !mvc_hud_visible()) return;
    int lowNow = cur && cur->fuel <= cfg.lowFuel;
    for (int i = 0; i < nst; i++) {
        V2 w = {st[i].pos.x, st[i].pos.y}, r, s;
        RadarWorldToRadar(&r, &w);
        float d = RadarLimit(&r);
        // short-range blips like the original; when low, the nearest stays on the radar edge
        if (d > 1.f && !(lowNow && i == nearestIdx)) continue;
        RadarToScreen(&s, &r);
        if (cfg.radarIcon > 0 && DrawRadarSprite) DrawRadarSprite((uint16_t)cfg.radarIcon, s.x, s.y, 255);
        else ShowRadarTrace(s.x, s.y, 3, 255, 170, 0, 255);
    }
}

// hot reload
// `reload Gasoline` in the console: stash every tank the way a save does; the
// new copy reads it back from the replayed save-loaded event.
MVC_API void MVCPluginUnload(void) { on_event(MVC_EV_SAVE_WRITING, NULL); }

// init
#define SYM(var, name) do { *(void **)&(var) = mvc_sym(name); if (!(var)) { mvc_log_error("missing %s", name); ok = 0; } } while (0)

MVC_API void MVCPluginInit(void) {
    int ok = 1;
    SYM(FindPlayerVehicle, "_Z17FindPlayerVehiclev");
    SYM(FindPlayerPed, "_Z13FindPlayerPedv");
    SYM(FindPlayerCoors, "_Z15FindPlayerCoorsv");
    SYM(FindPlayerSpeed, "_Z15FindPlayerSpeedv");
    SYM(CPad_GetPad, "_ZN4CPad6GetPadEi");
    SYM(SetHelpMessage, "_ZN4CHud14SetHelpMessageEPtbb");
    SYM(AddMessageJumpQ, "_ZN9CMessages15AddMessageJumpQEPtjt");
    SYM(RadarWorldToRadar, "_ZN6CRadar35TransformRealWorldPointToRadarSpaceER9CVector2DRKS0_");
    SYM(RadarLimit, "_ZN6CRadar15LimitRadarPointER9CVector2D");
    SYM(RadarToScreen, "_ZN6CRadar32TransformRadarPointToScreenSpaceER9CVector2DRKS0_");
    SYM(ShowRadarTrace, "_ZN6CRadar14ShowRadarTraceEffjhhhh");
    SYM(DrawRadarSprite, "_ZN6CRadar15DrawRadarSpriteEtffh");
    SYM(PlaceMarkerSet, "_ZN10C3dMarkers14PlaceMarkerSetEjtR7CVectorfhhhhtfs");
    SYM(IsPlayerOnAMission, "_ZN11CTheScripts18IsPlayerOnAMissionEv");
    SYM(Players, "_ZN6CWorld7PlayersE");
    SYM(PlayerInFocus, "_ZN6CWorld13PlayerInFocusE");
    if (!ok) { mvc_log_error("unsupported macVC build - Gasoline disabled"); return; }

    // read struct offsets out of this build's code so game updates don't break us
    int m = mvc_first_load_imm(mvc_sym("_ZN8CVehicle9UsesSirenEv"), 2, 2);
    if (m > 0) off_model = m; else mvc_log("vehicle model offset not found, using 0x%x", off_model);
    uint32_t *spi = (uint32_t *)mvc_sym("_ZN11CPlayerInfo14SavePlayerInfoEPhPj");
    if (spi && (spi[0] & 0xFF800000) == 0x52800000) size_playerinfo = (spi[0] >> 5) & 0xFFFF;
    int mo = mvc_first_load_imm(spi, 12, 4);
    if (mo > 0) off_money = mo; else mvc_log("money offset not found, using 0x%x", off_money);
    mvc_log("offsets: model +0x%x, money +0x%x, CPlayerInfo size 0x%x", off_model, off_money, size_playerinfo);

    load_config();
    static const char *const gauges[] = {"Modern", "Original"};
    int pg = mvc_menu_page("Gasoline");
    const char *ini = "plugins/Gasoline.ini";
    mvc_menu_toggle(pg, "Fuel system", &cfg.enabled, ini, "Gasoline", "Enabled", NULL);
    mvc_menu_toggle(pg, "Stop to refuel", &cfg.requireStop, ini, "Gasoline", "RequireStop", NULL);
    mvc_menu_number(pg, "Fuel economy", &cfg.mpg, 0.25f, 30.f, 0.25f, "%.2f MPG", ini, "Gasoline", "MilesPerGallon", NULL);
    mvc_menu_number(pg, "Price per gallon", &cfg.pricePerGallon, 0.f, 200.f, 2.5f, "$%.2f", ini, "Gasoline", "PricePerGallon", NULL);
    mvc_menu_choice(pg, "Fuel gauge", gauges, 2, &cfg.hudStyle, ini, "Gasoline", "HudStyle", NULL);
    mvc_menu_toggle(pg, "Pump markers", &cfg.showMarkers, ini, "Gasoline", "ShowPumpMarkers", NULL);
    mvc_menu_toggle(pg, "Stations on radar", &cfg.showRadar, ini, "Gasoline", "ShowStationsOnRadar", NULL);
    mvc_menu_toggle(pg, "Drain on missions", &cfg.drainOnMissions, ini, "Gasoline", "DrainOnMissions", NULL);
    mvc_menu_keybind(pg, "Refuel key", &cfg.refuelKey, ini, "Gasoline", "RefuelKey", NULL);
    pad0 = CPad_GetPad(0);
    if (mvc_hook_sym("_ZN4CPad13GetAccelerateEv", (void *)h_GetAccelerate, (void **)&o_GetAccelerate) ||
        mvc_hook_sym("_ZN4CPad8GetBrakeEv", (void *)h_GetBrake, (void **)&o_GetBrake)) {
        mvc_log_error("could not hook the accelerator - Gasoline disabled");
        return;
    }
    mvc_on_tick(tick);
    mvc_on_event(on_event);
    mvc_on_hud(hud);
    mvc_on_radar(radar);
    mvc_log("ready");
}
