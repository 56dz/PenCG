# PenCG 应用市场合规说明（6 条新规 × 当前状态 × 整改清单）

本文是 PenCG 面向词典笔应用市场审核的**合规自述**。每条规则分三块写：

1. **规则原文** —— 照抄，不改语义。
2. **PenCG 当前状态** —— 只写能核查的事实，附 `file:line`。凡没在真机上验证过的，一律写「未验证」，不写「应该没问题」。
3. **整改动作** —— 分「已完成 / 待做 / 不适用」三档。

文中的证据分级沿用 `docs/BUGFIX-SUMMARY.md` 的约定：

| 标记 | 含义 |
| :-- | :-- |
| **【实测】** | 真机读数或真机日志，能给出具体数值 / 文件路径 |
| **【代码】** | 只从源码读出，未在真机观察 |
| **【待确认】** | 没有证据，需要现场取证 |

> 时效：**已在架软件须在两周内更新，逾期下架。** PenCG 当前处于整改窗口内，整改项与负责人见 §7。

---

## 1. 禁用 Emoji

### 1.1 规则原文

> 禁用 Emoji：词典笔系统没有 emoji 字体，一律替换为图片（PNG）；提交前必须真机走查，界面不得出现方块/乱码。

### 1.2 当前状态

**先说清一件事，否则审计方按 `grep` 计数会得到错误结论**：PenCG 源码里的非 ASCII 符号绝大多数出现在**注释和日志字符串**里，这些**不会渲染到界面**，不构成违规。必须分开统计。

全 `PenCG/src` 扫描（`grep -rhoP "[\x{1F000}-\x{1FAFF}\x{2190}-\x{27BF}\x{2B00}-\x{2BFF}\x{FE0F}]"`）的结果，按「是否渲染」分类：

| 字符 | 次数 | 出现位置 | 是否渲染 | 判定 |
| :-- | --: | :-- | :-- | :-- |
| `→` | 166 | 注释、日志字符串 | 否 | 不违规 |
| `─` | 93 | 注释里的分隔线 | 否 | 不违规 |
| `★` | 60 | 注释标记（改动锚点） | 否 | 不违规 |
| `①②③④⑤` | 31 | 注释里的编号 | 否 | 不违规 |
| `≤ ≈ ← ∈ ⇒ ≥ ≠` | 14 | 注释、日志 | 否 | 不违规 |
| `⓿` `●` `⌫` `⚠️` | 4 | 见下方逐条 | `●` 是、`⌫` 是、`⓿`/`⚠️` 否 | **已处理（2026-11-14 真机实测）** |

只有 3 处落在「可能渲染」的范围内：

**(1) `PenCG/src/pages/play/play.vue:80` —— 自绘键盘的删除键**

修复前：

```
<div class="p2-kbkey p2-kbdel" @click="kbDel"><text class="p2-kbnum">⌫</text></div>
```

修复后：

```
<div class="p2-kbkey p2-kbdel" @click="kbDel"><text class="p2-kbnum">删</text></div>
```

`⌫`（U+232B ERASE TO THE LEFT）虽然是单色几何符号、不属于 emoji 码段，但**真机实测渲染成空白方框（tofu）** —— X6 Pro 的系统字体没有这个码位的字形。

> **状态（2026-11-14）**：**已实测为 tofu，已替换为中文字符**。`PenCG/src/pages/play/play.vue:80` 现为 `<text class="p2-kbnum">删</text>`，键盘键宽与字号未变（`.p2-kbdel` 样式只改背景色，与字形无关），无需再走查。

**(2) `PenCG/src/pages/login/login.vue:107` —— 密码掩码**

```js
function dots(n) {
  let out = '';
  for (let i = 0; i < n; i++) out = out + '●';
  return out;
}
```

`●`（U+25CF BLACK CIRCLE）**真机实测正常渲染**，不需要替换。

> **状态（2026-11-14）**：**已实测正常渲染，可保留**。同一轮真机走查里，`●`（U+25CF）、`…`（U+2026）、`·`（U+00B7）、`—`（U+2014）、`→`（U+2192）、`★`（U+2605）全部正常出字形；同期只有 `⌫` 是 tofu。所以「单色几何符号有没有字体覆盖」不能靠码段推理，只能逐个实测 —— 已实测的这几个可以放心用。

**(2b) 逐字实测结果（X6 Pro，2026-11-14 00:10）**

