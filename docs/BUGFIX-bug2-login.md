# 修复：登录成功后跳回仍显示「未登录」，点首页再点回来才有用户信息

> 用户反馈（bug 2）：
> 「登录界面登录成功后跳转回去显示"未登录"，只有点击左侧边栏首页再点回来才能加载出用户信息。」

复现基线：设备 9D91500007505171（X6 Pro，slot a，appid 8002026110200003），
盘上 `pencg_session` 里确实有完整有效的登录态（token / userId / nickname /
encrypt / offset / freeTimeLeft / vip / cc / loginAt 全齐）。
所以**这不是「没存下」，是「界面没重新读」**。

---

## 一、完整调用链与断点

### 1.1 登录成功这一侧（全部正常）

| 步骤 | 位置 | 做什么 |
|---|---|---|
| ① 点「登录」 | `PenCG/src/pages/login/login.vue:422` `async submit()` | 校验手机号/密码，`busy = true` |
| ② 发登录请求 | `PenCG/src/pages/login/login.vue:441` `await loginByPassword(...)` | 进 `auth.js` |
| ③ 取 token | `PenCG/src/services/auth.js:144` `applyLogin(j)` | 写 `cg.js` 的 `G`（**登录页自己这份 JS 上下文的 G**） |
| ④ 落盘 | `PenCG/src/services/auth.js:146` `await saveSession({phone, cc})` | 经 `storage.js:80` `setJSON` 写 `pencg_session` |
| ⑤ **新增** 版本号 | `PenCG/src/services/auth.js:147` `await bumpSessionVersion()` | 见下文 §4.1 |
| ⑥ 提示 | `PenCG/src/pages/login/login.vue:450-452` | `statusText = '登录成功，正在跳转…'` + toast |
| ⑦ 延时 600ms 后返回 | `PenCG/src/pages/login/login.vue:454` `setTimeout(..., 600)` → `:462` `goBackToMine()` | 等 toast 看得见 |
| ⑧ 写 pending + 关自己 | `PenCG/src/pages/login/login.vue:217` `setPendingTab('mine')`；`:221` `navBack('login')` | → `nav.js:46` / `nav.js:124` → `$falcon.closePageByName('login')`（`nav.js:129`） |

**这一侧没有 bug。** 会话确实写进了盘，导航也确实关了登录页。

### 1.2 回到 index 这一侧（断在这里）

index 页面对「登录成功」这件事的**全部**感知手段，原本只有两处：

**入口 A —— `onShow` 读 pending 标记**（`PenCG/src/pages/index/index.vue:187-219`）

```
186  onShow() {
187    let t = '';
188    try {
189      t = takeTabParam();          // ← 断点 1
190    } catch (e) {
...
193    if (t && t !== this.tab) {
194      this.setTab(t);
...
201      this.$nextTick ? this.$nextTick(this.refreshMineBody) : this.refreshMineBody();
204      return;
205    }
206    if (this.tab === 'mine') this.refreshMineBody();
213    this.checkSeq();               // ← 新增（原代码没有）
214  },
```

**入口 B —— `setTab('mine')` 已在 mine 栏目时刷一次**（`PenCG/src/pages/index/index.vue:543-549`）

```
505    if (id === 'mine') {
506      if (this.tab !== 'mine') {
507        this.tab = 'mine'; this.title = '我的'; this.errText = '';
510        return;                    // 首次进入：靠 mine-body 自己 mounted 的 boot()
511      }
514      const b = (this.$refs && this.$refs.body) || (this.$root && this.$root.__mineBody) || this.__mineBody;
515      if (b && b.refresh) { try { await b.refresh(); } catch (e) { ... } }
518      return;
519    }
```

断点有 **三处**，任意一处单独出现都足以造成用户看到的现象：

#### 断点 1：`takeTabParam()` 读不到登录页写下的标记（`nav.js:59`，原 L27-39）

`setPendingTab()` 原实现只写模块级变量 `let pendingTab = null`（`nav.js:35`）。

而 HANDOFF.md L220-222 已经记过同一类坑：
> `requestHome/takeHomeHop/clearHomeHop` 接力旗 **没有用上，且跨页面不共享模块状态**。

登录页和 index 页是**两份独立的 JS 上下文**，各有自己的模块实例。
登录页 `setPendingTab('mine')` 写的是登录页那份模块的 `pendingTab`；
index 页 `takeTabParam()` 读的是 index 那份模块的 `pendingTab` —— **恒为 `null`**。
于是 `t = ''`，入口 A 的整个 `if (t && t !== this.tab)` 分支被跳过。

> 补充：即便上下文共享，这个标记也是「取一次即销 + 30 秒过期」的
> （`nav.js:59` `takeTabParam` 里 `g[TAB_KEY] = null`），
> 详情页/搜索页（`detail.vue:403`、`search.vue:400`）也会调 `setPendingTab`，
> 任何一次抢先取走都会让登录这次落空。

#### 断点 2（**根因**）：`refresh()` 只读本页内存快照，不重新读盘

即使入口 A 或入口 B 成功跑到了 `refreshMineBody()` → `mine-body.refresh()`，
**界面仍然显示未登录**，因为修复前的 `refresh()`（`PenCG/src/components/mine-body.vue:425`）是：

```js
async refresh() {
  await this.reloadLocal();       // ← 只读 currentSession()
  ...
}
async reloadLocal() {             // :441
  const s = currentSession();      // ← cg.js:152 的 G 快照，纯内存
  this.logged = !!(s && s.token);
```

`currentSession()` → `auth.js:106` → `cg.js:152` `session()`，读的是**本 JS 上下文**里
`cg.js` 的 `G` 对象。登录页 `applyLogin()` 灌的是**登录页那份** `G`；
index 页这份 `G` 从头到尾没被碰过，`G.token` 仍是空串。

所以：`logged = false` → 模板走 `v-else-if="!logged"`
（`mine-body.vue:27-35`）→ 一直是「还没有登录网易云游戏」。

**这是最根本的一环。** 它解释了为什么前两轮修复（`navBack` 改 `closePageByName`、
`onShow` 去掉提前 `return`）都没真正解决问题 —— 那两次修的都是**调用有没有发生**，
而这里是**调用发生了也没用**。

#### 断点 3：`onShow` 在 closePageByName 之后不一定被调用

登录页是 `navTo` **push** 上来的，index 页一直在栈底、从未销毁。
`$falcon.closePageByName('login')` 之后 index 是否重跑 `onShow`，
取决于宿主是否给被压住的页面补一次 `onShow` 回调。

`base-page.js:72-75` 里 Vue 的 `onShow` 是由页面基类转发的：

```js
onShow() {
  super.onShow();
  if (this.$root && this.$root.onShow) this.$root.onShow();
}
```

