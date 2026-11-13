# Bug3/Bug4 —— `cm_gst.c` 侧三处落地（GStreamer 出画层）

> 作者：audio-route（本单元写域 = `PenCG/native/csrc/cgmedia/cm_gst.c`）
> 日期：2026-11-13
> 上游：`docs/BUGFIX-bug3-bug4-video.md`（video-integrity 的 RTP 层修复，已真机生效）
> 本文只记录**出画层**`cm_gst.c`里的三处改动。RTP 层（`cm_rtp.c` / `cm_media.c` /
> `cm_session.h`）的改动不在本文范围。

---

## 0. 一句话结论

| hunk | 内容 | 状态 |
|---|---|---|
| 1 | `mppvideodec` 加 `ignore-error=false max-errors=16` | ✅ **落地** |
| 2 | 视频 buffer 打 `DELTA_UNIT` 标记 | ✅ **落地（降级版）** |
| 2 | 视频 buffer 打 PTS/DTS | ❌ **跳过**（符号不存在 + 无头文件，见 §2） |
| 3 | 分辨率变化时发 flush 事件强制重新协商 | ✅ **落地** |

两次编译均 exit 0：
- `native/build-media.ps1` → `OK -> libs/libjsapi_cgmedia.so (82.1 KB)`
- `native/build.ps1` → `libjsapi_cgnative.so (60.7 KB)` + `libjsapi_cgmedia.so (82.1 KB)`

唯一告警是**既有**的（本次改动之前就有，与本文无关）：
```
cm_gst.c:561:12: warning: address of function 'gst_debug_message_get'
                 will always evaluate to 'true' [-Wpointer-bool-conversion]
```

---

## 1. hunk 1：解码器错误策略（已落地）

**位置**：`cm_gst.c:239`（`video_pipeline_fmt()` 的返回字符串）

```c
return "appsrc name=vsrc is-live=true format=time do-timestamp=true "
       "caps=video/x-h264,stream-format=byte-stream,alignment=au "
       "! h264parse ! mppvideodec rotation=270 ignore-error=false max-errors=16 "
       "! videoconvert" KMS_SINK_PROPS;
```

**只加了 `ignore-error=false max-errors=16` 两个属性，字符串其余部分一字未动**
（`rotation=270`、`caps=...`、`KMS_SINK_PROPS` 全部原样；软解分支
`video_pipeline_soft()` 的 `avdec_h264` 没有这两个属性，未改）。

### 目的

`gst-inspect-1.0 mppvideodec` 真机核实（2026-11-12 现场）：

```
ignore-error : Ignore MPP decode errors
               flags: readable, writable
               Boolean. Default: true
max-errors   : Max consecutive decoder errors before returning flow error
               flags: readable, writable
               Integer. Range: -1 - 2147483647 Default: -1
```

默认 `ignore-error=true` 是「解坏了也不报错、把坏帧直接吐出去上屏」——现象是
**花屏，而日志里一条都没有**，排查时完全看不见。

RTP 层已经拦住了残帧（bug3/bug4 的第一道防线），这里是**第二道防线**：让网关侧
编码器自身出错这类我们识别不出的坏帧形态，变成一个 `GST_MSG_ERROR` 被
`cm_box_drain_bus()`（`cm_gst.c:1126` 附近）抓下来打进日志，而不是悄悄糊在屏幕上。

`max-errors=16` 是**连续**错误上限：连续错 16 次才让解码器吐 flow error，
偶发单帧坏不会立刻停摆。

### 风险

- 真机上若看到「画面卡住 + 日志有 mpp 解码错误」，就是这道防线在生效（16 次连续
  错误后 flow error）。此时**不会**自动重建管线——`b->failed` 只由 push 返回值置位，
  而 bus 上的错误不会置它。这是已知遗留项（见 §5）。
- **回退方式**：把 `ignore-error=false max-errors=16 ` 这段删掉，恢复插件默认值即可，
  一个 token 都不影响其它逻辑。

---

## 2. hunk 2：buffer 标记（**DELTA_UNIT 落地，PTS/DTS 跳过**）

