# 网页版 API 抓包逆向 · 交接提示词

> 给接手这个任务的 agent 的完整上下文。**照着做就能干活，不需要再问原作者。**
>
> 目标仓库：**https://github.com/56dz/163CG-API** —— 网易云游戏 `cg.163.com` 网页（H5 / 触摸版）
> 接口逆向与可运行 SDK。当前状态：**272 条接口 · 135 条实测用例（111 通过）· 23 份脱敏样本 · 6 张真机截图**。
>
> **本次任务：把它更新到当前线上版本，并补齐已知空白。**

---

## 0. 一句话背景

`163CG-API` 已经把「首页推荐 → 选游戏 → 申请机位 → 连网关 → WebRTC 收流 → 触摸操控 → 悬浮球调画质」
整条链路拆开写清楚了，文档质量很高（`docs/API文档.md` 1812 行、`README.md` 523 行、
`sdk/cgapi.py` 1296 行 / 166 个方法）。

**但它是 2026-11-06 的快照**，而网易云游戏的前端发版很勤（add/remove 字段是常态）。
你这次要做三件事：**① 重新抓一遍确认哪些变了；② 补已知空白；③ 顺手把 PenCG 项目卡住的那个问题解掉。**

---

## 1. 仓库现状（动手前先读，别重复劳动）

### 1.1 目录结构

```
163CG-API/
├── README.md                  523 行，按「需求」组织的导览（推荐先读这个）
├── report.html                111697 B，单页可视化报告（272 行可搜索接口表 + 截图）
│                              ★ 是手写/一次性生成的静态文件，仓库里没有生成脚本
├── docs/
│   ├── API文档.md            1812 行 / 96640 B，主文档，14 章
│   ├── 接口清单.csv           272 行，列 = 分类,Method,Path,函数,参数,源文件,偏移
│   └── img/                  6 张真机截图（已打码）
├── samples/                   23 份脱敏响应样本（.json）
└── sdk/
    ├── cgapi.py              1296 行 / 166 个方法，只用标准库
    └── _des3.py              7.1 KB，纯标准库 3DES（密码登录用）
```

### 1.2 `docs/接口清单.csv` 的真实构成（已统计，不用再数）

| 分类 | 条数 | | 分类 | 条数 |
|---|---|---|---|---|
| 我的/账户 | 96 | | 游戏/详情 | 22 |
| 社区/直播 | 52 | | 登录/鉴权 | 17 |
| 其他 | 35 | | 启动/排队 | 13 |
| 首页/推荐 | 22 | | 云电脑/云手机 | 9 |
| 悬浮球/键位/画质 | 4 | | SEO | 2 |
| **合计** | | | | **272** |

Method 分布：`GET 173 / POST 78 / DELETE 13 / PATCH 8`。
列格式：`"分类","Method","Path","函数","参数","源文件","偏移"`（`源文件`/`偏移` 指它在哪个
bundle 的字节偏移，可以直接 grep 定位 —— **这是最值钱的一列，逆向新接口时照着找**）。

### 1.3 关键协议事实（**已验证，直接用，不要推翻**）

**网关**：所有 XHR 走 `https://n.cg.163.com/`（备用 `nr.cg.163.com`）。
- `api/v1/*` —— 明文，**匿名可读**
- `api/v2/*` —— 必须 `Authorization: Bearer <token>`，请求体和响应体都加密

**移动端标识头**：`X-Platform: 1`（web-mobile；iOS H5 是 `4`，端游 web 是 `0`），
另有 `X-Channel` / `X-Source-Type` / `X-Ver`。

**传输加密**（不是标准 AES，是「偏移量异或 + Base64」）：

```js
randomSplit = e => e.split(",").map(x => x.split(":")[1]).join("d")
cookieSet   = e => e.split("").map(c => c.charCodeAt() + "-" + rand2()).join("-")
cookieGet   = e => String.fromCharCode(...e.split("-").filter((_, i) => i % 2 === 0))
getOffset2  = e => getOffset(...e.split("d").map(Number).slice(1))
getOffset   = (a, b, c) => (a | b) % 256 + c
offsetEn  = (off, txt) => base64(utf8(txt).map(b => (b + off) % 256))
offsetOut = (off, b64) => utf8(base64decode(b64).map(b => (b - off + 256) % 256))
```

