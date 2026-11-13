# bug1 伴随改动：补齐 RTCP Receiver Report（PT=201）

> **一句话**：本次改动给 cgmedia 补上了周期 1Hz 的 RTCP Receiver Report。
> 它**不修复 bug1**（"游戏打开约四分钟后停界面传输"）。bug1 的根因指向
> 服务端挂机 kickout 定时器，见 §7。RR 的定位是**补齐 RFC 3550 的协议义务**。

---

## 1. 时间线与现象（真机，X6 Pro，2026-11-12 / 11-13）

### 1.1 第一次现场（2026-11-12 22:43–22:58，原神 yssy）

| 时刻 | `rx` | `fps` | 现象 |
| --- | --- | --- | --- |
| 22:43 起播 | 稳步涨，每 20s 约 +1400 | 29 | 正常 |
| 22:48:10 | **冻结在 18701** | **0** | 画面停在最后一帧 |
| 22:48:10 之后 10 分钟 | 一直 18701 | 0 | 完全不动 |

同一时刻其它通道全部健康：

```
ws=open  phase=playing        # WebSocket 正常，JS 每 10s 的 keepAlive 被网关回显
op=input 0 <ts>               # 回显可见 -> 上行没断
screen=0(ON)  keep #N 每 20s   # 屏幕保活从未中断
[cgmedia] PLI sent (26 bytes, srtcp)   # 每 2s 一次，SRTCP 加密，说明密钥 ready
[cgmedia] pt histogram 98:7362 111:11339   # 数值也冻结
```

### 1.2 第二次现场（2026-11-13，三次独立复现）

- 起播后 **4m19s / 4m18s / 4m18s** 停推，误差 1 秒。
- **音视频同时停**：直方图 98:7362 + 111:11339 = 18701，正好等于冻结时的 `rx`。
- 音频 11339 包，opus 20ms ≈ 50 包/秒 → 11339 / 50 ≈ **227 秒 ≈ 3.8 分钟**。
- `grep -a 'rtcpIn'` / `grep -a 'RTCP'` 在整份日志里**全空**：网关从不回任何 RTCP。
- WS op 统计：2751 input / 7 error / 4 result / 4 offer，**没有** keep / kick / not_playing。

---

## 2. 根因假设的第一版（已被降级，但结论要保留）

第一版假设：

> 我们**从未发送过任何 RTCP Receiver Report（PT=201 RR）**。整个 native 侧只发
> PLI（PT=206 PSFB），全代码 grep 不到任何 RR 构造。多数 SFU 依靠周期性 RR 判断
> "接收端还活着、链路还健康"；长时间收不到 RR 就会把该订阅者 pause 或降级，
> 超时通常在分钟级——这与"约四分钟后停"高度吻合。

**核对结果：假设的事实基础成立，因果推断不成立。**

成立的部分（代码事实，已逐行确认）：

- `cm_send_pli()`（`cm_cmd.c:60`）是 native 侧**唯一**的 RTCP 发送点。
- `cm_rtcp_tick()` 在这次改动前**完全不存在**，`grep -rn 'PT=201\|pt == 201\|0xC9'` 在
  `native/csrc/cgmedia/` 下零命中。
- `cm_kf_tick()`（`cm_cmd.c:93`）是**故障驱动**的：顺畅对局时整局一个 RTCP 都不发。
- `cm_srtp.c` 的入站 `cm_srtp_unprotect()` 对 RTCP 直接 `return -4`（不解密），
  所以网关即使回 RR/SR 我们也读不到——这与 §1.2 的 `RTCP` 日志全空一致，
  **不能用"没看到 RTCP 日志"反证"网关没发 RTCP"**（这一点必须在下一轮真机
  把 `setLogLevel 4` 打开后重测，见 §9）。

不成立的部分（三条反证）：

1. 网关**从不回**任何 RTCP，说明它对这个订阅者本来就不做 RTCP 双向往来。
2. 停推是**音视频同时停**、且停在一个精确的整包数上——不像"判活超时"那种
   逐流、带抖动的降级。
3. 三次复现落在 **4m18s ± 1s**。SFU 判活超时通常松散（几十秒量级抖动），
   这种精度更像**服务端定时器**。

---

## 3. 为什么还是实现了 RR

理由收窄成一条：**协议正确性**，不是修 bug。

- RFC 3550 §6.4 要求接收端周期上报接收质量。我们整局一个 RR 都不发是残缺的。
- 任何合规 SFU 都可能拿 RR 做带宽估计与丢包反馈（决定要不要补关键帧、
  要不要降码率、要不要切层）。我们此前的全部上行反馈只有 PLI。
- 代价极低、无副作用：1Hz，明文 32 / 56 字节，SRTCP 后 46 / 70 字节
  （约 0.4–0.6 kbps），不参与任何现有判定逻辑。

**预期收益：零（对 bug1）。预期副作用：零。**

---

## 4. RFC 依据