若基类 `onShow` 没被宿主回调，index.vue 的 `onShow()` 整段**一行都不会执行**，
入口 A 连进场的机会都没有。这一条在本固件上**没有实证**（见 §6 真机验证第 3 步），
所以补丁不能依赖它。

### 1.3 mine-body 侧：没有任何「会话变了」的感知机制

修复前的 `mine-body.vue`：

- **没有 `watch`**（组件里一个 `watch` 都没有）；
- `mounted()`（`mine-body.vue:312`）只往 `$root.__mineBody` 挂引用然后 `boot()`；
- 数据全靠外部主动调 `refresh()`。

而 `auth.js` / `cg.js` 侧也**没有任何订阅/广播/版本号**：
`loginByPassword` 只写 `G` + storage，谁都不知道。

**一旦错过那一次性的 `refresh()` 调用，就永远没有第二次机会** ——
界面对「会话变化」完全没有收敛机制。

### 1.4 断点汇总

```
登录成功 ──┬─ applyLogin → 登录页的 G          ✗ index 的 G 没变（断点 2）
           ├─ saveSession → 盘上有 token        ✓
           └─ setPendingTab('mine') → 登录页模块  ✗ index 读不到（断点 1）
                                   ↓
                        closePageByName('login')
                                   ↓
            index.onShow() 是否被调用？            ✗ 不保证（断点 3）
                                   ↓
                     refreshMineBody() → refresh()
                                   ↓
                     currentSession() = 空         ✗ 断点 2
                                   ↓
                        logged = false
                                   ↓
                     「还没有登录网易云游戏」
```

---

## 二、端到端链路（修复前 vs 修复后）

本节回答五个确定性问题，**不写「可能是」**。凡是没有真机实证的，明确标注置信度。

### 2.1 跳回的目标页是哪个？

**`index` 页，不是独立的「我的」页**（置信度：确定，代码路径唯一）。

`PenCG/src/pages/login/login.vue:221` 的 `navBack('login')` →
`PenCG/src/services/nav.js:124` `navBack()` → `:129` `$falcon.closePageByName('login')`
→ 登录页被弹出，露出栈底一直没被销毁的 `index` 页。

「我的」在 index 里是**内嵌区块**，不是独立路由：

```
PenCG/src/pages/index/index.vue:24
<div v-if="tab === 'mine'" class="ih-mine-wrap ih-fade">
  <mine-body ref="body"></mine-body>
</div>
```

`PenCG/src/app.json` 虽然登记了独立的 `pages/mine/mine.vue`，但
`PenCG/src/pages/index/index.vue:536-541` 的 `setTab('mine')` 只改 `this.tab`，
**不 navTo**（注释写明「原来跳过去后 index 被销毁……用户报的 #9」）。
所以登录返回后停留的永远是 index，走的一直是内嵌的 mine-body。
`PenCG/src/pages/mine/mine.vue` 是遗留薄壳，本次只做卫生清理。

### 2.2 谁负责拉用户信息？

两处，缺一不可（置信度：确定）：

| 内容 | 位置 | 拉到什么 |
|---|---|---|
| 登录态（是否已登录 / 昵称 / 剩余时长快照） | `PenCG/src/components/mine-body.vue` 的 `boot()`（`:413`）与 `refresh()`（`:448`）→ `reloadLocal()`（`:486`）→ `currentSession()`（`PenCG/src/services/auth.js:106`）→ `PenCG/src/services/cg.js:152` `session()` | `logged` / `headSub` / `nickname` computed / `initial` 头像首字 |
| 服务端实时资料（免费时长、VIP、头像 URL 等） | `PenCG/src/components/mine-body.vue:533` `refreshProfile()` → `v2Get('api/v2/users/@me')` | `user` 对象 → `timeText` / `vipName` / `blurayText` |

关键点：`refresh()` 修好之前**没有** `restoreSession()`，只有
`PenCG/src/components/mine-body.vue` 的 `reloadLocal()`（`:486`）。

### 2.3 修复前为什么拿不到会话？—— 确定答案

**不是**「会话还没写进内存」，**不是**「storage 读太慢」，**也不是**「页面复用旧 vnode 不重新 mount」。

真实原因：**那份会话被写进了「登录页」的 JS 上下文，而 index/mine-body 读的是「自己」那份上下文的对象，两者不是同一个对象。**

三步证据链：

1. `PenCG/src/services/cg.js:136` 的会话是一个**模块级常量对象**：
   ```js
   const G = { token: '', userId: '', encrypt: '', offset: 0, nickname: '', ... };
   ```
   `PenCG/src/services/cg.js:156` 的 `applyLogin()` 只往这个 `G` 上赋值，**不碰 storage**。
2. `PenCG/src/services/auth.js:123` 的 `loginByPassword()` 在登录页执行，
   `applyLogin(j)` 灌满的是**登录页这份模块实例的 `G`**。
   而 `PenCG/HANDOFF.md:220-222` 已经实证过本运行时「**跨页面不共享模块状态**」
   —— 每个页面是独立的 QuickJS 上下文，`cg.js` 被各自实例化一份。
3. index 页里的 mine-body 调 `reloadLocal()`（`PenCG/src/components/mine-body.vue:486`）
   → `currentSession()`（`PenCG/src/services/auth.js:106`）→ `session()`
   （`PenCG/src/services/cg.js:152`）读的是 **index 这份上下文的 `G`**。
   它从启动到此刻**从未被 `applyLogin()` 碰过**，`G.token` 仍是 `''`
   → `session()` 返回 `null` → `logged = false`
   → 模板走 `PenCG/src/components/mine-body.vue:27` `v-else-if="!logged"`
   → 显示「还没有登录网易云游戏」。

**决定性旁证**：盘上 `pencg_session` 是完整有效的（见文首复现基线），
`restoreSession()`（`PenCG/src/services/auth.js:175`）能把 `G` 灌满 —— 这正是
`boot()`（`:413`）一开头做的事。所以「同一份数据，boot() 能读到、refresh() 读不到」
的唯一解释就是 **refresh() 少了一次读盘**。这不是推测，是两个函数体直接对比的结果：

```
PenCG/src/components/mine-body.vue:413  boot()     开头有 await restoreSession()
PenCG/src/components/mine-body.vue:448  refresh()  修复前**没有**（现已补上）
```

置信度：**确定**。依据是对比两个函数的源码 + 盘上数据有效 + HANDOFF 已实证的
「跨页不共享模块状态」。唯一未经真机直接观测的环节是「两个页面的 `cg.js` 确实是
两份独立实例」——但 HANDOFF.md:220-222 与本次补丁的日志判据都能间接验证它。

