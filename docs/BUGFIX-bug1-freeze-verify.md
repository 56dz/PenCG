# Bug1 二期「弱网冻结」判据 — 真机验证方案

> 状态：**方案（未执行）**。本文只写"怎么做、看什么、怎么判"，由 Lead 在真机上执行。
> 写域：本文是 `PenCG/src/pages/play/play.vue`、`PenCG/src/services/media.js` 之外唯一允许新增的文件。
> 相关文档：`PenCG/docs/BUGFIX-bug1-lifecycle.md`（熄屏/生命周期）、`PenCG/docs/BUGFIX-bug1-stallguard.md`（收包停滞判据，一期）。

---

## 0. 结论速览

| # | 结论 | 证据强度 |
|---|---|---|
| 1 | **这台设备造不出人为丢包。** `tc` 是 BusyBox 版（无 `netem`），内核也没有 netem 模块；**没有 `iptables`**，没有 `tcpdump`。设备侧无法限速/丢包。 | 强（`tc qdisc show` 直接 `RTNETLINK answers: Operation not supported`；`/proc/config.gz` 不存在、`/sys/module/` 无 `sch_netem`；`which iptables` rc=1） |
| 2 | 所以场景必须**在设备外面造**：路由器/PC 热点侧加丢包，或物理衰减（隔墙/裹锡纸）+ 设备自身 `wget` 抢占带宽。 | 强（设备只读探查已做完，见 §3） |
| 3 | **判据要触发，光有"少量丢包"不够。** 必须达到"native 放行 P 帧之后画面依然解不出来"。native 的 `IDR_STARVE_RELAX_MS = 4000` 一到期就放行 P 帧，只要放行的 P 帧能解出画面，`fps` 立刻回到非 0，判据自行清零。真实触发窗口是**丢包率足够高、坏 AU 持续产生**（历史上高铁现场 5~8%）。 | 强（代码推演，见 §2） |
| 4 | **必须有正对照**：用一版把 `FREEZE_GATE_MS` 临时压到 3000 的调试构建，证明"判据能跑、日志能落盘、自愈能发起"。否则"没看到 freeze 日志"永远分不清是"场景没造出来"还是"判据是死的"。 | 强（这是本文最重要的方法论要求） |
| 5 | 判读时要同时看**三方**：JS 日志（`[pencg]`）、native 日志（`[cgmedia]`）、DRI 平面状态。只看 JS 日志会把 native 的兜底误判成"判据失效"。 | 强（两条通道都跟进程 stdout 走，**必落同一文件**；实测 `loli_s6_dev.txt` 是 1 字节空文件、真日志在 `DictPen_*.log`，现场用 `P=$(pidof miniapp); readlink /proc/$P/fd/1` 定，已真机定论） |

---

## 1. 判据回顾：精确的触发条件

代码：`PenCG/src/pages/play/play.vue:1603` `checkFreeze()`，常量在 `PenCG/src/pages/play/play.vue:246-248`。

一次触发需要**同时**满足：

| 条件 | 代码 | 含义 |
|---|---|---|
| A | `stage === 'live'`（`:1613`） | 只对"已经出过画"的会话判定。还在 `waiting`/`connecting` 时不判 |
| B | `!videoHidden`（`:1613`） | 键盘摘平面期间不算（见 §8 的坑） |
| C | 每个 stat 采样判一次（`statSeq`/`freezeLastSeq`，`:1611`） | stat 约 2s 一次 |
| D | `fps` 连续为 0，且采样数 `>= FREEZE_MIN_SAMPLES（3）`（`:1670`） | 约 6 秒内没有任何一帧解出来 |
| E | 全零期内 `(gateDrop + auDrop)` 比全零起点大 **或** `rx` 比起点大（`:1658-1659`） | 证明包还在进、是我们解不出来，不是网关停推 |
| F | `frozenMs >= FREEZE_GATE_MS（12000）`（`:1669`） | 从 fps 第一次为 0 起算满 12 秒 |

