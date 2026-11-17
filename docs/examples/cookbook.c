// all the examples from docs/COOKBOOK.md in one plugin, so CI can make sure they compile.
// it's not installed by default, copy it to plugins/cookbook/ if you want to try it in game.
#include <stdio.h>
#include <string.h>
#include "../../include/macvc.h"

MVC_PLUGIN("Cookbook", "1.0", "macVC loader docs", "", "The examples from docs/COOKBOOK.md");

// 1. a HUD element
static int show_speed = 1;
static void draw_speed(void) {
    if (!show_speed || !mvc_player_vehicle()) return;
    float u = mvc_screen_h() / 448.f; // the game's UI is laid out for a 448-pixel-high screen
    MVCVec3 p = mvc_player_pos();
    char text[64];
    snprintf(text, sizeof text, "%.0f %.0f", p.x, p.y);
    mvc_text(mvc_screen_w() - 20 * u, 120 * u, text, 0.5f * u, 1.0f * u, (MVCColor){255, 150, 225, 255},
             MVC_ALIGN_RIGHT, 1, 1);
}

// 2. a rebindable key that does something
static int teleport_key = 'H'; // GLFW key codes: letters are their ASCII capital
static float lift = 20;         // metres; set in the Mods menu (recipe 4)
static int times_teleported;    // kept in the save (recipe 3)
static void tick(void) {
    if (mvc_key_pressed(teleport_key)) {
        MVCVec3 p = mvc_player_pos();
        p.z += lift;
        mvc_player_teleport(p);
        times_teleported++;
    }
}

// 3. save data next to the player's save
static void on_event(int ev, void *arg) {
    (void)arg;
    if (ev == MVC_EV_SAVE_WRITING) mvc_save_set("cookbook/teleports", &times_teleported, sizeof times_teleported);
    if (ev == MVC_EV_SAVE_LOADED && mvc_save_get("cookbook/teleports", &times_teleported, sizeof times_teleported) < 0)
        times_teleported = 0;
}

// 4. settings in Options > Mods
static void menu(void) {
    int page = mvc_menu_page("Cookbook");
    mvc_menu_toggle(page, "Speed readout", &show_speed, "plugins/Cookbook.ini", "Main", "ShowSpeed", NULL);
    mvc_menu_keybind(page, "Teleport up", &teleport_key, "plugins/Cookbook.ini", "Main", "TeleportKey", NULL);
    mvc_menu_number(page, "Height", &lift, 5, 100, 5, "%.0f m", "plugins/Cookbook.ini", "Main", "Lift", NULL);
}

// 5. patching a constant in game code
// find the instruction once (here by its bytes), then write the new one.
static int patch_id;
static void patch_example(void) {
    // e.g. "mov w8, #5" -> "mov w8, #9": MOVZ W8, #imm is 0x52800008 | imm << 5
    uint32_t *at = mvc_find_pattern("A8 00 80 52"); // mov w8, #5
    if (!at) return;
    uint32_t mov9 = 0x52800008 | (9 << 5);
    patch_id = mvc_write(at, &mov9, 4); // mvc_unpatch(patch_id) undoes it
}

// 6. hooking a game function
static void (*o_SetHelpMessage)(uint16_t *, _Bool, _Bool);
static void h_SetHelpMessage(uint16_t *text, _Bool quick, _Bool display_forever) {
    // runs instead of CHud::SetHelpMessage; call the original to keep the game's behaviour
    o_SetHelpMessage(text, quick, display_forever);
}

// 7. a mid-function hook that changes a register
static void on_mid(MVCContext *ctx, void *user) {
    (void)user;
    ctx->x[0] = 0; // the instruction at the hook point now sees x0 = 0
}

// 8. a console command
static void cmd_teleports(int argc, const char **argv) {
    (void)argc; (void)argv;
    mvc_log("teleported %d time(s) this save", times_teleported);
}

MVC_API void MVCPluginInit(void) {
    mvc_on_hud(draw_speed);
    mvc_on_tick(tick);
    mvc_on_event(on_event);
    menu();
    mvc_hook_sym("_ZN4CHud14SetHelpMessageEPtbb", (void *)h_SetHelpMessage, (void **)&o_SetHelpMessage);
    mvc_console_command("teleports", "how often Cookbook teleported you", cmd_teleports);
    (void)patch_example; (void)on_mid; // shown in the cookbook, not run here
}
