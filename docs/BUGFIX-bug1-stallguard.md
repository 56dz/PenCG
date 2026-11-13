# BUGFIX-bug1-stallguard —— 网关静默停推 RTP 时的客户端检测与三级自愈

> 排查对象：PenCG（有道词典笔 X6 Pro 网易云游戏客户端）
> 日期：2026-11-13
> 涉及文件：`PenCG/src/pages/play/play.vue`、`PenCG/src/services/media.js`（+ 本文档）
> 未触碰：`PenCG/native/**`、`PenCG/src/services/screen.js`、`PenCG/src/app.js`、`PenCG/src/services/log.js`
> 前置文档：`PenCG/docs/BUGFIX-bug1-lifecycle.md`（第一阶段的熄屏/生命周期修复）

---

## 0. 结论速览

| # | 结论 | 证据强度 |
|---|---|---|
| 1 | **这次「四分钟停画面」和第一阶段那个不是同一件事。** 起播后 4m18s~4m19s（三次复现：4m19s / 4m18s / 4m18s）画面停住，但**会话还活着**：`ws=open`、`phase=playing`、屏幕 `screen=0(ON)`、`keep #N` 每 20s 照常、我方 10s 一次心跳被网关原样回显。 | 强（三次独立复现 + 四条链同时取证的日志） |
| 2 | **根因在服务端：网关单方面停止推送 RTP，音视频同时停，且不发 error / close / 任何提示 op。** 客户端侧没有任何"被踢"的通知可接。 | 强（`rx` 冻结 + `fps` 归零 + WS op 统计无 keep/kick） |
| 3 | 最可能的服务端机制是**官方 SDK 的 `RtcKeepPlaying` / `RtcKeepaliveCfg` 我们没有实现**：`kickoutDuration` 默认 300s，从 ticket 阶段起算，减去起播前的容器冷启动约 42s ≈ **4m18s**，与实测吻合。 | 中（推断；300s − 42s 的算术吻合，但没拿到网关源码/文档实证） |
| 4 | 客户端能做的只有**检测 + 自愈**：`rx`（`stats.rtp_rx`，单调递增）冻结 > 20s 即判停滞，然后按 L1 补关键帧 → L2 复用网关重连 → L3 重新申请机位 逐级自愈，最多 3 次。 | 强（代码已落地，构建通过） |
| 5 | **这是治标不治本。** 若网关每 4 分钟必停，用户每局都会看到一次短暂中断（L2/L3 期间黑屏 1~3 秒）。真正修法是在服务端/协议层实现 keep-playing。 | 强（设计使然） |

已交付的补丁：2 个文件，**前端构建通过**（`node ../env/node_modules/aiot-vue-cli/src/cli.js -q -p` → exit 0），未部署、未 git 操作、未碰设备。

> **二期补充（2026-11-13 傍晚，见 §8）**：上线后真机撞上了 `rx` 判据的盲区 —— 高铁丢包 5~8% 时 `rx` 涨到 73867 但 `fps` 连续 12 分钟为 0，判据 1 一次都没触发。因此追加**第二条判据「弱网冻结」**（`fps` 连续为 0 + 包仍在进），与判据 1 互斥、共用 3 次自愈上限。**判据 1 的设计与阈值未变**，§3 仍然完全有效。

---

## 1. 现象与真机证据

### 1.1 用户报的现象

> 「游戏打开约四分钟后会停界面传输，应该是自动退出了。」

观感是「画面冻住 → 然后就像被退出去了」，但实际是两段不同的事：

- **前 20 秒左右**：画面已经停了（网关不推 RTP），但 UI 还在、心跳还在，用户只是看到一张静止的图。
- **之后**：旧代码没有任何停滞检测，所以不会自动恢复；用户最终自己退出，或等到别的超时。

### 1.2 三次复现的共同指纹（Lead 真机实测，本文直接引用）

起播后 **4m19s / 4m18s / 4m18s**，停流时刻的快照：

| 观测项 | 停流时的值 | 说明 |
|---|---|---|
| `rx` | 冻结在某个整数（例如 18874）后再不动 | `rx` = `s->stats.rtp_rx`，过 SSRC 校验的 RTP 包数，**单调递增**。这是唯一可靠的停滞判据 |
| `fps` | 29~30 → 0 | 解码输出也跟着停 |
| `screen` | `0(ON)` | **不是熄屏**，第一阶段那条链这次没参与 |
| `keep #N` | 每 20s 正常打印 | 保活正常，屏幕一直是亮的 |
| `ws` | `open` | WebSocket 没断 |
| `phase` | `playing` | 媒体状态机认为还在播 |
| 我方心跳 `op=input cmd="0 <ts>"` | 10s 一次，**被网关原样回显** | 网关活着、链路通，但就是不推流 |
| 网关下发 | **无** error / close / 任何提示 op | WS op 统计：2751 input / 7 error / 4 result / 4 offer；无 keep、kick、not_playing |
| native 侧 | 只剩 `PLI sent (26 bytes, srtcp)`，pt histogram 数值冻结 | 我们还在要关键帧，网关不再给 |

**结论：会话没死，是服务端单方面停推。** 客户端收不到任何"你该退了"的信号，只能自己发现「怎么不涨了」。

### 1.3 为什么是 4m18s

官方 WebRTC 云游戏 SDK 里有 `RtcKeepPlaying` / `RtcKeepaliveCfg` 这一套 keep-playing 机制，作用是让客户端主动告诉服务端「人还在玩，别把我踢了」。**我们从来没实现过它**（协议是从 WS+RTP 手搓的，见 `PenCG/docs/CGAPI.md`）。

服务端 `kickoutDuration` 默认 300s（5 分钟），计时起点是 **ticket 阶段**（拿票进游戏那一刻），不是起播那一刻。而我们的起播路径里，ticket 之后还要等容器冷启动（`applyTicket` 的轮询 + 机位分配 + 网关握手 + ICE/DTLS），实测约 **42s**。

