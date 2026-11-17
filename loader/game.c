// the game side of the loader: symbol lookup, drawing, input, plugin callbacks, game
// events and the hooks behind them.
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "internal.h"

// symbols
// every symbol defined in the game binary (exported and local), sorted by
// address, built on first use. names are without the Mach-O leading '_'.
static GameSym *g_syms;
static int g_nsyms = -1;
static intptr_t g_slide;

static int sym_addr_cmp(const void *a, const void *b) {
    uintptr_t x = ((const GameSym *)a)->addr, y = ((const GameSym *)b)->addr;
    return x < y ? -1 : x > y;
}

int game_symbols(const GameSym **out) {
    if (g_nsyms < 0) {
        g_nsyms = 0;
        const struct mach_header_64 *mh = NULL;
        for (uint32_t i = 0; i < _dyld_image_count(); i++) {
            const struct mach_header_64 *h = (const void *)_dyld_get_image_header(i);
            if (h && h->filetype == MH_EXECUTE) { mh = h; g_slide = _dyld_get_image_vmaddr_slide(i); break; }
        }
        if (!mh) goto done;
        const struct load_command *lc = (const void *)(mh + 1);
        const struct symtab_command *st = NULL;
        const struct segment_command_64 *le = NULL;
        for (uint32_t i = 0; i < mh->ncmds; i++) {
            if (lc->cmd == LC_SYMTAB) st = (const void *)lc;
            if (lc->cmd == LC_SEGMENT_64 && !strcmp(((const struct segment_command_64 *)lc)->segname, "__LINKEDIT"))
                le = (const void *)lc;
            lc = (const void *)((const char *)lc + lc->cmdsize);
        }
        if (!st || !le) goto done;
        uintptr_t base = le->vmaddr + g_slide - le->fileoff;
        const struct nlist_64 *syms = (const void *)(base + st->symoff);
        const char *strs = (const char *)(base + st->stroff);
        g_syms = malloc(sizeof *g_syms * st->nsyms);
        if (!g_syms) goto done;
        for (uint32_t i = 0; i < st->nsyms; i++) {
            if ((syms[i].n_type & N_STAB) || (syms[i].n_type & N_TYPE) != N_SECT) continue;
            const char *s = strs + syms[i].n_un.n_strx;
            if (s[0] == '_') s++;
            if (!s[0]) continue;
            g_syms[g_nsyms].name = s;
            g_syms[g_nsyms].addr = (uintptr_t)(syms[i].n_value + g_slide);
            g_nsyms++;
        }
        qsort(g_syms, (size_t)g_nsyms, sizeof *g_syms, sym_addr_cmp);
    }
done:
    *out = g_syms;
    return g_nsyms;
}

intptr_t game_slide(void) { const GameSym *s; game_symbols(&s); return g_slide; }

const GameSym *game_sym_at(uintptr_t a) {
    const GameSym *s;
    int n = game_symbols(&s), lo = 0, hi = n - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (s[mid].addr <= a) { best = mid; lo = mid + 1; } else hi = mid - 1;
    }
    return best >= 0 ? &s[best] : NULL;
}

static void *symtab_lookup(const char *want) {
    const GameSym *s;
    int n = game_symbols(&s);
    for (int i = 0; i < n; i++)
        if (!strcmp(s[i].name, want)) return (void *)s[i].addr;
    return NULL;
}

void *mvc_sym(const char *name) {
    if (!name) return NULL;
    if (name[0] == '_' && name[1] == '_') name++; // accept nm-style names too
    void *p = dlsym(RTLD_MAIN_ONLY, name);
    return p ? p : symtab_lookup(name);
}

int mvc_api_version(void) { return MVC_API_VERSION; }
int mvc_safe_mode(void) { return g_safe_mode; }
const char *mvc_game_dir(void) { return g_game_dir; }

// game API
typedef struct { float left, bottom, right, top; } CRect;
static void (*f_SetScale)(float, float);
static void (*f_SetColor)(MVCColor);
static void (*f_SetDropColor)(MVCColor);
static void (*f_SetFontStyle)(short);
static void (*f_SetDropShadowPosition)(short);
static void (*f_SetJustifyOff)(void);
static void (*f_SetCentreOff)(void), (*f_SetCentreOn)(void), (*f_SetCentreSize)(float);
static void (*f_SetRightJustifyOn)(void), (*f_SetRightJustifyOff)(void), (*f_SetRightJustifyWrap)(float);
static void (*f_SetBackgroundOff)(void), (*f_SetPropOn)(void), (*f_SetWrapx)(float);
static void (*f_PrintString)(float, float, uint16_t *);
static void (*f_DrawFonts)(void);
static void (*f_DrawRect)(const CRect *, const MVCColor *);
static void *(*f_GetPad)(int);
static void (*f_PadClear)(void *, _Bool);
static void *(*f_FindPlayerVehicle)(void);
static _Bool (*f_IsPlayerOnAMission)(void);
static uint8_t *p_FrontEnd;
static int g_menumap_off = -1;
static uint8_t *p_RsGlobal, *p_WantsHud, *p_TheCamera, *p_ReplayMode, *p_UserPause, *p_Players, *p_PlayerInFocus;
static char *p_SelJoyName;
static uint32_t *p_TimeMs;
static int g_cam_widescreen_off = 0x75, g_wb_off = -1, g_playerinfo_size = 0x1a0, g_joy_off = -1;

static int (*glfwGetKey_)(void *, int);
static int (*glfwGetMouseButton_)(void *, int);
static int (*glfwJoystickPresent_)(int);
static int (*glfwJoystickIsGamepad_)(int);
static int (*glfwGetGamepadState_)(int, void *);
static const char *(*glfwGetJoystickName_)(int);
static const char *(*glfwGetJoystickGUID_)(int);
static int (*glfwUpdateGamepadMappings_)(const char *);

