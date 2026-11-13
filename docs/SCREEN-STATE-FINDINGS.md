# getScreenState() 返回值语义审查结论（bug1 熄屏守卫）

审查日期：2026-11-12 / 2026-11-13
审查方式：**纯离线**（源码通读 + 全仓库日志取证 + 设备二进制静态反汇编），未调用 adb、未接触设备、未修改任何代码。
审查对象：`PenCG/src/services/screen.js`、`PenCG/src/pages/play/play.vue`、`PenCG/src/app.js`
触发问题：bug1 的修复（"游戏开约四分钟后画面停止传输"）依赖 `getScreenState()` 的取值语义，而真机命令行 `hal-screen state` 只观测到 0 和 4，怀疑判据 `n === 3 || n === 1` 恒假、守卫完全失效。

---

## 0. 一句话结论

> **现有 `n === 3 || n === 1` 判据：证据不足，需实测（但"完全失效"不成立）。** 拆开看：`n === 1` 有强证据但**不是 JSAPI 层的直接实测**（详见下方⚠️），`n === 3` 基本无证据；而「JSAPI 只返回 0/4」这个担忧**不成立** —— 那是把另一个通道的 `screen_state:N`（值域恰为 {0,4}，实为 `bl_power`）误当成了 JSAPI `getScreenState()` 的返回值。两个数字从头到尾就是两个量。

**⚠️ 本次取证最重要的边界（必须先读）**：`hal-screen` 与 JSAPI 走的是**两个不同的层**。

| | 调用方 | 底层符号 | 证据状态 |
|---|---|---|---|
| 命令行 | `.tmp/bh2/hal-screen` | `yd_hal_screen_module_new` / `yd_hal_screen_module_destory`（**HAL C API**） | 已反汇编，行为确定（§4.2） |
| JSAPI | `libjsapi_export.so` | `YDALAPI::YBrightnessModule::getScreenState()`（**YDAL C++ 层**） | **实现不在工作区**，无法离线确认（§4.1） |

两者是否最终落到同一个 HAL 函数，**无法离线证明**（`libyddal_business.so` / `libydhal_*.so` 全部缺失）。所以：**HAL 层返回 1，不能直接等同于 JSAPI 层也返回 1**，只能说"极可能"。这条限制决定了本报告的整体判定是"需实测"，而不是"成立"。

三个分句各自的关键证据：

| 结论 | 证据 |
|---|---|
| `getScreenState()` **能**返回 `1`（HAL 层已证） | 设备上 `hal-screen state` 子命令的 `cmp w0,#1` 分支是 `SCREEN_OFF` 的唯一打印者；真机 `off` 时**确实打出了 `get_screen_state: SCREEN_OFF`** → 该次 HAL 调用返回值必为 `1`。反汇编见 §4.2。**但这是 HAL 层，JSAPI 层仍需一次实测**（§6.3）。 |
| 「只返回 0/4」**不成立** | `screen_state:N` 的观测值域 `{0,4}` 与 `bl_power` 的 `{0,4}` 完全重合，且 `down` 时 `screen_state:0` 而 `bl_br` 已掉到 5、`off` 时 `screen_state:4` —— 它是电源/背光电源量，不是 JSAPI 状态量。见 §3.3。 |
| `n === 3`（调暗）**证据不足** | `hal-screen` 在 `down` 后静默 ⇒ 存在非 {0,1} 值，与 3 相容但不确证；全仓库落盘日志中 `getScreenState()` 侧出现过 `state=0` 与 `state=null`，**从未出现过 `state=3` 或 `state=1`**；`3` 只存在于兄弟项目的源码注释里，无一份原始日志佐证。见 §6.1。 |

**可操作的底线**：无论 `3` 是否成立，把判据改成 **`n !== 0`**（读不到时为 `false`）即可同时覆盖 0/1/3/4 四种编号，且只依赖唯一被证实的事实（`0 = 亮屏`）。见 §7。

---

## 1. 依赖 `getScreenState()` 返回值的全部位置

判据本体共 **6 处**（`screen.js` 4 处 + `app.js` 1 处 + `play.vue` 1 处；其中 `screen.js:94` 只影响日志文案，但会主动误导人）。

| # | 位置 | 判据代码原文 | 假设语义 | 若真实语义是 `0/4`（即本报告要排除的那套）会发生什么 |
|---|---|---|---|---|
| 1 | `PenCG/src/services/screen.js:104` | `if (s === 3 \|\| s === 1) return true;` | 3=调暗、1=断电 → 必须救 | 恒假。`needsBrightRestore()` 落到 `:107` 亮度判据，而 `bright` 是档位（恒 20/40/80）→ 恒 false → **每轮硬兜底永不触发**，调暗/断电后屏幕再也不会被拉回。 |
| 2 | `PenCG/src/services/screen.js:113` | `return Number(s) === 1;` | 只有断电深熄态才需要同 tick 连发两次 keep | 恒假。`:389` 的 retry 分支永不进入 → 断电态（bp=4）下每轮只 keep 一次，而实测**单次 keep 不能上电**（`docs/audit/player2-review2.md:462`）→ 背光永远起不来。 |
| 3 | `PenCG/src/services/screen.js:410` | `if (sNum === 1) { ... writeScreenOn(1) ... }` | 断电态走 `screenOnOff(1)`；调暗态只 `setBrightness(80)` | 恒走 else 分支 → 断电态只 `setBrightness(80)`（只改档位、不上电，`PenCG/src/services/screen.js:27-28` 实证）→ **背光不上电**。 |
| 4 | `PenCG/src/app.js:73` | `screenOff = (n === 3 \|\| n === 1);` | 系统息屏 ⇒ 不拆会话 | 恒假 → 系统息屏时被当作「真退后台」→ `:83-106` 走 `stopPlay()` + `stopGame(code)` → **直接结束对局并释放机位**。这正是 bug1 观感（"像自动退出"）的成因链。 |
| 5 | `PenCG/src/pages/play/play.vue:1223` | `return n === 3 \|\| n === 1;` | 同上，`screenIsOff()` 是全部守卫的总闸 | 恒假 → `onHide`(:486) 判为真退后台 → `clearTimers()` + `keepAliveStop()` + `bgTeardown()`(:505) → `stopPlay()` 且 `stopGame()` 释放机位；`onStop`(:512)、`bgTeardown`(:1248) 的熄屏守卫同时失效。 |
| 6 | `PenCG/src/services/screen.js:94` | `if (n === 4) return '未知(4)';` | 4 = 未知 | 仅日志文案，但**它把唯一真正观测到的"非亮屏"值标成了"未知"**，直接导致 §3 的误判；`screenStateName()` 也是 `play.vue:1233 screenText()` 与 `app.js:42` 的唯一可读性来源。 |

