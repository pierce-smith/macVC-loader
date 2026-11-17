# macVC loader

an asi loader for macVC (the reVC port for apple silicon macs), plus a mod loader, a cleo runner and a plugin sdk.

i made this because i wanted Big Cojones running on my mac and there was no way to load anything into macVC. on windows you drop an .asi in the folder and you're done. now you can do basically the same thing here.

what's in it:
- loads native `.dylib` plugins (the mac version of an .asi)
- mod loader for drag and drop file mods, works like LINK/2012's modloader on windows
- runs windows cleo `.cs` scripts as-is, and javascript scripts the way CLEO Redux does them
- a plugin sdk with hooks, patches, a real Options > Mods menu, save data, hud drawing, etc
- a console (~ key) with dev tools
- limit adjuster, controller rumble, the intro movies macVC skips

docs: [api](docs/API.md) / [cookbook](docs/COOKBOOK.md) / [porting asi mods](docs/PORTING-ASI.md) / [porting cleo scripts](docs/PORTING-CLEO.md) / [cleo compatibility](docs/COMPATIBILITY.md) / [contributing](CONTRIBUTING.md) / [changelog](CHANGELOG.md)

## install

**drag and drop (easiest):**
1. right click `macVC.app` > Show Package Contents > `Contents`
2. drag the 3 files from the zip's `Drag and Drop/Frameworks` into `Contents/Frameworks` and hit Replace for `libmpg123.dylib`. drag the files, not the folder
3. drag `Drag and Drop/GameData/plugins` and `scripts` into `Contents/Resources/GameData`
4. run `xattr -rd com.apple.quarantine /path/to/macVC.app` once so macos doesn't block it

**installer:** double click `macVC Loader Installer.app` (or `Install.command`). it looks for macVC.app on your desktop or in applications, or asks where it is. no dev tools needed.

**from source:** `./install.sh [path/to/macVC.app]`. needs `xcode-select --install`.

heads up: a macVC update replaces the Frameworks folder, so copy those 3 files back in (or rerun the installer) after you update.

**uninstall:** `./uninstall.sh` or the Uninstall button in the installer. by hand, delete `libmpg123.dylib` and `macVCLoader.dylib` from `Contents/Frameworks` and rename `libmpg123.real.dylib` back to `libmpg123.dylib`.

**safe mode:** double click `Safe Mode.command` to start with no plugins. `SafeMode=1` in the ini or an empty file called `GameData/SAFE_MODE` does the same.

## how it works

the game binary never gets touched. macVC already loads `libmpg123.dylib` (its mp3 decoder) from its Frameworks folder, so the loader ships a fake one with that name, same trick the windows asi loader pulls with `dinput8.dll`. the fake one passes everything through to the real decoder (`libmpg123.real.dylib`) and loads `macVCLoader.dylib` on the side. if the loader is missing the game still boots fine.

plugins go in `GameData/plugins/` or `GameData/scripts/`. they load in the order from `LoadOrder=` in the ini first, then alphabetically.

files the loader uses, all in `GameData`:
- `macVCLoader.ini` for settings. `[Plugins] Name=0` turns a plugin off. when i add a new setting it gets added to your existing ini next launch, your own stuff is left alone
- `macVCLoader.log` is the log, with the last 5 in `logs/`. it has the game's own output in it too
- `crashes/crash-<time>.log` if the game crashes: backtrace, which plugin it was probably in, every hook, last 200 log lines. send me this one if something breaks
- `macVCLoader.state`: if a plugin crashes the game during startup twice it gets turned off automatically. delete this file to give it another shot

plugins are native code with full access to the game, same as an .asi. the log and the `plugins` command show how each one is signed, and you get a warning if one was downloaded and is still quarantined or has no manifest. only run stuff you trust. the loader itself never goes online and never asks for your password.

## mod loader

drop mods in `GameData/modloader/<ModName>/`, any folder layout you want. the original files are never touched.

- a file with the same name or path as a game file replaces it. handling.cfg, hud.txd, audio, whatever. every file the game opens goes through reVC's `casepath()`, so one hook covers all of it
- `.dff` `.txd` `.col` `.ifp` files that aren't loose game files replace the entry with that name in gta3.img, same with anything in a folder called `gta3.img`. they get packed into `modloader/.cache/modloader.img` (only rebuilt when a mod changes) which gets loaded last, so mod files win
- when 2 mods change the same data file their changes get merged line by line against the original instead of one stomping the other. gta_vc.dat, .ide, .ipl, handling.cfg, carcols.dat, weapon.dat and the rest of windows modloader's list. .ipl coords get compared at 2 decimals so a map editor re-save doesn't look like every line changed. .col files merge per entry
- data lines in a mod's readme get applied like on windows: handling lines (`!` bikes, `%` boats, `$` flying), weapon.dat, car colours, default.ide car/ped lines, gta_vc.dat lines
- a mod's .gxt doesn't replace macVC's text file, because macVC's text has stuff the 2003 files don't (its graphics menu, controller names) and the menu breaks. only the strings the mod actually changed get applied
- gta_vc.dat: new map files get found by name anywhere in a mod, and a line pointing at a file that doesn't exist gets dropped instead of crashing the game
- when a few game files share a name, the one whose folders match the mod's folders best gets replaced