float mvc_screen_w(void) { return p_RsGlobal ? (float)*(int32_t *)(p_RsGlobal + 8) : 1280.f; }
float mvc_screen_h(void) { return p_RsGlobal ? (float)*(int32_t *)(p_RsGlobal + 12) : 720.f; }
uint32_t mvc_time_ms(void) { return p_TimeMs ? *p_TimeMs : 0; }

int mvc_menu_map_active(void) { return p_FrontEnd && g_menumap_off >= 0 && p_FrontEnd[g_menumap_off] == 1; }

int mvc_hud_visible(void) {
    if (!p_WantsHud || !p_TheCamera) return 0;
    if (p_ReplayMode && *p_ReplayMode == 1) return 0;
    return *p_WantsHud == 1 && !(p_TheCamera[g_cam_widescreen_off] & 1);
}

void mvc_rect(float l, float t, float r, float b, MVCColor c) {
    if (!f_DrawRect) return;
    CRect rc = {l, b, r, t};
    f_DrawRect(&rc, &c);
}

void mvc_text(float x, float y, const char *s, float sx, float sy, MVCColor c, int align, int style, int shadow) {
    if (!f_PrintString || !s) return;
    static uint16_t buf[512];
    int n = 0;
    for (; s[n] && n < 511; n++) {
        unsigned char ch = (unsigned char)s[n];
        buf[n] = (ch == '~' || ch == '_' || ch < 32 || ch > 126) ? '-' : ch;
    }
    buf[n] = 0;
    float w = mvc_screen_w();
    f_SetBackgroundOff();
    f_SetScale(sx, sy);
    f_SetPropOn();
    f_SetFontStyle((short)style);
    f_SetJustifyOff();
    f_SetCentreOff();
    f_SetRightJustifyOff();
    f_SetWrapx(w * 2);
    if (align == MVC_ALIGN_CENTER) { f_SetCentreOn(); f_SetCentreSize(w * 2); }
    if (align == MVC_ALIGN_RIGHT) { f_SetRightJustifyOn(); f_SetRightJustifyWrap(0); }
    f_SetDropShadowPosition((short)shadow);
    f_SetDropColor((MVCColor){0, 0, 0, c.a});
    f_SetColor(c);
    f_PrintString(x, y, buf);
    f_SetRightJustifyOff();
    f_SetCentreOff();
    f_SetDropShadowPosition(0);
}

void mvc_flush_text(void) { if (f_DrawFonts) f_DrawFonts(); }

// input
#define NKEYS 350
#define NPADS 8
static uint8_t k_watch[NKEYS], k_cur[NKEYS], k_prev[NKEYS];
static struct { int jid; unsigned char cur[15], prev[15]; float axes[6]; } g_pads[NPADS];
static int g_npads;

static void *game_window(void) {
    if (!p_RsGlobal) return NULL;
    void **ps = *(void ***)(p_RsGlobal + 0x20);
    return ps ? ps[0] : NULL;
}

void *loader_game_window(void) { return game_window(); }

// Enter, Space, Escape, keypad Enter, a left click, or A / Start on a gamepad (raw GLFW state)
int loader_skip_pressed(void) {
    void *w = game_window();
    int any = 0;
    if (w && glfwGetKey_) any = glfwGetKey_(w, 257) == 1 || glfwGetKey_(w, 32) == 1 || glfwGetKey_(w, 256) == 1 || glfwGetKey_(w, 335) == 1;
    if (w && glfwGetMouseButton_ && glfwGetMouseButton_(w, 0) == 1) any = 1;
    if (glfwGetGamepadState_ && glfwJoystickPresent_ && glfwJoystickIsGamepad_)
        for (int j = 0; j < 16 && !any; j++) {
            struct { unsigned char buttons[15]; float axes[6]; } st;
            if (glfwJoystickPresent_(j) && glfwJoystickIsGamepad_(j) && glfwGetGamepadState_(j, &st)) any = st.buttons[0] || st.buttons[7];
        }
    return any;
}

static void poll_input(void) {
    void *win = game_window();
    for (int k = 0; k < NKEYS; k++) {
        if (!k_watch[k]) continue;
        k_prev[k] = k_cur[k];
        k_cur[k] = (win && glfwGetKey_) ? (glfwGetKey_(win, k) != 0) : 0;
    }
    int n = 0;
    if (glfwJoystickIsGamepad_ && glfwGetGamepadState_) {
        for (int j = 0; j < 16 && n < NPADS; j++) {
            if (!glfwJoystickIsGamepad_(j)) continue;
            struct { unsigned char buttons[15]; float axes[6]; } st;
            if (!glfwGetGamepadState_(j, &st)) continue;
            if (g_pads[n].jid != j) memset(g_pads[n].cur, 0, sizeof g_pads[n].cur);
            g_pads[n].jid = j;
            memcpy(g_pads[n].prev, g_pads[n].cur, 15);
            memcpy(g_pads[n].cur, st.buttons, 15);
            memcpy(g_pads[n].axes, st.axes, sizeof st.axes);
            n++;
        }
    }
    for (int i = n; i < g_npads; i++) memset(&g_pads[i], 0, sizeof g_pads[i]);
    g_npads = n;
}

