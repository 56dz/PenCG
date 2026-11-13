# bug2 真机验证方案：登录成功后「我的」页仍显示未登录

> 配套文档：[BUGFIX-bug2-login.md](BUGFIX-bug2-login.md)（根因分析与补丁说明）。
> 本文只讲**怎么在真机上证明它修好了**，不重复代码分析。
>
> **当前状态：本方案的任何一步都还没有被执行过。** bug2 的修复到目前为止
> 只有「代码评审 + `aiot-vue-cli` 构建通过」两项证据，**没有一行真机日志**。
> 因此本文档里的所有"期望看到的日志行"都是**按源码推断的**，不是实测结果。
> 第一次执行的人请把它当假设去验证，不要当结论去复述。

---

## 零、为什么需要一个专门方案（不能"手点一下看看"）

这台设备 **无法用 adb 走完一次登录**：

1. 登录页的手机号 / 密码输入框是**自绘软键盘**（`div` 网格 + `@click` 逐位输入，
   `PenCG/src/pages/login/login.vue:53` / `:60` 的 `@click="editPhone"` / `@click="editPwd"`（方法在 `:357` / `:372`）），
   不是原生 `<input>`。
2. 本机 `/dev/input/event1` 被 weston/seatd 独占，`send_event touch press/release`
   对 `scroller` 内的元素**全部无效**，`slip X Y` 也只对部分按钮有效
   （见 `PenCG/src/dev-autostart.js:3-6` 的注释与 HANDOFF.md）。
3. 所以「人工完成一次登录」在不改代码的前提下**不可复现**。

因此本方案用 `PenCG/src/dev-autostart.js` 里的三个取证开关把这条路铺平。
它们都是**纯取证用的、默认全部为 `null`**，生产构建里等同空函数。

| 开关 | 作用 | 消费点 | 当前值 |
| --- | --- | --- | --- |
| `DEV_AUTOSTART` | app 启动后自动 `navTo` 到指定页（可先切栏目） | `index.vue:423 devAutostart()` | `{ page: 'play', ... }`（原值，非 bug2 用） |
| `DEV_AUTOLOGIN` | 登录页 boot 后自动填表 + 提交（走生产 `submit()`） | `login.vue:192 devAutoLogin()` | `null` |
| `DEV_LOGOUT` | app 启动后自动走生产 `logout()` 退回未登录 | `index.vue:401 devLogout()` | `null` |

> ### ⚠ 三个开关的收尾义务
> 1. **取证结束后必须把三个都改回 `null`**（`DEV_AUTOSTART` 按项目惯例也归 null）。
> 2. `DEV_AUTOLOGIN` 里会写明文密码 —— 只要它非 null，**打出来的包就不能外发**。
> 3. 三个开关都在 `git status` 里显示为 `?? src/dev-autostart.js`（未跟踪），
>    不会误入别人的提交；但也意味着**它不会被 git 保护**，删掉就没了。

---

## 一、前置：把设备退回「未登录」

两条路，**优先 A**。

### A. 用 `DEV_LOGOUT`（推荐）

改 `PenCG/src/dev-autostart.js`：

```js
export const DEV_LOGOUT = { delay: 3000 };
```

构建 + 装机 + 启动（三段，见 §二 的通用命令），等首页 boot 完（`delay` 之后）。

**它做了什么**：`index.vue:401 devLogout()` → `setTimeout(3000)` →
`logout()`（`PenCG/src/services/auth.js:211`）→ 内部
`clearSession()`（清本进程 `cg.js` 的 `G`）→ `await remove(SESSION_KEY)`（删盘上
`pencg_session`）→ `await bumpSessionVersion()`（`auth.js:214`，版本号 +1）。

**为什么比改 json 好**：

- `pencg_session` 在 `preferences.json` 里是**「JSON 字符串里再套一层 JSON」**，
  外部改完**只改了盘**，app 进程内存里 `cg.js` 的 `G` 还留着旧 token →
  界面仍显示已登录，非常容易得出"没退干净"的假证据（HANDOFF.md 已记过这个坑）。
- 走 `logout()` 是**本进程内一致**的，且它顺手把版本号推进一格 —— 这一格本身
  就是 §四 要观测的量。

