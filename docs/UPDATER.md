# 检查更新（自动检查 / 手动检查 / 下载安装）

PenCG 的更新功能：启动时静默检查一次新版本，「我的」页也有手动入口；发现新版本后整页替换成更新弹窗，可从 GitCode 或 GitHub 直连下载 `PenCG.amr`。

实现移植自同作者的 PenBili（`PenBili/src/services/updater.js`，529 行），**文件头的真机结论全部原样继承**，只在网络层、存储层与安装通道上做了 PenCG 适配。

---

## 0. 一句话结论

- **能检查、能下载，暂时不能自动安装。** PenCG 的 native 模块没有导出 `installAmr`，所以下载完成后弹窗会停在「安装包已下载到设备」并显示落盘路径，需要用户手动装。native 侧补上这个接口后，只需改 `updater.js` 里 `installAmr()` 一个函数（函数体末尾已留好整段实现，注释掉的）。
- 版本号唯一来源是 `PenCG/src/services/version.js` 的 `APP_VERSION`，发布前要三处一致：该文件、`PenCG/package.json` 的 `version`、git tag。

---

## 1. 文件分工

| 文件 | 职责 |
| :-- | :-- |
| `PenCG/src/services/updater.js` | 「怎么查、怎么下、怎么装」。双源检测、版本比较、release 归一化、资产挑选、下载、安装，以及提示状态的持久化。纯逻辑函数全部无副作用导出（可被 Node 直接 import 做单测）。 |
| `PenCG/src/services/upd-view.js` | 「弹窗现在该显示什么」。视图状态机（模块单例 + 回调广播），手动/自动检测、换源、跳过、今天内不提示、下载安装的编排。 |
| `PenCG/src/components/update-dialog.vue` | 整页替换的更新弹窗（960x266 居中卡片），纯展示 + 按钮。 |
| `PenCG/src/components/mine-body.vue` | 「我的」页里「检查更新」卡片（`funcs` 里的 `update` 项）。 |
| `PenCG/src/pages/index/index.vue` | 挂弹窗（`updOn` 整页替换）+ `boot()` 末尾触发一次自动检测。 |
| `PenCG/src/pages/mine/mine.vue` | 老入口薄壳，同样挂弹窗（否则从这条路进来点「检查更新」会没反应）。 |

### 1.1 为什么把状态机单独拆成 `upd-view.js`

PenCG 的「我的」页是**组件**（`components/mine-body.vue`），被两个页面共用：`pages/index/index.vue`（内嵌栏目）和 `pages/mine/mine.vue`（`navTo('mine')` 的老入口薄壳）。而自动检测发生在 `index.vue` 的 `boot()` 里 —— 那一刻「我的」栏目可能还没渲染，`mine-body` 根本没挂载。

于是：**入口在组件里，弹窗在页面级**。两边要共享同一份状态，只能提到模块层。

为什么用模块单例 + 回调，而不是 props / $emit：本运行时全仓没有一处 `props` 或 `$emit`（PenCG 与 PenBili 都是），`nav.js` 的注释里也写了「没有可用的跨页 Vue 事件总线」。现有唯一被验证过的父子通道是 `$root.__mineBody`（`mine-body` 自己挂上去的），但那条通道只在组件已挂载时成立。模块级回调是同一页面上下文里的普通函数调用，没有渲染/生命周期依赖，最稳。

> 注意区分「同页共享」与「跨页共享」：`nav.js` 踩过的坑是**跨页面**的模块状态不共享（每个页面是独立的 QuickJS 上下文）。`index.vue` 与 `mine-body.vue` 在同一个页面 bundle 里，属于同一份模块实例，所以这里成立。

---

## 2. 双源策略，以及为什么

```js
// updater.js
export const GITCODE_REPO = 'dz56dz/PenCG';
export const API_GITCODE = 'https://api.gitcode.com/api/v5/repos/' + GITCODE_REPO + '/releases/latest';
export const REPO = '56dz/PenCG';
export const API_LATEST = 'https://api.github.com/repos/' + REPO + '/releases/latest';
```

