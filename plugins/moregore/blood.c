// more gore's blood. more of the game's own blood, used more like real blood.
//


// only uses vice city's own particles (BLOOD, BLOOD_SPURT, BLOODDROP) and its blood pool
// texture and colour so it still looks like 2002:
//   spray out the far side of the ped along the bullet, bigger for stronger guns
//   splatter decals on the ground behind the ped where the spray lands
//   corpse pools that start small and spread for about 15 seconds
//   stumps pump blood for a bit, then drip
//   hurt peds (and the player) leave drips, more the worse they are
//   every melee hit bleeds, the chainsaw sprays
//   pools and splatter last minutes instead of 40 seconds
//


// the game only has a 48 slot store for ground decals, shared with lamp post shadows and
// such. so i keep my own list of 512 and hand the game the closest ones each frame
// (16 on More, 28 on Extreme), leaving it the rest.
#include "moregore.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { B_VANILLA, B_MORE, B_EXTREME };
static int g_blood = B_MORE, g_spray = 1, g_splatter = 1, g_pools = 1, g_stumps = 1, g_trails = 1, g_melee = 1,
           g_lasting = 1;
static const char *INI = "plugins/MoreGore.ini";

typedef struct { float x, y, z; } V3;
enum { P_BLOOD = 6, P_BLOOD_SMALL = 7, P_BLOOD_SPURT = 8, P_BLOODDROP = 41 };
static void *(*AddParticle)(int type, const V3 *pos, const V3 *vel, void *ent, float size, int rotSpeed, int rot,
                            int startFrame, int lifeSpan);
static _Bool (*StoreStaticShadow)(uint32_t id, uint8_t type, void *tex, const V3 *pos, float fx, float fy, float sx,
                                  float sy, int16_t intensity, uint8_t r, uint8_t g, uint8_t b, float zdist, float scale,
                                  float drawdist, _Bool temporary, float updist);
static void (*o_UpdatePermanentShadows)(void);
static void (*o_DoBulletImpact)(void *weapon, void *shooter, uint8_t *victim, V3 *src, V3 *dst, V3 *colpoint,
                                float aheadx, float aheady);
static void (*GetComponentPosition)(void *ik, V3 *out, uint32_t node);
static void **BloodPoolTex;            // gpBloodPoolTex
static int g_off_ik = -1, g_off_speed = 0x84; // CPed::m_pedIK; CPhysical::m_vecMoveSpeed (checked in init)

static float frand(float a, float b) { return a + (b - a) * (float)arc4random_uniform(10001) / 10000.f; }
int blood_level(void) { return g_blood; }
static float amount(void) { return g_blood == B_EXTREME ? 2.f : 1.f; }
static V3 pos_of(void *e) { MVCVec3 p = mvc_entity_pos(e); return (V3){p.x, p.y, p.z}; }
static void particle(int type, V3 p, V3 v) { if (AddParticle) AddParticle(type, &p, &v, NULL, 0.f, 0, 0, 0, 0); }

// decals
#define MAX_DECALS 512
typedef struct { V3 pos; float size, angle; uint32_t born, life; int used; } Decal;
static Decal g_decals[MAX_DECALS];
static int g_next;

static void add_decal(V3 pos, float size, uint32_t life_ms) {
    if (!g_splatter && !g_pools && !g_trails && !g_stumps) return;
    // reuse a free slot, else the oldest
    int at = -1;
    uint32_t now = mvc_time_ms(), oldest = 0;
    for (int k = 0; k < MAX_DECALS; k++) {
        int i = (g_next + k) % MAX_DECALS;
        if (!g_decals[i].used || now - g_decals[i].born >= g_decals[i].life) { at = i; break; }
        if (now - g_decals[i].born > oldest) { oldest = now - g_decals[i].born; at = i; }
    }
    g_next = (at + 1) % MAX_DECALS;
    if (!g_lasting && life_ms > 40000) life_ms = 40000; // the game's own pool time
    g_decals[at] = (Decal){pos, size, frand(0, 6.2832f), now, life_ms, 1};
}

