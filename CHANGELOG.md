# changelog

## 1.0

first release.

- plugin loader: native `.dylib` plugins from `GameData/plugins` and `GameData/scripts`, loaded through a stand-in `libmpg123.dylib` so the game binary is never patched
- ARM64 inline hooks, mid-function hooks with full register access, memory patches, pattern scanning, hook chaining between plugins
- plugin sdk: Options > Mods menu pages, save data next to the save file, text and fxt, sprites, 3d helpers, sound, gamepads and rumble, player/car/world helpers
- console with dev tools (`sym`, `mem`, `poke`, `watch`, `perf`, `find`, `copy`, hot reload) and reVC's hidden debug menu
- crash reports, safe mode, auto disabling a plugin that crashes on startup, plugin signing/quarantine warnings
- mod loader: file and gta3.img overlays, data file merging, readme lines, texture folders, texture packs, windows modloader.ini, live refresh
- limit adjuster and streaming memory fix
- cleo runner for windows `.cs` scripts with windows addresses mapped to macVC, and CLEO Redux style javascript
- startup movies and controller rumble, both of which macVC doesn't do
- bundled: Gasoline, GPS, More Gore
