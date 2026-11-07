# PenCG 登录页 / 我的页 —— 异常态与边界态清单

> 作者：ui-account（task-4）。本文只覆盖 `src/pages/login/` 与 `src/pages/mine/` 两条链路。
> 全部结论来自**读代码路径**（`auth.js` / `cg.js` / `net.js` / `native.js` / `storage.js` / 两页 .vue），
> 未经真机注入异常复验（设备正在给 native-media 做机位验证）。每条标注了「可否真机验证 + 怎么验」。
> 代码改动只落在 `login.vue` / `mine.vue`（我的 write scope）；需要动 services 的都列为「建议」交给 lead。

---

## 0. 传输层与错误对象：先搞清楚异常长什么样

这是后面所有条目的前提，也踩过坑。

### 0.1 两条传输通道
`net.js` 的 `httpText()` 顺序：
1. `loadNative()` 拿到 `cgnative` → `mod.requestSync(...)`（curl 直连，能拿非 2xx body）
2. 拿不到 native，或 `requestSync` 返回非对象 → `jsapiRequest()`（平台 `jsapi.http.request`）
3. `jsapi` 也不可用 → `{status:0, body:'', error:'jsapi.http 不可用', curlCode:-1}`

### 0.2 错误对象的形状**不一致**（重要坑）
| 来源 | `err.message` | `err.status` | `err.code` |
|---|---|---|---|
| `net.js` 的 `httpJSON()`（**v1 匿名接口走这条**） | `'<errcode> <errmsgcn>'` 或 `'http <status>'` 或原始 error 串 | ✅ 有 | ❌ **没有** |
| `cg.js` 的 `v2()`（**v2 登录态接口走这条**） | 同上 | ✅ 有 | ✅ 有（从 body 的 `errcode` 解析） |

**后果**：`auth.js` 的 `loginByPassword` / `loginBySms` / `sendSmsCode` 走的是 `v1Post`，
抛出来的 Error **没有 `code` 字段**，errcode 只以 `"1102 短信验证码错误"` 的形式藏在 message 里。
`login.vue` 原来的 `friendlyErr()` 只读 `e.code`，对 v1 全失效。

**已在 login.vue 修**：`friendlyErr()` 里加了正则回退
```js
let code = (e && e.code) || 0;
if (!code) { const m = /^(\d{3,5})\s+/.exec(msg); if (m) code = parseInt(m[1], 10); }
```
**建议（交 lead）**：`net.js` 的 `httpJSON()` 也应该像 `cg.js:v2()` 那样解析并挂 `err.code`，
这样所有调用方都能统一读 `e.code`，不用各自写正则。这是 services 层的改动，我没动。

### 0.3 「native 不可用」不等于「网络不通」
`loadNative()` 有 `state.netTried` 记忆化：一旦首次 `import('cgnative')` 失败，
**整个进程生命周期内**都返回 `null`，后续全部回退 jsapi。
所以「dev 构建提示 `未找到以下模块: cgnative`」时，密码登录会在
`auth.js:46` 抛 `native 加密模块不可用（需要 cgnative）` —— 这是**环境问题，不是密码错**。
已在 `friendlyErr()` 里单独识别并提示「可改用短信登录」（短信登录不依赖 cgnative）。

---

## 1. 登录失败态（密码错 / 验证码错 / 未注册）

### 1.1 密码错误
| 项 | 内容 |
|---|---|
| **触发条件** | `POST api/v1/user-pwd-tokens` 返回非 2xx，body 含 `errcode`（密码错的具体码 CGAPI.md 未实测记录，落在兜底分支） |
| **代码路径** | `login.vue:submit()` → `auth.loginByPassword()` → `v1Post` → `net.httpJSON` 抛 `Error('<errcode> <errmsgcn>')` → `catch` |
| **修复前行为** | `errText = String(e.message).slice(0,60)`，页面直接显示 `1101 密码错误` 这种带数字的原文 |
| **当前行为** | `errText = this.friendlyErr(e, 'pwd')`。密码错会走 `st === 401` → **「手机号或密码有误」**；其他码走兜底，剥掉数字前缀显示服务端原文（截 40 字） |
| **是否可接受** | ✅ 可接受。文案不含内部错误码，用户知道是密码/手机号问题 |
| **重试** | ✅ 按钮必然恢复。`catch` 里第一句就是 `this.busy = false`（login.vue:388），**不存在卡在「登录中…」的风险**（`busy` 同时控制按钮文案与 `submit()` 的入口守卫） |
| **可否真机验证** | 可，但**会消耗一次真实登录尝试**。建议只在需要时做一次：故意输错密码 → 截图 → 再输对。不值得为此单独占设备 |

