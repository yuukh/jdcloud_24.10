# 验证记录

## 2026-09-14：067f7e4 实机复核后的 Full Cone / 捕获修复

实机输入是 `results-20260914-100644`，不是上一节旧版本的结果。
详细证据及尚未闭环的问题见 `results/2026-09-14-067f7e4-audit.md`。
以下是本轮修补后重新执行的验证，不沿用旧固件的通过记录。

| 验证 | 本轮实际结果 |
|---|---|
| 固定 Linux 6.6.133 的相关补丁序列 | 含新 Full Cone 补丁，全部零 fuzz 应用成功 |
| 用户态回归 | 54 项通过，无跳过 |
| Full Cone 真实函数的宿主机逻辑回归 | 5 组测试，ASan/UBSan；旧 confirmed 扩展/状态修改及端口回绕可复现，新代码通过；覆盖新流、已有 helper/expectation、末端端口、耗尽、无效范围及分配/设置失败 |
| ARM64 内核对象 | 记录器、netdev、skb、GRO、bridge、IPv4 输出、TCP 接收、MediaTek Ethernet、HNAT 以及新增的 nf_nat_masquerade.o 全部编译通过 |
| ARM64 QEMU + KASAN + UBSAN | 20 项 KUnit 全部通过；新增两个测试直接调用实际 nf_nat_masquerade_ipv4，验证 confirmed 连接有/无 helper 扩展均不被重新初始化 |
| QEMU 真实 debugfs/TCP 运行测试 | 64 KiB TCP 收发，33 个记录事件；停止 session 后完整读取 2359392 字节；25 次 quiet 启停成功；SMOKE_EXIT=0 |
| 日志复核 | KUnit 原始日志与 runtime smoke 中未发现 BUG、WARNING、KASAN、UBSAN、Kernel panic 或 runtime error 诊断 |
| 静态检查 | ShellCheck、Bash/BusyBox ash 语法、PowerShell AST、git diff --check 通过；合法 unified-diff 末尾上下文空行沿用补丁专用 whitespace 例外，不放宽 C/Python/shell 源码检查 |
| 原始归档重分析 | A 的保留 ACK 计数器累计重传最大值为 1；A 的主要 765 次突发发生于冻结之后；已保留警告均在对应轮次之前，没有被误算为四轮分别新增 |

54 项由原有 43 项加 5 项 Full Cone 测试、6 项警告/区间归类测试组成。
控制器测试同时检查 A/B 的 capture=2、bypass 的 capture=1、quiet 的 capture=0。
PowerShell 测试是在 Linux ARM64 上模拟 ssh/scp/iperf 流程，不冒充真实 Windows 网卡测试。
Full Cone 宿主机测试使用真实函数和依赖桩；内核测试使用未加入哈希表的 confirmed 生命周期夹具。
两者都不是生产 conntrack 的并发压力测试，也不模拟 PPE/WED 或无线固件。

```text
/cache/hnat418-debug/prepare-100644.log
/cache/hnat418-debug/tests-100644.log
/cache/hnat418-debug/objects-100644.log
/cache/hnat418-debug/kernel-16f9491b7da6a0d7-objects.log
/cache/hnat418-debug/kunit-100644.log
/cache/hnat418-debug/kernel-16f9491b7da6a0d7-kunit/test.log
/cache/hnat418-debug/runtime-100644.log
/cache/hnat418-debug/runtime-smoke/qemu.log
/cache/hnat418-debug/session-20260914-100644-audited.json
```

本轮没有构建或刷入新的整机 sysupgrade 镜像。已修补的 Full Cone 生命周期错误不能据此
被宣称为全部重传的实机根因；新的内核警告是否消失、硬件路径的吞吐/重传是否改善，
仍需使用新提交、新缓存构建后复测。旧版启动告警、重复反馈与瞬时重传不得被平均速率掩盖。

## 2026-09-14：残留 metadata 消费点与捕获修复

输入与实机证据见 `results/2026-09-14-post-provenance-audit.md`。
以下验证针对本次修改后的源码，不沿用旧版测试结果。