**风险 / 注意**：

- `delay` 给 3000ms 是因为 `index.vue boot()` 里 `await restoreSession()`
  与 `await loadHome()` 都走网络/读盘，太早调 `logout()` 可能被随后的
  `restoreSession()` 又灌回来。**若日志显示退出后又恢复成已登录，把 delay 调到 6000。**
- 这一步**不需要**再点「退出登录」按钮（按钮在 `mine-body.vue:669` 的
  `runFunc(id)`（`'logout'` 分支在其内部），会弹二次确认框，而确认框的按钮**点不动**）。

**判据（退干净了）**：

```
[dev] autologout 完成（盘上 pencg_session 已删）
```
（`index.vue:410`）

再查盘（只读）：

```bash
./env/adb/adb.exe -s 9D91500007505171 shell \
  "cat /userdisk/miniapp/data/mini_app/pkg/8002026110200003/data/sharedpreferences/preferences.json" \
  | tr ',' '\n' | grep -i pencg_session
```

期望：`pencg_session` 的值为 `""` 或该键不存在。
**不要**用 `sed` / `echo` 去改这个文件（HANDOFF.md 明确禁止）。

### B. 用 UI 的「退出登录」（不推荐，但可作为交叉验证）

进「我的」→ 点「退出登录」→ **弹窗点「确定」**。

- 弹窗按钮在 `mine-body.vue` 的 `mb-modal` 里，**是否可被 `slip` 点中未验证**。
  若点不动，这条路直接废，改走 A。
- 走 UI 的话，`logout()` 由 `mine-body.vue:659 doLogout()` 调，
  同样会 `bumpSessionVersion()` —— 效果与 A 等价，只是不写日志到 `[dev]` 前缀。

---

## 二、通用命令（每一版 `DEV_AUTOSTART` 都要重走一遍）

以下命令都在 `PenCG/` 目录下执行。

### 0. 先清场（关键，否则日志里混着上一轮）

```bash
# 记下清场时刻，后面 grep 从这个时间点往后看
date "+%Y-%m-%d %H:%M:%S"

# 只读：确认设备在线
../env/adb/adb.exe devices
```

**建议**：把 `/userdata/applog/loli_s6_dev.txt` 的一个快照记号记下来（行号），
后面用 `tail -n +行号` 只读新增部分。避免"把上一轮的成功日志当成这一轮的"。

```bash
../env/adb/adb.exe -s 9D91500007505171 shell "wc -l /userdata/applog/loli_s6_dev.txt"
```

### 1. 改开关 + 构建

```bash
cd /d/Users/Daniel/code/DSH/x6pro/haas-x6pro/PenCG
node ../env/node_modules/aiot-vue-cli/src/cli.js -q -p
```

期望末三行：

```
libs 同步成功: ...\PenCG\.falcon_\libs
应用构建成功,目录: ...\PenCG\.falcon_
打包成功,路径:...\PenCG\8002026110200003.1_0_3.amr
```

两条固有 WARNING 忽略：`找不到theme "theme-dark"`（falcon-ui 未装）、
`未找到以下模块: brightness,global,cgnative,cgmedia`（原生模块）。

### 2. 装机 + 启动

**推荐用脚本**（它把 push / install / 校验 / 启动 / 日志都串好了）：

```powershell
cd D:\Users\Daniel\code\DSH\x6pro\haas-x6pro\PenCG
powershell -NoProfile -ExecutionPolicy Bypass -File tools/deploy.ps1 -Page index -NoBuild -Shot
```

- `-NoBuild`：刚构建过，别重复构建。
- `-Shot`：顺带截一张图（**但要注意 `capture` 截的是 Falcon UI 合成面
  960x266，不含 KMS 视频平面**；<5000 字节基本等于黑屏/被抢屏）。
- 脚本内部会 `miniapp_cli start <desktop>` 退回桌面再 `start <appid> index`，
  这是为了确保**冷启动**（`boot()` 会跑）。

手工等价命令（脚本出问题时的退路）：

