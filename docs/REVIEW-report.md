# 上线前代码审查报告（bug1/bug3/bug4 补丁批次）

审查对象：工作树 21 个已修改 + 11 个未跟踪文件（全部未 commit）。
审查方式：逐函数实读源码 + 交叉核验注释自述与代码实际行为。**未改动任何代码**。

被审文件的 md5（若行号与本文不符，先核 md5 判断是否同一版本）：

| 文件 | 行数 | md5 |
| --- | --- | --- |
| `native/csrc/cgmedia/cm_rtp.c` | 1470 | aa2d19b47336892ac8909e33c7a8f924 |
| `native/csrc/cgmedia/cm_cmd.c` | 605 | 532a43a939ad5d78e70e4bb4b9d9dadb |
| `native/csrc/cgmedia/cm_media.c` | 368 | 7b4a54f088ec0b3b315d66b945db7d4b |
| `native/csrc/cgmedia/cm_gst.c` | 1407 | d79fca6efa972d40130e1c2ba8ceaba3 |
| `native/csrc/cgmedia/cm_srtp.c` | — | 07fb88e7e93015481ac216592959fd56 |
| `native/csrc/cgmedia/cm_session.h` | — | 0186ee1e783c5915bda55e3c7fda5bd0 |
| `src/pages/play/play.vue` | 2813 | 89e25666b8ba8cd5ee0b5d10d037694c |
| `src/services/screen.js` | 589 | fbeb67a3b352e684bc7734ad54838a89 |
| `src/app.js` | 118 | 6f93aca2ed1d4bc04f3bf1be160b8054 |
| `src/services/log.js` | 84 | 31b1e25f5c12ccbb3ee7b2177d90366d |
| `src/services/media.js` | — | fdfefe86d5e14b8294f7b2e8c0d7582e |
| `src/dev-autostart.js` | 49 | 55dd8b4371c18b29ed63840e81eda852 |

---

## 总表

| 编号 | 严重度 | 一句话问题 | file:line | 建议 |
| --- | --- | --- | --- | --- |
| **F1** | 🔴 P0 | `au_broken_fixable` 不随 AU 复位，结构性坏 AU 会被当成「已补洞修好」推给解码器 | `cm_rtp.c:369-390`（reset 缺清）、`:704-709`（误判点） | `rtp_reset_au()` 里补 `au_broken_fixable = 0; au_start_ms = 0;`，并给 `:993` 补显式置 0 |
| **S1** | 🔴 P0 | `DEV_AUTOSTART` 非 null，生产包启动 4s 后自动跳「明日方舟」play 页并占机位 | `src/dev-autostart.js:17` | 改回 `null`；顺带删掉 `:33` 注释里的明文手机号/密码 |
| **R2** | 🔴 P0 | 重传包不判归属，属于**已丢弃的上一 AU** 的分片会被拼进当前 AU 且不置 `au_broken` | `cm_rtp.c:1395-1407`、`:1089-1097` | `gap_take` 命中后校验 seq 落在 `[au_first_seq, au_last_seq]`，否则丢弃 |
| **N1** | 🟠 P1 | NACK 报文 `nfci >= 27` 时 SRTCP 加密必然失败并**退回明文发送**，在 SRTP 会话里全部作废 | `cm_cmd.c:409`、`:453-457`；`cm_srtp.c:432` | 缓冲区 `+16`（132+14=146≤148）或 `cap` 传 `sizeof(pkt)-14`；`enc <= 0` 时不发 |
| **R3** | 🟠 P1 | 重排路径覆盖 `out_p`，把刚补插的 SPS/PPS 丢掉；「裸 IDR + 重传乱序」同时成立时命中 | `cm_rtp.c:805-826`、`:836-858`（覆盖在 `:856`） | 重排结果写回 `au_out`（参数集长度先记下来，重排内容接在后面） |
| **R5** | 🟠 P1 | `gap_purge_expired()` 摘任意过期缺口后**无条件** `au_broken=1`，把当前完整 AU 打成残缺 | `cm_rtp.c:463-490`（置位在 `:484`） | 只在 seq 落在当前 AU 范围时置位（`au_first_seq/au_last_seq` 已具备但无人读） |
| **R6** | 🟠 P1 | **✅ 已修 2026-11-13** 把**收到的** NACK 当成「我们的 AU 坏了」；纯接收端语义不成立 | `cm_media.c:246-248` | 删掉这条，或只在 SSRC 属于对端视频且我们确实发过媒体时才认 |
| **R4** | 🟠 P1 | **✅ 已修 2026-11-13** IDR hold 窗口被下一个 AU 的 FU-A start 顺手清掉，60fps 下必然失效 | `cm_rtp.c:716-723`、`:1047-1050` | hold 期间 `in_fu` 新 start 时先放行挂起的 IDR（`flush_frame`）再 reset |
| **P1-7** | 🟠 P1 | **✅ 已修 2026-11-13** IDR 饥饿放行后无「重新收紧」机制，`have_kf` 永久为 1 | `cm_rtp.c:782-801` | 可选：放行后设一个 `relax_until_ms`，到期仍无 IDR 则重新关闸 |
| **L1** | 🟡 P2 | NACK 的全部统计字段没进 stat JSON，且相关日志是 `CM_LOGD`（默认不打）→ 真机上无法判断 NACK 是否生效 | `cm_media.c:112-129`、`cm_rtp.c:1401` 等 | stat JSON 增 `nackSent/nackReq/nackRecovered/relax`；关键 4 条日志升到 `CM_LOGI` |
| **L2** | 🟡 P2 | **✅ 已修 2026-11-13** `gap_drop()` 是死函数；它负责的三件事被三处内联代码各实现一遍 | `cm_rtp.c:441-461` | 三处内联改调 `gap_drop()`，或删函数 |
| **L3** | 🟡 P2 | `screen.js` 保活失败时 1s 重试、无退避无上限 | `screen.js:511-516` | 指数退避到 20s 上限 |
| **L4** | 🟡 P2 | `log.js` 无文件轮转；`/userdata` 剩余空间**无法核实**（设备离线 + 禁 adb） | `src/services/log.js:10-44` | 见 §P2-11，建议 native 侧按大小自轮转 |
| **L5** | 🟡 P2 | 两份文档对「native stdout 是否落进 `loli_s6_dev.txt`」结论矛盾 | `docs/VERIFY-playbook.md:298` vs `docs/BUGFIX-bug1-freeze-verify.md:17` | 现场核实后统一 |
| **L6** | 🟡 P2 | **✅ 已修 2026-11-13** `flush_frame` 重排循环未检查 `cm_buf_put` 返回值 | `cm_rtp.c:851-854` | 检查并在失败时 `au_broken=1` |
| **P0-1** | 🟢 非问题 | 分片表溢出的静默截断 | `cm_rtp.c:905-931`、`:836` | 已用 `seg_overflow` 正确修掉，见 §P0-1 |
| **P0-2** | 🟢 非问题 | `gap_add` 表满 memmove | `cm_rtp.c:429-434` | 界内 |
| **P0-3** | 🟢 非问题 | `au_miss[]` 与 `au_missing` 不同步 | `cm_rtp.c` 五处 | 五处全配对 |
| **P0-4** | 🟢 非问题 | 主循环 flush 与收包线程 ABBA | `cm_rtp.c:889-901`、`cgmedia.c:245` | 媒体栈单线程串行，结构上不可能 |
| **P0-5** | 🟢 非问题 | `nack_sort_gaps` off-by-one | `cm_cmd.c:398-405` | 写址恒在界内 |
| **P1-6** | 🟢 非问题 | `drop_streak` 增/清配对 | `cm_rtp.c` 8 处 | 配对成立（有双计但只加速，无害） |
| **P1-8** | 🟢 未发现问题 | `flush-stop(TRUE)` 的 reset_time 引起时间轴跳变 | `cm_gst.c:1386-1395` | 与我们从不设 PTS 自洽；去掉 TRUE 反而有害。真机表现未验证 |
| **P1-9** | 🟢 非问题 | 首次协商分辨率与 appsrc caps 不符 | `cm_gst.c:1077-1079` | caps 里根本没有 width/height |
| **P2-10** | 🟢 非问题 | play.vue 定时器泄漏 | `play.vue:1385-1401`、`:1847-1852`、`:1894-1940` | 三条销毁路径都清 |
| **P2-12** | 🟢 非问题 | `audio_route_restore` 覆盖用户运行中调的音量 | `cm_gst.c:466-500` | card1#54 只读不写，结构上不可能 |