触发后：`stallTries=1`、`stallOwner='freeze'`、`requestKeyFrameBurst(3, 400)`（连发 3 个 PLI，间隔 400ms），9 秒后仍未恢复则升级 L2（复用网关重连）。

**关键点**：E 里的 `gateDrop` **不是判据的主时钟**，只是"包还在进"的一个证据；主时钟是 D+F（fps 连续为 0 的时长）。这是二期返工后的设计——一期的写法是"gateDrop 连续增长 12s"，那个写法在 native 有 IDR 饥饿兜底之后是**死代码**（详见 `PenCG/src/pages/play/play.vue:216-245` 的常量注释块）。

---

## 2. 为什么家里网络几乎造不出这个场景（必须先理解，否则会白测）

时序（正常丢包 5% 左右时）：

```
t=0     丢包开始，视频 AU 开始残缺
        → native 置 have_kf=0，进入「等首个 IDR」闸门
        → gateDrop++ 开始涨，fps 掉到 0
t=4.0s  native IDR_STARVE_RELAX_MS 到期
        （PenCG/native/csrc/cgmedia/cm_rtp.c:782-791）
        → 打印 [cgmedia] #N IDR starvation 4.0s, relaxing gate
        → have_kf=1，放行后续 P 帧
t≈4.0s+ 放行的 P 帧开始解码
        ├─ 情况甲：能解（丢包只打碎了少数 AU）
        │        → frames 涨 → fps > 0 → 判据 D 的条件断掉 → 清零，永不触发 ✅ 正确行为
        └─ 情况乙：仍然解不出（丢包率高到每个 AU 都残缺，参考帧链接不上）
                 → frames 不涨 → fps 仍为 0 → 判据继续累积 → t=12s 触发 L1
```

- **情况甲**是绝大多数"轻丢包"，包括家里 Wi-Fi 偶尔重传。**这种情况下判据不触发是正确的**，因为 native 兜底已经救了，用户看不到冻结。
- **情况乙**才是判据要抓的。它是**历史上的高铁现场**：`auDrop=401 / gateDrop=15511`，即平均每个坏 AU 赔掉 39 个 P 帧，放行 P 帧也解不出来。

所以验证目标不是"让网络抖一下"，而是**造出情况乙**：持续、足够高的丢包（经验值 ≥8%，且持续 ≥20 秒）。

> 顺带一条：`gateDrop` 在放行后停止增长，但 `auDrop` 会继续涨（坏 AU 照样计数），`rx` 在网关照推时也继续涨。这就是 E 条件写"`gateDrop+auDrop` 或 `rx` 变大"而不是"`gateDrop` 增长"的原因。

---

## 3. 造场景手段盘点（设备侧已实测，结论是"做不到"）

| 手段 | 设备是否具备 | 实测结论 |
|---|---|---|
| `tc ... netem` | `tc` 是 **BusyBox v1.34.1** multi-call binary，只有 `pfifo/bfifo/tbf/prio/cbq/red` 等 | ❌ `tc qdisc show dev wlan0` → `RTNETLINK answers: Operation not supported` / `tc: dump terminated` |
| 内核 netem 模块 | 无 | ❌ `/proc/config.gz` 不存在；`/sys/module/` 下 `grep -iE "sch|netem|qdisc"` 无输出；`find / -name "*netem*"` 无结果 |
| `iptables` / `nft` | 无 | ❌ `which iptables` → rc=1；BusyBox applet 列表只有 `ip`，没有 `iptables`/`nft` |
| `tcpdump` | 无 | ❌ `/system/bin/`、`/usr/bin/`、`/sbin/` 都没有 |
| `wpa_cli` 断开 | 有 `/sbin/wpa_cli` | ⚠️ **只能用测一期（rx 冻结），不能用来测二期**——断网后 `rx` 停止增长，E 条件必为 false，二期判据被设计成"让路"，这是正确行为 |
| `iw dev wlan0 set power_save on` | 有 `/sbin/iw`，当前 **本来就是 on** | ⚠️ 只能加剧延迟抖动，不制造丢包；效果不足以进情况乙 |
| `wget` 抢占带宽 | 有 `/bin/wget`（BusyBox） | ⭕ 可用作**辅助**：多个 `wget` 并发拉大文件，把 Wi-Fi 空口和路由器队列打满，诱发真实丢包/重传（见 §4 方案 C） |
| 物理衰减（隔墙/裹锡纸/放远） | 天然 | ⭕ 可用作**辅助**，真实且不需要工具，但不可控、不好复现 |