### 2.4 修复后这条链路为什么每一步都成立

| 步 | 修复后发生什么 | 为什么一定成立 |
|---|---|---|
| ① 用户在登录页点登录 | `login.vue:441` `loginByPassword()` → `auth.js:123` | — |
| ② 内存 | `auth.js:144` `applyLogin(j)` 灌满**登录页**的 `G` | 同步赋值 |
| ③ 落盘 | `auth.js:146` `await saveSession(...)` → `storage.js:80` `setJSON` | await 完成才继续 |
| ④ 版本 +1 | `auth.js:147` `await bumpSessionVersion()` | **在 ③ 之后**（顺序已核）；三处写：`LOCAL_SEQ`、`$falcon[SEQ_KEY]`（`:87`）、盘上 `__pencg_session_seq`（`:93`） |
| ⑤ 600ms 后导航 | `login.vue:462` `goBackToMine()` → `setPendingTab('mine')`（`nav.js:46`）→ `navBack('login')`（`nav.js:129` `closePageByName`） | pending 标记双写 `$falcon[__pencgPendingTab]`（`nav.js:52`），跨页可见 |
| ⑥ index 露出 | 任一路径触发（`onShow` / 轮询 / mine-body 自身轮询），**至少一条**必然触发 | ③④ 只依赖 `setTimeout`，不依赖 onShow |
| ⑦ **读盘** | `mine-body.vue:470` `await restoreSession()` | ★ 根因补丁；`auth.js:175` 把盘上 token 灌进 **index 这份** `G` |
| ⑧ 判定 | `mine-body.vue:474` `await this.reloadLocal()` → `:489` `logged = !!(s && s.token)` → **true** | ⑦ 的 await 已完成（顺序天然串行） |
| ⑨ 渲染 | 模板走 `mine-body.vue:37` `v-else` 分支 | 昵称 `computed nickname`、头像 `initial`、时长 `timeText` |
| ⑩ 实时资料 | `mine-body.vue:478` `if (this.logged) await this.refreshProfile()` → `v2Get('api/v2/users/@me')` | ⑧ 已置 `logged=true` |

**为什么 ⑥ 一定有路可走**（这是本轮复核的核心加固点）：

| 通道 | 位置 | 依赖 |
|---|---|---|
| A. `onShow` + pending 标记 | `index.vue:187-219` | 宿主回调 onShow（**未实证**，断点 3） |
| B. `onShow` + 版本比对 | `index.vue:204` `this.checkSeq()` | 同上 |
| C. index 轮询 | `index.vue:321` `startSeqWatch()`，2.5s | 只要 `setTimeout` |
| D. mine-body 轮询 | `mine-body.vue:368` `startVersionWatch()`，2.5s | 只要 `setTimeout` |
| E. `onShow` 里 `if (this.tab === 'mine')` | `index.vue:218` | 同上 |

A/B/E 全废的情况下 C/D 仍成立，所以结果确定。C/D 的判定都收敛到
`mine-body.vue:470` 的 `await restoreSession()` —— 那一句就是根因补丁本身，
所以**感知通道修好了，根因也修好了，两者互为保险**。

---

## 三、为什么「点首页再点回来」就能刷新

这条路径绕开了全部三个断点，**靠的是组件重建，不是任何 refresh 通道**：

| 步 | 位置 | 发生了什么 |
|---|---|---|
| ① 点「推荐」 | `index.vue:532` `setTab('recommend')` → `:552` `this.tab = 'recommend'` | — |
| ② mine-body 被销毁 | `index.vue:24-26` 模板 `<div v-if="tab === 'mine'">`，条件转假 | 触发 `mine-body.vue:323-330` `beforeDestroy()`，清 `$root.__mineBody` |
| ③ 点「我的」 | `index.vue:532` `setTab('mine')` → `:536` `this.tab !== 'mine'` 成立 | 走 `:541` `return`（**不调 refresh**） |
| ④ v-if 重新为真 | 同上 | **挂载一个全新的 mine-body 实例** |
| ⑤ 新实例 `mounted()` | `mine-body.vue:312` | 挂 `$root.__mineBody`，调 `this.boot()` |
| ⑥ `boot()` 读盘 | `mine-body.vue:413` → `:425` `await restoreSession()` | ★ **从盘上把 token 灌进本页的 `G`** |
| ⑦ `reloadLocal()` | `mine-body.vue:486` → `currentSession()` 这次有值 | `logged = true` |
| ⑧ 拉资料 | `mine-body.vue:438` `if (this.logged) this.refreshProfile()` | 昵称/头像/时长全部出来 |

**关键差别只有一个**：`boot()` 里有 `await restoreSession()`（`mine-body.vue:425`），
而修复前的 `refresh()` 里没有。`boot()` 会读盘 → `G` 被灌满 → 登录态正确；
`refresh()` 不读盘 → `G` 还是空的 → 显示未登录。

所以用户观察到的「点首页再点回来就好了」，本质是
**「强制重建组件，从而跑到了唯一会读盘的那条初始化路径」**，
而不是「点了首页所以刷新了」。

---

## 四、最小补丁

四条改动，都落在允许改的范围内。核心思路：
**把「读盘」补进 `refresh()`（治根因），再把「会话变了」做成跨页可见的显式信号（治感知），
最后用一条与 `onShow` 无关的通道兜底（治断点 3）。**

### 4.1 `PenCG/src/services/auth.js` —— 跨页会话版本号

`auth.js:11-98` 新增（`SEQ_KEY` 在 `:30`，`SEQ_STORE_KEY` 在 `:31`）：

```js
const SEQ_KEY = '__pencgSessionSeq';
const SEQ_STORE_KEY = '__pencg_session_seq';
let LOCAL_SEQ = 0;

export function sessionVersion() {          // :55 同步版：只读 $falcon / 本模块
  return Math.max(readHost(), LOCAL_SEQ);
}
export async function sessionVersionAsync() { // :60 异步版：额外读一次盘
  ...
  return Math.max(readHost(), LOCAL_SEQ, st);
}
export async function bumpSessionVersion() {  // :72
  const n = (await sessionVersionAsync()) + 1;
  LOCAL_SEQ = n;
  /* 写 $falcon 与盘，两处都写 */
}
```

**为什么用递增整数而不是对象引用比较**（任务里点名要求）：
`cg.js:152` 的 `session()` 是

```js
export function session() { return G.token ? Object.assign({}, G) : null; }
```

每次调用都 `Object.assign` 出一个**新对象**，所以 `session() !== session()` 恒真。
用引用判「会话变没变」，要么永远触发（每次 onShow 都刷一遍）、
要么被误写成 `===` 永远不触发，在 QuickJS 下都不可靠。递增整数语义明确。

