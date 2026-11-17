# api reference

everything in `include/macvc.h`, for loader 1.0 (plugin api 1). for examples see [COOKBOOK.md](COOKBOOK.md).

## overview

plugins are arm64 .dylib files in GameData/plugins/ or GameData/scripts/, the mac version of an .asi. set up hooks from a constructor (like DllMain) or export void MVCPluginInit(void), which the loader calls right after loading you. add a manifest with MVC_PLUGIN(...) so your name and version show up and the api version gets checked. hot reload: `reload <plugin>` in the console undoes your hooks, patches, callbacks, commands and menu rows, loads the new file, calls MVCPluginInit again and replays the events you missed (game init, new session, save loaded). statics start fresh. to keep state, export void MVCPluginUnload(void), stash it with mvc_save_set and read it back on MVC_EV_SAVE_LOADED. stop your own threads and timers there too. within one MVC_API_VERSION functions only get added, never changed or removed.

## contents

- [plugin manifest](#plugin-manifest)
- [logging (shows in the console and GameData/macVCLoader.log)](#logging-shows-in-the-console-and-gamedatamacvcloaderlog)
- [game symbols](#game-symbols)
- [hooks](#hooks)
- [memory patches (like injector::WriteMemory / MakeNOP / MakeCALL)](#memory-patches-like-injectorwritememory--makenop--makecall)
- [callbacks](#callbacks)
- [input (GLFW key / gamepad button codes)](#input-glfw-key--gamepad-button-codes)
- [game state helpers](#game-state-helpers)
- [drawing (call from hud/radar/frame callbacks)](#drawing-call-from-hudradarframe-callbacks)
- [ARM64 instruction helpers, for reading struct offsets out of game code](#arm64-instruction-helpers-for-reading-struct-offsets-out-of-game-code)
- [more input](#more-input)
- [console commands](#console-commands)
- [game text (GXT keys) and FXT files](#game-text-gxt-keys-and-fxt-files)
- [Mods menu (a real page in Options > Mods)](#mods-menu-a-real-page-in-options--mods)
- [save data (stored next to each save, never inside it)](#save-data-stored-next-to-each-save-never-inside-it)
- [player](#player)
- [vehicles](#vehicles)
- [world](#world)
- [3D](#3d)
- [sprites](#sprites)
- [audio (through the game's OpenAL device)](#audio-through-the-games-openal-device)

## plugin manifest

#### `MVCPluginInfo`
```c
typedef struct {
    int api_version;          // MVC_API_VERSION the plugin was built against
    const char *name;
    const char *version;
    const char *author;
    const char *url;
    const char *description;
} MVCPluginInfo;
```

#### `MVC_PLUGIN`
```c
#define MVC_PLUGIN(name, version, author, url, description) \
    const MVCPluginInfo mvc_plugin_info = {MVC_API_VERSION, name, version, author, url, description}
```
use once in a plugin:  MVC_PLUGIN("Gasoline", "1.1", "you", "https://...", "Fuel system");

## logging (shows in the console and GameData/macVCLoader.log)

#### `mvc_log`
```c
void mvc_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
```

#### `mvc_log_warn`
```c
void mvc_log_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
```

#### `mvc_log_error`
```c
void mvc_log_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
```

## game symbols

#### `mvc_sym`
```c
void *mvc_sym(const char *name);
```
name is the C++ mangled name without the leading underscore Mach-O adds, e.g. "_ZN4CHud4DrawEv" for CHud::Draw(). returns NULL if not found.

#### `mvc_find_pattern`
```c
void *mvc_find_pattern(const char *pattern);
```
finds a byte pattern in the game's code, e.g. "1F 20 03 D5 ?? ?? 40 F9". returns the address of the first match or NULL.

## hooks

#### `mvc_hook`
```c
int mvc_hook(void *target, void *replacement, void **original);
```
inline-hooks `target` so it jumps to `replacement`. if `original` is not NULL it receives a callable trampoline to the unhooked function. two plugins may hook the same function; the hooks chain. returns 0 on success.

#### `mvc_hook_sym`
```c
int mvc_hook_sym(const char *name, void *replacement, void **original);
```

#### `mvc_unhook`
```c
int mvc_unhook(void *target);
```
removes a hook. fails if another plugin has hooked the function after you.

#### `MVCContext`
```c
typedef struct {
    uint64_t x[31];        // x0..x30 (x29 = frame pointer, x30 = link register)
    uint64_t sp;           // read-only
    uint64_t nzcv;         // condition flags
    uint64_t reserved;
    __uint128_t q[32];     // NEON / floating point registers (s0/d0 live in q[0])
} MVCContext;
```
mid-function hook: runs `fn` before the instruction at `at` executes, with every register available (and writable) through `ctx`. don't change ctx->sp.

#### `mvc_midhook_fn`
```c
typedef void (*mvc_midhook_fn)(MVCContext *ctx, void *user);
```

#### `mvc_midhook`
```c
int mvc_midhook(void *at, mvc_midhook_fn fn, void *user);
```

## memory patches (like injector::WriteMemory / MakeNOP / MakeCALL)

#### `mvc_write`
```c
int mvc_write(void *addr, const void *bytes, size_t len);
```
each returns a patch id > 0 (pass to mvc_unpatch) or -1 on failure.

#### `mvc_nop`
```c
int mvc_nop(void *addr, int instructions);
```

#### `mvc_patch_branch`
```c
int mvc_patch_branch(void *at, void *dest, int link);
```
writes a B (link = 0) or BL (link = 1) at `at` to `dest` (must be within +-128MB).

#### `mvc_unpatch`
```c
int mvc_unpatch(int patch_id);
```

## callbacks

#### `mvc_on_tick`
```c
void mvc_on_tick(mvc_callback cb);  // game logic, once per CGame::Process
```

#### `mvc_on_frame`
```c
void mvc_on_frame(mvc_callback cb); // every rendered frame, after HUD and menus (drawing)
```

#### `mvc_on_hud`
```c
void mvc_on_hud(mvc_callback cb);   // right after CHud::Draw, only while the HUD is visible
```

#### `mvc_on_radar`
```c
void mvc_on_radar(mvc_callback cb); // right after the radar blips are drawn
```

#### `mvc_on_radar_under`
```c
void mvc_on_radar_under(mvc_callback cb); // after the radar map, before the blips (draws under them)
```

#### `mvc_menu_map_active`
```c
int mvc_menu_map_active(void);
```
radar callbacks also run on the pause-menu map, where the CRadar transforms map to the big map automatically. this tells the two apart.

#### Constants
```c
enum {
    MVC_EV_GAME_INIT = 1,      // game systems initialised (once)
    MVC_EV_NEW_SESSION,        // a new game started or a save finished loading
    MVC_EV_SAVE_LOADED,        // arg: NULL
    MVC_EV_SAVE_WRITTEN,       // arg: (void *)(intptr_t)slot
    MVC_EV_SHUTDOWN,           // game is quitting
    MVC_EV_PAUSED,             // pause menu opened
    MVC_EV_RESUMED,
    MVC_EV_VEHICLE_ENTERED,    // arg: vehicle
    MVC_EV_VEHICLE_EXITED,     // arg: vehicle
    MVC_EV_VEHICLE_DESTROYED,  // arg: vehicle (memory about to be freed - drop references)
    MVC_EV_WASTED,
    MVC_EV_BUSTED,
    MVC_EV_MISSION_STARTED,
    MVC_EV_MISSION_ENDED,
};
```

#### `mvc_event_fn`
```c
typedef void (*mvc_event_fn)(int event, void *arg);
```

#### `mvc_on_event`
```c
void mvc_on_event(mvc_event_fn fn);
```

#### `mvc_event_name`
```c
const char *mvc_event_name(int event);
```

## input (GLFW key / gamepad button codes)

#### `mvc_key_down`
```c
int mvc_key_down(int glfw_key);
```
all return 0 while the loader console is open.

#### `mvc_key_pressed`
```c
int mvc_key_pressed(int glfw_key);   // went down this frame
```

#### `mvc_pad_down`
```c
int mvc_pad_down(int glfw_button);    // first connected gamepad
```

#### `mvc_pad_pressed`
```c
int mvc_pad_pressed(int glfw_button);
```

#### `mvc_console_open`
```c
int mvc_console_open(void);
```

## game state helpers

#### `mvc_game_dir`
```c
const char *mvc_game_dir(void);       // .../Contents/Resources/GameData
```

#### `mvc_screen_w`
```c
float mvc_screen_w(void);
```

#### `mvc_screen_h`
```c
float mvc_screen_h(void);
```

#### `mvc_hud_visible`
```c
int mvc_hud_visible(void);
```

#### `mvc_time_ms`
```c
uint32_t mvc_time_ms(void);           // game clock (stops while paused)
```

#### `mvc_safe_mode`
```c
int mvc_safe_mode(void);
```

## drawing (call from hud/radar/frame callbacks)

#### Constants
```c
enum { MVC_ALIGN_LEFT = 0, MVC_ALIGN_CENTER = 1, MVC_ALIGN_RIGHT = 2 };
```

#### `mvc_rect`
```c
void mvc_rect(float left, float top, float right, float bottom, MVCColor c);
```

#### `mvc_text`
```c
void mvc_text(float x, float y, const char *ascii, float scale_x, float scale_y,
                      MVCColor c, int align, int style, int shadow);
```
style: game font (0, 1 or 2). scale in screen pixels per font unit.

#### `mvc_flush_text`
```c
void mvc_flush_text(void);
```

## ARM64 instruction helpers, for reading struct offsets out of game code

#### `mvc_first_load_imm`
```c
int mvc_first_load_imm(void *func, int max_insns, int size);
```
returns the immediate of the first matching load in the first `max_insns` instructions of `func`, or -1. size: 1 = LDRB, 2 = LDRH, 4 = LDR W, 8 = LDR X.

#### `mvc_load_imm_from_base`
```c
int mvc_load_imm_from_base(void *func, int max_insns, void *base_addr, int size);
```
emulates ADRP/ADD in `func` and returns the immediate of the first load of `size` whose base register holds `base_addr`, or -1.

#### `MVCVec3`
```c
typedef struct { float x, y, z; } MVCVec3;
```
plugin SDK

## more input

#### `mvc_mouse_down`
```c
int mvc_mouse_down(int glfw_button);              // 0 left, 1 right, 2 middle
```

#### `mvc_pad_count`
```c
int mvc_pad_count(void);                           // connected gamepads
```

#### `mvc_pad_down_n`
```c
int mvc_pad_down_n(int pad, int glfw_button);      // pad = 0..mvc_pad_count()-1
```

#### `mvc_pad_axis`
```c
float mvc_pad_axis(int pad, int glfw_axis);        // -1..1 (triggers 0..1 from -1 at rest)
```

#### `mvc_pad_name`
```c
const char *mvc_pad_name(int pad);
```

#### `mvc_pad_rumble`
```c
int mvc_pad_rumble(int pad, float strength, int milliseconds);
```
rumble (DualSense / Xbox / MFi controllers through Apple's GameController framework).

#### `mvc_consume_key`
```c
void mvc_consume_key(int glfw_key, int consume);
```
while consumed, the game never sees this key (plugins still can).

#### `mvc_key_name`
```c
const char *mvc_key_name(int glfw_key);           // "G", "F9", "SPACE"...
```

#### `mvc_key_from_name`
```c
int mvc_key_from_name(const char *name);           // inverse; -1 if unknown
```

## console commands

#### `mvc_command_fn`
```c
typedef void (*mvc_command_fn)(int argc, const char **argv);
```
typed into the ~ console: "name arg1 arg2". argv[0] is the command name.

#### `mvc_console_command`
```c
void mvc_console_command(const char *name, const char *help, mvc_command_fn fn);
```

## game text (GXT keys) and FXT files

#### `mvc_text_set`
```c
void mvc_text_set(const char *key, const char *ascii);
```
adds or replaces a text key (max 7 characters) usable anywhere the game shows text: help messages, menus, mission text. '~' colour codes work.

#### `mvc_text_get`
```c
const uint16_t *mvc_text_get(const char *key);     // game or custom text
```

#### `mvc_text_load_fxt`
```c
int mvc_text_load_fxt(const char *path);           // "KEY text" per line; returns keys loaded
```

## Mods menu (a real page in Options > Mods)

#### `mvc_menu_changed`
```c
typedef void (*mvc_menu_changed)(void);
```
every function takes the ini file (relative to GameData), section and key the value is saved to; the loader writes it back when the player changes it. changed() is called after a change. register from MVCPluginInit. 10 settings fit on a page; more continue on "TITLE (2)", the next value of the MOD row.

#### `mvc_menu_page`
```c
int mvc_menu_page(const char *title);
```

#### `mvc_menu_toggle`
```c
int mvc_menu_toggle(int page, const char *label, int *value,
                            const char *ini, const char *section, const char *key, mvc_menu_changed changed);
```

#### `mvc_menu_choice`
```c
int mvc_menu_choice(int page, const char *label, const char *const *choices, int count, int *value,
                            const char *ini, const char *section, const char *key, mvc_menu_changed changed);
```

#### `mvc_menu_number`
```c
int mvc_menu_number(int page, const char *label, float *value, float min, float max, float step,
                            const char *format, const char *ini, const char *section, const char *key,
                            mvc_menu_changed changed);
```

#### `mvc_menu_keybind`
```c
int mvc_menu_keybind(int page, const char *label, int *glfw_key,
                             const char *ini, const char *section, const char *key, mvc_menu_changed changed);
```

#### `mvc_ini_get`
```c
int mvc_ini_get(const char *ini, const char *section, const char *key, char *out, size_t cap);
```
small ini helpers (paths relative to GameData unless absolute)

#### `mvc_ini_set`
```c
int mvc_ini_set(const char *ini, const char *section, const char *key, const char *value);
```

## save data (stored next to each save, never inside it)

#### `mvc_save_set`
```c
int mvc_save_set(const char *key, const void *data, size_t len);
```
write your data from an MVC_EV_SAVE_WRITING handler; read it back on MVC_EV_SAVE_LOADED. data is cleared on MVC_EV_NEW_SESSION without a load.

#### `mvc_save_get`
```c
int mvc_save_get(const char *key, void *out, size_t cap); // returns length, -1 if missing
```

#### `mvc_save_slot`
```c
int mvc_save_slot(void);                                    // 1..8, 0 if unknown
```

#### Constants
```c
enum { MVC_EV_SAVE_WRITING = 100 };  // fired before the game writes a save, arg = slot
```

## player

#### `mvc_player_ped`
```c
void *mvc_player_ped(void);
```

#### `mvc_player_vehicle`
```c
void *mvc_player_vehicle(void);
```

#### `mvc_player_pos`
```c
MVCVec3 mvc_player_pos(void);
```

#### `mvc_player_money`
```c
int mvc_player_money(void);
```

#### `mvc_player_set_money`
```c
void mvc_player_set_money(int dollars);
```

#### `mvc_player_health`
```c
float mvc_player_health(void);
```

#### `mvc_player_set_health`
```c
void mvc_player_set_health(float hp);
```

#### `mvc_player_armour`
```c
float mvc_player_armour(void);
```

#### `mvc_player_set_armour`
```c
void mvc_player_set_armour(float armour);
```

#### `mvc_player_set_wanted`
```c
void mvc_player_set_wanted(int stars);
```

#### `mvc_player_teleport`
```c
void mvc_player_teleport(MVCVec3 pos);
```

#### `mvc_player_give_weapon`
```c
void mvc_player_give_weapon(int weapon_type, int ammo);
```

## vehicles

#### `mvc_vehicle_model`
```c
int mvc_vehicle_model(void *vehicle);
```

#### `mvc_entity_pos`
```c
MVCVec3 mvc_entity_pos(void *entity);              // any ped, vehicle or object
```

#### `mvc_vehicle_spawn`
```c
void *mvc_vehicle_spawn(int model, MVCVec3 pos, float heading);
```
spawns a car, bike or boat (by model ID) at pos facing heading (degrees); NULL on failure.

#### `mvc_vehicle_fix`
```c
void mvc_vehicle_fix(void *vehicle);
```

## world

#### `mvc_world_time`
```c
void mvc_world_time(int *hours, int *minutes);
```

#### `mvc_world_set_time`
```c
void mvc_world_set_time(int hours, int minutes);
```

#### `mvc_world_set_weather`
```c
void mvc_world_set_weather(int weather);   // 0 sunny, 1 cloudy, 2 rainy, 3 foggy, 4 extra sunny, 5 hurricane
```

## 3D

#### `mvc_on_render3d`
```c
void mvc_on_render3d(mvc_callback cb);     // after the 3D scene, before 2D (use mvc_line3d here)
```

#### `mvc_world_to_screen`
```c
int mvc_world_to_screen(MVCVec3 world, float *sx, float *sy); // 0 if behind the camera
```

#### `mvc_line3d`
```c
void mvc_line3d(MVCVec3 a, MVCVec3 b, MVCColor ca, MVCColor cb);
```

#### `mvc_text3d`
```c
void mvc_text3d(MVCVec3 world, const char *ascii, float scale, MVCColor c); // call from HUD/frame callbacks
```

#### `mvc_corona`
```c
void mvc_corona(uint32_t id, MVCVec3 pos, float size, MVCColor c, float draw_distance); // call every tick
```

#### `mvc_marker`
```c
void mvc_marker(uint32_t id, MVCVec3 pos, float radius, MVCColor c);  // mission-style ring; every tick
```

## sprites

#### `mvc_sprite_load`
```c
int mvc_sprite_load(const char *path_or_txd);
```
loads a texture from a PNG (path relative to GameData) or from a .txd ("models/hud.txd:radar_gasstation"). returns a handle or 0.

#### `mvc_sprite_draw`
```c
void mvc_sprite_draw(int sprite, float left, float top, float right, float bottom, MVCColor tint);
```

## audio (through the game's OpenAL device)

#### `mvc_sound_play`
```c
int mvc_sound_play(const char *path, float volume, int loop);
```
plays a .wav or .mp3 (path relative to GameData). volume 0..1 is scaled by the game's SFX volume. returns a handle (0 on failure).

#### `mvc_sound_stop`
```c
void mvc_sound_stop(int handle);
```

