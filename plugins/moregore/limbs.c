// severed heads and limbs fall on the ground instead of vanishing.
//


// when a part comes off (mine or the game's gunshot ones) i make a new instance of the
// ped's model with CClumpModelInfo::CreateInstance, the same way the game makes peds, and
// copy the ped's skeleton into it as it was right then. the severed part's bones stay and
// every other bone collapses onto the joint, so only the head, arm or leg shows. it gets
// thrown away from the hit, tumbles, falls, lands (CWorld::FindGroundZFor3DCoord) and sits
// there for a while.
//


// vc's skeleton matrices are world space (UPDATEMODELLINGMATRICES | UPDATELTMS), so moving
// the piece means transforming those matrices. nothing animates it.
#include "moregore.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct { float x, y, z; } V3;
typedef struct { V3 right; uint32_t flags; V3 up; uint32_t pad1; V3 at; uint32_t pad2; V3 pos; uint32_t pad3; } Mat; // rw::Matrix, 64 bytes

static void *(*CreateInstance)(void *modelinfo);
static void (*ClumpDestroy)(void *clump);
static void (*ClumpRender)(void *clump);
static void *(*ClumpGetFrame)(void *clump);
static Mat *(*FrameGetMatrix)(void *frame);
static void (*FrameUpdateObjects)(void *frame);
static void *(*GetAnimHierarchyFromSkinClump)(void *clump);
static void *(*GetAnimHierarchyFromClump)(void *clump);
static int (*RpHAnimIDGetIndex)(void *hier, int id);
static float (*FindGroundZ)(float x, float y, float z, _Bool *found);
static _Bool (*SetupLighting)(void *entity);
static void (*RemoveLighting)(void *entity, _Bool lit);
static void **ModelInfoPtrs;           // CModelInfo::ms_modelInfoPtrs
static int g_off_clump = -1, g_off_model = -1;

static int g_on = 1, g_amount = 1;    // g_amount: 2 on Extreme blood (more pieces, longer)
static const char *INI = "plugins/MoreGore.ini";

// PedNode -> the bones of that part; the first is the joint it comes off at
static const int *bones_of(int node, int *n) {
    static const int HEAD[] = {5}, LARM[] = {32, 33, 34, 35}, RARM[] = {22, 23, 24, 25}, LLEG[] = {41, 42, 43},
                     RLEG[] = {51, 52, 53};
    switch (node) {
    case 2: *n = 1; return HEAD;
    case 3: *n = 4; return LARM;
    case 4: *n = 4; return RARM;
    case 7: *n = 3; return LLEG;
    case 8: *n = 3; return RLEG;
    }
    *n = 0;
    return NULL;
}

#define MAX_PIECES 48
#define MAX_NODES 64
typedef struct {
    int used, landed, nodes, keep[MAX_NODES];
    void *clump, *hier;
    Mat start[MAX_NODES];              // world matrices at the cut
    V3 c0, c, vel;                     // centre at the cut, now, velocity (m/s)
    V3 axis; float angle, spin;        // tumbling
    float radius, ground;
    uint32_t born, life;
} Piece;
static Piece g_pieces[MAX_PIECES];

static float frand(float a, float b) { return a + (b - a) * (float)arc4random_uniform(10001) / 10000.f; }
static V3 rot(V3 v, V3 k, float a) { // Rodrigues: v rotated by a around unit axis k
    float c = cosf(a), s = sinf(a), d = k.x * v.x + k.y * v.y + k.z * v.z;
    V3 x = {k.y * v.z - k.z * v.y, k.z * v.x - k.x * v.z, k.x * v.y - k.y * v.x};
    return (V3){v.x * c + x.x * s + k.x * d * (1 - c), v.y * c + x.y * s + k.y * d * (1 - c), v.z * c + x.z * s + k.z * d * (1 - c)};
}

static void free_piece(Piece *p) {
    if (p->clump) ClumpDestroy(p->clump);
    memset(p, 0, sizeof *p);
}

// write the piece's matrices for where it is now
static void pose(Piece *p) {
    Mat *m = *(Mat **)((uint8_t *)p->hier + 8);
    int joint = -1;
    for (int i = 0; i < p->nodes; i++) if (p->keep[i] == 2) joint = i;
    V3 jp = joint >= 0 ? p->start[joint].pos : p->c0;
    V3 jr = rot((V3){jp.x - p->c0.x, jp.y - p->c0.y, jp.z - p->c0.z}, p->axis, p->angle);
    V3 jw = {jr.x + p->c.x, jr.y + p->c.y, jr.z + p->c.z};
    for (int i = 0; i < p->nodes; i++) {
        Mat s = p->start[i];
        if (!p->keep[i]) { // collapsed onto the joint
            s.right = s.up = s.at = (V3){0, 0, 0};
            s.pos = jw;
        } else {
            s.right = rot(s.right, p->axis, p->angle);
            s.up = rot(s.up, p->axis, p->angle);
            s.at = rot(s.at, p->axis, p->angle);
            V3 r = rot((V3){s.pos.x - p->c0.x, s.pos.y - p->c0.y, s.pos.z - p->c0.z}, p->axis, p->angle);
            s.pos = (V3){r.x + p->c.x, r.y + p->c.y, r.z + p->c.z};
        }
        m[i] = s;
    }
    // keep the clump's own frame at the piece (its bounding sphere); the skin uses the matrices
    void *f = ClumpGetFrame(p->clump);
    Mat *fm = f ? FrameGetMatrix(f) : NULL;
    if (fm) { fm->pos = p->c; FrameUpdateObjects(f); }
}