### 1.2 未注册手机号
| 项 | 内容 |
|---|---|
| **触发条件** | `GET api/v1/user-pwd-info` 对未注册号码**只返回 `{"is_registered":false}`**（CGAPI.md:1198），不带 `user_pwd_login_salt` |
| **代码路径** | `auth.js:36` `if (!info \|\| !info.user_pwd_login_salt) throw new Error('取登录参数失败')` |
| **修复前行为** | 页面显示「取登录参数失败」——**这句是给开发者看的，用户看不懂** |
| **当前行为** | `friendlyErr()` 匹配 `取登录参数失败` → **「该手机号未注册，或未设置密码」**，并提示可改用短信登录注册 |
| **是否可接受** | ✅ |
| **可否真机验证** | 可，且**不消耗时长**（`user-pwd-info` 是匿名接口，失败在登录前）。用一个没注册过的号码测即可 |

### 1.3 短信验证码错误 / 次数过多
| 项 | 内容 |
|---|---|
| **触发条件** | `errcode=1102`（验证码错误，HTTP 401）、`errcode=1109`（错误次数过多，HTTP 401）（CGAPI.md:1183-1184） |
| **当前行为** | `friendlyErr()` → 「验证码错误，请重新输入」/「错误次数过多，请重新获取验证码」 |
| **是否可接受** | ✅ 1109 额外引导了「重新获取」这个正确动作 |
| **可否真机验证** | 1102 可测（随便填 6 位数字），不消耗时长。1109 要连错多次，不建议浪费请求 |

### 1.4 验证码发送过于频繁
| 项 | 内容 |
|---|---|
| **触发条件** | `errcode=1100`「短信验证码申请太频繁」（CGAPI.md:1170） |
| **发现的真实 bug** | `sendCode()` 原来**只判 `this.smsLeft > 0`**。`smsLeft` 要等请求成功返回后才由 `startTick()` 置数，中间有网络往返窗口（v1 接口实测 500~1500ms）。在这窗口里连点两下，会**并发发出两次请求**，服务端直接回 1100，用户看到「发送失败」但其实是自己点太快 |
| **已修** | `if (this.smsLeft > 0 \|\| this.busy) return;` —— `busy` 在请求发起的同一 tick 就置 true |
| **当前行为** | 连点被静默忽略，不会产生第二次请求 |
| **是否可接受** | ✅ |
| **可否真机验证** | 可，不消耗时长（发码不发会话）。但会真的给那个手机号发短信，建议少发 |

### 1.5 区号无效 / 号码格式
| 项 | 内容 |
|---|---|
| **触发条件** | `errcode=1103`（invalid ctcode）/ `1104`（phone not exist）（CGAPI.md:1165-1166） |
| **当前行为** | 「区号无效，请检查 +86」/「该手机号未注册」 |
| **输入侧防护** | `editCc()` 走 `digits(r.text, 4)` 只留数字，`kbOk()` 里 `digits(v, 4) \|\| this.cc` 保证不会把区号清空；手机号 `digits(..., 11)` 硬截断 |
| **是否可接受** | ✅ |

---

## 2. 网络错误态

### 2.1 完全断网
| 项 | 内容 |
|---|---|
| **触发条件** | 设备无网 → native `requestSync` 返回 curl 错误码（`curlCode != 0`）或 `status:0` |
| **代码路径** | native 分支：`r.status` 是 number 就直接返回，`error = ok ? null : (r.error \|\| 'http '+r.status)`。**注意：curl 失败时 `r.status` 可能是 0**，此时 `httpJSON` 判 `status < 200` → 抛 `Error(detail)`，`detail` = `r.error`（形如 `curl: 6 couldn't resolve host`） |
| **修复前行为** | 页面显示 `curl: 6 couldn't resolve host 'n.cg.163.com'` —— 用户完全看不懂 |
| **当前行为** | `friendlyErr()` 里 `msg.indexOf('curl') >= 0 \|\| st === 0` → **「网络不可用，请检查网络后重试」** |
| **是否可接受** | ✅ |
| **可否真机验证** | ✅ **最容易验的一条，且完全不消耗时长**：`adb shell 'ifconfig eth0 down'`（或关 WiFi）→ 进登录页试登录 → 截图 → `ifconfig eth0 up` 恢复。**注意恢复网络前不要退出 app，否则要重启** |
| **遗留问题** | ⚠️ 没有「自动重试」也没有「离线提示条」。用户必须自己再点一次登录。对笔这种小屏设备我认为可以接受，但**如果要更好，建议加一个 `retry` 按钮**（我在 login.vue 里没加，怕按钮撑爆 266px 高度，交 lead 决定） |

