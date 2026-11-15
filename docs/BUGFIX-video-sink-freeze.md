# 游玩中「卡一下 → 闪黑 → 重连」—— 定位与修复

> 现象（用户报）：
> 1. m16811「我现在正在游玩，出现操作时经常性卡顿，但是静止无操作就不卡」
> 2. m16813「不对，现在画面没了」
> 3. m16929「我怀疑是网络问题，因为刚才卡住一秒钟，画面突然闪黑之后重连就恢复」

## 一、结论（一句话）

**不是网络问题，也不是用户操作引起的。** 真正的因果链是：

1. 网关每隔一段时间**单方面停止推送 RTP**（音视频同时停），WS 控制面照常；
2. native 上屏看门狗（VWD）判定「下游停止消费」，**自己拆掉 kmssink 管线重建**；
3. 而这一局原神的 IDR 是**周期性的、间隔精确 604 秒**，网关又不响应 PLI、
   不重传 NACK ⇒ 新解码器拿不到 IDR，参考帧全空，**只能等下一个 604 秒周期**
   ⇒ 用户看到「卡住」「闪黑」「黑很久」；
4. 最后 JS 侧的 L2 换会话（5 秒）才真的恢复 —— 所以主观上像「闪黑之后重连就恢复」。

修复的核心不是"让网关别停推"（那在服务端），而是**别再拆管线**：
先发 PLI 求一次 IDR，宽限 4 秒，仍然不出画就直接报 `-7` 交 JS 换会话
（换会话 5 秒、进度不丢），绝不自己重建解码器。

## 二、决定性证据

### 2.1 不是网络：冻结期间心跳 RTT 毫无劣化

从日志里提取网关回显的 keepAlive 心跳（`<- op=input ... 0 <13 位毫秒戳>`），
用「日志本地时间 − 回显戳」算往返延迟：

| 时段 | RTT 中位数 |
| --- | --- |
| 冻结前（07:48:30–07:49:20） | 126 ms |
| **冻结中（07:49:22–07:49:35）** | **149 ms** |
| 冻结前（07:53:00–07:53:40） | 186 ms |
| **冻结中（07:53:44–07:53:57）** | **135 ms** |

全程 min/med/max = 65 / 176 / 258 ms。链路没有任何劣化。
`RcvbufErrors=0`、CPU 2~4.5%、`cm_udp_open()` 从不设 `SO_RCVBUF`。

### 2.2 不是用户操作：整局没有一次真实触摸

113 条 `<- op=input` **严格每 10 秒一条**，全部是心跳回显
（`input cmd 忽略: 0 <ts>`）。cmd 分布 `{'0':95, '8':4, '23':6}`，
其中 `8`（tap）与 `23`（拉起包名）**全部集中在会话刚建立时**，
是重连后网关补发的初始化指令。

### 2.3 是网关停推：音视频同时停，WS 照常

冻结窗口内：

- `rx`（`stats.rtp_rx`）**瞬间停止增长**（不是缓慢衰减）；
- `fps` 同步归零；
- `pt histogram` 里 `98`(H264) 与 `111`(opus) **同时**停止增长；
- `ws=open` 全程，**无 `op=close` / `op=error` / ping 超时**；
- 丢包极少：`lost=1 auDrop=1 pDrop=1 nack=3 recov=0`。

### 2.4 PLI 无效、NACK 不重传、IDR 周期 604 秒

- 整局发出 **506 条 PLI**（约每 2 秒一条，健康期与冻结期完全一致，
  来自 `cm_kf_tick` 的 `next_pli_ms = now + 2000`），网关**零响应**。
- `recov=0` 全程 ⇒ NACK 也不重传。
- IDR 间隔精确 604 秒（`kf` 计数变化时刻）：

```text
kf=1 08:02:49   kf=2 08:13:00   kf=3 08:23:04   kf=4 08:33:08
```

`AU push key=1` 全份只有 8 条 = 每个会话起播 1 条 + 上述 3 条。

### 2.5 三个 native 缺陷（`cm_media.c`）

