# 真机验证 Playbook —— 5 个 bug 的现场取证清单

> 本文把 `BUGFIX-bug1-lifecycle.md` / `BUGFIX-bug2-login.md` /
> `BUGFIX-bug3-bug4-video.md` / `BUGFIX-bug5-audio.md` 四份文档里的验证步骤抽成
> **一张可以直接照着做的表**：前置条件 → 操作 → 应出现日志串 → 判据 → 失败时看什么。
>
> 写作时间 2026-11-12 晚。设备 X6 Pro `9D91500007505171`，PenCG `1.0.3`，
> appid `8002026110200003`。**本文不含任何未经验证的断言**；拿不准的一律标「待现场确认」。

---

## 0. 取证通道（**任何一条验证之前先读这一节**）

### 0.1 唯一会落盘的 JS 通道是 `console.error`

Lead 2026-11-12 22:15 在 X6 Pro 上实测：

| 调用 | 是否落盘 | 证据 |
|---|---|---|
| `console.error(...)` | ✅ 落盘 | miniapp 进程 stdout 里有对应行 |
| `console.warn(...)` | ❌ **完全不落盘** | 全盘 `grep -a -c 'W][JS]' "$L"` = **0** |
| `console.log(...)` | ❌ **完全不落盘** | 同上 |

**这是本项目最容易踩的坑**：改回 `console.warn` 之后，日志不是"级别低了"，
而是**整条输出凭空消失**，现场会误判成「代码没跑」「时机没到」「设备不对」。
`PenCG/src/services/log.js:9` 的注释已经把这个教训写在代码里。

### 0.2 落盘路径与行格式

落盘文件 = **miniapp 进程的 stdout**，文件名带启动时间戳，每次重启都换，**不能写死**：

```bash
ADB="./env/adb/adb.exe -s 9D91500007505171"
P=$($ADB shell 'pidof miniapp' | tr -d '\r')
L=$($ADB shell "readlink /proc/$P/fd/1" | tr -d '\r')   # 实测 /userdata/applog/DictPen_20261113_232219.log
echo "LOG=$L"
```

> **路径更正（2026-11-14 真机定论）**：本手册早期版本（以及四份 BUGFIX 文档）
> 把落盘文件写成 `/userdata/applog/loli_s6_dev.txt`，**这是错的** ——
> 该文件实测只有 **1 字节**，`grep -c pencg` / `grep -c cgmedia` 都是 0，是个空文件。
> 真日志在 `DictPen_YYYYMMDD_HHMMSS.log`。**先 `readlink` 再 grep，不要写死、也不要只靠 `ls -t` 猜。**

行格式：

```
行格式：[2026-11-12 22:15:24.947][E][JS] [pencg][W] [screen] keep #3 backend=... ok bright=113
        └──────────┬────────────┘ └┬┘ └┬┘ └────┬────┘ └───────────────┬──────────────────┘
              落盘时间戳         级别 来源  PenCG 级别前缀       PenCG 正文
```

最常用的读日志命令（**任何一节都从这里开始**）：

```bash
./env/adb/adb.exe -s 9D91500007505171 shell "P=\$(pidof miniapp); L=\$(readlink /proc/\$P/fd/1); echo LOG=\$L; grep -a -n 'pencg' \$L | tail -50"
```

三条铁律：

1. **落盘级别恒为 `[E]`。** 底层通道是 `console.error`，这个 `[E]` 跟我们想表达的
   严重程度**毫无关系**。`logInfo` / `logWarn` 打出来的东西一样带 `[E]`。
2. **真正的级别只看行内 `[pencg][I]` / `[pencg][W]` / `[pencg][E]`。**
   grep 级别请 grep 行内前缀，**不要** grep `][E][JS]`。
3. PenCG 侧所有输出都经 `PenCG/src/services/log.js` 的 `push()` 走 `console.error`
   （`PenCG/src/services/log.js:44`），统一前缀是 `'[pencg][' + level + '] ' + text`
   （`PenCG/src/services/log.js:41`）。

### 0.3 第二条通道：native 的 `cm_log()`

`PenCG/native/csrc/cgmedia/cm_util.h:20-23` 的 `CM_LOGE/W/I/D` 全部落到
`PenCG/native/csrc/cgmedia/cm_util.c:19-29` 的 `cm_log()`：

```c
fprintf(stdout, "[cgmedia] ");
vprintf(fmt, ap);            // 实际是 vfprintf(stdout, ...)
fputc('\n', stdout);
fflush(stdout);
```

- 前缀是 **`[cgmedia]`**，不是 `[pencg]`，默认级别 `g_cm_log_level = 3`（即 `CM_LOGD` 不打）。
- **它和 JS 的 `console.error` 必落同一个文件**：两者都跟着 miniapp 进程的 stdout 走，
  而一个进程只有一个 stdout（2026-11-14 已确认）。所以**不是两条通道两个文件**，
  是**一个文件两种前缀**。