| 内容 | 出处 |
| --- | --- |
| RTCP 包通用头（V/P/RC、PT、length 以 32 位字为单位且不含头字） | RFC 3550 §6.4.1 |
| RR 包格式、length = 7（1 个 block） | RFC 3550 §6.4.2 |
| report block 六字段定义 | RFC 3550 §6.4.1（图中 RR 与时序同构） |
| `cumulative number of packets lost` 为 **24 位有符号** | RFC 3550 §6.4.1 |
| `extended highest sequence number received` | RFC 3550 §A.1 |
| 丢包率（fraction lost）与 expected/received 的算法 | RFC 3550 §A.3 |
| interarrival jitter（`J += (|D| - J)/16`） | RFC 3550 §A.8 / §6.4.1 |
| LSR / DLSR 语义（DLSR 单位 1/65536 秒，无 SR 时填 0 合法） | RFC 3550 §6.4.1 |
| 发送间隔可不按带宽折算（实现取固定 1s） | RFC 3550 §6.4 允许实现自定，WebRTC 惯例 1s |
| SRTCP 封装（E 位 + 31 位 index + tag） | RFC 3711 §3.4 |
| Opus RTP 时钟 48000 | RFC 7587 §4.1 |

关于 LSR/DLSR：`§6.4.1` 明确 "If no SR has been received yet, the LSR field is
set to zero"，DLSR 同理。我们当前 `have_sr` 恒 0（入站 SRTCP 不解密，见 §2），
所以两个字段恒填 0 —— **合法且诚实**，网关只是算不出 RTT。

---

## 5. 报文逐字段构造

### 5.1 整体布局（两路都有效时 = RC=2，明文 56 字节）

```
 offset  bytes                    值 / 含义
 ------  -----------------------  ------------------------------------------
  0      1                        V=2 P=0 RC=n  -> 0x80|n
  1      1                        PT=201 (RR)
  2..3   2                        length = (总长/4) - 1
                                   n=1 -> 7  (32 字节明文)
                                   n=2 -> 13 (56 字节明文)
  4..7   4                        sender SSRC  = s->cfg.tx_ssrc ('PENC')
  8..31  24                       reception report block #1（视频）
 32..55  24                       reception report block #2（音频，仅当存在）
```

**为什么 sender SSRC 在 `pkt+4`**：`cm_srtcp_protect()`（`cm_srtp.c:425`）用
`cm_rd32(pkt + 4)` 作为 CTR IV 里的 SSRC。RR 与 PLI 头 8 字节布局天然一致，
所以两者能走同一条加密路径，不需要任何适配代码。

### 5.2 RR 头

| 字段 | 值 | 说明 |
| --- | --- | --- |
| V | 2 | 版本 |
| P | 0 | 无 padding |
| RC | 实际填了几个 block（1 或 2） | 视频先、音频后 |
| PT | 201 | RR |
| length | `len/4 - 1` | **不含头字自身**（RFC 3550 §6.4.1） |
| sender SSRC | `s->cfg.tx_ssrc` | 懒初始化为 `0x50454E43u`（'PENC'），与 PLI 共用同一个值。**绝不能等于媒体 SSRC**，否则网关按 RTCP 环路丢弃 |

### 5.3 reception report block（24 字节，每路一段）

| 偏移 | 字段 | 我们的取值 | 为什么这么填 |
| --- | --- | --- | --- |
| +0 | SSRC_n | `cfg.video_ssrc ?: stats.vid_ssrc_seen`（音频同理） | SDP 协商的优先，没有就用实际收到的第一个 SSRC。**两路都取不到就不发这一段**；整条 RR 一个 block 都填不出就整条不发 —— SSRC_n=0 会让网关丢弃整条 RR |
| +4 | fraction lost (8 bit) | `((exp_iv - rcv_iv) << 8) / exp_iv`，clamp 255 | **区间**丢包率：只统计"本 RR 与上一个 RR 之间"（RFC A.3）。用累计值直接除会得到开机以来的平均值，网关看不出"现在突然变差"。区间内 `exp_iv == 0` 或 `exp_iv < rcv_iv`（重复包过多）时置 0，不报虚假 100% |
| +5..7 | cumulative lost (24 bit, **有符号**) | `expected - received`，clamp `+0x7FFFFF / -0x800000` | 重复包多于丢包时真值为负，按二进制补码写（RFC 3550 §6.4.1）。实现上直接按无符号 24 位截断低位，补码语义等价 |
| +8 | extended highest seq | `stats.ext_seq_max` | RFC A.1 的简化版：16 位 seq + 循环计数。`int16_t d = seq - ext_max`，`d > 0 && d <= 3000` 才推进（`RR_MAX_DROPOUT`），迟到/乱序不推进 |
| +12 | interarrival jitter | `stats.jitter_q4 >> 4` | 内部按 Q4 定点保存避免整数除法抹零；报文要的是整数 RTP 时间戳单位 |
| +16 | LSR | `stats.sr_ntp_mid`（当前恒 0） | 最近一个 SR 的 NTP 中间 32 位 |
| +20 | DLSR | `(now - sr_arr_ms) * 65536 / 1000`（当前恒 0） | 单位 1/65536 秒 |

