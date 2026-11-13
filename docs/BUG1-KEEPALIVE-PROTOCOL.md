# bug1 挂机保活协议研究（RtcKeepPlaying / RtcKeepaliveCfg）

> 状态：**纯研究文档，本阶段未改动任何代码**。
> 输入：lead 2026-11-13 指令（四问 Q1–Q4）。
> 结论先行：**Q1 与 Q2 都是死路**——`gameToken` 在我们这条链路上根本不存在，
> 官方那条 `20015/20021` 控制通道是 protobuf 信令面，我们的 `/ws` JSON op 面接不进去。
> 不要为了"有产出"而给出无法落地的方案，本文按这个原则写。

---

## 1. 结论摘要

| 问题 | 结论 | 一句话依据 |
| --- | --- | --- |
| **Q1 gameToken 在哪** | **拿不到（死路）** | 它只在 `POST /dispatcher/api/paasDispatch` 响应的 `finish_result.sdk_param` 里，即 `proto.paas.SdkStartGameParams` 字段 4；我们走的是 `api/v2/tickets`，响应里没有这个字段，且 paasDispatch 请求体要靠官方 web 包里内嵌的 `appEnv.appKey` 签名。 |
| **Q2 我们的 JSON WS 能否承载 20015/20021** | **不能（死路）** | 官方走 `sendProxy` = channel 2 = `<gameServerWssUrl>/rtc-sdk` 的 protobuf 帧（`'Eg'` magic + cmdId + len + `0x89 0xAB`）；我们连的是 `gateway_url` = `wss://.../ws`，两者是同一台 gw 上的**两个不同端点、两套不同帧格式**。我们的 `cmd` 文本表里也没有任何一个等价物。 |
| **Q3 退路** | **可行：提前主动重建（复用 gatewayUrl，不换机位）** | `startPlay()` 只清 JS 侧状态并重开 WS，**不碰 ticket/机位**；真正重新申请机位的只有 `applySeat()`。代价是重建期黑屏（实测值未知）。 |
| **Q4 零代码实验** | **可做，且必须做** | ★ 但实验目的要改：**2026-11-13 的两局现场都没有复现 bug1**（17:58 那局跑了 ≥21 分 50 秒），所以第一件事不是区分 A/B/C，而是先确认「4m18s 现在还发生吗」。实验矩阵见 §5。 |

### 1.1 ★ 一个必须由 Lead 纳入决策的新事实

- **已知的三次复现现场采集**（`.tmp/bug1-soak.log`，2026-11-12 22:46:30–22:58:22）
  发生在 RTCP RR 代码合入**之前**——那份日志里 `RR sent` 计数为 0。
- **RR 合入之后**（2026-11-13）的两局：
  - 17:58:04 起播 → 18:25:15 仍在出帧（`rx` 从 250 单调涨到 131099，`fps=29`），
    跨越 4m18s / 8m / 16m 三个窗口**均无冻结**；
  - 18:47:20 起播 → 18:50:43 `rx=20359 fps=19`，同样无冻结。
- 这两局**几乎没有真实操作**（全量日志里 `input cmd 忽略: 0 ` 命中 3003 条，全是 keepAlive 回显；
  没有 cmd 1/2/3/8）。按 A 假设（无操作 kickout=300s），17:58 那局应该在 18:02:22 就停，**实际没有**。

**结论：A 假设（挂机 kickout）被 2026-11-13 的现场连续两次反证。**
同时，RR 上线与"不再复现"在时间上高度相关。样本量只有两局，**不能据此断言因果**，
但这足以把 §4 的优先级重排——详见 §7 决策项 1。

---

## 2. Q1 —— gameToken 全链路（结论：拿不到）

### 2.1 官方链路（逐跳，带偏移）

1. 用户在官方网页点「开始游戏」→ `Ue()` → `Se()` 派发流程
   → `POST /dispatcher/api/paasDispatch`
   （端点枚举在 `.tmp/cgsdk/web.103b867a.js` 偏移 174726–175400：
   `{WalletGet:"/wallet/wallet/get", StatusCheck:"/dispatcher/api/statusCheck",
   GetNodesInfo:"/dispatcher/api/getNodesInfo", PaasDispatch:"/dispatcher/api/paasDispatch",
   PreDispVeri:"/dispatcher/api/preDispatchVerify"}`）。
2. 请求体由 `getPaasDispatchPayload()`（web bundle 偏移 222700–224400）构造，
   签名一行是 `b=(0,r.R)(y,_.U.appEnv.appKey)`，payload 含
   `hint / net_state / queue_type / sign / using_new_cmd_line / regions`。
   **`appEnv.appKey` 内嵌在官方 web 包里，我们的仓库没有这个值**（`appEnv.appKey` 在 web bundle 出现 5 次，
   用途全是 `sign` 计算：`paasDispatch` / `preDispatchVerify` / `caCodeReport`）。
3. 响应里 `a.data.finish_result`（`result_code === FINISHED`）→ `e.startGame(t)`
   （web bundle 偏移 ~241523）→
   `l.$.startGame({applicationId, environment, accountId, sdkParam: e.sdk_param, isOversea, client:{type}}, 3e4)`。
   **`sdk_param` 只在这里出现。**
4. `.tmp/cgsdk/cg-sdk.891daeae.js` 偏移 ~1175150 的 `ClientCore.startGame`：
   `const r = proto.paas.SdkStartGameParams.deserializeBinary(e.sdkParam)` →
   `WebSessionInfo.fromDispatchStartGame(e, r)`。