`OFFSET` 由登录响应的 `encrypt` 字段推导 → `cookieSet(randomSplit(encrypt))` 写进
localStorage 的 `NCG-eid` → 之后每次 `cookieGet` → `getOffset2` → **`% 256`** 还原
（**`getOffset2` 可能 > 255，必须 `% 256`**，这是坑）。

**网关 WS 的 `auth` 包必须发明文**，其余（`answer` / `input` / `game_set_quality`）走 `offsetEn`。
判据在源码里：

```js
socketSend(e, t, i, n) {
  const c = {id: …, op: e, data: t || {}};
  this.$ws.send(n || s ? c : offsetEn(this.OFFSET, JSON.stringify(c)));
  //               ↑ plain=true                  ↑ 正常路径
}
```

`onOpen` 里是 `socketSend("auth", i, cb, !0)` —— 第 4 参 `true`。
发错了会得到 `{"errcode":400,"errmsg":"format invalid"}`。

**v2 响应体是裸 Base64**（`Content-Type: application/octet-stream; method=1`），
不是 JSON 字符串；`b64decode` 必须补 padding，否则异常被上层吞掉后静默降级成 `{"raw": …}`。

**`run.html` 的 localStorage 有 `{"data": <值>}` 信封**：读写都要 `.data`。
浏览器自动化时必须 `localStorage.setItem(k, JSON.stringify({data: v}))`，
且要先 `goto` 到 `cg.163.com` 源下再写。

**SDP 后处理 4 步**（移动端小带宽优化）：给含 `level-asymmetry-allowed` 的 fmtp 追加
`x-google-start-bitrate=1000;x-google-max-bitrate=2800;x-google-min-bitrate=1000`；
每条 `a=rtcp-fb:N nack pli` 后插同号 `a=rtcp-fb:N rrtr`；opus fmtp 追加 `stereo=1`；
无 candidate 的 m 段补 host candidate；最后正则剔除 `a=rtpmap:100 H265/90000`。

**输入指令编码**：坐标统一 `Math.ceil(65535 * x / videoWidth)`。
`1/2/3 <x> <y> <id>` 触摸按下/移动/抬起、`8` tap、`4` 取消、`5 <text>` / `6 <keycode>`、
`0 <ts>` 心跳、`25 tochscreen|keymouse`、`26`/`28` 摇一摇、`29 <text>` 粘贴到云端、
`100/101/102/103/104/105` PC 鼠标键盘、`110~114` 手柄。

---

## 2. 你这次要做的三件事

### 2.1 任务 A：重新抓一遍，确认线上有没有变（必做）

**目标**：把 272 条接口里**线上已经变了的**找出来，更新文档 + CSV + 样本。

**方法**（原作者的抓包手法，照抄即可）：

1. **拿前端 bundle**。`cg.163.com` 是 SPA，接口全在 `chunk-common.*.js`、`run.*.js` 等
   bundle 里。打开 `https://cg.163.com/` → F12 → Network → 过滤 `.js` → 全部下载下来存到
   `.tmp/bundles/`。**bundle 文件名带 hash，每次发版都会变**（当前文档引用的是
   `chunk-common.144278fd.js`、`run.cbc92ca7.js`）。
2. **抽接口**。对每个 bundle `grep -o 'api/v[12]/[a-zA-Z0-9_/{}.@:-]*'`，
   再去重、和现有 `docs/接口清单.csv` 做 diff。
   > ★ CSV 里的 `源文件` + `偏移` 两列就是靠这个定位的，新接口也照这个格式补。
3. **实测**。`sdk/cgapi.py` 里的 166 个方法可以直接调。匿名接口不需要 token；
   v2 接口需要登录态（见 §2.2 的登录说明）。
4. **存样本**。新接口的响应脱敏后放 `samples/<name>.json`（现有 23 份的命名风格就是
   `api路径换下划线.json`，例如 `main_recommendations_mobile.json`）。

**重点复核这几处最可能变的地方**（都是在「已知限制」里、且作者自己说没深挖的）：

