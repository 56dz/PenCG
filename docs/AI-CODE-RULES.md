# AI 生成代码规范（PenCG / X6 Pro Falcon 平台）

市场新规第 6 条要求补充「AI 生成软件规范」。本文是 PenCG 的落地版本：**面向 AI agent 和用 AI 写代码的人**，把「在这台设备上怎么写出不翻车的代码」写成可执行的规则。

每条规则尽量带 **file:line** 或**真机证据**。凡是「推测」的都标出来。

---

## 0. 为什么需要这份文档

X6 Pro 上的 Falcon 运行时是一个**高度裁剪的 Weex 类方言 + QuickJS + 自研 native 模块**。它不是浏览器，也不是标准 Weex：

- 没有 DOM、没有 `XMLHttpRequest` / `fetch` / `WebSocket`（JS 侧）。
- CSS 只支持单类名选择器，`em`/`rem` 被构建器静默改写。
- `<text>` 内容换行即整块不渲染。
- 只有 `console.error` 落盘 —— 写 `console.warn` 调试等于**日志凭空消失**。

AI 生成代码最容易在这些「与常识相悖」的地方翻车。所以规范的核心不是风格，而是**防呆**。

---

## 1. 绝对禁止（违反即黑屏 / 崩溃 / 日志全丢）

### 1.1 禁止 `console.warn` / `console.log` 打日志

**这条是本项目历史上踩过最惨的坑。**

真机实测（2026-11-12/13，X6 Pro）：Falcon 的 JS console **只有 `console.error`** 会落盘，`console.warn` 与 `console.log` **完全不落盘**（全盘 `grep -a -c 'W][JS]'` 计数为 **0**）。

落盘文件**不能写死**：它是 miniapp 进程 stdout 指向的那个文件，文件名带启动时间戳。

```
P=$(adb shell pidof miniapp)
L=$(adb shell "readlink /proc/$P/fd/1")     # 实测形如 /userdata/applog/DictPen_YYYYMMDD_HHMMSS.log
```

> **路径更正（2026-11-14 真机定论）**：早期文档里写的 `/userdata/applog/loli_s6_dev.txt` 是**错的** —— 该文件实测只有 1 字节，`grep -c pencg` / `grep -c cgmedia` 都是 0，是个空文件。真正的日志在 `DictPen_*.log`（实测 13.2MB，`[cgmedia]` 2236 行 + 大量 `[pencg]` 行）。**先 `readlink` 再 grep，不要写死、也不要靠 `ls -t` 猜。**

```js
// ❌ 绝对不要 —— 这行在真机上什么都不留下
console.warn('[pencg] something happened');

// ✅ 正确
console.error('[pencg][I] [screen] keep #3 backend=...');
```

统一入口是 `PenCG/src/services/log.js`，它内部最终走 `console.error`（`:44`）。所以：

```js
// ✅ 业务代码一律用 log.js 的封装
import { logInfo, logWarn, logError } from '../services/log.js';

logInfo('[mine] refresh logged=' + this.logged);
logWarn('[play] stall detected: rx=' + rx);
logError('[media] fatal');
```

**关于日志级别的反直觉事实**：落盘行的格式是

```
[2026-11-12 22:15:24.947][E][JS] [pencg][W] [screen] keep #3 ...
                            ↑              ↑
                        通道级别恒为 E   真正的级别在这里
```

**中间那个 `[E]` 永远是 `E`**，不代表严重程度。真正的级别看行内的 `[pencg][I|W|E]` 前缀。按 `[E]` 过滤会得到全部日志，按 `[pencg][E]` 过滤才是错误。

详见 `docs/VERIFY-playbook.md` §0。

### 1.2 禁止 emoji 进入 UI 文案

设备没有 emoji 字体，emoji 渲染为方块或空白。

```vue
<!-- ❌ -->
<text class="t">游戏 🎮</text>

<!-- ✅ -->
<text class="t">游戏</text>
<!-- 需要图标就用 <image> 加载 PNG -->
```