```
300s（kickoutDuration）
- 42s（ticket → 画面出现 的冷启动开销）
≈ 258s = 4m18s   ← 与三次实测 4m18s/4m18s/4m19s 吻合
```

> 这条推断的价值：它解释了为什么**三次都非常稳定地落在同一个数字**——不是网络、不是随机丢包，是服务端一个固定计时器。但它是推断，不是实证：我们没拿到网关的 `kickoutDuration` 实际配置值，42s 也是按日志估算的。**要坐实，需要抓 ticket 时刻与停流时刻的真实时间差**（见 §5）。

---

## 2. 为什么这个 bug 之前没被发现

第一阶段（`PenCG/docs/BUGFIX-bug1-lifecycle.md`）修的是「屏幕熄了 → onHide → 拆会话」，那条链的特征是：

- 屏幕状态会变（`screen` 变成非 0）
- `keep #N` 停了或兜底日志出现
- App 级 `onHide` 会打日志

而这次的故障**四条链全都正常**：屏幕亮、保活跑、WS 开、心跳通。所以：

1. 第一阶段的守卫（`hiddenSince` / 宽限期 / 熄屏豁免）**一条都不会触发** —— 因为没有 onHide。
2. `cm_media.c` 的看门狗（`PEN_CG_WATCHDOG_MS`，已放宽到 180s）**也不会触发** —— 因为 JS 定时器还在跑，`js_poll` 心跳一直在刷新。看门狗管的是「JS 挂了」，不是「网关不推了」。
3. 唯一的症状是 `rx` / `fps` 不再增长，而这两个数之前只用来**显示**（bitrate/fps 那栏），从来没被当作**判据**。

所以补丁的本质是：**把一直在采集的 `rx` 从"展示用"升级成"判据用"**。

---

## 3. 设计

### 3.1 判据：为什么看 `rx`，不看别的

候选判据逐个排除：

| 候选 | 能否用 | 原因 |
|---|---|---|
| `rxPkts`（`stats.rx_pkts`） | ❌ **不能用** | 这是 socket 收到的 datagram 总数，含 STUN / DTLS / RTCP。网关停推 RTP 之后，ICE 保活的 **STUN 照样在涨**（真机验证过），拿它判停滞会永远判不出来 |
| `fps` | ❌ 不稳 | 它由 native 每 2s 的 stat 里带出来，是 2s 窗口的瞬时值，正常播放遇到一帧大 I 帧抖动也会掉到 0，容易误触 |
| `frames`（累计解码帧） | ⚠️ 可用但次优 | 解码器卡在某帧时 `frames` 也可能冻，但它滞后于 `rx`；`rx` 是"网络层还收不收得到包"的直接证据 |
| `bytes_rx` | ⚠️ 可用但次优 | 与 `rx` 同源，但它是字节数，遇到网关只发小包（音频）时会涨，判据不够干净 |
| **`rx`（`stats.rtp_rx`）** | ✅ **采用** | 过 SSRC 校验的 RTP 包数，**只有网关在真推流才会涨**，单调递增，不掺 STUN/DTLS/RTCP 噪音。停推即冻结，是这次故障最干净的指纹 |

### 3.2 阈值：20 秒 / 5 秒 / 3 次

| 常量 | 值 | 位置 | 理由 |
|---|---|---|---|
| `STALL_RX_MS` | **20000**（20s） | `PenCG/src/pages/play/play.vue:206` | 正常播放时 `rx` 每秒涨几十上百，任何网络抖动都不会让计数冻结 20 秒；而 20s 又远小于 4m18s 的故障点，第一次自愈能在用户还没意识到「卡了」时就发起。取 10s 太容易被一次 GC 或换档毛刺误触；取 30s+ 则用户已经盯着静止画面等太久 |
| `STALL_L1_WAIT_MS` | **5000**（5s） | `PenCG/src/pages/play/play.vue:209` | PLI 是向网关要 IDR，网关处理 + 一帧编码回来通常 <1s，留 5s 是给丢包重发的余量。等太久会让用户多冻 5 秒 |
| `STALL_MAX_TRIES` | **3** | `PenCG/src/pages/play/play.vue:214` | 这个故障每次都在 4 分钟出头出现。若网关/账号侧真有硬限制，无限重连只会把用户剩下的免费时长（和机位）在几秒内烧光，还不如老实告诉他手动点「重连」。上限 3 次对应 L1/L2/L3 三档 |

### 3.3 三级自愈

逐级、有时间间隔、越往后代价越大：

| 级 | 动作 | 代价 | 触发条件 |
|---|---|---|---|
| **L1** | `requestKeyFrameBurst(2, 500)` —— 连发 2 次 PLI，间隔 500ms | 几乎为零。不拆会话、不动 WS、不换机位，最坏情况就是多发两个 26 字节的 SRTCP | 停滞 > 20s 且本次故障还没自愈过 |
| **L2** | `onReconnect()` —— 复用旧 `gatewayUrl` 重连 | 拆当前媒体会话，**黑屏 1~3 秒**，机位仍是原来的 | L1 之后 5s `rx` 仍未涨 |
| **L3** | `onReapply()` —— 清掉网关、重新申请机位 | 更重：可能要重新排队、重新拿机位 | L2 之后又过 20s 仍未涨 |
| **放弃** | `this.fail('画面已停止（网关停止推流），自动恢复失败，请点「重连」', 0)` | 释放机位，回到 fail 态；`canReconnect()` 在 fail 态恢复可用，用户可手动点「重连」 | L3 之后又过 20s 仍未涨 |

时序图（一次完整的失败路径）：

