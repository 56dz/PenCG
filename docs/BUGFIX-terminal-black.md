# 明日方舟：终末地（mrfzzmd）画面全黑 / 卡死 —— 定位与修复

> 现象（用户报）：只有「明日方舟：终末地」进游戏后**画面全黑但声音正常**；
> 原神、卡拉比丘、鸣潮、崩坏星穹铁道、明日方舟都正常。
> 真机复核（2026-11-14）确认还包含第二种形态：**画面先正常、几十秒后冻结在某一帧**。

## 一、结论（一句话）

**我们的 RTP 层把终末地的一个 IDR 帧切成了 4 个残缺 AU**（详见 §五）：网关把
RTP marker 打在「切片组」边界而不是帧边界，旧代码无条件 `if (marker) flush_frame()`，
于是 15358 字节的完整 IDR 被截成 5409 字节就送进解码器，宏块只覆盖约 53%
=> RK3562 的硬件解码器（`mpp_rkvdec2`）解码越界、被内核**强制复位**；
复位丢掉全部参考帧，而终末地一局只给 1 个 IDR、网关对 NACK 又从不重传，
解码器再也回不到可解码状态 —— 于是「全黑」或「冻结」，音频链路完全不受影响。

> **阅读顺序建议**：先看 §五（最终根因与修复），§二~§四是通往它的取证过程
> （VPU 复位这个中间结论就是从那里得到的），§六 是仍未解决的遗留项。

## 二、决定性证据

### 2.1 内核日志：VPU 报错并复位

```text
mpp_rkvdec2 ff340100.rkvdec: session 32 task 199652 timeout 0 abort 0 force_dequeue 1
mpp_rkvdec2 ff340100.rkvdec: resetting...
mpp_rkvdec2 ff340100.rkvdec: reset done
mpp_rkvdec2 ff340100.rkvdec: session 32 task 199652 irq_status 0x001123 timeout 0 abort 0
```

- 采样口径：`dmesg | grep -c "reset done"`；`dmesg | grep -o "irq_status 0x[0-9a-f]*" | sort | uniq -c`
  → 同一轮里 **21 次全部是 `irq_status 0x001123`**。
- 时间定位：`uptime` 与本地时间对齐后，这 21 次 reset 全部落在 **唯一那次全黑会话**的时间窗内
  （`uptime 30991~31102`），之后连续 4 次健康局增量均为 **0**。
- 对照：黑屏会话里 reset 计数持续上涨（36 → 53 → 75），健康会话里不涨。

### 2.2 码流本身是好的

`dumpau.txt` 抓到的 `au.h264`（2546832B）用 ffmpeg 解 **零 corrupt / 零 error**，
`ffprobe` 报 `h264 / Constrained Baseline / 1280x720`。
⇒ 问题不在 RTP 组装、不在参数集注入，而在「把这些合法字节交给硬解」这一步。

### 2.3 不是我们没喂

在 `cm_rtp.c` 里加了逐帧 AU 指纹（`/userdisk/miniapp/data/auld.txt` 开关，见 §四），
冻结期间日志持续输出：

```text
#4 AULD t=... n=10312 key=0 h=37df8306...
#4 AULD t=... n=11064 key=0 h=5311705d...
```

**每帧哈希都不同** ⇒ 我们一直在推内容各异的新帧；同刻 `plane[76]` 抓帧的 md5 连续 12 次完全相同
⇒ 冻结发生在**解码输出侧**，不是收包侧、不是提交侧。

### 2.4 已排除的方向

