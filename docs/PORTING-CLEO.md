# porting a cleo script

most cleo scripts don't need porting at all. the cleo runner (`loader/cleo.c`) runs windows `.cs` files as is on the game's own interpreter. this is for the ones that don't run, or only half run, and for turning a script into a real plugin, which is what i did with Gasoline.

## 1. check it

```
python3 tools/cleo_compat.py vc.json Script.cs
```

`vc.json` is Sanny Builder's opcode library (github.com/sannybuilder/library, `vc/vc.json`). you'll get one of:

- works: drop it in `GameData/CLEO` and you're done
- partly: it runs but some addresses or functions get skipped, and the log (`[CLEO] ... skipped`) says which. a lot of the time it's just a struct field or game variable that isn't mapped yet. map it (step 3) and the script works unchanged
- needs a port: it writes x86 machine code into gta-vc.exe. that's never running on ARM, so the feature has to be redone natively (step 4)
- won't run: it needs opcodes from a windows `.cleo` plugin. either add those opcodes to the runner or port the script

it's a static check, so addresses a script works out while running only show up in the game log. test it in game too.

## 2. read the bytecode

```
python3 tools/csdis.py vc.json Script.cs            # full listing
python3 tools/csdis.py vc.json Script.cs --summary  # just the cleo opcodes it uses
```

a compiled .cs is the same bytecode as main.scm: a 2 byte opcode (bit 15 = NOT) then typed params (`01` int32, `04` int8, `05` int16, `06` float, `02` global var, `03` local var, `0E` string with a length byte, `09` 8 byte text label). labels are negative offsets from the start of the file, csdis shows them as `@<hex>`.

what to look for:
- `0A8C`/`05DF` (WRITE_MEMORY) to anything under `0x600000`. those are x86 code patches. disassemble that bit of gta-vc.exe to see what it's patching
- `0A8D`/`05E0` (READ_MEMORY) and writes anywhere else. those are game variables and struct fields, and they get translated by name
- `0AA5`-`0AA8` (CALL_FUNCTION) are game functions by windows address, translated through `loader/cleo_funcs.h`
- `0AC6`/`0AC7` and pointer math, scripts walking structs themselves

## 3. map a missing address

windows addresses go through 3 tables, built from plugin-sdk's VC data and macVC's own symbols, so a macVC update can't quietly break them:

| table | what's in it |
|---|---|
| `loader/cleo_addrs.h` | game variables |
| `loader/cleo_funcs.h` | game functions and their arm64 arguments |
| `loader/cleo_fields.h` | struct fields of peds, cars, objects and player info |

to rebuild them, `tools/gen_cleo_addrs.py`, `gen_cleo_funcs.py` and `cleo_layout/gen_cleo_fields.py` need a plugin-sdk checkout (and the macVC binary for the first two), and `gen_cleo_ops.py`, `gen_cleo_natives.py` and `gen_cleo_enums.py` just need Sanny Builder's `vc.json` / `enums.json`. usage is at the top of each one. a function plugin-sdk has the wrong name for goes in the `FIXES` list in `gen_cleo_funcs.py`.

if you add a struct field, check its macVC offset in the game's code first. a wrong field offset will corrupt game memory.

## 4. port it to a plugin

when a script patches x86 code, port the feature itself:

1. write down what the script does from the disassembly: its loop, its checks, what memory it reads and writes, what code it patches
2. find the game functions involved in macVC (`sym` in the console). a script that patches a compare inside some function becomes a hook or midhook on that function
3. keep the data exactly: coords, prices, timings, text keys. Gasoline's 7 stations, pump zones, prices and burn rate came straight out of the script's bytecode
4. write the plugin (`tools/new-plugin.sh Name`). script loops turn into `mvc_on_tick`, `wait` timers turn into checks against `mvc_time_ms()`, text keys turn into `mvc_text_set` or an .fxt, settings turn into Mods menu rows
5. stop the original script from also running. add it to the native replacement list in `cleo.c` (like `[VC]Gasoline.cs` -> Gasoline) so having both installed doesn't double up

## 5. or just redo it in javascript

if a script only uses normal commands you can also rewrite it as a `.js` in `GameData/CLEO`, CLEO Redux style:

```js
while (true) {
    wait(250);
    const p = new Player(0);
    if (p.isPlaying() && Pad.IsKeyPressed(KeyCode.F7)) Text.PrintHelpString("hello");
}
```

enums (`KeyCode.F7`, `WeaponType.Pistol`), `Memory.Read*/Write*` with windows addresses, and `Memory.Fn.Cdecl(addr)(...)` for game functions all work. the readme's javascript section has the rest.