---

## 🔴 P0

### F1（新发现）：`au_broken_fixable` 不随 AU 复位，结构性坏 AU 会被「误判为已修复」推给解码器

**证据**

`PenCG/native/csrc/cgmedia/cm_rtp.c:369-390` 的 `rtp_reset_au()`，每收完一个 AU 都会清 frame / have_data / in_fu / au_broken / au_has_* / seg_n / seg_ordered / seg_overflow / au_have_range / au_missing / au_hold_state / au_hold_until_ms，**但没有清 `au_broken_fixable`，也没有清 `au_start_ms`**。

`grep -n au_broken_fixable` 全文件只有 4 个写入点：

```
:706   g_rtp.au_broken_fixable = 0;   /* 修复判定成功后 */
:1080  g_rtp.au_broken_fixable = 1;   /* FU-A start lost */
:1368  g_rtp.au_broken_fixable = 0;   /* diff > 64，结构性断流 */
:1373  g_rtp.au_broken_fixable = 1;   /* 普通 rtp gap */
```

而 `au_broken = 1` 的置位点有 **8 处**，其中 **5 处完全不碰 fixable**：

```
:484   gap_purge_expired 放弃过期缺口（TTL 300ms）        ← 不碰 fixable
:993   STAP-A 内部长度字段越界（结构性损坏）               ← 不碰 fixable
:1079  FU-A start lost                                    → :1080 置 fixable=1
:1154  cm_rtp_note_stale（peer NACK / srtp auth fail）    ← 不碰 fixable
:1367  diff > 64 结构性断流                               → :1368 置 fixable=0
:1372  普通 rtp gap                                       → :1373 置 fixable=1
:1405  真乱序（else 分支）                                ← 不碰 fixable
:1423  AU over FRAME_CAP                                  ← 不碰 fixable
```

于是 fixable 的值会**跨 AU 继承**。最严重的一条链路：

1. 上一帧因普通 rtp gap 置 `au_broken=1; au_broken_fixable=1`，该帧被正常丢弃，末尾 `rtp_reset_au()` 把 `au_broken` 清 0、`au_missing` 清 0，**但 fixable 仍是 1**；
2. 下一个 AU 收到一个 STAP-A，内部长度字段越界 → `cm_rtp.c:993` 置 `au_broken=1`（不碰 fixable，仍是残留的 1）；
3. marker 到，进 `flush_frame`，`:704` 的判据 `if (au_broken && au_broken_fixable && au_missing == 0)` **三条全部成立** → 打 `"#%d AU repaired by retransmissions (%llu bytes)"` → `au_broken=0`；
4. 这个被截断的 AU 被 push 进 appsrc，解码器拿半帧、用旧参考帧补齐错误宏块并**污染参考帧** —— 这正是 bug3/bug4 的「上一帧马赛克残留 / 绿块」症状。

同一机制也覆盖 `:1154`（srtp 认证失败）、`:1405`（乱序）、`:1423`（AU 超帧上限）、`:484`（TTL 放弃缺口）四条路径。

**附带缺陷（同一处修法顺手解决）**：`au_start_ms` 也不清，而 `gap_upgrade_since(g_rtp.au_start_ms)`（`:1016`、`:1066`、`:1131`）用它筛「本 AU 之后登记的缺口」。`au_start_ms` 残留上一帧的值时，会把上一帧的旧缺口一并升成 `NACK_PRIO_IDR`，白占 IDR 重传预算（`NACK_ROUNDS_IDR=5` 轮，比 P 帧的 3 轮贵）。更糟的是 **STAP-A 分支从不设置 `au_start_ms`**（`:1016` 直接用，全文件只有 `:1052` 的 FU-A start 与 `:1130` 的单 NAL 分支赋值），所以走 STAP-A 的帧永远在用别人留下的时间戳。

**后果**：中低概率触发（需要上一帧恰好是可修复损坏 + 当前帧是结构性损坏），但一旦触发就是把坏帧当做好帧送进解码器，且**日志会打「已修复」，把排查方向彻底带偏**。

**建议改法**（一行级 + 一行）：

```c
static void rtp_reset_au(void) {
    ...
    g_rtp.au_hold_until_ms = 0;
    /* 这两个必须随 AU 一起清：fixable 残留会让结构性坏 AU 在 flush 时
     * 被「补洞成功」判据误命中，把截断帧推给解码器；au_start_ms 残留
     * 会让 gap_upgrade_since 把上一帧的缺口升成 IDR 优先级。 */
    g_rtp.au_broken_fixable = 0;
    g_rtp.au_start_ms = 0;
}
```

并在 `:993`（STAP-A 越界）显式补 `g_rtp.au_broken_fixable = 0;` —— `:702` 的注释已经声明了这个语义（「结构性损坏（STAP-A 长度字段越界）置 0」），但代码里并没有做，属于注释与实现不一致。

---

### S1（新发现）：`DEV_AUTOSTART` 非 null，生产包会自动跳进某个游戏的 play 页

**证据**

`PenCG/src/dev-autostart.js:17`：

```js
export const DEV_AUTOSTART = { page: 'play', delay: 4000, params: { code: 'mrfzzmd', name: '明日方舟：终末地（官服）', quality: 'low', gameType: 'mobile' } };
```

文件头 `:9` 自己写着「取证结束必须把它改回 null（或在最终交付前整文件删除）」，但没执行。同文件的 `DEV_AUTOLOGIN`（`:36`）与 `DEV_LOGOUT`（`:49`）**当前都是 null**，是干净的。

消费方：`PenCG/src/pages/index/index.vue:118` import；`:392-393` boot 完成后 `this.devLogout(); this.devAutostart();`；`devAutostart()`（`:423`）在 `DEV_AUTOSTART` 非 null 时按 `delay` 调 `navTo(d.page, d.params)`。

附带：`:33` 的用法示例注释里有明文手机号与密码（已脱敏）。我核过当前 `.falcon_/` 全目录与 `8002026110200003.1_0_3.amr`，`grep -a` 两个串都是 0 命中（因为它们在注释里，不进产物）；但注释留在仓库里本身是凭据泄露，建议一并删掉。

**后果**：装到用户机器上后，打开 app 4 秒自动进入「明日方舟：终末地」并申请机位 —— 既占免费时长，也让用户以为 app 坏了。

**建议改法**：`export const DEV_AUTOSTART = null;`，并把 `:11-15`、`:33` 示例里的真实 code / 手机号 / 密码换成占位符。

---

### R2（新发现）：重传包不判归属，会污染当前 AU 且不留痕迹

**证据**

`cm_rtp.c:1395-1407`，旧包（`diff >= 0x8000`）分支：