| 字符 | 码位 | 渲染结果 |
| :-- | :-- | :-- |
| `…` | U+2026 | **正常**（三个点） |
| `·` | U+00B7 | **正常**（中点） |
| `—` | U+2014 | **正常**（长破折号） |
| `→` | U+2192 | **正常**（实心箭头） |
| `★` | U+2605 | **正常**（实心五角星） |
| `●` | U+25CF | **正常**（实心圆，密码掩码够用） |
| `⌫` | U+232B | **空白方框 tofu，违规** |

取证方式：在 `PenCG/src/pages/diag/diag.vue` 临时加 3 行字形对照（`A…·—B` / `E● F⌫` / `G→H ★I`），构建装机后 `miniapp_cli capture` 截图、放大 6 倍逐字辨认。证据 `.tmp/g1.png` + `.tmp/g1_zoom.png`。**该 3 行已删除**（`diag.vue` 现 720 行，`grep -c gchk` = 0）。

**由此得到一条新硬规则**（详见 `docs/AI-CODE-RULES.md` §1.2.2）：杂项技术符号区段 **U+2300–U+23FF**（`⌫` `⇧` `⌘` `⏎` `⇥`）在 X6 Pro 上整体不可信，UI 里一律改用中文词或 PNG 图标。当前 `PenCG/src` 该区段命中数为 **0**。

**(3) `PenCG/src/services/session.js:88` —— 注释里的 `⚠️`**

```js
// ⚠️ region 必须按游戏查，不能写死。
```

注释不渲染，不构成违规。但**建议清掉**：`⚠️` 带 U+FE0F 变体选择符，是真正的 emoji 序列；留着会让「grep emoji」的自动审计报出假阳性，增加解释成本。

**历史遗留（已清理）**：`PenCG/src/pages/diag/diag.vue` 曾经有一组「符号 N」测试行，直接渲染 `😀🎮🔥📱` 等真 emoji，用于验证设备字体缺字时的表现。该组已于本轮删除。当前 `diag.vue` 共 720 行，**已无任何 emoji**。

**待确认的连带风险**：`diag` 页仍注册在 `PenCG/src/app.json:10`（`"diag": "pages/diag/diag.js"`），`PenCG/src/services/nav.js:13` 也有路由名 `diag`。需要确认它在**普通用户路径上不可达**（无 UI 入口），否则作为一个开发者诊断页出现在商店包里，本身也是审核风险。**【待确认】**

### 1.3 整改动作

| 项 | 动作 | 状态 |
| :-- | :-- | :-- |
| diag 页 emoji 测试行 | 删除 | **已完成** |
| `session.js:88` 的 `⚠️` | 清掉（仅为降低审计噪声） | 待做（低优先） |
| `play.vue:80` 的 `⌫` | 真机走查；方块则换 PNG / 文字 | **已完成**（实测 tofu → 已换中文「删」） |
| `login.vue:107` 的 `●` | 真机走查；建议直接改 `*` | **无需修**（实测正常渲染，保留） |
| U+2300–U+23FF 区段全量排查 | 新增硬规则并扫全仓 | **已完成**（`PenCG/src` 命中 0） |
| diag 页可达性 | 确认无普通用户入口 | 待确认 |

---

## 2. 禁用固定字号 px

### 2.1 规则原文

> 禁用固定字号 px：改用 vw / vh 等视口单位。

### 2.2 当前状态

**已完成全量迁移。**核查命令与结果：

```
$ grep -rn "font-size:[^;]*px" PenCG/src --include=*.vue --include=*.less | wc -l
0
$ grep -rn "line-height:[^;]*px" PenCG/src --include=*.vue --include=*.less | wc -l
0
```

- `font-size` 共 **175 处，全部为 vw**（分布：`mine-body.vue` 38、`play.vue` 30、`index.vue` 25、`login.vue` 20、`detail.vue` 18、`queue.vue` 16、`search.vue` 14、`base.less` 10、`diag.vue` 4）。
- `line-height` 共 **8 处，全部为 vw**。
- `em` / `rem` **零残留**。

换算基准、真机 A/B 证据、常用对照表、禁用单位说明，全部见 **`docs/VW-VH-GUIDE.md`**。一句话版本：`1vw = 9.6px`、`1vh = 2.66px`，**`vw = px / 9.6`**。

### 2.3 本轮**没有**转换的部分（重要，避免审核方误解）

源码里还有 **351 处布局类 px**（`width` / `height` / `padding` / `margin` / `left` / `top` 等），**本次不转 vw**，这是有意为之，不是漏改：

