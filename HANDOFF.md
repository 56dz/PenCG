# PenCG 交接说明（2026-11-07 20:52）

## 一句话
网易云游戏（cg.163.com）的有道 X6 Pro 客户端。**界面层、网络层、信令层、媒体层协议栈全部完成，
真机端到端出画已验证**（魔兽世界 1600x900、第五人格 720x1280）。
播放页已按需求重构为三栏（左信息 / 中视频 / 右操作），退出软件时 KMS 平面会正确释放。

## 当前状态（全部真机验证通过）
输入（触摸/鼠标）已打通、云游戏内文本输入与数字键盘、音频出声、**蓝牙 A2DP 音频路由与 150ms 缓冲优化**、
**画质收敛为「流畅」单档**（标清/高清/蓝光在真机上仍旧出马赛克或纯绿，用户 m12491 拍板只留流畅）、
导航 5 栏（推荐/手游/端游/限免/我的）「我的」原地渲染、限免栏走官方 `tag=限免`（服务端 total=16，与官网限免专区一致）、
左栏显示「已玩时长」（`time_left` 是 ticket 有效期不是单局上限）、退出软件自动释放 KMS 平面与机位。

### 三个语义/延迟上的坑（都已实测定案，别再翻案）
1. **`time_left` 不是游玩时长** —— 它是 ticket 连网关的有效期（≈25 秒倒计时，`expires` 恒定、`time_left`
   逐次递减）。服务端**不下发单局时长限制**，玩家能一直玩到主动退出。证据：三连
   `POST api/v2/tickets` 得到 `time_left=24/22/20` 而 `expires-now=25.1/23.0/20.8`。
2. **bluealsa PCM 默认缓冲有 256 ms 死延迟** —— `aplay` setup dump 的 `BUFFER_BYTES: 98284`
   = 12285 帧 / 48000 Hz。这就是「耳机比画面慢约 0.5 秒」里可控的那一半（剩下的是 SBC 编码 +
   耳机侧 jitter buffer）。蓝牙分支现在传 `-B 150000 -F 30000`，并在 `cg_aplay.log` 落 `-v` dump 便于复核。
   **真机已验证（Redmi Buds 6，2026-11-07 19:30）**：dump 显示 `buffer_size: 7200` /
   `period_size: 1440` / `Selected HW buffer: 5 periods x 5760 bytes == 28800 bytes`（=150 ms），
   整局 `underrun` 计数 **0**，`aplay` 全程存活，`/proc/asound/card1/pcm0p/sub0/status` = `closed`
   （声音走 bluealsa 不走扬声器）；同一局平面缓冲 `uniq 91196` 正常出画。
   查法：`cat /userdisk/miniapp/data/cg_aplay.log`。回退开关：`/userdisk/miniapp/data/audiodev.txt`
   写 `speaker` 强制外放、写 `bt` 强制蓝牙、删文件=自动。
3. **`aplay` 的 `-B`/`-F` 是时间（微秒），不是帧数** —— 换采样率不用跟着改。设备上
   `/bin/aplay` 是 **1.2.4**，`-B/-F/--buffer-size/--period-size` 全支持。

### 登录态三处修正（2026-11-07 20:52，用户 m13280 反馈）

1. **「我的」页先闪「还没有登录」** —— `components/mine-body.vue` 的 `logged` 初值 false 而
   `restoreSession()` 是异步的，模板在等待期走了未登录分支。修法：加 `booted` 状态，
   `boot()` 里 `await reloadLocal()` 之后才置 true，等待期显示「正在读取账号…」。
