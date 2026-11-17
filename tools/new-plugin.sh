#!/bin/zsh
# creates a new plugin or script from a template.
#   tools/new-plugin.sh MyMod            -> plugins/mymod/  (GameData/plugins/MyMod.dylib)
#   tools/new-plugin.sh MyScript script  -> scripts/myscript/ (GameData/scripts/MyScript.dylib)
set -e
cd "${0:A:h}/.."
NAME=$1
KIND=${2:-plugin}
[[ -z $NAME || ! $NAME =~ ^[A-Za-z][A-Za-z0-9_]*$ ]] && { echo "usage: tools/new-plugin.sh Name [plugin|script]"; exit 1; }
[[ $KIND == script ]] && DIR=scripts/${NAME:l} || DIR=plugins/${NAME:l}
[[ -e $DIR ]] && { echo "$DIR already exists"; exit 1; }
mkdir -p $DIR
echo $NAME > $DIR/NAME
cat > $DIR/${NAME:l}.c <<EOF
// $NAME for macVC
#include <stdio.h>
#include "../../include/macvc.h"

MVC_PLUGIN("$NAME", "0.1", "you", "", "What $NAME does");

static int enabled = 1;
static int hotkey = 'K';

static void tick(void) {
    // game logic, once per game tick
    if (!enabled) return;
    if (mvc_key_pressed(hotkey)) {
        MVCVec3 p = mvc_player_pos();
        mvc_log("hotkey pressed at %.1f, %.1f, %.1f", p.x, p.y, p.z);
    }
}

static void hud(void) {
    // drawing, only while the HUD is visible
    if (!enabled) return;
    float u = mvc_screen_h() / 448.f;
    mvc_text(20 * u, 400 * u, "$NAME", 0.4f * u, 0.8f * u, (MVCColor){255, 150, 225, 255}, MVC_ALIGN_LEFT, 1, 1);
}

static void on_event(int event, void *arg) {
    (void)arg;
    if (event == MVC_EV_VEHICLE_ENTERED) mvc_log("got in a vehicle");
}

static void cmd(int argc, const char **argv) {
    (void)argc; (void)argv;
    enabled = !enabled;
    mvc_log("$NAME %s", enabled ? "on" : "off");
}

MVC_API void MVCPluginInit(void) {
    const char *ini = "$KIND""s/$NAME.ini";
    char v[32];
    if (mvc_ini_get(ini, "$NAME", "Enabled", v, sizeof v)) enabled = v[0] != '0';
    if (mvc_ini_get(ini, "$NAME", "Hotkey", v, sizeof v) && mvc_key_from_name(v) > 0) hotkey = mvc_key_from_name(v);

    int page = mvc_menu_page("$NAME");
    mvc_menu_toggle(page, "Enabled", &enabled, ini, "$NAME", "Enabled", NULL);
    mvc_menu_keybind(page, "Hotkey", &hotkey, ini, "$NAME", "Hotkey", NULL);

    mvc_on_tick(tick);
    mvc_on_hud(hud);
    mvc_on_event(on_event);
    mvc_console_command("${NAME:l}", "toggle $NAME", cmd);
    mvc_log("ready");
}
EOF
echo "Created $DIR - run ./build.sh (or ./install.sh) to build it."
