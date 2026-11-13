# bug4：终末地「连上就全黑」根因分析与诊断手段

> 现象：明日方舟·终末地，建立连接后**画面一开始就全黑**（不是花屏、不是绿屏），
> 同一版本跑原神正常出画。
> 本文档记录 2026-11-13 这一轮的真机取证、假设排序、为定位而加的只读诊断通道，
> 以及顺带修掉的 4 个 native 缺陷（来自 `docs/REVIEW-report.md` 的 F1/R2/R5/N1）。
>
> 相关文档：`docs/BUGFIX-bug3-bug4-video.md`（bug3 马赛克残留 / bug4 早期绿屏）、
> `docs/BUGFIX-bug3-bug4-gst.md`（gst 侧三个 hunk 的落地与回退）、
> `docs/REVIEW-report.md`（独立审查，本文档 F1/R2/R5/N1 的四条来源）。

---

## 〇、定案（2026-11-13 20:35）：根因是 `kmssink sync=true`

> **先读这一节。** 本文档 §一 之后是 2026-11-13 上午～下午的排查过程记录，
> 其中关于「码流损坏」「`IDR without SPSPPS` 是根因」「SPS/PPS 补插错」的推断
> **已被真机四层验证推翻**，不要按那些方向继续排查。保留原文是为了留下判据与
> 反证链，不是待办。

### 0.1 结论

**根因是 `kmssink` 的 `sync=true`，不是码流、不是解码器、不是 AU 组装。**

Lead 已把 `PenCG/native/csrc/cgmedia/cm_gst.c` 的 `KMS_SINK_PROPS`
（`cm_gst.c:221` 附近）改成 `can-scale=true sync=false`，并就地写了约 40 行
防退步注释。**这是唯一的修复动作**，视频管线字符串其余部分（`rotation=270`、
`ignore-error=false max-errors=16`、plane-id=76）一律未动。

### 0.2 机制

`appsrc` 是 `format=time do-timestamp=true` —— 每个 buffer 的 PTS 由
**到达时刻**打上绝对时钟戳。`sync=true` 时 kmssink 按 PTS 决定显示时刻，
PTS 与 running-time 基准一旦错位（首次缓冲、重连、片头 I 帧丢失导致的跳变都算），
后续 buffer 就被**整体丢弃**。

结果正是用户看到的：**平面绑着、`fb` 有值、`AU push` 计数在涨，但平面不翻页，
停在最早那一帧**。所以「全黑」其实是「停在第一帧（片头黑场）」，
不是解不出来。

### 0.3 四层独立验证（Lead 已做，此处仅记录结论，不必重做）

| # | 验证 | 结果 | 排除了什么 |
| --- | --- | --- | --- |
| 1 | 把 §5.1 的 AU dump（`/userdisk/miniapp/data/au.h264`，615675 B / 300 帧）拉回本地用 ffmpeg 解 | 解出 **233 帧**，`corrupt decoded frame` 仅 **3 次**且都在片头 I 帧；抽帧 frame 200/232 是**完整清晰**的终末地画面（灰度均值 182.9，非零 89%） | **「码流损坏」——已撤回** |
| 2 | `gst-launch-1.0 filesrc ! h264parse ! mppvideodec` | 输出 954368 B = 233 帧 × 4096，与软解帧数一致 | **硬解本身正常** |
| 3 | `videotestsrc pattern=smpte ! … ! kmssink plane-id=76` | 全屏彩条，用户肉眼确认可见 | **plane76 硬件通道正常** |
| 4 | 同一条管线换数据源（`multifilesrc loop=true` 播同上 au.h264，`sync=true` + 小窗 `render-rectangle=<107,244,266,473>`） | 用户能看到终末地画面 | **管线 / rotation / 缩放 / plane 全对**，剩下的差异只有 `sync` |

第 1 层还顺带证否了「`IDR without SPSPPS, injected cached parameter sets`
是根因」：如果参数集真的错了，dump 出来的字节不可能解出 233 帧清晰画面。
**注入代码是对的**（§3 的逐字节推演结论仍然成立，只是它不是本案根因）。

### 0.4 真机对照（同一个游戏，唯一变量 = `sync`）

| 配置 | 平面观测 | 用户目视 |
| --- | --- | --- |
| `sync=true` | `plane[76] crtc=video_port0 fb=130`；10 次 × 2 秒采样 **`fb` 恒 130，一次不变**；同期 `AU push count=77` | 「有画面但完全静止（卡住的一帧）」 |
| `sync=false` | `fb` 在 127 / 130 之间**持续交替**（20 次 × 1 秒采样） | 「画面在动了！能正常玩」 |
| 原神回归（`sync=false`） | 同样交替 | 「画面正常、在动」 |

**判据固化**（以后任何「画面卡住/全黑但平面绑着」的问题，先用这条分辨）：

- `AU push` 在涨 + `fb` 恒定 ⇒ **sink 侧同步问题**（不是解码、不是网络）；
- `AU push` 在涨 + `fb` 交替 ⇒ 渲染正常，问题在上游或内容本身。

`fb` 编号本身不能当「在翻页」判据（原神正常局也曾长期恒为 127），
**必须看它在若干次采样里有没有变化**。

### 0.5 对本文后续章节的处理

