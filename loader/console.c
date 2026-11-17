// the ~ console: log view, input line with history, built in and plugin commands.
// key events go through glfw callbacks chained in front of the game's, so the console
// (and plugins that eat keys) can keep them away from the game.
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "internal.h"

typedef void (*keyfun)(void *, int, int, int, int);
typedef void (*charfun)(void *, unsigned int);
static keyfun o_key;
static charfun o_char;
static int g_open;
static char g_line[160];
static int g_len;
static char g_hist[32][160];
static int g_nhist, g_hpos = -1;
static int g_scroll;
static uint8_t g_consumed[350];
static char g_filter[64];       // `find`: only matching log lines are shown
static void *g_window;

int console_is_open(void) { return g_open; }

void mvc_consume_key(int k, int c) { if (k >= 0 && k < 350) g_consumed[k] = c ? 1 : 0; }

// commands
#define MAXCMD 128
static struct { char name[32]; char help[96]; mvc_command_fn fn; } g_cmds[MAXCMD];
static int g_ncmds;

void mvc_console_command(const char *name, const char *help, mvc_command_fn fn) {
    if (!name || !fn || g_ncmds >= MAXCMD) return;
    for (int i = 0; i < g_ncmds; i++)
        if (!strcasecmp(g_cmds[i].name, name)) { g_cmds[i].fn = fn; return; }
    snprintf(g_cmds[g_ncmds].name, sizeof g_cmds[0].name, "%s", name);
    snprintf(g_cmds[g_ncmds].help, sizeof g_cmds[0].help, "%s", help ? help : "");
    g_cmds[g_ncmds++].fn = fn;
}

int commands_release(const char *owner) {
    int w = 0, n = 0;
    for (int i = 0; i < g_ncmds; i++) {
        char o[64];
        owner_name((void *)g_cmds[i].fn, o, sizeof o);
        if (!strcmp(o, owner)) { n++; continue; }
        g_cmds[w++] = g_cmds[i];
    }
    g_ncmds = w;
    return n;
}

static void out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void out(const char *fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    log_push(b, LOG_INFO);
}