```bash
../env/adb/adb.exe -s 9D91500007505171 push 8002026110200003.1_0_3.amr /userdisk/miniapp/data/pencg.amr
../env/adb/adb.exe -s 9D91500007505171 shell "miniapp_cli install /userdisk/miniapp/data/pencg.amr"
#   ↑ 必须出现 "ret":0，否则装机失败，后面全是白测
../env/adb/adb.exe -s 9D91500007505171 shell "miniapp_cli start 8080222437664451"
../env/adb/adb.exe -s 9D91500007505171 shell "miniapp_cli start 8002026110200003 index"
```

> **启动命令不带 `--`**（`miniapp_cli start 8002026110200003 index`）。

### 3. 取日志 / 截图

```bash
# 全部 pencg 日志
../env/adb/adb.exe -s 9D91500007505171 shell \
  "grep -h '\[pencg\]' /userdata/applog/loli_s6_dev.txt | tail -n 80"

# 只取本次相关（登录态 + 开发钩子 + 导航）
../env/adb/adb.exe -s 9D91500007505171 shell \
  "grep -hE '\[pencg\].*(\[login\]|\[auth\]|\[mine\]|\[index\]|\[nav\]|\[dev\])' /userdata/applog/loli_s6_dev.txt | tail -n 60"
```

> **只有 `console.error` 落盘**（`PenCG/src/services/log.js:1-9` 有实测记录）。
> `log.js` 的 `push()` 一律走 `console.error`，行内靠 `[pencg][I|W|E]` 前缀区分级别。
> 落盘行的固定形态是 `[时间戳][E][JS] [pencg][W] ...`，中间那个 `[E]` 是**通道级别**（恒为 E），
> 不是我们想表达的严重程度 —— **别被它误导**。

```bash
# 截图（MSYS 下 adb pull /tmp 会被改写，必须 powershell 包一层）
../env/adb/adb.exe -s 9D91500007505171 shell "hal-screen on"
../env/adb/adb.exe -s 9D91500007505171 shell "miniapp_cli capture /tmp/a.png"
powershell -NoProfile -Command "& '.\env\adb\adb.exe' pull /tmp/a.png 'PenCG\profiles\evidence\bug2.png'"
```

---

## 三、主实验（核心，一条链证明"登录后不点首页也显示已登录"）

### 3.1 实验设计意图

要复现的正是用户报的那条路：

> 在「我的」栏目里点「去登录」→ 登录页登录成功 → 自动跳回 → **不碰左侧边栏** →
> 「我的」页应当直接显示昵称 / 剩余时长 / 头像。

修复前这条路会停在「还没有登录网易云游戏」，而**点一下左侧「首页」再点「我的」**
就好了 —— 因为那样会销毁重建组件，触发 `boot()` 里的 `restoreSession()`。
所以本实验的**关键约束是：跳回之后绝对不要碰左侧边栏**，等 10 秒看它自己变不变。

### 3.2 实验前置状态

1. 已按 §一 A 退到未登录（盘上 `pencg_session` 为空）。
2. `DEV_LOGOUT` 改回 `null`（否则会和主实验打架）。
3. `DEV_AUTOLOGIN` 设为：

```js
export const DEV_AUTOLOGIN = {
  phone: '<账号手机号>',
  pwd: '<账号密码>',
  cc: '86',
  delay: 1200,
};
```

4. `DEV_AUTOSTART` 设为（**先在「我的」栏目，再进登录页** —— 这是用户真实路径）：

```js
export const DEV_AUTOSTART = { page: 'login', delay: 2500, tab: 'mine', params: {} };
```

> `tab` 字段是本轮新加的（`index.vue:423 devAutostart()`）：先 `setTab('mine')`
> 让 `mine-body` 挂载并处于「还没有登录」状态，600ms 后再 `navTo('login')`。
> 这样「登录页是 push 上来的、index 还活着且 tab 已经是 mine」这个条件才成立，
> 才是最接近用户报障的现场。

5. 构建 → 装机 → **冷启动**（走 §二 的 0/1/2 步）。

### 3.3 时间线（每步期望看到的日志）

启动 `index` 后，按时间顺序应当看到（时间戳为示意）：