**汇总影响**：若假设成立（真实为 0/4），则 1~5 全部恒假，`keepAlive` 的"救屏"能力与"系统息屏不拆会话"能力**整体失效** —— 但本报告结论是：**该假设不成立**，真正的问题只在第 3 个值（调暗）上不确定。

---

## 2. 证据分级说明（很重要）

本仓库关于 `getScreenState()` 取值的材料分三类，**可信度差三个数量级**，此前所有争议都源于把它们混为一谈：

| 级别 | 材料 | 数量 | 能否作为证据 |
|---|---|---|---|
| **A. 原始落盘日志** | `.tmp/*.log`、`.tmp/bug1-soak.log`、`.tmp/e2e/dev.log` 里设备侧采样行与 `[screen]`/`[pencg]` 行 | 数千行 | ✅ 唯一硬证据 |
| **B. 设备二进制静态反汇编** | `.tmp/bh2/hal-screen`、`env/tmp/libjsapi_export.so` | 2 个 ELF | ✅ 强证据（不可翻案的机器码） |
| **C. 源码注释 / review 文档转述** | `PenBili`/`PenDouyu`/`PenYS`/`VocabularyMemorization` 的 `screen.js:313-323` 注释块、`docs/readme-facts.md:243`、`docs/audit/player2-review2.md:459`、`PenCG/docs/BUGFIX-bug1-lifecycle.md:150` | 6+ 处 | ❌ **不是证据** —— 全部同源（PenBili 一次注释），四个项目互相 copy，重复出现不代表独立验证 |

> **关键澄清**：`docs/design/playback-rewrite-adr.md` 里**没有**任何 0/1/3 编号的记载。对该文件全文 grep `getScreenState|screenOnOff|SCREEN_ON|DIM` 只有 3 处命中（`:406` 列方法名、`:651` 说 `screenOnOff(1)` 能拉回、`:655` 说主判据必须用 `getScreenState()`），**均未声称取值映射**。所以"ADR 记录了 JSAPI 是 0/1/3"这个说法本身有误。

---

## 3. 核心争议的解构：`screen_state:N` 与 `getScreenState()` 是两个量

### 3.1 `screen_state:N` 不是 `hal-screen` 打的

对 `.tmp/bh2/hal-screen`（设备上该工具的副本，ELF64 AArch64，10096 字节，2026-10-31）做 `strings -a`，屏幕相关字符串**完整清单**只有：

```
get_screen_state: SCREEN_ON
get_screen_state: SCREEN_OFF
yd_hal_screen_module_new
yd_hal_screen_module_destory
%s yd_hal_screen_module_new return NULL.
set [0-100]:set screen brightness value   get:get screen brightness
on:set screen power on                    off:set screen power off
switch:set screen power swithch           state:get screen power state
down:set screen brightness down           restore:set screen brightness restore
reset: reset screen state on              keep: keep screen on 30s
```

**没有** `screen_state` / `sys_br` / `bl_br` / `map2brightness` 任何一个字符串（`strings | grep -c screen_state` → 命中数仅来自上面两条 `get_screen_state`；`grep -E "sys_br|bl_br|map2brightness"` → 0 命中）。
→ 所以形如

```
screen_state:0 sys_br:80 bl_br:113 map2brightness:113
```

的复合行**必然来自另一个生产者**（设备上一个未入库的采样脚本 / 另一个版本的 hal-screen）。而仓库里也确实找不到它：`grep -rln "map2brightness" --include="*.sh"` 全树 0 命中；`strings` 扫 `env/tmp/*.so` + `*.txt` 也 0 命中。

> 顺带纠正一处工程瑕疵：`PenCG/../.tmp/m_idle.sh:24` 写的是 `ss=$(sh_ "hal-screen state 2>/dev/null | grep -o 'screen_state:[0-9]*'")` —— 在 10-31 版 `hal-screen` 上**这个 grep 永远为空**，因为它只打 `get_screen_state:`。该脚本的 `screen_state` 列历史上可能是空的，取数需谨慎。

### 3.2 `screen_state:N` 的值域恰好等于 `bl_power`

全仓库 `screen_state:N` 精确统计（`grep -rhoE "screen_state:[0-9]+"`）：

| 取值 | 次数 |
|---|---|
| `screen_state:0` | **249** |
| `screen_state:4` | **23** |
| `screen_state:1` | **0** |
| `screen_state:3` | **0** |

`{0, 4}` 这个值域不是巧合 —— 它正是 `/sys/class/backlight/backlight/bl_power` 的取值域（0=上电，4=断电）。交叉验证三重吻合：

**(a) 与 `bl_br` 正交。** 按 `screen_state / sys_br / bl_br` 三列交叉统计：

| screen_state | sys_br | bl_br | 次数 | 解读 |
|---|---|---|---|---|
| 0 | 40 | 59 | 376 | 正常亮屏 |
| 0 | 80 | 113 | 65 | 正常亮屏（满档） |
| 0 | 20 | 32 | 48 | 正常亮屏（低档） |
| **4** | 40 | **59** | 24 | **背光电源已断，但 bl_br 文件还没更新** |
| **4** | 40 | **5** | 16 | 断电 + 已调暗 |
| **4** | 80 | **5** | 1 | 同上 |

关键在 `screen_state:4 / bl_br:59` 那 24 行：`bl_br` 是**满值 59（完全没暗）**，`screen_state` 却已经是 4。这说明 `screen_state` **不是**从亮度推出来的，而是一个独立的"电源"量；`sys_br→bl_br` 的映射关系也自洽（80→113、40→59、20→32），说明 `map2brightness` 是算出来的档位。

