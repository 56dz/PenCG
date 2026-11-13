# 修复：bug 3「原神」游玩时上一帧马赛克残留 / bug 4「明日方舟终末地」一连上就马赛克+绿屏

> **bug 3**（用户原话）：「用『原神』测试，进入游戏后游玩画面上经常性出现"上一帧的马赛克残留"
> （stale tiles / 旧宏块残留，画面局部卡住不动）。」
>
> **bug 4**（用户原话）：「『明日方舟终末地』建立连接后画面一开始就有马赛克和绿屏。」

两个 bug 是**同一条视频链路的两端**：bug 4 是「一开始就没有可用的参考帧」，
bug 3 是「运行中参考帧被缺片的 AU 污染后无法自愈」。所以修复放在同一份补丁、
同一份文档里，用同一组计数器判定。

复现基线：X6 Pro 9D91500007505171，`Persist` 名 `PenCG`，视频链路
UDP → 自写 STUN/DTLS-SRTP → RTP 拆包 → `appsrc` → `h264parse` → `mppvideodec rotation=270`
→ `videoconvert` → `kmssink plane-id=76`。

**本次只能在代码层面定案，无法做真机 A/B**：设备上装的是旧版 1.0.3 且当前没在跑，
`/userdata/applog/` 里 `grep -a pencg` 命中数为 **0**，没有可作为对照的历史日志。

> 事后补充（2026-11-12 晚真机复测）：那次「命中数为 0」**不是代码没跑，是取证通道用错了**。
> 当时 `PenCG/src/services/log.js` 还在走 `console.warn`，而 X6 Pro 的 Falcon 运行时里
> **只有 `console.error` 会写盘**，`console.warn` / `console.log` 完全不落盘，所以日志根本没产生。
> 正确做法是用 `P=$(pidof miniapp); readlink /proc/$P/fd/1` 解析出**当前进程**的日志文件
> 再 grep，不要写死文件名。详见 **§五 步骤 -1**。
> （2026-11-14 再更正：当时说「正确路径是 `loli_s6_dev.txt`、不是 `DictPen_*.log`」也是错的 ——
> 实测 `loli_s6_dev.txt` 只有 1 字节、是空文件，`DictPen_*.log` 才是真日志。）
>
> ⚠️ **定案（2026-11-13 20:35）：bug3/bug4 的真正根因是 `kmssink` 的 `sync=true`，已改为
> `sync=false`（`PenCG/native/csrc/cgmedia/cm_gst.c:253-255`），两个游戏已由用户目视确认修好。
> 本文 §一～§四 里的 RTP 层修复是有效的健壮性加固，但不是这两个 bug 的根因 ——
> 读本文前请先看 §九「定案」，以及 `docs/BUGFIX-bug3-bug4-gst.md` 的 §7。**

所以下面严格分成「**已由代码证明**」和「**需真机 A/B 验证**」两档，
真机验证步骤见 §五，全部是可直接粘贴的 adb 命令。

---

## 一、bug 3：原神游玩时「上一帧马赛克残留」

### 1.1 用户看到的是什么（症状→链路的映射）

「局部卡住不动、旧宏块残留」在 H.264 硬解里只有一种成因：

> 解码器收到了**缺片的访问单元（AU）**，MPP 不认为这是错误，
> 于是用**上一帧的参考像素**去补齐缺失的宏块并上屏；
> 补出来的错误像素又被写进 DPB 当作新的参考帧 →
> **错误从此沿参考链一直传下去，直到下一个 IDR 才清掉**。

关键前提是 `mppvideodec` 的 `ignore-error` 出厂默认 `true`（§三 hunk 1 已在真机
`gst-inspect-1.0` 核实），所以「坏帧不上屏」这层保护**默认是关掉的**。
软解 `avdec_h264` 自己会丢坏帧，所以我们历史上一直没在软解路径上看到这个症状
——这也解释了为什么它只在「直播 + 硬解」组合下暴露。

### 1.2 已由代码证明的根因（补丁前）

#### 根因 A：RTP 层完全不看序号，缺片的 AU 照样进解码器 ★主因

补丁前 `cm_rtp.c` 的 `rtp_ctx` 只有四个字段：

```
$ sed -n '28,34p' PenCG/native/csrc/cgmedia/cm_rtp.c   # 补丁前
typedef struct {
    cm_buf frame;
    int in_fu;
    int have_data;
    uint32_t last_ts;
} rtp_ctx;
```

**没有 `seq`。** `cm_rtp_on_packet()` 也从不读 `pkt[2..3]`。于是：

1. 网关发出 FU-A 的第 1/5、`2/5`、`4/5`、`5/5` 片，中间的 `3/5` 被丢（UDP 丢包 / SRTP
   认证失败 / 乱序）；
2. 第 4 片、第 5 片照旧 `cm_buf_put(&g_rtp.frame, p + 2, n - 2)` 追加进去
   （补丁前 `cm_rtp.c:334`）；
3. `end` 分片触发 `flush_frame()`（补丁前 `cm_rtp.c:337`），它只看
   `have_data && frame.len > 0`（补丁前 `cm_rtp.c:260-261`）就 push；
4. **缺一片的 AU 进了 appsrc** → MPP 拿旧参考帧补齐 → 用户看到马赛克残留。

注意这个 AU 的字节流本身是**语法合法**的（只是少了一段 slice data），
`h264parse` 也发现不了，所以整条链路上没有任何一层会拒绝它。

> 顺带一个「信号已定义但从未接线」的证据：`cm_session.h:75-76` 有
> `uint32_t last_seq; int have_last_seq;` 两个字段，全项目**没有任何一处赋值**
> （`grep -rn 'have_last_seq' PenCG/native/` 只命中定义）。说明当初就打算做序号
> 连续性校验，但没写完。

#### 根因 B：STAP-A 无条件当关键帧，导致「IDR 丢了却永不补」 ★让残留持续的元凶

补丁前 STAP-A(24) 分支的收尾是：

```c
/* 补丁前 cm_rtp.c:309 */
if (marker) flush_frame(s, 1);   /* ← 硬编码 key=1 */
```

**任何** STAP-A——哪怕它只含 SPS + PPS + SEI、一条 slice 都没有——都会被
`flush_frame(s, 1)` 当成关键帧，进而 `mark_keyframe()`：

```c
/* cm_rtp.c:352-356（未变） */
static void mark_keyframe(cm_sess *s) {
    s->kf_count++;
    s->have_kf = 1;
    s->last_kf_ms = cm_now_ms();     /* ← 刷新「关键帧很新鲜」 */
}
```

而 `cm_kf_tick()` 的稳态判据正是「关键帧还新鲜就不发 PLI」：

```c
/* cm_cmd.c:67-68 */
if (s->have_kf && (now - s->last_kf_ms) < 2000) return;
```

于是形成死锁：网关周期性发一个只带参数集的 STAP-A → 我们误判成 IDR →
`last_kf_ms` 被刷新 → 2 秒窗口内不发 PLI → 真正的 IDR 丢了也**永远要不回来**
→ 解码器抱着被污染的参考帧一直解 → **马赛克残留持续存在而不是闪一帧就过**。

这一条是 bug 3 与「普通的偶发丢包」区别开来的关键：普通丢包只要下一个 IDR 来
就自动恢复，而这里**下一个 IDR 永远不会被请求**。

#### 根因 C：SRTP 认证失败只丢一个包，不通知识别层「这帧已经残了」★第二条上游

补丁前 `cm_media.c` 的认证失败分支只有两行：

```c
/* 补丁前 cm_media.c:142-143 */
} else if (plen == -7) {
    CM_LOGD("#%d srtp auth fail", s->id);
}
```

`cm_srtp.c` 那边也只做计数 + 落盘（`cm_srtp.c:381-399`，`return -7`），
既没有重放窗口，也不向 RTP 层反馈。

所以「SRTP 认证失败」和「UDP 丢包」对拆包层是**完全等价且不可见**的：
丢掉的是 FU-A 中间片时，后续片照样拼进同一个 AU（根因 A）。

为什么会出现认证失败——`cm_srtp.c:356-364` 的 ROC 推算有个缺陷：

```c
if (!ss->inited) { ss->roc = 0; ss->max_seq = seq; ss->inited = 1; }
else if (seq < ss->max_seq && (uint16_t)(ss->max_seq - seq) > 0x8000u) {
    ss->roc++;                       /* 序号回绕 */
}
ss->max_seq = seq;                   /* ← 无条件回退 */
```

`max_seq` 无条件被赋成当前 seq，**一个迟到包就会把 max_seq 拉低**。
后续正常包相对这个被拉低的基准继续算，ROC 从此偏移一档，
之后每一个包的 SRTP 认证都会失败——一次乱序引发一片 auth_fail。

（`cm_srtp.c` 不在本次可改范围，这条写进 §六 遗留风险，修法也给了。）

#### 根因 D：FU-A 丢 start 后静默吞掉整帧

补丁前：

```c
/* 补丁前 cm_rtp.c:326 */  if (g_rtp.in_fu) cm_buf_reset(&g_rtp.frame);
/* 补丁前 cm_rtp.c:333 */  if (!g_rtp.in_fu) return;
```

两处都**不计数、不补 PLI**。丢帧完全隐形，日志里查不到任何痕迹。

### 1.3 需真机 A/B 验证的假设（按可能性排序）

| # | 假设 | 怎么验 | 若成立 |
|---|---|---|---|
| 3-A1 | 丢包率在 1% 量级（典型云游戏弱网），足以每十几秒打坏一个 AU | §五 步骤 3 看 `lost` / `auDrop` 增速 | 本补丁的 `auDrop` 会等于「本来会出现的马赛克次数」 |
| 3-A2 | 网关对 PLI 响应很慢甚至不响应（历史基线 `kf=1 pli=11` 已经是这个形状：17 秒只给 1 个 IDR、我们发了 11 次 PLI） | §五 步骤 3 对比 `kfReq` 涨幅与 `kf` 涨幅 | 补丁能保证「坏帧不上屏」，但恢复速度受网关限制（§六 风险 1） |
| 3-A3 | `SRTCP 未 ready 时退回明文 PLI`（`cm_cmd.c:41-44`）导致 PLI 实际没被网关看到 | §五 步骤 4 看 `PLI sent` 日志里是 `srtcp` 还是 `plain` | 全部 `plain` = PLI 无效，得先修 DTLS 导出密钥时序 |
| 3-A4 | SRTP ROC 误判造成的 auth_fail 风暴（根因 C 后半段） | §五 步骤 3 看 `authFail` 与 `reorder` 是否同步上涨 | 需改 `cm_srtp.c`（§六 风险 2） |

### 1.4 本 bug 的最小补丁（已落地）

全部在 `cm_rtp.c` / `cm_media.c` / `cm_session.h` 内，**未动 `cm_gst.c`**。

