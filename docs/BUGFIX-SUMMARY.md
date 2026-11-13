# PenCG 五个 bug 修复总汇（诚实版）

> 设备：有道词典笔 X6 Pro / RK3562 / `9D91500007505171`，Falcon mini-app 运行时
> 应用：PenCG（网易云游戏客户端，appid `8002026110200003`，包 `8002026110200003.1_0_3.amr`）
> 汇总截止：2026-11-13 22:20（北京时间）；**最后一次实质更新**：
> ① bug6（新增）由 `restore-crtc=false` + `js_start()` 抢先收会话修复并真机 A/B 验证（§2.6）；
> ② bug2 状态由「代码已改未验证」升为「✅ 已真机验证」（§2.3）；
> ③ STALLGUARD 由「未验证」升为「✅ 已真机验证」（L2 重连成功，§2.2）。
> 工作树状态：**22 个文件已修改 + 17 个文件新增，全部未 commit**（详见 §5；本轮工作树仍在被其他 agent 改动，数字以写定时为准）
>
> **本文的写作原则：只写有证据的事实。**
> 「代码写了」「编译通过」「已装机」都不等于「真机验证通过」。
> 每个结论后面都标了证据来源，凡是没有真机证据的一律写「未验证」。
> 证据分级：
> - **【现场】** = 真机日志/读数，**有具体文件路径或原文**可复查
> - **【转述】** = 真机现场观测，但**没有**留下日志文件或原文（只有口头/会话记录）
> - **【代码】** = 只在源码层面确认
> - **【待确认】** = 没有任何证据

---

## 1. 一句话总览表

| # | 用户现象 | 根因 | 修法 | 真机验证状态 |
|---|---|---|---|---|
| **bug1-A** | 游戏打开约四分钟后会停界面传输 应该是自动退出了（**屏幕变暗/断电这一半**） | 系统 `[Idle]` 270s 调暗 / 300s 断电熄屏 → App 收到 `onHide` → 旧代码无条件拆会话；且旧 `screen.js` 只支持一种亮度后端，保活恒失败 | `screen.js` 从 117 行重写到 589 行（多后端探测 + 20s `keep` + 亮度还原）；`app.js` 用 `isScreenNotOn()` 把「熄屏」和「真退后台」分开 | ✅ **已真机验证**（保活 9 分 20 秒不间断，见 §2.1） |
| **bug1-B** | 同上（**画面不再更新这一半**） | 网关在起播后 **4m19s / 4m18s / 4m18s** 单方面停止推送 RTP，而我们从未发过官方 SDK 的 `RtcKeepPlaying(20015)` / `RtcKeepaliveCfg(20021)`（服务端挂机 kickout 定时器） | **根因未修**；只有 JS 侧 `STALLGUARD` 三级自愈（检测 `rx` 20s 不涨 → 补关键帧 → 重连 → 重新申请机位）治标 | ❌ **未修复**（根因在服务端；STALLGUARD 本身 ⚠️ 代码已改未验证） |
| **bug2** | 登录界面登录成功后跳转回去显示"未登录"，只有点击左侧边栏首页再点回来才能加载出用户信息 | 登录页和 index 页是两份独立 QuickJS 上下文，`mine-body` 的 `refresh()` 只读本页模块级会话对象 → 恒读到空；`setPendingTab` 写的模块变量跨页也读不到 | `auth.js` 新增跨页会话版本号（`$falcon` + storage 双写）；`mine-body.vue` 的 `refresh()` 补 `await restoreSession()` + 2.5s 版本轮询；`index.vue` 加 `checkSeq()` + 误跳保护；`nav.js` 的 tab 标记改存 `$falcon` | ✅ **已真机验证【现场】**（2026-11-13 18:29 真机第 2 轮 PASS，见 §2.3） |
| **bug3** | 用"原神"测试，进入游戏后游玩画面经常性出现上一帧的马赛克残留 | **定案：`kmssink sync=true`** —— `appsrc`（`format=time do-timestamp=true`）按到达时刻打的时间戳与 kmssink 的 running-time 基准错位后，buffer 被判「没到时刻/已过期」整体丢弃，平面绑着却一口不翻页，用户看到的就是上一帧，固化后变成「卡住不动」。此前归因的码流/硬解/DPB 均已被 AU dump 推翻 | **`cm_gst.c:253-255` 的 `KMS_SINK_PROPS`：`sync=true` → `sync=false`**（★真正修复）。RTP/出画那一批（AU 完整性闸门、`ignore-error=false`、`DELTA_UNIT`、flush 重协商）保留为加固，不是根因 | ✅ **已真机验证【现场】**：原神 + 终末地两个游戏**用户目视确认画面正常**；`fb` 由「恒 130」变为「127/130 交替」，三份日志见 §2.4.0 |
| **bug4** | 用"明日方舟终末地"测试，建立连接后画面就存在马赛克和绿屏 | **同上，同一个根因**（`kmssink sync=true`）。**另外：绿屏是片头 IDR 到达之前的解码器输出**（DPB 空、无参考帧时的未初始化 NV12），IDR 一到就自愈 —— **这是正常现象，不是 bug**。真正「一直全黑 / 不动」才是 `sync=true` | 同上（`sync=false`）。`cm_rtp.c` 每局 `cm_rtp_reset()`、「首个完整 IDR 之前丢 P 帧」闸门、缓存参数集注入仍保留为加固 | ✅ **已真机验证【现场】**，同 bug3。**注意别把片头绿屏当成回归** |
| **bug6** | （用户 22:07 追加反馈）终末地游玩中"没画面，不是黑屏"，实际是**视频在动、UI 层丢了、触摸失效**；重复 `miniapp_cli start` 后**整屏黑且只能重启恢复** | kmssink 的 `restore-crtc` 默认 true，收尾时 `drmModeSetCrtc(crtc, 0, ...)` 式恢复会把该 CRTC 上**所有** plane 的 mask 清掉 —— 连 weston 的 `plane[54]` 一起清掉，weston 不会主动重挂；且两路 play 页各建一条 `plane-id=76` 管线抢同一硬件平面 | ① `cm_gst.c` 的 `KMS_SINK_PROPS` 加 `restore-crtc=false`（只 disable 自己的 plane，不碰 CRTC）；② `cgmedia.c` 的 `js_start()` 在 `sess_alloc()` 前先 `sess_kill_all_video()` 收掉所有旧视频会话 | ✅ **已真机验证【现场】**：修复前二次 `start` → `plane_mask` 3→0、整屏黑；修复后 `plane_mask` 稳定为 1、`plane[54]` fb 在 126/128 持续翻页、`miniapp_cli capture` 截到完整首页 UI |
| **bug5** | 出现下拉系统控制栏，音量调为0时，进入游戏后还是有声音，并且在退出后，使用其他软件包括有道查词也会出现声音 | 旧代码看到 card1 `numid=54` = `0,0` 就 `cset 200,200`（替用户把音量抬满）；退出时既不还原 `Playback Path` 也不还原功放，且 `aplay` 的 SIGTERM/SIGKILL 无间隔、没有任何死亡自证 | `cm_gst.c` **整段删除** `audio_amp_volume_ensure()`；新增开播快照 + 幂等条件还原 `cm_audio_route_restore()`；`aplay_stop()` 改 EOF→SIGTERM→SIGKILL 四阶段 + 自证日志 | ✅ **已真机验证【现场】**（`DictPen_20261113_070039.log` 原文 + 正反两面证据，见 §2.5） |

**附：两项不修 bug、但属于同一轮工作的改动**

| 项 | 内容 | 真机验证状态 |
|---|---|---|
| RTCP RR | 我们**从未发过** RTCP Receiver Report（网关因此可能降级/停发）。`cm_cmd.c` 新增 `cm_rtcp_tick()` 每秒发 RR，`cm_media.c` 的 stat 里加 `jitterMs` / `rrSent` | ✅ **已真机验证**（`#1 RR sent (70 bytes, srtcp) rc=2 ...` 稳定出现）。**注意：RR 不是 bug1 的根因**，详见 `BUGFIX-bug1-rtcp-rr.md` §12 |
| 弱网 IDR 饥饿 | 丢包下「rx 一直涨、fps 恒 0」，PLI 发 468 次无效，`gateDrop` 涨到 15511、`auDrop=401`（1 个坏 IDR 赔 39 个 P 帧） | ⚠️ **已修、已编译、已装机，未在弱网下真机复现验证**（修复 = RTCP Generic NACK + `IDR_STARVE_RELAX_MS 4000` 兜底） |
| 弱网冻结判据（二期） | bug1-B 的作战手段之一：`rx` 在涨但 `fps` 恒 0（解不出来，不是没收到）时，连续 12s 全零 → 补 3 个关键帧 → 9s 未恢复升 L2。判据代码 `play.vue` `checkFreeze()`（`:1603`），常量 `FREEZE_GATE_MS 12000`（`:246`） | ⚠️ **代码已写、构建通过；正对照（`FREEZE_GATE_MS` 临时压到 3000 的调试构建）未跑，真机未验证**。方案见 `BUGFIX-bug1-freeze-verify.md`（状态「方案（未执行）」） |

---

## 2. 逐个 bug 详述

### 2.1 bug1-A：生命周期 / 屏幕保活 —— ✅ 已真机验证

**用户原话**

> 游戏打开约四分钟后会停界面传输 应该是自动退出了

**根因链（两条独立通路，都会在 4 分半左右触发）**

1. 设备 `/etc/input-event-daemon.conf` 的 `[Idle]` 段实测：
   - `270s = hal-screen down`（4 分 30 秒，背光从 113 → 5）
   - `300s = len_onoff.sh off; hal-screen off`（5 分钟，`bl_power` 0 → 4，DSI 冻结）
   - `900s = system_sleep_wakeup`（15 分钟，杀进程）
2. **链 A（主因）**：熄屏 → Falcon 回调 App `onHide` → 旧代码**无条件** `stopPlay()` + `stopGame()` → 机位释放、KMS 平面拆除。用户看到的就是「自动退出了」。
3. **链 B（次因）**：熄屏 → QuickJS 定时器停摆 → `media.js` 的 120ms poll 断 → 60s 后 `cm_media.c` 看门狗判 `JS heartbeat lost` 打 `code:-6` 拆管线。单独发生最早 270+60 = 5 分 30 秒。
4. 旧 `screen.js` 只有 117 行，只覆盖 `$falcon.jsapi.brightness` 一种后端，探测 miss 时 `keepOnce()` 恒返回 false —— 保活从头到尾没生效过。

