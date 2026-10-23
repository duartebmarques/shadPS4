#!/bin/bash
# package.sh <Shadps4-sdl.AppImage> <tag> <out dir>: the release folder zip (shadps4-<tag>-linux.zip),
# as CI publishes it. One folder: the extracted build (Steam's runtime can lack fusermount for
# AppImages), shadPS4.sh, the games manifest and helpers, and the default user files.
set -euo pipefail
appimage="$(readlink -f "$1")" tag="$2" out="$(mkdir -p "$3" && cd "$3" && pwd)"
repo="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
dir="$stage/shadPS4-infamous"
mkdir -p "$dir"
chmod a+x "$appimage"
(cd "$stage" && "$appimage" --appimage-extract >/dev/null)
mv "$stage/squashfs-root" "$dir/app"
cp -r "$repo/dist/user" "$dir/dist-user"
cp -r "$repo/dist/tools" "$dir/tools"
cp "$repo/dist/shadPS4.sh" "$repo/dist/games.json" "$repo/dist/install.sh" "$repo/LICENSE" \
    "$repo/README.md" "$dir/"
echo "$tag" > "$dir/VERSION"
chmod a+x "$dir/shadPS4.sh" "$dir/install.sh" "$dir/tools/sp4.py"
rm -f "$out/shadps4-$tag-linux.zip"
(cd "$stage" && zip -qry "$out/shadps4-$tag-linux.zip" shadPS4-infamous)
echo "$out/shadps4-$tag-linux.zip"
