#!/bin/bash
set -euo pipefail
#
# https://github.com/P3TERX/Actions-OpenWrt
# File name: diy-part2.sh
# Description: OpenWrt DIY script part 2 (After Update feeds)
#
# Copyright (c) 2019-2024 P3TERX <https://p3terx.com>
#
# This is free software, licensed under the MIT License.
# See /LICENSE for more information.
#

DIY_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PATCH_DIR="$DIY_DIR/patches"

apply_source_patch() {
	local patch_file="$1"

	echo "Applying ${patch_file#"$DIY_DIR/"}"
	git apply --check "$patch_file"
	git apply "$patch_file"
}

# Install package-source patches into the source/feeds fetched by the workflow.
# These are deliberately copied after `feeds install -a`, so they always target
# the exact miniupnpd/firewall4 packages that the current build will compile.
install -Dm0644 \
	"$PATCH_DIR/miniupnpd/9999-local-fix-private-ip-reconnect.patch" \
	feeds/packages/net/miniupnpd/patches/9999-local-fix-private-ip-reconnect.patch
install -Dm0644 \
	"$PATCH_DIR/firewall4/9999-local-notify-ruleset-reload.patch" \
	package/network/config/firewall4/patches/9999-local-notify-ruleset-reload.patch

# Patch OpenWrt/package integration files after feeds are populated. A dry-run
# is performed first so an upstream layout change fails the workflow instead of
# silently producing a firmware without the requested fixes.
apply_source_patch "$PATCH_DIR/openwrt/100-miniupnpd-fw4-lifecycle.patch"
apply_source_patch "$PATCH_DIR/openwrt/110-pr430-fix-ebtables-ipv6.patch"
apply_source_patch "$PATCH_DIR/openwrt/120-hnat-cpu-to-ge-bypass.patch"

# Modify default IP
#sed -i 's/192.168.1.1/192.168.50.5/g' package/base-files/files/bin/config_generate

# Modify default theme
#sed -i 's/luci-theme-bootstrap/luci-theme-argon/g' feeds/luci/collections/luci/Makefile

# Modify hostname
#sed -i 's/OpenWrt/P3TERX-Router/g' package/base-files/files/bin/config_generate

# Set default WiFi SSID and password.
# Fresh boot / sysupgrade without keeping config: apply defaults.
# Sysupgrade with preserved config: keep existing wireless settings untouched.
cat > package/base-files/files/etc/uci-defaults/99-wifi-defaults <<'EOF'
#!/bin/sh
. /lib/functions.sh

SSID="ImmortalWrt"
KEY="immortalwrt"
AP_COUNT=0

# During a config-preserving sysupgrade, the restored backup is still present
# when uci-defaults runs (boot START=10). It is removed later by init.d/done.
# Do not overwrite any restored wireless settings in that case.
if [ -f /sysupgrade.tgz ] || [ -f /tmp/sysupgrade.tar ]; then
    logger -t wifi-defaults "preserved sysupgrade detected, keeping wireless configuration"
    return 0
fi

set_wifi_defaults() {
    local section="$1"
    local mode

    config_get mode "$section" mode
    [ "$mode" = "ap" ] || return 0

    uci -q set wireless."$section".ssid="$SSID" || return 1
    uci -q set wireless."$section".encryption='psk2' || return 1
    uci -q set wireless."$section".key="$KEY" || return 1
    AP_COUNT=$((AP_COUNT + 1))
}

config_load wireless
config_foreach set_wifi_defaults wifi-iface

# If wireless generation was unexpectedly incomplete, fail so OpenWrt keeps
# this uci-defaults script and retries it on the next boot instead of deleting it.
[ "$AP_COUNT" -gt 0 ] || {
    logger -t wifi-defaults "no AP wifi-iface found, retrying on next boot"
    return 1
}

uci commit wireless || return 1
logger -t wifi-defaults "applied default SSID/password to $AP_COUNT AP interface(s)"
return 0
EOF
chmod +x package/base-files/files/etc/uci-defaults/99-wifi-defaults