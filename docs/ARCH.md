# PenCG 架构与接口契约

PenCG = 有道 X6 Pro（Falcon 小程序运行时）上的**网易云游戏（cg.163.com）客户端**。
逻辑画布 **960x266**，`DESIGN_WIDTH=960`（`src/app.js`）。

## 分层

```
src/pages/*          8 个页面（Vue 单文件，falcon 方言）
src/services/*       业务与传输层（纯 JS / QuickJS）
libs/libjsapi_cgnative.so   native：raw HTTP + crypto（OpenSSL）
libs/libjsapi_cgmedia.so    native：WebRTC（ICE + DTLS-SRTP + RTP 解包）+ GStreamer 出画
```

平台限制（真机实测，勿假设）：
- JS 侧**只有** `$falcon.jsapi.http.request`（无 WebSocket、无 crypto、无 fetch）。
- `jsapi.http.request` 会对 `data` 字符串二次 JSON 序列化 → **发不了裸 body**；v2 接口的
  裸 Base64 请求体必须走 native。
- 响应头永远不可见；非 2xx 时 body 不可得。
- QuickJS `bigNum:false` → 无 BigInt。
- falcon 运行时：`flex:1` 简写无效；`<text>` 内容不能换行；`position:absolute` 不可靠、
  无 `z-index`；每个 `.vue` 必须 `<style lang="less" scoped>` + `@import "base.less"`。
- 视频走 **KMS overlay 平面**，盖在 UI 之上 → 播放态 UI 只能画在视频矩形之外。

## 页面

| 页面 | 职责 |
| --- | --- |
| `index` | 首页：左侧导航 + 推荐模块横滑 / 分类游戏网格 |
| `detail` | 游戏详情：封面、简介、截图、开始游戏、历史记录 |
| `search` | 搜索：热搜、联想词、结果列表 |
| `mine` | 我的：用户卡、免费时长、VIP、游玩历史、功能入口 |
| `login` | 登录：密码 / 短信验证码 |
| `play` | 游玩：全屏视频 + 悬浮球菜单 + 画质切换 + 虚拟键位 |
| `queue` | 排队：排名、预计等待、取消 |
| `diag` | 自检/调试：接口连通性、native 模块状态、媒体统计 |

## services 契约（页面只依赖这些导出）

### `services/const.js`
```js
export const API_BASE      // 'https://n.cg.163.com/'
export const MOBILE_UA     // Chrome UA（服务端按 UA 区分 mobile/pc 布局）
export const QUALITIES     // ['low','middle','high','bluray']
export const QUALITY_LABEL // {low:'流畅',middle:'标清',high:'高清',bluray:'蓝光'}
export const PLATFORM      // {MOBILE:1, PC:2}
```

### `services/log.js`
```js
export function logInfo(...a)
export function logWarn(...a)
export function logError(...a)
export function getLogTail(n)   // 环形日志，给 diag 页
```

### `services/storage.js`
```js
export const KEYS   // {session, settings, history, token, autotest}
export async function getJSON(key, fallback)
export async function setJSON(key, value)
export async function getString(key, fallback)
export async function setString(key, value)
export async function remove(key)
```

### `services/native.js`
```js
export async function loadNative()      // cgnative 模块（http+crypto），失败返回 null
export async function loadMedia()       // cgmedia 模块，失败返回 null
export function nativeReady()           // {http:boolean, media:boolean}
```

### `services/net.js`
```js
// 统一返回 { status:number, body:string, error:string|null, curlCode:number }
export async function httpText(url, opts)          // opts:{method,headers,body,timeout}
export async function httpJSON(url, opts)          // 解析 JSON，失败抛错
```

### `services/auth.js`
```js
export async function loginByPassword(phone, cc, pwd)  // → {token,userId,encrypt,nickname,freeTimeLeft,vip}
export async function sendSmsCode(phone, cc)           // → {ok, waitSeconds}
export async function loginBySms(phone, cc, code)      // → 同 loginByPassword
export function currentSession()                       // → {token,userId,encrypt,offset,loginAt} | null
export async function restoreSession()                 // 启动时从 storage 恢复
export async function logout()
export function isLoggedIn()
```

### `services/cg.js`
```js
export function getClient()                        // CGClient 单例
export async function v1(path, params)             // 匿名 GET → object
export async function v1Post(path, body)           // 匿名 POST JSON
export async function v2(path, body?)              // 登录态，自动走加密
export function applyLogin(payload)                // 用 login 响应刷新 offset/token
```

### `services/catalog.js`
```js
export async function loadHomeTabs()                            // → [{id,name}]
export async function loadHome(tabId)                           // → {banners, modules[], notices[]}
export async function loadRecommendations(tabId)                 // → modules[]
export async function loadGameList({page, perPage, gameType, tabId, sort})
export async function loadGameDetail(code)                       // → 详情对象
export async function searchGames(keyword, page)
export async function loadHotSearches()
export async function loadHints(keyword)
export async function loadSpeedUrl(gameType)                     // → {质->建议码率}
export async function loadNotices()
```

