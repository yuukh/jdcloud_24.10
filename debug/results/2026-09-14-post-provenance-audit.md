# 19b12448 实机复核与残留问题修复

## 输入与结论边界

读取的是 `openwrt/results-20260914-022708/router-evidence.tar.gz` 内的原始记录、
四轮 before/after/timeline/server JSON，并对照同目录四份 client JSON；未修改原始证据。
归档 SHA-256：`fc58e85731c0efa0a436794484465de1f4ca0131c2bdb0af5964b79705f74bd5`。
session 为 `run-20260913T182714Z-17083`，即北京时间 2026-09-14 02:27:14。
归档 build-manifest 确认 `firmware-repo=19b12448be8fb1d049fdf88b587af7c718310376`，
固定上游 `ec9ef10efc65da1e6d1de4e2c043c0e13d08eed8`，Linux 6.6.133，RE-CP-03。

结论：上轮本机包被误判为 ingress 的问题，在本轮保留窗口中已不再出现；
但重复接收反馈和一次显著的 TCP 恢复异常仍存在，不能宣布所有问题解决。
本次已修补进一步发现的 metadata 消费点缺口，同时改善下一轮捕获。
这些代码缺口可由源码和回归测试证明，尚不能把它们直接认定为本轮全部 DSACK 的根因。

## 四轮量化结果

吞吐取客户端 `end.sum_received`，重传取服务端 `end.sum_sent.retransmits`。
DSACK 三列是路由器 before/after 的系统计数器差值，**不是该 socket 独占计数，也不是抓取窗口计数**。

| 模式 | 接收 Mbit/s | 服务端重传 | TCPDSACKRecv | TCPDSACKIgnoredDubious | TCPDSACKRecvSegs |
|---|---:|---:|---:|---:|---:|
| hardware-a / 5201 | 946.409 | 3 | 12185 | 12185 | 0 |
| bypass / 5202 | 753.142 | 0 | 6344 | 6344 | 0 |
| hardware-quiet / 5203 | 926.144 | 1287 | 16165 | 16156 | 9 |
| hardware-b / 5204 | 979.688 | 24 | 35657 | 35656 | 1 |

四轮服务端和采样器均正常退出，提供的日志中未发现 BUG/Oops/Call trace 等匹配项。
5201 的初始连接失败文件属于客户端重试，不能据此否定后来完整成功的该轮结果。
此前结果重传为 6350 / 2430 / 5242 / 6853；本轮显著改善，但顺序无线测试不是严格控制环境的因果实验。

quiet 在约第 46 秒和第 55 秒的单秒区间分别重传 245、974 段。
该轮 socket 最终 `reord_seen=37`，系统出现 6 次 SACK recovery、37 次 SACK reorder；
它不记录逐包事件，因此只能确认这次异常存在，不能重建这些时刻的底层发送路径。

## 上轮修复的实机证据

按完整四元组、序号区间、IP ID、cookie 和时间匹配 IP_OUT 与 BR_DECISION，
A / bypass / B 分别匹配到 1014 / 1360 / 1700 个本机包，误判 ingress 数全部为零。
真正的回注包仍有 ingress/extge 标识，不把这些包计入本机样本。
A 的 IP_OUT / CPU_INJECT / QDMA_MAP / DMA_RELEASE 各为 1014；B 各为 1700。
新的早期 RX 解析点也有有效记录，不再将 network_header 尚未建立误当成没有 PPE 回包。

这只证明所保留窗口内的改进，不能外推为整段 60 秒所有包均经过完整观测链路。

残留 metadata 并未因此消失。按本次 MT7986 固件使用的 legacy `hnat_desc` 布局，
A / bypass / B 的 IP_OUT 分别还有 876 / 586 / 1517 条记录带旧的有效 magic。
在完整身份和时间匹配的 IP_OUT→BR_DECISION 中，352 / 5 / 608 个本机包只有
`meta0` 的 ALG 位（第 23 位）从零变一，其他两个 metadata 字保持不变。
这与旧 IPv4 LOCAL_OUT 消费遗留 tag 并设置 ALG 的代码吻合，说明不能只修桥上的来源判断。
分析器通过显式 `--hnat-metadata-layout legacy` 复现；不为其他平台猜测 descriptor 布局。
这种观测证明了 metadata 消费缺口仍存在，但不等同于证明它导致全部重复接收反馈。

## DSACK 与发送区间逐一关联

由更新后的 `analyze_session.py` 复现，时间范围为每个 ACK 之前 500 ms 的保留数据：

