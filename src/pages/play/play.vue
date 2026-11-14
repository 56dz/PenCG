<template>
  <!-- 播放态三栏：左 243 + 中 473（KMS 视频挖孔）+ 右 244 = 960，高度统一 266。
       KMS 平面按绝对坐标挖孔，所以 .stage 必须紧接在 .p2-left 后面且宽度分毫不差；
       左右两栏都是不透明底色，.stage 自身保持透明把画面透出来。 -->
  <div class="page page-live">
    <!-- 左栏 243x266：品牌 + 游戏名（两行）· 状态调试条 · 地区/画质 · 免费时长 · 底部出画统计 -->
    <div class="p2-left">
      <div class="p2-lhead">
        <image class="p2-logo" :src="logo" :width="26" :height="26"></image>
        <div class="p2-namebox">
          <text class="p2-name">{{ nameL1 }}</text>
          <text v-if="nameL2" class="p2-name">{{ nameL2 }}</text>
        </div>
      </div>
      <div class="p2-lstrip"><text class="p2-lstrip-text">{{ stripClip }}</text></div>
      <div class="p2-lrow">
        <div class="chip"><text class="chip-text">{{ regionText }}</text></div>
        <div class="chip p2-chipgap"><text class="chip-text">{{ qualityLabel }}</text></div>
      </div>
      <!-- ★ 这里原来显示"本局剩余 N 分钟"是错的：tickets 的 time_left 是 ticket
           有效期（约 25 秒），不是单局时长限制，服务端根本不下发单局限制。
           改成显示"已玩时长"，既真实又对玩家有用。 -->
      <div class="p2-ltime">
        <text class="p2-ltime-label">已玩时长</text>
        <text class="p2-ltime-value">{{ playedText }}</text>
      </div>
      <div class="p2-lmsg">
        <text :class="errText ? 'p2-lmsg-main-err' : 'p2-lmsg-main'">{{ statusMainClip }}</text>
        <text v-if="statusSub1" class="p2-lmsg-sub">{{ statusSub1 }}</text>
        <text v-if="statusSub2" class="p2-lmsg-sub">{{ statusSub2 }}</text>
      </div>
      <div class="p2-lstat"><text class="p2-lstat-text">{{ dbgText }}</text></div>
    </div>

    <!-- 中栏 473x266：KMS 平面挖孔位。自身必须透明；未出画时 .mask 不透明盖满整块。 -->
    <div class="stage" @touchstart="onTS" @touchmove="onTM" @touchend="onTE" @touchcancel="onTC">
      <div v-if="!vizOn" class="mask">
        <div class="p2-mask-box">
          <text class="p2-mask-title">{{ maskTitle }}</text>
          <text v-if="dots" class="p2-mask-dots">{{ dots }}</text>
          <text class="p2-mask-sub">{{ maskSub }}</text>
          <text v-if="errText" class="p2-mask-err">{{ errText }}</text>
        </div>
      </div>
    </div>

    <!-- 右栏 244x266：画质四档（两行）· 码率统计 · 重连/重新申请/退出 · 底部说明 -->
    <div class="p2-right">
      <!-- ★ 数字键盘（云游戏内输入手机号/验证码）。
           放在右栏而不是整屏，是因为 KMS 视频平面的挖孔矩形固定为中间 473 宽
           （VIDEO.x=243, VIDEO.w=473），右栏 244 完全在平面之外 —— 键盘自己
           就是一块普通 UI，不需要把视频平面藏起来，也就绕开了 1x1 棘轮、
           READY 重置、等 IDR 这一整串麻烦，玩家还能边看画面边按。
           远端仍是官方那套：每位 sendText(d) 走 cmd 5，退格 sendKey(1) 走
           cmd 6，确认 sendKey(2) 走 cmd 6（官方 run.js onKeyup 的 8→1、13→2）。 -->
      <div v-if="kbShow" class="p2-kb">
        <div class="p2-kbhead">
          <text class="p2-kbtitle">{{ kbTitle }}</text>
          <div class="p2-kbsys" @click="kbSystem"><text class="p2-kbsys-text">系统键盘</text></div>
        </div>
        <div class="p2-kbdisp">
          <text class="p2-kbval">{{ kbNums || kbTip }}</text>
        </div>
        <div class="p2-kbrow">
          <div class="p2-kbkey" @click="kbDigit('1')"><text class="p2-kbnum">1</text></div>
          <div class="p2-kbkey" @click="kbDigit('2')"><text class="p2-kbnum">2</text></div>
          <div class="p2-kbkey" @click="kbDigit('3')"><text class="p2-kbnum">3</text></div>
        </div>
        <div class="p2-kbrow">
          <div class="p2-kbkey" @click="kbDigit('4')"><text class="p2-kbnum">4</text></div>
          <div class="p2-kbkey" @click="kbDigit('5')"><text class="p2-kbnum">5</text></div>
          <div class="p2-kbkey" @click="kbDigit('6')"><text class="p2-kbnum">6</text></div>
        </div>
        <div class="p2-kbrow">
          <div class="p2-kbkey" @click="kbDigit('7')"><text class="p2-kbnum">7</text></div>
          <div class="p2-kbkey" @click="kbDigit('8')"><text class="p2-kbnum">8</text></div>
          <div class="p2-kbkey" @click="kbDigit('9')"><text class="p2-kbnum">9</text></div>
        </div>
        <div class="p2-kbrow">
          <div class="p2-kbkey p2-kbdel" @click="kbDel"><text class="p2-kbnum">删</text></div>
          <div class="p2-kbkey" @click="kbDigit('0')"><text class="p2-kbnum">0</text></div>
          <div class="p2-kbkey p2-kbok" @click="kbOk"><text class="p2-kbok-text">确定</text></div>
        </div>
      </div>

      <!-- 退出确认：KMS 盖住中间视频区，弹层只能落在右栏 244 宽里 -->
      <div v-else-if="confirmExit" class="p2-confirm">
        <text class="p2-ctitle">确认退出游戏？</text>
        <text class="p2-csub">退出后立即释放云端机位</text>
        <text class="p2-csub">剩余免费时长不受影响</text>
        <text class="p2-cplayed">本局已玩 {{ playedText }}</text>
        <div class="p2-cbtn p2-act-main" @click="cancelExit"><text class="p2-act-text-on">继续游戏</text></div>
        <div class="p2-cbtn p2-act-danger" @click="doExit"><text class="p2-act-text-on">确认退出</text></div>
      </div>

      <div v-else class="p2-rbody">
        <!-- 画质只剩「流畅」一档（用户 m12491 授权移除其余三档：
             标清/高清/蓝光在本机 mppvideodec 上全是马赛克或纯绿）。
             qualityBottom 为空时不再渲染第二行，避免留出一条 44px 的空白。 -->
        <div class="p2-qrow">
          <div v-for="q in qualityTop" :key="q.id" :class="q.cls" @click="pickQuality(q.id)">
            <text :class="q.tcls">{{ q.label }}</text>
          </div>
        </div>
        <div v-if="qualityBottom.length" class="p2-qrow p2-qrow2">
          <div v-for="q in qualityBottom" :key="q.id" :class="q.cls" @click="pickQuality(q.id)">
            <text :class="q.tcls">{{ q.label }}</text>
          </div>
        </div>
        <!-- 画质提示恒定占位（无提示时为空串），避免按钮上下跳动 -->
        <div class="p2-rhint"><text class="p2-rhint-text">{{ qualityHint }}</text></div>

        <!-- 220px 内容宽放不下一整行 stats（12.3 Mbps · 60fps · 丢包 0 · 1280x720），拆两行 -->
        <div class="p2-rstat">
          <text class="p2-rstat-main">{{ statsMain }}</text>
          <text class="p2-rstat-dim">{{ statsSub }}</text>
        </div>
        <div class="p2-rfill"></div>

        <div v-if="stage === 'nologin'" class="p2-racts">
          <div class="p2-act p2-gap p2-act-main" @click="goLogin"><text class="p2-act-text-on">去登录</text></div>
          <div class="p2-act p2-gap" @click="goIndex"><text class="p2-act-text">返回首页</text></div>
          <div class="p2-act p2-act-lock" @click="askExit"><text class="p2-act-text">离开</text></div>
        </div>
        <div v-else class="p2-racts">
          <div :class="reconnectCls" @click="onReconnect"><text :class="reconnectTcls">重连</text></div>
          <div :class="reapplyCls" @click="onReapply"><text class="p2-act-text">重新申请</text></div>
          <div class="p2-act p2-act-danger" @click="askExit"><text class="p2-act-text-on">退出</text></div>
        </div>

        <div class="p2-rfoot"><text class="p2-rfoot-text">{{ footText }}</text></div>
      </div>
    </div>
  </div>
</template>
<script>
import { navTo, navBack, peekParams, requestHome } from '../../services/nav.js';
import { restoreSession, isLoggedIn, currentSession } from '../../services/auth.js';
import { applyTicket, stopGame } from '../../services/session.js';
import { startPlay, stopPlay, setQuality, setRect, mediaState, setEventHandler, setVideoVisible, setInputBusy, requestKeyFrame, requestKeyFrameBurst } from '../../services/media.js';
import { down, move, up, tap, cancel, setInputMode, declarePlatform, keepAlive, mouseMove, mouseDown, mouseUp, sendText, sendKey } from '../../services/input.js';
import { ImSession } from '../../services/im.js';
import { initScreen, keepAliveStart, keepAliveStop, getScreenState, screenStateName, isScreenNotOn, KEEP_INTERVAL_MS } from '../../services/screen.js';
import { loadSettings, saveSettings } from '../../services/settings.js';
import { DEV_AUTOTAP } from '../../dev-autostart.js';
import { recordPlay, touchPlay } from '../../services/history.js';
import { QUALITIES, QUALITY_LABEL, normQuality } from '../../services/const.js';
import { logInfo, logWarn, logError } from '../../services/log.js';
import { stageVideoRect } from '../../services/device_profile.js';
import logoImg from '../../assets/logo.png';

// KMS 硬件平面是盖在 UI 之上的 overlay：视频矩形与 UI 矩形必须完全分离。
// 播放页三栏按画布宽度切分：左 25.3125vw + 中 49.2709vw（视频）+ 右 25.4167vw，
// 中栏就是挖孔，必须与模板里 .p2-left 的宽度分毫不差，否则挖孔错位。
//
// 这里不再写死 960 画布下的 243/473/266 —— 三栏比例固定在 device_profile.js 的
// STAGE_LEFT_RATIO / STAGE_VIDEO_RATIO，由当前画布宽度反算像素矩形。
// 模块加载时 onLaunch 已经跑过 bootProfile()，所以拿到的就是真实画布；
// 万一没有（例如单独 import 做静态检查），stageVideoRect() 会退回 X6 基线
// 960x266，即与历史行为完全一致，不会抛错。
var VIDEO = stageVideoRect();
var VW = 1280;
var VH = 720;
var MAX_TOUCH = 5;

/* ★ 退后台宽限期（方案 B，纯时长判据）
 *
 * 为什么不再拿 getScreenState() 当触发条件：
 * 那一层只有「0 = 亮屏」被真机证实（docs/SCREEN-STATE-FINDINGS.md §3/§7.1），
 * 1/3/4 的语义来自 hal-screen 的另一套编号，两层不能互证。把它当"要不要拆会话"
 * 的总闸，会出现两种坏结果：一是编号猜错时守卫恒假（系统息屏被当成真退后台 →
 * stopGame 释放机位，正是 bug1 的观感）；二是读状态失败时把"读不到"误判成熄屏。
 * 所以改成只看时间：onHide 只摘视频平面（廉价可逆），60 秒内回来就无缝继续，
 * 超时才真正拆管线释放机位。
 *
 * 60 秒这个值的依据：系统的 [Idle] 是 270s 调暗 / 300s 断电 / 900s 杀进程，
 * 都比 60s 远；而用户"按一下电源键看看时间再按回来"这种动作通常在 10 秒量级。
 * 取 60s 是为了让「短暂切走」和「真的不玩了」有明显区分度，同时多烧的免费
 * 时长可控（一局 10 分钟里最多多烧 1 分钟）。 */
var HIDE_GRACE_MS = 60000;

/* 熄屏加速：屏幕确实非亮屏时，用户多半不会马上回来（按电源键回来要解锁），
 * 宽限期缩到 10 秒。注意这是"加速条件"不是"触发条件"：即使这一路判错，
 * 最坏也只是少等 50 秒就释放机位，不会误拆正在看的画面。
 * 判据走 isScreenNotOn()：只依赖"0 = 亮屏"，对"3 到底存不存在"不敏感。 */
var HIDE_GRACE_DARK_MS = 10000;

/* ===================== STALLGUARD（bug1 第二阶段） =====================
 *
 * 真机实测（2026-11-13，X6 Pro，三次独立复现）：起播后 4m19s / 4m18s / 4m18s，
 * 网关**静默停止推送 RTP**——音视频同时停，`rx`（native stats.rtp_rx）冻在
 * 一个固定值（如 18874）、`fps` 从 29~30 掉到 0；而这一时刻：
 *   screen=0(ON)、`keep #N` 每 20s 照常、ws=open、phase=playing、
 *   我们 10s 一次的心跳 `op=input cmd="0 <ts>"` 被网关原样回显，
 *   网关**不发** error / close / 任何提示 op（WS op 统计：2751 input / 7 error
 *   / 4 result / 4 offer，没有 keep/kick/not_playing 这类东西）、
 *   native 侧同时刻只剩 `PLI sent (26 bytes, srtcp)`，pt histogram 也冻结。
 * ⇒ **会话没死，服务端单方面停推。** 官方 SDK 有 RtcKeepPlaying / RtcKeepaliveCfg
 * 这套「告诉网关我还在玩」的机制，我们从没实现。最初按 `kickoutDuration`
 * 默认 300s、减去起播前容器冷启动 ~42s 推出「4m18s」，**这个算法 2026-11-14
 * 已被更精确的实测推翻**（见下面 ACTIVE_RENEW_MS 注释的三局 247/253/248s）：
 * 计时器不是从 ticket 起算，而是**每个 WS 会话（每次 wsOpen）各自约 250 秒**。
 * 这也解释了为什么 L2 重连之后又能再活四分钟 —— 新会话重新计时。
 * 根因在服务端，客户端改不掉它，**只能检测 + 自愈**。
 *
 * 判据为什么看 rx 而不是 fps/decoded：
 *   - fps 与 decoded 是「解码输出」，参考帧缺失时会被 native 丢掉，波动大；
 *   - rx（stats.rtp_rx）是「通过 SSRC 校验的 RTP 包数」，只要网关还在推就单调涨，
 *     停推就一点不动，是最干净的判据。
 *   注意不能用 rxPkts：那是 socket 上的 datagram 总数，含 STUN/DTLS，
 *   停推之后 ICE 保活的 STUN 照样在涨（真机验证过），拿它判停滞会永远判不出来。
 *
 * 8 秒这个阈值（2026-11-14 由 20s 收紧）：停推是**网关单方面停**，
 * 判出来之后只能靠 L2 换会话救，所以「发现得越早，黑屏越短」。
 * 真机实测正常播放时 rx 每 20s 稳定 +1600（≈80 包/秒，fps 29~30），
 * 一次瞬间抖动最多让计数顿一两秒，8 秒仍留了 4 倍余量，误触风险很低。
 * 收紧后的最坏恢复路径 ≈ 8s 发现 + 3s L1 等待 + L2 重连出画 ≈ 15s，
 * 原来是 20s + 5s + 重连 ≈ 28s。
 * 取 5s 以下会有风险：起播瞬间容器还在出静帧、或一次换档重建都可能让
 * rx 停 5~6 秒，会被误判成停推而白白换掉一个好会话。 */
var STALL_RX_MS = 8000;
/* L1（补关键帧）之后等多久判定「还是没救回来」。PLI 是向网关要 IDR，
 * 网关处理 + 一帧编码回来通常 <1s，留 3s 是给丢包重发的余量。
 * 3s（2026-11-14 由 5s 收紧）：停推场景下网关根本不看 PLI，L1 注定无效，
 * 早点判定失败能早 2 秒升到 L2 换会话。 */
var STALL_L1_WAIT_MS = 3000;
/* 一次故障最多自动恢复 3 次（L1 补帧 / L2 复用网关重连 / L3 重新申请机位）。
 * 上限存在的意义：这个故障每次都在 4 分钟出头出现，若网关/账号侧真有硬限制，
 * 无限重连只会把用户剩下的免费时长（和机位）在几秒内烧光，还不如老实告诉他
 * 手动点「重连」。 */
var STALL_MAX_TRIES = 3;