| 假设 | 结论 |
|---|---|
| `kmssink sync=true`（bug4 的老根因） | 不成立：`fb` 在 127/130 之间交替，sink 在翻页 |
| UI 面遮挡视频面 | 不成立：`modetest -p` 实测 plane54 `zpos=0`、plane76 `zpos=2` |
| KMS 平面未绑定 | 不成立：健康局 `plane[76] crtc=video_port0`，坏局也不是「绑了却翻页」 |
| SPS 中途变化导致重配失败 | 不成立：两版 SPS 都解出 1280x720/Baseline/level31/ref=2，只差 constraint_set |
| NACK 没发出去 | 不成立：`NACK sent (30 bytes, srtcp)` 有，`srtcp protect failed` 为 0；但网关**从不重传**（`recov=0`） |
| 僵尸 MPP 会话堆积 | 不成立：`/proc/mpp_service/sessions-summary` 只有 1 个 session |
| UDP 缓冲溢出 | 不成立：`RcvbufErrors` 开机至今不变 |
| 帧率/性能 | 不成立：健康局 `pushed == shown`，RSS 稳定无泄漏 |

## 三、修复（已落地）

### 3.1 native：真实上屏计数 + 上屏看门狗（`cm_gst.c` / `cm_media.c` / `cm_rtp.c` / `cm_session.h`）

1. **`cm_box.shown`**：在 `kmssink` 的 sink pad 上挂 `GST_PAD_PROBE_TYPE_BUFFER` 探针计数。
   `pushed` 只证明「我们推给 appsrc 了」，`shown` 才是「真的有 buffer 上屏」——
   两者的差值就是「下游停止消费」的可观测判据。
2. **上屏看门狗**（`cm_media_loop`）：`pushed` 涨而 `shown` 不动持续 `VWD_STALL_MS`(2.5s)
   ⇒ 判定下游卡死并重建解码器管线（先 `cm_box_destroy` 再 `cm_box_create`，
   否则新 kmssink 抢不到 plane 76），重建后 `cm_rtp_after_rebuild()` 关闸、清半截 AU、补 PLI。
3. **重建退避**：2.5s → 5s → 10s（`s_vwd_wait_ms` 翻倍，上限 8s）。
   早期版本是 2.5s 硬间隔连打三次，8 秒就把额度烧光，等于没给网关任何反应时间。
4. **重建额度用尽** ⇒ `cm_emit` 发 `{"t":"error","msg":"video sink frozen","code":-7}`。
   只报错不拆会话 —— 用户此刻还能听见声音，拆了更糟。
5. **`cm_rtp_after_rebuild()`**：重置 RTP 重排/闸门状态并补 PLI。
   **刻意不清 SPS/PPS 缓存**：终末地的 IDR 常常不带参数集，新解码器要靠缓存注入。
6. `emit_stat` 增加 `"shown"` 字段；每 10 秒额外打一行
   `video sink pushed=N shown=M`（超差打 `(DOWNSTREAM STALLED)`），
   因为 `cm_emit` 的 stat JSON 不进日志文件，不打这行真机上看不见。

### 3.2 JS：`-7` 触发自愈（`play.vue`）

- `onMedia` 的 `t === 'error'` 分支原先只处理网关错误（`ev.data.errcode`），
  native 的 `{"t":"error","msg":"video sink frozen","code":-7}` 会被当成
  「服务端错误：video sink frozen」显示红字、且不重连。现在：
  - `code === -7` 且无 `data` 包装 ⇒ 第 1 次走 `stallHealL2`（复用网关重连），
    第 2 次起走 `stallHealL3`（重新申请机位换机器 —— 新机器的流是一定带 IDR 的新流）。
  - `code === -4`（管线重建失败）同样走 L2。
- 计数 `nativeFrozen` 只在 `stat.shown > 0`（真有 buffer 上屏）时清零：
  `出画` 事件在纯黑局也会触发，不能当判据。

## 四、诊断开关（都留在仓库，真机可用）

| 开关文件（`/userdisk/miniapp/data/`） | 作用 |
|---|---|
| `auld.txt` | 每帧打印 AU 的 FNV-1a 64 位指纹（区分「码流在变」与「码流静止」） |
| `dumpau.txt` | 从关键帧起 dump 300 帧到 `au.h264` |
| `gstdebug.txt` | 内容如 `kmssink:7` / `mppvideodec:5`，建管线时读一次，30 分钟自动失效 |
| `softdec.txt` | 强制 `avdec_h264` 软解分支 |