int game_key_raw(int k) {
    if (k <= 0 || k >= NKEYS) return 0;
    k_watch[k] = 1;
    return k_cur[k];
}
static int key_pressed_raw(int k) {
    if (k <= 0 || k >= NKEYS) return 0;
    k_watch[k] = 1;
    return k_cur[k] && !k_prev[k];
}
int mvc_key_down(int k) { int r = game_key_raw(k); return console_is_open() ? 0 : r; }
int mvc_key_pressed(int k) { int r = key_pressed_raw(k); return console_is_open() ? 0 : r; }
int mvc_pad_down(int b) { return mvc_pad_down_n(0, b); }
int mvc_pad_pressed(int b) {
    return (!console_is_open() && g_npads && b >= 0 && b < 15) ? (g_pads[0].cur[b] && !g_pads[0].prev[b]) : 0;
}
int mvc_pad_count(void) { return g_npads; }
int mvc_pad_down_n(int p, int b) {
    return (!console_is_open() && p >= 0 && p < g_npads && b >= 0 && b < 15) ? g_pads[p].cur[b] : 0;
}
float mvc_pad_axis(int p, int a) {
    return (!console_is_open() && p >= 0 && p < g_npads && a >= 0 && a < 6) ? g_pads[p].axes[a] : 0.f;
}
const char *mvc_pad_name(int p) {
    return (p >= 0 && p < g_npads && glfwGetJoystickName_) ? glfwGetJoystickName_(g_pads[p].jid) : NULL;
}
int mvc_mouse_down(int b) {
    void *win = game_window();
    return (!console_is_open() && win && glfwGetMouseButton_) ? glfwGetMouseButton_(win, b) == 1 : 0;
}
int mvc_console_open(void) { return console_is_open(); }

static const struct { int key; const char *name; } g_keynames[] = {
    {32, "SPACE"}, {39, "'"}, {44, ","}, {45, "-"}, {46, "."}, {47, "/"}, {59, ";"}, {61, "="}, {91, "["},
    {92, "\\"}, {93, "]"}, {96, "GRAVE"}, {161, "SECTION"}, {256, "ESC"}, {257, "ENTER"}, {258, "TAB"},
    {259, "BACKSPACE"}, {260, "INSERT"}, {261, "DELETE"}, {262, "RIGHT"}, {263, "LEFT"}, {264, "DOWN"},
    {265, "UP"}, {266, "PAGEUP"}, {267, "PAGEDOWN"}, {268, "HOME"}, {269, "END"}, {280, "CAPSLOCK"},
    {340, "LSHIFT"}, {341, "LCTRL"}, {342, "LALT"}, {343, "LCMD"}, {344, "RSHIFT"}, {345, "RCTRL"},
    {346, "RALT"}, {347, "RCMD"}, {335, "NUMENTER"}, {0, NULL}};

const char *mvc_key_name(int k) {
    static char b[16];
    if ((k >= 'A' && k <= 'Z') || (k >= '0' && k <= '9')) { b[0] = (char)k; b[1] = 0; return b; }
    if (k >= 290 && k <= 314) { snprintf(b, sizeof b, "F%d", k - 289); return b; }
    if (k >= 320 && k <= 329) { snprintf(b, sizeof b, "NUM%d", k - 320); return b; }
    for (int i = 0; g_keynames[i].name; i++) if (g_keynames[i].key == k) return g_keynames[i].name;
    snprintf(b, sizeof b, "KEY%d", k);
    return b;
}

int mvc_key_from_name(const char *s) {
    if (!s || !s[0]) return -1;
    if (!s[1] && ((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= '0' && s[0] <= '9')))
        return s[0] >= 'a' ? s[0] - 32 : s[0];
    if ((s[0] == 'F' || s[0] == 'f') && s[1] >= '0' && s[1] <= '9') { int n = atoi(s + 1); if (n >= 1 && n <= 25) return 289 + n; }
    if (!strncasecmp(s, "NUM", 3) && s[3] >= '0' && s[3] <= '9' && !s[4]) return 320 + s[3] - '0';
    if (!strncasecmp(s, "KEY", 3)) return atoi(s + 3);
    for (int i = 0; g_keynames[i].name; i++) if (!strcasecmp(g_keynames[i].name, s)) return g_keynames[i].key;
    return -1;
}

// callbacks & events
// every callback call is timed for the `perf` console command.
#define MAXCB 64
typedef struct { const char *kind; void *fn[MAXCB]; uint64_t ns[MAXCB], max[MAXCB]; uint32_t calls[MAXCB]; int n; } CbList;
static CbList L_tick = {.kind = "tick"}, L_frame = {.kind = "frame"}, L_hud = {.kind = "hud"}, L_radar = {.kind = "radar"},
              L_radar_under = {.kind = "radar-under"}, L_3d = {.kind = "render3d"}, L_event = {.kind = "event"};
static CbList *const g_lists[] = {&L_tick, &L_frame, &L_hud, &L_radar, &L_radar_under, &L_3d, &L_event};
static void cb_add(CbList *l, void *fn) { if (fn && l->n < MAXCB) l->fn[l->n++] = fn; }
static inline uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static inline void cb_account(CbList *l, int i, uint64_t t0) {
    uint64_t dt = now_ns() - t0;
    l->ns[i] += dt;
    l->calls[i]++;
    if (dt > l->max[i]) l->max[i] = dt;
}
static void cb_run(CbList *l) {
    for (int i = 0; i < l->n; i++) { uint64_t t0 = now_ns(); ((mvc_callback)l->fn[i])(); cb_account(l, i, t0); }
}
static void ev_run(int ev, void *arg) {
    for (int i = 0; i < L_event.n; i++) { uint64_t t0 = now_ns(); ((mvc_event_fn)L_event.fn[i])(ev, arg); cb_account(&L_event, i, t0); }
}
void mvc_on_tick(mvc_callback cb) { cb_add(&L_tick, (void *)cb); }
void mvc_on_frame(mvc_callback cb) { cb_add(&L_frame, (void *)cb); }
void mvc_on_hud(mvc_callback cb) { cb_add(&L_hud, (void *)cb); }
void mvc_on_radar(mvc_callback cb) { cb_add(&L_radar, (void *)cb); }
void mvc_on_radar_under(mvc_callback cb) { cb_add(&L_radar_under, (void *)cb); }
void mvc_on_render3d(mvc_callback cb) { cb_add(&L_3d, (void *)cb); }
void mvc_on_event(mvc_event_fn fn) { cb_add(&L_event, (void *)fn); }

