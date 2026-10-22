#!/bin/bash
# Puts the shadPS4-infamous folder app in place, finds the games and adds Steam shortcuts.
# The app needs no installing: extracting the release and running shadPS4.sh is enough. This
# script is the one-line way to get it, and the update path of installs older than the folder app.
#
#   bash <(curl -fsSL https://github.com/duartebmarques/shadPS4/releases/latest/download/install.sh)
#
# Options:
#   --dir DIR    the app folder (default: ~/Applications/shadPS4-infamous; an older install there
#                keeps its user folder: saves, controls, patch choices)
#   --no-steam   do not add Steam shortcuts
#   -y, --yes    answer yes to every question
set -euo pipefail

REPO="duartebmarques/shadPS4"
DIR="$HOME/Applications/shadPS4-infamous"
STEAM=1
YES=()
SRC="$(cd "$(dirname "$(readlink -f "$0")")" 2>/dev/null && pwd || true)"

# Started from a file manager: reopen in a terminal so the questions can be answered.
if [ ! -t 0 ] && [ -z "${INFAMOUS_IN_TERM:-}" ] && [ -f "$0" ]; then
    export INFAMOUS_IN_TERM=1
    command -v konsole >/dev/null && exec konsole --hold -e bash "$0" "$@"
fi

die() { echo "error: $*" >&2; exit 1; }
info() { echo "==> $*"; }

while [ $# -gt 0 ]; do
    case "$1" in
    --dir) DIR="$2"; shift ;;
    --no-steam) STEAM=0 ;;
    -y | --yes) YES=(-y) ;;
    --update) ;; # installs before the folder app called this with --update
    -h | --help) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown option: $1 (see --help)" ;;
    esac
    shift
done

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
if [ -n "$SRC" ] && [ -x "$SRC/shadPS4.sh" ] && [ -d "$SRC/app" ]; then
    rel="$SRC"
else
    command -v curl >/dev/null || die "curl is required"
    command -v unzip >/dev/null || die "unzip is required"
    url="$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" |
        grep -o '"browser_download_url": *"[^"]*-linux\.zip"' | head -1 | cut -d'"' -f4)"
    [ -n "$url" ] || die "could not find the latest release"
    info "Downloading $(basename "$url")..."
    curl -fL --progress-bar -o "$work/release.zip" "$url"
    unzip -q "$work/release.zip" -d "$work"
    rel="$work/shadPS4-infamous"
fi

mkdir -p "$DIR"
DIR="$(cd "$DIR" && pwd)"
if [ "$rel" != "$DIR" ]; then
    info "Installing into $DIR"
    rm -rf "$DIR/app.prev"
    [ -d "$DIR/app" ] && mv "$DIR/app" "$DIR/app.prev"
    rm -rf "$DIR/dist-user" "$DIR/tools"
    cp -a "$rel/app" "$rel/dist-user" "$rel/tools" "$DIR/"
    for f in shadPS4.sh install.sh games.json README.md LICENSE VERSION; do
        [ -f "$rel/$f" ] && cp -a "$rel/$f" "$DIR/$f"
    done
    # Pre-folder-app installs: the AppImage and its copy are no longer used.
    rm -f "$DIR/Shadps4-sdl.AppImage" "$DIR/Shadps4-sdl.AppImage.prev" "$DIR/.installed"
    rm -rf "$DIR/.dist"
fi
# Their Steam shortcut starts this launcher: it now opens the game through shadPS4.sh.
if [ -f "$DIR/inFAMOUS Second Son.sh" ]; then
    printf '#!/bin/bash\nexec "%s/shadPS4.sh" CUSA00004 "$@"\n' "$DIR" >"$DIR/inFAMOUS Second Son.sh"
fi

"$DIR/shadPS4.sh" --setup "${YES[@]}"

if [ "$STEAM" = 1 ]; then
    if pgrep -x steam >/dev/null; then
        "$DIR/shadPS4.sh" --add-to-steam || true
        info "Per-game shortcuts with their library art need Steam closed: exit Steam and run"
        info "  \"$DIR/shadPS4.sh\" --add-to-steam CUSA00004   (or another title id)"
    else
        "$DIR/shadPS4.sh" --add-to-steam || true
        while IFS=$'\t' read -r id _; do
            "$DIR/shadPS4.sh" --add-to-steam "$id" || true
        done <"$DIR/user/games.list"
    fi
fi

cat <<EOF

Done. Start "shadPS4 (inFAMOUS)" (or a game) from Steam (Library > Non-Steam), or run
    "$DIR/shadPS4.sh"
Choose a game, then Play or Patches (render resolution, skip intro). Update later with
    "$DIR/shadPS4.sh" --update
EOF
