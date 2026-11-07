import { BasePage } from './base-page.js';
import { stopPlay, mediaState, currentGameCode, inputBusy } from './services/media.js';
import { stopGame } from './services/session.js';

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
    try { console.warn('[pencg][life] app onShow'); } catch (e) {}
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
    try { console.warn('[pencg][life] app onHide'); } catch (e) {}
    /* ★ 系统输入法守卫。
     * Falcon 的系统键盘是另一个全屏 miniapp，弹出/收起时本 App 会收到 onHide。
     * 这里若照常 stopPlay()，用户在云游戏里刚点开远端输入框、键盘刚起来，
     * 整局就被拆掉（视频平面消失、机位释放）—— 绝对是灾难。
     * play 页在弹键盘前后用 setInputBusy(true/false) 打标；PenBili 用同样的
     * imBusy 守卫（src/pages/index/index.vue:3298-3310）。 */
    let busy = false;
    try { busy = inputBusy(); } catch (e) {}
    if (busy) {
      try { console.warn('[pencg][life] app onHide 忽略：系统输入法使用中'); } catch (e) {}
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