**注意区分**：注释和日志字符串里的 `★` `→` `⇒` 不渲染，不算违规。审计时按「是否在 `<template>` 里或会被赋给 UI 变量」判断。

#### 1.2.1 单色几何符号也可能缺字形 —— 只能实测，不能靠码段推理

「不是 emoji 码段 ⇒ 一定有字形」是**错的**。X6 Pro 真机（2026-11-14 走查）实测：

| 字符 | 码位 | 真机表现 | 处置 |
| :-- | :-- | :-- | :-- |
| `⌫` | U+232B ERASE TO THE LEFT | **空白方框（tofu）** | 已换成中文「删」（`src/pages/play/play.vue:80`） |
| `●` | U+25CF BLACK CIRCLE | 正常 | 保留（登录页密码掩码） |
| `…` | U+2026 | 正常 | 保留 |
| `·` | U+00B7 | 正常 | 保留 |
| `—` | U+2014 | 正常 | 保留 |
| `→` | U+2192 | 正常 | 保留（仅注释/日志） |
| `★` | U+2605 | 正常 | 保留（仅注释标记） |

`⌫` 恰恰是「看起来最容易有字形」的那个（几何图形、无变体选择符、非 emoji），却唯一缺席。所以**任何要上屏的非常用汉字/英文/数字字符，都必须真机走查**；不确定就退回中文或 ASCII。

#### 1.2.2 硬规则：禁止「杂项技术符号」区段（U+2300–U+23FF）进入 UI

> **UI 可见字符不得使用 `⌫`、`⇧`、`⌘`、`⏎`、`⇥` 等「杂项技术符号（U+2300–U+23FF）」区段的字符，实测无字形、渲染成 tofu；需要此类语义时用中文词（删 / 回车 / Tab）或 PNG 图标。**

这条规则是**硬性的**，不因「看起来像几何图形」「不是 emoji」「别的平台能显示」而豁免。理由：

- U+2300–U+23FF 是字符集里**字体覆盖率最不稳定**的一段。它的语义是「技术符号」，系统默认字体通常不给它做字形。
- 我们已经在这段上翻过车：`⌫`（U+232B）在 X6 Pro 上就是空白方框。
- 同一段里的 `⇧`（U+21E7 实际在 Arrows 段，但同类问题）、`⌘`（U+2318）、`⏎`（U+23CE）、`⇥`（U+21E5）都是键盘/命令语义的高频误用对象，一个都不能赌。

**已实测可用的字符（X6 Pro，2026-11-14）**——这些可以放心用在 UI 文案里：

| 字符 | 码位 | 名称 |
| :-- | :-- | :-- |
| `…` | U+2026 | HORIZONTAL ELLIPSIS（省略号三个点） |
| `·` | U+00B7 | MIDDLE DOT（中点） |
| `—` | U+2014 | EM DASH（长破折号） |
| `→` | U+2192 | RIGHTWARDS ARROW（箭头） |
| `★` | U+2605 | BLACK STAR（五角星） |
| `●` | U+25CF | BLACK CIRCLE（实心圆） |

注意 `…` `·` `—` 这三个是**中文排版必备**的，实测可用意味着中文文案不必退回 `...` / `.` / `-`。

**替代写法对照**：

| 想表达的语义 | ❌ 不要用 | ✅ 改成 |
| :-- | :-- | :-- |
| 删除 / 退格 | `⌫` U+232B | 中文「删」，或 PNG 图标 |
| 回车 | `⏎` U+23CE | 中文「回车」/「确定」 |
| Tab | `⇥` U+21E5 | 中文「Tab」 |
| 命令键 | `⌘` U+2318 | 中文「命令」，或 PNG 图标 |
| Shift | `⇧` | 中文「Shift」 |

**审查命令**（构建前跑一遍）：

```bash
# 直接扫 U+2300–U+23FF 区段
grep -rnP '[\x{2300}-\x{23FF}]' src --include=*.vue --include=*.js
```