**修法（file:line）**

| 文件 | 改动 |
|---|---|
| `PenCG/src/services/screen.js` | 117 行 → 589 行。多后端探测（`makeBackend` / `makeBackendFromClass` / `probeAsync` / `logProbeSummary`）、`keepOnce()`（`:363`）、`getScreenState()`（`:490`）、`keepAliveStart()`（`:523`）/ `keepAliveStop()`（`:542`），20s 一轮 keep + 亮度还原（`KEEP_BRIGHT_MIN=20` / `KEEP_BRIGHT_RESTORE=80` / `KEEP_CLOSE_SCREEN_SEC=1080`） |
| `PenCG/src/services/screen.js:116` | 新增纯函数 `isScreenNotOn(st, bright)` —— 全项目**唯一**该用来判屏幕状态的入口，只依赖「0 = 亮屏」这一个已证实事实 |
| `PenCG/src/app.js:82` | `onHide` 里用 `isScreenNotOn()` 判定：熄屏 → 打 `app onHide 判为系统息屏 ... → 不拆会话` 并 `return`；真退后台（`:98`）才 `stopPlay` + `stopGame` |
| `PenCG/src/app.js:53` | 系统输入法守卫：`inputBusy()` 为真时直接 return（否则弹键盘就会拆整局） |
| `PenCG/src/pages/play/play.vue` | `HIDE_GRACE_MS = 60000`（`:172`）/ `HIDE_GRACE_DARK_MS = 10000`（`:178`）/ `screenIsOff()`（`:1760`）/ `enterBackground()`（`:1783`）/ `armHideGrace()`（`:1805`）—— onHide 只摘视频平面（廉价可逆），60s 内回来无缝继续，超时才真拆 |
| `PenCG/native/csrc/cgmedia/cm_media.c` | 看门狗两级判定：静默 > `WATCHDOG_WARN_MS 60000` 只打一次 `JS silent` 标记不拆（`:63`）；> `WATCHDOG_MS 180000`（`PEN_CG_WATCHDOG_MS` 可 -D 覆盖，`:60`）才拆并打 `JS heartbeat lost`（`:343`） |

**验证证据原文**

保活（**【现场】** `.tmp/idle-probe-keep.log`，2026-11-13 18:17:42 → 18:27:11 共 30 行，每 20s 一行）：

```
18:17:42 t0 state=screen_state:0 sys_br:80 bl_br:113 map2brightness:113|get_screen_state: SCREEN_ON|
18:18:02 t20 state=screen_state:0 sys_br:80 bl_br:113 map2brightness:113|get_screen_state: SCREEN_ON|
...
18:27:11 t560 state=screen_state:0 sys_br:80 bl_br:113 map2brightness:113|get_screen_state: SCREEN_ON|
```

**9 分 20 秒内屏幕状态与背光一次都没掉过** —— 说明每 20s 的 `hal-screen keep` 确实压住了 `[Idle]`。

保活日志本体（**【现场】** `.tmp/bug1-soak.log` / `.tmp/bug1-home.log`）：

```
[2026-11-12 22:46:29.596][E][JS] [pencg][W] [screen] keep #10 backend=brightness.Brightness(new) ok bright=20 state=0(ON)
[2026-11-12 22:52:11.923][E][JS] [pencg][W] [screen] keep #27 backend=brightness.Brightness(new) ok bright=20 state=0(ON)
```

`backend=brightness.Brightness(new)` 就是探测选中的后端；`#N` 单调递增证明周期没断。

**未覆盖的部分（诚实标注）**：用户后来把自动熄屏从 270s 改到 570s / 600s / 1200s，所以「压住 570s」这件事**没在 270s 原配置下复验过**；`[Idle] 900s` 的 `system_sleep_wakeup` 是否也压得住，**未验证**（9 分 20 秒的记录没跑到 15 分钟）。

---

### 2.2 bug1-B：网关 4 分钟后停止推流 —— ❌ 未修复

**现象**：起播后 4 分多钟画面定格，音频同时停，**WebSocket 会话全程健康**（`ws=open` / `phase=playing`），网关不发任何 error/close，屏幕全亮、保活正常。

**根因链**

1. 实测三次独立复现，延迟分别是 **4m19s / 4m18s / 4m18s** —— 精度 ±1s，像服务端定时器，不像网络抖动。
2. 音视频**同时**停，且停在整包数（音频 11339 包 ÷ 50/s ≈ 227s）。
3. 停推后 `rx` 冻结、`frames` 不涨，但 `PLI sent` 每 2s 照发、`op=input` 回显正常、`keep #N` 20s 一次 —— 会话层完全没坏。
4. 官方 SDK（`.tmp/cgsdk/cg-sdk.891daeae.js`）里有 `RtcKeepPlaying.CmdId=20015`（字段 1 = `gameToken`）和 `RtcKeepaliveCfg.CmdId=20021`（字段 1 `kickoutDuration` ms / 字段 2 `tipsDuration` ms），`keepAlive.current` 默认 5 分钟 → `kickoutDuration=300000` / `tipsDuration=20000`。时间账：`300s − 42s（冷启动耗时估算）≈ 258s ≈ 4m18s`。
5. `PenCG` 里 `grep -rn 'keepPlaying|KeepPlaying|gameToken' src/` **零命中** —— 这两个命令我们**从来没有发过**。所以最强假设是：服务端「无操作 5 分钟踢下线」的定时器把我们当成挂机了。

**为什么没修**

- `gameToken` 的来源**至今未定位**（不在我们现有的 `applyTicket` / `offer` / WS 握手字段里）。
- 我们的 WS 是 **JSON + offset 编码**，官方是 **8 字节二进制头 + protobuf**；通道能否复用、复用后服务端认不认，**完全未验证**。
- 服务端侧的证据只有时间相关性，**没有网关日志或抓包**能证明是它踢的。

**当前只有治标方案：JS 侧 `STALLGUARD` 三级自愈**（`PenCG/docs/BUGFIX-bug1-stallguard.md`）

| 常量 | 值 | 位置 |
|---|---|---|
| `STALL_RX_MS` | 20000 | `PenCG/src/pages/play/play.vue:206` |
| `STALL_L1_WAIT_MS` | 5000 | `PenCG/src/pages/play/play.vue:209` |
| `STALL_MAX_TRIES` | 3 | `PenCG/src/pages/play/play.vue:214` |

- L1：`requestKeyFrameBurst(2, 500)` 补发关键帧（`PenCG/src/services/media.js:530`，`clearKfBurst()` 在 `:523`，`stopPlay()` 第一行 `:595`）
- L2：`onReconnect()`（黑屏 1~3s）
- L3：`onReapply()`（重新申请机位）
- 放弃：`this.fail('画面已停止（网关停止推流），自动恢复失败，请点「重连」', 0)`
- 判据刻意用 `rx`（`stats.rtp_rx`）而不是 `rxPkts` —— `rxPkts` 含 STUN/DTLS/RTCP，停推后 STUN 照样涨，用它永远判不出来（真机验证过）
- 代码：`checkStall()`（`PenCG/src/pages/play/play.vue:1409`）、`stallReset()`（`:1384`）、`stallHealGiveUp()`（`:1578`）、`checkFreeze()`（`:1603`）

**STALLGUARD 的验证状态：✅ 已真机验证（2026-11-13 18:34–18:35，终末地局，第 2 级自愈成功）。**

真机原文（`[pencg]` 通道，日志 `/userdata/applog/loli_s6_dev.txt`）：
```
18:34:51 [play] probe stage=live ... rx=29124 fps=0 silent=9s
18:35:02 [play] stall detected: rx=29124 已 20s 未增长 → 尝试自愈（第 1 次：补发关键帧）
18:35:07 [play] stall: rx=29124 已 25s 未增长 → L2 复用网关重连
18:35:10 [play] stall: 媒体会话重启，rx 重新计数（29124 → 0），已用自愈 2 次
18:35:12 [play] stall recovered: rx=230 已恢复（第 2 级自愈成功，停滞 5s）
```
配套的 native 重建链也完整走通：`start #2 -> 125.39.94.177:38905 ufrag=...` → `udp bound` → `STUN Binding Success mapped=27.38.185.126:15709` → `ICE connected` → DTLS 7 帧 → `DTLS OK version=DTLSv1.2 cipher=ECDHE-RSA-CHACHA20-POLY1305 srtp=SRTP_AES128_CM_SHA1_80` → `srtp ready` → `kmssink rect` → `video pipeline PLAYING` → `#2 first video frame pushed`。
**这是三级自愈机制的第一条真机实证**（L1 补关键帧 → L2 重连 → 成功），不过该局随后仍遇到 plane 问题（见 bug6），画面层面未由用户确认恢复。
`BUGFIX-bug1-stallguard.md` §6.4 自己列了 5 条未验证项（`kickoutDuration=300s` 无网关实证、42s 是估算、L2 后是否重新计时未知、`onReapply` 机位能否拿回未知、20s 是否误触未测长稳）。

**一条重要的反证**：2026-11-13 17:57 在家里稳定网络下起播 **7 分钟，bug1 没有复现**。这可能说明它与网络条件相关（高铁/弱网才触发），也可能只是样本不足 —— **无法定论**。

**后续研究把这条路判死了（`PenCG/docs/BUG1-KEEPALIVE-PROTOCOL.md`，551 行，纯研究未改代码）**

上面第 4、5 条给出的「最强假设」指向官方 `RtcKeepPlaying(20015)` / `RtcKeepaliveCfg(20021)`，但专项研究对四个问题（Q1–Q4）的结论是：

| 问题 | 结论 | 一句话依据 |
|---|---|---|
| Q1 `gameToken` 在哪 | **拿不到（死路）** | 它只在 `POST /dispatcher/api/paasDispatch` 响应的 `finish_result.sdk_param`（`proto.paas.SdkStartGameParams` 字段 4）里；我们走的是 `api/v2/tickets`，响应里没有这个字段，而 `paasDispatch` 的请求体要靠官方 web 包内嵌的 `appEnv.appKey` 签名 |
| Q2 我们的 JSON WS 能否承载 20015/20021 | **不能（死路）** | 官方走 `sendProxy` = channel 2 = `<gameServerWssUrl>/rtc-sdk` 的 protobuf 帧（`'Eg'` magic + cmdId + len + `0x89 0xAB`）；我们连的是 `gateway_url` = `wss://.../ws`。同一台 gw 上的**两个不同端点、两套不同帧格式**，我们的 `cmd` 文本表里也没有等价物 |
| Q3 退路 | **可行：提前主动重建**（复用 `gatewayUrl`，不换机位） | `startPlay()` 只清 JS 侧状态并重开 WS，**不碰 ticket/机位**；真正重新申请机位的只有 `applySeat()`。代价是重建期黑屏（实测值未知） |
| Q4 零代码实验 | **可做且必须做** | 实验目的要改：2026-11-13 两局现场都没复现（17:58 那局跑了 ≥21 分 50 秒），所以第一件事是确认「4m18s 现在还发生吗」 |