2. **登录成功后导航塌成两项 / 仍显示未登录** —— 两个独立根因：
   - 登录页 `navTo('mine')` 压的是一个只有「首页 / 我的」两项的薄壳页；改成
     `setPendingTab('mine')` + `navBack()` 回到 index 原地切栏目（`services/nav.js` 新增
     `setPendingTab`/`takeTabParam`，30 秒过期）。
   - **更隐蔽的一处**：`index.vue` 的 `onShow()` 在 `if (t && t !== this.tab) { this.setTab(t); return; }`
     里直接 `return`，导致「登录成功 → navBack → setTab('mine')」这条路径下
     `mine-body.refresh()` 永远不执行。而组件是 `v-if` 挂载的，登录前就已挂上（logged=false），
     登录后不会重新挂载 → 界面一直停在「还没有登录」。修法：抽出 `refreshMineBody()`
     （`$refs.body` → `$root.__mineBody` → `this.__mineBody` 三级回退 + 120/260ms 两次重试），
     并在 `setTab` 之后用 `$nextTick` 调它，不再 `return`。
3. **易盾文案** —— 密码登录返 `1125 need_yidun_validate_fail_enough`、发短信返
   `1122 phone_validate_fail_yidun`，`friendlyErr()` 都没处理，直接把「易盾校验失败次数过大,
   要前端cool」甩给用户。统一成「易盾校验失败，请切换网络或稍后重试」；
   `sendCode()` 的 catch 里对这类完整文案**不再加「发送失败：」前缀**。

> 注：易盾风控会把账号锁成 `1125`（正确密码也拒），主机侧 `POST api/v1/phone-captchas/86-<phone>`
> 同样返 `1122`。短信修法时断了、修好后又能发。冷却时长来自
> `getCustomizeSettings("yidun_signin_validate")` 的 `risk_control_cooling_time`，
> 但 `api/v1/customize-settings/yidun_signin_validate/all` 返回 `{"val":null}`（服务端未配置）。

### 两个必须记住的回归陷阱
1. **视频管线不能加 `! queue`** —— 加了之后 `mppvideodec` 输出全零缓冲，画面变成 BT.601 的纯绿 `(0,135,0)`。
   音频那条 `queue` 必须保留（opusdec 的 caps 协商依赖它）。
2. **`onTS` 里的 `var ts = (e && e.changedTouches) || [];` 不能删** —— 删了会抛 ReferenceError，
   表现是「画面在播但怎么点都没反应」，极像协议问题。

### 待办：登录成功提示未真机验收

`#4`（「登录成功，正在跳转…」）的代码已进包、字符串也在 `.falcon_/login.js.bin` 里验到，
但**没有真机跑到那 600 ms 的窗口**：易盾风控把账号锁成 `1125`（正确密码也拒），
主机侧 `POST api/v1/phone-captchas/86-****8726` 同样返 `1122`/`1125`。
等冷却后做一次短信登录即可验收；跳转路径本身已验（登录后确实回到了 index 的「我的」）。

### 设备端 preferences.json 的正确修改方式（我踩过一次）

`/userdisk/miniapp/data/mini_app/pkg/8002026110200003/data/sharedpreferences/preferences.json`
里的 `pencg_session` 是「JSON 字符串里再套一层 JSON」，值形如 `{\"token\":\"...\"}`。