这条命令会把注释里的命中一起报出来，所以**不能拿「有命中」直接判违规**：命中的每一处都要看一眼是不是在 `<template>` 里、或会不会被赋给 UI 变量。只有会渲染的那些才算阻断。注释和日志里的技术符号不渲染，不违规 —— 但建议顺手清掉，否则自动审计会一直有假阳性（见 §1.2 的「注意区分」）。

**当前状态（2026-11-14 实测）**：`PenCG/src` 该区段命中数为 **0**，`⌫` 换成「删」之后已无残留。

排查手法（构建前，纯静态）：

```bash
# 列出 src 下所有非常见码位的候选（CJK 与 ASCII 之外）
grep -rnP '[^\x00-\x7F\x{4e00}-\x{9fff}\x{3000}-\x{303f}\x{ff00}-\x{ffef}]' src --include=*.vue | head -30
```

命中后逐个人眼确认「是否在 `<template>` 里或会被赋给 UI 变量」——是，就必须走查。

### 1.3 禁止 `font-size` 用 `px`

新规要求。统一用 vw，`vw = px / 9.6`。

```less
/* ❌ */
.title { font-size: 18px; }

/* ✅ */
.title { font-size: 1.875vw; }
```

换算表与真机 A/B 证据见 `docs/VW-VH-GUIDE.md`。

### 1.4 禁止 `em` / `rem`

构建器 `falcon-styler` 会告警并**自动改写**成无单位数字，真机该行完全不生效：

```
NOTE: unit `em` is not supported and property value `1em` is autofixed to `1`
```

这是**静默失败**，不会报错，只会样式不生效 —— 最难查的一类 bug。

### 1.5 禁止缺 `<style lang="less" scoped>` + `@import "base.less"`

```vue
<!-- ✅ 每个 .vue 都必须是这个形状 -->
<style lang="less" scoped>
@import "base.less";

.xxx { ... }
</style>
```

没有 style 块时**样式规则不进 bundle**，表现是整页黑屏（真机实测过，2026-09-22）。这是 `PenCG/src/styles/base.less:11-15` 的第一条铁律。

### 1.6 禁止 `flex: 1` 简写

框架不支持简写，必须三连：

```less
/* ❌ 无效 */
.item { flex: 1; }

/* ✅ */
.item {
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
}
```

### 1.7 禁止 `<text>` 内容换行

```vue
<!-- ❌ 整块不渲染 -->
<text class="t">这是很长的一段
说明文字</text>

<!-- ✅ 先切数组，再逐行渲染 -->
<text class="t" v-for="(l, i) in lines" :key="i">{{ l }}</text>
```

### 1.8 禁止多类名 / 后代选择器

```less
/* ❌ 构建直接失败 */
.a .b { }
.a.b { }

/* ✅ 只支持单类名 */
.a { }
.b { }
```

### 1.9 禁止内置 shell / adb 能力

新规第 5 条。JS 侧根本没有这个能力面（`PenCG/src/services/screen.js:36` 有书面记录）。不要在 native 里新增 `popen` / `system` / `execl` 去做「下载文件」「执行命令」之类的事；已有的 `amixer` / `ffmpeg` 调用是历史资产，不属于可扩展的能力。

### 1.10 禁止把调试开关留在默认开启

`PenCG/src/dev-autostart.js` 有三个开关：

```js
export const DEV_AUTOSTART = null;  // 非 null 会让生产包自动跳指定页面
export const DEV_AUTOLOGIN  = null; // 非 null 会自动登录
export const DEV_LOGOUT     = null; // 非 null 会自动登出
```

上面是**应该的样子**。**提交前必须确认三个都是 `null`**（`grep -n "export const DEV_" PenCG/src/dev-autostart.js`）。

这曾是一个 P0 级审查问题（`docs/REVIEW-report.md` 的 S1）。**2026-11-14 已修复**：1.2.0 发布包里 `dev-autostart-eed06a3a.js.bin` 只有 **139 字节**（三个 `null` 字面量的 AST 骨架），且 `yssy` / 手机号 / 密码 / 测试游戏码全部 0 命中。详见 `docs/MARKET-COMPLIANCE.md` §3.2。