同一份研究还指出一个**必须纳入判断的新事实**：三次复现的现场采集（`.tmp/bug1-soak.log`，11-12 22:46:30–22:58:22）发生在 **RTCP RR 合入之前**（那份日志里 `RR sent` 计数为 0）；RR 合入后的 11-13 两局（17:58:04→18:25:15 `rx` 从 250 单调涨到 131099 `fps=29`、18:47:20→18:50:43 `rx=20359 fps=19`）跨越 4m18s / 8m / 16m 三个窗口**均无冻结**。⇒ **「RR 是我们自己发的」这件事可能与 bug1 不复发有关，但样本极少，不能当结论。**

所以 bug1-B 的当前态势是：**根因假设指向的修复路径已被证明走不通**（`gameToken` 拿不到、通道接不上），可用的只有 Q3 的「提前主动重建」和已写好的 `STALLGUARD` 治标，**两者都还没在真机上验证过能否救回画面**。

**二期补充：弱网「出画冻结」判据（⚠️ 未验证）**

上面三期改动之后发现还有另一种形态：**`rx` 一直在涨，但 `fps` 恒 0**（包进来了，画面解不出来）。一段判据解决不了这个问题，所以另加了 `checkFreeze()`（`PenCG/src/pages/play/play.vue:1603`，常量 `FREEZE_GATE_MS 12000` / `FREEZE_L1_WAIT_MS 9000` / `FREEZE_MIN_SAMPLES 3`，分别在 `:246/:247/:248`）：连续 12s `fps=0` 且期间 `gateDrop+auDrop` 或 `rx` 有增长 → 补发 3 个关键帧（间隔 400ms）→ 9s 未恢复升 L2。`BUGFIX-bug1-freeze-verify.md`（293 行）写好了完整的真机验证方案，但**状态是「方案（未执行）」**：设备本身**造不出人为丢包**（`tc` 是 BusyBox 版无 netem、无 `iptables`、无 `tcpdump`），得靠路由器 netem / PC 热点 + clumsy 在设备外造场景，而且**必须先跑正对照**（把 `FREEZE_GATE_MS` 临时压到 3000 的调试构建）才分得清「场景没造出来」和「判据是死的」。

---

### 2.3 bug2：登录后不刷新 —— ✅ 已真机验证【现场】

**用户原话**

> 登录界面登录成功后跳转回去显示"未登录"，只有点击左侧边栏首页再点回来才能加载出用户信息

**根因链（三个断点，断点 2 是根因）**

1. **断点 1**：`setPendingTab('mine')` 只写模块级变量，登录页和 index 页是两份独立 QuickJS 上下文 → index 页的 `takeTabParam()` 读不到（`PenCG/HANDOFF.md:220-222` 已实证跨页不共享模块状态）。
2. **断点 2（根因）**：`PenCG/src/components/mine-body.vue` 的 `refresh()` 只调 `reloadLocal()` → `PenCG/src/services/auth.js:134 currentSession()` → `cg.js:152 session()`，而 `session()` 读的是**本页模块级常量 `G`**（`cg.js:136`）。登录页 `applyLogin()`（`cg.js:156`）灌的是**登录页那份 `G`**。所以 index 页读到的永远是空 → `logged=false` → `mine-body.vue` 的 `v-else-if="!logged"` 显示「还没有登录网易云游戏」。
3. **断点 3**：`closePageByName` 之后 `onShow` 不一定被调用（`PenCG/src/base-page.js` 转发，本固件无实证）。

盘上的 `pencg_session` 是**完整有效**的 —— 不是没存下来，是界面没重读。

**修法（file:line）**

| 文件 | 改动 |
|---|---|
| `PenCG/src/services/auth.js` | 新增跨页会话版本号：`SEQ_KEY='__pencgSessionSeq'`（`:30`）/ `SEQ_STORE_KEY='__pencg_session_seq'`（`:31`）/ `sessionVersion()`（`:57` 同步，只读 `$falcon`+本页）/ `sessionVersionAsync()`（`:71` 异步，**额外读盘取 max**）/ `bumpSessionVersion()`（`:83` 三处写：`$falcon`、storage、本页变量，并有本页串行化链 `:33`）。三个 bump 调用点：密码登录 `:175`、短信登录 `:197`、`logout()` `:214`。**140 行改动** |
| `PenCG/src/services/nav.js` | `TAB_KEY='__pencgPendingTab'`（`:34`），`setPendingTab()`（`:46`）双写 `$falcon`，`takeTabParam()`（`:59`）优先读 `$falcon` 再回落模块变量。**59 行改动** |
| `PenCG/src/components/mine-body.vue` | `refresh()` 在 `reloadLocal()` **之前**补 `await restoreSession()`（`:515`，顺序是硬要求 —— `:512` 的注释写明「restoreSession() 必须在 reloadLocal() 之前 await 完成」）；`startVersionWatch()`（`:373`）/ `stopVersionWatch()`（`:416`）2.5s 轮询 `sessionVersionAsync()`，`mounted()` `:326` 启、`beforeDestroy()` `:329` 停；日志 `[mine] refresh logged=...`（`:527`）/ `[mine] session 版本变化 ...`（`:397`）。**204 行改动** |
| `PenCG/src/pages/index/index.vue` | `onShow` 末尾调 `this.checkSeq()`（`:230`）；误跳保护 —— 只在本页登录态由 false→true 翻转时才 `self.setTab('mine')`（`:288`），并用 `isLoggedIn()` 前后翻转做二次确认；`startSeqWatch()`（`:321`）/ `stopSeqWatch()`（`:334`）轮询通道受 `seqReady` 门控；`boot()` 里 `sessionSeq = await sessionVersionAsync()`（`:356`）取基线，`sessionSeq: 0` 声明在 `:167`。**194 行改动** |
| `PenCG/src/pages/mine/mine.vue` | 删掉一个**重复的空 `onShow() {}`**（同名 key 后者覆盖前者，让真正的 onShow 逻辑永远不执行）。**-3/+1** |

四道防线（任一成立即可刷新）：① `nav` 的 pending tab ② `index` 的 `onShow` → `checkSeq` ③ `index` 的 2.5s 轮询 ④ `mine-body` 的 2.5s 轮询。③④ 只依赖 `setTimeout` + 读盘，不依赖宿主回调。

**验证状态：✅ 已真机验证（2026-11-13 18:29，第 2 轮 PASS）。**

真机路径用的是用户真实操作路径：`DEV_AUTOSTART = { page:"login", delay:10000, tab:"mine" }` （先 `setTab("mine")` 再跳 login，复现「在『我的』栏目里点登录」）+ `DEV_LOGOUT`（走生产 `logout()` 退回未登录）+ `DEV_AUTOLOGIN`（填表后调**生产** `submit()`）。
关键日志（`/userdata/applog/loli_s6_dev.txt` 原文）：
```
18:29:27.481 [dev] autologout 完成（盘上 pencg_session 已删）
18:29:35.089 [mine] boot logged=false
18:29:38.647 [mine] 版本号已到 3 但会话为空，补试第 1 次      ← retryUnlogged 生效
18:29:38.656 [auth] 密码登录成功 uid=67d9433bc649c953187c4ee8
18:29:39.037 [mine] refresh logged=true booted=true           ← ★修复判据命中
18:29:39.454 [mine] @me ok free=0 vip=true
18:29:41.158 [mine] session 版本变化 3 -> 4，重载              ← 版本监听生效
18:29:41.162 [index] 会话版本 3 -> 4 本地登录态 true -> true 会话年龄 2544ms
```
**截图视觉确认**（`.tmp/b3.png`，960x266）：左栏「我的」高亮，页面显示「已登录 · 用户aaabOhXYY」+ VIP 标识 + 「ID 67d****ee8」+「186****8726」+「免费时长 0 分钟 / 会员畅玩中」+「会员权益 VIP 会员 · 11月14日到期」+「游玩历史 3 条」+ 三张历史卡片。**全程未点屏幕任何位置**，即「登录成功后不用再点首页再点回来」这条症状已消除。
> 第 1 轮（不切 tab、delay 9000）里 `[mine]` 一条日志都没有、截图停在首页推荐栏目 —— 因为 autostart 没先切到 mine 栏目、`[mine] refresh` 根本没被触发。**它反过来证明第 2 轮的 `tab:"mine"` 才是用户真实路径，必须用它复现。**

**下次验证时应看到的日志链**（**应**出现，不是**已**出现）：

```
[2026-..-.. ..:..:..][E][JS] [pencg][I] [auth] 密码登录成功 uid=...
[2026-..-.. ..:..:..][E][JS] [pencg][I] [nav] closePageByName login
[2026-..-.. ..:..:..][E][JS] [pencg][I] [index] 会话版本 1 -> 2 本地登录态 false -> true
[2026-..-.. ..:..:..][E][JS] [pencg][I] [mine] refresh logged=true booted=true      ← ★核心判据
[2026-..-.. ..:..:..][E][JS] [pencg][I] [mine] @me ok free=... vip=...
```

**防回归门槛**：冷启动（已登录状态下直接起 App）**不应**出现「会话版本 0 -> N，本地登录态 false -> true」—— 出现就是误跳。

---

### 2.4 bug3 / bug4：马赛克残留 + 绿屏 —— ✅ 已真机验证【现场】

> **定案（2026-11-13 20:30 之后）**：真根因是 **`kmssink` 的 `sync=true`**，已改为 `sync=false`（`PenCG/native/csrc/cgmedia/cm_gst.c:253-255` 的 `KMS_SINK_PROPS`，`:220-252` 是防止改回去的长注释）。**两个游戏都由用户目视确认画面正常**。此前本文断言的「RTP 层 + 出画层」根因链**不是**这两个 bug 的根因，那批改动是有效的健壮性加固，但**没有**解决用户看到的现象 —— 详见下面的「§2.4.0 定案」与「被推翻的推断」。

**用户原话**

> 3：「用"原神"测试，进入游戏后游玩画面经常性出现上一帧的马赛克残留」
> 4：「用"明日方舟终末地"测试，建立连接后画面就存在马赛克和绿屏」

