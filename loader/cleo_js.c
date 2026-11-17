// javascript scripts, CLEO Redux style.
//


// GameData/CLEO/*.js (and .js in a mod's CLEO folder) run on JavaScriptCore, which macos
// already has. each script gets a thread but only one thing runs at a time: the game thread
// gives a script its turn during the script tick and waits until it calls wait() or ends.
// so script code always runs while the game sits inside CTheScripts::Process, same spot
// cleo scripts run.
//


// the api copies CLEO Redux: wait, log, exit, showTextBox, native("COMMAND", ...), the
// classes from sanny builder's library (Player, Char, Car...), Memory.Read*/Write* with
// gta-vc.exe 1.0 addresses, Memory.Fn.* for calling game functions, Memory.To*/From*,
// the enums, GAME, HOST, __filename, __dirname. full list in docs/PORTING-CLEO.md.
//


// static members keep their case (Car.Create), instance members are camelCase
// (player.getChar()). conditions return true/false, results come back as a value or an
// object, handles come back as class instances.
//


// every command gets assembled into bytecode and run by the game's own interpreter, through
// the same path as cleo opcodes (cleo_js_exec in cleo.c).
#include "internal.h"
#include <JavaScriptCore/JavaScriptCore.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint16_t op; const char *name, *cls, *member; int flags; const char *in, *out, *names; } Native;
static const Native g_nat[] = {
#include "cleo_natives.h"
};
#define NNAT ((int)(sizeof g_nat / sizeof g_nat[0]))
#include "cleo_enums.h"

enum { T_GAME = 0, T_SCRIPT = 1 };
typedef struct Js {
    char file[96], path[PATH_MAX], mod[64];
    Cleo *rec;
    pthread_t th;
    pthread_mutex_t mx;
    pthread_cond_t cv;
    int turn, done, terminate, parked;
    uint32_t wake;
    JSGlobalContextRef ctx;
} Js;

#define MAX_JS 32
static Js *g_js[MAX_JS];
static int g_njs;
static __thread Js *t_self; // the script running on this thread

// helpers
static char *js_cstr(JSContextRef ctx, JSValueRef v) {
    JSStringRef s = JSValueToStringCopy(ctx, v, NULL);
    if (!s) return strdup("");
    size_t n = JSStringGetMaximumUTF8CStringSize(s);
    char *b = malloc(n);
    JSStringGetUTF8CString(s, b, n);
    JSStringRelease(s);
    return b;
}

static JSValueRef js_throw(JSContextRef ctx, JSValueRef *exc, const char *msg) {
    JSStringRef s = JSStringCreateWithUTF8CString(msg);
    JSValueRef arg = JSValueMakeString(ctx, s);
    JSStringRelease(s);
    *exc = JSObjectMakeError(ctx, 1, &arg, NULL);
    return JSValueMakeUndefined(ctx);
}

static void set_global(JSContextRef ctx, const char *name, JSValueRef v) {
    JSStringRef n = JSStringCreateWithUTF8CString(name);
    JSObjectSetProperty(ctx, JSContextGetGlobalObject(ctx), n, v, kJSPropertyAttributeNone, NULL);
    JSStringRelease(n);
}
static void set_fn(JSContextRef ctx, const char *name, JSObjectCallAsFunctionCallback fn) {
    JSStringRef n = JSStringCreateWithUTF8CString(name);
    set_global(ctx, name, JSObjectMakeFunctionWithCallback(ctx, n, fn));
    JSStringRelease(n);
}
static JSValueRef str_val(JSContextRef ctx, const char *s) {
    JSStringRef j = JSStringCreateWithUTF8CString(s);
    JSValueRef v = JSValueMakeString(ctx, j);
    JSStringRelease(j);
    return v;
}

// turns
// game thread: give the script its turn and wait until it hands it back
static void run_turn(Js *j) {
    pthread_mutex_lock(&j->mx);
    j->turn = T_SCRIPT;
    pthread_cond_broadcast(&j->cv);
    while (j->turn == T_SCRIPT) pthread_cond_wait(&j->cv, &j->mx);
    pthread_mutex_unlock(&j->mx);
}