**为什么这条这么危险**：如果它被写成 `{ page: 'play', ... }`，发布包一启动就会**自动进播放页并占用机位** —— 用户什么都没点，机位已经在计时扣费了。

---

## 2. 写日志的规范

### 2.1 前缀约定

| 前缀 | 归属 | 落盘文件 |
| :-- | :-- | :-- |
| `[pencg][I]` / `[pencg][W]` / `[pencg][E]` | JS 业务日志（`log.js`） | miniapp 进程 stdout（`readlink /proc/$(pidof miniapp)/fd/1`） |
| `[im]` | 即时通讯（`PenCG/src/services/im.js:18`） | 同上（同一个文件） |
| `[media]` | 媒体 SDP / 信令（`PenCG/src/services/media.js:388`） | 同上（同一个文件） |
| `[pencg][diag]` | 诊断页 | 同上（同一个文件） |
| `[cgmedia]` | **native** C 层（`cm_log()`） | 同上（同一个文件） |

**关键：`[pencg]` 和 `[cgmedia]` 必落同一文件。** JS 的 `console.error` 和 native 的 `fprintf(stdout)` 都跟着 miniapp 进程的 stdout 走，进程只有一个 stdout，所以只有一份日志。

```
P=$(adb shell pidof miniapp)
L=$(adb shell "readlink /proc/$P/fd/1")
adb shell "grep -a -n -e '\[pencg\]' -e '\[cgmedia\]' $L | tail -50"
```

> **路径更正（2026-11-14）**：`/userdata/applog/loli_s6_dev.txt` 是**空文件**（1 字节），真日志在 `DictPen_*.log`。文件名带启动时间戳、每次重启都变，**不能写死、也不能只靠 `ls -t` 猜**（`ls -t` 拿到的是最近修改的，不一定是当前进程的那份）。上面 `readlink` 的写法才是准确的。

### 2.2 日志要带判据，不要带情绪

```js
// ❌ 无法判断状态
logWarn('[play] 出问题了');

// ✅ 一眼能判断
logWarn('[play] stall detected: rx=' + rx + ' 已 ' + sec + 's 未增长');
```

**具体数字 > 形容词。**这是项目里所有有效日志的共同特征，例如：

- `[screen] keep #10 backend=brightness.Brightness(new) ok bright=20 state=0(ON)`
- `[play] probe stage=live v=1 screen=0(ON) phase=playing ws=open rx=6578 fps=13 kf=2 keep=on`
- `#1 RR sent (70 bytes, srtcp) rc=2 v[lost=0 jit=12] a[lost=0 jit=8]`

### 2.3 一条日志只讲一件事

不要拼装跨领域信息：

```js
// ❌ 查 bug 时要正则拆解
logInfo('[all] rx=' + rx + ' amp=' + amp + ' batt=' + batt + ' net=' + net);

// ✅ 各自一条
logInfo('[play] probe rx=' + rx + ' fps=' + fps);
logInfo('[audio] amp=' + amp);
```

### 2.4 循环里打日志必须节流

```js
// ❌ 每帧一条，日志文件几秒内涨到几十 MB，且拖慢主线程
logInfo('[play] frame ' + n);

// ✅ 时间节流（项目里已经这么做的例子）
if (now - this.lastLogMs > 10000) {
  this.lastLogMs = now;
  logInfo('[play] probe ...');
}
```

参考实现：`PenCG/native/csrc/cgmedia/cm_media.c:98` 的 `still no IDR`（10s 一次）、`PenCG/native/csrc/cgmedia/cm_cmd.c:340` 的 `NACK_LOG_INTERVAL_MS 5000`。

### 2.5 不要记录隐私

```js
// ❌ 手机号、token、密码进日志（日志会被上传取证）
logInfo('[auth] login phone=' + phone + ' token=' + token);

// ✅ 脱敏
logInfo('[auth] login phone=***');
```

已有的正确写法：`PenCG/src/pages/login/login.vue` 的 dev autologin 日志用 `phone=***`。

---

## 3. 样式规范

### 3.1 单位

