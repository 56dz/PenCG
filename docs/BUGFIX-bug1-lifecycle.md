# BUGFIX-bug1-lifecycle —— 游戏打开约四分钟后「停界面传输 / 像是自动退出了」

> 排查对象：PenCG（有道词典笔 X6 Pro 网易云游戏客户端）
> 日期：2026-11-12
> 涉及文件：`PenCG/src/services/screen.js`、`PenCG/src/pages/play/play.vue`、`PenCG/src/app.js`、
> `PenCG/native/csrc/cgmedia/cm_media.c`（+ 本文档）
> 未触碰：`cm_gst.c`（audio-route）、`cm_rtp.c`（video-integrity）

---

## 0. 结论速览

| # | 结论 | 证据强度 |
|---|---|---|
| 1 | **「约四分钟」不是网络问题，也不是网关问题，是系统 idle 计时器**：本固件 `/etc/input-event-daemon.conf` 的 `[Idle]` 段是 `270s = hal-screen down` / `300s = len_onoff.sh off; hal-screen off`。270s = **4 分 30 秒**，正是用户说的「约四分钟」。 | 强（配置原文 + 算术） |
| 2 | **主因是链 A：屏幕被系统调暗/断电 → Falcon 回调 App `onHide` → 旧代码无条件拆会话（stopPlay + stopGame）**。拆完 KMS 平面归还、机位释放，用户看到的就是「画面没了、像自己退出」。 | 强（时序吻合 4m30s；代码与 PenBili 真机日志同型） |
| 3 | **次因是链 B：熄屏让 QuickJS 定时器停摆 → `media.js` 的 120ms `poll` 断 → 60s 后 `cm_media.c` 看门狗判 `JS heartbeat lost` 打 `code:-6` 拆管线**。链 B 单独发生的话最早在 **5 分 30 秒**（270+60）才爆，与「四分钟」不符，所以它不是主因，但它是「画面冻住」这一观感的来源，必须一起修。 | 中（时序可区分，需真机日志坐实） |
| 4 | **保活（`keepScreenOn`）在这台机器上实际没生效**：旧 `screen.js` 只有 117 行、只覆盖 `$falcon.jsapi.brightness` 一种后端形态；一旦 miss，`keepOnce()` 永远返回 false，定时器空转，`[Idle]` 270s 照常触发。 | 中（代码可证 miss 路径存在；到底是不是 miss 需真机日志） |
| 5 | 两处「反向」设计也是 bug 的一部分：**「熄屏」和「用户把小程序切走了」在 UI 层必须相反处理**（前者保会话，后者拆会话），旧代码把两者当同一件事。 | 强（代码可证） |

已交付的最小补丁：4 个文件，**前端构建通过、cgmedia zig 编译通过**，未部署、未 git 操作。

---

## 1. 「四分钟」这个数字到底是什么

### 1.1 设备 idle 配置（实测原文，不是技能文档里那份）

```ini
# /etc/input-event-daemon.conf
[Idle]
270s = hal-screen down; hal-gsensor wakeup 1
300s = len_onoff.sh off; hal-screen off; freq_adj low
900s = system_sleep_wakeup
reset = sleep 0.1; freq_adj high; hal-screen reset; system_post_wakeup;
[Extend]
gsensor = /usr/bin/runGsorChger
```

- `270s`（**4 分 30 秒**）= `hal-screen down` → 背光 `bl_br` 113 → **5**，视觉上「几乎全黑」。
- `300s`（5 分）= `len_onoff.sh off; hal-screen off` → `bl_power` 0 → **4**，`screen_switch off` → weston 冻结 DSI → `vop2_crtc_atomic_disable`，视觉上「全黑」。
- `900s`（15 分）= `system_sleep_wakeup`（`echo mem > /sys/power/state`）→ **进程会被杀**。所以不管怎么修，15 分钟这条线必须靠保活拦住。

`hal-screen bright_time` 当前是 300，但**它改不动 `[Idle]`**（PenYS 已验证：把它设成 10，`[Idle]` 该 270s 照样 270s）。所以「调大 `bright_time` 省掉轮询」这条路是死的。

### 1.2 算术对照表

| 用户描述 | 候选解释 | 发生时刻 | 是否吻合 |
|---|---|---|---|
| 「约四分钟」 | `[Idle] 270s` 调暗 + 紧随的 onHide 拆会话 | **4m30s** | ✅ 完全吻合 |
| 「约四分钟」 | 链 B：270s 挂起 + 60s 看门狗 | 5m30s | ❌ 偏晚 1 分钟 |
| 「约四分钟」 | 链 B：300s 熄屏 + 60s 看门狗 | 6m00s | ❌ 偏晚 1.5 分钟 |
| 「四分钟」 | 网络 / ws 超时 | 随机 | ❌ 用户是稳定复现且时长恒定 |

**只要是「恒定 4 分钟出头」，就只能是 `[Idle]` 计时器。** 网络侧没有 240s/270s 级别的定时器（cgmedia 只有 `HANDSHAKE_BUDGET_MS 13000`、`STAT_INTERVAL_MS 2000`、`WATCHDOG_MS`；cgnative 的 curl/ws 超时都是 15s 级；im.js 的 `IDLE_TIMEOUT_MS=300000` 只在输入法里）。