// a cut is seen twice (in InflictDamage, then by the blood tick): spawn once
static struct { uint8_t *ped; int node; uint32_t t; } g_recent[64];
static int g_nrecent;

void limbs_spawn(uint8_t *ped, int node, float dx, float dy) {
    if (!g_on || !CreateInstance || g_off_clump < 0 || g_off_model < 0) return;
    uint32_t now0 = mvc_time_ms();
    for (int i = 0; i < 64; i++)
        if (g_recent[i].ped == ped && g_recent[i].node == node && now0 - g_recent[i].t < 30000) return;
    g_recent[g_nrecent] = (typeof(g_recent[0])){ped, node, now0};
    g_nrecent = (g_nrecent + 1) % 64;
    g_amount = blood_level() >= 2 ? 2 : 1;
    int nb;
    const int *bones = bones_of(node, &nb);
    if (!bones) return;
    void *pclump = *(void **)(ped + g_off_clump);
    uint8_t *phier = pclump ? GetAnimHierarchyFromSkinClump(pclump) : NULL;
    int model = *(int16_t *)(ped + g_off_model);
    void *mi = model >= 0 && model < 6500 ? ModelInfoPtrs[model] : NULL;
    if (!phier || !mi) return;
    int nodes = *(int32_t *)(phier + 4);
    Mat *pm = *(Mat **)(phier + 8);
    if (nodes <= 0 || nodes > MAX_NODES || !pm) return;
    // a slot: free, else the oldest
    Piece *p = NULL;
    int cap = g_amount >= 2 ? MAX_PIECES : MAX_PIECES / 2;
    for (int i = 0; i < cap && !p; i++) if (!g_pieces[i].used) p = &g_pieces[i];
    if (!p) {
        p = &g_pieces[0];
        for (int i = 1; i < cap; i++) if (g_pieces[i].born < p->born) p = &g_pieces[i];
        free_piece(p);
    }
    void *clump = CreateInstance(mi);
    void *hier = clump ? GetAnimHierarchyFromClump(clump) : NULL;
    if (!hier || *(int32_t *)((uint8_t *)hier + 4) != nodes) { if (clump) ClumpDestroy(clump); return; }
    p->clump = clump;
    p->hier = hier;
    p->nodes = nodes;
    memcpy(p->start, pm, sizeof(Mat) * (size_t)nodes);
    V3 c = {0, 0, 0};
    int kept = 0;
    for (int b = 0; b < nb; b++) {
        int idx = RpHAnimIDGetIndex(phier, bones[b]);
        if (idx < 0 || idx >= nodes) continue;
        p->keep[idx] = b == 0 ? 2 : 1;
        c.x += pm[idx].pos.x; c.y += pm[idx].pos.y; c.z += pm[idx].pos.z;
        kept++;
    }
    if (!kept) { free_piece(p); return; }
    c.x /= (float)kept; c.y /= (float)kept; c.z /= (float)kept;
    if (node != 2) { // a limb's centre sits between its joint and its end
        int j = RpHAnimIDGetIndex(phier, bones[0]);
        c = (V3){(c.x + pm[j].pos.x) * 0.5f, (c.y + pm[j].pos.y) * 0.5f, (c.z + pm[j].pos.z) * 0.5f};
    }
    p->used = 1;
    p->c0 = p->c = c;
    float hl = sqrtf(dx * dx + dy * dy);
    if (hl < 0.01f) { float a = frand(0, 6.2832f); dx = cosf(a); dy = sinf(a); hl = 1; }
    float sp = frand(1.5f, 3.5f);
    p->vel = (V3){dx / hl * sp, dy / hl * sp, frand(1.5f, 3.0f)};
    V3 ax = {frand(-1, 1), frand(-1, 1), frand(-0.3f, 0.3f)};
    float al = sqrtf(ax.x * ax.x + ax.y * ax.y + ax.z * ax.z);
    p->axis = al > 0.01f ? (V3){ax.x / al, ax.y / al, ax.z / al} : (V3){1, 0, 0};
    p->spin = frand(6.f, 14.f);       // rad/s
    p->radius = node == 2 ? 0.12f : 0.07f;
    _Bool found = 0;
    p->ground = FindGroundZ ? FindGroundZ(c.x, c.y, c.z + 0.5f, &found) : c.z - 1.f;
    if (!found) p->ground = c.z - 1.f;
    p->born = mvc_time_ms();
    p->life = g_amount >= 2 ? 300000 : 120000;
    pose(p);
}