### `services/session.js`（机位申请 / 排队 / 游玩编排）
```js
export async function applyTicket({gameCode, region, quality, width, height})   // → {gatewayUrl, lockDetail, ...}
export async function reportNetworkTest({region, latency, bandwidth, quality})
export async function stopGame(gameCode)
export async function playingList()
export async function fetchTurnCredentials(region)
```

### `services/media.js`（native 媒体会话封装）
```js
export async function startMedia(cfg)   // cfg:{gatewayUrl, sdpOffer, token, userId, gameCode, quality, rect:{x,y,w,h}}
export function mediaEvents()           // 轮询取出事件数组
export async function sendInput(cmd)
export async function setQuality(q)
export async function stopMedia()
```

### `services/history.js`
```js
export async function recordPlay({code, name, icon, at})
export async function listPlays()
export async function clearPlays()
```

### `services/settings.js`
```js
export async function loadSettings()   // {quality, showBall, hideOnTouch, autoQuality}
export async function saveSettings(patch)
```

### `services/img.js`
```js
export function imgUrl(u)   // https→http 归一化（设备无 CA，必须降级）
```

## native 契约

### `libjsapi_cgnative.so`（模块名 `cgnative`）
```c
JQUICK_EXPORT void custom_init_jsapis(void);
```
JS 导出：
| 函数 | 说明 |
| --- | --- |
| `postSync(url, headerLines, body, timeoutMs)` | 同步；返回 `{status, body, error, curlCode}` JSON 字符串 |
| `startPost(url, headerLines, body, timeoutMs)` | 异步，返回 int id |
| `takeResult(id)` | 未完成返回 `null`，完成返回上面那个 JSON 字符串 |
| `cancelAll()` | 丢弃所有在途请求 |
| `sha256Hex(str)` | 十六进制 SHA-256 |
| `md5Hex(str)` | 十六进制 MD5 |
| `b64enc(str)` / `b64dec(str)` | Base64 |
| `tripleDesB64(plain, key, ivHex)` | 3DES-EDE3-CBC + PKCS7 → Base64 |
| `randomHex(n)` | n 字节随机 → 2n 个 hex 字符 |

`headerLines` 形如 `"Key: v\r\nKey2: v2"`。

### `libjsapi_cgmedia.so`（模块名 `cgmedia`）

> 实现细节、真机证据、6 个已修 bug 的完整纪实、`cm_*` 文件职责表与机位纪律，
> 见 [`docs/MEDIA.md`](MEDIA.md)。

```c
JQUICK_EXPORT void custom_init_jsapis(void);
```
JS 导出（**全部返回 JSON 字符串**，调用方一律 `JSON.parse` 后再用）：
| 函数 | 说明 |
| --- | --- |
| `start(cfgJson)` | 建 UDP socket → ICE → DTLS → SRTP → GStreamer（异步，立即返回）；成功 `{"id":n}`，失败 `{"error":"<原因>"}`（`cfgJson required` / `cfgJson not a string` / `nomem` / `bad cfgJson` / `thread create failed`） |
| `poll(id)` | 返回**自上次 poll 以来**的事件数组 JSON，无新事件返回 `[]`；未知 id 也返回 `[]`（不抛） |
| `cmd(id, cmdJson)` | 下发命令，成功 `{"ok":true}`，失败 `{"error":"cmd failed"}` / `{"error":"no such id"}` / `{"error":"cmd(id,cmdJson)"}` |
| `stop(id)` | 拆除会话、释放 KMS 平面；**幂等**。`{"ok":true}`，已停过返回 `{"ok":true,"already":true}` |
| `version()` | 构建信息，当前为 `"cgmedia/1.0.0 (ICE+DTLS-SRTP+H264/Opus->GStreamer)"` |

`cfgJson` 字段（**两种命名都吃**，见下）：
```json
{
  "remoteIp": "1.2.3.4", "remotePort": 57584,
  "remoteUfrag": "ogGZ", "remotePwd": "GRJvpfGvijTJvVomfUX37i",
  "ufrag": "ogGZ", "pwd": "GRJvpfGvijTJvVomfUX37i",
  "ourUfrag": "a86dff46", "ourPwd": "...",
  "fingerprint": "sha-256 AB:CD:...",
  "videoPt": 98, "audioPt": 111,
  "videoSsrc": 3398264230, "audioSsrc": 489236220,
  "rect": {"x":0, "y":0, "w":473, "h":266},
  "codec": "h264", "logLevel": 3, "audio": 1
}
```
- `remoteUfrag`/`remotePwd` 是**网关**的值（offer 的 `a=ice-ufrag`/`a=ice-pwd`）；
  `ufrag`/`pwd` 是同义别名（`media.js` 用这对名字）。`remoteUfrag` 优先，缺了才回落到 `ufrag`。
- `ourUfrag`/`ourPwd` 是**我方**的值（answer 里公布的），STUN USERNAME 拼成
  `"<remoteUfrag>:<ourUfrag>"`，STUN 响应的 MESSAGE-INTEGRITY 用 `ourPwd`。