5. `proto.paas.SdkStartGameParams` 字段表（serializer 在偏移 478142）：

   | 字段号 | 名字 |
   | --- | --- |
   | 1 | `sid` |
   | 2 | `caId` |
   | 3 | `podId` |
   | **4** | **`gameToken`** |
   | 5 | `resolution` |
   | 6 | `targetFps` (int32) |
   | 7 | `cmdLine` |
   | 8 | `gameSvrAddr` |
   | 9 | `rtcUdpPort` (uint32) |
   | 10 | `additionalGameSvrAddrs` (repeated) |
   | 11 | `gameServerWssUrl` |
   | 12 | `additionalGameServerWssUrls` |
   | 13 | `bigIsp` |
   | 14 | `gameControlChannelUrl` |
   | 15 | `additional` |
   | 16 | `isIpv6` |
   | 17–23 | IPv6 变体 |

   → **`getGameToken()` 就是 `SdkStartGameParams` 的字段 4。**
   （`GameSessionInfo` 的字段 24 是另一个无关 message，别被名字带偏。）
6. `WebSessionInfo`（偏移 1073005–1073900）构造函数
   `constructor(e,t,o,r,i,s,n,a,g)` =
   `(gameToken, accountId, sid, podId, clientType, serverWssUrlList, serverIpAddrList, serverGccUrlList, rtcPort)`；
   `fromDispatchStartGame(e,t)` 从 `t`（SdkStartGameParams）取
   `getGameToken() / getSid() / getPodId() / getGameServerWssUrl() + getAdditionalGameServerWssUrlsList()
   / getGameSvrAddr() + Additional / ...getGameControlChannelUrl() / getRtcUdpPort() || 0`。
7. `webSessionInfo.gameToken` 的全部消费点（`setGameToken(this.webSessionInfo.gameToken)` 共 9 处，
   偏移 1180193 / 1180717 / 1181028 / 1186264 / 1203442 / 1209475 / 1210111 / 1212550 / 1212924）：
   `RtcPauseReq` / `RtcResumeReq` / `ResetInputsReq` / **`RtcKeepPlaying`(20015)** /
   `StopGameReq`（两处）/ `SdkStatsInfo` / `SdkDeviceInfo`。

### 2.2 我方链路（为什么拿不到，硬证据）

| 环节 | 事实 | 位置 |
| --- | --- | --- |
| 申请机位 | 走 `POST api/v2/tickets`，请求体固定 8 项 | `PenCG/src/services/session.js:117-135`、`:144` |
| 请求体内容 | `{regions, game_code, codecs:['h264'], width, height, extra:{dpi,resolution,source_type:'web',network_test_info_list}, apk_decoder:{model,decoder}}`，**无 `sign` / 无 `sdk_param` / 无 `biz_key`** | `session.js:117-135` |
| 响应投影 | `{gatewayUrl, lockDetail, ticket, region, regionName, ticketTtl, screen, raw: t}`，`raw` 是全量响应 | `session.js:147-157` |
| `raw` 是否被裁 | 不裁：`v2Post` → `v2()` 返回 `j.data !== undefined ? j.data : j` | `PenCG/src/services/cg.js:284` |
| `raw` 谁在读 | **无人读**（`play.vue:859-860` 读的是画质列表的 `raw`，不是 tickets 的 `raw`） | `PenCG/src/pages/play/play.vue:859-860` |
| 落盘的全量条目 | `.tmp/cgprobe/q6.json` 逐字段遍历：**没有 `gameToken` / `game_token`** | `.tmp/cgprobe/q6.json` |
| 里面唯一的 `token` | `/params/token` = 用户级 JWT（`{"iat":...,"type":"access","user_claims":{"number":557449638,...}}`），与官方 `auth` 用的用户 token 同源，**不是每局一个的 gameToken** | 同上 |
| SDP / offer 里有没有 | 10 个落盘 SDP（`offer2.sdp` / `offer_live.sdp` / `offer_wow_cn_high.sdp` / `offer_hold.sdp` …）`grep -il token` **零命中** | `.tmp/cgprobe/*.sdp` |
| 全仓库 grep | `grep -rn 'sdk_param\|game_token\|gameToken\|rtc_token' PenCG/` → **代码零命中**，只有本项目的文档提到 | — |

### 2.3 Q1 判定

**拿不到。** 三步证据链闭合：

1. 产生 gameToken 的接口是 `paasDispatch`，不是 `api/v2/tickets`——我们从来没调过它；
2. 调它需要 `appEnv.appKey` 做签名，这个 key 内嵌在官方 web 包里，我们仓库没有；
3. 因此即使把 `api/v2/tickets` 的 `raw` 全量打印出来，也不可能出现 gameToken。

**唯一可能的迂回**（代价极高，不建议）：逆向官方 web 包的 `appEnv.appKey`，自己实现 `paasDispatch`
和 `SdkStartGameParams` 解析，再切到 `<gameServerWssUrl>/rtc-sdk` 的 protobuf 信令面。
这等于重写整个信令层，且 appKey 是否随版本轮换未知。**不建议在 bug1 语境下做。**

---

## 3. Q2 —— 通道可行性（结论：不可行）

### 3.1 官方 channel 2 不是"keepalive 专用"，而是一整个 RTC 控制面

