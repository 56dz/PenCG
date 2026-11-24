# 游玩中「卡一下 → 闪黑 → 重连」—— 定位与修复

> 现象（用户报）：
> 1. m16811「我现在正在游玩，出现操作时经常性卡顿，但是静止无操作就不卡」
> 2. m16813「不对，现在画面没了」
> 3. m16929「我怀疑是网络问题，因为刚才卡住一秒钟，画面突然闪黑之后重连就恢复」

## 一、结论（一句话）

**不是网络问题，也不是用户操作引起的。** 真正的因果链是：

1. 网关每隔一段时间**单方面停止推送 RTP**（音视频同时停），WS 控制面照常；
2. native 上屏看门狗（VWD）判定「下游停止消费」，**自己拆掉 kmssink 管线重建**；
3. 而这一局原神的 IDR 是**周期性的、间隔精确 604 秒**，网关又不响应 PLI、
   不重传 NACK ⇒ 新解码器拿不到 IDR，参考帧全空，**只能等下一个 604 秒周期**
   ⇒ 用户看到「卡住」「闪黑」「黑很久」；
4. 最后 JS 侧的 L2 换会话（5 秒）才真的恢复 —— 所以主观上像「闪黑之后重连就恢复」。

修复的核心不是"让网关别停推"（那在服务端），而是**别再拆管线**：
先发 PLI 求一次 IDR，宽限 4 秒，仍然不出画就直接报 `-7` 交 JS 换会话
（换会话 5 秒、进度不丢），绝不自己重建解码器。

## 二、决定性证据

### 2.1 不是网络：冻结期间心跳 RTT 毫无劣化

从日志里提取网关回显的 keepAlive 心跳（`<- op=input ... 0 <13 位毫秒戳>`），
用「日志本地时间 − 回显戳」算往返延迟：

| 时段 | RTT 中位数 |
| --- | --- |
| 冻结前（07:48:30–07:49:20） | 126 ms |
| **冻结中（07:49:22–07:49:35）** | **149 ms** |
| 冻结前（07:53:00–07:53:40） | 186 ms |
| **冻结中（07:53:44–07:53:57）** | **135 ms** |

全程 min/med/max = 65 / 176 / 258 ms。链路没有任何劣化。
`RcvbufErrors=0`、CPU 2~4.5%、`cm_udp_open()` 从不设 `SO_RCVBUF`。

### 2.2 不是用户操作：整局没有一次真实触摸

113 条 `<- op=input` **严格每 10 秒一条**，全部是心跳回显
（`input cmd 忽略: 0 <ts>`）。cmd 分布 `{'0':95, '8':4, '23':6}`，
其中 `8`（tap）与 `23`（拉起包名）**全部集中在会话刚建立时**，
是重连后网关补发的初始化指令。

### 2.3 是网关停推：音视频同时停，WS 照常

冻结窗口内：

- `rx`（`stats.rtp_rx`）**瞬间停止增长**（不是缓慢衰减）；
- `fps` 同步归零；
- `pt histogram` 里 `98`(H264) 与 `111`(opus) **同时**停止增长；
- `ws=open` 全程，**无 `op=close` / `op=error` / ping 超时**；
- 丢包极少：`lost=1 auDrop=1 pDrop=1 nack=3 recov=0`。

### 2.4 PLI 无效、NACK 不重传、IDR 周期 604 秒

- 整局发出 **506 条 PLI**（约每 2 秒一条，健康期与冻结期完全一致，
  来自 `cm_kf_tick` 的 `next_pli_ms = now + 2000`），网关**零响应**。
- `recov=0` 全程 ⇒ NACK 也不重传。
- IDR 间隔精确 604 秒（`kf` 计数变化时刻）：

```text
kf=1 08:02:49   kf=2 08:13:00   kf=3 08:23:04   kf=4 08:33:08
```

`AU push key=1` 全份只有 8 条 = 每个会话起播 1 条 + 上述 3 条。

### 2.5 三个 native 缺陷（`cm_media.c`）

**bugA 重建额度永不清零。** `s_vwd_rebuilds` 只在换会话
（`s_vwd_sid != s->id`）时归零，**画面恢复的分支不清**。于是
08:12:40 用掉的 2 次额度到 08:38 还在，一次小抖动就直接撞
`VWD_MAX_REBUILDS` 把 `-7` 抛给 JS。

**bugB `VWD_STALL_MS 2500` 太激进。** 08:12:40 那次
`frozen 2560 ms (shown=17352 pushed=17447)` 的 `pushed-shown` 只有 95，
比健康水位还小，纯属 2.5 秒抖动，却触发拆管线；拆完必须等新 IDR，
而 IDR 间隔 604 秒 ⇒ 黑屏 20 秒。

**bugC `(DOWNSTREAM STALLED)` 每 10 秒误报。** 判据是
`pushed > shown + 60`，而健康会话里 `pushed-shown` 长期稳定在 **142~157**
（probe 采样时刻与 kmssink 取帧之间的正常管线延迟）。于是整局都在喊停摆，
把真冻结淹掉。

## 三、修复

### 3.1 `native/csrc/cgmedia/cm_media.c`

```c
#define VWD_CHECK_MS        500    /* 检查粒度 */
#define VWD_STALL_MS        6000   /* pushed 涨而 shown 不动的容忍时长 */
#define VWD_PLI_GRACE_MS    4000   /* 先求 IDR 的宽限期，过了才交 JS */
#define VWD_BACKLOG_SLACK   60     /* 超过正常管线延迟多少帧才算下游停摆 */
static uint64_t s_sink_backlog_avg = 0;   /* pushed-shown 正常水位（EMA） */
```