**位置**：声明区 `cm_gst.c:81-104`；函数体 `cm_gst.c:1206-1250`

### 2.1 落地部分：`DELTA_UNIT`

```c
if (b->is_video && !key) {
    gst_buffer_set_flags(buf,
                         gst_buffer_get_flags(buf) | GST_BUFFER_FLAG_DELTA_UNIT);
}
```

原代码是两行 `(void)pts_ns; (void)key;`，把关键帧信息完全丢弃。下游只能靠 H.264
NAL 类型自己猜；而 `cm_rtp.c` 的丢包逻辑**确实会丢帧**（`packet loss seen
(lost=.. auDrop=..)`），被丢过帧之后 NAL 猜测与真实帧序一旦错位，解码器就会把
P 帧当 I 帧用。显式标 `DELTA_UNIT` 是把我们已知的信息传下去，成本为零。

### 2.2 ⚠ 一处必须记住的取值陷阱：`DELTA_UNIT` 是 `1u << 13`，不是 `1u << 4`

交接文档 `docs/BUGFIX-bug3-bug4-video.md:482` 写的是
`#define GST_BUFFER_FLAG_DELTA_UNIT (1u << 4)` —— **这个值是错的，那是 `LIVE`**。

GStreamer 1.22.0 的 `gstbuffer.h` 原文：

```c
typedef enum {
  GST_BUFFER_FLAG_LIVE       = (GST_MINI_OBJECT_FLAG_LAST << 0),
  GST_BUFFER_FLAG_DECODE_ONLY= (GST_MINI_OBJECT_FLAG_LAST << 1),
  GST_BUFFER_FLAG_DISCONT    = (GST_MINI_OBJECT_FLAG_LAST << 2),
  GST_BUFFER_FLAG_RESYNC     = (GST_MINI_OBJECT_FLAG_LAST << 3),
  GST_BUFFER_FLAG_CORRUPTED  = (GST_MINI_OBJECT_FLAG_LAST << 4),
  GST_BUFFER_FLAG_MARKER     = (GST_MINI_OBJECT_FLAG_LAST << 5),
  GST_BUFFER_FLAG_HEADER     = (GST_MINI_OBJECT_FLAG_LAST << 6),
  GST_BUFFER_FLAG_GAP        = (GST_MINI_OBJECT_FLAG_LAST << 7),
  GST_BUFFER_FLAG_DROPPABLE  = (GST_MINI_OBJECT_FLAG_LAST << 8),
  GST_BUFFER_FLAG_DELTA_UNIT = (GST_MINI_OBJECT_FLAG_LAST << 9),   /* ← 这个 */
  GST_BUFFER_FLAG_TAG_MEMORY = (GST_MINI_OBJECT_FLAG_LAST << 10),
  ...
} GstBufferFlags;
```

而 `GST_MINI_OBJECT_FLAG_LAST = (1 << 4) = 16`，于是：

| 宏 | 展开 | 数值 |
|---|---|---|
| `GST_BUFFER_FLAG_LIVE` | `16 << 0` | `16` = `1u<<4` |
| `GST_BUFFER_FLAG_DELTA_UNIT` | `16 << 9` | `8192` = `1u<<13` |

代码里已按 **`1u << 13`** 写死并附原文注释。若照抄 `1u<<4`，会把每个 P 帧标成
LIVE（"PAUSED 时应丢弃的直播数据"）——**语义完全相反**，PAUSED 状态下画面会被丢，
QoS 行为也会变。

### 2.3 跳过部分：PTS/DTS —— 三条硬理由

任务书允许对这处降级。**不是漏做，是不能做**：

1. **仓库里没有任何 GStreamer 头文件。**
   `find . -iname gstbuffer.h` → 0 命中（整个 `D:\...\haas-x6pro` 与 skills 目录都搜过）。
   本文件走的是「手写最小声明」路线（见文件头注释），而
   `GST_BUFFER_PTS(buf)` / `GST_BUFFER_DTS(buf)` / `GST_BUFFER_FLAGS(buf)` 都是
   `gstbuffer.h` 里的**结构体字段宏**，没有头文件根本无法展开。硬写结构体偏移
   等于赌 ABI 布局，赌错就是内存写坏——比不改严重得多。