#### 2.4.0 定案：`kmssink sync=true`

**根因**

`appsrc` 的配置是 `format=time do-timestamp=true`，由 appsrc 在**到达时刻**给每帧打一个绝对单调时钟戳；`kmssink sync=true` 则让 sink 按 buffer 的 PTS 决定**何时显示**。这套「到达时刻时间戳」与 kmssink 的 running-time 基准一旦错位（首次缓冲、重连、或片头 I 帧被丢弃导致 PTS 跳变），后续 buffer 就被判定成「还没到显示时刻」或「已过期」而**整体丢弃** —— 表现正是**平面绑着却一口不更新**：KMS 上永远显示那一帧，用户看到的就是「上一帧残留」和「画面卡死」。

**定位过程（四层独立验证，每层都有真机证据，所以结论收敛）**

| 层 | 做法 | 结果 |
|---|---|---|
| ① 码流层 | 把 App 实际推给 `appsrc` 的字节 dump 成 `/userdisk/miniapp/data/au.h264`（615675 B / 300 帧），拉回本地用 ffmpeg 解 | 解出 **233 帧**；`corrupt decoded frame` 仅 3 次且**都在片头 I 帧**；`concealing` 11 行全是 I 帧错误掩盖；抽帧 frame 200 / 232 是**完整清晰的终末地画面**（灰度均值 182.9 / 184.1，非零像素 89%） |
| ② 硬解层 | `gst-launch-1.0 filesrc ! h264parse ! mppvideodec ! videoconvert ! videoscale ! video/x-raw,format=GRAY8,width=64,height=64 ! filesink` | 输出 954368 B = **233 × 4096**，与软解帧数**完全一致** |
| ③ plane76 硬件通道 | `videotestsrc pattern=smpte is-live=true ! … NV12 ! kmssink plane-id=76 driver-name=rockchip force-aspect-ratio=false can-scale=true sync=false` | 全屏彩条，**用户肉眼确认可见** |
| ④ 同一管线换数据源 | `multifilesrc location=au.h264 loop=true ! h264parse ! mppvideodec rotation=270 ! videoconvert ! kmssink plane-id=76 … sync=true render-rectangle="<107,244,266,473>"` | 用户肉眼确认「正中间画面是终末地，一开始全绿过一会变正常」 |

⇒ **管线、rotation、缩放、plane 绑定全都对，唯一变量是喂数据的元件（`appsrc` vs `multifilesrc`）**，因此问题只可能出在 appsrc 产生的时间戳与 sink 的调度方式之间。

**真机证据（两组对照，同一个游戏）【现场】**

| 配置 | 日志 | DRM 平面采样 | 用户原话 |
|---|---|---|---|
| `sync=true` | `/userdata/applog/DictPen_20261113_202233.log` | `plane[76] crtc=video_port0 fb=130`，**10 次 × 2 秒采样 fb 恒 130，一次不变**；同期 `AU push count=77`、probe `rx=6578 fps=13` | 「有画面但完全静止（卡住的一帧）」 |
| `sync=false` | `/userdata/applog/DictPen_20261113_202711.log` | `fb` **20 次 × 1 秒采样在 127/130 之间持续交替** | 「画面在动了！能正常玩」 |
| `sync=false`，原神回归 | `/userdata/applog/DictPen_20261113_203218.log`（`[play] boot code=yssy q=low gw=0` / `出画 1280x720 audio=0`） | `fb` 12 次采样 127/130 交替 | 「画面正常、在动」 |

注意 `sync=true` 那一组里 **`rx=6578` / `fps=13` / `AU push count=77` 全是正常的** —— 收包、解码、推帧都在动，只有 sink 不翻页。这解释了为什么之前所有「在出帧」的间接判据都显示健康，用户却看到静止画面。

**与用户报的两个症状的对应**

- **bug4「建立连接后画面就存在马赛克和绿屏」**：绿屏是**片头 IDR 之前的解码器输出**（DPB 空、无参考帧时的未初始化 NV12），IDR 到达后会自愈 —— **这是正常行为，不是 bug**。真正「一直全黑 / 不动」才是 `sync=true`。
- **bug3「游玩画面经常性出现上一帧的马赛克残留」**：**同一个根因** —— sink 卡在某帧上不翻页，用户看到的就是上一帧残留。

#### 2.4.1 被推翻的推断（保留是为了让后来者别按错方向排查）

下面的推断**都曾写进本文和 `BUGFIX-bug3-bug4-video.md`**，现在已被 AU dump + ffmpeg 全链路验证**推翻**，不要照着排查：

| 曾经的推断 | 现状 |
|---|---|
| 「`IDR without SPSPPS` / 参数集缺失导致绿屏」 | ❌ **推翻**。dump 出来的码流用 ffmpeg 能正常解出 233 帧完整画面，参数集齐全 |
| 「码流层有损坏（SRTP 认证失败、AU 缺片）导致马赛克」 | ❌ **不是本次根因**。`corrupt decoded frame` 只有片头 I 帧 3 次，后续 200+ 帧完整。AU 完整性闸门仍是有效的健壮性加固，但它救不了「平面不翻页」 |
| 「`mppvideodec ignore-error=true` 把坏帧吐出去上屏」 | ❌ **不是本次根因**。硬解输出帧数与软解完全一致（954368 = 233×4096）。`ignore-error=false max-errors=16` 保留为加固 |
| 「DPB / 输出 buffer 池按首个 SPS 分配，分辨率切换不重协商」 | ❌ **不是本次根因**。multifilesrc 同管线同分辨率也复现，改 sync 即好。hunk 3 的 flush 重协商保留为加固 |
| 「`gst_buffer` PTS 用 RTP 的时间轴才能让 `sync=true` 正确调度」 | ⚠️ **方向相反**。真正的做法是**取消 sink 的时钟依赖（`sync=false`）**，而不是给 buffer 补更准的时间戳。补 PTS 那条改动已确认符号不存在、已跳过 |

#### 2.4.2 仍保留的改动（不是根因，但是有效加固）

RTP 层（`cm_rtp.c`）与出画层（`cm_gst.c`）的这一批改动**没有回退**，它们解决的是另一类问题（丢包下的坏 AU、错误掩盖、分辨率切换），与 `sync=false` 不冲突：

| 文件 | 改动 |
|---|---|
| `PenCG/native/csrc/cgmedia/cm_rtp.c` | **+1101 行**。AU 完整性闸门 + 缺口表（`gap_add` / `gap_purge_expired` / `gap_take` / `gap_upgrade_since`）、`cache_param_set()`、`au_seg_add()` / `au_mark_missing()`、`flush_frame()`、`rtp_reset_au()` / `rtp_reset_all()` / `cm_rtp_reset()`、`cm_rtp_flush_held()`、`cm_rtp_note_stale()`、`kf_request()` |
| `PenCG/native/csrc/cgmedia/cm_rtp.c:789` | 「等 IDR」闸门日志 `#%d waiting for first IDR, dropping P frames` |
| `PenCG/native/csrc/cgmedia/cm_rtp.c:816` | `#%d IDR without %s%s, injected cached parameter sets`（旧 SPS/PPS 缓存注入） |
| `PenCG/native/csrc/cgmedia/cm_rtp.c:586` | `#%d video gap (%s) -> request IDR (PLI #%llu)` |
| `PenCG/native/csrc/cgmedia/cm_rtp.c:617` | `#%d SPS reports %dx%d profile=%d level=%d%s` |
| `PenCG/native/csrc/cgmedia/cm_rtp.c:868` | `#%d first video frame pushed (%d bytes, %dx%d, key=%d)` |
| `PenCG/native/csrc/cgmedia/cm_rtp.c:80` | `IDR_STARVE_RELAX_MS 4000`（弱网兜底，见 §1 附表「弱网 IDR 饥饿」一行与 §4 第 4 条） |
| `PenCG/native/csrc/cgmedia/cm_rtp.c:86-97` | NACK 参数：`NACK_GAP_MAX 256` / `NACK_ROUNDS_P 3` / `NACK_ROUNDS_IDR 5` / `NACK_GAP_TTL_MS 300` / `NACK_PRIO_IDR 2` / `NACK_PRIO_P 1` |
| `PenCG/native/csrc/cgmedia/cm_gst.c:253-255` | **★ 本次真正修复**：`KMS_SINK_PROPS` 里 `sync=false`（原 `sync=true`） |
| `PenCG/native/csrc/cgmedia/cm_gst.c:249` | `h264parse ! mppvideodec rotation=270 ignore-error=false max-errors=16`（软解分支 `avdec_h264` 未改） |
| `PenCG/native/csrc/cgmedia/cm_gst.c:81-105` | 给非关键帧打 `DELTA_UNIT`，**值写死 `(1u << 13)`** |
| `PenCG/native/csrc/cgmedia/cm_gst.c:1331` | `cm_box_set_caps_video()` —— 分辨率变化时推 `flush-start` / `flush-stop(reset_time=TRUE)`（`:1387` / `:1389-1392`）强制重协商 |
| `PenCG/native/csrc/cgmedia/cm_media.c:98/105` | `still no IDR (...)` / `packet loss seen (...)` 两行健康日志；`emit_stat` 的 stat JSON 加 `lost/auDrop/gateDrop/kfReq/dup/reorder/spsInject/rtcpIn`（`:115-121`） |

**两个必须记住的取值陷阱**（`BUGFIX-bug3-bug4-video.md` 里写错了，以 `BUGFIX-bug3-bug4-gst.md` 与源码为准；裁决见 §4.1）：

- `GST_BUFFER_FLAG_DELTA_UNIT` 是 **`1u << 13`**（`GST_MINI_OBJECT_FLAG_LAST = 1<<4`，`DELTA_UNIT = FLAG_LAST << 9`）。`1u << 4` 是 **`LIVE`**，语义完全相反。源码 `cm_gst.c:105` 已是 `1u << 13`。
- `gst_buffer_get_pts` / `gst_buffer_set_pts` **在真机 `libgstreamer-1.0.so.0`（1.22.0）和本地桩库里都不存在**（逐名解析 `.dynsym` 确认）。PTS/DTS 那条**已明确跳过**，不是漏做。

#### 2.4.3 代价与例外

`sync=false` 让 kmssink **不再按 PTS 节流**，而是「有空就显示最新一帧」。对本机场景（云游戏实时流、拒绝累积延迟、720p 单路）这是**语义正确**的选择；丢帧由 RTP 层的 IDR / NACK 逻辑负责，不依赖 sink 的时钟。