static void cmd_help(int argc, const char **argv) {
    (void)argc; (void)argv;
    out("commands:");
    for (int i = 0; i < g_ncmds; i++) out("  %-10s %s", g_cmds[i].name, g_cmds[i].help);
}
static void cmd_clear(int argc, const char **argv) { (void)argc; (void)argv; for (int i = 0; i < 60; i++) log_push("", LOG_GAME); }
static void cmd_hooks(int argc, const char **argv) {
    (void)argc; (void)argv;
    int n = 0;
    for (int i = 0; i < hook_count(); i++) {
        const PatchRec *r = hook_get(i);
        if (!r->active) continue;
        out("  %-8s %-48s %s", r->kind == PATCH_HOOK ? "hook" : r->kind == PATCH_MIDHOOK ? "midhook" : "patch", r->what, r->owner);
        n++;
    }
    out("%d active hooks/patches", n);
}
static void cmd_plugins(int argc, const char **argv) { (void)argc; (void)argv; plugins_print(); }
static void cmd_pos(int argc, const char **argv) {
    (void)argc; (void)argv;
    MVCVec3 p = mvc_player_pos();
    out("position: %.2f, %.2f, %.2f", p.x, p.y, p.z);
}
static void cmd_tp(int argc, const char **argv) {
    if (argc < 3) { out("usage: tp x y [z]"); return; }
    MVCVec3 p = {(float)atof(argv[1]), (float)atof(argv[2]), argc > 3 ? (float)atof(argv[3]) : -100.f};
    mvc_player_teleport(p);
}
static void cmd_money(int argc, const char **argv) {
    if (argc < 2) { out("money: $%d", mvc_player_money()); return; }
    mvc_player_set_money(atoi(argv[1]));
}
static void cmd_health(int argc, const char **argv) {
    if (argc < 2) { out("health %.0f, armour %.0f", mvc_player_health(), mvc_player_armour()); return; }
    mvc_player_set_health((float)atof(argv[1]));
    if (argc > 2) mvc_player_set_armour((float)atof(argv[2]));
}
static void cmd_wanted(int argc, const char **argv) { if (argc > 1) mvc_player_set_wanted(atoi(argv[1])); }
static void cmd_time(int argc, const char **argv) {
    int h, m;
    if (argc < 2) { mvc_world_time(&h, &m); out("time %02d:%02d", h, m); return; }
    mvc_world_set_time(atoi(argv[1]), argc > 2 ? atoi(argv[2]) : 0);
}
static void cmd_weather(int argc, const char **argv) { if (argc > 1) mvc_world_set_weather(atoi(argv[1])); else out("weather 0-5: sunny cloudy rainy foggy extrasunny hurricane"); }
static void cmd_spawn(int argc, const char **argv) {
    if (argc < 2) { out("usage: spawn <model id>   (e.g. 141 infernus, 191 pcj600)"); return; }
    MVCVec3 p = mvc_player_pos();
    p.x += 5;
    void *v = mvc_vehicle_spawn(atoi(argv[1]), p, 0);
    out(v ? "spawned" : "spawn failed (see log)");
}
static void cmd_fix(int argc, const char **argv) { (void)argc; (void)argv; void *v = mvc_player_vehicle(); if (v) mvc_vehicle_fix(v); }
static void cmd_give(int argc, const char **argv) {
    if (argc < 2) { out("usage: give <weapon id> [ammo]"); return; }
    mvc_player_give_weapon(atoi(argv[1]), argc > 2 ? atoi(argv[2]) : 100);
}
static void cmd_echo(int argc, const char **argv) {
    char b[256] = "";
    for (int i = 1; i < argc; i++) { strncat(b, argv[i], sizeof b - strlen(b) - 2); strcat(b, " "); }
    out("%s", b);
}
static void cmd_rumble(int argc, const char **argv) {
    int pad = argc > 1 ? atoi(argv[1]) : 0;
    out(mvc_pad_rumble(pad, 1.0f, 400) ? "rumble sent" : "rumble not available for that controller");
}
static void cmd_pads(int argc, const char **argv) {
    (void)argc; (void)argv;
    int n = mvc_pad_count();
    out("%d gamepad(s)", n);
    for (int i = 0; i < n; i++) out("  pad %d: %s", i, mvc_pad_name(i));
}

// find <text>: show only matching lines; find alone shows everything again
static void cmd_find(int argc, const char **argv) {
    g_filter[0] = 0;
    for (int i = 1; i < argc; i++) {
        if (i > 1) strncat(g_filter, " ", sizeof g_filter - strlen(g_filter) - 1);
        strncat(g_filter, argv[i], sizeof g_filter - strlen(g_filter) - 1);
    }
    for (char *c = g_filter; *c; c++) if (*c == '_') *c = '-'; // the log stores '_' as '-'
    g_scroll = 0;
    if (g_filter[0]) out("showing lines containing \"%s\" (type find to show all)", g_filter);
}
static void set_clipboard(const char *s) {
    void (*set)(void *, const char *) = dlsym(RTLD_DEFAULT, "glfwSetClipboardString");
    if (set && g_window) set(g_window, s);
}
// copy [lines]: the newest lines (of the find filter, if one is set) to the clipboard
static void cmd_copy(int argc, const char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 100;
    if (argc > 1 && !strcasecmp(argv[1], "all")) n = LOG_LINES;
    if (n < 1) { out("usage: copy [lines|all]"); return; }
    char *s = log_join(n, g_filter);
    if (!s) return;
    int lines = 0;
    for (char *c = s; *c; c++) lines += *c == '\n';
    set_clipboard(s);
    free(s);
    out("copied %d line%s to the clipboard", lines, lines == 1 ? "" : "s");
}