- 抓日志时两条前缀一起抓：`grep -a -e '\[pencg\]' -e '\[cgmedia\]' "$L"`。

### 0.4 进场三条命令（每次都跑）

```bash
cd /d/Users/Daniel/code/DSH/x6pro/haas-x6pro
ADB="./env/adb/adb.exe -s 9D91500007505171"

# ① 设备在线
$ADB shell 'echo ok'

# ② 解析当前日志文件并清空（本次唯一会"写"设备的动作，只动日志文件）
P=$($ADB shell 'pidof miniapp' | tr -d '\r')
L=$($ADB shell "readlink /proc/$P/fd/1" | tr -d '\r')
echo "LOG=$L"
$ADB shell "mv \"$L\" \"$L.bak\" 2>/dev/null; sync"

# ③ 通道自检：随便触发一次 PenCG 操作（如切到「我的」），再数计数
$ADB shell "grep -a -c 'pencg' $L"
```

**③ 的计数为 0 就是通道问题，不是代码问题。** 先修通道（确认装的是含
`console.error` 的新包、确认 `log.js:44`），再去怀疑逻辑 —— 否则后面每一条判据
都会"看起来没生效"。

> **路径更正（2026-11-14）**：`/userdata/applog/loli_s6_dev.txt` 实测是 **1 字节空文件**，
> 真日志在 `DictPen_YYYYMMDD_HHMMSS.log`。早期版本本文档把两者说反了，已全部更正。

---

## 1. bug 1 —— 熄屏被判成退后台，游戏会话被拆（lifecycle）

对应 `BUGFIX-bug1-lifecycle.md`（§5 断言 V1–V5、§6 步骤、§6.5 验收 A1–A6）与 `BUGFIX-bug1-active-renew.md`（传输侧主动续命）。

> **bug1 有两半，别只测一半**（另有第三半：传输侧停推后的自愈时效 —— 2026-11-14 已把 `STALL_RX_MS` 收紧到 8s，并关闭主动续命，见本节末两行）：
> - **屏幕这一半**（本节大部分条目）：`[Idle]` 熄屏 → `onHide` → 拆会话。判据是 `[screen] keep #N` 与 probe 的 `screen=`。
> - **传输这一半**（本节最后一条 + 失败指纹最后两条）：网关在**每个 WS 会话约 250s** 后停推（2026-11-14 三局实测 247/253/248s）。判据是 probe 的 `sess=` / `renew=` 与 `[play] renew:` 行。
>
> 两半互相独立：屏幕全亮、保活正常、`ws=open`，画面照样会在 ~250s 停更。

### 前置条件

- 设备在跑含 life-cycle 补丁的包；`cd /d/Users/Daniel/code/DSH/x6pro/haas-x6pro`。
- 基线 idle 配置：`270s = hal-screen down`、`300s = len_onoff.sh off`、`900s = system_sleep_wakeup`
  （`$ADB shell "cat /etc/input-event-daemon.conf"`）。**若不同，本文所有秒数按实际值等比缩放。**
- 当前屏幕状态：`$ADB shell "hal-screen state"`。

### 操作 → 应出现日志串