| 改动 | 位置 | 作用 |
|---|---|---|
| `rtp_ctx` 扩容：`have_seq/last_seq/au_broken/au_has_idr/au_has_sps/au_has_pps/au_has_slice/drop_streak/sps/pps/sps_ok/pps_ok/au_out/sess_id` | `cm_rtp.c:42-80` | 让拆包层「知道自己在拼什么」 |
| `rtp_reset_au()` / `rtp_reset_all()` | `cm_rtp.c:240` / `cm_rtp.c:254` | 严格区分「清当前 AU」与「清会话」，避免复位过猛/不足 |
| RTP 序号三态判定（重复/连续/缺口/乱序） | `cm_rtp.c:708-751` | **根因 A 的根治点**：任何缺口都把 `au_broken=1` |
| `flush_frame()` 残缺 AU 一律不 push | `cm_rtp.c:411-425` | 残帧永不进解码器 → 参考帧不再被污染 |
| 连续 `DROP_STREAK_FORCE_KF`(=3) 个坏 AU → `have_kf=0` + 补 PLI | `cm_rtp.c:416-422` | 参考链已坏时整条重起，而不是「丢坏帧继续用坏参考」 |
| STAP-A 收尾改成 `flush_frame(s)`（不再硬编码 key=1） | `cm_rtp.c:527-532` | **根因 B 的修复点**：关键帧只由 `au_has_idr`(NAL 5) 决定 |
| FU-A 丢 start / 上一 AU 未收完 → 计账 + 补 PLI | `cm_rtp.c:549-578` | **根因 D**：丢帧不再隐形 |
| `cm_rtp_note_stale()` 对外接口 | `cm_rtp.c:620-632` | **根因 C**：把 SRTP/NACK 的失败反馈进拆包层 |
| `cm_media.c` 的 `plen == -7` 分支识别视频 PT 并调 `cm_rtp_note_stale` | `cm_media.c:251-269` | 认证失败 → 当前 AU 标脏 + 补 PLI |
| `emit_stat` 的 `lost` 从硬编码 0 改成真实 `vid_lost` | `cm_media.c:119-122` | JS 侧终于能看到丢包 |
| `emit_stat` 新增 7 个字段 + 每 10 秒健康告警 | `cm_media.c:89-109`、`cm_media.c:110-144` | 真机定位不再靠肉眼猜 |

**为什么这样改（设计取舍）**

* **为什么在 RTP 层判完整性，而不是在 `h264parse` 之后判？**
  `h264parse` 只做语法解析，缺 slice data 的 AU 在语法上完全合法，它没有能力
  发现问题。必须在还知道「RTP 分片边界」的地方判。

* **为什么 `flush_frame` 的签名从 `(s, key)` 改成 `(s)`？**
  技能文档 `webrtc-realtime.md:264-266` 明确要求「记账只在一处做」。
  补丁前 FU-A 的 `start` 分支和 `flush` 分支各可能记一次，`kf` 会翻倍
  （历史基线 `kf=1` 就偏小）。现在 key 只有一个来源：`g_rtp.au_has_idr`。

* **为什么故意不做「等 IDR 超时就放行 P 帧」的兜底？**
  试过、否掉了，理由写在 `cm_rtp.c:35-43`：没有 IDR 就没有参考帧，
  放行的 P 帧解出来必然满屏绿块/花屏——**正好是 bug 4 的症状**。
  等于用「画面在动」换「画面全废」。现在的取舍是宁可冻结。
  代价写在 §六 风险 1。

* **为什么 `kf_request()` 的限速放在锁外？**
  `cm_send_pli()` 内部会做 UDP send。持 `g_rtp_mu` 在收包热路径上做系统调用，
  一旦对端窗口满就会把整个收包线程卡住（`cm_rtp.c:295-307`、`cm_rtp.c:630-631`）。

* **为什么 `cm_media.c` 收到加密 SRTCP 后什么破坏性动作都不做？**
  `cm_srtp.c` 只实现了 RTP 解密，没有 `cm_srtcp_unprotect`。硬去猜类型的话，
  网关 1Hz 的 RR 会让每个循环都误判「丢了一帧」→ 比原 bug 更糟。
  所以只计数 + 认**明文** NACK（`cm_media.c:231-245`），其余照旧。

**回滚方式（三条命令级）**

1. 单 bug 回滚：把 `cm_rtp.c:411-425` 的 `if (g_rtp.au_broken) {...}` 整块注释掉
   → 立刻退化成补丁前的「坏帧照推」行为，其余计数器仍在。
2. 整体回滚：`git checkout -- PenCG/native/csrc/cgmedia/cm_rtp.c cm_media.c cm_session.h`
   （注意 `cm_gst.c` 若已按 §三 落地，需一并 `git checkout`）。
3. 灰度回滚：把 `DROP_STREAK_FORCE_KF` 从 3 改成 1000（等价于永不升级为
   「强制等 IDR」），只保留「坏帧不入库」这一条——症状会比现在轻但不会完全好。

---

## 二、bug 4：终末地建立连接后「一开始就有马赛克和绿屏」

### 2.1 与 bug 3 的关系

bug 4 是 bug 3 的**首帧特例**，但多了一条独立的、必然触发的成因：
**会话刚开始时 `have_kf == 0`，而补丁前的代码根本不知道这件事。**

绿屏（特别是不均匀的绿色方块）在 MPP 硬解里的标准成因是
「解码器拿到了一个引用了不存在参考帧的 P 帧」——DPB 里全是未初始化的
NV12 缓冲，未初始化内容在 YUV→RGB 后就是绿/紫色块。

### 2.2 已由代码证明的根因（补丁前）

#### 根因 E：没有「首个 IDR 之前不许推 P 帧」的闸门 ★主因

补丁前 `flush_frame()` 的唯一门槛是 `have_data && frame.len > 0`
（补丁前 `cm_rtp.c:260-261`）。**它不看 `s->have_kf`。**

于是会话建立后到达的第一个视频包（几乎一定是一个 P 帧，因为网关是先接上
再等关键帧，或者关键帧还在路上）就被直接推给解码器：

```
P 帧 → mppvideodec（DPB 空）→ 无参考帧 → 输出未初始化 NV12 → 绿块上屏
```

而且这个坏帧会被写进 DPB 当参考，后面几个 P 帧继续在它上面预测，
**绿屏会持续若干帧**——正好对上用户说的「一开始就有马赛克和绿屏」。

这条对**所有**游戏都成立，但终末地更容易撞上，因为它的首帧码率/分辨率更高
（§2.3），开局那几个 P 帧的体积更大、被 `FRAME_CAP` 打断的概率更高。

#### 根因 F：SPS/PPS 只随「开局那一个 STAP-A」下发，丢了就没救

真机网关的常见形态（`webrtc-realtime.md:278-281` 记录过）是：
**只有开局那个 STAP-A 带 SPS+PPS，之后所有 IDR 都是裸 NAL。**

补丁前没有任何参数集缓存机制，于是只要那个 STAP-A 丢了 / 被乱序挤掉 /
被 `FRAME_CAP`（`cm_rtp.c:26`，4MB）的 `cm_buf_reset` 清掉（补丁前
`cm_rtp.c:396-400`），解码器就**永远缺参数集**，后面所有 IDR 都解不出来。

更糟的是 `note_sps()` 也有个逻辑漏洞（`cm_rtp.c:327`，**这条改动后仍然存在**）：

```c
changed = (s->sps_profile && (old_prof != prof || old_lev != lev));
```

`&& s->sps_profile` 意味着**首次 SPS（`sps_profile == 0`）永远不算 changed**，
于是「首个 SPS 触发一次 PLI 突发」这条保险从来没有生效过。
这是有意的（首个 SPS 不需要「因为变了所以重要」），但代价是
「首个 SPS 本身丢了」这件事也没有任何补偿——只能靠下面的补插机制兜。

#### 根因 G：`cm_box_set_caps_video()` 是空实现，SPS 变化时解码器不重新起

```c
/* cm_gst.c:1242-1247（未改，属 gst 侧）*/
void cm_box_set_caps_video(cm_box *b, int w, int h) {
    if (!b || !b->src) return;
    /* h264parse 会重算 caps，这里只在拿到真实分辨率时更新统计值 */
    b->width = w;
    b->height = h;
}
```

注释说得没错：`h264parse` 确实会重算 caps。但**重算 caps ≠ flush 解码器**。
MPP 内部还留着按旧 SPS 初始化的 DPB；`videoconvert` 还按旧的
`render-rectangle` + 旧 stride 在跑。真机症状就是「换档/换 SPS 后画面错位、
一层旧内容叠在新内容上」。

真机取证支持这条：`/sys/kernel/debug/dri/0/state` 里
`plane[76]: Esmart1-win0` 的 `src-pos=1280.000000x720.000000`、
`crtc-pos=240x427+107+299`——分辨率被 `apply_rect` 的公式钉死在 1280x720，
一旦码流真的换了尺寸，KMS 侧的 src rect 不会跟着变。

#### 根因 H：无 RTCP RR，网关可能降级/不推关键帧

`grep -rn '201\|cm_rtcp' PenCG/native/csrc/cgmedia/` **零命中**。
我方只有 `cm_send_pli()`（`cm_cmd.c:22-52`）这一个 RTCP 出口，
**从不发 Receiver Report**。

很多 SFU 的行为是「收不到 RR 就认为接收端不健康 → 降低码率 / 停发关键帧」。
对首帧绿屏来说，隐含的后果是「等不到 IDR，只能一直推 P 帧」。

调用链现状：`cm_net.c:333` 会把 RTCP 分类出来返回 `cls == 2`，
补丁前 `cm_media.c` 对它只打一行 `CM_LOGD` 就丢掉（补丁前 `cm_media.c:135-137`）。

### 2.3 需真机 A/B 验证的假设（按可能性排序）

| # | 假设 | 怎么验 | 结论怎么用 |
|---|---|---|---|
| 4-A1 | 终末地首帧分辨率/profile 与原神不同（1080p 或 High@4.1 对 720p High@3.1） | §五 步骤 2 看首条 `SPS reports WxH profile=P level=L` | 若为 1080p，`FRAME_CAP` 4MB 与 appsrc `max-bytes` 2MB 都偏紧（§六 风险 4） |
| 4-A2 | 终末地开局第一个 STAP-A 丢失/乱序（弱网下的首包最容易丢） | §五 步骤 3 看 `spsInject` 是否 ≥1 | ≥1 = 补插机制确实被用上了，补丁有效 |
| 4-A3 | 终末地中途换 SPS（切画质/动态码率） | §五 步骤 2 找 `SPS profile changed` 日志 | 有 = 根因 G 相关，需 gst 侧 hunk 3 |
| 4-A4 | 网关只在收到 PLI 后才给首个 IDR | §五 步骤 3 看首帧 `key=1` 之前 `kfReq` 是否已 ≥1 | 是 = 首帧必须靠 PLI 推动，`pli_burst` 的 3 次节奏很关键 |

**已排除的假设（真机 `gst-inspect-1.0 mppvideodec` 实证）**

* ❌「caps 缺 profile/level 导致 High profile 被拒」：
  mppvideodec 的 sink template `video/x-h264` **只标注 `parsed: true`，
  不约束 profile/level**。所以 `cm_gst.c:1023-1024` 那个不含 profile 的 caps
  字符串**不是**绿屏原因。不用改（改了也没用）。