**结论：必须在设备外面加丢包。** 见下面三个方案，按推荐度排序。

---

## 4. 推荐方案

### 方案 A（首选）：Linux 路由器上 netem

适用：手边有能刷 OpenWrt 的路由器，或有任意 Linux 机器能做 AP/路由。

```sh
# 路由器上（OpenWrt 需要先 opkg install kmod-sched-netem）
# 1) 确认 netem 可用
tc qdisc add dev br-lan root netem loss 1% && tc qdisc show dev br-lan && tc qdisc del dev br-lan root

# 2) 正式开始：8% 丢包 + 40ms±20ms 延迟抖动（只影响下行更贴近"网关→笔"的方向，
#    netem 的 root qdisc 挂在 egress，所以笔的上行也会被丢，这没关系）
tc qdisc add dev br-lan root netem loss 8% delay 40ms 20ms distribution normal

# 3) 加重：如果 8% 不够（没触发），逐步加到 12%
tc qdisc change dev br-lan root netem loss 12% delay 40ms 20ms distribution normal

# 4) 测完必须清掉！
tc qdisc del dev br-lan root
```

**有效性先验证再进游戏**：让笔 ping 一个公网地址 20 秒，看丢包率是否真到 8%。

```bash
cd /d/Users/Daniel/code/DSH/x6pro/haas-x6pro && ./env/adb/adb.exe -s 9D91500007505171 shell \
  'ping -c 20 -i 0.5 223.5.5.5 | tail -3'
# 期望：8% loss 左右（20 包丢 1~2 个）。若 0% loss → netem 没挂到对的方向/网卡，别继续
```

### 方案 B：Windows 机器做热点 + clumsy（无 Linux 时的替代）