| 操作 | 应出现（`$L` 内 grep） | 判据 |
|---|---|---|
| 冷启动进游戏页 | `[screen] probe 形态=... → 命中/miss(...)` ×6，然后 `[screen] 后端探测结论：采用 <name> 能力[keep=1 ...]` | **出现 `后端探测结论：全部未命中` 就是红色警报**：六条回退全不通，保活是空转，bug1 修了等于没修（对应断言 V1） |
| 游戏在播，不碰屏幕 | 每 20s 一行 `[screen] keep #N backend=... ok bright=... state=0(ON) always=on closeScreen=1080s ok` | `N` **必须单调涨**；出现两段交错的 N（1,1,2,2…）= 定时器泄漏 |
| 同上，看探针 | 每 20s 一行 `[play] probe stage=live v=1 screen=0(ON) phase=playing ws=open rx=... fps=... kf=... keep=on` | `probe` **不许断**；`screen=` 恒为 `0(ON)`；`rx` 应持续上涨 |
| 等 270s 后（屏已暗） | `[play] probe` 里 `screen=` 的取值 | `screen=3(DIM)` 或 `screen=1(OFF)` → 断言 **V2 成立**，守卫会生效；`screen=4(未知(4))` 或 `screen=null` → **V2 失败**，按 `BUGFIX-bug1-lifecycle.md` §5 备选方案改判据（`PenCG/src/pages/play/play.vue` 的 `screenIsOff` 与 `PenCG/src/app.js:73`，从 `n === 3 \|\| n === 1` 改成 `n !== 0`；**先别直接改**，`n !== 0` 会把"读状态失败"也算熄屏，真退后台时平面拆不掉） |
| 手动按电源键熄屏 10s 再按亮 | `[pencg][life] app onHide screen=N(名字)`、`[pencg][life] app onHide 判为系统息屏（screen=N(名字)）→ 不拆会话，交给 play 页重新保活`、`[play] onHide 但屏幕已熄/调暗 → 判为系统息屏，保持会话并重新保活 stage=...`、`[play] bgTeardown 跳过：屏幕已熄/调暗（系统息屏，不是退后台）screen=...`、`[screen] 保活启动 20000ms ...` | 亮屏后**游戏还在**，`stage=live`，能继续操作（验收 A3） |
| 手动按 Home 键（**真退后台**） | `[pencg][life] app onHide 真退后台 → stopPlay streaming=X code=Y`、`[play] onHide 拆管线 stage=... viz=...`；`$ADB shell "grep -a -A9 'Esmart1' /sys/kernel/debug/dri/0/summary"` 变 `INACTIVE`；`[screen] 保活停止 ticks=N` | 回桌面**看不到残留画面**（验收 A4）。**这是回归门槛**：为了修 bug1 让 KMS 平面永久盖住系统 UI，比 bug1 本身更糟 |
| 连玩 8 分钟不碰屏 | `[screen] keep #N` 涨到 ~24（8min/20s），`state=0(ON)`；`[play] probe` 不断 | 验收 A1/A2 |
| 玩到 15 分钟以上 | `$ADB shell pidof miniapp` 仍非空 | 没被 900s `system_sleep_wakeup` 杀（验收 A5） |
| 长按退出 / 正常结束 | `stage=end`、`[screen] 保活停止 ticks=N（定时器已清，后台不再保活）` | 机位已释放（验收 A6） |
| **连玩 6 分钟以上，只看传输侧** | probe 行里的 `sess=`（会话寿命秒数）；停推后应出现整条自愈序列：`[play] stall detected: rx=N 已 8s 未增长 → 尝试自愈（第 1 次：补发关键帧）` → `[play] stall: rx=N 已 11s 未增长 → L2 复用网关重连` → `[media] wsOpen id=2` → `[play] reconnect ok（复用旧网关）` → `[play] stall: 媒体会话重启，rx 重新计数（N → 0），已用自愈 2 次，继续观察` → `[play] 出画 1280x720 audio=1` → `[play] stall recovered: rx=M 已恢复（第 2 级自愈成功，停滞 Ns）` | **健康态**：`sess=` 每局从 0 涨到约 250s；停推后 **`rx` 冻结满 8s** 触发 L1，**约 11 秒内** `rx` 重新增长、`stall recovered` 出现、画面回。用户看到约 11 秒静止画面，**游戏进度不丢**。判据来源见 `docs/BUGFIX-bug1-stallguard.md` §5.6 |
| 同上，确认主动续命已关闭 | probe 行里的 `renew=` | **`renew=` 恒为 0 是正确的**（`ACTIVE_RENEW_MS = 0`，2026-11-14 起默认关闭：换会话会让游戏重开，见 `docs/BUGFIX-bug1-active-renew.md` §9）。若看到 `[play] renew:` 行，说明这版包把开关改回了非 0 —— 那不是回归，是配置被改 |

### 失败时看什么（三条链的现场指纹）

| 观测 | 结论 |
|---|---|
| `[play] probe` **断了**，之后**有** `[pencg][life] app onHide screen=3/1` + `[play] onHide 拆管线` | **链 A**（熄屏被当退后台）—— 守卫没生效，回到 V2 检查状态码 |
| `[play] probe` **断了**，之后**没有** onHide 行，但有 `#N JS heartbeat lost (... ms silent, rx=... bytes)` | **链 B**（JS 挂起 → native 看门狗，`WATCHDOG_MS 180000` / `WATCHDOG_WARN_MS 60000` / `WATCHDOG_CHECK_MS 500`） |
| `[play] probe` 一直在刷，但 `rx` 不涨、`ws=closed`，`Esmart1 INACTIVE` | **网络 / 网关侧断流**，与屏幕无关 |
| `[play] probe` 一直在刷，`rx` 在涨，`screen=3/1`，`Esmart1 ACTIVE` | 只是屏黑了，**画面其实还在** = 纯保活问题（最理想结果） |
| `$ADB shell pidof miniapp` 为空 | 进程被杀（900s 唤醒或 OOM），是另一条链 |
| `#N JS silent %llu ms but stream alive (rx=%llu bytes)` | JS 静默但码流还在 → 看门狗判定为"流活着"，不会拆 |
| `sess=` 涨过 260s 后出现 `[play] stall detected` | **正常**（每会话约 250s 必停推，这是服务端行为，根因未修）。只要后面跟着 `stall recovered`，就是兜底成功 |
| `stall detected: rx=N 已 20s 未增长`（注意是 20s 不是 8s） | 装的是**旧包**：`STALL_RX_MS` 已于 2026-11-14 收紧到 8s。先确认 `.amr` 版本再判故障 |
| `stall detected` 之后 30 秒仍**没有** `stall recovered`，也没走到 L3 | L2 没救回来。看 `[media] wsOpen id=` 是否出现、`[play] reconnect ok` 是否有、`出画` 是否重打；再看是否被 `op=close` 踢（`grep 'op=close'`） |
| `[play] stall recovered` 出现但 `停滞 5s` 这类数字远大于 11s | 恢复得慢：看是不是走到 L3（`→ L3 重新申请机位`）。L3 会重拉对局容器 = **游戏从头开始**，这是最坏路径 |
| `renew=` 不为 0 且画面每约 230s 重开一次游戏 | 主动续命被重新启用（`ACTIVE_RENEW_MS` 非 0）。这是已知不可接受的行为，应改回 0，见 `docs/BUGFIX-bug1-active-renew.md` §9 |

