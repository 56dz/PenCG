# PenCG 媒体层技术文档（`libjsapi_cgmedia.so`）

> 面向后续维护者。本文记录**已经真机跑通**的媒体链路、每个环节的原始日志证据、
> 以及 6 个踩过的 bug 的完整纪实。所有"证据"段落都是真机日志原文，未改写。

产物：`PenCG/libs/libjsapi_cgmedia.so` —— 63832 字节（62.3 KB），
md5 `3606a6460438b7af4800c2eff8d84747`，交叉编译目标 aarch64-linux-gnu。
模块名 `cgmedia`，用 `import * as gm from 'cgmedia'` 引入。

---

## 1. 完整链路

```
WS auth (明文 {"op":"auth",...})
  └─> 网关推 {"op":"offer","data":{"sdp":...}}
        └─> media.js parseSdp()  →  {remoteIp, remotePort, ufrag, pwd, videoPt, audioPt, ...}
        └─> media.js buildAnswer()  →  answer SDP（a=setup:active / a=recvonly /
        │                              a=rtcp-mux / 我方真实证书指纹）
        └─> ws send {"op":"answer","data":{"sdp":answer}}     ← 不回 answer 网关永远不给流
        └─> cgmedia.start(cfgJson)  →  {"id":1}
              │
              ├─ 1. cm_net.c      bind 0.0.0.0:0（本地端口，非阻塞）
              ├─ 2. cm_ice.c      STUN Binding Request ×10（300ms 间隔）
              │        attrs: USERNAME(0x0006)="<remote_ufrag>:<our_ufrag>"
              │               PRIORITY(0x0024)=1845501695
              │               ICE-CONTROLLING(0x8029)=0x7F000000/0x11223344
              │               USE-CANDIDATE(0x0025) len=0     ← 关键，见 bug ③
              │               CHANGE-REQUEST(0x000C) 偶数次
              │        MESSAGE-INTEGRITY → FINGERPRINT → 收 0x0101 = connected
              ├─ 3. cm_net.c      connect(peer)                    ← 关键，见 bug ②
              ├─ 4. cm_dtls.c     DTLS 1.2 client handshake（PEM 自签证书内嵌 cm_cert.h）
              │        SSL_CTX_set_tlsext_use_srtp("SRTP_AES128_CM_SHA1_80")
              │        BIO_new_dgram + SET_PEER + SET_CONNECTED + SET_MTU(1200)
              │        SSL_set_options(SSL_OP_NO_QUERY_MTU) + SSL_set_mtu(1200)  ← bug ①
              │        握手期间用 cm_udp_prepare_dtls() 把 STUN 挑出去并应答  ← bug ③
              ├─ 5. cm_dtls.c     SSL_export_keying_material(km,60,
              │                      "EXTRACTOR-dtls_srtp", strlen=19, NULL,0,0)  ← bug ④
              │        km[0..15]=client_write_key  km[16..31]=server_write_key
              │        km[32..45]=client_write_salt km[46..59]=server_write_salt
              ├─ 6. cm_srtp.c     srtp_init(rx, km+16, km+46)  我们是 client，收流用 server_write_*
              │                   srtp_init(tx, km+0 , km+32)  发 PLI 用 client_write_*
              │                   AES-CM PRF(label 0=key / 2=salt / 1=auth，iv[7]^=label)
              └─ 7. cm_media.c    主循环 20ms：
                       cm_udp_pump_media() 按首字节分发（STUN 应答 / 迟到 DTLS 丢 / RTP·RTCP 返回）
                       → cm_srtp_unprotect()（先验 HMAC-SHA1 前 10B，再 AES-CTR 解密）
                       → cm_rtp_on_packet() 按 PT 分流（98=H264 / 111=Opus，其它丢）
                       → H264: STAP-A(24) 拆 / FU-A(28) 重组(start 补 NAL 头, end flush)
                               / 单 NAL(marker flush) → Annex-B → cm_box_push()
                       → Opus: 裸包直接 cm_box_push()
                       → cm_kf_tick() 缺 IDR 就发 RTCP PLI（SRTCP 加密，26 字节）
                       → 每 2s emit_stat()

   视频：appsrc(name=vsrc, is-live, format=time, do-timestamp)
         caps=video/x-h264,stream-format=byte-stream,alignment=au
         ! h264parse ! mppvideodec ! videoconvert
         ! kmssink(name=vsink, plane-id=76, driver-name=rockchip,
                   force-aspect-ratio=false, can-scale=true, sync=true)

   音频：appsrc(name=asrc, is-live, format=time, do-timestamp)
         caps=audio/x-opus,rate=48000,channels=2
         ! opusdec ! audioconvert ! audioresample ! alsasink(name=asink)
```