| 章节 | 处置 |
| --- | --- |
| §一（`fps>0` 的错误前提） | **仍然有效**，它是这次定案的关键判据来源，保留 |
| §二（`rtp_reset_all()` 清缓存） | 结论有效（确实清了），但「缓存来自上一局」这个怀疑方向已无意义 |
| §三（注入字节布局） | 推演结论有效，§3.4 的「重排覆盖注入」是**独立的真 bug**，修复保留 |
| §四 假设 A（注入让 IDR 解成黑帧） | ❌ **已推翻**（见 0.3 第 1 层） |
| §四 假设 B | ❌ 早已排除 |
| §四 假设 C（mppvideodec 静默吐黑帧） | ❌ **已推翻**（见 0.3 第 2 层） |
| §四 假设 D（kmssink `sync=true` + `do-timestamp`） | ✅ **就是它**。当时给的「中低」可能性是错的 |
| §四 假设 E（bus ERROR 不置 failed） | 与本案无关，但修复本身是对的，保留 |
| §五（诊断通道） | **全部保留并继续可用**。§5.1 的 AU dump 正是定案的关键证据来源；§5.4 的观测方法需按 0.4 修正为「看 `fb` 是否变化」 |
| §六（REVIEW 四条修复） | 全部保留（与本案根因无关的独立缺陷） |
| §八（装机后 3 分钟判据） | 保留为「上次排查的方法论」，本次定案不需要再跑 |

### 0.6 一条方法论教训

这一轮把「`fps>0` 当作解码器在工作」的误读，导致我先去查码流和参数集，
绕了一大圈。真正的破局点是**把「推给 appsrc 的字节」和「KMS 平面上的字节」
切开**（AU dump + 平面 `fb` 采样）——**分层取证永远比顺着症状猜更快**。

---

## 一、先纠正一个错误前提（重要）

上一轮我（以及 lead）都说过「`fps>0` 说明解码器在出帧」。**这句话是错的**，
它会把整个排查带偏。核实结果：

| 字段 | 真实含义 | 证据 |
| --- | --- | --- |
| `emit_stat` 的 `"fps"` | **推帧速率**：`(stats.frames 增量)*1000/dms` | `cm_media.c:80` |
| `stats.frames++` 的唯一位置 | `cm_box_push()` **返回 0**（appsrc 接受）时 | `cm_rtp.c:988`（push 成功分支内） |
| `"decode"` | `s->stats.nal_out`，也是推帧侧计数 | `cm_media.c:140`（`s->stats.nal_out`） |

也就是说：**全仓库没有任何一处代码统计「解码器真正输出/丢弃了多少帧」**。
`fps=29` 只证明「我们每秒稳定把 29 个 AU 塞进了 appsrc，appsrc 都收下了」，
它完全不能证明 mppvideodec 吐出了 29 帧、更不能证明 kmssink 显示的是新画面。

同时确认另一件事：`plane[76]` 绑上 `fb=127`、`allocated by = mppvideodec0:sr`
说明 **mppvideodec 至少输出过一帧**并交给了 kmssink（平面不会在没有 buffer
的情况下绑定）。但 `fb` 编号在原神正常播放局里也长期恒为 127（lead 1s 间隔采样
6 次全是 127），所以 **fb 编号不能当作「平面在翻页」的判据**。

修正后的判定链：

```
first video frame pushed + 平面绑定  ⇒  解码器确实输出过至少一帧，走到 kmssink 了
屏幕全黑                             ⇒  ① 那一帧本身就是黑的/未初始化，或
                                        ② 平面停在最早那一帧再也不更新
```

能解释「解码器输出黑帧」的最强线索，仍然是那条计数差：

```
#1 IDR without SPSPPS, injected cached parameter sets   →  终末地 9 次 / 原神 0 次
```

---

## 二、`rtp_reset_all()` 到底有没有清参数集缓存？

**答案：清了。** 这是 lead 点名要回答的问题，这里给出确定结论。

`PenCG/native/csrc/cgmedia/cm_rtp.c:409-423`：

```c
static void rtp_reset_all(void) {
    rtp_reset_au();
    g_rtp.have_seq = 0;
    g_rtp.last_seq = 0;
    g_rtp.drop_streak = 0;
    g_rtp.sps_ok = 0;        /* ← 参数集缓存标记归零 */
    g_rtp.pps_ok = 0;
    g_rtp.gap_n = 0;
    g_rtp.au_hold_state = 0;
    g_rtp.au_hold_until_ms = 0;
    cm_buf_reset(&g_rtp.sps);      /* ← 实际字节也清空 */
    cm_buf_reset(&g_rtp.pps);
    cm_buf_reset(&g_rtp.au_out);
    cm_buf_reset(&g_rtp.au_sorted);
}
```

而 `cm_media.c:229` 在每一局进主循环时都会调 `cm_rtp_reset(s)`，后者无条件
调用 `rtp_reset_all()`（`cm_rtp.c` 的 `cm_rtp_reset` 内）。

⇒ **「注入的参数集来自上一局残留」这个假设不成立。** 该路径已经排除，
不必再往这个方向查。

同理，**注入顺序也是对的**（见下一节逐字节推演），所以 lead 担心的
「SPS/PPS 被放在 IDR 之后」也不成立。

---

## 三、注入字节布局的逐字节推演

### 3.1 `annexb_put` 写的是什么

`cm_rtp.c:360-364`（`annexb_put`）：

