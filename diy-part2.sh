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