VWD 改为三级，**彻底删除 `cm_box_destroy` + `cm_box_create` 的重建分支**：

| 级 | 触发 | 动作 |
| --- | --- | --- |
| 0 | `shown` 增长 | 更新 backlog EMA，清 `pli_at` / `gave_up`，`wait_ms` 回到 6s |
| 1 | `pushed` 涨、`shown` 不动满 6s | 打 `video sink stalled ... -> request IDR (PLI), give it 4000 ms`，`cm_send_pli(s)`，`wait_ms += 4s` |
| 2 | 再满 4s 仍不动 | 打 `still frozen ... asking JS to reconnect instead of rebuilding`，`cm_emit({"t":"error","msg":"video sink frozen","code":-7})`（只报一次） |

统计行的 `(DOWNSTREAM STALLED)` 改用 EMA 水位：
`pushed > shown + s_sink_backlog_avg + VWD_BACKLOG_SLACK`。

### 3.2 `src/pages/play/play.vue`

`STALL_L1_WAIT_MS` 3000 → **2000**。依据：跨 5 份真机日志
`第 1 级自愈成功` 出现 **0 次** —— 网关不看 PLI，这两秒等的是已知不会来的回应。
保留 L1 本身（只花一次 UDP 发送）。

## 四、真机验证

- 装机校验：`strings` 查新标记串（不能看 md5/体积，loader 会重命名 .so 并追加 ~118 KB）：

```text
give it %d ms before                              1
asking JS to reconnect instead of rebuilding      1
still frozen %llu ms after PLI                    1
request IDR first                                 0   ← 旧串
rebuild decoder                                   0   ← 旧串
```

- 重启后 `grep -o 'libjsapi_cg[a-z]*_[0-9]*\.so' /proc/$(pidof miniapp)/maps` 确认加载的是新 .so。
- 用户实测游玩 2 分钟：**无闪黑、无卡顿、无换会话**（m17628「我玩了两分钟，没有问题了」）。

> 注意：本轮设备在验证后已离线（`adb` 报 device not found），
> 更长时间的浸泡（跨过一次 604 秒周期 IDR）留待下一轮。

## 五、网络波动策略：流畅优先 / 画质优先（v1.2.6）

修复上面三处缺陷后，用户仍报告「每 2~3 分钟卡死重连」（m17715）。复现确认：
网关对一个会话的 IDR 周期可以长达 **604 秒**、对 PLI 零响应、对 NACK 不重传。
在这种前提下，「拿不到 IDR」不是瞬时故障而是一种**可持续十分钟的常态**，
此时只剩两种消费同一份码流的方式，且各有代价 —— 所以做成了用户可选的两档，
而不是替用户猜。

| 策略 | IDR 饥饿 4 秒后的动作 | 代价 | 适用 |
| :-- | :-- | :-- | :-- |
| **流畅优先**（默认） | 放行残缺 P 帧（`vid_relax++`） | 画面继续动，但错块残留到下一个 IDR（马赛克） | 想不中断地玩 |
| **画质优先** | 拒绝放行（`vid_relax_dropped++`），发 `code:-8` 让 JS 换会话 | 换会话瞬间闪黑 1~2 秒，对局进度不丢 | 画面必须干净 |

### 5.1 为什么不是「再修一次」而是让用户选

两者都不可能免费：放行 = 花屏，不放行 = 冻结。这不是实现缺陷，是服务端
（IDR 稀疏 + 不响应 PLI/NACK）强加给客户端的二选一。替用户做选择只会让
一半人觉得「怎么老是花」、另一半觉得「怎么老是黑」。

### 5.2 落点

- `src/services/const.js`：`POLICIES` / `POLICY_LABEL` / `POLICY_DESC` / `normPolicy`
- `src/services/settings.js`：`DEFAULTS.policy`（默认 `smooth`）+ 读档纠正
- `src/services/media.js`：`startMedia` 的 `cfgObj.policy`、`setPolicy()` 热切换
- `src/pages/play/play.vue`：右栏第二行两个按钮（复用只剩一档画质后空出来的 `p2-qrow2`
  位置）+ `hintLine` 常显代价 + `onMedia` 的 `code:-8` 分支
- `native/csrc/cgmedia/cm_session.h`：`cm_cfg.policy`、`cm_sess.policy_gave_up`、
  `stats.vid_relax_dropped`
- `native/csrc/cgmedia/cm_cfg.c`：解析 `"policy"`
- `native/csrc/cgmedia/cm_rtp.c`：`flush_frame()` 的饥饿分支按策略分叉
- `native/csrc/cgmedia/cm_cmd.c`：`setPolicy` op（热切换，不重建会话）

### 5.3 三个实现要点（别再踩）

1. **`policy_gave_up` 是必需的**。饥饿期间每分钟约有 30 个 P 帧走到闸门，
   若每帧都发 `code:-8`，JS 侧 `stallTries` 会被瞬间吃满，直接从 L2 跳到
   L3（换机位 = 对局重开）。所以每段饥饿只发一次，`mark_keyframe()` 里复位。
2. **热切换必须清 `kf_starve_ms` 和 `relax_until_ms`**。否则从画质优先切回
   流畅优先时，饥饿计时还带着上一次策略留下的旧起点，会立刻放行一大批
   残缺帧 —— 用户会觉得「切了反而更花」。
3. **策略按钮不能用 `.p2-pbtn.sel-on` 这种双类选择器**。本运行时是 Weex 子集，
   只支持 single-classname selector，双类在编译期直接 `ERROR` 且整条样式被丢弃
   （页面看起来「点击没反应」，实际是样式没生效）。改用 `.p2-pbtn-on/off`。