**(b) 与电源键事件同刻。** `.tmp/e2e/dev.log` 里 `screen_state:4` 的采样紧跟在
`[YKeyEventTranslator] key=Power ... emit onKeyClick` / `[YButtonEventModule] [onPowerButtonClick] ::onPowerButtonClick()`（dev.log:2776-2779）之后 —— 即 **4 = 用户按电源键 / 手动关屏路径**。

**(c) `down`（只调暗）时是 0，`off`（真断电）时是 4** —— 与 Lead 的真机记录完全一致，也与 `bl_power` 在 `down` 时保持 0、`off` 时变 4 的实测一致（`PenCG/docs/BUGFIX-bug1-lifecycle.md:40-42`：`270s` → `hal-screen down` → `bl_br 113→5`（`bl_power` 仍 0）；`300s` → `hal-screen off` → `bl_power 0→4`）。

### 3.3 因此 Lead 的两条关键观测，正确读法是

Lead 记录（m00001）与反汇编对照：

| Lead 观测 | 复合行第一段 `screen_state:N` | 第二段 `get_screen_state:` | 对 `getScreenState()` 的推论 |
|---|---|---|---|
| `keep` / `on` / `reset` | `screen_state:0` | `SCREEN_ON` | **= 0** ✅ |
| `down`（270s 调暗，bl_br 5） | `screen_state:0`（=bl_power 仍 0） | **不打印** | **∉ {0,1}** ← 关键 |
| `off`（300s 断电，bl_power 4） | `screen_state:4`（=bl_power 4） | `SCREEN_OFF` | **= 1** ← 关键 |

**"不打印"和"打印 SCREEN_OFF"是这段证据里最有信息量的部分**，它们恰好证明了：

- `off` 时 `getScreenState()` 返回 **1**（否则不可能走进 `cmp w0,#1` 的 `SCREEN_OFF` 分支）→ **1 是真实可达的取值，"只返回 0/4"被证伪**；
- `down` 时 `getScreenState()` 返回**既不是 0 也不是 1**的某个值 → 调暗态有独立编号，与文档声称的 `3` 不矛盾（但也没被证实）。

---

## 4. 二进制反汇编（机器码级证据）

### 4.1 JS 桥接层是纯直通，不做任何值映射

`env/tmp/libjsapi_export.so`（设备 `/etc/miniapp/jsapis/libjsapi_export.so` 副本，1316816 字节，ELF64 AArch64）。
`JSBrightnessProxy::getScreenState` @ `0xc9bd4`（68 字节）与 `JSGlobalProxy::getScreenState` @ `0x9c6fc`（68 字节）**结构完全相同**：

```asm
0xc9be4  bl  YDALBASEAPI::YSingleton<YDALAPI::YBrightnessModule>::instance()   ; PLT#800 @0x8da00
0xc9be8  bl  YDALAPI::YBrightnessModule::getScreenState()                      ; PLT#655 @0x8d0f0
0xc9bec  mov w19, w0                     ; ← 原始返回值，未加工
0xc9bf4  adrp x1,#0x106000 ; add x1,#0x685   ; "JSBrightnessProxy::getScreenState %d\n\n"
0xc9bfc  mov w0,#0x80 ; bl YLog_print
0xc9c04  mov w1, w19 ; add x0, x20, #0x50
0xc9c14  b   JQReturnValue::Set(int)                                            ; PLT#675 @0x8d230
```

函数体内**没有任何 `cmp` / `csel` / 分支**（对比 `screenOnOff` @ `0xc9c18` 有 `cmp w1,#0` @0xc9c4c 与 `cmp w1,#0 / ccmp w1,#7,#4,ne` @0xc9c5c 的参数校验），`getBrightness` @ `0xc9b90` 同构。

> **结论**：JS 侧拿到的就是 `YBrightnessModule::getScreenState()` 的原始 `int`。`0/1/3` 若存在，只能来自该模块本身；**桥接层不会把 3 归一化成 4、也不会把 4 归一化成 1**。所以"编号是两套"这个说法在多通道层面判断正确，但不能推出"JSAPI 只返回 0/4"。

`YBrightnessModule::*` 在 `libjsapi_export.so` 里全部是**未定义符号**（`st_value=0, st_size=0, st_shndx=0`），实现在 `libyddal_business.so` / `libydhal_*.so`（DT_NEEDED 里有 60+ 个），而**这些库工作区里一个都没有**（`find . -name "*yddal*" -o -name "*ydhal*"` 零命中）→ 模块内部逻辑无法离线反汇编，这是本次取证的硬边界。

### 4.2 `hal-screen state` 只认 0 和 1（这段是判定"1 可达"的关键）

`.tmp/bh2/hal-screen`，`.text` addr=off=0xe00 size=0x550，`state` handler @ `0x1270`：

```asm
0x1270  stp x29,x30,[sp,#-0x20]!
0x1294  str wzr,[sp,#0x14]        ; int st = 0
0x1298  ldr x1,[x0,#0x38]         ; module->getScreenState，写出参数=&st
0x12a0  blr x1
0x12a4  ldr w0,[sp,#0x14]
0x12a8  cbz w0,#0x12d4            ; ==0 → 打印 "get_screen_state: SCREEN_ON"
0x12ac  cmp w0,#1
0x12b0  b.eq #0x12e4              ; ==1 → 打印 "get_screen_state: SCREEN_OFF"
0x12b4  ...                       ; 其它值：直接落空，什么都不打印
```