**texture folders:** a folder named like a txd (`models/hud.txd/`, `models/gta3.img/infernus.txd/`) full of tga, bmp, png or jpg images adds those textures to that txd, or replaces the ones with the same name.

**texture packs:** images in a `textures` folder anywhere in a mod (`modloader/HD Cars/textures/infernus.png`) replace that texture in every txd that has it, loose or in gta3.img. the first time, it indexes all the game's textures (takes a few seconds) and caches that in `modloader/.cache/textures.idx`. built txds get reused until you change an image, so a big pack doesn't slow down every launch. anything that doesn't match a game texture gets listed in the log, and `texfind <name>` tells you where a texture lives.

mod textures are stored uncompressed, so a 2048x2048 one eats 16 MB of streaming memory. `TextureMaxSize=1024` in the ini scales them down, and bump `StreamingMemory` if you run a big pack.

**settings:**
- in game: Options > Mods. every mod gets a page in the MOD selector with Enabled and Priority
- `[Mods]` in macVCLoader.ini: `ModName=0` turns it off, `ModName=1-100` sets priority (default 50, higher wins). this beats modloader.ini
- windows `modloader/modloader.ini` works too: profile Priority, IgnoreAllMods, ExcludeAllMods + IncludeMods, IgnoreMods, IgnoreFiles, ExclusiveMods
- folders starting with `.` or `_` are skipped

you can add, edit or remove mods while the game is paused. unpause (or type `mods refresh`) and it rescans. handling, weapons and timecyc reload on the spot. models, textures, maps and car colours need a restart since the game only reads those at startup, same as windows.

console: `mods`, `mods refresh`, `conflicts`, `modfile <game path>`, `texfind <texture>`

`.asi`/`.dll`/`.cleo` files in a mod can't run (windows code), they just get listed in the log. `.cs` scripts and `.fxt` text go to the cleo runner.

## cleo scripts and javascript

windows cleo `.cs` scripts run unchanged on the game's own script interpreter. `.js` scripts run like CLEO Redux. [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md) has which Big Cojones scripts work.

scripts go in `GameData/CLEO/` (plus .cs files and `CLEO/*.js` inside modloader mods). text goes in `CLEO/CLEO_TEXT/*.fxt`.

each script gets its own slot in the game's script list and its own copy of the code close enough to ScriptSpace for the interpreter to reach it. cleo labels are offsets from the start of the file so the loader handles the jumps for cleo scripts, every other standard opcode is the game's own code. scripts never go into your save, they restart from the top whenever a game starts or loads, like on windows.

cleo opcodes that are built in:
- call/return (`0AB1`/`0AB2`, `05F5`/`05F6`), label and variable pointers, starting and ending custom scripts
- memory read/write, ped/car/object pointers and handles, calling game functions (`0AA5`-`0AA8`, `05E1`-`05E4`, `POP_FLOAT`)
- keys (windows key codes), button emulation, cheat strings, widescreen check
- random ped/car/object in a sphere, weapon models, spawning a car
- car gears (`0AB7`/`0AB8`), engine and lights (`0ABD`-`0ABF`), model names (`0ADB`), `PLAY_ANIMATION` (`0673`)
- help/message/big text, formatted text, on screen text (`0608`/`0609`), `STRING_FORMAT`, `SCAN_STRING`
- ini files, files and folders, int and bit math, shared vars, audio streams, clipboard

anything else in Sanny Builder's opcode list gets skipped with a warning in the log (right now that's loading windows dlls and a couple audio plugin commands). an opcode nobody's heard of, from some windows .cleo plugin, stops that script with an error.

**windows addresses:** scripts use gta-vc.exe 1.0 addresses, which get mapped to macVC by name, so a macVC update moving things around doesn't break them.
- 402 game variables in `loader/cleo_addrs.h`, including pointer vars (`CTheScripts::pActiveScripts`, `CHeli::pHelis`) and `CWorld::Players`
- 1674 game functions in `loader/cleo_funcs.h`. calls use macVC's own arguments for the function (from its mangled name) in ARM64 registers, and pointers get translated
- 295 struct fields of peds, cars, objects and player info in `loader/cleo_fields.h`, from plugin-sdk's VC headers built for ARM64 (reVC keeps the original field order). i only included spans i checked against offsets in macVC's code. those structs get 32 bit stand-in pointers so scripts can do their usual pointer math