**绝对不要用** `sed 's/"pencg_session": "[^"]*"/.../'` —— `[^"]*` 会在第一个转义引号
`\"` 处停下，只吃掉 `{\` 就把前半截截断，整个文件变成非法 JSON，
`restoreSession()` 直接失败、用户表现为「登录态丢了」。
正确做法：**pull 到主机用 python `json.loads` 校验后再 push 回去**（我在主机侧修复并
重新 push，登录态已恢复、7 条历史与 VIP 资料都在）。

### 返回动画：`$falcon.navBack` 在本固件不存在（2026-11-07 21:20，用户 m14169）

现象：进子页是「从右往左滑入」，返回**也**是从右往左滑入，应该向右滑出。

真因：`/etc/miniapp/resources/framework/jsfm-nvue.js.bin` 与 `js-framework.min.bin`
里 `grep -c navBack` **都是 0** —— 本固件只有 `navTo` / `closeApp` /
`closePageByName` / `closePageById`。所以 `nav.js` 里
`typeof $falcon.navBack === 'function'` 恒假，每次都掉进兜底 `navTo('index')`，
那是**新压一层 index**（push 动画），页面栈还会一层层变深。

修法：`navBack(pageName)` 改走 `$falcon.closePageByName(<当前页名>)`，
各页面调用点补自己的名字（`navBack('search')` 等）。

取证（`/usr/bin/miniapp_cli capture` 连拍，约 75ms/帧）：
- 修前 `profiles/evidence/nav_back_push_bug.png`：搜索页停在 0..525 没动，
  index 从右侧 540 压进来 → push。
- 修后 `profiles/evidence/nav_back_pop_fixed.png`：index 已占 0..460，
  搜索页整体右移到 460 之后继续滑出 → pop。

## 产物
| 文件 | 大小 | 说明 |
|---|---|---|
| `8002026110200003.1_0_0.amr` | 218665 B | 可安装包，含两个 native .so + logo 资源 |
| `app_icon.png` | 23106 B | 256x256，官方 logo 矢量化重建 |
| `src/assets/logo.png` | 10451 B | 128x128，界面内使用的软件图标 |
| `libs/libjsapi_cgnative.so` | 62200 B | HTTP + 加密原语 + 自建 WebSocket + webp 转码 |
| `libs/libjsapi_cgmedia.so` | 74640 B | ICE + DTLS-SRTP + RTP + GStreamer + aplay 音频 |

## 真机截图（`profiles/evidence/`）
| 页 | 文件 | 字节 |
|---|---|---|
| 首页（新版） | `ih_v2_1_recommend.png` | 198570 |
| 首页·手游 | `ih_v2_2_mobile.png` | 144211 |
| 首页·端游 | `ih_v2_3_pc.png` | 138413 |
| 首页·限免 | `ih_v2_4_free.png` | 145475 |
| 上滑切 tab | `ih_v2_6_swipe_up_after.png` | 145527 |
| 下滑切 tab | `ih_v2_8_swipe_down_after.png` | 191100 |
| webp 图标举证 | `shot_webp_final.png` | 198499 |
| 详情 | `final_detail.png` | 128287 |
| 搜索 | `final_search.png` | 71994 |
| 登录 | `final_login.png` | 35347 |
| 我的 | `final_mine.png` | 44048 |
| 播放（旧两栏） | `final_play2.png` | 38166 |
| 播放（新三栏） | `play_three_col.png` | 37745 |
| 出画·用户视角 | `play_game_user_view.png` | 102845 |
| 出画·面板态 | `play_panel_ok.png` | 34784 |
| 出画·原神（画面真正在动） | `play_genshin_live.png` | 26111 |
| 排队 | `final_queue.png` | 34353 |
| 自检 | `final_diag.png` | 62163 |

## 已验证通过（真机）
- 密码登录（真实账号）→ token → v2 加密接口全通
- 首页 8 个推荐模块 + 3 张 banner + 公告 + 榜单，真实数据渲染
- 详情页（崩坏3，真实简介/标签/时长/可用区）
- 搜索（7 个热搜词，6 个能搜到结果）
- 我的页（昵称、脱敏 ID、脱敏手机号、剩余时长「22 分钟」）
- 播放页/排队页渲染（状态机、四档画质、双列布局）
- 申请机位 → 网关 WSS → 明文 auth → 收 offer → 回 answer → **网关回 `op=result`、容器启动**
- STUN Binding + USE-CANDIDATE 提名
- **DTLS 握手（DTLSv1.2 / ECDHE-RSA-CHACHA20-POLY1305 / SRTP_AES128_CM_SHA1_80）**
- **SRTP 密钥导出（60 字节 KM，与主机复算一致）**
- webp 图标自动转码（设备端 ffmpeg → png → `file://`，真机渲染确认）
- 首页左侧导航上下滑动手势切换
- 自检页 `cgnative diag ALL PASS 32`