| 场景 | 单位 | 例子 |
| :-- | :-- | :-- |
| 字号 | **vw** | `font-size: 1.1458vw;`（原 11px） |
| 行高 | **vw** | `line-height: 1.25vw;` |
| 布局尺寸 | px（画布 1:1） | `width: 76px;` |
| 相对父容器 | % | `width: 16.6667%;` |
| KMS 挖孔矩形 | **必须 px** | `VIDEO = { x: 243, w: 473, h: 266 }` |

### 3.2 类名必须带页前缀

`<style scoped>` 在这个框架里**实际是全局合并**，跨页撞类名会互相污染。

```less
/* ❌ 两个页面都叫 .title，后加载的覆盖前一个 */
.title { color: @text-color-strong; }

/* ✅ 页面前缀 */
.p2-title { color: @text-color-strong; }   /* play 页 */
.idx-title { color: @text-color-strong; }  /* index 页 */
```

### 3.3 主题色必须用变量

`PenCG/src/styles/var.less` 有 28 行主题变量。不要硬编码颜色：

```less
/* ❌ */
.bg { background: #1E2831; }

/* ✅ */
.bg { background: @background-color; }
```

常用变量：`@background-color` `#1E2831`、`@card-background-color` `#28333D`、`@card-border-color` `#304051`、`@text-color` `#B4C0C9`、`@text-color-strong` `#F7FAFC`、`@text-color-dim` `#8B969F`、`@accent` `#03C47E`、`@accent-blue` `#24C5FF`、`@danger` `#E35043`、`@gold` `#EFC678`。

### 3.4 弹层用 `v-if` 整页替换，不要 `position: absolute`

框架**没有可用的 `z-index`**，`position: absolute` 不可靠。所有弹层都用 `v-if` 切换整块内容。

```vue
<!-- ✅ 项目里的实际做法 -->
<div v-if="exiting" class="p2-exit-mask">...</div>
```

### 3.5 滚动容器必须撑开

```vue
<!-- ✅ scroller 必须有固定宽高，或用 flex 三连撑开 -->
<div class="scroll-y list">
  <scroller class="scroll-y">...</scroller>
</div>
```

`PenCG/src/styles/base.less` 里已有 `.scroll-y` / `.scroll-x`，直接用。

---

## 4. JS 代码规范

### 4.1 能力边界（先认清再写）

JS 侧**只有** `$falcon.jsapi`：

- `jsapi.http.request` —— 唯一的网络能力。**没有** `fetch` / `XMLHttpRequest` / `WebSocket`。
- `jsapi.storage.getStorage` / `setStorage` —— `setStorage({key, data})` 才落盘。
- `jsapi.brightness` —— 屏幕保活。

其它能力全靠自研 native 模块（`libjsapi_cgnative.so` / `libjsapi_cgmedia.so`）。

**QuickJS 是 `bigNum: false`**，即**没有 BigInt**。做不了 53 位以上整数运算，涉及大数（时间戳纳秒、64 位计数器）时必须走字符串或拆位。

### 4.2 `jsapi.http.request` 的三个坑

1. 会对 `data` 字符串**二次 JSON 序列化** → 发不了裸 body。
2. **响应头永远不可见。**
3. 非 2xx 时 **body 不可得**，只有 `errorMessage` 里的 `resCode:N`。

```js
// ✅ 项目里的写法（PenCG/src/services/net.js:29）
const m = /resCode:(\d+)/.exec(msg || '');
```

### 4.3 日志用封装，不用裸 console

见 §1.1和 §2。

### 4.4 定时器必须能被清掉

```js
// ✅ 每次 start 前先 stop，onUnload 时清理
startWatch() {
  this.stopWatch();
  this.watchTimer = setInterval(() => this.check(), 2500);
}
stopWatch() {
  if (this.watchTimer) { clearInterval(this.watchTimer); this.watchTimer = null; }
}
```

**原因**：QuickJS 的定时器在页面销毁后不会自动回收，重复 `start` 会导致多个定时器叠加跑，表现为「逻辑执行多次」这种极难查的 bug。项目里 `PenCG/src/components/mine-body.vue` 的 `startVersionWatch` / `stopVersionWatch`、`PenCG/src/pages/index/index.vue` 的 `startSeqWatch` / `stopSeqWatch` 都是这个形状。