- 首页 17 个接口里有没有新增/删除（`versions/@current`、`client-settings/@current`、
  `group-customize-settings` ×4、`main_recommendations`、`speed_url` 等）；
- `home_page_tabs` 的 6 个 `tab_id`（这些是硬编码的 ObjectId，运营会换）；
- 11 个 region code（`ghbtj ghdjn ghdnb ghnfs ghzcs gxbxa gxncq zmdhdcz zmdhdnb zmdhnfs zmdxbxa`）；
- 画质 4 档之外的取值、`speed_url` 的 `default_quality`。

### 2.2 任务 B：补已知空白（按价值排序）

作者在 README 末尾「已知限制」里自己列了 6 条（`README.md:508-521`）。逐条处理：

**① DataChannel 没实装 —— 最高优先（这条能顺带解掉 PenCG 的一个 bug）**

现状（`README.md:511-513` 原文）：

> **DataChannel 没实装**。`common_setting.enable_data_channel` 默认 `false`，输入全走 WS；
> 服务端会周期性推 `9 <port>,1,<hex>,<base64>` 催你建 DataChannel，但这条路径没验证过。

**这条很重要，因为 PenCG（同一个作者的有道词典笔客户端）正卡在这里**：

- PenCG 网关也在**每秒推一条** `{"op":"input","data":{"cmd":"9 <port>,1,<hex>,<base64>"}}`
  （载荷每字节 **+0x20** 解密后是 `{"m":"dataChannelReady","pkg":"com.hypergryph.endfield ..."}`）；
- PenCG 从不响应，只是丢弃；
- 而 PenCG 有个未解 bug：**游戏约 4 分 18 秒后网关静默停止推流**（`rx` 冻结，WS 仍 open）。
  （**2026-11-14 更正**：实测判据应为「**每一个 WS 会话约 250 秒**，每次 `wsOpen` 重新计时」，247/253/248s；「4m18s」是「250s + 40~60s 起播冷启动」的口径。见 `docs/BUGFIX-bug1-active-renew.md`。）
  官方客户端的挂机保活（`RtcKeepPlaying` CmdId=20015 / `RtcKeepaliveCfg` CmdId=20021）
  走的是 protobuf 二进制信令，PenCG 拿不到 `gameToken`，所以这条路是死的。

**所以要你验证的是**：DataChannel 建起来之后，
1. 服务端推的 `9` 消息是不是就停了 / 变成真正的数据？
2. **不建 DataChannel 会不会导致服务端判超时停推？**（就 PenCG 那个 4m18s；2026-11-14 更正：实测是「每个 WS 会话约 250 秒」）
3. `common_setting.data_channel_params` 里这几个字段的实际行为：

```json
"data_channel_params": {"connect_timeout_ms": 5000, "disable_keepalive": 1,
  "keepalive_interval_ms": 1000, "keepalive_interval_ms_after_no_resp": 500,
  "switch_ws_after_x_failure": 12}
```

（`docs/API文档.md:718-724` 有完整实测响应）

> ★ 提示：网页端 `enable_data_channel` 默认 `false`、`enable_data_channel_v2` 是 `true`，
> 而**网页版自己是能正常长玩的**。所以「不建 DataChannel 必被封」未必成立 ——
> 先把网页版正常玩的场景抓清楚，再对比 PenCG 少了什么。这是关键对照。

**② 登录链路只验证到「取到 token 前」**

作者为了不给测试号发短信、不碰别人密码，没真正完成一次登录。
**这次可以用真实账号完整跑一遍**（凭据在 §5 的「凭据」一节，**只在本机用，绝不写进仓库**）：

- 短信：`POST api/v1/phone-captchas/<ctcode>-<phone>` → `POST api/v1/tokens`
  （★ **区号在前**：`86-138xxxx` → 200，反过来 → 400 `errcode=1103`）
- 密码：`GET api/v1/user-pwd-info?phone=…&ctcode=86` → 三次加密 →
  `POST api/v1/user-pwd-tokens`
  （`login_random_text` 和 salt **每次请求都变**，两步必须连着发）
