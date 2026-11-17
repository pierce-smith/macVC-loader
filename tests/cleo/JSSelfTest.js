// JavaScript runtime self-test (CLEO Redux style API).
// copy into GameData/CLEO and load a game. after ~2.5 seconds it writes
// GameData/CLEO/JSSelfTest.ini ([Result] Passed=1, or Failed=<step>) and shows a help box.
// F8 then shows another help box (key input from a loop with wait()).
const INI = "CLEO\\JSSelfTest.ini";
function fail(step, why) {
    log("FAILED at step", step, why);
    IniFile.WriteInt(step, INI, "Result", "Failed");
    showTextBox("~r~JS self-test failed at step " + step);
    exit();
}

log("starting");
wait(2500);

// 1. game commands through classes: handles come back as objects, results as values
const player = new Player(0);
if (!player.isPlaying()) fail(1, "player isn't playing");
const ped = player.getChar();
if (!(ped instanceof Char)) fail(1, "getChar didn't return a Char");
const pos = ped.getCoordinates();
if (typeof pos.x !== "number" || (pos.x === 0 && pos.y === 0)) fail(1, "bad coordinates");
log("player at", pos.x.toFixed(1), pos.y.toFixed(1), pos.z.toFixed(1), "health", ped.getHealth(), "money", player.storeScore());

// 2. CLEO commands (ini files) through the same path
IniFile.WriteInt(42, INI, "Result", "Answer");
if (IniFile.ReadInt(INI, "Result", "Answer") !== 42) fail(2, "ini round trip");

// 3. memory with a Windows 1.0 address (CTimer::m_snTimeInMilliseconds)
const t1 = Memory.ReadI32(0x974B2C);
wait(100);
const t2 = Memory.ReadI32(0x974B2C);
if (!(t2 > t1)) fail(3, "game timer didn't advance: " + t1 + " -> " + t2);

// 4. native() by command name
if (native("GET_GAME_TIMER") <= 0) fail(4, "GET_GAME_TIMER");

IniFile.WriteInt(1, INI, "Result", "Passed");
showTextBox("JS OK: " + ped.getHealth() + " health, $" + player.storeScore());
log("passed");

while (true) {
    wait(0);
    if (Pad.IsKeyPressed(0x77)) { // F8
        showTextBox("~g~JS runtime: F8 pressed");
        wait(500);
    }
}