// Tab: complete the command name (or list the candidates)
static void complete(void) {
    if (!g_len || strchr(g_line, ' ')) return;
    int hits = 0, first = -1;
    size_t common = 0;
    for (int i = 0; i < g_ncmds; i++) {
        if (strncasecmp(g_cmds[i].name, g_line, (size_t)g_len)) continue;
        if (first < 0) { first = i; common = strlen(g_cmds[i].name); }
        else {
            size_t c = 0;
            while (c < common && tolower((unsigned char)g_cmds[i].name[c]) == tolower((unsigned char)g_cmds[first].name[c])) c++;
            common = c;
        }
        hits++;
    }
    if (!hits) return;
    if (hits == 1) { snprintf(g_line, sizeof g_line, "%s ", g_cmds[first].name); g_len = (int)strlen(g_line); return; }
    if ((int)common > g_len) { snprintf(g_line, sizeof g_line, "%.*s", (int)common, g_cmds[first].name); g_len = (int)strlen(g_line); return; }
    char b[400] = "";
    for (int i = 0; i < g_ncmds; i++)
        if (!strncasecmp(g_cmds[i].name, g_line, (size_t)g_len)) { strncat(b, g_cmds[i].name, sizeof b - strlen(b) - 3); strcat(b, "  "); }
    out("%s", b);
}

static void paste(void) {
    const char *(*get)(void *) = dlsym(RTLD_DEFAULT, "glfwGetClipboardString");
    const char *s = get && g_window ? get(g_window) : NULL;
    for (; s && *s && g_len < (int)sizeof g_line - 1; s++) {
        if (*s == '\n' || *s == '\r') break;
        if (*s >= 32 && *s < 127) g_line[g_len++] = *s;
    }
    g_line[g_len] = 0;
}

void console_register_builtins(void) {
    mvc_console_command("help", "list commands", cmd_help);
    mvc_console_command("clear", "clear the console", cmd_clear);
    mvc_console_command("plugins", "loaded plugins and versions", cmd_plugins);
    mvc_console_command("hooks", "every hook and patch, and who made it", cmd_hooks);
    mvc_console_command("pads", "connected controllers", cmd_pads);
    mvc_console_command("rumble", "rumble <pad> - test controller rumble", cmd_rumble);
    mvc_console_command("pos", "your position", cmd_pos);
    mvc_console_command("tp", "tp x y [z] - teleport", cmd_tp);
    mvc_console_command("money", "money [amount] - show or set", cmd_money);
    mvc_console_command("health", "health [hp] [armour] - show or set", cmd_health);
    mvc_console_command("wanted", "wanted <stars>", cmd_wanted);
    mvc_console_command("time", "time [h] [m] - show or set the clock", cmd_time);
    mvc_console_command("weather", "weather <0-5>", cmd_weather);
    mvc_console_command("spawn", "spawn <model> - car, bike or boat next to you", cmd_spawn);
    mvc_console_command("fix", "repair your car", cmd_fix);
    mvc_console_command("give", "give <weapon id> [ammo]", cmd_give);
    mvc_console_command("echo", "print text", cmd_echo);
    mvc_console_command("find", "find <text> - show only matching lines (find alone: all)", cmd_find);
    mvc_console_command("copy", "copy [lines|all] - log lines to the clipboard (honours find)", cmd_copy);
    devtools_register();
}

void console_exec(const char *line) {
    char buf[160];
    snprintf(buf, sizeof buf, "%s", line);
    const char *argv[16];
    int argc = 0;
    for (char *t = strtok(buf, " \t"); t && argc < 16; t = strtok(NULL, " \t")) argv[argc++] = t;
    if (!argc) return;
    char echo[200];
    snprintf(echo, sizeof echo, "> %s", line);
    log_push(echo, LOG_WARN);
    for (int i = 0; i < g_ncmds; i++)
        if (!strcasecmp(g_cmds[i].name, argv[0])) { g_cmds[i].fn(argc, argv); return; }
    out("unknown command \"%s\" - type help", argv[0]);
}