辅助读数（都只读）：

```bash
$ADB shell "hal-screen state"                                  # screen_state / bl_br / bl_power
$ADB shell "cat /sys/class/backlight/backlight/bl_power"       # 0=上电 4=断电
$ADB shell "grep -a -A9 'Esmart1' /sys/kernel/debug/dri/0/summary"
$ADB shell "pidof miniapp"; $ADB shell "pidof aplay"
```

### 仍未证实的点（待现场确认）

- **V3**：20s 保活是否真的拦住了 270s（靠 A1/A2 判定）。（注意这里的 20s 是**保活间隔** `KEEP_INTERVAL_MS`，与 STALLGUARD 的 `STALL_RX_MS` 是两回事，不要混）
- **V4**：熄屏瞬间 Falcon 到底调不调 App `onHide`。
- **V5**：熄屏后 QuickJS 定时器是否真的停摆（决定链 B 是否真实存在）。
- `screenSt == null`（读不到状态）时按"没熄"走保守拆管线，这是**有意的取舍**，不是 bug。

---

## 2. bug 2 —— 登录成功但「我的」不刷新（login）

对应 `BUGFIX-bug2-login.md`（§五 步骤 0–6）。

### 前置条件

- 设备 `9D91500007505171`，已验证在线；清过 `$L`（见 §0.4）。
- 起始状态必须是**未登录**：手动在设备上「我的」→ 退出登录。
  ⚠ **不要**用 adb 改 `preferences.json`（值是「JSON 字符串里再套一层 JSON」，
  必须 pull → `json.loads` 校验 → push，`HANDOFF.md` L68-77 明确禁用 sed）。
- 记录已登录态的 session 形态（只读）：
  `$ADB shell "cat /userdisk/miniapp/data/mini_app/pkg/8002026110200003/data/sharedpreferences/preferences.json" | grep -o 'pencg_session[^,]*'`

### 操作 → 应出现日志串

| 操作 | 应出现 | 判据 |
|---|---|---|
| 点「去登录」→ 输账号密码 → 点登录，**登录成功后 600ms 内不要碰左侧栏** | `[pencg][I] [auth] 密码登录成功 uid=...` | 登录环节本身成功 |
| ↑ 紧接着 | `[pencg][I] [nav] closePageByName login` | 防线①的导航落点 |
| ↑ 紧接着 | `[pencg][I] [index] 会话版本 1 -> 2 本地登录态 false -> true` | 防线②走通 |
| ↑ 紧接着 | `[pencg][I] [mine] refresh logged=true booted=true` | **★ 核心判据** |
| ↑ 紧接着 | `[pencg][I] [mine] @me ok free=... vip=...` | 资料真的拉到了 |

- **关键判据**：出现 `[mine] refresh logged=true`。修复前这里会是 `logged=false`；
  若根本没人调 refresh，**这行完全不出现**。
- 界面判据：600ms 后自动回到「我的」并直接显示头像首字（`initial`）、昵称
  （如 `用户aaabOhXYY`）、剩余时长，**不是**「还没有登录网易云游戏」。
- 兜底通道也算通过：若只看到
  `[pencg][W] [index] mine-body 实例未就绪，重试` +
  `[pencg][I] [mine] session 版本变化 1 -> 2，重载`，
  且最后仍有 `[mine] refresh logged=true`，则修复生效，只是走的是兜底（对应遗留风险 2/3）。

### 反证与防回归

| 检查 | 命令 | 通过标准 |
|---|---|---|
| 导航没塌陷（没多压一层页） | `$ADB shell "grep -a '\[nav\]' $L \| tail -n 20"` | 有 `closePageByName login`，**不应**有 `[nav] 无法关闭页面 name=login`（那是 `navTo('index')` 兜底被触发） |
| 会话变化能被无刷新感知 | 停在「推荐」→ 从其它入口登录 → **不点任何东西**等 2.5~5s | 界面自动切到「我的」；日志有 `[index] 会话版本 ...` 或 `[mine] session 版本变化 ...` |
| **防回归（最容易踩的坑）** | 已登录状态下杀小程序 → 冷启动 | 启动后停在**「推荐」**，不被自动拽到「我的」。若冷启动出现「会话版本 **0 -> N**，本地登录态 false -> true」，说明 `isLoggedIn()` 翻转保护失效 —— **这是必须修的回归** |
| 退出登录也同步 | 已登录停在「我的」→ 点退出登录 → 确认 | 提示变「未登录」，其它栏目点回来仍是未登录（版本号 bump 生效，不会复活旧会话） |