### 2.2 请求超时
| 项 | 内容 |
|---|---|
| **触发条件** | native 侧 curl 超时（`timeout` 默认 15000ms，v2 是 20000ms）；jsapi 侧是内置 `setTimeout(timeout + 3000)` 兜底 |
| **当前行为** | `friendlyErr()` 匹配 `timeout` → 「网络超时，请检查网络后重试」 |
| **发现的问题** | ⚠️ **jsapi 兜底的超时时间比请求本身还长 3 秒**（`net.js` `jsapiRequest` 里 `setTimeout(..., timeout + 3000)`）。意味着最坏情况下用户要等 18 秒才看到错误。对触屏笔来说体感很差 |
| **是否可接受** | ⚠️ 勉强。**建议（交 lead，services 改动）**：把兜底超时改成 `timeout` 本身，或给登录接口单独传 `{timeout: 8000}` |
| **可否真机验证** | 难精确构造，跳过 |

### 2.3 native 与 jsapi 双双不可用
| 项 | 内容 |
|---|---|
| **触发条件** | `cgnative` import 失败 **且** `$falcon.jsapi.http` 缺失（正常固件不会发生） |
| **代码路径** | `jsapiRequest()` 返回 `{status:0, error:'jsapi.http 不可用'}` → `httpJSON` 抛该串 |
| **当前行为** | `friendlyErr()` 匹配 `jsapi` → 「网络不可用，请检查网络后重试」 |
| **评价** | ⚠️ **文案会误导**：真实原因是「本机 HTTP 能力缺失」，不是网络问题。但这种情况应该只在开发构建出现，且 `native 加密模块不可用` 那条会先命中。**优先级低，不改** |
| **可否真机验证** | 不可复现（固件必带 jsapi） |

---

## 3. token 过期态

### 3.1 这是最严重的一条，修复前会导致「永久卡在假登录」
| 项 | 内容 |
|---|---|
| **触发条件** | storage 里残留一个已过期的 token。v2 接口回 **HTTP 401 + `errcode=1006`「登录信息失效，请重新登录」**（CGAPI.md:64） |
| **代码路径漏洞** | `auth.restoreSession()` 的逻辑是 `if (!s \|\| !s.token) return false; applyLogin(s); return true;` —— **它只看 token 字符串存不存在，完全不做有效性校验**。所以过期 token 也会让它返回 `true` |
| **修复前的完整恶化链** | ① mine `boot()` → `restoreSession()` 返回 true → `currentSession()` 有 token → `this.logged = true` → **渲染「已登录」整页 UI**；② `refreshProfile()` → `v2Get('users/@me')` → 401 抛错；③ `catch` 里只写 `errText = '刷新失败：1006 登录信息失效，请重新登录'`，**但 `logged` 仍是 true，页面停在已登录布局**；④ `errText` 在已登录分支**根本没有渲染点**（模板里只有 `v-if="!logged"` 的 `.guest-err` 会显示 errText）；⑤ 结果：**昵称空白、ID 显示 `--------`、手机号「未绑定手机号」、时长「登录后查看」，右上角还是「已登录 · 云游戏玩家」，没有任何提示，也没有出口** |
| **已修（mine.vue `refreshProfile` 的 catch）** | 识别 `status === 401 \|\| code === 1006` → **主动 `logout()` 清掉 storage** → `this.user = null; this.session = null; this.logged = false;` → `errText = '登录已过期，请重新登录'` → `showToast('登录已过期')` → `return`。页面自动落回未登录引导态，「去登录」按钮可用 |
| **当前行为** | ✅ 自动踢回未登录态并给出明确文案 |
| **是否可接受** | ✅ 可接受。**这是本轮最有价值的修复** |
| **建议（交 lead，services 改动）** | 更彻底的做法是给 `auth.restoreSession()` 加一个**可选的在线校验**（`restoreSession({verify:true})` 时打一次轻量 v2 接口），或给 token 加 `expiresAt`（JWT 是 HS256，前端可 base64 解 payload 读 `exp`，不验签）。现在这种「靠第一个 v2 请求撞 401」的方式能用，但用户会先看到一帧假已登录 UI |
| **可否真机验证** | ✅ **不消耗时长**，最容易做：直接往 storage 里写一个假 token。方法见 §6 验证手册 |

