# bug6：对局健康时页面被意外销毁，KMS 平面解绑后整屏全黑

> 排查域：`PenCG/src/pages/play/play.vue`、`PenCG/src/services/media.js`
> native 侧（`PenCG/native/**`）**本节成文时未改动**，patch 建议在 §8。
>
> ★ **2026-11-13 22:19 状态更新：§8 的两条 native patch 已由 Lead 实施并真机验证通过。**
> - 8.1 `KMS_SINK_PROPS` 加 `restore-crtc=false`（`PenCG/native/csrc/cgmedia/cm_gst.c:255`）
> - 8.3 `js_start()` 在 `sess_alloc()` 前先收起表里所有视频会话（`sess_kill_all_video()`，
>   `PenCG/native/csrc/cgmedia/cgmedia.c`）
>
> 真机 A/B（设备 9D91500007505171，原神 yssy）：起播健康时 `plane_mask=3`；
> 再执行一次 `miniapp_cli start ... index`（就是本 bug 的触发动作）后，
> **修复前 `plane_mask` 掉 0、整屏黑；修复后 `plane_mask` 稳定为 1**（只剩 weston 的
> `plane[54]`，fb 在 126/128 持续翻页），`miniapp_cli capture` 截到完整正常的首页 UI。
> 下面 §0–§7 保留成文时的原始判断（含「没有修好」的自评），作为排查过程记录。

---

## 0. 结论速览

| 问题 | 结论 | 证据强度 |
| --- | --- | --- |
| 重复 `miniapp_cli start` 是宿主合法行为吗？ | **是**。它是调试/守护/误触都可能触发的常态路径，我们无法禁止，只能假设它会发生 | 强（Lead 实测两次 start → 页面被销毁） |
| 页面被销毁时平面为什么会掉到 `plane_mask=0` | **根因在 native/kmssink 的收尾语义**：`cm_box_destroy()` 只做 `set_state(NULL)`，不显式恢复任何 plane；kmssink 的 `restore-crtc` 只保证「恢复 CRTC mode」，不保证「把 weston 的 plane 重新挂回 crtc」 | 中（代码 + `gst-inspect` 原文 + DRI 现象三者自洽，但**缺拆除瞬间的 DRM trace**） |
| JS 侧能防住吗 | **防不住**。JS 无法让 weston 重新 bind 自己的平面；能做的只有「拆除前先把我们自己的平面缩到 1x1」和「别让旧 native 会话泄漏」——两件都做了 | 强（改动可审计） |
| 重复 `navTo('play')` 会 push 两个页面吗 | **判断不了**。观测只支持「第二次 start 重载了 index，连带销毁 play」；两个 play 是否曾同时存在**没有直接证据**，需真机验证（§7 给出方法） | 弱 |
| 诊断日志是否值得加 | **值得，已加**。销毁时刻的归因一行日志，成本极低 | — |

**不自欺的结论**：本次改动**没有**让「对局中途页面被销毁」不再发生；它让下一次发生时，日志能一眼指出「这次销毁没有任何我们自己发起的退出路径走过 = 宿主干的」，并且让泄漏的 native 会话/平面窗口变短。

---

## 1. 现场（Lead，2026-11-13 21:39，干净重启后受控实验）

只启动 App、不跑 gst-launch/kmsgrab：

- t=0–32s：`crtc[68] plane_mask=1`，`plane[54]`（weston UI）`fb=126/128` 正常翻页。
- **t=34s：`plane_mask=0`，两个平面全解绑，整屏全黑。**

日志时间线（`/userdata/applog/DictPen_20261113_211235.log`，该文件已被滚动删除，只能用 Lead 的摘录）：

