// In-game self test for the loader API. not installed by default:
//   ./build.sh && xcrun clang -arch arm64 -dynamiclib tests/selftest.c build/macVCLoader.dylib -o build/test/SelfTest.dylib
// then copy SelfTest.dylib to GameData/plugins and read the console / log.
#include <stdio.h>
#include <string.h>
#include "../include/macvc.h"

MVC_PLUGIN("SelfTest", "1", "macVC loader", "", "Exercises the loader API and logs PASS/FAIL");

static int fails;
#define CHECK(cond, what) do { if (cond) mvc_log("PASS  %s", what); else { mvc_log_error("FAIL  %s", what); fails++; } } while (0)

static volatile int mid_calls, mid_x0_ok, hook_calls, ticks;
static void *pad0;

static void on_mid(MVCContext *ctx, void *user) {
    mid_calls++;
    if (user == (void *)0x1234 && ctx->sp && (ctx->sp & 15) == 0) mid_x0_ok++;
}

static void *(*o_FindPlayerPed)(void);
static void *h_FindPlayerPed(void) { hook_calls++; return o_FindPlayerPed(); }

static void tick(void) {
    ticks++;
    if (ticks == 120) {
        CHECK(mid_calls > 0, "midhook on FindPlayerVehicle+0x20 (CBZ) runs");
        CHECK(mid_x0_ok == mid_calls && mid_calls > 0, "midhook gets user data and an aligned sp");
        CHECK(hook_calls > 0, "inline hook on FindPlayerPed (ADRP relocated) runs");
        CHECK(mvc_unhook(mvc_sym("_Z13FindPlayerPedv")) == 0, "unhook FindPlayerPed");
        hook_calls = 0;
    }
    if (ticks == 150) {
        // SDK, in game
        MVCVec3 p = mvc_player_pos();
        CHECK(p.x != 0 || p.y != 0, "player position");
        CHECK(mvc_player_health() > 0 && mvc_player_health() < 1000, "player health is readable");
        int m = mvc_player_money();
        mvc_player_set_money(m + 1);
        CHECK(mvc_player_money() == m + 1, "money write/read");
        mvc_player_set_money(m);
        float sx, sy;
        MVCVec3 head = {p.x, p.y, p.z + 1};
        CHECK(mvc_world_to_screen(head, &sx, &sy), "world to screen (player is on screen)");
        CHECK(mvc_entity_pos(mvc_player_ped()).x == p.x, "entity position offset");
        int h, mi;
        mvc_world_time(&h, &mi);
        CHECK(h >= 0 && h < 24 && mi >= 0 && mi < 60, "world clock");
        CHECK(mvc_sprite_load("models/hud.txd:radar_north") > 0 || mvc_sprite_load("models/hud.txd:radarDisc") > 0, "sprite from a TXD");
    }
    if (ticks == 240) {
        CHECK(hook_calls == 0, "no calls after unhook");
        if (fails) mvc_log_error("SELFTEST: %d check(s) FAILED", fails);
        else mvc_log("SELFTEST: all checks passed");
    }
}

static void crash_now(int argc, const char **argv) {
    (void)argc; (void)argv;
    mvc_log("crashing on purpose...");
    volatile int *p = NULL;
    *p = 1;
}

static void on_event(int ev, void *arg) {
    if (ev != MVC_EV_VEHICLE_DESTROYED) mvc_log("event received: %s", mvc_event_name(ev));
}

MVC_API void MVCPluginInit(void) {
    CHECK(mvc_api_version() == MVC_API_VERSION, "api version");
    // SDK, startup
    mvc_text_set("MVTEST", "Hello");
    const uint16_t *t = mvc_text_get("MVTEST");
    CHECK(t && t[0] == 'H' && t[4] == 'o' && !t[5], "custom game text");
    CHECK(mvc_ini_set("selftest.ini", "Test", "Value", "42"), "ini write");
    char v[16] = "";
    CHECK(mvc_ini_get("selftest.ini", "Test", "Value", v, sizeof v) && !strcmp(v, "42"), "ini read back");
    CHECK(mvc_save_set("selftest.key", "abc", 4) && mvc_save_get("selftest.key", v, sizeof v) == 4 && !strcmp(v, "abc"), "save data set/get");
    CHECK(mvc_key_from_name(mvc_key_name('G')) == 'G' && mvc_key_from_name("F9") == 298 && mvc_key_from_name("LSHIFT") == 340, "key names");
    mvc_console_command("crashtest", "crash the game on purpose to test the crash reporter", crash_now);
    uint8_t *brake = mvc_sym("_ZN4CPad8GetBrakeEv");
    CHECK(brake != NULL, "symbol lookup");

    char pat[64];
    snprintf(pat, sizeof pat, "%02X %02X %02X %02X ?? %02X %02X %02X", brake[0], brake[1], brake[2], brake[3], brake[5], brake[6], brake[7]);
    void *found = mvc_find_pattern(pat);
    CHECK(found != NULL, "pattern scan finds something");

    // patch + unpatch round trip on code
    uint8_t *hb = mvc_sym("_ZN4CPad12GetHandBrakeEv");
    uint8_t before[8];
    memcpy(before, hb + 8, 8);
    int id = mvc_nop(hb + 8, 2);
    uint32_t nop;
    memcpy(&nop, hb + 8, 4);
    CHECK(id > 0 && nop == 0xD503201F, "mvc_nop writes NOPs into code");
    CHECK(mvc_unpatch(id) == 0 && !memcmp(before, hb + 8, 8), "mvc_unpatch restores the original bytes");

    CHECK(mvc_midhook((uint8_t *)mvc_sym("_Z17FindPlayerVehiclev") + 0x20, on_mid, (void *)0x1234) == 0, "install midhook");
    CHECK(mvc_hook(mvc_sym("_Z13FindPlayerPedv"), (void *)h_FindPlayerPed, (void **)&o_FindPlayerPed) == 0, "install inline hook");
    mvc_on_tick(tick);
    mvc_on_event(on_event);
    (void)pad0;
}