### 3.2 登录页的同类问题（未修，属可接受）
| 项 | 内容 |
|---|---|
| **触发条件** | 同上，过期 token |
| **当前行为** | `login.vue:refreshStatus()` 调 `restoreSession()`，因为它对过期 token 也返回 true，所以右上角显示「<过期快照里的昵称> · 可直接使用」 |
| **是否可接受** | ⚠️ 轻微误导，但**后果可控**：用户在这个页面上就是要重新登录，一旦真的提交，`applyLogin()` 会覆盖 `G` 并 `saveSession()` 写新 token，旧的自动作废 |
| **建议** | 如果想更准，改成「快照里有 `loginAt`，超过 7 天就不显示『可直接使用』」。我没改，避免引入新的时间判断逻辑 |

---

## 4. 免费时长耗尽态 / 未知态

### 4.1 修复前的三重误判
这一块原来有**三个独立的错误**叠加，显示结果完全不可信：

| # | 错误 | 具体机制 |
|---|---|---|
| a | **单位错** | `free_time_left` 是**秒**（CGAPI.md:1093 `\| free_time_left \| 手游免费剩余（秒） \| 975 \|`），却被当分钟直接用 → 1333 秒显示成「22 小时 13 分」 |
| b | **`restoreSession` 把时长清零** | `cg.js` 的 `applyLogin()` 里是 `G.freeTimeLeft = payload.free_time_left \|\| 0`（**下划线**），但 storage 里存的快照字段是 `freeTimeLeft`（**驼峰**，由 `session()` 返回的 `G` 直接序列化而来）。于是每次启动恢复会话，`G.freeTimeLeft` 都被重置成 0 |
| c | **0 无法区分「没数据」和「用完了」** | 用 c 和 a、b 叠加，用户刚进 mine 页那一瞬间，`freeSeconds` 一定是 0 |

### 4.2 已修（mine.vue）
1. **单位**：`freeMinutes() = Math.floor(this.freeSeconds / 60)`（lead 先修的，我核对保留）
2. **区分未知态**：新增 `hasFreeData()`。判据是「`this.user.free_time_left` 有值」**或**「`session.freeTimeLeft` 是正数」。
   - 为什么不判 `!= null`：如 §4.1-b，恢复会话后 `freeTimeLeft` 恒为 0，`!= null` 恒真，等于没判。
   - 为什么不直接看 `this.user`：`user` 在 `users/@me` 返回前是 `null`，但登录快照里可能有真实值，要允许快照兜底。
3. **三态文案**：
   | 状态 | `timeText()` | `timeSub()` |
   |---|---|---|
   | 未登录 | 「登录后查看」 | 「登录后自动同步」 |
   | 已登录但还没拿到数值 | **「同步中…」** | **「正在同步账户资料」** |
   | 有数据且 > 0 | 「22 分钟」/「1 小时 5 分」 | 「免费时长可继续畅玩」；**< 5 分钟时**「不足 5 分钟，即将用完」 |
   | 有数据且 = 0 | **「已用尽」** | **「今日额度已用完，明日 0 点重置」** |
4. **不再显示裸的「0 分钟」**：`timeText()` 里 `m <= 0` 直接返回「已用尽」，`timeSub()` 给出去哪儿获取的动作提示。

### 4.3 仍未做（建议，交 lead）
- ⚠️ **没有跳转到「签到 / 开通会员」的入口**。当前只是文字引导，用户点不动。真正的 163CG 官网在时长耗尽时会弹「开通会员无限畅玩」（CGAPI.md:724-727 的 `free_time_left_tips_info`）。**建议**：让时长卡整体可点 → `navTo('mine')` 之外没有更好去处，或者接 lead 决定是否要做一个「时长获取」页。我没加按钮，因为 `.tcard` 只有 232x78，塞不下第二个可点区域而不会误触。
- ⚠️ **`free_time_left_tips_info` 服务端已经算好了文案**（含 `##时间##` 占位符），我们完全没用。**建议**：`users/@me` 已经返回这个字段，直接取第 2 条（`##时间##分钟`）替换占位符即可得到官方口径文案。这是纯 `mine.vue` 改动，我可以做，但要 lead 确认要不要动。