数据流方向要点：**我们是 DTLS client（answer 里 `a=setup:active`）**，所以
收流用 `server_write_key/salt`，发送（PLI）用 `client_write_key/salt`。

---

## 2. 真机证据（原文日志）

### 2.1 ICE

```
[cgmedia] start #1 -> 112.90.156.147:48352 ufrag=a86dff46 pt=98/111 rect=0,0,960,266
[cgmedia] udp bound 0.0.0.0:54234 -> 112.90.156.147:48352
[cgmedia] #1 STUN binding #1 sent
[cgmedia] #1 STUN Binding Success from 112.90.156.147:48352 mapped=163.125.229.253:4...
[cgmedia] #1 USE-CANDIDATE confirm #1 sent
[cgmedia] #1 USE-CANDIDATE confirm #2 sent
*** STUN BINDING SUCCESS {"t":"ice","state":"connected","localPort":46258,"mapped":"163.125.229.253:..."}
[cgmedia] #1 ICE connected local_port=46258
```

### 2.2 STUN keepalive 应答（bug ③ 的验收证据）

```
[cgmedia] #1 drained STUN 80 bytes pre-DTLS (answered)
[cgmedia] #1 STUN keepalive reply sent (64 bytes, #1)
[cgmedia] #1 drained STUN 92 bytes pre-DTLS (answered)
[cgmedia] #1 STUN keepalive reply sent (64 bytes, #2)
[cgmedia] #1 STUN type=0x0011 (ignored in keepalive path)
[cgmedia] #1 drained 4 pre-DTLS datagrams
[cgmedia] #1 socket connected to peer
```

### 2.3 DTLS

握手报文（`msg_cb` 打印，含记录头 hex）：

```
[cgmedia] #1 DTLS >> ? len=13 type=-1
[cgmedia] #1 DTLS >> ? len=203 type=1        ← ClientHello
[cgmedia] #1 DTLS hdr[1] ct=22 ver=fefd epoch=0000 len=110 raw=16fefd0000000000000000006e
[cgmedia] #1 DTLS << ? len=110 type=2        ← ServerHello
[cgmedia] #1 DTLS << ? len=457 type=11       ← Certificate
[cgmedia] #1 DTLS << ? len=213 type=12       ← ServerKeyExchange
[cgmedia] #1 DTLS << ? len=66 type=13        ← CertificateRequest
[cgmedia] #1 DTLS << ? len=12 type=14        ← ServerHelloDone
[cgmedia] #1 DTLS >> ? len=791 type=11       ← 我方证书
[cgmedia] #1 DTLS >> ? len=78 type=16
[cgmedia] #1 DTLS >> ? len=272 type=15
[cgmedia] #1 DTLS >> ? len=24 type=20        ← Finished
[cgmedia] #1 DTLS hdr[6] ct=20 ver=fefd epoch=0000 len=1
[cgmedia] #1 DTLS hdr[7] ct=22 ver=fefd epoch=0001 len=40
[cgmedia] #1 DTLS OK version=DTLSv1.2 cipher=ECDHE-RSA-CHACHA20-POLY1305 srtp=SRTP_AES128_CM_SHA1_80
*** DTLS HANDSHAKE OK {"t":"dtls","state":"connected","profile":"SRTP_AES128_CM_SHA1_80"}
[cgmedia] #1 keying material (60 bytes) = 7d659857731bd26dc14380480254fea5...
*** SRTP KEYING READY km8=7d659857731bd26d
*** record-layer: hdr=7 badHdr=0 retx=0 in_msgs=7 in_bytes=883
*** srtp profile: SRTP_AES128_CM_SHA1_80
```