| # | 时刻 | 日志（`grep '\[pencg\]'` 后） | 源码位置 | 含义 |
| --- | --- | --- | --- | --- |
| 1 | +0.0s | `[index] 推荐模块 N 个, banner M 个` | `index.vue:474` | 首页加载完 |
| 2 | +2.5s | `[dev] autostart → login in 2500ms tab=mine` | `index.vue:427` | 钩子挂上 |
| 3 | +2.5s | `[dev] autostart 先切栏目 tab=mine` | `index.vue` | 落到「我的」 |
| 4 | +2.5s~ | `[mine] boot logged=false` | `mine-body.vue:482` | **修复前基线**：组件在这一刻确定自己是未登录 |
| 5 | +3.1s | `[dev] autostart fire → login` | `index.vue:439` | push 到登录页 |
| 6 | +3.2s | `[login] ready phone=186****` | `login.vue:185` | 登录页 boot 完 |
| 7 | +3.2s | `[dev] autologin 将在 1200ms 后提交 phone=186****` | `login.vue:206` | 自动登录已排期 |
| 8 | +4.4s | `[dev] autologin fire → submit()` | `login.vue:217` | 走生产 submit |
| 9 | +5.0s | `[auth] 密码登录成功 uid=67d9433bc649c953187c4ee8` | `auth.js:176` | **会话落盘 + bump 版本** |
| 10 | +5.0s | `[dev] autologin 将在...`（无） / `[login] success uid=...` | `login.vue:486` | submit 里那行 |
| 11 | +5.6s | `[nav] closePageByName login` | `nav.js:141` | **`navBack('login')` 成功** |
| 12 | +5.6s | `[index] 会话版本 X -> Y 本地登录态 false -> true 会话年龄 Nms` | `index.vue:273-285` | **checkSeq 被 onShow 触发**（第二道防线） |
| 13 | +5.6s | `[mine] refresh logged=true booted=true` | `mine-body.vue:527` | **★ 核心判据：refresh 读到会话了** |
| 14 | +6.0s | `[mine] @me ok free=0 vip=false` | `mine-body.vue:599` | 资料拉回来了（`@me` 成功） |
| 15 | +5.6~8s | `[mine] session 版本变化 X -> Y，重载` | `mine-body.vue:397` | 第 13 行没出现时的兜底（第 ④ 道防线） |

### 3.4 判据

**修好（PASS）** —— 必须同时满足：

1. **不碰任何 UI**，跳回后 10 秒内，屏幕上「我的」页显示：
   - 头像圆圈里的**昵称首字**
   - **昵称**（形如 `用户aaabOhXYY`）而不是「还没有登录网易云游戏」
   - `免费时长 0 分钟`（该账号 `freeTimeLeft=0`）或实际剩余时长
   - `会员权益` 卡片
2. 日志里**出现** `[mine] refresh logged=true`（**这是最关键的一行**）。
   它证明：`refresh()` 跑到了 `reloadLocal()` 之后，且 `logged` 为 `true`。
   - 修复前同一位置会是 `[mine] refresh logged=false`（或这行根本不出现，
     因为 `refresh()` 压根没被调用）。
3. 日志里**出现** `[mine] @me ok free=`（证明真的联网拉到了用户资料，
   而不只是本地标志位翻了个身）。

**没修好（FAIL）**，分两种失败模式，**必须能区分**：

| 现象 | 日志特征 | 指向 |
| --- | --- | --- |
| **(i) 登录成功但 mine 读不到 session**（原 bug） | 有 `[auth] 密码登录成功`、有 `[nav] closePageByName login`、有 `[mine] refresh logged=false`，且**没有** `[mine] @me ok` | `refresh()` 跑了但 `restoreSession()` 没把会话读进本上下文 —— 根因修复失效 |
| **(ii) 版本号机制没触发刷新**（修复一期引入的问题） | 有 `[auth] 密码登录成功`，但**完全没有** `[mine] refresh`，也**没有** `[index] 会话版本`、**没有** `[mine] session 版本变化` | 三道兜底全军覆没：`onShow` 没回调 + 两条轮询也没发现版本变化 |
| **(iii) 跳回后界面卡在「正在读取账号…」** | 有 `[mine] refresh logged=true` 但没有 `[mine] @me ok` | `refreshProfile()` 网络失败或 `busy` 互斥把后到的挡了（看 `[mine] @me fail`） |
| **(iv) 被误踢回未登录** | `[mine] token 失效，清除会话踢回未登录 status=401 code=1006` | 会话其实没落盘成功 / 账号被易盾锁（`1125 need_yidun_validate_fail_enough`） |

