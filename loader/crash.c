// crash reporter. on a fatal signal it writes GameData/crashes/crash-<time>.log with a
// backtrace, which plugin it was probably in, the loaded plugins and the last log lines.
// startup crashes get counted per plugin, and one that keeps crashing at launch gets
// turned off.
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <mach-o/dyld.h>
#include "internal.h"

static char g_dir[PATH_MAX];
static char g_state[PATH_MAX];
static char g_loading[256];   // plugin file being loaded/initialised right now
static char g_build[64] = "?";
static char g_macos[64] = "?";

const char *crash_state_path(void) { return g_state; }

void crash_set_loading(const char *plugin_file) {
    if (plugin_file) snprintf(g_loading, sizeof g_loading, "%s", plugin_file);
    else g_loading[0] = 0;
}

static void wr(int fd, const char *s) { write(fd, s, strlen(s)); }

static void wrf(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void wrf(int fd, const char *fmt, ...) {
    char b[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    wr(fd, b);
}

static int is_plugin_image(const char *path) {
    return path && (strstr(path, "/GameData/plugins/") || strstr(path, "/GameData/scripts/"));
}

static void describe(int fd, int i, uintptr_t pc, char *culprit, size_t cn) {
    Dl_info di;
    const PatchRec *isl = hook_find_island(pc);
    if (isl) {
        wrf(fd, "  #%-2d 0x%016lx  [hook island for %s, installed by %s]\n", i, (unsigned long)pc, isl->what, isl->owner);
        if (!culprit[0] && strcmp(isl->owner, "Loader")) snprintf(culprit, cn, "%s (hook on %s)", isl->owner, isl->what);
        return;
    }
    if (dladdr((void *)pc, &di) && di.dli_fname) {
        const char *img = strrchr(di.dli_fname, '/');
        img = img ? img + 1 : di.dli_fname;
        if (di.dli_sname)
            wrf(fd, "  #%-2d 0x%016lx  %s  %s + %lu\n", i, (unsigned long)pc, img, di.dli_sname,
                (unsigned long)(pc - (uintptr_t)di.dli_saddr));
        else
            wrf(fd, "  #%-2d 0x%016lx  %s + 0x%lx\n", i, (unsigned long)pc, img,
                (unsigned long)(pc - (uintptr_t)di.dli_fbase));
        if (!culprit[0] && is_plugin_image(di.dli_fname)) snprintf(culprit, cn, "%s", img);
    } else {
        wrf(fd, "  #%-2d 0x%016lx  ???\n", i, (unsigned long)pc);
    }
}

// startup crash bookkeeping: "<plugin file> <count>" lines.
static void bump_startup_crash(const char *plugin) {
    char lines[64][300];
    int n = 0, found = 0;
    FILE *f = fopen(g_state, "r");
    if (f) {
        while (n < 64 && fgets(lines[n], sizeof lines[n], f)) n++;
        fclose(f);
    }
    f = fopen(g_state, "w");
    if (!f) return;
    for (int i = 0; i < n; i++) {
        char name[256];
        int c;
        if (sscanf(lines[i], "%255s %d", name, &c) == 2 && !strcmp(name, plugin)) { c++; found = 1; fprintf(f, "%s %d\n", name, c); }
        else fputs(lines[i], f);
    }
    if (!found) fprintf(f, "%s 1\n", plugin);
    fclose(f);
}

static volatile sig_atomic_t g_in_handler;

static void on_crash(int sig, siginfo_t *si, void *uc_) {
    if (g_in_handler) _exit(128 + sig);
    g_in_handler = 1;
    ucontext_t *uc = uc_;
    uintptr_t pc = uc->uc_mcontext->__ss.__pc;
    uintptr_t lr = uc->uc_mcontext->__ss.__lr;
    uintptr_t fp = uc->uc_mcontext->__ss.__fp;

    char path[PATH_MAX], ts[32];
    time_t now = time(NULL);
    strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", localtime(&now));
    snprintf(path, sizeof path, "%s/crash-%s.log", g_dir, ts);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) fd = STDERR_FILENO;

    wr(fd, "macVC crash report (macVC Plugin Loader " MVC_LOADER_VERSION ")\n");
    wrf(fd, "time: %s   macVC: %s   macOS: %s\n", ts, g_build, g_macos);
    wrf(fd, "signal: %d (%s)   fault address: %p\n", sig, strsignal(sig), si ? si->si_addr : NULL);
    if (g_loading[0]) wrf(fd, "crashed while loading plugin: %s\n", g_loading);
    wr(fd, "\nbacktrace:\n");
    char culprit[200] = "";
    if (g_loading[0]) snprintf(culprit, sizeof culprit, "%s", g_loading);
    describe(fd, 0, pc, culprit, sizeof culprit);
    describe(fd, 1, lr, culprit, sizeof culprit);
    for (int i = 2; i < 48 && fp && !(fp & 7); i++) {
        uintptr_t *frame = (uintptr_t *)fp;
        uintptr_t ret = frame[1];
        if (!ret) break;
        describe(fd, i, ret, culprit, sizeof culprit);
        if (frame[0] <= fp) break;
        fp = frame[0];
    }
    wrf(fd, "\nlikely cause: %s\n", culprit[0] ? culprit : "macVC itself (no plugin code in the crashing call chain)");

    wr(fd, "\npatches and hooks:\n");
    for (int i = 0; i < hook_count(); i++) {
        const PatchRec *r = hook_get(i);
        if (r->active) wrf(fd, "  %-10s %-40s by %s\n", r->kind == PATCH_HOOK ? "hook" : r->kind == PATCH_MIDHOOK ? "midhook" : "patch", r->what, r->owner);
    }
    wr(fd, "\nregisters:\n");
    for (int i = 0; i < 29; i++) wrf(fd, "  x%-2d %016llx%s", i, uc->uc_mcontext->__ss.__x[i], (i % 4 == 3) ? "\n" : "");
    wrf(fd, "\n  fp  %016llx  lr  %016llx  sp  %016llx  pc  %016llx\n",
        uc->uc_mcontext->__ss.__fp, uc->uc_mcontext->__ss.__lr, uc->uc_mcontext->__ss.__sp, uc->uc_mcontext->__ss.__pc);
    wr(fd, "\nlast log lines:\n");
    log_dump_fd(fd, 200);
    if (fd != STDERR_FILENO) close(fd);

    // a crash during startup counts against the plugin responsible
    if (g_loading[0]) bump_startup_crash(g_loading);
    else if (culprit[0] && hook_count() >= 0) {
        // culprit may be "Name.dylib" or "Name (hook on ...)"
        char name[200];
        snprintf(name, sizeof name, "%s", culprit);
        char *sp = strchr(name, ' ');
        if (sp) *sp = 0;
        if (!strstr(name, ".dylib")) strncat(name, ".dylib", sizeof name - strlen(name) - 1);
        extern int g_in_startup_window;
        if (g_in_startup_window) bump_startup_crash(name);
    }

    // hand over to the default handler so macOS still writes its own report
    signal(sig, SIG_DFL);
    raise(sig);
}

void crash_init(const char *game_dir) {
    snprintf(g_dir, sizeof g_dir, "%s/crashes", game_dir);
    mkdir(g_dir, 0755);
    snprintf(g_state, sizeof g_state, "%s/macVCLoader.state", game_dir);
    struct utsname u;
    if (!uname(&u)) snprintf(g_macos, sizeof g_macos, "Darwin %s", u.release);
    // macVC version from Info.plist (best effort, no plist parsing)
    char plist[PATH_MAX];
    snprintf(plist, sizeof plist, "%s/../../Info.plist", game_dir);
    FILE *f = fopen(plist, "r");
    if (f) {
        char line[256];
        int next = 0;
        while (fgets(line, sizeof line, f)) {
            if (next) {
                char *a = strstr(line, "<string>"), *b = strstr(line, "</string>");
                if (a && b && b > a + 8) { *b = 0; snprintf(g_build, sizeof g_build, "%s", a + 8); }
                break;
            }
            if (strstr(line, "CFBundleShortVersionString")) next = 1;
        }
        fclose(f);
    }

    static char altstack[65536];
    stack_t ss = {.ss_sp = altstack, .ss_size = sizeof altstack, .ss_flags = 0};
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP};
    for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++) sigaction(sigs[i], &sa, NULL);
}