> 注：`270s` 是从**最后一次输入事件**算起的，不是从「点开游戏」算起。用户手指一直在动（玩云游戏）就会一直 push 这个窗口 —— 这一点很关键：**用户报的「四分钟后」说明他这四分钟内其实没产生输入事件**（云游戏场景下确实可能：看剧情、等待加载、触屏事件没送到 input-event-daemon）。这也解释了为什么「有的人两分钟就掉、有的人十分钟才掉」。

---

## 2. 两条链的时序推演

### 2.1 链 A（**主因**）：系统熄屏 → 被当成「退后台」拆会话

```
t=0      用户点开游戏，beginSession() 起 keepAliveStart(20000)
           （PenCG/src/pages/play/play.vue:589 beginSession / :605 keepAliveStart）
         旧 PenCG/src/services/screen.js: keepOnce() 需要 state.mod（亮度后端）
         后端探测只覆盖 `$falcon.jsapi.brightness` 扁平形态一种
           └─ 若 miss → keepOnce() 返回 false，20s 定时器空转 ──┐
t≈270s   input-event-daemon [Idle] 到点 → hal-screen down          │
         bl_br 113 → 5，getScreenState() → 3(DIM)                   │
t≈270s   Falcon 认为「屏幕不亮了」→ WindowManager::setVisible(0)
         → App.onHide()（PenCG/src/app.js:36）
           与 play 页 onHide（PenCG/src/pages/play/play.vue:475）被调│
         【旧代码】onHide 里没有屏幕判据：                          │
           play.vue: clearTimers() + keepAliveStop() + bgTeardown()
           app.js  : stopPlay() + stopGame(code)
         → stopPlay() 同步走完 clearInterval + __cgMedia.stop + wsClose
         → kmssink set_state(GST_STATE_NULL) 归还 KMS plane
         → 退机位 → 用户下次亮屏：游戏页停在 stage='end'
           errText='已切到后台，本局已结束并释放机位'
                                                  
t≈300s   （若上面没拆）hal-screen off → bl_power=4，DSI 冻结
```

**用户观感**：玩到四分钟出头，屏幕一暗，点一下亮回来 —— 游戏没了 / 或者干脆黑着不回来。这就是他说的「停界面传输，应该是自动退出了」。

**为什么旧代码会这么写**：`bgTeardown` 的存在是有充分理由的 —— KMS 平面 `zpos=2`，比小程序合成面和系统桌面都高，**不解 `set_state(NULL)` 就会把最后一帧永久盖在系统 UI 上**（HANDOFF.md 里那个「软件都退出了画面还在」的报障）。策略本身没错，错的是**没有区分「真退后台」和「只是熄屏」**。

### 2.2 链 B（次因）：JS 定时器停摆 → native 看门狗自伤

```
t≈270s~300s  hal-screen down/off → weston freeze DSI
             Falcon 侧把 QuickJS 的执行轮次降下来/挂起
             （屏黑 + 无输入 = 系统判定"没人看"，不需要跑 JS）
t+120ms...   media.js pump() 的 setInterval(tick, 120)（PenCG/src/services/media.js:269）
             不再被调度 → __cgMedia.poll 不再调用
             cm_session.h 的 s->last_poll_ms 停在 t0（唯一刷新点是
             PenCG/native/csrc/cgmedia/cgmedia.c 的 js_poll）
             注意：**此时 UDP socket 还在收包、网关照推**，
             s->stats.bytes_rx 一直在涨（PenCG/native/csrc/cgmedia/cm_rtp.c:667）
t0+60s       旧看门狗只看 last_poll_ms（PenCG/native/csrc/cgmedia/cm_media.c 改前 :167-173）：
             `silent > WATCHDOG_MS(60000)` → CM_LOGW "JS heartbeat lost"
             → cm_emit code=-6 → break → destroy vbox/abox → 拆管线
```

**旧代码的判定错在哪**：它把「JS 定时器还活着」当成「会话还该继续」。这两件事不等价 ——

- JS 定时器停摆的**真实原因**可能只是「屏黑了，系统不给你 CPU」；
- 而「会话还该继续」的**真实判据**其实是：socket 还开着、网关还在推、进程还在跑。

`bytes_rx` 一直在涨就是这三件事同时成立的硬证据，旧代码完全没用它。

**为什么它不是主因**：60s 阈值把 270s 推到 330s（5m30s）、把 300s 推到 360s（6m00s），都比用户说的「四分钟」晚 1 分钟以上。只有「用户在 4m30s 时看到了变化」这一条与链 A 精确吻合。

**但它确实是「画面冻住」的来源**：链 B 发生时 `cm_emit` 的事件要等 JS 恢复才被 drain（`cg_ev_drain` 在 `js_poll` 里），用户亮屏的瞬间看到的是「最后一帧冻住 → 然后才消失」。

### 2.3 两条链的现场判别（真机按这个表看日志）

| 现象 | 链 A（熄屏→拆会话） | 链 B（JS 挂起→看门狗） |
|---|---|---|
| `[pencg]` 的 `[play] probe` 20s 一行 | **断在 4m30s 附近**，之后出现 `[play] onHide 拆管线` | **断在 4m30s 附近**，之后**没有** onHide 行 |
| `[play] onHide 拆管线 stage=... viz=1` | ✅ 有 | ❌ 无 |
| `[cgmedia] #N JS heartbeat lost (... ms silent, rx=... bytes)` | ❌ 无（管线已被 JS 主动拆走） | ✅ 有，且 `silent` ≈ 60000 |
| `pidof miniapp` | **还在**（进程没死，只是会话拆了） | **还在**（同上） |
| `dri/0/summary` 里 `Esmart1-win0` | `INACTIVE` / 消失（plane 已归还） | `INACTIVE` / 消失（看门狗拆的） |
| `hal-screen state` | `bl_power=4` 或 `bl_br=5` | 同左 |
| 亮屏后 App 状态 | play 页 `stage='end'`，文案「已切到后台，本局已结束并释放机位」 | 事件 `code:-6`，play 页走 `fail` 分支 |