**设计后果**：
- `get_screen_state: SCREEN_OFF` 这一行的**唯一**生产者就是 `cmp w0,#1` 分支 → **只要它出现过，那次 `hal-screen` 的 HAL 调用就必然返回 1**。真机 `off` 时观测到了它 ⇒ **HAL 层 `getScreenState` 返回 1 已证实**。⚠️ 但注意这是 **HAL** 层；JSAPI 走的是 `YDALAPI::YBrightnessModule::getScreenState()`（§4.1），两者是否同一实现无法离线证明 —— 这是本报告仍判"需实测"的原因。
- 任何非 `{0,1}` 的值（含 3、含 4）都会**静默不打印** → 所以"`down` 时没有 get_screen_state 行"本身就是一条正面证据：调暗态返回了非 0/1 的值。
- 值 `4` 在 `hal-screen` 里也是静默的 → 这也解释了为什么 `screen_state:4` 那个数字看起来"无人认领"：**它压根不经过这个函数**。

### 4.3 JS 侧另有一条推送通道（未验证，但值得记录）

`initObserver` @ `0xca7a0`（856 字节）注册了 3 个 observer，绑定
`JSBrightnessProxy::onScreenStatusChanged(bool)` @ `0xca528` /
`onBrightnessValueChanged(int)` @ `0xca6b8` / `onBrightnessChange`，
`.rodata` 中对应 JS 属性名：`brightnessChanged` @0x106770、`screenStatusChanged` @0x106782、`brightnessValueChanged` @0x106796、`onBrightnessChange` @0x1067ad。
日志串 `JSBrightnessProxy::onScreenStatusChanged onoff ` @0x106807，源码路径 `/data1/hardware/workspace/Melon_pro_Package/app/yd-dictpen-haasui/jsapi/system/jsapi_brightness_module.cpp`。

`onScreenStatusChanged` 的参数是 **bool（on/off）**，不带 0/1/3/4 编号 —— 理论上是最不依赖编号语义的通道。**但**：全仓库 `grep -rn "screenStatusChanged|brightnessChanged|brightnessValueChanged|onBrightnessChange" --include=*.js --include=*.vue` **0 命中**，四个兄弟项目没有任何一处订阅过它。属于**完全未验证的 API**，不能作为方案地基（可作为后续可选实验）。

---

## 5. 关于 `getBrightness()` 与 sysfs 的硬事实

| 事实 | 出处 |
|---|---|
| `getBrightness()` 返回的是**设置档位**（sys_br），不是实际背光；屏幕被调到 `bl_br=5` 时它仍返回 80 | `PenCG/src/services/screen.js:26`；实测复现 `docs/audit/player2-review2.md:462`、`.tmp/js2/review2_append.md:82` |
| `setBrightness()` / `hal-screen set N` **只改档位、不上电**；能上电的是 `keepScreenOn()` / `startAlwaysScreenOn()` / `screenOnOff(1)` | `PenCG/src/services/screen.js:27-28`、`:402-405` |
| 深熄态（`bp=4`）**单次 keep 不够**，第 1 次只置 `/tmp/.screen_state=1`、`bp` 仍 4、`br` 变 113，第 2 次才 `bp=0` | `PenCG/src/services/screen.js:27-28`；`docs/audit/player2-review2.md:462`；`PenYS/docs/feasibility.md:93` |
| **`bl_power` 是唯一可靠的物理判据**；`/tmp/.screen_state` 方向是反的（熄屏写 `screen_on=1`、亮屏写 `screen_on=0`），绝不能当判据 | `.tmp/rv2/snap9/core/clock.c:12-23`（PenBili native 实测注释）；`.tmp/rv2/snap8/core/clock.h:25-40` 的 `pb2_screen_off()` 契约：1=熄屏 / 0=亮屏 / -1=无法判断 |
| `bl_power` 与 `brightness` 都是 <32 字节小文件，单次 open+read+close **<50us** | `.tmp/rv2/snap8/core/clock.h:25-40` |
| **JS 层读不到 sysfs**：`strings -a env/tmp/libjsapi_export.so` 对 `backlight / bl_power / /sys/class / /proc/ / readFile / openFile` **零命中**，整个 .so 没有 shell/exec 类 JSAPI | 本次反汇编 + `strings` 核查 |
| `PenCG/native/csrc/cgnative/cgnative.c` **不读 sysfs**，导出表只有 `cancelAll / sha256Hex / md5Hex / b64enc / b64dec / tripleDesB64 / randomHex / hmacSha1B64 / utf8Hex / version / wsOpen / wsState / wsSend / wsPoll / wsPollEx / wsClose / wsList / wsLastFrame / imgScan / imgConv` | `cgnative.c:3025-3046`（`CG_EXPORTS` + `registerCModuleLoader("cgnative", ...)`） |

**推论**：要在 JS 里拿到真实的 `bl_power`，必须**新增一个 native 导出方法并重新交叉编译 `cgnative.so`**（成本：一处 C 函数 + 一次交叉编译 + 一次设备部署）。当前没有这条通路。

---

## 6. 结论

### 6.1 `n === 3 || n === 1` 到底成立吗

拆成两半回答：

| 子判据 | 判定 | 依据 |
|---|---|---|
| `n === 1`（熄屏断电） | 🟡 **强证据，但需一次 JSAPI 层实测确认** | (i) `hal-screen state` 的 `SCREEN_OFF` 只可能来自 `cmp w0,#1`（§4.2），真机 `off` 时观测到了它 → **HAL 层**返回值必为 1；(ii) 与 `docs/audit/player2-review2.md:462` 记录的深熄态行为（`bp=4`）在语义上一致；(iii) **但 HAL ≠ JSAPI 层**（§4.1、§0 的边界表），两者是否同一实现无法离线证明。 |
| `n === 3`（调暗） | ⚠️ **证据不足，需实测** | (i) `down` 时 `hal-screen` 静默 → 值 ∉ {0,1}，与 3 相容，但也与其它任意非 0/1 值相容；(ii) 全仓库落盘日志中 JS 侧 `[screen] keep ... state=` 出现过 `state=0`（78 次以上）与 `state=null`，**`state=3` / `state=1` / `state=4` 一次都没有**；(iii) 声称 3 的 6 处全是同源注释转述（§2 的 C 级材料）。 |
| 「JSAPI 只返回 0/4」 | ❌ **不成立** | §3.2 / §3.3 / §4.2：`0/4` 是 `screen_state:N`（= `bl_power`）的值域，与 JSAPI 无关；`hal-screen` 里 4 连打印分支都没有，它根本不经过 `getScreenState`。 |