## 媒体层修掉的 4 个真 bug
1. `SSL_OP_NO_QUERY_MTU` 后缺 `SSL_set_mtu()` → 第一条记录 `SSL_ERROR_SYSCALL`，一个字节都发不出。
2. UDP socket 未 `connect()` → dgram BIO 走 `sendmsg` 且 `msg_name==NULL` → `errno=89 EDESTADDRREQ`。
3. STUN keepalive 污染 DTLS 记录层（首字节 `0x00` 被当非法记录类型静默丢弃）→
   用 `MSG_PEEK|MSG_DONTWAIT` 分类，STUN 当场应答，DTLS 留给 OpenSSL。
4. **`SSL_export_keying_material` 的 label 长度写成 21（实际 19）** → 直接改 TLS 1.2 PRF seed
   → 导出的 KM 完全不同 → DTLS 握手毫无异常但每个 SRTP 包 HMAC 全失败。
   教训：离线自检服务端用了同一个 21，**对称的错误永远暴露不出来**；现加 `PASS4` 判别器。

## 真机出画（已验收）
```
[cgmedia] #1 SPS reports 1600x900
[cgmedia] #1 first video frame pushed (31446 bytes, 1600x900, key=1)
[pencg][I] [play] 出画 1600x900 audio=1
```
`/sys/kernel/debug/dri/0/summary` 出现 `Esmart1-win0: ACTIVE / zpos: 2 / format NV12 /
src rect[1600x900] / dst pos[107,487] rect[266x473]`，比例 0.5625 与 0.5623 一致 → 零拉伸。

## 之后再修掉的 4 个 bug
1. `GST_STATE_NULL_` 常量写成 0（应为 1，0 是 `VOID_PENDING`）→ `set_state` 失败，
   退出后 KMS 平面永远留着最后一帧。已改为 1 并加 `gst_element_get_state` 同步等待。
2. 页面级 `onHide`/`onUnload` 在小程序被切走时**不回调**（真机实测），
   → 改到 **App 级 `src/app.js` 的 `onHide`** 兜底拆管线 + 退机位，三路（点退出/切后台/直接退出）全部 `PLANE=GONE`。
3. 端游 offer 的 `region` 被写死成手游区 `gh*` → 全部返 `1288 请重新测速`（真实语义是
   「这些 region 没有该游戏的容器」）。改为按 `listMediaServers(gameCode)` 查真实 region。
4. `cm_udp_pump_media()` 把 ICMP 引发的 `ECONNREFUSED` 当致命错误 → 首帧后立刻
   `udp recv error, quitting media loop`。改为按瞬态处理（`rx_icmp++` + continue）。
   这条是「点了开始游戏永远黑屏」的直接原因。

## 又修掉的 2 个 bug（让画面第一次真正「动」起来）
5. **answer 接受了 SCTP DataChannel**：官方 `socketSend` 是
   `if(this.isDataChannel) try{return this.onDataChannelSend(c.data.cmd)}catch(u){}` ——
   优先走 DataChannel，失败才回落 WS。我方没有 SCTP 实现，一旦 answer 的 `m=application`
   段被接受，网关就把 input 定向到一条永远建不起来的通道。已在 `media.js` 的 `buildAnswer()`
   里把该段端口写 0 + `a=inactive`（RFC 8122/3264 的拒绝语义）。
   **效果：之前的会话全是冻结帧（`motion 0`），拒绝 DC 后原神启动动画正常播放（`motion 25807`）。**
6. `cm_rtp.c` 的 `emit_media_if_needed()` 要求 `audio_ok` 才发 `media` 事件，而网关常常
   只发视频不发音频 → JS 侧 `vizOn` 恒 false、遮罩不消失、点击被闸门丢弃。改为三态
   `media_ev_sent`（0 未发 / 1 已发但无音频 / 2 已带音频），视频到即发、音频晚到补发。