**关键区分点**：「有没有 onHide 那一行」和「有没有 `heartbeat lost` 那一行」。

---

## 3. 旧实现的三处硬缺陷（代码级，已全部修掉）

### 3.1 `screen.js` 是删减版（117 行 → 561 行）

| 能力 | 旧版 | 后果 | PenBili 完整版 | 新版 |
|---|---|---|---|---|
| 后端探测形态 | 只有 `$falcon.jsapi.brightness` 扁平 | 真机旧日志命中的是 `new Brightness()` 形态 → **全 miss** | 6 条回退 | ✅ 6 条 |
| 续期手段 | 只有 `keepScreenOn()` | 只续 30s 窗口，**不恢复已掉下去的背光** | + `setBrightness` / `screenOnOff` | ✅ 全用上 |
| 深熄态复检 | 无 | `bp=4` 时第 1 次 keep 只置 state=1，**第 2 次才点亮** | 同 tick 补一次 | ✅ `keepRetryNeeded` |
| 长期 API | 无 | 少一层保险 | `startAlwaysScreenOn` + `setCloseScreenTime(1080)` | ✅ 每个会话武装一次 |
| 观测 | 无 | 「保活没生效」在日志里**看不出来** | 每轮一行 | ✅ 每轮一行 |
| 主判据 | `st > 0` 才救 | 依赖 `getScreenState` 可用，且没有「读不到」时的兜底 | `needsBrightRestore` | ✅ 带亮度兜底 |

**最容易骗人的一条**（写进新文件头注释了）：`getBrightness()` / `hal-screen get` 返回的 **80 是设置档位，不是实际 `bl_br`**。屏被调暗到 5 的时候它仍然返回 80。**主判据必须是 `getScreenState()`。**

`getScreenState()` 语义（PenBili 真机实测）：`0=ON`、`3=DIM`（可救回）、`1=OFF`（播放中也要救）、`4=未知`。

### 3.2 `play.vue` 的 `onHide` 把「熄屏」当「退后台」

旧 `onHide`：`hidden=true; clearTimers(); keepAliveStop(); bgTeardown();` —— 四步无条件执行。
`bgTeardown` 里也没有屏幕判据，只有输入法守卫（`imBusy || kbOpen`）。
`onStop` 更直接：`bgTeardown()`。

### 3.3 `app.js` 的 `onHide` 同理（而它是**唯一可靠**的收尾点）

HANDOFF.md:164-165 已确认：页面级 `onHide`/`onUnload` 在小程序被切走时真机不回调。所以 `PenCG/src/app.js:36` 的 `onHide` 是主收尾点 —— 它当时唯一「不能拆」的判断是输入法，没有屏幕判断。

### 3.4 `cm_media.c` 看门狗的判据错位

`WATCHDOG_MS 60000` + 只看 `s->last_poll_ms`。历史上还把 6 秒当阈值过（正常对局直接被掐死），后来放宽到 60s，但**判据一直是错的**，只是把误杀概率降下来了。

---

## 4. 最小补丁

### 4.1 `PenCG/src/services/screen.js`（重写，117 → 561 行，照 PenBili 语义）

结构（行号为本文件当前行号）：

| 段落 | 行 | 内容 |
|---|---|---|
| 头注释 | 1-36 | 本固件 `[Idle]` 原文、4 条真机教训、两条故障链 |
| 常量 | 38/44/45/50 | `KEEP_INTERVAL_MS=20000`、`KEEP_BRIGHT_MIN=20`、`KEEP_BRIGHT_RESTORE=80`、`KEEP_CLOSE_SCREEN_SEC=1080` |
| 单例 state | 53 | `{mod,probed,loading,warned,ticks,alwaysArmed,timer,active,intervalMs,lastState}` |
| 纯函数 | 88/102/111 | `screenStateName`、`needsBrightRestore`、`keepRetryNeeded` |
| 后端适配 | 120/147/159/163 | `makeBackend`（缺 `keepScreenOn` 一律视为非亮度后端，九项能力全部 `fn.call(holder)`）、`makeBackendFromClass`、`noteProbe`、`logProbeSummary` |
| 探测链 | 182/219 | `probeFlat()`：`$falcon.jsapi.brightness` → `$falcon.brightness`；`probeAsync()`：`import('brightness').default` → `new d.Brightness()` → `new Global()` → `global.default` |
| 读写 | 265/276/288/299/312 | `readBright`、`writeBright`、`readState`、`writeScreenOn`、`applyCloseScreenTime(1080)` |
| **`keepOnce()`** | 338 | 三段式，见下 |
| 旧导出保留 | 450/455/462/468/475 | `keepScreenOnOnce`、`screenStateCode`、`getScreenState`、`screenOn`、`initScreen` |
| 定时器 | 483/495/514 | `armTick`（setTimeout 链）、`keepAliveStart`、`keepAliveStop` |
| 观测 | 535/539/544 | `isKeepingScreen`、`keepAliveTicks`、`screenDiag` |