### 5.4 真机验证判据

```text
start #N -> ... policy=smooth-first        ← 起播时策略已生效
#N policy -> quality-first (...)           ← 热切换生效
#N IDR starvation 4.0s under quality-first policy: keep gate closed ...   ← 画质优先真的拒收了
need fresh IDR（code -8）→ L2 复用网关重连                                 ← JS 正确响应
```

`probe` 行里的 `relax` / `relaxDrop` 互斥增长，可直接在设备上判断当前生效的是哪一档。

## 六、遗留

1. 网关为什么停推、以及为什么换个机位能长活 30 分钟（`.177` vs `.140/.141`）仍未解释，
   根因在服务端（见 `docs/HANDOFF-163CG-API.md`）。
2. 停推后的冻结画面仍会持续约 10 秒（6 + 4），这 10 秒只能等，不能缩短到误判。
3. 若网关将来开始响应 PLI，第 1 级就能直接恢复，无需换会话。
4. **画质优先档的 `code:-8` 换会话尚未在真机上观测到**（需要真的进入 IDR 饥饿）。
   逻辑已按 `native 只发一次 + JS 挡 busy/非 quality 态` 写好，但缺一次现场样本。

## 七、2026-11-17 定案：appsrc 时间轴是周期性卡死的真因

### 7.1 五段计数把断点夹到解码器内部

v1.3.5 在管线上挂了五个 BUFFER 探针，把「一帧从我们手里到屏幕上」切成四段：

```
appsrc 推入 -> parsed (h264parse 出口) -> vdecin (mppvideodec 入口)
            -> decoded (mppvideodec 出口) -> shown (kmssink 入口)
```

真机一次冻结的现场（`DictPen_20261117_122152.log`）：

```
健康  pushed=1881 parsed=1886 vdecin=1886 decoded=1881 shown=1881  queue=0
冻结  pushed=2069 parsed=2074 vdecin=2074 decoded=1943 shown=1943  queue=0
                     ↑ 继续涨            ↑ 死钉
```

131 帧进了解码器的 sink pad，一帧都没从 src pad 出来。这个组合一次性排掉四条：

| 假设 | 排除依据 |
|---|---|
| 我们组帧 / RTP 丢帧 | `parsed == vdecin`，且全份 `drop broken P` 只有 3 次 |
| appsrc 内部积压 | `queue = 0`（v1.3.3 起可读的 `current_level_buffers`）|
| h264parse 吞帧 | `parsed` 与 `vdecin` 完全相等 |
| MPP 硬件复位 | `dmesg | grep -c resetting` = **0** |
| 解码器报错 | `bus ERROR` = 0、`appsrc push flow` = 0 |
| dma-buf 泄漏耗尽输出池 | `fd` 稳定 153、`dmabuf` 稳定 65，6 次采样无增长 |

注意 `dmesg` 那条是**反例排除**的关键：v1.3.3 那次冻结确实伴随
`mpp_rkvdec2 ... irq_status 0x000017 / resetting / reset done`（2 次），
但 v1.3.4/v1.3.5 这几十次冻结**一次复位都没有**，说明另有软件层原因。

### 7.2 真因：`do-timestamp=true` 产生的时间轴丢失包后断裂

原管线源是 `appsrc is-live=true format=time do-timestamp=true`。这种配置下
appsrc 按**到达时刻**给每个 buffer 打绝对时钟戳。RTP 层一旦丢包
（`rtp gap: lost N pkt`），我们就少推几帧，下一个 buffer 的时间戳随即跳变，
解码器侧的时间线因此断裂 —— 之后它把后续帧判成「不在时间轴上」而静默弃掉，
表现为**收下 buffer 却永不出帧**，且 `flush` / READY 重置都救不回来。

这与 bug3/bug4 那次「kmssink `sync=true` 因时间轴错位整体丢帧」是同一类故障，
只是这次发生在解码器而不是 sink。

### 7.3 修复：源改成字节流，时间轴完全不参与

```diff
- appsrc name=vsrc is-live=true format=time  do-timestamp=true
+ appsrc name=vsrc is-live=true format=bytes do-timestamp=false
```

`format=bytes` 下 appsrc 只维护字节 offset、不产生时间戳，h264parse 拿不到
依赖时间的 segment，解码器也就没有「按时间轴弃帧」的依据。下游 kmssink 本来就是
`sync=false`（见 `KMS_SINK_PROPS` 上方长注释：那次定案不是「补准 PTS」而是
「不再让 SINK 依赖 PTS」），所以整条链上没有东西需要时间戳。

### 7.4 验证数据

同一台设备、同一个游戏（原神），三版对照：

| 版本 | 会话数 | 中位续航 | 最长 | `native -7` |
|---|---|---|---|---|
| v1.3.4 | 40 | 496 帧 ≈ **17 秒** | 6773 帧 | 37 次 |
| v1.3.5 | 4 | 2493 帧 ≈ **83 秒** | 5740 帧 ≈ 3 分钟 | 3 次 |
| **v1.3.6** | **1** | — | **2470 帧 ≈ 323 秒，全程零停摆** | **0 次** |

v1.3.6 单会话从 `12:39:13` 连续健康到 `12:44:36`（5 分 23 秒），
`-7` / `decoder stalled` / `rtp gap` 全为 0，计数一路对齐：

```
pushed=2470 parsed=2476 vdecin=2476 decoded=2470 shown=2464  queue=0 recov=0
```

### 7.5 保留下来的三层恢复机制

时间轴修好之后，周期性停摆的主因消失，但下面三层仍保留作兜底
（网关停推、真实弱网等场景仍会触发）：