/* ============ 第三条防线：会话寿命预警（主动续命，不让用户看到卡顿）============
 *
 * 真机三局实测（2026-11-14 01:12–01:30，原神 yssy，同一台 X6 Pro）：
 *   A  01:12:52 wsOpen → 01:16:59 停推 = 247s
 *   B  01:17:24 wsOpen → 01:21:37 停推 = 253s   （L2 重连后的新会话）
 *   D  01:25:09 wsOpen → 01:29:17 停推 = 248s   （手动重新申请机位后的新会话）
 * 均值 249.3s，离散度 ±3s —— 这不是"从 ticket 算 300s"，而是
 * **每一个 WS 会话（每次 wsOpen 重新计时）都只有约 250 秒寿命**。
 * 历史上"4 分 18 秒"之所以看起来像固定值，是因为起播冷启动恰好也在
 * 40~60 秒量级，两个数加起来的抖动被掩盖了。
 *
 * 结论：最初的想法是「与其等它停了、再靠 STALLGUARD 补救（用户会看到十几秒
 * 静止画面），不如在寿命到期**之前**主动换一个新会话」——230s 留 20s 余量、
 * 换会话只要 2~4 秒、画面由新 IDR 重建，理论上几乎无感。
 *
 * **但这条路真机走不通**（见下方 ★★ 默认关闭 ★★）：换会话必然换机位、
 * 必然重拉对局容器，用户看到的是游戏从头开始。所以现在默认关闭。
 * 下面这些 230s/245s 的推导保留，是为了以后拿到「不重开容器」的续命手段时
 * 能直接复用。
 *
 * ★ 换会话必须"换机位"，不能"复用旧网关"（2026-11-14 01:51 真机教训）：
 *   第一版 renewNow 调的是 onReconnect()（复用同一个 gatewayUrl 重新连），
 *   230s 准点触发、日志全对，但网关 5 秒后就 op=close 把新会话关掉 ——
 *   同一机位短时间内第二次连会被当成重复连接踢掉。结果用户看到
 *   "230s 黑 3 秒 → 被踢 → 再等 40 秒靠 L3 重新申请才恢复"，比不续命更差
 *   （不续命是 250s 停推后约 25 秒恢复）。
 *   现在改为 onReapply()（重新申请机位，实测会从 -06 换到 -02），代价 ~7 秒，
 *   换来的是一条不会撞上"重复连接"的干净会话。
 *
 * ★★ 默认关闭（=0），原因：换机位会让对局重启（2026-11-14 02:09~02:13 真机）★★
 *   把阈值临时降到 60s 连做三次，每次都准点触发，但每次重新申请机位都会
 *   **重新拉起一个对局容器**：日志里 `出画 1280x720 audio=0` 会重新打一次，
 *   玩家看到的是「游戏从头开始」，不是无缝切换。三次实测黑屏 9s / 8s / 7s，
 *   恢复音频 11s / 19s / 18s，且游戏进度全部丢失。
 *   ⇒ 为了规避一个 250s 后才发生的停推，每 230s 打断一次对局，是不可接受的取舍。
 *   所以策略改为：**不主动续命，靠收紧后的 STALLGUARD（8s + 3s）兜底**，
 *   单次代价从「游戏重开」降到「黑屏约 15 秒、进度不丢」。
 *   实现与十二条门全部保留，把这里改成 230000 即可随时重新开启。
 *   若以后确认网关对频繁换机位没有惩罚、且能拿到「不重开容器」的续命手段
 *   （例如 RtcKeepPlaying + gameToken），第一个要改的就是这个值。 */
var ACTIVE_RENEW_MS = 0;
/* 主动重连后至少等这么久才允许再次主动重连，防止「换完会话立刻又到期」
 * 导致的连环重连把机位烧光。取 60s 是因为正常换会话 2~4 秒就出画了，
 * 若 60s 还没恢复，说明网关侧有问题，该交给 STALLGUARD 处理而不是再换一次。 */
var ACTIVE_RENEW_GUARD_MS = 60000;

/* ================= 第二条判据：弱网冻结（rx 在涨但画面不动） =================
 *
 * 真机现场（2026-11-13 16:25–16:42，原神，高铁 Wi-Fi，丢包 5~8%）：
 *   [play] probe stage=live v=1 screen=0(ON) phase=playing ws=open
 *          rx=73867 fps=0 kf=6 silent=3s tries=0 keep=on
 *   native: still no IDR (rx=74418 kfReq=468 gateDrop=15511 auDrop=401)
 *
 * rx 从 194 一路涨到 73867，收包停滞判据（看 rx 冻结 STALL_RX_MS）**永远不会触发**；
 * 而 fps 自 16:30:30 起连续 12 分钟为 0，tries 一直是 0 —— 一次自愈都没发起。
 * 原因是：弱网下网关照推、包照收，但每个 AU 都被丢包打碎，native 的
 * 「首个完整 IDR 之前丢 P 帧」闸门把 15000+ 个 P 帧全丢了（gateDrop 持续增长）。
 * 所以「rx 在涨」≠「画面在动」，必须另立一条判据。
 *
 * 判据的**主时钟是 fps 连续为 0**，不是 gateDrop 增长：
 *   `gateDrop` 只在 native「首个完整 IDR 之前丢 P 帧」那道闸门里自增
 *   （native/csrc/cgmedia/cm_rtp.c 的 gate 分支）。而 native 现在有 IDR 饥饿兜底
 *   （IDR_STARVE_RELAX_MS = 4000）：4 秒拿不到 IDR 就把 have_kf 置 1、放行 P 帧，
 *   **从此 gateDrop 再也不涨**。若把判据写成「gateDrop 连续增长 12s」，那么只要
 *   native 兜底生效（几乎必然），窗口就永远攒不满 —— 判据成了永不触发的死代码。
 *   而放行的 P 帧没有参考帧，画面照样是死的（fps 仍为 0），这正是我们要抓的场景。
 * 所以改成：
 *   ① fps 连续为 0 且采样数 ≥ FREEZE_MIN_SAMPLES（防 2s 窗口抖动误伤）；
 *   ② 期间「(gateDrop+auDrop) 在涨」或「rx 在涨」——证明包还在进、是我们这边解不出来，
 *      而不是网关停推（后者归第一条判据管）。
 *   auDrop 在 native 放行 P 帧之后仍会随坏 AU 增长，正好补上 gateDrop 停涨的空档。
 *
 * 12s 的理由：必须比 native 的 IDR 饥饿兜底（4s 放行 P 帧）**慢**，让 native 先自救；
 * native 一旦放行且画面真的恢复，fps 回到非 0，本判据自行清零。留 12s 也给弱网
 * IDR 重传留够时间。9s 的 L1 复查同理：弱网下一发 PLI 换回完整 IDR 往往要重传，
 * 比收包停滞的 5s 更慢才算合理。 */
var FREEZE_GATE_MS = 12000;
var FREEZE_L1_WAIT_MS = 9000;
var FREEZE_MIN_SAMPLES = 3;

// applyTicket 内部已重试 10 次，页面拿不到真实次数，按「约 3 秒一轮」估算展示。
var ERR_TEXT = {
  1209: '暂无空闲机位，正在为你重试',
  1250: '当前网络质量太差，无法流畅云游戏',
  1251: '该游戏需要会员权益，请先开通会员',
  1269: '请先选择可用的云机房区域',
  1279: '云端电脑异常，请重新申请机位',
  1288: '网络测速未通过，正在重新测速',
  1506: '恢复上局失败，请重新申请机位',
  33803: '机位被其他设备占用，正在重试'
};

function clip(s, n) {
  var v = String(s === undefined || s === null ? '' : s);
  if (v.length <= n) return v;
  return v.slice(0, n) + '…';
}

function blocked(v) {
  if (v === false || v === 0) return true;
  if (typeof v === 'string' && (v === '0' || v === '')) return true;
  return false;
}

function pad2(n) {
  var v = Math.max(0, Math.floor(n));
  return v < 10 ? '0' + v : String(v);
}

