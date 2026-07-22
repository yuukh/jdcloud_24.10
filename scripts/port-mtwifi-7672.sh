#!/bin/bash

set -euo pipefail

TOPDIR="${1:-$PWD}"
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ASSET_DIR="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"

LGS_COMMIT="181a4ee52239d0fc9c2b38ec22d8ce1dc5c02026"
SOURCE_BASE="https://raw.githubusercontent.com/lgs2007m/immortalwrt-mt798x/${LGS_COMMIT}"

MTWIFI_ARCHIVE="mt79xx_20231229-4012a0.tar.xz"
MTWIFI_SHA256="763bfc104337ef5ff45602db0371141ecf59e5cfde6ef8614a8f16a3f2f274c6"
WARP_ARCHIVE="warp_20231229-5f71ec.tar.xz"
WARP_SHA256="e7e2c2221207f4e72ff7f839ddfa5d7fb87b1be86dee608838a94f26124e6aaf"
CONNINFRA_ARCHIVE="mt79xx_conninfra_20231229-f2fa25.tar.xz"
CONNINFRA_SHA256="672929de84ff2828ab8caef7ae56b6b3aac91210d5700c568559ebfa2d3cd176"
DATCONF_ARCHIVE="datconf-757f9679.tar.bz2"
DATCONF_SHA256="3cbb7489e3df1be52d184b600129a6081e7225981acdbcc5c2cb7149f11c37f1"

die() {
	echo "错误: $*" >&2
	exit 1
}

for required in \
	"$TOPDIR/rules.mk" \
	"$TOPDIR/target/linux/mediatek/Makefile" \
	"$TOPDIR/package/mtk/drivers/mt_wifi/Makefile" \
	"$TOPDIR/package/mtk/drivers/warp/Makefile" \
	"$TOPDIR/package/mtk/drivers/conninfra/Makefile"; do
	[ -f "$required" ] || die "不是受支持的 ImmortalWrt 源码树，缺少 $required"
done

grep -q '^KERNEL_PATCHVER:=6\.6$' "$TOPDIR/target/linux/mediatek/Makefile" || \
	die "此移植仅适用于 MediaTek Linux 6.6 源码树"

for required_patch in \
	mtwifi-7.6.7.2-linux-6.6.patch \
	warp-20231229-linux-6.6.patch \
	conninfra-20231229-linux-6.6.patch \
	iwinfo-mtwifi-v7672.patch; do
	[ -f "$ASSET_DIR/patches/$required_patch" ] || die "缺少补丁 patches/$required_patch"
done

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/mtwifi-7672.XXXXXX")"
trap 'rm -rf "$WORK_DIR"' EXIT

fetch_checked() {
	local relative_path="$1"
	local destination="$2"
	local expected_hash="$3"
	local actual_hash

	mkdir -p "$(dirname -- "$destination")"
	if [ -f "$destination" ]; then
		actual_hash="$(sha256sum "$destination" | awk '{print $1}')"
		if [ "$actual_hash" = "$expected_hash" ]; then
			echo "使用缓存: ${destination#$TOPDIR/}"
			return
		fi
		mv "$destination" "${destination}.invalid.$$"
	fi

	echo "下载: $relative_path"
	curl -fL --retry 3 --retry-delay 2 \
		-o "${destination}.partial" "$SOURCE_BASE/$relative_path"
	actual_hash="$(sha256sum "${destination}.partial" | awk '{print $1}')"
	[ "$actual_hash" = "$expected_hash" ] || \
		die "$relative_path 校验失败：期望 $expected_hash，实际 $actual_hash"
	mv "${destination}.partial" "$destination"
}