// script thread: hand the turn back and wait for the next one
static void yield(Js *j) {
    pthread_mutex_lock(&j->mx);
    j->turn = T_GAME;
    pthread_cond_broadcast(&j->cv);
    while (j->turn == T_GAME) pthread_cond_wait(&j->cv, &j->mx);
    pthread_mutex_unlock(&j->mx);
}

static void park_forever(Js *j) { // a script that keeps going after it was told to stop
    pthread_mutex_lock(&j->mx);
    j->parked = 1;
    j->turn = T_GAME;
    pthread_cond_broadcast(&j->cv);
    for (;;) pthread_cond_wait(&j->cv, &j->mx);
}

// API
static JSValueRef f_wait(JSContextRef ctx, JSObjectRef fn, JSObjectRef self, size_t argc, const JSValueRef argv[], JSValueRef *exc) {
    Js *j = t_self;
    if (j->terminate) {
        if (j->terminate++ > 1) park_forever(j);
        return js_throw(ctx, exc, "script stopped (the game restarted its scripts)");
    }
    double ms = argc ? JSValueToNumber(ctx, argv[0], NULL) : 0;
    j->wake = mvc_time_ms() + (uint32_t)(ms > 0 ? ms : 0);
    yield(j);
    if (j->terminate) { j->terminate++; return js_throw(ctx, exc, "script stopped (the game restarted its scripts)"); }
    return JSValueMakeUndefined(ctx);
}

static JSValueRef f_log(JSContextRef ctx, JSObjectRef fn, JSObjectRef self, size_t argc, const JSValueRef argv[], JSValueRef *exc) {
    char line[1024] = "";
    size_t o = 0;
    for (size_t i = 0; i < argc && o < sizeof line - 2; i++) {
        char *s = js_cstr(ctx, argv[i]);
        o += (size_t)snprintf(line + o, sizeof line - o, "%s%s", i ? " " : "", s);
        free(s);
    }
    mvc_log("[JS] %s: %s", t_self->file, line);
    return JSValueMakeUndefined(ctx);
}

static JSValueRef f_exit(JSContextRef ctx, JSObjectRef fn, JSObjectRef self, size_t argc, const JSValueRef argv[], JSValueRef *exc) {
    t_self->terminate = 2; // the next wait() parks; this exception unwinds the script
    return js_throw(ctx, exc, "exit()");
}

// __commands(): [[index, name, class, member, flags, outputs, output names], ...]
static JSValueRef f_commands(JSContextRef ctx, JSObjectRef fn, JSObjectRef self, size_t argc, const JSValueRef argv[], JSValueRef *exc) {
    JSValueRef *rows = malloc(sizeof(JSValueRef) * NNAT);
    for (int i = 0; i < NNAT; i++) {
        const Native *n = &g_nat[i];
        JSValueRef cols[7] = {JSValueMakeNumber(ctx, i), str_val(ctx, n->name), str_val(ctx, n->cls), str_val(ctx, n->member),
                              JSValueMakeNumber(ctx, n->flags), str_val(ctx, n->out), str_val(ctx, n->names)};
        rows[i] = JSObjectMakeArray(ctx, 7, cols, NULL);
    }
    JSValueRef a = JSObjectMakeArray(ctx, NNAT, rows, NULL);
    free(rows);
    return a;
}

static void put_u16(uint8_t *b, size_t *o, int v) { b[(*o)++] = (uint8_t)v; b[(*o)++] = (uint8_t)(v >> 8); }
static void put_i32(uint8_t *b, size_t *o, int32_t v) { memcpy(b + *o, &v, 4); *o += 4; }