```
21:39:05.491 [pencg][I] [play] 出画 1280x720 audio=0
21:39:20.471 [pencg][I] [play] 出画 1280x720 audio=1
21:39:24.592 [pencg][W] [play] probe stage=live v=1 screen=0(ON) phase=playing ws=open rx=950 fps=14 kf=12 gateDrop=0 auDrop=0 kfReq=0 silent=0s frozen=0s tries=0 keep=on
21:39:25.923 [pencg][I] [play] cleanup（beforeDestroy）        ← 对局健康时被销毁
21:39:25.924 [im] 放弃会话(page-unload) hadPending=0 保持面板不动
21:39:25.324 [pencg][W] [screen] 保活停止 ticks=3（定时器已清，后台不再保活）
21:39:27.677 [pencg][I] [play] 记账 41s
21:39:27     [cgmedia] video pipeline -> NULL (set=1, state=1, pending=0)
             [cgmedia] box destroyed
             [cgmedia] #1 media loop exit
```

关键否定证据：**没有** `[nav] closePageByName play`、**没有** onHide/onStop、**没有**任何 error。页面自己 `beforeDestroy` 了。

对比实验：改成「`kill -9 miniapp` → sleep 6 → 只 start 一次」后同样配置跑 177 秒，`plane_mask=3` 稳定、`plane[76] crtc=video_port0 fb=130` 持续交替、无任何 beforeDestroy、用户目视确认完全正常。

→ 触发因子是**第二次 `miniapp_cli start`**（两次相差 4 秒）。

---

## 2. Q1：这是不是我们该管的事？

### 2.1 清理链本身是对的

`PenCG/src/pages/play/play.vue:1905` `cleanup()` → `stopPlay()`（`PenCG/src/services/media.js:591`）→ `__cgMedia.stop(mediaId)` → native `js_stop`（`PenCG/native/csrc/cgmedia/cgmedia.c:306`）→ `cm_box_destroy(vbox)`（`PenCG/native/csrc/cgmedia/cm_gst.c:1209`）→ `set_state(NULL)` → kmssink 解绑 plane 76 → `box destroyed`。

**这条链没有任何一步是错的**（它甚至同步等了 1.5s 让 NULL 落地）。问题不在「没清理」，而在「清理完之后系统回不到原状」。

### 2.2 kmssink 侧：`restore-crtc` 的语义被误解了

`.tmp/e2e/kmssink.txt:126`（`gst-inspect-1.0 kmssink` 原文）：

```
restore-crtc : When enabled and CRTC was set with a new mode, previous CRTC
               mode will be restored when going to NULL state.
               flags: readable, writable
               Boolean. Default: true
```

它保证的是 **CRTC mode**，也就是 `drmModeSetCrtc(crtc, fb, ...)` 那一层。而 `plane_mask` 是 **plane 层**的状态：weston 的 `plane[54]`、我们的 `plane[76]` 各自 bind/unbind。

`PenCG/native/csrc/cgmedia/cm_gst.c:253-255` 的管线串**没有显式设置 `restore-crtc`**，于是走默认 true —— 也就是 kmssink 在 NULL 时会自己去动 CRTC。如果它在收尾时把 CRTC 恢复成一个「没有 fb」的状态（`drmModeSetCrtc(crtc, 0, ...)`），内核会把这个 CRTC 上**所有** plane 的 mask 清掉 —— 正好就是观测到的 `plane_mask=0`。

两条竞争假设（**必须真机验证，见 §9**）：

- **H1：kmssink 的 CRTC 恢复路径写入了 fb_id=0**，一次性清掉 plane_mask，weston 之后不会主动重挂（它认为自己的 plane 状态没变，不需要重新 commit）→ 整屏全黑且无法自愈。
- **H2：weston 的 plane 状态被外部改动后 weston 自己放弃了该输出**（`vop2_crtc_atomic_disable` 类路径），需要重启合成器才能恢复。

两者都指向同一个结论：**问题出在 kmssink 收尾对 CRTC/plane 的副作用，而不是我们的 JS 清理顺序**。

### 2.3 所以这该谁管

