# cookbook

quick examples for the stuff you'll actually want to do in a plugin. all of these come from [examples/cookbook.c](examples/cookbook.c), which is one whole plugin that CI compiles, so they're not going to be out of date. the full function list is in [API.md](API.md).

start a plugin with `tools/new-plugin.sh MyMod`. that makes `plugins/mymod/`, and `./build.sh` builds it to `build/plugins/MyMod.dylib`. drop that in `GameData/plugins`.

every plugin needs a manifest and an init function:

```c
#include "../../include/macvc.h"
MVC_PLUGIN("MyMod", "1.0", "you", "https://link-to-your-mod", "what it does");

MVC_API void MVCPluginInit(void) {
    // register callbacks, hooks, menu rows and commands here
}
```

## hud stuff

`mvc_on_hud` only runs while the hud is showing, so not in menus or cutscenes or with the hud off. the game lays out its ui for a 448 pixel tall screen, so scale everything by `mvc_screen_h() / 448`.

```c
static void draw_speed(void) {
    if (!show_speed || !mvc_player_vehicle()) return;
    float u = mvc_screen_h() / 448.f;
    MVCVec3 p = mvc_player_pos();
    char text[64];
    snprintf(text, sizeof text, "%.0f %.0f", p.x, p.y);
    mvc_text(mvc_screen_w() - 20 * u, 120 * u, text, 0.5f * u, 1.0f * u, (MVCColor){255, 150, 225, 255},
             MVC_ALIGN_RIGHT, 1, 1);
}
// in MVCPluginInit:  mvc_on_hud(draw_speed);
```

`mvc_rect` draws boxes. for icons, `mvc_sprite_load("models/hud.txd:radar_gasstation")` then `mvc_sprite_draw`. it takes a png or any texture out of a txd.

## a key that does something

game logic goes in `mvc_on_tick`. it runs once per game tick and stops while paused. keys are glfw codes, letters are just their capital ascii.

```c
static int teleport_key = 'H';
static void tick(void) {
    if (mvc_key_pressed(teleport_key)) {
        MVCVec3 p = mvc_player_pos();
        p.z += lift;
        mvc_player_teleport(p);
        times_teleported++;
    }
}
// in MVCPluginInit:  mvc_on_tick(tick);
```

keys read as 0 while the console is open. `mvc_consume_key(key, 1)` hides a key from the game so it doesn't also do whatever the game uses it for. controllers: `mvc_pad_pressed(button)`, buttons 0-14 in glfw order.

## saving stuff with the game

save data goes in a separate file next to each save (`GTAVCsfN.b.mvc`), never inside it, so taking a plugin out can't wreck anybody's save.

```c
static void on_event(int ev, void *arg) {
    if (ev == MVC_EV_SAVE_WRITING) mvc_save_set("cookbook/teleports", &times_teleported, sizeof times_teleported);
    if (ev == MVC_EV_SAVE_LOADED && mvc_save_get("cookbook/teleports", &times_teleported, sizeof times_teleported) < 0)
        times_teleported = 0;
}
// in MVCPluginInit:  mvc_on_event(on_event);
```

put your plugin's name in front of your keys. on a new game (no load) the data starts empty.

## settings in Options > Mods

each call adds a real row to the game's menu. the loader saves it to your ini when it changes and reads it back on startup. 10 rows fit on a page, more go on "Title (2)".

```c
int page = mvc_menu_page("Cookbook");
mvc_menu_toggle(page, "Speed readout", &show_speed, "plugins/Cookbook.ini", "Main", "ShowSpeed", NULL);
mvc_menu_keybind(page, "Teleport up", &teleport_key, "plugins/Cookbook.ini", "Main", "TeleportKey", NULL);
mvc_menu_number(page, "Height", &lift, 5, 100, 5, "%.0f m", "plugins/Cookbook.ini", "Main", "Lift", NULL);
```

`mvc_menu_choice` takes a list of strings. the last argument is a function to call after a change, or NULL.

## patching a constant

this is what `injector::WriteMemory` was on windows. find the instruction (symbol + offset, or by its bytes) and write a new one. patches show up in `hooks`, get undone on hot reload, and `mvc_unpatch(id)` puts the original back.

```c
uint32_t *at = mvc_find_pattern("A8 00 80 52");   // mov w8, #5
if (at) {
    uint32_t mov9 = 0x52800008 | (9 << 5);         // mov w8, #9
    patch_id = mvc_write(at, &mov9, 4);
}
```

arm64 instructions are always 4 bytes. `mvc_nop(addr, n)` kills n of them and `mvc_patch_branch(at, dest, link)` writes a B or BL. to work out an encoding, `xcrun llvm-mc -triple arm64 -show-encoding` or arm's A64 docs.

## hooking a function

use the mangled name. `nm macVC.app/Contents/MacOS/macVC | c++filt` lists them all, or `sym` in the console. drop the leading underscore nm shows.

```c
static void (*o_SetHelpMessage)(uint16_t *, _Bool, _Bool);
static void h_SetHelpMessage(uint16_t *text, _Bool quick, _Bool display_forever) {
    o_SetHelpMessage(text, quick, display_forever); // the original
}
// in MVCPluginInit:
mvc_hook_sym("_ZN4CHud14SetHelpMessageEPtbb", (void *)h_SetHelpMessage, (void **)&o_SetHelpMessage);
```

member functions get `this` as the first argument. if 2 plugins hook the same function they just chain.

## mid-function hooks

for code that got inlined, or a value you need halfway through a function. your handler runs right before the instruction at `at`, and you can read and change any register.

```c
static void on_mid(MVCContext *ctx, void *user) {
    ctx->x[0] = 0; // the instruction at the hook now sees x0 = 0
}
// mvc_midhook((uint8_t *)mvc_sym("_ZN...") + 0x40, on_mid, NULL);
```

floats live in `ctx->q[n]` (low 32 bits are s<n>, low 64 are d<n>). don't touch `ctx->sp`.

## console commands

```c
static void cmd_teleports(int argc, const char **argv) {
    mvc_log("teleported %d time(s) this save", times_teleported);
}
// in MVCPluginInit:
mvc_console_command("teleports", "how often Cookbook teleported you", cmd_teleports);
```

## struct offsets that survive updates

a macVC update can shift struct fields around. instead of hardcoding an offset, pull it out of a game function that uses the field:

```c
// CVehicle::UsesSiren() starts by loading the 2 byte model index, so that load's offset is the field
int model_off = mvc_first_load_imm(mvc_sym("_ZN8CVehicle9UsesSirenEv"), 2, 2);
```

`mvc_load_imm_from_base` is for functions that load through a global (ADRP/ADD then LDR). if you get -1 back, turn your feature off and log it. don't guess.

## while you're working on it

- `reload MyMod` in the console swaps in your rebuilt plugin without restarting the game. export `MVCPluginUnload()` if you need to stash state with `mvc_save_set` first
- `watch player+44C f32` puts a value on screen. `mem`, `poke`, `addr` and `sym` for poking around
- `perf` shows how much frame time each plugin is eating
- `open -a macVC.app --env MVC_EXEC="cmd;cmd"` runs console commands 2 seconds after launch