`SOURCES` 数组的顺序即优先级：**GitCode 主，GitHub 备**。理由：

1. **GitCode 国内直连稳。** 词典笔走的是国内网络，GitHub API 在部分网络下完全打不开（真机实测过）。
2. **GitHub 是权威源。** 代码托管在 GitHub（`56dz/PenCG`），GitCode 是镜像仓库。GitCode 挂掉或还没同步时，GitHub 仍然拿得到。
3. **两者互补。** 一个平台的网络可达性出问题，另一个大概率还能用，所以弹窗里给了两个「立即更新」按钮分别直连两个源。

### 2.1 串行而不是并发

两个平台的限流都很紧（GitCode 429 / GitHub 403）。并发只会更快撞限流，而正常情况下第一个源 1~2 秒就返回，兜底源大多数时候根本不会被访问。所以 `fetchLatestRelease()` 是 `for` 循环依次试，第一个成功的立刻返回。

### 2.2 检测走直连，镜像只用于下载

**这条是 PenBili 的真机实测结论，必须原样保留：**

> `https://v4.gh-proxy.org/` 访问 `api.github.com` 会被共享出口 IP 限流，实测返回 `403 rate limit exceeded`。

镜像站是所有用户共用一个出口 IP 的，GitHub 对匿名 API 的限额是按 IP 算的，所以走镜像检测必然频繁 403。镜像只对**下载资产**有意义（资产是 CDN 直链，不经 API 限额）。

因此 `MIRROR_PREFIX = 'https://v4.gh-proxy.org/'` 只在 `downloadUrl(assetUrl, useMirror)` 里用到，检测路径（`fetchLatestRelease`）永远是直连。

### 2.3 只用 `/releases/latest`

`/releases/latest` 天然排除 draft 与 prerelease，不用自己翻列表。但 **GitCode 的字段语义不同**：它在 release 对象上多一个 `release_status`，值为 `'pre'` 或 `'latest'`，`/releases/latest` 仍可能返回 `'pre'` 的。所以 `normalizeRelease()` 里对三个字段都做了过滤：

```js
if (rel.draft || rel.prerelease || rel.release_status === 'pre') return null;
```

---

## 3. 五条实测约束（继承自 PenBili，勿改）

1. **检测不走镜像。** 见 §2.2。镜像访问 `api.github.com` 会被共享出口 IP 限流。
2. **只用 `/releases/latest`，且 GitCode 要额外过滤 `release_status === 'pre'`。** 见 §2.3。
3. **`net.js` 拿不到响应头，非 2xx 时 body 不可得。** 所以状态码只能从返回值里取，不能指望从 header 判断；错误文本形如 `curl ... resCode:403`，只能正则取码。PenCG 的 `httpText` 已经把 `status` 提出来了（见 §4.1），这一条在 PenCG 侧缓解了，但**仍然不能假设 body 一定可读**。
4. **纯逻辑函数全部导出为无副作用函数。** `parseVersion` / `compareVersion` / `isNewer` / `normalizeRelease` / `bodyLines` / `dayKey` / `normalizeUpdateState` / `shouldAutoPrompt` / `progressText` / `isMirrorable` / `downloadUrl` / `extractFilePath` 都能在 Node 里直接 import 调用，不依赖 `$falcon`。这样版本比较这类逻辑可以在桌面上跑断言，不用每次都上真机。
5. **版本号 = `APP_VERSION` 常量，且必须与 `package.json` 一致。** 发布 tag 也要用同一个号。三处不一致会出现「release 是 v1.2.3、软件里写 1.2.2」这种不自洽。

补充两条 PenCG 侧新增的约束：

6. **下载必须用 `$falcon.jsapi.http.downloadFile`，不能用 `http.request`。** `http.request` 只能拿到内存字符串，200KB+ 的二进制 amr 经过 QuickJS 字符串会损坏；`downloadFile` 是原生直写磁盘。
7. **`saveName` 参数不生效。** 真机回调里落盘名是 url 内容的 md5（天然去重缓存），形如 `/userdisk/miniapp/downloaded/<md5>`。所以不要依赖 `PenCG-update.amr` 这个名字去找文件，一律用回调返回的路径。

