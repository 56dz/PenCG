# 明日方舟：终末地（mrfzzmd）画面全黑 / 卡死 —— 定位与修复

> 现象（用户报）：只有「明日方舟：终末地」进游戏后**画面全黑但声音正常**；
> 原神、卡拉比丘、鸣潮、崩坏星穹铁道、明日方舟都正常。
> 真机复核（2026-11-14）确认还包含第二种形态：**画面先正常、几十秒后冻结在某一帧**。

## 一、结论（一句话）

RK3562 的硬件解码器（`mpp_rkvdec2`）在这条码流上解错，被内核**强制复位**；
复位丢掉全部参考帧，而终末地一局只给 1 个 IDR、网关对 NACK 又从不重传，
解码器再也回不到可解码状态 —— 于是「全黑」或「冻结」，音频链路完全不受影响。

## 二、决定性证据

### 2.1 内核日志：VPU 报错并复位

```text
mpp_rkvdec2 ff340100.rkvdec: session 32 task 199652 timeout 0 abort 0 force_dequeue 1
mpp_rkvdec2 ff340100.rkvdec: resetting...
mpp_rkvdec2 ff340100.rkvdec: reset done
mpp_rkvdec2 ff340100.rkvdec: session 32 task 199652 irq_status 0x001123 timeout 0 abort 0
```

- 采样口径：`dmesg | grep -c "reset done"`；`dmesg | grep -o "irq_status 0x[0-9a-f]*" | sort | uniq -c`
  → 同一轮里 **21 次全部是 `irq_status 0x001123`**。
- 时间定位：`uptime` 与本地时间对齐后，这 21 次 reset 全部落在 **唯一那次全黑会话**的时间窗内
  （`uptime 30991~31102`），之后连续 4 次健康局增量均为 **0**。
- 对照：黑屏会话里 reset 计数持续上涨（36 → 53 → 75），健康会话里不涨。

### 2.2 码流本身是好的

`dumpau.txt` 抓到的 `au.h264`（2546832B）用 ffmpeg 解 **零 corrupt / 零 error**，
`ffprobe` 报 `h264 / Constrained Baseline / 1280x720`。
⇒ 问题不在 RTP 组装、不在参数集注入，而在「把这些合法字节交给硬解」这一步。

### 2.3 不是我们没喂

在 `cm_rtp.c` 里加了逐帧 AU 指纹（`/userdisk/miniapp/data/auld.txt` 开关，见 §四），
冻结期间日志持续输出：

```text
#4 AULD t=... n=10312 key=0 h=37df8306...
#4 AULD t=... n=11064 key=0 h=5311705d...
```

**每帧哈希都不同** ⇒ 我们一直在推内容各异的新帧；同刻 `plane[76]` 抓帧的 md5 连续 12 次完全相同
⇒ 冻结发生在**解码输出侧**，不是收包侧、不是提交侧。

### 2.4 已排除的方向

| 假设 | 结论 |
|---|---|
| `kmssink sync=true`（bug4 的老根因） | 不成立：`fb` 在 127/130 之间交替，sink 在翻页 |
| UI 面遮挡视频面 | 不成立：`modetest -p` 实测 plane54 `zpos=0`、plane76 `zpos=2` |
| KMS 平面未绑定 | 不成立：健康局 `plane[76] crtc=video_port0`，坏局也不是「绑了却翻页」 |
| SPS 中途变化导致重配失败 | 不成立：两版 SPS 都解出 1280x720/Baseline/level31/ref=2，只差 constraint_set |
| NACK 没发出去 | 不成立：`NACK sent (30 bytes, srtcp)` 有，`srtcp protect failed` 为 0；但网关**从不重传**（`recov=0`） |
| 僵尸 MPP 会话堆积 | 不成立：`/proc/mpp_service/sessions-summary` 只有 1 个 session |
| UDP 缓冲溢出 | 不成立：`RcvbufErrors` 开机至今不变 |
| 帧率/性能 | 不成立：健康局 `pushed == shown`，RSS 稳定无泄漏 |

## 三、修复（已落地）

### 3.1 native：真实上屏计数 + 上屏看门狗（`cm_gst.c` / `cm_media.c` / `cm_rtp.c` / `cm_session.h`）

1. **`cm_box.shown`**：在 `kmssink` 的 sink pad 上挂 `GST_PAD_PROBE_TYPE_BUFFER` 探针计数。
   `pushed` 只证明「我们推给 appsrc 了」，`shown` 才是「真的有 buffer 上屏」——
   两者的差值就是「下游停止消费」的可观测判据。