### 4.4 可否真机验证
✅ **两条都能不消耗时长验证**：
- 「已用尽」态：把 `mine.vue` 临时改成读一个常量 0，截完图改回来（或用 §6 的 storage 注入法写 `free_time_left: 0` 到缓存的 user 对象）
- 「同步中」态：`adb shell` 断网后进 mine 页，`@me` 请求失败 → `user` 保持 null → 显示「同步中…」

---

## 5. `bluray` 字段的边界态

### 5.1 字段语义（这里之前理解错过）
`CGAPI.md:1096`：`| bluray | 是否有蓝光权限 | False |` —— **布尔值，不是时长**。
我第一版把它当数字拼成「无蓝光时长」「蓝光 10 分钟」，语义全错。

### 5.2 已修（mine.vue `blurayText()`）的四种组合
| `bluray_left` | `bluray` | 显示 | 说明 |
|---|---|---|---|
| > 0 | 任意 | 「蓝光可看 N 分钟」（`Math.floor(left/60)`） | `bluray_left` 与 `free_time_left` 同族，单位**秒** |
| 0 或缺失 | `true`/`1`/`'true'` | 「蓝光画质可用」 | 有权限但没单独计时 |
| 0 或缺失 | `false`/`0`/`'false'` | 「无蓝光权限」 | 明确否定 |
| 0 或缺失 | **`undefined`/`null`/缺失** | **`this.user` 存在 → 「无蓝光权限」；不存在 → 「待同步」** | **这是本次新加的三态区分** |

### 5.3 为什么要区分「待同步」
修复前，`users/@me` 还没返回（`this.user === null`）时，`b` 取到 `undefined`，
直接走 `b === undefined → '无蓝光权限'`。也就是说**页面上会先斩钉截铁地写「无蓝光权限」，然后数据回来又变** —— 典型的闪烁性假信息。
现在 `!this.user` 时显示「待同步」，数据回来才给结论。

### 5.4 发现的语义疑点（需要 lead / 抓包确认）
- ⚠️ `pc_bluray: true`（CGAPI.md:1040）与 `bluray: false`（:972）同时出现在同一份 `users/@me` 样例里。
  **我们只读了 `bluray`，没读 `pc_bluray`**。这两个大概率是「手游蓝光」vs「端游蓝光」两个独立权限。
  PenCG 走的是**手游**（`X-Platform: 1`，`MOBILE_UA`），所以读 `bluray` 是对的。
  **建议**：如果以后支持端游，`blurayText()` 要按 `pc_` 前缀分流。当前不改。
- ⚠️ `bluray_left` 这个字段**在 CGAPI.md 的字段表里没有**（我 grep 过全文，只有 `bluray`、`pc_bluray`）。
  也就是说 §5.2 表格里第一行是**防御性代码**，真机上大概率永远走不到。
  **建议**：真机抓一次 `users/@me` 的完整 key 列表确认；如果没有 `bluray_left`，这行可以删掉减少混淆。

---

## 6. 真机验证手册（设备空出来后按这个跑）

**优先级从高到低，全部不消耗云游戏时长**（只打匿名 v1 接口或本地态）。

### 6.1 token 过期态（最高价值，验证本轮核心修复）
```bash
# 1. 往 storage 写一个假 token（key 见 storage.js KEYS.session = 'pencg_session'）
#    用 diag 页或临时页面调 $falcon.jsapi.storage.setStorage({key:'pencg_session', data:'<json>'})
#    json 形如: {"token":"fake_expired_token","userId":"67d9","nickname":"过期用户","freeTimeLeft":1333}
# 2. 启动 mine 页
adb shell "miniapp_cli start 8002026110200003 mine"; sleep 12
# 3. 预期：先一闪已登录布局 → refreshProfile 撞 401 → 自动落回未登录引导态
#    截图应看到「还没有登录网易云游戏」+「登录已过期，请重新登录」
adb shell "miniapp_cli capture /tmp/t1.png"
# 4. 日志确认
adb shell "grep -h '\[mine\]' /userdata/applog/DictPen_*.log | tail -5"
#    必须出现: [mine] token 失效，清除会话踢回未登录 status=401 code=1006
```

### 6.2 网络错误态（第二价值）
```bash
adb shell 'ifconfig eth0 down'          # 断网（或用 settings 关 WiFi）
adb shell "miniapp_cli start 8002026110200003 login"; sleep 12
adb shell 'cd /tmp && ./x6tap tap 560 57'   # 点手机号 → 系统键盘
# 输一个号码 + 密码后点登录，预期 errText = "网络不可用，请检查网络后重试"
adb shell "miniapp_cli capture /tmp/t2.png"
adb shell 'ifconfig eth0 up'            # ★ 必须先恢复网络再退出 app
```