static int put_value(JSContextRef ctx, JSValueRef v, char kind, int game_cmd, uint8_t *b, size_t *o, size_t cap) {
    if (*o + 260 > cap) return -1;
    if (kind == 's' || (kind == 'a' && JSValueIsString(ctx, v))) {
        char *s = js_cstr(ctx, v);
        size_t n = strlen(s);
        if (game_cmd && kind == 's') { memset(b + *o, 0, 8); memcpy(b + *o, s, n < 7 ? n : 7); *o += 8; } // VC text key
        else { if (n > 255) n = 255; b[(*o)++] = 0x0E; b[(*o)++] = (uint8_t)n; memcpy(b + *o, s, n); *o += n; }
        free(s);
        return 0;
    }
    double d = JSValueToNumber(ctx, v, NULL);
    if (kind == 'f' || (kind == 'a' && d != (double)(int32_t)d)) {
        float f = (float)d;
        b[(*o)++] = 6;
        memcpy(b + *o, &f, 4);
        *o += 4;
    } else {
        b[(*o)++] = 1;
        put_i32(b, o, d >= 4294967295.0 ? -1 : d > 2147483647.0 ? (int32_t)(uint32_t)d : (int32_t)d);
    }
    return 0;
}

// __enums(): the library's enums as JSON
static JSValueRef f_enums(JSContextRef ctx, JSObjectRef fn, JSObjectRef self, size_t argc, const JSValueRef argv[], JSValueRef *exc) {
    return str_val(ctx, CLEO_ENUMS_JSON);
}

// __native(index, ...args) -> [condition, result1, result2...]
static JSValueRef f_native(JSContextRef ctx, JSObjectRef fn, JSObjectRef self, size_t argc, const JSValueRef argv[], JSValueRef *exc) {
    int idx = argc ? (int)JSValueToNumber(ctx, argv[0], NULL) : -1;
    if (idx < 0 || idx >= NNAT) return js_throw(ctx, exc, "unknown command");
    const Native *n = &g_nat[idx];
    uint8_t code[1024];
    size_t o = 0;
    put_u16(code, &o, n->op);
    size_t ai = 1;
    int nin = (int)strlen(n->in), game_cmd = n->op < 0x05DC;
    for (int k = 0; k < nin; k++) {
        if (n->in[k] == 'a') {
            for (; ai < argc; ai++) if (put_value(ctx, argv[ai], 'a', game_cmd, code, &o, sizeof code)) return js_throw(ctx, exc, "too many arguments");
            break;
        }
        if (ai >= argc) {
            char m[160];
            snprintf(m, sizeof m, "%s needs %d argument(s)", n->name, nin);
            return js_throw(ctx, exc, m);
        }
        if (put_value(ctx, argv[ai++], n->in[k], game_cmd, code, &o, sizeof code)) return js_throw(ctx, exc, "arguments too long");
    }
    int nout = 0;
    char kinds[16];
    for (const char *p = n->out; *p && nout < 16; nout++) { // outputs go to locals 0@, 1@...
        kinds[nout] = *p == 'f' && (p[1] == ',' || !p[1]) ? 'f' : *p == 'i' && (p[1] == ',' || !p[1]) ? 'i' : 'h';
        code[o++] = 3;
        put_u16(code, &o, nout);
        p = strchr(p, ',');
        if (!p) { nout++; break; }
        p++;
    }
    if (strchr(n->in, 'a')) code[o++] = 0; // end of the variable-length list
    int32_t out[16] = {0};
    int cond = cleo_js_exec(t_self->rec, code, o, out, nout);
    JSValueRef vals[17];
    vals[0] = JSValueMakeBoolean(ctx, cond);
    for (int i = 0; i < nout; i++) {
        if (kinds[i] == 'f') { float f; memcpy(&f, &out[i], 4); vals[i + 1] = JSValueMakeNumber(ctx, f); }
        else vals[i + 1] = JSValueMakeNumber(ctx, out[i]);
    }
    return JSObjectMakeArray(ctx, (size_t)nout + 1, vals, NULL);
}