// after the game's own permanent decals: submit the closest of ours
static void h_UpdatePermanentShadows(void) {
    o_UpdatePermanentShadows();
    if (g_blood == B_VANILLA || !StoreStaticShadow || !BloodPoolTex || !*BloodPoolTex) return;
    int budget = g_blood == B_EXTREME ? 28 : 16;
    void *player = FindPlayerPed();
    if (!player) return;
    V3 me = pos_of(player);
    uint32_t now = mvc_time_ms();
    // pick the closest live decals within the game's 40 m shadow draw distance
    static int idx[MAX_DECALS];
    static float dist[MAX_DECALS];
    int n = 0;
    for (int i = 0; i < MAX_DECALS; i++) {
        Decal *d = &g_decals[i];
        if (!d->used) continue;
        if (now - d->born >= d->life) { d->used = 0; continue; }
        float dx = d->pos.x - me.x, dy = d->pos.y - me.y, dd = dx * dx + dy * dy;
        if (dd > 40.f * 40.f) continue;
        idx[n] = i;
        dist[n++] = dd;
    }
    for (int s = 0; s < budget && s < n; s++) { // partial selection sort: nearest first
        int b = s;
        for (int k = s + 1; k < n; k++) if (dist[k] < dist[b]) b = k;
        int ti = idx[s]; idx[s] = idx[b]; idx[b] = ti;
        float td = dist[s]; dist[s] = dist[b]; dist[b] = td;
        Decal *d = &g_decals[idx[s]];
        uint32_t age = now - d->born;
        float life = (float)d->life, fade = (float)age > life * 0.75f ? 1.f - ((float)age - life * 0.75f) / (life * 0.25f) : 1.f;
        float c = cosf(d->angle) * d->size, sn = sinf(d->angle) * d->size;
        // the game's own blood-pool look: dark shadow type, red, full intensity
        StoreStaticShadow(0xB1D00000u + (uint32_t)idx[s], 1, *BloodPoolTex, &d->pos, c, sn, sn, -c,
                          (int16_t)(255 * fade), (uint8_t)(255 * fade), 0, 0, 4.f, 1.f, 40.f, 0, 0.f);
    }
}

// bullets
static int gun_power(int w) {
    switch (w) {
    case 17: case 18: return 4;                 // pistol, Python
    case 19: case 20: case 21: return 3;        // shotguns (per pellet)
    case 22: case 23: case 24: case 25: return 3; // SMGs
    case 26: case 27: return 5;                 // M4, Ruger
    case 28: case 29: return 9;                 // snipers
    case 32: case 33: return 4;                 // M60, minigun
    }
    return 2;
}

static void h_DoBulletImpact(void *weapon, void *shooter, uint8_t *victim, V3 *src, V3 *dst, V3 *colpoint,
                             float ax, float ay) {
    o_DoBulletImpact(weapon, shooter, victim, src, dst, colpoint, ax, ay);
    if (g_blood == B_VANILLA || !victim || !is_ped(victim) || !src || !colpoint) return;
    V3 hit = *colpoint, d = {hit.x - src->x, hit.y - src->y, hit.z - src->z};
    float len = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    if (len < 0.01f) return;
    d.x /= len; d.y /= len; d.z /= len;
    int w = weapon ? *(int32_t *)weapon : 0, power = gun_power(w);
    if (g_spray) {
        int n = (int)(power * amount());
        for (int i = 0; i < n; i++) { // out the far side, along the bullet, spreading and falling
            float sp = frand(0.04f, 0.12f);
            V3 v = {d.x * sp + frand(-0.02f, 0.02f), d.y * sp + frand(-0.02f, 0.02f), d.z * sp + frand(0.0f, 0.04f)};
            particle(i % 3 == 0 ? P_BLOOD_SPURT : P_BLOOD, hit, v);
        }
        for (int i = 0; i < n / 2; i++) particle(P_BLOODDROP, hit, (V3){d.x * 0.03f, d.y * 0.03f, 0.f});
    }
    if (g_splatter) { // where that spray lands: behind the ped
        int splats = power >= 5 ? 2 : 1;
        if (g_blood == B_EXTREME) splats++;
        for (int i = 0; i < splats; i++) {
            if (arc4random_uniform(100) >= (uint32_t)(g_blood == B_EXTREME ? 100 : 65)) continue;
            float r = frand(0.6f, 1.2f + 0.25f * power);
            V3 p = {hit.x + d.x * r + frand(-0.3f, 0.3f), hit.y + d.y * r + frand(-0.3f, 0.3f), hit.z};
            add_decal(p, frand(0.12f, 0.22f + 0.04f * power), g_lasting ? 180000 : 40000);
        }
    }
}

// melee
void blood_melee_hit(uint8_t *ped, int weapon) {
    if (g_blood == B_VANILLA || !g_melee) return;
    V3 p = pos_of(ped);
    p.z += 0.4f;                                   // chest
    int n = (int)((weapon == 11 ? 8 : weapon >= 5 ? 5 : 3) * amount());
    for (int i = 0; i < n; i++)
        particle(weapon == 11 && i % 2 ? P_BLOOD_SPURT : P_BLOOD, p,
                 (V3){frand(-0.06f, 0.06f), frand(-0.06f, 0.06f), frand(0.0f, 0.07f)});
    if (g_splatter && (weapon == 11 || arc4random_uniform(2)))
        add_decal((V3){p.x + frand(-0.5f, 0.5f), p.y + frand(-0.5f, 0.5f), p.z}, frand(0.12f, 0.28f),
                  g_lasting ? 180000 : 40000);
}