- **不该由 JS 兜底**：JS 没有 DRM 能力，`setVideoVisible(false)` 只能改我们自己那条 plane 的 `render-rectangle`。
- **该由 native 修**：在 `cm_box_destroy()` 的视频分支里，要么让 kmssink 别碰 CRTC（`restore-crtc=false`），要么在 NULL 之后显式把 weston 的 plane 重新挂回去（本工程没有 DRM 头文件，此路不通）。
- **重复 start 本身**是宿主行为，我们既不能禁止，也（按目前的证据）无法在自己被销毁时阻止它。

精确 patch 建议见 §8。

---

## 3. Q2：JS 侧能做什么防护（已实现）

### 3.1 `cleanup()` 里「先缩 1x1 再拆管线」

`PenCG/src/pages/play/play.vue:1959-1961`：

```js
try {
  setVideoVisible(false);
} catch (e) {}
try {
  var p = stopPlay();
  ...
```

理由：

- native 侧 `setVideoVisible(false)` 走 `cm_cmd.c:585-594`，动作是 `cm_box_set_rect(s->vbox, 0, 0, 1, 1)` + `s->video_hidden = 1`，**这条路径 native 已支持且真机验证过**（弹系统键盘时就用它）。
- 它不能让 weston 复活，但能保证「即使我们卡在销毁过程中，平面上也只有 1x1 的残影而不是整屏黑的视频帧盖住 UI」，并且让 `set_state(NULL)` 之前 kmssink 的 CRTC 状态更「轻」。
- 失败不阻塞：`try/catch` 吞掉，后面照常 `stopPlay()`。

### 3.2 `startPlay()` 不再泄漏上一轮 native 会话（真 bug，已修）

`PenCG/src/services/media.js:436-452`（改动前只有 `S.mediaId = 0;` 一行）：

```js
const oldMediaId = S.mediaId;
const oldWsId = S.wsId;
if (oldMediaId || oldWsId) {
  logWarn('[media] startPlay 前清理上一轮会话 mediaId=' + oldMediaId + ' wsId=' + oldWsId);
  try {
    clearKfBurst();
    if (oldMediaId && globalThis.__cgMedia) globalThis.__cgMedia.stop(oldMediaId);
    if (oldWsId && globalThis.__cgNet) globalThis.__cgNet.wsClose(oldWsId);
  } catch (e) { ... }
}
S.mediaId = 0;
S.wsId = 0;
```

为什么这是真 bug：旧代码把 `S.mediaId` 抹成 0 之后，那条 native 会话就**再也没有人 stop 它、也没有人给它 `js_poll` 心跳** → 它的 vbox 继续挂在 `plane-id=76` 上、继续收 RTP，直到 `cm_media.c` 的 180s 看门狗才发现「JS 心跳丢了」把它拆掉。这 3 分钟里用户看到的是「界面卡住 / 黑屏但没退出」。

触发面：重连（`onReconnect` 已经先 `stopPlay()`，不受影响）、**快速重进对局**、以及任何「上一轮会话没走 `stopPlay()` 就重新 `startPlay()`」的路径。

### 3.3 「两个页面抢同一个 vbox」——不会发生在 JS 变量层，但会发生在 native 层

- Falcon 每个页面是**独立的 QuickJS 上下文**（`PenCG/HANDOFF.md:220-222` 已验证）。所以两个 play 页各有自己的 `S`（`PenCG/src/services/media.js:8-27` 模块级单例），**不会互相覆盖 `S.mediaId`**。
- 但也正因为互相看不见，**两个页面可以各自向 native `start` 一个会话，两个 vbox 都要求 `plane-id=76`** —— 这才是真正危险的：第二个 kmssink 抢同一个 plane 时行为未定义（大概率是后来者胜、前者静默失效）。
- JS 侧无法解决（页面 A 看不到页面 B 的 mediaId）。**只有 native 能解**：在 `js_start` 里检测「已有会话持有视频平面」时拒绝或先拆旧会话。这条也写进 §8。

