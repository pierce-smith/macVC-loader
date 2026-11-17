# contributing

bug reports, ports, docs and code are all welcome.

## before you start

- bugs: open an issue with the bug template. attach `GameData/macVCLoader.log`, and if the game crashed, the newest file in `GameData/crashes/`. say what macVC build you're on (it's at the top of the log) and what plugins and mods you have in
- bigger stuff: open an issue first so we can agree on how to do it before you write a ton of code
- no game files, ever: no gta3.img, txd, dff, gxt, audio, the macVC binary or anything pulled out of it. other people's mod files don't go in here either. tests that need game files read them from your own install (see `tests/run.sh`)

## building and testing

```
./build.sh                  # loader, bundled plugins, tools -> build/
MVC_WERROR=1 ./build.sh     # what CI runs, warnings are errors
tests/run.sh                # hook tests + parser fuzzers, no game needed
tests/run.sh 200000         # fuzz longer
tests/smoke.sh              # launches the game, checks the log, quits
```

run `tests/run.sh` before a pull request and `tests/smoke.sh` before a release.

## code

- C11 for the loader and plugins, objective-c only where apple's frameworks need it (`.m` files)
- 4 space indents, braces on the same line, lines up to about 130 characters
- comment the why, especially anything you found by reversing: which game function, which offset, how you confirmed it. nobody else saw what you saw in the disassembler
- no hardcoded struct offsets without a check. read them out of game code at startup (`mvc_first_load_imm`, `mvc_load_imm_from_base`, or a scan like `find_vehicle_offsets` in `cleo.c`) and turn the feature off with a log line if the check fails
- errors go in the log (`mvc_log_warn`/`mvc_log_error`) with enough info to actually do something about it: the file, the mod, what got skipped
- the loader never goes online, never asks for admin, and only writes inside the macVC app and the user's Library folder. keep it that way

## adding to the api

`include/macvc.h` is a promise. within one `MVC_API_VERSION`, functions only get added, never changed or removed.

1. open an issue with the api request template: what you're trying to do and why the current api can't
2. new functions go at the end of their section in `macvc.h`, with a comment saying what it does, what it returns when it fails, and which callbacks you can call it from
3. update `docs/API.md` to match
4. if it's something people will use a lot, add it to `docs/examples/cookbook.c` and `docs/COOKBOOK.md`
5. put it in `CHANGELOG.md`

## game symbols and addresses

- for plugins there's nothing to add, `mvc_sym` already finds every symbol in macVC by its mangled name
- for cleo scripts (windows addresses), the tables in `loader/cleo_*.h` come from plugin-sdk through `tools/gen_cleo_*.py`. fix the script and rerun it instead of editing the table by hand. struct fields have to be checked against an offset in macVC's code before they go in, because a wrong field corrupts game memory

## porting etiquette

most vc mods have authors who are still around, or at least people who remember them.

- ask first. get the original author's ok before you put out a port of their mod. if you can't reach them say so in your release, and take it down if they object
- credit them. original author in your `MVC_PLUGIN` manifest (`"Name (macVC port)"`), your readme and your release page, with a link to the original
- keep it faithful or rename it. a port should play like the original, same data, same rules. if you change how it plays, give it a different name so nobody thinks your design is theirs. that's why More Gore isn't called Severed Limbs
- don't reupload their files. ship your own code. if the port needs art, audio or data from the original, tell people where to get it

## releases

1. bump `MVC_LOADER_VERSION` in `include/macvc.h` and update `CHANGELOG.md`
2. `tests/run.sh` and `tests/smoke.sh` pass
3. `./package.sh` makes `dist/macVC-loader-<version>.zip` and its `.sha256`. put both up