`signalingChannel` 的四个发送方法（`cg-sdk.891daeae.js` 偏移 991500–992150）：

```
sendHandShake = e => this.sendMessage(e, 3)
sendProxy     = e => this.sendMessage(e, 2)   // ← sendTcpMessage 就是它
sendSignaling = e => this.sendMessage(e, 1)
sendKeepAlive = () => this.sendMessage("", 4) // ← 空负载，每 10s
```

`sendTcpMessage` = `signalingChannel.sendProxy`（偏移 1042210）→ **channel 2**。
`sendTcpMessage(` 共 **18 个调用点**：

| 偏移 | 消息 |
| --- | --- |
| 1097333 / 1102111 | `ReliableMessageQueueData` / `...Ack`（RMQ） |
| 1180244 | `RtcPauseReq` |
| 1180768 | `RtcResumeReq` |
| 1181077 | `ResetInputsReq` |
| 1182686 | `RtcSetBitrate` |
| 1183336 | `RtcSetBitrateMultiplier` |
| 1183846 | `RtcSetFps` |
| **1186313** | **`RtcKeepPlaying`(20015)** |
| **1186988** | **`RtcKeepaliveCfg`(20021)** |
| 1187497 | `RtcSetGraphicsMode` |
| 1190566 | `KeyFrameReq` |
| 1194312 | `RtcDataChannel`(20010) |
| 1204093 | `StartGameReq` |
| 1209524 / 1210160 | `StopGameReq`（两处） |
| 1212599 | `SdkStatsInfo` |
| 1214132 | `SdkDeviceInfo` |

→ Lead 原先的假设「只有 keepPlaying/setKeepAliveConfig 走 channel 2」**不成立**，
但结论反而更强：channel 2 是一个**完整的 protobuf 信令面**（18 条消息、RMQ、码率控制、关键帧请求、DataChannel），
我们一条都没接。想只塞进 20015/20021 两条，等于先要接入这套面的一半。

### 3.2 帧格式（如果真要接）

`Packet.toUint8Array()`（`minPacketSize 12`）：

```
e[0]=69, e[1]=103          // 'E' 'g'  magic
e[2..3] = cmdId   (大端)
e[4..5] = headLen (大端)
e[6..9] = msgLen  (大端)
e[10..] = head (protobuf)
         + msg  (protobuf)
末两字节 = 0x89 0xAB
```

`parsePacket` 校验 magic `17767`（0x4567）。

### 3.3 但端点不同——这才是死结

| | 官方 | 我方 |
| --- | --- | --- |
| WS 地址 | `getServerWssUrl()` = `gameServerWssUrl`（来自 `SdkStartGameParams` 字段 11），并强制追加 `/rtc-sdk`（偏移 ~1073800：`e.endsWith("/rtc-sdk") \|\| (e += "/rtc-sdk")`） | tickets 返回的 `gateway_url` = `wss://gw-dx-hnfs-03.cg.163.com:6443/ws` |
| 帧格式 | 二进制 `'Eg'` + protobuf | JSON `{op,id,data}`，逐字节 `+offset(72)` 再 base64（`PenCG/src/services/cg.js:122-134`） |
| 首个包 | `sendHandShake`（channel 3） | `auth`（明文 JSON 首发，`PenCG/src/services/media.js:305-343`） |

同一台 gw 主机、**两个不同端点、两套不同协议**。而且：

- `web.103b867a.js` 里 `op:"input"` / `game_set_quality` / `offer` / `answer` / `game_stop` **全部 0 命中**；
- `send_message` 在 `cg-sdk.891daeae.js` 与 `web.103b867a.js` **都零命中**（官方 bundle 没带 chunk-common 源码）；
- 我方 WS 收到的 op 全集只有 `input`（回显）/ `result` / `offer` / `info` / `error` / `game_stop` / `close`，
  **从来没有收到官方的 `RtcUserNotPlayingTips`(-1021) / `RtcUserNotPlayingKickout`(-1022)**
  （`ResultCode` 枚举，偏移见 `cg-sdk.891daeae.js` 里 `e[e.RtcUserNotPlayingTips=-1021]="RtcUserNotPlayingTips"`）。
  官方是**先收到 Tips 再提示用户**，我们连 Tips 都收不到，这从反面证明我们的 `/ws` 不在那条消息面上。

### 3.4 我们能不能从 `cmd` 文本协议里"蹭"到等价语义

`PenCG/src/services/input.js` 全部 `sendInput` 前缀（grep 实锤）：

| cmd | 我方实现 | 语义 | 官方对应 |
| --- | --- | --- | --- |
| `1 x y id` | `input.js:41` | 按下 | `send_touchstart_message` |
| `2 x y id` | `input.js:47` | 移动 | `send_touchmove_message` |
| `3 x y id` | `input.js:51` / `:56` | 抬起 / 抬起(原始) | `send_touchend_message` |
| `4` | `input.js:60` | 取消 | `send_touchcancel_message` |
| `5 <text>` | `input.js:68` | 文本 | 键盘 |
| `6 <keycode>` | `input.js:64` | 键码 | 键盘 |
| `8 x y id` | `input.js:37` / `:146` | 点按 | `EVENTS.tap = 8` |
| `10 <platform>` | `input.js:91` | 声明平台 | `"10 " + ("android"!==ti \|\| inApp ? "" : "h5-") + ti` |
| `0 <ts>` | `input.js:78` `heartbeat` / `:97` `keepAlive` | 心跳 | 官方 `sendKeepAlive → sendMessage("", 4)`（**空负载 channel 4**） |
| `21/22/23` | `input.js:132/136/140` | 外设鼠标 down/up/move | `send_message_peripheral` |
| `25 touchscreen\|keymouse` | `input.js:74` | 输入模式（官方拼写 `tochscreen` 少 u） | 同 |
| `28` | `input.js:82` | 摇一摇 | 同 |
| `100/101/102` | `input.js:108/112/116/122` | 端游鼠标（65535 归一化） | `send_message_pc` |
| `18` | **未实现** | — | **未找到对应** |
| `9` | **未实现** | `{"m":"dataChannelReady","pkg":"<包名> ..."}` 的 base64（约 1 条/秒，服务端→客户端） | `RtcDataChannel`/`DataChannelMessage` |

