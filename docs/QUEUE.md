# 排队推送通道 `wss://n.cg.163.com/push/ws`

> 全部结论来自真机账号实测（2026-11-07 00:47–00:56 UTC+8），脚本在 `.tmp/cgprobe/q1~q6.mjs`，
> 原始帧存 `q4.json` / `q5.json` / `q6.json`（我跑的，零成本），
> 另参考 `pushws3.json`（lead 早期跑的，会烧时长，但录到了完整的 `queue_status`/`queue_success`/
> `ticket_destroyed` 三帧）。**不是从文档或推测抄的。**
>
> ⚠️ 除本文档标注「未实测」的条目外，每条都有抓到的原始帧支撑。

## 0. 一句话结论

排队页要走两条通道：**WS 接收（密文，必须解密）** + **HTTP 兜底轮询**。
`enqueue` 一旦成功，服务端 **约 2.3 秒内就分配机位**并开始扣免费时长 —— 排队不是"排队"，是"立刻开房"。

---

## 1. 帧格式：错误帧明文、业务帧密文（最容易踩的坑）

这是本文档最重要的一节，写错会导致客户端**永远收不到任何推送**。

| 帧类型 | 编码 | 判据 |
| --- | --- | --- |
| 服务端 **错误**帧 | **明文 JSON** | 首字符是 `{` |
| 服务端 **业务**帧 | **base64 密文** | 首字符是 base64 字符 |
| 客户端 `auth` | **明文 JSON** | — |
| 客户端其余 op | **必须加密** | — |

实测原始帧对照（同一次会话内）：

```
明文(155B) {"op": "error", "data": {"errcode": 1006, "errmsg": "登录信息失效，请重新登录"}}
密文( 60B) head="zHPAwXOLcXPDtsTGvcVzfXFztMO2ssW2sMW6vrZz"
      dec  → {"op":"result","create_time":1793984066}
密文(1948B) head="umGur2F5YbC0pLSknrK0oqKksrJha2Gj..."
      dec  → {"op":"queue_success","data":{...}}
```

### 1.1 解密算法

和 HTTP v2 的 body 编码是**同一套**（`cg.mjs` 的 `dec()` 可直接用）：

```js
function decFrame(raw, off) {
  const b = Buffer.from(String(raw).replace(/[^A-Za-z0-9+/]/g, ''), 'base64');
  for (let i = 0; i < b.length; i++) b[i] = (b[i] - off + 256) & 255;
  return b.toString('utf8');
}
// 判断是否需要解密
const isPlain = /^\s*[{[]/.test(raw);
const json = isPlain ? JSON.parse(raw) : JSON.parse(decFrame(raw, off));
```

### 1.2 offset 每次都变，绝对不能硬编码

`off` 从**当次登录**响应的 `encrypt` 字段推导（`cg.mjs` 的 `offsetFromEncrypt()`）。
连续登录 3 次的实测值：

```
method:1,number:985045090,timestamp:1793984114,salt:72   → off = 186
method:1,number:108900929,timestamp:1793984115,salt:97   → off = 212
method:1,number:835665267,timestamp:1793984115,salt:97   → off = 212
```

不同会话累计观测到 **93 / 63 / 7 / 81 / 186 / 212**。
用错 offset 解出来是乱码（不是 `JSON.parse` 抛错，是**解出合法的 UTF-8 乱码**），
所以不要用"解码是否抛异常"来判断 offset 对不对。

### 1.3 客户端发送要按 op 分支

```js
function sendOp(ws, off, pkt) {
  const raw = JSON.stringify(pkt);
  ws.send(pkt.op === 'auth' ? raw : off ? encFrame(raw, off) : raw);
}
```

实测反例（都会收到 `10003 无效数据格式`）：

- `auth` 用加密发 → 10003（auth 解析器只认明文）
- `enqueue` / `destroy_ticket` 用明文发 → 10003
- 未 auth 就发加密 op → 10003

---

## 2. 客户端动作

### 2.1 `auth`（明文）

```json
{"op":"auth","data":{"user_id":"<24位hex字符串>","token":"<JWT>","platform":1}}
```

