#!/bin/bash
# shadPS4-infamous: the whole app lives in this folder (app/, user/, games.json, tools/).
#
#   ./shadPS4.sh                 game picker (Big Picture): choose a game, its patches, play
#   ./shadPS4.sh CUSA00004       play a game directly (a title id from games.json, or an eboot.bin)
#   ./shadPS4.sh --setup         find the supported games again (also their update, DLC and pkg files)
#   ./shadPS4.sh --add-to-steam  add each game to Steam (plays it directly) and the game picker;
#                                the game page's Add to Steam button does it for one game
#   ./shadPS4.sh --update [tag]  download the latest release (or that one) over this folder
#                                (user/ is kept)
#
# Setup runs by itself on the first start: it looks for the games next to this folder, in your
# home folder and on SD cards, links updates and DLC, and installs the patch files with the
# Steam Deck ones on.
set -euo pipefail

ROOT="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
REPO="duartebmarques/shadPS4"
EXTRACTOR_REPO="AzaharPlus/shadPS4Plus"
U="$ROOT/user"
EMU="$ROOT/app/usr/bin/shadps4"
GAMES_LIST="$U/games.list"
YES=0

die() {
    echo "error: $*" >&2
    [ -t 2 ] || notify "$*"
    exit 1
}
info() { echo "==> $*"; }
warn() { echo "warning: $*" >&2; }
notify() { command -v notify-send >/dev/null && notify-send -a shadPS4 "shadPS4" "$*" || true; }
# Yes/no question: in the terminal, else a desktop dialog (started by double-click), else no.
ask() {
    [ "$YES" = 1 ] && return 0
    if [ -t 0 ]; then
        local reply
        read -r -p "$1 [Y/n] " reply
        [[ -z "$reply" || "$reply" =~ ^[Yy] ]]
    elif [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] && command -v kdialog >/dev/null; then
        kdialog --title shadPS4 --yesno "$1" 2>/dev/null
    elif [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] && command -v zenity >/dev/null; then
        zenity --question --title shadPS4 --text "$1" 2>/dev/null
    else
        return 1
    fi
}
py() { python3 -I "$ROOT/tools/sp4.py" "$@"; }

# Replaces dst through a rename, so a running copy keeps reading the old file.
put() {
    cp "$1" "$2.new.$$"
    mv -f "$2.new.$$" "$2"
}

running() {
    local exe
    for exe in /proc/[0-9]*/exe; do
        [[ "$(readlink "$exe" 2>/dev/null)" == "$ROOT/app/"* ]] && return 0
    done
    return 1
}

# Copies this release's defaults into user/: tuned per-game settings follow each release (a
# changed copy is kept as .bak), the controller map and patch choices stay yours.
install_defaults() {
    local d="$ROOT/dist-user" f rel
    mkdir -p "$U/custom_configs" "$U/input_config" "$U/patches" \
        "$U/home/1000/savedata" "$U/home/1000/trophy" "$U/home/1000/inputs"
    while IFS= read -r -d '' f; do
        rel="${f#"$d"/}"
        mkdir -p "$(dirname "$U/$rel")"
        case "$rel" in
        custom_configs/*)
            if [ -f "$U/$rel" ] && ! cmp -s "$f" "$U/$rel"; then cp "$U/$rel" "$U/$rel.bak"; fi
            put "$f" "$U/$rel" ;;
        input_config/*)
            if [ ! -f "$U/$rel" ]; then put "$f" "$U/$rel"; elif ! cmp -s "$f" "$U/$rel"; then
                put "$f" "$U/$rel.release"; fi ;;
        art/*) ;; # read in place by steam_shortcut
        patches/*/*.xml)
            if [ -f "$U/$rel" ]; then
                cp "$f" "$U/$rel.new" && py patches "$U/$rel.new" "$U/$rel" >/dev/null &&
                    mv -f "$U/$rel.new" "$U/$rel"
            else put "$f" "$U/$rel"; fi ;;
        *) put "$f" "$U/$rel" ;;
        esac
    done < <(find "$d" -type f -print0)
}

# Patch files ship in dist-user/patches (install_defaults); the emulator finds them by serial here.
index_patches() {
    py patch-index "$U/patches/shadPS4/files.json" "$ROOT/games.json"
}