**为什么存三处**：
1. `LOCAL_SEQ` —— 本页内一致性；
2. `$falcon[SEQ_KEY]` —— 宿主全局对象，所有页面共用同一份（同步可读，首选）；
3. storage 键 `__pencg_session_seq` —— **万 `$falcon` 被宿主冻结/只读**，
   sharedpreferences 是唯一确定跨页可见的通道（异步，但轮询用得起）。

读的时候取三者最大值，谁写成功了都能被看见。

调用点：`auth.js:147`（密码登录）、`:169`（短信登录）、`:186`（退出登录）。

### 4.2 `PenCG/src/services/nav.js` —— pending 标记挪到宿主全局

`nav.js:34` 新增 `const TAB_KEY = '__pencgPendingTab';`，
`setPendingTab()`（`:46`）与 `takeTabParam()`（`:59`）改为**优先读写 `$falcon[TAB_KEY]`**，
模块变量保留作兜底。这直接修断点 1。

### 4.3 `PenCG/src/components/mine-body.vue` —— `refresh()` 补读盘（治根因）

`mine-body.vue:448-484`（含防重入锁，见下）：

```js
async refresh() {
  if (this.refreshing) { this.refreshAgain = true; return; }   // :455 合并并发
  this.refreshing = true;
  try {
    for (let round = 0; round < 2; round++) {                  // :461 最多补跑一轮
      this.refreshAgain = false;
      try {
        await restoreSession();                                // :470 ★ 根因修复
      } catch (e) {
        logWarn('[mine] refresh restore fail ' + ((e && e.message) || e));
      }
      await this.reloadLocal();                                // :474 必须在 :470 之后
      logInfo('[mine] refresh logged=' + this.logged + ' booted=' + this.booted);
      if (this.logged) await this.refreshProfile();            // :478
      if (!this.refreshAgain) break;                           // :479
    }
  } finally {
    this.refreshing = false;                                   // :482
    this.refreshAgain = false;
  }
}
```

**为什么必须防重入**：登录成功那一刻有三条路径同时叫 `refresh()` ——
`index.onShow` 的 `refreshMineBody()`（`index.vue:277`）、`index.checkSeq()`
（`index.vue:267`）、本组件的版本轮询（`mine-body.vue:392`）。不加锁会并发跑两三遍
`reloadLocal()`/`refreshProfile()`，而 `refreshProfile()` 内部有 `busy` 互斥
（`mine-body.vue:540` `if (this.busy) return;`），后到的那次会提前 `return`，表现成「刷新按钮偶发没反应」。
现在并发只让第一轮跑，其余置 `refreshAgain`，第一轮结束后补跑一次。

`boot()`（`:413`）用**同一把锁**：boot 的 `await restoreSession()` 期间到达的
`refresh()` 一律合并到 boot 结束后补跑一次，避免更旧的 `currentSession()` 快照把界面
写回去（`:418-421` 与 `:440-444`）。

外加组件级版本监看（`startVersionWatch` `:368` / `stopVersionWatch` `:402`，
在 `mounted()` `:321` 启动、`beforeDestroy()` `:324` 停掉）：
每 2.5s 比一次版本号，变了就 `refresh()`。这样即使**没有任何人**调 `refresh`，
组件自己也能收敛。

### 4.4 `PenCG/src/pages/index/index.vue` —— 两条兜底通道

- `onShow()` **最前面**（`:204`）加 `this.checkSeq();`
- 新增 `checkSeq()`（`:230`）：异步比版本号，变了就 `restoreSession()` 后
  **只有「本地登录态由未登录翻转为已登录」**才 `setTab('mine')`，并刷新 mine-body。
- 新增 `startSeqWatch()`（`:321`）/ `stopSeqWatch()`（`:334`），
  `mounted()` `:175` 启动、`beforeDestroy()` `:178` 停掉。
  这是**与 `onShow` 完全无关**的通道，专治断点 3。
- `boot()` `:345` 里取基线 `this.sessionSeq = await sessionVersionAsync()`（`:356`），
  再置 `this.seqReady = true`（`:362`），避免冷启动误判。

**误跳保护（重要）**：版本号是持久化的，冷启动时若首次读盘失败读到 0、
随后读到历史版本号 N，会被误判成「刚登录」→ 用户一进软件就被拽到「我的」。
所以 `checkSeq()` 里用 `isLoggedIn()` 的**前后翻转**做二次确认
（`index.vue:233-257`）：冷启动时本页 `boot()` 已经 `restoreSession()` 过，
本来就在登录态，翻转不成立，不会误跳；真登录时之前是未登录，翻转成立。

### 4.5 `PenCG/src/pages/mine/mine.vue` —— 清理重复 key

`mine.vue:44-45` 原本有一个**空的 `onShow() {}`** 紧跟真正的 `onShow()`，
对象字面量同名 key 后者覆盖前者，虽不致命但会误导排查。已删除。

### 4.6 改动清单

| 文件 | 改动（`git diff --numstat`） | 针对 |
|---|---|---|
| `PenCG/src/services/auth.js` | +92/-0 行，会话版本号三处落盘 + 三个 bump 调用点 | 断点 3 的感知前提 |
| `PenCG/src/services/nav.js` | +42/-6 行，pending 标记挪到 `$falcon` | 断点 1 |
| `PenCG/src/components/mine-body.vue` | +135/-19 行，`refresh()` 补 `restoreSession()` + 重入锁 + 版本监看 | **断点 2（根因）**、断点 3 |
| `PenCG/src/pages/index/index.vue` | +155/-11 行，`checkSeq()` + 轮询 + 基线 + `refreshMineBody()` 重试收敛 | 断点 3 |
| `PenCG/src/pages/mine/mine.vue` | -1/+2 行，删空 `onShow` | 卫生 |

**没有动** `PenCG/native/**`，没有 `git commit`/`git add`，没有写设备。

### 4.7 四道防线的关系（为什么要这么多层）

| 防线 | 依赖 | 失效场景 |
|---|---|---|
| ① nav pending 标记（`nav.js:46`） | `$falcon` 可写 + `onShow` 被调用 | 断点 3 成立时全废 |
| ② `onShow` 里 `checkSeq()`（`index.vue:204`） | `onShow` 被调用 | 断点 3 成立时全废 |
| ③ index 轮询（`index.vue:321`） | 只依赖 `setTimeout` + 读盘 | 页面被销毁才失效（但那时也不需要了） |
| ④ mine-body 自身轮询（`mine-body.vue:368`） | 只依赖 `setTimeout` + 读盘 | 同上 |

