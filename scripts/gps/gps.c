// gps, gta iv style route to your map waypoint.
//


// set a waypoint on the pause map. in a vehicle the road route gets drawn on the radar and
// the pause map in vice city colours, hot pink near you through purple to cyan at the end.
// it recalculates when you leave the route and goes away when you get there.
// settings: GameData/scripts/GPS.ini
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "../../include/macvc.h"

MVC_PLUGIN("GPS", "1.0", "Big Cojones (macVC)", "",
           "GTA IV style GPS route to the map waypoint, on the radar and the pause map");

typedef struct { float x, y, z; } V3;
typedef struct { float x, y; } V2;
typedef struct { uint8_t r, g, b, a; } RGBA;

// game API
static void (*DoPathSearch)(void *self, uint8_t type, V3 start, int32_t startNode, V3 target,
                            void **nodes, int16_t *numNodes, int16_t maxNodes, void *vehicle,
                            float *dist, float distLimit, int32_t forcedTargetNode);
static void *(*FindPlayerVehicle)(void);
static V3 (*FindPlayerCoors)(void);
static void (*WorldToRadar)(V2 *, const V2 *);
static float (*LimitRadarPoint)(V2 *);
static void (*RadarToScreen)(V2 *, const V2 *);
static void (*Draw2DPolygon)(float, float, float, float, float, float, float, float, const RGBA *);
static uint8_t *ThePaths;
static int32_t *TargetMarkerId;
static float *TargetMarkerPos; // CVector

// CPathNode: int16 x, y, z at +4/+6/+8, in 1/8 units (checked against
// CPathFind::FindNodeClosestToCoors in macVC 1.0.5226)
static V3 node_pos(const uint8_t *n) {
    return (V3){*(const int16_t *)(n + 4) / 8.f, *(const int16_t *)(n + 6) / 8.f, *(const int16_t *)(n + 8) / 8.f};
}

// settings
static struct {
    RGBA near, mid, far, outline;
    float width;        // route width in 640x448 HUD units
    float arriveDist;   // metres
    float offRouteDist; // metres before recalculating
    int showOnMap, onFoot, enabled;
} cfg;

static const char *DEFAULT_INI =
"; GPS for macVC - GTA IV style route to the map waypoint\n"
"[GPS]\n"
"; 0 = GPS off. Also in Options > Mods > GPS\n"
"Enabled=1\n"
"; route colours, near you -> middle -> destination (R,G,B,A) - Vice City palette\n"
"ColorNear=255,60,190,255\n"
"ColorMid=170,80,255,255\n"
"ColorFar=0,215,255,255\n"
"ColorOutline=25,10,45,220\n"
"; line thickness in HUD units (640x448)\n"
"Width=2.2\n"
"; route clears when you are this close to the waypoint (metres)\n"
"ArriveDistance=25\n"
"; recalculate when you are this far from the route (metres)\n"
"OffRouteDistance=30\n"
"; also draw on the pause-menu map\n"
"ShowOnMap=1\n"
"; 1 = show the road route on foot too (GTA IV: vehicles only)\n"
"ShowOnFoot=0\n";

static void trim(char *s) {
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    char *b = s;
    while (isspace((unsigned char)*b)) b++;
    if (b != s) memmove(s, b, strlen(b) + 1);
}

static RGBA parse_rgba(const char *v, RGBA def) {
    int r, g, b, a = 255;
    if (sscanf(v, "%d , %d , %d , %d", &r, &g, &b, &a) >= 3)
        return (RGBA){(uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)a};
    mvc_log_warn("bad colour \"%s\"", v);
    return def;
}