**上表里没有任何一个 cmd 号等价于 `RtcKeepPlaying`。**
我们实现的 `4`（cancel）、`21/22`（外设鼠标）语义都已确认与 keepPlaying 无关；
`18` 我们没实现，但它在 2026-11-13 18:51:20.036 的真机日志里由**网关推给我们**（`input cmd 忽略: 18 0`，
原始帧 base64 解出 `...\x02CMD\x02 ...\x11\x18\x00\x10\x02]]`，按每字节 +0x20 解出文本 `18 0`），
也就是说它是服务端→客户端的通知，不是我们要发的上行。**结论：借道 cmd 号这条路不存在。**

### 3.5 我们的 `op=input cmd="0 <ts>"` 与官方 `sendKeepAlive` 是否等价

**不等价，且这是我们自己的猜测。**
`PenCG/src/services/input.js:96-98` 的注释自述「官方在 datachannel 保活里每 1s 发一次 `0 <ms>`
（我们不走 datachannel）」，`PenCG/src/pages/play/play.vue:1330-1339` 再降到 10s 一次。
线上唯一可验证的事实是：我们发 `0 <ts>`，网关**原样回显** `op=input 0 <ts>`。
回显只能证明"帧被解析且分发回来了"，**不能证明它被算作"玩家活跃"**——
官方那条保活是 channel 4 的空负载帧（不同通道、不同语义），两者是否在网关侧落到同一个"活性"计数器上，
**未验证**（见 §6）。

### 3.6 另外：我们主动拒绝了 data channel

`PenCG/src/services/media.js:103-169` 的 `buildAnswer` 在遇到 `application` m 段时主动回
`m=application 0 UDP/DTLS/SCTP ...` + `a=inactive`（`:123-132`），理由写在注释里：
我方没有 SCTP 实现，一旦接受，网关就会把 input 定向到一条永远建不起来的通道。
官方 offer 里确实带 `m=application 9 UDP/DTLS/SCTP webrtc-datachannel` + `a=sctp-port:5000`
（`.tmp/cgprobe/offer2.sdp`、`offer_wow_cn_high.sdp`）。

**这与 keepPlaying 无关**（官方 keepPlaying 走 WS channel 2，不走 SCTP），
但如果将来要接 `RtcDataChannel`(20010) 游戏数据通道，必须先实现 SCTP。

### 3.7 Q2 判定

**不可行。** 三条独立理由：
(1) 端点与帧格式都不同，且我们拿不到官方那条 WS 的地址（它来自 `SdkStartGameParams` 字段 11）；
(2) 官方 channel 2 是 18 条消息组成的完整信令面，不是能单独摘出来的 2 条；
(3) 我们的 cmd 文本表里没有 keepPlaying 的等价物，也没有证据表明 `0 <ts>` 被算作活跃。

---

## 4. Q3 —— 退路排序

前提修正：`startPlay()` 与 `stopPlay()` 的真实行为如下（这决定了"重建"到底有多轻）。

| 动作 | 做什么 | 不做什么 | 位置 |
| --- | --- | --- | --- |
| `startPlay(cfg)` | 清 `S.offerSdp/remote/ourUfrag/ourPwd/ourFp/mediaId/vizOn/wsState`，重开 `wsOpen(gatewayUrl, ...)` | **不碰 ticket、不重新申请机位** | `PenCG/src/services/media.js:422-449` |
| `startMedia()` | 用当前 `S.offerSdp` 起新的 native `cgmedia.start(cfgJson)`（新 sess id） | 不换 WS | `media.js:454` 起 |
| `stopPlay()` | `clearKfBurst()` → `__cgMedia.stop(mediaId)` → `__cgNet.wsClose(wsId)` → `phase='closed'` | 不释放机位 | `media.js:591-619` |
| `onReconnect()` | `stopPlay()` → 有 `gatewayUrl` 就 `startPlay(...)`，成功打 `[play] reconnect ok（复用旧网关）`；失败才清 `gatewayUrl` 走 `applySeat()` | 不重新排队 | `PenCG/src/pages/play/play.vue:1120-1154` |
| `onReapply()` / `applySeat()` | `stopPlay()` + 清 `gatewayUrl` + `applyTicket(...)` | — | `play.vue:1155-1164` / `:743` |

**native 侧没有"只重建 RTP/SRTP 不换 sess"的接口**：
`PenCG/native/csrc/cgmedia/cgmedia.c` 只有 `sess_alloc`(`:119`) / `sess_free`(`:148`) /
`js_start`(`:217`) / `js_stop`(`:306`)，一个 sess 的生命周期是
ICE → DTLS → `cm_srtp_open` → `cm_media_loop`（`cgmedia.c:166-215`），
而 `startMedia` 必须拿 `S.remote.cand`（来自新一轮 offer 的 candidate），
所以**"只重启 native 媒体会话"不能独立成立**——没有新 offer 就没有新 candidate。