- **`user_id` 必须是字符串**。写成数字 → `10003 无效数据格式`（实测）。
- `apk` / `ua` 是**可选**的：只发三个字段能通（q1/q2 实测），带 `apk:"6.2.2"` +
  `ua:<Chrome Mobile>` 也能通（lead 的 pushws3 实测）。不要因为多带了字段就以为 auth 失败。
- 成功**不回任何 ACK**，直接开始推业务帧。不要去等"认证成功"回包。
- token 无效 → 明文 `{"op":"error","data":{"errcode":1006,"errmsg":"登录信息失效，请重新登录"}}`，
  随后**服务端主动关闭连接**（`onclose code=1005`）。
  → 客户端收到 1006 应当清 session 并跳登录页，不要重连。

### 2.2 `enqueue`（加密）

```json
{"op":"enqueue","data":{"game_code":"mrfzzmd","region":"ghnfs","platform":1}}
```

- `region` 可省，服务端用最快机房兜底；带上能缩短分配时间。
- `extra:{dpi,source_type}` **不是必需**。
- game_code 不存在 → 加密 `{"op":"error","data":{"errcode":11000,"errmsg":"未知服务端错误"}}`（实测）。
- ✅ 成功时序见 §5。**注意：成功即扣时长**。

> **`enqueue` 与 HTTP `POST api/v2/tickets` 是两条不同的申请机位路径** ——
> 后者见 [CGAPI.md](./CGAPI.md) §申请机位（同步返回 `gateway_url`/`ticket`），
> 前者是推送式的（`queue_success` 推 ticket，`gateway_url` 在 `games_playing_status` 里）。
> 两者**都会真的占机位、真的扣时长**，不要拿任一条做探测。

### 2.3 `destroy_ticket`（加密）

```json
{"op":"destroy_ticket","data":{}}
```

- 空 data 即可，服务端按 `user_id` 找当前 ticket。
- 带 `{"game_code":"xxx"}` 也接受（两种形态都实测回 `result`）。
- 回执恒为加密 `{"op":"result","create_time":<unix>}`。
- 有真实 ticket 时另推 `ticket_destroyed`（**已实测**，见 §3.6），与 `queue_success.data` 同构。
- 无 ticket 时只有 `result`，不会有 `ticket_destroyed` —— 所以「零成本探测」用它。

### 2.4 `dequeue`（加密）

```json
{"op":"dequeue","data":{"game_code":"mrfzzmd"}}
```

- 实测回执同样是 `{"op":"result","create_time":...}`。
- 当前没在排队时发它也是 `result`（幂等 no-op，实测 3 次全是 result）。
- 语义推测：从排队列表里撤出，**但不清 ticket**。真正的"取消"要用 `destroy_ticket`。

---

## 3. 服务端推送

### 3.1 `queue_status` —— 排队中的进度帧

**字段表（真实抓包，完整）**：

| 字段 | 实测值 | 说明 |
| --- | --- | --- |
| `ranking` | `1` | **前面还有几个人**（← UI 要读这个） |
| `notice_ranking` | `500` | 超过这个数显示成 `500+` |
| `ranking_time` | `1` | 排名快照时间（秒级相对值） |
| `vip_queue_len` / `non_vip_queue_len` | `0` / `1` | VIP / 普通队列长度 |
| `vip_queue_time` / `non_vip_queue_time` | `1` / `1` | 两队列预计等待（秒） |
| `show_ranking_time` | `false` | 是否展示"预计等待" |
| `queue_id` | `"6aee0562bc162556e975df83"` | 本次排队 id |
| `region` / `region_name` | `"ghnfs"` / `"华南三区"` | **当前最佳选区** |
| `game_code` / `game_name` / `game_icon` | — | 游戏信息 |
| `user_type` / `user_type_rank` / `user_type_sub_rank` | `"potential_pay"` / `9` / `0` | 用户分级 |
| `created_by` | `"phone-captcha"` | 登录方式 |
| `user_id` / `phone` | `"67d9…"` / `"186****8726"` | 账号（明文手机号，注意别外泄） |
| `cpu` / `only_vp8` / `channels` | `1.0` / `false` / `[]` | 端能力声明 |
| `game_type` / `game_logo` | `"mobile"` / `""` | 游戏信息 |
| `starting` / `preboot` / `fast_created` | `false` | 机位预启动标记 |
| `timeout` | `false` | 是否已超时 |
| `gp_ext_business_data` | `{user_ticket_time, en_queue_time}` | 入队时间戳 |
| `client_ip` / `last_channel` / `last_device` | — | 端信息 |
| `tags` / `game_platforms` | — | 标签 / 平台码 |