**所以 bug1 守卫不是"完全失效"，而是"没验过"**：若 JSAPI 与 HAL 同源（可能性较高），真断电（300s 之后）时 `n===1` 会命中，会话不会被打断；**真正的缺口在 270s（4 分 30 秒）那一步 —— 那是调暗，恰好正是用户报障的时间点，而这一步依赖完全未证实的 `3`。** 这就是"判据看起来存在、bug 却照旧复发"的最合理解释。

### 6.2 一条独立于本议题的重要旁证：bug1 并非只由熄屏驱动

`.tmp/bug1-soak.log`（2026-11-12 22:46–22:57，与 Lead m00328 同一次会话，原神在玩，device `9D91500007505171`）里，21 个采样点全部是：

```
t1 22:46:30 | screen_state:0 sys_br:20 bl_br:32 map2brightness:32 get_screen_state: SCREEN_ON | amp=... aplay=7135
```

而同一时段的 JS 侧是
`[pencg][W] [screen] keep #10 backend=brightness.Brightness(new) ok bright=20 state=0(ON)`、
`[play] probe stage=live v=1 screen=0(ON) phase=playing ws=open rx=... fps=29 kf=3 keep=on`。

**从 t5（22:48:34）起 `fps` 掉到 0、`rx` 冻在 18701 不再增长，而 `screen_state` 全程保持 0/SCREEN_ON、`keep` 全程 `keep=on`。**
即：**在完全没有发生任何熄屏事件、保活一直生效的情况下，画面照样停止传输了。**
→ 熄屏守卫的修复是必要的但**不充分**；bug1 至少还有一条与屏幕状态无关的成因（传输/解码侧），修判据不能指望单点修复 bug1。这条建议单独开一条排查线。

---

### 6.3 收口条件：一次就能定案的实测

本报告不需要推翻任何东西，只需要**一条日志**。当前 `PenCG/src/services/screen.js:441-445` 每 20s 已经打印
`keep #N ... state=<raw>(<name>)`，其中 `state` 就是 `getScreenState()` 的原始返回值。因此：

1. 用现成包，让设备自然进入 `[Idle]` 270s（调暗）与 300s（断电）——**不需要任何新代码**；
2. 之后取 miniapp 进程的日志（`P=$(pidof miniapp); L=$(readlink /proc/$P/fd/1)`，实测形如 `/userdata/applog/DictPen_YYYYMMDD_HHMMSS.log`），`grep -a "keep #" "$L"` 即可读到那两次的 `state=` 原始值（**不要用 `loli_s6_dev.txt`，那是 1 字节空文件**）；
3. 判定：若调暗时出现 `state=3(DIM)`、断电时出现 `state=1(OFF)` → 现有判据成立，bug1 守卫有效，可以不改；若调暗时仍是 `state=0(ON)`、断电时是 `state=4(...)` → JSAPI 与 HAL 不同源，判据按 §7 改。

在拿到这条日志之前，**§7 的 `n !== 0` 判据是无需额外信息的稳妥选择**（它不依赖 3 是否存在）。

---

## 7. 建议的稳妥判据（不依赖 `3` 是否成立）

### 7.1 核心表达式：把"仅认 3/1"改成"只要不是 0 就算非亮屏，但读不到不算"

```js
/* 建议替换 PenCG/src/pages/play/play.vue:1223 的 return n === 3 || n === 1; */
function isScreenNotOn(st, bright) {
  var n = Number(st);
  if (st == null || !isFinite(n)) {
    // 读不到：不判熄屏（宁可拆平面，也不能让 KMS 平面永久盖住系统 UI）
    // 亮度仅作弱兜底 —— 注意 getBrightness 返回的是档位，只在档位被调到极低时有意义
    var b = Number(bright);
    return bright != null && isFinite(b) && b <= KEEP_BRIGHT_MIN;   // KEEP_BRIGHT_MIN = 20
  }
  return n !== 0;    // 0=亮屏（真机确认）；1/3/4/其它 一律视为非亮屏
}
```

**逐条取值含义与证据出处**：

| `getScreenState()` | 含义 | 证据 | `n !== 0` 判为 |
|---|---|---|---|
| `0` | 亮屏（SCREEN_ON） | `.tmp/bug1-soak.log` 全程 21 点 + `get_screen_state: SCREEN_ON`；`.tmp/e2e/dev.log` 大量；`PenBili...` 注释 | 亮屏（不拆） |
| `1` | 熄屏 / 背光断电 | §4.2 `cmp w0,#1`→`SCREEN_OFF` 分支，Lead `off` 实测命中；`docs/audit/player2-review2.md:462` 深熄态行为 | 非亮屏（不拆会话） |
| `3` | 调暗（背光仍上电） | **仅有 C 级注释**；但 `down` 时 `hal-screen` 静默证明其存在一个非 {0,1} 的值，与 3 相容 | 非亮屏（不拆会话） |
| `4` | 未在任何 JS 侧日志出现；`hal-screen` 对 4 静默 | §3.2/§4.2 | 非亮屏（保守，不拆会话） |
| `null` / `NaN` | 模块不可用或抛错 | `PenCG/src/services/screen.js:288-297` `readState()` 契约 | **不判熄屏**（走亮度弱兜底）→ 保证真退后台时平面一定拆得掉 |

这样做的三个好处：
1. **对 3 不敏感** —— 3 成立或不成立，行为一致，都不再需要真机确认才能上线；
2. **只依赖唯一被证实的事实** —— 全仓库唯一能被 A 级日志 + B 级反汇编共同确认的取值只有 `0 = 亮屏`（§3、§4.2）。判据只建立在"已是 0 就别动，其余一律当非亮屏"之上，不需要相信任何未证实的编号；
3. **`null` 仍然保守** —— 避开了 `PenCG/docs/BUGFIX-bug1-lifecycle.md:352-353` 警告过的坑（"`n !== 0` 会把「读状态失败」也算成熄屏，真退后台时平面就拆不掉了"）。上面表达式通过显式排除 `null/NaN` 解决了这个缺陷。