> **(i) 与 (ii) 的区分点就是有没有 `[mine] refresh` 这一行**：
> 有 → refresh 被执行了，问题在"读到什么"；没有 → 问题在"有没有人叫它"。
> 这条判据是写成本方案的唯一目的，务必分清。

### 3.5 "一次就成功"怎么排除侥幸

单次 PASS 不能证明补丁生效（也许本来就是好的）。必须做**负向对照**，
按可信度从高到低：

#### 对照 1（最强，但需要重编）—— 临时摘掉根因修复

把 `PenCG/src/components/mine-body.vue:514-518` 的

```js
          try {
            await restoreSession();
          } catch (e) {
            logWarn('[mine] refresh restore fail ' + ((e && e.message) || e));
          }
```

整段**注释掉**（只注释 `refresh()` 里这一份，`boot()` 里 `:470` 那份保留），
重编 + 装机 + 重跑 §三 全流程。

期望：**复现用户报的 bug** —— 日志出现 `[mine] refresh logged=false`，
界面停在「还没有登录网易云游戏」，**此时点左侧「首页」再点「我的」就正常了**
（那会重建组件走 `boot()`）。

→ 这就同时证明了两件事：① bug 的根因就是这个缺失的 `restoreSession()`；
② 本实验的判据确实能观测到它。

**做完必须把代码改回原样并重编**（这是唯一一处允许临时改被测代码的地方，
且只在对照实验里，做完立刻还原）。

#### 对照 2（不用重编）—— 冷启动不该被拽走

`DEV_AUTOLOGIN = null`、`DEV_LOGOUT = null`、
`DEV_AUTOSTART = null`，盘上保持**已登录**状态，冷启动 index。

期望：
- **不应**自动跳到「我的」（`checkSeq()` 的 `fresh` 门控 + `seqReady` 门控在挡）。
- `[mine] boot logged=true`（若此时 tab 是 mine）。
- 屏幕上停在首页推荐栏目。

若被拽到「我的」→ 说明 `index.vue:271-272` 的误跳保护失效
（`sessionAge()` 返回了 `>= 0 && < 600000` 的值）。
这不会导致"显示未登录"，但会让每个已登录用户每次开 app 都被跳走，属于不可接受。

#### 对照 3（最弱，只做观测）—— 不登录直接返回

`DEV_AUTOLOGIN = null`，前置状态为**未登录**，`DEV_AUTOSTART` 仍跳到 login 页，
然后什么都不做（人手动按顶栏「返回」，或等 15s 后 `navBack` 由其他路径触发）。

期望：跳回「我的」仍是「还没有登录网易云游戏」，日志有
`[mine] refresh logged=false`（或 `boot logged=false`）。

这条只证明"未登录时不会凭空变成已登录"，排除"版本号机制把 UI 强制点亮"的假象。

---

## 四、定点验证（每条防线单独证一次，可选但推荐）

主实验证明的是"整条链能通"，但通的是哪条防线并不知道。想分清就跑下面四条，
每次只开一路。

| 编号 | 防线 | 怎么造出"只有这一路" | 判定日志 |
| --- | --- | --- | --- |
| ① | `setPendingTab` + `onShow` 的 `takeTabParam` 分支 | 正常主实验即可（登录页会写标记） | `[nav] closePageByName login` 之后紧跟 `[index] 会话版本` 与 `[mine] refresh logged=true` |
| ② | `onShow` 里的 `checkSeq()`（`index.vue:204`） | 无法单独隔离（和 ① 同时触发）。**能证明它在跑**：只要看到 `[index] 会话版本` 这行就是它 | `[index] 会话版本 X -> Y 本地登录态 false -> true 会话年龄 Nms` |
| ③ | index 的 2.5s 轮询（`index.vue:349 startSeqWatch()`） | 把 `takeTabParam` 那条废掉：临时在 `login.vue:257` 把 `setPendingTab('mine')` 注释掉 → ① 失效。**若仍能刷新，说明 ②③ 兜住了** | `[index] 会话版本`（来自轮询，时间上比 ① 晚 ~2.5s） |
| ④ | mine-body 的 2.5s 轮询（`mine-body.vue:373`） | 把 index 三条都废掉（`setPendingTab` 注释 + `index.vue:204 this.checkSeq();` 注释 + `index.vue:349` 的 `startSeqWatch` 不调） | `[mine] session 版本变化 X -> Y，重载` 后接 `[mine] refresh logged=true` |