1. 逻辑画布 960x266 通过 `PenCG/src/app.js:10` 的 `DESIGN_WIDTH = 960` + `onLaunch` 里的 `setViewPort(960)` **1:1 无缩放**渲染，布局 px 等于真实像素，转成 vw 只是同一个数字换一种写法，没有收益。
2. 布局自适应靠 flex（`PenCG/src/styles/base.less` 里的 `.bar` 76px + `.content` 884px = 960），不是靠视口比例。
3. **最硬的约束**：播放页视频区是 KMS 硬件平面挖孔，UI 矩形必须与平面**像素级重合**，否则会出现黑边或错位。该矩形定义在 `PenCG/src/pages/play/play.vue:153`：

```js
var VIDEO = { x: 243, y: 0, w: 473, h: 266 };
```

转成 vw 后一旦有取整误差，画面与 UI 就会错缝。

> 结论：新规第 2 条针对的是**字号**（字号不随视口变化会导致小屏/异形屏字挤爆容器），字号 vw 化已完整覆盖。布局 px 保留是工程判断，如需向审核说明，可用本节文字。

---

## 3. 提高代码质量

### 3.1 规则原文

> 提高代码质量：人工审查、去除冗余、资源按需加载、不得卡顿闪退、禁止不测试直接上传。

### 3.2 人工审查

已做一次覆盖源码全量的上线前审查，成文 `docs/REVIEW-report.md`（781 行），逐条给出严重级别、位置与修法。截至本文写作时的现状（逐条在源码里核对过标记与代码）：

| 编号 | 主题 | 状态 | 检索锚点 |
| :-- | :-- | :-- | :-- |
| F1 | `au_broken_fixable` / `au_start_ms` 未随 AU 复位 | **已修** | `grep -n "F1 修正" cm_rtp.c` |
| N1 | NACK 打包缓冲区越界（132 字节越 146 上限） | **已修** | `grep -n "12 + NACK_MAX_FCI \* 4 + 16" cm_cmd.c` |
| R2 | 重传包未判归属 | **已修** | `grep -n "R2 修正" cm_rtp.c` |
| R5 | 缺口登记只看 seq、不看所属 AU | **已修** | `grep -n "R5 修正" cm_rtp.c` |
| R3 | SPS/PPS 注入与重排顺序 | **已修** | 无 `R3 修正` 标记，凭 `cm_rtp.c` 里「必须先重排、后注入」的注释判断 |
| L1 | stat JSON 缺 NACK / relax 字段 | **已修** | `grep -n "nackRecovered" cm_media.c` |
| S1 | `DEV_AUTOSTART` 非 null 会让生产包自动跳页 | **✅ 已修（2026-11-14，包内核验，见下方）** |
| R4 | IDR hold 会被下一个 FU-A start 清掉 | **未修** |
| P1-7 | `relax_until_ms`（饥饿放行时间戳）不存在 | **未修** |
| L2 | `gap_drop` 三处内联、未抽公共函数 | **未修**（仅 `cm_rtp.c:453` 有定义） |
| L3 | `screen.js` 保活无退避 | **未修** |
| L4 | `log.js` 无日志轮转 | **未修** |
| L5 | 两份文档对 `[cgmedia]` 落盘去向说法矛盾 | **已在 `docs/BUGFIX-SUMMARY.md` §4.1 裁决**（落 `/userdata/applog/DictPen_*.log`，每次开机换名） |
| L6 | `flush_frame` 未查 `cm_buf_put` 返回值 | **未修** |

未修的 6 条（R4 / P1-7 / L2 / L3 / L4 / L6）均**不阻塞上线**：R4 与 P1-7 属于丢包极端场景下的加固，L2 是重构、L3/L4 是健壮性、L6 是错误处理。但它们应当进入下一轮迭代，不应被遗忘。

#### S1：已于 2026-11-14 修复（包内核验通过）

**修复前（历史快照，仅作记录）**：`DEV_AUTOSTART` 曾是 `{ page: 'login', delay: 2500, params: {} }`，是取证留下的开关。它非 `null` 意味着**发布包会自己跳到指定页面**，否则：
- 用户打开应用会莫名其妙进入登录页；
- 若指向 `play` 页会**直接占用机位**，等于免费消耗用户的机位资源。

**当前状态（1.2.0 发布包实测）**：

```js
export const DEV_AUTOSTART = null;
export const DEV_AUTOLOGIN = null;
export const DEV_LOGOUT    = null;
```

不只看了源码，还核了**包内产物**：`8002026110200003.1_2_0.amr` 里的 `dev-autostart-eed06a3a.js.bin` 只有 **139 字节**（三个 `null` 字面量的 AST 骨架），且 `yssy` / 手机号 / 密码 / 测试游戏码全部 0 命中。