---

## 4. PenCG 与 PenBili 的差异（会影响本功能的部分）

### 4.1 网络层字段名不同

| | PenBili | PenCG |
| :-- | :-- | :-- |
| 函数 | `httpGet(url, opts)` | `httpText(url, opts)` / `httpJSON(url, opts)` |
| 状态码 | `statusCode` | **`status`** |
| 错误文本 | `errorMessage` | **`error`** |
| 额外 | - | `curlCode`（curl 的退出码，0 表示成功） |
| 成功判定 | `statusCode === 200` | `status === 200`，且 `status === 0` 表示传输层失败 |

`fetchLatestRelease()` 里的适配：先判 `!code`（传输层就没拿到响应）→ 记 `stage:'transport'`；再判 `code !== 200` → 记 `stage:'http'`。**这个顺序不能反**，否则网络断了会显示成「发布信息解析失败」，把网络问题说成数据问题。

### 4.2 存储层函数名大小写不同

PenBili 是 `getJson` / `setJson`，PenCG 是 **`getJSON` / `setJSON`**（大写 JSON）。写错不会报错，只会静默拿到 `undefined`，表现为「跳过了但下次还弹」。这个坑已在 `updater.js` 的 `loadUpdateState()` 注释里标注。

另外 PenCG 的 `storage.js` 没有导出 `logWarn`（PenBili 的 `storage.js` 导出了），日志统一从 `services/log.js` 拿。

### 4.3 没有 `installAmr`（关键差异）

| | PenBili | PenCG |
| :-- | :-- | :-- |
| native 模块 | `player2` / `cgmedia` | `cgnative` / `cgmedia` |
| 安装接口 | `player2.installAmr(path)` | **无** |
| 实现 | `native/csrc/jsapi_player2.c` 的 `js_install_amr`，内部调 `/bin/miniapp_cli install <path>` | - |

已 grep 全仓核实：

```
grep -rn "installAmr|miniapp_cli" PenCG/
```

- `installAmr` 命中 0 处。
- `miniapp_cli` 只出现在 `PenCG/tools/*.ps1` 与 `docs/` 里（部署脚本和文档），**没有任何 native 导出**。
- `PenCG/native/csrc/cgnative/cgnative.c` 的 `CG_EXPORTS` 宏（约 3004-3029 行）导出的是：`postSync` `requestSync` `startRequest` `startPost` `takeResult` `cancelAll` `sha256Hex` `md5Hex` `b64enc` `b64dec` `tripleDesB64` `randomHex` `hmacSha1B64` `utf8Hex` `version` `wsOpen` `wsState` `wsSend` `wsPoll` `wsPollEx` `wsClose` `wsList` `wsLastFrame` `imgScan` `imgConv`。没有安装类接口。

因此 `installAmr()` 目前恒返回：

```js
{ ok: false, code: 'NO_INSTALLER', message: '当前版本不支持自动安装，请手动下载' }
```

`upd-view.js` 的 `tryInstall()` 对这个 code **特殊处理**：不判成「安装失败」（那样会吓到用户），而是进 `stage:'downloaded'`，提示「安装包已下载到设备」并把落盘路径显示出来。

`updateCapability()` 返回 `{canDownload, canInstall:false}`，弹窗据此把主按钮文案从「立即更新」改成「下载安装包」—— 不能让用户点了一个写着「立即更新」的按钮，最后只下到一个包。

### 4.4 版本号来源不同

PenBili 在 `updater.js` 里自己定义 `APP_VERSION`；PenCG 已经有唯一来源 `services/version.js`，所以 `updater.js` 改成 `import { APP_VERSION } from './version.js'` 并 `export { APP_VERSION }` 转出，避免出现两份版本号。

### 4.5 主机侧单测

纯逻辑部分有单测：`bash PenCG/tools/updtut/run.sh`（76 条断言，只需要 node，
不需要设备）。做法是把真实的 `src/services/updater.js` 复制出来、只把
`net/log/storage/version` 换成 stub，所以**被测的是仓库里那份代码**，改完
`updater.js` 重跑即可，不需要同步任何模型。

