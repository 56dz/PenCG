import { BasePage } from './base-page.js';
import { stopPlay, mediaState, currentGameCode, inputBusy } from './services/media.js';
import { stopGame } from './services/session.js';
import { getScreenState, screenStateName, isScreenNotOn } from './services/screen.js';

// 逻辑画布宽度来自设备 profile（真机截屏 + cfg.json 证实）：
//   有道 X6 Pro ：Falcon UI 合成面横向条带 960x266，物理面板 480x960（direction=270）
// setViewPort(W) 把 W 个设计像素铺满合成面宽度，因此 W 必须等于合成面宽度，
// 否则整页被等比缩放且**纵向裁掉底部**。
const DESIGN_WIDTH = 960;

class App extends $falcon.App {
  constructor() {
    super();
  }

  onLaunch(options) {
    super.onLaunch(options);
    this.setViewPort(DESIGN_WIDTH);
    $falcon.useDefaultBasePageClass(BasePage);
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