export default {
  data: function () {
    return {
      code: '',
      name: '',
      icon: '',
      logo: logoImg,     // 左栏顶部软件图标
      gameType: 'mobile',
      quality: 'low',
      gatewayUrl: '',
      stage: 'boot',
      tip: '',
      errText: '',
      errCode: 0,
      mediaErr: '',
      vizOn: false,
      dots: '',
      clock: 0,
      ticketAt: 0,
      connectAt: 0,
      startAt: 0,
      elapsed: 0,
      slowHint: false,
      ticketTtl: 0,   /* ticket 有效期秒数（≈25），只用于连接预算，不是游玩时长 */
      regionName: '',
      region: '',
      blurayLocked: false,
      qualityHint: '',
      phase: 'idle',
      wsState: 'idle',
      iceState: '',
      dtlsState: '',
      rx: 0,
      lost: 0,
      br: 0,
      fps: 0,
      vw: VW,
      vh: VH,
      kf: 0,
      pli: 0,
      statAt: 0,
      hbAt: 0,   /* 上次发 WS 心跳 "0 <ms>" 的时间 */
      tapAt: 0,  /* 上次 autotap 注入触摸的时间（DEV_AUTOTAP 取证用） */
      tapCount: 0, /* autotap 已注入次数（日志显示用） */
      probeAt: 0, /* 上次打 [play] probe 存活探针的时间（20s 一次，bug1 排查用） */
      /* ---- STALLGUARD：网关停推检测 + 三级自愈（见文件头 STALL_RX_MS 注释） ---- */
      stallLastRx: 0,  /* 上次看到的 rx（native stats.rtp_rx）。变大就刷新 stallSince */
      /* ---- 第三条防线：会话寿命预警（见文件头 ACTIVE_RENEW_MS 注释） ----
       * wsOpenAt 在**每一次** wsOpen 事件里刷新，而不是只在首播时设一次：
       * 主动重连连出来的新会话，同样有 250s 寿命，同样需要在 230s 时再换一次。
       * renewAt 记录本次主动重连的时刻，配合 ACTIVE_RENEW_GUARD_MS 防连环重连。 */
      wsOpenAt: 0,     /* 本次 WS 会话建立的时刻（Date.now()）。0=还没连上 */
      renewAt: 0,      /* 上一次「主动续命」重连的时刻。0=本局还没主动重连过 */
      renewCount: 0,   /* 本局主动续命次数（仅用于日志与探针） */
      stallSince: 0,   /* rx 最后一次增长的时刻。0=还没收到过任何包（尚未进入判据） */
      stallTries: 0,   /* 本次故障已用掉的自动恢复次数（L1/L2/L3 各加一） */
      /* 这条故障是「哪条判据」发起的：'rx'=收包停滞 / 'freeze'=弱网冻结 / ''=无。
         必须分开记，否则会互相拆台：弱网冻结时 rx 一直在涨，收包判据的
         「rx 变大=已恢复」会在每次 tick 把 stallTries 清零，「L1→L2→L3」
         的升级链永远走不到第二级（真机上就会表现为无限次 L1 补帧）。 */
      stallOwner: '',
      stallAt: 0,      /* 本次停滞的判定时刻，用于日志里算「已停 N 秒」 */
      /* native 报告「上屏看门狗重建用尽」(-7) 的次数。第 1 次走 L2，
         第 2 次起走 L3 换机位 —— 理由见 onMedia 里 -7 分支的注释。 */
      nativeFrozen: 0,
      stallL1Timer: 0, /* L1 之后等 5s 复查的定时器句柄（必须能清） */
      /* ---- 第二条判据：弱网冻结（rx 在涨但画面不动）。计时器与 stalls 完全独立，
         否则两条判据会互相清掉对方的计时基准（见文件头 FREEZE_GATE_MS 注释） ---- */
      freezeDropBase: -1,     /* fps 开始连续为 0 时的 (gateDrop+auDrop)。-1=未建立 */
      freezeRxBase: 0,        /* fps 开始连续为 0 时的 rx，用于判「包还在进」 */
      freezeFpsZeroSince: 0,  /* fps 从何时起连续为 0。0=当前不是全零 */
      freezeFpsZeroCnt: 0,    /* 连续 fps==0 的 stat 采样数（防毛刺，见 FREEZE_MIN_SAMPLES） */
      freezeLastSeq: 0,       /* 上次判定时看到的 statSeq，保证一个采样只判一次 */
      freezeZeroAt: 0,        /* 本次冻结的判定时刻，日志里算「已冻 N 秒」 */
      freezeL1Timer: 0,       /* 冻结 L1 之后等 9s 复查的定时器句柄（必须能清） */
      statSeq: 0,             /* stat 采样序号。checkFreeze 用它保证「按采样」而不是「按 tick」判定 */
      gateDrop: 0,            /* native stats.vid_gate_drop 的最新值（只用于显示/判据） */
      auDrop: 0,              /* native stats.vid_au_drop 的最新值 */
      kfReq: 0,               /* native stats.vid_kf_req 的最新值（与 kf 对比=网关理不理 PLI） */
      kfTimers: [],   /* 换画质后补发 PLI 的定时器（必须能清，见 clearQualityKf） */
      confirmExit: false,
      pcDownAt: 0,   /* 端游按下时刻，用于区分点击与长按拖动 */
      kbOpen: false, /* 远端弹出了输入框、正在等用户用数字键盘输入 */
      videoHidden: false, /* KMS 视频平面是否已被缩成 1x1（弹系统键盘期间 / 退后台宽限期内） */
      hiddenSince: 0,   /* 进入 hidden 的时刻（Date.now()）。宽限期的计时起点，0=当前不在后台 */
      hideGraceTimer: 0, /* 后台宽限期定时器句柄（必须能清，见 clearHideGrace） */
      /* ---- 自绘数字键盘（右栏 244x266，在 KMS 平面之外，不用藏视频） ---- */
      kbShow: false,   /* 键盘是否可见 */
      kbNums: '',      /* 已输入的数字串（用于回显与拼接） */
      kbTip: '',       /* 底部提示（例如「已发送 11 位」） */
      kbTitle: '输入数字', /* 顶部标题：手机号 / 验证码 */
      busy: false,
      /* ---- 页面销毁归因（见 docs/BUGFIX-play-page-teardown.md） ----
       * 真机上出现过「对局健康进行中，页面自己 beforeDestroy，日志里看不出是谁
       * 干的」：宿主重复 start 会重载 index 并把栈上的 play 一起摘掉，这条路径
       * 不经过 onHide/onStop，也没有任何 error。下面两个字段让 cleanup 的日志
       * 能一眼区分「我们自己退的」和「被宿主干掉的」。 */
      selfClosedBy: '', /* doExit/finish/fail/bgTeardown/queue 置位：这条销毁是我们主动发起的 */
      destroyedAt: 0    /* cleanup 执行时刻，仅用于同一行日志里算总时长 */
    };
  },
  computed: {
    // 左栏名字区 181px / 13px 字 → 每行最多 12 个全角字；两行放不下就省略号。
    // Falcon 的 <text> 不会自动换行，中文长文本也不自己裁切，所以必须 JS 拆行 + 外层定高。
    nameL1: function () {
      var v = String(this.name || this.code || '云游戏');
      return v.length <= 12 ? v : v.slice(0, 12);
    },
    nameL2: function () {
      var v = String(this.name || this.code || '云游戏');
      if (v.length <= 12) return '';
      return clip(v.slice(12), 12);
    },
    attemptNo: function () {
      return Math.max(1, Math.min(10, Math.floor(this.elapsed / 3) + 1));
    },
    footText: function () {
      var ss = currentSession();
      if (ss && ss.isVip) return '退出立即释放机位 · 会员畅玩中';
      var f = Number(ss && ss.freeTimeLeft) || 0;
      if (f > 0) return '退出立即释放机位 · 账号剩余 ' + Math.floor(f / 60) + ' 分钟';
      return '退出立即释放机位 · 账号免费时长已用尽';
    },
    regionText: function () {
      var r = this.regionName || this.region || '';
      return r ? clip(r, 6) : '自动机房';
    },
    qualityLabel: function () {
      return QUALITY_LABEL[this.quality] || this.quality;
    },
    tagsThird: function () {
      if (this.vizOn && this.statAt) return this.vtagText;
      return this.qualityLabel;
    },
    playedText: function () {
      if (!this.startAt) return '0 秒';
      var sec = Math.max(0, Math.floor((this.clock - this.startAt) / 1000));
      var m = Math.floor(sec / 60);
      var s = sec % 60;
      if (m <= 0) return s + ' 秒';
      return m + ' 分 ' + pad2(s) + ' 秒';
    },
    stageChip: function () {
      var map = {
        boot: '准备中',
        ticket: '申请机位',
        queued: '排队中',
        connecting: '连接中',
        waiting: '等待画面',
        live: '游玩中',
        lost: '已断开',
        fail: '已失败',
        end: '已结束',
        nologin: '未登录'
      };
      return map[this.stage] || '准备中';
    },
    maskTitle: function () {
      var map = {
        boot: '正在准备云游戏…',
        ticket: '正在申请云端机位…',
        queued: '机位繁忙，正在进入排队…',
        connecting: '正在连接游戏网关…',
        waiting: '正在连接云游戏…',
        live: '',
        lost: '连接已断开',
        fail: '未能进入游戏',
        end: '本局已结束',
        nologin: '请先登录网易云游戏'
      };
      var v = map[this.stage];
      return v === undefined ? '正在准备…' : v;
    },
    maskSub: function () {
      var s = this.stage;
      if (s === 'boot') return '校验登录状态与本地设置';
      if (s === 'ticket') return '第 ' + this.attemptNo + ' 次尝试 · 已等待 ' + this.elapsed + ' 秒';
      if (s === 'queued') return '即将进入排队页，请稍候';
      if (s === 'connecting') return '网关已下发，正在建立安全连接';
      if (s === 'waiting') return this.tip || '等待服务端下发画面参数';
      if (s === 'lost') return this.errText || '与游戏服务器断开';
      if (s === 'fail') return this.errText || '请点「重连」或「重新申请」';
      if (s === 'end') return this.errText || '云机位已释放，可重新申请';
      if (s === 'nologin') return '登录后才能申请云端机位';
      return '';
    },
    stripText: function () {
      var viz = this.vizOn ? ' · 已出画' : '';
      return 'phase ' + this.phase + ' · ws ' + this.wsState + ' · ice ' + (this.iceState || '-') + viz;
    },
    statusMain: function () {
      var s = this.stage;
      if (s === 'ticket') return '申请机位中 · 第 ' + this.attemptNo + ' 次尝试';
      if (s === 'queued') return '机位繁忙，正在进入排队…';
      if (s === 'connecting') return '连接游戏网关…';
      if (s === 'waiting') return this.vizOn ? '游玩中' : '等待画面…';
      if (s === 'live') return '游玩中 · ' + this.vw + 'x' + this.vh;
      if (s === 'lost') return '连接已断开';
      if (s === 'fail') return '启动失败';
      if (s === 'end') return '本局已结束';
      if (s === 'nologin') return '未登录';
      return '正在准备…';
    },
    statusSub: function () {
      if (this.errText) return clip(this.errText, 80);
      if (this.tip) return clip(this.tip, 80);
      if (this.stage === 'live') return '触摸中间画面即可操作游戏';
      if (this.stage === 'ticket') return '云端正在分配机位，通常 5~20 秒';
      if (this.stage === 'nologin') return '登录后可申请云端机位并继续本局';
      return '';
    },
    // 左栏只有 219px 内容宽，状态主行必须硬截断（<text> 不裁切会溢出到视频区）
    statusMainClip: function () {
      return clip(this.statusMain, 16);
    },
    statusSub1: function () {
      var v = this.statusSub || '';
      if (!v) return '';
      return v.length <= 15 ? v : v.slice(0, 15);
    },
    statusSub2: function () {
      var v = this.statusSub || '';
      if (v.length <= 15) return '';
      return clip(v.slice(15), 15);
    },
    // 右栏内容宽 220px 放不下一整行「12.3 Mbps · 60fps · 丢包 0 · 1280x720」，拆两行
    /* 统计以 stat 事件为准（native cgmedia 的 poll 里带 kf/pli/rx/bitrateKbps/fps），
       与 WS 心跳频率无关：心跳降到 10s 一次不影响这里刷新。 */
    statsMain: function () {
      if (!this.statAt && !this.vizOn) return '暂无画面数据';
      var mb = this.br > 0 ? (this.br / 1000).toFixed(1) + ' Mbps' : '--';
      return mb + ' · ' + (this.fps || 0) + 'fps';
    },
    statsSub: function () {
      if (!this.statAt && !this.vizOn) return '尚未出画';
      return '丢包 ' + (this.lost || 0) + ' · ' + this.vw + 'x' + this.vh;
    },
    stripClip: function () {
      return clip(this.stripText, 36);
    },
    qualityTop: function () {
      return this.qualityList.slice(0, 2);
    },
    qualityBottom: function () {
      return this.qualityList.slice(2);
    },
    statsText: function () {
      if (!this.statAt) return '暂无画面数据（尚未出画）';
      var mb = this.br > 0 ? (this.br / 1000).toFixed(1) + ' Mbps' : '--';
      return mb + ' · ' + (this.fps || 0) + 'fps · 丢包 ' + (this.lost || 0) + ' · ' + this.vw + 'x' + this.vh;
    },
    dbgText: function () {
      if (!this.statAt) return 'kf - · pli - · rx -';
      return 'kf ' + this.kf + ' · pli ' + this.pli + ' · rx ' + this.rx + ' · ' + this.phase;
    },
    vtagText: function () {
      var mb = this.br > 0 ? (this.br / 1000).toFixed(1) + 'M' : '--';
      return (QUALITY_LABEL[this.quality] || this.quality) + ' ' + mb + ' ' + (this.fps || 0) + 'fps';
    },
    qualityList: function () {
      var out = [];
      for (var i = 0; i < QUALITIES.length; i++) {
        var q = QUALITIES[i];
        var on = this.quality === q;
        var locked = q === 'bluray' && this.blurayLocked;
        // 右栏 2x2 排布：每行两个，只有行首那个需要右间距。
        // 只剩一档时改成铺满整行（220 = 244 - 左右 padding 12*2），且不需要右间距。
        var solo = QUALITIES.length === 1;
        var cls = solo ? 'p2-qbtn p2-qbtn-solo' : 'p2-qbtn';
        if (i % 2 === 0 && !solo) cls += ' p2-gap';
        if (on) cls += locked ? ' p2-qbtn-onlock' : ' sel-on';
        else cls += locked ? ' p2-qbtn-lock' : ' sel-off';
        var label = QUALITY_LABEL[q] || q;
        var tcls = 'p2-qbtn-text';
        if (locked) {
          label = '蓝光·会员';
          tcls = 'p2-qbtn-text-lock';
        } else if (on) {
          tcls = 'p2-qbtn-text-on';
        }
        out.push({ id: q, label: label, cls: cls, tcls: tcls });
      }
      return out;
    },
    canReconnect: function () {
      if (this.busy) return false;
      var s = this.stage;
      if (s === 'boot' || s === 'ticket' || s === 'queued' || s === 'nologin') return false;
      if (s === 'live' && this.wsState === 'open') return false;
      return true;
    },
    canReapply: function () {
      if (this.busy) return false;
      var s = this.stage;
      if (s === 'boot' || s === 'ticket' || s === 'queued' || s === 'nologin') return false;
      return true;
    },
    reconnectCls: function () {
      return this.canReconnect ? 'p2-act p2-gap p2-act-main' : 'p2-act p2-gap p2-act-lock';
    },
    reconnectTcls: function () {
      return this.canReconnect ? 'p2-act-text-on' : 'p2-act-text-lock';
    },
    reapplyCls: function () {
      return this.canReapply ? 'p2-act p2-gap' : 'p2-act p2-gap p2-act-lock';
    }
  },
  // ImSession 必须在 created 建：mounted 时 boot() 已经发出 startPlay，
  // 而网关的 cmd 1 可能在首帧后几百毫秒内就到达，晚建会漏掉第一次弹键盘。
  created: function () {
    this.im = new ImSession();
    this.imBusy = false;
    this.kbOpen = false;
    const self = this;
    // 系统输入法长时间收不到事件时解除忙碌标志，但保留会话（用户之后确认仍能送回文字）
    this.im.onIdle = function () { self.imBusy = false; };
  },
  mounted: function () {
    this.dead = false;
    this.booted = false;
    this.recorded = false;
    this.touchIds = {};
    this.touchCount = 0;
    this.boot();
  },
  beforeDestroy: function () {
    this.cleanup();
  },
  // base-page 的 onUnload 会回调 $root.onUnload（真机实测 beforeDestroy 不一定触发），
  // cleanup 用 this.dead 做幂等，两条路径重复调用都安全。
  onUnload: function () {
    this.cleanup();
  },
  onHide: function () {
    /* 系统输入法弹出的 onHide 不是退后台：优先级最高，直接返回，连 hiddenSince 都不置。
       （app.js 里还有一层 inputBusy 守卫，两处语义必须一致，否则会出现
        "play 当作真退后台拆了会话、app 却以为在打字"的错位。） */
    if (this.imBusy || this.kbOpen) {
      logWarn('[play] onHide 忽略：系统输入法使用中');
      return;
    }
    this.enterBackground('onHide');
  },
  onStart: function () {
    logWarn('[play] page onStart');
  },
  onStop: function () {
    logWarn('[play] page onStop');
    /* onStop 与 onHide 在真机上是同一轮"退后台"的两个回调（顺序不保证），
       所以它们必须共用同一个宽限期：谁先到谁起定时器，后来的那个只是确认，
       绝不能再拆一次 —— 旧代码两条路径各自 bgTeardown()，第二次会把已经在
       宽限期里等着的会话立刻打死。 */
    if (this.imBusy || this.kbOpen) {
      logWarn('[play] onStop 忽略：系统输入法使用中');
      return;
    }
    this.enterBackground('onStop');
  },
  onShow: function () {
    logWarn('[play] page onShow stage=' + this.stage + ' screen=' + this.screenText() +
      ' hiddenFor=' + (this.hiddenSince ? Math.round((Date.now() - this.hiddenSince) / 1000) + 's' : 0));
    var wasHidden = !!this.hiddenSince;
    /* 平面可能是被系统键盘路径摘掉的（kbSystem → hideVideoPlane）：那种情况下
       hiddenSince 是 0，但平面依然是 1x1。恢复判据必须同时看这两个来源，
       否则「弹过系统键盘后页面又被切走一次再回来」会留下一块永远不回来的黑区。 */
    var needRestore = wasHidden || !!this.videoHidden;
    this.hiddenSince = 0;
    this.clearHideGrace();
    if (this.dead) return;
    /* STALLGUARD：后台期间 rx 自然不涨，回来时若沿用旧基准会立刻误判停滞，
       故把基准重置到「此刻」，让 20s 计数从回到前台重新开始。
       已用掉的 stallTries 保留：重连后又被网关停推时应当直接升级到下一级，
       而不是从 L1 重来。 */
    this.stallLastRx = this.rx;
    this.stallSince = 0;
    if (this.stallL1Timer) {
      clearTimeout(this.stallL1Timer);
      this.stallL1Timer = 0;
    }
    /* 冻结判据同样要重建基准：后台期间 gateDrop/fps 都不再推进（native 还在收包，
       但 stat 事件到不了 JS），沿用旧基准会让回来第一帧就判成「已冻结 30s」。
       只清计时与基准，**stallTries / stallOwner 保留**（理由同上）。 */
    if (this.freezeL1Timer) {
      clearTimeout(this.freezeL1Timer);
      this.freezeL1Timer = 0;
    }
    this.freezeDropBase = -1;
    this.freezeRxBase = 0;
    this.freezeFpsZeroSince = 0;
    this.freezeFpsZeroCnt = 0;
    if (this.booted) this.startTimers();
    /* 恢复顺序有讲究：先把平面重建回来（native 侧是销毁重建 vbox），
       再补一发 PLI 求 IDR —— 重建后解码器没有参考帧，不发 PLI 就是一片灰绿。 */
    if (needRestore) this.showVideoPlane();
    var s = this.stage;
    if (s === 'live' || s === 'waiting' || s === 'connecting') {
      try {
        keepAliveStart(KEEP_INTERVAL_MS);
      } catch (e) {}
      try {
        setRect(VIDEO);
      } catch (e) {}
      if (needRestore) {
        try {
          requestKeyFrame();
        } catch (e) {}
      }
    }
  },
  methods: {
    boot: function () {
      var self = this;
      var p = peekParams('play') || {};
      self.code = p.code || p.gameCode || '';
      self.name = p.name || self.code || '云游戏';
      self.icon = p.icon || '';
      self.gameType = p.gameType === 'pc' ? 'pc' : 'mobile';
      // 画质只允许当前档位（现在只有 low），历史参数里的 high/bluray 一律纠正
      if (p.quality) self.quality = normQuality(p.quality);
      if (p.gatewayUrl) self.gatewayUrl = p.gatewayUrl;
      if (p.ticketTtl) self.ticketTtl = Number(p.ticketTtl) || 0;
      if (p.regionName) self.regionName = p.regionName;
      logInfo('[play] boot code=' + self.code + ' q=' + self.quality + ' gw=' + (self.gatewayUrl ? 1 : 0));
      /* 主动续命的计数只在本局有效：进了新一局（哪怕同 code）就从头算。 */
      self.wsOpenAt = 0;
      self.renewAt = 0;
      self.renewCount = 0;
      self.startTimers();
      self.booted = true;
      if (!self.code) {
        self.fail('缺少游戏参数，无法开始游戏', 0);
        return;
      }
      loadSettings().then(function (st) {
        if (p.quality) return null;
        if (st && st.quality) self.quality = normQuality(st.quality);
        return null;
      }).catch(function () {
        return null;
      }).then(function () {
        return restoreSession();
      }).catch(function (e) {
        logWarn('[play] restore fail ' + clip((e && e.message) || e, 60));
        return null;
      }).then(function () {
        if (!isLoggedIn()) {
          self.stage = 'nologin';
          logWarn('[play] 未登录，引导到登录页');
          return null;
        }
        return self.beginSession();
      }).then(function () {
        if (self.stage === 'nologin' || self.dead) return null;
        if (self.gatewayUrl) {
          if (!self.startAt) self.startAt = Date.now();
          return self.notePlay().then(function () {
            return self.connect();
          });
        }
        return self.applySeat();
      }).catch(function (e) {
        logError('[play] boot 异常 ' + clip((e && e.message) || e, 80));
        self.fail('启动失败：' + clip((e && e.message) || e, 40), 0);
        return null;
      });
    },
    beginSession: function () {
      try {
        setRect(VIDEO);
      } catch (e) {}
      var self = this;
      try {
        setEventHandler(function (ev) {
          self.onMedia(ev);
        });
      } catch (e) {
        logWarn('[play] setEventHandler fail');
      }
      return initScreen().catch(function () {
        return null;
      }).then(function () {
        try {
          keepAliveStart(KEEP_INTERVAL_MS);
        } catch (e) {}
        return null;
      });
    },
    applySeat: function () {
      var self = this;
      if (self.busy) return Promise.resolve(null);
      self.busy = true;
      self.stage = 'ticket';
      self.errText = '';
      self.errCode = 0;
      self.tip = '';
      self.slowHint = false;
      self.ticketAt = Date.now();
      self.elapsed = 0;
      // gameType 由详情页透传（'pc' | 'mobile'）；size 必须与之一致，
      // 端游容器是 1920x1080、手游是 1280x720，传错会让 lock_detail 与实际分辨率不符。
      // regions 故意不传：session.applyTicket 会用 media-servers 查出该游戏真实可用的
      // region 列表（写死 ghnfs 会让所有端游拿到 1288）。
      var gt = self.gameType === 'pc' ? 'pc' : 'mobile';
      var sw = gt === 'pc' ? 1920 : VW;
      var sh = gt === 'pc' ? 1080 : VH;
      return applyTicket({
        gameCode: self.code,
        gameType: gt,
        quality: self.quality,
        size: { w: sw, h: sh }
      }).then(function (r) {
        if (self.dead) {
          // 页面已被销毁但票据刚回来：立刻释放，避免机位/排队位泄漏
          if (r && r.gatewayUrl) {
            logWarn('[play] 页面已销毁但拿到机位，立即释放');
            try {
              var sp = stopGame(self.code);
              if (sp && sp.catch) sp.catch(function () {});
            } catch (e) {}
          }
          return null;
        }
        if (r && r.queued) {
          self.stage = 'queued';
          self.selfClosedBy = 'queued(navTo queue)';
          self.stallReset();
          logInfo('[play] 进入排队 expires=' + r.expires);
          self.clearTimers();
          try {
            keepAliveStop();
          } catch (e) {}
          navTo('queue', { gameCode: self.code, name: self.name, icon: self.icon, expires: r.expires || 0 });
          return null;
        }
        self.gatewayUrl = (r && r.gatewayUrl) || '';
        self.ticketTtl = Number(r && r.ticketTtl) || self.ticketTtl;
        self.regionName = (r && r.regionName) || self.regionName;
        self.region = (r && r.region) || self.region;
        self.detectBluray(r);
        if (!self.startAt) self.startAt = Date.now();
        return self.notePlay().then(function () {
          self.stage = 'connecting';
          return self.connect();
        });
      }).catch(function (e) {
        self.fail(self.reasonOf(e), Number(e && e.code) || 0);
        return null;
      }).then(function () {
        self.busy = false;
        return null;
      });
    },
    connect: function () {
      var self = this;
      if (!self.gatewayUrl) {
        self.fail('没有拿到网关地址，请重新申请机位', 0);
        return Promise.resolve(null);
      }
      self.stage = 'connecting';
      self.connectAt = Date.now();
      self.slowHint = false;
      self.vizOn = false;
      self.tip = '';
      return startPlay({
        gameCode: self.code,
        quality: self.quality,
        gatewayUrl: self.gatewayUrl,
        headers: ''
      }).then(function () {
        if (self.dead) {
          logWarn('[play] 页面已销毁但 ws 已建立，立即停止');
          try {
            var zp = stopPlay();
            if (zp && zp.catch) zp.catch(function () {});
          } catch (e) {}
          return null;
        }
        self.stage = 'waiting';
        self.tip = '已建立 WebSocket，正在鉴权…';
        logInfo('[play] startPlay ok');
        return null;
      }).catch(function (e) {
        self.fail('连接游戏网关失败：' + clip((e && e.message) || e, 30), 0);
        return null;
      });
    },
    notePlay: function () {
      var self = this;
      if (self.recorded) return Promise.resolve(null);
      self.recorded = true;
      return recordPlay({
        code: self.code,
        name: self.name,
        icon: self.icon,
        at: Date.now(),
        seconds: 0
      }).catch(function () {
        return null;
      }).then(function () {
        return null;
      });
    },
    reasonOf: function (e) {
      var code = Number(e && e.code) || 0;
      if (ERR_TEXT[code]) return ERR_TEXT[code];
      var msg = (e && (e.message || e.errmsgcn)) || '';
      if (!msg) return '申请机位失败，请稍后重试';
      return clip(msg, 60);
    },
    detectBluray: function (r) {
      var vals = [];
      var raw = (r && r.raw) || {};
      if (raw.bluray !== undefined && raw.bluray !== null) vals.push(raw.bluray);
      var s = currentSession();
      if (s && s.bluray !== undefined && s.bluray !== null) vals.push(s.bluray);
      if (s && s.user && s.user.bluray !== undefined && s.user.bluray !== null) vals.push(s.user.bluray);
      var locked = false;
      for (var i = 0; i < vals.length; i++) {
        if (blocked(vals[i])) locked = true;
      }
      this.blurayLocked = locked;
      logInfo('[play] blurayLocked=' + locked);
    },
    onMedia: function (ev) {
      if (this.dead || !ev) return;
      var t = ev.t || '';
      var s = this.stage;
      if (t === 'ws') {
        if (ev.state) this.wsState = ev.state;
        if (ev.state === 'open') {
          /* ★ 会话寿命从这一刻起算（见 ACTIVE_RENEW_MS 注释）。
           * 必须在**每次** open 时刷新：L2 自愈 / 主动续命 / 用户点重连
           * 都会产生新会话，每个新会话都只有约 250s 寿命。 */
          this.wsOpenAt = Date.now();
          if (s === 'connecting' && !this.vizOn) this.stage = 'waiting';
          if (!this.vizOn) this.tip = '已连接网关，正在鉴权…';
          /* 官方原文 "25 "+(s?"keymouse":"tochscreen")（tochscreen 少一个 u 是官方拼写）。
             真机实测：切到 keymouse 之后 100/101/102 鼠标事件全部被远端丢弃（点击零响应），
             留在 touchscreen 模式下用鼠标协议反而是通的 —— 所以这里固定 touchscreen，
             端游靠下面的 pcClick() 走鼠标协议即可。 */
          try {
            setInputMode('touchscreen');
          } catch (e) {}
        }
        return;
      }
      if (t === 'offer') {
        if (!this.vizOn) {
          this.stage = 'waiting';
          this.tip = '已收到画面协商，正在拉起播放器…';
        }
        return;
      }
      if (t === 'answer') {
        if (ev.mediaErr) {
          this.mediaErr = String(ev.mediaErr);
          if (!this.vizOn) this.tip = '播放器启动失败：' + clip(this.mediaErr, 32);
        } else if (!this.vizOn) {
          this.tip = '媒体通道已建立，等待首帧…';
        }
        return;
      }
      if (t === 'mediafail') {
        this.mediaErr = ev.msg || '媒体模块不可用';
        this.fail('播放器（cgmedia）启动失败：' + clip(this.mediaErr, 32), 0);
        return;
      }
      if (t === 'ice') {
        this.iceState = ev.state || '';
        if (ev.state === 'probing') { if (!this.vizOn) this.tip = '正在打通媒体网络（ICE）…'; }
        else if (ev.state === 'connected') { if (!this.vizOn) this.tip = '媒体网络已连通，正在安全握手…'; }
        else if (ev.state === 'failed') this.fail('媒体网络打不通（ICE 失败），请点「重连」', 0);
        return;
      }
      if (t === 'dtls') {
        this.dtlsState = ev.state || '';
        if (ev.state === 'connected') { if (!this.vizOn) this.tip = '安全握手完成，正在等待首个视频帧…'; }
        else if (ev.state === 'failed') this.fail('安全握手失败（DTLS），请点「重连」', 0);
        return;
      }
      if (t === 'media') {
        this.vizOn = true;
        this.stage = 'live';
        this.tip = '';
        this.errText = '';   /* 出画即证明协商成功，清掉残留的 auth 期错误文案 */
        this.errCode = 0;
        this.slowHint = false;   /* 复位「等待超过 20 秒」，否则出画后文案仍可能被写回 */
        this.connectAt = 0;
        if (ev.videoW) this.vw = ev.videoW;
        if (ev.videoH) this.vh = ev.videoH;
        try {
          setInputMode('touchscreen');
        } catch (e) {}
        /* 官方 startGame 起播后必发 "10 <platform>" 声明客户端平台，
           部分端游靠它切换鼠标/键盘映射。不发的话只有触摸能用。 */
        try {
          declarePlatform('android');
        } catch (e) {}
        logInfo('[play] 出画 ' + this.vw + 'x' + this.vh + ' audio=' + (ev.audio ? 1 : 0));
        return;
      }
      if (t === 'input') {
        /* ★ 网关下发的输入指令（官方 run.js 的 `op=input` 下行处理，逐条对齐）：
         *   cmd "0 <ts>"  —— 我们自己 keepAlive 心跳的回显，用来测延迟，忽略
         *   cmd "1"       —— 远端弹出了输入框，该唤起键盘了
         *   cmd "2"       —— 远端收起输入框，该收键盘
         *   cmd "3"       —— 微信扫码登录指引
         * 官方是在收到 cmd 1 后延迟 200ms 再聚焦（避免与远端 UI 动画抢焦点）。
         * 我们这边「聚焦」= 弹 Falcon 系统键盘（ImSession → global.startTextEdit），
         * 顺带必须先把 KMS 视频平面缩成 1x1，否则键盘被视频平面整个盖住。 */
        var ic = String(ev.cmd || '');
        var head = ic.split(' ')[0];
        if (head === '1') {
          this.openRemoteKb();
        } else if (head === '2') {
          this.closeRemoteKb();
        } else {
          logWarn('[play] input cmd 忽略: ' + clip(ic, 40));
        }
        return;
      }
      if (t === 'videovis') {
        /* native 回执：1=画面已恢复 0=画面已隐藏。仅用于日志与状态恢复判断。 */
        logInfo('[play] 视频平面 ' + (ev.on ? '恢复' : '隐藏'));
        return;
      }
      if (t === 'stat') {
        var now = Date.now();
        if (now - this.statAt < 900) return;
        this.statAt = now;
        /* native 的 shown > 0 证明真有buffer上屏了（不是「我们推出去了」），
           这才是把 -7 计数清零的可靠依据：出画事件在纯黑局也会触发。 */
        if (ev.shown > 0 && this.nativeFrozen) this.nativeFrozen = 0;
        /* 只靠 cgmedia 的 media 事件判断出画并不可靠：真机上有整轮 media 事件
           丢失、但 kf/pli/rx 照常在报的情况，于是遮罩一直盖着（KMS 平面在 UI 之上，
           玩家看到的是画面，中栏文案却永远是「正在连接云游戏…」）。
           stat 里只要出现任意解码/收包计数，就说明画面已经在跑了，直接转 live。 */
        if (!this.vizOn && ((ev.rx | 0) > 0 || (ev.kf | 0) > 0 || (ev.fps | 0) > 0)) {
          this.vizOn = true;
          this.stage = 'live';
          this.tip = '';
          this.errText = '';
          this.errCode = 0;
          this.slowHint = false;
          this.connectAt = 0;
          logInfo('[play] 由 stat 推断出画 ' + (ev.w || 0) + 'x' + (ev.h || 0));
        }
        this.rx = ev.rx || 0;
        this.lost = ev.lost || 0;
        this.br = ev.bitrateKbps || 0;
        this.fps = ev.fps || 0;
        this.kf = ev.kf || 0;
        this.pli = ev.pli || 0;
        /* ★ 弱网冻结判据的数据源（native cm_media.c:120 早已输出，JS 侧之前没人读）：
           gateDrop 涨 = native 正在「等完整 IDR」闸门丢 P 帧 = 一定没画面；
           kfReq vs kf 的差值回答「网关到底理不理我们的 PLI」。 */
        this.gateDrop = ev.gateDrop || 0;
        this.auDrop = ev.auDrop || 0;
        this.kfReq = ev.kfReq || 0;
        /* stat 采样序号 +1：checkFreeze 用「采样」而不是「tick」做计数单位。
           stat 由 native 每 2s 发一次、这里 900ms 节流，而 tick 是 500ms 一次——
           若按 tick 判，一半的 tick 看不到 gateDrop 变化，会把增长窗口反复清零，
           而 fps==0 的采样数又会虚高（一次 stat 被数成好几次）。 */
        this.statSeq++;
        if (ev.w) this.vw = ev.w;
        if (ev.h) this.vh = ev.h;
        return;
      }
      if (t === 'error') {
        var d = ev.data || {};
        /* ★ cgmedia 直接发的错误（没有 data 包装）：目前只有一个 -7。
         *   -7 = video sink frozen：上屏看门狗把解码器管线重建了 VWD_MAX_REBUILDS
         *        次，kmssink 仍然一帧新 buffer 都收不到。
         * 这不是网关错误，把它显示成「服务端错误」既误导玩家又救不回画面。
         * 真机实测（2026-11-14 18:01 那局）根因是 RK3562 VPU 报
         *   mpp_rkvdec2 ... irq_status 0x001123 timeout 0 -> resetting -> reset done
         * 复位后解码器丢掉全部参考帧，而终末地一局只给 1 个 IDR、网关又从不补
         * NACK，于是永远等不到可解码的起点。唯一有效的动作是**换会话**（L2），
         * 让新解码器从新的 IDR 开始。 */
        var ncode = Number(ev.code) || 0;
        if (!ev.data && ncode === -7) {
          /* ★ 第一次 -7 先试 L2（复用网关重连，代价最小）。
           * 真机实测：终末地的黑屏是「RK3562 VPU 报错被内核复位」引起的，
           * 复位后新解码器需要一个新的 IDR，而同一机位/同一会话里网关几乎不再
           * 给 IDR（一局只 1 个），所以 L2 往往也只能再黑一次。第二次 -7 就
           * 直接 L3 —— 重新申请机位换一台机器，那条流是新的、IDR 一定会来。 */
          this.nativeFrozen = (this.nativeFrozen | 0) + 1;
          logError('[play] native -7 video sink frozen（第 ' + this.nativeFrozen + ' 次）');
          if (this.stage === 'live' || this.stage === 'waiting' || this.stage === 'connecting') {
            var why2 = 'native video sink frozen x' + this.nativeFrozen;
            if (this.nativeFrozen <= 1) this.stallHealL2(Date.now(), 'freeze', why2);
            else this.stallHealL3(Date.now(), 'freeze', why2);
          }
          return;
        }
        if (!ev.data && ncode === -4) {
          logError('[play] native -4 video rebuild failed → L2 换会话');
          if (this.stage === 'live' || this.stage === 'waiting' || this.stage === 'connecting') {
            this.stallHealL2(Date.now(), 'freeze', 'native video rebuild failed');
          }
          return;
        }
        /* 网关在 ticket 校验阶段会对首个 auth 回一条 400 format invalid，
         * 但其实 offer 紧接着就来了、协商照常进行（真机实测三轮一致）。
         * 这种「协商已成功」后的残留错误不再上屏，免得玩家看到假的失败文案。 */
        var code0 = Number(d.errcode) || 0;
        /* 这条 400 在上一条连接的 auth 之后、新 offer 之前到达，本身是网关对
           「无 media 上下文的首个 auth」的固定回显，与会话是否合法无关。
           判据改成「本局还在进行中」而不是「已经出画」：出画那一瞬间 vizOn 还没
           置位、stage 也可能还停在 connecting，用旧判据会漏掉，于是玩家在正常
           游玩时看到一行假的「服务端错误：format invalid」。 */
        var inSession = this.vizOn
          || this.stage === 'connecting'
          || this.stage === 'waiting'
          || this.stage === 'live';
        if (code0 === 400 && inSession) {
          logWarn('[play] 忽略会话进行中残留的 400 format invalid');
          return;
        }
        this.errText = '服务端错误：' + clip(ev.msg || d.errmsgcn || d.errmsg || '未知错误', 56);
        if (d.errcode) this.errCode = Number(d.errcode) || 0;
        logError('[play] ws error ' + clip(JSON.stringify(d), 120));
        return;
      }
      if (t === 'gamestop') {
        var c = Number(ev.code) || 0;
        if (c === 0) this.finish('游戏已正常结束，机位已释放');
        else this.fail('游戏已被服务端停止（code ' + c + '）', c);
        return;
      }
      if (t === 'wsclose') {
        if (s === 'live' || s === 'waiting' || s === 'connecting') {
          this.stage = 'lost';
          this.vizOn = false;
          this.tip = '';
          this.errText = '与游戏服务器的连接已断开：' + clip(ev.msg || 'closed', 28);
          logWarn('[play] wsclose ' + (ev.msg || ''));
        }
        return;
      }
      if (t === 'rect') {
        logInfo('[play] rect ' + ev.x + ',' + ev.y + ',' + ev.w + ',' + ev.h);
      }
    },
    pickQuality: function (id) {
      if (id === 'bluray' && this.blurayLocked) {
        this.setHint('蓝光需要会员权益，当前账号不可用');
        return;
      }
      if (this.quality === id) return;
      this.quality = id;
      this.setHint('已切换到 ' + (QUALITY_LABEL[id] || id) + ' 画质');
      if (QUALITIES.length <= 1) return; // 只剩一档，不需要真的去换（q 恒定 low）
      try {
        setQuality(id);
      } catch (e) {
        logWarn('[play] setQuality fail');
      }
      /* 换档后主动要一个 IDR。
       *
       * 真机实测（2026-11-07）：换画质时 SPS 报的分辨率**恒为 1280x720**，
       * 所以「档位越高分辨率越高」这个假设是错的；除流畅档外的马赛克/纯绿
       * 只能来自「换档瞬间码流断点，解码器还在等新 IDR」。native 侧只在
       * SPS profile/level 变化时才自动补 PLI，SPS 完全没变的情况它察觉不到，
       * 所以这里补两发（600ms / 2s），跨越网关换档的实际生效时刻。 */
      requestKeyFrame();
      this.clearQualityKf();
      var self = this;
      this.kfTimers = [
        setTimeout(function () { requestKeyFrame(); }, 600),
        setTimeout(function () { requestKeyFrame(); }, 2000)
      ];
      try {
        saveSettings({ quality: id });
      } catch (e) {}
      logInfo('[play] quality → ' + id);
    },
    /* 换档时排的两个补 PLI 定时器必须能被下一次换档/页面销毁清掉，
     * 否则连点四个档位会留下 8 个定时器，而且旧的会打在新会话上。 */
    clearQualityKf: function () {
      var a = this.kfTimers;
      if (!a || !a.length) return;
      for (var i = 0; i < a.length; i++) {
        try { clearTimeout(a[i]); } catch (e) {}
      }
      this.kfTimers = [];
    },
    setHint: function (text) {
      this.qualityHint = text;
      var self = this;
      setTimeout(function () {
        if (self.qualityHint === text) self.qualityHint = '';
      }, 2600);
    },
    askExit: function () {
      if (this.stage === 'nologin') {
        this.doExit();
        return;
      }
      this.confirmExit = true;
    },
    cancelExit: function () {
      this.confirmExit = false;
    },
    onReconnect: function () {
      var self = this;
      if (!self.canReconnect) return Promise.resolve(null);
      self.busy = true;
      self.errText = '';
      self.errCode = 0;
      self.slowHint = false;
      self.tip = '正在重连…';
      return stopPlay().catch(function () {
        return null;
      }).then(function () {
        if (!self.gatewayUrl) return null;
        self.stage = 'connecting';
        self.connectAt = Date.now();
        return startPlay({
          gameCode: self.code,
          quality: self.quality,
          gatewayUrl: self.gatewayUrl,
          headers: ''
        }).then(function () {
          self.stage = 'waiting';
          self.tip = '已重新连接网关，等待画面…';
          logInfo('[play] reconnect ok（复用旧网关）');
          return true;
        }).catch(function () {
          logWarn('[play] 复用旧网关失败，改走重新申请机位');
          self.gatewayUrl = '';
          return false;
        });
      }).then(function (ok) {
        self.busy = false;
        if (ok) return null;
        return self.applySeat();
      });
    },
    onReapply: function () {
      var self = this;
      if (!self.canReapply) return Promise.resolve(null);
      return stopPlay().catch(function () {
        return null;
      }).then(function () {
        self.gatewayUrl = '';
        return self.applySeat();
      });
    },
    finish: function (text) {
      var self = this;
      self.selfClosedBy = 'finish';
      self.stallReset();
      self.stage = 'end';
      self.errText = text || '';
      self.vizOn = false;
      self.tip = '';
      return self.accumulate().then(function () {
        return stopPlay();
      }).catch(function () {
        return null;
      }).then(function () {
        return self.code ? stopGame(self.code) : null;
      }).catch(function () {
        return null;
      }).then(function () {
        try {
          keepAliveStop();
        } catch (e) {}
        return null;
      });
    },
    fail: function (msg, code) {
      var self = this;
      self.selfClosedBy = 'fail';
      self.stallReset();
      self.errText = msg || '发生未知错误';
      self.errCode = Number(code) || 0;
      self.stage = 'fail';
      self.vizOn = false;
      self.tip = '';
      logError('[play] fail code=' + self.errCode + ' ' + self.errText);
      self.accumulate().then(function () {
        return stopPlay();
      }).catch(function () {
        return null;
      }).then(function () {
        return self.code ? stopGame(self.code) : null;
      }).catch(function () {
        return null;
      }).then(function () {
        try {
          keepAliveStop();
        } catch (e) {}
        return null;
      });
    },
    accumulate: function () {
      if (!this.code || !this.startAt) return Promise.resolve(null);
      var sec = Math.max(0, Math.round((Date.now() - this.startAt) / 1000));
      this.startAt = 0;
      if (sec < 3) return Promise.resolve(null);
      return touchPlay(this.code, sec).then(function () {
        logInfo('[play] 记账 ' + sec + 's');
        return null;
      }).catch(function () {
        return null;
      });
    },
    doExit: function () {
      var self = this;
      if (self.busy) return Promise.resolve(null);
      // 未登录时从来没申请过机位，直接回首页，别白发一轮 DELETE
      if (self.stage === 'nologin') {
        self.selfClosedBy = 'doExit(nologin)';
        self.clearTimers();
        self.goHome();
        return Promise.resolve(null);
      }
      self.busy = true;
      self.confirmExit = false;
      self.selfClosedBy = 'doExit';
      self.stallReset();
      self.stage = 'end';
      self.tip = '正在退出并释放机位…';
      logInfo('[play] 用户退出，开始释放机位');
      // 顺序：先停媒体+收 WS（立刻断流量）→ 释放机位 → 停保活 → 记账
      return stopPlay().then(function () {
        return stopGame(self.code);
      }).catch(function () {
        logWarn('[play] stopGame fail（机位可能由服务端超时释放）');
        return null;
      }).then(function () {
        try {
          keepAliveStop();
        } catch (e) {}
        return self.accumulate();
      }).catch(function () {
        return null;
      }).then(function () {
        self.clearTimers();
        self.busy = false;
        logInfo('[play] 退出完成 → index');
        self.goHome();
        return null;
      });
    },
    goLogin: function () {
      navTo('login', {});
    },
    /* 回首页：只关掉自己，剩下「再退一层」交给被露出来的 detail / queue
     * 在自己 onShow 里接力（见 services/nav.js 的 requestHome 注释）。
     * 直接 navTo('index') 会新压一层 index —— 动画是 push、页面栈还会变深。 */
    goHome: function () {
      /* 一次退到底：先把夹在中间的那些页按名字从栈里摘掉，最后再关自己。
       * 不能「先关自己再 setTimeout 关别人」——自己的 JS 上下文一没，定时器
       * 就跟着没了，后面的关不掉（真机实测会停在中间页 detail 上）。
       * 反过来（先摘下面的、最后关自己）在本固件上可行，实测见 HANDOFF。 */
      try {
        if (typeof $falcon !== 'undefined' && typeof $falcon.closePageByName === 'function') {
          $falcon.closePageByName('detail');
          $falcon.closePageByName('queue');
        }
      } catch (e) { /* ignore */ }
      if (!navBack('play')) navTo('index', {});
    },
    goIndex: function () {
      this.goHome();
    },
    startTimers: function () {
      if (this.dead) return;
      this.clearTimers();
      var self = this;
      this.timer = setInterval(function () {
        self.tick();
      }, 500);
    },
    clearTimers: function () {
      this.clearQualityKf();
      if (this.timer) {
        clearInterval(this.timer);
        this.timer = 0;
      }
    },
    tick: function () {
      if (this.dead) return;
      var now = Date.now();
      this.clock = now;
      var s = this.stage;
      if (s === 'live' || s === 'waiting' || s === 'connecting' || s === 'lost') this.dots = '';
      else this.dots = '...'.slice(0, Math.floor(now / 400) % 4);
      try {
        var ms = mediaState();
        if (ms) {
          if (ms.phase) this.phase = ms.phase;
          if (ms.wsState) this.wsState = ms.wsState;
        }
      } catch (e) {}
      if (s === 'ticket' && this.ticketAt) {
        var el = Math.floor((now - this.ticketAt) / 1000);
        if (el !== this.elapsed) this.elapsed = el;
        if (el > 75 && !this.slowHint) {
          this.slowHint = true;
          this.tip = '申请机位较慢，可点「重新申请」重试';
        }
      }
      /* 一旦出画就绝不能再写回「等待画面」类文案：vizOn 是唯一判据。 */
      if (!this.vizOn && s === 'waiting' && this.connectAt && now - this.connectAt > 20000 && !this.slowHint) {
        this.slowHint = true;
        this.tip = '等待画面超过 20 秒，可点「重连」重试';
      }
      /* 兜底：vizOn 为真但 stage 还停在 connecting/waiting 时，把 stage 推到 live，
         中栏遮罩（v-if="!vizOn"）与 stageChip 才会同时正确。 */
      if (this.vizOn && (s === 'connecting' || s === 'waiting')) {
        this.stage = 'live';
        this.tip = '';
        this.slowHint = false;
      }
      /* WS 心跳：官方是走 DataChannel 每 1s 发一次 "0 <ms>"。
         我们没有 SCTP 通道，只能借 WS 的 input 发；但网关会把每一条 input
         都回显成 `op=input`，1s 一次会把日志和轮询队列都灌满（真机实测）。
         改成 10s 一次：既提醒网关会话还活着，又不至于淹没真正的业务帧。 */
      if (s === 'live' || s === 'waiting') {
        if (!this.hbAt || now - this.hbAt >= 10000) {
          this.hbAt = now;
          try { keepAlive(now); } catch (e) {}
        }
      }
      /* ★ 交互续命 A/B（取证开关 DEV_AUTOTAP，见 dev-autostart.js 说明）：
         live 态下每 30s 注入一次 1px 微动触摸（down@1,1 → up@2,2），
         验证「真实输入能否重置服务端 250s 踢出计时」。判定：sess 跨过
         260s 继续涨 = 输入续命有效；仍 ~250s 停推 = 无效。 */
      if (DEV_AUTOTAP && s === 'live' && !this.busy && this.vizOn) {
        if (!this.tapAt || now - this.tapAt >= 30000) {
          this.tapAt = now;
          try {
            down(1, 1, 0, this.vw || VW, this.vh || VH);
            var _selfTap = this;
            setTimeout(function () {
              try { _selfTap.up2(); } catch (e) {}
            }, 120);
            logWarn('[play] autotap: 注入微动触摸（第 ' +
              (++this.tapCount || (this.tapCount = 1)) + ' 次）');
          } catch (e) {}
        }
      }
      /* ★ STALLGUARD：网关静默停推检测（见文件头 STALL_RX_MS 注释）。
         放在心跳/探针之后：即使它触发重连，本轮的 probe 行也已经写下了
         「停推前最后一次 rx=…」这个现场，日志顺序更可读。 */
      try {
        this.checkStall(now, s);
      } catch (e) {
        logWarn('[play] checkStall 异常');
      }
      /* ★ 主动续命：赶在网关停推之前换会话（见 ACTIVE_RENEW_MS 注释）。
         必须放在 checkStall 之后：若某一局真的提前停推、STALLGUARD 已经在自愈，
         renewNow 的门会拦掉这次主动重连（busy / stallTries>0），不会打架。 */
      try {
        this.checkRenew(now, s);
      } catch (e) {
        logWarn('[play] checkRenew 异常');
      }
      /* ★ 第二条判据：弱网冻结（rx 在涨但画面不动，见 FREEZE_GATE_MS 注释）。
         必须放在 checkStall 之后：checkStall 会维护 stallLastRx 基准，
         freeze 依赖「rx 在涨」这个结论；反过来跑会让基准慢半拍。 */
      try {
        this.checkFreeze(now, s);
      } catch (e) {
        logWarn('[play] checkFreeze 异常');
      }
      /* ★ bug1 的现场指纹：每 20 秒打一行「存活探针」，把四条链的即时状态拼在一起：
       *   屏幕（getScreenState）、JS 定时器是否还在跑（本行本身出现=在跑）、
       *   媒体字节/帧数（rx/fps/kf）、WS 状态。
       * 排查「4 分钟后停了」时看这一行就够了：
       *   - 这一行还在刷 + rx 在涨 + screen=OFF  → 屏黑了，画面其实还在，是保活问题；
       *   - 这一行断了 + 之后出现 `JS heartbeat lost` → QuickJS 被挂起，JS 链断；
       *   - 这一行还在刷 + rx 不涨 + wsState 变了 → 网络/网关侧断流，跟屏幕无关。
       * 20 秒刚好和保活周期对齐，日志量可接受（一局 10 分钟 30 行）。 */
      if (!this.probeAt || now - this.probeAt >= 20000) {
        this.probeAt = now;
        logWarn('[play] probe stage=' + s + ' v=' + (this.vizOn ? 1 : 0) +
          ' screen=' + this.screenText() +
          ' phase=' + this.phase + ' ws=' + this.wsState +
          ' rx=' + this.rx + ' fps=' + this.fps + ' kf=' + this.kf +
          ' gateDrop=' + this.gateDrop + ' auDrop=' + this.auDrop +
          ' kfReq=' + this.kfReq +
          ' silent=' + (this.stallSince ? Math.round((now - this.stallSince) / 1000) + 's' : '0s') +
          ' frozen=' + (this.freezeFpsZeroSince ? Math.round((now - this.freezeFpsZeroSince) / 1000) + 's' : '0s') +
          ' tries=' + this.stallTries + (this.stallOwner ? '(' + this.stallOwner + ')' : '') +
          ' sess=' + (this.wsOpenAt ? Math.round((now - this.wsOpenAt) / 1000) + 's' : '-') +
          ' renew=' + this.renewCount +
          ' keep=' + (this.hiddenSince ? 'hidden' + Math.round((now - this.hiddenSince) / 1000) + 's' : 'on'));
      }
    },
    /* ============ STALLGUARD：网关停推检测 + 三级自愈 ============
       设计依据见文件头 STALL_RX_MS 注释（网关每会话约 250s 静默停推）。
       判据只看两件事：stage==='live' 且 rx 连续 STALL_RX_MS（8s）没涨。 */

    /* 把所有停滞状态清干净（含 L1 的复查定时器）。停/恢复/销毁路径都要调，
       否则退出后残留的 setTimeout 会对着死页面继续自愈。 */
    stallReset: function () {
      if (this.stallL1Timer) {
        clearTimeout(this.stallL1Timer);
        this.stallL1Timer = 0;
      }
      this.stallLastRx = 0;
      this.stallSince = 0;
      this.stallAt = 0;
      this.stallTries = 0;
      /* 第二条判据（弱网冻结）的计时器与基准一并清掉。stallTries 是两个判据
         共用的计数上限，所以这里清它会同时重置两边 —— 这是我们想要的语义：
         「这一局结束了/重连了，自愈额度重新算」。但两条判据各自的**计时器**
         必须分开清，绝不能在计时过程中互相对调 stallReset()。 */
      if (this.freezeL1Timer) {
        clearTimeout(this.freezeL1Timer);
        this.freezeL1Timer = 0;
      }
      this.freezeDropBase = -1;
      this.freezeRxBase = 0;
      this.freezeFpsZeroSince = 0;
      this.freezeFpsZeroCnt = 0;
      this.freezeZeroAt = 0;
      this.stallOwner = '';
    },
    /* ============ ACTIVE RENEW：会话寿命预警，主动换会话 ============
     * 判据只有一条：本次 WS 会话已存活 ≥ ACTIVE_RENEW_MS。
     * 依据与阈值推导见文件头 ACTIVE_RENEW_MS 注释（三局实测 247/253/248s）。
     *
     * ★ 默认关闭（ACTIVE_RENEW_MS = 0）：真机验证发现"换会话"必然换机位、
     *   必然重拉对局容器 = 游戏从头开始，代价远大于收益。整段实现保留，
     *   把常量改成 230000 即可重新启用。关闭后由 STALLGUARD 独立兜底。
     *
     * 设计初衷（2026-11-14 01:47 之前的思路，留档）：不等停推再救 ——
     * STALLGUARD 的判据是「rx 冻结 8s」，L1+L2 全成功也要用户盯着静止画面
     * 十几秒。主动续命本想把这段空窗压到 2~4 秒，但实测那 2~4 秒里
     * 换来的是游戏重开，所以放弃。
     *
     * 与 STALLGUARD 的互斥（顺序都写在这里，改的时候别漏）：
     *   - stallTries > 0  → 已经在自愈，让 STALLGUARD 走完，这次不抢；
     *   - busy            → 重连/申请/退出正在进行；
     *   - stallL1Timer    → L1 已发，正等 5s 复查；
     *   - !vizOn          → 画面本来就没出来（还在等首帧），此时重连毫无意义；
     *   - stage !== live  → 同上，只在真正出画stable态下才续命。
     * 这些门与 checkStall 顶部的门刻意保持一致 —— 两条防线不许同时动作，
     * 否则会互相把对方刚设的 stallTries / 计时基准改掉（这是本项目已经踩过的坑）。 */
    checkRenew: function (now, s) {
      if (this.dead) return;
      if (!ACTIVE_RENEW_MS) return;      /* 开关：置 0 即关闭主动续命 */
      if (!this.wsOpenAt) return;        /* 还没连上过网关 */
      if (this.hiddenSince) return;      /* 退后台不续命（同 checkStall 的理由） */
      if (this.busy) return;
      if (this.stallTries > 0) return;   /* STALLGUARD 正在自愈，让它走完 */
      if (this.stallL1Timer) return;
      if (!this.vizOn) return;           /* 没出画就别换会话 */
      if (s !== 'live') return;
      if (this.wsState !== 'open') return;
      /* 刚换过会话：给新会话留出出画时间，避免连环重连。 */
      if (this.renewAt && now - this.renewAt < ACTIVE_RENEW_GUARD_MS) return;
      var age = now - this.wsOpenAt;
      if (age < ACTIVE_RENEW_MS) return;
      this.renewNow(now, age);
    },
    renewNow: function (now, age) {
      var self = this;
      this.renewAt = now;
      this.renewCount++;
      this.wsOpenAt = now;   /* 立刻重置基准：若重连失败，下一轮 230s 后才会再试 */
      /* 和 stallHealL2/L3 一样重开时间窗：换机位要 5~10s，期间 checkStall 的
         silent 计时会一直走，不重置的话新会话刚回来就可能被上一轮计时误判。 */
      this.stallWindowReset(now);
      logWarn('[play] renew: 会话已存活 ' + Math.round(age / 1000) +
        's（阈值 ' + Math.round(ACTIVE_RENEW_MS / 1000) +
        's）→ 主动换会话（第 ' + this.renewCount + ' 次），避免网关停推');
      /* 与 stallHealL2 同样的动作序列，但**不写 stallTries**：
       * 主动续命是我们计划内的行为，不该占用 STALLGUARD 的自愈额度，
       * 否则真正遇到弱网停推时用户只剩 2 次机会。 */
      this.vizOn = false;
      this.stage = 'waiting';
      this.tip = '正在切换画面通道…';
      /* ★ 这里必须走 onReapply（重新申请机位），不能用 onReconnect（复用旧网关）。
       * 2026-11-14 01:51:37 真机实测：230s 准点触发后复用同一个 gatewayUrl 重连，
       * 网关 5 秒后直接 op=close 把新会话关掉（旧的仍在推流的会话被当成重复连接），
       * 于是用户看到「230s 黑 3s → 被踢 → 再等 40s 靠 L3 救回」，
       * 比不续命（250s 停推后 ~25s 恢复）更差。
       * 换机位是让服务端重新分配一个 gateway（实测 -06 → -02），代价 ~7s 重建，
       * 但不会撞上「同一机位重复连被踢」。 */
      try {
        var p = this.onReapply();
        if (p && p.catch) p.catch(function () {});
      } catch (e) {
        logWarn('[play] renew onReapply 异常');
      }
    },
    /* autotap 专用：抬起用另一坐标（2,2），模拟真实微动 */
    up2: function () {
      try { up(2, 2, 0, this.vw || VW, this.vh || VH); } catch (e) {}
    },
    /* 每 500ms 由 tick() 调一次。刻意很轻：只比较一个整数。 */
    checkStall: function (now, s) {
      if (this.dead) return;
      /* 退后台期间不自愈：hiddenSince 非 0 说明用户不在看，此时 rx 停涨可能
         只是系统把网络挂起了，这时候去重连/重申请会白烧机位时长，还会和
         后台宽限期（HIDE_GRACE_MS）的拆解抢同一把机位锁。 */
      if (this.hiddenSince) return;
      if (this.busy) return;         /* 别的流程（重连/申请/退出）正在进行 */
      if (this.stallL1Timer) return; /* L1 已发出，正等 5s 复查 */
      /* ★ 交叉互斥：冻结判据的 L1 正在等 9s 复查时，收包判据不许再发起自愈。
         没有这一条时，弱网下两条判据会各发起一次 L1、把共用的 stallTries
         一次吃掉 2 级（表现为「才补了一轮帧就直接重连」）。两条判据共用
         stallTries 上限，就必须共用「正在自愈中」这个门。 */
      if (this.freezeL1Timer) return;
      /* 一次都没收到过 RTP：还没出过画，交给 waiting 那条 20s 慢提示管，
         这里判「停滞」没有意义（正常起播本来就要等几秒）。 */
      if (!this.rx && this.stallTries === 0) {
        this.stallLastRx = 0;
        this.stallSince = 0;
        return;
      }

      /* ① 先把「rx 变化」判掉，**不分 stage**。
         这一条必须排在 stage 判断之前：L2 重连成功后 stage 会依次经过
         connecting → waiting → live，若只在 live 才承认「恢复了」，
         重连真的救回来时 stallTries 清不掉，一个 STALL_RX_MS 后会被误判成第二级停滞。 */
      if (this.rx > this.stallLastRx) {
        this.stallLastRx = this.rx;
        this.stallSince = now;
        /* 只有「收包停滞」这一家才有资格用 rx 增长宣判恢复。
           若当前故障是 weak-net freeze 发起的（stallOwner==='freeze'），
           rx 本来就在涨，这里的「恢复」是假的——放着不管会把升级链打断。 */
        if (this.stallTries > 0 && this.stallOwner === 'rx') {
          var secs = this.stallAt ? Math.round((now - this.stallAt) / 1000) : 0;
          logWarn('[play] stall recovered: rx=' + this.rx + ' 已恢复（第 ' +
            this.stallTries + ' 级自愈成功，停滞 ' + secs + 's）');
          this.stallTries = 0;
          this.stallOwner = '';
          this.stallAt = 0;
          this.tip = '';
        }
        return;
      }
      /* ② rx 后退：换了媒体会话，新的 cgmedia id 从 0 重新计数。
         这是「重新起算」，不是「恢复」。**刻意不清 stallTries**——
         否则「重连 → 20s 又停 → 重连」会无限循环，把机位和免费时长烧光；
         沿用已用掉的次数，下次直接升级到下一级。 */
      if (this.rx < this.stallLastRx) {
        if (this.stallTries > 0) {
          logWarn('[play] stall: 媒体会话重启，rx 重新计数（' + this.stallLastRx +
            ' → ' + this.rx + '），已用自愈 ' + this.stallTries + ' 次，继续观察');
        }
        this.stallLastRx = this.rx;
        this.stallSince = now;
        return;
      }

      /* ③ rx 没变。只有 live 才计时；但若自愈正在途中（stage 已被改成
         waiting/connecting），计时必须**继续**，否则「重连成功连上网关
         却不给流」会永远停在 waiting，再也不会升级到下一级。 */
      /* 当前故障若由「弱网冻结」判据接管（stallOwner==='freeze'），收包判据
         只维护 stallLastRx 基准（上面两个分支已做），完全退出升级链 ——
         两条判据同时动作会互相把对方刚设的 stallTries 改掉。 */
      if (this.stallOwner === 'freeze') return;
      if (s !== 'live' && this.stallTries === 0) {
        this.stallSince = now;
        return;
      }
      if (!this.stallSince) {
        this.stallSince = now;
        return;
      }
      var silentMs = now - this.stallSince;
      if (silentMs < STALL_RX_MS) return;

      var self = this;
      var silent = Math.round(silentMs / 1000);

      /* L1：补关键帧。便宜、无副作用、不打断会话，永远先试这个。 */
      if (this.stallTries === 0) {
        this.stallTries = 1;
        this.stallOwner = 'rx';
        this.stallAt = now;
        logWarn('[play] stall detected: rx=' + this.rx + ' 已 ' + silent +
          's 未增长 → 尝试自愈（第 1 次：补发关键帧）');
        try {
          requestKeyFrameBurst(2, 500);
        } catch (e) {
          logWarn('[play] stall L1 requestKeyFrameBurst 异常');
        }
        this.stallL1Timer = setTimeout(function () {
          self.stallL1Timer = 0;
          if (self.dead) return;
          self.checkStall(Date.now(), self.stage);
        }, STALL_L1_WAIT_MS);
        return;
      }

      /* L2：复用旧 gatewayUrl 重连。这一步会拆掉当前媒体会话，画面会短暂
         消失（1~3 秒），所以只在 L1 明确失败后才做。 */
      if (this.stallTries === 1) {
        this.stallHealL2(now, 'stall', 'rx=' + this.rx + ' 已 ' + silent + 's 未增长');
        return;
      }

      /* L3：连旧网关都拿不到流，重新申请机位（机票/机位可能已被服务端回收）。
         用 STALL_MAX_TRIES - 1 表示「最后一级」，改上限时这一段不用动。 */
      if (this.stallTries === STALL_MAX_TRIES - 1) {
        this.stallHealL3(now, 'stall', 'rx=' + this.rx + ' 已 ' + silent + 's 未增长');
        return;
      }

      /* 三级用完：不再自动重试，交给用户决定。 */
      this.stallHealGiveUp('stall', 'rx=' + this.rx + ' 已 ' + silent + 's 未增长',
        '画面已停止（网关停止推流），自动恢复失败，请点「重连」');
    },

    /* ============ 两条判据共用的自愈动作（第二个参数 tag 区分日志来源） ============
     * tag='stall' = 收包停滞（rx 冻结）；tag='freeze' = 弱网冻结（rx 涨但画面不动）。
     * 抽出来是为了保证两条判据**升级到同一级时动作完全一致**，而不是各写一份、
     * 日后改一处忘另一处。 */
    /* 升级到下一级前统一重开时间窗：收包停滞的 STALL_RX_MS（8s）窗口 + 冻结判据的 gate 窗口
       都要重置，否则重连刚回来还没等到第一个 stat，就会被上一轮的计时立刻判成
       下一级。 */
    stallWindowReset: function (now) {
      this.stallSince = now;
      this.freezeFpsZeroSince = 0;
      this.freezeFpsZeroCnt = 0;
      this.freezeDropBase = -1; /* 新会话的 gateDrop/auDrop 从头算 */
      this.freezeRxBase = 0;
    },
    stallHealL2: function (now, tag, detail) {
      this.stallTries = 2;
      this.stallOwner = tag === 'freeze' ? 'freeze' : 'rx';
      this.stallAt = now;
      this.stallWindowReset(now);
      logWarn('[play] ' + tag + ': ' + detail + ' → L2 复用网关重连');
      /* 画面确实已冻：把 vizOn 摘掉，让中栏遮罩显示「正在重连」，
         同时避免 tick 里 `vizOn && stage==='waiting' → live` 那条兜底
         把 stage 在重连过程中又推回 live。 */
      this.vizOn = false;
      /* canReconnect 在 live+open 时恒 false（它是给按钮用的：能自己玩就别点重连）。
         这里我们已经判定画面死了，只是借它走同一段重连逻辑，故临时把 stage
         挪出 live —— onReconnect 内部第一件事就会把它置成 'connecting'。 */
      this.stage = 'waiting';
      try {
        var p = this.onReconnect();
        if (p && p.catch) p.catch(function () {});
      } catch (e) {
        logWarn('[play] ' + tag + ' L2 onReconnect 异常');
      }
    },
    stallHealL3: function (now, tag, detail) {
      this.stallTries = STALL_MAX_TRIES;
      this.stallOwner = tag === 'freeze' ? 'freeze' : 'rx';
      this.stallAt = now;
      this.stallWindowReset(now);
      logWarn('[play] ' + tag + ': ' + detail + ' → L3 重新申请机位');
      this.vizOn = false;
      try {
        var q = this.onReapply();
        if (q && q.catch) q.catch(function () {});
      } catch (e) {
        logWarn('[play] ' + tag + ' L3 onReapply 异常');
      }
    },
    /* fail() 会释放机位，「重连 / 重新申请」两个按钮在 fail 态恢复可用
       （canReconnect 不再被 live+open 挡掉）。stallReset 放在 fail 之前：
       fail 里也会调一次，幂等，但先清掉可以保证 fail 的日志前面能看到这次
       停滞的收尾。 */
    stallHealGiveUp: function (tag, detail, failText) {
      logWarn('[play] ' + tag + ': 三级自愈全部失败，交给用户（' + detail + '）');
      this.stallReset();
      try {
        this.fail(failText, 0);
      } catch (e) {
        logWarn('[play] ' + tag + ' fail() 异常');
      }
    },

    /* ========= 第二条判据：弱网冻结（包在进 / 画面不动） ========
     * 判据（三条同时成立）：
     *   ① 主时钟是 **fps 连续为 0** 且持续 >= FREEZE_GATE_MS(12s)。
     *      注意不是「gateDrop 连续增长」——native 的 IDR 饥饿兜底
     *      （IDR_STARVE_RELAX_MS = 4000，见 native/csrc/cgmedia/cm_rtp.c）
     *      放行 P 帧后 gateDrop 立即停止增长，只认它会让本判据在 native
     *      兜底之后永久失效（详见下方 ② 与常量块的说明）。
     *   ② 最近连续 freezeFpsZeroCnt 次 stat 采样 fps 全为 0（>= FREEZE_MIN_SAMPLES）。
     *      单次 fps=0 是噪声（2s 窗口里正好没渲染完），连续多次才是真冻结。
     *   ③ 这段全零期内「丢包侧证据」在动：gateDrop+auDrop 比全零起点大，或 rx 还在涨。
     *      两者都不动 = 网关连包都不发了，那是第一条判据（rx 冻结）的地盘，这里让路。
     *   ④ stage==='live'。没出过画的场景交给 waiting 的慢提示，这里不管。
     * 与第一条判据的关系：收包停滞看「rx 不动」，弱网冻结看「rx 动但画面不动」，
     * 互斥、不会同时触发。两者共用 stallTries 计数上限（合计最多 3 次），
     * 但**计时器完全分开**（freezeL1Timer vs stallL1Timer），互不清对方。 */
    checkFreeze: function (now, s) {
      if (this.dead) return;
      if (this.hiddenSince) return;
      if (this.busy) return;
      if (this.freezeL1Timer) return; /* 冻结 L1 已发出，正等 9s 复查 */
      /* 视频平面被我们主动摘掉时（系统键盘弹出 → hideVideoPlane）不判冻结：
         native 收到 hideVideo 会把平面缩到 1x1，恢复时又会把 have_kf / kf_count /
         pli_burst 清零重新「等首个 IDR」（见 native/csrc/cgmedia/cm_cmd.c 的
         setVideoVisible 分支）。那段 gateDrop 增长 + fps=0 是我们自己造成的，
         不是网络冻结 —— 算进来自愈就变成「每次打字关键盘都补一次关键帧」。 */
      if (this.videoHidden) {
        /* 顺带把这一段全零清掉：否则「键盘弹出前已开始的冻结」会把摘平面的
           那几十秒也算进 frozenMs，键盘一关可能立刻误判升级。 */
        this.freezeFpsZeroSince = 0;
        this.freezeFpsZeroCnt = 0;
        this.freezeDropBase = -1;
        this.freezeRxBase = 0;
        return;
      }
      /* 只在「有新 stat 采样」时判定。tick 是 500ms，stat 约 2s 一次；
         按 tick 判会让 fps==0 的采样计数虚高、基准快照被反复重拍。
         计时（freezeFpsZeroSince 的毫秒数）仍然用真实时间，计数单位才是采样。 */
      if (this.statSeq === this.freezeLastSeq) return;
      this.freezeLastSeq = this.statSeq;
      if (s !== 'live') return;       /* 只对「已经出过画」的会话判定 */

      var dropNow = (this.gateDrop || 0) + (this.auDrop || 0);

      /* ⓿ 恢复判定：fps 回到非 0 = 画面动了 = 冻结解除。
         必须放在 gateDrop 判定之前，而且**只认 fps**（不认 gateDrop 停止增长）：
         gateDrop 不涨有两种可能——「画面好了」和「网关连包都不发了」，后者是
         第一条判据（rx 冻结）的地盘，不该在这里被误认为恢复。
         native 侧的 IDR 饥饿兜底（video-integrity 的 IDR_STARVE_RELAX_MS）
         放行 P 帧之后 fps 会恢复，正是走这条路解除。 */
      if ((this.fps | 0) > 0 && this.stallOwner === 'freeze' && this.stallTries > 0) {
        var recoveredSecs = this.freezeZeroAt
          ? Math.round((now - this.freezeZeroAt) / 1000) : 0;
        logWarn('[play] freeze recovered: fps=' + this.fps + ' 画面恢复（第 ' +
          this.stallTries + ' 级自愈成功 / native 兜底放行，冻结 ' + recoveredSecs + 's）');
        this.stallTries = 0;
        this.stallOwner = '';
        this.freezeZeroAt = 0;
        this.tip = '';
      }

      /* ① fps 连续为 0 的采样计数（防毛刺，单次 fps=0 不算）。
         进入一段新的全零段时，同时给「丢包侧证据」拍基准快照：
         freezeDropBase = gateDrop+auDrop，freezeRxBase = rx。 */
      if ((this.fps | 0) <= 0) {
        if (!this.freezeFpsZeroSince) {
          this.freezeFpsZeroSince = now;
          this.freezeFpsZeroCnt = 0;
          this.freezeDropBase = dropNow;
          this.freezeRxBase = this.rx;
        }
        this.freezeFpsZeroCnt++;
      } else {
        /* fps 有值 = 画面在动，本段冻结不成立，基准全清（下次重新拍）。 */
        this.freezeFpsZeroSince = 0;
        this.freezeFpsZeroCnt = 0;
        this.freezeDropBase = -1;
        this.freezeRxBase = 0;
        return;
      }

      /* ② 丢包侧证据：这段全零期内「闸门丢弃/坏 AU 还在涨」或「包还在进（rx 涨）」。
         两者都不成立 = 网关连包都不发了 → 那是第一条判据（rx 冻结）的地盘，这里让路。
         ★ 为什么不是「gateDrop 连续增长」：native 的 IDR 饥饿兜底
           （IDR_STARVE_RELAX_MS = 4000，见 native/csrc/cgmedia/cm_rtp.c）到期会放行
           P 帧，放行后 gateDrop **立即停止增长**，但画面依然可能是死的（放行的 P 帧
           没有参考帧）。只认 gateDrop 连续增长会让本判据在 native 兜底之后永久失效
           （grew 恒 false → 窗口永远攒不满）。auDrop 在兜底放行后仍随坏 AU 增长，
           rx 在网关照推时也照样涨 —— 「任一在涨」+「fps 持续为 0」才是真正的画面死。 */
      var grewDrop = (this.freezeDropBase >= 0) && (dropNow > this.freezeDropBase);
      var grewRx = this.rx > this.freezeRxBase;
      if (!grewDrop && !grewRx) {
        this.freezeFpsZeroSince = 0;
        this.freezeFpsZeroCnt = 0;
        this.freezeDropBase = -1;
        this.freezeRxBase = 0;
        return;
      }

      var frozenMs = now - this.freezeFpsZeroSince;
      if (frozenMs < FREEZE_GATE_MS) return;
      if (this.freezeFpsZeroCnt < FREEZE_MIN_SAMPLES) return;

      var fpsZeroSecs = Math.round(frozenMs / 1000);
      var dropDelta = grewDrop ? (dropNow - this.freezeDropBase) : 0;
      var rxDelta = this.rx - this.freezeRxBase;

      /* L1：连发 3 发 PLI（间隔 400ms）。弱网下单个 PLI 容易连 IDR 请求本身
         都丢掉，多打几发提高命中率；400ms 是为了在 RTT 内不互相覆盖。
         **绝不在这里直接重连**：弱网重连要重走起播（几秒黑屏 + 可能重新排队
         申请机位），代价远大于多等一轮 IDR。 */
      if (this.stallTries === 0) {
        this.stallTries = 1;
        this.stallOwner = 'freeze';
        this.freezeZeroAt = now;
        /* 清空这一段冻结的计时与基准：L1 的 9s 复查由定时器自己判，
           不必再等下一次 stat 攒窗口。 */
        this.freezeFpsZeroCnt = 0;
        this.freezeFpsZeroSince = 0;
        this.freezeDropBase = -1;
        this.freezeRxBase = 0;
        logWarn('[play] freeze: 连续 ' + fpsZeroSecs + 's fps=0，期间' +
          (dropDelta ? ' gateDrop+auDrop +' + dropDelta : '') +
          (rxDelta ? ' rx +' + rxDelta : '') +
          '（rx=' + this.rx + ' 仍在涨，判为丢包打碎 AU、拿不到 IDR）→ L1 补帧');
        try {
          requestKeyFrameBurst(3, 400);
        } catch (e) {
          logWarn('[play] freeze L1 requestKeyFrameBurst 异常');
        }
        var self = this;
        this.freezeL1Timer = setTimeout(function () {
          self.freezeL1Timer = 0;
          if (self.dead) return;
          if (self.hiddenSince || self.busy) return;
          /* ★ 复查由定时器自己做，**不重入 checkFreeze**：
             (a) 重入会让 freeze 判据要求重新攒满 12s 的 gate 窗口，
                 L1→L2 实际要 9+12=21s，与「9s 后仍冻就升级」的设计不符；
             (b) 更致命的是 checkFreeze 开头有 `if (freezeL1Timer) return`，
                 但定时器里此刻已置 0，所以主要还是要靠这里直接判。 */
          if ((self.fps | 0) > 0) {
            /* 画面已经回来了（多为 native 的 IDR 饥饿兜底放行了 P 帧，
               或 PLI 生效拿到了 IDR）。这里必须自己收尾：等待期内
               checkFreeze 是被 freezeL1Timer 挡住的，没人替它做恢复判定。 */
            var backSecs = self.freezeZeroAt
              ? Math.round((Date.now() - self.freezeZeroAt) / 1000) : 0;
            logWarn('[play] freeze recovered: fps=' + self.fps +
              ' 画面恢复（L1 补帧 / native 兜底生效，冻结 ' + backSecs + 's）');
            self.stallTries = 0;
            self.stallOwner = '';
            self.freezeZeroAt = 0;
            self.tip = '';
            return;
          }
          /* 仍是 0 fps：按当前级数直接升级。stallTries 此时必为 1（L1 已用）。 */
          var t = Date.now();
          if (self.stallTries !== 1) return;
          self.stallHealL2(t, 'freeze',
            'fps 仍为 0（L1 补帧 ' + Math.round(FREEZE_L1_WAIT_MS / 1000) +
            's 无效，rx=' + self.rx + ' 仍在涨）');
        }, FREEZE_L1_WAIT_MS);
        return;
      }

      /* L2 / L3：与收包停滞完全同一套动作（见 stallHealL2/L3 注释）。 */
      if (this.stallTries === 1) {
        this.stallHealL2(now, 'freeze',
          'fps 持续为 0 拿不到 IDR（rx=' + this.rx + ' 仍在涨）');
        return;
      }
      if (this.stallTries === STALL_MAX_TRIES - 1) {
        this.stallHealL3(now, 'freeze',
          'fps 持续为 0 拿不到 IDR（rx=' + this.rx + ' 仍在涨）');
        return;
      }
      this.stallHealGiveUp('freeze',
        'fps 连续为 0、rx=' + this.rx + ' 仍在涨',
        '画面已冻结（网络丢包严重，关键帧始终拿不到），自动恢复失败，请点「重连」');
    },
    /* 屏幕是否处于「非亮屏」—— 现在**只是宽限期的加速条件**，不再是拆不拆的总闸。
       判据统一走 isScreenNotOn()：只有「0 = 亮屏」被真机证实
       （docs/SCREEN-STATE-FINDINGS.md §7.1），1/3/4 的语义属 hal-screen 另一层，
       与 JSAPI getScreenState() 不能互证（真机 JS 日志里 state=3/1/4 一条都没有）。
       读不到（null/NaN）返回 false：本函数只用来把 60s 宽限缩到 10s，
       判错的最坏后果是少等 50 秒，不值得为它冒"把亮屏误判成熄屏"的风险。
       第二个参数传 null 是刻意的：这里拿不到亮度后端，而 isScreenNotOn 在
       「state 读不到 + 没有亮度」时恰好返回 false，正好是这个保守分支。 */
    screenIsOff: function () {
      var st = null;
      try {
        st = getScreenState();
      } catch (e) {
        return false;
      }
      return isScreenNotOn(st, null);
    },
    screenText: function () {
      var st = -1;
      try {
        st = getScreenState();
      } catch (e) {
        return 'err';
      }
      if (st == null) return 'null';
      return Number(st) + '(' + screenStateName(st) + ')';
    },
    /* 进入后台（onHide / onStop 共用）：只做可逆动作 + 起宽限期。
       两条生命周期的先后顺序在真机上不保证（有时先 onHide 后 onStop，有时只来一个），
       所以这里必须幂等：已经在宽限期里就不再重置计时起点，否则两条回调
       互相把 hiddenSince 往后推，宽限期会被无限续命。 */
    enterBackground: function (from) {
      if (this.dead) return;
      var now = Date.now();
      var first = !this.hiddenSince;
      if (first) this.hiddenSince = now;
      /* 摘平面是幂等的（hideVideoPlane 内部有 videoHidden 标志），重复调用无害。
         这一步必须在起定时器之前：KMS 平面 zpos 高于系统合成面，哪怕只停留
         一帧也会盖住用户正要看的系统 UI。 */
      this.hideVideoPlane();
      /* 保活**不停**：onHide 不等于不玩了。旧代码在这里 keepAliveStop()，
         一旦 onHide 只是熄屏/短暂切走，保活一停屏幕就真的黑下去再也回不来，
         那是把手电筒关了再找手电筒。保活只在真正 bgTeardown 时才停。 */
      if (!first) {
        logWarn('[play] ' + from + ' 已在后台宽限期内 hiddenFor=' +
          Math.round((now - this.hiddenSince) / 1000) + 's，不重置计时');
        return;
      }
      this.armHideGrace();
    },
    /* 起/重置后台宽限期。屏幕确实非亮屏时用短的 10s（用户按电源键回来要解锁，
       不会立刻回来），否则 60s。定时器句柄存 this.hideGraceTimer，
       cleanup / onShow / 到期自己都会清，防止页面销毁后还有回调打进来。 */
    armHideGrace: function () {
      if (this.dead) return;
      this.clearHideGrace();
      var self = this;
      var ms = HIDE_GRACE_MS;
      var dark = false;
      try {
        dark = self.screenIsOff();
      } catch (e) {}
      if (dark) ms = HIDE_GRACE_DARK_MS;
      logWarn('[play] 进入后台宽限期 ' + Math.round(ms / 1000) + 's（screen=' +
        self.screenText() + ' stage=' + self.stage + '）');
      this.hideGraceTimer = setTimeout(function () {
        self.hideGraceTimer = 0;
        if (self.dead) return;
        /* 到点前又回来了：hiddenSince 已被 onShow 清零，这里直接作废。 */
        if (!self.hiddenSince) return;
        logWarn('[play] 后台宽限期到期 hiddenFor=' +
          Math.round((Date.now() - self.hiddenSince) / 1000) + 's → 拆管线释放机位');
        self.clearTimers();
        /* 保活终于可以停了：会话马上要拆，继续续期只会白耗电、
           还可能和 stopGame 抢同一把机位锁。 */
        try {
          keepAliveStop();
        } catch (e) {}
        self.bgTeardown();
      }, ms);
    },
    clearHideGrace: function () {
      if (this.hideGraceTimer) {
        clearTimeout(this.hideGraceTimer);
        this.hideGraceTimer = 0;
      }
    },
    // 后台/退出软件时的兜底拆解：必须同步走到 __cgMedia.stop()，
    // 因为 stopPlay() 的首段全是同步调用（clearInterval + media.stop + ws.close），
    // 后续 Promise 链即使被挂起也不影响平面已经释放。
    bgTeardown: function () {
      var self = this;
      if (self.dead) return;
      /* 系统键盘弹出会触发 onHide/onStop：这不是真退后台，不能拆管线。 */
      if (self.imBusy || self.kbOpen) {
        logWarn('[play] bgTeardown 跳过：系统输入法使用中');
        return;
      }
      /* 注意：这里**不再**判 screenIsOff()。
         旧代码在这里又说「屏幕熄了就别拆」，结果和 onHide 的守卫叠成两层：
         宽限期到期时若屏幕恰好还是非亮屏，就会跳过拆解 → 会话残留、机位
         永不释放、平面一直挂在系统上（比误拆更糟，也更难自愈）。
         熄屏的豁免已经上移到 enterBackground 里用「缩短宽限期」表达，
         走到这里就代表"用户确实离开超过 60s（或熄屏超过 10s）"，该拆了。 */
      self.clearHideGrace();
      self.hiddenSince = 0;
      self.stallReset();
      /* 宽限期到期 = 我们自己判定的"该拆了"（不是被宿主干掉），记进归因字段，
       * 这样 cleanup 日志里能区分「后台超时拆解」和「无人认领的销毁」。 */
      self.selfClosedBy = 'bgTeardown';
      var s = self.stage;
      var playing = s === 'live' || s === 'waiting' || s === 'connecting';
      logWarn('[play] bgTeardown 拆管线 stage=' + s + ' viz=' + (self.vizOn ? 1 : 0));
      try {
        var p = stopPlay();
        if (p && p.catch) p.catch(function () {});
      } catch (e) {}
      if (!playing) return;
      self.vizOn = false;
      self.stage = 'end';
      self.tip = '';
      self.errText = '已切到后台，本局已结束并释放机位';
      try {
        var q = self.code ? stopGame(self.code) : null;
        if (q && q.catch) q.catch(function () {});
      } catch (e) {}
      try {
        var r = self.accumulate();
        if (r && r.catch) r.catch(function () {});
      } catch (e) {}
    },
    cleanup: function () {
      if (this.dead) return;
      this.dead = true;
      /* STALLGUARD：先把 L1 的复查定时器清掉 —— dead 之后它还会 fire 一次，
         虽然 checkStall 开头有 dead 守卫，但清干净比依赖守卫更可靠。 */
      this.stallReset();
      this.destroyedAt = Date.now();
      /* 销毁归因：这一行是排查「对局中途页面没了」的第一现场证据。
       * by 为空 = 没有任何我们自己发起的退出路径走过（= 被宿主摘掉/重载）；
       * 另外带上 stage/phase/已玩时长/im 状态/touch 状态，方便对着时间线回溯。 */
      var _st = this.stage;
      var _ms = mediaState();
      var _played = this.startAt ? Math.round((this.destroyedAt - this.startAt) / 1000) : 0;
      var _imPend = this.im && this.im.pending ? 1 : 0;
      var _by = this.selfClosedBy || 'none(外部销毁)';
      logWarn('[play] cleanup（beforeDestroy）by=' + _by +
        ' stage=' + _st + ' phase=' + (_ms && _ms.phase) + ' ws=' + (_ms && _ms.wsState) +
        ' played=' + _played + 's viz=' + (this.vizOn ? 1 : 0) +
        ' hidden=' + (this.hiddenSince ? Math.round((this.destroyedAt - this.hiddenSince) / 1000) + 's' : '0') +
        ' imBusy=' + (this.imBusy ? 1 : 0) + ' kb=' + (this.kbOpen ? 1 : 0) + ' imPend=' + _imPend +
        ' busy=' + (this.busy ? 1 : 0) + ' videoHidden=' + (this.videoHidden ? 1 : 0));
      /* 页面销毁时系统键盘可能还挂着：必须解除忙碌标志，否则 app.js 的
         onHide 守卫会把之后每一次真实的退后台都当成"在打字"而放过去。 */
      try {
        setInputBusy(false);
      } catch (e) {}
      try {
        if (this.im) this.im.dispose();
      } catch (e) {}
      this.videoHidden = false;
      this.hiddenSince = 0;
      /* 宽限期定时器必须在这里清掉：页面销毁后 JS 上下文还在（页面栈里被
         closePageByName 摘掉时确实会清，但 beforeDestroy 这条路径不一定），
         残留的 setTimeout 会在几十秒后对着死页面调 bgTeardown。 */
      this.clearHideGrace();
      this.clearTimers();
      try {
        setEventHandler(null);
      } catch (e) {}
      try {
        keepAliveStop();
      } catch (e) {}
      /* 先把视频平面缩成 1x1，再拆管线 —— 顺序是刻意的：
       * kmssink 在 set_state(NULL) 时只会按 restore-crtc 恢复 **CRTC mode**，
       * 不会把 weston 自己的 plane 重新挂回 crtc。正常路径下 zpos=2 的
       * Esmart1 被解绑后 weston 还能重新提交；但真机见过 plane_mask 掉到 0、
       * 两个平面全解绑、weston 也回不来的情况（详见 docs/BUGFIX-play-page-teardown.md）。
       * 缩 1x1 是 native 已支持且验证过的动作（cm_cmd.c setVideoVisible），
       * 它把「绑着但没人管」的窗口压到最小，也让拆管线这一步的 KMS 状态更干净。
       * 这里失败不阻塞后续：能缩就缩，缩不了也照样拆。 */
      try {
        setVideoVisible(false);
      } catch (e) {}
      try {
        var p = stopPlay();
        if (p && p.catch) p.catch(function () {});
      } catch (e) {}
      // 关键：'queued' 时不能 stopGame —— 本页是排队的发起方，销毁本页后由 queue 页
      // 继续轮询同一个排队位；这里再发一次 DELETE 会把刚申请的排队位取消掉。
      // 'nologin' / 'boot' 时也从未申请过机位，无需释放。
      var st = this.stage;
      if (this.code && st !== 'queued' && st !== 'nologin' && st !== 'boot') {
        try {
          var q = stopGame(this.code);
          if (q && q.catch) q.catch(function () {});
        } catch (e) {}
      }
      try {
        var r = this.accumulate();
        if (r && r.catch) r.catch(function () {});
      } catch (e) {}
    },
    /* ================= 云游戏内文本输入 =================
     * 链路（全部对照官方 run.js 的 op=input 下行分支实现）：
     *   网关 cmd "1" → 弹系统键盘（ImSession→global.startTextEdit）
     *                → 用户输入的文字用 cmd "5 <text>" 逐次送回远端
     *   网关 cmd "2" → 收键盘
     *
     * 两个非显然的坑：
     * 1) KMS 视频平面 zpos=2，**盖在系统键盘所用的合成面之上**。不把画面藏起来，
     *    用户看到的就是"游戏画面还在、键盘没出来"。所以弹键盘前先
     *    setVideoVisible(false)（native 把 render-rectangle 缩到 1x1），收键盘后
     *    setVideoVisible(true)（native 销毁重建 vbox + 补 PLI 求 IDR）。
     * 2) 系统键盘是**另一个全屏 miniapp**，弹出时 Falcon 会给本 App 发 onHide，
     *    而 app.js 的 onHide 会 stopPlay() —— 刚点开输入框整局就被拆了。
     *    所以全程用 setInputBusy(true/false) 打标，app.js 据此跳过。
     */
    openRemoteKb: function () {
      if (this.dead) return;
      /* 远端连发两次 cmd 1 是可能的（玩家来回点输入框），已在弹就忽略，
         否则会把用户正在打的字冲掉。 */
      if (this.kbShow || this.imBusy || this.kbOpen) {
        logWarn('[play] 忽略重复的 cmd 1（键盘已显示）');
        return;
      }
      /* ★ 默认走右栏自绘数字键盘，而不是系统键盘。
         系统键盘是另一个全屏 miniapp，zpos 比 KMS 视频平面低，必须先把视频
         缩成 1x1 才能看见；而 kmssink 的 render-rectangle 只能缩不能放
         （PenBili/session.c:4166 六轮对照：放大只有「管线过一次 READY」有效），
         恢复代价高且实测网关不肯补 IDR，画面常常回不来。
         数字键盘放右栏 244 宽里 —— KMS 挖孔只有中间 473 宽，右栏完全在画面
         之外，天然不遮挡，视频照常播，玩家还能边看边输。
         手机号/验证码都是纯数字，数字键盘够用；真要打汉字再点「系统键盘」。 */
      this.kbNums = '';
      this.kbTip = '';
      this.kbTitle = '输入数字';
      this.kbShow = true;
      this.kbOpen = true;
      logInfo('[play] 远端请求输入 → 显示数字键盘（不遮挡视频）');
    },
    /* ---- 数字键盘：官方 run.js 的发送方式 ----
       文字走 cmd "5 <text>"（onInput 逐次发 e.data），
       按键走 cmd "6 <code>"，其中 8→1 退格、13→2 回车/确认（onKeyup 映射）。 */
    kbDigit: function (d) {
      if (this.dead) return;
      this.kbNums = String(this.kbNums || '') + String(d);
      this.kbTip = '';
      try {
        sendText(String(d));
      } catch (e) {
        logWarn('[play] 发送数字失败 ' + ((e && e.message) || e));
      }
    },
    kbDel: function () {
      if (this.dead) return;
      var s = String(this.kbNums || '');
      if (s.length > 0) this.kbNums = s.slice(0, s.length - 1);
      this.kbTip = '';
      try {
        sendKey(1);
      } catch (e) {
        logWarn('[play] 发送退格失败 ' + ((e && e.message) || e));
      }
    },
    kbOk: function () {
      if (this.dead) return;
      var n = String(this.kbNums || '').length;
      try {
        sendKey(2);
      } catch (e) {
        logWarn('[play] 发送确认失败 ' + ((e && e.message) || e));
      }
      logInfo('[play] 数字键盘确认 len=' + n);
      this.kbShow = false;
      this.kbOpen = false;
      this.kbNums = '';
      this.kbTip = '';
    },
    /* 兜底：改回系统键盘（要打汉字、或远端要求非数字输入时）。
       这条路径才需要藏视频平面，所以代价还是老的。 */
    kbSystem: function () {
      var self = this;
      if (this.dead) return;
      this.kbShow = false;
      this.imBusy = true;
      try {
        setInputBusy(true);
      } catch (e) {}
      this.hideVideoPlane();
      logInfo('[play] 切换到系统键盘 → 隐藏视频平面');
      setTimeout(function () {
        self.askRemoteText();
      }, 250);
    },
    askRemoteText: function () {
      var self = this;
      var done = function () {
        self.imBusy = false;
        self.kbOpen = false;
        try {
          setInputBusy(false);
        } catch (e) {}
        self.showVideoPlane();
        /* 恢复后必须重新声明一次平台：vbox 是新建的，
           kmssink 侧的字幕/force-aspect 等由 native 自己重置，这里只需重发平台声明。 */
        try {
          declarePlatform('android');
        } catch (e) {}
      };
      this.askText({ value: '', placeholder: '输入要发到游戏里的文字', maxlength: 64 }, 'remote')
        .then(function (r) {
          var txt = (r && r.text) || '';
          if (r && r.confirmed && txt) {
            /* 官方是逐次把 e.data（本次新字符）用 "5 <text>" 发出去；
               我们一次拿到整串，按整串发（远端输入框会整体替换成这个值的样子，
               实测原神/第五人格都接受整串）。 */
            try {
              sendText(txt);
              logInfo('[play] 已回传文字 len=' + txt.length);
            } catch (e) {
              logWarn('[play] 回传文字失败 ' + ((e && e.message) || e));
            }
          } else {
            logWarn('[play] 用户取消输入 confirmed=' + (r && r.confirmed));
          }
          done();
        })
        .catch(function (e) {
          logWarn('[play] 输入异常 ' + ((e && e.message) || e));
          done();
        });
    },
    closeRemoteKb: function () {
      /* 远端收起输入框：把还在等的系统键盘关掉，然后恢复画面。 */
      logInfo('[play] 远端收起输入框 → 关闭键盘并恢复画面');
      /* 远端主动收键盘：两种键盘都要收掉。数字键盘只是 UI，不涉及视频平面；
         系统键盘才需要把画面恢复回来。 */
      this.kbShow = false;
      this.kbNums = '';
      this.kbTip = '';
      try {
        if (this.im && this.im.isPending && this.im.isPending()) {
          this.im.cancelPending('remote-close', true);
        }
      } catch (e) {}
      this.kbOpen = false;
      try {
        setInputBusy(false);
      } catch (e) {}
      this.showVideoPlane();
    },
    /* 隐藏/恢复都做成幂等的：cmd 1 与 cmd 2 可能交错到达（玩家快速点开又关掉），
       而 showVideoPlane 的代价是销毁重建整条管线 + 等一个 IDR，重复调用会闪黑屏。 */
    hideVideoPlane: function () {
      if (this.videoHidden) return;
      this.videoHidden = true;
      try {
        setVideoVisible(false);
      } catch (e) {}
    },
    showVideoPlane: function () {
      if (!this.videoHidden) return;
      this.videoHidden = false;
      try {
        setVideoVisible(true);
      } catch (e) {}
    },
    /* 与 login.vue 的 askText 同一范式（含 IME 空结果竞态重试）：
       ImSession.open() 开头会 _closeCurrent()，那个关闭动作会异步抛一个空
       textEditFinished；若它落在本次 pending 建好之后，新 promise 会被直接
       解析成 {confirmed:false,text:''}。判据是「极快返回空结果」= 人不可能
       800ms 内打完字并确认。 */
    askText: async function (opts, field) {
      if (!this.im) return { confirmed: false, text: '' };
      var r = null;
      for (var attempt = 0; attempt < 3; attempt++) {
        var t0 = Date.now();
        try {
          r = await this.im.open(opts);
        } catch (e) {
          logWarn('[play] im throw ' + ((e && e.message) || e));
          return { confirmed: false, text: '', error: 'IM_THROW' };
        }
        var cost = Date.now() - t0;
        var empty = !r || (!r.confirmed && !r.text);
        if (!empty || (r && r.error) || cost > 800 || (r && r.canceled)) break;
        logWarn('[play] im 空结果竞态 cost=' + cost + 'ms attempt=' + attempt + '，重开(' + field + ')');
        await new Promise(function (res) { setTimeout(res, 250); });
      }
      return r || { confirmed: false, text: '' };
    },

    /* 端游（gameType==='pc'）必须走鼠标协议，走触摸协议（cmd 1/2/3）远端不认：
       真机实测发 5 组不同坐标，远端画面变化量恒为 97px 且 bbox 完全一致，
       说明触摸事件被丢在同一个落点。官方 send_message_pc 的点击是
       「mousemove -> 50ms -> mousedown -> 50ms -> mouseup」，只发 down/up 不生效。 */
    pcClick: function (t, holdMs) {
      var p = this.toVideo(t);
      var hold = holdMs || 0;
      if (hold > 300) return;
      var vx = p.vx;
      var vy = p.vy;
      var self = this;
      /* 官方点击节奏是「mousemove -> 50ms -> mousedown -> 50ms -> mouseup」。
         三条 input 连发时，网关/容器侧可能在指针还没落位就收到 down，
         远端看到的是一次无位移点击，点击就丢了。这里按官方节奏串起来。 */
      try {
        mouseMove(vx, vy, self.vw || VW, self.vh || VH, 0);
      } catch (e) {}
      self.setTimeout(function () {
        try {
          mouseDown(vx, vy, self.vw || VW, self.vh || VH, 0, 0);
        } catch (e) {}
        self.setTimeout(function () {
          try {
            mouseUp(vx, vy, self.vw || VW, self.vh || VH, 0, 0);
          } catch (e) {}
        }, 60);
      }, 60);
    },
    pcDrag: function (t) {
      var p = this.toVideo(t);
      try {
        mouseMove(p.vx, p.vy, this.vw || VW, this.vh || VH, 0);
      } catch (e) {}
    },
    /* 触摸是否需要「已出画」才放行？
     * 原来写的是 `if (!this.vizOn) return;`。真机踩过坑：vizOn 由 cgmedia 的
     * media 事件置位，而那个事件以前要求音频也 ready 才发 —— 遇到只发视频不发
     * 音频的网关时 vizOn 永远 false，玩家点击被**全部静默丢弃**（画面在播但点
     * 不动）。native 侧已放宽为「视频首帧即发」，这里再补一层：只要已经进入
     * live/waiting 阶段就放行，vizOn 只用来决定遮罩显示，不再当点击闸门。 */
    touchReady: function () {
      if (this.vizOn) return true;
      var s = this.stage;
      return s === 'live' || s === 'waiting' || s === 'connecting';
    },
    onTS: function (e) {
      /* ★ 这一行曾经在清理临时诊断日志时被误删，导致 onTS 引用未定义的 `ts`
         直接抛 ReferenceError —— 触摸按下事件从头到尾没发出去，连带 touchend
         里 touchIds 为空也被跳过，表现就是「画面在播但屏幕怎么点都没反应」。
         任何时候改动 onTS 都要确认这一行还在。 */
      var ts = (e && e.changedTouches) || [];
      if (!this.touchReady()) return;
      for (var i = 0; i < ts.length; i++) {
        this.touchDown(ts[i]);
      }
    },
    onTM: function (e) {
      if (!this.touchReady()) return;
      var ts = (e && e.changedTouches) || [];
      if (this.gameType === 'pc') {
        for (var j = 0; j < ts.length; j++) this.pcDrag(ts[j]);
        return;
      }
      for (var i = 0; i < ts.length; i++) {
        var t = ts[i];
        var key = String((t && t.identifier) === undefined ? '0' : t.identifier);
        var id = this.touchIds[key];
        if (id === undefined) continue;
        var p = this.toVideo(t);
        try {
          move(p.vx, p.vy, id, this.vw || VW, this.vh || VH);
        } catch (err) {}
      }
    },
    onTE: function (e) {
      var ts = (e && e.changedTouches) || [];
      if (this.gameType === 'pc') {
        for (var j = 0; j < ts.length; j++) this.pcClick(ts[j], Date.now() - (this.pcDownAt || 0));
        return;
      }
      for (var i = 0; i < ts.length; i++) {
        var t = ts[i];
        var key = String((t && t.identifier) === undefined ? '0' : t.identifier);
        var id = this.touchIds[key];
        if (id === undefined) continue;
        var p = this.toVideo(t);
        try {
          up(p.vx, p.vy, id, this.vw || VW, this.vh || VH);
        } catch (err) {}
        this.dropTouch(key);
      }
    },
    onTC: function (e) {
      try {
        cancel();
      } catch (e) {}
      this.touchIds = {};
      this.touchCount = 0;
    },
    touchDown: function (t) {
      if (!t) return;
      var key = String(t.identifier === undefined ? '0' : t.identifier);
      if (!this.touchIds) this.touchIds = {};
      if (this.touchCount === undefined) this.touchCount = 0;
      var id = this.touchIds[key];
      if (id === undefined) {
        if (this.touchCount >= MAX_TOUCH) return;
        id = this.freeId();
        this.touchIds[key] = id;
        this.touchCount += 1;
      }
      var p = this.toVideo(t);
      try {
        down(p.vx, p.vy, id, this.vw || VW, this.vh || VH);
      } catch (e) {}
    },
    dropTouch: function (key) {
      if (this.touchIds && this.touchIds[key] !== undefined) {
        delete this.touchIds[key];
        this.touchCount = Math.max(0, (this.touchCount || 1) - 1);
      }
    },
    freeId: function () {
      for (var i = 0; i < MAX_TOUCH; i++) {
        var used = false;
        for (var k in this.touchIds) {
          if (this.touchIds[k] === i) {
            used = true;
            break;
          }
        }
        if (!used) return i;
      }
      return 0;
    },
    toVideo: function (t) {
      /* ★ pageX/pageY 已经是「stage 内」坐标，不要再减 VIDEO.x/VIDEO.y。
         真机实测铁证（三栏布局 VIDEO.x=243）：面板 (111,480) → 逻辑 (480,4)
         → 回调 page=(237,3)，237 = 480-243 正好是 stage 内横坐标，3≈4 是纵向。
         说明 Falcon 的 pageX 横向/纵向都已相对接收事件的元素归一化：
         pageX ∈ [0,473] 对应 stage 水平，pageY ∈ [0,266] 对应 stage 垂直。
         视频缓冲已被 mppvideodec rotation=270 摆正，所以 stage 水平就是视频水平，
         直接按 VIDEO.w/h 归一化即可（历史上唯一一次「减偏移」的改动是错的，已撤回）。 */
      var x = Number(t && t.pageX);
      var y = Number(t && t.pageY);
      if (!isFinite(x)) x = 0;
      if (!isFinite(y)) y = 0;
      if (x < 0) x = 0;
      if (x > VIDEO.w) x = VIDEO.w;
      if (y < 0) y = 0;
      if (y > VIDEO.h) y = VIDEO.h;
      // 必须用出画上报的真实分辨率（data.vw/vh 由 media 事件写入），不能用常量 VW/VH
      // 兜底：不同游戏会出 720x1280 / 1600x900 / 1920x1080，写死 1280x720 会让点击整体偏移。
      var vw = Number(this.vw) || VW;
      var vh = Number(this.vh) || VH;
      /* 坐标系换算（数学推导 + 真机实测双重确认）
       *
       *   原始帧 1280x720 --mppvideodec rotation=270--> 缓冲 720x1280
       *   缓冲 --KMS 缩放到面板 rect[266x473]--> 面板（面板被系统转了 90°）--> 用户所见 473x266
       *
       * 逐级代数展开（详见 HANDOFF 的推导）后，两级 90° 旋转正好抵消，得到：
       *     raw_x = vw * (u / 473)
       *     raw_y = vh * (v / 266)
       * 也就是**线性同轴映射**，与官方 getPercentPos 的语义一致（按显示画面的百分比取值）。
       * 交叉轴、双轴取反都在真机上验证无效，不要再改回去。
       *
       * pageX/pageY 已实测是 stage 内坐标（点面板 x=207 → 逻辑 x=300 → page.x=57=300-243），
       * 所以直接用 u=x、v=y，不要减 VIDEO.x/VIDEO.y。 */
      return {
        vx: Math.round((x / VIDEO.w) * vw),
        vy: Math.round((y / VIDEO.h) * vh)
      };
    }
  }
};
</script>
<style lang="less" scoped>
@import "base.less";