replace_tree() {
	local source_dir="$1"
	local target_dir="$2"
	local backup_dir="${target_dir}.pre-v7672.$$"

	[ -d "$source_dir" ] || die "待安装源码目录不存在: $source_dir"
	[ -d "$(dirname -- "$target_dir")" ] || die "目标父目录不存在: $target_dir"

	if [ -e "$target_dir" ]; then
		mv "$target_dir" "$backup_dir"
	fi
	if mv "$source_dir" "$target_dir"; then
		[ ! -e "$backup_dir" ] || rm -rf "$backup_dir"
	else
		[ ! -e "$backup_dir" ] || mv "$backup_dir" "$target_dir"
		die "替换源码失败: $target_dir"
	fi
}

ensure_config() {
	local key="$1"
	local value="$2"
	local config="$TOPDIR/.config"

	[ -f "$config" ] || return 0
	sed -i "/^${key}=/d; /^# ${key} is not set$/d" "$config"
	printf '%s=%s\n' "$key" "$value" >> "$config"
}

apply_openwrt_patch() {
	local patch_file="$1"

	if patch -s --batch --forward --dry-run -d "$TOPDIR" -p1 < "$patch_file" >/dev/null 2>&1; then
		patch -s --batch --forward -d "$TOPDIR" -p1 < "$patch_file"
	elif patch -s --batch --reverse --dry-run -d "$TOPDIR" -p1 < "$patch_file" >/dev/null 2>&1; then
		echo "补丁已应用: $(basename -- "$patch_file")"
	else
		die "补丁无法应用: $patch_file"
	fi
}

fetch_checked "dl/$MTWIFI_ARCHIVE" "$TOPDIR/dl/$MTWIFI_ARCHIVE" "$MTWIFI_SHA256"
fetch_checked "dl/$WARP_ARCHIVE" "$TOPDIR/dl/$WARP_ARCHIVE" "$WARP_SHA256"
fetch_checked "dl/$CONNINFRA_ARCHIVE" "$TOPDIR/dl/$CONNINFRA_ARCHIVE" "$CONNINFRA_SHA256"
fetch_checked "dl/$DATCONF_ARCHIVE" "$TOPDIR/dl/$DATCONF_ARCHIVE" "$DATCONF_SHA256"

mkdir -p "$WORK_DIR/mtwifi" "$WORK_DIR/warp" "$WORK_DIR/conninfra"
tar -xJf "$TOPDIR/dl/$MTWIFI_ARCHIVE" -C "$WORK_DIR/mtwifi"
tar -xJf "$TOPDIR/dl/$WARP_ARCHIVE" -C "$WORK_DIR/warp"
tar -xJf "$TOPDIR/dl/$CONNINFRA_ARCHIVE" -C "$WORK_DIR/conninfra"

[ -f "$WORK_DIR/mtwifi/mt_wifi_ap/Makefile" ] || die "mt_wifi 压缩包目录结构异常"
[ -f "$WORK_DIR/warp/warp/Makefile" ] || die "WARP 压缩包目录结构异常"
[ -f "$WORK_DIR/conninfra/conninfra/Makefile" ] || die "conninfra 压缩包目录结构异常"

patch -s -d "$WORK_DIR/mtwifi" -p1 < "$ASSET_DIR/patches/mtwifi-7.6.7.2-linux-6.6.patch"
patch -s -d "$WORK_DIR/warp/warp" -p1 < "$ASSET_DIR/patches/warp-20231229-linux-6.6.patch"
patch -s -d "$WORK_DIR/conninfra/conninfra" -p1 < "$ASSET_DIR/patches/conninfra-20231229-linux-6.6.patch"

replace_tree "$WORK_DIR/mtwifi" "$TOPDIR/package/mtk/drivers/mt_wifi/src"
replace_tree "$WORK_DIR/warp/warp" "$TOPDIR/package/mtk/drivers/warp/src"
replace_tree "$WORK_DIR/conninfra/conninfra" "$TOPDIR/package/mtk/drivers/conninfra/src"
apply_openwrt_patch "$ASSET_DIR/patches/iwinfo-mtwifi-v7672.patch"