```c
static const unsigned char sc[4] = {0, 0, 0, 1};
if (cm_buf_put(b, sc, 4) < 0) return -1;
return cm_buf_put(b, nal, (size_t)n);
```

即每写一个 NAL 都先写 4 字节起始码 `00 00 00 01`，再写 NAL 本体。

### 3.2 `cache_param_set` 缓存的是什么

`cm_rtp.c:560-573`：

```c
t = nal[0] & 0x1F;
if (t == 7) {
    cm_buf_reset(&g_rtp.sps);
    g_rtp.sps_ok = (annexb_put(&g_rtp.sps, nal, n) == 0);
    g_rtp.au_has_sps = 1;
} else if (t == 8) { ... pps 同理 ... }
```

缓存的是**带 4 字节起始码的裸 NAL**，所以 `g_rtp.sps.p` 的内容形如

```
00 00 00 01 67 <SPS payload...>
             ^^ 0x67 = forbidden_zero=0, nal_ref_idc=3, type=7 (SPS)
```

`g_rtp.pps.p` 形如 `00 00 00 01 68 <PPS payload...>`（0x68 = type 8）。

⇒ **判据（给真机用）**：`AU push ... sps[0..7]=0000000167...` 里的第 5 字节
必须是 `67`，PPS 的第 5 字节必须是 `68`。若是 `00 00 00 01 65`（type 5 = IDR
切片）或 `00 00 00 01 61`（type 1 = 非关键切片），说明 `cache_param_set` 收错了
东西，注入就是纯粹往码流里塞垃圾。这一段 hex 就是为此打的。

### 3.3 注入拼装的最终布局

`cm_rtp.c:955-985`（`flush_frame` 的第 3 步，**注意是重排之后**）：

```c
out_p = base_p;                       /* base_p = 重排结果或原始 frame */
out_n = base_n;
if (key) {
    need_sps = !g_rtp.au_has_sps && g_rtp.sps_ok;
    need_pps = !g_rtp.au_has_pps && g_rtp.pps_ok;
    if (need_sps || need_pps) {
        cm_buf_reset(&g_rtp.au_out);
        if (need_sps) cm_buf_put(&g_rtp.au_out, g_rtp.sps.p, g_rtp.sps.len);
        if (need_pps) cm_buf_put(&g_rtp.au_out, g_rtp.pps.p, g_rtp.pps.len);
        cm_buf_put(&g_rtp.au_out, base_p, (size_t)base_n);
        ...
    }
}
```

字节布局：

```
[00 00 00 01][67 SPS...]   ← 注入（缓存里的，带起始码）
[00 00 00 01][68 PPS...]   ← 注入
[00 00 00 01][65 IDR ...]  ← 原 AU 的第一个 NAL（frame 缓冲里本来就带起始码）
[00 00 00 01][65 ...]      ← 后续切片（同一 AU 的其余分片）
```

**注入段在 IDR NAL 之前**，完全符合 Annex-B 与 h264parse 的要求。
⇒ lead 的怀疑点 B（「注入发生在 IDR 之后」）**已排除**。

### 3.4 但这里原本有一个真实的下游覆盖 bug（已修）

修复前的顺序是「先注入、再重排」，而重排分支是这样收尾的（旧代码）：

```c
out_p = g_rtp.au_sorted.p;          /* ← 直接覆盖掉注入结果 */
out_n = (int)g_rtp.au_sorted.len;
```

`au_sorted` 是由 `segs[]` 逐段重拼出来的，**它只包含 `frame` 里的分片，
不含刚刚注入进 `au_out` 的 SPS/PPS**。于是当「这个 IDR 既有 NACK 补洞（触发
重排）又缺参数集（触发注入）」时，注入会被无声地丢掉，交给解码器的是一个
**裸 IDR、没有参数集**。

这与真机现象高度吻合：终末地 `spsInject=9`（注入代码确实执行了 9 次）、
日志一行不落地打出来，但画面全黑 —— 因为注入的字节从来没到达 appsrc。

**修复**：把重排挪到注入之前，用 `base_p/base_n` 承接「重排结果或原始 frame」，
最后统一注入。见 `cm_rtp.c:912-953`（重排）与 `:955-985`（注入）。

---

## 四、假设排序（按可能性）

终末地的已知事实：平面已绑、我们稳定推 AU、解码器至少输出过一帧、**全黑**、
`spsInject` 9 次而原神 0 次、`caps CHANGED` 0 次、bus 上没有任何 mpp 错误。

### A（❌ 已被推翻，见 §〇）：注入的参数集内容让片头 IDR 解成黑帧

> **2026-11-13 20:35 定案：本假设不成立。** 300 帧 AU dump 用 ffmpeg 解出 233 帧
> 清晰画面，`corrupt decoded frame` 只有 3 次且都在片头。参数集若真错，不可能解出来。
> 原文保留作为反证记录。

`injected cached parameter sets` 是把**缓存里的** SPS/PPS 塞给一个**没带参数集
的 IDR**。这条路径只在「网关不发 SPS/PPS、或者发了但我们没收到」时触发。

两种失败形态：

1. **缓存的 SPS 与当前流的 SPS 不是同一套。** 分辨率/`profile`/`level` 一变，
   解码器用旧参数集初始化，解出来的就是错位甚至全黑。终末地的 `spsInject=9`
   说明它有 9 个 IDR 走了补插路径 —— 这条路径在原神局一次都没走过，
   **正好是两局唯一的行为差异**。