覆盖范围与变异测试记录见 `PenCG/tools/updtut/README.md`。其中两条断言是被
变异测试逼出来的，值得记住：

- **`PenCG.amr` 在资产列表里排第二位时也必须被选中**。真实发布时
  GitCode/GitHub 会把源码包（`v1.2.3.zip` / `.tar.gz` / `.tar.bz2` / `.tar`）
  排在 `.amr` 前面，一旦资产挑选退化成 `assets[0]`，用户下到的就是源码压缩包。
  v1.2.3 首发时就踩过这个坑，重传过一次资产。
- **`bodyLines` 的连续空行压缩**。不压的话，发布说明里的大段空行会在 266px
  高的 scroller 上白吃好几行。

运行时行为（`fetchLatestRelease` / `downloadFileToDisk` / `installAmr`）需要
真机验证，不属于这个单测的范围。

### 4.6 真机验证记录（X6 Pro，2026-11-15）

| 环节 | 结论 | 证据 |
| --- | --- | --- |
| 自动检查 | 通过 | 1.2.3 干净包启动后日志 `[cg] upd auto suppressed (skip=- quiet=- remote=v1.2.3)` —— GitCode API 请求、`normalizeRelease`、版本比较全部跑通，并正确判定「已是最新」而不弹窗 |
| 弹窗渲染 | 通过 | 把 `version.js` 临时改成 1.2.2 后装机，自动弹出对话框：标题「发现新版本 v1.2.3」、徽标 `v1.2.3`、`325 KB`、副标题「新版本 v1.2.3 · 当前 v1.2.2」，正文多行未被裁切 |
| 双源按钮文案 | 通过 | 两个下载按钮的括号里是**当前命中的源名**（GitCode / GitHub），不是写死的 |
| 整页替换 | 通过 | 弹窗出现时首页整体不渲染，没有叠层（本运行时禁用 position:absolute/z-index，靠 `v-if` 换页） |
| 下载 + 安装 | **未验证** | 没点过下载按钮；`installAmr` 在本仓库恒返回 `NO_INSTALLER`，按钮文案已降级为「下载安装包」 |

自动检查失败或断网时的表现（回落 GitHub、再回落镜像）也只有单测覆盖，
真机上没制造过断网场景。

### 6.1 版本号

三处必须一致：

1. `PenCG/src/services/version.js` 的 `export const APP_VERSION = '1.2.3';`
2. `PenCG/package.json` 的 `"version": "1.2.3"`
3. 两个平台的 release tag（`v1.2.3` 或 `1.2.3`，比较时 `v` 前缀会被 `parseVersion` 去掉）

> 不要改 `PenCG/src/services/net.js` 里的 `X-Ver: 1.0.0` —— 那是冒充官方 web 客户端的请求头，不是我们的版本号。

### 6.2 构建

```bash
cd PenCG
node ../env/node_modules/aiot-vue-cli/src/cli.js -q -p
```

产物在 PenCG 根目录，名字由 `package.json` 的 `appid` 和 `version` 拼出来：`8002026110200003.1_2_3.amr`（版本号里的点变成下划线）。

### 6.3 发布到两个平台

**两个平台必须发同一个 tag、同一个资产文件**，否则弹窗里的换源按钮会报「该平台上还没有这个版本」（这是刻意设计：宁可不降级，也不能把旧包当新包下）。

| 平台 | 仓库 | API |
| :-- | :-- | :-- |
| GitCode（主） | `dz56dz/PenCG` | `https://api.gitcode.com/api/v5/repos/dz56dz/PenCG/releases/latest` |
| GitHub（备） | `56dz/PenCG` | `https://api.github.com/repos/56dz/PenCG/releases/latest` |

两边的 release 都要：

