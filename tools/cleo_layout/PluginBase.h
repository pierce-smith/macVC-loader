#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <numeric>
#define PLUGIN_API
#define PLUGIN_VARIABLE
#define VALIDATE_SIZE(s, n)
#define VALIDATE_OFFSET(s, m, o)
#define PLUGIN_NO_DEFAULT_CONSTRUCTION(c)
#define PLUGIN_NO_DEFAULT_CONSTRUCTION_VIRTUALBASE(c)
#define VTABLE_DESC(...)
#define __cdecl
#define __stdcall
#define __thiscall
#define __fastcall
#define __forceinline inline
#define __declspec(x)
#define GAME_10EN 0
namespace plugin {
struct dummy_func_t {}; static dummy_func_t dummy;
template <unsigned A, typename... T> void Call(T...) {}
template <typename R, unsigned A, typename... T> R CallAndReturn(T...) { return R(); }
template <unsigned A, typename... T> void CallMethod(T...) {}
template <typename R, unsigned A, typename... T> R CallMethodAndReturn(T...) { return R(); }
template <unsigned A, typename... T> void CallDyn(T...) {}
}
#define SUPPORTED_10EN 
#define SUPPORTED_10EN_11EN 
#define SUPPORTED_10EN_11EN_STEAM 
#define _stricmp strcasecmp
#include <strings.h>
typedef signed char bool8;