### 3.4 快速 `stopPlay()` → `startPlay()` 竞态

- `stopPlay()`（`PenCG/src/services/media.js:591-617`）**前三步全同步**：`clearKfBurst()` → `clearInterval(pollTimer)` → `__cgMedia.stop(mediaId)` → `__cgNet.wsClose(wsId)` → `phase='closed'`。所以「先 stop 再 start」这个顺序本身是安全的。
- 唯一的残留风险：`js_stop`（`cgmedia.c:306`）内部 `pthread_join(rx_thread)`，rx 线程可能正在 `cm_udp_pump_media(..., 20, ...)` 里阻塞最多 20ms。这意味着 `stopPlay()` 的同步段最坏会让 JS 线程卡 ~20ms。可接受，但要知道。
- `clearKfBurst()` 放在 `stopPlay()` 第一行**够用**（它清的是「几百毫秒后对 `S.mediaId` 发 cmd」的定时器），`startPlay()` 里新加的清理也调了一次，双保险。

---

## 4. Q3：幂等性

### 4.1 `index.vue` 的 `boot()` 不幂等（只读结论，**未改**，属于别人的写域）

`PenCG/src/pages/index/index.vue:174-177`：`mounted() { this.boot(); this.startSeqWatch(); }`；`onShow()`（`:187`）**不**再调 `boot()`。

也就是说：

- 如果宿主「重复 start」是**复用**已有 index 页 → `boot()` 不会重跑 → 不会重复 autostart。**安全。**
- 如果宿主「重复 start」是**重建** index 页（真机现象更像这种）→ `mounted` 再跑一次 → `boot()` 再跑一次 → `devAutostart()` 再跑一次 → **第二次 `navTo('play')`**。

`devAutostart()`（`PenCG/src/pages/index/index.vue:423-443`）**没有任何「已经跳过一次就不再跳」的守卫**：

```js
devAutostart() {
  const d = DEV_AUTOSTART;
  if (!d || !d.page) return;
  ...
  setTimeout(function () { navTo(d.page, d.params || {}); }, ...);
}
```

而 `PenCG/src/dev-autostart.js` **当前不是 null**（`DEV_AUTOSTART = { page:'play', delay:4000, params:{ code:'mrfzzmd', ... } }`），装机的 `.amr` 里也打进去了。→ **Lead 那次实验的环境里，两次 start = 两次 navTo('play')，这本身就能解释「play 页被销毁」。**

建议（给 Lead / index 页负责人，**本次未实施**）：给 `devAutostart` 加一次性标记，落 `$falcon` 或模块级：

```js
if (this._autoStarted) return;
this._autoStarted = true;
```

生产包里 `DEV_AUTOSTART` 为 null 时本来就 return，所以这条只影响取证环境。

### 4.2 重复 `navTo('play')` 时 Falcon 的实际行为 —— **判断不了**

证据缺口：

- 现象支持「push 新页 → 旧页被销毁」：`navTo` 在本固件是 push 语义（`navBack` 不存在，见 `PenCG/src/services/nav.js:121-131` 的注释），且观测到「无 nav 日志的 beforeDestroy」。
- **但没有直接证据**证明「两个 play 页曾经同时存在」，也没有证据证明宿主是「重载 index」还是「直接销毁栈上的 play」。

需要真机验证（§9 V3 给出命令）。在拿到结论前，不要在代码里假设哪种行为。

---

## 5. Q4：销毁归因日志（已实现）

`PenCG/src/pages/play/play.vue:1915-1929`：

```js
this.destroyedAt = Date.now();
var _st = this.stage;
var _ms = mediaState();
var _played = this.startAt ? Math.round((this.destroyedAt - this.startAt) / 1000) : 0;
var _imPend = this.im && this.im.pending ? 1 : 0;
var _by = this.selfClosedBy || 'none(外部销毁)';
logWarn('[play] cleanup（beforeDestroy）by=' + _by + ' stage=' + _st +
  ' phase=' + (_ms && _ms.phase) + ' ws=' + (_ms && _ms.wsState) +
  ' played=' + _played + 's viz=' + ... + ' hidden=' + ... +
  ' imBusy=' + ... + ' kb=' + ... + ' imPend=' + _imPend +
  ' busy=' + ... + ' videoHidden=' + ...);
```