### 6.3 未注册号码（不占时长，最快）
```bash
# 用一个没注册过的号码，在登录页填「手机号 + 任意密码」→ 点登录
# 预期 errText = "该手机号未注册，或未设置密码"
# 日志: [login] fail 取登录参数失败 | detail=该手机号未注册，或未设置密码
```
（这条只打 `GET user-pwd-info`，匿名，不产生会话）

### 6.4 时长耗尽 / 同步中
- 「同步中…」：断网进 mine 页，`@me` 失败 → `user` 为 null → 时长卡显示「同步中…」
- 「已用尽」：需要 `free_time_left` 真的为 0，**不要为了测试消耗真实时长**。做法：临时改 `mine.vue` 的 `freeSeconds()` 返回 0，构建 → 截图 → 改回。

### 6.5 短信验证码错误
填 6 位随机数字 → 点登录 → 预期「验证码错误，请重新输入」（`errcode=1102`）。不产生会话。

---

## 7. 本轮代码改动汇总（只有这两个文件）

### `PenCG/src/pages/login/login.vue`
| 改动 | 文件位置 | 理由 |
|---|---|---|
| 新增 `friendlyErr(e, mode)` | `methods` 内，`submit()` 前 | 把 errcode / curl 错误 / 401 / 5xx 翻译成可行动中文 |
| `friendlyErr` 内正则回退解析 errcode | 同上 | v1 接口的 Error **没有 `.code`**，只有 message 前缀 |
| `submit()` catch 改用 `friendlyErr` | `submit()` | 原来直接显示带数字的错误原文 |
| `sendCode()` catch 改用 `friendlyErr` | `sendCode()` | 同上 |
| `sendCode()` 入口加 `\|\| this.busy` | `sendCode()` 首行 | 修复倒计时启动前的连点并发（触发 1100） |

### `PenCG/src/pages/mine/mine.vue`
| 改动 | 文件位置 | 理由 |
|---|---|---|
| 新增 `hasFreeData()` | `computed` | 区分「没数据」与「真为 0」（`restoreSession` 会把 freeTimeLeft 清零） |
| `timeText()` 三态 + 「已用尽」 | `computed` | 不再显示裸「0 分钟」；未同步时显示「同步中…」 |
| `timeSub()` 三态 + `<5 分钟` 预警 | `computed` | 给下一步动作提示 |
| `blurayText()` 四态 + 「待同步」 | `computed` | `bluray` 是布尔不是时长；未加载时不下断言 |
| `refreshProfile()` catch 加 401/1006 分支 | `methods` | **核心修复**：token 过期主动 logout 踢回未登录 |
| `phoneText()` 三级回退 + 幂等 | `computed` | 上一轮修的（session.phone → user.phone → 未绑定；已脱敏串不再脱敏） |

构建验证：`node ../env/node_modules/aiot-vue-cli/src/cli.js -q -p` 通过，产物 `8002026110200003.1_0_0.amr`。
方言自检：无 `position:absolute` / `z-index` / `flex: 1` / 后代选择器；两个 .vue 都有 `<style lang="less" scoped>` + 首行 `@import "base.less";`。

---

## 8. 留给 lead 待决事项

| # | 事项 | 涉及层 | 我的建议 |
|---|---|---|---|
| 1 | `net.js` 的 `httpJSON()` 不给 err 挂 `code` | services | 建议统一挂 `err.code`，调用方就不用各写正则 |
| 2 | `jsapiRequest` 兜底超时是 `timeout + 3000` | services | 建议改成 `timeout` 本身，或登录接口传 `{timeout: 8000}` |
| 3 | `restoreSession()` 不校验 token 有效性 | services | 建议可选在线校验，或读 JWT `exp` 做本地过期判断 |
| 4 | 时长耗尽没有可点的「获取时长」入口 | UI | 需要产品决策：签到？开会员？还是只做提示 |
| 5 | `free_time_left_tips_info` 服务端已备好文案，我们没用 | UI | 可直接接，纯 mine.vue 改动，等你点头 |
| 6 | `bluray_left` 字段在文档里不存在 | 抓包 | 建议真机抓一次 `@me` 全 key 确认后再决定删不删这行防御代码 |
| 7 | 「同步中…」态没有 spinner | UI | 笔的性能下我认为纯文字够用，加动画有掉帧风险 |