### 4.1 候选退路（按性价比排序）

| # | 手段 | 代价 | 收益 | 风险 | 判定 |
| --- | --- | --- | --- | --- | --- |
| **R1** | **先做 §5 的真机实验** | 0 代码；每局消耗免费时长（`PenCG/docs/QUEUE.md:370-390`：一次 enqueue 约 35 秒） | 把 A/B/C 定性，决定 R2–R4 选哪条 | 免费时长有限（全账号约 25–30 分钟/天） | **必做，最高优先** |
| **R2** | **提前主动重建**（在停流点前，例如 3m50s，调 `onReconnect()` 路径） | 一次 `stopPlay()+startPlay()`；重建期黑屏（**实测值未知**，估计 0.5–3s，含等新 IDR） | 若停流是"会话级固定时长"（C），重建后计时重置，可做到相对零黑屏 | ① `canReconnect` 守卫在 `stage==='live' && wsState==='open'` 时返回 false（`play.vue:544-550`），必须照抄 `stallHealL2` 的 `stage='waiting'` 借道技巧（`play.vue:1539-1559`）；② 网关是否接受同一 ticket 的第二次 offer —— **未验证**；③ 若停流是机位级（B/A），重建白费 | 可行，但必须先验 ② |
| **R3** | 停流后靠现有 STALLGUARD 兜 | 已实现，0 额外成本 | 能自愈，但用户先黑屏 ≥25s（`STALL_RX_MS=20000` + `STALL_L1_WAIT_MS=5000`，`play.vue:206/209`） | 已在线上，稳定 | 兜底，不是解法 |
| **R4** | 缩短 `STALL_RX_MS`（20s → 8~12s） | 一行常量 | 缩短静止画面时间 | 误触风险：切画质/网络抖动时 `rx` 短暂不涨会被误判（`play.vue:1409` 起的 `checkStall` 只看 `rx` 是否增长） | 可选，需先测误触率 |
| **R5** | 定时发"无害真实输入"保活（如 `8 <角标坐标> 0`） | 每 N 秒一条 input | 若是 A（按最后一次操作计时）就有效 | ① 会真的点进游戏（可能误触 UI/暂停）；② 是否被算作"操作"**未验证**；③ 官方 `keepPlaying` 是显式消息，不是"假装操作" | 只有 §5 证明 A 才考虑 |
| **R6** | 实现 20015/20021 | 需要 appKey + paasDispatch + protobuf 信令面重写 | 唯一"正解" | 工作量等于重写信令层，且 appKey 可能随版本轮换 | **死路（§2/§3）** |
| **R7** | 请运营侧调大 `kickoutDuration` | 0 代码 | 若是 A 且服务端认这个概念，一劳永逸 | 官方默认 300000ms（`boxConfig.keep_alive_config` 默认 `{default:5}`，validator `1<=v<=30` 分钟，`kg = e => 1000*60*e` → 300000ms）；我们不是官方客户端，服务端是否会为我们这条链路单独配置 —— **未知** | 需业务侧支持 |

### 4.2 Q3 判定

**可行路径只有一条主线**：R1 先定性 → 若是 C（会话级）走 R2 → 若是 A（挂机）则 R5/R7 二选一 →
无论哪条都保留 R3 作兜底。R6 是死路，不要投入。

---

## 5. Q4 —— 零代码真机实验设计

### 5.1 设计原则

要能把三种假设**互相区分**，关键是找到"只影响其中一个自变量"的操作：

| 假设 | 停流时刻的公式 | 只改变什么的实验能区分它 |
| --- | --- | --- |
| **A 挂机计时器** | `最后一次有效操作 + kickout` | 中途发真实输入 |
| **B 单局时长** | `机位分配（tickets 成功）+ 固定值` | 什么都不变，只看它是否固定在 4m18s |
| **C 会话时长** | `WS/媒体会话建立 + 固定值` | 中途重建会话（复用 gatewayUrl，不换机位） |

三种实验正好一一对应，所以**做三组，每组一局**。

### 5.2 公共准备（每条都是只读或已确认安全）

```bash
cd D:/Users/Daniel/code/DSH/x6pro/haas-x6pro
ADB="./env/adb/adb.exe -s 9D91500007505171"

# 0) 确认设备在线、时间同步
$ADB shell 'date'

# 1) 找到当前主日志（★ 不要写死文件名，也不要只靠 ls -t 猜）
#    正确做法：解析 miniapp 进程的 stdout 指向
P=$($ADB shell 'pidof miniapp' | tr -d '\r')
L=$($ADB shell "readlink /proc/$P/fd/1" | tr -d '\r')
echo "LOG=$L"

# 2) 开跑前确认日志已存在且非空（只读统计用；不改设备状态）
$ADB shell "ls -la \"$L\""

# 3) 起播：用 dev-autostart（唯一能自动进 play 页的方式）
#    PenCG/src/dev-autostart.js:17  DEV_AUTOSTART = { page:'play', delay:4000,
#      params:{ code:'yssy', name:'原神-7.2版本', quality:'low', gameType:'mobile' } }
#    改完需要重新部署前端（本阶段禁改代码，所以由 Lead 决定用现有 build 跑）
```