```
t=0                                   rx 最后一次增长（网关停推）
 │
 ├── 每 500ms tick() 调 checkStall()：rx 没变，累加 silentMs
 │
t=20s   silentMs >= 20000 → stallTries 0→1
 │      logWarn('[play] stall detected: rx=18874 已 20s 未增长 → 尝试自愈（第 1 次：补发关键帧）')
 │      requestKeyFrameBurst(2, 500)
 │      setTimeout(复查, 5000) —— 注意：这里 return，5s 内不会再判
 │
t=25s   复查：rx 还是 18874 → stallTries 1→2，stallSince=now（重开 20s 窗口）
 │      vizOn=false；stage='waiting'（绕门禁，见 §3.4）
 │      onReconnect()
 │      └─ 内部：busy=true → stopPlay() → startPlay(旧 gatewayUrl) → stage='waiting'
 │
t=45s   复查：rx 还是 0/没涨 → stallTries 2→3，stallSince=now
 │      onReapply() → stopPlay() → applySeat() 重新申请机位
 │
t=65s   复查：还是没有 → logWarn('[play] stall: 三级自愈全部失败，交给用户（rx=… 已 65s 未增长）')
        stallReset() → this.fail(...)
```

**任何一级只要 `rx` 涨了，立刻 `stallTries = 0` 并打 `[play] stall recovered: ...`**，回到正常观察。

### 3.4 三个踩过的坑（写在代码注释里，防止后来者"优化"掉）

#### 坑 1：`canReconnect()` 在 `live + open` 时恒 false

`PenCG/src/pages/play/play.vue:492-498`：

```js
canReconnect: function () {
  if (this.busy) return false;
  var s = this.stage;
  if (s === 'boot' || s === 'ticket' || s === 'queued' || s === 'nologin') return false;
  if (s === 'live' && this.wsState === 'open') return false;  // ← :496，停滞时正是这个状态
  return true;
}
```

它是**给按钮用的**：能自己玩就别点重连。但停滞场景下我们恰恰是 `live + open`（WS 通、画面死），直接调 `onReconnect()` 会被这道门禁挡掉，L2 永远不生效。

处理：L2 里先把 `this.stage = 'waiting'`，再调 `onReconnect()` —— `onReconnect` 内部第一件事就是把 stage 置成 `'connecting'`，所以语义不变，只是绕过了"按钮该不该可点"这个判断。同处 `this.vizOn = false`，避免 tick 里 `vizOn && stage==='waiting' → live` 那条兜底把 stage 在重连过程中又推回 live。

#### 坑 2：`rx` 后退 ≠ 恢复

L2/L3 会销毁并重建 native 媒体会话，新的 `cgmedia` id 从 0 重新计数，所以 `rx` 会**变小**。

如果把它当成"恢复"清掉 `stallTries`，就会出现：

```
重连 → rx 从 0 开始 → 20s 后网关又停 → 重连 → ……（无限循环，机位和免费时长全烧光）
```

处理（`PenCG/src/pages/play/play.vue:1347`）：`rx` 后退只重置基准与计时刻，**刻意不清 `stallTries`**，让下一次停滞直接升级到下一级。

#### 坑 3：L1 的 5s 复查与 tick 的 20s 判据会打架

L1 发出后，若 tick 每 500ms 继续按 20s 判据走，5s 一到 `silentMs` 还是 >= 20000，就会立刻再判 L2 —— 但 L1 的复查定时器也要在 5s 触发。两者会重复升级。

处理：`if (this.stallL1Timer) return;`（`PenCG/src/pages/play/play.vue:1317`）—— L1 的定时器存在期间，tick 侧完全让位；由定时器自己去调 `checkStall(Date.now(), self.stage)` 做复查。

### 3.5 与"退后台"的关系（不要破坏第一阶段的 60s 宽限期）

`checkStall()` 开头第二道守卫：

```js
if (this.hiddenSince) return;   // 退后台期间不自愈
```

理由：退后台期间 `rx` 停涨可能只是系统把网络挂起了，此时去重连/重申请会白烧机位时长，还会和后台宽限期（`HIDE_GRACE_MS = 60000`）的拆解抢同一把机位锁。

配套的 `onShow`（`PenCG/src/pages/play/play.vue:581`）：

```js
this.stallLastRx = this.rx;   // 后台期间 rx 自然不涨，沿用旧基准会立刻误判
this.stallSince = 0;
if (this.stallL1Timer) { clearTimeout(this.stallL1Timer); this.stallL1Timer = 0; }
```

`stallTries` **刻意保留**：重连后又被网关停推时应当直接升级到下一级，而不是从 L1 重来。

`enterBackground()` / `armHideGrace()` / `hideGraceTimer` / `HIDE_GRACE_MS` / `HIDE_GRACE_DARK_MS` 一行没动。

---

## 4. 改动清单（file:line，行号为改后）

### 4.1 `PenCG/src/services/media.js`（579 → 617 行）

| 行 | 内容 |
|---|---|
| `:503` | `export function requestKeyFrame()` —— 原有，未改 |
| `:521` | **新增** `let kfBurst = [];` —— 模块级持有 PLI 连发的定时器句柄 |
| `:523-528` | **新增** `function clearKfBurst()` —— 逐个 `clearTimeout` 后清空数组 |
| `:530-544` | **新增** `export function requestKeyFrameBurst(count, gapMs)` —— 先 `clearKfBurst()` + 立即发一发，再按 `gap * i` 排后续；回调里先把自己从 `kfBurst` 摘掉（否则长会话里数组只增不减）再 `requestKeyFrame()`。参数钳制：`n = Math.min(5, Math.max(1, (count\|0) \|\| 1))`、`gap = Math.max(0, gapMs\|0)` |
| `:595` | **新增** `clearKfBurst();` —— `stopPlay()` 的第一行 |

为什么 `clearKfBurst()` 必须挂在 `stopPlay()` 上（`:591-595` 的注释）：