2. **官方 accessor 符号不存在。**
   `gst_buffer_get_pts` / `gst_buffer_set_pts` 在
   - 真机 `libgstreamer-1.0.so.0`（1.22.0）
   - 本地链接桩 `native/stubs-aarch64-gst/libgstreamer-1.0.so.0`

   **两边的导出符号表里都没有**（我用 Python 直接解析 ELF `.dynsym`，桩库共 2413 个
   导出符号，逐名比对）。官方头文件里也确实没有这两个函数——PTS 只能用宏访问。
   一经调用就是 dlopen 期未解析符号，**整个 `libjsapi_cgmedia.so` 加载失败**。

3. **与 `do-timestamp=true` 冲突。**
   appsrc 当前是 `format=time do-timestamp=true`，由 appsrc 按**到达时间**自己打
   时间戳。手工再塞一个 PTS 与之叠加，等于两套时间轴打架。要让 RTP 的 90kHz
   时间戳真正生效，必须先关掉 `do-timestamp`，那会改到 `sync=true` 的 kmssink
   调度语义，属于「为锦上添花冒破画出画的风险」，任务书明确要求不做。

**代价**：时间戳仍由 appsrc 按到达时间打，收包抖动会被当成帧间隔。这与改动前
行为**完全一致**，没有回归。**后续若要真做**：需要在构建时拿到 GStreamer 1.22 的
`gstbuffer.h`（交叉编译 sysroot 或 vendor 头），再加一个结构体布局断言
（`sizeof(GstBuffer)` / `offsetof(pts)` 与真机一致性测试），不要直接抄偏移。

---

## 3. hunk 3：分辨率变化时强制重新协商（已落地）

**位置**：`cm_gst.c:1321-1388`（`cm_box_set_caps_video`）

### 目的

`mppvideodec` 的 DPB / 输出 buffer 池是**按首个 SPS 分配**的。网关切档（或首帧前
我们解析到的 SPS 是错的）时 `width/height` 变了但下游没收到新 caps，解码器仍按旧
尺寸分配——表现是画面挤压/花屏，或干脆只出第一帧。

做法：向 appsrc 的 src pad 推一对 `flush-start` / `flush-stop`，h264parse 与解码器
收到 `flush-stop` 后会重新走一次 caps 协商 / 重分配 DPB。

### 三条不可违反的约束（都写进了代码注释）

1. **绝不能用 `gst_element_set_state(PAUSED→PLAYING)`。** 本函数在收包线程持着
   `s->gst_mu` 时被调用（`cm_rtp.c:339-341` 的 `note_sps`），`set_state` 会同步等待
   状态切换、而状态切换又要拿 `gst_mu` —— **直接死锁收包线程**。只许发 event。
2. **首次调用不 flush。** `old_w/old_h == 0` 时直接更新字段返回：那时下游根本还没
   协商过，发 flush 只会把刚起来的 pipeline 打回 NULL 附近，白丢首帧。
3. **`flush-start` 必须配 `flush-stop(reset_time=TRUE)`。** 否则 pipeline 永远停在
   flushing 状态，之后 push 的 buffer 全被丢掉，表现为 `video_ok=1` 但屏幕不动。

额外保护：只在 `state == PLAYING` 时 flush（READY/NULL 下下一次 PLAYING 自然就会
重新协商）。

### 引用计数

- `gst_element_get_static_pad()` 是 **transfer full**，用完 `gst_object_unref(pad)`。
- `gst_pad_push_event()` 是 **transfer full**（吃掉 event 引用），所以成功失败都
  **不要**再 `gst_event_unref(ev)`。代码里也没这么做。
- 刻意**不用** `gst_element_send_event()`：它对 event 的所有权语义（transfer-none，
  但元素实现里可能转交 sink pad 消费）容易踩双 free 或泄漏，边界不干净。

### 符号核实

桩库 `.dynsym` 逐项确认存在：`gst_event_new_flush_start`、`gst_event_new_flush_stop`、
`gst_event_unref`、`gst_pad_push_event`、`gst_element_get_static_pad`、
`gst_object_unref`。