1. **第 0 级** `decoded` 停涨 1 秒 → flush 上游重新协商；
2. **第 1.5 级** 再停 1 秒 → READY 重置整条视频管线（重建 h264parse 与解码器实例）；
3. **第 2 级** 再停 4 秒 → 发 `code:-7` 交给 JS 换会话。

⚠ 第 1.5 级有已知副作用：READY 重置瞬间 `plane 76` 上残留的旧帧会被重新绑定，
短时间出现**全屏竖条纹**，数秒后新帧覆盖即自愈（用户 2026-11-17 07:00 观察确认）。
若将来要消除，方向是重置前先用 1x1 把平面藏起来，重置后再放开。

### 7.6 遗留

- 未改用 `filesrc` + FIFO（同机型 PenBili 的做法）：`format=bytes` 已经解决，
  不必再动源模型。
- 竖条纹副作用未消除（见 7.5 ⚠）。
- 网关侧为什么会周期性停推仍未解释，根因在服务端。

---

## 八、2026-11-18 定案：`ignore-error=false` 才是周期性停摆的最后一层

### 8.1 现象与已排除项

1.3.1 ~ 1.3.7 的真机现场永远是同一个形状：

```
#N video sink pushed=2069 parsed=2074 vdecin=2074 decoded=1943 shown=1943 queue=0
```

- `parsed == vdecin`：h264parse 一个 buffer 都没丢；
- `queue == 0`：appsrc 没有积压，帧确实被下游取走了；
- `decoded` 死钉：**mppvideodec 收下了 buffer，却一帧都不吐**；
- 同时 `bus ERROR = 0`、`marked failed = 0`、`appsrc push flow = 0`：
  解码器停摆时是**完全静默**的。

已经用真机离线实验排除的：

| 假设 | 结论 |
|---|---|
| 码流坏 | 否。`au.h264`（300 帧）ffmpeg 解 0 错误 |
| 解码器 element 坏 | 否。同设备 `filesrc ! h264parse ! mppvideodec` 解满 300 帧 |
| h264parse 丢帧 | 否。`parsed == vdecin` 恒成立 |
| appsrc 积压/背压 | 否。`queue = 0` |
| MPP 硬件复位 | 否。停摆期间 `dmesg` 无 `irq_status` / `resetting` |
| flush 上游能救 | 否。1.3.3 实测 `decoded` 完全不动 |
| 源模型（appsrc 通路）坏 | 否。1.3.9 换回 appsrc 仍健康（见 8.6a） |
| READY 重置能救 | 部分。能救回解码器（`decoded` 800→2275），但需网关补发 IDR；且引出竖条纹 |

### 8.2 真因：我们把解码器的错误策略设反了

`gst-inspect-1.0 mppvideodec` 的插件默认值是：

```
ignore-error   Boolean  Default: true    —— 解码出错只 warn，继续解
max-errors     Integer  Default: -1      —— 不限次数
```

而我们从 bug3/bug4 时期起一直显式设成 **`ignore-error=false max-errors=16`**。
当时的理由是「让坏帧变成 `GST_MSG_ERROR` 被 `cm_box_drain_bus` 抓进日志，
而不是悄悄糊在屏幕上」。但真机数据把这个理由证伪了：

- 停摆时**一条 ERROR 都没有** —— `ignore-error=false` 并没有把「解码出问题」
  翻译成可见的错误；
- 反而让 MPP 内部一处出错后**整条会话静默停摆**（收下 buffer、不再出帧、
  不报错、不 flow error），必须靠我们自己的看门狗去猜；
- 而同机型、同面板的 PenBili（`PenBili/native/csrc/player.c`）用的是
  **插件默认值**，从不设这两个属性。

### 8.3 修复

```diff
- ignore-error=false max-errors=16
+ ignore-error=true max-errors=-1 discard-corrupted-frames=true
```

- `ignore-error=true`：坏帧只 warn，继续解，不停摆；
- `max-errors=-1`：恢复插件默认（不限次数）；
- `discard-corrupted-frames=true`：**新增**，让解码器把判定为损坏的帧直接丢弃，
  而不是吐出去造成花屏。这道防线替代了原来「靠上抛错误」的思路。

RTP 层的残帧拦截（`drop broken P AU` / `drop broken IDR AU`）完全保留，
它才是第一道防线；解码器不再承担「报告坏帧」的职责。

### 8.4 真机验证（2026-11-17 23:47 起，原神）

```
#1 start -> 111.32.221.19:36235  policy=smooth-first
#1 video sink pushed=1698 parsed=1696 vdecin=1696 decoded=1696 shown=1696 queue=0 recov=0
#1 video sink pushed=4081 parsed=4079 vdecin=4079 decoded=4079 shown=4079 queue=0 recov=0
```

- **单会话连续解码 4079 帧（约 159 秒）零停摆**：`decoder stalled = 0`、
  `native -7 = 0`、`start # = 1`、`bus ERROR = 0`。
- 同期网络并不好：`rtp gap = 47`、`drop broken = 20` —— **照样一次没停**。
  这直接证明「丢包 → 解码器静默停摆」这条链被切断了。
- 切换会话（#1→#2）是测试时执行 `hal-screen off` 触发 `app onHide → stopPlay`，
  不是卡死。

### 8.5 与第七章的关系

第七章的 `appsrc format=bytes do-timestamp=false` 修复同样有效且必须保留：
它解决的是**时间轴断裂**（`format=time` 下 appsrc 按到达时刻打绝对时钟戳，
丢包后时间轴跳变，h264parse 判 buffer「还没到」而整体丢弃）。
本章解决的是**解码器错误策略**。两者是不同层、不同症状：