> 证据来源：本表的 `ranking` 系字段来自一次完整的实时解码；`pushws3.json` 里同一帧
> 被脚本截断到 600 字符，只能佐证前半段（`user_id`/`phone`/`region_name`/`user_type_rank`/
> `only_vp8`/`starting`/`fast_created` 等）。**两份证据合起来才是完整表**。
>
> ❌ **`rank` 和 `wait_time` 这两个字段不存在**。旧版本文档写过它们，是取证截断导致的臆造。
> `queue.vue` 的 `applyStatus()` 读 `st.rank ?? st.position ?? st.queue_position` 会永远拿到
> `undefined`，排名会卡在「获取中」。**必须改读 `ranking`**，前面还有 N 人用 `non_vip_queue_len`。

### 3.2 `queue_success` —— 拿到机位（关键帧）

**实测到的部分**（`pushws3.json`，原始帧被脚本截断在 600 字符，以下字段都完整可见）：

```json
{"op":"queue_success","data":{
  "_id":"6aee056215ffaa4ece9d5260","play_id":"6aee056215ffaa4ece9d525f",
  "user_id":"67d***ee8",
  "game_code":"mrfzzmd","region":"ghnfs","region_name":"华南三区",
  "platform":"web-mobile",
  "game_name":"明日方舟：终末地（官服）",
  "user_type":"potential_pay","game_type":"mobile",
  "ticket":"33957828-1b00-41c1-ae2d-ac61ac08b57c",
  "game_icon":"https://g.fp.ps.netease.com/…","game_logo":"",
  "channels":[],"private_region":false,
  "game_platforms":[0,1,2,3,4,8,9,…]
  /* 600 字符处截断，后续字段未观测到 */
}}
```

⚠️ **落盘的这份 `queue_success` 没有 `gateway_url` / `lock_detail` / `expires`**，因为帧被截断了。

> 另有一次 **00:53 的实时解码**（未存盘，仅会话记录）在同一帧里看到过
> `gateway_url`、`expires`、`ticket_expires:25`、`time_left:24`、`screen`、`lock_detail{port:"6124",container_idx:24}`。
> **这一条只有单次观测、没有可复查的原始帧**，所以下面正文一律以有落盘证据的
> `games_playing_status` 为准；把 `expires/ticket_expires` 当可能存在但需自测确认处理。

**所以正确的对接姿势是**：`queue_success` 只当"ticket 到手"的信号
（`ticket` + `game_code` + `region`），**连网关要用的 `gateway_url` 去 `games_playing_status` 里取**
（实测 `queue_success` 后 3 秒内必推，带完整 `gateway_url`、`lock_detail`、`screen`）。

`games_playing_status` 条目里实测到的连接信息：

```json
{
  "gateway_url":"wss://gw-dx-hnfs-03.cg.163.com:6443/ws",
  "lock_detail":{
    "region":"ghnfs","ip":"10.85.10.31","external_ip":"121.11.36.82","port":"6104",
    "game_video_port":6804,"game_audio_port":6704,
    "width":1280,"height":720,"container_idx":4,
    "agent_ip":"10.85.10.31",
    "debug":{"container":"topa10_261106_210420,t-04","encoder":"high,None","host":"ahnfs10006"}
  },
  "screen":"horizontal","free":true,"playing_time":20,
  "is_game_fullscreen":false,"live_can_control":true,"is_ultimate":false
}
```