// per-ped tracking
#define MAX_TRACK 160
typedef struct { uint8_t *ped; uint32_t died, stump_since, last_drip, last_stump_drop; int pool_stage, part; } Track;
static Track g_track[MAX_TRACK];

static Track *track(uint8_t *ped) {
    Track *free_slot = NULL;
    for (int i = 0; i < MAX_TRACK; i++) {
        if (g_track[i].ped == ped) return &g_track[i];
        if (!g_track[i].ped && !free_slot) free_slot = &g_track[i];
    }
    if (free_slot) { memset(free_slot, 0, sizeof *free_slot); free_slot->ped = ped; free_slot->part = -1; }
    return free_slot;
}

static const float POOL_AT[] = {1.0f, 3.0f, 6.0f, 10.0f, 15.0f};     // seconds after death
static const float POOL_SIZE[] = {0.35f, 0.6f, 0.85f, 1.05f, 1.25f}; // metres (the game's pool is 0.75)

static void tick(void) {
    if (g_blood == B_VANILLA) return;
    uint8_t *pool = PedPool ? *PedPool : NULL;
    if (!pool || g_ped_size <= 0) return;
    uint8_t *first = *(uint8_t **)pool, *flags = *(uint8_t **)(pool + 8);
    int n = *(int32_t *)(pool + 0x10);
    uint32_t now = mvc_time_ms();
    // forget peds that are gone
    for (int i = 0; i < MAX_TRACK; i++) {
        uint8_t *p = g_track[i].ped;
        if (!p) continue;
        size_t slot = (size_t)(p - first) / (size_t)g_ped_size;
        if (slot >= (size_t)n || (flags[slot] & 0x80)) g_track[i].ped = NULL;
    }
    for (int i = 0; i < n; i++) {
        if (flags[i] & 0x80) continue;
        uint8_t *ped = first + (size_t)i * (size_t)g_ped_size;
        if (ped[g_off_invehicle] & 1) continue;
        float health = *(float *)(ped + g_off_health);
        int part = (int8_t)ped[g_off_part];
        int dead = health <= 0.f;
        if (!dead && part == -1 && health >= 50.f) continue; // nothing to bleed
        Track *t = track(ped);
        if (!t) continue;
        V3 p = pos_of(ped);
        // growing pool under a corpse
        if (dead && g_pools) {
            if (!t->died) t->died = now ? now : 1;
            float secs = (float)(now - t->died) / 1000.f;
            while (t->pool_stage < 5 && secs >= POOL_AT[t->pool_stage]) {
                add_decal((V3){p.x, p.y, p.z}, POOL_SIZE[t->pool_stage] * (g_blood == B_EXTREME ? 1.25f : 1.f),
                          g_lasting ? (g_blood == B_EXTREME ? 480000 : 240000) : 40000);
                t->pool_stage++;
            }
        }
        // a stump that pumps, then drips
        if (part != -1 && g_stumps && g_off_ik >= 0 && GetComponentPosition) {
            if (t->part != part) { t->part = part; t->stump_since = now; limbs_spawn(ped, part, 0, 0); }
            uint32_t age = now - t->stump_since, dur = g_blood == B_EXTREME ? 12000 : 6000;
            if (age < dur) {
                V3 s;
                GetComponentPosition(ped + g_off_ik, &s, (uint32_t)part);
                float strength = 1.f - (float)age / (float)dur;         // weaker as it bleeds out
                int pulse = (age % 800) < 260;                           // heartbeat
                if (pulse) {
                    int k = (int)(3 * amount() * strength) + 1;
                    for (int j = 0; j < k; j++)
                        particle(P_BLOOD_SPURT, s, (V3){frand(-0.03f, 0.03f), frand(-0.03f, 0.03f), frand(0.02f, 0.08f) * strength});
                } else if (arc4random_uniform(4) == 0) particle(P_BLOODDROP, s, (V3){0, 0, 0});
                if (now - t->last_stump_drop > 450) {
                    t->last_stump_drop = now;
                    add_decal((V3){s.x + frand(-0.2f, 0.2f), s.y + frand(-0.2f, 0.2f), s.z}, frand(0.1f, 0.2f),
                              g_lasting ? 180000 : 40000);
                }
            }
        }
        // a wounded ped drips as they move
        if (!dead && health < 50.f && g_trails) {
            V3 v = *(V3 *)(ped + g_off_speed);
            float speed = sqrtf(v.x * v.x + v.y * v.y);
            uint32_t every = (uint32_t)(300.f + 1200.f * (health / 50.f)) / (g_blood == B_EXTREME ? 2 : 1);
            if (speed > 0.01f && now - t->last_drip > every) {
                t->last_drip = now;
                add_decal((V3){p.x + frand(-0.15f, 0.15f), p.y + frand(-0.15f, 0.15f), p.z}, frand(0.06f, 0.13f),
                          g_lasting ? 120000 : 30000);
                particle(P_BLOODDROP, (V3){p.x, p.y, p.z + 0.2f}, (V3){0, 0, 0});
            }
        }
    }
}

