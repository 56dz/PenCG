import { BasePage } from './base-page.js';
import { stopPlay, mediaState, currentGameCode, inputBusy } from './services/media.js';
import { stopGame } from './services/session.js';
import { getScreenState, screenStateName, isScreenNotOn } from './services/screen.js';
import { bootProfile, FALLBACK_WIDTH, FALLBACK_HEIGHT, readCustomEnv } from './services/device_profile.js';
import { DEV_DESIGN_WIDTH } from './dev-autostart.js';

// 逻辑画布宽度来自设备 profile（真机截屏 + cfg.json 证实）：
//   有道 X6 Pro ：Falcon UI 合成面横向条带 960x266，物理面板 480x960（direction=270）
//   有道 X7 Pro ：参考项目实测合成面 936x280（部分固件报 960x280）—— **PenCG 未验证**
// setViewPort(W) 把 W 个设计像素铺满合成面宽度，因此 W 必须等于合成面宽度，
// 否则整页被等比缩放且**纵向裁掉底部**。
//
// 所以这里不再写死 960，改为**在 onLaunch 里同步**问一次 device_profile：
//   · 探测成功（$falcon.env 报出可信的视口尺寸）→ 用实测宽度；
//   · 探测失败/不可信 → bootProfile() 内部已经退回 FALLBACK_WIDTH=960，
//     也就是说「读不到」与「改动前」完全等价，绝不会因为探测出错而整页缩放。
//
// 为什么不在模块顶层求值，而是拖到 onLaunch：
//   顶层求值发生在 App 类实例化之前，此时框架可能还没跑完
//   JQuickFalconEnv::init()，$falcon.env 存在但字段为空 —— 那样会静默落到
//   兜底 960，探测永远「看起来是好的却不生效」。onLaunch 是 setViewPort
//   唯一有效的时机，也是 env 一定已就绪的最早时机，两边对齐。
//   仍然必须是**同步**取值：storage 之类的异步通道在这里来不及
//   （参考项目 PenBili 踩过这个坑）。
let DESIGN_WIDTH = FALLBACK_WIDTH;
let BOOT = null;

class App extends $falcon.App {
  constructor() {
    super();
  }

  onLaunch(options) {
    super.onLaunch(options);
    /* 屏幕适配验收开关：0 = 关闭（量产恒为 0，见 dev-autostart.js）。
     * 必须在 bootProfile() 之前写，因为它读的就是 custom 通道。
     *
     * ★ 关掉时必须主动清除，不能只「不写」★
     * custom 通道是持久化的：拿 720 应过一版验收包之后，只要不显式清，
     * 后续量产包每次启动都会读到 720，画布宽度被悄悄改掉（真机实测过：
     * 日志 width=720 source=custom，挖孔 rect 变成 182,33,355,200，
     * 而物理合成面仍是 960 宽，整页被缩放且视频区错位）。
     * 这里只在「读到的值确实非 0」时才写，避免每次启动都白写一次存储。 */
    try {
      if (DEV_DESIGN_WIDTH > 0) {
        $falcon.env.custom.$set('pencgDesignWidth', DEV_DESIGN_WIDTH);
      } else if (Math.round(Number(readCustomEnv('pencgDesignWidth')) || 0) !== 0) {
        $falcon.env.custom.$set('pencgDesignWidth', 0);
      }
    } catch (e) {}
    try {
      BOOT = bootProfile();
      DESIGN_WIDTH = BOOT.cfg.width > 0 ? BOOT.cfg.width : FALLBACK_WIDTH;
    } catch (e) {
      BOOT = null;
      DESIGN_WIDTH = FALLBACK_WIDTH;
    }
    this.setViewPort(DESIGN_WIDTH);
    $falcon.useDefaultBasePageClass(BasePage);
    /* 每次启动都留一条设备档案日志：这是机型适配唯一的现场证据。
     * 判据：source=env 说明 $falcon.env 真的读到了可信视口；source=fallback
     * 说明探测没成（或因闸门判为不可信被丢弃），此时行为与本改动前一致。
     * source=custom 表示用了 $falcon.env.custom.pencgDesignWidth 覆盖；
     * source=force 表示有人把 device_profile.js 的 FORCE_WIDTH 写死了。 */
    try {
      const d = (BOOT && BOOT.device) || null;
      console.error('[pencg][I] [device] width=' + DESIGN_WIDTH +
        ' height=' + ((BOOT && BOOT.cfg.height) || FALLBACK_HEIGHT) +
        ' source=' + ((BOOT && BOOT.source) || 'exception') +
        ' model=' + ((BOOT && BOOT.model) || '-') +
        ' raw=' + (d ? (d.width + 'x' + d.height) : 'null') +
        ' envModel=' + ((d && d.model) || '-') +
        ' platform=' + ((d && d.platform) || '-') +
        ' api=' + ((d && d.apiVersion) || '-') +
        ' note=' + ((BOOT && BOOT.note) || 'bootProfile 抛异常') +
        (BOOT && BOOT.layoutFixed ? ' ★layoutFixed(base.less 仍是 960 布局)' : ''));
    } catch (e) {}
    try { console.error('[pencg][life] app onLaunch viewport=' + DESIGN_WIDTH); } catch (e) {}
  }