### 失败时看什么

- `[mine] session 版本变化` 有、`[index] 会话版本` 没有 → `$falcon` 不可写，
  防线①②失效、退化到 2.5s 轮询（遗留风险 1）。
- 只看到轮询那条、看不到 `onShow` 那条 → `closePageByName` 后 `onShow` 未被回调
  （遗留风险 3，建议结论补进 HANDOFF.md）。
- 一条 `[pencg]` 都没有 → 回到 §0.4 第 ③ 步，**先确认通道**。

---

## 3. bug 3 / bug 4 —— 马赛克残留与开屏绿屏（video）

对应 `BUGFIX-bug3-bug4-video.md`（§五 步骤 0–7）。

- **bug 3**：原神游玩中「上一帧的马赛克残留」——运行中参考帧被缺片 AU 污染，无法自愈。
- **bug 4**：明日方舟终末地「一连上就马赛克 + 绿屏」——`have_kf == 0` 时把 P 帧推给解码器，
  DPB 全是未初始化 NV12。

### 前置条件

```bash
./env/adb/adb.exe -s 9D91500007505171 shell 'echo ok'

# GST_DEBUG 开着会把帧率打到 2.27fps，测性能前必须清掉
./env/adb/adb.exe -s 9D91500007505171 shell \
  'rm -f /userdisk/miniapp/data/*gst*debug* /tmp/*gst*debug* 2>/dev/null; ls /userdisk/miniapp/data/ | head'

# 确认旧版已彻底停掉，避免两套 KMS plane 打架
./env/adb/adb.exe -s 9D91500007505171 shell 'ps | grep -i miniapp'
```

### 操作 → 应出现日志串（native `[cgmedia]` 通道）

| 操作 | 应出现 | 判据 |
|---|---|---|
| 每次（重）连、每次切画质 | `#1 SPS reports 1280x720 profile=100 level=...` | `profile` 66=Baseline / 77=Main / 100=High；两游戏 profile 不同 → bug3/bug4 差异有码流侧解释 |
| 码流参数变化 | `#1 SPS profile changed (A -> B, level C -> D), request fresh IDR` | 旧版恒打 0 的 bug 已修好，现在能看到真实前后值 |
| 网关不重发 SPS/PPS | `#1 IDR without SPS,PPS, injected cached parameter sets` | ≥1 说明补丁把参数集补回来了 |
| 会话刚起 | `#1 waiting for first IDR, dropping P frames` | 只在开局短暂出现；**持续刷** = 始终没拿到 IDR |
| 丢包 | `#1 video gap (... ) -> request IDR (PLI #1)` | 丢包触发 IDR 请求 |
| 发 PLI | `#1 PLI sent (N bytes, srtcp)` | 出现 **`plain`** 说明 DTLS 导出的 SRTCP 密钥还没 ready，PLI 等于白发 —— 该先修 DTLS 时序，别再调 RTP 参数 |
| 每 2s 一条 stat JSON（`"t":"stat"`） | 字段 `lost / auDrop / gateDrop / kfReq / kf / dup / reorder / spsInject / rtcpIn` | 见下表 |
| 健康告警（每 10s 最多一条） | `still no IDR (rx=... kfReq=... gateDrop=... auDrop=...) — gateway may be ignoring PLI` | 网关侧问题，不是本端 bug（§六 风险 1） |
| ↑ | `packet loss seen (lost=... auDrop=... dup=... reorder=...) — broken AUs dropped, not shown` | 补丁正在工作：丢了包、丢了整帧、坏的没上屏 |

stat 四字段的相对关系（核心判据）：

| 字段 | 含义 | 健康值 | 异常含义 |
|---|---|---|---|
| `lost` | RTP 序号缺口累计（补丁前恒 0） | 缓慢增长 | 快速上涨 = 弱网 |
| `auDrop` | 因缺片/乱序而整帧丢弃的 AU 数 | >0 且 ≈ 用户看到的「卡一下」次数 | 长期 0 而仍有残留 = **丢包不在 RTP 层**（SRTP 已拦掉，或网关侧编码器出错） |
| `gateDrop` | 首个 IDR 之前丢掉的 P 帧数 | 只在开局短暂增长后停止 | 持续增长 = **始终没拿到 IDR** |
| `kfReq` vs `kf` | 要了几次 IDR / 真收到几个 | `kf` 跟着 `kfReq` 涨 | `kfReq` 涨而 `kf` 不涨 = **网关不理 PLI** |

> 「马赛克残留」在日志上的定义：**`auDrop` 在涨，但 `frames` 也还在涨**
> （坏帧被丢、好帧还在推，解码器不该再看到残帧）。
> 「补丁没生效」的定义：**`auDrop` 一直为 0，而用户仍然看到残留**。