MTWIFI_FW_DIR="$TOPDIR/package/mtk/drivers/mt_wifi/files/mt7986-fw-20240823"
mkdir -p "$MTWIFI_FW_DIR"
while read -r firmware_hash firmware_name; do
	fetch_checked \
		"package/mtk/drivers/mt_wifi/files/mt7986-fw-20240823/$firmware_name" \
		"$TOPDIR/dl/mt7986-fw-20240823/$firmware_name" \
		"$firmware_hash"
	install -m 0644 "$TOPDIR/dl/mt7986-fw-20240823/$firmware_name" "$MTWIFI_FW_DIR/$firmware_name"
done <<'EOF'
f8ef9893fe422d24ac4454fa2177a99112d5ada99ec206e2b665f60c09210387 7986_WACPU_RAM_CODE_release.bin
5eb175d860cc6f148cfa894ec796f1c64bfd23295d3eb235642205b68e147dfc WIFI_RAM_CODE_MT7986.bin
5a5340e8eaf49a7c4530560891a6618bc6107256f7eb215fa883d0fa5640d8d1 WIFI_RAM_CODE_MT7986_MT7975.bin
9dba42e316c8fcfe821bbf0e3b34c6a6e7e418688831a7dfb24e17589fedfb4e mt7986_patch_e1_hdr.bin
a62951769098b056ff3644881c171716a68b617223aa139b3bca5cf4f29b3070 mt7986_patch_e1_hdr_mt7975.bin
EOF

WARP_FW_DIR="$TOPDIR/package/mtk/drivers/warp/files/mt7986-fw-20231229"
mkdir -p "$WARP_FW_DIR"
for firmware in "$TOPDIR"/package/mtk/drivers/warp/src/bin/7986_WOCPU*_RAM_CODE_release.bin; do
	[ -f "$firmware" ] || die "WARP 20231229 源码中缺少 MT7986 WO 固件"
	install -m 0644 "$firmware" "$WARP_FW_DIR/$(basename -- "$firmware")"
done

MTWIFI_MAKEFILE="$TOPDIR/package/mtk/drivers/mt_wifi/Makefile"
sed -i \
	-e 's/^MT7986_FW_DIR := .*/MT7986_FW_DIR := mt7986-fw-20240823/' \
	-e 's/^PKG_VERSION:=7\.6\.[0-9.]*-$(PKG_SUFFIX)$/PKG_VERSION:=7.6.7.2-$(PKG_SUFFIX)/' \
	"$MTWIFI_MAKEFILE"
if grep -q '^define FIXUP_NEW_MCU_FW_API$' "$MTWIFI_MAKEFILE"; then
	sed -i '/^define FIXUP_NEW_MCU_FW_API$/,/^Hooks\/Prepare\/Post := FIXUP_NEW_MCU_FW_API$/d' "$MTWIFI_MAKEFILE"
fi
grep -q '^PATCH_DIR:=$' "$MTWIFI_MAKEFILE" || \
	sed -i '/^PKG_VERSION:=7\.6\.7\.2-$(PKG_SUFFIX)$/a PATCH_DIR:=' "$MTWIFI_MAKEFILE"

WARP_MAKEFILE="$TOPDIR/package/mtk/drivers/warp/Makefile"
sed -i \
	-e 's/^PKG_VERSION:=20221209-3e6ae1$/PKG_VERSION:=20231229-5f71ec/' \
	-e 's/^MT7986_NEW_WOFW_DIR := .*/MT7986_NEW_WOFW_DIR := mt7986-fw-20231229/' \
	"$WARP_MAKEFILE"
grep -q '^PATCH_DIR:=$' "$WARP_MAKEFILE" || \
	sed -i '/^PKG_VERSION:=20231229-5f71ec$/a PATCH_DIR:=' "$WARP_MAKEFILE"

sed -i 's/^PKG_VERSION:=bbf588$/PKG_VERSION:=f2fa25/' \
	"$TOPDIR/package/mtk/drivers/conninfra/Makefile"