```c
if (gap_take(seq)) {
    s->stats.vid_nack_recovered++;
    CM_LOGD("#%d retransmit recovered seq=%u (%llu/%llu)", ...);
    /* 落到下面正常处理：append 并按 seq 记录分片偏移。
     * au_broken 保持原样，等 au_missing 归零时由 flush 清掉。 */
} else {
    s->stats.vid_reorder++;
    g_rtp.au_broken = 1;
    ... unlock; return;
}
```

`gap_take` 只判断「这个 seq 在缺口表里」，**不判断它属于哪个 AU**。而缺口表 `rtp_reset_au()` 刻意不清（`:378-382` 的注释：让缺口按 TTL 自然过期），`NACK_GAP_TTL_MS = 300ms`（`:94`）远大于 30fps 的帧间隔 33ms。

**注意：作者是有意这么写的。** `:380-382` 的注释原文是「重传包回来时我们仍然希望能把它补进去（哪怕只能推给解码器当参考帧碎片也无害）」。「无害」这个判断只在**这个分片属于当前 AU** 时成立；一旦它属于已经丢弃的上一 AU，拼进去就是污染参考帧链，恰好是 bug3/bug4 的形态。这是本条的核心分歧点，不是作者疏忽了一个校验，而是把「补洞」的收益估得高于「污染」的风险。

于是：上一 AU 丢的包、我们发了 NACK、300ms 内的某次重传到达时，**当前 AU 很可能已经换过 3~9 个了**。这个迟到的分片会落进 `:1428` 的 `handle_h264(...)`：

- 若当前 `in_fu == 1`（正在收 FU-A 中段），`:1089-1097` 的 `cm_buf_put(&g_rtp.frame, p + 2, n - 2)` 会把它**当成当前 AU 的正文直接拼上去**；
- `au_seg_add(seq, off, len)` 因 `d = (int16_t)(seq - segs[n-1].seq) <= 0` 置 `seg_ordered = 0`（`:925`）；
- flush 时走重排，这个外来分片被排到**最前面**（它的 seq 最小）；
- 关键：`gap_take` 成功分支**没有置 `au_broken`**，所以这段污染不会被发现，坏 AU 会被当好的推给解码器。

**后果**：花屏/马赛克，且 `vid_au_drop` 不增长 —— 从统计上看不出来。与 R3 叠加时更糟（重排路径会连带丢参数集）。

**建议改法**：`gap_take` 命中后立刻校验归属，不属于当前 AU 就丢弃（这一帧本来就救不回来了，救回来也拼不回原位）：

```c
if (gap_take(seq)) {
    s->stats.vid_nack_recovered++;
    /* 缺口表按 TTL 过期、不随 AU 清，所以这里的重传包可能属于**更早的
     * 已经丢弃的 AU**。它拼不回原位，append 进来只会污染当前 AU 且不留
     * 痕迹（gap_take 成功路径不置 au_broken）。只接受属于当前 AU 的。 */
    if (g_rtp.au_have_range &&
        (int16_t)(seq - g_rtp.au_first_seq) >= 0 &&
        (int16_t)(seq - g_rtp.au_last_seq) <= 0) {
        /* 落到下面正常处理 */
    } else {
        s->stats.vid_nack_late++;   /* 新计数，便于观察 */
        pthread_mutex_unlock(&g_rtp_mu);
        return;
    }
}
```

`au_have_range` / `au_first_seq` / `au_last_seq` 这三个字段（声明在 `cm_rtp.c:173-175`）目前**全文件只有写、没有任何读取者**，注释写的用途正是「判定重传包归属」—— 已经准备好了，只差这一步。

**关于 `rtp_reset_au()` 里的 `au_have_range = 0`（`:386`）**：修 R2 时要注意，`rtp_reset_au` 会把它清零，所以新 AU 刚开始那段 `au_have_range == 0` 是真的（还没登记任何分片）。此时若重传包先到，上面的守卫会走 `else` 分支丢掉它 —— 这是我们想要的（它本来就不可能是当前 AU 的，当前 AU 一个分片都还没到）。但**不要**为了「别丢太狠」把守卫写成「`!au_have_range` 就放行」。

---

## 🟠 P1

### N1（新发现）：NACK 报文在下发多 FCI 时 SRTCP 加密必然失败，并退回明文

**证据**

`PenCG/native/csrc/cgmedia/cm_cmd.c:409`：

```c
unsigned char pkt[12 + NACK_MAX_FCI * 4];   /* NACK_MAX_FCI = 30 → 132 字节 */
```

`:453-457`：

```c
if (cm_srtcp_ready(s)) {
    enc = cm_srtcp_protect(s, pkt, len, (int)sizeof(pkt));   /* cap = 132 */
    if (enc > 0) len = enc;
}
trans = enc > 0 ? "srtcp" : "plain";
```

`PenCG/native/csrc/cgmedia/cm_srtp.c:425-432`：

```c
int cm_srtcp_protect(cm_sess *s, unsigned char *pkt, int len, int cap) {
    ...
    if (!st || !st->ready) return -1;
    if (len < 8 || len + 14 > cap) return -2;
```

SRTCP 固定增量 `+4`（index）+ `10`（auth tag）= `+14`（`:444-450` 的注释也这么写）。代入：

| nfci | len = 12+nfci*4 | len+14 | vs cap=132 |
| --- | --- | --- | --- |
| 26 | 116 | 130 | ✅ 通过 |
| **27** | **120** | **134** | ❌ 返回 -2 |
| 30（满） | 132 | 146 | ❌ 返回 -2 |

失效链：`enc = -2` → `enc > 0` 为假 → **`len` 保持明文长度、`trans = "plain"`，但 `:465` 的 `cm_udp_send` 仍然照发**。媒体面是 SRTP，明文 RTCP 会被网关丢弃（同文件 PLI 的注释 `:86-90`、RR 段注释都记过这个坑）。

而 `stats.vid_nack_sent++`（`:470`）与日志 `NACK sent (%d bytes, %s)`（`:475`）都照常执行，日志里只会显示 `plain` 两个字 —— 看不出异常。

**后果**：缺口多到需要 ≥27 个 FCI（即约 27 个以上分散缺口，**严重丢包场景**）时，NACK 全部变成明文死包。恰恰是最需要重传的时候失效，而计数器显示一切正常。

对照：PLI 用 `pkt[64]`、`len=12` → 12+14=26 ≤ 64 ✅；RR 用 `pkt[96]`、`len=56` → 70 ≤ 96 ✅。这两条只是「长度恰好够」，没有余量保护。

**建议改法**（任选其一 + 一条硬约束）：

```c
/* SRTCP 会追加 14 字节（4 index + 10 auth tag），缓冲区必须留出来。
 * 原来按 12+30*4=132 精确分配，nfci>=27 时 len+14>cap，cm_srtcp_protect
 * 返回 -2，而下面 enc<=0 只会把 trans 标成 plain —— 在 SRTP 会话里等于
 * 把 NACK 静默丢掉，偏偏是最需要重传的严重丢包场景。 */
unsigned char pkt[12 + NACK_MAX_FCI * 4 + 16];
```

并把 `:453-457` 改成「加密失败就不发」（PLI / RR 同样建议加）：

```c
if (cm_srtcp_ready(s)) {
    enc = cm_srtcp_protect(s, pkt, len, (int)sizeof(pkt));
    if (enc <= 0) {
        CM_LOGW("#%d NACK srtcp protect failed (%d), drop instead of plaintext", s->id, enc);
        return;
    }
    len = enc;
    trans = "srtcp";
} else {
    trans = "plain";
}
```

### R3（新发现）：重排路径覆盖 `out_p`，把刚补插的 SPS/PPS 丢掉

**证据**

`cm_rtp.c:805-826`（补插参数集）：

