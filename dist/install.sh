#!/bin/bash
# Installs or updates shadPS4 for inFAMOUS Second Son (CUSA00004) on Linux / Steam Deck.
#
#   ./install.sh            install: finds the game, its update and DLC, sets up the patches,
#                           copies old saves and adds a Steam shortcut. Run on its own (no
#                           release files next to it), it downloads the latest release first.
#   ./install.sh --update   download the latest release and install it over this one
#
# One line, in a Desktop Mode terminal (Konsole):
#   bash <(curl -fsSL https://github.com/duartebmarques/shadPS4/releases/latest/download/install.sh)
#
# Options:
#   --dir DIR          install folder (default: ~/Applications/shadPS4-infamous)
#   --game DIR         game folder (the one with eboot.bin); skips the search
#   --search DIR       extra folder to search for the game, pkg files and DLC (repeatable)
#   --extractor FILE   pkg_extractor AppImage, used when only .pkg files are found
#   --appimage FILE    use this Shadps4-sdl.AppImage (for example a CI build)
#   --no-steam         do not add a Steam shortcut
#   --steam-dir DIR    Steam folder (default: ~/.local/share/Steam)
#   -y, --yes          answer yes to every question
set -euo pipefail

TITLE_ID="CUSA00004"
APP_NAME="inFAMOUS Second Son"
REPO="duartebmarques/shadPS4"
EXTRACTOR_REPO="AzaharPlus/shadPS4Plus"
PATCH_URL="https://raw.githubusercontent.com/shadps4-emu/ps4_cheats/main/PATCHES/InfamousSecondSon.xml"
DEFAULT_PATCHES=("Skip intro movie" "Render resolution 1280x720")

# Empty when the script comes from a pipe (bash <(curl ...)).
SRC="$(cd "$(dirname "$(readlink -f "$0")")" 2>/dev/null && pwd || true)"
ARGS=("$@")
DIR="$HOME/Applications/shadPS4-infamous"
GAME=""
SEARCH=()
EXTRACTOR=""
APPIMAGE=""
STEAM=1
STEAM_DIR=""
YES=0
UPDATE=0

# Started from a file manager: reopen in a terminal so the questions can be answered.
if [ ! -t 0 ] && [ -z "${INFAMOUS_IN_TERM:-}" ] && [ -f "$0" ]; then
    export INFAMOUS_IN_TERM=1
    if command -v konsole >/dev/null; then
        exec konsole --hold -e bash "$0" "$@"
    elif command -v xterm >/dev/null; then
        exec xterm -hold -e bash "$0" "$@"
    fi
fi

die() { echo "error: $*" >&2; exit 1; }
info() { echo "==> $*"; }
step() { printf '\n\033[1m[%s/6] %s\033[0m\n' "$1" "$2"; }
warn() { echo "warning: $*" >&2; }
ask() {
    [ "$YES" = 1 ] && return 0
    [ -t 0 ] || return 1
    local reply
    read -r -p "$1 [Y/n] " reply
    [[ -z "$reply" || "$reply" =~ ^[Yy] ]]
}

while [ $# -gt 0 ]; do
    case "$1" in
    --dir) DIR="$2"; shift ;;
    --game) GAME="$2"; shift ;;
    --search) SEARCH+=("$2"); shift ;;
    --extractor) EXTRACTOR="$2"; shift ;;
    --appimage) APPIMAGE="$2"; shift ;;
    --no-steam) STEAM=0 ;;
    --steam-dir) STEAM_DIR="$2"; shift ;;
    -y | --yes) YES=1 ;;
    --update) UPDATE=1 ;;
    -h | --help) sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown option: $1 (see --help)" ;;
    esac
    shift
done

command -v python3 >/dev/null || die "python3 is required"
mkdir -p "$DIR"
DIR="$(cd "$DIR" && pwd)"
U="$DIR/user"