- **tag** 与 `APP_VERSION` 一致；
- **不是 draft、不是 prerelease**（GitCode 还要 `release_status` 不是 `'pre'`）；
- **资产文件名必须是 `PenCG.amr`** —— `normalizeRelease()` 优先挑 `name === ASSET_NAME` 的资产，没有才退回 `assets[0]`。用别的名字也能工作，但一旦将来一个 release 挂了多个资产（比如同时放 debug 包），挑错的风险就出现了。
- **更新说明（body）** 会原样显示在弹窗左栏，`bodyLines()` 会去掉 Markdown 标记（标题 `#`、引用 `>`、列表 `-`/`*`/`+` 换成 `·`、`**粗体**`、`` `代码` ``、`[文字](链接)` 只留文字）。**不要用 emoji** —— 设备没有 emoji 字体，会渲染成方块（`docs/AI-CODE-RULES.md` §1.2）。

### 6.4 发布顺序

先发 GitHub，再发 GitCode（或者反过来都行，但**两边都要发完再算发布完成**）。用户如果恰好在中间这个窗口里检查更新，会遇到「主源没有 / 备源有」的情况 —— 这是可以接受的，弹窗会给换源按钮。

---

## 7. 用户可见的交互

### 7.1 自动检测（进软件时）

`pages/index/index.vue` 的 `boot()` 末尾调一次 `autoCheckUpdate()`，**不阻塞首屏**（首屏已经渲染完，网络差的机器最多只是弹窗晚一点）。

三道闸，任意一道不过就不打扰用户：

1. 取不到版本（网络失败 / 两个平台都没有 release）；
2. 线上 tag 不比本地 `APP_VERSION` 新；
3. 用户点过「跳过此版本」（且线上没有更高的版本）或「今天内不提示」（且还是同一天，按**设备本地时区**算）。

整体 `try/catch`，静默失败，日志只到 W 级。

### 7.2 手动检测（「我的」页）

「设置与工具」里新增一张「检查更新」卡片（desc 显示当前版本）。点击后**永远弹窗**，包括「已是最新」—— 用户主动点了就得有反馈。

### 7.3 弹窗

整页替换（960x266 居中卡片），**不是浮层**。原因：本运行时 `position:absolute` 不可靠，全仓没有一个可用的 `z-index`（`base.less` 文件头铁律），浮层会退化成「插在文档流里的一块」，把下面的内容顶开。所以靠页面级的 `v-if` 把正常视图整体不渲染。

- **自动检测发现新版本**：4 个按钮 = 立即更新（直连当前命中的源）/ 立即更新（直连另一个源）/ 跳过此版本 / 今天内不提示。
- **手动检测发现新版本**：2 个按钮 = 立即更新（或「下载安装包」）/ 暂不更新。下载失败会自动换另一个源重试一次。
- **其它阶段**（检查中 / 已最新 / 下载中 / 安装中 / 已下载 / 失败）：1~2 个按钮 = 关闭 / 查看发布页。

「查看发布页」**不能真的跳转** —— 词典笔上没有能唤起浏览器的 JSAPI（全仓 grep 确认），只能把 URL 显示在右下角提示里让用户自己抄。

---

## 8. 排查

日志前缀统一是 `[cg] upd ...`，都在 `/userdata/applog/loli_s6_dev.txt`（`services/log.js` 只把 error 级落盘，`logWarn` 走的就是 `console.error`）。

常见日志与含义：

| 日志 | 含义 |
| :-- | :-- |
| `upd source gitcode transport fail: ...` | GitCode 连不上，正在试 GitHub |
| `upd source github http 403` | GitHub API 限流（匿名限额按 IP 算，频繁检查会撞） |
| `upd source ... parse fail` | 返回 200 但字段对不上（通常是 draft/prerelease/没有资产） |
| `upd auto suppressed (skip=... quiet=...)` | 用户设过跳过/今天不提示，自动检测被拦 |
| `upd downloadFile no path, raw=...` | `downloadFile` 回调没给路径，`raw` 是原始回调对象 |
| `upd install unsupported, amr at ...` | 下载成功但 native 没有安装接口（当前必然出现） |

真机上想手动验证下载链路，可以直接在 `diag` 页（只有 `DEV_AUTOSTART` 能进）或者临时加一个按钮调 `downloadFileToDisk(下载直链)`，看回调里的 `result` 字段是什么形态。