> 注：`cipher` 是透明加密层（CHACHA20-POLY1305），与 SRTP 无关；
> **SRTP 用的是 DTLS-SRTP 协商出的 `SRTP_AES128_CM_SHA1_80` 扩展**，两者互不影响。

### 2.4 SRTP + RTP

```
[cgmedia] #1 srtp ready (rx=server_write tx=client_write)
[cgmedia] #1 SPS reports 1280x720
*** RTP RX first packet pt=98 seq=7201 payload=... nal=24
*** RTP RX first packet pt=98 seq=7202 payload=... nal=28 (FU-A start=1 end=0 real=5)
*** NAL type=5 (IDR) first at seq=7202
[cgmedia] #1 first video frame pushed (61016 bytes, 1280x720, key=1)
*** MEDIA EVENT {"t":"media","video":true,"audio":true,"videoW":1280,"videoH":720,"fps":60}
[t=5s]  pkt=1892 rx=1892 frames=114 kf=1 pli=3 fps=22 1280x720 authfail=0 pushed=114
[t=10s] pkt=3810 rx=3810 frames=181 kf=1 pli=6 fps=22 1280x720 authfail=0 pushed=181
---- summary ---- rtp_rx=6850 bad=0 bytes=7559462 frames=289 nal=289 auth_fail=0
fps(prod)=0 fps(measured)=22 1280x720 kf=1 pli=11 pushed=289 gst=PLAYING
*** frames=289 idr=1 kf=1
RESULT: MEDIA OK
EXIT=0
```

### 2.5 GStreamer / KMS 出画

```
GST_STATE_CHANGE_ASYNC 卡死（bug ⑤ 修复前）：
== [after_create] get_state r=2 state=3 pending=4
== [pace] after 15..165: r=2 state=3 pending=4

修复后：
== [pace] after 15: r=1 state=4 pending=0
[cgmedia] kmssink rect logical=<0,0,473,266> panel-><107,0,266,473>
[cgmedia] video pipeline PLAYING
```

DRM 平面证据（`/sys/kernel/debug/dri/0/summary`）：

```
plane[54]: Esmart0-win0  crtc=video_port0 ... zpos:0   ← weston 的 UI 面
plane[?:  Esmart1-win0  ACTIVE ... zpos: 2
        dst: pos[107, 0] rect[266 x 473]
        src: pos[0, 0] rect[473 x 266]
        buf[0]: addr: 0x0000000000d58000 pitch: 1280 offset: 0
        buf[1]: addr: ...                offset: 921600        ← NV12 两个 plane
```

截图（设备无 `screencap`，只能从 KMS 平面读）：

```
gst-launch-1.0 -q kmssrc plane-id=76 num-buffers=1 ! videoconvert ! pngenc \
    ! filesink location=/userdisk/miniapp/data/media_ok.png
→ 197437 字节 / 1280x720 PNG，采样 1268 种颜色（testsrc2 彩条）
```

---

## 3. 六个已修 bug 的完整纪实

### ① MTU：设了 `SSL_OP_NO_QUERY_MTU` 就必须显式 `SSL_set_mtu`

- **症状**：`SSL_do_handshake` 立刻返回 -1，`SSL_get_error`=5（`SSL_ERROR_SYSCALL`），
  `errno=0`，错误队列为空，**ClientHello 一个字节都发不出去**。
