# 回函：163CG-API 复核结果（2026-11-13）

回应 `HANDOFF-163CG-API.md`。仓库已更新到 **338 条接口**（上一版 272），
新增 `docs/断流与保活根因分析.md`，commit `3772623` / `83f64c6`。

下面是交接文档里 6 个交付问题 + 对 PenCG bug1 的直接结论。

---

## 一、线上变化（对应交接文档任务 A）

| 项 | 结果 |
|---|---|
| bundle 变化 | 3 个主包 hash 全变（`chunk-common.eb5489e3` / `index.89d39400` / `run.5e59f26c`），`chunk-vendors` 与 4 个 CSS **未变** |
| 本次补抓 | 从 `index.89d39400.js` 解析出 webpack chunkhash 表，**下载了 98 个懒加载 chunk**（980,748 B） |
| 接口总数 | **272 → 338**（canonical 形式 313） |
| ADDED / REMOVED / Method 修正 | **65 / 0 / 10** |
| 说明 | 65 条里 40+ 条是「一直都在、上一版抽取器没抽到」，不是线上真新增。上一版正则只认 `return s+"api/..."` 与 `return c("/x")`，且 `.concat` 用 lookahead 会在内层逗号处截断 |

新增的主要是：社区 `article_list` / `hot_article_list` / `topic/article_list` /
`recommend/gy_music_sheet_list`（+6 个乐谱接口）、群组 `group/*`（8 条）、
`games_pro`、`customize-settings/{group}/{key}`、`welfare_center/{code}/info|tabs`、
`team_up_*`、`square/*`、`block`、`users/{id}`、`kol_user_recommends` 等。

**65 条里实测通了 33 条**（三轮迭代，共 137 次探测）。失败的都没有一条是「接口不存在」，
全是缺必填参数 / 需要真实业务 id。完整清单在 `docs/API文档.md` 第 4.1–4.3 与 12.6 节。

---

## 二、DataChannel（对应任务 B①）

**结论：不需要实现，可以直接忽略。**

| 观测 | 数据 |
|---|---|
| 服务端 offer 的 m 段 | **只有 2 段**：`m=audio` + `m=video`，**没有 `m=application`/SCTP** |
| 浏览器是否建 DataChannel | `RTCPeerConnection` 的 `datachannel` 事件**从未触发**（13 分钟观测） |
| `9 <port>,1,<hex>,<base64>` | 建连后 18 s 起每秒一次，**共推 12 次**后停止；base64 恒为 `{"m":"dataChannelReady","pkg":"…","t":<同一个值>}` |
| 网页端是否响应 | **从不响应**，照样连收 10 分钟 |
| `common_setting` | `enable_data_channel:false` / `enable_data_channel_v2:true` |
| `data_channel_params` | `{connect_timeout_ms:5000, disable_keepalive:1, keepalive_interval_ms:1000, keepalive_interval_ms_after_no_resp:500, switch_ws_after_x_failure:12}` —— 这 12 个值只在**真的建了** DataChannel 后才有意义（连续 12 次 1 s 心跳失败才回落 WS） |

`run.*.js` 的 `checkDataChannelConnect()` 里前置条件是 `if (this.isDataChannel && …)`，
`isDataChannel` 在浏览器路径上永远是 false，所以这套保活逻辑网页端**根本没跑**。

→ **PenCG 不建 DataChannel 不是 bug1 的原因。** 建议把「响应 dataChannelReady」从 bug1 的
假设里划掉，只当作一个可选优化（若将来想用 SCTP 传输入，再实现）。

---

## 三、断线/超时（对应任务 C）

交接文档问的四个问题，逐条答：

### Q1 「由于您太久没回来游戏已关闭」什么条件弹？

在 `run.*.js` 的 `onError(e)` 里，`n = i.errcode || i.code`，只有下面这一条路径：