> ③④ 是"最坏情况"的兜底，也是**唯一不依赖 `onShow` 回调**的通道。
> 它们若成立，就顺带回答了「`closePageByName` 之后 index 到底收没收到 `onShow`」
> 这个悬而未决的问题（见 [BUGFIX-bug2-login.md](BUGFIX-bug2-login.md) 断点 3）：
> **如果只看到 ④ 的日志、看不到 ② 的**，说明 `onShow` **没有**被回调。

同理，`$falcon` 是否可写也能从日志判定：

- 看到 `[index] 会话版本` → `checkSeq()` 至少看到版本变了（它走的是
  `sessionVersionAsync()`，三处取 max，**不单独证明 `$falcon` 可写**）。
- 想单独证 `$falcon` 可写：`DEV_AUTOSTART` 跳 `diag` 页（若有），或用
  §六 的"失败时排查"里那招 —— 更简单的办法是看 `[auth] 会话版本号落盘失败`
  **不该出现**（`auth.js:123`），它只在 `setString` 失败时打。

---

## 五、完整执行顺序（建议照抄）

```
[准备]
 0. 记下 loli_s6_dev.txt 当前行数（记作 N）
 1. dev-autostart.js: DEV_LOGOUT={delay:3000}; DEV_AUTOLOGIN=null; DEV_AUTOSTART=null
 2. build + deploy(-NoBuild) + 冷启动 index
 3. 等 8s，grep 日志：应见 [dev] autologout 完成
 4. 查盘确认 pencg_session 已空

[主实验]
 5. dev-autostart.js: DEV_LOGOUT=null;
    DEV_AUTOLOGIN={phone:'<账号手机号>',pwd:'<账号密码>',cc:'86',delay:1200};
    DEV_AUTOSTART={page:'login',delay:2500,tab:'mine',params:{}}
 6. build + deploy(-NoBuild) + 冷启动 index
 7. ★ 全程不碰屏幕，等 15s
 8. grep 日志，逐条对 §3.3 的表格
 9. 截图：看「我的」页是否有昵称/剩余时长/头像
10. 判据：§3.4 的 PASS 三条

[对照]
11. 若 PASS：做 §3.5 对照 2（关掉所有钩子、盘上保持已登录、冷启动，确认不被拽走）
12. 若 PASS 且有把握：做 §3.5 对照 1（注释掉 refresh 里的 restoreSession，复现 bug）
13. 若 FAIL：走 §六

[收尾]  ← 这一步最容易忘，务必执行
14. dev-autostart.js: 三个开关全部改回 null
15. build（确保包干净）+ 记下 amr 大小
16. 把结论补进 BUGFIX-bug2-login.md / HANDOFF.md
```

---

## 六、失败时看什么（排查入口）

### 6.1 先定位"断在哪一段"

按下面顺序 grep，**第一个空掉的地方就是断点**：

```bash
A=9D91500007505171
L=/userdata/applog/loli_s6_dev.txt
ADB=../env/adb/adb.exe

# ① 登录页有没有被拉起
$ADB -s $A shell "grep -hE '\[pencg\].*(\[dev\] autostart fire|\[login\] ready)' $L | tail -n 5"

# ② 登录请求有没有成功（会话有没有落盘）
$ADB -s $A shell "grep -hE '\[pencg\].*(\[login\] success|\[auth\] 密码登录成功|\[login\] fail)' $L | tail -n 5"

# ③ 有没有关掉登录页（回到 index）
$ADB -s $A shell "grep -hE '\[pencg\].*\[nav\]' $L | tail -n 10"

# ④ index 有没有察觉版本变了（防线②③）
$ADB -s $A shell "grep -hE '\[pencg\].*\[index\] 会话版本' $L | tail -n 5"

# ⑤ mine-body 有没有真的刷新、读到什么（防线④ + 根因修复）
$ADB -s $A shell "grep -hE '\[pencg\].*(\[mine\] refresh|\[mine\] session 版本变化|\[mine\] boot)' $L | tail -n 10"
```