// __mem(size, float, address[, value]) : read when no value is given
static JSValueRef f_mem(JSContextRef ctx, JSObjectRef fn, JSObjectRef self, size_t argc, const JSValueRef argv[], JSValueRef *exc) {
    if (argc < 3) return js_throw(ctx, exc, "Memory: missing address");
    int size = (int)JSValueToNumber(ctx, argv[0], NULL), sgn = size < 0, isf = JSValueToBoolean(ctx, argv[1]);
    if (sgn) size = -size;
    uint32_t a = (uint32_t)(int64_t)JSValueToNumber(ctx, argv[2], NULL);
    int write = argc > 3;
    uint8_t *p = cleo_js_addr(t_self->rec, a, (uint32_t)size, write);
    if (write) {
        double d = JSValueToNumber(ctx, argv[3], NULL);
        if (!p) return JSValueMakeUndefined(ctx);
        if (isf) { float f = (float)d; memcpy(p, &f, 4); }
        else { int64_t v = (int64_t)d; memcpy(p, &v, (size_t)size); }
        return JSValueMakeUndefined(ctx);
    }
    if (!p) return JSValueMakeNumber(ctx, 0);
    if (isf) { float f; memcpy(&f, p, 4); return JSValueMakeNumber(ctx, f); }
    uint32_t u = 0;
    memcpy(&u, p, (size_t)size);
    double r = u;
    if (sgn) r = size == 1 ? (int8_t)u : size == 2 ? (int16_t)u : (int32_t)u;
    return JSValueMakeNumber(ctx, r);
}

static const char *PRELUDE =
    "(function () {\n"
    "  const rows = __commands(), byName = {};\n"
    "  class ScriptObject { constructor(handle) { this.handle = handle; } valueOf() { return this.handle; }\n"
    "    toString() { return this.constructor.name + '(' + this.handle + ')'; } }\n"
    "  const wrap = (cls, h) => (h === -1 || h === undefined) ? undefined : (globalThis[cls] ? new globalThis[cls](h) : h);\n"
    "  function shape(c, r) {\n"
    "    const cond = r[0], outs = c.outs ? c.outs.split(',') : [];\n"
    "    if (!outs.length) return (c.flags & 1) ? cond : undefined;\n"
    "    if ((c.flags & 1) && !cond) return undefined;\n"
    "    const vals = outs.map((k, i) => (k === 'i' || k === 'f') ? r[i + 1] : wrap(k, r[i + 1]));\n"
    "    if (vals.length === 1) return vals[0];\n"
    "    const names = c.names.split(','), o = {};\n"
    "    vals.forEach((v, i) => o[names[i] || i] = v);\n"
    "    return o;\n"
    "  }\n"
    "  for (const [i, name, cls, member, flags, outs, names] of rows) {\n"
    "    const c = { i, name, flags, outs, names };\n"
    "    if (!byName[name]) byName[name] = c;\n"
    "    if (!cls || !member) continue;\n"
    "    let C = globalThis[cls];\n"
    "    if (!C) { C = class extends ScriptObject {}; Object.defineProperty(C, 'name', { value: cls }); globalThis[cls] = C; }\n"
    "    if (flags & 6) { if (!C[member]) C[member] = (...a) => shape(c, __native(i, ...a)); }\n"
    "    else { const m = member[0].toLowerCase() + member.slice(1);\n"
    "      if (!C.prototype[m]) C.prototype[m] = function (...a) { return shape(c, __native(i, this.handle, ...a)); }; }\n"
    "  }\n"
    "  globalThis.native = (name, ...a) => { const c = byName[name]; if (!c) throw new Error('unknown command ' + name); return shape(c, __native(c.i, ...a)); };\n"
    "  const M = {};\n"
    "  for (const [n, size, f] of [['I8', -1, 0], ['U8', 1, 0], ['I16', -2, 0], ['U16', 2, 0], ['I32', -4, 0], ['U32', 4, 0], ['Float', 4, 1]]) {\n"
    "    M['Read' + n] = (addr, vp) => __mem(size, f, addr);\n"
    "    M['Write' + n] = (addr, value, vp) => __mem(size, f, addr, value);\n"
    "  }\n"
    "  const f32 = new Float32Array(1), i32 = new Int32Array(f32.buffer);\n"
    "  const conv = { I8: v => (v << 24) >> 24, U8: v => v & 0xFF, I16: v => (v << 16) >> 16, U16: v => v & 0xFFFF,\n"
    "    I32: v => v | 0, U32: v => v >>> 0, Float: v => { i32[0] = v; return f32[0]; } };\n"
    "  M.ToFloat = conv.Float; M.FromFloat = v => { f32[0] = v; return i32[0]; };\n"
    "  for (const k of ['I8', 'U8', 'I16', 'U16', 'I32', 'U32']) M['To' + k] = conv[k];\n"
    "  // Memory.Fn: game functions with their arguments in normal order (CLEO's opcodes take them reversed)\n"
    "  const call = (addr, args) => native('CALL_FUNCTION_RETURN', addr, args.length, args.length, ...[...args].reverse()) | 0;\n"
    "  const method = (addr, obj, args) => native('CALL_METHOD_RETURN', addr, +obj, args.length, 0, ...[...args].reverse()) | 0;\n"
    "  const Fn = {};\n"
    "  for (const t of ['', 'I8', 'U8', 'I16', 'U16', 'I32', 'U32', 'Float']) {\n"
    "    const c = t ? conv[t] : (v => v);\n"
    "    Fn['Cdecl' + t] = Fn['Stdcall' + t] = addr => (...args) => c(call(addr, args));\n"
    "    Fn['Thiscall' + t] = (addr, obj) => (...args) => c(method(addr, obj, args));\n"
    "  }\n"
    "  M.Fn = Fn;\n"
    "  // Memory also has the library's commands (Memory.CallFunction, Memory.Allocate...)\n"
    "  globalThis.Memory = Object.assign(globalThis.Memory || {}, M);\n"
    "  const E = JSON.parse(__enums());\n"
    "  for (const k in E) {\n"
    "    if (!(k in globalThis)) globalThis[k] = Object.freeze(E[k]);\n"
    "    else for (const m in E[k]) if (!(m in globalThis[k])) globalThis[k][m] = E[k][m]; // a class of the same name\n"
    "  }\n"
    "  globalThis.showTextBox = (text) => native('PRINT_HELP_STRING', String(text));\n"
    "  globalThis.asyncWait = (ms) => { wait(ms); return Promise.resolve(); };\n"
    "})();\n";