**验收命令**（源码必须输出三行 `null`）：

```
grep -n "export const DEV_" PenCG/src/dev-autostart.js
```

**验收命令（产物必须只有 139 字节、且不含凭据）**：

```python
import zipfile
z = zipfile.ZipFile('8002026110200003.1_2_0.amr')
n = [x for x in z.namelist() if 'dev-autostart' in x][0]
d = z.read(n)
print(len(d))                                   # 期望 139
print([b for b in (b'yssy', b'mrzfzmd') if b in d])  # 期望 []
```


### 3.3 去除冗余

已知待清理项：

- **`PenCG/src/styles/base.less:6` 的注释是旧版布局遗留，与当前实现不符**（写的是播放态 213 + 469(视频 469x240 + 进度条 469x26) + 278 = 960）。实际实现见 `PenCG/src/pages/play/play.vue:153`（左 243 + 视频 473x266 + 右 244 = 960，**没有独立进度条**）。本次仅记录事实，**不改源码**。
- **`PenCG/native/csrc/cgmedia/cm_gst.c` 的 `KMS_SINK_PROPS` 宏**（本文写作时在 `:270-272`，上方 `:220-268` 是防改回的长注释）已改为 `sync=false restore-crtc=false`。**引用这类位置时请用符号名 `KMS_SINK_PROPS` 检索，不要用行号**——该文件在本轮迭代里从 1291 行涨到 1537 行，行号漂移过多次。
- 音频相关宏（`AUDIO_AMIXER_PATH` / `AUDIO_AMP_CTL` 等）同样建议按符号名检索。
- `PenCG/src/services/nav.js` 里 `requestHome` / `takeHomeHop` / `clearHomeHop` 这一组「接力旗」**没有调用方**（`grep` 无使用点），是 bug2 调查期留下的死代码。

### 3.4 资源按需加载

**当前状态（如实记录，未做优化）**：

- `PenCG/src/assets/` 只有两个图：`logo.png`（10451 B）、`sponsor.png`（68412 B），合计约 77 KB，都是**打进包里的本地资源**，不涉及网络加载时序。
- `PenCG/src/services/img.js` **只有 `imgUrl(u)` 一个导出**（第 3 行），作用是把 https 降级为 http（设备无 CA），**没有任何预加载 / 懒加载 / 缓存机制**。
- `<image>` 使用点：`PenCG/src/pages/index/index.vue` 6 处、`PenCG/src/components/mine-body.vue` 2 处。
- 游戏封面 / 头像等是**网络图**，由各页面在 `onShow` 里直接赋值 `src`，浏览器式懒加载不存在。

结论：**「资源按需加载」目前只做到了「本地资源本来就小」，没有主动的懒加载设计**。这是本条的**未完成项**。
（⚠️ 该结论写于审计之前，**已被下方 2026-11-14 的状态块修订**，请以状态块为准。）

> **状态（2026-11-14 审计）**：**已逐实证伪上述「未完成」判断的一半，剩下一半需真机验证。**
>
> 审计方法与结论（全部可在仓库内复现，不依赖设备）：
>
> 1. **图片没有内联进 JS bundle，已经是「用到才读」**。证据是已构建的
>    `PenCG/8002026110200003.1_1_1.amr`（27 个 entry）：`logo.png` 与 `sponsor.png`
>    被抽成两个独立文件 `images/15cf4db3086b79b04f55341cea2527b5.png`（10451 B，
>    即 logo）与 `images/92dca343f842484eea37cd25d6ba9839.png`（68412 B，即 sponsor），
>    而 `logo-323472c7.js.bin`（2071 B 的共享 chunk）里只有那两个**路径字符串**。
>    ⇒ 图片字节不在任何 `.js.bin` 里，`<image>` 渲染时才按路径去读。
>    所以「本地图片」这一层**没有可省的首屏带宽**，改懒加载不会有收益。
> 2. **页面已经按页拆 chunk**。同一 `.amr` 里每个页面是独立的
>    `*.js.bin`（index 24244 B、play 51601 B、detail 17857 B、mine-body 23819 B…），
>    不是一个大 bundle。⇒ 「按需加载」在模块粒度上**已经是现状**。
> 3. **唯一确有优化空间的一处：`fixWebpIcons()` 是串行 `await`**。
>    `PenCG/src/services/catalog.js:16-60` 先用 `imgScan` 做 64 字节 Range 探测
>    （`cgnative.c:2625` 的 `js_img_scan`，12 线程并发、单次 5s 超时、
>    只取前 64 字节），再只对判为 `webp` 的 URL 调 `imgConv`。
>    三处调用点：`catalog.js:153`（`loadRecommendations` 末尾
>    `for (i) await fixWebpIcons(out[i].games, ICON_KEYS);`）、
>    `catalog.js:192`（`loadGameList`）、`catalog.js:233`（`loadGameDetail`）。
>    `PenCG/src/pages/index/index.vue:496-505` 的首屏是
>    `await Promise.all([loadBanners(), loadNotices(), loadRecommendations(tabId)])`，
>    所以 **`loadRecommendations` 内部的串行探测会拖住整个首页首屏**
>    （每个推荐模块一个探测 RTT）。
>    **但本批不动它**：改成并发（`Promise.all` 或后置到渲染之后）属于改动
>    `PenCG/src/**`，且收益大小、是否会让首屏图标闪一下（先透明占位再换图）
>    都**必须在真机上量**才能定论。⇒ **列入「需真机验证」，本轮不改代码。**
>
> 结论修订：规则 3「资源按需加载」**不是未做**——图片与页面两级都已是按需；
> 真正的剩余项只有「推荐图标探测串行化」这一处，已记录为待验证优化，
> **不影响本次提审**（它不是「不做按需加载」，只是「首屏多等了几个 RTT」）。