### 画面到底有没有上屏（KMS 取证，唯一可信证据）

```bash
# debugfs 单次 read 有 576 字节上限，简单 cat 会漏内容，用循环读
./env/adb/adb.exe -s 9D91500007505171 shell '
  i=0
  while [ $i -lt 150 ]; do
    cat /sys/kernel/debug/dri/0/summary 2>/dev/null | tr "\n" " " | sed "s/^/$(date +%s%3N) /"
    echo; i=$((i+1)); sleep 0.2
  done' > /tmp/kms_summary.log
grep -c 'Video Port0: ACTIVE' /tmp/kms_summary.log   # >0 才说明视频平面真的挂上了

# 平面几何（apply_rect 公式的真机对照）
./env/adb/adb.exe -s 9D91500007505171 shell \
  'cat /sys/kernel/debug/dri/0/state | grep -A6 "Esmart1-win0"'
```

已确认的真机基线（设备空闲时）：

```
plane[76]: Esmart1-win0
        crtc-pos=240x427+107+299
        src-pos=1280.000000x720.000000+0.000000+0.000000
        rotation=1
```

判据：**`buf` 地址在变** = 还在出新帧；**`buf` 地址不变但 `ACTIVE` 仍是 1、画面仍有内容**
=「冻结」（网关不给 IDR，§六 风险 1）；`buf` 在变而画面局部错位 = 解码侧污染（本补丁治的）。

### 截图取证（视频平面必须用 `captureFB`）

```bash
# miniapp_cli capture 截的是 Falcon UI 合成面（960x266），**不含 KMS 视频平面**
# （plane 76 是硬件叠加，绕过 weston）
./env/adb/adb.exe -s 9D91500007505171 shell '/bin/miniapp_cli captureFB /tmp/v.png; ls -l /tmp/v.png'
./env/adb/adb.exe -s 9D91500007505171 pull /tmp/v.png ./bug3_video.png
```

判据：绿屏 = 大面积 `RGB(0,135,0)` 或未初始化 NV12 的绿/紫块；
马赛克残留 = 局部块状错位但整体画面仍在动（连续截 3 张对比同一局部区域）。

### 失败时看什么

- `auDrop > 0` 且用户仍反馈「还有残留」→ **补丁逻辑有漏**，用
  `BUGFIX-bug3-bug4-video.md` §1.3 的 3-A1/A2 继续查。
- `auDrop == 0` 但用户仍见残留 → 丢包不在 RTP 层，转 §六 风险 5（`mppvideodec` flow error
  后 `b->failed` 不置位）。
- `kfReq` 涨而 `kf` 不涨 → 网关彻底不理 PLI，画面只会冻结；这是网关侧问题，
  可用 `still no IDR` 告警与其他故障区分。
- **一条 `[cgmedia]` 都没有** → 先用 `P=$(adb shell pidof miniapp); adb shell "readlink /proc/$P/fd/1"`
  确认这个进程的 stdout 指向哪个文件，再去那个文件里 grep，别直接判定补丁没编译进去。
  **结论（已真机定论 2026-11-14）**：`[cgmedia]` 走 `cm_util.c` 的 `fprintf(stdout, ...)`，落进**当前
  miniapp 进程的 stdout**；该 stdout 指向哪个文件取决于进程怎么起的。`[pencg]`（`console.error`）同理。
  **两者永远落在同一个文件里**，不是一个 JS 文件一个 native 文件。
  实测（X6 Pro，1.2.0）：`/userdata/applog/loli_s6_dev.txt` 是 **1 字节空文件**，
  真正的日志在 `/userdata/applog/DictPen_20261113_232219.log`（13.2MB，`[cgmedia]` 2236 行、
  `[pencg]` 上万行）。所以**别默认去 `loli_s6_dev.txt` 找**，先跑
  `P=$(adb shell pidof miniapp); adb shell "readlink /proc/$P/fd/1"` 拿真实路径。

### 仍未证实的点（待现场确认）

- 本次写作时**没有真机 A/B**：设备当时装的是旧版 1.0.3 且没在跑，
  `/userdata/applog/` 里 `grep -a pencg` 命中数为 0 —— 事后确认这是
  **取证通道用错了**（当时 `log.js` 走 `console.warn`，日志根本没产生），
  不是「代码没执行」。
- 终末地是否真的 1080p（决定 §六 风险 4 的 `FRAME_CAP` 4MB / `max-bytes` 2MB 是否偏紧）。
- 网关是否使用 RTX / simulcast / 辅助 SSRC（看 `pt_hist` 直方图）。
- 真机跑完后，把 `auDrop / kfReq / kf / gateDrop` 四个数记进 `PenCG/docs/MEDIA_DEBUG.md`
  作为新基线。

---

## 4. bug 5 —— 系统音量被改成 200 且退出不还原（audio）

对应 `BUGFIX-bug5-audio.md`（§4 步骤 0–6）。

### 前置条件

