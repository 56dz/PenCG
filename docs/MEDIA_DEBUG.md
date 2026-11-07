# PenCG 媒体层攻坚纪实

本文记录 `libjsapi_cgmedia.so` 从「握手超时」到「真机出画 + 出声」的全过程，
以及途中定位的每一个真 bug。这些都是真机实测结论，不是推测。

> 项目主页见 [../README.md](../README.md)；分层架构见 [ARCH.md](ARCH.md)。

---

### 音频：从「有包无声」到「PCM RUNNING」

音频链路的每一环都在真机上取证过，最终定位到 **4 个叠加的根因**：

| # | 根因 | 判据 |
|---|---|---|
| A1 | rk817 `Playback Path` 开机默认 `OFF` | `amixer -c 0 cget name='Playback Path'` → `: values=0`；`cset 'SPK'` 后 `values=2` |
| A2 | card1（`rockchipaw883xx` 功放）`Master Playback Volume` = `0,0` | `amixer -c 1 cset numid=54 200,200` 后才有声音 |
| A3 | **appsrc caps 只写 `audio/x-opus`**，opusdec 两支 sink 模板同时命中 → 协商不出唯一结果 → `GST_FLOW_NOT_NEGOTIATED` | bus 报 `Internal data stream error (gst_base_src_loop)`、`The stream is in the wrong format`；PCM 停在 `state: OPEN`、`hw_ptr=0` |
| A4 | 进程内 `alsasink` 必报 `wrong format`（同 pipeline 在 adb shell 里却完全正常） | 改走 PenBili/PenDouyu 的 `fdsink` + 外部 `aplay` 方案，彻底绕开 |

修完 A3 之后：`[cgmedia] both: aplay spawned pid=32473` → `/proc/asound/card1/pcm0p/sub0/status` 变 **`state: RUNNING`**、`hw_ptr` 450280→546968→643560→740040 递增、bus 错误清零。

A3 的关键细节（**写在这里防止再踩**）：`gst-inspect-1.0 opusdec` 的 sink 模板是两支——
`audio/x-opus, channel-mapping-family: 0` 与
`audio/x-opus, channel-mapping-family: [1,255], channels, stream-count, coupled-count`。
只给 `audio/x-opus` 会同时命中两支、caps 交集不唯一，`appsrc` 就以 `NOT_NEGOTIATED` 收场，
而 `appsrc push` 本身**照样返回 OK**，不挂 bus 监视器根本看不见错误。必须写成
`audio/x-opus,rate=48000,channels=2,channel-mapping-family=0`（网关 fmtp 是 `sprop-stereo=1`）。

顺带记两个复用性很高的坑：
- **`amixer` 枚举解析**：必须两步——先建 `Item #n -> 名字` 表，再读 `: values=N` 定位当前名。
  直接取「第一个 `Item #` 行」永远读到 `OFF`（Item #0 恒为 OFF）；PenBili v2.3.2 踩的是「第一对单引号」。
- **宿主 `SIGCHLD=SIG_IGN`**：`popen`/`pclose`/`system` 返回 -1 但子进程照常执行 →
  所有命令改成 `(void)system(...)` + **回读产物/状态自证**，不能用返回值判成功。

### 媒体层又修掉的 6 个真 bug

除了上面 `bug 1..4`，端到端联调阶段又定位并修复了 6 个：

