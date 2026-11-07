// 屏幕保活（播放态必须，否则 270s 自动灭屏 / 300s 背光断电）
// 平台能力挂在 $falcon.jsapi.brightness（扁平），也可通过 import('brightness') 拿到。
// 真机教训：keepScreenOn() 只续「不动作窗口」，不恢复已掉下去的背光，
// 所以判据要用 getScreenState()，必要时再 screenOnOff(1)。
import { logInfo, logWarn } from './log.js';

const state = {
  mod: null,
  tried: false,
  timer: 0,
  lastState: -1,
  warns: 0
};

function flat() {
  const f = typeof $falcon !== 'undefined' ? $falcon : null;
  const b = f && f.jsapi && f.jsapi.brightness;
  if (b && typeof b.keepScreenOn === 'function') return b;
  return null;
}

export async function initScreen() {
  if (state.tried) return state.mod;
  state.tried = true;
  let mod = flat();
  if (!mod) {
    try {
      const m = await import('brightness');
      const cand = m && m.default && typeof m.default.keepScreenOn === 'function' ? m.default : m;
      if (cand && typeof cand.keepScreenOn === 'function') mod = cand;
    } catch (e) {
      logWarn('[screen] brightness 模块不可用');
    }
  }
  state.mod = mod;
  return mod;
}

// 一次性的「请勿息屏」
export function keepOnce() {
  const m = state.mod || flat();
  if (!m) return false;
  try {
    if (typeof m.keepScreenOn === 'function') {
      m.keepScreenOn();
      return true;
    }
  } catch (e) {
    /* ignore */
  }
  return false;
}

export function screenStateCode() {
  const m = state.mod || flat();
  if (!m || typeof m.getScreenState !== 'function') return -1;
  try {
    const v = m.getScreenState();
    return typeof v === 'number' ? v : Number(v) || -1;
  } catch (e) {
    return -1;
  }
}

export function screenOn() {
  const m = state.mod || flat();
  if (!m) return;
  try {
    if (typeof m.screenOnOff === 'function') m.screenOnOff(1);
  } catch (e) {
    /* ignore */
  }
}

// 播放态保活：每 intervalMs 续一次；发现掉进非亮屏就补一次开屏
export function keepAliveStart(intervalMs) {
  keepAliveStop();
  const ms = intervalMs || 20000;
  const tick = () => {
    keepOnce();
    const st = screenStateCode();
    if (st !== state.lastState) {
      state.lastState = st;
      logInfo('[screen] state=' + st);
    }
    if (st > 0) {
      if (state.warns < 3) {
        state.warns++;
        logWarn('[screen] 屏幕已非亮屏 state=' + st + '，尝试唤醒');
      }
      screenOn();
      keepOnce();
    }
  };
  tick();
  state.timer = setInterval(tick, ms);
  logInfo('[screen] 保活启动 ' + ms + 'ms');
}

export function keepAliveStop() {
  if (state.timer) {
    clearInterval(state.timer);
    state.timer = 0;
    logInfo('[screen] 保活停止');
  }
}

export function screenDiag() {
  const m = state.mod || flat();
  return {
    available: !!m,
    hasKeep: !!(m && typeof m.keepScreenOn === 'function'),
    hasState: !!(m && typeof m.getScreenState === 'function'),
    hasOnOff: !!(m && typeof m.screenOnOff === 'function'),
    state: screenStateCode()
  };
}