```js
if (403 == n || 1213 == n) {
  // …按 storage NCG-mediaservers 里 delay<750 的 region 重新 POST tickets，
  //   成功就写回 NCG-gwurl 等并 window.location.reload()
  //   失败 / 没测速信息 → 弹「由于您太久没回来游戏已关闭…请尝试自行重开游戏」
}
```

### Q2 服务端用哪个 op 通知？

**`op:"error"` + `errcode` ∈ {403, 1213}**。不是 `op:"game_stop"`，也不是 `op:"close"`。

另外这次抓到了**第三条路径**，交接文档里没有：

```json
{"op": "close", "data": {"code": 0, "reason": "normal"}}
```

`case"close": this.onGameStop(i), this.onClose(i);` → 走「正常结束」，**不弹任何超时文案**。
真机抓到的时序（2026-11-13）：

| 相对时刻 | 事件 |
|---|---|
| 406.9 s / 412.1 s | `games_playing_status` 里仍是当前会话 |
| 413.199 s | `games_playing_status` → `games_playing: []`（机位被摘除） |
| 413.255 s | `op:"close"` `{code:0, reason:"normal"}` |
| 413.263 s | 浏览器 `ws-close`，`video.currentTime` 仍靠本地缓冲继续涨 |
| 418.651 s | 同一 token 下出现**另一个 `play_id`** 的会话（被接管） |

### Q3 触发计时器是哪个、多长、能否从 common_setting 读到？

**不能从 `common_setting` 读，来自游戏详情：**

```js
commit("setGameCutTime", { total: 1000*(k.no_op_duration1 + k.no_op_duration2),
                           warn:  1000*k.no_op_duration2 })
```

实测 `mrfzzmd` / `wow_cn_high` 都是 `no_op_duration1=480`、`no_op_duration2=120` →
**total = 600 s、warn = 120 s**。`interval()` 每 120 s 递减一次，≤120 s 时弹常驻 toast
「您还在玩游戏吗？」，归零则 `onPostClientError(1002)` + `intervalExitGame()` +
弹「由于长时间不操作，已经自动退出」。

13 分钟实测里这条精确落在 **602 s**（两次 `pageerror` + 网关 WS close），
`video.currentTime` 之后仍在涨 —— **这是客户端自杀，不是服务端断流。**

### Q4 网页端靠什么避免被判超时？

两个答案，都很反直觉：

1. **无操作超时靠前端计时器重置**：`touchstart`（移动端）/ `keydown`+`click`（PC）/
   `inputAction` 总线事件 / `keyup`，任一命中就 `setIntervalTime()`。
   `setGameInputRefresh()` 会顺带发一次 `input {"cmd":"0 0"}`，但有 **600 s 节流**。
2. **服务端侧根本不看 WS 心跳**：网页端在建连时只发了 1 次 `{"cmd":"0 <ts>"}`，
   **29 秒之后 WS 上再没有任何帧**，视频照样流了 10 分钟。

→ **不要为了「保活」去加 WS 心跳。** 网页端没做这件事。

---

## 四、对 PenCG bug1 的直接结论

### 先排除三个错误假设

| 假设 | 反证 |
|---|---|
| 不响应 dataChannelReady 被踢 | 网页端也不响应，照样收流 10 分钟（`grep` 无任何响应逻辑） |
| WS 不心跳被踢 | 网页端 29 s 后 WS 完全静默，照样收流 |
| 服务端 300 s kickout | 网页端同样从 ticket 起算，258 s 时毫无异常；600 s 断的是前端计时器 |

`docs/BUGFIX-bug1-stallguard.md` 里「kickoutDuration 300 s − 冷启动 42 s ≈ 258 s」这个推断，
与网页端实测不吻合，建议降级为「已排除」。