归因字段 `selfClosedBy`（`PenCG/src/pages/play/play.vue:364`）在这些「我们自己发起的退出路径」里置位：

| 置位处 | 值 | 行号 |
| --- | --- | --- |
| `applySeat` 进排队 | `'queued(navTo queue)'` | `:787` |
| `finish` | `'finish'` | `:1175` |
| `fail` | `'fail'` | `:1198` |
| `doExit`（未登录分支） | `'doExit(nologin)'` | `:1238` |
| `doExit` | `'doExit'` | `:1245` |
| `bgTeardown` | `'bgTeardown'` | `:1887` |

判读规则（一目了然）：

- `by=doExit / finish / fail / bgTeardown / queued(...)` → **我们自己退的**，链路正常。
- `by=none(外部销毁)` → 没有任何退出路径走过 ⇒ **被宿主摘掉/重载了**。这才是事故告警信号。

顺带把原来的 `logInfo('[play] cleanup（beforeDestroy）')` 从「信息级」提到「告警级」并加全字段：页面被外部销毁**本身就是异常**，不该藏在 `[I]` 里。

---

## 6. 改动清单（file:line）

| 文件 | 位置 | 改动 |
| --- | --- | --- |
| `PenCG/src/pages/play/play.vue` | `:364-365` | data 新增 `selfClosedBy` / `destroyedAt` |
| `PenCG/src/pages/play/play.vue` | `:787` | 排队路径置 `selfClosedBy` |
| `PenCG/src/pages/play/play.vue` | `:1175` | `finish` 置 `selfClosedBy` |
| `PenCG/src/pages/play/play.vue` | `:1198` | `fail` 置 `selfClosedBy` |
| `PenCG/src/pages/play/play.vue` | `:1238` | `doExit` 未登录分支置 `selfClosedBy` |
| `PenCG/src/pages/play/play.vue` | `:1245` | `doExit` 置 `selfClosedBy` |
| `PenCG/src/pages/play/play.vue` | `:1887` | `bgTeardown` 置 `selfClosedBy` |
| `PenCG/src/pages/play/play.vue` | `:1905-1929` | `cleanup()` 增加销毁归因日志（替换原 `logInfo`） |
| `PenCG/src/pages/play/play.vue` | `:1951-1961` | `cleanup()` 在 `stopPlay()` 前先 `setVideoVisible(false)` |
| `PenCG/src/services/media.js` | `:436-455` | `startPlay()` 真正停掉上一轮 native 会话（mediaId + wsId），不再只抹 id |

行数变化：`PenCG/src/pages/play/play.vue` 2813 → 2853；`PenCG/src/services/media.js` 617 → 635。

未改动（写域外）：`PenCG/src/pages/index/index.vue`、`PenCG/native/**`、`PenCG/src/dev-autostart.js`。

---

## 7. 构建

```
cd PenCG && node ../env/node_modules/aiot-vue-cli/src/cli.js -q -p
```

→ **exit 0**：`应用构建成功` + `打包成功,路径:…\PenCG\8002026110200003.1_0_3.amr`。
仅两条既有 warning：`找不到theme "theme-dark"`、`未找到以下模块 brightness,global,cgnative,cgmedia`（原生模块，可忽略）。

---

## 8. native patch 建议（**未实施**，交由 Lead）

### 8.1 首选：让 kmssink 收尾时不要动 CRTC

`PenCG/native/csrc/cgmedia/cm_gst.c:253-255`

```c
#define KMS_SINK_PROPS \
    " ! kmssink name=vsink plane-id=76 driver-name=rockchip " \
    "force-aspect-ratio=false can-scale=true sync=false"
```