2. **注入的 SPS 是好的，但 PPS 与它不匹配**（缓存时刻错配）。

**为什么排在第一位**：它是唯一能与「原神正常、终末地全黑、其余指标全正常」
这个组合同时吻合的解释。

**怎么证伪/证实**：AU dump（见第五节）。ffmpeg 能解出画面 ⇒ A 排除；
ffmpeg 也黑/报错 ⇒ A 成立，接着比对 dump 里注入的 SPS 与流内 SPS 是否一致。

### B（❌ 已排除，见 §〇）：`rtp_reset_all()` 没清缓存 / 注入顺序错

见第二节、第三节：清了，顺序也对。**排除**。

### C（❌ 已被推翻，见 §〇）：mppvideodec 静默吐黑帧（不报错）

> **2026-11-13 20:35 定案：本假设不成立。** `gst-launch-1.0 filesrc ! h264parse
> ! mppvideodec` 对同一份 dump 输出 954368 B = 233 帧 × 4096，与软解一致 ⇒ 硬解正常。

`ignore-error=false max-errors=16` 已经加上了（`cm_gst.c:249`）。
如果参数集不对，MPP 有可能**不报错**、直接输出全黑或全绿的帧 —— 这种「静默
错误」不会在 bus 上留下痕迹，所以 `grep 'bus:'` 干净并不能排除 C。

**为什么排第二**：它能解释「全黑而不是花屏」（解不出内容就吐黑），且不报错。
但它无法解释「为什么只有终末地」，除非承认 A 是上游原因（A 导致 C）。

**怎么证伪**：软解开关（见第五节）。软解也黑 ⇒ C 排除（不是硬解的锅）。

### D（✅ 就是它，见 §〇；当时误评为「中低」）：kmssink `sync=true` + appsrc `do-timestamp=true` 让平面停住

> **2026-11-13 20:35 定案：这才是根因。** 已把 `KMS_SINK_PROPS` 改为 `sync=false`
> （`cm_gst.c:221` 附近）。真机对照：`sync=true` 时 `fb` 10×2s 采样恒为 130（平面不动）；
> `sync=false` 时 `fb` 在 127/130 间持续交替（画面在动）。

`cm_box_push` 目前 **不设 PTS**（`cm_gst.c:1220` 的 `(void)pts_ns;` 还在），
buffer 的时间戳由 appsrc 的 `do-timestamp=true` 按到达时间打。若某次 push 的
时间戳异常（或 appsrc 内部时钟与 kmssink 的 pipeline clock 不同步），
kmssink 可能一直在等「该显示的时刻」，平面就停在那一个 buffer 上。

**为什么排在后面**：`fps=29` 说明 appsrc 一直在收，若时间戳是单调正常的，
kmssink 不该卡住。而且同样的推送逻辑在原神局工作正常。

**怎么证伪**：见第五节的「平面刷新观测法」—— 若 `state` 里的 buf addr 在变，
说明平面在更新，D 排除。

### E（低）：`cm_box_drain_bus` 的 ERROR 没置 `failed`，push 继续成功

这是 audio-route 在 `docs/BUGFIX-bug3-bug4-gst.md §5.3` 留下的遗留：bus 上出现
ERROR 时只打印，不置 `b->failed`，于是 appsrc 继续收帧、`stats.frames` 继续涨、
`fps` 继续显示 29，而画面永久停住 —— **表现和全黑完全一样，且统计上完全正常**。

**为什么排最后**：`grep 'bus:'` 在本局没有出现 mpp 相关错误（只有那条既有的
无害 WARNING）。但只要它是「只在特定码流下触发」，就不能排除。

**已修**：见第六节第 4 条 —— ERROR 现在会置 `b->failed`，之后每次 push 都
会被拒绝并打一行醒目的 `video box marked failed after decoder error`。

### 已排除的假设（附证据）

| 假设 | 结论 | 证据 |
| --- | --- | --- |
| caps 缺 profile/level 导致 High profile 被拒 | **排除** | 真机 `gst-inspect-1.0 mppvideodec`：sink template `video/x-h264` 只标 `parsed: true`，不约束 profile/level |
| MPP 不支持 1080p / High | **排除** | 同上，src 支持 NV12/NV16/NV12_10LE40/NV21/I420/YV12/RGB… |
| 首个 buffer 带 PTS 导致输出未初始化 NV12 | **排除** | `cm_box_push` 里 `(void)pts_ns;`，PTS 根本没传下去 |
| 视频分支加了 `! queue` 导致纯绿 | **排除** | 管线字符串里没有 queue（`cm_gst.c` 的 `video_pipeline_fmt`），且本次全黑不是绿 |
| `caps CHANGED` 后 flush 没恢复 | **排除** | 本局 `caps CHANGED` 全程 0 次 |

---

## 五、诊断通道（本次新增，全部是只读，不改语义）

### 5.1 AU dump（最关键）

**用途**：把「我们喂给解码器的码流」「解码器/显示这一层」彻底切开。

**开关**：

```bash
# 打开（下一个 AU 关键帧开始记录）
./env/adb/adb.exe shell "touch /userdisk/miniapp/data/dumpau.txt"

# 关掉（并删除产物）
./env/adb/adb.exe shell "rm -f /userdisk/miniapp/data/dumpau.txt /userdisk/miniapp/data/au.h264"
```

**产物**：`/userdisk/miniapp/data/au.h264`