// frame timing and the loader's own per-frame work
static uint64_t g_perf_t0, g_perf_last_frame, g_perf_frame_max, g_perf_loader_ns;
static uint32_t g_perf_frames;

void perf_reset(void) {
    for (size_t k = 0; k < sizeof g_lists / sizeof *g_lists; k++) {
        CbList *l = g_lists[k];
        memset(l->ns, 0, sizeof l->ns); memset(l->max, 0, sizeof l->max); memset(l->calls, 0, sizeof l->calls);
    }
    g_perf_t0 = now_ns();
    g_perf_last_frame = 0;
    g_perf_frame_max = g_perf_loader_ns = 0;
    g_perf_frames = 0;
}

static void perf_frame(void) {
    uint64_t t = now_ns();
    if (!g_perf_t0) g_perf_t0 = t;
    if (g_perf_last_frame && t - g_perf_last_frame > g_perf_frame_max) g_perf_frame_max = t - g_perf_last_frame;
    g_perf_last_frame = t;
    g_perf_frames++;
}

void perf_report(int verbose) {
    double secs = (now_ns() - g_perf_t0) / 1e9;
    uint32_t fr = g_perf_frames ? g_perf_frames : 1;
    mvc_log("[perf] %u frames in %.1fs (%.1f fps, slowest frame %.1f ms) - since start or `perf reset`",
            g_perf_frames, secs, secs > 0 ? g_perf_frames / secs : 0, g_perf_frame_max / 1e6);
    mvc_log("[perf] loader itself: %.3f ms/frame", g_perf_loader_ns / 1e6 / fr);
    // per plugin totals
    struct { char owner[64]; uint64_t ns; uint64_t max; } tot[64];
    int nt = 0;
    for (size_t k = 0; k < sizeof g_lists / sizeof *g_lists; k++) {
        CbList *l = g_lists[k];
        for (int i = 0; i < l->n; i++) {
            char o[64];
            owner_name(l->fn[i], o, sizeof o);
            if (!o[0]) snprintf(o, sizeof o, "loader");
            int j = 0;
            while (j < nt && strcmp(tot[j].owner, o)) j++;
            if (j == nt) { if (nt == 64) continue; snprintf(tot[nt].owner, 64, "%s", o); tot[nt].ns = tot[nt].max = 0; nt++; }
            tot[j].ns += l->ns[i];
            if (l->max[i] > tot[j].max) tot[j].max = l->max[i];
        }
    }
    if (!nt) mvc_log("[perf] no plugin callbacks registered");
    for (int j = 0; j < nt; j++)
        mvc_log("[perf]   %-20s %.3f ms/frame   (slowest single call %.2f ms)", tot[j].owner, tot[j].ns / 1e6 / fr, tot[j].max / 1e6);
    if (!verbose) { mvc_log("[perf] `perf all` lists every callback, `perf reset` starts over"); return; }
    for (size_t k = 0; k < sizeof g_lists / sizeof *g_lists; k++) {
        CbList *l = g_lists[k];
        for (int i = 0; i < l->n; i++) {
            char o[64], fn[96];
            owner_name(l->fn[i], o, sizeof o);
            Dl_info di;
            if (dladdr(l->fn[i], &di) && di.dli_sname && di.dli_saddr == l->fn[i]) snprintf(fn, sizeof fn, "%s", di.dli_sname);
            else snprintf(fn, sizeof fn, "%p", l->fn[i]);
            mvc_log("[perf]     %-12s %-16s %-28s %.3f ms/frame, %u calls, max %.2f ms", l->kind, o[0] ? o : "loader", fn,
                    l->ns[i] / 1e6 / fr, l->calls[i], l->max[i] / 1e6);
        }
    }
}

const char *mvc_event_name(int e) {
    static const char *n[] = {"?", "game init", "new session", "save loaded", "save written", "shutdown",
                              "paused", "resumed", "vehicle entered", "vehicle exited", "vehicle destroyed",
                              "wasted", "busted", "mission started", "mission ended"};
    if (e == MVC_EV_SAVE_WRITING) return "save writing";
    return (e > 0 && e <= MVC_EV_MISSION_ENDED) ? n[e] : n[0];
}

static void fire(int ev, void *arg) {
    if (ev != MVC_EV_VEHICLE_DESTROYED) mvc_log("[Loader] event: %s", mvc_event_name(ev));
    ev_run(ev, arg);
}

// hot reload
// what has happened so far, so a plugin loaded mid-game can be caught up
static int g_seen_init, g_seen_session;

int callbacks_release(const char *owner) {
    int n = 0;
    for (size_t k = 0; k < sizeof g_lists / sizeof *g_lists; k++) {
        CbList *l = g_lists[k];
        int w = 0;
        for (int i = 0; i < l->n; i++) {
            char o[64];
            owner_name(l->fn[i], o, sizeof o);
            if (!strcmp(o, owner)) { n++; continue; }
            l->fn[w] = l->fn[i]; l->ns[w] = l->ns[i]; l->max[w] = l->max[i]; l->calls[w] = l->calls[i];
            w++;
        }
        l->n = w;
    }
    return n;
}

int event_handler_count(void) { return L_event.n; }

// sends the events a newly loaded plugin missed to its handlers only
void events_catch_up(int first_handler) {
    int evs[3], ne = 0;
    if (g_seen_init) evs[ne++] = MVC_EV_GAME_INIT;
    if (g_seen_session) evs[ne++] = MVC_EV_NEW_SESSION;
    if (g_seen_session) evs[ne++] = MVC_EV_SAVE_LOADED; // reads back what MVCPluginUnload stashed
    for (int e = 0; e < ne; e++) {
        mvc_log("[Loader]   catching up: %s", mvc_event_name(evs[e]));
        for (int i = first_handler; i < L_event.n; i++) ((mvc_event_fn)L_event.fn[i])(evs[e], NULL);
    }
}

