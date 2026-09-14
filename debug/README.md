# RE-CP-03 / MT7986 HNAT418 调试固件

这是 `debug_hnat418` 分支上的**完整固件构建方案**，包含已确认来源判定缺陷的修复，仍需新固件实机验收。
目标是定位此前抓包中的“批次迟到后触发重传”和“完整 IP 报文重复交付”。
保留 main 的设备配置、分区、Wi-Fi 驱动及 miniupnpd/firewall 修复；不修改引导程序或分区表。
核心基线固定为 `padavanonly/immortalwrt-mt798x-6.6@ec9ef10efc65da1e6d1de4e2c043c0e13d08eed8`，内核 6.6.133。

## 2026-09-13 实机反馈后的修复

`results-20260913-233641` 证实：部分本机报文因残留 HNAT metadata 被错误地当成入站报文，
同一连接因而混用 PPE 与直接无线发送。现在用 `skb_iif` / `offload_no_fdb` 判断真实入站来源，
不再用旧 headroom/cb magic 决定本机注入资格；真实回流仍禁止再次注入。
同时修复早期 PPE_RX 尚未初始化 network_header 时的只读解析，并补齐日志回收入口。
证据、边界和复现命令见 [本轮分析报告](results/2026-09-13-provenance-fix.md)。
绕过轮仍存在重传，不能据此声称所有无线重复交付已被一个补丁根治。

## 编译

将本地 `debug_hnat418` 分支推送后，在 GitHub Actions 原有的 `builder.yml` 工作流入口点击 Run workflow，
**分支选择 `debug_hnat418`**；运行内容为 **RE-CP-03 HNAT418 diagnostic firmware**。
保留现有工作流文件名，避免新文件不在 main 时没有手动运行入口；不用修改 main。
构建完成下载 `RE-CP-03-HNAT418-DEBUG` artifact。这个分支不会自动发布或部署。
本地构建也可使用：

```sh
cd /workspace/openwrt/jdcloud_24.10
bash build-debug.sh
```

编译、下载和缓存全部位于 `/cache/hnat418-firmware`；产物在其 `delivery` 子目录。
核心源码固定，feeds 使用首次准备时的版本并记录提交。再次运行会复用已准备的同一组输入；修改补丁或 LAN 参数后应设置新的 `HNAT418_CACHE=/cache/不同名称`，不混用旧树。
默认全新刷入的 LAN 地址为 `192.168.3.1`；保留配置升级沿用现有地址。
可以设置 `HNAT418_LAN_IP`（或 Actions 的 LAN_IP）改变**全新配置**默认地址。

`delivery/firmware` 是正常 OpenWrt Filogic 编译结果。仅使用适用于 **JDCloud RE-CP-03** 的 sysupgrade 镜像，不能刷其他机型文件。
`matching-kernel-toolchain.tar.gz`、`build-metadata/vmlinux` 等是开发者调试材料，**不是固件，不得刷写**。
生成镜像并不等于已在实机验证启动；请保留当前可用固件、配置备份及设备已有的恢复途径。

## 刷好后只需 Windows 和路由器

1. Windows 连接这台路由器的 Wi-Fi；停止旧的手动 iperf3 服务和测试。使用已设置 root 密码、可正常 SSH 登录的路由器。
2. 解压产物中的 `windows` 目录，将你已经使用的 `iperf3.exe` 及其 DLL 放进去（已在 PATH 中也可）。双击 **Run-tests.cmd**。
3. 输入路由器 IP（默认直接回车），首次连接核对 SSH 主机指纹并输入 root 密码。整个测试结束后下载日志可能再询问一次密码。

Windows 使用系统 OpenSSH 客户端和 PowerShell 5.1，不需要 Python、Wireshark、第二台服务器或 Linux 电脑。
若 Windows 没有 `ssh.exe`/`scp.exe`，需要先在系统可选功能中启用 OpenSSH 客户端；脚本不会下载第三方 SSH 程序或保存密码。
程序不自动接受未知 SSH 密钥、不保存密码；固件重装引起密钥变化时，应通过可信途径核对后更新 Windows 的 known_hosts，而不是关闭校验。
窗口会自动运行四轮，每轮单流反向 60 秒：