- 扫码：`POST api/v2/login-qrcodes` 匿名拿不到（401 `errcode=1006`，只有 `X-Platform: 6`
  返回 `1025`）—— 这条**不用再试**，作者已确认。

验证点：登出后用同一账号重新登录，`NCG-token` 能正常刷新、`NCG-eid` 的 offset 能正确重算。

**③ 端游 web（`X-Platform: 0`）没深挖**

路径相同但部分参数不同。最少要做到：`POST api/v2/tickets` 在 `X-Platform: 0` 下
能正常拿机位，`game_type=pc` 的 `enable_resolution_config=true` 游戏能调出
「画质/帧率/分辨率」三组选项（`docs/img/quality-selector.png` 就是那个面板）。

**④ `api/v2/groups` 系列、礼物/活动类**只做到「能列出来」，业务语义没验证 —— 按需补。

**⑤ iOS 原生 App / 小程序端**没碰（`NCGJsBridge` 分支只是读代码）—— 优先级低，可跳过。

**⑥「接口会变」** —— 这就是任务 A 要解决的。

### 2.3 任务 C：把「断线/超时」这一章做实

`docs/API文档.md:598-608` 的「8.6 断线与错误码」现在只有一行值得注意：

```
| 超时 | 「由于您太久没回来游戏已关闭」 |
```

**这正是 PenCG 那个 4m18s 停推的用户可见文案**。要你查清楚：

> **2026-11-14 更正**：「4m18s」已更正为「每个 WS 会话约 250 秒」（每次 `wsOpen` 重新计时，三局实测 247/253/248 s）。下面四个问题的方向不变，但「超时时长」要按 250 s 去对。
详见 `docs/BUGFIX-bug1-active-renew.md`。

1. 这句话在前端 bundle 里**什么条件下**弹？（grep 这句话的字面量，看它的调用点）
2. 服务端是通过**哪个 op** 通知的？（`op=error` 带什么 errcode？还是 `op=game_stop`？还是静默停流？）
3. 触发它的**计时器**是哪个？时长是多少？能不能从 `common_setting` 或
   `api/v2/users/@me/games/<code>/common_setting` 里读到配置？
4. 网页端**靠什么避免被判超时**？（鼠标移动？DataChannel 保活？`keepalive_interval_ms`？）

如果查到「网页端靠某个 ws op / http 心跳续命」，**那就是 PenCG 缺的东西**，
这条结论比补 10 个接口都值钱。

---

## 3. 工作环境

### 3.1 仓库位置

```bash
# 已经 clone 到（可以直接用）
D:/Users/Daniel/code/DSH/x6pro/haas-x6pro/.tmp/163CG-API
```

> ⚠️ `.tmp/` 在 PenCG 仓库里是 **gitignore 的**，如果你在那边干活要注意别被清理掉。
> 若需要长期工作，建议 clone 到 `D:/Users/Daniel/code/DSH/x6pro/163CG-API`（与 PenCG 平级）。

### 3.2 环境

- **Python**：系统 Python 3（不需要虚拟环境，`sdk/cgapi.py` 只用标准库 +
  可选 `pycryptodome` / `cryptography`；两者都没有时自动退回 `sdk/_des3.py`）
- **网络**：需要能访问 `cg.163.com` / `n.cg.163.com`。若需要代理，
  `make_api(proxy='http://127.0.0.1:10808')` 已支持。
- **浏览器**：做网页端抓包 / 自动化时需要。本机有 Chrome（接了 CDP，可用
  `browser-skill` 相关工具；也可直接用 F12 手动抓）。

### 3.3 怎么跑

```python
import sys; sys.path.insert(0, '163CG-API/sdk')
from cgapi import make_api, PlaySession, cookie_get, get_offset2

api = make_api()                                   # 匿名可用
tabs = api.home_tabs()
feed = api.home(tab_id='66051b810d5fa1f0204c294f')
detail = api.game('mrfzzmd')
rates = api.speed_url('mobile')

# 登录后
api.token  = '<NCG-token>'
api.offset = get_offset2(cookie_get('<NCG-eid>')) % 256
api.uid    = '<JWT 里的 identity>'
me = api.me()
tk = api.post_ticket('mrfzzmd', ['ghnfs'])

ps = PlaySession(api, tk['gateway_url']).connect()
ps.auth('mrfzzmd', quality='high', codecs=['h264'])
ps.tap(0.5, 0.5)
```