static void load_config(void) {
    cfg.near = (RGBA){255, 60, 190, 255};
    cfg.mid = (RGBA){170, 80, 255, 255};
    cfg.far = (RGBA){0, 215, 255, 255};
    cfg.outline = (RGBA){25, 10, 45, 220};
    cfg.width = 2.2f; cfg.arriveDist = 25; cfg.offRouteDist = 30; cfg.showOnMap = 1; cfg.onFoot = 0; cfg.enabled = 1;

    char path[1024];
    snprintf(path, sizeof path, "%s/scripts/GPS.ini", mvc_game_dir());
    FILE *f = fopen(path, "r");
    if (!f) {
        f = fopen(path, "w");
        if (f) { fputs(DEFAULT_INI, f); fclose(f); }
        f = fopen(path, "r");
        if (!f) return;
    }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        trim(line);
        if (!line[0] || line[0] == ';' || line[0] == '[') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        trim(k); trim(v);
        if (!strcasecmp(k, "ColorNear")) cfg.near = parse_rgba(v, cfg.near);
        else if (!strcasecmp(k, "ColorMid")) cfg.mid = parse_rgba(v, cfg.mid);
        else if (!strcasecmp(k, "ColorFar")) cfg.far = parse_rgba(v, cfg.far);
        else if (!strcasecmp(k, "ColorOutline")) cfg.outline = parse_rgba(v, cfg.outline);
        else if (!strcasecmp(k, "Width")) cfg.width = fmaxf(0.5f, (float)atof(v));
        else if (!strcasecmp(k, "ArriveDistance")) cfg.arriveDist = (float)atof(v);
        else if (!strcasecmp(k, "OffRouteDistance")) cfg.offRouteDist = fmaxf(5, (float)atof(v));
        else if (!strcasecmp(k, "ShowOnMap")) cfg.showOnMap = atoi(v);
        else if (!strcasecmp(k, "ShowOnFoot")) cfg.onFoot = atoi(v);
        else if (!strcasecmp(k, "Enabled")) cfg.enabled = atoi(v) != 0;
    }
    fclose(f);
}

// route state
#define MAXPTS 512
static V2 route[MAXPTS];
static float cum[MAXPTS];      // distance from route start
static int npts, startSeg;     // startSeg: segment the player is on
static float total;
static V2 lastTarget = {1e9f, 1e9f};
static int arrived;
static uint32_t lastCheck;
static int warnedFail;

static float d2(V2 a, V2 b) { float dx = a.x - b.x, dy = a.y - b.y; return dx * dx + dy * dy; }

// distance from p to segment ab, squared; t = position along it
static float seg_d2(V2 p, V2 a, V2 b, float *t) {
    float vx = b.x - a.x, vy = b.y - a.y, L = vx * vx + vy * vy;
    float u = L > 0 ? ((p.x - a.x) * vx + (p.y - a.y) * vy) / L : 0;
    u = u < 0 ? 0 : u > 1 ? 1 : u;
    if (t) *t = u;
    V2 q = {a.x + vx * u, a.y + vy * u};
    return d2(p, q);
}

static void calculate(V3 from, V2 to) {
    float tz = TargetMarkerPos[2];
    if (!(tz > -100 && tz < 500)) tz = from.z; // map waypoints may have no height
    V3 target = {to.x, to.y, tz};
    void *nodes[MAXPTS - 2];
    int16_t n = 0;
    float dist = 0;
    DoPathSearch(ThePaths, 0 /* car paths */, from, -1, target, nodes, &n, MAXPTS - 2, NULL, &dist, 999999.f, -1);
    npts = 0;
    startSeg = 0;
    if (n <= 0) {
        if (!warnedFail) { mvc_log_warn("no road route to the waypoint from here"); warnedFail = 1; }
        return;
    }
    warnedFail = 0;
    V3 first = node_pos(nodes[0]);
    // nodes far from the player (boats, aircraft, off-road) mean there is no sensible road route
    if ((first.x - from.x) * (first.x - from.x) + (first.y - from.y) * (first.y - from.y) > 80.f * 80.f) return;
    route[npts++] = (V2){from.x, from.y};
    for (int i = 0; i < n && npts < MAXPTS - 1; i++) {
        V3 p = node_pos(nodes[i]);
        route[npts++] = (V2){p.x, p.y};
    }
    route[npts++] = to;
    cum[0] = 0;
    for (int i = 1; i < npts; i++) cum[i] = cum[i - 1] + sqrtf(d2(route[i - 1], route[i]));
    total = cum[npts - 1];
}