// key / char callbacks
static int is_console_key(int k) { return k > 0 && (k == g_cfg.console_key || k == g_cfg.console_key2); }

static void on_key(void *win, int key, int sc, int action, int mods) {
    if (action != 0 && is_console_key(key)) { // press/repeat of the console key
        if (action == 1) { g_open = !g_open; g_scroll = 0; }
        return;
    }
    if (!g_open && menu_overlay_key(key, action)) return; // the Mods menu has the keyboard
    if (g_open && action != 0) {
        switch (key) {
        case 257: case 335: // enter
            if (g_len) {
                g_line[g_len] = 0;
                if (g_nhist < 32) snprintf(g_hist[g_nhist++], 160, "%s", g_line);
                else { memmove(g_hist, g_hist[1], sizeof g_hist[0] * 31); snprintf(g_hist[31], 160, "%s", g_line); }
                console_exec(g_line);
                g_len = 0; g_line[0] = 0; g_hpos = -1;
            }
            break;
        case 259: if (g_len) g_line[--g_len] = 0; break;           // backspace
        case 258: complete(); break;                                 // tab
        case 86:                                                     // cmd/ctrl+V pastes
            if (mods & (0x0002 | 0x0008)) paste();
            break;
        case 67:                                                     // cmd/ctrl+C copies the view
            if (mods & (0x0002 | 0x0008)) { const char *a[] = {"copy"}; cmd_copy(1, a); }
            break;
        case 256: g_open = 0; break;                                 // escape closes
        case 265:                                                    // up: history
            if (g_nhist) { g_hpos = g_hpos < 0 ? g_nhist - 1 : (g_hpos > 0 ? g_hpos - 1 : 0);
                           snprintf(g_line, sizeof g_line, "%s", g_hist[g_hpos]); g_len = (int)strlen(g_line); }
            break;
        case 264:                                                    // down
            if (g_hpos >= 0) { g_hpos++; if (g_hpos >= g_nhist) { g_hpos = -1; g_line[0] = 0; g_len = 0; }
                               else { snprintf(g_line, sizeof g_line, "%s", g_hist[g_hpos]); g_len = (int)strlen(g_line); } }
            break;
        case 266: g_scroll += 10; break;                             // page up
        case 267: g_scroll = g_scroll > 10 ? g_scroll - 10 : 0; break;
        case 268: g_scroll = LOG_LINES; break;                       // home
        case 269: g_scroll = 0; break;                               // end
        }
        return; // nothing reaches the game while the console is open
    }
    if (action != 0 && key >= 0 && key < 350 && g_consumed[key]) return;
    if (o_key) o_key(win, key, sc, action, mods); // releases always reach the game, so nothing sticks
}

static void on_char(void *win, unsigned int cp) {
    if (g_open) {
        if (cp >= 32 && cp < 127 && cp != '`' && cp != '~' && g_len < (int)sizeof g_line - 1) {
            g_line[g_len++] = (char)cp;
            g_line[g_len] = 0;
        }
        return;
    }
    if (o_char) o_char(win, cp);
}

static int g_installed;
void console_install(void *window) {
    if (g_installed || !window) return;
    keyfun (*setKey)(void *, keyfun) = dlsym(RTLD_DEFAULT, "glfwSetKeyCallback");
    charfun (*setChar)(void *, charfun) = dlsym(RTLD_DEFAULT, "glfwSetCharCallback");
    if (!setKey || !setChar) { mvc_log_error("[Loader] GLFW callbacks not found - console typing disabled"); g_installed = 1; return; }
    g_window = window;
    o_key = setKey(window, on_key);
    o_char = setChar(window, on_char);
    g_installed = 1;
}

