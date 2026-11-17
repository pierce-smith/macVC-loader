#!/bin/zsh
# double click in finder to install. looks for macVC.app on the desktop and in
# applications, asks where it is otherwise.
cd "${0:A:h}"
APP=""
for c in "$HOME/Desktop/macVC.app" "/Applications/macVC.app" "$HOME/Applications/macVC.app"; do
  [[ -d $c ]] && { APP=$c; break; }
done
if [[ -z $APP ]]; then
  APP=$(osascript -e 'POSIX path of (choose file of type {"app"} with prompt "Select macVC.app")' 2>/dev/null)
  [[ -z $APP ]] && { echo "Cancelled."; exit 1; }
fi
./install.sh "$APP"
echo
read -k1 "?Press any key to close."