`keepOnce()` 的三段（`PenCG/src/services/screen.js:338-445`）：

```js
// ① 每个保活会话只做一次：startAlwaysScreenOn() + setCloseScreenTime(1080)
//    用 alwaysArmed 而不是 ticks===1：keepAliveStop() 会 stopAlwaysScreenOn，
//    下次 onShow 必须重新武装，否则第二个会话没有持续亮屏保护。
if (!state.alwaysArmed) { ...; state.alwaysArmed = true; }

// ② 每次续 30s 窗口 —— 这才是等价 hal-screen keep 的动作
b.keep();

// ②b 深熄态（bp=4）：单次 keep 只置 state=1，同 tick 再补一次
let midState = readState();
if (b.keep && keepRetryNeeded(midState)) { b.keep(); midState = readState(); }

// ③ 硬兜底：keepScreenOn 不恢复已掉下去的背光
//    OFF(1) → screenOnOff(1) + setBrightness(80)   ← setBrightness 不能上电
//    DIM(3) → setBrightness(80)
if (needsBrightRestore(bright, st)) { ... }
```

每轮打一行（`logWarn` → 经 `console.error` 落盘；`console.warn` / `console.log` **都不落盘**）：

```
[pencg][W] [screen] keep #3 backend=brightness.Brightness(new) ok bright=113 state=0(ON) always=on closeScreen=1080s ok
[pencg][W] [screen] !!! 屏幕已非亮屏 state=3(DIM) bright=80 → 走兜底 兜底=setBrightness(80)ok !!!
[pencg][W] [screen] 保活启动 20000ms backend=brightness.Brightness(new) ticks=0
[pencg][W] [screen] 保活停止 ticks=17（定时器已清，后台不再保活）
```

后端不可用时 `warnOnce` 只叫一次 + 降级，`keepOnce()` 返回 false 让 keeper 用 **1000ms 快速重试**，避免探测窗口正好跨过 270s。

### 4.2 `PenCG/src/pages/play/play.vue`（4 处生命周期守卫 + 20s 探针）

| 位置 | 改动 |
|---|---|
| `:143` | import 扩成 `{ initScreen, keepAliveStart, keepAliveStop, getScreenState, screenStateName, KEEP_INTERVAL_MS }` |
| `:229` | data 新增 `probeAt: 0` |
| `:475-507` `onHide` | **先判屏**：`if (this.screenIsOff())` → 打 `[play] onHide 但屏幕已熄/调暗 → 判为系统息屏，保持会话并重新保活`、`hidden=false`、`keepAliveStart(KEEP_INTERVAL_MS)`、`startTimers()`、**return**；否则才走原来的 `clearTimers + keepAliveStop + bgTeardown`（`:497` 的 keepAliveStop 保留在这一支里） |
| `:509-520` `onStop` | 同样先判屏，熄屏则只 `keepAliveStart` + return，不 `bgTeardown` |
| `:521-535` `onShow` | 日志加 `screen=` + `screenText()`；`keepAliveStart` 统一用 `KEEP_INTERVAL_MS` |
| `:589` `beginSession` / `:605` | `keepAliveStart(20000)` → `keepAliveStart(KEEP_INTERVAL_MS)`（消灭魔法数字，两处周期必须一致） |
| `:1238` `bgTeardown` | 在 `imBusy/kbOpen` 守卫之后新增**第三道守卫** `if (self.screenIsOff())` → 打 `[play] bgTeardown 跳过：屏幕已熄/调暗（系统息屏，不是退后台）` + `keepAliveStart(KEEP_INTERVAL_MS)` + return |
| `:1213` / `:1225` | 新增 `screenIsOff()` / `screenText()`（`0=ON 3=DIM 1=OFF`；**`null`/非数一律当「没熄」**，走保守拆管线路径） |
| `:1200-1207` `tick` 末尾 | 新增 20s 一次的存活探针（见下） |

探针那行日志是这次排查的核心仪器：

```
[pencg][W] [play] probe stage=live v=1 screen=0(ON) phase=playing ws=open rx=1048576 fps=30 kf=3 keep=on
```

四条链一次看全：

- 这行**还在刷** → JS 定时器活着；
- `rx` 在涨 → 数据流活着；
- `screen=3(DIM)/1(OFF)` → 系统 idle 已经在动，保活没拦住；
- `ws=` 变了 → 网络侧断（跟屏幕无关）。

`keepAliveStop()` 的 6 个调用点现状（**确认过没有播放态误停**）：

| 行 | 场景 | 该不该停 |
|---|---|---|
| `:497` | `onHide` 的非熄屏分支（真退后台） | ✅ 该停 |
| `:650` | `applySeat` 进 `queued` 后 `navTo('queue')` | ✅ 该停（已离开播放页） |
| `:1036` | `finish()`（正常结束） | ✅ 该停 |
| `:1059` | `fail()`（失败收尾） | ✅ 该停 |
| `:1098` | `doExit()`（用户主动退出，顺序：停媒体+收 WS → 退机位 → 停保活 → 记账） | ✅ 该停 |
| `:1294` | `cleanup()`（页面销毁/onUnload） | ✅ 该停 |

