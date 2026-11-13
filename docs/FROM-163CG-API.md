# 回函：163CG-API 复核结果（2026-11-13，2026-11-14 更新）

回应 `HANDOFF-163CG-API.md`。仓库已更新到 **338 条接口**（上一版 272），
新增 `docs/断流与保活根因分析.md`，commit `3772623` / `83f64c6`。

下面是交接文档里 6 个交付问题 + 对 PenCG bug1 的直接结论。

> **2026-11-14 重要更新**：第四、六节已按最新实测**整体重写**。
> 上一版列的三条修复建议（会话期 ICE consent / answer 回显 extmap-rtcp-fb /
> recvonly→sendrecv）**全部已实现且全部无效** —— bug1 根因仍未定位，
> 但 11 个协议层假设已被逐一排除。详见 `docs/断流与保活根因分析.md`。

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

## 四、对 PenCG bug1 的直接结论（2026-11-14 03:00 重写）

> **这一节被整体重写过。** 上一版列了「三处缺口 + 三条修复建议」，那些建议已经
> **全部实测否定**。下面是现在的实际状态。

### 4.1 一句话结论

**bug1 的根因至今未定位。** 但我们把排查范围收窄到了「没有任何协议层可见差异」——
也就是说，继续对着报文/SDP/auth 包找差异，已经没有东西可找了。

### 4.2 已经实测排除的 11 个假设

方法：全部在**官方网页端**上用 Playwright + `addInitScript` 改写报文做（不改 PenCG 的
native 代码），把网页端改成「PenCG 的样子」，看它是否也复现 ~250 s 静默停推。

| # | 假设 | 实验手段 | 结果 |
|---|---|---|---|
| 1 | 没响应 `dataChannelReady` 被踢 | 网页端本来就不响应（offer 无 SCTP m 段） | 连播 10 min ❌ |
| 2 | WS 不心跳被踢 | 网页端本来只发 1 次 `0 <ts>` | 连播 10 min ❌ |
| 3 | 服务端 300 s kickout | 观察 258/300/360 s | 网页端无异常 ❌ |
| 4 | 前端 600 s 无操作超时 | `gameCutTime.total=1000*(480+120)=600 s` | 网页端 602 s 断（对上了），PenCG 250 s 差 2.4 倍 ❌ |
| 5 | **会话期不发 ICE consent** | 给 native 补 2.5 s 周期 consent（`cm_ice_consent_tick`） | `resp` 从 #1 涨到 #109、网关全回，**仍在 4m19s 冻结** ❌ |
| 6 | **answer 不回 `a=extmap`/`a=rtcp-fb`** | `MUNGE=flags`：从 Chrome answer 删掉 5 条 extmap + 19 条 rtcp-fb | 连播 **481 s**（`ct=477.883`）❌ |
| 7 | **answer 是 `a=recvonly`** | `MUNGE=full`：再把 `sendrecv` 全改 `recvonly` | 跑到 **354 s** 仍正常 ❌ |
| 8 | **上行 RTCP（RR/NACK/PLI）惹到网关** | 给 native 加 `noRtcp` 开关全关（consent 保留） | `PLI sent`=0、`RR sent`=0，**仍在 247 s 冻结** ❌ |
| 9 | 游玩期不轮询 HTTP 被判挂机 | 记录网页端所有 `/api/` 请求 | 31 个请求全在开头 16 s，之后零请求，`users/@me/status` 从未出现 ❌ |
| 10 | **auth 后不发任何 input 帧** | 丢弃全部 `op:"input"`（7 条开播帧 + 心跳） | 连播 **481 s**，`ws-close` 为 0 ❌ |
| 11 | **auth 包内容不同** | `MUNGE=all`：把网页端 auth 改写成完全等于 PenCG 的形态 | 连播 **284 s**，258 s 节点 `ct=254.787` 完全正常 ❌ |

第 11 条改写后的 auth（token 打码），网关接受并回了 `op:offer`：

```json
{"id":"1794595903308","op":"auth","data":{
  "user_id":"…","token":"<token>","game_code":"mrfzzmd",
  "w":1280,"h":720,"quality":"low","codecs":["h264"],"platform":1}}
```

**就是说：PenCG 的 auth 包（缺 `fps`、缺 `apk_decoder`、platform 4→1、codecs 15→1）
网关完全接受，而且不因此停推。**

### 4.3 上一版的三条修复建议，现在的状态