# Python helpers: PS4 param.sfo and pkg headers, the patch file, Steam's binary shortcuts.vdf.
py() {
    python3 -I - "$@" <<'PY'
import os, re, struct, sys, zlib

def sfo(path):
    try:
        with open(path, "rb") as f:
            d = f.read()
        if d[:4] != b"\0PSF":
            return {}
        _, kt, dt, n = struct.unpack_from("<IIII", d, 4)
        out = {}
        for i in range(n):
            ko, fmt, ln, _, do = struct.unpack_from("<HHIII", d, 20 + i * 16)
            key = d[kt + ko:d.index(b"\0", kt + ko)].decode()
            raw = d[dt + do:dt + do + ln]
            if fmt == 0x0404:
                out[key] = struct.unpack("<I", raw)[0]
            else:
                out[key] = raw.rstrip(b"\0").decode("utf-8", "replace")
        return out
    except (OSError, ValueError, struct.error):
        return {}

def scan(title, *roots):
    # Prints "kind<TAB>path<TAB>version" for the game (gd), updates (gp), DLC (ac) and pkgs.
    skip = {"steamapps", "compatdata", "shadercache", "node_modules"}
    seen = set()
    for root in roots:
        root = os.path.realpath(root)
        if not os.path.isdir(root):
            continue
        base = root.count(os.sep)
        for cur, dirs, files in os.walk(root):
            if ".git" in dirs or cur.count(os.sep) - base >= 6:
                dirs[:] = []
            dirs[:] = [d for d in dirs if not d.startswith(".") and d not in skip]
            if cur in seen:
                continue
            seen.add(cur)
            for f in files:
                if not f.lower().endswith(".pkg"):
                    continue
                p = os.path.join(cur, f)
                try:
                    with open(p, "rb") as h:
                        hdr = h.read(0x64)
                except OSError:
                    continue
                if hdr[:4] == b"\x7fCNT" and title.encode() in hdr[0x40:0x64]:
                    print(f"pkg\t{p}\t")
            if "sce_sys" not in dirs:
                continue
            p = sfo(os.path.join(cur, "sce_sys", "param.sfo"))
            if p.get("TITLE_ID") != title:
                continue
            cat, ver = str(p.get("CATEGORY", "")), p.get("APP_VER", "")
            if cat.startswith("gd") and os.path.isfile(os.path.join(cur, "eboot.bin")):
                print(f"game\t{cur}\t{ver}")
            elif cat.startswith("gp"):
                print(f"update\t{cur}\t{ver}")
            elif cat.startswith("ac"):
                print(f"dlc\t{cur}\t")

META = re.compile(r"<Metadata\b[^>]*>", re.S)

def name_of(tag):
    m = re.search(r'\bName="([^"]*)"', tag)
    return m.group(1) if m else None

def patches(new, old, *defaults):
    # Keeps the patches enabled in the old file (or the defaults on a first install).
    if os.path.isfile(old):
        txt = open(old, encoding="utf-8").read()
        want = {name_of(t) for t in META.findall(txt) if 'isEnabled="true"' in t}
    else:
        want = set(defaults)
    txt = open(new, encoding="utf-8").read()
    if "<Patch>" not in txt:
        sys.exit("not a patch file")
    def fix(m):
        on = "true" if name_of(m.group(0)) in want else "false"
        return re.sub(r'isEnabled="[^"]*"', f'isEnabled="{on}"', m.group(0))
    txt = META.sub(fix, txt)
    open(new, "w", encoding="utf-8").write(txt)
    print(", ".join(name_of(t) for t in META.findall(txt) if 'isEnabled="true"' in t))

# Binary KeyValues, as in Steam's shortcuts.vdf.
def vdf_read(d, i=0):
    out = []
    while True:
        t = d[i]
        i += 1
        if t == 0x08:
            return out, i
        j = d.index(b"\0", i)
        k = d[i:j].decode("utf-8", "surrogateescape")
        i = j + 1
        if t == 0x00:
            v, i = vdf_read(d, i)
        elif t == 0x01:
            j = d.index(b"\0", i)
            v = d[i:j].decode("utf-8", "surrogateescape")
            i = j + 1
        elif t == 0x02:
            v = struct.unpack_from("<i", d, i)[0]
            i += 4
        elif t == 0x07:
            v = ("u64", struct.unpack_from("<Q", d, i)[0])
            i += 8
        else:
            raise ValueError(f"unknown vdf type {t}")
        out.append([k, v])

def vdf_write(items):
    b = bytearray()
    for k, v in items:
        kk = k.encode("utf-8", "surrogateescape") + b"\0"
        if isinstance(v, list):
            b += b"\x00" + kk + vdf_write(v)
        elif isinstance(v, tuple):
            b += b"\x07" + kk + struct.pack("<Q", v[1])
        elif isinstance(v, int):
            b += b"\x02" + kk + struct.pack("<i", v)
        else:
            b += b"\x01" + kk + v.encode("utf-8", "surrogateescape") + b"\0"
    return bytes(b + b"\x08")

def shortcut(vdf, name, exe, start, icon):
    # Adds or updates the shortcut and prints its app id (for the grid artwork names).
    root = [["shortcuts", []]]
    if os.path.isfile(vdf) and os.path.getsize(vdf) > 0:
        with open(vdf, "rb") as f:
            root, _ = vdf_read(f.read())
    sc = next(v for k, v in root if k.lower() == "shortcuts")
    fields = {"AppName": name, "Exe": f'"{exe}"', "StartDir": f'"{start}"', "icon": icon}
    appid = zlib.crc32((fields["Exe"] + name).encode()) | 0x80000000
    lower = {k.lower(): v for k, v in fields.items()}
    for _, entry in sc:
        d = {k.lower(): v for k, v in entry}
        if d.get("exe") == fields["Exe"]:
            for kv in entry:
                kv[1] = lower.get(kv[0].lower(), kv[1])
            if isinstance(d.get("appid"), int):
                appid = d["appid"] & 0xFFFFFFFF
            break
    else:
        sc.append([str(len(sc)), [
            ["appid", appid - (1 << 32)], ["AppName", name], ["Exe", fields["Exe"]],
            ["StartDir", fields["StartDir"]], ["icon", icon], ["ShortcutPath", ""],
            ["LaunchOptions", ""], ["IsHidden", 0], ["AllowDesktopConfig", 1],
            ["AllowOverlay", 1], ["OpenVR", 0], ["Devkit", 0], ["DevkitGameID", ""],
            ["DevkitOverrideAppID", 0], ["LastPlayTime", 0], ["FlatpakAppID", ""],
            ["tags", []]]])
    data = vdf_write(root)
    assert vdf_read(data)[0] == root
    with open(vdf + ".tmp", "wb") as f:
        f.write(data)
    os.replace(vdf + ".tmp", vdf)
    print(appid)

def latest(repo, pattern=r"-linux\.zip$"):
    # Prints "tag<TAB>url" of the first asset of the latest release whose name matches pattern.
    import json, urllib.request
    url = f"https://api.github.com/repos/{repo}/releases/latest"
    try:
        with urllib.request.urlopen(url, timeout=30) as r:
            rel = json.load(r)
    except OSError as e:
        sys.exit(f"{url}: {e}")
    for a in rel["assets"]:
        if re.search(pattern, a["name"]):
            print(rel["tag_name"] + "\t" + a["browser_download_url"])
            return
    sys.exit(f"the latest release of {repo} has no {pattern} asset")

cmd, *args = sys.argv[1:]
{"scan": scan, "patches": patches, "shortcut": shortcut, "latest": latest}[cmd](*args)
PY
}