> ⚠️ **必须澄清一个流传已久的错误说法**：多处文档与源码注释声称"兄弟项目的页面判据是 `s !== 0`"（`PenBili/src/services/screen.js:323`、`PenDouyu/src/services/screen.js:335`、`PenYS/src/services/screen.js:335` 三处注释原文均为"只有 0 能确定为亮屏 —— 页面 isScreenOff() 用的是 `s !== 0`，与此一致"）。
> **但它们的实际代码并不是 `s !== 0`。** 逐个核对：
> - `PenBili/src/pages/index/index.vue:1719-1727`：`isScreenOff(st){ ... const n = Number(s); return n === 1; }`
> - `PenDouyu/src/pages/index/index.vue:1269-1277`：`const n = Number(s); return n === 1;`（`:1272` 注释："只有「断电熄屏(1)」算黑屏，调暗(3)仍然可见 → 不让位"）
> - `PenYS/src`、`VocabularyMemorization/src`：**没有 `isScreenOff` 这个函数**（grep 0 命中）
>
> 全仓库"`!== 0`"只出现在**那三条注释里**，没有一处真实代码。所以"与兄弟项目一致"这条论据**不成立** —— 它们三个的真实判据是 `n === 1`（比 PenCG 少认一个 3）。本报告推荐 `n !== 0` 的理由是上面第 1、2、3 条，与兄弟项目无关。

> 另注：兄弟项目用 `n === 1` 而非 `n !== 0` 是**有正当原因的**，但那个原因不适用于 PenCG —— 它们把该判据用于"看门狗是否让位"（调暗时用户仍看得见画面，看门狗不该让位，否则会把真卡死拖成不可恢复，见 `PenDouyu/src/pages/index/index.vue:1265-1268`）。PenCG 用它决定"系统息屏时是否拆会话/释放机位"，语义相反：调暗恰恰是 bug1 的触发点，**应该在调暗时就保住会话**。所以 PenCG 保留 3（甚至放宽到"非 0"）是对的，不必跟着兄弟项目走 `=== 1`。

### 7.2 需要同步改的另外三处

| 位置 | 现状 | 建议 |
|---|---|---|
| `PenCG/src/app.js:73` | `screenOff = (n === 3 \|\| n === 1);` | 改成 `screenOff = (n !== 0);`（`screenSt == null` 已在 `:70` 排除，保持"读不到→走保守拆会话"） |
| `PenCG/src/services/screen.js:104` | `if (s === 3 \|\| s === 1) return true;` | 改成 `if (s === 0) return false; if (s != null && isFinite(s)) return true;` 之后保留亮度兜底 —— 即「非 0 且数值有效 → 救」 |
| `PenCG/src/services/screen.js:94` | `if (n === 4) return '未知(4)';` | 改成 `return '非ON(' + n + ')'`，避免日志把真实状态误标成"未知"（本次误判的直接诱因） |
| `PenCG/src/services/screen.js:113` | `return Number(s) === 1;` | **可以保留不动**。`1` 已证实，且"连发两次 keep 只对断电态有意义"这一语义正确；若想更保险可放宽到 `n !== 0`，代价只是调暗时多一次 keep（无害） |
| `PenCG/src/services/screen.js:410` | `if (sNum === 1)` 走 `screenOnOff(1)` | **建议放宽为 `sNum !== 0 && sNum != null`**：调暗态当前走 `setBrightness(80)` 是够的（实测 `down` 后 `br` 会回 113，`docs/audit/player2-review2.md:462`），但既然 `screenOnOff(1)` 是唯一真正上电的动作，断电态判定放宽后能覆盖"编号未知但实际断电"的情形 |

---

## 8. 若仍要坐实 `3`：最小侵入测量方案

**不需要改任何代码。** 现有日志已经够用 —— 缺的只是**一次覆盖 270s→300s 的落盘 soak**（`[screen] keep #N ... state=N(NAME)` 每 20s 一行，`app.js:42` 的 `[pencg][life] app onHide screen=N(...)` 也会打出当时的原始值，而这一行**至今在任何落盘日志里都没出现过**）。

### 8.1 落地通道（先确认，否则 grep 全是空的）

按 `PenCG/docs/BUGFIX-bug1-lifecycle.md` §6.0：Falcon 里 JS **只有 `console.error` 会写盘**，`console.warn`/`console.log` 完全不落盘；行首级别恒为 `[E]`，真实级别看行内 `[pencg][W]`。PenCG 所有日志经 `PenCG/src/services/log.js:44` 的 `console.error(line)`，满足条件。

**落盘文件不能写死**：它是 miniapp 进程的 stdout，文件名带启动时间戳、每次重启都换。

```bash
ADB="./env/adb/adb.exe -s 9D91500007505171"
P=$($ADB shell 'pidof miniapp' | tr -d '\r')
L=$($ADB shell "readlink /proc/$P/fd/1" | tr -d '\r')   # 实测 /userdata/applog/DictPen_20261113_232219.log
echo "LOG=$L"
$ADB shell "grep -a -n -E 'pencg.*(keep #|app onHide|onHide 但屏幕|onStop 但屏幕|probe stage)' \"$L\" | tail -80"
```

> **路径更正（2026-11-14 真机定论）**：本节早期版本写「落盘文件是 `loli_s6_dev.txt`、
> `DictPen_*.log` 在本机不存在」，**两句都反了** —— `loli_s6_dev.txt` 实测只有 1 字节，
> 是空文件；真日志正是 `DictPen_YYYYMMDD_HHMMSS.log`。

### 8.2 需要制造的三段屏幕状态（同一局内连续采样）