```c
out_p = g_rtp.frame.p;
out_n = (int)g_rtp.frame.len;
if (key) {
    need_sps = !g_rtp.au_has_sps && g_rtp.sps_ok;
    need_pps = !g_rtp.au_has_pps && g_rtp.pps_ok;
    if (need_sps || need_pps) {
        cm_buf_reset(&g_rtp.au_out);
        ... put sps / pps / frame ...
        out_p = g_rtp.au_out.p;      /* :819 */
        out_n = (int)g_rtp.au_out.len;
        s->stats.vid_sps_inject++;
    }
}
```

`cm_rtp.c:836-858`（重排）：

```c
if (!g_rtp.seg_ordered && g_rtp.seg_n > 1 && !g_rtp.seg_overflow) {
    ...
    cm_buf_reset(&g_rtp.au_sorted);
    for (k = 0; k < g_rtp.seg_n; k++) {
        cm_buf_put(&g_rtp.au_sorted, g_rtp.frame.p + g_rtp.segs[t].off, segs[t].len);
    }
    out_p = g_rtp.au_sorted.p;       /* :856 —— 覆盖掉 :819 的 au_out */
    out_n = (int)g_rtp.au_sorted.len;
}
```

`au_sorted` 里只有 `frame` 的分片字节，**不含参数集**。两段都命中时，`:819` 的补插结果被 `:856` 覆盖 → 需要的 SPS/PPS 丢了 → 分辨率/参数集缺失 → 绿块/花屏，与 bug4 同形。

**触发条件需要两个同时成立**（我一开始把它写宽了，这里修正）：

1. **补插路径命中**：`:808` 要求 `key` 为真，且 `:809-810` 的 `need_sps = !au_has_sps && sps_ok` / `need_pps` 至少一个为真 —— 即「这是个 IDR，但这个 AU 自己没有带 SPS（或 PPS），而我们缓存里有」。网关发裸 IDR 时常见。
2. **重排路径命中**：`:836` 要求 `!seg_ordered && seg_n > 1 && !seg_overflow`。

关于 2 有一处容易误判：STAP-A（`:979-1023`）里一个聚合包内的多个 NAL **共享同一个 RTP seq**，第二个 NAL 喂进 `au_seg_add` 时 `d == 0` → `seg_ordered = 0`。所以**只要网关用「STAP-A 带 SPS+PPS」的形态，那个 AU 一定走重排路径** —— 但那个 AU 的 `au_has_sps` 此时为真，条件 1 不成立，补插根本不会发生，重排只是把同样的字节按 seq 顺序拼一遍（同 seq 的比较走 `else break`，稳定排序，字节顺序不变），**无害**。

两个条件真正同时成立的场景是：**网关发的是裸 IDR（无 SPS），且该 IDR 是 FU-A 分片、期间有 NACK 重传包把它打成了乱序**（`au_seg_add` 的 `d <= 0` 分支，`:924-927`）—— 也就是 R2/R5 描述的那个丢包场景。所以：

- 单独看概率：比 R5/R2 低（需要「裸 IDR」+「乱序」叠加）；
- 但这不是罕见组合：`cm_rtp.c:176-179` 的注释表明「补插」正是为裸 IDR 设计的，而裸 IDR 通常恰好在弱网/网关重协商时出现，同一时刻也正是 NACK 最活跃的时候。**两者在同一个故障场景里正相关**，不是独立事件。
- 症状是绿块/花屏（SPS/PPS 缺失），与 bug4 同形，且不会让画面冻结 —— 所以容易被误判成「网络差」而不是「我们的 bug」。

**建议改法**：重排结果不要直接当输出，而是统一写回 `au_out`（`au_out` 已经存在、已被 `cm_buf_reset` 管理，不需要新缓冲）：

```c
/* --- 3) 分片重排（补洞包把顺序打乱了） --- */
if (!g_rtp.seg_ordered && g_rtp.seg_n > 1 && !g_rtp.seg_overflow) {
    int idx[AU_SEG_MAX];
    int k, j;

    /* 先按 seq 排索引（原样保留） */
    for (k = 0; k < g_rtp.seg_n; k++) {
        int cur = k;
        for (j = k - 1; j >= 0; j--) {
            if ((int16_t)(g_rtp.segs[idx[j]].seq - g_rtp.segs[cur].seq) > 0) {
                idx[j + 1] = idx[j];
            } else break;
        }
        idx[j + 1] = cur;
    }

    /* 把「参数集 + 重排后的分片」按这个顺序拼进 au_out。
     * 参数集必须是前面已经补插过的那一份（如果补插过），否则 SPS/PPS 会被
     * 这一步覆盖掉 —— 与 bug4 的绿块同形。 */
    if (out_p == g_rtp.au_out.p) {
        /* 已补插：au_out = [SPS][PPS]，先截掉后面的 frame 部分，
         * 再把重排结果接上去。 */
        cm_buf_truncate(&g_rtp.au_out, parameter_sets_len);
        for (k = 0; k < g_rtp.seg_n; k++) {
            int t = idx[k];
            cm_buf_put(&g_rtp.au_out, g_rtp.frame.p + g_rtp.segs[t].off,
                       (size_t)g_rtp.segs[t].len);
        }
    } else {
        /* 没补插：直接重排进 au_out */
        cm_buf_reset(&g_rtp.au_out);
        for (k = 0; k < g_rtp.seg_n; k++) {
            int t = idx[k];
            cm_buf_put(&g_rtp.au_out, g_rtp.frame.p + g_rtp.segs[t].off,
                       (size_t)g_rtp.segs[t].len);
        }
    }
    out_p = g_rtp.au_out.p;
    out_n = (int)g_rtp.au_out.len;
}
```

注意：`cm_buf` 目前只有 `cm_buf_reset`（置 len=0，保留内存），**没有 truncate**。要么新增一个 `cm_buf_truncate(b, n)`（`if (n < b->len) b->len = n;`，三行），要么在补插那一步顺手把参数集长度记下来（`size_t ps_len = g_rtp.au_out.len;` 插在 put frame 之前），然后 `g_rtp.au_out.len = ps_len;` 直接截断 —— 后者不需要动 `cm_util.c`，验收更简单。

另外注意 `cm_buf_put` 的返回值这回**必须检查**（顺带修 L6）：任一 put 失败就该 `au_broken = 1` 并放弃这一帧，而不是推出一个短 buffer。

### R5（新发现）：`gap_purge_expired()` 无条件把 `au_broken` 置 1

**证据** `cm_rtp.c:463-490`：

```c
static void gap_purge_expired(void) {
    ...
    while (i < g_rtp.gap_n) {
        if (now > g_rtp.gaps[i].t_ms && (now - g_rtp.gaps[i].t_ms) > NACK_GAP_TTL_MS) {
            ... memmove 摘 gap、同步摘 au_miss ...
            g_rtp.au_broken = 1;   /* :484 洞被放弃了，这个 AU 确定是残的 */
            continue;
        }
        i++;
    }
}
```

它**不判断这个过期缺口属于哪个 AU**。`NACK_GAP_TTL_MS = 300ms` ≫ AU 间隔 33ms，所以一个**上一帧**的缺口过期时，会把**当前正在拼的、本来完整的 AU** 打成结构性损坏 → flush 整帧丢弃。若当前恰好是 IDR，还会走 `:727-736`（`have_kf=0` + `kf_request("broken IDR")`），造成「明明没丢包却在丢帧 + 反复要 IDR」。

`gap_purge_expired` 唯一调用点是 `cm_rtp_take_nack_gaps` 开头（`:510`），而后者被 `cm_nack_tick` 每 30ms 调一次 —— 也就是**每 30ms 都有一次机会误伤当前 AU**。