> PLI 连发定时器会在几百毫秒后对 `S.mediaId` 发 `cmd`，而 `stopPlay()` 下面几行立刻就把 `mediaId` 归零、native 会话销毁，留着只会在日志里多几条 "cmd 失败" 噪音，还会和重连抢同一个 id。

这条约束在 STALLGUARD 里尤其重要：L2/L3 内部第一件事就是 `stopPlay()`，如果 burst 不清，残留的 PLI 会打到新会话建起来之后的旧 id 上。

### 4.2 `PenCG/src/pages/play/play.vue`（2270 → 2498 行）

| 行 | 内容 |
|---|---|
| `:140` | import 增加 `requestKeyFrameBurst`（同一行里，其余导出不变） |
| `:180-214` | **新增** 文件头 STALLGUARD 说明块：真机证据、为什么看 `rx` 不看 `rxPkts`/`fps`、三个阈值的理由 |
| `:206` | **新增** `var STALL_RX_MS = 20000;` |
| `:209` | **新增** `var STALL_L1_WAIT_MS = 5000;` |
| `:214` | **新增** `var STALL_MAX_TRIES = 3;` |
| `:289-293` | **新增** data 字段 5 个：`stallLastRx` / `stallSince` / `stallTries` / `stallAt` / `stallL1Timer`（每个都带一行注释说明语义） |
| `:581-586` | `onShow` 内**新增**基准重置三行（见 §3.5） |
| `:1266-1273` | `tick()` 内**新增** `try { this.checkStall(now, s); } catch (e) { logWarn('[play] checkStall 异常'); }`。位置在心跳与 20s probe 之后 |
| `:1284-1290` | 20s probe 日志**增加两个字段**：`silent=`（rx 已静默多少秒）、`tries=`（已用自愈次数） |
| `:1299-1308` | **新增** `stallReset()` —— 清 `stallL1Timer` + 四个计时/计数字段归零 |
| `:1310-1450` | **新增** `checkStall(now, s)` —— 主体（结构见 §3.3/§3.4） |
| `:1369` | `if (silentMs < STALL_RX_MS) return;` |
| `:1374-1391` | L1 分支 |
| `:1393-1418` | L2 分支（`:1400` 重开 20s 窗口；`:1406` `vizOn=false`；`:1410` `stage='waiting'`） |
| `:1420-1436` | L3 分支（`:1422` 用 `STALL_MAX_TRIES - 1` 表达"最后一级"，改上限时这里不用动） |
| `:1438-1449` | 三级用完：`logWarn('[play] stall: 三级自愈全部失败，交给用户…')` → `stallReset()` → `this.fail(...)` |

### 4.3 生命周期清理（7 处 `stallReset()` 调用）

| 行 | 场景 | 为什么清 |
|---|---|---|
| `:717` | `applySeat()` 进入 `queued` 分支 | 换机位了，旧的停滞计数无意义 |
| `:1093` | `finish()` 开头 | 局结束了 |
| `:1115` | `fail()` 开头 | 已经失败，不再自愈（避免和 fail 抢） |
| `:1160` | `doExit()` 开头 | 用户主动退出，`busy=true` 之前清 |
| `:1444` | `checkStall()` 三级用完 | 交给用户之前清干净 |
| `:1557` | `bgTeardown()`（`clearHideGrace(); hiddenSince=0;` 之后） | 拆管线了 |
| `:1584` | `cleanup()`（`dead=true` 之后） | 页面销毁，L1 定时器必须清 |

> `beforeDestroy`（`PenCG/src/pages/play/play.vue:533`）/ `onUnload`（`:538`）都走 `cleanup()`，所以"`beforeDestroy` 清掉所有新增 timer"这一条由 `:1584` 覆盖。

### 4.4 已核对未破坏的门禁项

- `onTS` 里 `var ts = (e && e.changedTouches) || [];` 三处均在：`PenCG/src/pages/play/play.vue:1865`、`:1873`、`:1890`。
- `PenCG/native/**`、`PenCG/src/services/screen.js`、`PenCG/src/app.js`、`PenCG/src/services/log.js` 本阶段零改动。
- 用户"3 秒内有过操作就跳过自愈"这条**没有实现**，也没有跳过逻辑 —— 需求要求"不要跳过"，所以自愈不检查任何输入时间戳，纯粹按 `rx` 判定。这比"检查了但写错"更安全。

---

## 5. 真机验证

### 5.1 前置

设备序列号（Lead 实测那台）：`9D91500007505171`。日志文件：`/userdata/applog/loli_s6_dev.txt`（`log.js` 只把 `console.error` 落盘，形如 `[2026-11-13 06:53:35.123][E][JS] [pencg][W] …`）。

```bash
# 0) 清日志，拿到干净的起点
./env/adb/adb.exe -s 9D91500007505171 shell "rm -f /userdata/applog/loli_s6_dev.txt"

# 1) 确认装的是新构建：起播 25 秒后 probe 行里应该已经出现 silent= / tries= 两个新字段
#    （旧版本没有这两个字段。JS 是被打包进 .amr 的，不要试图在设备上找 src/services/media.js）
./env/adb/adb.exe -s 9D91500007505171 shell \
  "grep -a 'play. probe' /userdata/applog/loli_s6_dev.txt | tail -3"
# 期望形如：... [play] probe stage=live v=1 screen=0(ON) phase=playing ws=open rx=1234 fps=30 kf=2 silent=0s tries=0 keep=on
```

### 5.2 主实验：进游戏，放着手不动，等 6 分钟

```bash
# 2) 起播后另开一个窗口，每 10 秒抓一次 rx（辅助观察，非必须）
while true; do
  ./env/adb/adb.exe -s 9D91500007505171 shell \
    "grep -a 'play. probe' /userdata/applog/loli_s6_dev.txt | tail -1"
  sleep 10
done
```

**期望看到的日志序列**（4m18s 左右开始）：