extract_pkgs() {
    local extractor dest p rc
    extractor="$(find "$ROOT/tools" "$HOME" /run/media -maxdepth 5 -iname 'pkg_extractor*.AppImage' \
        -not -path '*/.*' 2>/dev/null | head -1 || true)"
    if [ -z "$extractor" ] && ask "Download the pkg extractor ($EXTRACTOR_REPO, a few MB)?"; then
        local rel
        rel="$(py latest "$EXTRACTOR_REPO" 'PkgExtractor.*linux\.AppImage$')" ||
            die "could not look up the pkg extractor"
        extractor="$ROOT/tools/pkg_extractor.AppImage"
        curl -fL --progress-bar -o "$extractor" "${rel#*$'\t'}"
        chmod +x "$extractor"
    fi
    [ -n "$extractor" ] || return 1
    for p in "$@"; do
        dest="$(dirname "$p")"
        rc=0
        "$extractor" "$p" --check-type >/dev/null 2>&1 || rc=$?
        info "Extracting $(basename "$p") (this can take a while)..."
        case "$rc" in
        101 | 102) "$extractor" "$p" "$dest" >/dev/null ;;
        103) "$extractor" "$p" "$U/addcont" >/dev/null ;;
        *) warn "skipping $p: not a game, update or DLC pkg" ;;
        esac
    done
}

# Finds the games of games.json (the folder around this app first, then home and SD cards) and
# writes user/games.list ("title id<TAB>game folder").
setup() {
    local serials scan game title path ver upd other d link n=0 roots=("$(dirname "$ROOT")" "$HOME" /run/media)
    command -v python3 >/dev/null || die "python3 is required"
    install_defaults
    index_patches
    serials="$(py games "$ROOT/games.json" | cut -f3 | paste -sd,)"
    scan="$(mktemp)"
    trap 'rm -f "$scan"' RETURN
    info "Looking for the games in your home folder and on SD cards..."
    py scan "$serials" "${roots[@]}" >"$scan" || true
    if ! grep -q '^game' "$scan" && grep -q '^pkg' "$scan"; then
        mapfile -t pkgs < <(awk -F'\t' '$1 == "pkg" { print $3 }' "$scan" | sort -u)
        info "No extracted game found, but these pkg files:"
        printf '    %s\n' "${pkgs[@]}"
        if ask "Extract them next to the pkg files?" && extract_pkgs "${pkgs[@]}"; then
            py scan "$serials" "${roots[@]}" >"$scan" || true
        fi
    fi
    : >"$GAMES_LIST.new"
    while IFS=$'\t' read -r _ title game ver; do
        grep -q "^$title"$'\t' "$GAMES_LIST.new" && continue
        printf '%s\t%s\n' "$title" "$game" >>"$GAMES_LIST.new"
        py config-add-dir "$U/config.json" "$(dirname "$game")"
        py config-for "$ROOT/games.json" "$U/custom_configs" "$title"
        upd=""
        for s in -UPDATE -patch; do
            if [ -f "$game$s/eboot.bin" ]; then upd="$game$s"; break; fi
        done
        other="$(awk -F'\t' -v t="$title" '$1 == "update" && $2 == t { print $3; exit }' "$scan")"
        if [ -z "$upd" ] && [ -n "$other" ]; then
            ln -sfn "$other" "$game-UPDATE"
            upd="$game-UPDATE"
        fi
        info "$title: $game${upd:+ (update: $(basename "$upd"))}"
        while IFS= read -r d; do
            [ -n "$d" ] || continue
            mkdir -p "$U/addcont/$title"
            link="$U/addcont/$title/$(basename "$d")"
            [ "$(readlink "$link")" = "$d" ] || ln -sfn "$d" "$link"
        done < <(awk -F'\t' -v t="$title" '$1 == "dlc" && $2 == t { print $3 }' "$scan")
        n=$((n + 1))
    done < <(grep '^game' "$scan")
    mv -f "$GAMES_LIST.new" "$GAMES_LIST"
    # Saves of an earlier shadPS4 setup (copied, the originals stay).
    local old="${XDG_DATA_HOME:-$HOME/.local/share}/shadPS4/home/1000/savedata"
    if [ "$(readlink -f "$U")" != "$(readlink -f "${old%/home/1000/savedata}")" ] && [ -d "$old" ]; then
        for d in "$old"/*; do
            [ -d "$d" ] && [ ! -e "$U/home/1000/savedata/$(basename "$d")" ] &&
                grep -q "^$(basename "$d")"$'\t' "$GAMES_LIST" && cp -a "$d" "$U/home/1000/savedata/" &&
                info "Saves of $(basename "$d") copied from $old"
        done
    fi
    touch "$U/.setup-done"
    [ "$n" -gt 0 ] || warn "no supported game found: copy your game dump (or its pkg files) into your home folder or an SD card and run --setup"
}

# Writes one shortcut (name, launch options = title id, game folder) into every Steam account,
# with the library art shipped in dist-user/art/<title id> (else the game's own); Steam closed.
steam_shortcut() {
    local name="$1" opts="$2" game="$3" steam_dir="$HOME/.local/share/Steam" cfg vdf appid out n=0
    local art="$ROOT/dist-user/art/${opts:-none}" sce="${game:+$game/sce_sys}" icon="" f
    [ -d "$steam_dir/userdata" ] || steam_dir="$HOME/.steam/steam"
    if [ -f "$art/icon.png" ]; then
        icon="$art/icon.png"
    elif [ -n "$sce" ]; then
        icon="$sce/icon0.png"
    fi
    for cfg in "$steam_dir"/userdata/*/config; do
        [[ "$(basename "$(dirname "$cfg")")" =~ ^[1-9][0-9]*$ ]] || continue
        vdf="$cfg/shortcuts.vdf"
        [ -f "$vdf" ] && cp "$vdf" "$vdf.bak"
        out="$(py shortcut "$vdf" "$name" "$ROOT/shadPS4.sh" "$ROOT" "$icon" "$opts")" || continue
        appid="${out%%$'\t'*}"
        mkdir -p "$cfg/grid"
        if [ -d "$art" ]; then
            # Steam's names: <id>.png grid, <id>p.png portrait, _hero, _logo (+ .json position), _icon.
            for f in grid: portrait:p hero:_hero logo:_logo icon:_icon; do
                [ -f "$art/${f%%:*}.png" ] && cp "$art/${f%%:*}.png" "$cfg/grid/${appid}${f#*:}.png"
            done
            [ -f "$art/logo.json" ] && cp "$art/logo.json" "$cfg/grid/$appid.json"
        elif [ -n "$sce" ]; then
            if [ -f "$sce/pic1.png" ]; then
                cp "$sce/pic1.png" "$cfg/grid/${appid}_hero.png"
                cp "$sce/pic1.png" "$cfg/grid/${appid}.png"
            fi
            if [ -f "$sce/icon0.png" ]; then
                cp "$sce/icon0.png" "$cfg/grid/${appid}_icon.png"
                cp "$sce/icon0.png" "$cfg/grid/${appid}p.png"
            fi
        fi
        n=$((n + 1))
    done
    [ "$n" -gt 0 ] || { warn "no Steam account found in $steam_dir"; return 1; }
    info "Steam shortcut \"$name\"${opts:+ (plays $opts)} ${out#*$'\t'}"
    # Started from the game page there is no terminal to read this.
    [ -t 1 ] || notify "$name: Steam shortcut ${out#*$'\t'} (Library > Non-Steam)"
}