抓视频平面（**这是本轮最重要的新手段**）：

```sh
ffmpeg -f kmsgrab -plane_id 76 -framerate 5 -i - \
       -vf hwdownload,format=nv12 -frames:v 1 -pix_fmt rgb24 /tmp/p.png -y
```

- `plane_id 54` 是 weston UI 面（要 `format=bgr0`），`76` 是视频面；
- `-crtc_id 68` 只拿到 CRTC 自己的 fb（=UI 面），**不是合成结果**；
- 回传主机：`adb shell "base64 /tmp/p.png" | tr -d '\r\n' | base64 -d > p.png`
  （`adb pull` 在 MSYS 下会被路径转换搞坏）。

## 五、★ 最终根因（2026-11-14 19:00 定案）：AU 边界被 RTP marker 切错 ★

> 上面 §一~§四 记录的是「VPU 复位」这个**中间结论**（它是对的，但只回答
> 「画面为什么黑」，没回答「VPU 为什么复位」）。本节补上最后一环，
> 并把 §六 里「为什么第一个 IDR 只有 5409 字节」这个当期的疑问彻底解开。

### 5.1 一句话

**终末地的编码器每帧切成 8 个 H.264 slice，而网关的 RTP marker 打在「切片组」
边界（每 2~3 个 slice）而不是「一帧结束」。我们旧代码 `if (marker) flush_frame(s)`
于是把一个完整的 15358 字节 IDR 拆成 4 个残缺 AU 推给解码器，第一个 AU 只覆盖
约 53% 的宏块 → `mppvideodec` 解码越界 → VPU 复位 → 全黑。**

### 5.2 证据链（三份独立证据互相咬合）

**（1）真机日志的字节数，与离线解析逐字节吻合**

`au_fail.h264`（黑屏局 dump，560986B）按 NAL 切出来：

```text
nal0  off=0      len=26    type=7 (SPS)
nal1  off=26     len=8     type=8 (PPS)
nal2  off=34     len=707   type=6 (SEI)
nal3  off=741    len=397   type=5 first_mb=0     slice_type=7
nal4  off=1138   len=339   type=5 first_mb=480   slice_type=7
nal5  off=1477   len=547   type=5 first_mb=880   slice_type=7
nal6  off=2024   len=3385  type=5 first_mb=1360  slice_type=7
--- 5409 字节 ← 真机日志里推出去的那个 key=1 AU 就到这里
nal7  off=5409   len=26    type=7  ← SPS 又来了（网关在此处打 marker）
nal8  off=5435   len=8     type=8  ← PPS 又来了
nal9  off=5443   len=3708  type=5 first_mb=1840  slice_type=7
nal10 off=9151   len=26    type=7
nal11 off=9177   len=8     type=8
nal12 off=9185   len=2935  type=5 first_mb=2240  slice_type=7
nal13 off=12120  len=26    type=7
nal14 off=12146  len=8     type=8
nal15 off=12154  len=339   type=5 first_mb=2720  slice_type=7
nal16 off=12493  len=2865  type=5 first_mb=3120  slice_type=7
--- 15358 字节 ← 这才是完整的一帧
nal17 off=15358  len=11    type=1 first_mb=0     slice_type=5  ← 第一个 P 帧
```

- `nal0..nal6` 累计 = **5409**，而真机日志同时刻打的是
  `#1 AU push key=1 bytes=5409 segs=9` —— **两个数逐字节相同**，
  说明「我们送进解码器的第一个 AU」就是这段被截断的字节。
- 健康局对照（`dp3.log:1811`）：`#1 AU push key=1 bytes=44440 segs=39` =
  44414（**单 slice 整帧** IDR）+ 26（SPS）=> 宏块全覆盖，从不复位。