game_running() {
    local exe
    for exe in /proc/[0-9]*/exe; do
        if [[ "$(readlink "$exe" 2>/dev/null)" == "$DIR/"* ]]; then
            return 0
        fi
    done
    return 1
}

# Replaces dst through a rename, so a running copy of it keeps reading the old file.
put() {
    cp "$1" "$2.new.$$"
    mv -f "$2.new.$$" "$2"
}

# ---- Update, or a lone install.sh: fetch the latest release and run its installer ----
if [ "$UPDATE" = 1 ] || { [ -z "$APPIMAGE" ] && [ ! -f "$SRC/Shadps4-sdl.AppImage" ]; }; then
    command -v curl >/dev/null || die "curl is required"
    command -v unzip >/dev/null || die "unzip is required"
    rel="$(py latest "$REPO")" || die "could not look up the latest release"
    tag="${rel%%$'\t'*}"
    url="${rel#*$'\t'}"
    if grep -qx "version=$tag" "$DIR/.installed" 2>/dev/null; then
        info "Already on $tag."
        ask "Reinstall it anyway?" || exit 0
    fi
    work="$(mktemp -d)"
    trap 'rm -rf "$work"' EXIT
    info "Downloading $tag..."
    curl -fL --progress-bar -o "$work/release.zip" "$url"
    unzip -q "$work/release.zip" -d "$work/release"
    args=()
    for a in "${ARGS[@]}"; do
        if [ "$a" != --update ]; then args+=("$a"); fi
    done
    INFAMOUS_IN_TERM=1 bash "$work/release/install.sh" "${args[@]}"
    exit
fi