建议改为（只加一个属性）：

```c
#define KMS_SINK_PROPS \
    " ! kmssink name=vsink plane-id=76 driver-name=rockchip " \
    "force-aspect-ratio=false can-scale=true sync=false restore-crtc=false"
```

理由：`restore-crtc=false` 后 kmssink 在 NULL 时只 disable 自己那条 plane（76），**不再对 CRTC 做 mode 恢复**，weston 的 `plane[54]` 保持绑定 → `plane_mask` 应稳定回到 1（只剩 weston）。这是「不碰别人的东西」的语义，也和「我们只申请了 plane 76」这个事实一致。

风险/代价（**必须真机确认**）：

- 若 weston 之后没有重新 commit，可能出现「我们的最后一帧残留」——但 `set_state(NULL)` 已经 disable 了 plane 76，理论上不该有残留。
- 需要验证正常对局路径、退出路径、重连路径、熄屏唤醒路径四种场景下 `plane_mask` 都符合预期。

### 8.2 备选：拆管线前先把视频盒缩成 1x1（native 内部兜底）

`PenCG/native/csrc/cgmedia/cm_gst.c:1209` `cm_box_destroy()` 里，`set_state(NULL)` **之前**插入：

```c
if (b->is_video && b->pipeline) {
    /* 收尾前先把平面压到 1x1：真机在重复 start 场景见过 CRTC 恢复把
     * plane_mask 清成 0，缩到最小至少不会留下整屏黑帧。 */
    cm_box_set_rect(b, 0, 0, 1, 1);
}
```

注意：`cm_box_set_rect` 内部有 `PAUSED → PLAYING` 脉冲，在销毁路径上调它可能触发一次 bus 事件/额外 1.5s 等待，**需要实测确认不会拖长 teardown**。若不可接受，退化为直接 `apply_rect(b)`（只写 `render-rectangle`，不做状态脉冲）。

### 8.3 会话级：`js_start` 拒绝「第二路视频会话抢同一 plane」

`PenCG/native/csrc/cgmedia/cgmedia.c:217` `js_start()`：在 `sess_alloc()` 之后、创建 rx 线程之前，遍历 `g_sessions`，若已有会话 `vbox` 非空且 `is_video`，则拒绝新会话（返回 `{"error":"video plane busy"}`）或先 `cm_box_destroy(旧 vbox)`。

理由：Falcon 每个页面独立 JS 上下文，两个 play 页各自 `start` 是**真实可达**的；两个 kmssink 抢 `plane-id=76` 的行为未定义。JS 侧看不见彼此，只有 native 能拦。

**建议同时做 8.1 + 8.3**：8.1 修「拆完之后回不去」，8.3 修「同时存在两个抢平面的人」。

---

## 9. 真机验证方案（**本次一条都没执行**，设备正在被 Lead 使用）

### V1（必做）确认拆除瞬间的 DRM 行为 —— 验证 H1/H2

```bash
# 1) 打开 DRM 调试（可写则成功；不可写说明这条路走不通）
./env/adb/adb.exe -s 9D91500007505171 shell "echo 0x1f > /sys/module/drm/parameters/debug; cat /sys/module/drm/parameters/debug"

# 2) 起一局，确认健康
./env/adb/adb.exe -s 9D91500007505171 shell "grep -a -A9 'Esmart1' /sys/kernel/debug/dri/0/summary"
#    期望：Esmart1-win0: ACTIVE，dst pos[107,244] rect[266x473]

# 3) 复现：第二次 start
./env/adb/adb.exe -s 9D91500007505171 shell "miniapp_cli start 8002026110200003 index"

# 4) 立刻看 plane_mask 与 dmesg
./env/adb/adb.exe -s 9D91500007505171 shell "cat /sys/kernel/debug/dri/0/summary | head -40"
./env/adb/adb.exe -s 9D91500007505171 shell "dmesg | tail -80"
```