static void on_event(int ev, void *arg) {
    if (ev == MVC_EV_NEW_SESSION) { memset(g_decals, 0, sizeof g_decals); memset(g_track, 0, sizeof g_track); }
}

static void load(void) {
    char v[32];
    struct { const char *key; int *val; } ints[] = {{"Blood", &g_blood}, {"BulletSpray", &g_spray}, {"Splatter", &g_splatter},
        {"GrowingPools", &g_pools}, {"StumpBleeding", &g_stumps}, {"WoundedTrails", &g_trails}, {"MeleeBlood", &g_melee},
        {"LastingBlood", &g_lasting}};
    for (size_t i = 0; i < sizeof ints / sizeof ints[0]; i++)
        if (mvc_ini_get(INI, "Blood", ints[i].key, v, sizeof v)) *ints[i].val = atoi(v);
    if (g_blood < 0 || g_blood > 2) g_blood = B_MORE;
}

void blood_menu(int pg) {
    static const char *const levels[] = {"Vanilla", "More", "Extreme"};
    mvc_menu_choice(pg, "Blood", levels, 3, &g_blood, INI, "Blood", "Blood", NULL);
    mvc_menu_toggle(pg, "Bullet spray", &g_spray, INI, "Blood", "BulletSpray", NULL);
    mvc_menu_toggle(pg, "Ground splatter", &g_splatter, INI, "Blood", "Splatter", NULL);
    mvc_menu_toggle(pg, "Growing pools", &g_pools, INI, "Blood", "GrowingPools", NULL);
    mvc_menu_toggle(pg, "Stump bleeding", &g_stumps, INI, "Blood", "StumpBleeding", NULL);
    mvc_menu_toggle(pg, "Wounded trails", &g_trails, INI, "Blood", "WoundedTrails", NULL);
    mvc_menu_toggle(pg, "Melee blood", &g_melee, INI, "Blood", "MeleeBlood", NULL);
    mvc_menu_toggle(pg, "Lasting blood", &g_lasting, INI, "Blood", "LastingBlood", NULL);
}

void blood_init(void) {
    load();
    AddParticle = mvc_sym("_ZN9CParticle11AddParticleE13tParticleTypeRK7CVectorS3_P7CEntityfiiii");
    StoreStaticShadow = mvc_sym("_ZN8CShadows17StoreStaticShadowEjhPN2rw7TextureEPK7CVectorffffshhhfffbf");
    GetComponentPosition = mvc_sym("_ZN6CPedIK20GetComponentPositionERN2rw3V3dEj");
    BloodPoolTex = mvc_sym("gpBloodPoolTex");
    // CPed::m_pedIK: CPed::RemoveBodyPart passes "add x0, x20, #ik" to GetComponentPosition
    const uint32_t *rb = mvc_sym("_ZN4CPed14RemoveBodyPartE7PedNodea");
    for (int i = 0; rb && i < 80 && g_off_ik < 0; i++)
        if ((rb[i] & 0xFFC003FF) == 0x91000280) g_off_ik = (int)((rb[i] >> 10) & 0xFFF); // add x0, x20, #imm
    // m_vecMoveSpeed (0x84): CTheScripts::Process reads entity speeds with "ldp s0, s1, [x9, #0x84]"
    int ok = AddParticle && StoreStaticShadow && BloodPoolTex;
    if (!ok) { mvc_log_warn("[MoreGore] blood overhaul not available on this macVC build"); return; }
    if (mvc_hook_sym("_ZN8CShadows22UpdatePermanentShadowsEv", (void *)h_UpdatePermanentShadows, (void **)&o_UpdatePermanentShadows))
        mvc_log_warn("[MoreGore] can't add ground blood on this macVC build");
    if (mvc_hook_sym("_ZN7CWeapon14DoBulletImpactEP7CEntityS1_P7CVectorS3_P9CColPoint9CVector2D", (void *)h_DoBulletImpact,
                     (void **)&o_DoBulletImpact))
        mvc_log_warn("[MoreGore] can't add bullet blood on this macVC build");
    mvc_on_tick(tick);
    mvc_on_event(on_event);
    mvc_log("[MoreGore] blood: %s (pedIK +0x%x)", g_blood == 0 ? "vanilla" : g_blood == 1 ? "more" : "extreme", g_off_ik);
}