```bash
ADB="./env/adb/adb.exe -s 9D91500007505171"

# 设备干净基线
$ADB shell "amixer -c 1 cget numid=54 | grep -o 'values=[0-9,]*' | tail -1"          # 期望 0,0
$ADB shell "amixer -c 0 cget name='Playback Path' | grep -o 'values=[0-9,]*' | tail -1"  # 期望 0 (OFF)
$ADB shell pidof aplay                                                                # 期望空
$ADB shell 'cat /proc/asound/card1/pcm0p/sub0/status'                                 # 期望 closed
```

> ⚠ **读 `cget` 必须 `grep -o 'values=[0-9,]*'`**。直接 `tail -1` 会拿到
> `| dBscale-min=...` 那一行，读数完全是错的（这是本 bug 排查里踩过的坑）。

音量映射：card0 `numid=45` Playback Path（`0`=OFF，`2`=SPK）、card0 `numid=51` Master=225,225、
card1 `numid=54` Master（`hal-audio -s 0` → `0,0`；`-s 50` → `176,176`；
**旧 bug 会把它抬到 `200,200`**）。

### 操作 → 应出现日志串（native `[cgmedia]` 通道）

| 操作 | 应出现 | 判据 |
|---|---|---|
| 系统音量 0，进游戏起播 | `[cgmedia] #3 audio snapshot: Playback Path=OFF amp(card1#54)=0,0` | 快照拿到了真实基线 |
| ↑ | `[cgmedia] Playback Path OFF -> SPK (now=SPK)` | 只改路由，**不改用户音量** |
| ↑ 立刻查 | `$ADB shell "amixer -c 1 cget numid=54 ..."` → 仍 `values=0,0`；`amixer -c 0 cget name='Playback Path'` → `values=2` | Bug5 现象 1 修复：**完全静音**（修复前会变 200,200 而有声） |
| 退出游戏 | `[cgmedia] aplay pid=2412 exited on EOF after 10 ms`（或 `exited on SIGTERM after 210 ms`） | aplay 正常收尾，不靠 SIGKILL 硬杀 |
| ↑ | `[cgmedia] #3 audio restore: Playback Path SPK -> OFF (back to OFF)` | 路由还原（修复前永久留在 SPK） |
| ↑ | `[cgmedia] #3 audio restore: amp(card1#54) 176,176 unchanged (we never write it)` | 自证我们从头到尾没写用户音量 |
| 系统音量 50，进游戏有声，退出后 | `pidof aplay` 空、`/proc/asound/card1/pcm0p/sub0/status` = `closed`、card1#54 仍 `176,176`、card0 Playback Path 回 `0` | Bug5 现象 2 修复 |
| 退出 PenCG 回到桌面，用有道查词 | 音量 0 → 查词**没声**；音量 50 → 查词正常出声 | 修复前音量 0 时会听到"莫名其妙很大声的查词音" |
| 游戏内网关下发 `setAudio {on:0}` | `audio restore` **只出现一次**（幂等） | 不会因为 `cm_cmd` 兜底再还原一遍 |
| 拔网让 RTP 静默 180s（`WATCHDOG_MS`） | 超时退出后同样是 `audio restore ... back to OFF`，`pidof aplay` 空 | 看门狗路径自动获得还原能力（可选，慢） |

### 不应出现（出现即新证据）

| 串 | 含义 |
|---|---|
| `[cgmedia] audio amp volume 0 -> 200` | **旧包的指纹**。这行代码已被删除，出现即说明装的不是本次修复包 |
| `[cgmedia] aplay pid=xxxx UNKILLABLE after 1000 ms (stuck in driver?)` | aplay 卡在驱动里，`aplay_stop()` 走满 200+300+500=1000ms |
| `[cgmedia] #3 audio restore: Playback Path now SPK (not ours ...), leave it` | 用户在我们运行期间自己改过路由，我们**按设计**不去覆盖 |

### 失败时看什么

- 退出后 card0 `Playback Path` 仍为 `2` → 还原没跑到。查 `cm_box_destroy()` 的
  音频分支（`PenCG/native/csrc/cgmedia/cm_gst.c:1117` 附近），确认
  `aplay_stop(b)` → 音频还原 → `free()` 的顺序没被视频侧改动打乱。
- `audio snapshot` 有、`audio restore` 没有 → 会话状态字段被提前清掉（幂等保护误触）。
- `/proc/asound/card*/pcm0p/sub0/status` 不是 `closed` → 声卡仍被独占，先查 `pidof aplay`。

### 仍未证实的点（待现场确认）

- 蓝牙分支的 XRUN（`cg_aplay.log` 里 `underrun!!!` / `state : XRUN`）与本次静音 bug 无关，
  是 `-B 150000 -F 30000` + `fdsink sync=false` 的缓冲/时钟问题，未处理。
- `hal-audio -g` 是坏接口（系统侧问题），已记录在 `PenCG/docs/audio-baseline.txt` 第 4 节。

---

## 5. 交叉速查：一条日志该归哪条通道