### 3.5 不得卡顿闪退

已知的卡顿/异常形态与处置（均有真机日志，详见 `docs/BUGFIX-SUMMARY.md`）：

| 形态 | 判据 | 处置 |
| :-- | :-- | :-- |
| 播放约 4 分钟画面停住、会话仍在 | `probe` 的 `rx` 冻结、`fps=0`、`ws=open` | **未根治**：服务端单方面停推，客户端只能检测 + 自愈（`docs/BUGFIX-bug1-stallguard.md`） |
| 息屏后会话被拆 | `[pencg][life] app onHide 判为系统息屏` | 已修（`PenCG/src/app.js:82`） |
| 弱网 fps 恒 0 但 `rx` 在涨 | native `still no IDR (...)` | 已修（IDR 饥饿放行 + `checkFreeze` 二期），**真机未验证** |
| 看门狗误杀 | `JS heartbeat lost (18446744073709551615 ms)` | 数值是 2^64−1，属无符号下溢，**时长不可信**；该行已出现在历史日志 `.tmp/e2e/dev.log` |

### 3.6 禁止不测试直接上传

见 §8 的提交前自检 checklist。**PenCG 的每一次上传都必须走完 checklist，并把结果贴进发布说明。**

---

## 4. 多机型适配

### 4.1 规则原文

> 多机型适配：真机测试布局与字号。

### 4.2 当前状态（2026-11-14 已从「完全没有」变为「有探测，X7 未实测」）

**原先完全没有机型探测**，`PenCG/src/app.js` 的 `const DESIGN_WIDTH = 960;` 是写死的常量。**本轮已补上**：

| 项 | 现状 |
| :-- | :-- |
| 探测实现 | `PenCG/src/services/device_profile.js`（202 行，新增） |
| 接入点 | `PenCG/src/app.js:5` `import { bootProfile, FALLBACK_WIDTH, FALLBACK_HEIGHT }`；`:25` `let DESIGN_WIDTH = FALLBACK_WIDTH;`；`:37` 用探测值覆盖；`:42` `this.setViewPort(DESIGN_WIDTH)` |
| 探测来源 | `$falcon.env` 的 `deviceWidth` / `deviceHeight` / `deviceModel`（`native/iot-miniapp-sdk/include/JQuickEnv.h:14-26` 有样例） |
| 兜底 | 读不到 / 读到不可信值 → 一律退回 `FALLBACK_WIDTH = 960`、`FALLBACK_HEIGHT = 266`（`device_profile.js:38/41`），**绝不让探测失败导致整页缩放** |
| 覆盖通道 | `FORCE_WIDTH`（`device_profile.js:46`，默认 0=不强制）+ `$falcon.env.custom` 的 `pencgDesignWidth`（`:86` `readCustomEnv`） |
| 取证日志 | 每次启动一行 `[pencg][I] [device] width=... source=...`（`app.js:51`） |

**仍未验证的**（`device_profile.js` 文件头已用 ★ 声明）：

- X7 Pro 的真机实际视口值、`env` 报的是「视口」还是「物理面板」，**都未验证**（手头只有 X6 Pro）。
- `PenCG/src/styles/base.less` 里仍有硬编码 px 布局（`.page` 宽 960、`.bar` 宽 76 等），所以即使视口探测正确，**布局也不会自动适配 936**。
- 播放页 `VIDEO` 矩形（`play.vue:153`）是 X6 Pro 的绝对值，且 X7 Pro 上 `planeId` / 列偏移都与 X6 不同（见 `docs/DEVICE-MATRIX.md` §1）。**【代码推导，未在 X7 Pro 上实测】**