* ❌「MPP 不支持 1080p / High」：src template 明确支持 `NV12_10LE40`（10bit），
  1080p High 完全在能力内。
* ❌「appsrc 首 buffer 带 PTS 导致未初始化 NV12」：
  `cm_box_push()` 里 `(void)pts_ns;`（`cm_gst.c:1156`）**PTS 被完全丢弃**，
  由 appsrc 的 `do-timestamp=true` 按到达时间重打。所以「畸形首 PTS」这条不成立。
  （PTS 被丢另有代价，见 §三 hunk 2。）

### 2.4 本 bug 的最小补丁（已落地）

| 改动 | 位置 | 作用 |
|---|---|---|
| **首个 IDR 之前的 P 帧一律丢弃**（`!key && !s->have_kf`） | `cm_rtp.c:438-447` | **根因 E 的根治点**：绿块永不进 DPB |
| 进闸门的第一帧发一次 PLI（`vid_gate_drop++ == 0` 才发，不刷屏） | `cm_rtp.c:440-444` | 主动催首个 IDR |
| 参数集缓存 `cache_param_set()`：SPS(7)/PPS(8) 落地即缓存 | `cm_rtp.c:272-293`、`cm_rtp.c:520`、`cm_rtp.c:598` | **根因 F**：参数集不再依赖单点 STAP-A |
| IDR 缺参数集时自动补插缓存的 SPS/PPS（用 `g_rtp.au_out` 拼接） | `cm_rtp.c:452-470` | 裸 IDR 也能解，`spsInject` 计数留证 |
| 只含参数集的 AU 不当作一帧推 | `cm_rtp.c:428-433` | 避免「空帧」扰动 kmssink |
| 换会话（`sess_id` 变化）时 `rtp_reset_all()` | `cm_rtp.c:689-696` | 上一局的 seq 基准/旧 SPS 不会带进新一局 |
| `cm_media_loop` 进循环处 `cm_rtp_reset(s)` | `cm_media.c:196-201` | **根因 E 的前置条件**：新一局从干净状态起 |
| RTCP 分支：统计 `rx_rtcp` + 认明文 NACK | `cm_media.c:219-246` | 根因 H 的取证通道 |

**为什么「宁可冻结也不放 P 帧」**（再次强调，因为这是本补丁最反直觉的一处）
关键帧缺失时，用户有两个选择：

* 放行 P 帧 → 画面在动，但满屏绿块/花屏（= bug 4 的症状）
* 冻结 → 画面停在上一帧/黑场，但**永远是干净的一帧**

对云游戏来说，后者显然更好看，而且**故障可诊断**（`still no IDR` 告警会响）。
放行 P 帧则连「到底是网络问题还是解码问题」都分不清。

### 2.5 本 bug 的回滚方式

把 `cm_rtp.c:438-447` 的闸门块注释掉即可单独回滚「绿屏」这一条，
参数集补插（`cm_rtp.c:452-470`）可以独立保留——它只在前者生效时才有意义，
但保留它不会造成任何新症状（补插的是**正确的**参数集）。

---

## 三、需要 gst 侧（`cm_gst.c`）落地的 3 个 hunk

> **本节是写给 Lead 的交接块。`cm_gst.c` 本次由另一位 teammate 在改音频部分，
> 我没有动它。以下 hunk 的行号是 2026-11-12 18:10 的当前版本
> （`cm_gst.c` 现 1256 行）。请 Lead 统一落地。**

### hunk 1（最重要）：`mppvideodec` 关掉 `ignore-error`

**现状**（`cm_gst.c:184-197`，关键在 `:196`）：

```c
static const char *video_pipeline_fmt(void) {
    ...
    return "appsrc name=vsrc is-live=true format=time do-timestamp=true "
           "caps=video/x-h264,stream-format=byte-stream,alignment=au "
           "! h264parse ! mppvideodec rotation=270 ! videoconvert" KMS_SINK_PROPS;
}
```

**改为**（只动第 196 行那个字符串）：

```c
    return "appsrc name=vsrc is-live=true format=time do-timestamp=true "
           "caps=video/x-h264,stream-format=byte-stream,alignment=au "
           "! h264parse ! mppvideodec rotation=270 ignore-error=false max-errors=16 "
           "! videoconvert" KMS_SINK_PROPS;
```

真机属性已核实（`gst-inspect-1.0 mppvideodec`，2026-11-12 现场）：

```
ignore-error : Ignore MPP decode errors
               flags: readable, writable
               Boolean. Default: true
max-errors   : Max consecutive decoder errors before returning flow error
               flags: readable, writable
               Integer. Range: -1 - 2147483647 Default: -1
```

**为什么**：`ignore-error=true` 是「解坏了也不报错、把坏帧吐出来」。
补丁后我们已经在 RTP 层拦住了残帧，这是**第二道防线**——
万一有我们没识别出的坏帧形态（比如网关侧编码器自身出错），
这一层能保证它变成一个 `GST_MSG_ERROR` 被 `cm_box_drain_bus` 抓到并打进日志，
而不是悄悄上屏。

**注意**：`max-errors=16` 意味着连续 16 个错误后 pipeline 会报 flow error 并
停住。`cm_box_drain_bus()`（`cm_gst.c:1126`）会把原文错误打出来。
如果真机上出现「画面卡住且日志有 mpp 错误」，那就是这一层在起作用，
此时应触发整条管线重建（当前代码里 `b->failed` 不会被置位，因为错误来自 bus
而不是 push 返回值——这一点见 §六 风险 5）。

**软解分支不用改**：`video_pipeline_soft()`（`cm_gst.c:199-205`）的
`avdec_h264` 没有这两个属性，软解自己会丢坏帧。

### hunk 2：`cm_box_push()` 尊重 `key` 与 `pts_ns`

**现状**（`cm_gst.c:1152-1166`）：

```c
int cm_box_push(cm_box *b, const unsigned char *data, int len, uint64_t pts_ns, int key) {
    GstBuffer *buf;
    size_t wrote;
    GstFlowReturn fr;
    (void)pts_ns;
    (void)key;
    if (!b || !b->src || b->failed || len <= 0) return -1;
    buf = gst_buffer_new_allocate(NULL, (size_t)len, NULL);
    if (!buf) return -1;
    wrote = gst_buffer_fill(buf, 0, data, (size_t)len);
    if (wrote != (size_t)len) {
        gst_buffer_unref(buf);
        return -1;
    }
    fr = gst_app_src_push_buffer(b->src, buf);
```

**改为**（`(void)pts_ns; (void)key;` 这两行删掉，换成时间戳与 DELTA_UNIT 标记）：

```c
int cm_box_push(cm_box *b, const unsigned char *data, int len, uint64_t pts_ns, int key) {
    GstBuffer *buf;
    size_t wrote;
    GstFlowReturn fr;
    if (!b || !b->src || b->failed || len <= 0) return -1;
    buf = gst_buffer_new_allocate(NULL, (size_t)len, NULL);
    if (!buf) return -1;
    wrote = gst_buffer_fill(buf, 0, data, (size_t)len);
    if (wrote != (size_t)len) {
        gst_buffer_unref(buf);
        return -1;
    }
    /* 视频分支才设时间戳；音频的 pts 是另一个时钟域（48kHz），且当前
     * do-timestamp 已经能正确处理，保持原样以免影响另一位 teammate 的音频改动。 */
    if (b->is_video) {
        GST_BUFFER_PTS(buf) = (GstClockTime)pts_ns;
        GST_BUFFER_DTS(buf) = (GstClockTime)pts_ns;
        if (key) {
            /* 关键帧：清掉 DELTA_UNIT，让下行（h264parse/kmssink）知道可以从这里起 */
        } else {
            GST_BUFFER_FLAG_SET(buf, GST_BUFFER_FLAG_DELTA_UNIT);
        }
    }
    fr = gst_app_src_push_buffer(b->src, buf);
```

**同时需要在文件顶部的「最小 GStreamer 声明」区（当前 `cm_gst.c:34-100` 附近）
补两条手写 extern + 两个宏**（不能 include 真头，设备上没有）：

> ### ❌ 已作废（2026-11-13 定案时核对，见 §九）
>
> **下面这段 `gst_buffer_get_pts/set_pts/get_dts/set_dts` 四条 extern 不能用，整段作废。**
>
> 理由：这四个符号**在真机 `libgstreamer-1.0.so.0`（GStreamer 1.22.0）与本地
> `stubs-aarch64-gst/` 桩库的 `.dynsym` 里都不存在**。GStreamer 从 1.x 起就把
> PTS/DTS 存取做成 **`GST_BUFFER_PTS(buf)` / `GST_BUFFER_DTS(buf)` 宏**
> （直接读 `GstBuffer` 结构体字段），从来没有导出成函数。手写 `extern` 声明
> 虽然能编译通过，但**运行时动态链接会失败，整个 `libjsapi_cgmedia.so` 加载不起来**
> —— 这比不设 PTS 严重得多。
>
> 而 `GstBuffer` 的结构布局是私有的，又不允许 include 真头，所以「手写结构体偏移
> 去填 PTS」这条路在这台设备上**不可行**。
>
> **实际落地的 hunk 2 是降级版**：只保留 `DELTA_UNIT` 标记（`gst_buffer_get_flags`
> / `gst_buffer_set_flags` 这两个符号**是**真实导出函数，可以用），
> `(void)pts_ns;` 保留不动，PTS 继续交给 `appsrc do-timestamp=true` 打。
> 对应代码在 `cm_gst.c:1251-1253`。
>
> 另注：本条与 bug3/bug4 的根因**无关**（定案根因是 `kmssink sync=true`，见 §九），
> 所以「不设 PTS」也没有副作用需要追。

```c
typedef unsigned long long GstClockTime;   /* 已有，cm_gst.c:42 */
typedef unsigned int GstBufferFlags;

/* GstBuffer 的最小布局：GstMiniObject(g_type: GType 4B, refcount 4B, lockstate 4B,
 * flags 4B) + GstBuffer 的 pts/dts/duration/offset/offset_end。
 * 这里**不要**假设完整 GstBuffer 结构 —— 只用官方稳定的 accessor 宏语义，
 * 通过下面两个手写函数访问，避免结构体布局写错直接把内存写坏。 */
extern GstClockTime gst_buffer_get_pts(void *buf);      /* ❌ 符号不存在，见上 */
extern void gst_buffer_set_pts(void *buf, GstClockTime pts);  /* ❌ 同上 */
extern GstClockTime gst_buffer_get_dts(void *buf);      /* ❌ 同上 */
extern void gst_buffer_set_dts(void *buf, GstClockTime dts);  /* ❌ 同上 */
extern unsigned int gst_buffer_get_flags(void *buf);    /* ✅ 真实导出 */
extern void gst_buffer_set_flags(void *buf, unsigned int flags);  /* ✅ 真实导出 */

/* ✅ 正确值。GStreamer 1.22.0 定义链：
 *   GST_MINI_OBJECT_FLAG_LAST = (1 << 4)          —— 这是「flag 池的起点」，
 *   而 GST_BUFFER_FLAG_LAST = GST_MINI_OBJECT_FLAG_LAST << 9 = (1u << 13)，
 *   于是 GST_BUFFER_FLAG_DELTA_UNIT = GST_BUFFER_FLAG_LAST << 0 = 1u << 13。
 * ⚠️ 曾经误写成 (1u << 4)：那正好是 GST_MINI_OBJECT_FLAG_LIVE 的值，
 *    会把关键帧标成「直播流标记」，语义完全错位。
 * 代码里 cm_gst.c:105 已按 1u << 13 写死，且有注释记录这次误写。 */
#define GST_BUFFER_FLAG_DELTA_UNIT (1u << 13)
```