- **根因**：`SSL_OP_NO_QUERY_MTU` 关掉了 OpenSSL 向 BIO 查 MTU 的行为，但没喂替代值，
  `s->d1->mtu` 保持 0，写第一条记录就失败。
- **坑**：`SSL_set_mtu` / `DTLSv1_get_timeout` / `DTLSv1_handle_timeout` /
  `SSL_OP_NO_QUERY_MTU` **在头文件里都是宏**（走 `SSL_ctrl`：
  `SSL_CTRL_SET_MTU=17`、`DTLS_CTRL_GET_TIMEOUT=73`、`DTLS_CTRL_HANDLE_TIMEOUT=74`；
  `SSL_OP_NO_QUERY_MTU=0x00001000U`）。按**符号名**在 `.so` 里查会误判"设备没有这些 API"，
  从而漏掉修复。`SSL_ctrl` / `SSL_set_options` 才是真符号。
- **修复**（`cm_dtls.c`）：`SSL_set_options(ssl, SSL_OP_NO_QUERY_MTU);` 之后紧跟
  `SSL_set_mtu(ssl, DTLS_MTU);`（1200）。

### ② UDP socket 必须真 `connect()`

- **症状**：STUN 拿到 0x0101、`BIO_CTRL_DGRAM_SET_PEER` 也设了，但握手报
  `handshake fail e=5 r=-1 errno=89(Destination address required)`。
- **根因**：`BIO_CTRL_DGRAM_SET_PEER` 只是把地址**记在 BIO 里**，OpenSSL 写记录时
  走的是 `sendmsg()`，并未填 `msg_name` —— 因为 kernel 认为这是一个"已连接"的 socket。
  socket 没 `connect()` 时内核直接 `EDESTADDRREQ(89)`。
- **修复**（`cm_net.c` `cm_udp_connect_peer()`，在 `cm_dtls_run()` 里 `drain_socket()` 之后调用，
  即 ICE 定稿 peer 之后）。
- **为什么自检没抓到**：`dselftest` 的 loopback 两个 socket 互相 `connect()` 过，永远走不到这条路径。
  **教训：自检的网络拓扑要和生产一致（故意不 connect）。**

### ③ STUN keepalive 污染 DTLS 记录层

- **症状**：握手 25 秒超时，日志里 `in=14 (in_bytes=182)` —— **收到的永远是 13 字节**、
  `type=-1`，且 `SSL3_RT_HANDSHAKE` 回调一次都没有。`14/25s ≈ 1.8s/个`。
- **根因**：ice-lite 网关持续发 STUN Binding Request keepalive（首字节 `00 01`）。
  `dtls1_get_record()` 读到非法的 content type 且版本不是 0xFEFD/0xFEFF 时，
  走 "drop this record, goto again" —— **不发回调、不报错、静默丢弃**。
  而 `SSL3_RT_HEADER` 回调（`ssl3_record.c` 里 `ssl3_read_n(..., DTLS1_RT_HEADER_LENGTH=13, ...)`
  之后）**每条记录头都会触发**，所以看到的是 14 个 13 字节的"神秘包"。
- **修复**：
  - `cm_net.c` 新增 `cm_udp_prepare_dtls()` / `cm_udp_pump_media()`，用
    `recvfrom(MSG_PEEK | MSG_DONTWAIT)` 先分类：
    `b[0]∈{20,21,22,23} && b[1]==0xFE && (b[2]==0xFD||0xFF)` → DTLS（**不消费**，留给 OpenSSL）；
    STUN → 消费并**当场应答**；其它 → 消费丢弃并计数。
  - 握手期间 `dtls_handshake()` 里的 `poll()` 换成 `cm_udp_prepare_dtls()`。
  - **STUN keepalive 必须应答**（否则网关认为 ICE 断了会停流/踢会话），
    `cm_ice.c` 的 `cm_stun_handle()` 回 `0x0101` + `XOR-MAPPED-ADDRESS(0x0020)`，
    MESSAGE-INTEGRITY 的 key 用**我方** `our_pwd`（RFC 8445 §7.2.2：响应用与请求相同的 key）。
  - `msg_cb` 补 `SSL3_RT_HEADER (0x100)` 具名分支，打印
    `ct= ver= epoch= len=` + 13 字节 hex，`badHdr` 计数。