### 风险

- 切档瞬间会有一小段 flush 空洞（几十毫秒），表现为**瞬时黑屏/停顿**。这是重新协商
  的正常代价。
- 若真机出现「切档后画面全黑且不恢复」，说明 `flush-stop` 没被下游正确消费或 caps
  协商失败，看日志里的 `video caps CHANGED` 与随后的 bus 错误。
- 回退方式：把 `cm_box_set_caps_video()` 恢复成只赋 `b->width/b->height` 两行。

---

## 4. 真机验证方法（adb 命令级）

### 4.0 前置

```bash
ADB=/d/Users/Daniel/code/DSH/x6pro/haas-x6pro/env/adb/adb.exe
$ADB devices          # 设备 9D91500007505171
```

日志取法（设备无 logcat，`console.warn/error` 落盘）：

```bash
$ADB shell 'ls -t /userdata/applog/DictPen_*.log | head -1'
$ADB shell 'grep -h "\[pencg\]" $(ls -t /userdata/applog/DictPen_*.log | head -1) | tail -80'
```

### 4.1 hunk 1 验证：解码器错误策略生效

**正常出画时应看到**（说明连一个错误都没有，最好的情况）：

```bash
$ADB shell 'grep -h "\[pencg\]" <log> | grep -E "mpp|decode|bus:|video bus" | tail -20'
```
→ **应当为空**。

**若网关/网络确实有坏帧**，应当看到（此前是完全静默的）：

```
[pencg] video bus: <mppvideodec 的错误原文> | debug=...
```

**失败信号（需要回退 hunk 1）**：

```
[pencg] video bus: ... (连续出现 ≥16 次)
[pencg] appsrc push flow=-5        # GST_FLOW_ERROR
```
→ 画面卡住 + 上述日志 = `max-errors=16` 触发。此时把 `ignore-error=false `
去掉（恢复默认 `true`）再验。

### 4.2 hunk 3 验证：分辨率变化重新协商

触发切档（在云游戏里改画质，或等网关自适应降档），然后：

```bash
$ADB shell 'grep -h "\[pencg\]" <log> | grep -E "SPS reports|video caps" | tail -20'
```

**期望输出（切档一次）**：

```
[pencg] #N SPS reports 1280x720 profile=100 level=31
[pencg] #N SPS reports 960x540 profile=100 level=31 (CHANGED)
[pencg] video caps CHANGED 1280x720 -> 960x540, flush to renegotiate
[pencg] #N SPS profile changed (100 -> 100, level 31 -> 31), request fresh IDR
```

**首帧时不应出现 flush**（这是约束 2 的验证点），只应看到：

```
[pencg] #N SPS reports 1280x720 profile=100 level=31
```
（`video caps ... no flush needed` 或什么都不打）

**失败信号**：

| 日志 | 含义 |
|---|---|
| `video caps CHANGED ...` 之后画面全黑不恢复 | `flush-stop` 没生效，回退 hunk 3 |
| `appsrc src pad not found, skip flush` | `gst_element_get_static_pad` 找不到 `src` pad（理论上不会发生，appsrc 必有 src pad） |
| `flush-start push failed` / `flush-stop push failed` | pad 已失效（pipeline 正在拆） |
| `video_ok=1` 但屏幕不动 | 典型的「停在 flushing 状态」，检查 flush-stop 是否漏发 |

### 4.3 hunk 2 验证：DELTA_UNIT 生效

这条**无法从我们自己的日志直接看到**（GStreamer 不打 buffer flags）。间接验证：

```bash
$ADB shell 'grep -h "\[pencg\]" <log> | grep -cE "appsrc push failed|flow=-5"'
```
→ 应为 `0`。若 DELTA_UNIT 取值写错成 LIVE，典型症状是**PAUSED 状态下画面被丢弃**
（比如 `setVideoVisible` 暂停/恢复时闪黑），以及 QoS 提前丢帧导致掉帧率。

**取值自检**（改了宏之后务必跑一遍）：

```bash
grep -n "define GST_BUFFER_FLAG_DELTA_UNIT" PenCG/native/csrc/cgmedia/cm_gst.c
# 必须是 (1u << 13)，不是 (1u << 4)
```