# Adds a Steam shortcut that starts each given game directly (without arguments: every set-up
# game, plus the game picker). Steam rewrites its shortcuts on exit, so it is closed and restarted.
add_to_steam() {
    local id game name restart=0 i
    local ids=("$@") picker=0
    [ "${#ids[@]}" -gt 0 ] || { picker=1; mapfile -t ids < <(cut -f1 "$GAMES_LIST" 2>/dev/null); }
    [ "${#ids[@]}" -gt 0 ] || { warn "no games set up yet (run --setup)"; return 1; }
    if pgrep -x steam >/dev/null; then
        if ! ask "Add your games to Steam? Steam closes for a moment and starts again."; then
            warn "to add them later: \"$ROOT/shadPS4.sh\" --add-to-steam"
            return 1
        fi
        steam -shutdown >/dev/null 2>&1 || true
        for ((i = 0; i < 60; i++)); do pgrep -x steam >/dev/null || break; sleep 1; done
        if pgrep -x steam >/dev/null; then
            warn "Steam did not close; exit it and run --add-to-steam again"
            return 1
        fi
        restart=1
    fi
    [ "$picker" = 0 ] || steam_shortcut "shadPS4 (inFAMOUS)" "" ""
    for id in "${ids[@]}"; do
        game="$(awk -F'\t' -v t="$id" '$1 == t { print $2 }' "$GAMES_LIST")"
        [ -n "$game" ] || { warn "$id is not set up (run --setup)"; continue; }
        name="$(py games "$ROOT/games.json" |
            awk -F'\t' -v t="$id" 'index("," $3 ",", "," t ",") { print $2 }')"
        steam_shortcut "${name:-$id}" "$id" "$game"
    done
    # Gaming Mode starts Steam again by itself.
    if [ "$restart" = 1 ] && [ "${XDG_CURRENT_DESKTOP:-}" != gamescope ]; then
        setsid steam >/dev/null 2>&1 < /dev/null &
    fi
}