// controller watch
// logs which controller the game uses; if it has none while a gamepad is
// connected (e.g. a different controller than last time), hands it that one.
static int g_last_joy = -2, g_last_present_mask = -1;

static void controller_watch(void) {
    if (!glfwJoystickPresent_ || !glfwGetJoystickName_) return;
    int mask = 0;
    for (int j = 0; j < 16; j++) if (glfwJoystickPresent_(j)) mask |= 1 << j;
    int changed = mask != g_last_present_mask;
    if (changed) {
        for (int j = 0; j < 16; j++) {
            int now = mask >> j & 1, before = g_last_present_mask >= 0 && (g_last_present_mask >> j & 1);
            if (now && !before) mvc_log("[Loader] controller connected: %s (slot %d%s)", glfwGetJoystickName_(j), j,
                                        glfwJoystickIsGamepad_ && glfwJoystickIsGamepad_(j) ? ", gamepad" : "");
            if (!now && before) mvc_log("[Loader] controller disconnected (slot %d)", j);
        }
        g_last_present_mask = mask;
    }
    if (g_joy_off < 0) return;
    uint8_t *ps = p_RsGlobal ? *(uint8_t **)(p_RsGlobal + 0x20) : NULL;
    if (!ps) return;
    int8_t joy = (int8_t)ps[g_joy_off];
    if (joy < 0 && g_cfg.controller_fallback) {
        for (int j = 0; j < 16; j++) {
            if (glfwJoystickPresent_(j) && glfwJoystickIsGamepad_ && glfwJoystickIsGamepad_(j)) {
                ps[g_joy_off] = (uint8_t)j;
                if (p_SelJoyName) snprintf(p_SelJoyName, 64, "%s", glfwGetJoystickName_(j));
                mvc_log("[Loader] the game had no controller - switched it to %s (slot %d)", glfwGetJoystickName_(j), j);
                joy = (int8_t)j;
                break;
            }
        }
    }
    // keep the game on the controller its own setting names (macVC.ini JoystickName).
    // macOS can list controllers in a different order each launch: with controller
    // compatibility mode the raw DualSense (which then sends nothing) can come
    // before the working "GamePad-1", and the game just takes the first one.
    if ((changed || joy != g_last_joy) && p_SelJoyName && p_SelJoyName[0]) {
        const char *cur = joy >= 0 ? glfwGetJoystickName_(joy) : NULL;
        if (!cur || strcmp(cur, p_SelJoyName)) {
            for (int j = 0; j < 16; j++) {
                const char *n = glfwJoystickPresent_(j) ? glfwGetJoystickName_(j) : NULL;
                if (n && !strcmp(n, p_SelJoyName) && (!glfwJoystickIsGamepad_ || glfwJoystickIsGamepad_(j))) {
                    ps[g_joy_off] = (uint8_t)j;
                    mvc_log("[Loader] the game picked %s, but its controller setting is %s - switched to %s (slot %d)",
                            cur ? cur : "nothing", p_SelJoyName, n, j);
                    joy = (int8_t)j;
                    break;
                }
            }
        }
    }
    if (joy != g_last_joy) {
        if (joy >= 0) mvc_log("[Loader] game controller in use: %s (slot %d)", glfwGetJoystickName_(joy), joy);
        else mvc_log("[Loader] game controller in use: none");
        g_last_joy = joy;
    }
}

// game rumble
// CPad::StartShake writes a duration (ms) and strength into the pad, but this
// PC build never counts the duration down or sends it anywhere: there is no
// rumble at all, and since a shake only replaces a longer one, a single shake
// would block most later ones. each frame this counts it down (as the PS2
// version did) and plays it on the real controller.
static int g_shake_dur_off = -1, g_shake_str_off = -1;

static void find_shake_offsets(void) {
    uint32_t *c = mvc_sym("_ZN4CPad10StartShakeEsh");
    if (!c) return;
    for (int i = 0; i < 32; i++) {
        if ((c[i] & 0xFFC00000) == 0x79000000 && g_shake_dur_off < 0) g_shake_dur_off = (int)((c[i] >> 10) & 0xFFF) * 2; // STRH
        else if ((c[i] & 0xFFC00000) == 0x39000000 && g_shake_dur_off >= 0) { g_shake_str_off = (int)((c[i] >> 10) & 0xFFF); break; } // STRB
        if (c[i] == 0xD65F03C0 && g_shake_dur_off >= 0) break; // RET
    }
    if (g_shake_str_off < 0) { g_shake_dur_off = -1; mvc_log_warn("[Loader] game rumble unavailable: CPad shake fields not found"); }
}

const char *game_joystick_name(void) {
    uint8_t *ps = p_RsGlobal && g_joy_off >= 0 ? *(uint8_t **)(p_RsGlobal + 0x20) : NULL;
    int joy = ps ? (int8_t)ps[g_joy_off] : -1;
    return joy >= 0 && glfwGetJoystickName_ ? glfwGetJoystickName_(joy) : NULL;
}

static void rumble_update(int paused) {
    static uint64_t last_t;
    static int playing, last_dur, last_str;
    uint64_t t = now_ns();
    int dt = last_t ? (int)((t - last_t) / 1000000) : 0;
    last_t = t;
    if (!g_cfg.game_rumble || g_shake_dur_off < 0 || !f_GetPad) return;
    uint8_t *pad = f_GetPad(0);
    if (!pad) return;
    int16_t *dur = (int16_t *)(pad + g_shake_dur_off);
    int str = pad[g_shake_str_off];
    if (paused || console_is_open()) { // hold the shake while paused, resume it after
        if (playing) { haptics_stop(); playing = 0; last_dur = 0; }
        return;
    }
    if (dt > 100) dt = 100;
    if (*dur > 0 && str > 0) {
        if (!playing || *dur > last_dur || str != last_str) {
            haptics_play(game_joystick_name(), 0.25f + 0.75f * (float)str / 255.f, *dur);
            playing = 1;
        }
        int left = *dur - dt;
        *dur = (int16_t)(left > 0 ? left : 0);
        last_dur = *dur;
        last_str = str;
        if (!*dur) playing = 0; // the pattern ends by itself
    } else if (playing) {
        haptics_stop(); // StopShaking / Clear
        playing = 0;
    }
}