**建议改法**：用已经存在但无人读的 `au_first_seq/au_last_seq` 做范围判断：

```c
/* 只有落在当前 AU 范围内的缺口过期，才说明这个 AU 残了。
 * TTL(300ms) 远大于 AU 间隔(33ms)，上一帧的缺口过期时把当前完整 AU
 * 判成残缺，是"丢包后恢复慢"的一个自伤源。 */
if (g_rtp.au_have_range &&
    (int16_t)(sq - g_rtp.au_first_seq) >= 0 &&
    (int16_t)(sq - g_rtp.au_last_seq) <= 0) {
    g_rtp.au_broken = 1;
}
```

### R6（新发现）：把「收到的 NACK」当成「我们的 AU 坏了」

**证据** `PenCG/native/csrc/cgmedia/cm_media.c:241-248`：

```c
/* 2. 认出 NACK（RTPFB, PT=205, fmt=1）：我们不支持 RTX 重传，
 *    收到 NACK 说明某帧确实缺包 → 把当前 AU 标脏并补 PLI。 */
if (rpt == 205 && fmt == 1) {
    cm_rtp_note_stale(s, "peer NACK");
}
```

我们是**纯接收端**：从不发媒体包（`cm_rtp_on_packet` 只有收包路径，无发送路径）。对端发来的 NACK 指的是「**我们发给它的**包缺了」，而我们从没发过 —— 所以这条 NACK 在语义上必然不是针对我们的视频流，它可能属于对端的上行音频、或干脆是乱发的。

后果：`cm_rtp_note_stale`（`:1149-1162`）会 `au_broken = 1` + `vid_au_drop++` + 锁外 `kf_request()`（补一发 PLI）。一旦对端周期性发 NACK，就按 NACK 频率丢我们的 AU。

同一段注释的第 3 点（`:249-251`「加密的 SRTCP 解不开时不做任何破坏性动作」）说明作者已经意识到「乱认会每循环丢一帧」的风险 —— 但第 2 点自己就踩了这个坑。

**建议改法**：删掉这条分支（只保留 `rx_rtcp` 计数用于判断「对端会不会回 RTCP」）；如果一定要用，必须校验 FCI 里的 PID 是否落在我们对端视频 seq 空间内。

### R4（新发现）：IDR hold 窗口被下一个 AU 的 FU-A start 清掉

**证据** `cm_rtp.c:716-723`（hold）：

```c
if (key && g_rtp.au_broken && g_rtp.au_hold_state == 0) {
    g_rtp.au_hold_state = 1;
    g_rtp.au_hold_until_ms = now + IDR_HOLD_MS;   /* 30ms */
    ...
    return;   /* 保留 AU 状态，等 cm_rtp_flush_held 或补洞完成 */
}
```

`cm_rtp.c:1047-1050`（下一个 AU 的 FU-A start）：

```c
if (g_rtp.in_fu) {
    ...
    /* 上一个 AU 的 FU-A 没收到 end 就被新 start 打断 */
    rtp_reset_au();
} else if (g_rtp.au_broken) {
    /* 之前的孤儿分片留下的破坏标记；新 AU 从干净的 start 起 */
    rtp_reset_au();          /* ← 这里把 au_hold_state 也清成 0，挂起的 IDR 直接消失 */
}
```

hold 挂起后 `flush_frame` 已 return，而 `in_fu` 在 flush 之前就被清成了 0（`:1101-1103`：`if (end) { g_rtp.in_fu = 0; flush_frame(s); }`），所以**第一个分支永远拦不住它**，必然落到 `else if (au_broken)`。此时 `au_broken` 仍为 1（hold 那条路径没调 `rtp_reset_au`）。于是下一个 AU 的 FU-A start 一到 → `rtp_reset_au()` → `au_hold_state=0; au_hold_until_ms=0`，**挂起的 IDR 连同它的 `frame` 缓冲一起被丢掉**，重传包回来也无处可放。

60fps 时帧间隔 16.7ms < 30ms → hold 必然失效；30fps 时靠 `cm_rtp_flush_held`（`:889-901`，主循环 20ms 粒度）在 ~30.4ms 放行，但那时重传还没到，放行的是「坏 IDR → 丢弃」。

**后果**：`IDR_HOLD_MS 30` 这条机制只在「帧间隔 > 30ms 且 RTT < 30ms」的窄窗口有收益。不致命（行为退化为「立即丢弃」），但代码意图与实际效果不符，会误导后续调参。

**建议改法**：在 `else if (au_broken)` 分支里先放行挂起的 AU 再 reset：

```c
} else if (g_rtp.au_broken) {
    /* 新 AU 从干净的 start 起。但若上一个 AU 还挂在 IDR hold 窗口里，
     * 必须让它走完 flush（该丢就丢、该补就补），否则 hold 等于从未生效。 */
    if (g_rtp.au_hold_state == 1) {
        g_rtp.au_hold_state = 2;
        g_rtp.au_hold_until_ms = 0;
        flush_frame(s);
    }
    rtp_reset_au();
}
```

### P1-7（lead 疑点，成立但风险可控）：IDR 饥饿放行后没有「重新收紧」机制

`cm_rtp.c:782-801`，`kf_starve_ms` 起算后 4s 仍无 IDR 就：

```c
s->stats.vid_relax++;
s->have_kf = 1;           /* 开门：后续 P 帧不再走闸门 */
s->last_kf_ms = now;      /* 假装刚有过关键帧 */
s->kf_starve_ms = 0;
```

确认：**没有任何「放行后 N 秒内仍无 IDR 就重新关闸」的机制**。`have_kf` 之后只有两条路径能再清 0：坏 IDR AU（`:727-736`）或连续 `DROP_STREAK_FORCE_KF=3` 个坏 P AU（`:751-758`）。如果网关此后只发 P 帧、且都不坏，`have_kf` 永久为 1 → 闸门永久打开 → 持续花屏（bug4 症状回归，但不会冻结）。

**缓解事实（使风险可控）**：

1. `cm_kf_tick`（`cm_cmd.c:93-110`）在 `have_kf && now-last_kf_ms >= 2000` 时**仍每 2 秒发一次 PLI**，恢复请求不会停；
2. 一旦真 IDR 到达并完整拼成，自然愈合；
3. 放行是**主动用花屏换回画面在动**，比永久冻结好——这是刻意的取舍，注释 `:776-781` 写得很清楚。

**结论**：这条不列为 blocking。若要加保险，建议放行时同时置一个 `relax_until_ms = now + 8000`，到期仍无完整 IDR 则 `have_kf = 0` 重新关闸（会再冻结，但至少不是无限花屏）。属于策略选择，需要产品判断。

---

## 🟡 P2

### L1：NACK 在真机上完全不可观测

两处叠加：

1. **stat JSON 没有 nack 字段**。`cm_media.c:112-129` 的 JSON 有 `lost/jitterMs/auDrop/gateDrop/kfReq/dup/reorder/spsInject/rtcpIn/rrSent`，但 `cm_session.h:130-138` 里定义的 `vid_nack_sent` / `vid_nack_req` / `vid_nack_recovered` / `vid_relax` **一个都没输出**（grep `nack` 在 `cm_media.c` 只命中注释与调用）。
2. **关键诊断日志是 `CM_LOGD`，默认不打**。`cm_util.h:20-23` 的宏是 `CM_LOGD=4`，`cm_cfg.c:32` 默认 `log_level = 3`。而 `"retransmit recovered"`（`cm_rtp.c:1397`）、`"AU resequenced"`（`:858`）、`"IDR AU incomplete, hold"`（`:721`）、`"drop broken P AU"`（`:753`）全是 `CM_LOGD`。
3. JS 侧也不消费：`play.vue:995-1011` 只读 `rx/lost/br/fps/kf/pli/gateDrop/auDrop/kfReq`。