// threads
static void report(Js *j, JSContextRef ctx, JSValueRef exc) {
    if (j->terminate) return; // the error we threw to stop it
    char *msg = js_cstr(ctx, exc);
    int l = 0;
    if (JSValueIsObject(ctx, exc)) { // the script's own line: the first stack frame in its file
        JSStringRef sn = JSStringCreateWithUTF8CString("stack"), ln = JSStringCreateWithUTF8CString("line");
        JSValueRef st = JSObjectGetProperty(ctx, (JSObjectRef)exc, sn, NULL);
        char *stack = st && JSValueIsString(ctx, st) ? js_cstr(ctx, st) : NULL;
        char *at = stack ? strstr(stack, j->path) : NULL;
        if (at && at[strlen(j->path)] == ':') l = atoi(at + strlen(j->path) + 1);
        if (!l) {
            JSValueRef line = JSObjectGetProperty(ctx, (JSObjectRef)exc, ln, NULL);
            if (line && JSValueIsNumber(ctx, line)) l = (int)JSValueToNumber(ctx, line, NULL);
        }
        free(stack);
        JSStringRelease(sn);
        JSStringRelease(ln);
    }
    mvc_log_error("[JS] %s%s%d: %s", j->file, l ? " line " : "", l, msg);
    free(msg);
}