- `gateway_url` 形如 `wss://gw-<idc>-<xy>-<NN>.cg.163.com:6443/ws`（同账号不同次 `-03` / `-04`）。
- `lock_detail.ip` / `port` 是**容器内网**，真机不要直连；一律走 `gateway_url` + 网关 SDP。
- `playing_time: 20` 是**秒**（本次会话已跑 20 秒），不是分钟。

### 3.3 `result` —— 动作回执，**data 可有可无**

两种形态都实测到：

**裸回执**（`destroy_ticket` / `dequeue` 的回包，43 字节）：

```json
{"op":"result","create_time":1793983846}
```

**带全量快照**（`enqueue` 的回包）：

```json
{"op":"result","data":{
  "user_id":"67d9…","phone":"186****8726","created_by":"phone-captcha",
  "current_is_vip":false,"cpu":1.0,"only_vp8":false,
  "game_code":"mrfzzmd","game_name":"明日方舟：终末地（官服）","game_type":"mobile",
  "game_icon":"…","game_logo":"","game_params":{},
  "game_platforms":[0,1,2,3,4,8,9,14,6],
  "channels":[],"codecs":[],"client_ip":"163.125.229.253",
  "cloud_pc_info":{},"device_info":{},"exit_popup":false,"fast_created":false,
  "additional_data":{"platform":1}
},"create_time":1793983842}
```

> 别把 `result` 当成"只有 op + create_time"。`enqueue` 的 `result.data` 会带
> 用户 + 游戏 + 平台能力的全量快照。**`current_is_vip` 在这个帧里，不在 `queue_status` 里。**
>
> 客户端不要靠 `result` 判断排队状态（它不保证有状态字段），状态请读 `queue_status`。

### 3.4 `games_playing_status` —— 全局机位广播（权威）

```json
{"op":"games_playing_status","data":{"games_playing":[{
  "_id","play_id","game_code":"mrfzzmd","region":"ghnfs","region_name":"华南三区",
  "status":"exiting",
  "start_time":1793984111,
  "name":"明日方舟：终末地（官服）","game_type":"mobile","game_icon","game_logo","game_tags":[...],
  "params":{ ...巨型对象：quality/width/height/token/stream_ip/agent_host_name/
             container_image/encode_fps_used/stream_status/is_cpa ... }
}]}}
```

- 空数组 `{"games_playing":[]}` = 当前没有机位。
- `status` 实测到 `exiting`；另有 `placehold`（`params.status` 内层）、推测 `playing` / `queueing`。
- **这是判断"我到底有没有机位"的唯一权威来源**，比 `GET users/@me/status` 更及时（推 vs 拉）。
- 实测时序：`destroy_ticket` 后先推一帧带旧条目（`status:"exiting"`），**立刻又推一帧空数组**。

**连网关需要的那几个字段就在这个条目里**（完整示例见 §3.2 末尾）：

| 字段 | 实测值 | 用途 |
| --- | --- | --- |
| `gateway_url` | `wss://gw-dx-hnfs-03.cg.163.com:6443/ws` | 连网关的唯一正确地址 |
| `lock_detail.port` | `"6104"`（字符串） | 容器内网端口，**不要直连** |
| `lock_detail.game_video_port` / `game_audio_port` | `6804` / `6704` | 仅供排查，实际由网关 SDP 下发 |
| `lock_detail.width` / `height` | `1280` / `720` | 画面规格 |
| `screen` | `"horizontal"` | 横/竖屏，决定渲染朝向 |
| `playing_time` | `20` | **秒**，本次会话已跑时长 |
| `free` | `true` | 本次会话是否走免费时长 |

- 条目顶层还有 `input` / `channels` / `private_region` / `is_hang_up` / `multiboxing` /
  `pc_halt_in_mobile` / `preboot` / `ext_business_data` / `real_user_id` / `additional_data` /
  `game_platforms` / `is_ultimate` / `live_can_control` / `is_game_fullscreen`（`q6.json` 实测键名）。

### 3.5 `message` —— 站内公告推送（额外发现）

