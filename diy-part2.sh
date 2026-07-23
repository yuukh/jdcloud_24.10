#!/bin/bash
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

set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

install_kernel_patch() {
	local patch_name="$1"
	local patch_dir="$PWD/target/linux/mediatek/patches-6.6"

	if [ ! -d "$patch_dir" ]; then
		echo "Kernel patch directory does not exist: $patch_dir" >&2
		exit 1
	fi

	echo "Installing kernel patch: $patch_name"
	install -m 0644 "$SCRIPT_DIR/patches/$patch_name" "$patch_dir/$patch_name"
}

# Segment CPU-generated GSO traffic before WED/PPE reinjection.
install_kernel_patch 999-9101-hnat-cpu-wifi-gso-fix.patch

# Modify default IP
#sed -i 's/192.168.1.1/192.168.50.5/g' package/base-files/files/bin/config_generate

# Modify default theme
#sed -i 's/luci-theme-bootstrap/luci-theme-argon/g' feeds/luci/collections/luci/Makefile

# Modify hostname
#sed -i 's/OpenWrt/P3TERX-Router/g' package/base-files/files/bin/config_generate

# Set default WiFi SSID and password (applied on first boot via uci-defaults)
cat > package/base-files/files/etc/uci-defaults/99-wifi-defaults <<'EOF'
#!/bin/sh
. /lib/functions.sh

SSID="ImmortalWrt"
KEY="immortalwrt"

# MTK mt_wifi (mtwifi-cfg): default wireless devices are rax0 (2.4G) and rai0 (5G)
for dev in $(uci show wireless 2>/dev/null | sed -n 's/^wireless\.\(@wifi-iface\[[0-9]\+\]\)\.device=.*/\1/p' | sort -u); do
    uci -q set wireless.$dev.ssid="$SSID"
    uci -q set wireless.$dev.encryption='psk2'
    uci -q set wireless.$dev.key="$KEY"
done

uci commit wireless
EOF
chmod +x package/base-files/files/etc/uci-defaults/99-wifi-defaults