## 未完成（一处）
**输入（触摸 / 鼠标）仍未被远端消费。** 已排除：
- 坐标：`pageX/pageY` 实测是 stage 内坐标（`∈[0,473]×[0,266]`），线性同轴映射代数正确
  （两级 90° 旋转抵消）；「交叉轴+单轴取反」「双轴取反」都真机验证无效。
- 帧格式：与官方 `send_message`/`send_message_pc` 逐字段一致，`NX/NY` 与官方 `getPercentPos` 相同。
- 上行通道：解密网关回显的 `op=input` 密文能看到我方 `2`(move)/`8`(tap) 帧。
- 点击闸门：`onTS/onTM` 早期用 `vizOn` 当闸门，已改 `touchReady()`。
- DataChannel 抢占：已在 answer 里拒绝（见上）。

仍在查：input 是否需要先发 `connect(110)` / `25` 模式声明；`op` 名是否不是 `input`；
是否根本必须自研最小 SCTP；触摸 `id` 是否必须从 1 开始。

## 资源
- VIP：已开通 7 天不限时手游（`is_vip=true`），不再受每日 30 分钟限制
- `free_time_left`：0（VIP 不走这个额度）
- 机位：`games_playing = []`，无残留
- 后台进程：无残留

### 「回首页」也从 push 改成 pop（2026-11-07 22:05）

`play.vue` 退出游戏 / `queue.vue` 排队成功 / `mine.vue` 薄壳页的「首页」原来都是
`navTo('index')` —— 和返回键那个 bug 同源：新压一层 index，动画是 push、栈还会变深。

**唯一可行的写法是「先摘中间页、最后关自己」**：
```js
$falcon.closePageByName('detail');   // 夹在中间的先从栈里摘掉
$falcon.closePageByName('queue');
navBack('play');                     // 最后关自己，露出 index
```
反过来（先关自己，再 `setTimeout` 关 detail）**真机实测会停在 detail 上**：自己的 JS
上下文一没，定时器跟着没，后面的关不掉。这条踩过一次，别再写回去。

真机证据（`profiles/evidence/`）：
- `nav_exit_play_pop.png` —— 动画中帧：index 已占满左侧，play 页只剩最右边一条正在滑出 = pop
- `nav_exit_play_index.png` —— 落点就是 index（197105 B），不再停在 detail
- 日志：`[play] 退出完成 → index` → `[nav] closePageByName play`，`plane=0`

顺带说一句：`src/services/nav.js` 里那套 `requestHome/takeHomeHop/clearHomeHop`
（接力旗）**没有用上，且跨页面不共享模块状态** —— 保留只为说明「为什么这条路不通」，
真要接力得把状态落在 `$falcon` 能共享的地方。当前实现不依赖它。

### 版本号必须三处一致（2026-11-07 22:20，用户 m14526 报「release 是 v1.0.3、软件里写 1.0.0」）

版本号现在只有一个来源：`src/services/version.js` 的 `APP_VERSION`。
`src/components/mine-body.vue` 底部、关于卡片、关于 toast 全部读它。

改版本要同时动三处，否则又会出现「tag 和界面不一致」：
1. `src/services/version.js` 的 `APP_VERSION`
2. `package.json` 的 `"version"`（它还决定 AMR 的文件名 `<appid>.<a>_<b>_<c>.amr`）
3. git tag / release（如 `v1.0.3`）

**不要动** `src/services/net.js` 里的 `X-Ver: 1.0.0` —— 那是冒充官方 web 客户端的
请求头，不是我们的版本号。

真机证据：`profiles/evidence/mine_version_1_0_3.png`（关于卡片「版本 1.0.3」+
页脚「PenCG 1.0.3 · appid 8002026110200003」）。

顺带记两条设备操作：`/usr/bin/send_event touch slip x y` 单次滑不动 `<scroller>`，
要 `press` → 多次 `slip`（每次几十像素）→ `release`；只发一次 slip 会被当成点击。
