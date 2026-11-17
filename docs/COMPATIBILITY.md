# cleo compatibility

which windows cleo scripts run on macVC through the cleo runner (`loader/cleo.c`) and why.

what the statuses mean:
- works: everything it uses is supported
- partly: it runs, but some opcodes, game memory or functions get skipped. each skip is logged once in `macVCLoader.log` (`[CLEO] ... skipped`)
- needs a port: it patches gta-vc.exe's x86 code, which can't run on an ARM mac. the feature has to be redone as a native plugin like Gasoline was
- won't run: it needs opcodes from a windows `.cleo` plugin (a dll). it stops with an error naming the opcode

## check a script yourself

```
python3 tools/cleo_compat.py vc.json path/to/*.cs
```

`vc.json` is Sanny Builder's opcode library (github.com/sannybuilder/library, `vc/vc.json`). it's a static check, so addresses a script works out while it's running can't be seen. the game log has the final say.

## Big Cojones (nexus mod 627)

checked with loader 1.0. ported natively: Gasoline (`plugins/gasoline`). tested in game: d_GoodGangs, VC.Walk, vcpd_ps2_teargas.

| script | status | notes |
|---|---|---|
| [VC]Gasoline.cs | needs a port | patches 1 x86 code address; 3 unmapped game addresses: 0x7E46F5, 0xA0FD04, 0xA0FD08 |
| Achievements.cs | won't run | needs a Windows .cleo plugin (0667, 0668); 21 game variables mapped; 1 game function mapped |
| d_chuvst.cs | needs a port | patches 4 x86 code addresses; 2 game variables mapped |
| d_GoodGangs.cs | works | - |
| d_moneyHUD.cs | partly | 1 unmapped game address: 0x697B4B |
| d_PoliceFix.cs | works | 8 game variables mapped |
| d_PS2pickup.cs | needs a port | patches 14 x86 code addresses |
| d_sandcastle.cs | works | - |
| d_weap_fix.cs | partly | 2 unmapped game addresses: 0x78368C, 0x783754 |
| Mix Sets.cs | needs a port | patches 49 x86 code addresses; 2 game variables mapped; 29 unmapped game addresses: 0x686E40, 0x686E44, 0x686E48, 0x686E4C; 1 game function mapped |
| SEAlarm.cs | needs a port | patches 5 x86 code addresses; 5 game variables mapped; 2 unmapped game addresses: 0x98410C, 0xA0CFE8; computes addresses from unmapped 0x6AD1B0, 0x6AD1B4 (check the log) |
| SEDetonator.cs | needs a port | patches 5 x86 code addresses |
| SEDrunk.cs | partly | 7 game variables mapped; 1 unmapped game address: 0xA10B63 |
| SEHavana.cs | partly | computes addresses from unmapped 0x945D30 (check the log) |
| SEPilotRanking.cs | needs a port | patches 32 x86 code addresses |
| SESlices.cs | needs a port | patches 3 x86 code addresses; 1 game variable mapped; 1 game function mapped |
| SETearGas.cs | needs a port | patches 5 x86 code addresses; computes addresses from unmapped 0x945D30 (check the log); 1 game function mapped |
| SEZoom.cs | needs a port | patches 2 x86 code addresses; 3 game variables mapped; 2 unmapped game addresses: 0x68FF04, 0x68FF0C |
| VC.Walk.cs | works | - |
| vcpd_ps2_teargas.cs | works | - |
| [VC]ChopperUpgrade.cs | needs a port | patches 5 x86 code addresses; 1 game variable mapped; 1 game function mapped |
| [VC]PoliceUpdate.cs | needs a port | patches 4 x86 code addresses |

notes:
- unmapped game addresses are windows addresses plugin-sdk has no name for: constants, text formats, entries inside the weapon table. those need adding by hand
- `[VC]Gasoline.cs` is replaced by the Gasoline plugin, so the runner doesn't start it while that's loaded
- SEDrunk (getting drunk at the Malibu and Pole Position) runs as is. its only unknown address, 0xA10B63, is a byte the script waits on to be 0. it reads as 0 on mac so the script is always on
- SESlices (the SLICES and HEADLESS cheats) patches x86 code so it does nothing here. More Gore covers the gore side of it
- SEHavana moves a pickup by walking the pickup array (0x945D30, 52 bytes a pickup on windows). that struct isn't mapped yet, so it reads 0s and does nothing. harmless, the fix just doesn't apply
- `Achievements.cs` uses opcodes 0667/0668 from `VCAchievementsSysBridge.cleo`, so that plugin needs porting first

## adding to this list

add a section per mod pack with the output of `cleo_compat.py` and what you actually saw in game.