| 段 | 制造方式 | 期望硬件读数 | 要采的 JS 行 | 判读 |
|---|---|---|---|---|
| ① 亮 | `hal-screen on` / `hal-screen reset; hal-screen restore` | `bl_power=0`、`bl_br=113`、`get_screen_state: SCREEN_ON` | `[screen] keep #N ... state=0(ON)` | 基线（已多次确认） |
| ② 270s 调暗 | **不要碰设备**，让 `[Idle] 270s` 自然触发 `hal-screen down`（或手动 `hal-screen down` 精确注入） | `bl_power=0`、`bl_br` **113→5**、`hal-screen state` **静默** | `[screen] keep #N ... state=?` | 若 `state=3` → 判据成立；若是别的值 → 记下真实编号 |
| ③ 300s 断电 | 继续不碰，让 `[Idle] 300s` 触发 `hal-screen off`（或手动 `hal-screen off`） | `bl_power` **0→4**、`hal-screen state` 打 `SCREEN_OFF` | `[pencg][life] app onHide screen=?` + `[play] onHide 但屏幕...` + `[screen] keep ... state=?` | `off` 会同时触发 miniapp onHide（`.tmp/js2/review2_append.md:67-73` 实测），正好一次性拿到 `app.js:42` 的原始返回值 |

**关键点**：②③ 必须在**同一局、保活开启时**做，因为 `PenCG/src/services/screen.js:38` 的 20s keep 会不断把屏幕拉回来 —— 我们要采的正是"**被拉回之前**那一轮读到的值"。若 keep 太快导致每次都采到 0，就把 `KEEP_INTERVAL_MS` 临时调大到 35s（**仅测试包**，>30s 会失效保活，但正好能稳定采到掉屏瞬间）。

### 8.3 若要同时拿到物理真值（可选，需 root shell）

复用仓库里现成的采样脚本 `.tmp/bh2/ab.sh`（读 `brightness`/`bl_power`/`/tmp/.screen_state`，状态变化才打印，并统计 `dmesg | grep -aE 'crtc_atomic_(disable|enable)'`）：

```bash
BP=$(cat /sys/class/backlight/backlight/bl_power)
BR=$(cat /sys/class/backlight/backlight/brightness)
hal-screen state 2>/dev/null          # 注意：只对 0/1 有输出
```

把 `ab.sh` 与 §8.1 的 grep **同时**跑，就能把「JS 侧 `state=N`」与「物理 `bl_power`/`bl_br`」逐点对齐 —— 这是唯一能一劳永逸关掉本议题的方法，成本约 6 分钟 idle。

### 8.4 采到之后怎么判

- ② 段出现 `state=3` → `n===3` 成立，现有判据**完全正确**，无需改动（但 §7.1 的 `n !== 0` 仍建议采用，因为它对未知值更健壮）。
- ② 段出现 `state=4` 或其它值 → `n===3` 不成立，**必须**改成 `n !== 0`。
- ② 段依然是 `state=0` → 说明 JSAPI 对"仅调暗"不敏感（值不变化），此时 `3` 根本不存在，**必须**改成 `n !== 0`，并且**不能再依赖 getScreenState 识别调暗**，需要按 §9 换成别的判据。

---

## 9. 退而求其次：不区分"熄屏"与"真退后台"的方案评估

前提：`getBrightness()` 不可靠（返回档位）；`bl_br`/`bl_power` JS 读不到（§5）；`screenStatusChanged` 未验证（§4.3）。

### 9.1 方案 A（任务里提到的那个）：onHide 就拆 KVM 平面，但保留会话/机位；onShow 重建

**技术上完全可行**，仓库已有现成手段：

- `PenCG/src/pages/play/play.vue:1466 hideVideoPlane()` → `setVideoVisible(false)` → native `cm_cmd.c` 执行 `cm_box_set_rect(s->vbox, 0, 0, 1, 1)`（把平面缩到 1×1 像素），日志 `#%d video plane hidden (1x1)`；
- `PenCG/src/pages/play/play.vue:1473 showVideoPlane()` → `setVideoVisible(true)` → `cm_box_reset_rect(...)`，即 **READY 重置**：不销毁 appsrc/caps，只让 kmssink 重新分配 plane 并重读 `render-rectangle`，并复位 `video_ok / media_ev_sent / have_kf / kf_count / pli_burst` 后补 PLI。

| 维度 | 评估 |
|---|---|
| **KMS 平面残留** | ✅ **严格变好**。每一次 onHide 都必然摘掉平面，"游戏都退出了画面还盖在系统界面上"这个用户报障在结构上不可能再发生。 |
| **bug1（会话被打断 / 释放机位）** | ✅ 变好 —— 只要 onHide 不再调用 `stopPlay()` + `stopGame()`，任何屏幕事件都无法结束对局。但**注意**：`bgTeardown()`（`play.vue:1238-1275`）里"拆平面"和"停会话"是**同一个函数**，方案 A 要求把它们**拆开**：拆平面 → 每次 onHide 都做；停会话/释放机位 → 只在真退后台时做。 |
| **恢复代价** | ⚠️ 每次 onShow 要走 READY 重置，**必须等一个 IDR 才出画**。`cm_cmd.c:167-178` 注释明确记录："新建的 appsrc 要等一个 IDR 才出画，而网关对 PLI 的响应实测补不上（真机 rx=0 frames=0 kf=0，只有每 2 秒一条 PLI 石沉大海），画面永远回不来。改用 READY 重置。" 现有代码用 `pli_burst` 连发 3 次 PLI 缓解，但**恢复延迟与偶发黑屏仍在**（`play.vue:1465-1466` 注释："重复调用会闪黑屏"）。 |
| **270s 调暗场景** | ⚠️ **注意**：调暗时屏幕本身就看不见，拆不拆平面用户都无感；但若调暗**不触发** onHide（很可能，因为 `down` 只改亮度、电源仍是 0），方案 A 对 270s 这一步**什么也没做** —— 它解决的是"onHide 之后怎么办"，而 bug1 的 270s 卡点可能根本没有 onHide。 |
| **误判风险** | `onShow` 时若 `booted` 为假或 stage 已 `end`，重建逻辑需要额外守卫，否则可能出现"平面回来了但没有管线"的空窗。 |

### 9.2 方案 B（推荐，纯时长判据，零语义依赖）

既然无法可靠区分"熄屏"与"真退后台"，就**不区分**，改用**时间**做区分：

