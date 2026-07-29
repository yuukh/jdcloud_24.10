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
	"$TOPDIR/package/mtk/applications/datconf/Makefile" \
	"$TOPDIR/package/mtk/applications/mtwifi-cfg/files/mtwifi.sh" \
	"$TOPDIR/package/mtk/drivers/mt_wifi/Makefile" \
	"$TOPDIR/package/mtk/drivers/warp/Makefile" \
	"$TOPDIR/package/mtk/drivers/conninfra/Makefile"; do
	[ -f "$required" ] || die "不是受支持的 ImmortalWrt 源码树，缺少 $required"
done

for required_command in awk curl install patch sed sha256sum tar; do
	command -v "$required_command" >/dev/null 2>&1 || \
		die "缺少构建工具: $required_command"
done

grep -q '^KERNEL_PATCHVER:=6\.6$' "$TOPDIR/target/linux/mediatek/Makefile" || \
	die "此移植仅适用于 MediaTek Linux 6.6 源码树"
grep -Fqx 'PKG_SOURCE:=$(PKG_NAME)-$(PKG_REVISION).tar.bz2' \
	"$TOPDIR/package/mtk/applications/datconf/Makefile" || \
	die "datconf 包结构不受支持"

for required_patch in \
	mtwifi-7.6.7.2-linux-6.6.patch \
	warp-20231229-linux-6.6.patch \
	warp-20231229-runtime-fixes.patch \
	conninfra-20231229-linux-6.6.patch \
	iwinfo-mtwifi-v7672.patch \
	110-mtwifi-secure-defaults.patch; do
	[ -f "$ASSET_DIR/patches/$required_patch" ] || die "缺少补丁 patches/$required_patch"
done

mkdir -p "$TOPDIR/tmp"
WORK_DIR="$(mktemp -d "$TOPDIR/tmp/mtwifi-7672.XXXXXX")"
trap 'rm -rf "$WORK_DIR"' EXIT