| 行内前缀 | 来源 | 落盘规则 | 典型串 |
|---|---|---|---|
| `[pencg][I/W/E]` | `PenCG/src/services/log.js:44` → `console.error` | ✅ 必落盘（通道恒 `[E][JS]`） | `[life] app onHide`、`[play] probe`、`[screen] keep #N`、`[mine] refresh`、`[index] 会话版本`、`[auth] 密码登录成功` |
| `[im]` / `[media]` | `PenCG/src/services/im.js:18`、`PenCG/src/services/media.js:388` → `console.error` | ✅ 落盘 | `[im] ...`、`[media] SDP ...` |
| `[pencg][diag]` | `PenCG/src/pages/diag/diag.vue:67/76/83/639` → `console.error` | ✅ 落盘 | 诊断页输出 |
| `[cgmedia]` | `PenCG/native/csrc/cgmedia/cm_util.c:19-29` → `fprintf(stdout)` | ❔ **待现场确认** | `SPS reports`、`PLI sent`、`audio snapshot`、`aplay pid=`、`JS heartbeat lost`、`still no IDR` |

**误判对照表**（现场最常见的四种"假阴性"）：

| 现象 | 真实原因 |
|---|---|
| 一条日志都没有 | 通道问题：装的是 `console.warn` 版本，或路径写成了 `DictPen_*.log` |
| 「明明打了 warn 却看不到」 | `console.warn` 不落盘（本题的根因） |
| 用 `grep 'W\]\[JS\]'` 数不到行 | 恒为 `E][JS]`，这是通道决定的，不是没有 warn 日志 |
| 有 `[pencg]` 但看不到 `audio snapshot` / `SPS reports` | 那是 native `[cgmedia]` 通道，另一路的落盘情况待确认 |

---

## 6. 附录：`PenCG/src/` 里 `console.warn` / `console.log` 的全量 grep 结果

命令（在仓库根执行）：

```bash
grep -rn "console\.warn\|console\.log" PenCG/src/
```

结果：**3 条命中，全部是注释，没有任何运行时调用**。也就是说 JS 侧已经不存在
「写了但永远看不到」的日志点。

| 文件:行 | 内容 | 性质 |
|---|---|---|
| `PenCG/src/services/log.js:7` | `// console.warn 与 console.log **完全不落盘**（全盘 grep 不到一行 [W][JS]）。` | ✅ 正确，是这次教训的书面记录 |
| `PenCG/src/services/log.js:9` | `// 若改回 console.warn，真机上所有 PenCG 日志会静默消失（这个坑踩过一次）。` | ✅ 正确 |
| `PenCG/src/services/screen.js:67` | `// console.log 不落盘（真机只有 console.warn / console.error 进 /userdata/applog/），` | ⚠️ **内容已过时/错误**：`console.warn` 并不落盘；且路径不是固定文件。仅注释，不影响运行；建议后续修正为「只有 `console.error` 落盘，落 miniapp 进程 stdout（`readlink /proc/$(pidof miniapp)/fd/1`）」 |

结论：
1. `PenCG/src/` 下**没有**任何 `console.warn(...)` / `console.log(...)` 的运行时调用，
   所有输出都收敛到 `PenCG/src/services/log.js:44` 的 `console.error` —— 与 §0.1 的实测结论一致。
2. 唯一需要后续处理的是 `PenCG/src/services/screen.js:67` 的过时注释（本次只写 `PenCG/docs/`，
   未改 `src`，故在此登记）。

文档侧同类残留（供后续统一清理，本次已修 `DEV.md`）：

- `PenCG/README.md:197` 仍写「只有 `console.warn` / `console.error` 进 `/userdata/applog/`」—— **错误**，
  且不在 `PenCG/docs/` 写作用域内，未改，登记在此。
- `PenCG/docs/audio-baseline.txt:193` 原写「console.warn/error 落 `/userdata/applog/DictPen_*.log`」，
  路径与通道两处都过时 —— 已在本次一并修正为「仅 `console.error` 落盘」。

> **2026-11-14 再更正**：上面这条 11-13 的「修正」本身只对了「仅 error 落盘」这一半，
> 路径半句还是错的。当时的判断是「`DictPen_*.log` 不存在、真日志在 `loli_s6_dev.txt`」，
> **事实正好相反**：`loli_s6_dev.txt` 是 1 字节空文件，`DictPen_*.log` 才是真日志。
> 现在统一的说法是「落盘文件 = miniapp 进程 stdout（`readlink /proc/$(pidof miniapp)/fd/1`），
> 实测形如 `DictPen_YYYYMMDD_HHMMSS.log`」，见 §0.2。凡是还写着 `loli_s6_dev.txt`
> 作为**落盘目标**的地方都已改成 `$L`。
- `PenCG/docs/STATES.md:236` 的 `grep -h '\[mine\]'` 原用 `DictPen_*.log`，11-13 被误改成
  `loli_s6_dev.txt`；正确做法是按 §0.2 先解析 `$L` 再 grep。
