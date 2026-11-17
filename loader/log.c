// log ring buffer, the log file and its rotation, and catching the game's stdout/stderr.
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "internal.h"

static char g_lines[LOG_LINES][LOG_LEN];
static uint8_t g_kind[LOG_LINES];
static int g_head, g_count, g_errors;
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_file;
static int g_real_stderr = -1;
int g_min_log_level = LOG_INFO;

#define KEEP_OLD_LOGS 5

static int name_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void rotate(const char *game_dir, const char *cur) {
    struct stat st;
    if (stat(cur, &st) != 0) return;
    char dir[PATH_MAX], dst[PATH_MAX], ts[32];
    snprintf(dir, sizeof dir, "%s/logs", game_dir);
    mkdir(dir, 0755);
    strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", localtime(&st.st_mtime));
    snprintf(dst, sizeof dst, "%s/macVCLoader-%s.log", dir, ts);
    rename(cur, dst);
    // keep only the newest KEEP_OLD_LOGS
    DIR *d = opendir(dir);
    if (!d) return;
    char *names[512];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 512)
        if (!strncmp(e->d_name, "macVCLoader-", 12)) names[n++] = strdup(e->d_name);
    closedir(d);
    qsort(names, (size_t)n, sizeof *names, name_cmp);
    for (int i = 0; i < n; i++) {
        if (i < n - KEEP_OLD_LOGS) {
            snprintf(dst, sizeof dst, "%s/%s", dir, names[i]);
            unlink(dst);
        }
        free(names[i]);
    }
}

void log_open(const char *game_dir) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/macVCLoader.log", game_dir);
    rotate(game_dir, path);
    g_file = fopen(path, "w");
}

void log_push(const char *s, int kind) {
    pthread_mutex_lock(&g_mx);
    char *dst = g_lines[g_head];
    snprintf(dst, LOG_LEN, "%s", s);
    for (char *c = dst; *c; c++) {
        if (*c == '\t') *c = ' ';
        else if (*c == '~' || *c == '_') *c = '-'; // '~' is a colour code; the game font has no '_'
        else if ((unsigned char)*c < 32 || (unsigned char)*c > 126) *c = '?';
    }
    g_kind[g_head] = (uint8_t)kind;
    g_head = (g_head + 1) % LOG_LINES;
    if (g_count < LOG_LINES) g_count++;
    if (kind == LOG_ERROR) g_errors++;
    if (g_file) {
        fprintf(g_file, "%s\n", s);
        fflush(g_file);
    }
    pthread_mutex_unlock(&g_mx);
}

int log_error_count(void) { return g_errors; }

// with a filter, only lines containing it (any case) count, newest last.
int log_copy(char out[][LOG_LEN], uint8_t *kinds, int max, int skip, int *total, const char *filter) {
    if (filter && !*filter) filter = NULL;
    pthread_mutex_lock(&g_mx);
    // walk newest to oldest, keep the window [skip, skip+max) of matching lines
    int match = 0, n = 0;
    for (int i = 0; i < g_count; i++) {
        int idx = (g_head - 1 - i + 2 * LOG_LINES) % LOG_LINES;
        if (filter && !strcasestr(g_lines[idx], filter)) continue;
        if (match >= skip && n < max) {
            memcpy(out[max - 1 - n], g_lines[idx], LOG_LEN);
            kinds[max - 1 - n] = g_kind[idx];
            n++;
        }
        match++;
    }
    *total = match;
    pthread_mutex_unlock(&g_mx);
    if (n < max && n) { // shift the filled tail to the front
        memmove(out, out[max - n], (size_t)n * LOG_LEN);
        memmove(kinds, kinds + (max - n), (size_t)n);
    }
    return n;
}

