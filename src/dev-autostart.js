/* ★ 仅取证用（不要随生产构建发布）★
 *
 * 为什么要这个文件：本机 `/dev/input/event1` 被 weston/seatd 独占，
 * `send_event touch press/release` 只对 `scroller` 之外的元素生效
 * （左侧竖导航栏能点动，卡片/banner 全部点不动）。所以真机 UI 流程
 * 无法靠 adb 注入走通，只能让首页 boot 完之后自己跳一次。
 *
 * 用法：把 DEV_AUTOSTART 改成下面任一形态，重新构建 + 装机即可。
 * 取证结束必须把它改回 null（或在最终交付前整文件删除）。
 *
 *   { page: 'play',   delay: 3000, params: { code:'yssy', name:'原神-7.2版本',
 *                                           quality:'low', gameType:'mobile' } }
 *   { page: 'detail', delay: 2000, params: { code:'mrfzzmd', name:'明日方舟：终末地（官服）' } }
 *   { page: 'login',  delay: 2000, params: {} }
 *   { page: 'mine',   delay: 2000, params: {} }
 */
export const DEV_AUTOSTART = null;

/* ★ 仅取证用（bug2 真机验证专用，方案见 docs/BUGFIX-bug2-verify.md）★
 *
 * 为什么还需要这一个：`DEV_AUTOSTART` 只能把 app 推到某一页，**推不动登录表单**。
 * 登录页的手机号/密码框是自绘软键盘（div 网格），而本机 `/dev/input/event1` 被
 * weston 独占，`send_event` / `slip` 对这类元素基本无效 —— 也就是说「人工完成一次
 * 登录」在不改代码的前提下无法用 adb 复现。
 *
 * 所以给登录页留一个可选的自动填充 + 自动提交开关：把下面的 null 换成一个对象，
 * `PenCG/src/pages/login/login.vue` 的 mounted() 会在 boot 完成后自动填表并提交，
 * 之后走的是**和生产完全一样的** submit() → loginByPassword() → 600ms →
 * goBackToMine() 这条链路，不绕过任何被测逻辑。
 *
 * 用法（两处必须一起改）：
 *   DEV_AUTOSTART  = { page: 'login', delay: 2000, params: {} }
 *   DEV_AUTOLOGIN  = { phone: '<YOUR_PHONE>', pwd: '<YOUR_PASSWORD>', cc: '86', delay: 1200 }
 *
 * ★ 取证结束必须同时把两处改回 null，否则包里带着明文密码。★ */
export const DEV_AUTOLOGIN = null;

/* ★ 仅取证用（bug2 真机验证专用）★
 *
 * 让 app 启动后自己走一次生产 `logout()`（= `clearSession()` 清本页 G +
 * 删盘上 `pencg_session` + `bumpSessionVersion()`），把设备退回「未登录」初态。
 *
 * 为什么不用 adb 改 preferences.json：那个键是「JSON 字符串里再套一层 JSON」，
 * 外部改完 app 内存里还留着旧 session，必须再重启 miniapp 才干净；而走
 * `logout()` 是本进程内一致的，且它顺便把版本号推进一格 —— 正好也是要测的路径。
 *
 * 用法：DEV_LOGOUT = { delay: 3000 }（delay 要留够首页 boot 与本页 restore 完成）
 * ★ 取证结束必须改回 null。★ */
export const DEV_LOGOUT = null;

/* ★ 仅取证用（bug1 二分定位专用）★
 *
 * 上行 RTCP 对比实验：Chrome 在整段会话里 pliCount=0 / nackCount=0 /
 * remote-inbound-rtp 全程缺失（即**一个上行 RTCP 都不发**），而 PenCG
 * 1Hz RR + 2s PLI + 按缺口 NACK。
 *
 * 置 true 时 `src/services/media.js` 的 cfgObj 会带 `noRtcp:1`，native
 * 主循环跳过 cm_kf_tick / cm_nack_tick / cm_rtcp_tick（ICE consent 仍发）。
 *
 * ★ 取证结束必须改回 false。★ */
export const DEV_NO_RTCP = false;

/* ---- DTLS 指纹 A/B 取证开关（2026-11-14）----
 * 背景见 docs/DTLS-CLIENTHELLO-FINGERPRINT.md：我们的 ClientHello 是
 * 28 个 OpenSSL 默认序套件（含静态 RSA）、无 ALPN，与 Chrome 四层全不同。
 * 置 true 时 cfgObj 带 chromeFp:1，native 把 cipher_list 换成 Chrome 序
 * （14 个现代套件，无静态 RSA）。use_srtp/扩展集合不动。
 *
 * ★ 取证结束必须改回 false。★ */
export const DEV_CHROME_FP = false;

/* ---- 交互续命 A/B 取证开关（2026-11-14）----
 * 背景：六局实测「每媒体会话 ~250s 准点停推」，10s 一次的 keepAlive 心跳
 * （cmd "0 <ts>"）不能重置计时，DTLS 指纹也已排除。剩最后一个假设：
 * 服务端的踢出计时器可能被「真实玩家输入」重置（官方 setKeepAliveConfig
 * 的 kickout 语义 = 「太久没回来」）。本开关在 play 页 live 态下每
 * DEV_AUTOTAP_MS 毫秒自动注入一次 1px 微动触摸（cmd 1/2 down-up，
 * 落点 1,1 → 2,2，不影响操作），若定时器被输入重置，sess 将跨过 260s
 * 继续增长；若仍 250s 被掐，则「kickout 只认官方 SDK keepPlaying 帧」。
 *
 * ★ 取证结束必须改回 false。★ */
export const DEV_AUTOTAP = false;

/* ★ 仅取证用（屏幕适配测试专用）★
 *
 * 在 onLaunch 里、bootProfile() 之前把画布宽度强制覆盖成这个值，
 * 用来在**同一台 X6 Pro 上**模拟另一块屏（例如 936 宽的 X7 Pro、720 宽的窄屏），
 * 看布局会不会溢出/错位。0 = 关闭（正常探测）。
 *
 * 走的是 device_profile 已有的 custom 通道（$falcon.env.custom.pencgDesignWidth），
 * 所以和手动在真机上 $set 的效果完全一致，只是省得重新打包。
 *
 * ★ 取证/验收结束必须改回 0。★ */
export const DEV_DESIGN_WIDTH = 0;