| 端口 | 模式 | 内核记录 |
|---|---|---|
| 5201 | 候选硬件路径 A | 开启，发送端恢复线索触发 |
| 5202 | 同一固件、本机流量绕过对照 | 开启，DSACK 触发 |
| 5203 | 候选硬件路径、不记录对照 | 关闭 |
| 5204 | 候选硬件路径 B | 开启，发送端恢复线索触发 |

四轮使用不同端口和新连接，不在存活连接中途切换路径。
这不是原 main 固件与候选固件的完整对照：四轮共享相同内核修补，只改变被测连接的本机 PPE 注入开关。
关键路径、ACK/SACK、系统状态和服务端 iperf 日志均由路由器记录。
Windows 除了发起测试和下载，还会尽力只读保存网卡驱动版本、RSC 状态和统计；
不修改网卡设置，不需要另外操作，不支持的命令记为不可用而不阻断测试。

结果在 Windows `results-日期时间` 目录，核心文件为 `router-evidence.tar.gz`，并附四轮客户端 JSON。
中途失败尽量自动下载 `router-partial-evidence.tar.gz`；即使 SSH 中断，原始文件保留在路由器 `/overlay/hnat418-debug/run-*`，最新成功打包文件是 `/overlay/hnat418-debug/latest.tar.gz`。
请整体提供该结果目录，不必再次单独抄写 iperf 输出。原始日志包括测试地址及最近系统日志，请勿直接公开发布。

只补下载已有日志时，双击 **Download-logs.cmd**；不需要 iperf、不运行测速、不修改路由器配置。
它下载路由器当前的 `latest.tar.gz`，不保证这个文件仍属于更早一次测试，请避免先重跑覆盖证据。
下载失败的文件保持 `.part` 后缀，错误记录在 `runner-error.txt`，不会冒充下载成功。

## 正常运行时不会自动测速

没有开机测速服务、没有常驻 iperf 监听器。静态记录开关默认关闭。
系统尚未挂载 debugfs 时，控制程序会在固定系统目录安全挂载它；已有挂载不覆盖、不卸载。
本机普通流量保持绕过 PPE；候选 CPU→PPE 路径仅用于 root 显式启动的指定 IPv4 服务端/客户端、测试 TCP 端口。全局 WAN HNAT 不关闭。
测试结束/异常退出会停止自己的子进程、取消测试选择器、删除自己的临时防火墙表，不卸载网卡/Wi-Fi 模块，不重启网络，不修改 UCI。
保留升级带来的 `cpu_to_ge_bypass=1` 加载参数被兼容接受；它在这个专用调试版中只是兼容参数，**调试连接的选择由 hnat418 控制**。
临时防火墙规则只限制本机 5201–5204 端口，测试期间仅接受当前 Windows LAN 地址，不改 fw4 的表；LAN 自定义策略若拒绝这些端口，脚本会报错而不擅自放宽策略。

## 内核记录的内容

观测点覆盖 IP 本地输出、桥 HNAT 来源判断、准备与注入、netdev 发送入口、QDMA 描述符成功构造、DMA skb 回收、PPE 回 CPU 原因、FOE 查询/绑定、TCP 收包入口的 SACK/DSACK。
记录单调时间、五元组、序号区间、IP ID/TTL、GSO size/segs、skb 匿名 cookie、HNAT 原始 metadata、队列与描述符索引等。**不记录业务 payload、不输出原始内核指针**。

每 CPU 16384 条、每条 144 字节；四核记录缓冲约 9 MiB，只在测试启动时分配。
5202 收到 16 个带有效 DSACK 的 ACK 后请求再记录 20 ms，再自动冻结。
5201 和 5204 不因这类早期 DSACK 冻结，而是在收到 ACK 时观察该 socket 的累计重传达到 32，
或者 `reord_seen` 非零，再请求记录 20 ms；这是有明确阈值的诊断触发，不保证复现另一轮的故障。
**冻结只停止记录，不改变当前硬件发送模式**。该尾窗受内核调度影响，实际时间以记录为准。
冻结原因 1 是 DSACK，2 是手动结束，3 是恢复线索。手动和自动冻结竞争时保留第一次冻结的头部信息。
新固件的 TCP_ACK 记录 `a/b/c` 分别为接收当前 ACK 之前的 `total_retrans/reord_seen/dsack_dups`；
旧固件这三个字段为保留值，不能把旧记录里的零当作真实 socket 统计。build-manifest 中注明此能力。
未触发时在该轮结束冻结。每 CPU 已覆盖的旧记录数量写入文件，解析器检验保留区间序号完整性，不将环形缓冲当成全程无损追踪。
样本、快照和 iperf 日志有时间/数量上限；至少要求 96 MiB 可用 RAM 和 128 MiB 可用 overlay，单次运行证据目录限制 64 MiB，正常结束保留最近两次原始目录和一个最新压缩包。
断电可能丢失尚在 RAM 中的记录，不能宣称电源故障后也必然取得异常现场。