```
onHide:
  1) 立刻 hideVideoPlane()            // 无论什么原因，平面必须马上摘掉（保护系统 UI）
  2) 记 hiddenSince = now             // 不 clearTimers、不 keepAliveStop、不 stopPlay
  3) 起一个 grace 定时器（60s）
grace 到期（仍是 hidden 且未被 onShow 取消）:
  → 认定"真退后台" → clearTimers() + keepAliveStop() + bgTeardown()（stopPlay + stopGame 释放机位）
onShow:
  → 取消 grace 定时器；若平面被摘过 → showVideoPlane()（READY 重置）+ 恢复 20s keepAlive
```

**为什么这个方案最稳**：
- 完全不读 `getScreenState()`，**"3 是否存在"这个问题彻底变得无关**；
- 系统息屏时用户会在 300s/900s 后被唤醒或触摸 → onShow 大概率在 60s 内发生 → 会话与机位保住（覆盖 bug1 主诉）；真退后台通常不会在 60s 内回来 → 正常释放机位（不影响排队/公平性）；
- 平面摘除时机从"依赖状态判据"提前到"无条件立即"，**KMS 残留问题被彻底消除**；
- 唯一的代价是"真退后台时多占 60 秒机位"。若机位有限且排队压力大，把 grace 调到 30s。

**风险点**：`onShow` 之后要等 IDR，与方案 A 相同；建议把 60s 内反复 onHide/onShow 做去抖（比如 5s 内不重复重建平面）。

### 9.3 若将来想把"调暗"也救活（需要 native）

`getScreenState()` 对"仅调暗"是否敏感至今未证实（§8.4 第三种情形）。若实测证明它对此不敏感，则**唯一可靠路径**是新增 native 导出读 `bl_power`：

```c
/* cgnative.c 新增（示意，本次未实现、未编译） */
/* 返回 1=熄屏(bl_power>=1) / 0=亮屏 / -1=读不到；契约与 .tmp/rv2/snap8/core/clock.h 的 pb2_screen_off() 对齐 */
```
成本：一处 C 函数 + 一次交叉编译 + 一次部署；收益：判据从此是物理真值，彻底摆脱编号语义争议。**这是长期正解，但不是本次修复的必要条件**（§7.1 的 `n !== 0` 已能覆盖 1 和 4）。

---

## 10. 附：本次审查发现的其它值得跟进的问题

1. **`screen.js:26` 的注释与 `needsBrightRestore` 的兜底逻辑自相矛盾**：注释说"getBrightness 返回的是档位不是实际背光，所以主判据必须是 getScreenState"，但 `:107` 在 `getScreenState` 返回未知值时**恰恰退回了这个不可靠的亮度判据**。已知档位恒为 20/40/80 而阈值是 `<= 20`，只有恰好设成 20 档时兜底才会命中（`.tmp/bug1-soak.log` 里正好就是 `sys_br:20`）—— 也就是说这个兜底在大多数配置下**恒假**。建议显式标注它是"仅在极低档位配置下有效"。
2. **`docs/audit/player2-review2.md:462` 与 `:710` 的自认尚未复验**："keep retry 路径尚未真机复验（需 `bp=4` 深熄 + 新包），期望日志里能看到 `retry=1` 且 `state=1(OFF)` 被救回"。本次全仓库盘点确认：**至今没有任何一条 `retry=1` 的落盘日志**。§8 的 soak 能顺带补上这个空缺。
3. **`docs/design/playback-rewrite-adr.md:828`** 记录"`bp=4` 时 `/sys/kernel/debug/dri/0/summary` 不更新，读到的是过期值" —— 若将来做 native sysfs 判据，要注意不能在该状态下用 DRI summary 做校验。
4. **`.tmp/m_idle.sh:24` 的 grep 在 10-31 版 `hal-screen` 上恒为空**（§3.1），历史取数脚本若有依赖该列，结论需重新核对。

---

## 附录 A：本报告引用的原始证据索引

| 类别 | 文件 | 关键内容 |
|---|---|---|
| 原始日志 | `.tmp/bug1-soak.log` | 2026-11-12 22:46–22:57，21 个采样点全部 `screen_state:0 ... SCREEN_ON`；`fps` 于 t5 掉为 0 而屏幕全程亮 |
| 原始日志 | `.tmp/e2e/dev.log` | 11-07，`screen_state` 仅 0/4 两值；`4` 紧跟电源键事件 |
| 原始日志 | `.tmp/js1.log` / `js2.log` / `js_now.log` | 11-04 PenBili/PenDouyu，`[screen] keep ... state=0(ON)`，无 `[screen]` 外侧证据 |
| 原始日志 | `.tmp/js2/review2_append.md` | §8.3 记录了 `state=3(DIM)` / `state=1(OFF)` 的**人为注入**实验（同文件 `:55` 自认是 `hal-screen off` 污染所致） |
| 反汇编 | `.tmp/bh2/hal-screen` @0x1270 | `state` 子命令仅认 0/1，其它静默 |
| 反汇编 | `env/tmp/libjsapi_export.so` @0xc9bd4 / @0x9c6fc | JS 桥接层纯直通，无值映射 |
| 反汇编 | `env/tmp/libjsapi_export.so` @0xca7a0 / @0xca528 | 存在 `screenStatusChanged(bool)` 推送通道（未验证） |
| native 契约 | `.tmp/rv2/snap9/core/clock.c:12-23`、`clock.h:25-40` | `bl_power` 是唯一可靠判据；`/tmp/.screen_state` 方向反 |
| 源码 | `PenCG/src/services/screen.js:26,29-30,94,104,113,410,441-445` | 6 处判据点本体 |
| 源码 | `PenCG/src/pages/play/play.vue:486,512,1223,1248,1466,1473` | 守卫与平面控制 |
| 源码 | `PenCG/src/app.js:42,73` | app 级守卫与原始值日志 |
| 源码 | `PenCG/native/csrc/cgmedia/cm_cmd.c:167-204` | `setVideoVisible` 的 hide/READY-reset 实现与注释 |
| 文档 | `PenCG/docs/BUGFIX-bug1-lifecycle.md:150,347,352-353,509` | V2 待验断言与应急改法 |
| 文档 | `docs/readme-facts.md:242-244` | 旧结论汇总（C 级） |