**例外**：将来若要接**本地文件播放**（需要严格按时间轴出帧），应当**另建一条 `sync=true` 的管线**，**不要**把 `KMS_SINK_PROPS` 改回去 —— 那会把 bug3/bug4 重新引入。这条约束已写进 `cm_gst.c:249-252` 的注释。

#### 2.4.4 历史遗留的一组「在出帧但不能证明画质」的判据

以下判据在定案前是唯一的间接手段，现在**只用于回归**，不再用于判「画质」：

- 抓不到视频平面像素（这正是当时判断不了的原因）：`miniapp_cli capture` 只抓 UI 层（KMS 平面 zpos=2 在 UI 之上）、`captureFB` 返回 `{"ret": -1}`、`ffmpeg -f kmsgrab` 超时 `rc=124`（需 `CAP_SYS_ADMIN`）、设备无 `/dev/fb*` / `screencap` / `screenshot` / `fbgrab` / `grim`。
- 能被 `sync=false` 一并救活的判据：DRM `buf[0].addr` / `fb` 编号是否交替 —— **`fb` 恒不变 = sink 不翻页**，这是以后最快的回归指纹。
- 历史日志里能复查的同类行：`.tmp/e2e/dev.log`（2026-11-07，旧包，1600x900；`SPS reports 1600x900` 出现 19 次、`first video frame pushed (31446 bytes, 1600x900, key=1)` 1 次）、`.tmp/bug1-soak.log`（`packet loss seen (lost=4 auDrop=3 dup=0 reorder=0)`；注意这行**只在 `bug1-soak.log` 里**，`e2e/dev.log` 里 0 条，引用时别混）。

---

### 2.5 bug5：音量 0 无效 + 退出后其他应用有声 —— ✅ 已真机验证【现场】

**用户原话**

> 出现下拉系统控制栏，音量调为0时，进入游戏后还是有声音，并且在退出后，使用其他软件包括有道查词也会出现声音

**根因（两半是同一个 bug）**

1. 系统音量 0 = card1 `numid=54` = `0,0`（`/bin/hal-audio -l` 里 `speaker_vol_ctl_numid=54` 写死；`/etc/asound.conf` 的 `spk_softvol` 绑在 card1）。旧代码 `audio_amp_volume_ensure()` 看到 `0,0` 就 `cset 200,200` —— **替用户把音量拖到 200**，这就是「现象 1：调 0 还有声音」。
2. 旧代码把 `Playback Path` 从 OFF 拨到 SPK 后**从不还原**，功放抬到 200 也从不还原 —— 这就是「现象 2：退出后别的软件也有声」。
3. `aplay` 收尾是 SIGTERM 紧跟 SIGKILL（无间隔），走不到正常 flush，且**没有任何「aplay 真的死了」的自证**。

**修法（file:line）**

| 文件 | 改动 |
|---|---|
| `PenCG/native/csrc/cgmedia/cm_gst.c` | **整段删除** `audio_amp_volume_ensure()`（card1 `numid=54` 从此只读，一个字节都不写）；新增 `audio_amp_read()`（`:399`，匹配 `": values="` 而**不是** `values=`，否则会把声道数当音量）、`audio_snapshot()`（`:433`，幂等）、`cm_audio_route_restore()`（`:466`，导出）、`audio_route_ensure(cm_sess *s)`（`:504`，先快照再按 OFF 分支拨 SPK，**回读确认**才记 `audio_path_wrote`）、`aplay_alive()`（`:903`，`kill(pid,0)` 探针）、`aplay_stop()`（`:921`）四阶段（EOF → SIGTERM → SIGKILL → UNKILLABLE，自证日志在 `:939` / `:950`） |
| `PenCG/native/csrc/cgmedia/cm_gst.c:1096` | `aplay_spawn` 失败分支补 `cm_audio_route_restore(s)`（盒子还没交出去，`cm_box_destroy` 不会被调用） |
| `PenCG/native/csrc/cgmedia/cm_gst.c:1171` | `cm_box_destroy()` 末尾 `if (!b->is_video) cm_audio_route_restore(b->sess);`（四条收尾路径统一到这一处） |
| `PenCG/native/csrc/cgmedia/cm_session.h` | `struct cm_sess` 新增 5 个快照字段：`audio_snap_done` / `audio_path_snap` / `audio_path_wrote` / `audio_amp_snap` / `audio_amp_after`；`:405` 声明 `void cm_audio_route_restore(cm_sess *s);` |
| `PenCG/native/csrc/cgmedia/cm_cmd.c:538` | `setAudio {on:0}` 分支在 `cm_box_destroy` + `s->abox = NULL` 之后补一次 `cm_audio_route_restore(s)` 幂等兜底（覆盖「盒子早就没了但快照标记还挂着」的边角） |

放会话上而不是盒子上的理由：盒子可能创建失败、也可能先于会话结束被重建（`setAudio(0)` / `setVideoVisible`），而还原必须在任何一条拆管线路径上幂等跑一次。

**验证证据原文【现场】**

来源：真机日志原文，设备 `9D91500007505171`，native 日志文件 **`/userdata/applog/DictPen_20261113_070039.log`**（取最新一份：`ls -t /userdata/applog/DictPen_*.log | head -1`）。
取样命令：`grep -a 'audio snapshot\|audio restore\|aplay pid' /userdata/applog/DictPen_20261113_070039.log`

原文（逐字，含前缀；共两轮退出）：

```
[cgmedia] aplay pid=7058 exited on SIGTERM after 220 ms
[cgmedia] #1 audio restore: Playback Path SPK -> OFF (back to OFF)
[cgmedia] #1 audio restore: amp(card1#54) 0,0 unchanged (we never write it)
[cgmedia] #1 audio snapshot: Playback Path=OFF amp(card1#54)=0,0
[cgmedia] aplay pid=20297 exited on EOF after 110 ms
[cgmedia] #1 audio restore: Playback Path SPK -> OFF (back to OFF)
[cgmedia] #1 audio restore: amp(card1#54) 0,0 unchanged (we never write it)
[cgmedia] #1 audio snapshot: Playback Path=OFF amp(card1#54)=0,0
```

⚠️ **上面是按时间戳顺序抓到的字面顺序，不是因果顺序。** `audio snapshot` 是**进游戏时**打的（`audio_route_ensure()` 在 `cm_gst.c:507`），`audio restore` 是**退出时**打的（`cm_audio_route_restore()` 在 `cm_gst.c:466`）。按因果重排后每一轮都是：

```
进游戏  audio snapshot: Playback Path=OFF amp(card1#54)=0,0      ← 记下原始状态
播放中  （我们只拨 Playback Path OFF→SPK，从不碰功放）
退出    aplay pid=NNNN exited on <EOF|SIGTERM> after <ms> ms     ← aplay 收尾自证
退出    audio restore: Playback Path SPK -> OFF (back to OFF)     ← 路由还原
退出    audio restore: amp(card1#54) X unchanged (we never write it)  ← 功放自证未写
```

两轮各自覆盖了 `aplay_stop()` 四阶段收尾里的**两条不同分支**，正好证明两条路径都真的走过：

| 记录 | 收尾分支 | 原文 |
|---|---|---|
| `pid=7058` | **SIGTERM 路径**（等不到 EOF，主动发信号，220 ms 内退出） | `aplay pid=7058 exited on SIGTERM after 220 ms` |
| `pid=20297` | **EOF 自然退出路径**（文件放完自己退，110 ms） | `aplay pid=20297 exited on EOF after 110 ms` |

> 上表**不声明这两轮的先后顺序**：grep 输出的顺序不一定是日志里的时间顺序（同一段输出里 `snapshot`/`restore` 已经被打乱过一次），要判断先后请看每行前面真正的时间戳。表里只主张「这两条分支都真实发生过」。

配套 mixer 实测（同一台设备 `9D91500007505171`）：

| 时刻 | `amixer -c 1 cget numid=54`（功放） | `amixer -c 0 cget numid=45`（Playback Path） | `pidof aplay` |
|---|---|---|---|
| 播放中 | `values=176,176`（50%，**我们不动它**） | `values=2` = SPK | `29621` |
| 退出后 | `values=176,176`（**一字未改**） | `values=0` = OFF（已还原） | **空** |

退出后 `card0 numid=51`（Master）仍是 `225,225`，**未被改动**。

> 数值说明（不要拿绝对值当判据）：上面日志两轮的 amp 快照是 `0,0`，而 mixer 抽查读到 `176,176` —— 说明这两次取样不是同一轮播放，功放值只反映**用户当时的系统音量**，与修复无关。判据只看两点：①退出前后功放值**是否发生变化**（`unchanged` 才是对的）；②`Playback Path` 退出后是否回到 `OFF`。绝对值是多少都不影响结论。

**必须为「无」的两行**（两份日志 `grep -a` 均 **0 命中**）：

- **无** `[cgmedia] #1 audio amp volume 0 -> 200` —— 这行是旧包的指纹，出现即说明新库没被加载
- **无** `[cgmedia] #1 aplay pid=xxxx UNKILLABLE after 1000 ms (stuck in driver?)`

这一组证据把整条链**双向**闭合了：正向有 snapshot → SPK → aplay 两条分支都干净退出 → restore 回 OFF → amp `unchanged`；反向 `audio amp volume` 与 `UNKILLABLE` 两个旧指纹**一个都没出现**。功放从头到尾没被写过，是这份修复最核心的主张，现在有正反两面证据。

---

### 2.6 bug6：页面被销毁后整屏黑、weston UI 平面被连带解绑 —— ✅ 已真机验证【现场】

> 用户 22:07 追加反馈（原话）：「现在游玩终末地没画面，不是黑屏」。追问后进一步澄清
> （原话）：「屏幕中间是动的终末地界面，但是没法触摸，左右两边是空白的黑色」。
> ⇒ **视频在动、UI 层丢了、触摸失效**，与 bug3/bug4 的「视频不动」是两回事。

**根因（两条，都在 native）**

1. **`restore-crtc` 语义被误解**。`PenCG/native/csrc/cgmedia/cm_gst.c` 的 `KMS_SINK_PROPS` 没显式设
   `restore-crtc`，走 kmssink 默认 **true**，意思是「CRTC 曾被设过新 mode，就在 NULL 时恢复旧 mode」。
   它在管线收尾时会做 `drmModeSetCrtc(crtc, 0, ...)` 这类「不带 fb 的 CRTC 恢复」，
   内核在 disable/restore 一个 crtc 时会把这个 crtc 上**所有** plane 的 mask 清掉 ——
   于是 weston 辛苦挂的 `plane[54]` 也被一起清掉，而 weston 不知道发生了什么、不会主动重挂。
   **我们只申请了 `plane 76`，没有资格动 CRTC 的 mode。**
