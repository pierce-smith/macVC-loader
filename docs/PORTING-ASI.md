# porting an .asi mod

a windows .asi is x86 code built against gta-vc.exe 1.0, so it's never going to run on a mac. porting it means rewriting it as an arm64 `.dylib` that does the same thing to macVC. this is roughly how i did Gasoline, Severed Limbs and More Gore.

if it's somebody else's mod, read the porting etiquette part of [CONTRIBUTING.md](../CONTRIBUTING.md) before you put it out.

## 1. figure out what the mod actually does

most .asi mods are a few byte patches and a hook or two. you need to know exactly which.

- if the source is out there, start with that. a lot of them use plugin-sdk, and those class and function names carry straight over to macVC
- no source? disassemble it. `xcrun llvm-objdump -d --x86-asm-syntax=intel Mod.asi` works on windows dlls too. look for:
  - writes into the game's range (`0x400000`-`0xC00000`). those are the patches. an `E8`/`E9` byte followed by an offset is a call/jmp patch, which means a hook
  - `VirtualProtect` calls, they come right before patches
  - `DllMain` or the init function, which sets everything up
- look at the game code it's patching. disassemble gta-vc.exe 1.0 around each address (`xcrun llvm-objdump -d --start-address=0x... --stop-address=0x... gta-vc.exe`) and figure out what function it is. plugin-sdk's `plugin_vc` headers have addresses for most of them, and `loader/cleo_funcs.h` maps 1674 of them to macVC names already
- check what vanilla does first. more than once "the mod does X" turned out to be the game doing it, or something the mod never did at all. compare patched vs unpatched

## 2. find the same code in macVC

macVC is reVC, a rebuild of the original source compiled for arm64, and it has symbols. nearly every function kept its name.

- `sym <words>` in the console searches the names (`sym ped remove body`), or `nm macVC.app/Contents/MacOS/macVC | c++filt` for everything
- in code use the mangled name without the leading underscore (`_ZN4CPed14RemoveBodyPartE7PedNodea`)
- `xcrun llvm-objdump -d --no-show-raw-insn --start-address=0x... macVC` shows the arm64 code. line it up against the x86 and reVC's source to find the matching spot
- inlining is different. reVC's compiler inlines small functions differently than msvc did in 2002, so a function the .asi hooked might not exist by itself in macVC. hook the caller instead, or midhook the spot where it got inlined

## 3. translate the patches

| windows (injector / plugin-sdk) | here |
|---|---|
| `injector::WriteMemory<T>(addr, v)` | `mvc_write(addr, &v, sizeof v)` for data, or rewrite the instruction for code |
| `injector::MakeNOP(addr, n)` | `mvc_nop(addr, instructions)` (4 bytes each) |
| `injector::MakeCALL` / `MakeJMP` | `mvc_hook` the function being called, or `mvc_patch_branch(at, dest, 1/0)` |
| `patch::RedirectCall` | hook the callee, or `mvc_midhook` at the call |
| inline `__asm` hook at an address | `mvc_midhook(at, fn, user)` and read/write registers in `MVCContext` |
| `plugin::Events::gameProcessEvent` | `mvc_on_tick` |
| `plugin::Events::drawHudEvent` | `mvc_on_hud` |
| `plugin::Events::drawingEvent` | `mvc_on_frame` |
| `plugin::Events::initRwEvent` | `MVC_EV_GAME_INIT` through `mvc_on_event` |
| `KeyPressed(VK_x)` | `mvc_key_down` / `mvc_key_pressed` with glfw keys |
| `CPools::ms_pPedPool` etc | `mvc_sym("_ZN6CPools11ms_pPedPoolE")` |
| reading an .ini | `mvc_ini_get` / `mvc_ini_set`, or just make it a Mods menu row (`mvc_menu_*`) |

constants: on x86 a constant is usually an immediate you can overwrite byte for byte (`push 0x41200000`). on arm64 it's built with one or two mov/movk, or loaded from a literal pool or a global. find out how this build gets the value, then rewrite the instruction (there's an example in the cookbook) or the data it loads.

structs: field offsets don't match windows. pointers are 8 bytes and the compilers lay things out differently.
- read offsets out of macVC's own code at startup with `mvc_first_load_imm` and `mvc_load_imm_from_base`. disassemble something small that uses the field (a getter, a constructor) and take the offset from the load or store
- `loader/cleo_fields.h` already maps 295 windows field offsets to macVC ones, all checked
- the helpers in `macvc.h` already know position, model, health, armour and money
- don't guess offsets. if you can't find one, log it and turn that feature off

calling conventions: windows `__thiscall` puts `this` in ecx and the rest on the stack, cdecl/stdcall are all stack. arm64 is all registers: x0-x7 for ints and pointers (`this` first), s0-s7/d0-d7 for floats, counted separately. declare a function pointer with the right signature and the compiler handles it. something that returns a `CVector` by value gives it back in s0-s2.

## 4. graphics

macVC draws with metal through librw, so direct3d calls from a windows mod have nothing to call into.

- 2d: `mvc_rect`, `mvc_text`, `mvc_sprite_draw`, or the game's own `CSprite2d`/`CFont`
- 3d: `mvc_on_render3d`, `mvc_line3d`, `mvc_corona`, `mvc_marker`, and the game's own renderware calls (`RwIm3D`, `RpClumpRender`) which librw turns into metal. More Gore's falling limbs get drawn that way
- shaders and post processing: not yet. that needs hooks into macVC's metal renderer, which means working it out with the macVC devs

## 5. test it

- `./build.sh`, copy the dylib into `GameData/plugins`
- `reload <plugin>` in the console reloads it without restarting
- `hooks` shows your hooks and patches, `perf` shows what they cost per frame
- if it crashes you get `GameData/crashes/crash-<time>.log`, and it names the plugin the crash was in

## example: Severed Limbs

`SeveredLimbs.VC.asi` hides heads and limbs that got shot off, which vice city's skinned peds otherwise keep showing.

1. the .asi hooked `0x4FEA87`, inside a CPed function right after the skeleton update. it read `m_nBodyPartBleeding` (windows `+0x56A`) and scaled that part's bones to zero with `RwMatrixScale`
2. in reVC the skeleton update is `CEntity::UpdateRpHAnim`, so the port hooks that, calls the original, then scales the bones
3. the clump, `m_nBodyPartBleeding` and the ped size all get read from macVC's code at startup: clump `0x58`, part `0x6BE`, ped size `0x8D0`
4. the bone ids (head 5, arms 22-25 and 32-35, legs 41-43 and 51-53) are just data, so those carried over as is

More Gore came out of that port and does the same bone hiding itself now.