| 章 | 症状 | 根因 | 判据 |
|---|---|---|---|
| 七 | `pushed` 涨、`parsed` 不涨 | appsrc 时间轴断裂 | `parsed == 0` 或严重滞后 |
| 八 | `parsed == vdecin` 涨、`decoded` 不涨 | 解码器错误策略 | 缺口稳定 43~58 帧 |

### 8.6a 源模型 A/B（确认修复与源无关）

`ignore-error` 修好后，两种源模型都单独做过真机长时间验证（同一台设备、原神）：

| 源模型 | 单会话连续解码 | `decoder stalled` | `native -7` | `queue` |
|---|---|---|---|---|
| `fdsrc` + 进程内 pipe | 23153 帧（约 9 分钟） | 0 | 0 | 0 |
| `appsrc`（默认） | 4157 帧 | 0 | 0 | 0 |

两者 `parsed == vdecin == decoded == shown` 全程对齐。所以在真机上
**「解码器静默停摆」与源模型无关**，`fdsrc` 改造不是必需的。

最终决定：**默认回到 `appsrc`**（更成熟、进程内只有一个 fd、不存在写管道丢帧），
`fdsrc` 保留为应急与对照开关 —— 建 `/userdisk/miniapp/data/vsrc_fdsrc.txt` 即切换。

### 8.6 保留下来的恢复机制

八章的修复不等于「不再需要看门狗」：网关停推、真弱网仍可能触发，
所以第 0 / 1.5 / 2 级恢复（flush → READY → `code:-7`）继续保留。
1.3.8 起希望它们**永远不被触发**；一旦日志里出现 `decoder stalled`，
说明又出现了新的解码器停摆原因，应当重新取证而不是依赖自愈。

### 8.7 遗留

- 竖条纹（READY 重置副作用）未消除，见 7.5 ⚠；
- `fdsrc` + 进程内 pipe 的源模型在本版一起落地，但**默认仍走 appsrc**：
  建 `/userdisk/miniapp/data/vsrc_fdsrc.txt` 才切到 fdsrc。真机 A/B 已确认
  8.3 的修复在两种源下都生效（appsrc 单会话 4157 帧零停摆、fdsrc 单会话
  23153 帧零停摆），所以没有理由把成熟度更低的 fdsrc 设为默认；
- 网关侧为什么会周期性停推仍未解释，根因在服务端。

## 九、2026-11-18 定案：`gap_drop` 语义混用是「马赛克残留」的真因

### 9.1 现象

用户在**校园网**（高丢包、低抖动）下游玩，反馈「**一直存在马赛克残留**」，
但**从不冻结、从不重连**。随身 WiFi（低丢包）下始终复现不出来。

现场日志（`DictPen_20261118_132106.log`，51438 行，7 个会话，画质优先档）：

| 计数 | 值 | 含义 |
|---|---|---|
| `rtp gap: lost` | 94 | 单包零星丢失（92 次 lost 1、2 次 lost 2） |
| `retransmit recovered` | 109 | 重传几乎都补回来了 |
| `AU resequenced` | 86 | 重排确实在干活 |
| `AU repaired by retransmissions` | 23 | ⚠ 见下 |
| `drop broken P AU` | 1 | ⚠ 几乎没有帧被判残 |
| `decoder stalled` / `native -7` | 0 / 0 | 没有冻结、没有换会话 |
| `bus ERROR` / `corrupt` | 0 / 0 | 解码器没报错 |

**决定性矛盾**：1540 条 `AU push` 日志**全部** `broken=0 missing=0`（0 例外）
—— 也就是说 AU 拼接层认为「每一帧都完整」，可用户看到的画面是花的。
而且 23 条 `AU repaired` 里有 **9 条前面根本没有 `retransmit recovered`**：
没有任何重传到达，它凭什么「被重传修好」？

### 9.2 真因

`cm_rtp.c` 里只有一个 `gap_drop(seq)`，它同时做两件事：
从 `gaps[]` 摘掉缺口、**并从 `au_miss[]` 摘掉且 `au_missing--`**。

而它被三处语义完全不同的地方调用：

| 调用点 | 语义 | 包收到了吗 |
|---|---|---|
| `gap_purge_expired()` | TTL(300ms) 过期，不等了 | ❌ 没有 |
| `cm_rtp_take_nack_gaps()` | 重发轮数用完（P 3 轮 / IDR 5 轮） | ❌ 没有 |
| `gap_take()` | 重传真的回来了 | ✅ 收到了 |

前两处也会把 `au_missing` 减到 0，于是 `flush_frame()` 的判据命中：

```c
if (g_rtp.au_broken && g_rtp.au_broken_fixable && g_rtp.au_missing == 0) {
    g_rtp.au_broken = 0;
    g_rtp.au_broken_fixable = 0;
    CM_LOGI("#%d AU repaired by retransmissions (%llu bytes)", ...);
}
```

**缺片的 AU 被当成「已被重传修好」，直接推给了解码器。**

后果比丢一帧严重得多：解码器拿到缺片的帧后，会用旧参考帧补出错误宏块，
并且**把这帧当成新参考帧继续解后面的 P 帧** —— 错误会在参考链里一路累积，
一直花到下一个 IDR 才恢复。而实测 IDR 间隔可以达到 **604 秒**。
这就是「马赛克残留」：它不会自愈，只会被下一个 IDR 清掉。

### 9.3 修复

把 `gap_drop()` 拆成两个语义明确的函数：

| 函数 | 摘 `gaps[]` | 摘 `au_miss[]` | 用在哪 |
|---|---|---|---|
| `gap_forget(seq)` | ✅ | ❌ **保留** | TTL 过期、轮数用完 |
| `gap_satisfied(seq)` | ✅ | ✅ | 只有 `gap_take()` 命中 |