static uint32_t g_last;
static void tick(void) {
    uint32_t now = mvc_time_ms();
    float dt = g_last ? (float)(now - g_last) / 1000.f : 0.f;
    g_last = now;
    if (dt <= 0.f || dt > 0.1f) dt = dt > 0.1f ? 0.1f : 0.f;
    for (int i = 0; i < MAX_PIECES; i++) {
        Piece *p = &g_pieces[i];
        if (!p->used) continue;
        if (now - p->born > p->life) { free_piece(p); continue; }
        if (p->landed || dt == 0.f) continue;
        p->vel.z -= 9.8f * dt;
        p->c.x += p->vel.x * dt; p->c.y += p->vel.y * dt; p->c.z += p->vel.z * dt;
        p->angle += p->spin * dt;
        if (p->c.z - p->radius <= p->ground) { // bounce once or twice, then lie still
            p->c.z = p->ground + p->radius;
            if (fabsf(p->vel.z) > 1.5f) { p->vel.z = -p->vel.z * 0.3f; p->vel.x *= 0.5f; p->vel.y *= 0.5f; p->spin *= 0.4f; }
            else p->landed = 1;
        }
        pose(p);
    }
}

static void render(void) {
    void *player = FindPlayerPed();
    if (!player || !ClumpRender) return;
    _Bool lit = SetupLighting ? SetupLighting(player) : 0; // the game's lighting for things at street level
    for (int i = 0; i < MAX_PIECES; i++) if (g_pieces[i].used) ClumpRender(g_pieces[i].clump);
    if (RemoveLighting) RemoveLighting(player, lit);
}

static void on_event(int ev, void *arg) {
    if (ev == MVC_EV_NEW_SESSION) for (int i = 0; i < MAX_PIECES; i++) if (g_pieces[i].used) free_piece(&g_pieces[i]);
}

void limbs_menu(int pg) {
    char v[16];
    if (mvc_ini_get(INI, "MoreGore", "SeveredParts", v, sizeof v)) g_on = atoi(v) != 0;
    mvc_menu_toggle(pg, "Severed parts fall", &g_on, INI, "MoreGore", "SeveredParts", NULL);
}

void limbs_init(int off_clump) {
    g_off_clump = off_clump;
    CreateInstance = mvc_sym("_ZN15CClumpModelInfo14CreateInstanceEv");
    ClumpDestroy = mvc_sym("_Z14RpClumpDestroyPN2rw5ClumpE");
    ClumpRender = mvc_sym("_Z13RpClumpRenderPN2rw5ClumpE");
    ClumpGetFrame = mvc_sym("_Z15RpClumpGetFramePKN2rw5ClumpE");
    FrameGetMatrix = mvc_sym("_Z16RwFrameGetMatrixPN2rw5FrameE");
    FrameUpdateObjects = mvc_sym("_Z20RwFrameUpdateObjectsPN2rw5FrameE");
    GetAnimHierarchyFromSkinClump = mvc_sym("_Z29GetAnimHierarchyFromSkinClumpPN2rw5ClumpE");
    GetAnimHierarchyFromClump = mvc_sym("_Z25GetAnimHierarchyFromClumpPN2rw5ClumpE");
    RpHAnimIDGetIndex = mvc_sym("_Z17RpHAnimIDGetIndexPN2rw14HAnimHierarchyEi");
    FindGroundZ = mvc_sym("_ZN6CWorld21FindGroundZFor3DCoordEfffPb");
    SetupLighting = mvc_sym("_ZN7CEntity13SetupLightingEv");
    RemoveLighting = mvc_sym("_ZN7CEntity14RemoveLightingEb");
    ModelInfoPtrs = mvc_sym("_ZN10CModelInfo16ms_modelInfoPtrsE");
    // CEntity::m_modelIndex: CVehicle::UsesSiren starts with "ldrh w8, [x0, #model]"
    g_off_model = mvc_first_load_imm(mvc_sym("_ZN8CVehicle9UsesSirenEv"), 2, 2);
    if (!CreateInstance || !ClumpDestroy || !ClumpRender || !ClumpGetFrame || !FrameGetMatrix || !FrameUpdateObjects ||
        !GetAnimHierarchyFromSkinClump || !GetAnimHierarchyFromClump || !RpHAnimIDGetIndex || !ModelInfoPtrs ||
        g_off_clump < 0 || g_off_model < 0) {
        mvc_log_warn("[MoreGore] severed parts can't fall on this macVC build: they'll just disappear");
        CreateInstance = NULL;
        return;
    }
    mvc_on_tick(tick);
    mvc_on_render3d(render);
    mvc_on_event(on_event);
}