```
[...][E][JS] [pencg][W] [play] probe stage=live v=1 screen=0(ON) phase=playing ws=open rx=18874 fps=29 kf=3 silent=0s tries=0 keep=on
[...][E][JS] [pencg][W] [play] probe stage=live v=1 screen=0(ON) phase=playing ws=open rx=18874 fps=0  kf=3 silent=8s tries=0 keep=on
[...][E][JS] [pencg][W] [play] probe stage=live v=1 screen=0(ON) phase=playing ws=open rx=18874 fps=0  kf=3 silent=16s tries=0 keep=on
[...][E][JS] [pencg][W] [play] stall detected: rx=18874 已 20s 未增长 → 尝试自愈（第 1 次：补发关键帧）
```

分三种结局判定：

| 结局 | 日志 | 含义 |
|---|---|---|
| **A. L1 就救回来**（最好） | `stall detected … 第 1 次` 之后 5s 内出现 `stall recovered: rx=18880 已恢复（第 1 级自愈成功，停滞 20s）` | PLI 就够。用户最多看到 5 秒静止画面 |
| **B. 一路走到 L2/L3** | 依次出现「第 2 次：复用网关重连」「第 3 次：重新申请机位」，最后 `stall recovered: …（第 3 级自愈成功 …）` | 网关确实停推了，靠重连救回。用户会看到 1~3 秒黑屏 |
| **C. 三级全废** | `[play] stall: 三级自愈全部失败，交给用户（rx=… 已 …s 未增长）` + `[play] fail code=0` | 网关彻底不给流。UI 回到 fail 态，用户可手动点「重连」 |

### 5.3 一条命令看结果

```bash
./env/adb/adb.exe -s 9D91500007505171 shell \
  "grep -a 'stall' /userdata/applog/loli_s6_dev.txt | tail -30"
```

### 5.4 关键判据：是"网关停推"还是"我这边断了"

停流时必须能证明**不是我们自己的问题**。四条链同时看：

```bash
# 链 1：网关还通着吗（WS 断言 + 我们的心跳有没有被回显）
./env/adb/adb.exe -s 9D91500007505171 shell \
  "grep -aE 'wsclose|ws=open|op=input' /userdata/applog/loli_s6_dev.txt | tail -20"

# 链 2：native 还在收包吗（只有一个平面在送 = 视频停了）
./env/adb/adb.exe -s 9D91500007505171 shell \
  "grep -a 'Esmart1' -A9 /sys/kernel/debug/dri/0/summary"

# 链 3：解码器还在吗（进程在 ≠ 在出帧）
./env/adb/adb.exe -s 9D91500007505171 shell "pidof miniapp; pidof ffmpeg"

# 链 4：native 侧有没有自己拆（看门狗 / 错误）
./env/adb/adb.exe -s 9D91500007505171 shell \
  "grep -aE 'heartbeat lost|JS silent|tearing down|media loop exit' /userdata/applog/loli_s6_dev.txt | tail -10"
```

判读表：

| `stall detected` 出现时… | 说明 |
|---|---|
| `ws=open` 且心跳 `op=input` 仍在被回显 | **网关停推**（本次 bug）。走到 L2/L3 是对的 |
| `ws=closed` 或 `wsclose` 日志 | 网关/网络断了。这时应该走 `onMedia` 的 wsclose 分支（`stage='lost'`），STALLGUARD 的 L2 会顺带重连，属于可接受的兜底 |
| 有 `heartbeat lost` / `JS silent` | JS 真挂了（不是本次 bug），看门狗拆掉管线是正确行为 |
| `pidof miniapp` 空 | 进程没了，和 STALLGUARD 无关 |
| `Esmart1-win0: INACTIVE` 但进程在、`rx` 在涨 | 平面/路由问题，属于别的 bug 域（`video-integrity`） |

### 5.5 回归检查（不要只测故障路径，正常路径也必须不被误触）

```bash
# 连续玩 5 分钟，检查误判：期望 0 条 stall detected
./env/adb/adb.exe -s 9D91500007505171 shell \
  "grep -ac 'stall detected' /userdata/applog/loli_s6_dev.txt"
```

第二项回归：**切后台/回前台不应触发 stall**。

```bash
# 游戏中按 Home 键 / 切走 → 等 30s → 切回来。
# 期望：日志里有 enterBackground 与 onShow，但【没有】stall detected
./env/adb/adb.exe -s 9D91500007505171 shell \
  "grep -aE 'stall|enterBackground|page onShow' /userdata/applog/loli_s6_dev.txt | tail -20"
```

第三项回归：**换画质**（`pickQuality` 会重建媒体会话，`rx` 会归零）。期望看到 `[play] stall: 媒体会话重启，rx 重新计数`（这一条只在 `stallTries > 0` 时打，正常换画质应该一条都没有），并且不触发 `stall detected`。

---

## 6. 风险与已知局限

### 6.1 治标不治本（最重要）

**根因在服务端。** 这个补丁只是让客户端"发现并重连"，没有解决"网关为什么停推"。

真正修法二选一：

1. **实现 keep-playing 协议**（对齐官方 SDK 的 `RtcKeepPlaying` / `RtcKeepaliveCfg`）：在 ticket 之后周期性向网关宣告"人还在玩"，让服务端不启动 kickout。这需要网关侧的协议细节（报文格式、周期、字段），目前 `PenCG/docs/CGAPI.md` 里没有。
2. **调大网关侧的 `kickoutDuration`**（若控制台可配）。这是运营侧配置，客户端改不了。

> 如果要走第 1 条，需要 Lead 向云游戏侧要协议文档。**在这之前，本补丁是唯一能让用户玩下去的手段。**

### 6.2 用户会感知到的代价

