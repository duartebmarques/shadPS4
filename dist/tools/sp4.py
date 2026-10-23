#!/usr/bin/env python3
# Helpers for shadPS4.sh: PS4 param.sfo and pkg headers, patch files, Steam's binary
# shortcuts.vdf, GitHub releases, the games manifest and config.json game folders.
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

def scan(titles, *roots):
    # Prints "kind<TAB>title<TAB>path<TAB>version" for games (gd), updates (gp), DLC (ac) and pkgs
    # of any of the comma separated title ids, and "other" for every other game found.
    titles = set(titles.split(","))
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
                cid = hdr[0x40:0x64].decode("ascii", "replace")
                if hdr[:4] == b"\x7fCNT" and cid[7:16] in titles:
                    print(f"pkg\t{cid[7:16]}\t{p}\t")
            if "sce_sys" not in dirs:
                continue
            p = sfo(os.path.join(cur, "sce_sys", "param.sfo"))
            title = p.get("TITLE_ID")
            cat, ver = str(p.get("CATEGORY", "")), p.get("APP_VER", "")
            if title not in titles:
                # Any other game (dumps merged with their update say gp): the game picker lists it.
                if title and cat[:2] in ("gd", "gp") and os.path.isfile(os.path.join(cur, "eboot.bin")):
                    print(f"other\t{title}\t{cur}\t{ver}")
                continue
            if cat.startswith("gd") and os.path.isfile(os.path.join(cur, "eboot.bin")):
                print(f"game\t{title}\t{cur}\t{ver}")
            elif cat.startswith("gp"):
                print(f"update\t{title}\t{cur}\t{ver}")
            elif cat.startswith("ac"):
                print(f"dlc\t{title}\t{cur}\t")

META = re.compile(r"<Metadata\b[^>]*>", re.S)

def name_of(tag):
    m = re.search(r'\bName="([^"]*)"', tag)
    return m.group(1) if m else None

def patches(new, old, *defaults):
    # Keeps your choices from the old file; patches it does not have yet start as the defaults
    # (or as the new file sets them).
    seen, want = set(), set(defaults)
    if os.path.isfile(old):
        tags = META.findall(open(old, encoding="utf-8").read())
        seen = {name_of(t) for t in tags}
        want = {name_of(t) for t in tags if 'isEnabled="true"' in t} | (want - seen)
    else:
        names = {name_of(t) for t in META.findall(open(new, encoding="utf-8").read())}
        for d in sorted(want - names):
            print(f"warning: patch '{d}' is not in the patch file", file=sys.stderr)
    txt = open(new, encoding="utf-8").read()
    if "<Patch>" not in txt:
        sys.exit("not a patch file")
    def fix(m):
        name = name_of(m.group(0))
        on = name in want or (name not in seen and not defaults and 'isEnabled="true"' in m.group(0))
        on = "true" if on else "false"
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

def shortcut(vdf, name, exe, start, icon, options=""):
    # Adds or updates the shortcut; prints its app id (for the grid artwork names) and added|updated.
    root = [["shortcuts", []]]
    if os.path.isfile(vdf) and os.path.getsize(vdf) > 0:
        with open(vdf, "rb") as f:
            root, _ = vdf_read(f.read())
    sc = next(v for k, v in root if k.lower() == "shortcuts")
    fields = {"AppName": name, "Exe": f'"{exe}"', "StartDir": f'"{start}"', "icon": icon,
              "LaunchOptions": options}
    appid = zlib.crc32((fields["Exe"] + name).encode()) | 0x80000000
    lower = {k.lower(): v for k, v in fields.items()}
    status = "updated"
    for _, entry in sc:
        d = {k.lower(): v for k, v in entry}
        if d.get("exe") == fields["Exe"] and d.get("launchoptions", "") == options:
            for kv in entry:
                kv[1] = lower.get(kv[0].lower(), kv[1])
            if isinstance(d.get("appid"), int):
                appid = d["appid"] & 0xFFFFFFFF
            break
    else:
        status = "added"
        sc.append([str(len(sc)), [
            ["appid", appid - (1 << 32)], ["AppName", name], ["Exe", fields["Exe"]],
            ["StartDir", fields["StartDir"]], ["icon", icon], ["ShortcutPath", ""],
            ["LaunchOptions", options], ["IsHidden", 0], ["AllowDesktopConfig", 1],
            ["AllowOverlay", 1], ["OpenVR", 0], ["Devkit", 0], ["DevkitGameID", ""],
            ["DevkitOverrideAppID", 0], ["LastPlayTime", 0], ["FlatpakAppID", ""],
            ["tags", []]]])
    data = vdf_write(root)
    assert vdf_read(data)[0] == root
    with open(vdf + ".tmp", "wb") as f:
        f.write(data)
    os.replace(vdf + ".tmp", vdf)
    print(f"{appid}\t{status}")

def latest(repo, pattern=r"-linux\.zip$", tag=""):
    # Prints "tag<TAB>url" of the first asset of the latest release (or of the release tag) whose
    # name matches pattern.
    import json, urllib.request
    url = f"https://api.github.com/repos/{repo}/releases/" + (f"tags/{tag}" if tag else "latest")
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


def games(manifest):
    # Prints "id<TAB>name<TAB>serials<TAB>update<TAB>patch_file<TAB>default patches (|)" per game;
    # "-" for an empty field, as shells merge empty tab separated fields.
    import json
    for gid, g in json.load(open(manifest)).items():
        print("\t".join(f or "-" for f in [gid, g["name"], ",".join(g["serials"]), g.get("update"),
                                            g.get("patch_file"), "|".join(g.get("default_patches", []))]))

def config_add_dir(cfg, path):
    # Adds a game folder to config.json's General.install_dirs (keeps everything else).
    import json
    c = json.load(open(cfg)) if os.path.isfile(cfg) else {}
    dirs = c.setdefault("General", {}).setdefault("install_dirs", [])
    # Folders that are gone (moved or renamed games) leave the list.
    before = list(dirs)
    dirs[:] = [d for d in dirs if os.path.isdir(d.get("path", ""))]
    # A folder under one already listed is found by the picker's recursive scan.
    covered = lambda d: d.get("enabled", True) and (path + "/").startswith(d.get("path", "").rstrip("/") + "/")
    if not any(covered(d) for d in dirs):
        dirs.append({"enabled": True, "path": path})
    if dirs != before or not os.path.isfile(cfg):
        tmp = cfg + ".tmp"
        json.dump(c, open(tmp, "w"), indent=4)
        os.replace(tmp, cfg)

def patch_index(index, manifest):
    # Lists each game's patch file under all its serials in the patch folder's files.json.
    import json
    idx = json.load(open(index)) if os.path.isfile(index) else {}
    for g in json.load(open(manifest)).values():
        if g.get("patch_file"):
            idx[g["patch_file"]] = sorted(set(idx.get(g["patch_file"], [])) | set(g["serials"]))
    json.dump(idx, open(index, "w"), indent=2)

def config_for(manifest, cfg_dir, title):
    # Gives a region's title id the tuned settings of its game (custom_configs/<main id>.json).
    import json, shutil
    dst = os.path.join(cfg_dir, title + ".json")
    for gid, g in json.load(open(manifest)).items():
        src = os.path.join(cfg_dir, gid + ".json")
        if title in g["serials"] and not os.path.exists(dst) and os.path.isfile(src):
            shutil.copy(src, dst)

cmd, *args = sys.argv[1:]
{"scan": scan, "patches": patches, "shortcut": shortcut, "latest": latest, "games": games,
 "config-add-dir": config_add_dir, "patch-index": patch_index, "config-for": config_for}[cmd](*args)