对应的 `GstBuffer` 用法改成：

> ❌ **同上，这段也作废**（`gst_buffer_set_pts/set_dts` 符号不存在）。
> 实际落地版本只做后半段：

```c
    /* ❌ 作废版本（不可用：set_pts/set_dts 符号不存在） */
    if (b->is_video) {
        gst_buffer_set_pts(buf, (GstClockTime)pts_ns);
        gst_buffer_set_dts(buf, (GstClockTime)pts_ns);
        if (!key) {
            gst_buffer_set_flags(buf, gst_buffer_get_flags(buf) | GST_BUFFER_FLAG_DELTA_UNIT);
        }
    }

    /* ✅ 实际落地版本（cm_gst.c:1251-1253）：(void)pts_ns 保留，只打 DELTA_UNIT */
    (void)pts_ns;
    if (b->is_video && !key) {
        gst_buffer_set_flags(buf, gst_buffer_get_flags(buf) | GST_BUFFER_FLAG_DELTA_UNIT);
    }
```

**为什么**（⚠️ 以下论证的前提已随定案更新，但仍值得保留）：

> **更新（2026-11-13 20:35 定案）**：这段「PTS 该用 RTP 时间戳」的论证方向是对的，
> 但**本案真正的根因不是 PTS 打错，而是 `kmssink sync=true` 依赖 PTS 做调度**。
> 在 `do-timestamp=true`（PTS=到达时刻）的前提下，只要 PTS 与 running-time 基准错位
> 一次，`sync=true` 就会把后续 buffer 整体丢弃 —— 现象是「平面绑着但永不翻页」，
> 也就是用户说的「全黑 / 卡住的一帧」。修复是 `sync=false`（`cm_gst.c:221` 附近），
> 不是改 PTS。**所以 hunk 2 与本 bug 无因果关系**，保持降级版即可（只打 DELTA_UNIT）。
> 详见 `docs/BUGFIX-bug4-endfield-black.md` §〇。

现在 PTS **被完全丢弃**，所有 buffer 的时间戳由
`appsrc do-timestamp=true` 按**到达时间**打。而 `kmssink sync=true` 是按时间戳
调度上屏的。后果是：

* 到达抖动（收包线程被 OS 调度延迟 10~20ms）会被当成「帧间隔 20ms」；
* 一段突发到达的多帧会被打上几乎相同的时间戳，`sync=true` 会把它们挤在
  同一时刻上屏（丢帧或加速）；
* 丢包造成的时间空洞在时间戳上**看不出来**，所以「画面卡了一下」在 KMS 侧
  完全没有痕迹，只能靠 RTP 层猜。

用 RTP 的 90kHz 时间戳（`flush_frame` 已经在算
`(uint64_t)g_rtp.last_ts * 1000000ULL / 90`，`cm_rtp.c:479-480`）才是正确的
媒体时间轴，`sync=true` 才能做正确的音视频同步和抖动缓冲。

**风险**：这条改动比 hunk 1 影响面大（改了所有视频 buffer 的时间戳语义）。
如果真机上出现「画面变慢/变快/音画不同步」，**优先怀疑这条**，
回滚方式就是恢复 `(void)pts_ns; (void)key;`。

### hunk 3：`cm_box_set_caps_video()` 在 SPS 变化时 flush 解码器

**现状**（`cm_gst.c:1242-1247`）：

```c
void cm_box_set_caps_video(cm_box *b, int w, int h) {
    if (!b || !b->src) return;
    /* h264parse 会重算 caps，这里只在拿到真实分辨率时更新统计值 */
    b->width = w;
    b->height = h;
}
```

**改为**：

```c
void cm_box_set_caps_video(cm_box *b, int w, int h) {
    int changed;
    if (!b || !b->src) return;
    changed = (b->width > 0 && b->height > 0 && (b->width != w || b->height != h));
    b->width = w;
    b->height = h;
    if (!changed) return;
    /* 分辨率变了：h264parse 会重算 caps，但 MPP 内部的 DPB 还按旧 SPS 分配，
     * videoconvert 还按旧 stride 在跑，kmssink 的 src-rect 还钉在旧尺寸上
     * （真机 /sys/kernel/debug/dri/0/state 里 src-pos 一直是 1280x720）。
     * 只重算 caps 不清 DPB 的典型症状是「换档后画面错位、旧内容叠在新内容上」。
     *
     * flush-start/flush-stop 会让 mppvideodec 丢掉 DPB 与队列中的旧帧，
     * 配合上游 cm_rtp.c 在 SPS 变化时清零 have_kf + 重发 PLI，
     * 新一个 IDR 到达时就是完全干净的起点。 */
    if (b->pipeline) {
        GstEvent *fs = gst_event_new_flush_start();
        GstEvent *fe = gst_event_new_flush_stop(TRUE);
        if (fs) { gst_element_send_event(b->pipeline, fs); }
        if (fe) { gst_element_send_event(b->pipeline, fe); }
        CM_LOGI("caps changed to %dx%d, decoder flushed", w, h);
    }
}
```

需要额外手写两条 extern（同 §三 hunk 2 的声明区）：

```c
typedef struct _GstEvent GstEvent;
extern GstEvent *gst_event_new_flush_start(void);
extern GstEvent *gst_event_new_flush_stop(int reset_time);
extern int gst_element_send_event(GstElement *element, GstEvent *event);
```

**容错要求**（与现有代码风格一致）：这三条符号万一在设备上的 GStreamer 1.22
里名字不对（`gst_element_send_event` 是稳定 API，风险低），
`gst_event_new_flush_start()` 返回 NULL 时上面代码会自然跳过，不会崩。
**千万不要**把 flush 写成 `gst_element_set_state(PAUSED)` 再 `PLAYING` ——
`cm_box_set_caps_video()` 是在**收包线程**里、持有 `s->gst_mu` 被调用的
（`cm_rtp.c:332-334`），而 `set_state` 会阻塞等异步状态切换（现有代码
`cm_box_destroy` 里就 `get_state` 等了 1.5 秒），
在收包热路径上这是必然的卡死。

**为什么不用 `set_state` 脉冲**：同上的阻塞问题。另外
`cm_box_set_rect()`（`cm_gst.c:1229-1238`）已经用了 PAUSED→PLAYING 脉冲，
但那是 JS 线程调用、可以忍受 1.5 秒——两个场景不能混。

---

## 四、7 个关键假设的逐条答复（file:line 级）

### 假设 1：PLI/IDR 请求真的发出去了吗？SSRC 对不对？有频率上限吗？丢包后多久恢复？

**发出了，SSRC 正确，有上限，恢复时间受网关限制。**

| 问 | 答 | 证据 |
|---|---|---|
| media SSRC 用的是远端（接收方向）的？ | **是**。`cm_wr32(pkt + 8, s->cfg.video_ssrc)`，`video_ssrc` 来自 SDP 的 `a=ssrc:`，是我方**接收**方向 | `cm_cmd.c:38` |
| sender SSRC 会不会触发网关环路检测？ | **不会**。`tx_ssrc` 缺省 `0x50454E43u`（ASCII `'PENC'`），与 `video_ssrc` 不同 | `cm_cmd.c:32`、`cm_cmd.c:37` |
| 有没有静默失败点？ | **有，而且是致命的**：`if (!s \|\| s->sock < 0 \|\| !s->cfg.video_ssrc) return;` —— SDP 里没解析出 `videoSsrc` 时，**永远不发 PLI 且没有任何日志** | `cm_cmd.c:27` |
| 加密了吗？ | `cm_srtcp_ready()` 为真才加密，否则**退回明文**（注释「网关有时宽容」）。明文 PSFB 在 SRTP 会话里几乎必被对端丢弃 | `cm_cmd.c:41-46` |
| 频率上限？ | 两层：①`cm_kf_tick()` 首流 3 次、间隔 200ms，之后**每 2 秒最多 1 次**，且 `have_kf && now-last_kf_ms < 2000` 直接 return；②本次新增的 `kf_request()` 丢包急发，限速 300ms | `cm_cmd.c:58-70`；`cm_rtp.c:30`、`cm_rtp.c:295-307` |
| 丢包后多久能恢复？ | **补丁前**：最坏要等 `cm_kf_tick` 的 2 秒窗口 + 网关响应 + 下一个 IDR 到达（历史基线 17 秒只给 1 个 IDR）；**补丁后**：丢包当拍发 PLI（≥300ms 间隔），且坏帧**立刻不再上屏**，所以「用户看到的残留时长」= 0，剩下的只是「画面冻结多久」 | 基线见 `docs/MEDIA_DEBUG.md` 的 `kf=1 pli=11` |

### 假设 2：RTCP 是否只收不发？有没有 RR/PLI 通道？网关会不会永远不给关键帧？

**只发 PLI，从不发 RR；发送通道是通的（UDP + SRTCP 加密），收侧不解密。**

* **发送通道存在且可用**：`cm_udp_send()`（`cm_cmd.c:46`）→
  `cm_srtcp_protect()`（`cm_srtp.c:425-450`，前 8 字节不加密、`idx |= 0x80000000u`
  置 E=1、尾部追加 10 字节 tag，总长 `len + 14`）。**所以「没有 PLI 通道」这个
  假设不成立。**
* **但从不发 RR**：`grep -rn '201' PenCG/native/csrc/cgmedia/` 零命中，
  没有任何 Receiver Report 构造代码。我方 RTCP 出口只有 PLI 一个。
* **收侧收到也不用**：`cm_srtp.c:337` 对 PT 192..223 直接 `return -4`，
  没有 `cm_srtcp_unprotect`；`cm_media.c:244` 对解不开的 SRTCP 只打一行
  `srtcp, not parsed`。补丁后新增了 `rx_rtcp` 计数和**明文** NACK 识别
  （`cm_media.c:234-245`），但加密的 RR 仍然不解析。
* **网关会不会永远不给关键帧？** 从历史基线 `kf=1 pli=11` 看，**网关是给关键帧的，
  但频率极低**（17 秒 1 个）。真正「永远不给」的情形无法从现有日志证明，
  但补丁的闸门保证「给了就一定能用」（坏帧不入库 + 参数集补插），
  所以即使网关很吝啬，画面质量也是**冻结**而不是**花屏**。

### 假设 3：SRTP 解包失败时是否只丢一个包就完事？FU-A 中间片丢失会怎样？

**补丁前：是的，只丢一个包。补丁后：会把整个 AU 标脏 + 补 PLI。**

