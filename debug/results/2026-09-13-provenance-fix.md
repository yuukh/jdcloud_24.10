# 2026-09-13 实机证据与来源判定修复

## 输入和结论边界

输入为用户提供的 `results-20260913-233641`，包含四份客户端 JSON，以及后来补入的
`overlay_hnat418-debug/latest.tar.gz`。压缩包 SHA-256：

`0105f9f14dfeddbfa9a3aa0ef9d373bb1d3da557239d4e5d88220f4fe0a07520`

会话为 `run-20260913T153724Z-11883`。四轮服务端和采样器退出码均为 0。
原始报文、地址、设备标识和系统日志不随本报告提交；原始输入保持不变。

**已确认并修复的是：本机发包被残留的 HNAT 头部信息误判成入站报文，导致同一流错误地混用发送路径。**
这不是“已经证明所有重传和重复交付只有一个原因”，也不是新固件已经过实机验收。

| 轮次 | 客户端接收 Mbit/s | 服务端重传 | 保留窗口内本机来源误判 |
|---|---:|---:|---:|
| hardware-a / 5201 | 744.482 | 6350 | 41 / 454 |
| bypass / 5202 | 548.438 | 2430 | 8 / 805 |
| hardware-quiet / 5203 | 645.980 | 5242 | 未开启记录，不作推断 |
| hardware-b / 5204 | 485.789 | 6853 | 11 / 177 |

分母是与 IP_OUT 完整关联的本机 bridge_decision 数量，包含 iperf 控制连接。
A 的 41 次误判中 40 次有 payload，但其中包括控制连接的小报文。
性能数值是四轮先后进行的单次 Wi-Fi 实验，不足以给出修复后的吞吐提升比例。

## 证据链：不是仅凭 magic tag 或 skb cookie 推测

关联同时要求：源/目的地址、源/目的端口、TCP 序号、payload 长度、IP ID、skb cookie、时间顺序。
确认本机原始记录的 `skb_iif=0`，bridge 处仍为 0，且 `offload_no_fdb=0`。

hardware-a 的一个完整例子：

```
IP_OUT          t=191357091732 ns  seq=2786386662  len=14600  IP_ID=30566
br-lan TX       t=191357096044 ns  same packet
BR_DECISION     t=191357097199 ns  from_extge=0  has_ingress_info=1
rax0 software TX t=191357120608 ns seq=2786386662  len=1460   IP_ID=30566
```

该报文是 10 段 GSO，本机来源明确，但保留的 head 数据为
`78c3726c 00067890 00000000`，足以通过旧的 HNAT magic/source 检查。
它没有进入 CPU_INJECT/QDMA，而是在软件分段后直接交给无线设备。
同一数据流的其他本机报文则进入 PPE。

另一个误判的原始区间从 `seq=2786602742` 开始，IP_OUT 时间为 `191367284597 ns`。
随后客户端累计 ACK 卡在该序号，SACK 却已确认从 `2786653842` 开始的后续区间。
这直接显示错误路径选择关联了“后发先到”的缺口；不能把多个重复报告同一缺口的 ACK 当成多个独立丢包。

hardware-a 的 413 次 CPU_INJECT 对应 413 次 QDMA_MAP 和 413 次 DMA_RELEASE。
hardware-b 三者均为 166。两轮保留窗口中未发现 IP_OUT、CPU_INJECT、QDMA_MAP 的相同完整头部键重复。
这并不等于看见了空口发送次数：QDMA_MAP 只是发布前的完整描述符构造，DMA_RELEASE 只是 skb 回收。

硬件 A/B 的原始记录分别覆盖约 142.672 ms / 181.290 ms，均无环形覆盖。
bypass 的 CPU1 已覆盖 21536 条旧记录，不可据此宣称其完整测试全程没有重复提交。

## 修复方法

`debug/kernel/base.patch` 中的 `hnat_cpu.h` 新增入站来源判断：

```c
return skb->skb_iif || skb->offload_no_fdb;
```

桥本地输出的 CPU→PPE 资格判断使用这些由当前 skb 生命周期维护的字段，
不再由旧 headroom 或 `cb[44]` 的 magic 决定。
读取 HNAT 来源信息和 EXTGE 标志也先确认真实入站来源。
真实 PPE 回流即使失去 magic，仍由入站字段或 `offload_no_fdb` 阻止再次注入，避免循环。