**后果**：NACK 是本轮最重要的机制之一，但真机日志里**无法回答「NACK 有没有被网关响应」**。`vid_nack_recovered / vid_nack_req` 这个比值正是注释（`cm_session.h:132-134`）自己点明的「网关重传响应率」，却没暴露出来。

**建议**：stat JSON 增 4 个字段（`nackSent/nackReq/nackRecovered/relax`）；把 `"AU repaired by retransmissions"`（`:707`）与 `"retransmit recovered"` 的限速版升到 `CM_LOGI`。

### L2：`gap_drop()` 是死函数，且语义被三处内联重复实现

`cm_rtp.c:441-461` 的 `gap_drop(uint16_t seq)` **全文件无调用者**（`grep -n "gap_drop("` 仅命中定义处）。它注释里说的三件事（重传到达 / AU 丢弃 / TTL 过期 → 摘 gap + 同步摘 `au_miss` + `au_missing--`）实际分别由：

- `gap_take`（`:956-977`）
- `gap_purge_expired` 内联（`:472-482`）
- `cm_rtp_take_nack_gaps` 轮数用尽分支内联（`:526-537`）

各实现了一遍。当前无功能错误，但改一处忘另两处是必然的维护性风险。建议三处统一改调 `gap_drop`。

### L3：`screen.js` 保活失败时 1s 重试，无退避无上限

`screen.js:511-516`：

```js
const sent = keepOnce() !== false;
const next = sent ? state.intervalMs : 1000;
state.timer = setTimeout(armTick, next);
```

若 brightness 后端永久不可用（`keepOnce` 恒返回 false），会形成**每秒一次探测 + 每秒一条日志**的长期风暴。构建 warning 里就有「未找到模块 brightness,global,cgnative,cgmedia」，说明模块缺失是真实可能的状态（真机当前可用）。建议指数退避：1s → 2s → 4s → … → 20s 上限。

### L4 / P2-11：日志轮转 —— **无法核实，只能定性**

- `src/services/log.js`（84 行全文已读）**没有任何文件轮转逻辑**：`MAX = 300` 只是内存环形缓冲（`:10`、`:39`），落盘由 Falcon 运行时的 `console.error`（`:42-44`）直接追加到 `/userdata/applog/loli_s6_dev.txt`。
- native 侧 `cm_log`（`cm_util.c:19-29`）走 **stdout**（`fprintf(stdout, ...)`），是否与 JS 同文件见下条 L5。
- 我们**能控制的是写入量**，实测/推算的稳态速率：JS 侧 play 页 `probe` 20s 一条（`play.vue:1364-1376`）+ 事件日志；native 侧 `RR sent` 10s、`pt histogram` 10s、健康告警 10s（`cm_media.c:60-67`、`:154-166`）→ 合计约 0.3~0.4 行/秒。
- `/userdata` 剩余空间与文件当前大小：**本次无法核实**（设备离线、任务禁止 adb）。lead 提到的 5.9MB 我无法独立确认，也不反驳。

**结论**：不能证明会写满，也不能证明不会。建议：① 让 native 的 `cm_log` 按大小自轮转（它自己在写 stdout，可控）；② 或交付前把 `logLevel` 默认值从 3 降到 2（只留 W/E），静态日志量可降一个数量级且不丢关键信息。

### L5：两份文档对「native stdout 是否落进同一个日志文件」结论矛盾

- `docs/VERIFY-playbook.md:298`：「一条 `[cgmedia]` 都没有 → 先确认 native stdout 是否落进 `loli_s6_dev.txt`（**待现场确认**）」
- `docs/BUGFIX-bug1-freeze-verify.md:17`：「JS 日志（`loli_s6_dev.txt`）、native 日志（`DictPen_*.log` 里的 `[cgmedia]`）两个文件是分开的，**已实测确认**」

两份都是本批未跟踪的新文档，结论直接冲突。这会影响所有「用日志证明某机制生效」的推论，建议现场核实后统一。**本文所有涉及 native 日志行是否可见的判断，都按「未定论」处理。**

### L6：`flush_frame` 重排循环未检查 `cm_buf_put` 返回值

`cm_rtp.c:851-854`：

```c
cm_buf_put(&g_rtp.au_sorted, g_rtp.frame.p + g_rtp.segs[t].off, (size_t)g_rtp.segs[t].len);
```

`cm_buf_put`（`cm_util.c:69-75`）在 `cm_buf_reserve` 失败（`cap > 1<<30`）时返回 -1，此处未判断 → 静默得到短 buffer。要 1GB 才会发生，纯理论边界，但改起来只有一行。

---

## 🟢 确认无问题项（lead 列的疑点，逐条给出依据）

### P0-1：分片表溢出 —— **非问题，已正确修复**

lead 的担心（表满置 `seg_ordered=0` 但不加段 → 重排只用登记过的段 → 溢出部分被静默截断）在当前代码里**已经不成立**：

`cm_rtp.c:905-931`：

```c
static void au_seg_add(uint16_t seq, uint32_t off, uint32_t len) {
    int n = g_rtp.seg_n;
    if (n >= AU_SEG_MAX) {
        /* ... 绝不能只置 seg_ordered=0 —— 那样 flush 会走重排路径，
         * 而重排只输出 segs[] 里登记过的段，后面这几十个分片会被静默截断 ... */
        g_rtp.seg_overflow = 1;
        return;
    }
```

`cm_rtp.c:836`：

```c
if (!g_rtp.seg_ordered && g_rtp.seg_n > 1 && !g_rtp.seg_overflow) {
```

溢出时**放弃重排**，直接用 `frame` 原始内容（`frame` 由 `annexb_put` 独立维护，含全部分片）。

`AU_SEG_MAX = 512`（`:109`）。一个 720p IDR 约 100KB / MTU 1200 ≈ 85 包，离 512 很远；`FRAME_CAP` 是 4MB ≈ 3400 包，理论上可达但需要极端情况。

**一个维护性提醒**：这个修复的正确性完全依赖 `seg_overflow` 与 `seg_ordered` **分开置位**。以后若有人「简化」成只用一个标志，截断 bug 立刻复活。`:907-919` 的注释已经写明，不要再精简。

### P0-2：`gap_add` 表满 memmove —— **非问题**

`cm_rtp.c:429-434`：

```c
if (g_rtp.gap_n >= NACK_GAP_MAX) {   /* 256 */
    memmove(&g_rtp.gaps[0], &g_rtp.gaps[1],
            sizeof(g_rtp.gaps[0]) * (size_t)(NACK_GAP_MAX - 1));   /* 搬 255 条 */
    g_rtp.gap_n = NACK_GAP_MAX - 1;                                 /* → 255 */
}
g_rtp.gaps[g_rtp.gap_n].seq = seq;   /* 写 [255]，最后一个元素 */
```

源 `[1..255]`、目的 `[0..254]`、长度 255 —— 完全在 `gaps[256]` 内，无重叠问题之外的问题（memmove 本就允许重叠）。写入下标最大 255，界内。**无越界、无 off-by-one。**

### P0-3：`au_miss[]` 与 `au_missing` —— **非问题，五处全配对**

`au_missing++` 只在 `au_mark_missing`（`:492-499`）；`au_missing--` 有三处，且每处都同时从 `au_miss[]` 摘除：

| 位置 | 动作 |
| --- | --- |
| `gap_take` `:966-974` | 摘 gap + 摘 au_miss + `--` |
| `gap_purge_expired` `:472-482` | 摘 gap + 摘 au_miss + `--` |
| `take_nack_gaps` 轮数用尽 `:526-537` | 摘 gap + 摘 au_miss + `--` |
| `rtp_reset_au` `:387` | `au_missing = 0` |