/* 播放态三栏：左 243 + 中 473（KMS 视频挖孔）+ 右 244 = 960，高度统一 266。
   铁律：.stage 必须紧接在 .p2-left 之后，宽度分毫不差 —— KMS 硬件平面按 native
   传过去的绝对 rect 挖孔（px=107+rect_y，py=960-rect_x-rect_w），DOM 顺序一错就错位。
   铁律：.stage 自身保持完全透明（画面从这里透出来），只有未出画时的 .mask 不透明。 */

/* ---------- 左栏 243x266 ---------- */
.p2-left {
  width: 25.3125vw;
  height: 100vh;
  flex-direction: column;
  padding-left: 1.25vw;
  padding-right: 1.25vw;
  padding-top: 3.0076vh;
  padding-bottom: 0.7519vh;
  background-color: @nav-background;
}

/* 品牌图标 + 游戏名两行（名字由 JS 截断成 nameL1/nameL2，text 不许自动换行） */
.p2-lhead {
  height: 13.5339vh;
  flex-direction: row;
  align-items: center;
}

.p2-logo {
  width: 2.7084vw;
  height: 9.7745vh;
  border-radius: 0.625vw;
  background-color: transparent;
}

.p2-namebox {
  width: 18.8542vw;
  height: 12.782vh;
  flex-direction: column;
  justify-content: center;
  margin-left: 0.8334vw;
  overflow: hidden;
}