① ② 是「正常路径」，③ ④ 是「`onShow` 不回调时的兜底」。
③ ④ 最终都走到同一个动作：`refresh()` → `restoreSession()` → `reloadLocal()`，
而 `restoreSession()` 是 §4.3 补上的那一句 —— 所以**即使前两道全废，结果也正确**。

### 4.8 复核自查表（第二轮，逐项对代码下结论）

| # | 自查项 | 结论 | 证据 |
|---|---|---|---|
| 1 | 三处 `bumpSessionVersion()` 是否都在**写盘之后** | **通过**，三处全在写盘后 | `auth.js:146` saveSession → `:147` bump；`:168` → `:169`；`logout()` `:185` remove → `:186` bump |
| 2 | `sessionVersionAsync()` 读盘失败是否不抛、不回退 | **通过** | `auth.js:71-77` 整个 `getString` 包在 try/catch，异常/超时/坏 JSON 一律当 0；`return Math.max(readHost(), LOCAL_SEQ, st)`（`:78`）取 max，**任何一路读到更大的值都赢**，不会回退。`storage.js:41` `getString` 本身也不抛（`withTimeout` + catch 回落 `MEMORY`） |
| 3 | `LOCAL_SEQ` 冷启动能否恢复 | **通过** | `LOCAL_SEQ` 冷启动为 0，但 `sessionVersionAsync()` 每次都读盘，`Math.max` 拿到盘上的历史版本。基线也走 `sessionVersionAsync()`（`index.vue:356` / `mine-body.vue:373`），口径一致，所以「冷启动版本为 0 → 比对永远不触发」不会发生。同步版 `sessionVersion()`（`:55`）冷启动确实为 0，但 grep 确认全仓无外部调用点，不影响 |
| 4 | `$falcon[TAB_KEY]` 与模块变量、30s 过期的交互 | **通过** | `setPendingTab`（`nav.js:46-57`）双写：`:48` 模块变量 + `:52` `$falcon`；`takeTabParam`（`:59`）先读 `$falcon` 并置 `null`，再 `if (!v) v = pendingTab`（`:70`）**兜底模块变量**，两条通道都覆盖；30s 判定 `:73` 用 `Number(v.at) \|\| 0`，缺字段时不会误判 |
| 5 | 「登录成功跳回时 `setTab('mine')` 真被执行」 | **通过（有条件）** | 走 `index.vue:205` `if (t && t !== this.tab)` → `:206` `this.setTab(t)`。若该分支因 pending 丢失未命中，则由 `:204` `checkSeq()` 兜底：`loggedIn` 为真且 `tab !== 'mine'` 时 `:261` 同步 `setTab('mine')`。两条互相覆盖 |
| 6 | `refresh()` 里 `restoreSession()` 是否在 `currentSession()` 之前完成 | **通过** | `mine-body.vue:470` `await restoreSession()` 在 `:474` `await this.reloadLocal()` 之前，而 `currentSession()` 只在 `reloadLocal()`（`:487`）里调 —— await 天然串行，不可能乱序 |
| 7 | `refresh()` 并发是否互相覆盖 | **已修（本轮新增）** | 新增 `refreshing` / `refreshAgain`（`:168-169`），`refresh()` `:455-459` 加锁、`:461-479` 最多补跑一轮、`:482-483` finally 释放；`boot()` `:418-422` 共用同一把锁 |
| 8 | 2.5s 轮询 `beforeDestroy` 是否停掉 | **通过** | mine-body `beforeDestroy()` `:324` 调 `stopVersionWatch()`（`:402`，置 `verStopped` + `clearTimeout`）；index `beforeDestroy()` `:179` 调 `stopSeqWatch()`（`:334`）。两者 tick 开头都先判 `verStopped`/`seqStopped` |
| 9 | 轮询读盘是否刷屏 | **通过** | `startVersionWatch`（`:380-399`）只在**版本变化时**打一行 `[mine] session 版本变化 X -> Y，重载`；`checkSeq` 只在变化时打 `[index] 会话版本 X -> Y`。未变化路径零日志 |
| 10 | 误跳保护 `const loggedIn = !was && !!now;` | **通过** | `index.vue:249` `was = isLoggedIn()` 在 `restoreSession()` **之前**取，`:256` `now` 在其后取 —— 「未登录→已登录」翻转只有真登录时成立；冷启动时 `boot()` 早已 `restoreSession()`（`:348`），`was` 就是 true，不翻转，不会被拽到「我的」 |
| 11 | `seqReady` 未就绪是否不比对 | **已修（本轮新增门控）** | 轮询早就门控（`:329` `if (self.seqReady) self.checkSeq();`）；本轮给 `checkSeq()` 本体也补了 `:239` `if (!self.seqReady) return;` —— 因为 `onShow` 可能早于 `boot()` 里的异步基线（`:356`），那时 `sessionSeq` 还是 0，会把历史版本误判成「刚登录」 |
| 12 | `beforeDestroy` 是否停掉 timer | **通过** | 见 #8 |

> 本轮**主动撤销了一个错误优化**：曾想「`$falcon` 读到非 0 就跳过读盘」以省 I/O，
> 随后发现正确性反例（各页面的 `$falcon` 若是各自副本，本页 host 停在旧值就永远
> 看不见别页 bump 出来的新版本）并**已回滚**。反例已写进 `auth.js:61-68` 的注释。

---

## 五、构建验证

```
cd PenCG && node ../env/node_modules/aiot-vue-cli/src/cli.js -q -p
```

结果：**通过**

```
libs 同步成功: ...\PenCG\.falcon_\libs
应用构建成功,目录: ...\PenCG\.falcon_
打包成功,路径: ...\PenCG\8002026110200003.1_0_3.amr
```

两条既有 WARNING 与本改动无关，是本机环境固有：
- 找不到 `theme-dark`（`falcon-ui` 未装）
- 未找到模块 `cgmedia,cgnative,global,brightness`（原生模块，属正常）

---

## 六、真机验证步骤（adb 命令级）

> **本章是简版。完整可执行方案（含三条负向对照、两种失败模式的区分、
> 失败时 5 段式排查入口）见 [BUGFIX-bug2-verify.md](BUGFIX-bug2-verify.md)。**
> 执行的人请直接看那份，本章只保留最核心的判据与命令，便于快速查阅。

### 取证通道（**先读这个**，否则下面所有 grep 都是空的）

X6 Pro 的 Falcon 运行时里，JS 的 `console` **只有 `console.error` 会写盘**；
`console.warn` / `console.log` 完全不落盘（全盘 `grep -a -c 'W][JS]'` 计数为 0）。
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
> `grep -c pencg` 为 0，是空文件。真日志在 `DictPen_YYYYMMDD_HHMMSS.log`，
> 且 native `[cgmedia]` 与 JS `[pencg]` **必落同一文件**（都跟 miniapp 进程 stdout 走）。