**一个观察项（不是 P0）**：`au_mark_missing` 在 `au_missing >= AU_MISS_MAX (256)` 时直接 return（不记）。于是 flush 的修复判据 `... && au_missing == 0` 在仍有 >256 个缺口时可能**误判为已修复**。实际很难命中（`diff > 64` 已经先把这类情况判成结构性损坏，`:1364-1369`），与 F1 修好后影响更小。列入 P2 观察。

### P0-4：主循环 `flush_frame` 与收包线程 —— **非问题，结构上不可能**

比「锁序推理」更硬的证据：**整个 cgmedia 媒体栈只有一个线程**。

- `PenCG/native/csrc/cgmedia/cgmedia.c:245` 是 `cgmedia` 目录下唯一的 `pthread_create`，线程函数 `cm_rx_main`（`:166-213`）。全仓库另有 3 个线程在 `cgnative.c`（`:919` job_thread、`:2030` ws_thread、`:2698` scan_worker），但它们都在 `cgnative/` 里，**不碰 `cm_*` 的任何锁或全局量**。
- `cm_media_loop` 的唯一调用者是 `cgmedia.c:208`（`grep -rn cm_media_loop` 全仓库只有定义 + 声明 + 这一处调用）。
- 因此 `cm_rtp_on_packet`（收包）、`cm_rtp_flush_held`、`cm_nack_tick`、`cm_rtcp_tick`、`cm_kf_tick`、`cm_rtp_reset`、建/拆 pipeline、`emit_stat` **全都在同一个线程里串行执行**，从不同时存在两个持 `g_rtp_mu` 或 `gst_mu` 的执行流。

跨线程的只有 JS 线程：`cm_cmd_apply` 的 ops 在 JS 线程跑，只与媒体线程通过 `s->gst_mu` 交叉。

按锁序再验一遍（作为第二重保险）：`g_rtp_mu → gst_mu` 是**单向**的——
- `cm_rtp.c:617-620`（`note_sps` 持 gst_mu 调 `cm_box_set_caps_video`）不取 `g_rtp_mu`；
- `cm_rtp.c:865-881`（`flush_frame` push）持 `gst_mu`，其外层已持 `g_rtp_mu`；
- `cm_cmd.c:508-511`（setRect）、`:529-542`（setAudio）、`:568-597`（setVideoVisible）块内都只调 `cm_box_*`，`cm_send_pli` 一律在 unlock 之后；
- `cm_media.c:189-196`、`:362-365` 同样只持 gst_mu 不取 g_rtp_mu。

**无 ABBA。**

### P0-5：`nack_sort_gaps` 偏移算术 —— **非问题**

`cm_cmd.c:398-405`：

```c
static void nack_sort_gaps(unsigned char *gaps, int n) {
    int i, j;
    unsigned char cur[4];
    for (i = 1; i < n; i++) {
        uint16_t cs = (uint16_t)((gaps[i*4] << 8) | gaps[i*4+1]);
        memcpy(cur, gaps + i * 4, 4);
        j = i - 1;
        while (j >= 0) {
            uint16_t js = (uint16_t)((gaps[j*4] << 8) | gaps[j*4+1]);
            if ((int16_t)(js - cs) <= 0) break;
            memcpy(gaps + (j+1) * 4, gaps + j * 4, 4);
            j--;
        }
        memcpy(gaps + (j+1) * 4, cur, 4);
    }
}
```

`i ∈ [1, n)`、`j ∈ [-1, i-1]` → `(j+1) ∈ [0, i] ⊆ [0, n)` → 写址恒在 `[0, n*4)`。**无越界。** 另外 `(int16_t)(js - cs)` 用的是 16 位回绕比较，对 seq 环绕正确。

`nack_pack_fci`（`:355-393`）同样复核：`d = (int)(int16_t)(s2 - pid); if (d <= 0) {...} if (d > 16) break; blp |= (1u << (d - 1));` → `d ∈ [1,16]` → 移位 `0..15`，界内；写 `out[nfci*4+0..3]`，`nfci < max_fci` 循环条件保证不越界。**无 off-by-one。**

### P1-6：`drop_streak` 增减配对 —— **基本成立，非问题**

`grep -n drop_streak` 结果（`:139` 是字段声明）：

| 行 | 动作 | 场景 |
| --- | --- | --- |
| `:399` | `= 0` | `rtp_reset_all()` 换会话 |
| `:733` | `= 0` | 坏 IDR AU（`:727-736`） |
| `:751` | `++` | flush 内坏 P AU |
| `:755` | 判断 `>= 3` | 才 `have_kf=0` |
| `:799` | `++` | 闸门丢 P 帧 |
| `:870` | `= 0` | 成功推帧 |
| `:1045` | `++` | FU-A truncated AU |
| `:1421` | `++` | AU over FRAME_CAP |

`++` 5 处、清 0 3 处，**所有退出路径都有对应清零**（推帧成功 / 坏 IDR / 换会话）。观察到的唯一瑕疵：同一个 AU 可能在 `on_packet`（`:1421`）与 flush（`:751`）各记一次，使 streak 提前触达 3 —— 但这只会让「降级等 IDR」更早发生，属安全方向，不是 bug。

### P1-8：`flush-stop(TRUE)` 的 reset_time —— **未发现问题，但标注「未验证」**

`cm_gst.c:1386-1395`：

```c
ev = gst_event_new_flush_start();
if (ev && !gst_pad_push_event(pad, ev)) CM_LOGW("flush-start push failed");
ev = gst_event_new_flush_stop(1 /* reset_time=TRUE */);
if (ev && !gst_pad_push_event(pad, ev)) CM_LOGW("flush-stop push failed");
```

推理：`appsrc` 是 `is-live=true format=time do-timestamp=true`（`cm_gst.c:247`），我们**从不设置 buffer PTS**（`cm_box_push` 里 `(void)pts_ns;`，`:1229-1240` 的长注释说明真机 1.22.0 上 `gst_buffer_get_pts` 符号缺失）。所以时间戳全部由 appsrc 按推入时的 pipeline 时钟生成。`reset_time=TRUE` 会让 running-time 归零并开一个新 segment，之后 appsrc 继续按新 segment 生成时间戳 —— **两端一致，不会出现「后续 buffer 的时间戳早于已显示帧」被 kmssink 丢弃**。而且注释 `:1356-1358` 明确写了「不配 flush-stop(TRUE) 会让 pipeline 永远停在 flushing」。

**结论**：没有证据表明有害，且去掉它反而有明确危害。但这条是纯代码推理，**「分辨率热切换」场景在真机上是否验证过，我不知道**（`.tmp` 目录为空，无法查日志）。建议列一条真机验证项：切画质 / 换游戏（分辨率变化）后观察是否有 `video caps CHANGED` 且画面恢复。

### P1-9：首次协商分辨率 —— **非问题，前提不成立**

lead 的担心是「appsrc caps 里预设了分辨率，首个 SPS 与之不符需要谁重协商」。实际上两套管线分支的 appsrc caps **都不含 width/height**：

`cm_gst.c:1077-1079`（`cm_box_create` 里软硬解共用）：

```c
GstCaps *caps = gst_caps_from_string(
    "video/x-h264,stream-format=byte-stream,alignment=au");
if (caps && b->src) gst_app_src_set_caps(b->src, caps);
```