Lead 在 Windows 上，可用 [clumsy](https://jagt.github.io/clumsy/)（基于 WinDivert）给流量加丢包。但它默认只作用于**本机进程**的流量，对"Windows 移动热点转发给笔的流量"是否生效需要**先验证**，别假设能用。

验证步骤（进游戏前必须做）：

1. Windows 开移动热点，笔连上去，`ping` 主机网关确认通。
2. clumsy 勾 `Drop`、Throttle 设 8%、过滤条件填笔的 IP（`ip.Dst == <笔IP> or ip.Src == <笔IP>`）。
3. 笔上 `ping -c 20 223.5.5.5`，**看丢包率是否 ≈8%**。
4. 若丢包率 0%，说明 clumsy 抓不到转发流量 → 放弃方案 B，走方案 A 或 C。

### 方案 C：设备自身 `wget` 抢占 + 物理衰减（最省事，可控性最差）

不需要任何外部工具，但要接受"触发与否靠运气"。

```bash
# 起播、画面正常之后，开 3~5 个 wget 并发拉大文件把链路打满
# （URL 换成你自己的服务器上一个大文件，笔能不能上网先确认）
cd /d/Users/Daniel/code/DSH/x6pro/haas-x6pro && ./env/adb/adb.exe -s 9D91500007505171 shell '
  for i in 1 2 3 4; do
    (wget -q -O /dev/null http://<你的服务器>/big.bin &) 
  done
  pidof wget'
# 观察：wget 在跑的同时游戏 fps 应该掉/抖。跑 60 秒后全部杀掉：
cd /d/Users/Daniel/code/DSH/x6pro/haas-x6pro && ./env/adb/adb.exe -s 9D91500007505171 shell \
  'for p in $(pidof wget); do kill $p; done; pidof wget; echo done'
```

配合物理手段（笔离路由器隔一堵墙、或把笔放进金属盒/裹锡纸只留一角）可以把丢包率推上去。**注意**：不要用 `wpa_cli disconnect` 代替——见 §3，那是测一期判据的手段。

---

## 5. 执行步骤（方案 A 为例）

### 5.1 清基线

```bash
D=/d/Users/Daniel/code/DSH/x6pro/haas-x6pro
# 真正要 rotate 的是 DictPen_*.log（loli_s6_dev.txt 实测是 1 字节空文件，顺手一起移）
$D/env/adb/adb.exe -s 9D91500007505171 shell 'P=$(pidof miniapp); L=$(readlink /proc/$P/fd/1); mv "$L" "$L.bak" 2>/dev/null; sync; ls -l /userdata/applog/ | grep -E "loli|DictPen" | tail -3'
```

> **两个通道落在同一个文件里（已真机定论 2026-11-14）**：
> JS 的 `[pencg]`（`console.error`）与 native 的 `[cgmedia]`（`fprintf(stdout)`）
> 都跟着 **miniapp 进程的 stdout** 走，**必落同一文件**。
> 实测（X6 Pro / 1.2.0）：`/userdata/applog/loli_s6_dev.txt` 是 **1 字节空文件**，
> 真正的日志是 `/userdata/applog/DictPen_YYYYMMDD_HHMMSS.log`（当时 13.2MB，
> `[cgmedia]` 2236 行、`[pencg]` 上万行）。
> **所以先解析路径再 grep，不要写死 `loli_s6_dev.txt`：**
>
> ```bash
> P=$($D/env/adb/adb.exe -s 9D91500007505171 shell pidof miniapp | tr -d '\r')
> L=$($D/env/adb/adb.exe -s 9D91500007505171 shell "readlink /proc/$P/fd/1" | tr -d '\r')
> echo "log = $L"   # 实测形如 /userdata/applog/DictPen_20261114_003451.log
> ```
>
> 下面命令里的 `$L` 就是这个值。

### 5.2 起播 + 确认基线正常

进游戏，等画面出来，先跑 30 秒确认没有误报：

```bash
$D/env/adb/adb.exe -s 9D91500007505171 shell \
  "grep -a '\[play\] probe' $L | tail -3"
# 期望：stage=live v=1 ... gateDrop=0 auDrop=0 frozen=0s tries=0
# 若有 frozen=Ns 一直涨 → 判据有 bug，先停下来看 §8
```

### 5.3 加压并计时

```bash
# 路由器上
tc qdisc add dev br-lan root netem loss 8% delay 40ms 20ms distribution normal
date +%T    # 记下加压时刻，便于和日志对齐
```

加压后**保持笔在游戏中不动**，持续观察 ≥60 秒（判据需要 12s 窗口 + 9s L1 复查，实际至少要 25 秒才可能看到升级链）。

### 5.4 三方观察（每 20 秒刷一次）

```bash
D=/d/Users/Daniel/code/DSH/x6pro/haas-x6pro

# ① JS 侧：probe + freeze 日志
$D/env/adb/adb.exe -s 9D91500007505171 shell \
  "grep -a -E '\[play\] (probe|freeze|stall)' $L | tail -20"

# ② native 侧：闸门/放行/坏 AU
$D/env/adb/adb.exe -s 9D91500007505171 shell \
  "grep -a '\[cgmedia\]' $L | grep -aE 'still no IDR|IDR starvation|waiting for first IDR|packet loss seen|drop broken' | tail -20"

# ③ 平面状态：确认视频平面还在活跃（不是熄屏/被摘）
$D/env/adb/adb.exe -s 9D91500007505171 shell \
  "grep -a 'Esmart1' -A9 /sys/kernel/debug/dri/0/summary"
# 期望：Esmart1-win0: ACTIVE，buf[0] 不为 0
```

### 5.5 结束并恢复

```bash
# 路由器上
tc qdisc del dev br-lan root && tc qdisc show dev br-lan
```

---

## 6. 正对照：证明"判据真的能跑"

**这是本文的方法论核心。** 如果 §5 跑完什么都没看到，有两种完全不同的可能：

- (i) 场景没造出来（丢包不够，进了情况甲）
- (ii) 判据本身是死的（逻辑 bug / 日志没落盘 / 条件永不成立）

**必须能把 (i) 和 (ii) 分开**，否则这份验证等于没做。

### 6.1 做法：临时阈值构建（只改一个常量）

在 `PenCG/src/pages/play/play.vue:246` 把 `FREEZE_GATE_MS` 从 `12000` 临时改成 `3000`，构建装一次，然后**用很轻的丢包**（`netem loss 2%`）跑：

```bash
cd PenCG && node ../env/node_modules/aiot-vue-cli/src/cli.js -q -p
# 然后按常规流程安装（由 Lead 执行 deploy）
```

轻丢包下 native 4 秒就兜底放行了，正常情况下 fps 会恢复、判据不该触发。但只要有**任何一次** 3 秒的 fps 全零，日志里就会看到：

```
[pencg][W] [play] freeze: 连续 3s fps=0，期间 gateDrop+auDrop +N rx +M（rx=…）
            → L1 补帧
```

**看到这一行 = 判据的感知、判定、日志、自愈四条链路全部打通。** 然后：

- 把 `FREEZE_GATE_MS` 改回 `12000`，重新构建。
- 如果调试构建下一行 freeze 日志都没有 → **不要往下做场景，先修代码**。

### 6.2 负对照（不造丢包）

恢复正式构建后，正常网络下玩 10 分钟：

```bash
D=/d/Users/Daniel/code/DSH/x6pro/haas-x6pro
$D/env/adb/adb.exe -s 9D91500007505171 shell \
  "grep -a -c '\[play\] freeze' $L"
# 期望：0（或极少，且伴随真实的 fps=0）
$D/env/adb/adb.exe -s 9D91500007505171 shell \
  "grep -a '\[play\] probe' $L | grep -a -v 'frozen=0s' | tail -5"
# 期望：空。正常播放 frozen 必须恒为 0s
```

负对照失败（正常网络下出现 freeze 日志）= **误报**，比漏报更严重，必须回头收紧阈值。

---

## 7. 判读表：看到什么 = 什么结论

| 现场 | JS 日志特征 | native 日志特征 | 结论 |
|---|---|---|---|
| **情况甲（判据不该触发）** | `frozen=` 涨到 4~11s 后回到 `0s`；无 `[play] freeze:` | `IDR starvation 4.0s, relaxing gate` 之后 `pt histogram` 继续涨 | ✅ 正确。native 兜底救回来了，判据按设计不动 |
| **情况乙 + 判据生效** | `[play] freeze: 连续 12s fps=0，期间 ... → L1 补帧`；随后可能 `freeze recovered` | relax 之后仍有大量 `drop broken AU` / `still no IDR` | ✅ **验证通过** |
| **情况乙 + 判据生效但救不回** | `freeze: ... → L2 复用网关重连` → 可能 `L3` → `三级自愈全部失败，交给用户` | relax 持续在打 | ✅ 链走完了；但说明弱网下自愈能力有限（见 §9） |
| **情况乙但判据完全没反应** | `frozen=` 一直在涨（>20s）但**没有** `[play] freeze:` 行 | 有 relax + 持续 `packet loss seen` | ❌ **漏报**。先看 §6.1 正对照：若正对照也没触发 → 代码/构建问题；若正对照能触发 → 说明 `frozenMs` 或 E 条件的语义与预期不符，回来读 `PenCG/src/pages/play/play.vue:1629-1690` |
| **`frozen=` 一直是 `0s`，但画面上确实冻住了** | probe 里 `fps` 也许还在涨 | — | ⚠️ 说明不是"出画冻结"而是别的问题（平面被摘/熄屏/管线停）。查 `videoHidden`、`screen=`、DRI 平面 |
| **断网测试** | `[play] stall detected: rx=... 已 8s 未增长`（一期判据；2026-11-14 由 20s 收紧为 8s，旧包可能仍写 20s） | `packet loss seen` 后 `rx` 冻结 | ✅ 正常。**二期的 E 条件刻意让路**，这是设计而非 bug |

---

## 8. 风险与坑

| 坑 | 说明 | 规避 |
|---|---|---|
| **`wpa_cli disconnect` 测不出二期** | 断网 → `rx` 不涨 → E 条件 false → 二期判据按设计让路，日志会走一期的 `stall detected` | 测二期只能用"丢包"方式，不能用"断网"方式 |
| **键盘/输入法是假冻结** | 系统键盘弹出会 `hideVideoPlane()`（`PenCG/src/pages/play/play.vue:2092`），native 把平面缩成 1x1，还有切到"等首个 IDR"的重置。这段 `fps=0` 是我们自己造成的 | 已在 `checkFreeze` 加 `videoHidden` 守卫（`PenCG/src/pages/play/play.vue:1608-1621`）并在摘平面时清空全零段。**验证时不要开键盘**，若开了要确认没产生 freeze 日志 |
| **熄屏会污染结果** | 270s 无操作会 `hal-screen down`（见 `PenCG/docs/BUGFIX-bug1-lifecycle.md`） | 测试时每 1~2 分钟摸一下屏幕，或先确认 `screen=0(ON)` |
| **`/userdata` 实测 123M 总 / 剩 54M，`applog` 已占 24M**（单文件 13.2MB，那是开了 `gstdebug.txt` 才会这么快） | 我们**没有任何轮转机制**，长时间跑会写满 | 实验前删旧备份 `rm /userdata/applog/*.bak`；**实验后务必删掉 `/userdisk/miniapp/data/` 下的 `gstdebug.txt`、`dumpau.txt`、`softdec.txt`**，否则日志会一直按调试速率涨 |
| **不知道该 grep 哪个文件** | `[cgmedia]` 走 `fprintf(stdout)`，`[pencg]` 走 `console.error`，**两者都跟着 miniapp 进程的 stdout、必落同一文件**。实测 `loli_s6_dev.txt` 是 **1 字节空文件**，真日志在 `DictPen_*.log` | 先 `P=$(pidof miniapp); readlink /proc/$P/fd/1` 拿实际文件（§5.1 的 `$L`），再在那个文件里同时 grep 两个前缀 |
| **测完忘记删 netem** | 会持续影响用户 | §5.5 的 `tc qdisc del` |
| **早期版本可能开着 `wget`** | 忘记 kill 会一直占带宽 | §4 方案 C 结尾那条 kill 命令 |

---

## 9. 没能验证的部分（明确交代）

1. **本机（开发机）没有 Linux 路由环境**，§4 方案 A 的 netem 命令是标准 netem 用法，但**没有在本项目的设备上跑通过**；丢包率是否足以进"情况乙"完全取决于实际环境，需要 Lead 用 §4 的 ping 校验先确认丢包确实生效。
2. **`clumsy` 对 Windows 移动热点转发流量是否生效** —— 未知，方案 B 里已写成"必须先验证、验证失败就放弃"。
3. **`netem loss 8%` 是否等价于高铁现场的 5~8%** —— 不等价。高铁的丢包是**突发性的**（一次打碎整个 AU），`netem` 默认是**独立随机的**（每包独立丢），后者更难打碎整个 AU，所以需要更高的百分比或叠加 `burst`/`correlation` 参数。这一点会影响实验成功率，如果 8% 不触发，先试 12%，再试 `loss 10% 25%`（带 25% 相关性，制造突发）。
4. **判据在真实弱网下的自愈成功率** —— 未验证。L1 只补 PLI，如果丢包持续 20 秒以上，补 PLI 也拿不到完整 IDR，最终会走到 L3 甚至 fail。这是"弱网下治标"的固有局限，不是判据问题。
5. **本文一条 adb 写操作都没执行**（只做了只读探查：`ls`/`cat`/`grep`/`which`/`tc qdisc show`/`iw`/`busybox`）。§5 的所有 adb 命令都是给 Lead 执行的。
