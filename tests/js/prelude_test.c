// evaluates the JavaScript prelude (loader/cleo_js.c) with the real command
// table and enums but a fake __native, and checks the CLEO Redux surface:
// classes, native(), Memory (library commands + Read/Write + Fn + To/From), enums.
#include "../fuzz/common.h"
#include "../../loader/cleo_js.c"
const char *fuzz_name = "js";

static char g_last[256];
// fake __native: records the command and its arguments, returns [true, 0x40490FDB (3.14159f)]
static JSValueRef fake_native(JSContextRef ctx, JSObjectRef fn, JSObjectRef self, size_t argc, const JSValueRef argv[], JSValueRef *exc) {
    int idx = (int)JSValueToNumber(ctx, argv[0], NULL);
    size_t o = (size_t)snprintf(g_last, sizeof g_last, "%s", g_nat[idx].name);
    for (size_t i = 1; i < argc && o < sizeof g_last - 16; i++) o += (size_t)snprintf(g_last + o, sizeof g_last - o, " %g", JSValueToNumber(ctx, argv[i], NULL));
    JSValueRef v[2] = {JSValueMakeBoolean(ctx, 1), JSValueMakeNumber(ctx, 0x40490FDB)};
    return JSObjectMakeArray(ctx, 2, v, NULL);
}

static JSValueRef last_call(JSContextRef ctx, JSObjectRef fn, JSObjectRef self, size_t argc, const JSValueRef argv[], JSValueRef *exc) {
    return str_val(ctx, g_last);
}
static int g_fail;
static void expect(JSGlobalContextRef ctx, const char *expr, const char *want) {
    JSStringRef s = JSStringCreateWithUTF8CString(expr);
    JSValueRef exc = NULL, r = JSEvaluateScript(ctx, s, NULL, NULL, 1, &exc);
    JSStringRelease(s);
    char *got = js_cstr(ctx, exc ? exc : r);
    if (strcmp(got, want)) { printf("FAIL: %s -> %s (want %s)\n", expr, got, want); g_fail++; }
    free(got);
}

int main(void) {
    JSGlobalContextRef ctx = JSGlobalContextCreate(NULL);
    set_fn(ctx, "__commands", f_commands);
    set_fn(ctx, "__native", fake_native);
    set_fn(ctx, "__enums", f_enums);
    set_fn(ctx, "lastCall", last_call);
    JSStringRef pre = JSStringCreateWithUTF8CString(PRELUDE);
    JSValueRef exc = NULL;
    JSEvaluateScript(ctx, pre, NULL, NULL, 1, &exc);
    if (exc) { char *m = js_cstr(ctx, exc); printf("FAIL: prelude threw %s\n", m); return 1; }
    expect(ctx, "typeof Memory.CallFunction", "function");          // library command kept
    expect(ctx, "typeof Memory.Allocate", "function");
    expect(ctx, "typeof Memory.ReadU8", "function");
    expect(ctx, "Memory.ToFloat(0x40490FDB).toFixed(5)", "3.14159");
    expect(ctx, "Memory.FromFloat(1)", "1065353216");
    expect(ctx, "Memory.ToI8(255)", "-1");
    expect(ctx, "Memory.ToU16(-1)", "65535");
    expect(ctx, "Memory.Fn.CdeclFloat(0x4B8700)(1, 2, 3).toFixed(5)", "3.14159");
    expect(ctx, "(Memory.Fn.Cdecl(0x4B8700)(1, 2, 3), lastCall())", "CALL_FUNCTION_RETURN 4.94976e+06 3 3 3 2 1");
    expect(ctx, "(Memory.Fn.ThiscallU8(0x5551A0, 0x9000)(7, 8), lastCall())", "CALL_METHOD_RETURN 5.59146e+06 36864 2 0 8 7");
    expect(ctx, "WeaponType.Pistol", "17");
    expect(ctx, "PedType.Cop", "6");
    expect(ctx, "Object.isFrozen(WeaponType)", "true");
    expect(ctx, "typeof Car.Create", "function");
    expect(ctx, "typeof native", "function");
    expect(ctx, "typeof ImGuiCond", "undefined");
    printf("JS prelude: %s\n", g_fail ? "FAILED" : "passed");
    return g_fail != 0;
}