| 验证 | 本次实际结果 |
|---|---|
| 固定 Linux 6.6.133 的完整 kernel 补丁序列 | 零 fuzz 应用成功 |
| mt_wifi tuple 补丁 | 固定源码上零 fuzz 应用及 git apply --check 成功 |
| 用户态回归 | 43 项通过，无跳过 |
| ARM64 相关内核对象 | 记录器、netdev、skb、GRO、bridge、IPv4 输出、TCP 接收、MediaTek Ethernet、HNAT 全部编译通过 |
| ARM64 QEMU + KASAN + UBSAN | 18 项 KUnit 全部通过 |
| QEMU 真实 debugfs/TCP 运行测试 | 64 KiB TCP 收发，32 个记录事件，停止 session 后完整读取 2359392 字节；25 次 quiet 启停成功；capture=2 与非法模式边界通过 |
| 静态检查 | ShellCheck、Bash/BusyBox ash 语法检查、git diff --check 通过 |

43 项用户态测试包括 7 项控制器、8 项二进制解码、6 项原证据关联、11 项 DSACK 区间覆盖、
3 项 metadata 变更关联、6 项 PowerShell 流程，以及 2 项实际 C 函数 ASan/UBSan 测试。
其中私有 Wi-Fi 回调测试分别执行修补前后的真实函数：旧函数会读写模拟本机 skb 的残留 tuple，
修补后不读不写；真实 ingress 与人工 PPE 回注路径仍正常进入回调流程。
这是函数逻辑验证，不是完整 mt_wifi 模块或无线硬件模拟。

最终 kernel/overlay 验证树为 `kernel-30d3ad51a5650036`。
原始 KUnit 和 runtime 日志均未匹配 BUG、WARNING、KASAN、UBSAN、Kernel panic 或 runtime error 诊断。

```text
/cache/hnat418-debug/tests-20260914-final.log
/cache/hnat418-debug/objects-20260914-final.log
/cache/hnat418-debug/kernel-30d3ad51a5650036-objects.log
/cache/hnat418-debug/kunit-20260914.log
/cache/hnat418-debug/kernel-30d3ad51a5650036-kunit/test.log
/cache/hnat418-debug/runtime-20260914.log
/cache/hnat418-debug/runtime-smoke/qemu.log
/cache/hnat418-debug/session-20260914-022708.json
```

本次没有编译完整 OpenWrt 镜像或整个 mt_wifi 模块，没有生成新 sysupgrade 交付文件，
没有在 RE-CP-03 上运行修补后的新固件。PowerShell 流程测试使用 Linux PowerShell 7.6.6，
不冒充 Windows 5.1 或客户端网卡实测；QEMU 不模拟 MT7986 PPE/WED/无线固件。
因此本次验证证明上述源码、生命周期和流程检查通过，不能证明剩余重复接收已在实机消失。
最终性能和 WAN/LAN/Wi-Fi 功能仍需使用匹配的新固件验收。

## 历史记录：2026-09-13 实机反馈后的来源判定修复

输入和定位见 `results/2026-09-13-provenance-fix.md`。
本轮实际执行并通过：固定 Linux 6.6.133 上整套补丁零 fuzz 应用检查、28 项用户态回归测试（无跳过）、
ShellCheck 和 `git diff --check`。
28 项包括 7 项控制器、8 项二进制解析、6 项证据关联、6 项 PowerShell 流程，
以及 1 项从实际源码提取 C 辅助函数的 ASan/UBSan 测试（内部覆盖多种来源/偏移/截断/VLAN 情况）。
PowerShell 模拟运行环境为 Linux PowerShell 7.6.6，不能冒充真实 Windows 5.1 实测。

新增三项早期 RX KUnit 测试代码，但本轮没有重新构建/运行 QEMU 测试内核；
没有重启本地 OpenWrt 整机编译。下面的对象编译和 QEMU 记录属于首版，**不代表新补丁已经完成这些验证**。
新固件编译交给 GitHub Actions；最终启动、硬件路径和性能还要由新镜像实机测试确认。

## 首版诊断固件的历史验证（本轮修改之前）

### 当时已完成

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

### 首版日志与构建位置

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

### 首版记录时尚未完成的验收

本次没有编译整个 OpenWrt 世界并生成可刷写 sysupgrade 镜像，没有在 RE-CP-03 实机上启动这版新内核，没有验证真实 Windows SSH 交互或无线硬件流控。
完整固件由用户通过已提供的脚本/Actions 编译；最终导出会再次检查诊断内核配置与关键匹配构建产物。
QEMU 不模拟 MT7986 PPE/WED。上述测试降低内存生命周期、错误处理与脚本流程风险，但不能据此承诺固件绝不会出错或一次测试必定找出最终硬件原因。