判据：

- `dmesg` 里出现 `vop2_crtc_atomic_disable` / `plane_mask` 相关行 + `summary` 中 `plane_mask=0` → 支持 H1（CRTC 恢复把 mask 清了）。
- `dmesg` 里有 weston 侧 error（如 `drmModeSetPlane failed`）→ 支持 H2。
- 两者都没有但 mask 仍然是 0 → 需要换 `restore-crtc=false` 的版本做 A/B（§8.1）。

### V2 验证本次 JS 加固（归因日志）

```bash
# 清基线（只读不了的写操作交给 Lead）
./env/adb/adb.exe -s 9D91500007505171 shell ": > /userdata/applog/loli_s6_dev.txt"

# 起局 → 正常玩 30s → 第二次 start
./env/adb/adb.exe -s 9D91500007505171 shell "grep -a '\[play\] cleanup' /userdata/applog/loli_s6_dev.txt"
```

期望（改后）：

```
[pencg][W] [play] cleanup（beforeDestroy）by=none(外部销毁) stage=live phase=playing ws=open played=41s viz=1 hidden=0 imBusy=0 kb=0 imPend=0 busy=0 videoHidden=0
```

改前只有 `[play] cleanup（beforeDestroy）`，看不出是谁干的。**这就是本次改动的验收点。**

### V3（必做）确认「重复 navTo('play')」到底是 push 还是替换

```bash
# 起局后，数一下 play 页被 mounted 了几次（boot 里会打 stage 变化）
./env/adb/adb.exe -s 9D91500007505171 shell "grep -a -c '\[play\] page onShow' /userdata/applog/loli_s6_dev.txt"
# 再第二次 start，然后再数
```

判据：

- 若第二次 start 后 `cleanup` 出现但**没有**第二个 `[play] page onShow` / 没有第二份 `probe` 日志 → 宿主是「销毁旧页但不新建」→ 后续要防的是「页面消失」，不需要防「抢平面」。
- 若出现两份交错的 `probe ... rx=` 日志（两个 `stage=live`）→ **两个 play 页真的同时存在过** → §8.3 的 native 互斥是必须的。

同时检查 autostart 是否被触发两次：

```bash
./env/adb/adb.exe -s 9D91500007505171 shell "grep -a '\[dev\] autostart' /userdata/applog/loli_s6_dev.txt"
```

出现两行 `[dev] autostart fire → play` 即证实「重复 start → 重复 autostart → 重复 navTo」。

### V4 验证 `startPlay()` 泄漏修复

```bash
./env/adb/adb.exe -s 9D91500007505171 shell "grep -a 'startPlay 前清理上一轮会话' /userdata/applog/loli_s6_dev.txt"
```

- 出现 = 确实存在「旧会话没被 stop 就重新 start」的路径（说明这个修复有实际价值）。
- 一直不出现 = 该路径在正常操作下走不到（修复仍应保留，防的是异常路径）。

配合 native 侧确认没有孤儿会话：

```bash
./env/adb/adb.exe -s 9D91500007505171 shell "grep -a -E 'media loop exit|box destroyed|video plane hidden' /userdata/applog/DictPen_*.log | tail -20"
```

---

## 10. 本次没做 / 做不到的事

1. **没有修好「重复 start 会毁掉正在跑的对局」**。要修得动 native（§8）。
2. **没有验证 H1/H2 哪条成立**，设备探针（`drm.debug`）未开启，拆除瞬间的 DRM 调用链缺失。
3. **没有验证「两个 play 页是否曾同时存在」**（V3 未跑）。
4. **没有改 `index.vue` 的 `devAutostart` 幂等**（写域外，且它属于 login-state 的域）。当前装机的包里 `DEV_AUTOSTART` 非 null，取证环境本身就在放大这个触发概率。
5. **没有在真机上跑过任何一次 `miniapp_cli start`**（设备被 Lead 占用，且改设备状态属于禁止操作）。