关键点：`gap_forget()` 之后 `au_missing` **永不归零**，于是这一帧走到
`flush_frame()` 的「残缺 AU」分支被整帧丢弃 ——
**宁可丢一帧（33ms），也不能把缺片的帧喂给解码器（污染到下一个 IDR）。**

配套改动：

1. **迟到的重传照样收下**（`au_miss_take`）。缺口表条目可能已经因为 TTL/轮数
   被 `gap_forget` 摘掉了，但那个包确实是 `au_miss[]` 里的 —— 它晚到一点
   仍然能把这一帧救回来。所以重传接受路径改成
   `if (gap_take(seq) || au_miss_take(seq))`，并且区分计数：
   `gapForgot`（真的放弃了）vs `gapLate`（放弃了但后来还是收到了）。
2. **日志带 seq 明细**：`drop broken P/IDR AU (bytes, missing=N [seq1,seq2,...])`
   —— 复盘时可以直接把丢帧和 dump 出来的码流对上。
3. **`gap_forget` 单独打一行**：
   `gap forgotten without retransmit (seq=.., round=.., age=..ms, total=..) -> AU stays broken`
   前 20 条全打，之后每 50 条打一条。
4. **健康行加三个计数**：
   `gapForgot=%llu gapLate=%llu auRep=%llu`（10 秒一行）。
   判据：`auRep` 应该只统计**真的补上了**的帧；`gapForgot` 增长而
   `auRep` 不跟涨，说明缺片帧现在走的是「整帧丢弃」这条正确路径。

### 9.4 判据表（真机复盘用）

| 观察 | 结论 |
|---|---|
| `gapForgot` 涨、`auRep` 不涨、`drop broken P AU` 跟着涨 | ✅ 修复生效，残帧被丢掉了 |
| `gapLate` ≈ `gapForgot` | TTL 太短，可以调大 `NACK_GAP_TTL_MS`（现在 300ms）少丢帧 |
| `gapLate` 远小于 `gapForgot` | 网关重传基本指望不上，丢帧是必然代价 |
| `auRep` 仍然频繁且伴随马赛克 | 还有别的路径在推坏帧，需要继续查 |

### 9.5 调试开关

新增 `/userdisk/miniapp/data/cdebug.txt`（`cm_util.c` 的 `CM_DEBUG_SWITCH`）：
文件存在时 `cm_log_set_level()` 强制把级别提到 4（DEBUG），
这样 `CM_LOGD` 那些「多 slice 压住/放行」「stale retransmit」等诊断才落盘。
**删除文件即恢复**，不用重新打包。排查完记得删 —— `/userdata` 只有 60MB 可用。

### 9.6 未在此机型验证

- `NACK_GAP_TTL_MS` / `NACK_ROUNDS_P` 的最优值没有扫过，只有 300ms / 3 轮这一个观测点。
- 音频侧 `a[lost=188]` 明显高于视频侧 `v[lost=0]`，是否影响「马赛克」感知未验证
  （马赛克是视频现象，音频丢包最多是爆音/卡顿，属另一条线）。

---

## 十、回归测试：主机侧单测新增 G 轮（缺口过期）

第九章的修复有一个可被单测钉死的判据：**缺口被「放弃重传」之后，含该缺口的 AU 必须整帧丢弃**。
`PenCG/tools/hostut/run.sh` 现在跑第 8 轮 `G(缺口过期)`：

- 喂法 = 策略 A 的 marker + 每 `drop_every`（默认 7）个包丢 1 个 + 每 100 个 NAL `sleep 350ms`
  并调一次 `cm_rtp_take_nack_gaps()`（内部第一件事就是 `gap_purge_expired()`）。
- 断言：**推出的 AU 数必须为 0**（该流每 7 包丢 1，连 IDR 都残缺，没有任何一帧能凑全）。

判定结果：

| 版本 | `au.h264`（多切片）G 轮推出 AU 数 | 结论 |
| --- | --- | --- |
| 修复前（`gap_drop` 同时摘 `au_miss`） | **1** | 缺片 AU 蒙混过关 → FAIL |
| 修复后（`gap_forget` 只摘缺口表） | **0** | 整帧丢弃 → PASS |

> 单切片流（如 `au_mrfzzmd2.h264`）上 G 轮两版都是 0 —— 那种流每帧只有一个切片，
> 丢一个包就等于整帧丢失，本来就推不出去，区分不出修复前后的差异。
> **做这个回归必须用多切片流**（`.tmp/au.h264`、`.tmp/vpu/au_fail.h264` 等）。

其余 7 轮（A/B/C/D/A+丢包/A 紧跟重放/F 永不发 fm=0）保持不变，仍全部 PASS。

---

## 十一、2026-11-18 晚自习反馈三条：重连死循环与像素残留

用户在 1.3.11 上实测反馈三条：

1. 18:56 原神：全白加载界面 → 游玩界面，出现**全白像素残留**，画面特别亮。
2. 21:28 原神：没复现全白，但**还有大量其他类型的像素残留**。
3. 21:30 终末地：**经常性重连 + 马赛克残留**。

日志 `.tmp/white/evening.log`（112168 行，pid 1266，1.3.11）：

```
start # 31   stop # 31   native -7 0   decoder stalled 0   still frozen 0
rtp gap 350   retransmit recovered 141   AU resequenced 1773   AU repaired 91
drop broken P AU 113   drop broken IDR AU 19   gap forgotten 24
AU starts at fm 109   IDR starvation 25   relaxing gate 0
```

### 11.1 终末地「经常性重连」= 画质优先的 -8 死循环