  onShow() {
    super.onShow();
    try { console.error('[pencg][life] app onShow'); } catch (e) {}
  }

  // ★ App 级兜底：小程序被切到后台 / 用户直接退出软件时，页面级 onHide/onUnload
  // 真机实测**不会回调**（`miniapp_cli start <别的 appid>` 之后日志里一条都没有），
  // 于是 play 页的 stopPlay() 永远跑不到，而 KMS 视频平面是硬件 overlay，
  // zpos 高于系统 UI —— 最后一帧就一直盖在系统界面上（用户报障原话：
  // "画面为什么没有消失，我游戏都结束了，软件都退出了"）。
  //
  // stopPlay() 的前三步（clearInterval / __cgMedia.stop / wsClose）全是同步调用，
  // 即使后面 Promise 链被系统挂起也不影响 kmssink 已经把 plane 还回去。
  onHide() {
    super.onHide();
    let screenSt = null;
    try { screenSt = getScreenState(); } catch (e) {}
    try {
      console.error('[pencg][life] app onHide screen=' +
        (screenSt == null ? 'null' : screenSt + '(' + screenStateName(screenSt) + ')'));
    } catch (e) {}
    /* ★ 系统输入法守卫。
     * Falcon 的系统键盘是另一个全屏 miniapp，弹出/收起时本 App 会收到 onHide。
     * 这里若照常 stopPlay()，用户在云游戏里刚点开远端输入框、键盘刚起来，
     * 整局就被拆掉（视频平面消失、机位释放）—— 绝对是灾难。
     * play 页在弹键盘前后用 setInputBusy(true/false) 打标；PenBili 用同样的
     * imBusy 守卫（src/pages/index/index.vue:3298-3310）。 */
    let busy = false;
    try { busy = inputBusy(); } catch (e) {}
    if (busy) {
      try { console.error('[pencg][life] app onHide 忽略：系统输入法使用中'); } catch (e) {}
      return;
    }
    /* ★★ bug1（约 4 分钟后画面停顿/像自动退出）★★
     * 这台机器的 /etc/input-event-daemon.conf [Idle] 段实测是
     *     270s = hal-screen down;  300s = len_onoff.sh off; hal-screen off
     * 也就是 4 分 30 秒时系统自己把背光拉下来、5 分钟断电熄屏。
     * 熄屏同样会触发 App onHide —— 但它**不是**「用户把小程序切走了」，
     * 处理必须相反：真退后台要拆 KMS 平面（否则最后一帧永久盖住系统 UI），
     * 熄屏则万不能拆（拆了就是用户看到的那次"自动退出"）。
     *
     * 判据统一走 isScreenNotOn()：只依赖「0 = 亮屏」这一个被真机证实的事实
     * （docs/SCREEN-STATE-FINDINGS.md §7.1）。旧的 `n === 3 || n === 1` 里 3 在
     * JSAPI 层毫无证据，一旦它不出现，这个守卫就恒假 → 系统息屏被当成真退后台 →
     * 直接 stopGame 释放机位，正是 bug1 的观感。
     *
     * 读不到（null/NaN）时按「没熄」走保守路径 —— 宁可多拆一次，也不要让 KMS
     * 平面永久盖在系统桌面上（HANDOFF.md 里 GST_STATE_NULL_ 那个坑的邻居）。
     * 这里第二个参数传 null 而不是去读亮度：app 级没有亮度上下文，而
     * isScreenNotOn 在「state 读不到 + 没有亮度」时恰好返回 false，
     * 正好落在这个保守分支上，不需要额外判断。
     *
     * 注：屏幕保活的重新武装交给 play 页（它持有 stage），这里只做收尾判定。 */
    let screenOff = false;
    try {
      screenOff = isScreenNotOn(screenSt, null);
    } catch (e) {}
    if (screenOff) {
      try {
        console.error('[pencg][life] app onHide 判为系统息屏（screen=' + screenSt +
          '(' + screenStateName(screenSt) + ')）→ 不拆会话，交给 play 页重新保活');
      } catch (e) {}
      return;
    }
    // 先看会话有没有真的在跑，再决定要不要连带退机位：app 级这里拿不到
    // play 页的 stage（排队中不能退机位，否则刚排到的号会丢），只能问 media 的状态机。
    let streaming = false;
    try {
      const st = mediaState() || {};
      const ph = String(st.phase || '');
      streaming = ph === 'connecting' || ph === 'authing' || ph === 'waiting_offer' ||
        ph === 'answering' || ph === 'playing';
    } catch (e) {}
    const code = (function () { try { return currentGameCode(); } catch (e) { return ''; } })();
    try {
      console.error('[pencg][life] app onHide 真退后台 → stopPlay streaming=' +
        (streaming ? 1 : 0) + ' code=' + (code || '-'));
    } catch (e) {}
    try {
      const p = stopPlay();
      if (p && p.catch) p.catch(function () {});
    } catch (e) {}
    if (streaming && code) {
      try {
        const q = stopGame(code);
        if (q && q.catch) q.catch(function () {});
      } catch (e) {}
    }
  }

  onDestroy() {
    super.onDestroy();
  }
}

export default App;