| 模式 | 保留 DSACK | 对应字节范围的可见发送覆盖 |
|---|---:|---|
| A | 510 个，每个 1460 字节 | IP_OUT、br-lan、CPU_INJECT、eth0、QDMA_MAP 各一次 |
| bypass | 346 个，每个 1460 字节 | IP_OUT、br-lan、rax0 各一次；无 CPU_INJECT/QDMA |
| B | 508 个，每个 1460 字节 | IP_OUT、br-lan、CPU_INJECT、eth0、QDMA_MAP 各一次 |
| B | 1 个，40 字节 | 上述各阶段两次，与一个尾部重发相符 |

最后一项的区间为 `[1839541518,1839541558)`：首次在 20480 字节 GSO 中，
约 21.96 ms 后再次以 40 字节发送。分析能识别这次真正重复提交，而不是把所有 DSACK 统一归因为硬件。

尤其是 bypass：整个测速 socket 重传为零，但仍有明确的重复数据反馈。
不能把这解释成“Linux 重传之后收到正常 DSACK”，也不能把它只归因于 CPU→PPE 注入。
根据 RFC 2883，DSACK 是接收方对重复字节的反馈，不是独立的空口抓包证明。
Linux 6.6.133 的 `tcp_dsack_seen()` 会在 `dsack_dups > total_retrans` 等情况下拒绝将其用于恢复判断；
`TCPDSACKIgnoredDubious` 不是“收到的重复数据不存在”的证明。

当前证据把重点移向共同的无线发送/接收链路，但无法区分无线驱动、固件、接收网卡或接收端反馈问题。
不能据此宣布 WED 固件或 Windows RSC 为确定根因；本次不修改 RACK/SACK，也不全局关闭卸载掩盖现象。

## 捕获边界及修复内容

旧版 A/B 的保留跨度约 620/967 ms，均很早被“16 个 DSACK + 100 ms”冻结。
bypass 累计覆盖掉 229197 条旧记录，只有 346 个 DSACK 保留，内核累计检测到 360 个。
因此不能把没有记录到的早期包、后期重传或无线硬件动作解释成没有发生。

本次修复包括：

1. `hnat_cpu_has_valid_info()` 先检查真实 ingress / 人工 PPE 回注来源，再检查 headroom 和 magic。
   IPv4 LOCAL_OUT、HNAT post-routing、Wi-Fi 绑定入口均先做此检查；
   `hw_path.skb_hash` 不再在校验之前读取旧 metadata，表项访问前增加索引范围检查。
2. 私有 Wi-Fi `wifi_tx_tuple_add()` 原先在调用 HNAT 钩子之前就可能改写 head/tail tuple。
   对没有 ingress 来源的本机 skb 现在提前返回，连旧 reason 字段也不读取。
   原回调与修补回调的 ASan/UBSan 测试已重现前者错误写入、后者不读不写，真实 ingress 和回注路径仍执行。
   不通过直接 memset 一个可能共享的 skb head 处理这个问题。
3. DSACK 诊断解析要求 ACK 标志、有效非空有序区间和有效的嵌套块，保留 TCP 序号回绕语义。
4. A/bypass 的触发后尾窗缩短为请求 20 ms；B 使用 `capture=2`，
   根据 socket `total_retrans >= 32` 或 `reord_seen != 0` 触发，不再被同类早期 DSACK 提前冻结。
   这些是捕获阈值，不是对 TCP 算法的调整，也不保证下一轮复现 quiet 的同一事件。
5. 冻结头部由单独锁保护，第一次冻结原因和时间保持不变；手动冻结先阻止新记录并等待 RCU 写者退出，
   再取消延迟任务，避免已经冻结的快照头被迟到的工作改写。
6. 分析器按各阶段的字节区间计算最小/最大覆盖，区分 GSO、相邻分段、部分缺口和真正重叠。
   Windows 脚本自动附上只读网卡/RSC/统计快照，不改变网卡设置或增加人工测试步骤。

144 字节事件、64 字节文件头保持兼容；新 ACK 字段能力记录在 build-manifest，旧记录中的保留零值不是统计证据。
验证命令和实际完成范围见 `../VALIDATION.md`。本次修补后的实机性能和剩余重复包是否消失，仍需新镜像实测。

## 复现分析

在仓库根目录运行：

```sh
python3 debug/analyze_session.py ../results-20260914-022708/router-evidence.tar.gz \
  --clients ../results-20260914-022708 --hnat-metadata-layout legacy
python3 -m unittest discover -s debug/tests -p 'test_*.py' -v
python3 debug/validate.py check
```

本次机器可读审计输出位于 `/cache/hnat418-debug/session-20260914-022708.json`。
原始归档、测试地址及完整日志不纳入源码提交。