fetch_checked() {
	local relative_path="$1"
	local destination="$2"
	local expected_hash="$3"
	local actual_hash
	local partial="${destination}.partial"

	mkdir -p "$(dirname -- "$destination")"
	if [ -f "$destination" ]; then
		actual_hash="$(sha256sum "$destination" | awk '{print $1}')"
		if [ "$actual_hash" = "$expected_hash" ]; then
			echo "使用缓存: ${destination#$TOPDIR/}"
			return
		fi
		echo "删除校验失败的缓存: ${destination#$TOPDIR/}" >&2
		rm -f -- "$destination"
	fi

	echo "下载: $relative_path"
	rm -f -- "$partial"
	if ! curl -fL --connect-timeout 20 --retry 5 --retry-delay 2 \
		--retry-all-errors -o "$partial" "$SOURCE_BASE/$relative_path"; then
		rm -f -- "$partial"
		die "$relative_path 下载失败"
	fi
	actual_hash="$(sha256sum "$partial" | awk '{print $1}')"
	if [ "$actual_hash" != "$expected_hash" ]; then
		rm -f -- "$partial"
		die "$relative_path 校验失败：期望 $expected_hash，实际 $actual_hash"
	fi
	mv "$partial" "$destination"
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

	if patch -s --batch --forward --no-backup-if-mismatch --dry-run \
		-d "$TOPDIR" -p1 < "$patch_file" >/dev/null 2>&1; then
		patch -s --batch --forward --no-backup-if-mismatch \
			-d "$TOPDIR" -p1 < "$patch_file"
	elif patch -s --batch --reverse --no-backup-if-mismatch --dry-run \
		-d "$TOPDIR" -p1 < "$patch_file" >/dev/null 2>&1; then
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

patch -s --batch --forward --no-backup-if-mismatch \
	-d "$WORK_DIR/mtwifi" -p1 < "$ASSET_DIR/patches/mtwifi-7.6.7.2-linux-6.6.patch"
patch -s --batch --forward --no-backup-if-mismatch \
	-d "$WORK_DIR/warp/warp" -p1 < "$ASSET_DIR/patches/warp-20231229-linux-6.6.patch"
patch -s --batch --forward --no-backup-if-mismatch \
	-d "$WORK_DIR/warp/warp" -p1 < "$ASSET_DIR/patches/warp-20231229-runtime-fixes.patch"
patch -s --batch --forward --no-backup-if-mismatch \
	-d "$WORK_DIR/conninfra/conninfra" -p1 < "$ASSET_DIR/patches/conninfra-20231229-linux-6.6.patch"

WARP_PROC="$WORK_DIR/warp/warp/warp_proc.c"
[ "$(grep -c '^static const struct proc_ops proc_' "$WARP_PROC")" -eq 13 ] || \
	die "WARP proc_ops 移植不完整"
! grep -q '^static const struct file_operations proc_' "$WARP_PROC" || \
	die "WARP 仍包含旧式 proc file_operations"
grep -Fq 'warp_proc_copy_from_user(char *dst, size_t dst_size,' "$WARP_PROC" || \
	die "WARP proc 输入边界检查未应用"
grep -Fq 'input_total > 1 && input_total <= 3' "$WARP_PROC" || \
	die "WARP WO 命令参数边界检查未应用"
grep -Fq '(sub_str = strsep(&cursor, " ")) != NULL' "$WARP_PROC" || \
	die "WARP WO 命令解析并发修复未应用"
! grep -Fq 'char *__strtok;' "$WARP_PROC" || \
	die "WARP 仍包含非线程安全 strtok 状态"
grep -Fq 'ring_id >= ring_ctrl->ring_num || idx >= ring_ctrl->ring_len' \
	"$WORK_DIR/warp/warp/wed.c" || die "WARP WED ring 边界检查未应用"
grep -Fq 'ring_id >= ring_ctrl->ring_num || idx >= ring_ctrl->ring_len' \
	"$WORK_DIR/warp/warp/wdma.c" || die "WARP WDMA ring 边界检查未应用"
grep -Fq 'void __iomem *base_addr;' "$WORK_DIR/warp/warp/mcu/warp_ccif.h" || \
	die "WARP CCIF I/O 地址类型修复未应用"
grep -Fq 'irq_dispose_mapping(ccif->irq);' "$WORK_DIR/warp/warp/mcu/warp_ccif.c" || \
	die "WARP CCIF IRQ 映射释放修复未应用"

replace_tree "$WORK_DIR/mtwifi" "$TOPDIR/package/mtk/drivers/mt_wifi/src"
replace_tree "$WORK_DIR/warp/warp" "$TOPDIR/package/mtk/drivers/warp/src"
replace_tree "$WORK_DIR/conninfra/conninfra" "$TOPDIR/package/mtk/drivers/conninfra/src"
apply_openwrt_patch "$ASSET_DIR/patches/iwinfo-mtwifi-v7672.patch"
apply_openwrt_patch "$ASSET_DIR/patches/110-mtwifi-secure-defaults.patch"

MTWIFI_FW_DIR="$TOPDIR/package/mtk/drivers/mt_wifi/files/mt7986-fw-20240823"
mkdir -p "$MTWIFI_FW_DIR"
firmware_count=0
while read -r firmware_hash firmware_name; do
	bundled_firmware="$ASSET_DIR/mt7986-fw-20240823/$firmware_name"
	cached_firmware="$TOPDIR/dl/mt7986-fw-20240823/$firmware_name"
	if [ -f "$bundled_firmware" ]; then
		actual_hash="$(sha256sum "$bundled_firmware" | awk '{print $1}')"
		[ "$actual_hash" = "$firmware_hash" ] || \
			die "仓库固件 $firmware_name 校验失败：期望 $firmware_hash，实际 $actual_hash"
		echo "使用仓库固件: mt7986-fw-20240823/$firmware_name"
		firmware_source="$bundled_firmware"
	else
		fetch_checked \
			"package/mtk/drivers/mt_wifi/files/mt7986-fw-20240823/$firmware_name" \
			"$cached_firmware" \
			"$firmware_hash"
		firmware_source="$cached_firmware"
	fi
	install -m 0644 "$firmware_source" "$MTWIFI_FW_DIR/$firmware_name"
	actual_hash="$(sha256sum "$MTWIFI_FW_DIR/$firmware_name" | awk '{print $1}')"
	[ "$actual_hash" = "$firmware_hash" ] || die "安装后的固件 $firmware_name 校验失败"
	firmware_count=$((firmware_count + 1))
done <<'EOF'
f8ef9893fe422d24ac4454fa2177a99112d5ada99ec206e2b665f60c09210387 7986_WACPU_RAM_CODE_release.bin
5eb175d860cc6f148cfa894ec796f1c64bfd23295d3eb235642205b68e147dfc WIFI_RAM_CODE_MT7986.bin
5a5340e8eaf49a7c4530560891a6618bc6107256f7eb215fa883d0fa5640d8d1 WIFI_RAM_CODE_MT7986_MT7975.bin
9dba42e316c8fcfe821bbf0e3b34c6a6e7e418688831a7dfb24e17589fedfb4e mt7986_patch_e1_hdr.bin
a62951769098b056ff3644881c171716a68b617223aa139b3bca5cf4f29b3070 mt7986_patch_e1_hdr_mt7975.bin
EOF
[ "$firmware_count" -eq 5 ] || die "MT7986 20240823 固件数量异常: $firmware_count"

WARP_FW_DIR="$TOPDIR/package/mtk/drivers/warp/files/mt7986-fw-20231229"
mkdir -p "$WARP_FW_DIR"
for stale_firmware in "$WARP_FW_DIR"/7986_WOCPU*_RAM_CODE_release.bin; do
	[ ! -e "$stale_firmware" ] || rm -f -- "$stale_firmware"
done
while read -r firmware_hash firmware_name; do
	firmware="$TOPDIR/package/mtk/drivers/warp/src/bin/$firmware_name"
	[ -f "$firmware" ] || die "WARP 20231229 源码中缺少 $firmware_name"
	actual_hash="$(sha256sum "$firmware" | awk '{print $1}')"
	[ "$actual_hash" = "$firmware_hash" ] || \
		die "WARP 固件 $firmware_name 校验失败：期望 $firmware_hash，实际 $actual_hash"
	install -m 0644 "$firmware" "$WARP_FW_DIR/$firmware_name"
done <<'EOF'
79a50d78d87e5390f6dcb1b5a4b113781d37d82b46c6b4ba806c974310507d5a 7986_WOCPU0_RAM_CODE_release.bin
7924a4a6dec1c775eb50bc2557706e81e9b7f57366c57beb9b767918d1f8c186 7986_WOCPU1_RAM_CODE_release.bin
EOF

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
grep -q 'set wireless\.default_${dev}\.encryption=psk2' \
	"$TOPDIR/package/mtk/applications/mtwifi-cfg/files/mtwifi.sh" || \
	die "mtwifi 默认加密配置写入失败"
grep -q 'set wireless\.default_${dev}\.key=${key}' \
	"$TOPDIR/package/mtk/applications/mtwifi-cfg/files/mtwifi.sh" || \
	die "mtwifi 默认密钥配置写入失败"

cat > "$TOPDIR/package/mtk/drivers/mt_wifi/VERSION.v7672" <<EOF
mt_wifi=7.6.7.2
mt7986_fw=20240823
warp=20231229-5f71ec
conninfra=f2fa25
datconf=757f9679
source_commit=$LGS_COMMIT
kernel=6.6
port_revision=20260729.3
EOF

echo "已移植 mt_wifi 7.6.7.2 + MT7986 FW 20240823（Linux 6.6）"