### 4.5 不要把状态放模块级变量跨页共享

**每个页面是独立的 QuickJS 上下文，模块级状态不共享。**

这是 bug2 的根因（`PenCG/src/services/cg.js:136` 的 `const G = {}` 是**每页一份**，登录页 `applyLogin()` 灌的是登录页那份，个人页读不到）。修复方式是把状态落在 `$falcon` 或 `jsapi.storage`：

```js
// ✅ 跨页共享必须走 $falcon 或 storage
$falcon.__pencgPendingTab = 'mine';

// ❌ 模块级变量跨页无效
let pendingTab = null;
```

项目里的正确实现：`PenCG/src/services/auth.js` 的会话版本号（三处写：模块变量 + `$falcon` + `storage`）、`PenCG/src/services/nav.js:46` 的 `setPendingTab()`。

### 4.6 异步必须处理错误

```js
// ❌ 静默失败，排查时零线索
doAsync();

// ✅
try {
  await doAsync();
} catch (e) {
  logError('[xxx] 失败: ' + (e && e.message ? e.message : e));
}
```

### 4.7 不要 `JSON.parse` 不加保护

```js
// ❌ 一个坏数据让整个页面白屏
const cfg = JSON.parse(raw);

// ✅
let cfg = {};
try { cfg = JSON.parse(raw) || {}; } catch (e) { logWarn('[cfg] 解析失败: ' + raw.slice(0, 80)); }
```

磁盘上的配置可能被外部写坏（历史上 `preferences.json` 就是「JSON 字符串里再套一层 JSON」，`PenCG/src/pages/index/index.vue:430` 有注释记录）。

---

## 5. native (C) 代码规范

### 5.1 日志用 `cm_log`，不用 `printf`

```c
#include "cm_util.h"

CM_LOGE("#%d fatal: %s", s->id, msg);   /* 级别 1 */
CM_LOGW("#%d still no IDR (rx=%llu ...)", s->id, rx);  /* 级别 2 */
CM_LOGI("#%d audio snapshot: ...", s->id);             /* 级别 3 */
CM_LOGD("#%d verbose ...", s->id);                     /* 级别 4，默认不输出 */
```

实现见 `PenCG/native/csrc/cgmedia/cm_util.c:19-29`：`fprintf(stdout, "[cgmedia] ")` + `vfprintf` + `fflush`，默认级别 `g_cm_log_level = 3`。

**所有 native 日志带 `#%d` 会话 id 前缀**，便于多局日志混在一起时区分。

### 5.2 落盘去向：与 JS 同一个文件

`[cgmedia]` 走 `stdout`，而 JS 的 `console.error` 也走同一个进程的 `stdout` —— **两条通道必落同一份文件**（2026-11-14 真机定论）。所以不要去找「第二个日志文件」，一份文件里两种前缀。

路径**不能写死、也不能用 `ls -t` 猜**（文件名带启动时间戳，`ls -t` 在网络/时钟异常或进程未启动时会拿到旧局）。正确做法是解析进程 stdout：

```
P=$(adb shell 'pidof miniapp' | tr -d '\r')
L=$(adb shell "readlink /proc/$P/fd/1" | tr -d '\r')   # 实测 /userdata/applog/DictPen_20261113_232219.log
```

历史坑：`/userdata/applog/loli_s6_dev.txt` 曾被当成落盘文件，实测只有 **1 字节**、`grep -c pencg` = 0，是空文件；`.tmp/soak.sh` 还硬编码过更早的文件名，导致整轮采样都在报 `No such file or directory`。

### 5.3 常量必须具名并写注释

```c
/* ✅ 项目里的实际风格 */
#define WATCHDOG_MS 180000      /* 180s 无心跳判定 JS 挂了 */
#define WATCHDOG_WARN_MS 60000  /* 60s 先打一条 WARN */
#define IDR_STARVE_RELAX_MS 4000 /* 4s 等不到 IDR 就放行 P 帧，避免永久黑屏 */
```