`#6`–`#30`（21:29:44 → 21:35:03）是 **24 个连续会话，每个只活 3~9 秒**，全部死在同一条路径：

```
#N IDR starvation 4.0s under quality-first policy: keep gate closed (no artifacts), ask JS to switch session
emit #N: {"t":"error","msg":"need fresh IDR","code":-8}
[play] freeze: native 要新 IDR（画质优先，饥饿 4s） → L2 复用网关重连
```

而逐会话 `AU push key=1` 计数：**15 个会话整局 `key1push = 0`**（一个 IDR 都没收到），其余也只有 1 个且常被丢包打碎（`drop broken IDR AU 8819 bytes missing=1`）。整晚 `relaxing gate = 0`。

根因：**「换会话必定拿到新 IDR」这个前提在终末地上不成立**。原来的降级路径是「4 秒拿不到 IDR → 报 -8 让 JS 换会话」，换回来还是不给 IDR，于是无限循环。

修复（`cm_rtp.c`）：给画质优先的 `-8` 加次数上限 `POLICY_MAX_GIVEUPS = 2`。

- 第 1、2 次：照常报 `-8` 换会话（多数局换完立刻出画，这是首选路径）。
- 第 3 次起：**降级为放行 P 帧**，日志
  `quality-first gave up after 2 session switches without any IDR: degrading to smoother behavior`，
  计数 `vid_policy_degrade`。画面有马赛克但能继续玩，比每 3 秒重连强。
- 拿到真 IDR 时（`mark_keyframe()`）把额度还回去，否则一次「开局 24 连换」会把额度永久耗光。

### 11.2 「像素残留」= `discard-corrupted-frames=true`

`mppvideodec` 的属性默认值（真机 `gst-inspect-1.0` 实测）：

```
discard-corrupted-frames: Boolean. Default: false
ignore-error            : Boolean. Default: true
max-errors              : Integer. Default: -1
```

1.3.9 跟 PenBili 对齐时改了两件事：`ignore-error` 恢复默认（对，保留）与
`discard-corrupted-frames=true`（**错，本次撤掉**）。后者让 MPP 把「标记为损坏」的帧
**直接丢弃、不输出** —— 于是这些帧本该刷新的宏块永远停在旧内容上，直到下一个 IDR
才被重画，而实测 IDR 间隔可达 **604 秒**。

对照现场：

- `#4` 会话（18:51:19–18:54:20，就是用户说的「18:56 原神」）实测
  `drop broken = 0`、`AU starts at fm = 0`、`gapForgot = 20`、`auRep = 1`
  ⇒ **我们自己的组帧一帧没丢**，残留只能来自被解码器静默丢弃的损坏帧。
- 全白加载界面的宏块没人重画 ⇒ 用户看到的「全白残留、画面特别亮」。
- 反证：1.3.6~1.3.8 期间一直是 `discard=false`，从没收到过「像素残留」投诉。

改成 `false` 后损坏帧会被输出（局部花一下），但下一帧正常 P 帧会把它重画掉，
残留只持续一两帧。参考帧污染的风险由 RTP 层的残缺 AU 拦截兜底
（`drop broken P AU` / `drop broken IDR AU`）。

### 11.3 短包日志限速

`cm_media.c` 的 `plen == -2 || -5` 分支原来是无条件 `CM_LOGD`，打开 `cdebug.txt` 后
每 20 秒刷出几百行 25 字节短包，**晚自习那份 11 万行日志九成是它**。改成 1 秒最多一条
并输出「这 1 秒被抑制了多少条」。

### 11.4 判据

| 现象 | 期望 |
| --- | --- |
| 终末地重连 | 不再 24 连换；出现 `quality-first gave up after 2 session switches`，`vid_policy_degrade` 增长 |
| 像素残留 | 全白/错块最多持续一两帧，不再残留数分钟 |
| 日志噪声 | `short packet` 每分钟 ≤ 60 行 |
| 健康判据不变 | `pushed == parsed == vdecin == decoded == shown`、`queue=0`、`decoder stalled=0` |

### 11.5 未验证

- `POLICY_MAX_GIVEUPS = 2` 的取值只有一晚上的观测依据，终末地换会话能否稳定拿到 IDR 取决于
  网关实现，尚未在校园网下复测。
- 若把 `discard-corrupted-frames` 改回 `false` 后出现「参考帧被污染导致长时间花屏」，
  说明 RTP 层拦截没能完全挡住，需要再看 `au_starts_at_fm` 与 `drop broken` 的比例。

---

## 十二、2026-11-23 定案：整帧丢失是「像素残留」的最终真因

第十一章把 `discard-corrupted-frames` 改回 `false` 之后，用户仍报残留，且给出了
一条极关键的现场描述：

> 在移动角色这种小面积像素变化不容易出现残留，但是打开地图和活动这种大面积变化
> 就会大概率出现上一帧相同区域的像素残留。

「与画面变化面积相关」这一点直接排除了随机丢包 —— 丢包的影响和面积无关。于是
换了思路：不再从我们的计数里找，而是**把码流本身拿到离线环境，和真机解码结果逐一
对照**。

### 12.1 决定性实验：同一份字节，硬解 300 帧 / 软解 290 帧

用 v1.3.13 新增的滚动抓流拿到用户那一局的 600 帧（`au_a.h264` / `au_b.h264`，
都不是加载界面，因为滚动抓流只保留最近 10 秒）。

设备侧硬解：

```sh
gst-launch-1.0 -q filesrc location=au_b.h264 ! h264parse ! mppvideodec \
  ! video/x-raw,format=NV12 ! filesink location=hw_b.raw
# hw_b.raw = 414720000 B = 300 × 1382400（720p NV12 单帧）
```