what can't work: scripts that patch x86 code. that code can't run on an ARM mac, period. those writes, unmapped fields and unknown functions get logged once and skipped, never written. those mods need a real port like Gasoline got.

settings: `[CLEO]` in the ini, `Script.cs=0` turns one off. a script that a native plugin already replaces (`[VC]Gasoline.cs`) doesn't get started.

locals: `0@`-`15@` plus timers `16@`/`17@` like the game. scripts written for more (some SA ports go up to `33@`) only overwrite their own slot.

**javascript:**
- runs on JavaScriptCore which is already on every mac, nothing to install
- each script gets its own thread but only runs during the game's script tick, and hands back control when it calls `wait()`. a script that never calls `wait()` freezes the game, same as windows
- `wait(ms)`, `log(...)`, `exit()`, `showTextBox(text)`, `native("COMMAND", ...args)`
- classes from Sanny Builder's library (970 commands). static members keep their name (`Car.Create(...)`), instance members are camelCase (`new Player(0).getChar().getCoordinates()` gives you `{x, y, z}`)
- conditions return true/false, handles come back as class instances
- `Memory.ReadI8/U8/I16/U16/I32/U32/Float(addr)` and `Memory.Write...(addr, value)` with windows addresses
- `Memory.Fn.Cdecl(addr)(...args)`, `Memory.Fn.CdeclFloat`, `Memory.Fn.Thiscall(addr, struct)(...args)` etc for game functions, args in normal order. `Memory.CallFunction` and the rest of the library's Memory commands work too
- `Memory.ToFloat/FromFloat/ToU8/ToI8/ToU16/ToI16/ToU32/ToI32`
- the enums: `WeaponType.Pistol`, `KeyCode.F7`, `PedType.Cop`, 58 of them
- `GAME` is `"vc"`, `HOST` is `"macvc"`, plus `__filename` and `__dirname`. errors get logged with the line number
- not there: CLEO Redux's permission system (every script can use everything) and `Memory.Fn.X64`, which is for 64 bit windows games anyway

console: `cleo` lists scripts, `cleo reload` restarts them.