```json
{"op":"message","data":{
  "title":"终末地打卡-赠PC高配体验时长活动领奖通知",
  "content":"<br/>亲爱的玩家：<br/>…（HTML 片段，实测 466 字符）",
  "create_time":1793984132,"no_read_num":1,"msg_id":"...","is_native_notification":false
}}
```

- 与排队无关，`auth` 后无条件推送（如果有未读公告）。
- `content` 是**富文本 HTML**，Weex `<text>` 不能直接渲染 → 要剥标签取纯文本。

### 3.6 已实测到的 `ticket_destroyed`

```json
{"op":"ticket_destroyed","data":{
  "_id":"6aee056215ffaa4ece9d5260","play_id":"6aee056215ffaa4ece9d525f",
  "user_id":"67d9…","game_code":"mrfzzmd","region":"ghnfs","region_name":"华南三区",
  "platform":"web-mobile","game_name":"明日方舟：终末地（官服）",
  "user_type":"potential_pay","game_type":"mobile",
  "ticket":"33957828-1b00-41c1-ae2d-ac61ac08b57c",
  "game_icon":"…","game_logo":"","channels":[],"private_region":false,
  "game_platforms":[0,1,2,3,4,8,9,14,6]
}}
```

- 与 `queue_success.data` **同构**（本文档前文的"未实测"标注已更正：**这条是实测的**）。
- 触发时机：服务端销毁 ticket 时推，无论销毁原因是客户端 `destroy_ticket`、
  ticket 过期、还是被别的会话顶掉。
- 客户端收到它 = 这次排队窗口彻底结束，应当回到空闲态并重新 `queue_status` 等待下一次。

### 3.7 未实测的三种（静态推断，勿当契约）

| op | 推断 payload | 备注 |
| --- | --- | --- |
| `queue_success_soon` | `{queue_id, play_id, ...}` 前缀同 `queue_status` | 分配前摇；**未实测** |
| `queue_canceled` | `{queue_id, reason?}` | ticket 未及时确认时会推；**未实测**，超时秒数不确定 |
| `queue_fail` | `{errcode, errmsg, queue_id?}` | 无可用机位；**未实测** |

⚠️ 为凑这几个 schema 再发 `enqueue` 的代价是**真实免费时长**，不要为了测试去排队。
另外注意：本账号在空闲机位充足时 **`queue_status` 只推了 1 帧就被 `queue_success` 覆盖**，
所以「真排队」时 `ranking` 如何递减、`queue_success_soon` 何时出现，**我没有任何实测数据**。
queue.vue 必须把这两条当"可能永远不出现"来写（必须有兜底）。

---

## 4. 兜底轮询

```
GET api/v2/users/@me/status        （v2，需加密 body / Bearer）
→ {"games_playing":[]}
```

- `games_playing` 为空 = 无机位。
- 页面隐藏时暂停，前台 3 秒一次足够。
- 优先级：WS `games_playing_status` > 本接口。WS 断了才轮到它。

---

## 5. 完整时序（实测，来自 `pushws3.json` 的 `t` 字段）

以客户端发出 `enqueue` 为 t≈0：

```
t=0.00s  client → (明文) auth
t=0.00s  client → (加密) enqueue {game_code:"mrfzzmd", region:"ghnfs", platform:1}
t=1.66s  server → (密文) queue_status           1924B
t=1.70s  server → (密文) result  带全量快照     1128B   ← enqueue 回执
t=2.31s  server → (密文) queue_success          1948B   ← 机位到手，扣时长开始
t≈5.6s   client → (加密) destroy_ticket {}
t=5.63s  server → (密文) ticket_destroyed       1058B
t=5.64s  server → (密文) result  裸回执           43B
t=5.65s  server → (密文) games_playing_status   562B    ← [{status:"exiting"}]
         server → (密文) games_playing_status            ← {games_playing:[]}
```

**注意 1.66s 这个数字**：`enqueue` 到 `queue_success` 只有 **2.31 秒**。这台账号在
"potential_pay 分级 + 华南三区有空闲机位"的条件下**根本没有排队**，直接分配。
真排队时的 `queue_status` 会持续推送、`ranking` 递减 —— 但那个分支我**没有实测过**。