---

## 4. 已知的坑（照着避，别重踩）

| # | 坑 | 现象 | 正解 |
|---|---|---|---|
| 1 | WS `auth` 包加密发 | `{"errcode":400,"errmsg":"format invalid"}` | `auth` 必须**明文**，其余才 `offsetEn` |
| 2 | v2 响应体当 JSON 解析 | 拿到 `{"raw": …}` | 响应体是**裸 Base64**，`b64decode` 要补 padding |
| 3 | `getOffset2` 不 `% 256` | offset 值 > 255，解密乱码 | **必须 `% 256`** |
| 4 | `games` 列表不带分页 | 一次吐 1826 条 / 6 MB | 必带 `?page=0&per_page=10`（→ 59 KB） |
| 5 | `tickets` 的 `regions` 传空数组 | 404 / 1288 | 必填具体 region code，如 `['ghnfs']` |
| 6 | 短信验证码路径参数顺序 | 400 `errcode=1103 无效区号` | **区号在前**：`86-138xxxx` |
| 7 | `pwd_info` 和 `pwd_tokens` 不同一次会话 | 加密失败 | `login_random_text`/salt 每次变，**必须连着发** |
| 8 | 蓝光画质没权限时 | 以为接口不支持 | 前端在发帧前就拦截（`user.bluray === false`），**一个字节都不会发出去** |
| 9 | `setFps` / `setResolution` 当热切换 | 没反应 | 它们会 `resetGateWaty()` **重连整个 WS 重新 auth**，不是 `game_set_quality` |
| 10 | `run.html` 的 localStorage 写裸值 | 被读成 `null` → 弹回首页 | 必须 `JSON.stringify({data: v})`，且先 `goto` 到 `cg.163.com` 源下 |
| 11 | `api/v2/pc_vip/games` 不带分页 | 500（后端超时） | `?page=1&page_size=10` → 200 |
| 12 | `welfare_center/<x>` | 404 | 该路径不存在，真实入口是 `welfare_center_coupons` |

还有 11 条类似的在 `README.md:487-503` 的表格里。

---

## 5. 凭据（**只在本机用，绝不入库、绝不进 commit**）

真实账号（网易云游戏，VIP）：

- 手机号：`<PHONE>`（找作者要，**不要写进任何文件**）
- 密码：`<PASSWORD>`（同上）
- 区号：`86`
- 用户 ID：`67d9433bc649c953187c4ee8`

**提交前必须扫描**：

```bash
# 把下面的 <PHONE>/<PASSWORD> 换成真实值再跑（真实值除了这条命令，别写进任何文件）
grep -rn '<PHONE>\|<PASSWORD>' --include='*.py' --include='*.md' --include='*.json' \
     --include='*.html' --include='*.csv' --include='*.js' .
# 必须 0 命中
```

样本脱敏规则（现有 23 份的做法）：手机号 → `138****0000`、token → `eyJ…`、
container → `topa10_261106_000000,t-25`、host → `ahnfs10000`、nickname → `用户****`。

---

## 6. 交付标准

完成后**必须**能回答：

1. **272 条接口里，有多少条线上已变？**（列出 change/add/remove 三类，各给条数）
2. **`docs/接口清单.csv` 是否已更新到最新？**（新旧 diff 可复现）
3. **DataChannel 那条路验证到哪一步？** 给出：
   - `9` 消息在 DataChannel 建起来之后还会不会推；
   - **网页端不建 DataChannel 能不能长玩**（这是关键对照）；
   - `data_channel_params` 五个字段的实际行为。
4. **「由于您太久没回来游戏已关闭」的触发条件是什么？** 时间多长、靠什么续命、服务端用哪个 op 通知。
5. **登录链路是否已端到端跑通？**（短信 / 密码两条，给出实测结果）
6. **README / API文档 / CSV / samples / report.html 是否同步更新？**
   （report.html 是静态的，改了接口表要一起重生成或手工同步）