### 4.4 全量回归（三处一起）

```bash
# 1) 起播：能出画、无 bus 错误
$ADB shell 'grep -h "\[pencg\]" <log> | grep -E "first video frame pushed|gst_init done"'

# 2) KMS 平面真的在吃帧
$ADB shell 'cat /sys/kernel/debug/dri/0/summary 2>/dev/null | head -30'
#   期望：plane-id=76 那一行 ACTIVE，buf[0].addr != 0

# 3) 帧率不塌（bug5 修完后音频收尾多 1s，别和这里混）
$ADB shell 'grep -h "\[pencg\]" <log> | grep -oE "fps=[0-9.]+" | tail -10'

# 4) 退出后没有残留
$ADB shell 'pidof aplay; pidof gst-launch-1.0'
#   期望：都为空
```

---

## 5. 已知遗留 / 冲突风险

### 5.1 与 video-integrity 文档的两处偏差（需回写）

1. `docs/BUGFIX-bug3-bug4-video.md:482` 的 `GST_BUFFER_FLAG_DELTA_UNIT (1u << 4)`
   **是错的**，正确值 `1u << 13`。该文档那段建议整体不可直接采用。
2. 该文档 `:475-480` 建议 `extern GstClockTime gst_buffer_get_pts(void*)` 等四个
   **不存在的符号**，照抄会导致 `.so` 加载失败。该段应删除或标注为「需 vendor 头
   文件后才能做」。

### 5.2 与 video-integrity 的视频字符串改动

本次只改 `cm_gst.c:239` 一处字符串（加两个属性）。若 video-integrity 后续也要改
`video_pipeline_fmt()` 或 `video_pipeline_soft()`，合并时注意这一行的
`ignore-error=false max-errors=16 ` 不要被覆盖掉。

### 5.3 bus 错误不触发管线重建

hunk 1 让解码错误浮出水面，但**不会**自动重建管线：`b->failed` 只由 `appsrc push`
返回值置位，bus 上的 `GST_MSG_ERROR` 仅被 `cm_box_drain_bus()` 打印。若真机出现
「16 次连续错误后画面永久卡死」，需要在 `cm_box_drain_bus()` 里对 video 盒子的
ERROR 消息置 `b->failed = 1`，再由上层重建——**本次未做**（超出本单元写域，
且需与 `cm_media.c` 的重建策略配合）。

### 5.4 bug5 音频语义完整保留

本次改动集中在视频路径，音频部分一处未动。已核验以下符号/调用仍在：

```
audio_snapshot / cm_audio_route_restore / audio_route_ensure(s) /
aplay_stop / aplay_alive / audio_amp_read / struct cm_box 的 cm_sess *sess
```
（`grep -c` 合计 13 处命中，全部保留。）

---

## 6. 改动清单（file:line）

| 位置 | 内容 |
|---|---|
| `cm_gst.c:81-104` | 新增 `gst_buffer_get_flags` / `gst_buffer_set_flags` 声明、`GST_BUFFER_FLAG_DELTA_UNIT (1u<<13)`、`GstEvent` 系列与 `gst_pad_push_event` / `gst_element_get_static_pad` 声明 |
| `cm_gst.c:224-241` | hunk 1：`mppvideodec rotation=270 ignore-error=false max-errors=16` |
| `cm_gst.c:1206-1250` | hunk 2：非关键帧打 `DELTA_UNIT`（PTS 保留 `(void)` 并注明跳过理由） |
| `cm_gst.c:1321-1388` | hunk 3：`cm_box_set_caps_video()` 分辨率变化时 flush-start/flush-stop |

`git diff --stat`：`cm_gst.c | 405 insertions(+), 41 deletions(-)`（含 bug5 的既有改动，
本次新增约 150 行，其余为上一轮音频修复）。

**未改动**：`cm_rtp.c` / `cm_media.c` / `cm_session.h` / `cm_cmd.c` / `PenCG/src/**`，
未 git add / commit，未碰 adb 设备状态。

---

## 7. 定案（2026-11-13 晚）：`KMS_SINK_PROPS` 的 `sync=true` → `sync=false`