sed -i 's/^PKG_REVISION:=6bb733f7$/PKG_REVISION:=757f9679/' \
	"$TOPDIR/package/mtk/applications/datconf/Makefile"

ensure_config CONFIG_MTK_MT7986_NEW_FW y
ensure_config CONFIG_MTK_WIFI_FW_BIN_LOAD y
ensure_config CONFIG_WARP_CHIPSET '"mt7986"'
ensure_config CONFIG_WARP_VERSION 2
ensure_config CONFIG_WARP_NEW_FW y

grep -q '^PKG_VERSION:=7\.6\.7\.2-$(PKG_SUFFIX)$' "$MTWIFI_MAKEFILE" || die "mt_wifi 版本写入失败"
grep -q '^MT7986_FW_DIR := mt7986-fw-20240823$' "$MTWIFI_MAKEFILE" || die "MT7986 固件目录写入失败"
grep -q '^PATCH_DIR:=$' "$MTWIFI_MAKEFILE" || die "mt_wifi 旧补丁目录未禁用"
! grep -q '^Hooks/Prepare/Post := FIXUP_NEW_MCU_FW_API$' "$MTWIFI_MAKEFILE" || die "mt_wifi 旧固件 API 补丁未禁用"
grep -q '^PKG_VERSION:=20231229-5f71ec$' "$WARP_MAKEFILE" || die "WARP 版本写入失败"
grep -q '^MT7986_NEW_WOFW_DIR := mt7986-fw-20231229$' "$WARP_MAKEFILE" || die "WARP 固件目录写入失败"
grep -q '^PKG_VERSION:=f2fa25$' "$TOPDIR/package/mtk/drivers/conninfra/Makefile" || die "conninfra 版本写入失败"
grep -q '^PKG_REVISION:=757f9679$' "$TOPDIR/package/mtk/applications/datconf/Makefile" || die "datconf 版本写入失败"
grep -q '^#define AP_DRIVER_VERSION.*"7.6.7.2"$' "$TOPDIR/package/mtk/drivers/mt_wifi/src/mt_wifi/include/os/rt_linux.h" || die "mt_wifi 驱动版本校验失败"
grep -q '^#define OID_GET_CEN_CH1.*0x09F0$' "$TOPDIR/package/mtk/drivers/mt_wifi/src/mt_wifi/embedded/include/oid.h" || die "mt_wifi CEN_CH1 OID 写入失败"
grep -q '^#define OID_GET_CEN_CH2.*0x09F1$' "$TOPDIR/package/mtk/drivers/mt_wifi/src/mt_wifi/embedded/include/oid.h" || die "mt_wifi CEN_CH2 OID 写入失败"
grep -q '^#define OID_GET_CHANNEL_LIST.*0x09C0$' "$TOPDIR/package/mtk/drivers/mt_wifi/src/mt_wifi/embedded/include/oid.h" || die "mt_wifi CHANNEL_LIST OID 校验失败"
grep -q '^#define OID_GET_CEN_CH1.*0x09F0$' "$TOPDIR/package/network/utils/iwinfo/src/mtwifi.h" || die "iwinfo CEN_CH1 OID 写入失败"
grep -q '^#define OID_GET_CEN_CH2.*0x09F1$' "$TOPDIR/package/network/utils/iwinfo/src/mtwifi.h" || die "iwinfo CEN_CH2 OID 写入失败"

cat > "$TOPDIR/package/mtk/drivers/mt_wifi/VERSION.v7672" <<EOF
mt_wifi=7.6.7.2
mt7986_fw=20240823
warp=20231229-5f71ec
conninfra=f2fa25
datconf=757f9679
source_commit=$LGS_COMMIT
kernel=6.6
EOF

echo "已移植 mt_wifi 7.6.7.2 + MT7986 FW 20240823（Linux 6.6）"