if game_running; then
    die "the game is running; close it first"
fi

step 1 "Emulator and tuned settings"
[ -n "$APPIMAGE" ] || APPIMAGE="$SRC/Shadps4-sdl.AppImage"
[ -f "$APPIMAGE" ] || die "Shadps4-sdl.AppImage not found next to install.sh (use --appimage)"
# The shipped user folder; an installed copy keeps it as .dist for reinstalls.
DIST="$SRC/user"
[ "$SRC" != "$DIR" ] || DIST="$DIR/.dist"
[ -d "$DIST" ] || die "the user folder is missing next to install.sh"
info "Installing to $DIR"
if [ "$(readlink -f "$APPIMAGE")" != "$(readlink -f "$DIR/Shadps4-sdl.AppImage")" ]; then
    if [ -f "$DIR/Shadps4-sdl.AppImage" ]; then
        cp -p "$DIR/Shadps4-sdl.AppImage" "$DIR/Shadps4-sdl.AppImage.prev"
    fi
    put "$APPIMAGE" "$DIR/Shadps4-sdl.AppImage"
    chmod +x "$DIR/Shadps4-sdl.AppImage"
fi
if [ "$SRC" != "$DIR" ]; then
    for f in install.sh README.md LICENSE VERSION; do
        if [ -f "$SRC/$f" ]; then put "$SRC/$f" "$DIR/$f"; fi
    done
    chmod +x "$DIR/install.sh"
    rm -rf "$DIR/.dist"
    cp -r "$DIST" "$DIR/.dist"
fi
mkdir -p "$U/custom_configs" "$U/input_config" "$U/patches/shadPS4" "$U/addcont/$TITLE_ID"
# The tuned settings follow each release; a copy you changed is kept as .bak.
cfg="$U/custom_configs/$TITLE_ID.json"
if [ -f "$cfg" ] && ! cmp -s "$cfg" "$DIST/custom_configs/$TITLE_ID.json"; then
    cp "$cfg" "$cfg.bak"
    warn "your $TITLE_ID.json differed from this release's; the old one is now $TITLE_ID.json.bak"
fi
put "$DIST/custom_configs/$TITLE_ID.json" "$cfg"
# The controller map is yours once installed; a newer default lands next to it.
ini="$U/input_config/default.ini"
if [ ! -f "$ini" ]; then
    put "$DIST/input_config/default.ini" "$ini"
elif ! cmp -s "$ini" "$DIST/input_config/default.ini"; then
    put "$DIST/input_config/default.ini" "$ini.release"
fi
put "$DIST/patches/shadPS4/files.json" "$U/patches/shadPS4/files.json"
# Without these, the first start of a new user folder asks a "Save Migration" question.
mkdir -p "$U/home/1000/savedata" "$U/home/1000/trophy" "$U/home/1000/inputs"

step 2 "Game patches (skip intro, 1280x720)"
pf="$U/patches/shadPS4/InfamousSecondSon.xml"
if curl -fsSL --max-time 30 -o "$pf.dl" "$PATCH_URL" &&
    on="$(py patches "$pf.dl" "$pf" "${DEFAULT_PATCHES[@]}")"; then
    mv -f "$pf.dl" "$pf"
    info "Patches on: ${on:-none}"
else
    rm -f "$pf.dl"
    if [ -f "$pf" ]; then
        warn "could not download the patch file; keeping the installed one"
    else
        warn "could not download the patch file; without its 1280x720 patch the game is slower"
        warn "run install.sh again when online"
    fi
fi

step 3 "Looking for the game, its update and DLC"
roots=()
if [ -n "$GAME" ]; then roots+=("$(dirname "$GAME")"); fi
roots+=("${SEARCH[@]}")
old_game="$(sed -n 's/^game=//p' "$DIR/.installed" 2>/dev/null || true)"
if [ -n "$old_game" ]; then roots+=("$(dirname "$old_game")"); fi
roots+=("$HOME" /run/media)

scan_file="$(mktemp)"
trap 'rm -f "$scan_file"' EXIT
find_all() {
    info "Searching for $APP_NAME ($TITLE_ID) in your home folder and SD card..."
    py scan "$TITLE_ID" "${roots[@]}" >"$scan_file" || true
}
field() { awk -F'\t' -v k="$1" '$1 == k { print $2 }' "$scan_file"; }
find_all