行形态固定：

```
[2026-11-12 22:15:24.947][E][JS] [pencg][I] [mine] refresh logged=true booted=true
└──────────┬────────────┘ └┬┘ └┬┘ └────┬────┘
        落盘时间戳        级别 来源   PenCG 自己打的级别前缀
```

- **落盘级别恒为 `[E]`**（底层通道是 `console.error`），跟我们想表达的严重程度无关。
- **真正的级别只看行内 `[pencg][I|W|E]`**。本节下文所有 `[pencg][I]` / `[pencg][W]` 示例，
  在设备上都长成 `...[E][JS] [pencg][I] ...`，**grep 时请 grep 行内前缀**。
- PenCG 侧所有输出都经 `PenCG/src/services/log.js` 的 `push()` 走 `console.error`（`log.js:44`），
  所以 `logInfo` 打的行一样落盘。

> 快速验证通道是否通：触发一次操作后 `$ADB shell "grep -a -c 'pencg' $L"`，
> 计数为 0 说明是**取证通道**的问题，不是补丁没生效。

`adb` 路径：`/d/Users/Daniel/code/DSH/x6pro/haas-x6pro/env/adb/adb.exe`
设备序列号：`9D91500007505171`

> ⚠ 以下第 1 步「装新包」写设备，**必须由 Lead 执行**（本排查任务是只读设备）。
> 部署命令参考 `PenCG/docs/DEV.md`。

### 步骤 0：清干净基线

```bash
cd /d/Users/Daniel/code/DSH/x6pro/haas-x6pro

# 确认设备在线
./env/adb/adb.exe devices

# ★ 解析当前日志文件（后面所有 grep 都用这两个变量；真日志不在 loli_s6_dev.txt）
ADB="./env/adb/adb.exe -s 9D91500007505171"
P=$($ADB shell 'pidof miniapp' | tr -d '\r')
L=$($ADB shell "readlink /proc/$P/fd/1" | tr -d '\r')
echo "LOG=$L"

# 清空旧日志，便于只看本次
$ADB shell "mv \"$L\" \"$L.bak\" 2>/dev/null; sync"

# 记下当前 pencg_session（此时应是已登录状态；下一步要清掉它来做「未登录→登录」）
./env/adb/adb.exe shell "cat /userdisk/miniapp/data/mini_app/pkg/8002026110200003/data/sharedpreferences/preferences.json" | grep -o 'pencg_session[^,]*'
```

### 步骤 1：装新包（Lead 执行）

按 `PenCG/docs/DEV.md` 的安装流程装 `8002026110200003.1_0_3.amr`。

### 步骤 2：制造「未登录」初态

在设备上手动操作：进入「我的」→ 若已是登录态，点「退出登录」→ 确认。
（**不要**用 adb 改 `preferences.json`：该值是「JSON 字符串里再套一层 JSON」，
HANDOFF.md L68-77 明确禁止 sed，必须 pull→`json.loads` 校验→push。）

此时界面应显示「还没有登录网易云游戏」。

### 步骤 3：★ 核心验证 —— 登录后**不要**碰左侧栏，直接看

在设备上：点「去登录」→ 输入账号密码 → 点登录。

**登录成功的 600ms 后，页面应自动回到「我的」栏目并直接显示用户资料，
全程不需要点任何侧边栏按钮。**

用 adb 抓证据，确认「昵称 / 剩余时长 / 头像」三项（这三项分别来自
`mine-body.vue` 的 `nickname` computed、`timeText`、`initial`）：

```bash
# ① 看日志里四道防线的落点（哪一条生效了）
./env/adb/adb.exe shell "grep -hE '\[pencg\].*(\[auth\]|\[mine\]|\[index\]|\[nav\])' $L | tail -n 40"
```

期望能看到（至少其中之一，且**后面紧跟** `[mine] refresh logged=true`）：

```
[pencg][I] [auth] 密码登录成功 uid=...
[pencg][I] [nav] closePageByName login                     ← 防线①的导航落点
[pencg][I] [index] 会话版本 1 -> 2 本地登录态 false -> true  ← 防线②走通
[pencg][I] [mine] refresh logged=true booted=true          ← ★ 关键判据
[pencg][I] [mine] @me ok free=... vip=...
```

若只看到
```
[pencg][W] [index] mine-body 实例未就绪（已重试 2 次） ← 防线②没抓到实例
[pencg][I] [mine] session 版本变化 1 -> 2，重载      ← 防线④补上了
[pencg][I] [mine] refresh logged=true booted=true
```
也说明修复生效，只是走的是兜底通道（对应 §七 遗留风险 2/3）。

**关键判据**：出现 `[mine] refresh logged=true`。修复前这里会是 `logged=false`
（若根本没人调 refresh，则这行完全不出现）。

```bash
# ② 截图留证（需 miniapp_cli，见 DEV.md L74）
./env/adb/adb.exe shell "miniapp_cli capture /tmp/login_after.png"
./env/adb/adb.exe pull /tmp/login_after.png ./shots/login_after.png
```

截图应能看到：头像首字（`initial`）、昵称（`用户aaabOhXYY` 这类）、
「免费时长 0 分钟」或实际剩余时长，而**不是**「还没有登录网易云游戏」。

```bash
# ③ 反证：确认导航没有塌陷（没有多压一层页）
./env/adb/adb.exe shell "grep -h '\[nav\]' $L | tail -n 20"
```

期望 `closePageByName login` 出现、**不应**出现 `navTo('index')` 兜底被触发
（即不应有 `[nav] 无法关闭页面 name=login`）。

### 步骤 4：验证「会话变化能被无刷新感知」

这一步单独验 §4.3/§4.4 的兜底通道。

1. 停在「推荐」栏目（**不在**「我的」）。
2. 从其它入口（如详情页的登录引导）完成一次登录。
3. **不做任何点击**，等 2.5～5 秒。
4. 期望：界面自动切到「我的」并显示资料。

```bash
./env/adb/adb.exe shell "grep -hE '\[pencg\].*\[(index|mine)\].*(会话版本|版本变化|refresh logged)' $L | tail -n 10"
```

### 步骤 5：验证不误跳（防回归 —— 最容易踩的坑）

1. 确认已登录（盘上有有效 `pencg_session`）。
2. 杀掉小程序、重新冷启动。
3. **期望：启动后停在「推荐」，不要被自动拽到「我的」。**

```bash
./env/adb/adb.exe shell "grep -hE '\[pencg\].*\[index\].*会话版本' $L | tail -n 20"
```

若冷启动就出现「会话版本 0 -> N，本地登录态 false -> true」，说明
`isLoggedIn()` 翻转保护失效了 —— **这是必须修的回归**。