### 5.4 音视频时钟不同（音频的坑）

- 视频：**90000 Hz**（H.264，`VIDEO_CLOCK_HZ`，见 `CGAPI.md:124`）。
- 音频：**48000 Hz**（Opus，RFC 7587 §4.1，`AUDIO_CLOCK_HZ`）。

jitter 的 `transit = 到达时刻 - RTP 时间戳` 必须在**同一时钟域**里算。如果音频
照抄 90000，jitter 会报成真值的 90000/48000 = **1.875 倍**假值。反过来，
`cm_media.c` 的 `jitterMs` 只对视频有意义：`(jitter_q4 >> 4) / 90`。

### 5.5 SRTCP 加密

与 PLI 完全同一条路径（`cm_cmd.c:276-280`）：

```c
if (cm_srtcp_ready(s)) {
    enc = cm_srtcp_protect(s, pkt, len, (int)sizeof(pkt));
    if (enc > 0) len = enc;
}
trans = enc > 0 ? "srtcp" : "plain";
```

- 密钥 ready → SRTCP（+4 字节 E+index，+10 字节 tag，共 +14）。
- 密钥未 ready → 退回明文，日志里 `%s` 打 `plain`（与 PLI 一致的做法）。
  明文 RTCP 在 SRTP 会话里多半会被网关丢弃，所以 `plain` 是个**需要关注的信号**，
  不是正常状态。

### 5.6 周期与日志

- 周期 `RR_INTERVAL_MS = 1000`（1Hz）。
- 日志限速 `RR_LOG_INTERVAL_MS = 10000`：首条必打，之后每 10 秒一条。
  每秒一条会把 PLI 和丢包告警埋掉，而那两条才是排障主线。
- 日志格式（两路的 lost/jitter 印在同一行，方便和 `pt histogram` 对账）：

```
[cgmedia] #1 RR sent (70 bytes, srtcp) rc=2 v[lost=0 jit=12] a[lost=0 jit=8]
```

---

## 6. 改动的 file:line 列表

所有行号为改动后（本次提交时）的实际行号。

### `PenCG/native/csrc/cgmedia/cm_cmd.c`

| 行号 | 内容 |
| --- | --- |
| 16–44 | 文件头"为什么补 RR"长注释。**★ 18 行明确写了"RR 不是 bug1 的修复方案"**，含第一版假设 + 三条反证 + 定位收窄 |
| 55 | `#define RR_INTERVAL_MS 1000` |
| 58 | `#define RR_LOG_INTERVAL_MS 10000` |
| 60 | `void cm_send_pli(cm_sess *s)`（**未改动**，作为 SRTCP 路径的参照） |
| 93 | `void cm_kf_tick(cm_sess *s)`（**未改动**） |
| 111–146 | RR 报文布局的块注释（word 编号 + 三个容易踩的点） |
| 155–219 | `static int rr_fill_block(...)`：填一个 24 字节 reception report block |
| 221–299 | `void cm_rtcp_tick(cm_sess *s)`：调度 + 组包 + SRTCP + 发送 + 限速日志 |
| 301 | `int cm_cmd_apply(...)`（**未改动**，只是行号后移） |

### `PenCG/native/csrc/cgmedia/cm_session.h`

| 行号 | 内容 |
| --- | --- |
| 120–158 | `cm_stats` 视频侧 RR 统计 + 说明注释（含"★ 注意定位：发 RR 是补齐协议义务，不是 bug1 的修复方案"） |
| 137 | `uint64_t vid_rx;`（received 口径，含重复/乱序包） |
| 138 | `uint32_t vid_ssrc_seen;` |
| 139–141 | `ext_seq_max` / `ext_seq_base` / `have_ext_seq` |
| 142–144 | `jitter_q4` / `last_transit` / `have_jitter` |
| 145–147 | `rtcp_sr_rx` / `sr_ntp_mid` / `sr_arr_ms` |
| 153 | `int have_sr;`（注释说明当前恒 0 是合法的） |
| 157–158 | `rr_expected_prior` / `rr_received_prior`（区间丢包率快照） |
| 160–176 | `cm_stats` 音频侧同构统计（`aud_rx` / `aud_ssrc_seen` / `aud_ext_seq_max` / `aud_ext_seq_base` / `have_aud_ext_seq` / `aud_jitter_q4` / `aud_last_transit` / `have_aud_jitter` / `aud_rr_expected_prior` / `aud_rr_received_prior`） |
| 241–248 | `cm_sess`：`next_rr_ms` / `rr_count` / `rr_log_ms` / `rr_last_lost` / `rr_last_jit` |
| 340 | `void cm_rtcp_tick(cm_sess *s);` 原型 |

### `PenCG/native/csrc/cgmedia/cm_rtp.c`