硬解管线串（`:247`）与软解管线串（`:257-260`）里也没有 `width=`/`height=`。分辨率完全由 `h264parse` 从 SPS 推导，**首次天然一致，不存在「不符」**。`cm_box_set_caps_video` 的 `old_w==0` 不 flush 也就完全合理（那时下游确实还没协商过，`b->width` 只在 `:1396-1397` 被赋值）。

### P2-10：play.vue 定时器泄漏 / L1 雪崩 —— **非问题**

三条新增定时器在**所有销毁路径**都被清：

| 定时器 | 清理点 |
| --- | --- |
| `stallL1Timer` | `stallReset()` `play.vue:1385-1388` |
| `freezeL1Timer` | `stallReset()` `play.vue:1398-1401` |
| `hideGraceTimer` | `clearHideGrace()` `:1847-1852` |

调用链覆盖完整性：
- `cleanup()`（`:1894-1940`）先 `this.stallReset()`（`:1899`）再 `this.clearHideGrace()`（`:1914`）→ 三条全清；
- `bgTeardown()`（`:1856-1892`）先 `clearHideGrace()`（`:1870`）再 `stallReset()`（`:1872`）→ 三条全清；
- `onShow()`（`:627` 处 `clearHideGrace` + `:635` 起内联清 stall/freeze 定时器）→ 回来时全清；
- `beforeDestroy`（`:585`）与 `onUnload`（`:590`）都调 `cleanup`，靠 `this.dead` 幂等。

`clearTimers()`（`:1289-1297`）确实**不**清这三个，只清 `clearQualityKf()` 与 `this.timer` —— 这是设计如此（分别由 `stallReset` / `clearHideGrace` 负责），不是遗漏；但**调用方必须记得两条都调**，这是脆弱点，注释已在 `:1393-1396` 提示。

`kfTimers` 是被跟踪的：`setQuality` 里赋值 `:1084-1087`，`clearQualityKf()` `:1095-1102` 逐个 `clearTimeout`，`clearTimers()` 会调它。剩余的裸 `setTimeout` 只有两处且都无害（`setHint` 的 2600ms 回调自带 `if (self.qualityHint === text)` 守卫；`kbSystem` 的 250ms）。

**L1 雪崩复核**：`stallL1Timer` 回调（`:1498-1504`）重入 `checkStall`，此时 `stallTries` 已为 1 → 走 L2（`stallHealL2` 置 2），**不会重复起 L1**。freeze 侧回调（`:1712-1720`）有 `if (self.stallTries !== 1) return;`，也不会雪崩。

### P2-12：`audio_route_restore` 覆盖用户运行中调的音量 —— **非问题，结构上不可能**

`cm_gst.c:466-500`（`cm_audio_route_restore`）的实际行为：

1. `audio_snapshot`（`:433-452`）**只读** Playback Path 与 card1#54（音量），把 card1#54 存进 `audio_amp_snap` 仅用于日志自证；
2. `audio_route_ensure`（`:504-530`）只在 Playback Path == `"OFF"` 时才写 `"SPK"`（`:518`），并记 `audio_path_wrote`；遇到 HP/BT/其它值直接 `leave it`（`:515-516`）；
3. `cm_audio_route_restore` **只还原 Playback Path**，且带守卫：`audio_mixer_get_path(cur) == 0 && strcmp(cur, audio_path_wrote) == 0` 才写回快照值，否则 `Playback Path now %s (not ours %s), leave it`（`:482-485`）；
4. **card1#54 全程没有任何写入路径**（只 `audio_amp_read`，定义在 `:399`，唯一调用点 `:445` 与 `:492`），恢复时只回读打日志（`:494` `unchanged (we never write it)` / `:497` `changed by system, not us`）。

所以「用户运行中把音量从 50% 调到 20%，退出时被还原成 50%」在结构上不可能发生：我们从没写过音量。守卫逻辑我也验过（`audio_snap_done` 先置 0 保证重入立即返回，不会二次还原）。

---

## 清单

### 上线前必须修的（blocking）

1. **S1** `src/dev-autostart.js:17` → `DEV_AUTOSTART = null`（同时删 `:33` 注释里的手机号/密码）。**这个不修，包一装上去就自动跳游戏。**
2. **F1** `cm_rtp.c:369-390` 的 `rtp_reset_au()` 补清 `au_broken_fixable` 与 `au_start_ms`；`:993` 补显式置 `au_broken_fixable = 0`。
3. **N1** `cm_cmd.c:409` 缓冲区加余量，且 `enc <= 0` 时不发明文。
4. **R5** `cm_rtp.c:484` 加「seq 属于当前 AU」判断（`au_first_seq/au_last_seq` 已备好）。
5. **R2** `cm_rtp.c:1395` 的 `gap_take` 命中后校验归属，不属于当前 AU 就丢。

> 前三条（S1/F1/N1）我判断是「必须修」，因为它们分别对应：包行为错误、坏帧污染参考链、安全机制在最需要时静默失效。R5/R2 是「丢包后花屏/恢复慢」的直接成因，但不会让功能整体坏掉 —— 如果时间紧，可以放到下一批，但要记在案。

### 可以延后的

- **R3**（重排覆盖补插参数集）—— 需要「裸 IDR」与「重传乱序」同时成立，单独概率低于 R5/R2，但两者在弱网场景里同源、正相关，症状是绿块。**如果这批要修 NACK 链路，建议连它一起修**（都在 `flush_frame` 里，改动面重叠）。
- **R4**（IDR hold 被清）—— 不致命，是「机制没生效」而非「引入故障」，顺手改。
- **R6**（peer NACK 误解）—— 取决于网关是否真的发 NACK；先加计数观察，别盲删。
- **P1-7**（放行后无收紧）—— 策略问题，需要产品判断「无限花屏 vs 反复冻结」哪个更能接受。
- **L1**（NACK 可观测性）—— 强烈建议同批加（4 个 JSON 字段 + 2 行日志升级），否则下一轮真机验证仍然无法判断 NACK 是否生效。
- **L2/L3/L6** —— 纯维护性。
- **L4/L5** —— 需要现场设备才能定论。

### 需要真机验证才能确认的（我给出的结论都是推理，不是实测）

1. **P1-8**：分辨率热切换后 flush-start/flush-stop 是否让画面正常恢复（`video caps CHANGED` → 恢复出画）。
2. **L4/L5**：`/userdata` 剩余空间、日志增长速度、native stdout 落哪个文件。
3. **R6**：网关到底会不会发 NACK（看 `rtcpIn` 是否增长 + 若有日志看 fmt）。
4. **N1 修好后**：`vid_nack_recovered / vid_nack_req` 是否 > 0（这决定「NACK 这条路到底有没有用」，是 bug1 弱网场景能否收敛的关键数据）。

---

## 补充：本次审查的边界（避免误解）

- 我**没有**逐行审阅 `src/pages/index/index.vue`（+232）、`src/components/mine-body.vue`（+204）、`src/services/auth.js`（+140）、`src/services/nav.js`（+59）、`src/pages/login/login.vue`（+40）、`src/pages/diag/diag.vue`（+8）、`src/services/im.js`、`src/base-page.js`、`src/pages/mine/mine.vue`。这些只在「dev-autostart 消费方」「stat 字段消费方」这两处被顺带核过。**bug2（登录）那一批改动本次没有覆盖。**
- 我**没有**跑构建、没有碰设备。所有结论都来自源码静态阅读。
- `src/pages/play/play.vue` 的 `checkStall` / `checkFreeze` 两级判据我只核了「定时器与状态机是否自洽、是否会雪崩、是否被 hidden 正确短路」，**没有**评估 20s / 12s / 3 次采样这些阈值是否合适 —— 那需要真机数据。
- 报告里凡写「未验证」「无法核实」的地方，都是我确实没有证据，不是省略。