### 步骤 6：验证退出登录也同步

1. 已登录，停在「我的」。
2. 点「退出登录」→ 确认。
3. 期望：提示变「未登录」，且**其它栏目再点回「我的」也仍是未登录**
   （即版本号 bump 生效，不会从别的地方复活旧会话）。

```bash
./env/adb/adb.exe shell "grep -hE '\[pencg\].*(\[auth\]|\[mine\])' $L | tail -n 20"
```

---

## 七、遗留风险

> 本章是**第二轮复核时**列的风险。第三轮（a–e 复核）又发现并修掉了 5 个问题，
> 其中 2 个是真正会导致"永久显示未登录"的，详见 **§八.3**。
> 本章部分条目的定位信息（行号）在第三轮改动后已后移，以 §八 为准。

1. **`$falcon` 是否可写未实证。** 若被冻结，`$falcon[SEQ_KEY]` 与
   `$falcon[TAB_KEY]` 都写不进去，防线 ①②失效、防线 ③④退化为纯读盘
   （`sessionVersionAsync` 仍能从 storage 看到变化），功能不受影响，只是延迟 2.5s。
   **真机验证步骤 3 的日志能直接判定**：若看到 `[mine] session 版本变化`
   这条（mine-body 自己的轮询发现版本变了）而不见 `[index] 会话版本` 那条，
   就说明 `$falcon` 不可写、`onShow` 里那条通道没看见变化。

2. **`$nextTick` 存在性未实证**（`index.vue:213-215` 是三元兜底）。
   若不存在则同步调 `refreshMineBody()`，此时组件可能尚未挂载，靠 120/260ms
   重试兜住；若仍抓不到实例，防线 ③④会在 2.5s 内补上。功能不丢，只是慢。

3. **`onShow` 是否在 `closePageByName` 后被回调仍未实证**（断点 3）。
   补丁不依赖它，但**验证步骤 3 若只看到轮询那条日志、没看到 `onShow` 那条**，
   就说明断点 3 在这个固件上确实成立 —— 建议把结论补进 HANDOFF.md。

4. **轮询的代价**：index 与 mine-body 各一条 2.5s 递归 `setTimeout`，
   每次一次 storage 读。`storage.js:41` 的 `getString` 带 3000ms 超时并回落
   `MEMORY` 缓存，正常情况下开销很小；但若真机上 storage 每次都慢，
   需要把间隔放宽到 5s（改 `index.vue:330`、`index.vue:332` 与 `mine-body.vue:400` 的 `2500`）。

5. **未真机验收。** 本次工作只做到「代码 + 构建通过」。
   HANDOFF.md L61-66 记着的「登录成功提示未真机验收」同样是待办 ——
   那 600ms 窗口需要 Lead 在设备上跑到。

---

## 八、真机验证状态

> **本节是全文最重要的免责说明，请先读它再引用任何结论。**

### 8.1 当前状态：完全未验证

截至本文档最后一次修改（2026-11-13），bug2 的修复**没有任何一行真机日志**支撑。
已有证据只有两类：

| 证据 | 强度 | 说明 |
| --- | --- | --- |
| 代码静态复核（本文 §一 / §二 / §四） | 高（逻辑层） | 逐行读码 + 交叉验证调用点，能排除"代码写错"，**排除不了"运行时行为与静态推断不符"** |
| `aiot-vue-cli` 构建通过 + 打包成功 | 中 | 只证明语法/依赖/DOM 结构合法，**不证明运行期任何行为** |
| 真机日志 | **无** | 一条都没有 |

所谓"验证过"的历史记录里也**没有**这一项：
`PenCG/HANDOFF.md:61-66` 记着「登录成功提示未真机验收」，
当时是因为易盾风控锁号（`1125 need_yidun_validate_fail_enough`）跑不到那 600ms 窗口；
本次环境虽已登录，但**设备被长测占用、且 UI 无法用 adb 注入**，
所以仍然没有跑通这条链路。

**结论：这是"看起来对的修复"，不是"验证过的修复"。**
它的每一项论断都建立在 §8.3 列出的假设上。

### 8.2 验收方案在哪

完整可执行方案在 [BUGFIX-bug2-verify.md](BUGFIX-bug2-verify.md)，含：

- 前置（两条退到未登录的路 + 各自风险）
- 三版 `DEV_AUTOSTART` / `DEV_AUTOLOGIN` / `DEV_LOGOUT` 的确切 JSON
- 每步 adb 命令与期望日志行（15 行时间线表）
- PASS/FAIL 判据，**含两种失败模式的区分方法**
- 三条负向对照（其中最硬的一条是注释掉根因修复后复现原 bug）
- 失败时 5 段式排查入口

**本文不重复那些步骤**，只在下面记录"复核阶段新发现的风险"。

### 8.3 复核阶段新发现的风险点（本轮新增）

第一轮实现时没意识到、本轮 a–e 复核才发现的，按严重度排序：

#### 风险 1（已修）：`refresh()` 读盘失败一次就永久卡在未登录

- **现象**：`refresh()` 里 `await restoreSession()` 若因 `storage.js:45` 的 3000ms
  超时回落空 `MEMORY` 而读不到会话，`logged` 保持 false；
  而版本号在这一次已经被**消费掉**（`mine-body.vue:395 self.seq = v`），
  轮询再也不会因为"版本变化"触发下一次 `refresh()`。
  结果：界面**永久**停在「还没有登录网易云游戏」，除非用户手动切栏目重建组件。
- **修法**：新增 `mine-body.vue:446 retryUnlogged(v)` —— 版本号没再变但会话仍空时，
  有界补试最多 2 次（`seqTry >= 2` 停止，登录成功后 `:521-524` 归零）。
  日志判据：`[mine] 版本号已到 N 但会话为空，补试第 i 次`。

#### 风险 2（已修）：`refresh()` 抛出未捕获 rejection 会打断整个轮询链

- **现象**：`startVersionWatch()` 的 `tick` 里原来是 `self.refresh();`（async 不等待）。
  在 QuickJS 里未捕获的 Promise rejection 可能让后续 `setTimeout` 递归**不再排上**，
  而这条链是"`onShow` 不回调"时的**唯一**兜底。
- **修法**：所有内部自发调用改走 `mine-body.vue:431 fireRefresh()`（内部 `.catch` +
  打 `[mine] refresh 未捕获异常`）；tick 的 `.then(again)` 保证不管成败都续排。
- **顺带确认**：`stopVersionWatch()`（`mine-body.vue:416`）是
  `verStopped = true` + `clearTimeout` + `verTimer = null` 三件事都做，
  **能清掉已排队的那次 `setTimeout`**，销毁后不会再跑一次。这条原本是疑点，核过是安全的。