- 这也解释了 §六 那个疑问：**「黑屏局 IDR 只有 5409 字节」不是网关给了退化
  码流，而是我们自己把它切碎了**。码流本身一直是完整的。

**（2）离线重放：把残缺前缀喂给 mppvideodec，必然复位**

在设备上用 `gst-launch-1.0 filesrc ! h264parse ! mppvideodec ! fakesink`，
前后 `dmesg -c`：

| 输入 | 内容 | resets | irq_status |
|---|---|---|---|
| `au_1812.h264` | 健康局整份（单 slice） | **0** | — |
| `t1_au1.h264` | 黑屏局前 **5409** 字节 | **1** | `0x001123` |
| `t3_head.h264` | 黑屏局前 20000 字节 | **5** | `0x000017`×1 + `0x001123`×4 |
| `a1_healthy` | 健康 SPS/PPS + 健康 IDR | **0** | — |
| `au_fail.h264` | 黑屏局**整份**（合法流） | **0** | — |

- 关键：**整份合法流不触发复位，只有「按 5409 字节截断」才触发**。
- 官方立场一致：rockchip-linux/mpp issue #588 里维护者 HermanChen 对该类
  reset 的答复是「Hardware reset happened on stream error. So use ffmpeg
  check the input stream is correct or not first.」—— 即复位是输入不完整
  导致的，不是解码器 bug。

**（3）主机侧两种切帧规则的对照**

`.tmp/vpu/verify_fix.py` 对同两份码流分别按「旧规则（marker/参数集处切）」与
「新规则（`first_mb==0` 才切）」重放：

```text
终末地 au_fail.h264
  OLD  AU=6    首个 IDR = 5409B     ← 与真机日志一致，缺陷被独立复现
  NEW  AU=246  首个 IDR = 15256B    每帧恰好 8 slice
健康局 au_1812.h264
  OLD  AU=1    （300 slice 全糊成一个 AU —— 旧规则在单 slice 流上「碰巧」能用）
  NEW  AU=300  每帧 1 slice，med=7944B   ← 没有劣化
```

=> 旧规则只在「单 slice 流」上碰巧正确，一遇到多 slice 流就把帧切碎。
这就是「6 个游戏里只有终末地坏」的全部原因。

**（4）对抗性验证：4 种 marker 策略 × 3 份码流**

`.tmp/vpu/sim_state.py` / `sim_stress.py` 用 python 复刻 C 状态机，对真实 NAL
序列按四种 marker 放置策略驱动（A=每组参数集前 / B=每帧末片 / C=每个 slice /
D=marker 全丢），并与 `first_mb==0` 切出的 ground truth 比对：

```text
终末地 au_fail.h264    4 种策略全部 246 帧  ✓
健康局 au_1812.h264    4 种策略全部 300 帧  ✓
合成 synth_s8.h264     4 种策略全部 300 帧  ✓   (ffmpeg -x264-params slices=8 现造)
```

策略 C（每个 slice 都打 marker，最恶劣的情况）在修复前会切出 1981 个 AU
（修复完全失效），修复后正确切出 246 个。

### 5.3 修复

`PenCG/native/csrc/cgmedia/cm_rtp.c`：

1. **新增 `slice_parse()`** —— 解析 slice NAL（type 1/5）的
   `first_mb_in_slice` / `slice_type`。复用文件里已有的
   `unescape_rbsp()` + `br_ue()`；**必须先剥 1 字节 NAL 头再反转义**，
   否则 `first_mb` 恒为 1/2（离线分析时踩过这个坑）。
2. **新增 `note_slice()`** —— `first_mb == 0 && 已有 slice` => 返回 1
   （新图像开始）；否则累加 `au_slices`，并**续期 marker 的等待窗口**
   （防止在帧中间超时放行）。
3. **AU 边界改为 `first_mb == 0`**：`au_flush_boundary()` 在下一个
   `first_mb==0` 的 slice 到来时把上一帧放出去。