static void tick(void) {
    void *veh = FindPlayerVehicle();
    if (!cfg.enabled || *TargetMarkerId == -1 || (!veh && !cfg.onFoot)) { npts = 0; return; }
    V2 target = {TargetMarkerPos[0], TargetMarkerPos[1]};
    V3 p3 = FindPlayerCoors();
    V2 p = {p3.x, p3.y};

    int newTarget = d2(target, lastTarget) > 1.f;
    if (newTarget) { lastTarget = target; arrived = 0; npts = 0; }
    if (arrived) return;
    if (d2(p, target) < cfg.arriveDist * cfg.arriveDist) {
        if (npts) mvc_log("arrived at the waypoint");
        arrived = 1; npts = 0;
        return;
    }
    uint32_t now = mvc_time_ms();
    if (!npts) {
        if (newTarget || now - lastCheck > 2000 || now < lastCheck) { lastCheck = now; calculate(p3, target); }
        return;
    }
    // follow the player along the route; recalculate when they leave it
    float best = 1e18f;
    int bestSeg = startSeg;
    for (int i = startSeg > 2 ? startSeg - 2 : 0; i < npts - 1 && i < startSeg + 40; i++) {
        float dd = seg_d2(p, route[i], route[i + 1], NULL);
        if (dd < best) { best = dd; bestSeg = i; }
    }
    startSeg = bestSeg;
    if (best > cfg.offRouteDist * cfg.offRouteDist && (now - lastCheck > 700 || now < lastCheck)) {
        lastCheck = now;
        calculate(p3, target);
    }
}

// drawing
static uint32_t frameId, drawnFrame = ~0u;
static void frame_counter(void) { frameId++; }

static RGBA lerp(RGBA a, RGBA b, float t) {
    return (RGBA){(uint8_t)(a.r + (b.r - a.r) * t), (uint8_t)(a.g + (b.g - a.g) * t),
                  (uint8_t)(a.b + (b.b - a.b) * t), (uint8_t)(a.a + (b.a - a.a) * t)};
}
static RGBA colour_at(float t) {
    return t < 0.5f ? lerp(cfg.near, cfg.mid, t * 2) : lerp(cfg.mid, cfg.far, (t - 0.5f) * 2);
}

static void tri(V2 a, V2 b, V2 c, const RGBA *col) {
    // a triangle passed as a degenerate quad renders correctly whatever the vertex order
    Draw2DPolygon(a.x, a.y, b.x, b.y, c.x, c.y, c.x, c.y, col);
}

static void thick(V2 a, V2 b, float w, const RGBA *col) {
    float dx = b.x - a.x, dy = b.y - a.y, L = sqrtf(dx * dx + dy * dy);
    if (L < 0.01f) return;
    float nx = -dy / L * w * 0.5f, ny = dx / L * w * 0.5f;
    // extend slightly along the line so consecutive segments overlap at the joints
    float ex = dx / L * w * 0.35f, ey = dy / L * w * 0.35f;
    V2 p1 = {a.x + nx - ex, a.y + ny - ey}, p2 = {b.x + nx + ex, b.y + ny + ey};
    V2 p3 = {b.x - nx + ex, b.y - ny + ey}, p4 = {a.x - nx - ex, a.y - ny - ey};
    tri(p1, p2, p3, col);
    tri(p1, p3, p4, col);
}

// clips radar-space segment ab to the unit circle; returns 0 if fully outside
static int clip_circle(V2 *a, V2 *b) {
    float dx = b->x - a->x, dy = b->y - a->y;
    float A = dx * dx + dy * dy, B = 2 * (a->x * dx + a->y * dy), C = a->x * a->x + a->y * a->y - 1;
    int ain = C <= 0, bin = b->x * b->x + b->y * b->y <= 1;
    if (ain && bin) return 1;
    if (A < 1e-9f) return 0;
    float disc = B * B - 4 * A * C;
    if (disc < 0) return 0;
    float s = sqrtf(disc), t0 = (-B - s) / (2 * A), t1 = (-B + s) / (2 * A);
    if (t1 < 0 || t0 > 1) return 0;
    if (t0 < 0) t0 = 0;
    if (t1 > 1) t1 = 1;
    V2 na = {a->x + dx * t0, a->y + dy * t0}, nb = {a->x + dx * t1, a->y + dy * t1};
    *a = na; *b = nb;
    return 1;
}

