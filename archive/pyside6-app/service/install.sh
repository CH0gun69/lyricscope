#!/usr/bin/env bash
# Install (or refresh) the user service that starts LyricScope with DeaDBeeF.
#
#   ./service/install.sh            install and start
#   ./service/install.sh --uninstall  stop and remove
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="$(dirname "$HERE")"
UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
UNIT="lyricscope-watch.service"

if [[ "${1:-}" == "--uninstall" ]]; then
    systemctl --user disable --now "$UNIT" 2>/dev/null || true
    rm -f "$UNIT_DIR/$UNIT"
    systemctl --user daemon-reload
    echo "removed $UNIT"
    exit 0
fi

mkdir -p "$UNIT_DIR"
sed -e "s|@WATCHER@|$HERE/watcher.py|g" \
    -e "s|@APP_DIR@|$APP_DIR|g" \
    "$HERE/$UNIT" > "$UNIT_DIR/$UNIT"

systemctl --user daemon-reload
systemctl --user enable --now "$UNIT"

echo "installed $UNIT_DIR/$UNIT"
systemctl --user --no-pager status "$UNIT" | head -5