* 补丁前 `cm_media.c:142-143` 只 `CM_LOGD("#%d srtp auth fail", ...)` 就结束。
* `cm_srtp.c:378-399`：`tag_ok` 比对失败 → `srtp_auth_fail++` + `dump_chunk()` +
  `CM_LOGE` hex + 前 2 个包跑 `brute_diag` → **`return -7`**。
  没有重放窗口（只做了 `max_seq`/`roc` 的粗略推算，`cm_srtp.c:356-364`），
  **不通知上层「这一帧已经残了」**。
* 所以丢失 FU-A 中间片时，**后续片确实会被当成同一帧的一部分推给解码器**，
  形成 §1.2 根因 A 描述的「上一帧马赛克残留」。
* 补丁后的通路：`cm_media.c:251-269` 用 `buf[1] & 0x7F` 取 ept（SRTP 的 RTP 头
  是明文，`cm_srtp.c:339` 也这么取 seq，可行）→ 若 `ept == s->cfg.video_pt`
  则 `cm_rtp_note_stale(s, "srtp auth fail")` → `cm_rtp.c:620-632` 加锁置
  `au_broken = 1` + `vid_au_drop++`，解锁后 `kf_request()`。
  音频包的 auth fail **不会**触发视频 PLI（`cm_media.c:264-266`），
  避免音频认证失败把画面打断。

### 假设 4：`cm_box_push` 在非关键帧且 `have_kf=0` 时是否直接丢弃？会不会留半帧？

**补丁前：不丢弃，`have_kf` 根本没人看。补丁后：在 `cm_rtp.c` 侧就丢弃了，
且不会留半帧。**

* `cm_box_push()`（`cm_gst.c:1152-1175`）的判据只有
  `!b || !b->src || b->failed || len <= 0`，**与 key 无关**；
  `key` 参数被 `(void)key;`（`cm_gst.c:1157`）直接扔掉。
  所以补丁前「非关键帧 + 无关键帧」照样上屏 → bug 4 的绿屏。
* 补丁后闸门在 `flush_frame()`：`cm_rtp.c:438-447`
  （`!key && !s->have_kf` → `vid_gate_drop++` + 首次发 PLI + `rtp_reset_au(); return;`）。
* **会不会留半帧**：不会，但这是原代码就做对的部分——
  `flush_frame()` 无论走哪条 return 分支，最后都会 `rtp_reset_au()`
  （`cm_rtp.c:423`、`:431`、`:445`、`:493`），把 `frame.len` 归零。
  真正的「半帧」风险不在 `cm_box_push` 的丢弃策略，
  而在**没人发现它缺片就 push 了**（根因 A）——那个用 `au_broken` 治。
* 另一个真实存在的半帧风险：`FRAME_CAP`（4MB）超限时旧代码只
  `cm_buf_reset` 且不计账（补丁前 `cm_rtp.c:396-400`）。
  补丁后 `cm_rtp.c:754-764` 会 `vid_au_drop++` + 置 `au_broken` + 补 PLI，
  并顺手解决了「SPS/PPS 正好在这个 AU 里被一起清掉」的隐患。

### 假设 5：首帧绿屏的 5 个候选，哪个成立？

| 候选 | 结论 | 证据 |
|---|---|---|
| SPS/PPS 是否随第一个 IDR 一起下行并一起 push | **网关侧不保证**，所以补丁改成「不保证也能活」：缓存 + 缺则补插 | `cm_rtp.c:272-293`（缓存）、`cm_rtp.c:452-470`（补插） |
| h264parse 的 caps 是否给了 profile/level | **没给**（`cm_gst.c:1023-1024` 只有 `stream-format`/`alignment`），但**这不是绿屏原因** | 真机 `gst-inspect-1.0 mppvideodec` sink template 只标 `parsed: true`，**不约束 profile/level** |
| MPP 对 High/1080p 的支持 | **支持**，src template 含 `NV12_10LE40`（10bit） | 真机 `gst-inspect-1.0 mppvideodec` |
| appsrc 首个 buffer 的 PTS 导致未初始化 NV12 | **不成立**：`(void)pts_ns;` 把 PTS 丢了（`cm_gst.c:1156`），由 `do-timestamp=true` 按到达时间重打。PTS 丢失另有代价但**不是绿屏原因** | `cm_gst.c:1156`、`cm_gst.c:1090` 前的 `format=time do-timestamp=true` |
| **无 IDR 时推 P 帧** | ✅ **成立，是主因** | 补丁前 `flush_frame` 不看 `have_kf`（补丁前 `cm_rtp.c:260-261`）；补丁后 `cm_rtp.c:438-447` |

### 假设 6：终末地 vs 原神的差异

**代码层面没找到「按游戏区分」的分支**——两者走完全相同的管线、相同的
`video_pt`（默认 98，`cm_cfg.c:28-34`）、相同的 `plane-id=76`。
差异只能来自**码流本身**：

| 维度 | 原神 | 终末地 | 影响 |
|---|---|---|---|
| 分辨率 | 真机实测 1280x720（`/sys/kernel/debug/dri/0/state` 的 `src-pos=1280x720`） | 待测（§五 步骤 2） | 若 1080p，`FRAME_CAP` 4MB 与 appsrc `max-bytes` 2MB 都偏紧 |
| profile/level | 待测 | 待测 | 若换档，触发根因 G |
| 是否中途换 SPS | 切画质时 | 可能开局就换 | 触发 `note_sps` 的 changed 分支 |
| **多路 SSRC / simulcast** | **不支持**（见下） | 同 | 若网关真的用了，整路视频解不出来 |
| 首帧是否单点 STAP-A | 是 | 是 | 根因 F |

**关于多路 SSRC / RTX / simulcast**：`cm_cfg.c:61-79` 只解析 `videoSsrc` 和
`audioSsrc` 两个值，**没有解析辅助 SSRC、没有 RTX、没有 simulcast**。
`cm_rtp_on_packet()` 里 `if (s->cfg.video_ssrc && ssrc != s->cfg.video_ssrc) return;`
（`cm_rtp.c:680`）——**任何非主 SSRC 的视频包直接丢弃且不计数**。
如果终末地真的用了 RTX 重传，我们会把重传包全丢掉，表现就是「丢包永远补不回来」。
这条需要真机验证（§五 步骤 3 看 `pt_hist` 里有没有未知 PT 在涨）。

### 假设 7：kmssink 在 rect 变化 / caps 变化后是否需要强制清屏/重配？

**rect 变化：现有代码够用（有 PAUSED→PLAYING 脉冲）。
caps 变化：不够，需要 flush（= §三 hunk 3）。**

* rect 变化走 `cm_box_set_rect()`（`cm_gst.c:1229-1238`）：
  `apply_rect()` + `GST_STATE_PAUSED` → `GST_STATE_PLAYING` 脉冲。
  `apply_rect()` 的注释已经写清「`render-rectangle` 是只写属性，
  kmssink 只在 reconfigure（分配 plane）那一刻读一次，PAUSED 下设无效」
  ——这个脉冲就是为了触发 reconfigure。**结论：rect 路径正确，不用改。**
* caps 变化走 `cm_box_set_caps_video()`（`cm_gst.c:1242-1247`）——**空实现**。
  它被 `note_sps()` 在持 `s->gst_mu` 的情况下从收包线程调用
  （`cm_rtp.c:332-334`），所以**不能**在里面做 `set_state`（会阻塞）。
  正确做法是 flush event（§三 hunk 3）。
* 「kmssink 是否需要强制清屏」还有一个隐含场景：`video_ok` 之后
  `setVideoVisible(0)` 把 rect 缩到 1x1 再恢复。这条路径由
  `setVideoVisible` 自己重置 `have_kf/kf_count/pli_burst/next_pli_ms`
  并重发 PLI（`cm_cmd.c:183-192`），配合本补丁的闸门，恢复时不会出绿帧。

---

## 五、真机验证步骤（adb 命令级）

### 步骤 -1：取证通道（**先读这个**，否则下面所有 grep 都是空的）

> 这一条是本文档写作时**最大的盲点**：§一 里写着「`grep -a pencg` 命中数为 0，没有可对照的历史日志」，
> 当时误以为「代码没跑」；真机复测（2026-11-12 晚）证明是**取证通道用错了** ——
> X6 Pro 的 Falcon 运行时里 JS 的 `console` **只有 `console.error` 会写盘**，
> `console.warn` / `console.log` 完全不落盘（全盘 `grep -a -c 'W][JS]'` 计数为 0）。
> 而当时 `PenCG/src/services/log.js` 走的是 `console.warn`（现已在 `log.js:44` 改为 `console.error`），
> 所以那批日志**根本没有产生**，不是「没有命中」。

落盘文件是 **miniapp 进程的 stdout**，文件名带启动时间戳、每次重启都换，**不能写死**：

```bash
ADB="./env/adb/adb.exe -s 9D91500007505171"
P=$($ADB shell 'pidof miniapp' | tr -d '\r')
L=$($ADB shell "readlink /proc/$P/fd/1" | tr -d '\r')   # 实测 /userdata/applog/DictPen_20261113_232219.log
echo "LOG=$L"
$ADB shell "grep -a -n 'pencg' $L | tail -50"
```

> **路径更正（2026-11-14 真机定论）**：本节早期版本写的
> `/userdata/applog/loli_s6_dev.txt` **是错的** —— 该文件实测只有 **1 字节**，
> `grep -c pencg` / `grep -c cgmedia` 都是 0，是空文件。
> 真日志在 `DictPen_YYYYMMDD_HHMMSS.log`（当时 `/userdata/applog/DictPen_20261113_232219.log`，13.2MB）。

行形态固定：

```
[2026-11-12 22:15:24.947][E][JS] [pencg][W] [play] probe stage=live v=1 screen=0(ON) ...
└──────────┬────────────┘ └┬┘ └┬┘ └────┬────┘
        落盘时间戳        级别 来源   PenCG 自己打的级别前缀
```

- **落盘级别恒为 `[E]`**（底层通道是 `console.error`），跟严重程度无关。
- **真正的级别只看行内 `[pencg][I|W|E]`**；本节下文示例里的 `[pencg][W]` 在设备上是 `...[E][JS] [pencg][W] ...`。
- 下文步骤 2~4 里 grep 的都是 **native 侧 `[cgmedia]`** 字符串（`SPS reports` / `stat` / `PLI sent` 等），
  它们走 `cm_log()` → `fprintf(stdout)`。**它们和 JS 的 `[pencg]` 必落同一个文件** ——
  两者都跟 miniapp 进程的 stdout 走，一个进程只有一个 stdout（2026-11-14 已确认）。
  所以就在同一个 `$L` 里 grep 两种前缀，不用找第二个文件。

> 前提：`cd /d/Users/Daniel/code/DSH/x6pro/haas-x6pro`（下文 `./env/adb/adb.exe` 均相对此目录）
> 设备序列号 `9D91500007505171`。**本节的命令全部只读，不改设备状态。**

### 步骤 0：确认设备在线 + 清掉会拖慢性能的调试开关