.p2-name {
  height: 6.391vh;
  font-size: 1.3542vw;
  color: @text-color-strong;
  font-weight: 700;
}

/* phase · ws · ice 调试条 */
.p2-lstrip {
  height: 5.2632vh;
  margin-top: 2.2557vh;
  padding-left: 0.625vw;
  padding-right: 0.625vw;
  border-radius: 0.4167vw;
  justify-content: center;
  overflow: hidden;
  background-color: @background-color;
}

.p2-lstrip-text {
  font-size: 0.9375vw;
  color: @text-color-dim;
}

/* 地区 + 当前画质 */
.p2-lrow {
  height: 6.767vh;
  flex-direction: row;
  align-items: center;
  margin-top: 2.2557vh;
}

.p2-chipgap {
  margin-left: 0.625vw;
}

.p2-ltime {
  height: 9.0226vh;
  flex-direction: row;
  align-items: center;
  justify-content: space-between;
  margin-top: 3.0076vh;
}

.p2-ltime-label {
  font-size: 1.0417vw;
  color: @text-color-dim;
}

.p2-ltime-value {
  font-size: 1.5625vw;
  color: @gold;
  font-weight: 700;
}

.p2-lmsg {
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0px;
  flex-direction: column;
  margin-top: 3.0076vh;
  overflow: hidden;
}