### 4.3 整改动作

完整的多机型测试矩阵、机型档案、每项判据见 **`docs/DEVICE-MATRIX.md`**。剩余整改建议：

1. ~~移植 `env` 读取 + 机型判定，把 `DESIGN_WIDTH` 变成运行时值~~ —— **已完成**（`device_profile.js`）。
2. `base.less` 的硬编码 px 布局改为按视口计算（受 §2.3「布局 px 本次不转 vw」约束，需要单独评估）。
3. 播放页 `VIDEO` 矩形改为按视口计算（保持 16:9 居中），并处理 X7 的 `planeId` / 列偏移差异。
4. 至少完成 X6 Pro 全量回归 + X7 Pro 冒烟（首屏 / 导航 / 播放 / 字号是否溢出容器）。**X7 Pro 至今零测试。**

---

## 5. 禁用 shell 与 adb 相关功能

### 5.1 规则原文

> 禁用 shell 与 adb 相关功能：不得内置 shell / adb 等系统级能力，相关能力由 PenTool 统一提供。

### 5.2 JS 侧：合规，无任何 shell / adb 能力

`PenCG/src` 内**不存在** shell 执行、`adb` 调用、进程创建能力。核查要点：

- `PenCG/src/services/screen.js:36` 的注释就是设计约束的书面记录：「JS 层不能跑 shell，所以不调 `system()` / `native.exec()`；后端不可用时只降级 + 打一行日志」。
- `grep "exec"` 的全部命中都是**正则**方法 `RegExp.prototype.exec`：`PenCG/src/services/media.js:63`、`:87`、`PenCG/src/services/net.js:29`、`PenCG/src/pages/login/login.vue:429`。
- `adb` 字样只出现在**注释**里（`PenCG/src/dev-autostart.js:6`、`:24`、`:43`，`PenCG/src/pages/index/index.vue:430`），是开发者说明「为什么不用 adb 改配置文件」，不执行任何命令。
- JS 侧唯一的能力面是 `$falcon.jsapi`（`http` / `storage` / `brightness`）与自研 native 模块。

### 5.3 native 侧：有 `popen` / `system` / `fork`+`execl`，但都不是 shell / adb（需向审核说明）

如实登记，不藏：

| 位置 | 调用 | 用途 | 是否 shell/adb |
| :-- | :-- | :-- | :-- |
| `PenCG/native/csrc/cgmedia/cm_gst.c`（`AUDIO_AMIXER_*` / `AUDIO_AMP_*` 宏，写作时 `:365-371`） | 宏定义 `AUDIO_AMIXER_PATH "/bin/amixer"`、`AUDIO_AMIXER_CTL "Playback Path"`、`AUDIO_AMP_CARD "-c 1"`、`AUDIO_AMP_CTL "numid=54"` | 音频通道路由与音量快照 | 否，读/写本机 ALSA mixer |
| `cm_gst.c` 的 `audio_amp_read()` / `audio_snapshot()` / `bt_audio_present()` | `popen(cmd, "r")` | 读 mixer 当前值 / 探测蓝牙音频设备 | 否 |
| `cm_gst.c` 的 `audio_path_set()` | `system(cmd)` | 设置 Playback Path | 否 |
| `PenCG/native/csrc/cgnative/cgnative.c:2847` + `:2856` | `fork()` + `execl("/usr/bin/ffmpeg", ...)` | webp → png 转码（设备不支持 webp） | 否，调设备自带转码工具 |

**判断依据**：这些都是「调一个固定的本地可执行文件完成一件具体的媒体/格式工作」，命令参数是编译期常量或经过校验的路径，**不接收用户输入、不提供任意命令执行能力、与调试桥（adb）无关**，因此不属于规则所指的「内置 shell / adb 等系统级能力」。

**但如果审核口径从严**（要求「不得 spawn 任何外部进程」），整改路径是：`amixer` 改为直接打开 ALSA control 设备做 `snd_ctl_elem_read/write`；`ffmpeg` 改为用 libwebp 或已有的 mpp 解码链。工作量约 2~3 天。**【待确认审核口径】**

### 5.4 开发机侧脚本（不进包）

`PenCG/tools/deploy.ps1` 命中 `adb` 14 次，`PenCG/tools/deploy-net.ps1` 同类。这些是**开发机上的部署脚本，不会被打进 `.amr` 包**。验证方法：