**实现**：`cm_rtp.c:734` `au_dump_hook(const unsigned char *p, int n, int key)`，
调用点在 `cm_rtp.c:1031` —— **紧贴 `cm_box_push` 成功之后、传同一份
`out_p/out_n`**，所以落盘字节与推给 appsrc 的字节**逐字节相同**（含注入的
SPS/PPS）。

三条硬约束（都已实现）：

1. **只从完整 IDR 开始**：`if (!key) return;`（`cm_rtp.c:748`），直到见到
   `key==1` 才 `fopen`。从 P 帧起头的码流没有参考帧，ffmpeg 也解不出来，
   判断会直接错。
2. **限量**：`AU_DUMP_MAX_FRAMES 300` / `AU_DUMP_MAX_BYTES 8MB`，满了自动停
   并打 `AU dump done: %llu frames, %llu bytes`（`/userdata` 只剩 55MB）。
3. **零开销**：开关文件不存在时 `g_dump_state = 1` 永久关闭，之后每次调用只做
   一次 `if` 判断。

**验证（lead 侧执行）**：

```bash
./env/adb/adb.exe shell "/bin/ffmpeg -f h264 -i /userdisk/miniapp/data/au.h264 -frames:v 1 -y /tmp/f.png"
./env/adb/adb.exe pull /tmp/f.png .
```

若是「出图但花/错位」而不是「完全无输出」，再加 `-vf scale=240:-1` 看缩略图，
或者解多个帧：

```bash
./env/adb/adb.exe shell "/bin/ffmpeg -f h264 -i /userdisk/miniapp/data/au.h264 -frames:v 5 /tmp/f%02d.png"
```

### 5.2 软解强制开关（A/B 分层）

```bash
# 强制走 avdec_h264（软解），跳过 mppvideodec
./env/adb/adb.exe shell "touch /userdisk/miniapp/data/softdec.txt"
# 恢复硬解
./env/adb/adb.exe shell "rm -f /userdisk/miniapp/data/softdec.txt"
```

**实现**：`cm_gst.c:644` `#define VIDEO_SOFTDEC_SWITCH`，判断在 `cm_gst.c:1086`。
命中时打 `video: forcing software h264 decoder (...)` 并选
`video_pipeline_soft()`（**不改任何管线字符串**，两条都是现成的）。

| 硬解 | 软解 | 结论 |
| --- | --- | --- |
| 全黑 | **有画面** | 码流是好的，**锅在 mppvideodec 这一层** → 查 MPP 日志 / 换 profile 验证 |
| 全黑 | 全黑 | **锅在我们的 AU 组装 / SPS-PPS 注入** → 看 5.1 的 dump |

### 5.3 AU push 诊断行（1 秒限速）

`cm_rtp.c:1000` 起，push 成功后每秒打一行：

```
#1 AU push key=1 bytes=5413 sps=0 pps=0 inject=1 reseq=0 segs=42 broken=0 missing=0 \
   sps.len=29 sps[0..7]=00000001674d002a pps.len=8 pps[0..7]=0000000168ee3c80
```

字段读法：

| 字段 | 含义 / 判据 |
| --- | --- |
| `key` | 1 = 关键帧。全黑时先看这行的 `key` 是不是一直 0（那就根本没推过 IDR） |
| `bytes` | 实际推给 appsrc 的字节数 |
| `sps`/`pps` | 这个 AU **自带**参数集吗（1 带 / 0 不带） |
| `inject` | 累计注入次数。**在涨 = 一直在走补插路径** |
| `sps.len` / `sps[0..7]` | 缓存 SPS 的长度与前 8 字节。第 5 字节必须是 `67` |
| `pps.len` / `pps[0..7]` | 缓存 PPS。第 5 字节必须是 `68` |
| `segs`/`reseq`/`broken`/`missing` | 分片账目。`reseq=1` 说明这个 AU 走了重排 |

### 5.4 平面刷新观测（区分「在刷新」与「停在同一个 buffer」）

`summary` 被内核截断在 576 字节，读不到视频面，**必须读 `state`**：

```bash
# 连续采样，看 plane[76] 的 buf addr / 帧号是否在变
./env/adb/adb.exe shell 'for i in 1 2 3 4 5 6 7 8 9 10; do \
  cat /sys/kernel/debug/dri/0/state | grep -a -A1 "plane\[76\]"; sleep 0.3; done'
```

关注 `plane[76]: Esmart1-win0` 那一行的 `fb=` 编号 —— **但注意 lead 已在原神
正常局验证过 fb 编号长期恒为 127，所以 fb 不是可靠判据**。可靠的替代做法：

```bash
# 用 modetest 直接 dump 视频平面到文件，两次比较
./env/adb/adb.exe shell "/bin/modetest -M rockchip -s 76@68:720x1280 -d 2>&1 | head -5"
```

更实际的办法（不需要额外工具）：**软解 A/B + AU dump 这两条已经足以定性**，
平面采样只用于确认「排除 D」。