### 6.2 各段空掉的含义

| 空在哪 | 含义 | 下一步 |
| --- | --- | --- |
| ① | 钩子没生效 / 装机没成功 / app 没冷启动 | 查 `miniapp_cli install` 有没有 `"ret":0`；查 `.falcon_/` 的 mtime；确认 `-NoBuild` 之前真的 build 过 |
| ② | 登录失败 | 看 `[login] fail ... | detail=...`，`detail` 是易盾/风控原文。若是 `1125` / `1122`，**账号被锁，本次无法继续**，等解锁（HANDOFF.md:50-53） |
| ③ | `closePageByName` 失败 | 看 `[nav] $falcon.closePageByName 不存在`（→ API 名字错了）或 `[nav] closePageByName 失败:` / `[nav] 无法关闭页面 name=login`（→ 页面名对不上）。**这是最要命的一种**：登录页留在栈上，index 永远不 onShow |
| ④ | `onShow` 里的 `checkSeq` 没跑（断点 3 成立）+ 轮询也没醒 | 先看 ⑤；若 ⑤ 有内容而 ④ 没有，就是 `onShow` 没回调（此时靠 ⑤ 兜住也算 PASS） |
| ⑤ | `refresh()` 压根没被调用 | 三条通道全废。查 `index.vue:204/349` 与 `mine-body.vue:373` 是否真在跑；查 `setPendingTab` 是否被别处取走 |

### 6.3 `refresh logged=false` 但 `[auth] 密码登录成功` 也在

这是**最接近原 bug 的失败**，说明根因修复没起效。依次查：

1. **盘上到底有没有会话**（可能在登录页就写失败了）：
   ```bash
   $ADB -s $A shell "cat /userdisk/miniapp/data/mini_app/pkg/8002026110200003/data/sharedpreferences/preferences.json" | tr ',' '\n' | grep -A1 pencg_session
   ```
   期望看到一长串带引号的 JSON（token 是 `eyJ0eXAiOiJKV1Qi...`）。
   若为空 → 问题在 `saveSession()`（`PenCG/src/services/auth.js:128`，调用点 `:174`），不在本次修复范围。

2. **`restoreSession()` 是不是抛了**：看 `[mine] refresh restore fail ...`
   （`mine-body.vue:517`）。有的话日志里带异常 message。

3. **storage 是不是超时了**：`storage.js:45` 的 `withTimeout(..., 3000)`
   在真机上若 `getStorage` 每次都 >3s，`getString` 会回落 `MEMORY`
   （本页可能是空）→ `getJSON` 返回 `null` → `restoreSession()` 返回 false。
   **判据**：`[mine] refresh logged=false` 每隔 2.5s 反复出现、且
   `[auth] 恢复会话` 这行（`auth.js:207`）**一次都没有** → 说明
   `restoreSession()` 每次都在 `if (!s || !s.token) return false` 处提前返回，
   **就是读盘没读到**。
   → 处理：把 `storage.js:45` 的超时从 3000 调大，或改用同步内存兜底。

4. **是不是另一个 JS 上下文**：这是本次修复的**核心假设**
   （HANDOFF.md:220-222「跨页面不共享模块状态」）。若假设不成立
   （即两页其实共享 `cg.js` 的 `G`），那修复前就应该是好的、bug 无法解释。
   **可以直接验证**：在 `index.vue` 与 `login.vue` 各打一行
   `logInfo('[ctx] G.token=' + (currentSession() ? '1' : '0'))`，
   看登录前后两页是不是各自独立变化。

### 6.4 `refresh logged=true` 但界面还显示未登录

说明 `logged` 已经是 true，但模板没跟着更新。查：