.p2-lmsg-main {
  height: 6.0151vh;
  font-size: 1.25vw;
  color: @text-color;
}

.p2-lmsg-main-err {
  height: 6.0151vh;
  font-size: 1.25vw;
  color: @danger;
}

.p2-lmsg-sub {
  height: 5.2632vh;
  font-size: 1.0417vw;
  color: @text-color-dim;
  margin-top: 0.7519vh;
}

.p2-lstat {
  height: 5.2632vh;
  justify-content: center;
  overflow: hidden;
}

.p2-lstat-text {
  font-size: 0.9375vw;
  color: @text-color-dim;
}

/* ---------- 中栏：视频挖孔 473x266 ----------
 * 宽度 49.2709vw / 高度 100vh 与 JS 里的 var VIDEO = { x:243, y:0, w:473, h:266 }
 * 等价（473/960、266/266）。用 vw/vh 而不用 px，是为了让三栏 25.3125 + 49.2709 +
 * 25.4167 = 100vw 在任何画布宽度下都严丝合缝；挖孔用的绝对值仍只由 JS 那份
 * VIDEO 常量提供，CSS 不参与 KMS 坐标计算。 */
.stage {
  /* 中栏宽度 = 473/960。这里用 vw 而不是 px，是为了让容器缩放时左右两栏
     与挖孔始终严丝合缝（三者相加恰好 100vw）。 */
  width: 49.2709vw;
  height: 100vh;
  flex-direction: column;
  justify-content: center;
  align-items: center;
  background-color: rgba(0, 0, 0, 0);
}