> **2026-11-14 补充**：上面这条「建议降级为已排除」的判断，与 PenCG 侧最新的真机结论**方向一致** ——
> 那个 `300 s − 42 s` 的算式确实不成立（PenCG 三局实测：把起点改成每次 `wsOpen` 后是 247/253/248 s，
> 即「每个 WS 会话 ≈250 s」，不是「ticket + 300 s」）。两边的差别只在于是「没有固定计时器」
> （网页端看起来是）还是「有计时器但绑定在会话上、时长约 250 s」（PenCG 实测看起来是）。
> **网页端 258 s 时毫无异常这个事实，本身也可能只是网页端的会话在那之前已经被重建过**——
> 这一点我没有网页端的 `wsOpen` 时间戳，无法判断，标为**待现场确认**。
> 详见 `docs/BUGFIX-bug1-active-renew.md`。

### 真正的差异：SDP 与 getStats

网页端一次完整的 SDP + 20×5 s `getStats()` 采样（2026-11-13）：

**服务端 offer**（`a=ice-lite`，2 个 m 段，无 SCTP）：
- 每个视频 PT（98 H264 / 96 VP8 / 107 H265）都带**全套** `ccm fir` / `nack` / `nack pli` /
  `goog-remb` / `rrtr` / `transport-cc`
- extmap：audio `2`(audio-level) `9`(transport-cc)；video `11`(transport-cc)
  `12`(playout-delay) `13`(generic-frame-descriptor)
- 每个 m 段都是 `a=sendrecv`

**Chrome answer**：`a=sendrecv`，**逐段回显全部 extmap 与 rtcp-fb**。

**getStats（20×5 s）**：

| 指标 | 观测 |
|---|---|
| `candidate-pair.consentRequestsSent` | **5 → 40，单调递增（≈每 2.5 s 一次）** |
| `candidate-pair.responsesReceived` | 6 → 41（网关每次都回） |
| `requestsReceived` / `responsesSent` | 恒 1 / 1 |
| `inbound-rtp.pliCount` / `nackCount` | **全程 0** |
| `remote-inbound-rtp` | 全程缺失（网关从不发 RTCP RR） |

**两个关键结论**：

1. 网关**不靠 RTCP 反馈判活**（浏览器一次 PLI/NACK 都不发也能收 10 分钟）。
   → 补 RR / NACK 是补协议义务，但别指望它治 bug1。这一点 `BUGFIX-bug1-rtcp-rr.md`
   的自我判断是对的。
2. **唯一的周期双向报文是 ICE consent（STUN Binding，~2.5 s 一次）。**

### PenCG 的三处缺口（已核对源码）

| # | 缺口 | 证据 |
|---|---|---|
| 1 | **会话期不发 STUN** | `cm_ice.c:1-14` 头注释自述「之后立刻停发 STUN、转 DTLS」；`grep -rn "consent\|refresh" cm_ice.c` → 0 命中 |
| 2 | **answer 不回 extmap / rtcp-fb** | `src/services/media.js` 里 `grep -c extmap` = 0；`buildAnswer()` 只回 `a=recvonly` + rtpmap/fmtp；native 侧 `grep -rn "transport-cc\|twcc"` → 0 命中 |
| 3 | **answer 是 `recvonly` 不是 `sendrecv`** | `media.js:141` 每段 `a=recvonly`；网页端是 `sendrecv` |

### 修复建议（按性价比，可独立验证）

**① 会话期 ICE consent（先做这个，改动最小）**

`cm_ice.c` 里**已经有**完整能力：

- `ice_send_binding(cm_sess *s, const char *key, int with_change_req, int as_controlled)`
  —— 构造 USERNAME / PRIORITY / ICE-CONTROLLING / USE-CANDIDATE / MESSAGE-INTEGRITY /
  FINGERPRINT，发到 `s->cfg.remote_ip:remote_port`
- `cm_stun_handle()` —— 收到网关的 Binding Request 能正确回 `0x0101`

缺的只是一个**会话期定时器**：

