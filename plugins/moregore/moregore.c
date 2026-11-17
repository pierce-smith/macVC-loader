// more gore: melee dismemberment, for peds standing or on the ground.
//


// vanilla only takes parts off with some guns by chance, plus the katana finisher.
// this adds:
//   melee: the killing blow takes a part off. melee always hits the torso so the part is random
//   shotguns: take off the part they hit, always at point blank, less likely further out,
//     never past the gore range (the game's own small chance still applies there)
//   python: headshots pop the head
// losing the head kills, same as the game's headshots. other guns keep the game's rules.
//


// skinned peds keep drawing removed parts, so after each skeleton update the part's bones
// get scaled to zero (same thing severed limbs did, so this works without it).
// parts come off through the game's own CPed::RemoveBodyPart so the blood is the game's.
//


// settings are in Options > Mods > More Gore, saved to GameData/plugins/MoreGore.ini.
// never the player, never a ped sitting in a car (game's own rule for gunshots).
#include "moregore.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

MVC_PLUGIN("More Gore", "1.0", "Big Cojones", "", "Dismemberment for melee, shotguns and Python headshots, and a blood overhaul");

enum { W_BLADES, W_ALL, W_CHAINSAW }; // the menu's order
enum { P_RANDOM, P_HEAD, P_LIMBS };
static int g_enabled = 1, g_weapons = W_BLADES, g_parts = P_RANDOM, g_shotguns = 1, g_python = 1;
static float g_near = 3.f, g_far = 15.f; // shotgun: certain within g_near metres, impossible past g_far