> 只**新增**统计，未改动任何现有丢包判定 / 关键帧闸门逻辑（`vid_lost`、
> `vid_dup`、`vid_reorder`、`vid_kf_req`、`cm_rtp_note_stale` 全部原样）。

| 行号 | 内容 |
| --- | --- |
| 40 | `#define RR_MAX_DROPOUT 3000u` |
| 41 | `#define VIDEO_CLOCK_HZ 90000u` |
| 45 | `#define AUDIO_CLOCK_HZ 48000u`（RFC 7587 §4.1） |
| 660–669 | 埋点说明注释（为什么 received 含重复包） |
| 670–675 | 通用内核说明注释 |
| 676–724 | `static void rr_note_seq_jitter(...)`：扩展最高序号 + 抖动，音视频共用 |
| 726–738 | `static void rr_note_video(cm_sess *s, uint16_t seq, uint32_t ts)` |
| 740–747 | `static void rr_note_audio(cm_sess *s, uint16_t seq, uint32_t ts)` |
| 783 | `s->stats.vid_ssrc_seen = ssrc;` |
| 804 | `rr_note_video(s, seq, cm_rd32(pkt + 4));`（放在 seq 判定**之前**，所以 received 含重复包） |
| 888 | `s->stats.aud_ssrc_seen = ssrc;` |
| 889 | `rr_note_audio(s, (uint16_t)((pkt[2] << 8) | pkt[3]), cm_rd32(pkt + 4));` |

### `PenCG/native/csrc/cgmedia/cm_media.c`

| 行号 | 内容 |
| --- | --- |
| 76 | `uint64_t jitter_ms = (s->stats.jitter_q4 >> 4) / 90ULL;`（新增局部） |
| 115 | stat JSON 的 `"jitterMs":%llu`（原先是硬编码 0） |
| 121 | stat JSON 新增 `"rrSent":%llu` |
| 127 | 参数表补 `(unsigned long long)jitter_ms` |
| 282 | `cm_kf_tick(s);`（原有） |
| 284–291 | `cm_rtcp_tick(s);` + 三行调用点注释（含"★ 定位"） |

### 未改动（门禁）

- `PenCG/src/**`：一行没动。
- `PenCG/native/csrc/cgmedia/cm_gst.c`：一行没动（audio-route 同事的写域）。
- `cm_srtp.c`：一行没动（不需要访问器，`cm_srtcp_ready` / `cm_srtcp_protect`
  已有原型且语义够用）。
- `cm_net.c` / `cm_ice.c` / `cm_dtls.c` / `cm_util.c` / `cgmedia.c`：一行没动。

---

## 7. bug1 的真正候选原因（按可能性排序）

每条给判定 + 证据。**(A) 是当前最强解释。**

### (A) 服务端挂机 kickout 定时器 —— **最可能（成立，但未做端到端真机验证）**

官方 SDK 里挖到了确切的机制（`.tmp/cgsdk/cg-sdk.891daeae.js`、
`.tmp/cgsdk/web.103b867a.js`，2026-11-13 重新 grep 确认）：

```
proto.msg.RtcKeepPlaying.CmdId = { NONE: 0, CMD_ID: 20015 }
  // 序列化只写字段 1（string）：gameToken
proto.msg.RtcKeepaliveCfg.CmdId = { NONE: 0, CMD_ID: 20021 }
  // 字段 1 = kickoutDuration (uint32 ms)、字段 2 = tipsDuration (uint32 ms)
```

- `keepPlaying()` 的实现：`if (!stateMachine.is(Connected)) return false;
  const e = new proto.msg.RtcKeepPlaying; e.setGameToken(webSessionInfo.gameToken);
  return sendTcpMessage(20015, e)`。
- `setKeepAliveConfig(kickOutMs, tipsMs)` 的实现：`if (kickOutMs <= tipsMs || <= 0) return false;
  i.setKickoutDuration(kickOutMs); i.setTipsDuration(tipsMs);
  return sendTcpMessage(20021, i)`。
- `sendTcpMessage` = `signalingChannel.sendProxy(pkt)`，即**走同一条 WebSocket**，
  帧格式：8 字节头（`r[0..3] = op` 大端，`r[4..7] = 长度` 大端）+ `Packet.toUint8Array()`；
  `Packet` 内部是 12 字节固定头：magic `0x45 0x67`(`'Eg'`) + cmdId(uint16 大端) +
  headLen(uint16) + msgLen(uint32 大端) + ... + magic tail `0x89 0xAB`(`0x89AB`)。
- 官方 `setReady()` → `sdkReadyInit()` 里调 `setKeepAliveConfig(kg(keepAlive.current), tipsMs)`。
  `keepAlive.current` 的来源是 `boxConfig.keep_alive_config`，其默认值：
  **`{ default: 5, list: [] }`，validator `1 <= v <= 30`（单位分钟）**。
  `kg` = `e => 1000 * 60 * e`，所以 **kickoutDuration = 5 分钟 = 300000 ms**。
  `d.zy`（tipsDuration）= `m` = **`2e4` = 20000 ms**（`web.103b867a.js` 模块 23340）。