#### 风险 3（已修）：冷启动可能被误判成"刚登录"而强行跳到「我的」

- **现象**：`checkSeq()` 的跳栏条件 `const loggedIn = !was && !!now;` 只看前后翻转。
  冷启动时若 `boot()` 里 `restoreSession()`（`index.vue:376`）与
  `sessionVersionAsync()`（`:384`）两次读盘都失败（同上 3s 超时），
  `was = false`；随后 2.5s 轮询读盘恢复成功 → `now = true` → 翻转成立 →
  **已登录的老用户一开软件就被拽到「我的」**。
- **修法**：新增 `auth.js:230 sessionAge()`（读盘上 `loginAt`，返回距今毫秒，读不到 -1），
  `index.vue:271-272` 改为
  `const fresh = age >= 0 && age < 10 * 60 * 1000;`
  `const loggedIn = !was && !!now && fresh;`
  —— 只有 10 分钟内写下的会话才可能是"用户刚在登录页登录"。
- **仍未消除**：若用户真的在 10 分钟内登录过、又冷启动，仍可能被跳一次。
  这是一次性体验问题，不会导致"显示未登录"。

#### 风险 4（已修）：两次 `bumpSessionVersion()` 并发会读到同一个旧值

- **现象**：`bumpSessionVersion()` 里 `const n = (await sessionVersionAsync()) + 1;`
  中间有 `await` 读盘。登出后立刻登录（两次 bump 间隔 < 一次读盘）时，
  两次都可能读到同一个 `n`、都写 `n+1`，**版本号只前进 1 格**。
  后果：`checkSeq()` / 版本轮询可能少看见一次变化。
- **修法**：`auth.js:34` 加 `let BUMP_CHAIN = Promise.resolve();`，
  `bumpSessionVersion()`（`:83`）用串行链排队，函数体抽成 `auth.js:109 bumpOnce()`。
- **顺序也已核对**：三处 bump 调用点（`auth.js:175` loginByPassword /
  `:197` loginBySms / `:214` logout）**全部在写盘之后**
  （`:174` `saveSession`、`:196` `saveSession`、`:213` `remove`）——
  顺序错了会导致"版本变了但盘上还是旧会话"，这条原本是重点疑点，核过是合规的。

#### 风险 5（已修）：`takeTabParam()` 会丢掉更晚写入的标记

- **现象**：原实现优先读 `$falcon[TAB_KEY]`，读到就无条件盖过模块变量；
  若模块变量里有一个**更晚**写入的标记（比如 index 页自己 `setPendingTab`
  之后再被别的路径写了一次），会被丢掉。同时 `if (!v) v = pendingTab;` 之后
  无条件 `pendingTab = null`，会顺手清掉未被消费的那一份。
- **修法**：`nav.js:59 takeTabParam()` 改为「两条通道各取一份、比 `at` 取更晚、
  两条都置空」。
- **诚实说明**：**"两个页面同时读、一个拿到值一个拿到 null" 这条疑点是伪命题**。
  `takeTabParam` 全仓**唯一消费点**是 `index.vue:190`（grep 证实），
  不存在第二个消费者，所以没有跨页竞争。真正的问题只是上面那两条丢弃逻辑，
  而且它在实际路径里也几乎触发不到 —— 现在改掉了是"防未来的坑"。

### 8.4 仍然完全没验证的假设（引用本文任何结论前请先看这里）

以下每一条都**只是静态推断**，真机上可能不成立。按对结论的影响排序：

| # | 假设 | 若不成立会怎样 | 怎么证伪 |
| --- | --- | --- | --- |
| 1 | **两个页面是两份独立的 JS 上下文**，各自有一份 `cg.js` 的 `G` | **整个根因分析崩塌**：若共享，修复前就不该有 bug。这是 §二 的全部基础 | `index.vue` 与 `login.vue` 各打 `logInfo('[ctx] ' + (currentSession() ? 1 : 0))`，看登录前后是否各自独立变化 |
| 2 | `$falcon` 可写（`$falcon[SEQ_KEY]` / `$falcon[TAB_KEY]` 能存住） | 防线 ①② 失效、退化为纯读盘；功能不丢，延迟 2.5s。**不影响最终能否修好** | 看有没有 `[auth] 会话版本号落盘失败`（`auth.js:123`，只在 `setString` 失败时打）；或对比只出现 ④ 的日志、不出现 ② |
| 3 | `closePageByName('login')` 之后 index 的 Vue `onShow` **会被回调** | 防线 ①② 失效，靠 ③④ 的轮询兜住（2.5s 延迟）。**不影响最终能否修好** | 只看到 `[mine] session 版本变化`、看不到 `[index] 会话版本` → 说明 `onShow` **没**回调（断点 3 成立，应记进 HANDOFF.md） |
| 4 | `$nextTick` 在本运行时存在 | `index.vue:213-215` 走三元兜底同步调 `refreshMineBody()`，组件可能未挂载，靠 0/120/260ms 重试 + 3④ 兜住 | 看到 `[index] mine-body 实例未就绪（已重试 2 次）` 但最终仍 PASS → 说明 `$nextTick` 没帮上或不存在 |
| 5 | `storage.getStorage` 在真机上通常 < 3s | 风险 1 描述的"永久卡未登录"会**高概率复现**，`retryUnlogged` 只能多试 2 次 | `[mine] refresh logged=false` 每 2.5s 反复出现、且 `[auth] 恢复会话`（`auth.js:207`）一次都没有 |
| 6 | `computed` 在模板里能正常求值（`nickname` / `initial` / `timeText` 等） | `logged` 变 true 了但**界面不跟着变** —— 这与 `index.vue:150` 注释记录的"这个运行时对 computed 的模板求值不可靠"是同一类缺陷，需改成 data + 手动赋值 | `[mine] refresh logged=true` + `[mine] @me ok` 都有，但截图仍显示「还没有登录」 |

**特别注意第 1 条**：它是根因判断的唯一支点。若真机验证发现
"注释掉 `refresh()` 里的 `restoreSession()` 后 bug 不复现"，
说明两页其实共享 `G`，那么**本文的根因结论就是错的**，需要重来。
真机验证方案 §3.5 的对照 1 就是为这件事设计的。

### 8.5 引用纪律

- 本文 §一 §二 §三 是**代码事实**（file:line 可核对），可以直接引用。
- §四 是**代码现状**，与磁盘一致。
- **§二.3「修复前为什么拿不到会话」的结论是推断，不是观测。**
  引用时应写"按代码推断"，不要写成"实测证明"。
- §八 的 PASS/FAIL 判据**尚未有任何一次执行记录**。

---