/* 未出画占位：必须不透明盖满 473x266，否则会透出上一页内容 */
.mask {
  width: 49.2709vw;
  height: 100vh;
  background-color: #191F29;
  justify-content: center;
  align-items: center;
}

.p2-mask-box {
  width: 43.75vw;
  flex-direction: column;
  align-items: center;
}

.p2-mask-title {
  font-size: 1.7708vw;
  color: @text-color-strong;
  font-weight: 700;
  text-align: center;
}

.p2-mask-dots {
  height: 8.2707vh;
  font-size: 1.7708vw;
  color: @accent;
  font-weight: 700;
  text-align: center;
  margin-top: 0.7519vh;
}

.p2-mask-sub {
  font-size: 1.25vw;
  color: @text-color-dim;
  text-align: center;
  margin-top: 1.5038vh;
}

.p2-mask-err {
  font-size: 1.25vw;
  color: @danger;
  text-align: center;
  margin-top: 3.0076vh;
}

/* ---------- 右栏 244x266 ---------- */
.p2-right {
  width: 25.4167vw;
  height: 100vh;
  flex-direction: column;
  background-color: @background-color;
}

/* ---------- 数字键盘（右栏 244x266 内，完全不遮挡 KMS 视频平面） ----------
   竖向预算：padding 8+8 = 16，内容 250。
   标题 20 + 回显 26 + 四行 (46*4) + 行间 4*3 = 20+26+184+12 = 242，余 8 留白。
   横向预算：244 - padding 20 = 224；键 70*3 + 间距 6*2 = 222 ≤ 224。 */