// controller mapping fixes
// macVC's gamecontrollerdb.txt maps the DualSense's right stick Y to axis 3,
// which is also L2. a released trigger reads -1, so the game sees the right
// stick held up: the camera keeps looking up. GLFW sorts axes by HID usage
// (X, Y, Z, Rx, Ry, Rz = left X, left Y, right X, L2, R2, right Y), so right
// stick Y is axis 5. re-applied with the controller's real GUID after the
// game loads its database and whenever a controller connects.
static const struct { const char *vidpid; const char *name; const char *map; } g_padfix[] = {
    {"4c050000e60c", "Sony DualSense", "a:b1,b:b2,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12,"
     "leftshoulder:b4,leftstick:b10,lefttrigger:a3,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b11,"
     "righttrigger:a4,rightx:a2,righty:a5,start:b9,x:b0,y:b3,platform:Mac OS X,"},
    {"4c050000f20d", "Sony DualSense Edge", "a:b1,b:b2,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12,"
     "leftshoulder:b4,leftstick:b10,lefttrigger:a3,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b11,"
     "righttrigger:a4,rightx:a2,righty:a5,start:b9,x:b0,y:b3,platform:Mac OS X,"},
};
static char g_fixed_guid[16][40];

static void fix_pad_mappings(void) {
    if (!glfwJoystickPresent_ || !glfwGetJoystickGUID_ || !glfwUpdateGamepadMappings_) return;
    for (int j = 0; j < 16; j++) {
        if (!glfwJoystickPresent_(j)) continue;
        const char *guid = glfwGetJoystickGUID_(j);
        if (!guid || strlen(guid) != 32) continue;
        for (size_t k = 0; k < sizeof g_padfix / sizeof *g_padfix; k++) {
            // GUID: bus(4) 0000 vendor(4) 0000 product(4) 0000 version(4) 0000
            char vp[13];
            memcpy(vp, guid + 8, 4); memcpy(vp + 4, guid + 12, 4); memcpy(vp + 8, guid + 16, 4); vp[12] = 0;
            if (strcasecmp(vp, g_padfix[k].vidpid)) continue;
            char line[512];
            snprintf(line, sizeof line, "%s,%s,%s", guid, g_padfix[k].name, g_padfix[k].map);
            if (glfwUpdateGamepadMappings_(line) && strcmp(g_fixed_guid[j], guid)) {
                snprintf(g_fixed_guid[j], sizeof g_fixed_guid[j], "%s", guid);
                mvc_log("[Loader] fixed the %s mapping in macVC's gamecontrollerdb.txt (right stick up/down was read from L2)",
                        g_padfix[k].name);
            }
        }
    }
}

// hooks
static void (*o_HudDraw)(void), (*o_DrawBlips)(void), (*o_DrawAfterFade)(void), (*o_RenderEffects)(void);
static void (*o_UpdatePads)(void), (*o_GameProcess)(void), (*o_InitWhenRestarting)(void);
static _Bool (*o_GameInit)(const char *), (*o_GenericLoad)(void), (*o_GenericSave)(int);
static _Bool (*o_ShutDown)(void);
static void (*o_VehicleDtor)(void *);
static int (*o_OpenFile)(const char *, const char *);
static void (*o_InitJoys)(void);
static void (*o_JoysChange)(int, int);
static void h_InitJoys(void) { o_InitJoys(); fix_pad_mappings(); }
static void h_JoysChange(int jid, int event) { if (event == 0x40001 /* GLFW_CONNECTED */) fix_pad_mappings(); o_JoysChange(jid, event); }

static void h_HudDraw(void) {
    o_HudDraw();
    if (mvc_hud_visible())
        cb_run(&L_hud);
}

static void h_DrawBlips(void) {
    cb_run(&L_radar_under);
    o_DrawBlips();
    cb_run(&L_radar);
}

static void h_RenderEffects(void) {
    o_RenderEffects();
    cb_run(&L_3d);
}

static void h_DrawAfterFade(void) {
    o_DrawAfterFade();
    perf_frame();
    cb_run(&L_frame);
    uint64_t t0 = now_ns();
    sdk_frame();
    devtools_draw();
    console_draw();
    g_perf_loader_ns += now_ns() - t0;
}

static int g_was_paused, g_watch_tick;

static void h_UpdatePads(void) {
    plugins_run_pending();
    console_install(game_window());
    uint64_t t0 = now_ns();
    poll_input();
    menu_frame();
    g_perf_loader_ns += now_ns() - t0;
    o_UpdatePads();
    // while the console or the Mods menu is open the game sees no controller input
    if ((console_is_open() || menu_overlay_open()) && f_GetPad && f_PadClear) f_PadClear(f_GetPad(0), 0);
    int paused = p_UserPause && *p_UserPause;
    rumble_update(paused);
    if (paused != g_was_paused) { g_was_paused = paused; fire(paused ? MVC_EV_PAUSED : MVC_EV_RESUMED, NULL); }
    if (++g_watch_tick % 60 == 1) controller_watch();
}

static void *g_last_vehicle;
static int g_last_wb, g_last_mission, g_first_tick = 1;

