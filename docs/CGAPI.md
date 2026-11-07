# 网易云游戏（cg.163.com）接口笔记（真机实测）

网关 `https://n.cg.163.com/`

## 通用头
```
X-Platform: 1
X-Channel: web
X-Source-Type: web
X-Ver: 1.0.0
User-Agent: <Chrome UA>
Referer: https://cg.163.com/
Origin: https://cg.163.com
```

## 加密（v2 接口）
- 登录响应里的 `encrypt` 形如 `method:1,number:579572448,timestamp:1793976143,salt:58`。
- 推导偏移：
  ```js
  const parts = encrypt.split(',').map(s => s.split(':')[1]);
  const nums  = parts.join('d').split('d').map(Number);
  const [, a, b, c] = nums.slice(0, 4);
  const off = (((a | b) % 256) + c) % 256;
  ```
- 请求体：`base64( map(b => (b + off) % 256, utf8(JSON)) )`，
  头 `Authorization: Bearer <token>` + `content-type: application/octet-stream`。
- 响应体：裸 Base64（`content-type: application/octet-stream`），补 padding 后
  `map(b => (b - off + 256) % 256)` 再 `utf8`。
- **HTTP 与 WS 共用同一个 off**。

## 密码登录
1. `GET api/v1/user-pwd-info?phone=<p>&ctcode=86`
   → `{has_pwd, is_registered, login_iv, login_random_text, pwd_encrypt_key, user_pwd_login_salt}`
2. `secret = SHA256(salt + password)` 的小写 hex
   （**必须走 native `sha256Hex`**，QuickJS 无 crypto；JS 手写 SHA256 也可，但 native 更快更稳）
3. `plain = secret + login_random_text`
4. `key = pwd_encrypt_key`（24 字节 ASCII → 3DES-EDE3）
5. `iv = hex(BigInt(login_iv).toString(16).padStart(16,'0').slice(-16))`（8 字节）
   —— **无 BigInt**，直接取十进制字符串转 hex（16 位 hex 足够，用字符串运算）。
6. `pwd = base64(3DES-EDE3-CBC-PKCS7(plain))`（**native `tripleDesB64`**）
7. `POST api/v1/user-pwd-tokens {phone, ctcode:'86', pwd, login_mode:'password'}`
   → `{token, user_id, encrypt, free_time_left, nickname, vip, ...}`

## 短信登录
1. `POST api/v1/phone-captchas/86-<phone>` → 发码
2. `POST api/v1/tokens {auth_method:'phone-captcha', ctcode:'86', phone, captcha}`
   → 同密码登录返回

## 匿名接口（v1，无需 token）
| 路径 | 说明 |
| --- | --- |
| `api/v1/home_page_tabs` | 首页 tab 列表 |
| `api/v1/main_recommendations?main_recommendation_type=mobile_home[&tab_id=]` | 推荐模块数组（8 个） |
| `api/v1/banners?banner_type=mobile_home` | 轮播 |
| `api/v1/games?page=0&per_page=10&game_type=mobile&src=4` | 游戏列表（**必须带分页**，否则 6MB） |
| `api/v1/games/<game_code>` | 游戏详情 |
| `api/v1/search?keyword=<kw>&page=0&per_page=20` | 搜索 |
| `api/v1/hot-searches` | 热搜 |
| `api/v1/search_keyword_hints?keyword=<kw>` | 联想 |
| `api/v1/speed_url?game_type=mobile` | 各画质建议码率 |
| `api/v1/notices/list` | 公告 |
| `api/v1/client-settings/@current` | 客户端配置 |
| `api/v1/resource_icons?type=main_page` | 图标资源 |
| `api/v1/versions/@current?package_name=H5` | 版本 |

## 登录态接口（v2，需加密）
| 路径 | 说明 |
| --- | --- |
| `GET users/@me` | 用户信息（96 字段：bluray/free_time_left/vip/cloud_pc…） |
| `POST tickets` | 申请机位 |
| `POST network-tests` | 上报测速 `{latency, bandwidth(Mbps), quality, ua, details:[{region,latency}]}` |
| `GET media-servers?game_code=<code>` | 可用机房列表 |
| `POST turn-credentials {region}` | TURN 长凭证 |
| `GET users/@me/game-playing-histories` | 游玩历史 |
| `DELETE users/@me/games-playing/<game_code>` | 释放机位 |
| `GET users/@me/status` | 游玩状态（**排队兜底轮询**；排队主通道见 [QUEUE.md](./QUEUE.md)） |
| `POST users/@me/games/<code>/common_setting` | 悬浮球位置等 |