另外一个坑（`PenCG/src/services/screen.js:495` 的注释里也写了）：旧 `keepAliveStart` 是「先 `keepAliveStop()` 再 `setInterval`」，单例 `state.timer` 会被清 —— 新版保留这个幂等语义（`onShow` / `beginSession` 会重复调），但改用 **setTimeout 链**，避免单次续期被系统拖慢后回调堆积；`keepAliveStart` 里把 `ticks` 归零，防止日志里出现两条交错序列（那是定时器泄漏的指纹）。

### 4.3 `PenCG/src/app.js`（唯一可靠收尾点加熄屏守卫）

- `:4` 新增 `import { getScreenState, screenStateName } from './services/screen.js';`
- `:36-47` `onHide` 开头取 `screenSt` 并打进日志（**`app onHide screen=3(DIM)`** —— 光是这一行就能定位 bug1）
- `:51-54` 输入法守卫不动（保留）
- `:69-83` **新增熄屏守卫**（在输入法守卫之后、`stopPlay` 之前）：

```js
let screenOff = false;
if (screenSt != null) { const n = Number(screenSt); screenOff = (n === 3 || n === 1); }
if (screenOff) {
  console.error('[pencg][life] app onHide 判为系统息屏（screen=N(名字)）→ 不拆会话，交给 play 页重新保活');
  return;   // 不动 stopPlay / stopGame
}
```

- `:85-96` 真退后台分支多一行 `[pencg][life] app onHide 真退后台 → stopPlay streaming=X code=Y`
- `screenSt == null`（读不到）**按「没熄」走保守拆管线路径**：宁可多拆一次，也不能让 KMS 平面永久盖住系统 UI。

### 4.4 `PenCG/native/csrc/cgmedia/cm_media.c`（看门狗改两级 + 阈值可配）

| 位置 | 改动 |
|---|---|
| `:19-57` | 头注释重写：阈值历史「6s → 60s → **180s**」、为什么「只看 js_poll」是错的、为什么不能改成「永不拆」 |
| `:59-64` | `#ifndef PEN_CG_WATCHDOG_MS` / `#define PEN_CG_WATCHDOG_MS 180000` / `#define WATCHDOG_MS PEN_CG_WATCHDOG_MS` / `#define WATCHDOG_WARN_MS 60000` / `#define WATCHDOG_CHECK_MS 500`（原来定义了但没用，现在真用上） |
| `:171-179` | 循环内局部状态 `s_wd_last_check / s_wd_progress / s_wd_last_bytes / beat / silent / s_wd_sid / s_wd_warned`（`PenCG` 同时只有一个媒体会话；`s->id` 变了就在循环里重置） |
| `:285-332` | 看门狗主体，见下 |

核心判定：

```c
if (now - s_wd_last_check >= WATCHDOG_CHECK_MS) {          // 500ms 一次，不每 20ms 都算
    s_wd_last_check = now;
    if (s_wd_sid != s->id) { /* 新会话：重置 progress/last_bytes/warned */ }
    if (s->stats.bytes_rx != s_wd_last_bytes) {            // ★ 有数据流就算活
        s_wd_last_bytes = s->stats.bytes_rx;
        s_wd_progress = now;
    }
    beat = s->last_poll_ms > s_wd_progress ? s->last_poll_ms : s_wd_progress;   // 心跳取 max
    if (beat && now > beat) {                              // ★ 先比大小再相减（防无符号下溢）
        silent = now - beat;
        if (silent > WATCHDOG_MS) {                        // 180s：真拆
            CM_LOGW("#%d JS heartbeat lost (%llu ms silent, rx=%llu bytes), tearing down pipeline + plane", ...);
            cm_emit(s, "{\"t\":\"error\",\"msg\":\"JS heartbeat lost, session torn down\",\"code\":-6}");
            break;
        }
        if (silent > WATCHDOG_WARN_MS && !s_wd_warned) {     // 60s：只告警一次，不拆
            s_wd_warned = 1;
            CM_LOGW("#%d JS silent %llu ms but stream alive (rx=%llu bytes) "
                    "— screen off / JS suspended, keeping pipeline", ...);
        }
    }
}
```

三条硬约束都守住了：

1. **`now > beat` 在相减之前**。这是防无符号下溢的写法，不能改。踩过的坑记在注释里：`js_poll()` 在 JS 线程里刷新 `last_poll_ms`，媒体线程取完 `now` 之后 JS 线程又写了更大的值，`now - last_poll_ms` 下溢成 `2^64-1`，日志打 `18446744073709551615 ms` 并立刻拆掉正在玩的对局。`volatile` 挡不住这种跨线程语义竞态。
2. **拆管线的路径完整保留**（`:317-323` → `:336` 的 destroy vbox/abox + 归还 plane）。改成「永不拆」会让 KMS 平面永久盖住系统 UI，那是 HANDOFF.md 里 `GST_STATE_NULL_` 那个坑的邻居。
3. **阈值可配**：`zig cc -DPEN_CG_WATCHDOG_MS=60000` 就能现场缩短观察窗口，不必改代码。

阈值取 180s 的理由：要跨过 270s 的 `hal-screen down`（60s 跨不过），又要远小于 900s 的 `system_sleep_wakeup` —— 否则真到整机休眠、进程被杀，就没机会自己还 plane 了。

### 4.5 构建验证