```bash
./env/adb/adb.exe -s 9D91500007505171 shell 'echo ok'
# GST_DEBUG 开着会把帧率打到 2.27fps，测性能前必须清掉（debugging.md:213-215）
./env/adb/adb.exe -s 9D91500007505171 shell 'rm -f /userdisk/miniapp/data/*gst*debug* /tmp/*gst*debug* 2>/dev/null; ls /userdisk/miniapp/data/ | head'
```

### 步骤 1：装新包（Lead 负责，此处仅记录顺序）

```bash
# 先确认旧版已彻底停掉，避免两套 KMS plane 打架
./env/adb/adb.exe -s 9D91500007505171 shell 'ps | grep -i miniapp'
```

### 步骤 2：确认识别到 SPS 及其变化

```bash
# 每次（重）连、每次切画质都打一条。看 WxH / profile / level 三个值，
# profile 66=Baseline, 77=Main, 100=High
./env/adb/adb.exe -s 9D91500007505171 shell "grep -a 'SPS reports' $L | tail -20"

# 变化时会额外打一条（本次修好了旧版恒打 0 的 bug，现在能看出真实的前后值）
./env/adb/adb.exe -s 9D91500007505171 shell "grep -a 'SPS profile changed' $L | tail -20"

# 参数集补插是否被触发：>=1 说明网关确实没重发 SPS/PPS，补丁救回来了
./env/adb/adb.exe -s 9D91500007505171 shell "grep -a 'injected cached parameter sets' $L | tail -20"
```

**判据**：若「原神」和「终末地」的 `SPS reports` 里 `profile=` 或 level
不同（例如原神 `100`、终末地 `66`），说明两者码流参数不同，
bug 4 的差异有码流侧解释。若完全相同，则差异只在丢包率/首帧时序。

### 步骤 3：量化「马赛克残留」（核心）

「马赛克残留」在日志上的定义：**`auDrop` 在涨，但 `frames` 也还在涨**
（说明坏帧被丢了、好帧还在推，解码器不该再看到残帧）。
「补丁没生效」的定义：**`auDrop` 一直为 0，而用户仍然看到残留**（说明丢包不在 RTP 层）。

```bash
# 每 2 秒拉一次 stat（emit_stat 里加了 auDrop/gateDrop/kfReq/dup/reorder/spsInject/rtcpIn）
# 连续采样 60 秒，存成 CSV 便于算增速
for i in $(seq 1 30); do
  ./env/adb/adb.exe -s 9D91500007505171 shell "grep -a '\"t\":\"stat\"' $L | tail -1" \
    >> /tmp/pencg_stat.log
  sleep 2
done
wc -l /tmp/pencg_stat.log
```

**看这 4 个字段的相对关系**（都在同一条 stat JSON 里）：

| 字段 | 含义 | 健康值 | 异常含义 |
|---|---|---|---|
| `lost` | RTP 序号缺口累计（补丁前恒 0） | 缓慢增长 | 快速上涨 = 弱网 |
| `auDrop` | 因缺片/乱序而整帧丢弃的 AU 数 | >0 且 ≈ 用户看到的「卡一下」次数 | 长期 0 而仍有残留 = 丢包不在 RTP 层 |
| `gateDrop` | 首个 IDR 之前丢掉的 P 帧数 | 只在开局短暂增长，然后停止 | 持续增长 = **始终没拿到 IDR**（§六 风险 1） |
| `kfReq` vs `kf` | 我们要了几次 IDR / 真收到几个 | `kf` 应该跟着 `kfReq` 涨 | `kfReq` 涨而 `kf` 不涨 = **网关不理 PLI** |

```bash
# 一次性看健康告警（补丁新增，每 10 秒最多一条）
./env/adb/adb.exe -s 9D91500007505171 shell "grep -a -E 'still no IDR|packet loss seen' $L | tail -30"
```

`still no IDR (rx=... kfReq=... gateDrop=... auDrop=...) — gateway may be ignoring PLI`
= 我们发了 PLI 但网关不给 IDR，画面只能冻结（网关侧问题，不是本端 bug）。

`packet loss seen (lost=... auDrop=... dup=... reorder=...) — broken AUs dropped, not shown`
= 补丁正在工作：丢了包、丢了整帧、坏的没上屏。

### 步骤 4：确认 PLI 是否真的被加密发出

```bash
# 看每一发 PLI 的形态：srtcp（加密，正确）还是 plain（明文，几乎必被丢）
./env/adb/adb.exe -s 9D91500007505171 shell "grep -a 'PLI sent' $L | tail -20"
```

**判据**：如果全是 `plain`，说明 DTLS 导出的 SRTCP 密钥还没 ready，
PLI 等于白发——此时应先去修 DTLS 时序，而不是继续调 RTP 参数（§1.3 的 3-A3）。

### 步骤 5：KMS 平面取证（画面到底有没有在上屏）

`/sys/kernel/debug/dri/0/summary` 是唯一可信的「有没有出画」证据。
**注意 debugfs 单次 read 有 576 字节上限**（`debugging.md:47-49`），
简单 `cat` 会漏内容，用循环读：

```bash
# 采样 30 秒，每 200ms 一次，只保留 buf addr，用来数「地址变化次数 = 实际帧率」
./env/adb/adb.exe -s 9D91500007505171 shell '
  i=0
  while [ $i -lt 150 ]; do
    cat /sys/kernel/debug/dri/0/summary 2>/dev/null | tr "\n" " " | sed "s/^/$(date +%s%3N) /"
    echo
    i=$((i+1)); sleep 0.2
  done
' > /tmp/kms_summary.log
grep -c 'Video Port0: ACTIVE' /tmp/kms_summary.log   # >0 才说明视频平面真的挂上了
```

```bash
# 平面几何：确认 1280x720 被缩到 240x427 放在 107,299（apply_rect 公式的真机对照）
./env/adb/adb.exe -s 9D91500007505171 shell \
  'cat /sys/kernel/debug/dri/0/state | grep -A6 "Esmart1-win0"'
```

本次已确认的真机基线（设备空闲时）：

```
plane[76]: Esmart1-win0
        crtc-pos=240x427+107+299
        src-pos=1280.000000x720.000000+0.000000+0.000000
        rotation=1
```

**量化「马赛克残留」的判据**：
「残留」= 视频平面的 `buf` 地址**停止变化**但 `ACTIVE` 仍然是 1，
且用户看到的还是**有内容的图像**（不是黑场）。
若 `buf` 地址在变而画面局部错位 → 是解码侧污染（本补丁治的）；
若 `buf` 地址不变 → 是「冻结」（§六 风险 1，网关不给 IDR）。

### 步骤 6：截图取证（视频平面必须用 captureFB）

```bash
# 注意：miniapp_cli capture 截的是 Falcon UI 合成面（960x266），
# **不含 KMS 视频平面**（plane 76 是硬件叠加，绕过 weston）。
# 要看视频必须抓 fb 原始内容：
./env/adb/adb.exe -s 9D91500007505171 shell \
  '/bin/miniapp_cli captureFB /tmp/v.png; ls -l /tmp/v.png'
./env/adb/adb.exe -s 9D91500007505171 pull /tmp/v.png ./bug3_video.png
```

**判据**：绿屏 = 大面积 `RGB(0,135,0)` 或未初始化 NV12 的绿/紫块；
马赛克残留 = 局部块状错位但整体画面仍在动（连续截 3 张对比同一局部区域）。

### 步骤 7：A/B 对照（同一局游戏内）

1. 装补丁版，玩原神 3 分钟，按步骤 3 采样 → 记下 `auDrop`、`lost`、`kf`、`kfReq`；
2. 若 `auDrop > 0` 且用户反馈「还有残留」→ **补丁逻辑有漏**，
   用 §1.3 的 3-A1/A2 继续查；
3. 若 `auDrop == 0` 但用户仍见残留 → 丢包不在 RTP 层（SRTP 已拦掉，
   或网关侧编码器出错），转到 §六 风险 5。

---

## 六、遗留风险清单

| # | 风险 | 影响 | 修法 / 归属 |
|---|---|---|---|
| 1 | **网关彻底不理 PLI 时画面会一直冻结**（有意的取舍，`cm_rtp.c:35-43`） | 用户看到「卡住不动」而不是「马赛克」，可能被误报为新 bug | 靠 `still no IDR` 告警区分；这是网关侧问题。若现场必须恢复，可临时把 `DROP_STREAK_FORCE_KF` 调大 + 接受花屏 |
| 2 | **`cm_srtp.c` 的 ROC 推算会让一次乱序引发一片 auth_fail**（`cm_srtp.c:356-364`，`ss->max_seq = seq` 无条件回退） | 补丁只能把这些包识别成「帧残了」并丢帧，不能避免它们被误判 | 应改成标准的 `if (seq > max_seq) { if (高位跨越) roc++ ... max_seq = seq }`，即**只在 seq 前进时更新 max_seq**。`cm_srtp.c` 不在本次可改范围 |
| 3 | **`cm_cmd.c` 无 `video_ssrc` 时静默不发 PLI**（`cm_cmd.c:27`） | SDP 解析失败时整条恢复链失效，且无日志 | 应改成打一次 `CM_LOGE` 并计数。`cm_cmd.c` 不在本次可改范围 |
| 4 | **`FRAME_CAP` 4MB / appsrc `max-bytes` 2MB 对 1080p IDR 偏紧** | 1080p 大 IDR 可能被截断（`au_broken`）→ 整帧丢 → 持续 `gateDrop` | 若 §五 步骤 2 测出终末地是 1080p，把 `cm_rtp.c:26` 的 `FRAME_CAP` 提到 8MB、`cm_gst.c:1086` 的 `max-bytes` 提到 4MB |
| 5 | **`mppvideodec` 报 flow error 后 `b->failed` 不会置位** | hunk 1 生效后，解码器停住但代码不知道，会一直往里 push，`appsrc push flow=...` 刷屏 | 应在 `cm_box_drain_bus()`（`cm_gst.c:1126`）里遇到 `GST_MSG_ERROR` 时置 `b->failed = 1`，由上层重建 box。属 gst 侧，未改 |
| 6 | **`cm_stats.last_seq` / `have_last_seq`（`cm_session.h:75-76`）仍然无人使用** | 死字段，容易误导后来者 | 本次用 `g_rtp.last_seq`（`cm_rtp.c:48`）实现了同样功能；建议下次清理时删掉这两个字段 |
| 7 | **`note_sps()` 的 `changed` 判据含 `&& s->sps_profile`（`cm_rtp.c:327`）** | 首个 SPS 永不触发「重要 IDR」突发 | 有意的（首帧本来就在等 IDR），但意味着「首个 SPS 丢了」只能靠补插缓存兜；补插也失败（缓存里没有）时无解。若要硬化，可在首帧 `gateDrop` 超过 100 时强制 `have_kf=0` 并重发 PLI 突发 |
| 8 | **SRTCP 只发不收：没有 `cm_srtcp_unprotect`，加密 RR 完全不解析** | 无法感知对端的接收质量反馈；部分 SFU 收不到 RR 会降级 | 需新写 SRTCP 解密（复用 `srtp_tag`/`ctr_xor`，`cm_srtp.c:425-450` 已有对称实现）。本次只加了 `rx_rtcp` 计数和明文 NACK 识别 |
| 9 | **不支持 RTX / simulcast / 辅助 SSRC**（`cm_cfg.c:61-79`、`cm_rtp.c:680`） | 若网关用 RTX 重传，重传包被静默丢弃，丢包永远补不回来 | 需扩 `cm_cfg` 解析 + `cm_rtp_on_packet` 的 SSRC 白名单。先靠 §五 步骤 3 的 `pt_hist` 判断是否真的存在 |
| 10 | **`cpps` 时间戳语义改动（hunk 2）影响面比 hunk 1 大** | 若真机出现音画不同步/变快变慢，优先怀疑它 | 回滚 = 恢复 `(void)pts_ns; (void)key;`；见 §三 hunk 2 |
| 11 | **本次无法真机 A/B**（设备旧版、日志无 `pencg` 命中） | 所有结论都只有静态代码证据，没有「修复前后对比」 | Lead 装机后必须按 §五 跑一遍，把 `auDrop/kfReq/kf/gateDrop` 四个数记进 `docs/MEDIA_DEBUG.md` 作为新基线 |

