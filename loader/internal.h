// shared between the loader's own files, not part of the plugin api.
#pragma once
#include <stdint.h>
#include "../include/macvc.h"

// log.c
enum { LOG_GAME = 0, LOG_INFO = 1, LOG_WARN = 2, LOG_ERROR = 3 };
#define LOG_LINES 1024
#define LOG_LEN 200
void log_open(const char *game_dir);
void log_capture_stdio(void);
void log_push(const char *line, int kind);
void log_vprintf(int kind, void *caller, const char *fmt, __builtin_va_list ap);
// copies up to `max` lines ending `skip` lines before the newest into out/kinds
// (only lines containing `filter`, any case, when it isn't NULL/empty).
int log_copy(char out[][LOG_LEN], uint8_t *kinds, int max, int skip, int *total, const char *filter);
char *log_join(int last_n, const char *filter); // newest lines as one string; free() it
int log_error_count(void);
void log_dump_fd(int fd, int last_n); // async-signal-tolerant, used by the crash handler
extern int g_min_log_level;

// hook.c
typedef struct {
    uintptr_t target;      // patched address
    uintptr_t island;      // island start (0 for plain patches)
    uintptr_t island_end;
    uint32_t len;          // bytes patched at target
    uint8_t orig[64];
    int kind;              // 0 hook, 1 midhook, 2 patch
    int id;
    int active;
    char owner[64];        // plugin that installed it
    char what[96];         // symbol name of the target, if known
} PatchRec;
enum { PATCH_HOOK, PATCH_MIDHOOK, PATCH_WRITE };
int hook_count(void);
const PatchRec *hook_get(int i);
const PatchRec *hook_find_island(uintptr_t addr);
void hooks_set_startup(int in_startup);
void owner_name(void *addr, char *out, size_t n);
int hooks_release(const char *owner);      // hot reload: undo everything owner patched

// game.c
typedef struct { const char *name; uintptr_t addr; } GameSym; // name without the leading '_'
int game_symbols(const GameSym **out);   // every symbol in the game binary, sorted by address
const GameSym *game_sym_at(uintptr_t a); // nearest symbol at or below a
intptr_t game_slide(void);               // ASLR slide of the game binary
void perf_reset(void);
void perf_report(int verbose);
int game_init(void);                 // resolve symbols, install core hooks
int game_key_raw(int glfw_key);      // ignores the console-open block
int callbacks_release(const char *owner);
int event_handler_count(void);
void events_catch_up(int first_handler); // replays game init / new session / save loaded

// loader.c
typedef struct {
    int console_key, console_key2;
    int console_opacity;
    float console_scale;
    int log_level;
    int controller_fallback;
    int game_rumble;
    int streaming_mb;
    int play_movies;
    int texture_max;        // Mod Loader: largest image side built into a TXD (0 = any)
} LoaderConfig;
extern LoaderConfig g_cfg;
extern char g_game_dir[1024];
extern int g_safe_mode;
extern int g_plugins_loaded, g_plugins_failed, g_plugins_disabled;
void startup_finished(void);         // called on the first game tick

// loader.c (plugins)
void plugins_print(void);
void plugins_run_pending(void);
int plugin_loaded(const char *name);  // by name without .dylib, any case       // queued reload/unload/load, run at the start of a frame

// devtools.c
void devtools_register(void);        // sym/addr/mem/poke/watch/perf commands
void devtools_draw(void);            // the watch overlay

// console.c
int console_is_open(void);
void console_exec(const char *line);
void console_install(void *window);
void console_draw(void);
void console_register_builtins(void);
int commands_release(const char *owner);

// text.c / menu.c / save.c / sdk.c / haptics.m
int text_init(void);
void text_mod_clear(void);                       // Mod Loader strings from mods' .gxt files
void text_mod_language(const char *gxt_file);   // the language file the game loaded ("american.gxt")
void text_mod_set(const char *lang, const char *key, const uint16_t *w);
int menu_init(void);
void menu_frame(void);
int menu_release(const char *owner);
void menu_refresh(void);
int menu_overlay_open(void);
int menu_overlay_key(int key, int action);
int sdk_init(void);
enum { SDK_OFF_POS, SDK_OFF_MODEL, SDK_OFF_HEALTH, SDK_OFF_ARMOUR, SDK_OFF_MONEY, SDK_SIZE_PINFO, SDK_OFF_WMODEL };
int sdk_offset(int which);           // -1 if this build's offset wasn't found
void sdk_frame(void);
void save_clear(void);
int save_write(int slot);
int save_read(int slot);
void save_set_slot(int slot);
int haptics_play(const char *glfw_name, float strength, int ms); // haptics.m
void haptics_stop(void);
const char *game_joystick_name(void); // game.c: the controller the game reads

// modloader.c
void modloader_ini(const char *key, const char *value); // a [Mods] line of macVCLoader.ini
void modloader_init(void);
void modloader_menu(void);         // a Mods-menu page per mod, after the plugins' pages
// txdbuild.c: "name.txd/" folders of images -> a texture dictionary
int txd_is_image(const char *name);
int txd_build(const uint8_t *base, size_t blen, const char *const *images, int n, const char *out, const char *what);
// the texture names in a TXD (lowercase); returns how many (max `max`), -1 if it isn't one
int txd_texture_names(const uint8_t *d, size_t len, char (*names)[32], int max);
void limits_init(void);
void movies_init(void);              // movies.m: startup movies
void *loader_game_window(void);      // game.c: the GLFW window
int loader_skip_pressed(void);       // game.c: Enter/Space/Esc/click/pad A or Start held // limits.c: room for more map objects
void limits_report(void);
// modmerge.c: merges mod versions of a text file against the original; returns
// how many overlapping changes were dropped, or -1
int merge_text(const char *base, const char *const *versions, const int *prio, const char *const *names, int n,
               const char *out, const char *what);
int merge_col(const char *base, const char *const *versions, const int *prio, const char *const *names, int n,
              const char *out, const char *what); // collision archives, entry by entry

// cleo.c
void cleo_init(void);                                    // after modloader_init
void cleo_add_mod_script(const char *path, const char *mod); // a .cs/.js inside a Mod Loader mod
typedef struct Cleo Cleo;
Cleo *cleo_js_record(const char *path, const char *mod);      // a script record for a JS script
int cleo_js_exec(Cleo *c, const uint8_t *code, size_t len, int32_t *out, int nout); // one command; returns the condition
uint8_t *cleo_js_addr(Cleo *c, uint32_t a, uint32_t size, int write); // Windows/stand-in address -> real, or NULL
uint32_t cleo_js_ptr(void *p);                                // real pointer -> stand-in
// cleo_js.c
int js_start(const char *path, const char *mod);
void js_stop_all(void);
void js_tick(void);

// trust.c
enum { TRUST_QUARANTINED = 1, TRUST_UNSIGNED = 2, TRUST_BAD_SIGNATURE = 4, TRUST_ADHOC = 8 };
int plugin_trust(const char *path, char *out, size_t n); // flags; out gets "ad-hoc signed, quarantined"...

// crash.c
void crash_init(const char *game_dir);
void crash_set_loading(const char *plugin_file); // NULL when done
const char *crash_state_path(void);