**产出物清单**：

- `README.md` —— 改了哪些接口、新增什么章节，在顶部加一段 update 说明
- `docs/API文档.md` —— 对应章节更新
- `docs/接口清单.csv` —— 全量 272 行（或最新行数）
- `samples/` —— 新增/更新的脱敏样本
- `sdk/cgapi.py` —— 新接口的方法（保持「只用标准库」的约束）
- `report.html` —— 接口表同步

---

## 7. 纪律

- **不要**给测试号发短信轰炸。短信验证码那条链路作者故意没跑完，你若要做，
  自己控制频率（同号码 60 秒内重复 → `errcode=1100 申请太频繁`）。
- **不要**碰别人的账号 / 密码。只用 §5 给的这一个。
- **不要**把凭据、真实 token、真实手机号写进任何入库文件。
- **不要**为了「补全接口」去调用付费/充值类接口（`orders` / `recharges` / `coins` 只读即可）。
- 这是**学习与安全研究**项目（README 有免责声明）。请勿用于商业用途或对服务方造成负担。
- 接口会变，**别把快照当永久真理**。每次写结论都要带抓取日期。

---

## 8. 与 PenCG 的关系（背景，别混进这个仓库）

同一个作者还有个 **PenCG**（https://github.com/56dz/PenCG）—— 网易云游戏的**有道词典笔
X6 Pro 客户端**，自研了整条 WebRTC 协议栈（ICE + DTLS-SRTP + RTP 全手写）。

两个项目的关系：

| | 163CG-API（本仓库） | PenCG |
|---|---|---|
| 目标 | 把网页版接口**写清楚** | 在词典笔上**跑起来** |
| 传输 | 浏览器原生 WebRTC | 自研 ICE/DTLS-SRTP/RTP 栈 |
| WS op | 明文 JSON + `offsetEn` | 同样 JSON + offset，已对齐 |
| DataChannel | **没实装**（网页端默认也不用） | **没实装**（网关每秒催） |
| 4m18s 停推 | 网页端**正常长玩**，为什么？ ← **这就是要你查的** | 待解（**2026-11-14：PenCG 侧判据已更正为「每会话 ≈250s」，并已用 `ACTIVE_RENEW` 主动换会话规避；「网页端为什么能长玩」仍未答**） |

> ★ **最有价值的交付不是多列 10 个接口，而是回答「网页端凭什么能长玩」**。
> 如果你查清了网页端的续命机制，PenCG 那个卡了很久的掉线 bug 就能直接修掉。
> 反之，如果查清后确认「网页端也没做特殊处理、是网关对浏览器有天然豁免」，
> 那也是一条极有价值的结论（说明 PenCG 需要从 UA / TLS 指纹层面伪装）。

---

## 9. 附：本次交接时已经确认的事实（不用再查）

以下是我（写这份文档的人）在 2026-11-13 已经核实过的，你可以直接引用：

1. 仓库 clone 正常，`997eab3 feat: 补分辨率/屏幕适配四层结构…` 是最新 commit。
2. 文件规模：`README.md` 523 行 / `docs/API文档.md` 1812 行 / `sdk/cgapi.py` 1296 行
   （166 个 `def`）/ `docs/接口清单.csv` 272 行 / `samples/` 23 个 json。
3. CSV 分类统计：我的/账户 96、社区/直播 52、其他 35、首页/推荐 22、游戏/详情 22、
   登录/鉴权 17、启动/排队 13、云电脑/云手机 9、悬浮球/键位/画质 4、SEO 2。
4. `common_setting` 实测响应里有 `data_channel_params`
   （`docs/API文档.md:718-724`），五个字段值见 §2.2①。
5. `docs/API文档.md:598-608` 的「断线与错误码」表里确实有
   `| 超时 | 「由于您太久没回来游戏已关闭」 |`，但**没有触发条件的任何细节**。
6. README 的「已知限制」6 条在 `README.md:508-521`。
7. `report.html` 是静态文件，仓库里**没有**生成脚本（`grep -c 'function\|const data ='` = 1，
   只有内联样式/脚本，没有数据生成逻辑）—— 所以更新它需要手工或自己写个脚本。