> **✅ 2026-11-13 20:35 定案后的修正（重要，本节原判断已反了）**
>
> 当时写「`fb` 不是可靠判据」，这句话只说对了一半：**`fb` 的绝对值不可靠，
> 但 `fb` 是否变化极其可靠** —— 定案正是靠这一条翻盘的。
>
> | `AU push` | `fb` 多次采样 | 结论 |
> | --- | --- | --- |
> | 在涨 | **恒定不变**（如 10×2s 全是 130） | **sink 侧同步卡死**（`sync=true` 的典型症状），别查码流和解码器 |
> | 在涨 | **在 127/130 间交替**（20×1s） | 渲染正常，问题在上游或内容本身 |
>
> 所以 `fb` 要按「变化与否」用，不要按「等于几」用。
> `sync=true` → `fb` 恒 130、`AU push count=77`；改 `sync=false` → `fb` 持续交替、
> 用户肉眼确认「画面在动了」。详见 §〇.4。

### 5.5 bus ERROR（已加强）

`cm_gst.c:1241` 起：video 的 `GST_MESSAGE_ERROR` 现在

1. 首次出现必定打 `video bus ERROR (first) domain=%u code=%d msg=... debug=...`
   （`CM_LOGE`，真机一定落盘）；
2. 置 `b->failed = 1` 并打 `video box marked failed after decoder error: ...`。

WARNING / EOS 单独一轮 pop（`cm_gst.c:1265` 起），**绝不置 failed**，
避免既有那条无害的 `Pipeline construction is invalid, please add queues.`
让每次播放瞬间失败。

⇒ 装了新包之后 `grep 'bus ERROR'` 若出现，E 就是根因；若 `grep 'marked failed'`
出现，说明解码器真的报错而以前被我们吞掉了。

---

## 六、本次同时修掉的 4 个 native 缺陷（来自 REVIEW-report）

这 4 条与 bug4 的直接因果未必成立，但都是**会让「偶发问题」变成「必然问题」**
的真实缺陷，且都在本次改动路径上，一并修掉。

### 1. F1：`au_broken_fixable` / `au_start_ms` 不随 AU 复位

**位置**：`rtp_reset_au()`，`cm_rtp.c:371-407`（修复后）。

**旧行为**：清了 11 个字段，漏了这两个。后果链：

```
上一帧丢包 → au_broken_fixable = 1
   ↓ AU 复位（fixable 残留 1）
下一帧 STAP-A 长度字段越界 → 只置 au_broken=1（结构性损坏，不可修复）
   ↓ flush_frame 的判据 if (au_broken && au_broken_fixable && au_missing == 0)
三条全中 → 打「AU repaired by retransmissions」→ 把截断的 AU 推给解码器
```

**修复**：`cm_rtp.c:399-400` 补 `au_broken_fixable = 0; au_start_ms = 0;`；
STAP-A 越界分支（`cm_rtp.c:813`）显式置 `au_broken_fixable = 0`；
STAP-A 分支开头（`cm_rtp.c:1146`）补 `if (!g_rtp.au_start_ms) au_start_ms = now;`。

### 2. R2：重传包不判归属

**位置**：`gap_take(seq)` 命中后的处理，`cm_rtp.c:1560-1585`（修复后）。

**旧行为**：缺口表按 300ms TTL 存活，而 AU 边界会 reset。上一 AU 的重传包回来时，
当前正在拼的可能是**另一个** AU，直接 append 会把别人的分片拼进来，污染整帧
且不被发现（成功分支原本不置 `au_broken`）。

**修复**：用 `au_first_seq/au_last_seq`（这两个字段本来就为「判定重传包归属」
而存在，之前全文件无读取者）做范围校验，越界则计入 `vid_stale_rtx`
并丢弃。

### 3. R5：`gap_purge_expired()` 无条件置 `au_broken`

**位置**：`cm_rtp.c:477-515`。

**旧行为**：摘掉**任意**过期缺口就 `au_broken = 1`。而 TTL 300ms 远大于帧间隔
（33ms）：上一帧遗留的缺口一过期，就把当前这个**完整**的 AU 打成残帧整帧丢掉。

**修复**（`cm_rtp.c:504-513`）：

- 过期 `seq` 落在 `[au_first_seq, au_last_seq]` 区间内 → 置脏（这个 AU 确实缺了这一片）；
- 落在区间外 → 什么都不做（那是别的 AU 的旧缺口）；
- `au_have_range == 0`（此刻没有正在拼的 AU，处于帧间空隙）→ **也什么都不做**。
  这一条是修完 R5 之后又发现的一个连带问题：`au_broken` 只在 `flush_frame` 里被消费，
  而 STAP-A 与单 NAL 两条分支都不会在开头清它 —— 若在帧间空隙置脏，它会一直被带到
  下一个 AU，把一帧好画面无端判成残帧丢掉。没有 AU 就没有可损坏的对象。

### 4. N1：NACK 缓冲越界导致静默退化明文

**位置**：`cm_cmd.c:417` 与 `cm_cmd.c:460-478`。

**旧行为**：`pkt[12 + NACK_MAX_FCI*4]` = 132 字节，而 `cm_srtcp_protect`
要求 `len + 14 <= cap`（4 字节 index + 10 字节 auth tag，见 `cm_srtp.c`）。
`nfci = 27` 时 `len = 120`，`120 + 14 = 134 > 132` → `cm_srtcp_protect` 返回 -2
→ 旧代码 `if (enc > 0) len = enc;` **失败后照样按明文发出去**。

明文 RTCP 在 SRTP 会话里被网关直接丢弃，但 `vid_nack_sent++` 和
`NACK sent` 日志照打 —— **计数器显示一切正常，实际全是死包**。
而 `nfci >= 27` 恰好就是丢包最严重、最需要重传的时候。