| 上一版建议 | 现状 |
|---|---|
| ① 会话期 ICE consent | **已实现**（`cm_ice_consent_tick` + `ice_consent_count`/`rx_consent_resp` 统计 + stat JSON 的 `consentSent`/`consentResp` 字段）。实测网关正常回，但**不治 bug1**。留着——它是 RFC 8445 §11 的正当行为，去掉反而更不对。 |
| ② answer 回显 `a=extmap`/`a=rtcp-fb` | **已实现**（`media.js` 的 `parseSdp` 认 `a=extmap:`/`a=rtcp-fb:`，`buildAnswer` 回显；但**显式跳过 `transport-wide-cc`/`transport-cc`**，因为 native 还没有 TWCC，声明了不发会让网关的拥塞控制以为「一个包都没收到」，比不声明更糟）。**不治 bug1**。 |
| ③ `recvonly` → `sendrecv` | **已实现**。**不治 bug1**。 |

### 4.4 一条关键的附带发现（对定位有用）

冻结期间我们发的 ICE consent **仍然被网关正常回应**：

```
[cgmedia] #1 ICE consent sent (#97, every 2500ms) resp=98  rxStun=108
（此时 RTP 已停了 12 s）
```

**网关没有丢弃这个会话**——UDP 五元组是活的、我们的报文它照收照回。它是**单独停掉了
RTP 的推送**。这排除了「会话被回收 / NAT 超时 / 端口失效」一大类解释。

### 4.5 剩下的候选方向（按性价比排序）

> 2026-11-14 03:40 更新：这一节按设备侧新取证重排了。两条关键新事实：
> **①第一个媒体包 `seq` 已经是 31394（不是 0）**，实测包率 ≈80 pkt/s，
> 第一次回绕要 (65536−31394)/80 ≈ **427 s**，与 230~259 s 的冻结差 1.7 倍；
> **②SRTP 认证从未失败过一次**（`grep 'DUMP fail'`=0、`SRTP BRUTE`=0），
> 若 ROC 错了认证会成片失败。所以原来的第 1 名降到第 2 名。

1. **DTLS 层的差别（现在是第一优先）。** 设备侧实测值已取到，三次会话完全一致：
   `DTLS OK version=DTLSv1.2 cipher=ECDHE-RSA-CHACHA20-POLY1305 srtp=SRTP_AES128_CM_SHA1_80`。
   对应代码：`cm_dtls.c:352 SSL_CTX_set_tlsext_use_srtp(ctx, "SRTP_AES128_CM_SHA1_80")`
   （**单一 profile**，Chrome 是 `SRTP_AEAD_AES_128_GCM:SRTP_AES128_CM_SHA1_80`，GCM 在前）、
   `:350 SSL_CTX_new(DTLS_client_method())`、`:355 SSL_VERIFY_NONE`、
   `:357 SSL_OP_NO_TICKET`；**没有设 min/max_proto、cipher_list、extended_master_secret，
   全走 OpenSSL 默认**。网关最终选中同一套 SRTP profile，但 **ClientHello 本身不一样**
   ——若网关按指纹风控，入口在这里。
   验证：抓两边 ClientHello 逐字段 diff（cipher suite 顺序、extension 顺序与内容、use_srtp 列表）。
2. **SRTP 的 ROC / 重放窗口（基本排除，可花 10 分钟坐实）。**
   真机日志里前 8 个包 `roc=0 seq=31394..31401`，加上实测 80 pkt/s，
   第一次回绕在 ~427 s，不在冻结窗口内；且认证零失败。
   要彻底坐实：`cm_srtp.c:369` 的 dump 条件现在是
   `if (srtp_auth_fail + rtp_rx < CM_SRTP_DUMP_MAX)`（`CM_SRTP_DUMP_MAX=8`，
   **只打开头 8 个包就永久静默**），改成「每 500 包一条」或「seq 接近 0/65535 时打」，
   跑一轮看冻结时的 `seq` 离回绕有多远。
3. **网关按 `pkg`/`game_code`/宿主机分档。** 已有两组对照：
   自建客户端 `container=t-04 / host=ahnfs10015 / encoder=high,None` → 230 s 冻结；
   网页端 `container=t-18 / host=ahnfs10007 / encoder=high,None` → 481 s 未冻结。
   `encoder` 两边一样，判据不在它身上；但 `container`/`host` 不同，
   **每轮都记下来**，凑够样本才能判断是「某些宿主机有问题」还是巧合。
4. **UDP 接收缓冲区**（较弱）。冻结期间 `rxStun` 同步增长说明 socket 仍在正常读取，
   如果是 rcvbuf 满导致内核丢弃，`rxStun` 也会停。所以基本可以排除。