> **本节是本文件之后追加的定案记录**（作者 audio-route，追加人 docs-verify）。
> **§1–§6 保持原样作为历史记录，不修改。** 但读本文时必须知道一件事：
> **§1–§3 的三个 hunk 不是 bug3/bug4 的根因。** 真根因是下面这一行。
> 三个 hunk 作为健壮性加固保留，未回退，与本节结论不冲突。

### 7.1 一句话

`cm_gst.c` 里 `KMS_SINK_PROPS` 的 `sync=true` 让 kmssink 按 buffer PTS 调度，而
我们的 `appsrc` 打的是「到达时刻」的绝对单调时钟戳 —— 两者基准一旦错位，buffer 就被
判成「还没到显示时刻」或「已过期」而**整体丢弃**，KMS 平面绑着却**一口不翻页**。
改成 `sync=false` 后，两个游戏都由用户目视确认画面正常。

```c
/* PenCG/native/csrc/cgmedia/cm_gst.c:253-255 */
#define KMS_SINK_PROPS \
    " ! kmssink name=vsink plane-id=76 driver-name=rockchip " \
    "force-aspect-ratio=false can-scale=true sync=false"
```

`cm_gst.c:220-252` 是防止有人改回去的约 40 行长注释，第一行是：

```
 * ★ sync=false 是 bug3/bug4「终末地黑屏 / 画面卡住不动」的真正修复，别改回 true ★
```

### 7.2 四层定位（每层都有真机证据，所以结论收敛）

| 层 | 做法 | 结果 |
|---|---|---|
| ① 码流层 | 把 App 实际推给 `appsrc` 的字节 dump 成 `/userdisk/miniapp/data/au.h264`（615675 B / 300 帧），拉回本地用 ffmpeg 解 | 解出 **233 帧**；`corrupt decoded frame` 仅 3 次且**都在片头 I 帧**；`concealing` 11 行全是 I 帧错误掩盖；抽帧 frame 200 / 232 是**完整清晰的终末地画面**（灰度均值 182.9 / 184.1，非零像素 89%） |
| ② 硬解层 | `gst-launch-1.0 filesrc location=au.h264 ! h264parse ! mppvideodec ! videoconvert ! videoscale ! video/x-raw,format=GRAY8,width=64,height=64 ! filesink` | 输出 **954368 B = 233 × 4096**，与软解帧数**完全一致** |
| ③ plane76 硬件通道 | `gst-launch-1.0 videotestsrc pattern=smpte is-live=true ! video/x-raw,width=720,height=1280,framerate=30/1 ! videoconvert ! video/x-raw,format=NV12 ! kmssink plane-id=76 driver-name=rockchip force-aspect-ratio=false can-scale=true sync=false` | 全屏彩条，**用户肉眼确认可见** |
| ④ 同一管线换数据源 | `multifilesrc location=au.h264 loop=true ! h264parse ! mppvideodec rotation=270 ! videoconvert ! kmssink plane-id=76 driver-name=rockchip force-aspect-ratio=false can-scale=true sync=true render-rectangle="<107,244,266,473>"` | 用户肉眼确认「正中间画面是终末地，一开始全绿过一会变正常」 |

⇒ 管线、rotation、缩放、plane 绑定**全都对**，唯一变量是喂数据的元件
（`appsrc` vs `multifilesrc`）。所以问题只可能出在「appsrc 产生的时间戳」与
「sink 的调度方式」之间。

### 7.3 真机对照（同一个游戏，两次只差 `sync`）

| 配置 | 日志文件 | DRM 平面采样 | 用户原话 |
|---|---|---|---|
| `sync=true` | `/userdata/applog/DictPen_20261113_202233.log` | `plane[76] crtc=video_port0 fb=130`，**10 次 × 2 秒采样 fb 恒 130，一次不变**；同期 `AU push count=77`、probe `rx=6578 fps=13` | 「有画面但完全静止（卡住的一帧）」 |
| `sync=false` | `/userdata/applog/DictPen_20261113_202711.log` | `fb` **20 次 × 1 秒采样在 127/130 之间持续交替** | 「画面在动了！能正常玩」 |
| `sync=false`（原神回归） | `/userdata/applog/DictPen_20261113_203218.log`，`[play] boot code=yssy q=low gw=0`、`出画 1280x720 audio=0` | `fb` 12 次采样在 127/130 交替 | 「画面正常、在动」 |