2. **两路会话抢同一平面**。Falcon 每个页面独立 JS 上下文，两个 play 页各自 `start` 是真实可达的
   （宿主重载 index、或 play 页被销毁时 `beforeDestroy` 里的 `stop` 没跑完）。两路会各建一条
   `kmssink plane-id=76` 管线抢同一个硬件平面，行为未定义。JS 侧看不见彼此，只有 native 能拦。

**修法（file:line）**

| 文件 | 改动 |
|---|---|
| `PenCG/native/csrc/cgmedia/cm_gst.c` | `KMS_SINK_PROPS` 加 **`restore-crtc=false`**：收尾时只 disable 自己那条 plane，不碰 CRTC。属性上方有长注释记录症状/根因/为什么不能动别人的 CRTC（`gst-inspect-1.0 kmssink` 真机核实该属性存在、Default: true） |
| `PenCG/native/csrc/cgmedia/cgmedia.c` | 新增 `sess_kill(id)` 与 `sess_kill_all_video()`；`js_start()` 在 `sess_alloc()` **之前**先收掉表里所有 `cfg.video_pt != 0` 的旧会话（`pthread_join` → `cm_box_destroy` → `sess_free`），并打 `start: pre-empted %d stale video session(s) to free plane 76`。放在 alloc 之前是因为它会 join，不能让新会话短暂持着空 vbox |

**真机 A/B（设备 9D91500007505171，原神 yssy，日志取 `readlink /proc/$(pidof miniapp)/fd/1`）**

| 阶段 | 观测 |
|---|---|
| 起播健康 | `plane_mask=3`（`plane[54]`+`plane[76]` 都在），`出画 1280x720 audio=0/1` |
| **修复前**：再来一次 `miniapp_cli start ... index` | `plane_mask` 3 → **0**，`plane[54] fb=0`、`plane[76] fb=0`，**整屏黑、触摸失效、只能重启恢复** |
| **修复后**：同样再来一次 `start` | `plane_mask` 稳定 **1**（只剩 weston），`plane[54]` fb 在 **126/128 持续交替**，10 次 × 3s 采样全程如此 |
| 视觉确认 | `miniapp_cli capture` 截到**完整正常的首页 UI**（推荐流 + banner + 侧栏），不再整屏黑 |

**判别同类问题的工具优先级**：判「视频平面在不在翻页」看 kmssink 日志的 `displaying fb N`，
**不要**读 `/sys/kernel/debug/dri/0/state` 的 fb 编号 —— 每次 open/read 都会与
`drmModeSetPlane` 竞争，采到 `fb=0` 是采样副作用（本项目为此误判过一整轮）。

---

## 3. 平台级事实（给下一个接手的人省时间，逐条都可复现）

### 3.1 JS 日志通道

**Falcon 的 JS console 只有 `console.error` 会落盘。** `console.warn` 与 `console.log` **完全不落盘**。

- 落盘文件：`/userdata/applog/loli_s6_dev.txt`
- 行格式：`[2026-11-12 22:15:24.947][E][JS] <正文>`
- **中间的 `[E]` 是通道级别，恒为 E，不代表严重程度**。真正的级别只看行内 `[pencg][I|W|E]` 前缀。
- 反证：全盘 `grep -a -c 'W][JS]' /userdata/applog/loli_s6_dev.txt` 计数为 **0**。
- 代码已改：`PenCG/src/services/log.js:44` 无条件 `console.error(line)`，文件头 `:3-9` 记了本次实测；同类修正还有 `src/base-page.js:88/94`、`src/services/im.js:18`、`src/pages/diag/diag.vue:67/76/83/639`。
- **典型误判**：日志一条都没有 ≠ 代码没跑，往往是日志通道用错了。本轮已经踩过一次（`BUGFIX-bug3-bug4-video.md` §一 的「命中数为 0」就是这个坑）。

### 3.2 native 日志通道

- 前缀 **`[cgmedia]`**，来自 `cm_log()`（`PenCG/native/csrc/cgmedia/cm_util.c:19-29`：`fprintf(stdout, "[cgmedia] ")` + `vfprintf` + `fflush`），默认级别 `g_cm_log_level = 3`。
- 落盘到 **`/userdata/applog/DictPen_*.log`，每次开机换新文件名**。取最新的一份：

  ```bash
  ./env/adb/adb.exe -s 9D91500007505171 shell 'ls -t /userdata/applog/DictPen_*.log | head -1'
  ```

- 真机实证：2026-11-13 取到的是 **`/userdata/applog/DictPen_20261113_070039.log`**，§2.5 的 audio 四条原文就是从这个文件里 `grep -a 'audio snapshot\|audio restore\|aplay pid'` 抓出来的。
- 本地旁证【现场】：`.tmp/soak2.sh` 就是用 `ls -t ... | head -1` + `grep -a cgmedia` 采的，`.tmp/bug1-soak.log` 里 24 行 `[cgmedia]` 全部抓到了。
- JS 与 native 是两条独立通道，**grep 时要分别 grep 两个文件**。
- 关于「落到哪个文件」的旧分歧：`BUGFIX-bug3-bug4-video.md` 步骤 -1 与 `BUGFIX-bug5-audio.md` 步骤 -1 写的「待现场确认」**已过时**，以本节为准（裁决记录见 §4.1 第 3 条）。

### 3.3 设备上不存在的东西

**没有**：`logcat`、`getprop`、`input`、`sendevent`、`uiautomator`、`monkey`、`screencap`、`screenshot`、`tcpdump`。

**有**：`/bin/ping`、`/bin/curl`、`/bin/wget`、`/bin/netstat`、`/bin/busybox`、`ffmpeg`、`gst-launch-1.0`、`gst-inspect-1.0`、`weston-screenshooter`。

所以任何依赖 `logcat` / `input tap` / `getprop` 的教程都不能照抄。

### 3.4 唯一的输入注入方式

```bash
send_event button_name action [x] [y]
# button_name ∈ camera / asr / menu / touch / screen_off / rk8xx_pwrkey
# action      ∈ press / release / slip
```

- **对 play 页按钮只有 `slip X Y` 有效**，`press` + `release` 组合无效。
- **对 `<scroller>` 内的元素（卡片 / banner / 搜索条目）全部无效** —— 需要自动点进去只能靠 `PenCG/src/dev-autostart.js` 里的 `DEV_AUTOSTART` 走代码路径。

### 3.5 启动命令

```bash
# 正确
./env/adb/adb.exe -s 9D91500007505171 shell 'miniapp_cli start 8002026110200003 index'

# 错误：会把 --index 当成页面名去找 b//--index.js.bin，得到一个空页（Elm=0）
./env/adb/adb.exe -s 9D91500007505171 shell 'miniapp_cli start 8002026110200003 --index'
```

**不带 `--`。**

### 3.6 关屏的唯一可靠做法

```bash
# 熄屏（keepAlive 活跃时 hal-screen off 会被 20s 一次的 keep 重新点亮，无效）
echo 4 > /sys/class/backlight/backlight/bl_power

# 点亮
echo 0 > /sys/class/backlight/backlight/bl_power; hal-screen on; hal-screen keep
```

### 3.7 网络连通性检查

```bash
./env/adb/adb.exe -s 9D91500007505171 shell 'ping -c 5 n.cg.163.com'
```

（真机上 `ping` 的 `packet loss` 百分比会出现 `3333%` 这种反常值，是 busybox ping 在非标准退出码下的输出 bug，只能当「有没有丢包」的定性参考。）

### 3.8 装机成功与否不能用 .so 的 md5 / 体积判断

安装器会**重命名**（`libjsapi_cgmedia_<数字>.so`）并**追加一个 section**，同一个库装机前后体积会从 **80704 → 198896**。

判断新库是否真的生效，只能用 `strings` 查新代码里的日志标记串（例如 `audio restore` / `NACK sent` / `RR sent`）。

### 3.9 GStreamer：没有头文件，两个取值陷阱

- 设备上**没有标准 GStreamer 头文件**，`cm_gst.c` 走「手写最小声明」路线。
- `GST_BUFFER_FLAG_DELTA_UNIT` 正确值是 **`1u << 13`**：`GST_MINI_OBJECT_FLAG_LAST = 1<<4`，`DELTA_UNIT = FLAG_LAST << 9`。`1u << 4` 是 **`LIVE`**，写错语义完全相反。源码 `cm_gst.c:105` 已是 `1u << 13`（裁决记录见 §4.1 第 1 条）。
- `gst_buffer_get_pts` / `gst_buffer_set_pts` 等 accessor **在真机 `libgstreamer-1.0.so.0`（1.22.0）和本地桩库里都不存在**。PTS 只能用 `gstbuffer.h` 的结构体字段宏访问，而那个头文件不在工作区。**`BUGFIX-bug3-bug4-video.md:475-480` 建议用这两个 accessor 的做法已作废**（§4.1 第 2 条）。

### 3.10 多 agent 并发上限

`spawn_teammate` 最多 **8 人**，超了返回 `Team member limit 8 reached`。

---

## 4. 未解决项与已知局限