4. **marker 不再是帧边界**：`au_on_slice_done(s, marker)` 收到 marker 时
   只是「压住等确认」，超时 `AU_HOLD_MAX_MS`(150ms) 才兜底放行。
   正常路径的延迟 = 一帧（20~30fps 下 33~50ms），换来的收益是
   **绝不再把残缺 IDR 送进解码器**。
5. **FU-A 路径也要解析**：`handle_h264` 的 FU-A start 分支原来只置
   `au_has_slice=1`，从不解析 slice 头，导致多 slice 流走 FU-A 时修复完全失效。
   现在用 start 包正文前 32 字节解出 `first_mb`（切片头在 NAL 最前面，
   不必等整片拼完）。
6. **`cache_param_set()` 不再置 `au_has_sps` / `au_has_pps`**：这两个标志原本在
   「看见参数集」时就置位，导致 `flush_frame()` 的补插判据
   `need_sps = !au_has_sps && sps_ok` 永远不成立，**裸 IDR 直接被推给解码器**。
   现在由真正 `annexb_put` 进 frame 的地方（单 NAL 与 STAP-A 各一处）置位，
   缓存与「本帧已带」彻底解耦。

   > 途中试过「把帧中间的重复 SPS/PPS 丢弃、只 cache」，**已撤回**（commit
   > `57a0d2b`）。理由：整份含 6 组重复参数集的 `au_fail.h264` 用 ffmpeg 解码
   > 零错误，说明重复参数集本身不是复位原因；而判据 `au_slices > 0` 在单 slice
   > 流里无法区分「帧中重复」和「新帧开头」，误判会把新 IDR 自己的参数集丢掉。
7. **头部宏块缺失的 AU 整帧丢弃（乱序安全）**：`note_slice()` 维护
   `au_min_first_mb`（本 AU 见过的最小 `first_mb`），`flush_frame()` 里判
   `au_slices > 0 && au_min_first_mb != 0` —— 一帧完整图像必定从 `first_mb==0`
   开始覆盖宏块，见不到 0 就说明我们**从一帧的中间才开始收**（刚建连、刚从坏
   AU reset、或前面几个 slice 全丢了）。这种 AU 前半部宏块永远补不回来（那些
   seq 根本没登记进 gap 表，NACK 也要不回来），所以置 `au_broken=1` 且
   `au_broken_fixable=0`（不能被「NACK 补洞成功」那个分支清掉），整帧丢弃并补
   PLI 要一个干净 IDR。
   判据用「最小值」而不是「先到的那个 slice」，是为了对**乱序**不敏感 ——
   先到的可能是 fm=1840，`fm=0` 一到就把最小值拉回 0，不会误判。
8. **FU-A 的切片头探测只对 slice NAL 做**（type 1/5）：FU-A 承载的也可能是
   SEI 或参数集，那时硬去解 slice 头必然失败，`note_slice()` 会把
   `au_slice_unknown` 置 1 —— 于是后面**每一帧**都退回旧行为，修复被一次
   SEI 分片永久关掉。现在用 `if (typ == 1 || typ == 5)` 守住。
9. **`au_slice_unknown` 分支只在 marker 上 flush**：退回旧行为的意思是
   「和修复前一样只在 marker 处切帧」，不是「见到 FU-A 的 end 就切」。
   后者会把一段残缺 AU 推给解码器，正好制造本次要消灭的 VPU 复位。
10. **兜底**：`slice_parse()` 失败（数据太短 / 结构意外）时置
   `au_slice_unknown`，整帧退回「只看 marker」的旧行为 —— 宁可跟以前一样，
   也不能把画面卡住。

判据与观测：
- 每 10 秒心跳日志 `video sink pushed=N shown=M join=K`，
  **`join` 增长 = 修复真的在起作用**（被 marker 切碎的帧被接回）；
- `stat` JSON 增 `"join"` 字段；换局时 `join` 清零（`rtp_reset_all()`）。

