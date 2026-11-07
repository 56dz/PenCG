// native 模块加载器（QuickJS 动态 import，模块由 libs/libjsapi_*.so 提供）
import { logInfo, logWarn } from './log.js';

const state = {
  net: null,
  netTried: false,
  media: null,
  mediaTried: false,
  netErr: '',
  mediaErr: ''
};

function pick(mod, names) {
  if (!mod) return null;
  if (typeof mod === 'object' && names.some((n) => typeof mod[n] === 'function')) return mod;
  const d = mod.default;
  if (d && names.some((n) => typeof d[n] === 'function')) return d;
  return null;
}

// cgnative：http(S) 裸 body + 加密原语
export async function loadNative() {
  if (state.netTried) return state.net;
  state.netTried = true;
  try {
    const m = await import('cgnative');
    const mod = pick(m, ['postSync', 'startPost', 'sha256Hex']);
    if (!mod) {
      state.netErr = 'cgnative 未导出预期函数';
      logWarn('[native] cgnative 模块无有效导出');
      return null;
    }
    state.net = mod;
    logInfo('[native] cgnative 就绪');
  } catch (e) {
    state.netErr = (e && e.message) || String(e);
    logWarn('[native] cgnative 加载失败:', state.netErr);
  }
  return state.net;
}

// cgmedia：WebRTC + GStreamer 出画
export async function loadMedia() {
  if (state.mediaTried) return state.media;
  state.mediaTried = true;
  try {
    const m = await import('cgmedia');
    const mod = pick(m, ['start', 'poll', 'cmd']);
    if (!mod) {
      state.mediaErr = 'cgmedia 未导出预期函数';
      logWarn('[native] cgmedia 模块无有效导出');
      return null;
    }
    state.media = mod;
    logInfo('[native] cgmedia 就绪');
  } catch (e) {
    state.mediaErr = (e && e.message) || String(e);
    logWarn('[native] cgmedia 加载失败:', state.mediaErr);
  }
  return state.media;
}

export function nativeReady() {
  return { http: !!state.net, media: !!state.media };
}

export function nativeDiag() {
  return {
    http: !!state.net,
    media: !!state.media,
    netErr: state.netErr,
    mediaErr: state.mediaErr
  };
}

// 同步调用 helper：native 返回 JSON 字符串，这里解析成对象
export function callJSON(mod, fn, args) {
  if (!mod || typeof mod[fn] !== 'function') return null;
  let raw;
  try {
    raw = mod[fn].apply(mod, args || []);
  } catch (e) {
    return { __error: (e && e.message) || String(e) };
  }
  if (raw == null) return null;
  if (typeof raw === 'object') return raw;
  try {
    return JSON.parse(raw);
  } catch (e) {
    return { __raw: raw };
  }
}