```
# 解包后确认产物内不含 deploy.ps1 / adb 相关文件
$ unzip -l 8002026110200003.1_1_1.amr | grep -i "deploy\|adb"
（应为空）
```

---

## 6. 优化开发文档

### 6.1 规则原文

> 优化开发文档：补 AI 生成软件规范、vw/vh 使用示例、多机型真机测试清单。

### 6.2 本轮补齐的四份文档

| 文件 | 内容 | 对应要求 |
| :-- | :-- | :-- |
| `docs/AI-CODE-RULES.md` | AI 生成代码规范：禁止事项、日志规范、样式规范、注释与文档规范、审查清单、常见翻车模式 | 「AI 生成软件规范」 |
| `docs/VW-VH-GUIDE.md` | vw/vh 换算基准、真机 A/B 证据、常用对照表、用法示例、禁用单位、不该转 vw 的地方、迁移前后像素级 diff | 「vw/vh 使用示例」 |
| `docs/DEVICE-MATRIX.md` | 机型档案、测试项矩阵、布局与字号专项、符号渲染基线、执行步骤、记录模板 | 「多机型真机测试清单」 |
| 本文 `docs/MARKET-COMPLIANCE.md` | 6 条新规逐条对状态、整改清单、提交前 checklist | 「优化开发文档」的总纲 |

`PenCG/README.md` 已新增「开发文档（规范与验证）」小节作为索引入口（在原有 `## 文档` 小节之后）。

---

## 7. 整改清单（可跟踪）

| # | 项 | 对应规则 | 状态 | 验收方式 |
| --: | :-- | :-- | :-- | :-- |
| 1 | diag 页 emoji 测试行 | 规则 1 | **已完成** | `grep` 无 emoji 码段 |
| 2 | 175 处 `font-size` 转 vw | 规则 2 | **已完成** | `grep "font-size:[^;]*px"` = 0 |
| 3 | 8 处 `line-height` 转 vw | 规则 2 | **已完成** | `grep "line-height:[^;]*px"` = 0 |
| 4 | `⌫` / `●` 真机走查 | 规则 1 | **已完成** | `⌫` 实测 tofu → 已换中文「删」；`●` 实测正常 → 保留 |
| 5 | `login.vue` 掩码改 `*` | 规则 1 | **不需要** | `●` 实测正常渲染，无改的必要 |
| 6 | diag 页入口可达性确认 | 规则 1/3 | 待确认 | 全仓无 UI 入口 |
| 7 | 资源按需加载 | 规则 3 | **通过（2026-11-14 审计）** | 图片不进 JS bundle（`images/<md5>.png` 独立文件）、页面按页拆 `*.js.bin`；唯一待优化项 `fixWebpIcons` 串行探测已记入 §3.4「需真机验证」，不阻断提审 |
| 8 | 未修 6 条审查项（R4/P1-7/L2/L3/L4/L6） | 规则 3 | **已修**（本轮补齐） | `grep -rn "REVIEW" src native` 逐条关闭 |
| 9 | 机型探测代码 | 规则 4 | **已写，已真机验证（结论为否定，见 §4.2）** | `PenCG/src/services/device_profile.js` 存在；启动打 `[device] width=... source=...` |
| 9b | X6 Pro 上确认 `[device]` 日志 | 规则 4 | **✅ 已做（2026-11-14）** | 实测 `width=960 height=266 source=fallback raw=800x480 envModel=HaaS UI`，X6 零回归 |
| 10 | X7 Pro 冒烟测试 | 规则 4 | **未做（零测试）** | 按 `docs/DEVICE-MATRIX.md` 记录 |
| 11 | native 进程调用口径确认 | 规则 5 | 待确认 | 审核方书面答复 |
| 12 | 三份新文档 + README 索引 | 规则 6 | **已完成** | 文件存在、README 有小节 |
| 13 | **`DEV_AUTOSTART` 改回 `null`** | 规则 3（不测试直接上传） | **✅ 已完成（2026-11-14）** | 源码三行全 null + 包内 `.js.bin` 仅 139 字节、无凭据 |

---

## 8. 提交前自检 checklist

每次上传前逐项打勾，**并在发布说明里附上结果**。任何一项为「否」就不提交。

### 8.1 构建