### 5.3.1 验证（设备离线期间完成）

**主力证据 = `PenCG/tools/hostut/`：直接编译真实的 `cm_rtp.c` 做单测。**

python 模拟脚本（`.tmp/vpu/sim_*.py`）只是模型，模型与 C 不一致时结论毫无
价值 —— 本项目已经因此误判过两次。所以另做了一套 host harness：直接用
zig 把 `native/csrc/cgmedia/cm_rtp.c` 编成主机程序，只把 gst / 网络 / 会话
管理 stub 掉，把真实码流按 4 种 RTP marker 策略打包喂进
`cm_rtp_on_packet()`，统计**实际推给 appsrc 的 AU 数与首个 IDR 字节数**。

A/B（同一份码流、同一套 harness，只换 `.c` 文件）：

| marker 策略 | 修复前 `8c75587` | 修复后 |
|---|---|---|
| A 每组参数集前（**真机实际形态**） | 70 AU / 首 IDR **5409B** | 246 AU / 首 IDR **15358B** |
| B 每帧末（marker 位置本来就对） | 307 AU / 首 IDR **5409B** | 246 AU / 15358B |
| C 每个 slice 都打 | 1968 AU / 首 IDR 1138B | 246 AU / 15358B |
| D marker 全丢 | 68 AU / 5409B | 245 AU / 15358B |

- 修复前策略 A 切出的 **5409 字节**与真机日志 `AU push key=1 bytes=5409 segs=9`
  **逐字节吻合** ⇒ 真机网关走的就是策略 A，旧代码把一个 15358 字节的完整
  IDR 按 marker 切成 4 段，第一段只有 5409 字节。
- **策略 B 是关键对照**：它的 marker 位置本来就是对的（每帧最后一个 slice），
  旧代码却依然切出 5409B。原因是旧代码在 **FU-A 的 end 上也 flush**，而
  mtu=1200 时那个 3385 字节的大 slice 必须走 FU-A ⇒ 首帧在第 6 个 slice 之后
  就被截断了。所以这是**两个独立缺陷**：marker 位置不可信 + FU-A end 被误当
  帧边界，分别由 `68bf090` 与 `f60b57d` 修掉。
- 策略 C 最糟（首 IDR 只有 1138 字节 = 1/13 帧），说明 marker 打得越碎
  mppvideodec 越容易越界 —— 与 `irq_status 0x001123` 的 VPU 复位对得上。
- **首切片不变量（最强的一条证据）**：harness 还会检查「任何推给解码器的
  AU，只要含切片，第一个切片的 `first_mb` 就必须是 0」。这条不需要事先知道
  正确答案，所以在丢包场景下也能跑。修复前它直接指认了问题：

  | 策略 | 修复前被推出去的残帧 |
  |---|---|
  | A 参数集前 | `AU[1] len=3742 首切片 first_mb=1840` |
  | B 每帧末 | `AU[1] len=3742 首切片 first_mb=1840` |
  | C 每片 marker | `AU[1] len=373 首切片 first_mb=480` |
  | D 无 marker | `AU[1] len=3742 首切片 first_mb=1840` |

  修复后（含丢包轮）全部「首切片不变量: 全部从 fm=0 开始」。
- **丢包场景**：harness 的第 5 轮是「策略 A + 每 N 包丢 1」，模拟真机
  `reseq=3945` 那种弱网。实测：

  | 丢包率 | 结果 |
  |---|---|
  | 每 3 / 7 / 20 包丢 1 | `pushes=0` —— 一帧都不推 |
  | 每 50 / 100 包丢 1 | `pushes=56 / 62`，首 IDR 15332 |

  **丢得狠时宁可不推**（闸门在等新 IDR）是安全行为：不推残帧就不会让 VPU
  复位，代价是没有画面 —— 与用户报的「冻结」现象吻合。丢包轻时能自行恢复。