注意 `sync=true` 那一组里 **`rx=6578` / `fps=13` / `AU push count=77` 全是正常的** ——
收包、解码、推帧都在动，只有 sink 不翻页。这正是为什么之前所有「在出帧」的间接判据
都显示健康、用户却看到静止画面。

**以后最快的回归指纹**：DRM `fb` 编号是否交替。`fb` 恒不变 = sink 不翻页 = 回归了。

### 7.4 与用户报的两个症状的对应

- **bug4「建立连接后画面就存在马赛克和绿屏」**：绿屏是**片头 IDR 到达之前**的解码器
  输出（DPB 空、无参考帧时的未初始化 NV12），IDR 一到就自愈 —— **这是正常现象，不是 bug**。
  真正「一直全黑 / 不动」才是 `sync=true`。
- **bug3「游玩画面经常性出现上一帧的马赛克残留」**：**同一个根因** —— sink 卡在某帧上
  不翻页，用户看到的就是上一帧残留。

### 7.5 本文 §1–§3 三个 hunk 的定位修正

| hunk | 原记录 | 定案后的定位 |
|---|---|---|
| 1 `ignore-error=false max-errors=16` | §1 记作修复 | **不是根因**，保留为加固（`ignore-error=true` 确实会把坏帧吐出去上屏，只是本案例里坏帧没那么多） |
| 2 `DELTA_UNIT` 标记 | §2 记作修复（降级版） | **不是根因**，保留为加固。`1u<<13` 的纠错仍然成立 |
| 2' PTS/DTS | §2.3 记作「跳过」 | **跳过是对的**。方向上「补准 PTS 让 `sync=true` 正确调度」与本定案**相反** —— 最终解是取消 sink 的时钟依赖 |
| 3 分辨率变化 flush 重协商 | §3 记作修复 | **不是根因**，保留为加固（同管线的 multifilesrc 测试在固定分辨率下也复现） |

### 7.6 代价与例外

`sync=false` 让 kmssink **不再按 PTS 节流**，而是「有空就显示最新一帧」。对本机场景
（云游戏实时流、拒绝累积延迟、720p 单路）这是**语义正确**的选择；丢帧由 RTP 层的
IDR / NACK 逻辑负责，不依赖 sink 的时钟。

**例外**：将来若要接**本地文件播放**（需要严格按时间轴出帧），应当**另建一条
`sync=true` 的管线**，**不要**把 `KMS_SINK_PROPS` 改回去 —— 那会把 bug3/bug4 重新引入。
这条约束已写进 `cm_gst.c:249-252` 的注释。

### 7.7 被推翻的旧推断（别照着排查）

| 曾经的推断 | 现状 |
|---|---|
| 「`IDR without SPSPPS` / 参数集缺失导致绿屏」 | ❌ 推翻。dump 的码流 ffmpeg 能正常解出 233 帧完整画面 |
| 「码流层损坏（SRTP 认证失败、AU 缺片）导致马赛克」 | ❌ 不是本次根因。`corrupt decoded frame` 只有片头 I 帧 3 次 |
| 「`mppvideodec ignore-error=true` 把坏帧吐出去上屏」 | ❌ 不是本次根因。硬解帧数与软解完全一致 |
| 「DPB / 输出 buffer 池按首个 SPS 分配，分辨率切换不重协商」 | ❌ 不是本次根因。multifilesrc 同管线同分辨率也复现 |
| 「补准 buffer PTS 让 `sync=true` 正确调度」 | ⚠️ 方向相反。正确做法是取消 sink 的时钟依赖 |

### 7.8 本次未改的过时注释

`cm_gst.c:1355` 仍写着「那会动到 `sync=true` 的 kmssink 调度」—— `sync` 已改 `false`，
这条注释**已过时**。属源码注释，本次只改 docs，未动；留给下次改代码时顺手修。