- **附带坑**：`MSG_PEEK` 不带 `MSG_DONTWAIT` 时，在**阻塞** socket 上会永久睡在
  `recvfrom`（`/proc/<pid>/task/*/wchan` 能看到 `__skb_wait_for_more_packets`）。
  生产 socket 是非阻塞所以被掩盖，自检的 loopback 是阻塞的就挂死 → 统一加 `MSG_DONTWAIT`。

### ④ `SSL_export_keying_material` 的 label_len 21 → 19

- **症状**：DTLS 握手完全正常、60 字节 KM 正常导出，但**每个 SRTP 包 HMAC 都验不过**
  （真机 2594 次 `srtp unprotect -> -7`，0 帧）。
- **根因**：
  ```c
  SSL_export_keying_material(ssl, km, 60, "EXTRACTOR-dtls_srtp", 21, NULL, 0, 0);
  //                                                          ^^ strlen 是 19
  ```
  `label_len` 直接进 TLS 1.2 PRF 的 seed 计算 ⇒ seed 完全不同 ⇒ 导出的 KM 完全不同。
- **修复**：`sizeof("EXTRACTOR-dtls_srtp") - 1`。
- **★ 教训（load-bearing，写在这里提醒后来人）**：
  **对称的错误永远暴露不出来。** `dselftest` 的服务端当时也用同一个 `21`，
  两端导出同一份错误 KM，于是 `PASS1: identical keying material` 照样通过。
  现在 `dselftest` 里有判别性用例 **PASS4**：在同一个 SSL 会话上分别用 `19` 和 `21`
  导出，断言两者**必须不同** —— 只有这样才能抓住这类"两边一起错"的 bug。
  同理，SRTP 的 KDF 必须用**外部固定向量**（RFC 3711 附录 B.3）校验，
  不能用自己写的对端来验：
  master key `E1F97A0D3E018BE0D64FA32C06DE4139`
  + master salt `0EC675AD498AFEEBB6960B3AABE6` ⇒
  cipher key `C61E7A93744F39EE10734AFE3FF7A087` /
  cipher salt `30CBBC08863D8C85D49DB34A9AE1` /
  auth key `CEBE321F6FF7716B6FD4AB49AF256A156D38BAA4`。
  （KDF 里 **`iv[7] ^= label`**，穷举 16 个位置只有 `iv[7]` 能三把全中。）

### ⑤ kmssink 管线字符串漏一个 `!`

- **症状**：`pushed=289`、`video pipeline PLAYING`，但屏幕上什么都没有，
  `Esmart1-win0` 平面从未出现。
- **根因**：`KMS_SINK_PROPS` 宏原来以空格开头而不是 `" ! "`，拼出来是
  ```
  ... ! videoconvert kmssink name=vsink plane-id=76 ...
  ```
  `kmssink` 变成**孤立元素**，`videoconvert` 的 src pad 悬空，preroll 永远完不成，
  状态机卡在 `GST_STATE_CHANGE_ASYNC`（`r=2 state=3 pending=4`）。
- **诊断方法（决定性）**：`gst_element_get_state(pipeline, &state, &pending, 0)` 打
  `r/state/pending`。`r=2 (ASYNC) + state=3 (PAUSED) + pending=4 (PLAYING)` 永久不变
  = preroll 卡死；修好后立刻 `r=1 state=4 pending=0`。
- **修复**：`#define KMS_SINK_PROPS " ! kmssink name=vsink plane-id=76 ..."`。
- **弯路记录（别再走）**：曾用临时 `getenv("CM_VIDEO_PIPE")` 跑过 4 个管线变体
  （去 `is-live` / 去 `do-timestamp` / `is-live=false` / 加 `queue`），
  4 个全部 `state=4` 但仍不上屏 —— 因为缺 `!` 是独立问题，掩盖了其它一切。
  也怀疑过 weston 独占 DRM master、`/dev/dri/card1` 是 rknpu（`ff300000.npu`，非显示），
  均与故障无关。