**bugA 重建额度永不清零。** `s_vwd_rebuilds` 只在换会话
（`s_vwd_sid != s->id`）时归零，**画面恢复的分支不清**。于是
08:12:40 用掉的 2 次额度到 08:38 还在，一次小抖动就直接撞
`VWD_MAX_REBUILDS` 把 `-7` 抛给 JS。

**bugB `VWD_STALL_MS 2500` 太激进。** 08:12:40 那次
`frozen 2560 ms (shown=17352 pushed=17447)` 的 `pushed-shown` 只有 95，
比健康水位还小，纯属 2.5 秒抖动，却触发拆管线；拆完必须等新 IDR，
而 IDR 间隔 604 秒 ⇒ 黑屏 20 秒。

**bugC `(DOWNSTREAM STALLED)` 每 10 秒误报。** 判据是
`pushed > shown + 60`，而健康会话里 `pushed-shown` 长期稳定在 **142~157**
（probe 采样时刻与 kmssink 取帧之间的正常管线延迟）。于是整局都在喊停摆，
把真冻结淹掉。

## 三、修复

### 3.1 `native/csrc/cgmedia/cm_media.c`

```c
#define VWD_CHECK_MS        500    /* 检查粒度 */
#define VWD_STALL_MS        6000   /* pushed 涨而 shown 不动的容忍时长 */
#define VWD_PLI_GRACE_MS    4000   /* 先求 IDR 的宽限期，过了才交 JS */
#define VWD_BACKLOG_SLACK   60     /* 超过正常管线延迟多少帧才算下游停摆 */
static uint64_t s_sink_backlog_avg = 0;   /* pushed-shown 正常水位（EMA） */
```

VWD 改为三级，**彻底删除 `cm_box_destroy` + `cm_box_create` 的重建分支**：

| 级 | 触发 | 动作 |
| --- | --- | --- |
| 0 | `shown` 增长 | 更新 backlog EMA，清 `pli_at` / `gave_up`，`wait_ms` 回到 6s |
| 1 | `pushed` 涨、`shown` 不动满 6s | 打 `video sink stalled ... -> request IDR (PLI), give it 4000 ms`，`cm_send_pli(s)`，`wait_ms += 4s` |
| 2 | 再满 4s 仍不动 | 打 `still frozen ... asking JS to reconnect instead of rebuilding`，`cm_emit({"t":"error","msg":"video sink frozen","code":-7})`（只报一次） |

统计行的 `(DOWNSTREAM STALLED)` 改用 EMA 水位：
`pushed > shown + s_sink_backlog_avg + VWD_BACKLOG_SLACK`。

### 3.2 `src/pages/play/play.vue`

`STALL_L1_WAIT_MS` 3000 → **2000**。依据：跨 5 份真机日志
`第 1 级自愈成功` 出现 **0 次** —— 网关不看 PLI，这两秒等的是已知不会来的回应。
保留 L1 本身（只花一次 UDP 发送）。

## 四、真机验证

- 装机校验：`strings` 查新标记串（不能看 md5/体积，loader 会重命名 .so 并追加 ~118 KB）：

```text
give it %d ms before                              1
asking JS to reconnect instead of rebuilding      1
still frozen %llu ms after PLI                    1
request IDR first                                 0   ← 旧串
rebuild decoder                                   0   ← 旧串
```

- 重启后 `grep -o 'libjsapi_cg[a-z]*_[0-9]*\.so' /proc/$(pidof miniapp)/maps` 确认加载的是新 .so。
- 用户实测游玩 2 分钟：**无闪黑、无卡顿、无换会话**（m17628「我玩了两分钟，没有问题了」）。

> 注意：本轮设备在验证后已离线（`adb` 报 device not found），
> 更长时间的浸泡（跨过一次 604 秒周期 IDR）留待下一轮。

## 五、遗留

1. 网关为什么停推、以及为什么换个机位能长活 30 分钟（`.177` vs `.140/.141`）仍未解释，
   根因在服务端（见 `docs/HANDOFF-163CG-API.md`）。
2. 停推后的冻结画面仍会持续约 10 秒（6 + 4），这 10 秒只能等，不能缩短到误判。
3. 若网关将来开始响应 PLI，第 1 级就能直接恢复，无需换会话。