```bash
# cgmedia（zig 交叉编译）—— 通过
cd PenCG && powershell -NoProfile -ExecutionPolicy Bypass -File native/build-media.ps1
#   OK -> ...\PenCG\libs\libjsapi_cgmedia.so (78.9 KB)     exit 0
#   （唯一的 warning 是 cm_gst.c 里既有的 gst_debug_message_get 判空写法，与本次改动无关）

# 前端 —— 通过
cd PenCG && node ../env/node_modules/aiot-vue-cli/src/cli.js -q -p
#   应用构建成功 / 打包成功,路径:...\PenCG\8002026110200003.1_0_3.amr     exit 0
#   （既有警告：找不到 theme "theme-dark"；未找到模块 global,brightness,cgnative,cgmedia
#     —— 后四个是原生模块，运行时才解析，可忽略）
```

本次**没有**部署到设备（门禁），因此 `8002026110200003.1_0_3.amr` 与 `libs/libjsapi_cgmedia.so` 只是构建产物。

---

## 5. 需要真机验证的断言（**按重要性排序**）

| # | 断言 | 如果不成立会怎样 | 怎么验 |
|---|---|---|---|
| **V1** | `$falcon` 上确实能从某一条回退拿到亮度后端（期望结论行 `后端探测结论：采用 ...`，而不是 `全部未命中`） | 保活仍然是空转，`[Idle]` 270s 照旧 —— 修了等于没修 | §6.3 第 1 步 |
| **V2** | 系统调暗/断电时 `getScreenState()` 返回 **3 或 1**（不是 4） | `screenIsOff()` 恒为 false → 守卫失效 → 链 A 原样复发 | §6.3 第 3 步 |
| **V3** | 20s 保活真的拦住了 270s（连玩 8 分钟不出现 `screen=3/1`） | 周期/后端还得调 | §6.4 |
| **V4** | 熄屏瞬间 Falcon 到底调不调 App `onHide` | 不调 → 链 A 少了触发点，只剩链 B，那 `WATCHDOG` 的 180s 是唯一防线（还能撑到 5m30s）；调 → 守卫必须正确 | §6.2 |
| **V5** | 熄屏后 QuickJS 定时器是否真的停摆 | 不停 → 链 B 不存在，`WATCHDOG` 改动只是加保险；停 → 链 B 真实存在 | §6.4 |

> **如果 V2 失败**（真机返回 4），改一行即可：`PenCG/src/pages/play/play.vue:1223`（`screenIsOff`）与 `PenCG/src/app.js:73`，把判据从 `n === 3 || n === 1` 改成 `n !== 0`。
> 但**先别直接改** —— `n !== 0` 会把「读状态失败」也算成熄屏，真退后台时平面就拆不掉了（KMS 覆盖系统 UI 那个坑）。等 V2 有结论再定。

---

## 6. 真机验证步骤（adb 命令级）

### 6.0 取证通道（**先读这个**，否则后面所有 grep 都是空的）

X6 Pro 的 Falcon 运行时里，JS 的 `console` **只有一个方法会写盘：`console.error`**。
`console.warn` 和 `console.log` 完全不落盘（全盘 `grep -a -c 'W][JS]'` 计数为 0）。
所以看日志的第一条命令永远是：

```bash
./env/adb/adb.exe -s 9D91500007505171 shell "grep -a -n 'pencg' /userdata/applog/loli_s6_dev.txt | tail -50"
```

落盘文件是 **`/userdata/applog/loli_s6_dev.txt`**（不是 `DictPen_*.log`，那个路径在本机不存在）。
每行的形态固定是：

```
[2026-11-12 22:15:24.947][E][JS] [pencg][W] [screen] keep #3 backend=... ok bright=113 state=0(ON)
└──────────┬────────────┘ └┬┘ └┬┘ └────┬────┘
        落盘时间戳        级别 来源   PenCG 自己打的级别前缀
```

三个要点，别读错：

1. **落盘级别恒为 `[E]`**。因为底层通道是 `console.error`，这一点和我们想表达的严重程度无关。
2. **真正的级别看行内 `[pencg][I]` / `[pencg][W]` / `[pencg][E]`**。
   grep 级别时要 grep 行内前缀，不要 grep `][E][JS]`。
3. PenCG 侧所有输出都经 `PenCG/src/services/log.js` 的 `push()` 走 `console.error`（`log.js:44`），
   所以 `logInfo` / `logWarn` 打出来的东西一样会落盘 —— 只是行内前缀不同。

> 校验通道是否通的快速办法：随便触发一次，然后
> `grep -a -c 'pencg' /userdata/applog/loli_s6_dev.txt`，计数为 0 就是**通道问题**，不是代码没跑。
> 先确认这件事，再去怀疑逻辑。

> native 层（`[cgmedia]`）走的是 `cm_log()` → `fprintf(stdout)`，是**另一条独立通道**，
> 与上面的 JS console 无关；它是否也落进 `loli_s6_dev.txt` 待现场确认
> （本文档只需关注 `[pencg]`）。

> 前置：X6 Pro 在跑 PenCG `1.0.3`，`appid 8002026110200003`。
> adb 在 `./env/adb/adb.exe`（MSYS Bash 下**不要**用 `adb pull /tmp/x.png`，
> 会被改写成 `C:\Users\...\Temp\x.png`；需要 pull 就走 PowerShell，见 docs/DEV.md）。
> 取证通道与行格式见 §6.0（**只有 `console.error` 落盘**，`console.warn`/`console.log` 都不落盘）。
> 设备上 `ps` 是残的（`ps -ef` 常扫不到东西），进程用 `pidof`。

```bash
cd /d/Users/Daniel/code/DSH/x6pro/haas-x6pro
ADB=./env/adb/adb.exe
```