- `nickname` / `initial` 是 `computed`（`mine-body.vue` 顶部），
  而 **`index.vue:150` 的注释记着「这个运行时对 computed 的模板求值不可靠」**，
  所以项目里计数文案刻意走了 data。若这里 computed 真的不更新，
  就是**同一类运行时缺陷**在 mine-body 上复发 —— 需要把 `nickname` / `initial` /
  `maskedId` / `timeText` 等改成 data + 手动赋值。
  **判据**：`refresh logged=true` 出现后，日志里 `[mine] @me ok free=...`
  也有，但截图仍是「还没有登录」→ 就是这个。
- 或者 `booted` 还是 false（模板 `v-else-if="!booted"` 先命中）：
  看 `[mine] boot logged=...` 有没有打出来。

---

## 七、本方案自身的已知不确定性（诚实清单）

写这份方案时**没有一行真机日志**支撑，以下是纯推断、**可能在真机上不成立**的点：

1. **`navTo` 能否把 `login` 页 push 起来**（`DEV_AUTOSTART` 走的就是 `navTo('login', {})`）。
   `nav.js:88` 的 `navTo` 是对 `$falcon.navTo` 的薄封装，
   而 `$falcon.navTo` 在本固件上**是否接受 `login` 这个页面名**未验证
   （`PAGES` 映射里有 `login: 'login'`，且 `mine-body.vue:630` 的 `goLogin()`
   就是 `navTo('login', {})`，所以**大概率**可行）。
2. **`DEV_AUTOSTART` 的 `tab` 字段是本次新加的**（`index.vue:423`），
   `setTab('mine')` 会 `return`（`index.vue:598 setTab()`），
   随后 600ms 才 `navTo` —— 若 `mine-body` 的 `boot()` 比 600ms 慢，
   可能出现"组件还没 boot 完就 push 走了"，日志会看到 `[mine] boot logged=false`
   **在** `[dev] autostart fire` 之后。不影响判据，但读日志时别混淆。
3. **`devAutoLogin` 的跳过条件依赖 `statusText`**（`login.vue:200-202`）：
   它用 `this.statusText.indexOf('未登录') < 0` 判断"已登录"。
   若 `refreshStatus()` 还没跑完（`statusText` 仍是初值），判断会失效。
   实际上 `boot()` 里是先 `await this.refreshStatus()` 再 `this.devAutoLogin()`，
   所以理论上没问题 —— 但这是本轮新代码，尚未真机跑过。
4. **`logout()` 的 `delay` 3000ms 是否够**：`index.vue boot()` 里
   `await restoreSession()` + `await loadHome()` 两步都可能慢，
   3s 可能不够（尤其弱网）。太早 `logout()` 会被后续 `restoreSession()` 灌回来。
   **看到"退出后又变已登录"就把 delay 加到 6000。**
5. **`capture` 截图可能拿不到「我的」页**：`miniapp_cli capture` 截的是
   Falcon UI 合成面（960x266），**如果那一刻有 KMS 视频平面在播会混**。
   本实验不播视频，理论上没问题；但 <5000 字节就当失败重截。
6. **`hal-screen` 的灭屏**：`270s` 自动灭屏、播放态需每 20s 保活
   （`DEV.md:86-87`）。本实验全程在首页/登录页/我的页，不播放，
   但**整个流程若超过 4.5 分钟没交互可能灭屏**，看起来像"卡住了"。
   真卡住先 `hal-screen on` 再判断。

---

## 八、完成后必须做的事

1. **三个开关全部改回 `null`**（`DEV_AUTOSTART` / `DEV_AUTOLOGIN` / `DEV_LOGOUT`）。
2. **重新构建一次**，确认 amr 里不再含明文密码
   （可 `grep -a '<账号密码>' 8002026110200003.1_0_3.amr` 应为空，
   `.falcon_/login.js.bin` 同理）。
3. 结果写回：
   - [BUGFIX-bug2-login.md](BUGFIX-bug2-login.md) 的「真机验证状态」一节
     （改成已验收 + 关键日志摘录 + 失败的防线编号）。
   - `PenCG/HANDOFF.md` 的待办清单（L61-66 那条「登录成功提示未真机验收」
     与本条可以合并处理）。
4. 如果验证出「`onShow` 在 `closePageByName` 后没有被回调」，
   这条结论要单独记进 HANDOFF.md —— 它影响的不止 bug2，
   所有依赖 `onShow` 的页面刷新逻辑都受影响。