**修复**：`pkt` 加 16 字节余量；加密失败时**不发**（`CM_LOGW("NACK dropped:
srtcp protect failed")`），不再退回明文。只有 DTLS-SRTP 尚未 ready 时才允许
明文（那时明文是唯一选择，PLI 也是这么干的）。

### 5. L1：可观测性

`emit_stat` 的 JSON 增加 5 个字段（`cm_media.c:131-132`）：

```
"nackSent" / "nackReq" / "nackRecovered" / "relax" / "staleRtx"
```

读法：

| 组合 | 结论 |
| --- | --- |
| `nackSent` 涨 + `nackRecovered` 一直是 0 | 网关不重传，或 NACK 没送到（现在至少不会因为 N1 而静默降级） |
| 两者同步涨 | 重传在工作，重点看 `auDrop` 是否下降 |
| `relax > 0` | 已经靠「接受花屏」换画面在动，前两层没救回 |
| `staleRtx > 0` | 缺口 TTL 跨过了 AU 边界（R2 生效，属健康信号） |

`still no IDR` 与 `packet loss seen` 两行告警也补了 `nack/recov/relax/staleRtx`。

---

## 七、构建验证

```
cd PenCG
powershell -NoProfile -ExecutionPolicy Bypass -File native/build.ps1
```

结果（2026-11-13 19:02）：

```
OK -> ...\PenCG\libs\libjsapi_cgnative.so (60.7 KB)
OK -> ...\PenCG\libs\libjsapi_cgmedia.so  (94.5 KB)
exit 0
```

唯一 warning 是既有的 `cm_gst.c:571 address of function 'gst_debug_message_get'
will always evaluate to 'true' [-Wpointer-bool-conversion]`，与本次改动无关
（该行是上一轮就有的）。

本次改动**没有引入任何新的 GStreamer 符号**：AU dump 只用 `fopen/fwrite/fclose`
（标准 C），bus 加强只用已有的 `gst_bus_timed_pop_filtered` /
`gst_message_parse_error` / `gst_message_parse_warning` / `gst_message_unref`，
软解开关只用 `fopen/fclose`。

---

## 八、装机后怎么跑（3 分钟判据）

**步骤 0** —— 先确保没有调试开关拖慢性能（GST_DEBUG 会把帧率打到 2fps）：

```bash
./env/adb/adb.exe shell "rm -f /userdisk/miniapp/data/gstdebug.txt"
./env/adb/adb.exe shell "touch /userdisk/miniapp/data/dumpau.txt"   # 要 dump 才 touch
```

**步骤 1** —— 装上 lead 构建的包，开一局终末地，玩 3 分钟。

**步骤 2** —— 现场抓日志：

```bash
./env/adb/adb.exe shell "grep -a 'pencg' /userdata/applog/DictPen_*.log | tail -400" > /tmp/en.log
```

**步骤 3** —— 按下面的判据表 grep：

| # | 命令 | 看什么 | 结论 |
| --- | --- | --- | --- |
| 1 | `grep -a -c 'AU dump done' /tmp/en.log` | 1 | dump 成功产出，可以做 5.1 的 ffmpeg 验证 |
| 2 | `grep -a 'AU push key=1' /tmp/en.log \| head -3` | `inject=` 在涨吗？`sps[0..7]` 第 5 字节是 `67` 吗？ | 判据见 5.3 |
| 3 | `grep -a -c 'injected cached parameter sets' /tmp/en.log` | 是否仍 ~9 次 | 仍有 = 补插路径还在走 |
| 4 | `grep -a 'bus ERROR' /tmp/en.log` | 有无 | **有 = C/E 成立，看 msg/debug 文本** |
| 5 | `grep -a 'marked failed' /tmp/en.log` | 有无 | **有 = 解码器真的报错，以前被吞了** |
| 6 | `grep -a 'forcing software' /tmp/en.log` | 有无 | 确认软解开关生效 |
| 7 | `grep -a 'NACK sent' /tmp/en.log \| tail -3` | `srtcp` vs `plain` | `plain` 说明 SRTCP 没 ready（N1 之后不会再静默降级） |
| 8 | `grep -a 'AU push' /tmp/en.log \| tail -5` | `broken=` / `missing=` | 全 0 说明我们喂的 AU 是完整的 |

**步骤 4** —— AU dump 验证（这一步由 lead 执行）：

```bash
./env/adb/adb.exe shell "/bin/ffmpeg -f h264 -i /userdisk/miniapp/data/au.h264 -frames:v 1 -y /tmp/f.png"
./env/adb/adb.exe shell "ls -l /tmp/f.png"
./env/adb/adb.exe pull /tmp/f.png .
```

### 三种结果怎么解读

| AU dump 结果 | 软解 A/B 结果 | 根因 | 下一步 |
| --- | --- | --- | --- |
| ffmpeg **能解出正常画面** | 软解有画面 | **A 排除**，问题在 mppvideodec/显示层 | 试 `gst-launch-1.0` 把同一个 au.h264 直接喂硬解，确认是 MPP 对该码流的行为；必要时看 X6 Pro MPP 日志 |
| ffmpeg **能解出正常画面** | 软解也黑 | 码流好、软硬解都黑 ⇒ **D（kmssink 时间戳/同步）** | 查 pipeline clock、试 `sync=false` |
| ffmpeg **解不出 / 出黑帧/报错** | 都是黑 | **A 成立**：我们喂的码流就是坏的 | 看 5.3 那行 `sps[0..7]`，确认 SPS 第 5 字节是 `67`、PPS 是 `68`；若 hex 明显不对，说明 `cache_param_set` 缓存了错误内容，要抓「缓存那一刻」的原始 RTP |
| ffmpeg 报 `no frame` / 空文件 | — | dump 没产出（开关没生效或没等到 key=1） | 回到步骤 3 第 1 条 |