static void draw_route(void) {
    if (npts < 2) return;
    int onMap = mvc_menu_map_active();
    if (onMap && !cfg.showOnMap) return;
    if (!onMap && !mvc_hud_visible()) return;
    if (drawnFrame == frameId) return; // the pause map draws blips twice per frame
    drawnFrame = frameId;

    float u = mvc_screen_h() / 448.f;
    float w = cfg.width * u * (onMap ? 1.3f : 1.f);
    V3 p3 = FindPlayerCoors();
    V2 here = {p3.x, p3.y};
    float t0;
    seg_d2(here, route[startSeg], route[startSeg + 1 < npts ? startSeg + 1 : startSeg], &t0);
    float done = cum[startSeg] + t0 * (cum[startSeg + 1 < npts ? startSeg + 1 : startSeg] - cum[startSeg]);
    float left = total - done > 1 ? total - done : 1;

    for (int pass = 0; pass < 2; pass++) {       // outline first, then the colour
        V2 prev = here;
        for (int i = startSeg + 1; i < npts; i++) {
            V2 wa = prev, wb = route[i], ra, rb, sa, sb;
            prev = wb;
            WorldToRadar(&ra, &wa);
            WorldToRadar(&rb, &wb);
            if (!onMap && !clip_circle(&ra, &rb)) continue;
            RadarToScreen(&sa, &ra);
            RadarToScreen(&sb, &rb);
            if (pass == 0) {
                thick(sa, sb, w + 2.f * u, &cfg.outline);
            } else {
                float t = (cum[i] - done) / left;
                RGBA c = colour_at(t < 0 ? 0 : t > 1 ? 1 : t);
                thick(sa, sb, w, &c);
            }
        }
    }
}

// init
#define SYM(var, name) do { *(void **)&(var) = mvc_sym(name); if (!(var)) { mvc_log_error("missing %s", name); ok = 0; } } while (0)

MVC_API void MVCPluginInit(void) {
    int ok = 1;
    SYM(DoPathSearch, "_ZN9CPathFind12DoPathSearchEh7CVectoriS0_PP9CPathNodePssP8CVehiclePffi");
    SYM(FindPlayerVehicle, "_Z17FindPlayerVehiclev");
    SYM(FindPlayerCoors, "_Z15FindPlayerCoorsv");
    SYM(WorldToRadar, "_ZN6CRadar35TransformRealWorldPointToRadarSpaceER9CVector2DRKS0_");
    SYM(LimitRadarPoint, "_ZN6CRadar15LimitRadarPointER9CVector2D");
    SYM(RadarToScreen, "_ZN6CRadar32TransformRadarPointToScreenSpaceER9CVector2DRKS0_");
    SYM(Draw2DPolygon, "_ZN9CSprite2d13Draw2DPolygonEffffffffRK5CRGBA");
    SYM(ThePaths, "ThePaths");
    SYM(TargetMarkerId, "_ZN6CRadar14TargetMarkerIdE");
    SYM(TargetMarkerPos, "_ZN6CRadar15TargetMarkerPosE");
    if (!ok) { mvc_log_error("unsupported macVC build - GPS disabled"); return; }
    load_config();
    int pg = mvc_menu_page("GPS");
    const char *ini = "scripts/GPS.ini";
    mvc_menu_toggle(pg, "GPS route", &cfg.enabled, ini, "GPS", "Enabled", NULL);
    mvc_menu_toggle(pg, "Show on map", &cfg.showOnMap, ini, "GPS", "ShowOnMap", NULL);
    mvc_menu_toggle(pg, "Show on foot", &cfg.onFoot, ini, "GPS", "ShowOnFoot", NULL);
    mvc_menu_number(pg, "Line width", &cfg.width, 1.0f, 6.0f, 0.25f, "%.2f", ini, "GPS", "Width", NULL);
    mvc_on_tick(tick);
    mvc_on_frame(frame_counter);
    mvc_on_radar_under(draw_route);
    mvc_log("ready - set a waypoint on the map, then get in a vehicle");
}