tools:
- `tools/csdis.py vc.json script.cs` disassembles a script and lists the cleo opcodes it uses (`vc.json` is Sanny Builder's library, github.com/sannybuilder/library)
- `tools/cleo_compat.py vc.json *.cs` tells you if scripts will work and why not
- `tools/gen_cleo_*.py` rebuild the address/function/opcode tables in `loader/cleo_*.h`, see [docs/PORTING-CLEO.md](docs/PORTING-CLEO.md)
- `tests/cleo/make_selftest.py` builds `CLEOSelfTest.cs`, and `tests/cleo/JSSelfTest.js` is the js one. drop them in `GameData/CLEO` and load a game. they show "CLEO OK" / "JS OK" and write `Passed=1` to their ini

## limits and memory

basically open limit adjuster, for map and texture mods:
- buildings 7000 -> 40000, sector list nodes 50000 -> 200000, dummies 2340 -> 10000, collision models 4400 -> 12000
- simple objects 3885 + 4000, timed objects 385 + 1000
- streaming memory: macVC always picks 65 MB, which makes texture mods pop in and out constantly. `StreamingMemory=1024` (MB) fixes it
- usage gets logged once the map loads. txd slots are stuck at 1385, same as on windows

## startup movies

macVC skips the rockstar logo and title movies. the loader plays `movies/Logo.mpg` and `movies/GTAtitles.mpg` like the pc version did. enter, space, escape, a click or A/Start skips. `PlayMovies=0` turns them off.

## controller rumble

macVC has no vibration at all. the loader takes the game's own shakes and plays them on your controller, dualsense and xbox both, even with controller compatibility mode on. there's a Vibration row back under Options > Controller Setup. `GameRumble=0` turns it off. if macos shuffles your controllers around between launches, the loader keeps the game on the one named in macVC.ini (`JoystickName=`).

## console

~ (left of 1) opens it, and the game ignores input while it's open. pgup/pgdn scroll, home/end jump, tab completes, cmd+v pastes, cmd+c copies.

- info: `plugins`, `hooks`, `pads`, `rumble`, `pos`
- player: `tp`, `money`, `health`, `wanted`, `give`
- cars and world: `spawn`, `fix`, `time`, `weather`
- log: `find <text>` (just `find` shows everything again), `copy [lines|all]`
- dev: `sym`, `addr`, `mem`, `poke`, `watch`/`unwatch`, `perf`, `debugmenu`, `echo`, `clear`
- mods: `mods`, `conflicts`, `modfile`, `texfind`, `cleo`
- plugin dev: `reload <plugin>|all`, `unload`, `load`

macVC actually ships reVC's debug menu, it's just hidden. ctrl+m (or `debugmenu`) opens it: cheats, time and weather, spawning, camera and render stuff. plugins can add their own entries with the game's `DebugMenuAddCmd`, `DebugMenuAddInt32`, `DebugMenuAddFloat32` etc (grab them with `mvc_sym`, add from `MVC_EV_GAME_INIT`).

`open -a macVC.app --env MVC_EXEC="cmd;cmd"` runs console commands 2 seconds after launch.

## making plugins

`tools/new-plugin.sh MyMod` gives you a starter plugin in `plugins/mymod/`, and `./build.sh` builds it. everything is in `include/macvc.h`, [docs/API.md](docs/API.md) has the full list and [docs/COOKBOOK.md](docs/COOKBOOK.md) has examples.

the short version: declare it with `MVC_PLUGIN(...)`, export `MVCPluginInit()`, then
- hook stuff with `mvc_hook_sym("_ZN4CHud4DrawEv", fn, &orig)`. use the mangled name without the leading underscore `nm` shows. hooks from multiple plugins chain
- `mvc_midhook(addr, fn, user)` for the middle of a function, you get every register and can change them
- `mvc_write`, `mvc_nop`, `mvc_patch_branch`, `mvc_unpatch` for patches. overlapping patches from different plugins show up as conflicts in the log
- `mvc_find_pattern` for code with no symbol
- `mvc_on_tick` for logic, `mvc_on_hud`/`mvc_on_frame`/`mvc_on_radar` for drawing, `mvc_on_event` for game events
- `mvc_menu_*` adds real rows under Options > Mods, saved to your ini
- `mvc_save_set`/`mvc_save_get` saves data next to the save file (`GTAVCsfN.b.mvc`), never inside it, so pulling a mod can't break a save
- text, sprites, 3d lines/coronas/markers, sounds, gamepads, console commands, player/car/world helpers

`reload MyMod` in the console swaps in a rebuilt plugin without restarting.

if you're porting a windows mod, read [docs/PORTING-ASI.md](docs/PORTING-ASI.md) or [docs/PORTING-CLEO.md](docs/PORTING-CLEO.md).

## tests

- `tests/run.sh` runs the hook engine tests and fuzzes all the file parsers (ini, fxt, gxt, data merging, col, txd, cleo format strings) under ASan/UBSan, plus a js test and a texture pack test against your own gta3.img if you have macVC installed. `tests/run.sh 200000` fuzzes longer
- `tests/smoke.sh` launches the game, checks the log for errors and crashes, and quits. don't run it while you're playing
- `tests/selftest.c` is an in game api test, build it like the comment says
- CI builds with warnings as errors and runs `tests/run.sh` on every push

## bundled plugins

**Gasoline:** my port of the Gasoline v1.1 cleo mod from Big Cojones. the original 7 stations, fuel in gallons, and you have to actually stop at the pump and hold N (or dpad right) to fill up. settings in `GameData/plugins/Gasoline.ini`. F9 logs your position as a `Station=` line if you want to add stations.

**GPS:** gta iv style route to your map waypoint, on the radar and the pause map, using the game's own road pathfinding.

**More Gore:** not a port, this one's mine. vanilla vc only takes limbs off with a few guns, by chance, plus the katana finisher. this adds:
- melee killing blows take a part off, standing or on the ground
- shotguns take off what they hit depending on distance. always at point blank (3 m default), never past the gore range (15 m default)
- python headshots pop the head
- severed parts actually fall and bounce around, for 2 minutes (5 on extreme)
- a blood overhaul using the game's own blood: bullet spray out the far side, splatter on the ground, pools that spread, stumps that pump blood, wounded peds leaving trails, melee blood, and it all sticks around way longer than vanilla's 40 seconds
- it uses the game's own `CPed::RemoveBodyPart`, never the player and never someone sitting in a car

settings are under Options > Mods > More Gore (saved in `GameData/plugins/MoreGore.ini`): on/off, which melee weapons count, shotgun ranges, python headshots, which parts can come off, blood level and a toggle per blood effect. the game only has 48 decal slots shared with shadows, so More Gore keeps its own list of 512 blood decals and only hands the game the closest ones each frame.

## credits

- the gtamac team for macVC, and the re3/reVC people before them
- DK22Pac and everyone on plugin-sdk, the struct and address data came from there
- the Sanny Builder library for the opcode and enum data
- LINK/2012 for Mod Loader and Seemann for CLEO Redux, which this copies the behaviour of
- the original authors of the Big Cojones scripts and mods

no game files are in this repo or the release, you need your own copy of Vice City. the loader has none of reVC's source in it, it only looks up function names in your own macVC.