- `remoteIp`/`remotePort` 必填（缺失或非法 → `{"error":"bad cfgJson"}`）。
- `rect` 是**逻辑画布坐标（960x266 系）**，native 内部再换算成面板物理坐标（见 `docs/MEDIA.md`）。
- 缺省值：`videoPt=98`、`audioPt=111`、`rect={0,0,960,266}`、`logLevel=3`、`codec="h264"`、`audio=1`。
- `videoSsrc`/`audioSsrc` 用字符串路径解析，可超过 2^31。

事件（`poll` 数组元素，按产生顺序）：
```json
{"t":"ice","state":"probing"}
{"t":"ice","state":"connected","localPort":46258,"mapped":"163.125.229.253:4227"}
{"t":"ice","state":"connected"}
{"t":"dtls","state":"connected","profile":"SRTP_AES128_CM_SHA1_80"}
{"t":"pipelines","video":true,"audio":true}
{"t":"media","video":true,"audio":true,"videoW":1280,"videoH":720,"fps":60}
{"t":"rect","x":0,"y":0,"w":473,"h":266}
{"t":"stat","rx":1892,"lost":0,"jitterMs":0,"bitrateKbps":2400,"decode":114,
 "frames":114,"authFail":0,"bad":0,"kf":1,"pli":3,"w":1280,"h":720,"fps":22,
 "rxPkts":1892,"rxStun":2,"rxKept":2,"rxDtls":0,"rxOther":0}
{"t":"error","msg":"ice failed","code":-1}
{"t":"stop","code":0}
```
| 事件 | 时机 / 说明 |
| --- | --- |
| `ice/probing` | `start()` 后立刻 |
| `ice/connected` | STUN 收到 0x0101。**会连发两次**：第一次来自 `cm_ice_run`（带 `localPort`/`mapped`），第二次来自状态机（无字段）。消费方按 `state` 去重 |
| `ice/failed` | 10 次 Binding 都没响应（随后跟一条 `error` code `-1`） |
| `dtls/connected` | DTLS 握手完成；`profile` 是 `SSL_get_selected_srtp_profile` 的结果 |
| `dtls/failed` | 握手超时（13s 预算），随后 `error` code `-2` |
| `pipelines` | 进入媒体循环、两条 GStreamer 管线建好（或失败）之后 |
| `media` | **只在首次真正推到一帧视频时发一次**（`media_ev_sent` 去重）；`videoW/videoH` 来自 SPS |
| `rect` | `cmd setRect` 生效后回显 |
| `stat` | 每 2 秒一次；`rx`=解密成功的 RTP 包数，`decode`=`nal_out`（送出的 NAL 数），`frames`=推给 appsrc 的帧数，`authFail`=SRTP HMAC 失败数，`bad`=PT/SSRC 不匹配，`kf`=IDR 次数，`pli`=发出的 PLI 次数，`rxPkts/rxStun/rxKept/rxDtls/rxOther` 是收包分类对账计数 |
| `error` | `code`：`-1` ICE 失败、`-2` DTLS 失败、`-3` SRTP 密钥初始化失败、`-4` 视频管线创建失败、`-5` 音频管线创建失败 |
| `stop` | 媒体循环退出时（`stop(id)` 或对端断流） |

`cmdJson` 支持的操作（`op` 字段）：
| op | 入参 | 行为 |
| --- | --- | --- |
| `stats` | — | 立刻吐一个 `stat` 事件。注意这个**手动版**字段略少：只含 `rx/lost/jitterMs/bitrateKbps/decode/frames/authFail/bad/kf/pli/w/h/fps`（`bitrateKbps` 恒 0），**不含** `rxPkts/rxStun/rxKept/rxDtls/rxOther`；周期版（每 2s）才带全量计数 |
| `setRect` | `{"rect":{"x":0,"y":0,"w":473,"h":266}}`，或扁平 `{"x":..,"y":..,"w":..,"h":..}`（嵌套优先） | 改 kmssink `render-rectangle`（内部做面板坐标换算 + `PAUSED→PLAYING` 脉冲）；`w`/`h` ≤ 0 时保留原值；成功后回 `{"t":"rect",...}`。注意 `render-rectangle` 是**单向棘轮**：缩到 1x1 后无法再放大，只能重建管线 |
| `requestKeyFrame`（别名 `pli`） | — | 立刻发一个 RTCP PLI（有 SRTP 上下文时走 SRTCP，26 字节） |
| `setLogLevel` | `{"level":0..4}` | 调 native 日志级别 |
| `setAudio`（别名 `setAudioEnabled`） | `{"on":0|1}` | 拆/建音频管线 |

> `cmd` 不支持的 op 会打一条 `CM_LOGW` 并返回 `{"error":"cmd failed"}`。
> `cmd` 里**没有** `input`/`quality`：键盘/触摸事件由 JS 层通过 WS 直接发给网关，
> native 不参与输入转发。