- **策略 F：压住的 AU 会不会被永久卡住（自己给自己出的最坏情况）**
  harness 把**所有** `first_mb==0` 的切片都丢掉，模拟「网关永远把帧头
  切掉」。修复前实测 `pushes=0` —— 因为 `au_marker_until` 会被每个新切片
  不断续期，只要码流还在来，到期时刻就永远推不到，AU 被永久压住。

  > 这比放行一帧残帧更糟：残帧最多让解码器复位一次，永久压住不可恢复
  > （画面永久静止，连花屏都没有）。

  修法是给「压住」加一个**不随续期改变的硬上限**（`au_hold_start_ms` +
  `AU_HOLD_HARD_MAX_MS=400`，约 12 帧@30fps），与原有的「静默 150ms」
  软兜底并存：软兜底处理正常码流停顿，硬上限保证任何情况下都会放行。
  同时新增 `cm_rtp_au_hold_released()` 计数，用来区分「兜底没生效（死锁）」
  和「兜底生效了但这帧因缺帧头被按设计丢弃」。

  验证（策略 F 边喂边 tick，每 40 个 NAL 睡 60ms，模拟真实码流从不停止）：

  | 版本 | 喂包途中的放行次数 | 结果 |
  |---|---|---|
  | 加硬上限 | 2 | PASS |
  | 去掉硬上限（变异测试） | **0** | FAIL |

  F 轮对单 slice 流不适用（每帧只有一个切片且 `first_mb` 恒为 0，没有非零
  切片可丢），harness 会打印「不适用」并跳过该断言。

- **MTU 无关性**：修复后 `mtu = 576 / 800 / 1200 / 1400 / 1500` 五种取值下，
  策略 A~D 结果**完全一致**（246 AU / 首 IDR 15358B，策略 D 245）。修复前则
  **随 MTU 越大反而越糟**：mtu=576 时首 IDR 只剩 4702B（因为每片都要 FU-A，
  每个 FU-A end 都成了帧边界），mtu=800/1200/1500 时 5409B，策略 C 在
  mtu=576 下只剩 431B（= 1/35 帧）。这说明旧代码的残帧程度取决于网络 MTU 与
  网关切片的偶然组合 —— 也解释了为什么这个 bug 复现不稳定（有时全黑、有时
  冻结、有时正常）。
- 健康局 `au_1812.h264`（302 NAL）修复前后**都是 300 AU / 首 IDR 44440B**，
  四种策略全对 ⇒ 单 slice 流没有劣化（旧版在 A/D 策略下还会误切成 197/196
  帧，新判据反而更稳）。
- 期望值写成断言（`RESULT: PASS/FAIL`），并做过反向验证：用修复前的 `.c`
  编同一套 harness，四条断言全 FAIL ⇒ 断言确实有牙。

跑法：`bash PenCG/tools/hostut/run.sh <文件.h264> [mtu]`（详见
`PenCG/tools/hostut/README.md`）。

**辅助证据（模型，仅作参照）：**

| 脚本 | 验证什么 | 结果 |
|---|---|---|
| `verify_fix.py` | 两种切帧规则对真实码流的 AU 结构 | 终末地 OLD 6 个 AU / 首个 IDR 5409B → NEW 246 个 AU；健康局 NEW 300 个 AU |
| `emit_aus.py` | 按 AU 边界切好后重新拼 h264 交给 ffmpeg | 两种规则 stderr 均为空，帧数 250 / 246 ⇒ 字节流合法，问题只在 AU 边界 |
| `sim_fua.py` | FU-A 分片路径（MTU=1000 切包） | 终末地 246、健康局 300；证明 start 包前 32 字节足以解出 `first_mb` |
| `sim_join.py` | 头部缺失判据的 5 个场景 | 完整帧保留 / 中途加入丢弃 / 单 slice 不误伤 / 多 slice 不误伤 / 乱序只丢 1 帧且不级联 |
| `sim_p1.py` | 第 8、9 条两个回归点 | SEI 走 FU-A 不污染 `unknown`；`unknown` 下无 marker 的 end 不 flush。**变异测试**：回退对应修复，两条断言都失败 |