if [ -z "$GAME" ] && [ -z "$(field game)" ] && [ -n "$(field pkg)" ]; then
    mapfile -t pkgs < <(field pkg | sort -u)
    info "No extracted game found, but these pkg files:"
    printf '    %s\n' "${pkgs[@]}"
    if [ -z "$EXTRACTOR" ]; then
        EXTRACTOR="$(find "$HOME" /run/media -maxdepth 5 -iname 'pkg_extractor*.AppImage' \
            -not -path '*/.*' 2>/dev/null | head -1 || true)"
    fi
    if [ -z "$EXTRACTOR" ] && ask "Download the pkg extractor ($EXTRACTOR_REPO, a few MB)?"; then
        rel="$(py latest "$EXTRACTOR_REPO" 'PkgExtractor.*linux\.AppImage$')" ||
            die "could not look up the pkg extractor"
        mkdir -p "$DIR/tools"
        EXTRACTOR="$DIR/tools/pkg_extractor.AppImage"
        curl -fL --progress-bar -o "$EXTRACTOR" "${rel#*$'\t'}"
        chmod +x "$EXTRACTOR"
    fi
    [ -n "$EXTRACTOR" ] || die "extracting pkg files needs pkg_extractor (pass --extractor FILE)"
    dest="$(dirname "${pkgs[0]}")"
    need="$(du -cb "${pkgs[@]}" | tail -1 | cut -f1)"
    free="$(df -B1 --output=avail "$dest" | tail -1 | tr -d ' ')"
    [ "$free" -gt "$need" ] || die "extracting needs $((need >> 30)) GiB free in $dest"
    ask "Extract them into $dest with $(basename "$EXTRACTOR")?" || die "no game to install"
    for p in "${pkgs[@]}"; do
        rc=0
        "$EXTRACTOR" "$p" --check-type >/dev/null 2>&1 || rc=$?
        info "Extracting $(basename "$p") (this can take a while)..."
        case "$rc" in
        101 | 102) "$EXTRACTOR" "$p" "$dest" >/dev/null ;;
        103) "$EXTRACTOR" "$p" "$U/addcont" >/dev/null ;;
        *) warn "skipping $p: not a game, update or DLC pkg" ;;
        esac
    done
    roots=("$dest" "$U/addcont" "${roots[@]}")
    find_all
fi

if [ -n "$GAME" ]; then
    GAME="$(cd "$GAME" && pwd)"
    [ -f "$GAME/eboot.bin" ] || die "no eboot.bin in $GAME"
else
    mapfile -t games < <(field game)
    [ "${#games[@]}" -gt 0 ] || die "$TITLE_ID not found. Pass its folder with --game DIR"
    GAME="${games[0]}"
    if [ "${#games[@]}" -gt 1 ]; then
        warn "found ${#games[@]} copies of the game, using the first (choose with --game DIR)"
    fi
fi
info "Game: $GAME"

# The emulator loads the update from the folder next to the game named <game>-UPDATE or -patch.
upd=""
for s in -UPDATE -patch; do
    if [ -f "$GAME$s/eboot.bin" ]; then upd="$GAME$s"; break; fi
done
if [ -z "$upd" ]; then
    other="$(field update | head -1)"
    if [ -n "$other" ] && ask "Found the update at $other. Link it next to the game?"; then
        ln -sfn "$other" "$GAME-UPDATE"
        upd="$GAME-UPDATE"
    fi
fi
if [ -n "$upd" ]; then
    ver="$(awk -F'\t' '$1 == "update" { print $3; exit }' "$scan_file")"
    info "Update: $upd${ver:+ (v$ver)}"
else
    warn "no update found next to the game; the patches are made for update 1.07"
fi

while IFS= read -r d; do
    [ -n "$d" ] || continue
    link="$U/addcont/$TITLE_ID/$(basename "$d")"
    if [ "$(readlink -f "$d")" != "$(readlink -f "$link" 2>/dev/null || true)" ] &&
        [ ! -e "$link" ]; then
        ln -s "$d" "$link"
    fi
    info "DLC: $(basename "$d")"
done < <(field dlc)

step 4 "Saves from an earlier shadPS4 setup"
data="${XDG_DATA_HOME:-$HOME/.local/share}/shadPS4"
for old in "$data/home/1000/savedata/$TITLE_ID" "$data/savedata/1/$TITLE_ID"; do
    if [ -d "$old" ] && [ ! -e "$U/home/1000/savedata/$TITLE_ID" ]; then
        if ask "Copy your saves from $old?"; then
            cp -a "$old" "$U/home/1000/savedata/$TITLE_ID"
            info "Saves copied"
        fi
        break
    fi
