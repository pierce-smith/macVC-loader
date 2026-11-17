// Stand-ins for the loader and game functions the parsers link against.
// all weak, so a target that #includes a loader file keeps the real one.
// none of them are reached by the fuzz targets' code paths except logging.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../../loader/internal.h"

__attribute__((weak)) char g_game_dir[1024] = "/tmp";
__attribute__((weak)) LoaderConfig g_cfg;
__attribute__((weak)) int g_safe_mode;
__attribute__((weak)) void mvc_log(const char *f, ...) { (void)f; }
__attribute__((weak)) void mvc_log_warn(const char *f, ...) { (void)f; }
__attribute__((weak)) void mvc_log_error(const char *f, ...) { (void)f; }
__attribute__((weak)) void *mvc_sym(const char *n) { (void)n; return NULL; }
__attribute__((weak)) int mvc_hook(void *t, void *r, void **o) { (void)t; (void)r; (void)o; return -1; }
__attribute__((weak)) int mvc_hook_sym(const char *n, void *r, void **o) { (void)n; (void)r; (void)o; return -1; }
__attribute__((weak)) void mvc_console_command(const char *n, const char *h, mvc_command_fn f) { (void)n; (void)h; (void)f; }
__attribute__((weak)) void mvc_on_event(mvc_event_fn f) { (void)f; }
__attribute__((weak)) void mvc_on_hud(mvc_callback f) { (void)f; }
__attribute__((weak)) void owner_name(void *a, char *o, size_t n) { (void)a; if (n) o[0] = 0; }
__attribute__((weak)) const char *game_joystick_name(void) { return ""; }
__attribute__((weak)) int sdk_offset(int w) { (void)w; return -1; }
__attribute__((weak)) int plugin_loaded(const char *n) { (void)n; return 0; }
__attribute__((weak)) float mvc_screen_w(void) { return 640; }
__attribute__((weak)) float mvc_screen_h(void) { return 448; }
__attribute__((weak)) int mvc_key_down(int k) { (void)k; return 0; }
__attribute__((weak)) int mvc_mouse_down(int k) { (void)k; return 0; }
__attribute__((weak)) const char *mvc_key_name(int k) { (void)k; return "?"; }
__attribute__((weak)) int mvc_first_load_imm(void *f, int m, int s) { (void)f; (void)m; (void)s; return -1; }
__attribute__((weak)) void *mvc_player_ped(void) { return NULL; }
__attribute__((weak)) MVCVec3 mvc_entity_pos(void *e) { (void)e; return (MVCVec3){0, 0, 0}; }
__attribute__((weak)) int mvc_sound_play(const char *p, float v, int l) { (void)p; (void)v; (void)l; return 0; }
__attribute__((weak)) void mvc_sound_stop(int h) { (void)h; }
__attribute__((weak)) void mvc_text(float x, float y, const char *s, float a, float b, MVCColor c, int al, int st, int sh) { (void)x; (void)y; (void)s; (void)a; (void)b; (void)c; (void)al; (void)st; (void)sh; }
__attribute__((weak)) int js_start(const char *p, const char *m) { (void)p; (void)m; return -1; }
__attribute__((weak)) void js_stop_all(void) {}
__attribute__((weak)) void js_tick(void) {}
__attribute__((weak)) int haptics_play(const char *n, float s, int ms) { (void)n; (void)s; (void)ms; return 0; }
__attribute__((weak)) void cleo_add_mod_script(const char *p, const char *m) { (void)p; (void)m; }
__attribute__((weak)) uint8_t *cleo_js_addr(Cleo *c, uint32_t a, uint32_t s, int w) { (void)c; (void)a; (void)s; (void)w; return NULL; }
__attribute__((weak)) int cleo_js_exec(Cleo *c, const uint8_t *code, size_t len, int32_t *out, int n) { (void)c; (void)code; (void)len; (void)out; (void)n; return 0; }
__attribute__((weak)) Cleo *cleo_js_record(const char *p, const char *m) { (void)p; (void)m; return NULL; }
__attribute__((weak)) uint32_t mvc_time_ms(void) { return 0; }