### 6.1 准备：清日志、确认基线

```bash
# 1) 清掉旧日志，只留本次（注意：这是唯一会"写"设备的动作，只动日志文件）
$ADB shell "rm -f /userdata/applog/loli_s6_dev.txt"

# 2) 基线：确认 idle 配置与屏幕当前状态
$ADB shell "cat /etc/input-event-daemon.conf"
$ADB shell "hal-screen state"
$ADB shell "hal-screen bright_time"

# 3) 部署并启动（Lead 执行；本次排查未部署）
#    cd PenCG && powershell -NoProfile -ExecutionPolicy Bypass -File tools/deploy.ps1 -Page index
```

期望第 2 步看到 `270s = hal-screen down` / `300s = len_onoff.sh off`（若不同，本文档所有数字按实际值等比缩放）。

### 6.2 主实验：进游戏后**完全不碰屏幕**，盯 6 分钟

开一局能持续出画的游戏（端游/手游都行），然后**手指离开屏幕**，按下表采样。建议另开一个 shell 每 15s 打一次快照：

```bash
# 一次性快照脚本（只读）
snap() {
  echo "=== $(date +%H:%M:%S)"
  $ADB shell "hal-screen state"                       # bl_br / bl_power / screen_state
  $ADB shell "cat /sys/class/backlight/backlight/brightness /sys/class/backlight/backlight/bl_power"
  $ADB shell "pidof miniapp"                          # 小程序进程还在吗
  $ADB shell "pidof aplay"                            # 音频还在吗
  $ADB shell "grep -a -A9 'Esmart1' /sys/kernel/debug/dri/0/summary"   # KMS 视频平面还在吗
  $ADB shell "grep -a '\[pencg\]' /userdata/applog/loli_s6_dev.txt | tail -n 3"
  $ADB shell "grep -a '\[cgmedia\]' /userdata/applog/loli_s6_dev.txt | tail -n 3"
}
for i in $(seq 1 24); do snap; sleep 15; done
```

### 6.3 三个判据

**第 1 步：保活到底有没有拿到后端（V1）**

```bash
$ADB shell "grep -a -E '\[screen\] (probe|后端探测结论|保活启动)' /userdata/applog/loli_s6_dev.txt"
```

- 期望看到 6 行 `[screen] probe 形态=... → 命中/miss(...)`，然后是
  `[screen] 后端探测结论：采用 <name> 能力[keep=1 ...]`。
- **看到 `后端探测结论：全部未命中` 就是红色警报** —— 说明这台固件上六条回退都不通，
  得去 native 侧兜底（例如给 cgmedia 加一个 `keepScreenOn` 命令，走 `hal-screen keep`）。

**第 2 步：保活有没有在续（每 20s 一行，`keep #N` 的 N 必须单调涨）**

```bash
$ADB shell "grep -a '\[screen\] keep #' /userdata/applog/loli_s6_dev.txt | tail -n 12"
```

正常：

```
[screen] keep #5 backend=brightness.Brightness(new) ok bright=113 state=0(ON) always=on closeScreen=1080s ok
```

异常：

- `state=3(DIM)` / `state=1(OFF)` 频繁出现 → 保活的续期窗口不够（把 `KEEP_INTERVAL_MS` 降到 15000 试）
- `!!! 屏幕已非亮屏 ... → 走兜底 兜底=setBrightness(80)err=...` → 兜底 API 不可用，得换手段
- `keep #N` 出现两段交错的 N（1,1,2,2,...）→ 定时器泄漏

**第 3 步：屏幕状态码到底是什么（V2，最关键）**

在 270s 之后（屏幕已经暗了）立刻执行：

```bash
$ADB shell "hal-screen state"      # 看 bl_br=5 / bl_power=0 或 4
$ADB shell "grep -a '\[play\] probe' /userdata/applog/loli_s6_dev.txt | tail -n 3"
```

看 `[play] probe` 那行里的 `screen=` 字段：

- `screen=3(DIM)` 或 `screen=1(OFF)` → ✅ V2 成立，守卫会生效
- `screen=4(未知(4))` → ❌ V2 失败，按 §5 的备选方案改判据
- `screen=null` → JSAPI 读不到，守卫不生效（会走保守拆管线），也要按 §5 处理

### 6.4 判「是熄屏还是 ws 断」（三条链的现场指纹）

```bash
# ① 熄屏：屏幕状态与背光
$ADB shell "hal-screen state"                    # screen_state:4 sys_br:80 bl_br:5 ...
$ADB shell "cat /sys/class/backlight/backlight/bl_power"   # 0=上电 4=断电

# ② 有没有画面：KMS 视频平面是否还在
$ADB shell "grep -a -A9 'Esmart1' /sys/kernel/debug/dri/0/summary"
#   ACTIVE + src/dst 有正常尺寸 → 平面还在，画面只是没人看
#   INACTIVE / 整段消失        → 平面已经被拆（链 A 或链 B 干了它）

# ③ 媒体进程与音频（进程活着 = 不是崩溃/被系统杀）
$ADB shell "pidof miniapp"
$ADB shell "pidof aplay"

# ④ JS 链（20s 一行）
$ADB shell "grep -a '\[play\] probe' /userdata/applog/loli_s6_dev.txt | tail -n 20"

# ⑤ 收尾是谁干的
$ADB shell "grep -a '\[pencg\]\[life\]' /userdata/applog/loli_s6_dev.txt | tail -n 10"   # App 级 onHide
$ADB shell "grep -a '\[play\] onHide 拆管线' /userdata/applog/loli_s6_dev.txt | tail -n 3"
$ADB shell "grep -a 'JS heartbeat lost\|JS silent' /userdata/applog/loli_s6_dev.txt | tail -n 5"
$ADB shell "grep -a '\[play\]' /userdata/applog/loli_s6_dev.txt | grep -a 'stage=' | tail -n 5"
```

