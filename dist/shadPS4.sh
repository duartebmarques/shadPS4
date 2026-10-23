#!/bin/bash
# shadPS4-infamous: the whole app lives in this folder (app/, user/, games.json, tools/).
#
#   ./shadPS4.sh                 game picker (Big Picture): choose a game, its patches, play
#   ./shadPS4.sh CUSA00004       play a game directly (a title id from games.json, or an eboot.bin)
#   ./shadPS4.sh --setup         find the supported games again (also their update, DLC and pkg files)
#   ./shadPS4.sh --add-to-steam  add this app to Steam (one shortcut; with a title id, that game's)
#   ./shadPS4.sh --update        download the latest release over this folder (user/ is kept)
#
# Setup runs by itself on the first start: it looks for the games next to this folder, in your
# home folder and on SD cards, links updates and DLC, downloads the community patch files and
# turns the default ones on.
set -euo pipefail

ROOT="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
REPO="duartebmarques/shadPS4"
EXTRACTOR_REPO="AzaharPlus/shadPS4Plus"
PATCH_BASE="https://raw.githubusercontent.com/shadps4-emu/ps4_cheats/e34463942099072e5d8f5e2bcb3d7d927683b349/PATCHES"
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
ask() {
    [ "$YES" = 1 ] && return 0
    [ -t 0 ] || return 1
    local reply
    read -r -p "$1 [Y/n] " reply
    [[ -z "$reply" || "$reply" =~ ^[Yy] ]]
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
        patches/*/*.xml)
            if [ -f "$U/$rel" ]; then
                cp "$f" "$U/$rel.new" && py patches "$U/$rel.new" "$U/$rel" >/dev/null &&
                    mv -f "$U/$rel.new" "$U/$rel"
            else put "$f" "$U/$rel"; fi ;;
        *) put "$f" "$U/$rel" ;;
        esac
    done < <(find "$d" -type f -print0)
}

# The community patch file of each game, with the default patches on (or your earlier choices).
fetch_patches() {
    local id name serials update pfile defaults pf on
    mkdir -p "$U/patches/shadPS4"
    while IFS=$'\t' read -r id name serials update pfile defaults; do
        [ "$pfile" != - ] || continue
        [ "$defaults" != - ] || defaults=""
        pf="$U/patches/shadPS4/$pfile"
        IFS='|' read -ra defs <<<"$defaults"
        if curl -fsSL --max-time 30 -o "$pf.dl" "$PATCH_BASE/$pfile" &&
            on="$(py patches "$pf.dl" "$pf" "${defs[@]}")"; then
            mv -f "$pf.dl" "$pf"
            info "$name patches on: ${on:-none}"
        else
            rm -f "$pf.dl"
            [ -f "$pf" ] || warn "could not download the $name patch file; run --setup when online"
        fi
    done < <(py games "$ROOT/games.json")
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
    fetch_patches
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
            [ -e "$link" ] || ln -s "$d" "$link"
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

add_to_steam() {
    local name="shadPS4 (inFAMOUS)" exe="$ROOT/shadPS4.sh" opts="" id="${1:-}" game=""
    if [ -n "$id" ]; then
        game="$(awk -F'\t' -v t="$id" '$1 == t { print $2 }' "$GAMES_LIST")"
        [ -n "$game" ] || die "$id is not set up (run --setup)"
        name="$(py games "$ROOT/games.json" | awk -F'\t' -v t="$id" 'index("," $3 ",", "," t ",") { print $2 }')"
        opts="$id"
    fi
    if pgrep -x steam >/dev/null && command -v steamos-add-to-steam >/dev/null && [ -z "$id" ]; then
        steamos-add-to-steam "$exe"
        info "Sent to Steam: \"$exe\""
        return
    fi
    pgrep -x steam >/dev/null && die "close Steam first (Steam > Exit), then run this again"
    local steam_dir="$HOME/.local/share/Steam" cfg vdf appid icon="${game:+$game/sce_sys/icon0.png}" n=0
    [ -d "$steam_dir/userdata" ] || steam_dir="$HOME/.steam/steam"
    for cfg in "$steam_dir"/userdata/*/config; do
        [[ "$(basename "$(dirname "$cfg")")" =~ ^[1-9][0-9]*$ ]] || continue
        vdf="$cfg/shortcuts.vdf"
        [ -f "$vdf" ] && cp "$vdf" "$vdf.bak"
        appid="$(py shortcut "$vdf" "$name" "$exe" "$ROOT" "${icon:-}" "$opts")" || continue
        mkdir -p "$cfg/grid"
        if [ -n "$game" ] && [ -f "$game/sce_sys/pic1.png" ]; then
            cp "$game/sce_sys/pic1.png" "$cfg/grid/${appid}_hero.png"
            cp "$game/sce_sys/pic1.png" "$cfg/grid/${appid}.png"
        fi
        if [ -n "$icon" ] && [ -f "$icon" ]; then
            cp "$icon" "$cfg/grid/${appid}_icon.png"
            cp "$icon" "$cfg/grid/${appid}p.png"
        fi
        n=$((n + 1))
    done
    [ "$n" -gt 0 ] || die "no Steam account found in $steam_dir"
    info "Steam shortcut \"$name\" added ($n Steam account(s))"
}

update() {
    local rel tag url work
    command -v curl >/dev/null || die "curl is required"
    command -v unzip >/dev/null || die "unzip is required"
    running && die "a game is running; close it first"
    rel="$(py latest "$REPO" '-linux\.zip$')" || die "could not look up the latest release"
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
    [ -f "$src/shadPS4.sh" ] || die "the release has no shadPS4.sh"
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
    exec "$ROOT/shadPS4.sh" --setup
}

play() {
    local target="$1" game
    if [ -f "$target" ]; then
        game="$target"
    else
        game="$(awk -F'\t' -v t="$target" '$1 == t { print $2 }' "$GAMES_LIST" 2>/dev/null)"
        [ -n "$game" ] || die "$target not found (run ./shadPS4.sh --setup)"
        game="$game/eboot.bin"
    fi
    shift
    exec "$EMU" "$game" "$@"
}

cd "$ROOT"
[ -x "$EMU" ] || die "app/usr/bin/shadps4 is missing: extract the whole release folder"
case "${1:-}" in
-h | --help) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//' ;;
--setup) shift; [ "${1:-}" = -y ] && YES=1; setup ;;
--add-to-steam) shift; add_to_steam "${1:-}" ;;
--update) shift; [ "${1:-}" = -y ] && YES=1; update ;;
"")
    [ -f "$U/.setup-done" ] || setup
    exec "$EMU" --big-picture ;;
-*) exec "$EMU" "$@" ;;
*)
    [ -f "$U/.setup-done" ] || setup
    play "$@" ;;
esac