- 官方**不是**周期心跳：`keepPlaying()` 只在 `onAnyAction()`（玩家有操作）时发。
  另外 `startGameDataKeepAlive()` 用 `keep_alive_duration`（默认 **15 分钟**）
  按 `setTimeout(..., 6e4)` 每分钟触发一次 `onAnyAction()`。
- **PenCG 一侧：`grep -rn 'keepPlaying|KeepPlaying|gameToken' PenCG/src/` 零命中，
  20015 / 20021 从未发出过。**

时间账：300s − 起播前容器冷启动 ~42s ≈ **258s ≈ 4m18s**，与三次复现
（4m19 / 4m18 / 4m18）完全吻合。

**判定：成立（机制与时间账都对得上）。未验证的部分：服务端在我们从没发
20021 时用的 kickout 默认值是否恰好也是 300s —— 这只能靠真机抓包或实测
"多久后停"来确认。注意 prompt 里提到"300s − 42s ≈ 4m18s"，但 42s 这个
冷启动值是估算，未做秒级验证。**

### (B) ticket 有效期 —— **不成立**

`PenCG/src/services/session.js:73-79` 的注释说 `time_left` 是"这张 ticket 的
**有效期秒数**"，不是可游玩时长，并给了实测证据：

```
// 证据：连取三次票，expires 恒为同一时间戳，而 time_left 依次 24/22/20，
// 精确等于 ceil(expires - now)。它只表示"还有多少秒可以用这张票去连网关"。
// 服务端**不下发单局时长限制**，玩家可以一直玩到主动退出。
```

实测 `time_left` = 24/22/20 秒，量级差 10 倍以上，**不可能是 4 分钟**。
停推后 WS 仍正常回显 `op=input`，说明认证/会话本身没被作废。

**判定：不成立。** 附带说明：注释里"服务端不下发单局时长限制"这句本身
**未验证**（只是"没观察到"），但即使它错了，(A) 也仍能独立解释停推。

### (C) 未建立 SCTP dataChannel —— **可能是加重因素，不是主因（未验证）**

- 网关下发 `op=input cmd=9` 形如 `9 42854,1,e46b,<base64>`，约 1 条/秒。
  官方 SDK 的 dataChannel 是 `rtcDataChannel`（SCTP），`RtcDataChannel.CmdId = 20010`。
- PenCG 侧：native 代码里 **grep 不到任何 `sctp` / `SCTP` / `datachannel`**；
  JS 侧 `play.vue:889-907` 的 `input` 分支只处理 head `1`/`2`，**其它一律
  `logWarn('input cmd 忽略: ...')`**，所以 `cmd=9` 全被丢掉。
- 反向证据：停推后网关**仍然**持续下发这些 `cmd=9`（`op=input` 每分钟 6 条
  横跨停流点不变），并未因为"dataChannel 没建起来"而中断信号通道；
  且三次复现的时间精度指向定时器而非"某条握手没完成"。
- 但要承认：`cmd=9` 的载荷解出 `{"m":"dataChannelRe...`，如果那是
  `dataChannelReconnect/ReRegister` 之类的**重连请求**，忽略它可能让网关
  认为我们"不在玩"。这条**没有任何验证证据**，不能当结论。

**判定：可能是加重因素（未验证），不是 4m18s 定时停推的解释。**

### (D) 未发 RTCP RR / 未回 Sender Report —— **不成立（作为 bug1 原因）**

- 已确认我们从不发 RR（本次改动补上）。但见 §2 三条反证：
  网关从不回 RTCP、音视频同停、时间精度 ±1s。
- 反过来说，如果 SFU 真用 RR 判活，它应该**回** RR，或者至少 SR；实测全空。

**判定：不成立（作为 bug1 原因）。RR 仍已补齐，理由见 §3。**

### (E) 服务端单局时长策略（VIP 是否真的无限） —— **无法判断**

- 用户自称有"无限游玩时长"，但我们**从未验证**服务端是否按 kickout 而非
  时长计费/计时。
- `mine-body.vue:251-269` 处理的是 `free_time_left`（免费时长），
  `auth.js:149,171` 的 `freeTimeLeft` 与 VIP 是两套字段；`play.vue:20-26`
  已把"本节剩余 N 分钟"改成"已玩时长"，说明单局上限没被确认过。
- 但 4m18s 这个量级与任何合理的商业时长策略都不符（免费时长通常是
  分钟级且会先弹提示；我们没收到任何提示 op）。

**判定：无法判断，但可能性低。** 若 (A) 的补救无效，可用"玩到 4m18s 前后
主动发一次交互（点击/按键）能否续命"来区分：(A) 会续命，时长限制不会。

### (F) DTLS/SRTP 密钥轮换失败 —— **不成立**