| # | bug | 现象 | 根因 |
|---|---|---|---|
| 5 | `apply_rect()` 面板 y 起点算错 | 画面跑到面板最右侧 | 逻辑 `(x,y)` → 面板 `(107+y, 960-x)`，矩形 y 起点应为 `960-(x+w)`，原来写成 `rect_y` |
| 6 | pipeline 完全没有旋转 | 画面顺时针转 90° 且被拉伸 | plane 76 的 KMS `rotation` 属性只有 `rotate-0`/`reflect-y`，无 90/270，必须在解码器里转：`mppvideodec rotation=270` |
| 7 | `KMS_SINK_PROPS` 漏了前导 `!` | 管线 PLAYING 但屏幕全黑 | 拼成 `... ! videoconvert kmssink ...`，`kmssink` 根本没接进链路，preroll 永远完不成（`state=3 pending=4`） |
| 8 | `GST_STATE_NULL_` 常量写成 0 | 退出游戏后画面残留在屏上 | GStreamer 枚举是 `VOID_PENDING=0, NULL=1, READY=2, PAUSED=3, PLAYING=4`，传 0 等于传非法目标态，`set_state` 失败 |
| 9 | `cm_udp_pump_media()` 把 ICMP 错误当致命 | 首帧一推出媒体循环就退出 | connected UDP socket 上 ICMP port-unreachable 会让 `recvfrom` 返回 `ECONNREFUSED`，必须当瞬态处理 |
| 10 | `emit_media_if_needed()` 要求音频就绪 | 遮罩永不消失、点击全被闸门丢弃 | 网关常常只发视频不发音频；改成三态 `media_ev_sent`（视频到即发，音频晚到补发） |

另有两个协议级修复：
- **region 写死**：原本给所有游戏传手游 region 数组，端游全返 `1288 请重新测速`
  （真实语义是「这些 region 没有该游戏的容器」）。改为用 `listMediaServers(gameCode)` 查真实可用区。
- **487 Role Conflict**：`cm_ice.c` 的 ERROR-CODE 解码把 class 位算错（487 被读成 787），
  且完全没处理 487。已按 RFC 8445 §7.2.5.1 加 controlled 角色切换。

### 输入卡了两天，最后是两个自己引入的 bug（务必记住）

| # | bug | 现象 | 根因 |
|---|---|---|---|
| 11 | `onTS` 里 `var ts = (e && e.changedTouches) \|\| [];` 被误删 | **屏幕怎么点都没反应**（用户亲手点也无效） | 清理临时诊断日志时把这一行连带删了，`onTS` 引用未声明的 `ts` → 每次 `touchstart` 抛 ReferenceError → `down` 一次都没发出去；`touchend` 里 `touchIds` 因此为空，`up` 也被跳过。**JS 异常只进日志、不阻断画面，所以看起来像"输入协议不对"** |
| 12 | native 看门狗 `now - s->last_poll_ms > WATCHDOG_MS` 无符号下溢 | 对局中途被强拆（`JS heartbeat lost (18446744073709551615 ms)`） | `last_poll_ms` 由 JS 线程刷新、`now` 在媒体线程取，取完 `now` 后 JS 又写入更大的值 → `now - last_poll_ms` 下溢成 2^64-1，立刻判定超时。改为 `s->last_poll_ms && now > s->last_poll_ms && (now - s->last_poll_ms) > WATCHDOG_MS`。**`volatile` 防不住这种语义竞态** |

两条的共性：**都不在"协议"上，而在自己代码里，且都被"画面正常"掩盖了**。教训是
「异常必须显式上报到 UI」，以及「跨线程比较时间戳必须防下溢」。

### 媒体层调试纪实：从「握手超时」到「一键一字节」

`cgmedia` 的 ICE / 证书 / DTLS / SRTP / RTP 拆包 / GStreamer 管线全部自己实现，全链路在真机逐步打通。
过程中修掉 4 个真实 bug，每一个都不是靠猜出来的：

| 环节 | 状态 |
|---|---|
| HTTP 登录 / v2 加密接口 / 申请机位 | ✅ |
| 网关 WSS 连接 + 明文 auth + 加密收包 | ✅ |
| 提交 answer，网关回 `op=result`（容器启动） | ✅ |
| STUN Binding + USE-CANDIDATE 提名 | ✅ |
| DTLS 握手（DTLSv1.2 / ECDHE-RSA-CHACHA20-POLY1305） | ✅ |
| SRTP 密钥导出（`SRTP_AES128_CM_SHA1_80`） | ✅ |
| SRTP 解密 + H.264 成帧 + kmssink 出画 | ✅ **`RESULT: MEDIA OK`** |