数值型常量**必须解释为什么是这个值**，否则下一个人不敢改也不敢不改。

### 5.4 改热路径前先确认有没有并发

`cm_rtp.c` 的共享状态用 `g_rtp_mu` 保护（`:212`）。新增全局状态时必须一并加锁。

### 5.5 不要在关键管线里加阻塞调用

播放管线（`cm_gst.c`）里已经有的 `popen` 是历史资产。**不要新增**同步阻塞调用到每帧回调路径上，会直接导致掉帧。

---

## 6. 注释与文档规范

### 6.1 改动锚点用 `★`，写清「为什么」

```c
/* ★ sync=false 是 bug3/bug4「终末地黑屏 / 画面卡住不动」的真正修复，别改回 true ★
 *
 * 症状：建连成功、first video frame pushed 有输出、probe rx/fps 正常，
 *       但视频区深灰透出 #1E2831，或只显示一帧。
 * 根因：appsrc format=time do-timestamp=true 打绝对单调时钟戳，
 *       sync=true 让 kmssink 按 PTS 决定显示时刻，基准错位后 buffer 被整体丢弃。
 */
```

（实际位置：`PenCG/native/csrc/cgmedia/cm_gst.c` 的 `KMS_SINK_PROPS` 宏上方，按符号名检索）

**写症状 → 写根因 → 写结论。**只写「改成 false 了」等于没写，下一个人会改回去。

### 6.2 注释里的行号必须可核对

引用 `file:line` 前先 `grep` 确认。**行号会漂移**，改了代码就要同步改注释里的行号，否则注释比没有更糟。

### 6.3 过时注释必须标出来

项目里现在有两处已知过时注释（本次只记录，未改源码）：

- `PenCG/src/styles/base.less:6` —— 播放态布局算式（213 + 469 + 278）是旧版遗留，实际是 243 + 473 + 244（`PenCG/src/pages/play/play.vue:153`）。

（另一处 `cm_gst.c` 里「会动到 `sync=true` 的 kmssink 调度」的过时注释**已在写作期间被他人修正**，不再登记。）

**新增代码不要产生第三处。**

---

## 7. AI 生成代码的审查清单

AI 写完代码后，**逐条打勾**再提交。

### 7.1 硬性（不过不能提交）

- [ ] 没有 `console.warn` / `console.log`（`grep -rn "console\.warn\|console\.log" src` 只应有注释命中）
- [ ] 没有 emoji 进入 UI 文案
- [ ] 没有 U+2300–U+23FF 技术符号进入 UI（`grep -rnP '[\x{2300}-\x{23FF}]' src --include=*.vue --include=*.js`，命中要逐条看是否渲染）
- [ ] 没有 `font-size: NNpx`（`grep -rn "font-size:[^;]*px" src` = 0）
- [ ] 没有 `em` / `rem`（`grep -rn "[0-9]\(em\|rem\)\b" src` = 0）
- [ ] 每个新 `.vue` 都有 `<style lang="less" scoped>` + `@import "base.less"`
- [ ] 没有 `flex: 1` 简写
- [ ] 没有 `<text>` 内容换行
- [ ] 没有多类名选择器（`.a .b` / `.a.b`）
- [ ] `dev-autostart.js` 三个开关都是 `null`，且包内 `dev-autostart-*.js.bin` 只有 139 字节、不含凭据（1.2.0 已验证，每次打包重验）
- [ ] 没有新增 shell / adb 调用

### 7.2 正确性

- [ ] 异步调用都有 `try/catch` 或 `.catch()`
- [ ] `JSON.parse` 有保护
- [ ] 定时器有配对的 `clear`
- [ ] 跨页状态走 `$falcon` / `storage`，不是模块变量
- [ ] 新增日志带具体数字，不是形容词
- [ ] 循环里的日志有节流

### 7.3 构建与真机