- SRTP 的 2^31 包上限：4 分钟内收 ~18701 包，差 5 个数量级。
- 若密钥失效，`cm_srtp_unprotect` 会持续失败 → `srtp auth fail` 日志与
  `authFail` 计数会暴涨。现场日志里没有任何 auth fail。
- 停推后 PLI 每 2s 仍以 `srtcp` 发出，说明发送侧密钥一直 ready。

**判定：不成立。**

### (G) ICE consent freshness（RFC 7675）—— **基本排除**

- consent 检查的 request 就是 STUN Binding Request（0x0001），我们**已应答**
  （`cm_net.c` 的 `rx_stun` / `rx_kept` 计数正常、ICE 保活未断）。
- 若 consent 真的过期，ICE 会进入 failed 并拆连接，`ws` 与 native 会话都会
  断——但实测 `ws=open`、native 会话仍活着。

**判定：不成立（基本排除）。** 残留的未验证点：`cm_ice.c:225-230` 在 peer
切换时只改 `s->peer`，不改 `s->cfg.remote_ip/port`，而 PLI/RR 都用
`cm_udp_send(..., s->cfg.remote_ip, s->cfg.remote_port)` 发。如果发生过 peer
切换，我们的 RTCP 会发到旧地址。但这**同样解释不了**"网关一个 RTP 都不发"
（那是下行方向）。

### 补充发现：**没有"收包停滞就主动重连/重建会话"的 native 机制**

- `cm_media.c` 的看门狗（`WATCHDOG_MS 180000` / `WATCHDOG_WARN_MS 60000` /
  `WATCHDOG_CHECK_MS 500`）只看 **JS poll + bytes_rx 进展**：`bytes_rx` 停滞
  本身**不触发任何动作**，要连续 180s 收不到 JS poll 才拆管线。
- JS 侧**现在有** STALLGUARD（`play.vue:180-214`、`play.vue:1270-1319+`）：
  `STALL_RX_MS = 20000`（rx 连续 20s 不涨）、`STALL_L1_WAIT_MS = 5000`、
  `STALL_MAX_TRIES = 3`，三级自愈（L1 补关键帧 → L2 复用网关重连 →
  L3 重新申请机位），并且明确用 `rx`（`stats.rtp_rx`）而不是 `rxPkts` 作判据
  （`rxPkts` 含 STUN/DTLS，停推后照样在涨）。
  → 所以"检测 + 自愈"这一层**已经补上了**，只是它治的是症状。
- 结论：native 侧确实没有重连机制；诊断/自愈由 JS 侧 STALLGUARD 承担。

---

## 8. 编译验证

```
cd PenCG
powershell -NoProfile -ExecutionPolicy Bypass -File native/build-media.ps1   # exit 0
powershell -NoProfile -ExecutionPolicy Bypass -File native/build.ps1         # exit 0
```

产物：

| 文件 | 字节数 | 变化 |
| --- | --- | --- |
| `PenCG/libs/libjsapi_cgmedia.so` | **84112** (82.1 KB) | 基线 78.8 KB |
| `PenCG/libs/libjsapi_cgnative.so` | **62200** (60.7 KB) | 未改动（同一份） |

warning：**零新增**。全量冷编（`ZIG_CACHE=/tmp/pencg-zig-final`）只剩一条
**既有基线**告警，与本次改动无关：

```
cm_gst.c:571:12: warning: address of function 'gst_debug_message_get'
  will always evaluate to 'true' [-Wpointer-bool-conversion]
```

（行号从 517 → 527 → 571 一路漂移，是因为 audio-route 同事持续在同一文件
前部加代码，内容始终是同一条。）

用 `zig cc -target aarch64-linux-gnu.2.17 -Wall -Wextra -fPIC -DPEN_CG_MEDIA=1 ...`
逐文件单独编译确认：`cm_cmd.c` / `cm_rtp.c` / `cm_media.c` / `cm_session.h`
**零 warning**。

---

## 9. 真机验证方法

### 9.1 部署

```bash
ADB="./env/adb/adb.exe -s 9D91500007505171"
$ADB push PenCG/libs/libjsapi_cgmedia.so /userdata/libs/libjsapi_cgmedia.so
$ADB shell sync
```

（`libjsapi_cgnative.so` 未改动，可以不推。）

### 9.2 抓日志

```bash
# 日志文件不能写死：它是 miniapp 进程的 stdout（2026-11-14 真机定论）
P=$($ADB shell 'pidof miniapp' | tr -d '\r')
L=$($ADB shell "readlink /proc/$P/fd/1" | tr -d '\r')
echo "LOG=$L"
$ADB shell "grep -a -e '\[pencg\]' -e '\[cgmedia\]' \"$L\"" > bug1-rr.txt
```

`[cgmedia]` 前缀来自 `cm_util.c:19-29` 的 `cm_log()`（`fprintf(stdout, ...)`）。
**它和 JS 的 `[pencg]` 必落同一个文件**：两者都跟着 miniapp 进程的 stdout 走，
进程只有一个 stdout（2026-11-14 已确认，原先「两条独立通道、落哪个文件待确认」的说法已作废）。
顺带更正：`loli_s6_dev.txt` 实测只有 1 字节、是空文件，真日志在 `DictPen_YYYYMMDD_HHMMSS.log`。