.p2-kb {
  width: 25.4167vw;
  height: 100vh;
  flex-direction: column;
  padding-left: 1.0417vw;
  padding-right: 1.0417vw;
  padding-top: 3.0076vh;
  padding-bottom: 3.0076vh;
  background-color: @background-color;
}

.p2-kbhead {
  width: 23.3334vw;
  height: 7.5188vh;
  flex-direction: row;
  justify-content: space-between;
  align-items: center;
}

.p2-kbtitle {
  font-size: 1.3542vw;
  color: @text-color-strong;
}

.p2-kbsys {
  width: 6.4584vw;
  height: 6.767vh;
  border-radius: 0.9375vw;
  border-width: 1px;
  border-style: solid;
  border-color: @card-border-color;
  justify-content: center;
  align-items: center;
}

.p2-kbsys-text {
  font-size: 1.1458vw;
  color: @text-color-dim;
}

.p2-kbdisp {
  width: 23.3334vw;
  height: 9.7745vh;
  margin-top: 0.7519vh;
  border-radius: 0.625vw;
  background-color: @ctrl-background;
  justify-content: center;
  align-items: center;
}

.p2-kbval {
  font-size: 1.5625vw;
  color: @accent;
}

.p2-kbrow {
  width: 23.3334vw;
  height: 17.2933vh;
  flex-direction: row;
  justify-content: space-between;
  margin-top: 1.5038vh;
}

.p2-kbkey {
  width: 7.2917vw;
  height: 17.2933vh;
  border-radius: 0.8334vw;
  background-color: @card-background-color;
  border-width: 1px;
  border-style: solid;
  border-color: @card-border-color;
  justify-content: center;
  align-items: center;
}

.p2-kbnum {
  font-size: 2.0833vw;
  color: @text-color-strong;
}

.p2-kbdel {
  background-color: @ctrl-background;
}

/* 绿底必须纯白字（用户明确要求过，见 README「主题」） */
.p2-kbok {
  background-color: @accent;
  border-color: @accent;
}

.p2-kbok-text {
  font-size: 1.6667vw;
  color: #FFFFFF;
}

.p2-rbody {
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0px;
  flex-direction: column;
  padding-left: 1.25vw;
  padding-right: 1.25vw;
  padding-top: 3.0076vh;
  padding-bottom: 0.7519vh;
}

/* 画质四档 2x2：106*2 + 6 = 218 ≤ 219（244 减左右 padding 各 12，border-box） */
.p2-qrow {
  height: 16.5414vh;
  flex-direction: row;
}

.p2-qrow2 {
  margin-top: 3.0076vh;
}

.p2-qbtn {
  width: 11.0417vw;
  height: 16.5414vh;
  border-radius: 0.8334vw;
  justify-content: center;
  align-items: center;
}

/* 只剩一档画质时铺满整行（244 - 12*2 = 220），视觉上是一个状态条而不是按钮组 */
.p2-qbtn-solo {
  width: 22.9167vw;
  background-color: @card-background-color;
  border-width: 1px;
  border-style: solid;
  border-color: @accent;
}

.p2-gap {
  margin-right: 0.625vw;
}

.p2-qbtn-lock {
  background-color: @background-color;
  border-width: 1px;
  border-style: solid;
  border-color: @card-border-color;
}

.p2-qbtn-onlock {
  background-color: @card-background-color;
  border-width: 1px;
  border-style: solid;
  border-color: @gold;
}

.p2-qbtn-text {
  font-size: 1.25vw;
  color: @text-color;
}

.p2-qbtn-text-on {
  font-size: 1.25vw;
  color: #FFFFFF;
  font-weight: 700;
}

.p2-qbtn-text-lock {
  font-size: 1.1458vw;
  color: @text-color-dim;
}

/* 画质提示恒定占位，避免出现/消失时按钮跳动 */
.p2-rhint {
  height: 6.0151vh;
  margin-top: 2.2557vh;
  justify-content: center;
}

.p2-rhint-text {
  font-size: 1.0417vw;
  color: @gold;
}

/* 码率 / 帧率 / 丢包 / 分辨率：拆两行写，避免长文本在 220px 里被截 */
.p2-rstat {
  flex-direction: column;
  margin-top: 3.0076vh;
}

.p2-rstat-main {
  height: 4.8873vh;
  font-size: 1.0417vw;
  color: @accent-blue;
}

.p2-rstat-dim {
  height: 4.5113vh;
  font-size: 0.9375vw;
  color: @text-color-dim;
  margin-top: 0.7519vh;
}

.p2-rfill {
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0px;
}

/* 三个操作按钮：69*3 + 6*2 = 219 ≤ 220 */
.p2-racts {
  height: 13.5339vh;
  flex-direction: row;
  margin-top: 2.2557vh;
}

.p2-act {
  width: 7.1875vw;
  height: 13.5339vh;
  border-radius: 0.8334vw;
  justify-content: center;
  align-items: center;
  background-color: @ctrl-background;
}

/* 后定义，覆盖 .p2-act 的底色（同为单类名，靠顺序取胜） */
.p2-act-main {
  background-color: @accent;
}

.p2-act-danger {
  background-color: @danger;
}

.p2-act-lock {
  background-color: @ctrl-background;
}

.p2-act-text {
  font-size: 1.25vw;
  color: @text-color;
}

.p2-act-text-on {
  font-size: 1.25vw;
  color: #FFFFFF;
  font-weight: 700;
}

.p2-act-text-lock {
  font-size: 1.25vw;
  color: @text-color-dim;
}

.p2-rfoot {
  height: 6.0151vh;
  justify-content: center;
}

.p2-rfoot-text {
  font-size: 0.9375vw;
  color: @text-color-dim;
}

/* 退出确认：KMS 盖住中间视频区，弹层只能落在右栏 244 宽里 */
.p2-confirm {
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0px;
  flex-direction: column;
  padding-left: 1.6667vw;
  padding-right: 1.6667vw;
  padding-top: 3.7594vh;
  padding-bottom: 3.7594vh;
  justify-content: center;
}

.p2-ctitle {
  height: 8.2707vh;
  font-size: 1.6667vw;
  color: @text-color-strong;
  font-weight: 700;
}

.p2-csub {
  height: 6.0151vh;
  font-size: 1.1458vw;
  color: @text-color-dim;
  margin-top: 0.7519vh;
}

.p2-cplayed {
  height: 6.767vh;
  font-size: 1.25vw;
  color: @gold;
  margin-top: 3.0076vh;
}

/* 底色由 p2-act-main / p2-act-danger 给，这里不写 background-color，否则会覆盖掉 */
.p2-cbtn {
  width: 22.0834vw;
  height: 13.5339vh;
  border-radius: 0.8334vw;
  justify-content: center;
  align-items: center;
  margin-top: 3.7594vh;
}
</style>