// the newest `last_n` lines (matching filter, if any) joined by newlines; caller frees.
char *log_join(int last_n, const char *filter) {
    if (last_n < 1) last_n = 1;
    if (last_n > LOG_LINES) last_n = LOG_LINES;
    char (*lines)[LOG_LEN] = malloc((size_t)last_n * LOG_LEN);
    uint8_t *k = malloc((size_t)last_n);
    int total, n = lines && k ? log_copy(lines, k, last_n, 0, &total, filter) : 0;
    char *s = malloc((size_t)n * (LOG_LEN + 1) + 1);
    size_t used = 0;
    if (s) {
        for (int i = 0; i < n; i++) {
            size_t l = strnlen(lines[i], LOG_LEN);
            memcpy(s + used, lines[i], l);
            used += l;
            s[used++] = '\n';
        }
        s[used] = 0;
    }
    free(lines);
    free(k);
    return s;
}

void log_dump_fd(int fd, int last_n) {
    // no locking: called from the crash handler
    int n = g_count < last_n ? g_count : last_n;
    int start = (g_head - n + LOG_LINES) % LOG_LINES;
    for (int i = 0; i < n; i++) {
        const char *l = g_lines[(start + i) % LOG_LINES];
        write(fd, l, strnlen(l, LOG_LEN));
        write(fd, "\n", 1);
    }
}

void log_vprintf(int kind, void *caller, const char *fmt, va_list ap) {
    if (kind < g_min_log_level && kind != LOG_GAME) return;
    char msg[1024], line[1200], tag[64] = "";
    vsnprintf(msg, sizeof msg, fmt, ap);
    if (msg[0] != '[') owner_name(caller, tag, sizeof tag);
    const char *lvl = kind == LOG_ERROR ? "ERROR: " : kind == LOG_WARN ? "warning: " : "";
    const char *close = msg[0] == '[' ? strstr(msg, "] ") : NULL;
    if (tag[0]) snprintf(line, sizeof line, "[%s] %s%s", tag, lvl, msg);
    else if (close && lvl[0]) snprintf(line, sizeof line, "%.*s%s%s", (int)(close - msg + 2), msg, lvl, close + 2);
    else snprintf(line, sizeof line, "%s%s", lvl, msg);
    log_push(line, kind);
    if (g_real_stderr >= 0) dprintf(g_real_stderr, "%s\n", line);
}

void mvc_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_vprintf(LOG_INFO, __builtin_return_address(0), fmt, ap);
    va_end(ap);
}
void mvc_log_warn(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_vprintf(LOG_WARN, __builtin_return_address(0), fmt, ap);
    va_end(ap);
}
void mvc_log_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_vprintf(LOG_ERROR, __builtin_return_address(0), fmt, ap);
    va_end(ap);
}

// game stdout/stderr -> console
static int g_pipe[2] = {-1, -1};

static int looks_like_error(const char *s) {
    return strcasestr(s, "error") || strcasestr(s, "fail") || strcasestr(s, "couldn't");
}

static void *pipe_reader(void *arg) {
    (void)arg;
    char buf[4096], acc[LOG_LEN * 2];
    size_t used = 0;
    for (;;) {
        ssize_t n = read(g_pipe[0], buf, sizeof buf);
        if (n <= 0) break;
        if (g_real_stderr >= 0) write(g_real_stderr, buf, (size_t)n);
        for (ssize_t k = 0; k < n; k++) {
            char c = buf[k];
            if (c == '\n' || used >= sizeof acc - 1) {
                acc[used] = 0;
                // game output is never counted as a loader error, only tinted
                if (used) log_push(acc, looks_like_error(acc) ? LOG_WARN : LOG_GAME);
                used = 0;
                if (c == '\n') continue;
            }
            if (c != '\r') acc[used++] = c;
        }
    }
    return NULL;
}

void log_capture_stdio(void) {
    g_real_stderr = dup(STDERR_FILENO);
    if (pipe(g_pipe) != 0) return;
    // never let a slow reader stall the game: when the pipe is full, output is dropped
    fcntl(g_pipe[1], F_SETFL, fcntl(g_pipe[1], F_GETFL) | O_NONBLOCK);
    fflush(stdout);
    fflush(stderr);
    dup2(g_pipe[1], STDOUT_FILENO);
    dup2(g_pipe[1], STDERR_FILENO);
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    pthread_t t;
    pthread_create(&t, NULL, pipe_reader, NULL);
    pthread_detach(t);
}