**每 20 秒采一次 probe**（probe 行本身就是现成的采样器，`PenCG/src/pages/play/play.vue:1364-1379`）：

```bash
# 每局开始前重新解析一次（进程重启后 $P/$L 会变）
P=$($ADB shell 'pidof miniapp' | tr -d '\r')
LOG=$($ADB shell "readlink /proc/$P/fd/1" | tr -d '\r')
while true; do
  date '+%H:%M:%S'
  $ADB shell "grep -a -o 'probe stage=.*' $LOG | tail -1"
  sleep 20
done
```

> 判读要点：`rx` 冻结 + `fps=0` + `stage=live` + `ws=open` = bug1（网关停推）。
> `rx` 涨但 `fps=0` = `checkFreeze` 型（解码侧），**不是** bug1。
> `stage` 变成 `ticket`/`waiting`/`closed` = 页面/JS 侧动作，不是 bug1。

### 5.3 组 1（基线，判定 bug1 是否仍成立）

**做什么**：起播后**完全不操作**，坐等 15 分钟。

**判据**：
- 若在 4m18s±5s 出现 `rx` 冻结 → bug1 仍成立，进入组 2/组 3；
- 若 15 分钟不冻结 → **bug1 在当前离线环境下不复现**，
  这时组 2/组 3 都失去意义，应该直接向 Lead 报告"环境已变，先查变了什么"
  （优先查 RR 上线前后的差异、`cm_gst.c` 的音频改动、以及 `PenCG/libs/*.so` 的构建时间）。

### 5.4 组 2（区分 A：中途发真实输入）

**做什么**：起播后**每分钟**发一次真实的触摸 tap（走我们自己的 `onTS/onTE` → `cmd 8` 路径）。

```bash
# 点击视频画面左下角外侧一点（避开绝大多数游戏的 UI 热区）
# 坐标是屏幕坐标，设备分辨率 480x960（已实测 /sys/class/graphics/fb0/virtual_size）
$ADB shell 'send_event touch slip 60 900'
```

**为什么要这样**：`send_event touch slip X Y` 是设备上唯一能落到 play 页元素上的注入方式
（`PenCG/docs/BUGFIX-SUMMARY.md:389-399`）；它会被 `PenCG/src/pages/play/play.vue:36` 的
`<div class="stage" @touchstart=... @touchend=...>` 接住，走 `onTS`(`:2176`) → `tap`/`down` →
`PenCG/src/services/input.js:37` 的 `8 <x> <y> <id>`，**是真真正正的游戏输入**，
不是我们自己伪造的心跳。

**判据**：
- 停流时刻 ≈ **最后一次 tap + 4m18s**（跟着 tap 往后移）→ **A 成立**；
- 停流时刻仍固定在 **起播 + 4m18s**（与 tap 无关）→ **A 不成立**，看组 3。

**风险**：`slip 60 900` 可能点到游戏内的某个按钮。建议先用一局确认这个坐标对目标游戏无副作用
（可以先 play 页 `vizOn` 状态下试一次，观察无反应再用于正式实验）。

### 5.5 组 3（区分 C：中途重建会话，不换机位）

**做什么**：起播后**在 3 分钟整**触发一次"复用旧网关的重连"。

**触发方式（零代码，两选一）**：

```bash
# 方式 1（首选）：点 UI 上的「重连」按钮
#   PenCG/src/pages/play/play.vue:126  <div :class="reconnectCls" @click="onReconnect">重连</div>
#   注意 canReconnect 在 stage==='live' && wsState==='open' 时返回 false（:544-550），
#   直连态下按钮是锁的 —— 所以这条路只有在画面已经停了以后才能点，不能用于"提前主动重建"。
#   坐标需现场确认（未验证）。

# 方式 2：让 STALLGUARD 自己走 L2
#   在停流前无法触发；只能在停流后观察 L2 是否把画面救回来（这本身是一条有价值的验证：
#   L2 之后如果又能跑满 4m18s，说明计时跟着"会话"走 = C；如果 20 秒内又停 = 按机位算 = B）
```

**判据**：
- 若组 3 的停流时刻 ≈ **重建时刻 + 4m18s**（且明显晚于组 1 的停流时刻）→ **C 成立**
  （会话级计时，主动重建是有效解法，R2 可行）；
- 若停流时刻仍是 **机位分配 + 4m18s**（与重建无关）→ **B 成立**
  （机位级计时，只能靠换机位/换 ticket，代价高）；
- 若停流时刻 ≈ **最后一次操作 + 4m18s** → 回到 A。

**⚠️ 这个判据依赖一个未验证前提**：`onReconnect` 复用 `gatewayUrl` 后，网关是否重新发 offer 并重新推流。
如果复用旧 ticket 的第二次 offer 被拒（`op=error` 或直接 `op=close`），组 3 无效，
日志里会看到 `[play] reconnect ok（复用旧网关）` 之后紧跟 `[media] <- op=error ...` 或 `op=close`。

### 5.6 组 3 的替代方案（如果方式 1/2 都不可用）

用 **HTTP 侧观测**代替主动干预：在起播时记下 `tickets` 的返回时刻，然后对照停流时刻：
若 `停流时刻 - tickets 成功时刻` 恒为 4m18s（三局三次），而 `停流时刻 - WS 建立时刻` 有抖动 → **B**；
若反过来 → **C**。这需要我们把 `tickets` 时间戳打出来（当前 `session.js:144` 附近没有打时间戳，
属于**需要改代码**，本阶段不做，列为后续项）。