// chance of a shotgun hit taking a part off, from the attacker's distance
static int shotgun_roll(void *by, uint8_t *ped) {
    if (!by) return 0;
    MVCVec3 a = mvc_entity_pos(by), b = mvc_entity_pos(ped);
    float d = sqrtf((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
    float far = g_far > g_near ? g_far : g_near + 0.01f;
    float chance = d <= g_near ? 1.f : d >= far ? 0.f : (far - d) / (far - g_near);
    return arc4random_uniform(10000) < (uint32_t)(chance * 10000.f);
}
static const char *INI = "plugins/MoreGore.ini";

static void (*RemoveBodyPart)(uint8_t *ped, int node, int dir);
static int8_t (*o_InflictDamage)(uint8_t *ped, void *by, int weapon, float damage, int piece, int dir);
void *(*FindPlayerPed)(void);
int g_off_part = -1, g_off_health = -1, g_off_invehicle = -1;

// hiding removed parts: scale the part's bones to zero after the skeleton update
typedef struct { float x, y, z; } V3d;
static void *(*GetAnimHierarchyFromSkinClump)(void *clump);
static int (*RpHAnimIDGetIndex)(void *hier, int id);
static void *(*RwMatrixScale)(void *m, const V3d *s, int combine);
static void (*o_UpdateRpHAnim)(uint8_t *entity);
uint8_t **PedPool;           // CPools::ms_pPedPool: entries +0, size +0x10
int g_ped_size;
static int g_off_clump = -1;
// PedNode -> bones: 2 head 5; 3 left arm 32-35; 4 right arm 22-25; 7 left leg 41-43; 8 right leg 51-53
static const int BONES_HEAD[] = {5, 0}, BONES_LARM[] = {35, 34, 33, 32, 0}, BONES_RARM[] = {25, 24, 23, 22, 0},
                 BONES_LLEG[] = {43, 42, 41, 0}, BONES_RLEG[] = {53, 52, 51, 0};

int is_ped(uint8_t *e) {
    uint8_t *pool = PedPool ? *PedPool : NULL;
    if (!pool || g_ped_size <= 0) return 0;
    uint8_t *first = *(uint8_t **)pool;
    int n = *(int32_t *)(pool + 0x10);
    return e >= first && e < first + (size_t)n * (size_t)g_ped_size && (size_t)(e - first) % (size_t)g_ped_size == 0;
}

static void h_UpdateRpHAnim(uint8_t *e) {
    o_UpdateRpHAnim(e);
    if (!is_ped(e)) return;
    const int *bones;
    switch ((int8_t)e[g_off_part]) {
    case 2: bones = BONES_HEAD; break;
    case 3: bones = BONES_LARM; break;
    case 4: bones = BONES_RARM; break;
    case 7: bones = BONES_LLEG; break;
    case 8: bones = BONES_RLEG; break;
    default: return;
    }
    void *clump = *(void **)(e + g_off_clump);
    uint8_t *hier = clump ? GetAnimHierarchyFromSkinClump(clump) : NULL;
    uint8_t *matrices = hier ? *(uint8_t **)(hier + 8) : NULL; // RpHAnimHierarchy::pMatrixArray, 64 bytes each
    static const V3d zero = {0, 0, 0};
    for (int i = 0; matrices && bones[i]; i++) {
        int idx = RpHAnimIDGetIndex(hier, bones[i]);
        if (idx >= 0) RwMatrixScale(matrices + (size_t)idx * 64, &zero, 1 /* rwCOMBINEPRECONCAT */);
    }
}

// eWeaponType: 2 screwdriver, 3 golf club, 4 nightstick, 5 knife, 6 baseball bat, 7 hammer,
// 8 cleaver, 9 machete, 10 katana, 11 chainsaw
static int weapon_cuts(int w) {
    switch (g_weapons) {
    case W_CHAINSAW: return w == 11;
    case W_BLADES: return w == 5 || w == 8 || w == 9 || w == 10 || w == 11;
    default: return w >= 2 && w <= 11;
    }
}

// PedNode: 2 head, 3 left arm, 4 right arm, 7 left leg, 8 right leg
static int pick_part(void) {
    static const int limbs[] = {3, 4, 7, 8};
    if (g_parts == P_HEAD) return 2;
    if (g_parts == P_LIMBS) return limbs[arc4random_uniform(4)];
    int r = (int)arc4random_uniform(5);
    return r == 4 ? 2 : limbs[r];
}

// ePedPieceTypes -> PedNode, as the game's own gun code maps them:
// 2 left arm -> 3, 3 right arm -> 4, 4 left leg -> 7, 5 right leg -> 8, 6 head -> 2
static int piece_node(int piece) {
    switch (piece) { case 2: return 3; case 3: return 4; case 4: return 7; case 5: return 8; case 6: return 2; }
    return 0; // torso
}
static int part_allowed(int node) { return g_parts == P_RANDOM || (g_parts == P_HEAD) == (node == 2); }

static int8_t inflict(uint8_t *ped, void *by, int weapon, float damage, int piece, int dir);

static int8_t h_InflictDamage(uint8_t *ped, void *by, int weapon, float damage, int piece, int dir) {
    int before = (int8_t)ped[g_off_part];
    int8_t r = inflict(ped, by, weapon, damage, piece, dir);
    int after = (int8_t)ped[g_off_part];
    if (after != -1 && after != before) { // a part came off in this hit (ours or the game's): throw it
        float dx = 0, dy = 0;
        if (by) { MVCVec3 a = mvc_entity_pos(by), b = mvc_entity_pos(ped); dx = b.x - a.x; dy = b.y - a.y; }
        limbs_spawn(ped, after, dx, dy);
    }
    return r;
}

static int8_t inflict(uint8_t *ped, void *by, int weapon, float damage, int piece, int dir) {
    if (weapon >= 2 && weapon <= 11 && !(ped[g_off_invehicle] & 1)) blood_melee_hit(ped, weapon); // blood setting, not gore
    if (!g_enabled || ped == FindPlayerPed() || (ped[g_off_invehicle] & 1) || (int8_t)ped[g_off_part] != -1)
        return o_InflictDamage(ped, by, weapon, damage, piece, dir);
    float health = *(float *)(ped + g_off_health), armour = *(float *)(ped + g_off_health + 4);
    int killing = health > 0 && damage >= health + armour, node = 0;
    if (weapon_cuts(weapon) && killing) node = pick_part();                       // melee
    else if (g_shotguns && weapon >= 19 && weapon <= 21 && shotgun_roll(by, ped)) { // shotguns, by distance
        node = piece_node(piece);
        if (!node && killing) node = pick_part();
    } else if (g_python && weapon == 18 && piece == 6) node = 2;                   // Python headshot
    if (node && part_allowed(node) && health > 0) {
        RemoveBodyPart(ped, node, dir);
        if (node == 2 && (int8_t)ped[g_off_part] == 2 && damage < health + armour + 1)
            damage = health + armour + 1; // no head, no life
    }
    return o_InflictDamage(ped, by, weapon, damage, piece, dir);
}

// the last "strb wN, [xM, #imm]" before the end of a function, and the n-th load of a
// given size: offsets read from this build's code
static int last_strb_imm(const uint32_t *f, int max) {
    int imm = -1;
    for (int i = 0; f && i < max && f[i] != 0xD65F03C0; i++)
        if ((f[i] & 0xFFC00000) == 0x39000000) imm = (int)((f[i] >> 10) & 0xFFF);
    return imm;
}

static void load_settings(void) {
    char v[32];
    if (mvc_ini_get(INI, "MoreGore", "Enabled", v, sizeof v)) g_enabled = atoi(v) != 0;
    if (mvc_ini_get(INI, "MoreGore", "Melee", v, sizeof v)) g_weapons = atoi(v) % 3;
    if (mvc_ini_get(INI, "MoreGore", "Parts", v, sizeof v)) g_parts = atoi(v) % 3;
    if (mvc_ini_get(INI, "MoreGore", "Shotguns", v, sizeof v)) g_shotguns = atoi(v) != 0;
    if (mvc_ini_get(INI, "MoreGore", "PythonHeadshots", v, sizeof v)) g_python = atoi(v) != 0;
    if (mvc_ini_get(INI, "MoreGore", "ShotgunPointBlank", v, sizeof v)) g_near = strtof(v, NULL);
    if (mvc_ini_get(INI, "MoreGore", "ShotgunGoreRange", v, sizeof v)) g_far = strtof(v, NULL);
}

MVC_API void MVCPluginInit(void) {
    RemoveBodyPart = mvc_sym("_ZN4CPed14RemoveBodyPartE7PedNodea");
    FindPlayerPed = mvc_sym("_Z13FindPlayerPedv");
    void *inflict = mvc_sym("_ZN4CPed13InflictDamageEP7CEntity11eWeaponTypef14ePedPieceTypesh");
    // CPed::RemoveBodyPart stores the part last; CPed::InflictDamage compares the damage
    // with armour + health ("ldr s1, [x19, #armour]; ldr s2, [x19, #health]") and skips
    // peds in a vehicle ("ldrb w8, [x19, #bInVehicle]; tbnz w8, #0") just before that
    g_off_part = last_strb_imm((const uint32_t *)RemoveBodyPart, 400);
    const uint32_t *f = inflict;
    for (int i = 2; f && i < 900 && g_off_health < 0; i++) {
        // ldr s1, [x19, #a] ; ldr s2, [x19, #h]  with h + 4 == a
        uint32_t a = f[i - 1], h = f[i];
        if ((a & 0xFFC003FF) == 0xBD400261 && (h & 0xFFC003FF) == 0xBD400262) {
            int ao = (int)((a >> 10) & 0xFFF) * 4, ho = (int)((h >> 10) & 0xFFF) * 4;
            if (ao == ho + 4) {
                g_off_health = ho;
                for (int k = i - 1; k > i - 12 && k > 0; k--) // the in-vehicle test before it
                    if ((f[k] & 0xFFC003FF) == 0x39400268) { g_off_invehicle = (int)((f[k] >> 10) & 0xFFF); break; }
            }
        }
    }
    if (!RemoveBodyPart || !FindPlayerPed || !inflict || g_off_part < 0 || g_off_health < 0 || g_off_invehicle < 0) {
        mvc_log_error("[MoreGore] this macVC build doesn't have what's needed - not started "
                      "(part +%d, health +%d, in vehicle +%d)", g_off_part, g_off_health, g_off_invehicle);
        return;
    }
    if (mvc_hook(inflict, (void *)h_InflictDamage, (void **)&o_InflictDamage)) {
        mvc_log_error("[MoreGore] couldn't hook CPed::InflictDamage - not started");
        return;
    }
    // hiding removed parts
    GetAnimHierarchyFromSkinClump = mvc_sym("_Z29GetAnimHierarchyFromSkinClumpPN2rw5ClumpE");
    RpHAnimIDGetIndex = mvc_sym("_Z17RpHAnimIDGetIndexPN2rw14HAnimHierarchyEi");
    RwMatrixScale = mvc_sym("_Z13RwMatrixScalePN2rw6MatrixEPKNS_3V3dE15RwOpCombineType");
    PedPool = mvc_sym("_ZN6CPools11ms_pPedPoolE");
    void *update = mvc_sym("_ZN7CEntity13UpdateRpHAnimEv");
    g_off_clump = mvc_first_load_imm(update, 8, 8);               // CEntity::UpdateRpHAnim loads m_rwObject first
    const uint32_t *gp = mvc_sym("_ZN6CPools6GetPedEi");          // "mov w10, #ped size"
    for (int i = 0; gp && i < 16 && !g_ped_size; i++)
        if ((gp[i] & 0xFFE00000) == 0x52800000) g_ped_size = (int)((gp[i] >> 5) & 0xFFFF);
    int hiding = GetAnimHierarchyFromSkinClump && RpHAnimIDGetIndex && RwMatrixScale && PedPool && update &&
                 g_off_clump >= 0 && g_ped_size > 0 && !mvc_hook(update, (void *)h_UpdateRpHAnim, (void **)&o_UpdateRpHAnim);
    if (!hiding) mvc_log_warn("[MoreGore] can't hide removed parts on this macVC build: they'll bleed but stay visible");
    load_settings();
    static const char *const weapons[] = {"Blades/saw", "All melee", "Chainsaw only"};
    static const char *const parts[] = {"Random", "Head only", "Limbs only"};
    int pg = mvc_menu_page("More Gore");
    mvc_menu_toggle(pg, "More Gore", &g_enabled, INI, "MoreGore", "Enabled", NULL);
    mvc_menu_choice(pg, "Melee", weapons, 3, &g_weapons, INI, "MoreGore", "Melee", NULL);
    mvc_menu_toggle(pg, "Shotguns", &g_shotguns, INI, "MoreGore", "Shotguns", NULL);
    mvc_menu_number(pg, "Shotgun point blank", &g_near, 0.f, 10.f, 0.5f, "%.1f m", INI, "MoreGore", "ShotgunPointBlank", NULL);
    mvc_menu_number(pg, "Shotgun gore range", &g_far, 1.f, 40.f, 1.f, "%.0f m", INI, "MoreGore", "ShotgunGoreRange", NULL);
    mvc_menu_toggle(pg, "Python headshots", &g_python, INI, "MoreGore", "PythonHeadshots", NULL);
    blood_menu(pg);
    limbs_menu(pg);
    mvc_menu_choice(pg, "Parts", parts, 3, &g_parts, INI, "MoreGore", "Parts", NULL);
    blood_init();
    if (hiding) limbs_init(g_off_clump);
    mvc_log("[MoreGore] ready: %s, melee: %s, shotguns %s (%.1f-%.0f m), Python headshots %s, parts: %s (part +0x%x, health +0x%x, in vehicle +0x%x)",
            g_enabled ? "on" : "off", weapons[g_weapons], g_shotguns ? "on" : "off", g_near, g_far, g_python ? "on" : "off",
            parts[g_parts], g_off_part, g_off_health, g_off_invehicle);
}
