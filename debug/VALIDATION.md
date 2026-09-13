# 本次验证记录（2026-09-13）

## 已完成

- 原版源码工作区保持干净，基线固定 `ec9ef10efc65da1e6d1de4e2c043c0e13d08eed8`。
- `debug/kernel/base.patch` 与此前实机使用的候选补丁逐字节一致，SHA-256：`66496f070909a64dce25ab9c1bffca9221e2d967c9fac40d42faf6d7b17d4e9c`。
- 在按原项目 917 个补丁展开的 Linux 6.6.133 上，基线补丁及诊断补丁使用 `patch --fuzz=0` 完整应用成功。
- 最终诊断版本的 ARM64 对象编译通过：记录器、netdev、IPv4 输出、TCP 接收、skb、GRO、bridge、MediaTek Ethernet 与 HNAT 全部相关对象。
- ARM64 QEMU + KASAN + UBSAN 的 10 项内核测试连续两轮全部通过，没有跳过。
- 第二轮测试内核实际启动到 initramfs，使用真实 debugfs 文件操作和 64 KiB TCP 传输：记录 27 个事件，冻结后打开快照，停止 session 后继续完整读取 2,359,392 字节；随后 25 次安静模式启停全部成功。
- 原始 QEMU 日志中未发现 KASAN、UBSAN、BUG、WARNING、Kernel panic 或 runtime error 诊断。
- 17 项用户态测试通过：8 项二进制解析测试、7 项实际 BusyBox ash 控制流程模拟、2 项 PowerShell 控制与下载流程模拟。
- PowerShell AST 解析通过；PowerShell 7 Linux ARM64 下执行模拟 ssh/scp/iperf 流程通过，不冒充在真实 Windows 5.1/网卡上运行过。
- ShellCheck、Bash/BusyBox ash 语法检查通过。
- 干净 OpenWrt 构建树的 feeds 准备、main 其他补丁、诊断覆盖、`make defconfig` 成功。确认 RE-CP-03 profile、HNAT、hnat418-debug、iperf3、ip-full、ss、nftables-json、coreutils-timeout、SFTP 服务端全部选中，DEBUG_FS / HNAT418_DEBUG / IKCONFIG_PROC 已配置。

## 日志与构建位置

```text
/cache/hnat418-debug/kernel-f73682fa75d0c058-objects.log
/cache/hnat418-debug/kunit-run.log
/cache/hnat418-debug/kunit-run-with-initrd.log
/cache/hnat418-debug/kernel-f73682fa75d0c058-kunit/test.log
/cache/hnat418-debug/runtime-smoke/qemu.log
/cache/hnat418-debug/final-python-tests.log
/cache/hnat418-firmware-clean/prepare.log
```

验证期间的 `/cache/hnat418-firmware-clean` 是准备流程测试目录，不建议继续作为交付构建缓存；正式编译用默认 `/cache/hnat418-firmware`。
嵌套候选补丁的 context 行导致 Git whitespace 提示，这是补丁文件的上下文表示；基础补丁保留原 SHA-256，没有改写或“自动修正”它。
feeds 有一个未选中 `luci-app-radicale3` 的缺失依赖警告，未影响诊断包与目标配置展开；不把配置准备等同于全固件编译成功。

## 明确未完成的验收

本次没有编译整个 OpenWrt 世界并生成可刷写 sysupgrade 镜像，没有在 RE-CP-03 实机上启动这版新内核，没有验证真实 Windows SSH 交互或无线硬件流控。
完整固件由用户通过已提供的脚本/Actions 编译；最终导出会再次检查诊断内核配置与关键匹配构建产物。
QEMU 不模拟 MT7986 PPE/WED。上述测试降低内存生命周期、错误处理与脚本流程风险，但不能据此承诺固件绝不会出错或一次测试必定找出最终硬件原因。