- [ ] `PenCG/package.json` 的 `version` 已递增（当前 `1.2.0`），产物名与版本一致
- [ ] `npm run build` 输出 `打包成功`，无新增 warning（已知 warning 见 `docs/DEV.md`）
- [ ] native 侧：`powershell -File native/build.ps1` 退出码 0
- [ ] **`.so` 是否真的更新过，不能看 md5 / 体积判断** —— 装机时 `.so` 会被改名并追加 section，体积会从约 80 KB 涨到约 198 KB。判断依据见 `docs/DEV.md`
- [ ] 解包产物，确认不含 `deploy.ps1` / adb 相关文件

### 8.2 代码

- [ ] `PenCG/src/dev-autostart.js` 三个开关（`DEV_AUTOSTART` / `DEV_AUTOLOGIN` / `DEV_LOGOUT`）**全部为 `null`** —— 1.2.0 发布时已验证通过（源码三行 null + 包内 `.js.bin` 139 字节、无凭据）；此处仍是勾选项，每次打包都要重验
- [ ] `grep -rn "font-size:[^;]*px" src` = 0
- [ ] `grep -rn "[0-9]\(em\|rem\)\b" src --include=*.vue --include=*.less` = 0
- [ ] 新增代码没有引入 `console.warn` / `console.log`（只有 `console.error` 落盘，见 `docs/VERIFY-playbook.md` §0）
- [ ] 没有新增 emoji 到 UI 文案（注释不计）
- [ ] diag 页无调试残留（临时取证块、测试按钮）

### 8.3 真机（X6 Pro，必做）

- [ ] 冷启动 → 首屏正常，无黑屏、无「整页缩放」
- [ ] 左侧导航 5 个 tab 全部可点、切换无残留
- [ ] 搜索页输入法可弹、可输入
- [ ] 登录 → 返回 → 用户信息**立即可见**（不需点回首页）
- [ ] 进游戏 → 画面**在动**（不是静止一帧），无绿屏残留
- [ ] 退出游戏 → 声音停止，其他应用（有道查词）**不再有声音**
- [ ] 回桌面 / 熄屏 10 秒再回来 → 会话仍在
- [ ] 真退后台 → 会话被正确拆掉（**这是一条不能反的回归**）
- [ ] 界面无方块 / 乱码（重点看**中文长名截断**；`⌫` 已换成「删」、`●` 已实测正常，这两项不再是风险点）

### 8.4 真机（X7 Pro，有设备则必做）

- [ ] 首屏布局不裁切、不溢出
- [ ] 字号没有挤爆容器（检查最长文案所在行）
- [ ] 播放页视频区与 UI 不错缝（KMS 平面挖孔对齐）
- [ ] 详见 `docs/DEVICE-MATRIX.md` §2

### 8.5 取证留存

- [ ] 保存本次真机的日志：`P=$(pidof miniapp); readlink /proc/$P/fd/1` 解析出的文件（实测形如 `/userdata/applog/DictPen_YYYYMMDD_HHMMSS.log`）。**JS 的 `[pencg]` 与 native 的 `[cgmedia]` 都在这一份文件里**（两者都跟进程 stdout 走）；`/userdata/applog/loli_s6_dev.txt` 实测是 1 字节空文件，不要归档它
- [ ] 关键界面截图（至少：首屏、播放页、登录后个人页、退出后桌面）
- [ ] 若某项未测，**在发布说明里明确写「未测」**，不要含糊

---

## 9. 事实备注

### 9.1 `base.less:6` 的播放态注释与实现不符

`PenCG/src/styles/base.less:6` 的注释（213 + 469(视频 469x240 + 进度条 469x26) + 278）是旧版布局遗留，与当前实现不符；当前实现见 `PenCG/src/pages/play/play.vue:153`。本次仅记录事实，不改源码。

### 9.2 为什么源码里到处都是 `★` 和 `→`

`★` 是改动锚点（便于 `grep` 找到某次修复的落点），`→` 出现在日志与会话流转说明里。二者都在**注释或日志字符串**内，不渲染到界面。审计时请按「是否在 `<template>` 或 UI 字符串里」判断，而不是按字符计数。

### 9.3 规则 2 与规则 4 的交叉影响

字号改成 vw 后，在 936 宽（X7 Pro）上同一份样式会等比缩小约 2.5%。这是 vw 的**设计意图**（随视口缩放），不是 bug；但如果某些文案在 X6 Pro 上就已经贴边，到 X7 Pro 反而更宽松，反之亦然。多机型走查时**必须核对最长文案**，见 `docs/DEVICE-MATRIX.md` §3。

---

*本文与 `docs/VW-VH-GUIDE.md`、`docs/DEVICE-MATRIX.md`、`docs/AI-CODE-RULES.md` 同期维护。规则条文以市场方发布的原文为准，本文只做映射与现状记录。*