| 场景 | 用户感知 |
|---|---|
| L1 就救回来（理想） | 静止画面约 20~25 秒，然后自己动了。**无黑屏** |
| 走到 L2 | 静止 25s + **黑屏 1~3s**（拆会话重连）+ 重新等 IDR。总共约 30s |
| 走到 L3 | 再叠一次重申请机位，可能要重新排队。**最坏几十秒**，且机位可能拿不回来 |
| 三级全废 | 弹提示「画面已停止（网关停止推流），自动恢复失败，请点「重连」」，用户手动点 |
| 网关每 4 分钟必停 | **每局都会经历一次 L2**，即每 4 分钟黑屏 1~3 秒。这是当前频次下的必然结果 |

20 秒的检测窗口本身也是可见的代价：从网关停推到我们开始救，用户至少盯着静止画面 20 秒。想更快只能压缩 `STALL_RX_MS`，但会引来误判（见 §3.2），所以刻意没压。

### 6.3 技术风险

| 风险 | 说明 | 缓解 |
|---|---|---|
| **L2 借用了"按钮门禁"的漏洞** | `stage='waiting'` 是为了绕过 `canReconnect()` 的 `live+open → false`。这属于借用按钮路径，不是它设计的用法 | `onReconnect()` 内部立刻把 stage 置 `'connecting'`，语义无漂移；且 `busy` 守卫保证不会并发进两条重连 |
| **`rx` 归零会被当"重新起算"** | 换画质、L2、L3 都会重建 native 会话。若实现成"归零=恢复"，会无限重连烧机位 | 已刻意不清 `stallTries`（§3.4 坑 2） |
| **误判：网关只是短暂停推（如换场景）** | 场景切换时网关可能停 5~30 秒再恢复。若正好超过 20s，会误触发 L1 | L1 只发 PLI，代价几乎为零，**不会**误触发 L2。只有 L1 后 5s 还不涨才升级。这是把 L1 放在最前的主要动机 |
| **误判：设备极度卡顿** | 如果整机卡到 tick 500ms 都跑不动，`rx` 自然不涨。但这种卡顿本身用户已经察觉，且 STALLGUARD 的重连也救不了 | `busy` / `dead` / `hiddenSince` 三道守卫让它至少不会在错误时机动手 |
| **`stallTries` 只在"恢复"时清零** | 若一次故障后网关稳定了很久，`stallTries` 仍是 0（因为恢复时清零了），没问题；但若 L2 后 `rx` 涨了却没触发"恢复"路径（例如 stage 不是 live），`stallTries` 会留着 | `rx` 增长判定**刻意不看 stage**（`:1330` 注释），所以连接/等待/播放任何阶段的增长都会清零 |
| **日志量** | 每次自愈 3~4 条 `logWarn`，20s 一条 probe。一天下来日志会涨 | `log.js` 有 300 条环形缓冲；落盘文件由系统轮转，可接受 |

### 6.4 没能验证的部分（明确交代）

1. **`kickoutDuration = 300s` 没有得到网关侧实证。** 这个数字来自官方 SDK 的默认值，我们用"300 − 42 ≈ 258s ≈ 4m18s"反推吻合。**没有**抓到 ticket 时间戳与停流时间戳的真实差值来坐实。
2. **42s 冷启动开销是估算**，来自日志里 ticket → 首次出画的时间差，样本量只有 3 次。
3. **L2 重连后网关是否"重新计时 4 分钟"** —— 未知。若服务端是"从会话创建起算总时长"，那 L2 重连拿到的会话可能仍然只剩很短，会立刻再停；这种情况会表现为 L2 → 20s → L3 → 20s → fail。**这个行为只能在真机上观察**（§5.2 结局 B/C 的区分）。
4. **`onReapply()` 之后机位能不能拿回来** —— 依赖队列状态，本补丁无法保证。
5. **20s 会不会误触** —— 真机上没测过长时间稳定播放（>10 分钟）时的 `rx` 增长平滑度。§5.5 的回归检查是必做的。
6. **本阶段完全没碰 adb**（Lead 要求设备别动），所有 adb 命令都是"给 Lead 执行"的，我这边一条都没跑过。

---

## 7. 刻意没做的事

| 没做 | 原因 |
|---|---|
| 没实现 keep-playing 协议 | 缺网关侧报文格式；属于协议层工作，不该塞进客户端自愈补丁 |
| 没在 `rx` 停滞时直接调 `stopGame` / 释放机位 | 那等于替服务端做了"这局结束"的决定。L1/L2 都还有救，直接释放机位是不可逆的 |
| 没有"用户 3 秒内有操作就跳过自愈" | 需求明确要求不要跳过：这是"人在玩但网关停推"，更要救 |
| 没改 `PenCG/src/services/screen.js` / `app.js` / `native/**` | 写域门禁；且第一阶段那几个文件的问题域不同（熄屏/生命周期），混改会让两条链的日志互相污染 |
| 没动 `log.js` | 只允许用 `logWarn`；`log.js` 本身的行为（只有 console.error 落盘）没改 |
| 没做"停滞时降码率重试" | 网关是整体停推（音视频同时停），不是码率协商失败，降码率无效 |
| 没把 `STALL_RX_MS` 做成可配置 | 云游戏客户端没有设置页，多一个开关只会增加误配面。真要调，改常量重新构建即可 |

---

## 8. 二期补充判据：弱网冻结（`rx` 在涨但画面不动）

> 追加于 2026-11-13 傍晚，起因是真机撞上了 §3.1 判据的盲区。本节记录现象、双判据设计、以及一次设计返工（旧判据在 native 兜底之后是死代码）。

### 8.1 真机现场（Lead 实测，2026-11-13 16:25–16:42）

原神 `yssy`，高铁 Wi-Fi，丢包 5~8%：

```
[pencg][W] [play] probe stage=live v=1 screen=0(ON) phase=playing ws=open
           rx=73867 fps=0 kf=6 silent=3s tries=0 keep=on
```

同时 native 侧：

```
[cgmedia] #1 still no IDR (rx=74418 kfReq=468 gateDrop=15511 auDrop=401) — gateway may be ignoring PLI
```