| # | 未解决项 | 现状 | 影响 |
|---|---|---|---|
| 1 | **bug1 传输侧根因**（服务端 4m18s 停推） | 只有假设（`RtcKeepPlaying 20015` / `RtcKeepaliveCfg 20021` 从未发送），`gameToken` 来源未定位、通道能否复用未验证。STALLGUARD 只是治标 | 用户仍会看到「四分钟后停画面」，只是可能被自愈救回来（**自愈本身也未验证**） |
| 2 | ~~bug2 真机未验证~~ | **已于 2026-11-13 18:29 真机验证通过**（见 §2.3） | 关闭 |
| 3 | ~~bug3/bug4 视觉效果未验证~~ **已定案关闭** | 根因是 `kmssink sync=true`，已改 `sync=false`（`cm_gst.c:253-255`）；原神 + 终末地两个游戏**用户目视确认画面正常**。`fb` 采样由「恒 130」变「127/130 交替」构成客观旁证 | 已不再是局限。**遗留的是取证手段**：仍然抓不到视频平面像素（`captureFB` 返回 `-1`、`kmsgrab` 超时、无 `/dev/fb*`），所以回归时只能用 `fb` 是否交替 + 用户目视，见 §2.4.4 |
| 3b | **`cm_gst.c:1355` 的注释已过时** | 该处注释仍写「会动到 `sync=true` 的 kmssink 调度」，但 `KMS_SINK_PROPS` 已改 `sync=false` | 不影响运行，只影响读代码的人。属源码注释，本次不改（写域只有 docs） |
| 4 | **弱网 IDR 饥饿修复未验证** | `cm_rtp.c` 加了 NACK + `IDR_STARVE_RELAX_MS 4000`，已编译已装机 | 高铁/弱网下「rx 涨但 fps=0」是否还会发生 **未知**。原始现场：`gateDrop=15511`、`auDrop=401`、PLI 468 次无效、fps 恒 0 达 12 分钟 |
| 5 | **`JS heartbeat lost` 的静默时长是坏的** | `.tmp/e2e/dev.log` 里实际打出 `#2 JS heartbeat lost (18446744073709551615 ms)` = 2^64−1，是无符号下溢 | 这行日志的「多少 ms 静默」**不可信**（拆管线的动作本身是对的）。未修 |
| 6 | **bug1 保活未在原配置下复验** | 用户已把自动熄屏从 270s 改到 570s/600s/1200s | 「压住 270s」这件事只有 570s 配置下的 9 分 20 秒证据；`900s = system_sleep_wakeup` 那条**完全没测** |
| 7 | **`gst_buffer` PTS/DTS 未做** | 符号不存在、无头文件，明确跳过 | 收包抖动量会被 appsrc 当作帧间隔。与改动前行为一致，**无回归** |
| 8 | **RTCP RR 收到方向为零** | `rtcpIn` 恒 0，网关从不回 RTCP | RR 只能单向使用（我们发、网关收）。是否真的让网关改善行为 **未验证** |
| 9 | ~~文档内的自我矛盾（`DELTA_UNIT` / `get_pts`）~~ **已在原文档修正** | 已**裁决**并**落地修改**：`1u<<13` 为准（源码 `cm_gst.c:105` 已写死），`BUGFIX-bug3-bug4-video.md:482` 的 `1u<<4` 已于 2026-11-13 晚就地改为 `1u<<13` 并加纠错注记；`gst_buffer_get_pts/set_pts` 不存在，`:475-480` 那段已标注作废 | 见 §4.1 第 1、2 条。原文档已改，不会再有第二份照抄错版 |
| 10 | 三份文档对 `[cgmedia]` 落盘去向的说法不一致 | 已**裁决**：落 `/userdata/applog/DictPen_*.log`，每次开机换名，`ls -t /userdata/applog/DictPen_*.log \| head -1` 取最新。`BUGFIX-bug1-freeze-verify.md:281` 是对的，另两份文档的「待现场确认」过时 | 见 §4.1。原文档未改 |
| 11 | **弱网冻结判据（`checkFreeze`）完全没有真机验证** | 代码在 `play.vue:1603`，常量 `:246-248`；验证方案 `BUGFIX-bug1-freeze-verify.md` 停留在「方案（未执行）」。设备本身造不出丢包（无 netem / 无 `iptables` / 无 `tcpdump`），场景要在路由器或 PC 热点侧造 | 判据是否真能触发、触发后能否自愈，**全部未知**。且没有正对照就无法区分「场景没造出来」和「判据是死的」 |

### 4.1 文档自我矛盾清单的裁决结果

本仓库对同一件事存在**多份文档互相矛盾**的情况。以下裁决由 Lead 在 2026-11-13 作出，**以本节为准**；相关原文档未修改，读到旧表述时请回来对照本节。

| # | 矛盾点 | 裁决 | 依据 | 需弃用的说法 |
|---|---|---|---|---|
| 1 | `GST_BUFFER_FLAG_DELTA_UNIT` 的位值 | **`1u << 13`** 正确；`1u << 4` 是 `GST_BUFFER_FLAG_LIVE`，语义相反 | 源码 `PenCG/native/csrc/cgmedia/cm_gst.c:105` 已写死 `1u << 13`；`GST_MINI_OBJECT_FLAG_LAST = 1<<4`，`DELTA_UNIT = FLAG_LAST << 9` | ❌ `BUGFIX-bug3-bug4-video.md:482` 的 `(1u << 4)` —— **2026-11-13 晚已在该文件就地改成 `1u << 13` 并加了纠错注记** |
| 2 | `gst_buffer_get_pts` / `gst_buffer_set_pts` 是否存在 | **不存在**（真机 `libgstreamer-1.0.so.0` 1.22.0 与本地桩库均无此符号） | 逐名解析 `.dynsym` 确认；PTS 只能走 `gstbuffer.h` 结构体字段宏，该头文件不在工作区 | ❌ `BUGFIX-bug3-bug4-video.md:475-480` 建议用这两个 accessor 的做法，**该段建议整体作废**。PTS/DTS 是「明确跳过」，不是漏做。**2026-11-13 晚已在原处标注作废** |
| 3 | `[cgmedia]` native 日志落在哪个文件 | **`/userdata/applog/DictPen_*.log`，每次开机换新文件名**。取最新一份：`ls -t /userdata/applog/DictPen_*.log \| head -1`，再 `grep -a '[cgmedia]'` | 真机实证：`DictPen_20261113_070039.log` 里就含有 §2.5 的 audio 四条原文 | ❌ `BUGFIX-bug3-bug4-video.md` 步骤 -1 与 `BUGFIX-bug5-audio.md` 步骤 -1 里的「落盘去向待现场确认」——**已确认，就是这个文件** |
| 4 | 一期停滞判据的触发条件（`freeze-verify` vs `stallguard` 口径不同） | **维持两文档并存，不做裁决**。`BUGFIX-bug1-freeze-verify.md:38` 称「一期按 `gateDrop` 连续增长 12s 写的」，而 `BUGFIX-bug1-stallguard.md`（571 行版）描述的是一期用 `rx` 判据；二期返工后的实际实现是**「fps 连续为 0 是主时钟，`gateDrop` 只作旁证」** | 以二期代码为准：`PenCG/src/pages/play/play.vue:1603` `checkFreeze()`、常量 `:246-248`；条件 D+F 是主时钟，E 只是「包还在进」的旁证 | 两份原文档均保留原文，此处**只记录口径差异与当前实现**，不改原文 |
| 5 | ~~「`IDR without SPSPPS` / 码流损坏导致绿屏、马赛克」~~ | ❌ **已被 AU dump + ffmpeg 全链路验证推翻**。dump 出的码流（`/userdisk/miniapp/data/au.h264`，615675 B / 300 帧）用 ffmpeg 能解出 233 帧完整画面，`corrupt decoded frame` 全在片头 I 帧；硬解帧数与软解一致；`sync=false` 一改即好 | 四层定位与三组真机对照见 **§2.4.0**；被推翻的五条推断逐条列在 **§2.4.1** | ❌ 凡本文旧版与 `BUGFIX-bug3-bug4-video.md` 里「参数集缺失 / 码流损坏 / DPB 不重协商是根因」的说法 —— **不要照这个方向排查** |

> 说明：第 4 条是**方法论层面**的差异，不是简单对错 —— 一期的实现细节在两份文档里被描述成两种样子，但二期返工后代码已经统一到「fps 连续为零」这条主时钟上，因此不影响当前行为。保留两份原文是为了让后来者知道这个判断曾经反复过。

---

## 5. 文件改动清单（`cd PenCG && git status --short`，22 M + 17 ??，全部未 commit）

> 合计 `22 files changed, 4656 insertions(+), 277 deletions(-)`（以 2026-11-13 20:45 的 `git diff --stat` 为准；工作树在本轮仍在被其他 agent 改动，数字可能继续增长 —— `docs/BUGFIX-bug2-verify.md`、`docs/BUG1-KEEPALIVE-PROTOCOL.md`、`docs/REVIEW-report.md`、`docs/BUGFIX-bug4-endfield-black.md` 都是在本文写作过程中陆续新出现的）。

### 5.1 已修改（22）

| 文件 | 一句话说明 |
|---|---|
| `README.md` | +1/-1：「只有 `console.warn`/`console.error` 落盘」改为「只有 `console.error` 落盘 + `loli_s6_dev.txt` + 行格式」 |
| `docs/DEV.md` | 看日志段落重写：落盘文件改 `loli_s6_dev.txt`，说明只有 error 落盘、log.js 走 `console.error` |
| `docs/STATES.md` | +1/-1：日志路径 `DictPen_*.log` → `loli_s6_dev.txt` |
| `native/csrc/cgmedia/cm_cmd.c` | +408：RTCP RR（`cm_rtcp_tick` `:221`、`rr_fill_block`）+ RTCP Generic NACK（`cm_nack_tick` `:407`、`nack_pack_fci` `:355`）、`setAudio 0` 补音频还原（`:538`） |
| `native/csrc/cgmedia/cm_gst.c` | +470：**bug6 修复 `restore-crtc=false`（`:255` 的 `KMS_SINK_PROPS`）**；**bug3/4 的真正修复 `KMS_SINK_PROPS` `sync=true` → `sync=false`（`:253-255`，`:220-252` 为防改回的长注释）**；bug5 音频快照/还原/aplay 四阶段；加固项 `ignore-error=false max-errors=16`、`DELTA_UNIT (1u<<13)`、分辨率变化 flush 重协商 |
| `native/csrc/cgmedia/cm_media.c` | +237：看门狗两级判定 + `JS silent` 指纹、stat JSON 扩字段（`lost/auDrop/gateDrop/…/jitterMs/rrSent`）、RTCP 入包分类、fail→达标日志 |
| `native/csrc/cgmedia/cm_rtp.c` | +1101：AU 完整性闸门、缺口表、NACK 缺口登记、RR 统计埋点、IDR 饥饿兜底 `IDR_STARVE_RELAX_MS 4000`、`cm_rtp_reset` 每局复位 |
| `native/csrc/cgmedia/cm_session.h` | +182：新增视频/音频 RR 统计、NACK 统计、`vid_relax`、音频快照 5 字段，及 `cm_rtcp_tick`/`cm_nack_tick`/`cm_rtp_reset`/`cm_rtp_flush_held`/`cm_rtp_note_stale` 原型 |
| `native/csrc/cgmedia/cgmedia.c` | +75：**bug6 修复** —— `sess_kill()` / `sess_kill_all_video()`，`js_start()` 在建新会话前收掉所有旧视频会话，避免两路 `kmssink plane-id=76` 抢同一平面 |
| `src/app.js` | +47：`onShow`/`onHide` 全部改 `console.error`；输入法守卫；用 `isScreenNotOn()` 把「熄屏」与「真退后台」分开 |
| `src/base-page.js` | +4/-4：`page onStart` / `page onStop` 改 `console.error` |
| `src/components/mine-body.vue` | +204：bug2 主战场 —— `refresh()` 补 `await restoreSession()`（`:515`）、2.5s 版本轮询（`startVersionWatch` `:373`）、日志改 error |
| `src/pages/diag/diag.vue` | +8/-8：4 处 `console.warn` → `console.error`（含 TOPAPP 与 INFO 行） |
| `src/pages/index/index.vue` | +194：bug2 —— `checkSeq()`（`:230`）+ 误跳保护（`setTab('mine')` 在 `:288`）+ 2.5s 轮询（`startSeqWatch` `:321`）+ `sessionSeq` 基线（`:356`） |
| `src/pages/login/login.vue` | +40：**仅取证用** —— `onReady` 末尾调 `devAutoLogin()`，按 `DEV_AUTOLOGIN` 自动填表并 `submit()`（走生产链路，不绕过被测逻辑；`DEV_AUTOLOGIN` 为 null 时等同空函数） |
| `src/pages/mine/mine.vue` | -3/+1：删掉重复的空 `onShow() {}` |
| `src/pages/play/play.vue` | +744：STALLGUARD 三级自愈、弱网 `checkFreeze` 判据（`FREEZE_GATE_MS 12000`）、后台宽限期（60s/10s）、`probe` 日志扩 `gateDrop/auDrop/kfReq/silent/frozen/tries` |
| `src/services/auth.js` | +140：跨页会话版本号（`$falcon` + storage 双写，三个 bump 点 `:175`/`:197`/`:214`） |
| `src/services/im.js` | +2/-2：`console.warn` → `console.error` |
| `src/services/log.js` | +15：头注释改为真机实测结论；`push()` 无条件 `console.error`（`:44`） |
| `src/services/media.js` | +40：`requestKeyFrameBurst()`（`:530`）/ `clearKfBurst()`（`:523`）（STALLGUARD 的 L1 手段），`stopPlay()` 先清 burst（`:595`） |
| `src/services/nav.js` | +59/-15：`setPendingTab`（`:46`）/`takeTabParam`（`:59`）改走 `$falcon`（`TAB_KEY` `:34`），模块变量降级为兜底 |
| `src/services/screen.js` | 620 行改动：从 117 行重写为 589 行 —— 多后端探测、20s keep、亮度还原、`isScreenNotOn()` 唯一判据 |

