#!/usr/bin/env bash
# Full pinned build, without touching the original source checkout.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
CACHE=${HNAT418_CACHE:-/cache/hnat418-firmware}
JOBS=${JOBS:-6}
export HNAT418_LAN_IP=${HNAT418_LAN_IP:-192.168.3.1}
export FORCE_UNSAFE_CONFIGURE=1
REV=ec9ef10efc65da1e6d1de4e2c043c0e13d08eed8
DATCONF_ARCHIVE=datconf-6bb733f7.tar.bz2
DATCONF_SHA256=c1632b1950407e731c8a36b14d50d6e04bed7e7218938283e06e16ac9050d20b
MODE=${1:-build}
case "$MODE" in build|prepare) ;; *) echo 'Usage: bash build-debug.sh [build|prepare]' >&2; exit 2;; esac
CACHE=$(realpath -m -- "$CACHE")
case "$CACHE" in /cache/*) ;; *) echo 'HNAT418_CACHE must resolve below /cache/' >&2; exit 2;; esac
[[ $JOBS =~ ^[1-9][0-9]?$ ]] || { echo 'Invalid JOBS' >&2; exit 2; }
mkdir -p "$CACHE"
SOURCE="$CACHE/openwrt"

seed_datconf_source() {
    local target="$CACHE/dl/$DATCONF_ARCHIVE"
    local encoded="$ROOT/debug/vendor/$DATCONF_ARCHIVE.b64"
    local tmp="$target.part"
    local listing="$target.list.part"

    mkdir -p "$CACHE/dl"
    if [ -f "$target" ] && printf '%s  %s\n' "$DATCONF_SHA256" "$target" | sha256sum -c - >/dev/null 2>&1; then
        return 0
    fi
    rm -f "$target" "$tmp" "$listing"
    [ -s "$encoded" ] || { echo "Missing pinned datconf build input: $encoded" >&2; exit 1; }
    base64 -d "$encoded" > "$tmp"
    printf '%s  %s\n' "$DATCONF_SHA256" "$tmp" | sha256sum -c -
    bzip2 -t "$tmp"
    tar -tjf "$tmp" > "$listing"
    grep -qx 'datconf/datconf/libdatconf.c' "$listing"
    grep -qx 'datconf/kvcutil/libkvcutil.c' "$listing"
    rm -f "$listing"
    mv "$tmp" "$target"
}

if [ ! -d "$SOURCE/.git" ]; then
    git init "$SOURCE"
    git -C "$SOURCE" remote add origin https://github.com/padavanonly/immortalwrt-mt798x-6.6
    git -C "$SOURCE" fetch --depth 1 origin "$REV"
    git -C "$SOURCE" checkout --detach FETCH_HEAD
fi
[ "$(git -C "$SOURCE" rev-parse HEAD)" = "$REV" ] || { echo 'Wrong cached source revision' >&2; exit 1; }
cd "$SOURCE"
# Once prepared, rerunning the build reuses precisely these inputs; changing
# the debug source requires a different HNAT418_CACHE, not a silent mixture.
input_id="$(python3 "$ROOT/debug/source-id.py"):$HNAT418_LAN_IP"
if [ -e "$CACHE/prepared" ]; then
    [ "$(cat "$CACHE/prepared")" = "$input_id" ] || { echo 'Source revision changed; select a new HNAT418_CACHE.' >&2; exit 1; }
else
    [ -z "$(git status --porcelain)" ] || { echo 'Partial/modified build tree; inspect it and use a new HNAT418_CACHE.' >&2; exit 1; }
    cp "$ROOT/feeds.conf.default" feeds.conf.default
    bash "$ROOT/diy-part1.sh"
    ./scripts/feeds update -a
    ./scripts/feeds install -a
    cp "$ROOT/immortalwrt.config" .config
    bash "$ROOT/diy-part2.sh"
    printf '\nCONFIG_CCACHE=y\nCONFIG_CCACHE_DIR="%s/ccache"\nCONFIG_DOWNLOAD_FOLDER="%s/dl"\n' "$CACHE" "$CACHE" >> .config
    make defconfig
    grep -qx CONFIG_TARGET_mediatek_filogic_DEVICE_jdcloud_re-cp-03=y .config
    grep -qx CONFIG_PACKAGE_hnat418-debug=y .config
    grep -qx CONFIG_PACKAGE_kmod-mediatek_hnat=y .config
    printf '%s\n' "$input_id" > "$CACHE/prepared"
fi
seed_datconf_source
[ "$MODE" = prepare ] && { echo "Prepared: $SOURCE"; exit 0; }
make download -j"$JOBS"
make -j"$JOBS" V=s || make -j2 V=s
python3 "$ROOT/debug/export-build.py" "$SOURCE" "$CACHE/delivery"
echo "Firmware, Windows runner and matching build metadata: $CACHE/delivery"