---

## 七、改动清单（本次实际落地的文件与行号）

> 行号对应 2026-11-12 18:10 的工作区状态。
> `cm_rtp.c` 803 行、`cm_media.c` 343 行、`cm_session.h` 335 行。

### `PenCG/native/csrc/cgmedia/cm_session.h`

| 行 | 改动 |
|---|---|
| `:94-118` | `cm_stats` 末尾新增 8 个视频完整性计数器（`vid_lost/vid_dup/vid_reorder/vid_au_drop/vid_gate_drop/vid_kf_req/vid_kf_req_ms/vid_sps_inject`），每个字段带中文注释说明它对应哪种用户可见故障 |
| `:264` | 新增声明 `void cm_rtp_note_stale(cm_sess *s, const char *why);` |
| `:266` | 新增声明 `void cm_rtp_reset(cm_sess *s);` |

**踩过的坑**（写下来防止后人重犯）：第一次把这 8 个字段加进了 `cm_sess` 而不是
`cm_stats`，编译报 14 个 `no member named 'vid_lost' in 'cm_stats'`。
字段必须加在 `cm_stats` 里，因为 `emit_stat` 是通过 `s->stats.xxx` 访问的。

### `PenCG/native/csrc/cgmedia/cm_rtp.c`

| 行 | 改动 |
|---|---|
| `:30/:32/:34` | 新增 `KF_REQ_MIN_MS 300`、`LOSS_LOG_MAX 20`、`DROP_STREAK_FORCE_KF 3` |
| `:35-43` | 中文注释：说明**为什么故意不做**「等 IDR 超时放行 P 帧」的兜底 |
| `:42-80` | `rtp_ctx` 扩容 14 个字段 |
| `:231` | `annexb_put()` 返回类型 `void` → `int`（失败返回 -1） |
| `:240/:254` | 新增 `rtp_reset_au()` / `rtp_reset_all()` |
| `:272-293` | 新增 `cache_param_set()`：SPS(7)/PPS(8) 落地即缓存 |
| `:295-307` | 新增 `kf_request()`：丢包急发 PLI，300ms 限速，**UDP send 在锁外** |
| `:321-350` | `note_sps()`：修掉「旧 profile 恒打 0」的日志 bug（`:347`）；changed 分支保持 |
| `:352-356` | `mark_keyframe()` 未改（现在只被 `flush_frame` 调用） |
| `:370-383` | `emit_media_if_needed()` 未改 |
| `:402-495` | `flush_frame()` **重写**，签名 `(cm_sess *s, int key)` → `(cm_sess *s)`；四段：残帧丢弃（`:411`）、纯参数集 AU 丢弃（`:428`）、无 IDR 闸门（`:438`）、参数集补插（`:452`） |
| `:497-606` | `handle_h264()` **重写**：STAP-A 收尾 `flush_frame(s)`（`:532`，根因 B 修复点）、STAP-A 越界置 `au_broken`（`:510`）、FU-A start 计账（`:549-560`）、丢 start 计账（`:569-578`）、单 NAL 分支（`:598-605`） |
| `:620-632` | 新增 `cm_rtp_note_stale()` |
| `:634-647` | 新增 `cm_rtp_reset()` |
| `:649-803` | `cm_rtp_on_packet()`：新增 `sess_id` 变更复位（`:689-696`）、RTP 序号三态判定（`:708-751`）、`FRAME_CAP` 分支计账（`:754-764`） |

**踩过的坑**（`:724-731`）：正向缺口分支里必须**先**把旧 `last_seq` 存进 `prev`，
**再**更新 `last_seq`，否则日志打出来的 `seq %u->%u` 两个值相同，
看上去像「没丢包」——排查时会把人带偏。

### `PenCG/native/csrc/cgmedia/cm_media.c`

| 行 | 改动 |
|---|---|
| `:66-109` | `emit_stat()` 新增健康检查（每 10 秒最多一条）：`still no IDR` / `packet loss seen` |
| `:110-144` | stat JSON：`lost` 从硬编码 0 改成真实 `vid_lost`（`:119-122` 有注释说明）；末尾追加 `auDrop/gateDrop/kfReq/dup/reorder/spsInject/rtcpIn` |
| `:196-201` | `cm_media_loop` 进循环处新增 `cm_rtp_reset(s);` |
| `:219-246` | RTCP 分支重写：统计 `rx_rtcp`、认明文 `RTPFB fmt=1` NACK → `cm_rtp_note_stale`；**加密 SRTCP 不做破坏性动作** |
| `:251-269` | `plen == -7` 分支重写：按 `buf[1] & 0x7F` 判 PT，视频包 → 告警 + `cm_rtp_note_stale`；音频包只打 `CM_LOGD` |

### `PenCG/docs/BUGFIX-bug3-bug4-video.md`

本文件（新建）。

### 未改动

* `PenCG/native/csrc/cgmedia/cm_gst.c` —— 只读，改动以 §三 的 3 个 hunk 交接给 Lead。
* `cm_cmd.c` / `cm_srtp.c` / `cm_cfg.c` —— 未改，相关缺陷写进 §六。

## 八、构建验证

```bash
cd /d/Users/Daniel/code/DSH/x6pro/haas-x6pro/PenCG
powershell -NoProfile -ExecutionPolicy Bypass -File native/build-media.ps1
```

结果：

```
OK -> D:\Users\Daniel\code\DSH\x6pro\haas-x6pro\PenCG\libs\libjsapi_cgmedia.so (78.8 KB)
```

exit code 0。唯一告警是既有的 `cm_gst.c` 里
`address of function 'gst_debug_message_get' will always evaluate to 'true'
[-Wpointer-bool-conversion]`，与本次改动无关。

编译参数（`native/build-media.ps1`）：`zig cc -target aarch64-linux-gnu.2.17 -O2 -g0
-Wl,-s -fPIC -shared -fvisibility=hidden -DPEN_CG_MEDIA=1`，
只链接 `stubs-aarch64/*.so` 与 `stubs-aarch64-gst/*.so`，不 include 设备上没有的头。
本次新增代码**没有引入任何新的 GStreamer 符号**（全部是 C 标准库 + 已有的
`cm_box_push` / `cm_buf_*` / `cm_now_ms` / `cm_send_pli`），所以不会因为
「符号不存在」在真机上 dlopen 失败。

---

## 九、定案（2026-11-13 20:35）：真正根因是 `kmssink sync=true`

这一节是**后来补的**，它推翻了本文 §二 里关于 bug 4 的主因判断，
也把 bug 3「马赛克残留」的成因从「参考帧被污染」修正为「同一类同步问题的 milder 表现」。
原文保留，因为里面的判据、反证和诊断通道仍然有效；但**不要再按
「码流损坏 / SPS-PPS 补插错 / IDR without SPSPPS 是根因」这三个方向排查**。

### 9.1 结论

**根因：`kmssink` 的 `sync=true`。**

Lead 已把 `PenCG/native/csrc/cgmedia/cm_gst.c` 的 `KMS_SINK_PROPS`
（`cm_gst.c:221` 附近）从 `can-scale=true sync=true` 改为 `can-scale=true sync=false`，
并就地写了约 40 行防退步注释。**这是唯一的修复动作**，管线其余部分
（`mppvideodec rotation=270 ignore-error=false max-errors=16`、`plane-id=76`）一律未动，
**也没有加 `! queue`**。

### 9.2 机制

`appsrc` 是 `format=time do-timestamp=true`，每个 buffer 的 PTS 由**到达时刻**打上
绝对时钟戳。`sync=true` 时 kmssink 严格按 PTS 决定显示时刻；PTS 与 running-time
基准一旦错位（首次缓冲、重连、片头 I 帧丢失造成的跳变），后续 buffer 会被
**整体丢弃**。

于是出现本文 §一/§二 描述的那种「平面绑着、`fb` 有值、`AU push` 计数在涨，
但画面不动」——「全黑」其实就是停在片头那帧黑场上，不是解不出来。

bug 3 的「上一帧马赛克残留」是同一机制的下游：平面偶尔能推进到一张
**只解了一部分宏块**的帧就再也推不动，用户看到的就是「局部卡住不动、旧宏块残留」。
所以 §一 的 AU 完整性/IDR 闸门修复**并不是白做**（它们修的是独立真实缺陷，
在弱网下仍有价值），但它们不是这两个 bug 的主因。

### 9.3 四层独立验证（Lead 已做，此处只记结论）

| # | 验证 | 结果 | 排除了什么 |
| --- | --- | --- | --- |
| 1 | 把 AU dump（`au.h264`，615675 B / 300 帧）拉回本地 ffmpeg 解 | 解出 **233 帧**，`corrupt decoded frame` 仅 3 次且都在片头 I 帧；抽帧 200/232 画面完整清晰（灰度均值 182.9，非零 89%） | **「码流损坏」** |
| 2 | `gst-launch-1.0 filesrc ! h264parse ! mppvideodec` | 954368 B = 233 帧 × 4096，与软解一致 | **硬解本身有问题** |
| 3 | `videotestsrc pattern=smpte ! … ! kmssink plane-id=76` | 全屏彩条，肉眼可见 | **plane76 硬件通道** |
| 4 | 同管线换数据源（`multifilesrc loop=true` 播 au.h264，`sync=true` + `render-rectangle=<107,244,266,473>`） | 能看到终末地画面 | **管线 / rotation / 缩放 / plane** |

第 1 层同时证否了「`IDR without SPSPPS, injected cached parameter sets` 是根因」：
参数集若真错，dump 出来的字节不可能解出 233 帧清晰画面。**§三 的注入字节推演结论
仍然成立**（布局、起始码、顺序都对），只是它不是本案根因。
§3.4 发现的「重排覆盖注入」是一个**独立的真 bug**，修复保留。

### 9.4 真机对照（同一游戏，唯一变量 = `sync`）