本机软解：

```sh
ffmpeg -v error -i b1313.h264 -pix_fmt nv12 -f framemd5 sw_nv12.md5 -f null -
# 290 帧
```

逐帧 md5 对齐的结果：

| 项 | 值 |
| --- | --- |
| 硬解帧数 | 300 |
| 软解帧数 | 290 |
| 前 200 帧 | **逐字节完全相同** |
| 第 201 帧起 | 软解第 i 帧 == 硬解第 i+10 帧（90/90 全部匹配） |
| 软解中找不到对应硬解帧的 | **0** |

⇒ **硬解多输出了 10 帧，正好是第 201~210 帧，软解完全没有对应输出。**

### 12.2 码流级证据：`frame_num` 断档

自写解析器读 slice 头的 `frame_num`（`log2_max_frame_num=4`）：

```
pic 34   frame_num=2   prev=0    delta=2  <== GAP
pic 201  frame_num=1   prev=13   delta=4  <== 缺 3 帧
pic 203  frame_num=5   prev=2    delta=3  <== 缺 2 帧
...
pictures=300 duplicates=0 gaps=9         合计缺 10 帧
```

⇒ **这 10 帧在码流里根本不存在**，不是切片缺失，是整帧丢失。缺的帧数与硬解多出的
帧数完全对应。

### 12.3 为什么残留没有被我们的计数器发现

整帧丢失时**那个 AU 从未被组装**：它的 RTP 包一个都没到，`flush_frame()` 没有对象
可丢，于是：

- 不产生 `drop broken P AU` / `drop broken IDR AU`
- `AU push` 行的 `broken=0 missing=0` 全部正常
- `gapForgot` 也只在缺口过期时计一次，与「少了整帧」无关

**我们的完整性校验管的是「已组装 AU 的切片全不全」，管不到「两个 AU 之间少了一整帧」。**
这是设计上的盲区，不是实现 bug。

### 12.4 MPP 的行为：用旧内容补缺

把硬解多出的那 10 帧转成图逐帧看：

- frame 200 与上一帧突变 98.3%（新画面开始）
- frame 201 有 **64.6% 的 16×16 块仍是上一帧内容**，随后逐帧收敛
- 目视就是满屏大块马赛克

⇒ **MPP 在参考链断开时不报错、也不整帧丢弃，而是保留旧内容继续解后续 P 帧。**
缺失区域一直不刷新，直到下一个 IDR 才重画。ffmpeg 则直接跳过这些帧不输出，所以
之前所有离线校验都显示「码流合法、零解码错误」。

这条机制同时解释了三件历史现象：

1. 「小面积移动不残留、大面积变化残留」—— 小变化时旧内容本来就接近新内容；
2. 终末地更容易花 —— 它是 8~16 切片流，整帧丢概率远高于原神的单切片流；
3. 校园网残留多、随身 WiFi 复现不出 —— 取决于整帧丢失发生率。

### 12.5 修复：`note_frame_num()`

`cm_rtp.c` 新增整帧丢失检测：

1. `sps_parse_size()` 现在把 `log2_max_frame_num_minus4` 解出来（以前直接丢弃），
   经 `note_sps()` 存进 `g_rtp.log2_fn`；
2. `slice_parse()` 继续往后解 `pic_parameter_set_id` 与 `frame_num`
   （字段顺序：`first_mb_in_slice`(ue) / `slice_type`(ue) / `pps_id`(ue) / `frame_num`(u(n))）；
3. `note_frame_num(cm_sess *s, int key, int fn)` 在**每个新图像的起始 slice**
   （`first_mb_in_slice == 0`）上被调用，按 `2^log2` 取模算 `delta`，`delta > 1` 即缺帧：
   - `g_rtp.vid_frame_gap` / `s->stats.vid_frame_gap` 累加缺帧数
   - 打 `whole frame(s) lost: frame_num A -> B (delta=N), MPP will keep stale content until an IDR`
   - **画质优先档**顺带 `have_kf = 0`（该档语义就是「宁可冻也不要错块」）
   - 无条件 `kf_request(s, "whole frame lost")` 请求 IDR
4. IDR 会合法重置 `frame_num`，所以 `key` 帧直接跳过断档判定；
5. 换会话（`rtp_reset_all()`）清 `prev_frame_num` / `have_frame_num`，否则新会话第一个
   IDR 会被谎报成整片丢失。

为什么不给流畅优先档也清 `have_kf`：那会让闸门先冻 4 秒（`IDR_STARVE_RELAX_MS`）才放行，
比残留一两个 GOP 更难受。两档在此处的唯一差异就是这一点。

### 12.6 判据

| 现象 | 期望 |
| --- | --- |
| 健康局（随身 WiFi） | `frameGap=0` |
| 校园网 | `frameGap` 与我离线算出的断档数同量级（那一局 300 帧里 9 处、缺 10 帧） |
| 残留时长 | 不再持续到下一个自然 IDR（实测可达 604 秒），最坏一个 PLI 往返 |
| 日志 | 每次整帧丢失一条 `whole frame(s) lost` + 一条 `video gap (whole frame lost)` |

### 12.7 未验证

- 只在单切片流（原神）上验证过判据；终末地的 8~16 切片流里 `note_frame_num` 只在
  `first_mb_in_slice == 0` 的 slice 上触发，理论上每帧一次，待真机确认不重复计数。
- `kf_request` 有 `KF_REQ_MIN_MS` 限速，密集整帧丢失时部分请求会被限速吃掉，
  依赖 `cm_kf_tick` 的 2 秒兜底。是否需要为整帧丢失单独放宽限速，待看真机 PLI 密度。