static void *script_thread(void *arg) {
    Js *j = arg;
    t_self = j;
    pthread_mutex_lock(&j->mx);
    while (j->turn == T_GAME) pthread_cond_wait(&j->cv, &j->mx); // the first turn
    pthread_mutex_unlock(&j->mx);
    JSGlobalContextRef ctx = j->ctx;
    if (!j->terminate) {
        FILE *f = fopen(j->path, "rb");
        char *src = NULL;
        long n = 0;
        if (f) { fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET); src = malloc((size_t)n + 1); if (src) { n = (long)fread(src, 1, (size_t)n, f); src[n] = 0; } fclose(f); }
        if (src) {
            JSValueRef exc = NULL;
            JSStringRef pre = JSStringCreateWithUTF8CString(PRELUDE);
            JSEvaluateScript(ctx, pre, NULL, NULL, 1, &exc);
            JSStringRelease(pre);
            if (exc) report(j, ctx, exc);
            JSStringRef code = JSStringCreateWithUTF8CString(src), url = JSStringCreateWithUTF8CString(j->path);
            exc = NULL;
            JSEvaluateScript(ctx, code, NULL, url, 1, &exc);
            if (exc) report(j, ctx, exc);
            JSStringRelease(code);
            JSStringRelease(url);
            free(src);
        } else mvc_log_error("[JS] couldn't read %s", j->path);
    }
    pthread_mutex_lock(&j->mx);
    j->done = 1;
    j->turn = T_GAME;
    pthread_cond_broadcast(&j->cv);
    pthread_mutex_unlock(&j->mx);
    return NULL;
}

int js_start(const char *path, const char *mod) {
    if (g_njs >= MAX_JS) return 0;
    Js *j = calloc(1, sizeof *j);
    snprintf(j->path, sizeof j->path, "%s", path);
    const char *b = strrchr(path, '/');
    snprintf(j->file, sizeof j->file, "%s", b ? b + 1 : path);
    snprintf(j->mod, sizeof j->mod, "%s", mod ? mod : "");
    j->rec = cleo_js_record(path, mod);
    if (!j->rec) { free(j); return 0; }
    pthread_mutex_init(&j->mx, NULL);
    pthread_cond_init(&j->cv, NULL);
    j->ctx = JSGlobalContextCreateInGroup(NULL, NULL);
    JSContextRef ctx = j->ctx;
    set_fn(ctx, "wait", f_wait);
    set_fn(ctx, "log", f_log);
    set_fn(ctx, "exit", f_exit);
    set_fn(ctx, "__commands", f_commands);
    set_fn(ctx, "__native", f_native);
    set_fn(ctx, "__mem", f_mem);
    set_fn(ctx, "__enums", f_enums);
    set_global(ctx, "GAME", str_val(ctx, "vc"));
    set_global(ctx, "HOST", str_val(ctx, "macvc"));
    set_global(ctx, "__filename", str_val(ctx, j->file));
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", path);
    char *sl = strrchr(dir, '/');
    if (sl) *sl = 0;
    set_global(ctx, "__dirname", str_val(ctx, dir));
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 8u << 20);
    if (pthread_create(&j->th, &at, script_thread, j)) { JSGlobalContextRelease(j->ctx); free(j); return 0; }
    pthread_attr_destroy(&at);
    g_js[g_njs++] = j;
    return 1;
}

void js_tick(void) {
    uint32_t now = mvc_time_ms();
    for (int i = 0; i < g_njs; i++) {
        Js *j = g_js[i];
        if (!j->done && !j->parked && (int32_t)(now - j->wake) >= 0) run_turn(j);
    }
}

void js_stop_all(void) {
    for (int i = 0; i < g_njs; i++) {
        Js *j = g_js[i];
        if (!j->done && !j->parked) { j->terminate = 1; run_turn(j); }  // wait() throws; the script unwinds
        if (j->done) {
            pthread_join(j->th, NULL);
            JSGlobalContextRelease(j->ctx);
            pthread_mutex_destroy(&j->mx);
            pthread_cond_destroy(&j->cv);
            free(j);
        } // a parked script keeps its thread: it ignored being stopped
    }
    g_njs = 0;
}
