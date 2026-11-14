# 主机侧单元测试（真实 cm_rtp.c）

## 为什么要有这个

`.tmp/vpu/sim_*.py` 那一套是 **python 模型**，它复刻 C 的状态机。模型一旦与 C
不一致，模拟出的结论就是假的 —— 本项目已经因此误判过两次：

1. 模型里保留了 C 已经删掉的「单 slice 快速路径」，于是模拟显示修复有效；
2. 模型里 `note_slice()` 的判定顺序与 C 不同（C 先判「新图像开始」并 `return`，
   再记录最小 `first_mb`），于是模拟显示头部缺失判据无误伤。

这个 harness 直接编译 `native/csrc/cgmedia/cm_rtp.c`（真代码），只把
gst / 网络 / 会话管理替换成 stub，所以它的行为就是真机上 RTP 层的行为。

## 跑法

```bash
bash PenCG/tools/hostut/run.sh <某个 .h264 文件> [mtu]
```

需要 `env/pyzig/ziglang/zig.exe`（或设 `ZIG_EXE`）。主机 target 是
`x86_64-windows-gnu`，通过 `shim/netinet/in.h` 补齐 `struct sockaddr_in`
（真机走 aarch64-linux 的真实头文件，不用 shim）。

## 它验证什么

把一份 Annex-B 码流按 4 种 RTP marker 策略打包喂进 `cm_rtp_on_packet()`，
统计**实际推给 appsrc 的 AU 个数与首个 IDR 的字节数**：

| 策略 | 含义 |
|---|---|
| A | 每组参数集之前打 marker（**真机网关的实际形态**） |
| B | 每帧最后一个 slice |
| C | 每个 slice 都打（最恶劣） |
| D | marker 全丢（只能靠 150ms 超时兜底） |

断言（`au_fail.h264` / `au_1812.h264` 的期望值写在 `main()` 里）：

- 终末地流：**246 个 AU、首个 IDR 15KB 级**（15358 字节，8 切片全覆盖）
- 健康流：**300 个 AU、首个 IDR 44KB 级**（44440 字节，单切片整帧）

策略 D 允许少一帧：最后一帧之后没有「下一帧的第一个 slice」来触发边界，
只能靠超时兜底，而超时要有后续包到达才会被 tick 到。真实码流永远有下一帧。

## 反向验证（确认断言有牙）

```bash
# 用修复前的 cm_rtp.c（8c75587）编同一套 harness，四条断言必须全 FAIL
git show 8c75587:native/csrc/cgmedia/cm_rtp.c > /tmp/cm_rtp_old.c
```

修复前实测：策略 A 切出 **70 个 AU、首 IDR 5409 字节** —— 与真机日志
`AU push key=1 bytes=5409 segs=9` 逐字节吻合；策略 B/C 更糟（1968 个 AU、
首 IDR 只有 1138 字节）。这正是 `irq_status 0x001123` / VPU 复位的成因。