| 配置 | 平面观测 | 用户目视 |
| --- | --- | --- |
| `sync=true` | `plane[76] crtc=video_port0 fb=130`，10 次 × 2s 采样 **`fb` 恒 130 一次不变**；同期 `AU push count=77` | 「有画面但完全静止（卡住的一帧）」 |
| `sync=false` | `fb` 在 127/130 之间**持续交替**（20 次 × 1s 采样） | 「画面在动了！能正常玩」 |
| 原神回归（`sync=false`） | 同样交替 | 「画面正常、在动」 |

**判据固化**：

- `AU push` 在涨 + `fb` **恒定** ⇒ sink 侧同步问题，别去查码流和解码器；
- `AU push` 在涨 + `fb` **交替** ⇒ 渲染正常，问题在上游或内容本身。

注意 `fb` 编号本身不能当判据（原神正常局也曾长期恒为 127），
**必须看它在多次采样间有没有变化**。

### 9.5 本文的处置一览

| 章节 | 处置 |
| --- | --- |
| §一 1.2 的 A/B/C/D 四条 | 修的是独立真实缺陷，**修复保留**，但它们不是本案主因 |
| §二 2.2 的 E（无 IDR 闸门 → 空 DPB 绿块） | **保留**，这是 bug 4 早期「绿屏」的真实成因之一；本轮「全黑」不是它 |
| §二 2.3 假设 4-A1（注入内容错） | ❌ **已推翻**（见 9.3 第 1 层） |
| §三 hunk 1（`ignore-error=false`） | ✅ 已落地（`cm_gst.c:249`），**保留** |
| §三 hunk 2（PTS/DELTA_UNIT） | ⚠️ 已降级为「只打 DELTA_UNIT」，PTS 相关 extern **作废**（见本节 9.6） |
| §三 hunk 3（caps 变化时 flush） | ✅ 已落地，**保留** |
| §五 真机验证步骤 | 仍可用，但 §五 步骤 5/6 的平面判据按 9.4 修正 |
| §六 遗留风险 | 保留；新增本条（sink 侧 `sync` 语义） |

### 9.6 两处已作废的技术细节（务必别照抄）

**① `GST_BUFFER_FLAG_DELTA_UNIT` 的值。** 原文 §三 hunk 2 里写的
`(1u << 4)` **是错的**，正确值是 **`1u << 13`**。

推导（GStreamer 1.22.0）：`GST_MINI_OBJECT_FLAG_LAST = (1 << 4)` 是 flag 池起点，
`GST_BUFFER_FLAG_LAST = GST_MINI_OBJECT_FLAG_LAST << 9 = (1u << 13)`，
`GST_BUFFER_FLAG_DELTA_UNIT = GST_BUFFER_FLAG_LAST << 0 = 1u << 13`。
写成 `1u << 4` 正好撞上 `GST_MINI_OBJECT_FLAG_LIVE`，语义完全错位。
代码侧 `cm_gst.c:105` 已按 `1u << 13` 写死并留了误写注释。

**② `gst_buffer_get_pts/set_pts/get_dts/set_dts` 四个 extern 整段作废。**
这四个符号在真机 `libgstreamer-1.0.so.0`（1.22.0）与本地 `stubs-aarch64-gst/`
桩库的 `.dynsym` 里**都不存在**。GStreamer 从来把 PTS/DTS 做成
`GST_BUFFER_PTS(buf)` / `GST_BUFFER_DTS(buf)` **宏**（直接读 `GstBuffer` 字段），
没导出成函数。手写 extern 能编译通过，但**运行时 dlopen 会失败**，
整个 `libjsapi_cgmedia.so` 加载不起来 —— 比不设 PTS 严重得多。
而 `GstBuffer` 布局私有、又不许 include 真头，所以「按结构体偏移手填 PTS」
在这台设备上也不可行。

实际落地的是降级版（`cm_gst.c:1251-1253`）：保留 `(void)pts_ns;`，
只用**真实导出**的 `gst_buffer_get_flags` / `gst_buffer_set_flags` 打 DELTA_UNIT。

### 9.7 一条方法论教训

本轮把「`emit_stat` 的 `fps>0`」当成「解码器在出帧」（实际它是**推帧速率**，
见 `cm_media.c:80`），导致先去查码流和参数集，绕了一大圈。
真正的破局点是**把「推给 appsrc 的字节」和「KMS 平面上的字节」分层切开**
（AU dump + `fb` 采样）。**分层取证比顺着症状猜快得多。**

---

## 十、高铁弱网：RTCP Generic NACK 的落地与真机结果

### 10.1 背景与动机

2026-11-13 16:25–16:42，高铁 Wi-Fi（丢包 5–8%），原神 yssy，现场统计：

```
#1 still no IDR (rx=74418 kfReq=468 gateDrop=15511 auDrop=401) — gateway may be ignoring PLI
```

`rx` 持续增长（网关在推）但 `fps=0`。**401 个残缺 AU 赔掉了 15511 个正常 P 帧（约 39:1）**：
每个 IDR 被丢包打碎 → 整帧丢弃 → 连丢 3 个就清 `have_kf` → 所有 P 帧走闸门丢弃
→ 再要 IDR → 再被撕。判定为**丢包导致的永久 IDR 饥饿**，不是网关停推。

对应的三条设计（Lead 指定）：

1. **NACK 优先保护 IDR**：缺口记录带优先级，IDR/SPS/PPS 的缺口重发 4–5 轮，
   普通 P 帧 2–3 轮；一个 FCI 用 PID + BLP 覆盖 17 个连续 seq，必要时发多条报文。
2. **P 帧坏掉不清闸门**：坏的是 P 帧 AU 且 `have_kf` 仍为 1 时，只丢这一帧、
   保留 `have_kf`；只有连丢 `DROP_STREAK_FORCE_KF`(3) 个 **P 帧** AU 或丢的是 IDR
   才清。把「1 个坏 IDR → 39 帧全丢」变回「1 个坏 P 帧 → 只丢 1 帧」。
3. **`drop_streak` 只统计连续 P 帧 AU**，不把 IDR 计进去。

另加了 IDR 饥饿兜底 `IDR_STARVE_RELAX_MS`(4000)：等 IDR 超过 4 秒就放行 P 帧
（`have_kf=1`，接受花屏直到下一个 IDR），避免网关彻底不理 PLI 时永久定格。

### 10.2 代码落点（已在 `cm_rtp.c` / `cm_cmd.c` / `cm_media.c` / `cm_session.h`）

| 内容 | 位置 |
| --- | --- |
| 缺口表 `gaps[]` / `gap_add` / `gap_take` / `gap_purge_expired` / `cm_rtp_take_nack_gaps` | `cm_rtp.c`（`gap_purge_expired` 在 `:475-512`，`gap_take` 在 `:1112`） |
| 缺口 TTL / 轮数 / 优先级宏 | `cm_rtp.c`：`NACK_GAP_MAX 256`、`NACK_ROUNDS_P 3`、`NACK_ROUNDS_IDR 5`、`NACK_GAP_TTL_MS 300`、`NACK_PRIO_IDR 2`、`NACK_PRIO_P 1` |
| NACK 打包与发送（PT=205 FMT=1，PID+BLP，SRTCP） | `cm_cmd.c`（缓冲区 `pkt[12+NACK_MAX_FCI*4+16]` 在 `:417`） |
| 主循环接入 `cm_nack_tick` / `cm_rtp_flush_held` | `cm_media.c:285` / `:292` |
| 统计字段 `vid_nack_sent` / `vid_nack_req` / `vid_nack_recovered` / `vid_relax` / `vid_stale_rtx` | `cm_session.h`（`vid_stale_rtx` 在 `:141`） |

### 10.3 真机结果（2026-11-13 晚，家庭网络）

```
#1 NACK sent (30 bytes, srtcp) gaps=1 idr=0 fci=1 covered=1
```

**NACK 确实发出去了、确实走了 SRTCP、打包也对**（1 个 FCI 覆盖 1 个包）。
这是第一次在真机上确认这条通道成立（此前 `grep -a -c 'NACK sent'` 一直是 0）。

**但同一轮里 `AU repaired` 仍然是 0。**

### 10.4 怎么读这个「NACK 发了但没修好」

必须先说清：**这并不等于 NACK 没用。** 家庭网络丢包率很低，
这 2 次命中是**偶发单包丢失**，而 `AU repaired` 的判定条件是
「`au_broken && au_broken_fixable && au_missing == 0`」——
要求**这一 AU 的所有缺口都被补回来**才计数。所以「`gaps=1` 的 NACK 发出 +
`AU repaired=0`」有三种可能，本轮**没有区分**：

| 可能 | 含义 | 怎么证 |
| --- | --- | --- |
| a. 网关不回重传 | 网关不支持 RFC 4585 的 Generic NACK，或只认它自己的重传协议 | 抓包看有没有对应的 RTP 重传包（同 seq 二次到达）；本仓库当前**没有记录「收到的包 seq 是否曾被 NACK 点名」**，需要加一个 `vid_nack_hit` 计数 |
| b. 网关回了但太晚 | `NACK_GAP_TTL_MS 300` 太短，重传包到达时缺口已被 TTL 摘除 | 看 `vid_stale_rtx`（R2 加的归属校验计数）有没有涨；涨了就说明「回来得太晚、AU 已换」 |
| c. NACK 未生效 / 序号对不上 | BLP 语义或 media SSRC 填错，网关忽略了这条报文 | 需要 `gst-launch`/tcpdump 抓 SRTCP 侧证据；家庭网络下不会自然复现 |

**下次上高铁（高丢包）时才是这条改动的真正考场。** 那时要看的关键串是：

```bash
./env/adb/adb.exe shell "grep -a -E 'NACK sent|AU repaired|stale retransmit|IDR starvation|consecutive P AU drops' $L | tail -100"
```

判据：

- `NACK sent` 频率 ≈ 丢包引起的缺口数（不是 0，也不该刷屏）；
- `AU repaired > 0` ⇒ 重传真的在补洞，NACK 有效；
- `stale retransmit > 0` 且 `AU repaired == 0` ⇒ 是可能 b（太晚），把
  `NACK_GAP_TTL_MS` 从 300 加到 500 再试；
- `AU repaired == 0` 且 `stale retransmit == 0` ⇒ 是可能 a（网关不回），
  此时 NACK 这条线判死，价值只剩 §10.1 的第 2、3 条（P 帧坏不清闸门 +
  `drop_streak` 语义修正）和 `IDR_STARVE_RELAX_MS` 兜底 —— 这三条不依赖网关。

### 10.5 未验证事项（诚实清单）

- `NACK_ROUNDS_IDR 5` / `NACK_ROUNDS_P 3` 只是按 RTT 估算选的，**没有在高丢包下实测过**。
- `NACK_GAP_TTL_MS 300` 与 `IDR_HOLD_MS 30` 的关系没实测算过（30ms 的 IDR hold
  到底能等到几轮重传，取决于 RTT）。
- NACK 的总带宽上限（原本要求 ≤ 视频码率 5%）**没有实现**，当前只靠
  `NACK_MIN_INTERVAL_MS 30` 和单报文 30 个 FCI 兜着。
- `vid_nack_hit`（「点名的 seq 后来真的到了」）**没有加**，所以 10.4 的 a/b/c 无法自动区分。