**最信哪个**：**A**。理由是它是唯一能同时解释「原神正常 / 终末地全黑 /
`spsInject` 9 vs 0 / 其它指标全正常」这四个事实的假设；而且本次已经发现
「注入被重排覆盖」这个**确定的**代码缺陷（`spsInject` 涨但注入字节从没到过
appsrc），它在终末地局的高触发频率（9 次 vs 0 次）与症状完全对齐。

---

## 九、本次改动清单

行号以本次编译（19:07）后的文件为准。

| 文件 | 位置 | 改动 |
| --- | --- | --- |
| `cm_rtp.c` | `:369-406` | F1：`rtp_reset_au()` 补清 `au_broken_fixable` / `au_start_ms` |
| `cm_rtp.c` | `:475-512` | R5：`gap_purge_expired()` 按 AU seq 范围判断是否置脏 + 帧间空隙不置脏 |
| `cm_rtp.c` | `:721-793` | 新增 AU dump 宏与状态（`AU_DUMP_SWITCH` / `AU_DUMP_MAX_FRAMES` 等） |
| `cm_rtp.c` | `:735-775` | `au_dump_hook()` 实现 |
| `cm_rtp.c` | `:913-954` | 重排挪到注入之前（修掉「注入被重排覆盖」） |
| `cm_rtp.c` | `:956-987` | 注入改用 `base_p/base_n`，日志加 `sps=%dB pps=%dB au=%dB` |
| `cm_rtp.c` | `:1001-1040` | AU push 诊断行（1 秒限速）+ 调用 `au_dump_hook` |
| `cm_rtp.c` | `:1147` `:1296` | STAP-A / 单 NAL 分支补 `au_start_ms` |
| `cm_rtp.c` | `:1159` | STAP-A 长度越界显式清 `au_broken_fixable` |
| `cm_rtp.c` | `:1560-1585` | R2：重传包归属校验 + `vid_stale_rtx` |
| `cm_session.h` | `:141` | `cm_stats` 新增 `vid_stale_rtx` |
| `cm_cmd.c` | `:417` | N1：`pkt` 加 16 字节余量 |
| `cm_cmd.c` | `:465-479` | N1：加密失败不发、不再退回明文（`trans` 赋值明确化） |
| `cm_media.c` | `:131-132` | L1：JSON 加 `nackSent/nackReq/nackRecovered/relax/staleRtx` |
| `cm_media.c` | `:168-172` | L1：对应实参 |
| `cm_media.c` | `:98-121` | `still no IDR` / `packet loss seen` 两行补 nack/recov/relax/staleRtx |
| `cm_gst.c` | `:644` | 新增 `VIDEO_SOFTDEC_SWITCH` 宏 |
| `cm_gst.c` | `:1086-1095` | 软解强制开关（`cm_box_create` 的 video 分支） |
| `cm_gst.c` | `:1206-1290` | bus ERROR 两轮弹出 + 置 `failed` + 原样文本 |

**没有改视频管线字符串**（`cm_gst.c:249` 的 `... mppvideodec rotation=270
ignore-error=false max-errors=16 ...` 保持原样），**没有加 `! queue`**。

## 十、遗留风险

1. **A 若成立，根因在「缓存了错误的 SPS/PPS」，但缓存动作本身没有诊断。**
   本次只能看到「缓存内容是什么」（`sps[0..7]` hex），看不到「它是从哪个 RTP
   包缓存来的」。若 hex 正常但仍全黑，下一步要在 `cache_param_set` 里加一行
   dump 原始 NAL 全长（不只是前 8 字节）。
2. **AU dump 只 dump 前 300 帧**：终末地的关键帧间隔若很大，300 帧可能覆盖不到
   第二个 IDR，只能证明「第一个 IDR + 后续 P 帧」是否可解。够定位、不够看长跑。
3. **`fb` 编号不能当刷新判据**（lead 已在原神局验证）。平面是否在翻页目前没有
   便宜可靠的观测手段，只能靠软解 A/B + AU dump 间接判断。
4. **`vid_stale_rtx` 的阈值未定**：正常弱网下应接近 0；若持续增长说明
   `NACK_GAP_TTL_MS(300ms)` 相对 RTT 偏短，可能需要按 RTT 动态调（本次没做）。
5. **bus ERROR 置 `failed` 是单向的**：一旦置位，本局内不再恢复
   （`cm_box_push` 直接拒绝）。这是有意的（宁可干净失败也不要半死状态），
   但意味着出现一次解码错误后整局黑屏。若真机上一局偶发一次就全黑，
   需要改成「置 failed + 通知上层重建 vbox」，那是 `cm_cmd.c` 的活，本次没做。
6. **本次所有结论都基于代码与静态证据 + lead 转述的真机日志**。我自己没有
   装机验证的能力（禁 adb 运行真机测试），所以「A 是第一嫌疑」是**推断**而非
   **证实**，必须靠第八节的三步走实测收敛。