读法：

- `rx` 从 194 一路涨到 73867 —— **§3.1 的「`rx` 冻结 20s」判据永远不会触发**。
- `fps` 自 16:30:30 起连续 **12 分钟**为 0，画面实际是冻的。
- `tries=0` —— 一次自愈都没发起（因为一条判据都没触发）。

⇒ **`rx` 在涨 ≠ 画面在动。** 弱网下网关照推、包照收，但每个 AU 都被丢包打碎；native 的「首个完整 IDR 之前丢 P 帧」闸门把 15511 个 P 帧全丢了（`gateDrop` 持续增长），`auDrop=401` 是坏 AU 数。**平均每个坏 AU 赔掉 39 个 P 帧**。

### 8.2 双判据：两条互斥、共用一个上限

| | 判据 1（收包停滞，§3） | 判据 2（弱网冻结，本节） |
|---|---|---|
| 抓什么 | 网关**停推**（`rx` 冻结） | 网关**照推但解不出**（`fps` 恒 0） |
| 主时钟 | `rx` 不变持续 `STALL_RX_MS=20000` | `fps` 连续为 0 持续 `FREEZE_GATE_MS=12000` |
| 辅助条件 | — | 期间 `gateDrop+auDrop` 上涨 或 `rx` 上涨（证明包还在进） |
| 防毛刺 | — | 连续 `FREEZE_MIN_SAMPLES=3` 次 stat 采样全为 0 |
| L1 动作 | `requestKeyFrameBurst(2, 500)`，5s 后复查 | `requestKeyFrameBurst(3, 400)`，**9s** 后复查 |
| L2/L3 | 复用网关重连 → 重新申请机位 | **完全同一套动作**（`stallHealL2/L3` 复用） |

**共用 `stallTries` 上限（合计最多 3 次，不是各自 3 次共 6 次）。** 用 `stallOwner`（`'rx'` / `'freeze'`）标记"这次是谁发起的"：

- 判据 1 在 `stallOwner === 'freeze'` 时**完全退出升级链**（`PenCG/src/pages/play/play.vue:1471`），只继续维护 `stallLastRx` 基准；
- 判据 2 恢复后把 `stallOwner` 清空，两条都回到待命。

**计时器必须分开**（`stallL1Timer` vs `freezeL1Timer`），并且**交叉互斥**：判据 1 的入口在 `stallL1Timer` 之后再挡一道 `if (this.freezeL1Timer) return;`（`PenCG/src/pages/play/play.vue:1417-1423`）。反方向（判据 2 挡 `stallL1Timer`）**故意不加**，因为冻结判据的 L1 窗口（9s）本来就比收包判据（5s）长，而且判据 1 已经被 `stallOwner` 挡住升级链，再加一道会让冻结期间的恢复判定失效。

### 8.3 关键设计：为什么 L1 不直接重连

弱网下重连要重走一遍起播（几秒黑屏 + 可能重新排队申请机位），而丢包丢的是 IDR 本身——多打几个 PLI 就有机会重传回来。所以：

- L1 = 连发 3 个 PLI（间隔 400ms），**不动会话**；
- 9s 后 `fps` 仍是 0 才升 L2。

> 400ms 的间隔是为了在 RTT 内不互相覆盖；连发 3 发是因为弱网下单个 PLI 可能连请求本身都丢掉。

### 8.4 与 native「IDR 饥饿兜底」的配合边界（一次设计返工）

native 侧（video-integrity 的改动，不在我的写域）：`IDR_STARVE_RELAX_MS = 4000`，连续 4 秒拿不到 IDR 就 `have_kf=1` 放行 P 帧，接受花屏（`PenCG/native/csrc/cgmedia/cm_rtp.c:782-791`）。

**这推翻了我最初的判据写法。** 最初写的是「`gateDrop` 连续增长 12s → L1」。但：

- `gateDrop` 只在「首个完整 IDR 之前丢 P 帧」那道闸门里自增（`PenCG/native/csrc/cgmedia/cm_rtp.c:798`）；
- native 一旦放行（4s），`gateDrop` **立即停止增长**；
- ⇒ `grew` 恒为 false，12s 窗口永远攒不满 ⇒ **判据成了永不触发的死代码**。

**改法**：主时钟换成「`fps` 连续为 0 的时长」，`gateDrop+auDrop` / `rx` 只作为「包还在进」的辅助证据（任一在涨即可）。理由：

- 放行 P 帧后画面**可能依然是死的**（P 帧没有参考帧，解不出来），`fps` 仍为 0 —— 这正是要抓的场景；
- `auDrop` 在放行后**仍会涨**（坏 AU 照样计数），正好补上 `gateDrop` 停涨的空档；
- 若 native 兜底真的救活了（放行的 P 帧能解），`fps` 回到非 0，判据 2 在 ⓿ 处自行清零 —— 正确。

**时序上的分工**：native 4s 兜底 → 若有效，用户最多看到 4s 冻结，判据不介入；若无效，本判据 12s 才动。**故意比 native 慢**，避免抢在 native 前面重连把 native 的兜底机会抢掉。9s 的 L1 复查同理（弱网 IDR 要重传，比收包判据的 5s 长才合理）。

### 8.5 返工中修掉的四个真 bug