> 两个留在这张表里的作用只是「覆盖 host harness 不方便构造的边界场景」，
> **不作为提交版本的证明**。改动 C 之后必须同步模型，否则模拟什么也证明不了。

### 5.4 与上屏看门狗（§三）的关系

两者不冲突，是两层：

| 层 | 作用 | 触发条件 |
|---|---|---|
| AU 边界修复（本节） | **不再产生**残缺 IDR，从根上避免 VPU 复位 | 始终生效 |
| 上屏看门狗（§三） | 万一 VPU 还是复位了（别的码流），能察觉并求救 | `pushed` 涨而 `shown` 不动 2.5s |

修复之后看门狗应当**永不触发**；它保留着是因为「VPU 复位」这类故障
在别的游戏/别的码流上仍可能以别的形态出现，`shown` 是最可靠的观测面。

### 5.4.1 代价：所有流都多等一帧

新判据是「只有 `first_mb == 0` 的 slice 才算新图像开始」，所以**任何**帧都要
等到下一帧的第一个 slice 到达才被推给解码器，即固定多出**一帧的显示延迟**
（40~50 fps 下约 20~25 ms，30 fps 下约 33 ms）。

这条代价对所有 5 个正常游戏同样生效，是刻意接受的：

- 不这么做就无法区分「帧中间的 marker」与「帧末的 marker」—— 单 slice 流里
  两者长得完全一样（都是「一个 slice 拼完 + marker 到」），而正是这个区分
  错误让终末地全黑。
- 20~25 ms 在云游戏场景里察觉不到（网络单程就几十毫秒），而全黑的代价是
  整个游戏不能玩。
- 兜底 `AU_HOLD_MAX_MS = 150ms` 只在码流真停时才走到，正常路径靠下一帧的
  slice 0 放行，不会累积。

若将来要消除这一帧延迟，可行的方向是让编码器侧在帧末 marker 上提供
`first_mb == 0` 之外的判据（例如 SPS 里的 `slice_group_map` 或 SEI 的
`picture timing`），但那需要协议侧配合，不在本修复范围内。

### 5.5 为什么之前没发现

三次「健康局」把注意力引到了别处：健康局 `pushed == shown`、fb 交替、
`join` 会是 0 —— 因为那几局网关**碰巧**没在帧中间打 marker。而故障局的
特征（`key=1` 只有 5409B）当时被解读成「网关给了退化码流」而不是
「我们切碎了它」。

**教训：看到「关键帧特别小」要先算宏块覆盖率（`first_mb` 序列是否从 0
连续覆盖到 slice 总数），而不是先怀疑对端。**

---

## 六、仍未解决 / 下一步

1. ~~**为什么同一款游戏有时一帧都不上屏、有时完全正常**~~ —— **已由 §五 回答**：
   5409 字节不是网关给的「退化码流」，而是我们把完整的 15358 字节 IDR 切碎后的
   第一段。修复已落地（`cm_rtp.c` 按 `first_mb_in_slice==0` 判帧边界），
   **待真机验证**（判据见 §5.3：`join` 持续增长、`reset done` 增量为 0、
   `AU push key=1` 变成 15KB 级）。
2. **VPU 复位后如何救回**：目前只能靠 JS 换会话（L2/L3）。若能拿到
   `RtcKeepPlaying`（官方 SDK `CmdId=20015`）就不必换机位，但那需要
   `paasDispatch` 的 `appKey`，见 `docs/HANDOFF-163CG-API.md`。
3. **软解兜底不可用**：`avdec_h264 ! videoflip` 分支真机报
   `gst_parse_launch warning` 后 `video box marked failed after decoder error`，
   该通道目前是坏的（缺 element 或 caps 不匹配），需要单独修。