done

step 5 "Launcher"
launcher="$DIR/$APP_NAME.sh"
{
    echo '#!/bin/bash'
    echo '# Written by install.sh. The emulator uses ./user, so start it from this folder.'
    echo 'cd "$(dirname "$(readlink -f "$0")")" || exit 1'
    printf 'exec ./Shadps4-sdl.AppImage %q "$@"\n' "$GAME/eboot.bin"
} >"$launcher.new"
chmod +x "$launcher.new"
mv -f "$launcher.new" "$launcher"
{
    echo "version=$(cat "$SRC/VERSION" 2>/dev/null || echo unknown)"
    echo "game=$GAME"
    echo "installed=$(date '+%F %T')"
} >"$DIR/.installed"

# ---- 6. Steam shortcut (shortcuts.vdf is only written while Steam is closed) ----
add_shortcuts() {
    local icon="$GAME/sce_sys/icon0.png" cfg id vdf appid n=0
    [ -f "$icon" ] || icon=""
    for cfg in "$STEAM_DIR"/userdata/*/config; do
        id="$(basename "$(dirname "$cfg")")"
        [[ "$id" =~ ^[1-9][0-9]*$ ]] || continue
        vdf="$cfg/shortcuts.vdf"
        if [ -f "$vdf" ]; then cp "$vdf" "$vdf.bak"; fi
        if ! appid="$(py shortcut "$vdf" "$APP_NAME" "$launcher" "$DIR" "$icon")"; then
            warn "could not update $vdf"
            continue
        fi
        mkdir -p "$cfg/grid"
        if [ -f "$GAME/sce_sys/pic1.png" ]; then
            cp "$GAME/sce_sys/pic1.png" "$cfg/grid/${appid}_hero.png"
        fi
        if [ -n "$icon" ]; then cp "$icon" "$cfg/grid/${appid}_icon.png"; fi
        n=$((n + 1))
    done
    [ "$n" -gt 0 ] || return 1
    info "Steam shortcut \"$APP_NAME\" added ($n Steam account(s))"
}

step 6 "Steam shortcut"
if [ "$STEAM" = 1 ]; then
    own_steam=0
    if [ -z "$STEAM_DIR" ]; then
        own_steam=1
        STEAM_DIR="$HOME/.local/share/Steam"
        [ -d "$STEAM_DIR/userdata" ] || STEAM_DIR="$HOME/.steam/steam"
    fi
    restart=0
    if [ "$own_steam" = 1 ] && pgrep -x steam >/dev/null && [ -z "${GAMESCOPE_WAYLAND_DISPLAY:-}" ] &&
        ask "Steam must close for a moment to add the shortcut. Close it now?"; then
        steam -shutdown >/dev/null 2>&1 || true
        for _ in $(seq 60); do
            pgrep -x steam >/dev/null || break
            sleep 1
        done
        restart=1
    fi
    if [ "$own_steam" = 0 ] || ! pgrep -x steam >/dev/null; then
        add_shortcuts || warn "no Steam account found in $STEAM_DIR"
    elif command -v steamos-add-to-steam >/dev/null; then
        steamos-add-to-steam "$launcher"
        info "Shortcut sent to the running Steam"
    else
        warn "add \"$launcher\" to Steam as a non-Steam game"
    fi
    if [ "$restart" = 1 ]; then
        (setsid steam >/dev/null 2>&1 &)
        info "Starting Steam again"
    fi
fi

cat <<EOF

All done! Go back to Gaming Mode and start "$APP_NAME" from your library
(Library > Non-Steam).

Once, in Gaming Mode, while the game runs: press ... > Performance, turn on
"Advanced view" and "Use per-game profile", then "Manual GPU clock" at 1600 MHz.
Turn the frame limit OFF there: the emulator caps the game at 30 fps itself.

Good to know:
  - The first start builds shaders: expect stutter for the first minutes, then it goes away.
  - Shaking the Deck shakes the controller (graffiti), and the trackpads work as the
    PS4 touchpad. On a pad without motion, use the keyboard keys Home (shake) and T
    (two-finger touchpad press) from the game's controller settings.
To update later, run:
    "$DIR/install.sh" --update
EOF