- [ ] `npm run build` 通过，无新增 warning
- [ ] native 改动：`powershell -File native/build.ps1` 退出码 0
- [ ] **真机跑一遍**（不是「应该没问题」）
- [ ] 关键路径拉日志确认打点生效

---

## 8. 常见翻车模式（真实案例）

| # | 翻车方式 | 后果 | 教训 |
| --: | :-- | :-- | :-- |
| 1 | 用 `console.warn` 打日志 | 真机日志全空，排查时以为「代码没跑到」 | 整个 bug3/bug4 调查因此走了大弯路（`docs/BUGFIX-bug3-bug4-video.md` 早期记录「`grep -a pencg` 命中数为 0」，真因是通道用错） |
| 2 | 把跨页状态放模块变量 | 登录后个人页显示「未登录」 | 每页独立 QuickJS 上下文，必须走 `$falcon` |
| 3 | `kmssink sync=true` | 画面有但完全静止 | 云游戏流不能用 PTS 节流 |
| 4 | `em` 单位 | 样式静默失效，无任何报错 | 构建器 autofix，看告警 |
| 5 | 缺 `<style>` 块 | 整页黑屏 | 样式不进 bundle |
| 6 | 硬编码日志文件名 | 采样脚本整轮报错 | 每次开机换名，用 `ls -t \| head -1` |
| 7 | `miniapp_cli start <appid> --index` | 报找不到 `b//--index.js.bin` | 页面名不带 `--` |
| 8 | 用 md5/体积判断 `.so` 是否更新 | 装了个旧库还以为更新了 | 装机时 `.so` 会被改名并追加 section（约 80 KB → 198 KB） |
| 9 | 想截图看视频区 | 永远看到空白 | `capture` 只抓 UI，`captureFB` 返回 -1，`kmsgrab` rc=124，无 `/dev/fb*`。视频是否正常只能看 DRM `fb` 编号是否交替 |
| 10 | 相信「注释里的行号」 | 改错位置的代码 | 行号会漂移，动手前 `grep` 复核 |
| 11 | 用 `⌫`（U+232B）当键盘删除键 | 真机渲染成空白方框 tofu | 「不是 emoji 码段」不等于「有字形」。U+2300–U+23FF 整段不可信，用中文或 PNG（见 §1.2.2） |

---

## 9. 给 AI agent 的额外约定

如果你是一个 AI agent 在本仓库工作：

1. **不要改你自己没读过的文件。**先 `read` 再 `edit`。
2. **不要声称「已验证」除非有真机日志或截图。**没有证据就写「未验证」。
3. **不要编造日志行。**文档里的日志示例必须来自真实输出或源码里的格式串。
4. **不确定的事实写「待现场确认」**，不要用「应该」「通常」「大概」蒙混。
5. **改文档时区分「历史记录」和「当前事实」**，历史结论被推翻时要标注，不要直接删掉（后来的读者需要知道为什么排除了那条路）。
6. **一次只改一类东西。**改样式就别顺手改逻辑。
7. **行号引用必须自己 grep 复核**，不要从旧摘要里抄。

---

## 10. 速查卡

```
日志    : 只有 console.error 落盘，用 log.js 封装，前缀 [pencg][I|W|E]
字号    : vw = px / 9.6，禁用 em/rem/px
样式    : 必须有 <style lang="less" scoped> + @import base.less
flex    : 三连，不是 flex: 1
text    : 不能换行
选择器  : 单类名，带页前缀
弹层    : v-if 整页替换，不用 absolute
跨页    : $falcon 或 storage，不是模块变量
定时器  : start 前先 stop，销毁时 clear
单位    : px 用于布局（1:1 画布），vw 用于字号
native  : cm_log + #%d 前缀，落 DictPen_*.log（每次开机换名）
```

---

*相关文档：`docs/MARKET-COMPLIANCE.md`（合规总纲）、`docs/VW-VH-GUIDE.md`（单位换算）、`docs/DEVICE-MATRIX.md`（多机型测试）、`docs/PLATFORM.md`（平台硬约束）、`docs/VERIFY-playbook.md`（日志取证）、`docs/ARCH.md`（架构）。*