## 如何用结果缩小问题

在开发环境解析，不要求用户 Windows 安装 Python：

```sh
python3 debug/decode.py router-evidence.tar.gz --output /cache/hnat418-report --full-events
```

综合服务端、客户端和记录窗口的可复核审计：

```sh
python3 debug/analyze_session.py results/router-evidence.tar.gz --clients results
```

`dsack_path_coverage` 按反向四元组、序号区间和 ACK 前 500 ms 的保留记录关联各发送阶段，
正确处理 GSO、多个相邻分段和序号回绕；不会把多个阶段或分段数当成重复发包次数。
最小覆盖为零只代表可见证据有缺口；覆盖为一也不证明无线硬件实际只发出一次。

2026-09-14 这轮的实机结论、残留 metadata 消费点修复和证据边界见
`results/2026-09-14-post-provenance-audit.md`。本次同时包含 mt_wifi tuple 写入前的来源检查，
由 `install.py` 安装为对应驱动包补丁；必须用新的构建缓存重新生成匹配固件，不能只替换分析脚本。

查看同一 TCP 序号/IP ID 在 IP_OUT、CPU_INJECT、QDMA_MAP 是否重复，迟到范围是否误判 ingress，是否出现 PPE 例外后 Wi-Fi 软件 TX，再结合 DSACK 和 socket 累计计数。
`QDMA_MAP` 表示完整描述符链已经构造、尚未发布；`DMA_RELEASE` 表示 skb 回收，**均不等于客户端成功接收**。
记录器读到 SACK 不代表 TCP 必然接受该 ACK，仍要比对 TCP/socket 计数。
skb 匿名 cookie 可能复用/碰撞，不能只按 cookie 关联，应合并五元组、序号、IP ID 和时间。

这版大幅缩小路由器可观测路径，但无法保证一次运行就指认所有问题：PPE/WED 自主转发、无线固件与 Windows NIC 之间的内部行为仍不完全可见。
若在保留窗口中确认路由器只构造/提交一份，而 DSACK 仍大量出现，下一阶段需要进一步区分硬件/固件与接收端，不能伪称已经看见空口的第二份包。
本方案不通过调大 TCP 乱序容忍、关闭全部 TSO、丢弃重复序号或串行化所有 GSO 来掩盖异常。

`067f7e4` 实机结果及本次修复见 `results/2026-09-14-067f7e4-audit.md`。
新补丁禁止 Full Cone NAT 改写已确认连接及追加 helper 扩展，端口耗尽时有界退出；
这不是清空 conntrack、关闭 Full Cone 或关闭 WAN 硬件加速。
两轮硬件记录都保留各自的恢复事件，quiet 对照仍不记录逐包事件。
内核日志保留上限改为 256 KiB，分析器区分测试前旧警告和本轮时间范围内的新警告；
仍无法恢复内核环形日志中已经覆盖的数据。已有映射不被该修补重新设置。

## 验证与后续模块迭代

```sh
python3 debug/validate.py check
python3 debug/validate.py objects --jobs 6
python3 debug/validate.py kunit --jobs 6
python3 -m unittest discover -s debug/tests -p 'test_*.py' -v
shellcheck -s sh debug/package/files/hnat418-run debug/package/files/hnat418-snapshot
```

KUnit 用真实 ARM64 QEMU 内核、KASAN、UBSAN 检查解析边界、只读性、session 生命周期、冻结、scoping 和二进制布局，不模拟 MT7986 的 PPE/WED。
完整固件构建额外导出最终内核配置、Module.symvers、System.map、vmlinux、配置后的内核树及对应工具链，方便后续针对同一 ABI 编译诊断模块。
工具链的宿主架构与编译机器一致，例如 GitHub 的 x86_64 构建工具链不能当成 ARM64 Docker 的本机可执行文件直接运行。
这不是允许任意热替换的承诺：以太网是内建驱动，skb/GRO/bridge 跨层修改仍需重编匹配内核及模块；不要强制忽略版本或强制卸载网络模块。
