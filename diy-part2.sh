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

apply_source_patch() {
	local patch_name="$1"
	local patch_file="$SCRIPT_DIR/patches/$patch_name"

	if patch -p1 --forward --batch --dry-run < "$patch_file" >/dev/null 2>&1; then
		echo "Applying source patch: $patch_name"
		patch -p1 --forward --batch < "$patch_file"
	elif patch -p1 --reverse --batch --dry-run < "$patch_file" >/dev/null 2>&1; then
		echo "Source patch already applied: $patch_name"
	else
		echo "Source patch does not apply cleanly: $patch_name" >&2
		exit 1
	fi
}

"$SCRIPT_DIR/scripts/port-mtwifi-7672.sh" "$PWD"

# Keep both radios encrypted on a freshly generated MTK Wi-Fi configuration.
apply_source_patch 110-mtwifi-secure-defaults.patch

# Modify default IP
#sed -i 's/192.168.1.1/192.168.50.5/g' package/base-files/files/bin/config_generate

# Modify default theme
#sed -i 's/luci-theme-bootstrap/luci-theme-argon/g' feeds/luci/collections/luci/Makefile

# Modify hostname
#sed -i 's/OpenWrt/P3TERX-Router/g' package/base-files/files/bin/config_generate