### 9.3 必须看到的日志串

**（a）RR 通道确实起来了（起播后 1 秒内就必须出现）**

```
[cgmedia] #1 RR sent (70 bytes, srtcp) rc=2 v[lost=0 jit=12] a[lost=0 jit=8]
```

判定要点：

- `srtcp` —— 如果是 `plain`，说明 SRTCP 密钥没 ready，需要排查。
- `rc=2` —— 音视频两段都在。`rc=1` 说明某一路没收到包（看后面是 `a[` 还是 `v[` 缺失）。
- `bytes` = 70（SRTCP, 2 block）/ 56（明文, 2 block）/ 46（SRTCP, 1 block）/ 32（明文, 1 block）。

**（b）之后每 10 秒一条**（限速生效，不会淹日志）

```
[cgmedia] #1 RR sent (70 bytes, srtcp) rc=2 v[lost=3 jit=41] a[lost=0 jit=9]
[cgmedia] #1 RR sent (70 bytes, srtcp) rc=2 v[lost=5 jit=38] a[lost=1 jit=7]
```

**（c）stat JSON 里能看到 `rrSent` 与真实 `jitterMs`**（JS probe 每 20s 打一行）

```
[play] probe stage=live ... rx=18701 fps=29 ... 
```

native stat 事件（每 2s，`cm_media.c:110-121`）现在包含：

```json
{"t":"stat", ..., "jitterMs":12, ..., "rtcpIn":0, "rrSent":240}
```

- `rrSent` 应该等于"播放秒数"（1Hz）。若它停在某个值不动 = tick 没被调用。
- `jitterMs` 从 0 变成几毫秒到几十毫秒 = 抖动统计接通了。
- `rtcpIn` 一直是 0 = 网关从不回 RTCP（与历史一致）。

**（d）对账：`rrSent` 与 `rx` 同步停住，仍是 bug1**

停推之后：

```
[cgmedia] #1 RR sent (70 bytes, srtcp) rc=2 v[lost=0 jit=0] a[lost=0 jit=0]
[cgmedia] #1 PLI sent (26 bytes, srtcp)
```

`rrSent` **继续每秒涨**（我们照发），`rx` / `pt histogram` 冻结 —— 这正是
"我们从没停止上报，是网关单方面停推"的**直接证据**。这一条日志本身就有
独立的排障价值：它把"我们挂了"这个可能性彻底排除。

### 9.4 每次发送打日志？不，限速

只有首条 + 每 10 秒一条。如果你想看**每一条**，临时把
`cm_cmd.c:58` 的 `RR_LOG_INTERVAL_MS` 从 `10000` 改成 `0`（每次都比较
`now - rr_log_ms >= 0`，恒真），重编即可。**不要提交这个改动。**

---

## 10. 如果 RR 不是根因（fallback 计划）

已确认 RR 不是 bug1 的修复方案（§2/§7D），所以"如果 RR 不是根因"是**预期
路径**，不是意外。按优先级：

### F1. 实现挂机保活（20015 RtcKeepPlaying / 20021 RtcKeepaliveCfg）—— 直击 (A)

这是**最应该做的下一步**，但它跨了本次门禁（要改 `PenCG/src/**`，
涉及 WS 帧格式与 protobuf 手写编解码），所以本次只给出规格，不实现。

规格（从官方 SDK 逆向，`cg-sdk.891daeae.js`）：

```
WS 帧 = 8 字节头 + Packet
  头: [0..3] op(uint32 大端)  [4..7] payload 长度(uint32 大端)
      op: 1 = signaling, 2 = proxy, 3 = handshake, 4 = keepalive(空串)
      注：我们的 keepAlive 走的是 op=0 + "input" JSON，与官方 op=4 不同路径
Packet (toUint8Array):
  [0..1]  0x45 0x67         magic 'Eg'
  [2..3]  cmdId  (uint16, 大端)  -> 20015 / 20021
  [4..5]  headLen (uint16, 大端) -> 我们填 0
  [6..9]  msgLen  (uint32, 大端)
  [10..]  protobuf message
  [末2]   0x89 0xAB           magic tail
```

`RtcKeepaliveCfg` 的 protobuf（字段号 1/2，均 uint32，非零才写）：

```
0x08 <varint kickoutDuration_ms>   // 300000 -> 08 E0 A9 12
0x10 <varint tipsDuration_ms>      // 20000  -> 10 A0 9C 01
```

`RtcKeepPlaying`（字段号 1，string gameToken）：

```
0x0A <varint len> <gameToken 字节>
```

`gameToken` 从哪来：`webSessionInfo.gameToken`，即 offer/answer 阶段服务端
下发的会话信息里的字段 —— **这个还没有在我们代码里定位过（未验证）**，
需要先在 `media.js` 的 offer 解析里找到它。