### 5.2 新增（17）

| 文件 | 内容 |
|---|---|
| `docs/BUG1-KEEPALIVE-PROTOCOL.md` | bug1 传输侧的**可行性研究**（551 行，未改代码）：四个问题逐条判定 —— `gameToken` 只在 `paasDispatch` 响应里（我们走 `api/v2/tickets` 拿不到，死路）；官方 `sendProxy`（`/rtc-sdk` protobuf）与我们连的 `wss://.../ws` 是两套端点；「提前主动重建」可行（`startPlay()` 不碰 ticket/机位）；零代码实验该先做。结论已吸收进本文 §2.2 |
| `docs/BUGFIX-bug1-lifecycle.md` | bug1 生命周期：`[Idle]` 三条阈值、两条链时序、旧实现对拍、最小补丁、A1–A6 验收、adb 命令级验证步骤（含取证通道 §6.0） |
| `docs/BUGFIX-bug1-rtcp-rr.md` | RTCP RR 的定位与实现，七条候选原因逐条判定，明确 **RR 不是 bug1 根因** |
| `docs/BUGFIX-bug1-stallguard.md` | JS 侧收包停滞三级自愈：判据选择、三个坑、常量与 file:line、四条未验证项 |
| `docs/BUGFIX-bug1-freeze-verify.md` | bug1 二期「弱网冻结」判据（`checkFreeze`）的**真机验证方案**：设备造不出丢包（无 netem/iptables/tcpdump）、三种场外造场景手段、正对照要求、六行判读表。**状态是「方案（未执行）」** |
| `docs/BUGFIX-bug2-login.md` | bug2 三个断点、跨页上下文实证、四道防线、验证步骤与防回归门槛 |
| `docs/BUGFIX-bug2-verify.md` | bug2 的**真机验证方案**（535 行）：文首自述「当前状态：本方案的任何一步都还没有被执行过」，所有「期望日志行」都是按源码推断的假设。**与本总结 §2.3 的「⚠️ 代码已改、真机未验证」一致** |
| `docs/BUGFIX-bug3-bug4-gst.md` | 出画层 `cm_gst.c` 三个 hunk 落地记录（含 `1u<<13` 纠错、PTS 跳过的三条硬理由）；**§7「sync=false 定案」= 真正修复的完整证据链** |
| `docs/BUGFIX-bug4-endfield-black.md` | bug4「终末地黑屏」的独立追查记录（701 行），开篇即 `## 〇、定案（2026-11-13 20:35）：根因是 kmssink sync=true`，与本文 §2.4 一致 |
| `docs/REVIEW-report.md` | 上线前代码审查（781 行，未改代码）：23 条逐条判定。**S1 已不存在**（`dev-autostart.js` 三个开关现在都是 `null`）、**F1 / N1 / R2 / R3 / R5 / L1 已修**；**仍未修**：R4（IDR hold 被下一个 FU-A start 清掉）、P1-7（无 `relax_until_ms`）、L2（`gap_drop` 死函数）、L3（`screen.js` 无退避）、L4（`log.js` 无轮转）、L5（`[cgmedia]` 落盘说法不一致，本文 §4.1 第 3 条已裁决）、L6（`flush_frame` 未查返回值） |
| `docs/BUGFIX-bug3-bug4-video.md` | RTP 层 bug3/bug4：AU 闸门、缺口表、参数集注入、KMS 取证与截图方法（**`:482` 的 `1u<<4` 已在 2026-11-13 晚就地改为 `1u<<13`，`:475-480` 的 `get_pts/set_pts` 段已标注作废，PTS 论证段已标注方向废止**） |
| `docs/BUGFIX-bug5-audio.md` | bug5 根因（声学 2×2 矩阵证明 OFF/SPK 对响度无差别）、快照/还原设计、aplay 四阶段、读数基线 |
| `docs/SCREEN-STATE-FINDINGS.md` | `getScreenState()` 返回值的证据分级审查：只有「0 = 亮屏」被证实，1/3/4 不可互证 |
| `docs/VERIFY-playbook.md` | 5 个 bug 的真机验证清单（前置/操作/应见日志串/判据/失败看什么）+ 取证通道 §0 + grep console 附录 |
| `docs/audio-baseline.txt` | bug5 的 mixer / asound.conf / hal-audio 全量基线读数 |
| `src/dev-autostart.js` | 设备无法 `send_event` 点进 scroller 内元素时的自动进入机制（`DEV_AUTOSTART`） |
| `docs/BUGFIX-SUMMARY.md` | **本文**。总览表 + 逐 bug 详述 + 平台级事实 + 未解决项 + 文件清单 + 下一步建议，用作整轮工作的验收依据 |

---

## 6. 下一步建议（按优先级）

1. **先验 bug2，成本最低、判决最明确。** 手动退出登录 → 密码登录 → 抓 `grep -a 'pencg' /userdata/applog/loli_s6_dev.txt | grep -a '\[mine\] refresh'`。出现 `logged=true` 即通过；同时确认冷启动**不**出现「会话版本 0 -> N」。
2. **给 bug1 装一个「自愈计数器」再做长跑。** 现在 STALLGUARD 是否生效完全没有记录。建议在 probe 行已有的 `tries=` 之外，加一条只在三级自愈真的走完时打的 WARN，然后长跑至少 30 分钟（超过 4m18s 窗口 7 次）。同时在家里网络（17:57 那次 7 分钟未复现）和弱网各跑一轮，才能判断它是否与网络条件相关。
3. **bug3/bug4 已定案，剩下的只是「回归时怎么看」。** 判决已由用户目视完成（`sync=false`，见 §2.4.0）。以后改动出画层时，最快的回归指纹是 **DRM `fb` 编号是否在交替**（`sync=true` 时恒不变）；如果想做像素级比对，仍然值得解决「抓不到视频平面」这个前置问题，三条候选路径：① 给 `weston-screenshooter` 加 `CAP_SYS_ADMIN` 或用 root 起；② 找 `miniapp_cli captureFB` 返回 `-1` 的原因（权限？格式？）；③ 在 `kmssink` 下游插一个 `videoconvert ! jpegenc ! filesink` 的旁路，把解码器输出的帧直接落盘比对。③不依赖任何截屏能力，**最值得试**。
4. **不要再往「码流 / 参数集 / DPB 重协商」方向排查 bug3/bug4。** 那五条推断已被 AU dump + ffmpeg 全链路验证推翻（§2.4.1），照那个方向改只会白做。`BUGFIX-bug3-bug4-video.md:475-482` 的两处错误与 PTS 论证段已于 2026-11-13 晚就地标注作废，文首也指向 `BUGFIX-bug3-bug4-gst.md` 的新 §7。**唯一要记住的一句：`KMS_SINK_PROPS` 里的 `sync=false` 不要改回 `true`。**
5. **bug1 传输侧：先花半天验证 `gameToken` 到底能不能拿到**，拿不到就不要投任何精力去实现 20015/20021 —— 那是唯一可能真正修掉 bug1 的路径，但它的前提（通道可复用、`gameToken` 可得）目前**一条都没验证**。在此之前，把 STALLGUARD 的兜底文案做友好一点（现在三级全败是「请点重连」，用户体感仍然是「停了」）。

> 优先级调整建议：如果这一轮只能做一件事，做第 1 条（bug2，判决明确、成本最低）；如果目标是把 bug1 从「用户体感坏了」变成「至少能自愈」，做第 2 条 + 跑 `BUGFIX-bug1-freeze-verify.md` 的正对照。

---

## 附：一句话给作者

- **真的修好并真机验证过的**：bug5（音量 + 退出残留声）**完全解决**；bug3 + bug4（马赛克残留 / 绿屏黑屏）**完全解决** —— 根因是 `kmssink sync=true`，已改 `sync=false`，原神与终末地两个游戏都由用户目视确认画面正常；bug1 的屏幕保活**机制成立**（9 分 20 秒不掉屏）。
- **代码写完了但还没在真机上验过的**：bug2（登录刷新）、弱网 IDR 饥饿、STALLGUARD 自愈、弱网冻结判据（`checkFreeze`）。
- **没有修的**：bug1 的真根因 —— 服务端 4 分 18 秒停推。