---

## 6. ⚠️ 排队会消耗免费时长（血泪教训）

**旧文档里那句「排队不消耗免费时长」是 UI 文案，不是事实。**

实测：`enqueue` 成功后 **2.31 秒**就收到 `queue_success`（直接给机位，不排队），
`time_left` 当场开始倒数，账号免费时长 `1333 → 1298` 秒，**一次探测烧掉 35 秒**。

因此：

1. **不要为了探测 schema 发 `enqueue`**。想验证连接与解析，用 `destroy_ticket`（无 ticket 时是 no-op，零成本）。
2. 排队页「取消排队」必须三件事一起做：
   - WS `destroy_ticket`（加密）
   - HTTP `DELETE api/v2/users/@me/games-playing/<game_code>`
   - 若已收到 `queue_success`，还要 `stopGame()` 释放机位
3. 用户全账号每天只有约 **25–30 分钟**，每次 `enqueue` 都是真金白银。
4. **两个会烧时长的脚本，勿随意运行**：
   - `.tmp/cgprobe/pushws3.mjs`（加密 `enqueue`）—— **就是它烧掉的 35 秒**，
     它的 `pushws3.json` 是本文档时序与 `ticket_destroyed` 的证据来源。
   - `.tmp/cgprobe/pushws2.mjs`（`enqueue` 明文发送）—— 4 帧全是 `10003 无效数据格式`，
     **从未真正入队**（这也是必须加密的最早证据）。跑它不烧时长，但会误导人以为发出去没反应。
   两个都由 lead 早期编写。

---

## 7. 对 `PenCG/src/pages/queue/queue.vue` 的具体修正项

1. `applyStatus()` 读 `st.rank` → **改读 `st.ranking`**；「前面还有 N 人」用 `st.non_vip_queue_len`。
2. 「预计等待」不要用 `wait_time`（不存在），用 `non_vip_queue_time`（秒），并受 `show_ranking_time` 控制是否展示。
3. 区块标题/次要信息可以显示 `region_name`（如「华南三区」），这是服务端选的当前最佳选区。
4. 收到 `queue_success` 后要**立刻**去 `games_playing_status` 取 `gateway_url` 并发起连接。
   若 `data.ticket_expires` 存在则用它做倒计时，**没有时不要编一个数字**，显示「准备中」即可。
5. 解析 WS 帧必须先判明文/密文再 `JSON.parse`，且 offset 用当次登录值。
6. `message` 推送的 `content` 是 HTML，需剥标签。
7. **不要把 `queue_status` 当作 `queue_success` 的前置条件**。空闲机位充足时只推 1 帧
   `queue_status` 就紧跟 `queue_success`（实测 2.31 秒），甚至有可能是 0 帧。
   页面必须在「已 enqueue 但还没收到任何 queue_status」时也能正常显示等待态。
8. `result` 帧的 `data` **可能不存在**（`destroy_ticket`/`dequeue` 的裸回执只有 43 字节）。
   解析时不要直接 `msg.data.xxx`，先判 `msg.data`。
9. `ticket_destroyed` 才是「本次排队窗口彻底结束」的信号；只收到裸 `result` 不代表 ticket 没了。

---

## 8. 复现方法

```bash
cd .tmp/cgprobe
node q4.mjs   # 零成本：auth + dequeue + destroy_ticket，录帧格式
node q6.mjs   # 零成本：auth 错误分支 / offset 稳定性 / 未 auth 发 op
# node q5.mjs # 有成本风险：非法 game_code 的 enqueue（实测不分配机位，但仍不建议随意跑）
```

原始抓帧：`q4.json`、`q5.json`、`q6.json`（我跑的，零成本），
以及 `pushws3.json`（lead 早期跑的，**就是烧掉 35 秒那次的完整录帧**，本文档的时序与
`ticket_destroyed` 都出自它）。

> `pushws2.json` 是反例集：4 帧全是 `10003 无效数据格式`，可用来验证「明文不能发业务 op」。