static void h_GameProcess(void) {
    o_GameProcess();
    if (g_first_tick) { g_first_tick = 0; startup_finished(); limits_report(); }

    void *veh = f_FindPlayerVehicle ? f_FindPlayerVehicle() : NULL;
    if (veh != g_last_vehicle) {
        if (g_last_vehicle) fire(MVC_EV_VEHICLE_EXITED, g_last_vehicle);
        if (veh) fire(MVC_EV_VEHICLE_ENTERED, veh);
        g_last_vehicle = veh;
    }
    if (g_wb_off >= 0 && p_Players && p_PlayerInFocus) {
        int wb = p_Players[(size_t)*p_PlayerInFocus * g_playerinfo_size + g_wb_off];
        if (wb != g_last_wb) {
            if (wb == 1) fire(MVC_EV_WASTED, NULL);
            if (wb == 2) fire(MVC_EV_BUSTED, NULL);
            g_last_wb = wb;
        }
    }
    int mission = f_IsPlayerOnAMission ? f_IsPlayerOnAMission() : 0;
    if (mission != g_last_mission) { g_last_mission = mission; fire(mission ? MVC_EV_MISSION_STARTED : MVC_EV_MISSION_ENDED, NULL); }

    cb_run(&L_tick);
}

static _Bool h_GameInit(const char *level) {
    _Bool r = o_GameInit(level);
    g_seen_init = 1;
    fire(MVC_EV_GAME_INIT, NULL);
    return r;
}

// the save slot a load/save is about: remembered from the file the game opens
static int g_file_slot;
static int h_OpenFile(const char *path, const char *mode) {
    const char *p = path ? strstr(path, "GTAVCsf") : NULL;
    if (p && p[7] >= '1' && p[7] <= '9') g_file_slot = p[7] - '0';
    return o_OpenFile(path, mode);
}

static void h_InitWhenRestarting(void) {
    save_clear();          // a new game starts with no plugin save data...
    save_set_slot(0);
    o_InitWhenRestarting(); // ...a loaded game gets it back inside here (GenericLoad)
    g_last_vehicle = NULL;
    g_last_wb = 0;
    g_last_mission = 0;
    g_seen_session = 1;
    fire(MVC_EV_NEW_SESSION, NULL);
}
static _Bool h_GenericLoad(void) {
    g_file_slot = 0;
    _Bool r = o_GenericLoad();
    if (r) {
        save_set_slot(g_file_slot);
        save_read(g_file_slot);
        fire(MVC_EV_SAVE_LOADED, NULL);
    }
    return r;
}
// C_PcSave::SaveSlot opens GTAVCsf<N>.b first and passes the open file here,
// so the slot is already known from h_OpenFile (the argument is a file handle)
static _Bool h_GenericSave(int file) {
    int s = g_file_slot;
    g_file_slot = 0;
    if (!s) mvc_log_warn("[Loader] saving, but the save slot is unknown - plugin save data not written");
    fire(MVC_EV_SAVE_WRITING, (void *)(intptr_t)s);
    _Bool r = o_GenericSave(file);
    if (r && s) {
        save_set_slot(s);
        save_write(s);
        fire(MVC_EV_SAVE_WRITTEN, (void *)(intptr_t)s);
    }
    return r;
}
static _Bool h_ShutDown(void) {
    fire(MVC_EV_SHUTDOWN, NULL);
    return o_ShutDown();
}
static void h_VehicleDtor(void *self) {
    if (self == g_last_vehicle) g_last_vehicle = NULL;
    ev_run(MVC_EV_VEHICLE_DESTROYED, self);
    o_VehicleDtor(self);
}

// init
#define RESOLVE(var, name) do { *(void **)&(var) = mvc_sym(name); if (!(var)) { mvc_log_error("[Loader] missing game symbol %s", name); ok = 0; } } while (0)
#define HOOK(name, fn, orig) do { if (mvc_hook_sym(name, (void *)(fn), (void **)&(orig))) ok = 0; } while (0)