### 5.7 Q4 判定

**可做，但必须先做组 1**：2026-11-13 两局未复现，意味着"4m18s"这个前提本身可能已经不成立。
在组 1 确认 bug1 仍能复现之前，组 2/组 3 都没有意义。

---

## 6. 未验证事项（诚实清单）

1. **`4m18s` 是否仍成立** —— 2026-11-13 的两局（17:58 起 ≥21 分 50 秒、18:47 起 ≥3 分钟）都没复现，
   且这两局几乎没有真实操作。**这是本文件最大的不确定性。**
2. **`kickoutDuration = 300s`** —— 只是从官方 SDK 默认值与"300 − 42 ≈ 258s ≈ 4m18s"反推，
   **没有任何网关侧实证**（`PenCG/docs/BUGFIX-bug1-stallguard.md:414-421` 第 1 条）。
3. **42 秒冷启动** —— 估算值，样本仅 3 次（同上第 2 条）。
4. **我们的 `0 <ts>` 心跳是否被网关算作"活跃"** —— 只有"网关回显"这一个证据，回显 ≠ 计活。
5. **官方 `/rtc-sdk` 端点是否会接受我们的连接** —— 未验证；`curl` 实测 `/ws` → 400、`/rtc-sdk` → 403，
   两个都只对裸 HTTP 拒绝，说明不了并存性问题。
6. **`send_event touch slip` 是否能落到 `<div class="stage">` 上** —— 文档只验证过它能点 play 页按钮，
   没验证过它能命中 stage 区域。组 2 的第一步就是验证这个（发一次，看日志有没有 `input cmd 忽略` 之外的
   出站 input，或看画面有没有反应）。
7. **主动重建时网关是否接受同一 ticket 的第二次 offer** —— 未验证。
8. **L2 重建后网关是否重新计时** —— 未验证（`stallguard.md:414-421` 第 3 条）。
9. **cmd `18` 的语义** —— 2026-11-13 18:51:20.036 真机见过网关推 `18 0`（原始帧解出文本 `18 0`），
   我方未实现、直接丢弃，语义未知。
10. **cmd `9` 的完整语义** —— 已解出是 `{"m":"dataChannelReady","pkg":"<包名> ..."}` 的 base64，
    但被 `clip(ic,40)` 截断（`play.vue:968`），完整 JSON 未知。
11. **`[cgmedia]` 与官方是否"看到同一个会话"** —— 我们只从自己这一侧观测，网关侧无任何回执。
12. **AR 假设与 RR 的时间相关性** —— §1.1 指出"三次复现都在 RR 之前、RR 之后两局都不复现"，
    但样本量太小，**不能据此断言 RR 是解药**。

---

## 7. 建议的下一步（给 Lead 的三个决策项）

**决策项 1（最高优先）：先确认 bug1 是否还复现。**
理由：§1.1 的两局现场直接与 A 假设冲突，而 A 假设是上一轮"最强"的结论。
若 bug1 已不复现，那么本文件 §4/§5 的整套退路都不必投入，应该先把资源转向"找出 RR 上线与
不复现之间的因果或混淆项"（例如：是不是 `cm_gst.c` 的音频改动改变了网关侧的判活？是不是
`.so` 的构建时间本身就是变量？）。
需要一局**静置 15 分钟、全程不操作**的观测（组 1）。成本：约 15 分钟免费时长。

**决策项 2：若 bug1 仍复现，按 §5 的组 2 / 组 3 定性 A/B/C。**
每组一局，合计约 30 分钟免费时长。注意 `PenCG/docs/QUEUE.md:370-390`：一次 enqueue 约消耗 35 秒，
全账号每天约 25–30 分钟，**不要用 `enqueue` 去探测 schema**。

**决策项 3：定性之后的路线选择（需要业务/运营侧介入的提前打招呼）。**
- 若 **A**（挂机）：R5（定时真实输入，有游戏内副作用）或 R7（请运营调 `kickoutDuration`）二选一。
  R6（自己实现 20015）已判定死路，不要再评估。
- 若 **B**（机位级固定时长）：唯一解法是定时换机位（`applySeat`），代价是重新排队 + 重新加载容器，
  大概率是"几十秒黑屏"，比现状（25 秒静止）更差 —— 这种情况下**建议维持 STALLGUARD**。
- 若 **C**（会话级固定时长）：R2（提前主动重建，复用 gatewayUrl）是唯一能做到相对零黑屏的方案，
  需要先验证"同一 ticket 二次 offer 是否被接受"。

---

## 8. 附录（2026-11-13 20:35 补）：独立 bundle 取证对 Q1/Q2 的印证

> 本节由 video-integrity 追加，**只追加、不改动上文任何结论**。
> 上文的 Q1/Q2 判定（两条死路）与本节证据**互相印证**，冲突为零。

### 8.1 取证对象

Lead 上一轮要求反编译官方 SDK，产物在 `.tmp/cgsdk/`：

| 文件 | 大小 | 真实身份 |
| --- | --- | --- |
| `cg-sdk.891daeae.js` | 1230753 B | 米哈游云游戏 SDK，版本串 `CG_SDK_VERSION = 6.1.0.65` |
| `web.103b867a.js` | 563551 B | 同一套 web 客户端主体 |
| `index.html` | — | `<title>云·原神</title>`，资源全在 `webstatic.mihoyo.com/common/clgm-web-app/ys/…` |