### 4.6 工程上的建议

**在根因定位之前，用主动换会话兜底。** 每 ~230 s 重新 `POST tickets` + 重连，
用户侧表现是「画面卡 1~2 秒」，比「4 分钟后彻底冻结」好得多。

> ⚠️ 注意：`play.vue` 里 `ACTIVE_RENEW_MS` 已被改成 **0（默认关闭）**，理由是
> 「换会话必然换机位」的副作用（见 `docs/BUGFIX-bug1-active-renew.md`）。
> 现在的第一道防线是 **STALLGUARD**（`STALL_RX_MS = 8000`，rx 8 s 不涨就判停）+
> 三级自愈（补关键帧 → 复用网关重连 → 重新申请机位）。真机实测 L2 能在 11 秒内恢复
> （`stall recovered: rx=79 已恢复（第 2 级自愈成功，停滞 4s）`）。

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

## 六、还需要的实测（2026-11-14 03:00 重写）

上一版这里写的是「按顺序只加 ①②③，看 4m18s 还在不在」。**①②③ 已经全部做完，
全部无效**（见第四节）。所以现在的顺序是：

1. **抓两边 DTLS ClientHello 逐字段 diff（第一优先）。** 网页端用
   `chrome://webrtc-internals` 或 Playwright hook `RTCPeerConnection` 导出；PenCG 侧
   在 `cm_dtls.c` 的发送路径 dump（或用 `SSL_CTX_set_msg_callback` 已有的 `msg_cb`，
   它能看到每个握手报文）。重点比：`supported_versions`、`cipher_suites` 顺序、
   `use_srtp` 的 profiles（自建只给 `SRTP_AES128_CM_SHA1_80`，Chrome 是 GCM 在前）、
   `extended_master_secret`、`supported_groups`、证书 CN/有效期。
2. **（可选，10 分钟）坐实 ROC 排除。** 改 `cm_srtp.c:369` 的 dump 条件为周期性，
   跑到冻结看 `seq`。按现有数据（起始 31394、80 pkt/s）预计冻结时 `seq` 在
   31394+80*250≈51400，离 65536 还远，预期不跨 0。
3. **每局记录 `op:result` 的 `{"container","host"}` 与冻结时刻。** 跑 5~10 局，
   看是否某些 host/容器必冻（现有样本：`t-04/ahnfs10015` 冻、`t-18/ahnfs10007` 不冻）。
   注意设备日志里 `op:result` 是 offset 加密的，且 **offset 不一定是 62**
   （实测遇到过 55）——对 0..255 暴力搜 `b'"op"'` 最稳。
4. 若以上都排除 → 那基本可以认定是网关按**账号/容器/IP** 做的时长策略，
   PenCG 侧无法规避，只能靠 STALLGUARD 自愈兜底。

### 如果还需要我做什么

- 我可以再抓一次**网页端 answer 的完整文本**与你这边 answer 做逐行 diff（已有存档，
  `docs/断流与保活根因分析.md` §2 + `work/samples/` 里的 `server-offer.sdp`/`client-answer.sdp`）。
- 我也可以帮你把网页端的 `RTCPeerConnection` 完整配置（`getConfiguration()`、
  `sctp`、`iceTransportPolicy`、bundle 策略）导出来。
- **但报文层的差异已经查完了**，再做逐字节 diff 的边际收益很低。

### 一条方法上的提醒

想验证「X 是原因」，**去掉 X 看是否复现**，比「加上 X 看是否消失」可靠。
因为「加上 X 之后不断了」可能只是 X 顺手带出了别的效果（比如重连换了新机位）。
本轮三次假结论都是这么来的——另外，**跑实验前必须确认没有第二个客户端在用同一账号**，
否则每轮都会被 `games_playing_status` 顶掉。

---

## 七、本轮（2026-11-14）163CG-API 侧新增的东西

| 项 | 位置 |
|---|---|
| 断流与保活根因分析（11 个假设的完整实测记录） | `docs/断流与保活根因分析.md`（已重写） |
| 实验工具 7 个 | `work/get_ticket.py`、`longplay{,2}.js`、`sdp_probe.js`、`pencg_shape.js`、`no_input_probe2.js`、`poll_probe.js`、`pencg_auth_probe.js` |
| 两条「会话结束」路径的完整取证 | `docs/断流与保活根因分析.md` §4（含 `op:"close"` 的 6 行真实时刻表） |
| `errcode` 速查表 | 同上（403 / 1213 / 1811 / 1288 / 0+normal） |