| # | bug | 后果 | 修法 |
|---|---|---|---|
| 1 | `checkFreeze` 由 `tick`（500ms）驱动，但 `gateDrop`/`fps` 只在 stat（~2s，且有 900ms 节流）时更新 | `gateDrop` 增长窗口被没有新采样的 tick 反复清零、`fps==0` 计数虚高 | `statSeq` / `freezeLastSeq`：一个 stat 采样只判一次；毫秒计时仍用真实时间 |
| 2 | **两条判据互相拆台（致命）**：弱网冻结时 `rx` 一直在涨，判据 1 的「rx 变大 = 已恢复」分支每 tick 把 `stallTries` 清零 | L1→L2→L3 升级链永远走不到第二级，表现为「无限次 L1 补帧」 | 引入 `stallOwner`；恢复判定加 `&& stallOwner === 'rx'`；判据 1 在 `stallOwner === 'freeze'` 时退出升级链 |
| 3 | `checkFreeze` 开头的 `if (freezeL1Timer) return;` 让 9s 等待期内的**恢复判定**也失效 | 画面已回来也会在 9s 后误升 L2 | 改为 `freezeL1Timer` 回调**自己判**（`fps>0` → 打 `freeze recovered` 收尾；仍 0 → `stallHealL2`），不重入 `checkFreeze`。顺带解决「重入会让 L1→L2 实际要 9+12=21s」 |
| 4 | 键盘摘平面的那段 `fps=0` 会被算成冻结 | 每次打字关键盘都可能补一次关键帧 | `checkFreeze` 加 `if (this.videoHidden)` 守卫 + 清空全零段（`PenCG/src/pages/play/play.vue:1608-1621`） |

### 8.6 可观测性

20s 一次的 probe 日志（`PenCG/src/pages/play/play.vue:1366`）新增字段，可直接肉眼判断：

```
[pencg][W] [play] probe stage=live v=1 screen=0(ON) phase=playing ws=open
           rx=73867 fps=0 kf=6 gateDrop=15511 auDrop=401 kfReq=468
           silent=3s frozen=12s tries=1(freeze) keep=on
```

- `frozen=` = `fps` 从何时起连续为 0（0=不冻）
- `tries=N(owner)` = 已用自愈次数 + 是谁发起的
- `gateDrop` / `auDrop` / `kfReq` 直接对应 native 的 `still no IDR` 那三个数

冻结判据自己的日志（一眼区别于收包停滞的 `stall detected`）：

```
[pencg][W] [play] freeze: 连续 12s fps=0，期间 gateDrop+auDrop +312 rx +1840
           （rx=73867 仍在涨，判为丢包打碎 AU、拿不到 IDR）→ L1 补帧
[pencg][W] [play] freeze recovered: fps=18 画面恢复（L1 补帧 / native 兜底生效，冻结 14s）
```

### 8.7 二期的风险与局限

| 风险 | 说明 |
|---|---|
| **判据 2 只在 native 兜底失效时才动** | 这是设计（§8.4）。若 native 兜底一直有效，判据 2 长期不触发是**正常**的，不要当成 bug |
| **弱网下 L1 可能救不回** | 丢包持续 20s 以上时，补 PLI 也换不回完整 IDR，最终会走到 L3 甚至 fail。这是「弱网下治标」的固有上限，不是判据问题 |
| **误报代价** | 误判成冻结会多发 3 个 PLI（代价极小）并可能升级到重连（代价大）。所以 `FREEZE_GATE_MS` 取 12s、要求 3 次连续采样，宁可慢不可误 |
| **12s 内用户已经在盯着静止画面** | 与判据 1 的 20s 相比更短，但感知上仍是 12 秒静止。想更快只能靠 native 的 4s 兜底 |

**二期验证方案另见 `PenCG/docs/BUGFIX-bug1-freeze-verify.md`**（本机无法造丢包：`tc` 是 BusyBox 版无 netem、无 `iptables`、无 `tcpdump`，必须靠路由器 netem 或外部工具）。

---

## 9. 改动文件清单

| 文件 | 行数 | 改动 |
|---|---|---|
| `PenCG/src/pages/play/play.vue` | 2270 → 2813（+543） | **一期**：import 增加 `requestKeyFrameBurst`；文件头常量与说明块；data 字段；`onShow` 基准重置；`tick()` 内挂 `checkStall`；20s probe 增加 `silent`/`tries`；新增 `stallReset()` 与 `checkStall()`；7 处生命周期清理。**二期**：`FREEZE_GATE_MS`/`FREEZE_L1_WAIT_MS`/`FREEZE_MIN_SAMPLES` 三个常量 + 说明块；`freezeDropBase`/`freezeRxBase`/`freezeFpsZeroSince`/`freezeFpsZeroCnt`/`freezeLastSeq`/`freezeL1Timer`/`statSeq`/`gateDrop`/`auDrop`/`kfReq`/`stallOwner` 等 data；stat 分支读三个新字段并 `statSeq++`；`tick()` 内挂 `checkFreeze`；抽出 `stallWindowReset()`/`stallHealL2()`/`stallHealL3()`/`stallHealGiveUp()` 供两条判据共用；新增 `checkFreeze()`；`checkStall` 加 freeze 交叉互斥守卫；probe 增加 `gateDrop`/`auDrop`/`kfReq`/`frozen`/`tries(owner)` |
| `PenCG/src/services/media.js` | 579 → 617（+38） | `kfBurst` / `clearKfBurst()` / `requestKeyFrameBurst()`；`stopPlay()` 开头清 burst |
| `PenCG/docs/BUGFIX-bug1-stallguard.md` | 新建 → 551+ 行 | 本文（§8 为二期补充） |
| `PenCG/docs/BUGFIX-bug1-freeze-verify.md` | 新建 | 二期真机验证方案（设备无法造丢包，需外部手段） |

构建验证：

```
$ cd PenCG && node ../env/node_modules/aiot-vue-cli/src/cli.js -q -p
未找到以下模块,可能会导致应用运行时异常(如模块为原生模块请忽略):
brightness,global,cgnative,cgmedia          ← 既有 warning，原生模块
libs 同步成功: …\PenCG\.falcon_\libs
应用构建成功,目录: …\PenCG\.falcon_
打包成功,路径: …\PenCG\8002026110200003.1_0_3.amr
                                              ← exit 0
```

两条既有 warning（`找不到 theme "theme-dark"`、`未找到模块 brightness,global,cgnative,cgmedia`）与本补丁无关，忽略。