**关键定性：这套 bundle 是米哈游「云·原神」的 web 客户端，不是网易云游戏
（`cg.163.com`）的客户端。** 二者属于同一个「云游戏 SDK 家族」的两个不同部署。
证据：`163.com` / `netease` / `cg.163` / `gw-yd` 在 `cg-sdk.891daeae.js` 里
**各 0 命中**；`mihoyo` 在 `web.103b867a.js` 里 115 命中、`cloudgame.mihoyo` 3 命中。

⇒ **所以拿这套 bundle 去推我们的 `op` 文本协议，从出发点上就不成立。**

### 8.2 对 Q2 的印证：帧格式确实完全不通

官方信令帧（`SignalingChannel` 类）：

- 8 字节**大端头** + 裸 payload：`[0..3] = 通道`、`[4..7] = 长度`；
- 通道 **1 = signaling(JSON)** / **2 = proxy(protobuf)** / **3 = handshake(JSON)** / **4 = keepalive**；
- 通道 4 的保活是 `sendMessage("", 4)` → **`websocket.send("")`，连 8 字节头都没有**，
  `setInterval(…, 1e4)` 即 10 秒一次（与我们的 `op=input cmd="0 <ts>"` 周期巧合相同，
  但走的完全是另一条路）。

官方 proxy 通道（= 承载 `20015`/`20021` 的那条）外面还包一层 `Packet`：

```
45 67 | CMD(2B BE) | HEADLEN(2B BE) | MSGLEN(4B BE) | HEAD | MSG | 89 AB
```

`parsePacket` 里 `magic != 17767 (0x4567)` 直接 throw，尾部 `tail != 35243 (0x89AB)` 也 throw，
`minPacketSize = 12`。与我们 `{"op":…}` + `encodeBody(off, json)`（逐字节 `(b+off)&0xff` 再 base64）
是**两套毫无交集的协议**。

另外，`encodeBody` / `decodeBody` 这两个词在两个 bundle 里**都是 0 命中**；
裸 `{"op":` 也是 **0 命中**；`base64` 命中 51 次但全是 `goog.crypt.base64`
（Closure 库自带的 `btoa/atob` 兼容层）。

⇒ 上文 §3 的「端点不同 + 帧格式不同 = 死结」判定，**证据充分，维持**。
顺便给一条更强的表述：**我们的 `/ws` 上不可能出现官方帧格式**，即使拿到
`gameToken` 也没法把 `20015` 塞进这条连接。

### 8.3 对 Q1 的印证：`gameToken` 只由 paas 派发链路下发

`webSessionInfo.gameToken` 全部赋值点只有 4 处：

| 偏移 | 场景 | 来源 |
| --- | --- | --- |
| `@1175362` | `startGame`（服务端派发） | `proto.paas.SdkStartGameParams.deserializeBinary(e.sdkParam)` → `.getGameToken()` |
| `@1177944` | `startGameDirect`（离线 mock） | 外部传进来的 `i`，自己 `DispatchRequest.setGameToken(i)` |
| `@1209635` / `@1210374` | 清理 | `null` |

`SdkStartGameParams`（`proto.paas`）字段：1 sid、2 caId、3 podId、**4 gameToken**、
5 resolution、6 targetFps、7 cmdLine、8 gameSvrAddr…
`DispatchRequest` 的 `gameToken` 在字段 10。
`StartGameRsp`（`@546272`）只有 `retcode` / `gameAppId` / `gameConfig`，**不含 gameToken**。

⇒ 与上文 §2 完全一致：**PenCG 走的 `POST api/v2/tickets` + WS `auth` 链路上，
没有任何一个响应带 `gameToken`**（`PenCG/src` 全目录 `grep gameToken` = 0 命中）。
要拿它必须先拿到官方 web 包内嵌的 `appEnv.appKey` 去签 `paasDispatch`，我们仓库里没有。

### 8.4 对「`cmd=9` / `dataChannelReady`」的补充（仍未定论）

网关下发的 `op=input` 里 cmd `9` 形如 `9 42854,1,e46b,eyJtIjoiZGF0YUNoYW5uZWxSZ…`，
第 4 段 base64 解码确为 `{"m":"dataChannelR`（hex `7b226d223a22646174614368616e6e656c52`）。

- 两个官方 bundle 里 `dataChannelRe` / `dataChannelReady` / `{"m":"` / `"m":` **全部 0 命中**；
- `dataChannel` 在 `cg-sdk` 里只有 5 处，**全是统计字段名**（`dataChannelType`、
  `dataChannelRttMs` 等），无一处是消息类型定义。

⇒ **这是网易云游戏网关的私有下发，官方 bundle 里没有对应实现，「要不要回」无直接证据。**
但有一条间接事实对我们有利：`PenCG/docs/MEDIA.md:391` 记录过
「`+16503ms` 才第一个 `dataChannelReady`」，而我们的会话能正常起播出画，
说明**不回它并不会导致服务端立刻判超时**（至少头 4 分钟不会）。

### 8.5 与本文件主线的关系

本节**不改变** §1 的结论摘要与 §7 的三个决策项。它只做两件事：
把 Q1/Q2 的「死路」判定从「读过代码」升级为「读过代码 + 独立 bundle 双向印证」，
并留下可复核的关键词命中数，免得以后再花半天重查同样的东西。