2. **上屏看门狗**（`cm_media_loop`）：`pushed` 涨而 `shown` 不动持续 `VWD_STALL_MS`(2.5s)
   ⇒ 判定下游卡死并重建解码器管线（先 `cm_box_destroy` 再 `cm_box_create`，
   否则新 kmssink 抢不到 plane 76），重建后 `cm_rtp_after_rebuild()` 关闸、清半截 AU、补 PLI。
3. **重建退避**：2.5s → 5s → 10s（`s_vwd_wait_ms` 翻倍，上限 8s）。
   早期版本是 2.5s 硬间隔连打三次，8 秒就把额度烧光，等于没给网关任何反应时间。
4. **重建额度用尽** ⇒ `cm_emit` 发 `{"t":"error","msg":"video sink frozen","code":-7}`。
   只报错不拆会话 —— 用户此刻还能听见声音，拆了更糟。
5. **`cm_rtp_after_rebuild()`**：重置 RTP 重排/闸门状态并补 PLI。
   **刻意不清 SPS/PPS 缓存**：终末地的 IDR 常常不带参数集，新解码器要靠缓存注入。
6. `emit_stat` 增加 `"shown"` 字段；每 10 秒额外打一行
   `video sink pushed=N shown=M`（超差打 `(DOWNSTREAM STALLED)`），
   因为 `cm_emit` 的 stat JSON 不进日志文件，不打这行真机上看不见。

### 3.2 JS：`-7` 触发自愈（`play.vue`）

- `onMedia` 的 `t === 'error'` 分支原先只处理网关错误（`ev.data.errcode`），
  native 的 `{"t":"error","msg":"video sink frozen","code":-7}` 会被当成
  「服务端错误：video sink frozen」显示红字、且不重连。现在：
  - `code === -7` 且无 `data` 包装 ⇒ 第 1 次走 `stallHealL2`（复用网关重连），
    第 2 次起走 `stallHealL3`（重新申请机位换机器 —— 新机器的流是一定带 IDR 的新流）。
  - `code === -4`（管线重建失败）同样走 L2。
- 计数 `nativeFrozen` 只在 `stat.shown > 0`（真有 buffer 上屏）时清零：
  `出画` 事件在纯黑局也会触发，不能当判据。

## 四、诊断开关（都留在仓库，真机可用）

| 开关文件（`/userdisk/miniapp/data/`） | 作用 |
|---|---|
| `auld.txt` | 每帧打印 AU 的 FNV-1a 64 位指纹（区分「码流在变」与「码流静止」） |
| `dumpau.txt` | 从关键帧起 dump 300 帧到 `au.h264` |
| `gstdebug.txt` | 内容如 `kmssink:7` / `mppvideodec:5`，建管线时读一次，30 分钟自动失效 |
| `softdec.txt` | 强制 `avdec_h264` 软解分支 |

抓视频平面（**这是本轮最重要的新手段**）：

```sh
ffmpeg -f kmsgrab -plane_id 76 -framerate 5 -i - \
       -vf hwdownload,format=nv12 -frames:v 1 -pix_fmt rgb24 /tmp/p.png -y
```

- `plane_id 54` 是 weston UI 面（要 `format=bgr0`），`76` 是视频面；
- `-crtc_id 68` 只拿到 CRTC 自己的 fb（=UI 面），**不是合成结果**；
- 回传主机：`adb shell "base64 /tmp/p.png" | tr -d '\r\n' | base64 -d > p.png`
  （`adb pull` 在 MSYS 下会被路径转换搞坏）。

## 五、仍未解决 / 下一步

1. **为什么同一款游戏有时一帧都不上屏、有时完全正常**：观测到黑屏局的第一个 IDR 只有
   **5409 字节**、健康局是 **44440 字节**，且黑屏局 `rx` 极低（22 秒 52 个包）。
   怀疑网关在「几乎静止的画面」上给出了退化码流，触发 VPU 的 `irq_status 0x001123`。
   还需要在同一局里同时抓 AU dump + dmesg 时间戳来确认因果方向。
2. **VPU 复位后如何救回**：目前只能靠 JS 换会话（L2/L3）。若能拿到
   `RtcKeepPlaying`（官方 SDK `CmdId=20015`）就不必换机位，但那需要
   `paasDispatch` 的 `appKey`，见 `docs/HANDOFF-163CG-API.md`。
3. **软解兜底不可用**：`avdec_h264 ! videoflip` 分支真机报
   `gst_parse_launch warning` 后 `video box marked failed after decoder error`，
   该通道目前是坏的（缺 element 或 caps 不匹配），需要单独修。