update() {
    local rel tag url work
    command -v curl >/dev/null || die "curl is required"
    command -v unzip >/dev/null || die "unzip is required"
    running && die "a game is running; close it first"
    rel="$(py latest "$REPO" '-linux\.zip$' "${1:-}")" || die "could not look up the release"
    tag="${rel%%$'\t'*}"
    url="${rel#*$'\t'}"
    if [ "$(cat "$ROOT/VERSION" 2>/dev/null)" = "$tag" ] && ! ask "Already on $tag. Reinstall it?"; then
        return
    fi
    work="$(mktemp -d)"
    trap 'rm -rf "$work"' RETURN
    info "Downloading $tag..."
    curl -fL --progress-bar -o "$work/release.zip" "$url"
    unzip -q "$work/release.zip" -d "$work/r"
    local src
    src="$(dirname "$(find "$work/r" -maxdepth 3 -name shadPS4.sh | head -1)")"
    [ -f "$src/shadPS4.sh" ] && [ -x "$src/app/usr/bin/shadps4" ] ||
        die "$tag is not a folder app release (older than v1.1); nothing was changed"
    rm -rf "$ROOT/app.prev"
    [ -d "$ROOT/app" ] && mv "$ROOT/app" "$ROOT/app.prev"
    cp -a "$src/app" "$ROOT/app"
    rm -rf "$ROOT/dist-user" "$ROOT/tools.new"
    cp -a "$src/dist-user" "$ROOT/dist-user"
    for f in shadPS4.sh games.json README.md LICENSE VERSION; do
        [ -f "$src/$f" ] && put "$src/$f" "$ROOT/$f"
    done
    cp -a "$src/tools/." "$ROOT/tools/"
    chmod +x "$ROOT/shadPS4.sh" "$ROOT/app/usr/bin/shadps4"
    info "Updated to $tag (the previous build is in app.prev)"
    if [ "$YES" = 1 ]; then exec "$ROOT/shadPS4.sh" --setup -y; fi
    exec "$ROOT/shadPS4.sh" --setup
}

play() {
    local target="$1" game
    if [ -f "$target" ]; then
        game="$target"
    else
        game="$(awk -F'\t' -v t="$target" '$1 == t { print $2 }' "$GAMES_LIST" 2>/dev/null)"
        if [ ! -f "$game/eboot.bin" ]; then
            # Moved or renamed since the last setup: look again.
            setup
            game="$(awk -F'\t' -v t="$target" '$1 == t { print $2 }' "$GAMES_LIST" 2>/dev/null)"
        fi
        [ -n "$game" ] || die "$target not found (run ./shadPS4.sh --setup)"
        game="$game/eboot.bin"
    fi
    shift
    exec "$EMU" "$game" "$@"
}

cd "$ROOT"
[ -x "$EMU" ] || die "app/usr/bin/shadps4 is missing: extract the whole release folder"
case "${1:-}" in
-h | --help) sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//' ;;
--setup) shift; [ "${1:-}" = -y ] && YES=1; setup ;;
--add-to-steam) shift; [ "${1:-}" = -y ] && { YES=1; shift; }; add_to_steam "$@" ;;
--update) shift; [ "${1:-}" = -y ] && { YES=1; shift; }; update "${1:-}" ;;
"")
    [ -f "$U/.setup-done" ] || setup
    # The game page's Add to Steam button runs this script.
    SHADPS4_LAUNCHER="$ROOT/shadPS4.sh" exec "$EMU" --big-picture ;;
-*) exec "$EMU" "$@" ;;
*)
    [ -f "$U/.setup-done" ] || setup
    play "$@" ;;
esac