真机最终结果（`/tmp/cgm.log`，2026-11-07 08:36）：
```
[cgmedia] #1 DTLS OK version=DTLSv1.2 cipher=ECDHE-RSA-CHACHA20-POLY1305 srtp=SRTP_AES128_CM_SHA1_80
[cgmedia] #1 first video frame pushed (61016 bytes, 1280x720, key=1)
*** MEDIA EVENT {"t":"media","video":true,"audio":true,"videoW":1280,"videoH":720,"fps":60}
---- summary ----
rtp_rx=6850 bad=0 bytes=7559462 frames=289 nal=289 auth_fail=0
fps=0 1280x720 kf=1 pli=11 pushed=289
RESULT: MEDIA OK
```
`auth_fail` 从 2594 归零，289 帧 H.264 全部推出，17 秒真实游戏画面（`bytes=7559462` ≈ 7.2 MB）。

**bug 1 · MTU 查询被关掉**：`SSL_OP_NO_QUERY_MTU` 之后没有调 `SSL_set_mtu()` →
`dtls1_query_mtu()`（`ssl/d1_lib.c:892`）返回 0 → `dtls1_do_write()`
（`ssl/statem/statem_dtls.c:120`）第一条记录就 `SSL_ERROR_SYSCALL`，**一个字节都发不出去**。

**bug 2 · UDP socket 没 connect**：OpenSSL dgram BIO 写报文走 `sendmsg` 且 `msg_name==NULL`，
内核直接 `errno=89 EDESTADDRREQ`。`BIO_CTRL_DGRAM_SET_PEER` 只让 BIO 记住地址，**不能替代
`connect()`**；而且 `connect()` 必须在 ICE 结束之后（`cm_ice_run` 会因网关回包源地址不同而切换 peer）、
DTLS 开始之前。

**bug 3 · STUN keepalive 污染 DTLS 记录层**：ICE 通了以后网关每 1 到 2 秒发一个 STUN Binding
Request，首字节 `0x00` 被 DTLS 当成非法的 `rr->type`，`dtls1_get_record` 静默丢弃。表现是
「记录头回调 14 次、握手报文回调 0 次」。解法是收包前用
`recvfrom(..., MSG_PEEK | MSG_DONTWAIT)` 分类：STUN/垃圾消费掉（STUN 当场按 RFC 5389 应答
`0x0101` + `XOR-MAPPED-ADDRESS` + `MESSAGE-INTEGRITY` + `FINGERPRINT`，MI 的 key 用**我方**
`ice-pwd`），DTLS 记录不消费留给 OpenSSL dgram BIO 自己读。修完 `hdr=7 badHdr=0 retx=0`。

**bug 4 · `SSL_export_keying_material` 的标签长度写错（最有价值的一个）**：
```c
/* 修复前 */
SSL_export_keying_material(ssl, d->km, KM_LEN, "EXTRACTOR-dtls_srtp", 21, NULL, 0, 0);
```
`strlen("EXTRACTOR-dtls_srtp") == 19`，而这里传了 `21`。这个长度直接进 TLS 1.2 PRF 的 seed：
`seed = client_random ‖ server_random ‖ [label_len(2B) ‖ label ‖ context]`。
差 2 字节 → seed 完全不同 → 导出的 60 字节 keying material 完全不同 → **每个 SRTP 包
HMAC 全部验不过**（真机 `auth_fail=2594 / rxPkts=2917`），而 DTLS 握手本身毫无异常。
修复改成 `sizeof("EXTRACTOR-dtls_srtp") - 1`。

这条 bug 藏得深的原因值得记下来：离线自检 `dselftest` 的服务端**用了同一个 21**，两端犯同样的错，
导出的 KM 反而一致，`PASS1: identical keying material` 照样通过。**对称的错误永远暴露不出来。**
现在的自检里加了 `PASS4`：同一会话、同一 label 字面量，只把 label_len 从 19 改成 21，断言两者
导出的 KM 必须不同；服务端侧**故意写死 19**，这样客户端写错时 `PASS1` 会立刻失败。