不改写 TCP 保留的克隆 head 来“消除”证据；继续保留原有 `skb_cow()`、skb 所有权、
SG/TSO 描述符修补和合法 VLAN 处理。非测试本机流量仍默认绕过 PPE，
只有已有 root 控制器选择的 IPv4 测试流可以注入。没有修改全局 TCP 乱序容忍、关闭 TSO、
关闭 WAN HNAT、重载无线驱动或更改分区/引导代码。

## 修复观测盲点与日志回收

原版 H418_PPE_RX 放在 `mtk_poll_rx()` 的 `eth_type_trans()` 之后、GRO 之前；
此时 `network_header` 尚未被接收栈初始化。旧解析器从该偏移读数据，因此三份快照都没有 PPE_RX，
但后续 FOE_LOOKUP 和 bridge 的真实入站记录证明存在回流。

新解析器仅在早期 RX 观测点从 `skb->data` 解析，最多跨两层 inline VLAN，检查 IP 长度，
不把以太网 padding 当 payload。不调用 pull/reset、不修改网络头偏移或报文。
普通发送、GRO 后和 TCP 接收位置继续使用其已建立的网络头偏移。

Windows 脚本改为先下载路由器证据，再接收后台任务的错误流。
真实任务失败仍报告失败；下载先写 `.part`，成功才更名；错误写入 `runner-error.txt`。
增加 `Download-logs.cmd`，可补下载已经生成的日志，不要求 iperf、不重跑、不重刷。
**本次原始下载为何中断，没有 Windows 错误输出，不能断言就是该错误流问题。**

## 尚不能归因于本修复的现象

bypass 确认没有 CPU_INJECT/QDMA，但仍有 2430 次重传；关闭记录的 hardware-quiet 仍有 5242 次重传。
这排除了“所有现象都只由记录开销产生”的解释，却不能将所有剩余现象归因于同一个 HNAT 分支。

尤其不能把原始 DSACK 数量等同于确认的重复数据段数。首次/末次系统采样差值如下：

| 轮次 | TCPDSACKRecv | TCPDSACKIgnoredDubious | TCPDSACKRecvSegs |
|---|---:|---:|---:|
| hardware-a | 14498 | 13364 | 1134 |
| bypass | 10910 | 10858 | 52 |
| hardware-quiet | 13004 | 12332 | 672 |
| hardware-b | 9423 | 9283 | 140 |

这些是系统范围采样差值，不覆盖最后一次采样后的尾部，也不应直接等同于最终 socket 计数。
下一轮应比较乱序、重传和这些接受/忽略计数，不能只看 recorder 的 DSACK 触发次数。
本次保留的系统日志没有 BUG/Oops/Call trace 等匹配项；这不是对新固件稳定性的无限保证。

## 重现和验证

开发环境执行，不要求用户 Windows 安装 Python：

```sh
python3 debug/decode.py ../results-20260913-233641/overlay_hnat418-debug/latest.tar.gz --output /cache/hnat418-debug/report-20260913-233641 --full-events
python3 debug/analyze_session.py ../results-20260913-233641/overlay_hnat418-debug/latest.tar.gz --clients ../results-20260913-233641
python3 debug/validate.py check
python3 -m unittest discover -s debug/tests -p 'test_*.py' -v
shellcheck -s sh debug/package/files/hnat418-run debug/package/files/hnat418-snapshot
```

`validate.py check` 只复制受影响的文件，按固定内核执行零 fuzz 补丁验证，不启动内核或固件编译。
新增原始 C 辅助函数的 ASan/UBSan 测试，包括完整残留 magic、真实 RX、无 magic 的 PPE 回流标记、
不足 headroom、未初始化 network_header、负偏移、非线性 TCP 头、两层 VLAN、截断包、IP 长度和只读性。
另外加入来源关联、序号回绕、日志回收与失败场景的回归测试。
新加的三项 KUnit 用例需在重建的 QEMU 测试内核中执行；不能拿旧内核的既往通过结果冒充本次执行。

固件由 `debug_hnat418` 分支的 GitHub Actions 构建。实机验收继续使用四轮原有自动测试：
确认完整关联的本机报文不再因残留 metadata 判为入站，真实回流不会再次注入，
CPU_INJECT/QDMA 和早期 RX 的观测一致，再比较重传、乱序和吞吐。
在新固件实测之前，本版本是已修复确认缺陷的验证版，不是已经验收的最终性能结论。
