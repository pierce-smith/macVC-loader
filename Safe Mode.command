#!/bin/zsh
# double click to start macVC with no plugins (the console still works).
for c in "$HOME/Desktop/macVC.app" "/Applications/macVC.app" "$HOME/Applications/macVC.app"; do
  [[ -d $c ]] && { open --env MVC_SAFE_MODE=1 "$c"; exit 0; }
done
echo "macVC.app not found on the Desktop or in Applications."