### ⑥ `render-rectangle` 用面板物理坐标，且宽高互换

- **症状**：rect 传 `<0,0,960,266>`（逻辑 960x266 画布坐标）时，视频要么完全不见，
  要么位置离谱。
- **根因**：`kmssink` 的 `render-rectangle` 是**面板物理绝对坐标**，面板是
  **480x960 竖屏**（UI 逻辑 960x266 是旋转 90° 的）。逻辑矩形必须换算：

  ```c
  px = 107 + logical_x;   // 107 = Falcon/weston 合成面在面板上占列 107..372
  py = logical_y;
  pw = logical_h;         // 宽高互换（旋转 90°）
  ph = logical_w;
  ```
  `VIDEO={x:0,y:0,w:473,h:266}` → `<107,0,266,473>`。
  传 `960` 这种逻辑宽度直接越界面板（只有 480 宽）。
- **其他硬约束**：
  - 必须显式 `force-aspect-ratio=false`，否则在矩形内做信箱缩放，画面缩成一条。
  - `can-scale=true` 让 RGA 硬件直接缩放（1280x720 → 266x473 无需 `videoscale`）。
  - `render-rectangle` 是**只写属性、只在 reconfigure（分配 plane）那一刻读取**，
    PAUSED 下设置无效，必须"设属性 → 脉冲 PLAYING"。`cm_box_set_rect()` 里做了
    `PAUSED → PLAYING` 的脉冲。
  - 它是**单向棘轮**：缩到 1x1 之后无法再放大，只能重建管线。
  - `w=0 || h=0` 会回退成"全屏"语义并去抢 CRTC（与 weston 打架卡死），
    所以 `cm_box_create()` 里对 `rect_w/h<=0` 兜底成 `0,0,473,266`；
    真正隐藏要用 `1x1` 且**不加 107 偏移**。
- **正确判据**：`grep 'Esmart1-win0: ACTIVE' /sys/kernel/debug/dri/0/summary`。
  `Esmart0-win0` 的 `buf[0].addr == 0` **不是**故障（桌面正常时它也是 0）。

---

## 4. `cm_*` 文件职责表

