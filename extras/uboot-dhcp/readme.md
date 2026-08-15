# MT798x 系列设备 DHCP 自动下发 U-Boot

## 项目简介

本项目在 [hanwckf/bl-mt798x](https://github.com/hanwckf/bl-mt798x) 基础上实现了引导和刷写主线 OpenWrt FIT（.itb）格式固件的功能。基于 U-Boot 2022.07 稳定版制作，全系列支持自动下发 DHCP，无需手动配置 IP 地址，大幅简化刷机过程。

## 支持设备

目前支持以下设备：

- JDCloud RE-CP-03 (京东云无线宝)

## 使用说明

### 基本功能

- **Web Failsafe 模式**：按住 Reset 按钮 5 秒进入，后台地址固定为 `192.168.1.1`
- **DHCP 自动获取**：连接电脑后自动获取 IP 地址，无需手动配置

### 固件后缀说明

- **fit 后缀**：适用于 OpenWrt 23.05 及之前版本
- **fitblk 后缀**：适用于 OpenWrt 23.05 之后的版本
- 如果设备只提供了一种后缀（fit 或 fitblk），则直接使用即可

## 刷机步骤（以 JDCloud RE-CP-03 为例）

### 重要提示

1. **兼容性说明**：本 U-Boot 只能启动主线 OpenWrt 固件，不兼容基于 MTK SDK 的固件
2. **必要条件**：使用本 U-Boot 前，必须先刷入主线 OpenWrt 提供的 BL2（preloader）和 GPT 分区表（如有）
3. **风险提示**：刷机有风险，写入前务必备份好数据。如果刷砖，后果自负

### 刷写命令

```bash
# 写入 GPT 分区表
dd if=mt7986-jdcloud_re-cp-03-gpt.bin of=/dev/mmcblk0 bs=512 seek=0 count=34 conv=fsync

# 写入 BL2 (preloader)
echo 0 > /sys/block/mmcblk0boot0/force_ro
dd if=/dev/zero of=/dev/mmcblk0boot0 bs=512 count=8192 conv=fsync
dd if=openwrt-mediatek-filogic-jdcloud_re-cp-03-preloader.bin of=/dev/mmcblk0boot0 bs=512 conv=fsync

# 写入 U-Boot (FIP)
dd if=/dev/zero of=/dev/mmcblk0 bs=512 seek=13312 count=8192 conv=fsync
dd if=mt7986-jdcloud_re-cp-03-fip.bin of=/dev/mmcblk0 bs=512 seek=13312 conv=fsync
```

## 常见问题

1. **如何选择正确的固件版本？**

   - 对于 OpenWrt 23.05 及之前版本，使用 fit 后缀的 U-Boot
   - 对于 OpenWrt 23.05 之后的版本，使用 fitblk 后缀的 U-Boot

2. **刷机后无法启动怎么办？**

   - 确认是否使用了正确版本的 U-Boot
   - 检查是否已正确刷入 BL2 和 GPT 分区表
   - 确认使用的是主线 OpenWrt 固件，而非 MTK SDK 固件

3. **为什么需要刷入 BL2 和 GPT 分区表？**
   - BL2 是引导加载程序的第一阶段，负责初始化硬件并加载 U-Boot
   - GPT 分区表定义了设备的分区布局，是正确引导系统的必要条件