```c
/* 在 cm_ice_run() 成功后启动；会话结束时取消 */
static void ice_consent_tick(void *arg) {
    cm_sess *s = arg;
    /* key = answer 里公布的 ice-pwd；as_controlled=0 保持 controlling 角色 */
    ice_send_binding(s, s->cfg.our_pwd[0] ? s->cfg.our_pwd : s->cfg.pwd,
                     /*with_change_req=*/0, /*as_controlled=*/0);
    /* 2500 ms 后再次 */
    cm_timer_after(s, 2500, ice_consent_tick, s);
}
```

注意：

- key 用 **`ice-pwd`**（answer 公布的那个），不是网关的密码；
- `as_controlled` 保持 0，除非网关回过 487 Role Conflict；
- 响应 `0x0101` 媒体期可以丢弃，**但日志里要能看见它在回**（`rx_kept++`）——
  这是验证 patch 生效的最快信号。

如果 4m18s 消失，bug1 就定案了。（**2026-11-14 更正**：按新判据应读作「如果 `wsOpen` + 250s 之后画面还在动」，见 `docs/BUGFIX-bug1-active-renew.md`）

**② answer 回显 `a=extmap` / `a=rtcp-fb`**

`buildAnswer()` 里按 m 段处理时，把 offer 里该段的 `a=extmap:*` 与 `a=rtcp-fb:*` 原样搬过去；
native 侧按 extmap id 解析 RTP 头扩展、每 100 ms 发一条 TWCC（RTCP **PT=205 FMT=15**，
PenCG 现有 NACK 是 PT=205 FMT=1，可以复用同一套 RTCP 打包代码）。

短时间做不完 TWCC 的话，**至少先把 extmap/rtcp-fb 回显上** —— 让网关看到「这个对端声明了
transport-cc」，比完全静默好。

**③ `a=recvonly` → `a=sendrecv`**

网页端就是 sendrecv；`recvonly` 可能让网关不把你当活跃订阅者。

---

## 五、其它修正（对 PenCG 有用）

1. **`op:"close"` 要当成终止信号**，和 `op:"error"` 分开处理。只监听 error 会把「礼貌下线」
   误判成网络故障，白跑一轮重连。
2. **`op:"error"` 的 403 / 1213 是「机位没了」**，正确反应是重新申请机位
   （网页端就是自己 POST tickets），而不是重连同一个 gateway。
3. **机位 ttl 只有 24 s**（`tickets.time_left=24`），必须「申请即连」。
4. **`regions` 必须给具体 region code**，空数组报 `404 errcode=1288 请重新测速`；
   必须先 `POST api/v2/network-tests` 上报测速结果。
5. **刚有机位未退时申请新机位报 `400 errcode=1811 退出游戏中，请重试`** →
   先 `DELETE api/v2/users/@me/games-playing/<code>`，等 ~4 s 再申请。
6. **`GET api/v2/media-servers` 的 `game_code` 是必填 query**，缺了报
   `400 errcode=1302 "'game_code' is a required property"`。

---

## 六、还需要的实测（如果 ①②③ 都不成立）

按顺序：

1. 只加 ①，跑满 10 分钟，看 4m18s 是否还在。
   （**2026-11-14 更正**：按新判据应读作「看每个 WS 会话能否越过 ≈250 s」，
   即 `wsOpen` 之后 250 s 处是否停推；见 `docs/BUGFIX-bug1-active-renew.md`。）
2. 若还在 → 加 ②，再跑一次。
3. 若还在 → 加 ③。
4. 若还都在 → 那就是服务端按「客户端 IP + 会话时长」做的策略，需要对比
   「PenCG 的 DTLS 指纹 / answer 的 sctp-port / `a=mid` / `a=msid` 是否缺失」等更细的差异。
   目前网页端 answer 的完整文本已存档在 `docs/断流与保活根因分析.md` 与本报告 2.1 节，
   可以逐行对比。

如果需要，我这边可以再抓一次 PenCG 的 answer 与网页端 answer 做逐行 diff。