| 文件 | 职责 |
| --- | --- |
| `cgmedia.c` | quickjs 模块注册（`custom_init_jsapis` / `cgmedia_module_load`）、`start/poll/cmd/stop/version` 五个导出、会话表（`g_sessions` + `g_next_id`）、事件链表 `cm_emit` / `cm_ev_drain`、收包主线程 `cm_rx_main`（ICE→DTLS→SRTP→media 状态机） |
| `cm_session.h` | 全部共享类型与原型：`cm_cfg`（cfgJson 解析结果）、`cm_stats`（计数器）、`cm_sess`（会话状态 + 事件队列 + box 指针 + 私有点）、状态枚举 |
| `cm_util.c/h` | `cm_log` 分级日志、`cm_buf` 动态字符串（JSON 拼装）、`cm_strdup`、`cm_rd16/24/32`、`cm_wr16/32`、`cm_now_ms`、`cm_sleep_ms`、迷你 JSON 取值（`cm_json_get_str/int/obj`） |
| `cm_cfg.c` | cfgJson → `cm_cfg`。字段别名兼容：`remoteUfrag`/`ufrag`、`remotePwd`/`pwd`、`ourUfrag`、`ourPwd`；缺省 `videoPt=98 audioPt=111 rect=960x266 logLevel=3` |
| `cm_net.c` | UDP 收发 + **收包分类**：`cm_udp_open`（bind 0.0.0.0:0 非阻塞）、`cm_udp_connect_peer`、`cm_udp_send`/`cm_udp_send_peer`、`cm_udp_recv`、`classify()`、`cm_udp_prepare_dtls()`（握手期挑出 STUN）、`cm_udp_pump_media()`（媒体期统一分发） |
| `cm_ice.c` | STUN 编解码（`stun_attr` / `stun_build`（MI/FP length 规则）/ `stun_parse`）、`cm_ice_run`（10×300ms）、`cm_stun_is` / `cm_stun_handle`（keepalive 应答） |
| `cm_dtls.c` | DTLS-SRTP 客户端：`cm_dtls_run`、`dtls_handshake`（deadline 循环 + `DTLSv1_handle_timeout` 重传驱动）、`drain_socket`、`msg_cb`（记录头 + 握手报文诊断）、`load_cert`、`cm_dtls_get_km`、`cm_dtls_diag`、`cm_dtls_fingerprint` |
| `cm_cert.h` | 内嵌 PEM 自签证书 + 私钥 + `CM_CERT_FINGERPRINT`（answer SDP 必须用它，否则网关指纹校验不过） |
| `cm_srtp.c` | AES-CM PRF（`aes_cm_prf`，`iv[7]^=label`）、`srtp_init`（label 0/2/1）、`ctr_xor`、`srtp_tag`（追加 4B ROC 后 HMAC-SHA1 取前 10）、`cm_srtp_open`（rx=server_write / tx=client_write）、`cm_srtp_unprotect`（ROC 回绕推断）、`cm_srtcp_protect`（PLI 加密）、诊断 dump/brute |
| `cm_rtp.c` | RTP 拆包 + H.264 重组：SPS 解析（`sps_parse_size`，得真实分辨率）、`annexb_put`、`flush_frame`、STAP-A / FU-A / 单 NAL、Opus 直送；`emit_media_if_needed` |
| `cm_gst.c` | GStreamer（无头文件，手写最小声明）：`cm_box_create/destroy/push/set_rect/set_caps_video`、`apply_rect`（逻辑→面板换算）、appsrc push |
| `cm_cmd.c` | `cmd()` 各 op（`stats` / `setRect` / `requestKeyFrame` / `setLogLevel` / `setAudio`）+ `cm_send_pli`（RTCP PSFB 12 字节，SRTCP 加密后 26 字节）+ `cm_kf_tick`（keyframe 看门狗：首流连发 3 次 PLI，之后每 2s 补一次） |
| `cm_media.c` | 媒体主循环 `cm_media_loop`：建 box → 20ms pump → SRTP → RTP → `cm_kf_tick` → 每 2s `emit_stat`；退出时拆 pipeline 释放 KMS 平面 |

---

## 5. 机位纪律（用户只有 25~30 分钟云游戏免费时长）

跑任何真机测试前必须做到：

1. **冒烟先过**：`adb shell "sleep 3; echo SMOKE_OK" &` + `wait`，确认宿主侧后台 adb
   能存活（**设备侧 `&` 会被 SIGHUP 杀掉**，这是白跑过一整轮的根因）。
2. **单轮 ≤ 25 秒**：`cgmtest` 从起到 stop。容器冷启动约 17 秒，媒体窗口要留够。
3. **`trap` 三段释放**，无论成败：
   ```
   trap cleanup EXIT
   cleanup() {
     adb shell "killall cgmtest 2>/dev/null"
     kill $HOLD_PID                       # 断开网关 WS
     node .tmp/cgprobe/rel.mjs $GAME_CODE # DELETE api/v2/users/@me/games-playing/<code>
     adb shell "miniapp_cli start 8080222437664451"   # 回桌面
   }
   ```
4. **抢前台**：设备被多 agent 共用，起测试前 `adb shell "miniapp_cli start 8002026110200003 diag"`。
5. **日志自证**（防拿旧日志当本轮结果）：`/tmp/cgm.log` 的 mtime ≥ 本轮开始时间，
   且首行 `udp bound 0.0.0.0:<port> -> <cfg.remoteIp>:<cfg.remotePort>` 与 cfg 字符串一致。
   不通过 → 本轮数据作废，不得解读。