int game_init(void) {
    int ok = 1;
    RESOLVE(f_SetScale, "_ZN5CFont8SetScaleEff");
    RESOLVE(f_SetColor, "_ZN5CFont8SetColorE5CRGBA");
    RESOLVE(f_SetDropColor, "_ZN5CFont12SetDropColorE5CRGBA");
    RESOLVE(f_SetFontStyle, "_ZN5CFont12SetFontStyleEs");
    RESOLVE(f_SetDropShadowPosition, "_ZN5CFont21SetDropShadowPositionEs");
    RESOLVE(f_SetJustifyOff, "_ZN5CFont13SetJustifyOffEv");
    RESOLVE(f_SetCentreOff, "_ZN5CFont12SetCentreOffEv");
    RESOLVE(f_SetCentreOn, "_ZN5CFont11SetCentreOnEv");
    RESOLVE(f_SetCentreSize, "_ZN5CFont13SetCentreSizeEf");
    RESOLVE(f_SetRightJustifyOn, "_ZN5CFont17SetRightJustifyOnEv");
    RESOLVE(f_SetRightJustifyOff, "_ZN5CFont18SetRightJustifyOffEv");
    RESOLVE(f_SetRightJustifyWrap, "_ZN5CFont19SetRightJustifyWrapEf");
    RESOLVE(f_SetBackgroundOff, "_ZN5CFont16SetBackgroundOffEv");
    RESOLVE(f_SetPropOn, "_ZN5CFont9SetPropOnEv");
    RESOLVE(f_SetWrapx, "_ZN5CFont8SetWrapxEf");
    RESOLVE(f_PrintString, "_ZN5CFont11PrintStringEffPt");
    RESOLVE(f_DrawFonts, "_ZN5CFont9DrawFontsEv");
    RESOLVE(f_DrawRect, "_ZN9CSprite2d8DrawRectERK5CRectRK5CRGBA");
    RESOLVE(f_GetPad, "_ZN4CPad6GetPadEi");
    find_shake_offsets();
    RESOLVE(f_PadClear, "_ZN4CPad5ClearEb");
    RESOLVE(f_FindPlayerVehicle, "_Z17FindPlayerVehiclev");
    RESOLVE(f_IsPlayerOnAMission, "_ZN11CTheScripts18IsPlayerOnAMissionEv");
    RESOLVE(p_RsGlobal, "RsGlobal");
    RESOLVE(p_WantsHud, "_ZN4CHud19m_Wants_To_Draw_HudE");
    RESOLVE(p_TheCamera, "TheCamera");
    RESOLVE(p_ReplayMode, "_ZN7CReplay4ModeE");
    RESOLVE(p_TimeMs, "_ZN6CTimer22m_snTimeInMillisecondsE");
    RESOLVE(p_UserPause, "_ZN6CTimer11m_UserPauseE");
    RESOLVE(p_Players, "_ZN6CWorld7PlayersE");
    RESOLVE(p_PlayerInFocus, "_ZN6CWorld13PlayerInFocusE");
    RESOLVE(p_FrontEnd, "FrontEndMenuManager");
    if (!ok) return 0;
    p_SelJoyName = mvc_sym("gSelectedJoystickName");

    // read this build's struct offsets out of its own code
    int off = mvc_load_imm_from_base(mvc_sym("_ZN4CHud4DrawEv"), 80, p_TheCamera, 1);
    if (off >= 0) g_cam_widescreen_off = off;
    else mvc_log_warn("[Loader] widescreen flag offset not found in CHud::Draw, using 0x%x", g_cam_widescreen_off);
    uint32_t *spi = mvc_sym("_ZN11CPlayerInfo14SavePlayerInfoEPhPj");
    if (spi && (spi[0] & 0xFF800000) == 0x52800000) g_playerinfo_size = (spi[0] >> 5) & 0xFFFF;
    g_wb_off = mvc_first_load_imm(spi, 20, 1); // first byte saved after money is m_WBState
    if (g_wb_off < 0) mvc_log_warn("[Loader] wasted/busted state not found - those events are disabled");
    g_menumap_off = mvc_load_imm_from_base(mvc_sym("_ZN6CRadar32TransformRadarPointToScreenSpaceER9CVector2DRKS0_"), 24, p_FrontEnd, 1);
    // _InputInitialiseJoys starts by clearing PSGLOBAL(joy1id/joy2id) with an STURH
    const uint32_t *ij = mvc_sym("_InputInitialiseJoys");
    for (int k = 0; ij && k < 16; k++)
        if ((ij[k] & 0xFFE00C00) == 0x78000000) { g_joy_off = (int)((int32_t)(ij[k] << 11) >> 23); break; }
    if (g_joy_off < 0) mvc_log_warn("[Loader] controller slot not found - controller fallback disabled");

    void *glfw = dlopen("@executable_path/../Frameworks/libglfw.3.dylib", RTLD_NOW | RTLD_NOLOAD);
    if (!glfw) glfw = RTLD_DEFAULT;
    *(void **)&glfwGetKey_ = dlsym(glfw, "glfwGetKey");
    *(void **)&glfwGetMouseButton_ = dlsym(glfw, "glfwGetMouseButton");
    *(void **)&glfwJoystickPresent_ = dlsym(glfw, "glfwJoystickPresent");
    *(void **)&glfwJoystickIsGamepad_ = dlsym(glfw, "glfwJoystickIsGamepad");
    *(void **)&glfwGetGamepadState_ = dlsym(glfw, "glfwGetGamepadState");
    *(void **)&glfwGetJoystickName_ = dlsym(glfw, "glfwGetJoystickName");
    *(void **)&glfwGetJoystickGUID_ = dlsym(glfw, "glfwGetJoystickGUID");
    *(void **)&glfwUpdateGamepadMappings_ = dlsym(glfw, "glfwUpdateGamepadMappings");
    if (!glfwGetKey_) mvc_log_error("[Loader] glfwGetKey not found - keyboard input disabled");

    HOOK("_ZN4CHud4DrawEv", h_HudDraw, o_HudDraw);
    HOOK("_ZN6CRadar9DrawBlipsEv", h_DrawBlips, o_DrawBlips);
    HOOK("_ZN4CHud13DrawAfterFadeEv", h_DrawAfterFade, o_DrawAfterFade);
    HOOK("_Z13RenderEffectsv", h_RenderEffects, o_RenderEffects);
    HOOK("_ZN4CPad10UpdatePadsEv", h_UpdatePads, o_UpdatePads);
    HOOK("_ZN5CGame7ProcessEv", h_GameProcess, o_GameProcess);
    HOOK("_ZN5CGame10InitialiseEPKc", h_GameInit, o_GameInit);
    HOOK("_ZN5CGame24InitialiseWhenRestartingEv", h_InitWhenRestarting, o_InitWhenRestarting);
    HOOK("_Z11GenericLoadv", h_GenericLoad, o_GenericLoad);
    HOOK("_Z11GenericSavei", h_GenericSave, o_GenericSave);
    HOOK("_ZN5CGame8ShutDownEv", h_ShutDown, o_ShutDown);
    HOOK("_ZN8CVehicleD2Ev", h_VehicleDtor, o_VehicleDtor);
    HOOK("_ZN8CFileMgr8OpenFileEPKcS1_", h_OpenFile, o_OpenFile);
    if (!ok) return 0;
    // optional: without these the DualSense mapping fix is just skipped
    if (mvc_hook_sym("_InputInitialiseJoys", (void *)h_InitJoys, (void **)&o_InitJoys))
        mvc_log_warn("[Loader] _InputInitialiseJoys not found - DualSense mapping fix disabled");
    else if (mvc_hook_sym("_Z12joysChangeCBii", (void *)h_JoysChange, (void **)&o_JoysChange))
        mvc_log_warn("[Loader] joysChangeCB not found - DualSense fix won't apply to controllers connected later");

    console_register_builtins();
    if (!text_init()) mvc_log_warn("[Loader] custom game text unavailable");
    if (!menu_init()) mvc_log_warn("[Loader] Options > Mods menu unavailable");
    sdk_init();
    return 1;
}