// drawing
void console_draw(void) {
    if (!g_open) return;
    float W = mvc_screen_w(), H = mvc_screen_h();
    float u = H / 448.f * g_cfg.console_scale;
    float sy = 0.62f * u, sx = 0.30f * u;
    float lh = 11.0f * u, pad = 6 * u;
    float panelH = H * 0.5f, headH = lh + pad;
    int errs = log_error_count();

    mvc_flush_text();
    mvc_rect(0, 0, W, panelH, (MVCColor){0, 0, 0, (uint8_t)g_cfg.console_opacity});
    mvc_rect(0, 0, W, headH, (MVCColor){40, 22, 60, 240});
    mvc_rect(0, panelH - lh - pad, W, panelH, (MVCColor){25, 14, 38, 245});
    mvc_rect(0, panelH - 2 * u, W, panelH, g_safe_mode ? (MVCColor){255, 200, 0, 255} : (MVCColor){255, 150, 225, 255});

    char head[256];
    snprintf(head, sizeof head, "MACVC PLUGIN LOADER %s%s   |   %d PLUGIN%s%s%s   |   %d HOOKS   |   TYPE HELP",
             MVC_LOADER_VERSION, g_safe_mode ? "   |   SAFE MODE - PLUGINS OFF" : "",
             g_plugins_loaded, g_plugins_loaded == 1 ? "" : "S",
             g_plugins_disabled ? " (+ DISABLED)" : "", g_plugins_failed ? " (+ FAILED)" : "", hook_count());
    mvc_text(pad, pad * 0.5f, head, sx, sy, (MVCColor){255, 150, 225, 255}, MVC_ALIGN_LEFT, 1, 0);
    if (errs) {
        char e[48];
        snprintf(e, sizeof e, "%d ERROR%s", errs, errs == 1 ? "" : "S");
        mvc_text(W - pad, pad * 0.5f, e, sx, sy, (MVCColor){255, 90, 80, 255}, MVC_ALIGN_RIGHT, 1, 0);
    }
    mvc_flush_text();

    int rows = (int)((panelH - headH - lh - 2 * pad) / lh);
    if (rows < 1) return;
    if (rows > 64) rows = 64;
    static char lines[64][LOG_LEN];
    uint8_t kinds[64];
    int total;
    if (g_scroll > LOG_LINES) g_scroll = LOG_LINES;
    int n = log_copy(lines, kinds, rows, g_scroll, &total, g_filter);
    if (total - rows < g_scroll) g_scroll = total > rows ? total - rows : 0;
    float y = headH + pad * 0.5f;
    for (int r = 0; r < n; r++) {
        MVCColor c = kinds[r] == LOG_ERROR ? (MVCColor){255, 110, 110, 255}
                   : kinds[r] == LOG_WARN  ? (MVCColor){255, 205, 90, 255}
                   : kinds[r] == LOG_INFO  ? (MVCColor){255, 255, 255, 255}
                                           : (MVCColor){165, 175, 190, 255};
        mvc_text(pad, y, lines[r], sx, sy, c, MVC_ALIGN_LEFT, 1, 0);
        y += lh;
        if ((r & 7) == 7) mvc_flush_text();
    }
    if (g_scroll > 0 || g_filter[0]) {
        char m[128];
        if (g_filter[0]) snprintf(m, sizeof m, "-- FIND \"%s\": %d LINE%s%s --", g_filter, total, total == 1 ? "" : "S",
                                  g_scroll ? ", SCROLLED UP" : "");
        else snprintf(m, sizeof m, "-- SCROLLED UP %d LINES --", g_scroll);
        mvc_text(W - pad, panelH - 2 * lh - pad, m, sx, sy, (MVCColor){255, 200, 0, 255}, MVC_ALIGN_RIGHT, 1, 0);
    }
    char in[200];
    int blink = (mvc_time_ms() / 400) & 1;
    snprintf(in, sizeof in, "> %s%s", g_line, blink ? "-" : " ");
    mvc_text(pad, panelH - lh - pad * 0.5f, in, sx, sy, (MVCColor){255, 255, 255, 255}, MVC_ALIGN_LEFT, 1, 0);
    mvc_flush_text();
}