时序建议：`setKeepAliveConfig(300000, 20000)` 在"已 Connected"后发一次；
`keepPlaying()` 在**每次玩家操作**以及**每分钟兜底**各发一次
（官方是 `onAnyAction` + `startGameDataKeepAlive` 每分钟兜底）。

### F2. 把 `cmd=9`（dataChannel 相关下行）解出来看语义 —— 针对 (C)

`play.vue:889-907` 现在把 `cmd=9` 全丢了。下一步应该：

1. 把 `9 42854,1,e46b,<base64>` 的 base64 完整解出来，
   确认是不是 `{"m":"dataChannelRe..."}` 的完整 JSON，看清 `m` 到底是什么
   （Reconnect / Register / RegisterAck / RequireDataChannel…）。
2. 若语义是"请建立 dataChannel / 请回应"，评估是否要在 answer 里加
   `m=application` + SCTP，以及 native 是否要支持 SCTP —— 这个工程量很大
   （native 目前完全没有 SCTP 栈），**只有在 F1 无效后才值得投入**。

### F3. 用"主动交互续命"实验区分 (A) 与 (E)

一个**零代码**的判别实验：

- 正常玩到 3 分钟，然后**手动点击/按键一次**（触发我们已有的
  `op=input cmd="2 ..."` 触摸帧上行）。
- 如果停推时间因此**推后**到"最后一次交互 + 4m18s" → 坐实 (A) 挂机计时。
- 如果仍然是起播 + 4m18s 就停 → 指向 (E) 单局时长策略，需要去查账号权益。

这个实验成本最低，建议**优先做**。

### F4. 若入站 SRTCP 能解密，把 LSR/DLSR 补上

等哪天 `cm_srtp_unprotect()` 支持 RTCP（`cm_srtp.c:337` 现在直接 `return -4`），
`stats.have_sr` 就能置 1，RR 的 LSR/DLSR 自动变成实值，网关就能算 RTT。
**报文构造函数不用改** —— 这是 `cm_rtcp_tick` 里预留好的。收益是"更完整的
RR"，与 bug1 无关。

### F5. 在 JS 侧把"停推"变成一个明确的用户可见状态

STALLGUARD 已经能检测（20s）+ 三级自愈（3 次）。如果 (A) 是真的，
自愈的上限（3 次）会在 ~13 分钟内烧完。可以考虑：

- 自愈次数用尽后，把 `statusMain` 文案改成"云端已断开（长时间无操作）"，
  并给一个"继续游戏"按钮（点击即重连 + 触发一次 `keepPlaying`）。
- 这些都是 `PenCG/src/**` 的活，不在本次门禁内。

---

## 11. 未验证事项汇总（诚实清单）

| # | 内容 | 状态 |
| --- | --- | --- |
| 1 | 服务端在我们从未发 20021 时使用的 kickout 默认值是否恰好 300s | **未验证**（推断自 `keep_alive_config.default = 5` 分钟） |
| 2 | "起播前容器冷启动 ~42s"这个值 | **未验证**（估算），时间账 300−42≈258s 依赖它 |
| 3 | `cmd=9` 载荷里 `{"m":"dataChannelRe...` 的完整语义 | **未验证**（被截断） |
| 4 | `gameToken` 在我们代码里的来源字段 | **未验证**（还没在 offer 解析里定位） |
| 5 | 入站 RTCP 是否真的存在（我们不解密，`CM_LOGD` 默认也不打） | **未验证**，需要 `setLogLevel 4` 后重测 |
| 6 | `session.js:73-79` 注释里"服务端不下发单局时长限制" | **未验证**（只是没观察到） |
| 7 | `[cgmedia]` 日志落哪个文件 | **已确认（2026-11-14）**：miniapp 进程 stdout，与 `[pencg]` **同一文件**；实测 `loli_s6_dev.txt` 是 1 字节空文件，真日志在 `DictPen_YYYYMMDD_HHMMSS.log` |
| 8 | `cm_ice.c:225-230` 的 peer 切换是否会让 PLI/RR 发到旧地址 | **未验证**（代码读出来是这样，但没实测） |
| 9 | RR 报文是否被网关接受（它不回 RTCP，无法从回包确认） | **无法验证**；只能从"没有 error/异常日志 + 停推时间没变"间接判断 |

---

## 12. 一句话结论

**RR 的"从未发送"这个事实成立，但它不是 bug1 的根因。**
bug1（4m18s ± 1s 网关静默停推、WS 健康）指向服务端挂机 kickout 定时器
（官方 `RtcKeepPlaying` 20015 / `RtcKeepaliveCfg` 20021，PenCG 零实现）。
RR 已经按 RFC 3550 §6.4.2 补齐（音视频双 report block、1Hz、走 SRTCP、
真实丢包率与抖动、LSR/DLSR 预留），定位是**补齐协议义务**，预期对 bug1
零收益、零副作用。