## 申请机位

> 排队与机位推送走 `wss://n.cg.163.com/push/ws`，与本节 HTTP 接口是两条不同通道。
> 帧编码、字段、时序一律以 **[QUEUE.md](./QUEUE.md)** 为准（本文档不重复）。
> ⚠️ `enqueue` 成功会**立刻分配机位并开始扣免费时长**，不是"排队不消耗"。

```js
POST api/v2/tickets {
  regions: ['ghnfs','ghzcs','ldhnfs'],
  game_code, codecs: ['h264'], width: 1280, height: 720,
  extra: { dpi: 3, resolution: '1280x720', source_type: 'web',
           network_test_info_list: [{ region: 'ghnfs', latency: 30, bandwidth: 60 }] },
  apk_decoder: { model: '', decoder: [] }
}
```
- **bandwidth 单位是 Mbps**（传 50000000 会一直 `1288 请重新测速`）。
- 成功响应含 `gateway_url`（`wss://gw-xx-xx-03.cg.163.com:6443/ws`）、`lock_detail`、
  `expires`、`ticket`、`region_name`、`screen`、`time_left`、`user_type`。
- 连续申请同一 game_code 会偶发返回 `{}` → 先 `DELETE users/@me/games-playing/<code>`
  再 sleep 4~5s 重试；`33803` 是抢锁、`1209` 是无空闲机位，重试即可。
- 错误码：`33803` 多实例 / `1209` 无资源 / `1288` 需重测速 / `1269` 需选区域 /
  `1250` 网络太差 / `1279` 云电脑 C 盘损坏 / `1506` resume game failed。

## 游玩信令（网关 WebSocket）
- 连 `gateway_url`，**首个 auth 包必须明文 JSON**；之后所有包走 offset 加密。
- 包结构 `{id, op, data}`。
- auth（**只发这 7 个字段，多一个都会 1506**）：
  ```js
  {user_id, token, game_code, w:1280, h:720, quality:'high', codecs:['h264'], platform:1}
  ```
  ❌ 不要带 `encoder` / `fps` / `res` / `apk_decoder`。
- 服务端 op：`offer`(SDP) / `result{container,encoder,host}` / `input{cmd}` /
  `game_stop` / `info` / `error{errcode,errmsg}` / `answer` / `close`。
- 收到 `offer` 后：解析 SDP → 建 ICE → DTLS → 回 `answer`。
- 输入：`{op:'input', data:{cmd:'<code> <args>'}}`，坐标 `Math.ceil(65535 * x / videoWidth)`。
  `1/2/3 x y id` 按下/移动/抬起；`8 x y id` 点按；`4` 取消；`5 <text>`；`6 <keycode>`；
  `0 <ts>` 心跳；`10 <platform>`；`25 touchscreen|keymouse`。
- 画质：`{op:'game_set_quality', data:{quality:'low|middle|high|bluray'}}`（热切换）。

## offer SDP 关键点
- `a=ice-lite`，每个 m 段一条 `a=candidate:N 1 udp <prio> <公网IP> <port> typ host`，
  三段候选的 **port 相同**（BUNDLE + rtcp-mux）。
- m 段数量不固定（2 或 3 段：audio / video / application-datachannel）。
- `a=setup:actpass` → 我方做 **DTLS client（`a=setup:active`）**。
- `a=fingerprint:sha-256 ...` 是服务端证书指纹。
- 视频首选 `98 H264/90000`，`fmtp:98 level-asymmetry-allowed=1;profile-level-id=42e01f;packetization-mode=1`。
- 音频 `111 opus/48000/2`。
- **解析 SDP 一律 `line.slice(prefix.length)`，不要硬编码数字**（曾因 `slice(11)` 多带一个
  冒号导致 ufrag 错误、服务端静默丢弃 STUN 包，卡了很久）。