判读表：

| 观测 | 结论 |
|---|---|
| `[play] probe` **断了**，之后**有** `[pencg][life] app onHide screen=3/1` + `[play] onHide 拆管线` | **链 A**（熄屏被当退后台） |
| `[play] probe` **断了**，之后**没有** onHide 行，但有 `#N JS heartbeat lost (... ms silent, rx=... bytes)` | **链 B**（JS 挂起 → 看门狗） |
| `[play] probe` **一直在刷**，但 `rx` 不涨、`ws=closed`，`Esmart1 INACTIVE` | **网络 / 网关侧断流**，与屏幕无关 |
| `[play] probe` 一直在刷，`rx` 在涨，`screen=3/1`，`Esmart1 ACTIVE` | 只是屏黑了，**画面其实还在** = 纯保活问题（最理想的结果） |
| `pidof miniapp` 为空 | 进程被杀了（900s 的 `system_sleep_wakeup` 或 OOM），是另一条链 |

**注意 adb shell 的引号坑**（`references/debugging.md`）：`grep 'a b'` 在 `adb shell "..."` 里容易把单引号吃掉，所以上面统一用 `grep -a`，且正则里不要用 `\|` 之类需要转义两次的写法 —— 分开跑几条 `grep` 最省事。

### 6.5 修后验收标准

| # | 动作 | 通过标准 |
|---|---|---|
| A1 | 进游戏后**不碰屏幕** 8 分钟 | `[play] probe` 每 20s 不断；`screen=` 恒为 `0(ON)`；画面不黑 |
| A2 | 同上，看 `[screen] keep #N` | N 单调涨到 ~24（8min/20s），`state=0(ON)` |
| A3 | 同上，**手动按电源键熄屏 10 秒再按亮** | 亮屏后**游戏还在**（`stage=live`，`viz=l`），能继续操作；日志里有 `onHide 但屏幕已熄/调暗 → 判为系统息屏，保持会话并重新保活` |
| A4 | 手动按 Home 键（**真退后台**） | 必须拆：日志有 `app onHide 真退后台 → stopPlay` + `[play] onHide 拆管线`，`dri` 里 `Esmart1` 变 `INACTIVE`，回到桌面**看不到残留画面** |
| A5 | 玩到 15 分钟以上 | 不出现 `system_sleep_wakeup` 杀进程（`pidof miniapp` 仍在） |
| A6 | 长按退出 / 正常结束 | `stage=end`，机位已释放，`[screen] 保活停止 ticks=N` |

A4 是**回归门槛**：为了修 bug1 而让 KMS 平面永久盖住系统 UI，比 bug1 本身更糟。

---

## 7. 刻意没做的事（以及为什么）

| 没做 | 原因 |
|---|---|
| 把 `getScreenState()` 返回 4/null 也当熄屏 | 会让「真退后台」不再拆管线 → KMS 平面永久盖住系统 UI。等 V2 有真机结论再定（§5 备选方案） |
| 把 `WATCHDOG_MS` 改为「永不拆」 | 同上；且真到整机休眠/进程被冻时再也没人还 plane 了 |
| 改 `/etc/input-event-daemon.conf` 的 `[Idle]` | 影响整机行为 + OTA 会重置 + 用户明确设过自动熄屏时间。**改系统配置不是这个 bug 的修法** |
| 改 `hal-screen bright_time` | 已实测改不动 `[Idle]`（PenYS 验证） |
| 动 `cm_gst.c` | 归 audio-route。如果后续发现 `aplay` 卡住主循环导致 `cm_udp_pump_media(..., 20, ...)`（`PenCG/native/csrc/cgmedia/cm_media.c:214`）实际耗时远超 20ms、进而让 `bytes_rx` 更新节流，那是 audio 侧的事，不在本 bug 的最小补丁范围内 |
| 动 `cm_rtp.c` | 归 video-integrity。本次只**读**它的 `s->stats.bytes_rx`（`PenCG/native/csrc/cgmedia/cm_rtp.c:667`）作为存活证据 |
| 在 `screen.js` 里调 shell（`hal-screen keep`） | JS 层没有 shell 能力，明文权限边界见 docs/PLATFORM.md |

---

## 8. 附：本次改动的完整文件清单

| 文件 | 前 | 后 | 性质 |
|---|---|---|---|
| `PenCG/src/services/screen.js` | 117 行 | 561 行 | 重写（照 PenBili 完整语义） |
| `PenCG/src/pages/play/play.vue` | 2109 行 | 2187 行 | 4 处生命周期守卫 + 20s 探针 + 2 个新方法 |
| `PenCG/src/app.js` | 77 行 | 114 行 | 熄屏守卫 + 诊断日志 |
| `PenCG/native/csrc/cgmedia/cm_media.c` | 183 行 | 343 行 | 看门狗两级判定 + 阈值可配 |
| `PenCG/docs/BUGFIX-bug1-lifecycle.md` | — | 本文档 | 新增 |