6. **约 1 次为一个单位**：失败先离线复盘（`dselftest` / `gtest` 都是零机位），
   不要循环申请。
7. `adb push/pull` 走 `/tmp` 时必须 `export MSYS2_ARG_CONV_EXCL='*'` 且目标写 `//tmp/xxx`。

---

## 6. 离线验证工具（零机位成本，改代码后先跑这两个）

| 工具 | 位置 | 覆盖 |
| --- | --- | --- |
| `dselftest` | `.tmp/cgprobe/dtest/dselftest.c` | loopback 起 DTLS server 让真实 `cm_dtls_run()` 连；验证 KM 一致（PASS1）、SRTP+RTP 解出 6 帧（part2）、**STUN 污染下仍握手**（PASS3a-e）、**label_len 判别**（PASS4） |
| `gtest` | `.tmp/cgprobe/dtest/gtest.c` | 读 Annex-B `.h264` 文件 → 真实 `cm_box_push()` → 验证 kmssink 状态机 `r=1 state=4 pending=0`、`Esmart1-win0: ACTIVE`、`kmssrc plane-id=76` 截图 |

构建 `dselftest`：
```bash
env/pyzig/ziglang/zig.exe cc -target aarch64-linux-gnu.2.17 -O1 -g0 -Wl,-s \
  -o .tmp/cgprobe/dtest/dselftest -I PenCG/native/csrc/cgmedia -I PenCG/native/vendor/openssl-include \
  .tmp/cgprobe/dtest/dselftest.c .tmp/cgprobe/dtest/dtest_stub.c \
  PenCG/native/csrc/cgmedia/{cm_util,cm_net,cm_ice,cm_dtls,cm_srtp,cm_rtp}.c \
  PenCG/native/stubs-aarch64/libssl.so.1.1 PenCG/native/stubs-aarch64/libcrypto.so.1.1 -lpthread -lm
```
构建 `gtest` 需要额外链 `-Wl,--allow-shlib-undefined -ldl` 和全部 gst stub
（`libgstapp-1.0.so.0 / libgstreamer-1.0.so.0 / libgobject-2.0.so.0 / libglib-2.0.so.0 / libgmodule-2.0.so.0`）。

设备上跑（注意无缓冲 + 后台）：
```bash
export MSYS2_ARG_CONV_EXCL='*'
./env/adb/adb.exe -s 9D91…5171 push .tmp/cgprobe/dtest/dselftest //tmp/dselftest
./env/adb/adb.exe -s 9D91…5171 shell 'chmod 755 /tmp/dselftest; cd /tmp && (./dselftest > /tmp/ds.log 2>&1 &); sleep 22; cat /tmp/ds.log'
```

---

## 7. 已知限制 / 未完成项

- **音频未经真链路验证**：Opus 裸包直送 `appsrc ! opusdec ! alsasink` 的管线建起来了
  （`audio pipeline PLAYING`），但没有一次真机测试确认扬声器真的出声。
- **KMS 出画的真链路截图仍缺**：`Esmart1-win0: ACTIVE` + 197437 字节彩条 PNG 是
  **零机位 gtest（文件源）** 的成果；真 RTP 链路那次（08:35）帧已推到 kmssink
  （`pushed=289`），但当时 `!` 还没修，所以没有对应的真链路截图。
- **`requestKeyFrame` / `setRect` / `setAudio` 三个 cmd 未做真机验证**（代码路径已实现）。
- `video_pipeline_soft()` 的 `avdec_h264` 降级路径在 X6 Pro 上**不可用**（设备没这个元素）。
- 09:04 那轮真机 `frames=0` 的原因是**网关侧容器冷启动**（`+16503ms` 才第一个
  `dataChannelReady`），媒体窗口 18s 用完时编码器还没出帧 —— 不是代码问题。
