// 检查更新的**视图状态机**（与 services/updater.js 的分工：
// updater.js 只管「怎么查、怎么下、怎么装」，本文件只管「弹窗现在该显示什么」）。
//
// 为什么要把状态机单独放一个模块，而不是像 PenBili 那样直接写在页面里：
//   PenCG 的「我的」页是 components/mine-body.vue 这个**组件**，它被
//   pages/index/index.vue（内嵌栏目）和 pages/mine/mine.vue（老入口薄壳）共用；
//   而自动检查更新发生在 index.vue 的 boot() 里（那时「我的」栏目可能还没渲染，
//   mine-body 根本没挂载）。所以：
//     - 入口在 mine-body（组件里的一张卡片）
//     - 弹窗在页面级（整页替换，自动检测也用它）
//   两边要共享同一份状态，只能提到模块层。
//
// 为什么用模块单例 + 回调，而不是 props / $emit：
//   本运行时全仓没有一处 props 或 $emit（PenCG 与 PenBili 都是），
//   nav.js 的注释里也写了「没有可用的跨页 Vue 事件总线」。
//   现有唯一被验证过的父子通道是 `$root.__mineBody`（mine-body 自己挂上去的），
//   但那条通道只在组件已挂载时成立。模块级回调是同一页面上下文里的普通函数调用，
//   没有渲染/生命周期依赖，最稳。
//
// 同一页面内的模块状态是共享的（nav.js 踩过的坑是**跨页面**不共享，
// 这里 index.vue 与 mine-body.vue 在同一个页面 bundle 里，属于同一份模块实例）。

import {
  fetchLatestRelease,
  isNewer,
  bodyLines,
  shouldAutoPrompt,
  sourceById,
  sourceLabel,
  downloadUrl,
  downloadFileToDisk,
  installAmr,
  loadUpdateState,
  rememberSkip,
  rememberQuiet,
  progressText,
  updateCapability,
  SAVE_DIR,
  SOURCES,
  REPO_URL
} from './updater.js';
import { APP_VERSION } from './version.js';
import { logWarn } from './log.js';

/** 弹窗的初始状态（每次开窗都从这份重建，避免上一轮的 tip/message 残留） */
export function initialView() {
  return {
    visible: false,
    mode: 'manual',      // 'auto' 进软件时自动弹 / 'manual' 用户手点
    source: SOURCES[0].id,
    stage: 'idle',       // idle|checking|latest|ready|downloading|installing|downloaded|error
    message: '',
    release: null,
    lines: [],
    tip: '',
    filePath: ''
  };
}

let view = initialView();
const listeners = [];

/** 当前状态快照（只读，别直接改，走 setView） */
export function getView() {
  return view;
}

/**
 * 注册一个状态接收者。fn(nextView) 会在每次状态变化时被同步调用。
 * @param {(v:object)=>void} fn
 */
export function bindView(fn) {
  if (typeof fn !== 'function') return;
  for (let i = 0; i < listeners.length; i++) if (listeners[i] === fn) return;
  listeners.push(fn);
  fn(view);
}

/** 注销（组件 beforeDestroy 里必须调，否则销毁后的回调会写到已释放的 vm 上） */
export function unbindView(fn) {
  for (let i = listeners.length - 1; i >= 0; i--) {
    if (listeners[i] === fn) listeners.splice(i, 1);
  }
}

/** 合并更新状态并广播。整体替换成新对象，保证响应式能看见变化。 */
export function setView(patch) {
  view = Object.assign({}, view, patch || {});
  /* 遍历快照而不是 listeners 本身：回调里可能触发组件销毁 -> beforeDestroy
   * 里 unbindView 会 splice 数组，直接在原数组上 for 循环会漏掉下一个监听者。
   * （Vue 的销毁是下一 tick 才发生，实际很难踩到，但快照的代价是 0。） */
  const list = listeners.slice();
  for (let i = 0; i < list.length; i++) {
    try {
      list[i](view);
    } catch (e) {
      logWarn('[cg] upd listener fail: ' + ((e && e.message) || e));
    }
  }
  return view;
}

/** 关窗（不动持久状态：下次进软件还会提醒） */
export function closeView() {
  setView({ visible: false, stage: 'idle' });
}

/* ============================ 动作 ============================ */

/**
 * 手动/自动检查更新。
 *
 * 手动检测永远弹窗（用户主动点了就得有反馈，包括「已是最新」），
 * 自动检测由调用方先过 shouldAutoPrompt 三关（见 autoCheckUpdate）。
 *
 * @param {'manual'|'auto'} mode
 */
export async function checkUpdate(mode) {
  const m = mode === 'manual' ? 'manual' : 'auto';
  setView(Object.assign(initialView(), {
    visible: true,
    mode: m,
    source: SOURCES[0].id,
    stage: 'checking',
    message: '正在检查最新正式版本…'
  }));
  let r = null;
  try {
    r = await fetchLatestRelease();
  } catch (e) {
    r = { ok: false, stage: 'transport', message: (e && e.message) || String(e) };
  }
  if (!r.ok) {
    logWarn('[cg] upd check fail: ' + r.stage + ' ' + (r.message || ''));
    setView({
      stage: 'error',
      message: r.message || '网络错误',
      tip: '检查失败不影响正常使用；稍后可再试一次'
    });
    return getView();
  }
  if (!isNewer(r.release.tag, APP_VERSION)) {
    setView({
      source: r.source || getView().source,
      stage: 'latest',
      message: '当前 v' + APP_VERSION + ' 已是线上最新的正式版本（' + r.release.tag + '）',
      release: r.release,
      /* 已是最新时也把**本版本**的发布说明摆出来 —— 用户点「检查更新」多半
       * 就是想看看这版改了什么，左栏空着一句占位反而像坏了。 */
      lines: bodyLines(r.release.body),
      tip: ''
    });
    return getView();
  }
  return showReady(m, r.release, r.source);
}

/** 「发现新版本」：把发布说明切成行（<text> 里不能有换行，见 base.less 铁律） */
export function showReady(mode, release, source) {
  const src = source || getView().source || SOURCES[0].id;
  const cap = updateCapability();
  const v = setView({
    visible: true,
    mode: mode === 'auto' ? 'auto' : 'manual',
    source: src,
    stage: 'ready',
    message: '新版本 ' + release.tag + ' · 当前 v' + APP_VERSION,
    release: release,
    lines: bodyLines(release.body),
    tip: cap.canInstall ? '' : '当前版本不支持自动安装，下载后需手动安装',
    filePath: ''
  });
  logWarn('[cg] upd ready ' + release.tag + ' src=' + src +
          ' asset=' + (release.assetName || '-') + ' size=' + (release.assetSize || 0) +
          ' canInstall=' + (cap.canInstall ? 1 : 0));
  return v;
}

/**
 * 进入软件时的自动检测（静默失败：检查更新绝不能打断正常使用）。
 * 三道闸：①取不到版本 ②不是更新版本 ③用户跳过了该版本 / 今天内不提示。
 */
export async function autoCheckUpdate() {
  try {
    const st = await loadUpdateState();
    const r = await fetchLatestRelease();
    if (!r.ok) {
      logWarn('[cg] upd auto check fail: ' + r.stage + ' ' + (r.message || ''));
      return null;
    }
    if (!shouldAutoPrompt(st, r.release.tag, APP_VERSION, Date.now())) {
      logWarn('[cg] upd auto suppressed (skip=' + (st.skipVersion || '-') +
              ' quiet=' + (st.quietDay || '-') + ' remote=' + r.release.tag + ')');
      return null;
    }
    return showReady('auto', r.release, r.source);
  } catch (e) {
    logWarn('[cg] upd auto check error: ' + ((e && e.message) || e));
    return null;
  }
}

/** 「跳过此版本」：持久化到 storage，只有出现**更高**版本时才会再提醒 */
export async function onSkip() {
  const r = getView().release;
  if (!r) return;
  await rememberSkip(r.tag);
  logWarn('[cg] upd skip ' + r.tag);
  closeView();
}

/** 「今天内不提示」：按**本地日期**记一天（用户理解的「今天」是设备时区的今天） */
export async function onQuiet() {
  const st = await rememberQuiet(Date.now());
  logWarn('[cg] upd quiet until tomorrow (' + st.quietDay + ')');
  closeView();
}

/**
 * 「查看发布页」：词典笔上没有能唤起浏览器的 JSAPI（全仓 grep 确认），
 * 所以只能把地址显示出来让用户自己抄/扫码，不能假装能跳转。
 */
export function onShowUrl() {
  const r = getView().release;
  const url = (r && r.htmlUrl) || REPO_URL;
  setView({ tip: url });
  logWarn('[cg] upd release page: ' + url);
}

/**
 * 换源下载：按指定平台重新取 Release 再直连下载。
 * 两个平台的网络可达性互补（GitCode 国内直连稳、GitHub 在部分网络完全打不开），
 * 互为备用；但**都走直连**，不套镜像。
 *
 * @param {string} onlyId 'gitcode' | 'github'
 */
export async function installFrom(onlyId) {
  const id = onlyId || getView().source;
  const cur = getView().release;
  // 目标源就是当前命中的源、包也已经在手 -> 不用再请求一次
  if (id === getView().source && cur && cur.assetUrl) {
    return doInstall();
  }
  const tag = cur ? cur.tag : '-';
  setView({
    stage: 'checking',
    message: '正在从 ' + sourceLabel(id) + ' 获取 ' + tag + '…',
    tip: ''
  });
  logWarn('[cg] upd switch source -> ' + id + ' for ' + tag);
  const r = await fetchLatestRelease(id);
  if (!r.ok || !r.release || !r.release.assetUrl) {
    setView({
      stage: 'ready',
      message: '新版本 ' + tag + ' · 当前 v' + APP_VERSION,
      tip: sourceLabel(id) + ' 暂时取不到该版本，可点另一个按钮重试'
    });
    logWarn('[cg] upd switch source fail ' + id + ': ' + (r.message || '-'));
    return getView();
  }
  // 两个平台的发布顺序不保证同步：目标平台上可能还没有这个 tag。
  // 这时**不能**把手上那个源的包当作「从目标源下载」去下 —— 日志和提示都会说谎，
  // 用户也会以为 GitHub 能通（实际根本没连到 GitHub）。宁可明确报「还没有」。
  const sameOrNewer = r.release.tag === tag || isNewer(r.release.tag, tag);
  if (!sameOrNewer) {
    setView({
      stage: 'ready',
      message: '新版本 ' + tag + ' · 当前 v' + APP_VERSION,
      tip: sourceLabel(id) + ' 上还没有 ' + tag + '，可点另一个按钮'
    });
    logWarn('[cg] upd switch source: ' + id + ' has ' + r.release.tag +
            ' < ' + tag + ' -> keep ' + getView().source);
    return getView();
  }
  setView({
    source: r.source,
    stage: 'ready',
    release: r.release,
    message: '新版本 ' + r.release.tag + ' · 当前 v' + APP_VERSION
  });
  return doInstall();
}

/**
 * 下载 + 安装。altSourceId 给了且首源失败时，换另一个平台再试一次：
 * 「我的」页手动检测只有两个按钮，用户没法自己换源，必须有兜底。
 *
 * @param {string} [altSourceId]
 */
export async function doInstall(altSourceId) {
  const r = getView().release;
  if (!r || !r.assetUrl) {
    setView({ tip: '该版本没有可用安装包，请到发布页手动下载' });
    return getView();
  }
  const ok = await tryInstall(r, sourceLabel(getView().source));
  if (ok || !altSourceId) return getView();
  /* 下载成功但本机没有安装通道时，**不能**换源重试：换源只会把同一个包再下一次，
   * 用户的观感是「点了两次都在转圈」，而真正的原因（native 缺 installAmr）
   * 一点都没变。tryInstall 把这种情况停在 stage:'downloaded'，这里直接收工。 */
  if (getView().stage === 'downloaded') return getView();
  // 换源重试：换的是**源**，不是镜像前缀
  const alt = sourceById(altSourceId);
  setView({ stage: 'checking', message: '正在从 ' + alt.label + ' 重试…', tip: '' });
  logWarn('[cg] upd retry via source ' + alt.id);
  const r2 = await fetchLatestRelease(alt.id);
  if (!r2.ok || !r2.release || !r2.release.assetUrl) {
    setView({ stage: 'error', message: '两个平台都取不到安装包', tip: '请到发布页手动下载' });
    return getView();
  }
  setView({
    source: r2.source,
    stage: 'ready',
    release: r2.release,
    message: '新版本 ' + r2.release.tag + ' · 当前 v' + APP_VERSION
  });
  await tryInstall(r2.release, alt.label);
  return getView();
}

/**
 * 单个源上的一次完整「下载 -> 安装」。成功返回 true；失败返回 false 并把
 * 阶段的 message/tip 写好（调用方决定要不要换源重试）。
 *
 * PenCG 适配点：installAmr 目前恒返回 NO_INSTALLER（native 没有导出安装接口）。
 * 这时不能把流程判成「安装失败」吓用户 —— 包已经在盘上了，改成
 * stage:'downloaded'，把落盘路径原样显示出来，让有 adb 的用户自己装。
 */
async function tryInstall(rel, label) {
  const url = downloadUrl(rel.assetUrl, false);
  if (!url) {
    setView({ stage: 'error', message: '没有可用的下载地址', tip: '请到发布页手动下载' });
    return false;
  }
  setView({
    stage: 'downloading',
    message: '正在从 ' + label + ' 下载 ' + (rel.assetName || '安装包') + '…',
    tip: progressText(0, rel.assetSize)
  });
  logWarn('[cg] upd download via ' + label + ' 直连: ' + url);
  const d = await downloadFileToDisk(url);
  if (!d.ok) {
    logWarn('[cg] upd download fail via ' + label + ': ' + (d.message || ''));
    setView({
      stage: 'error',
      message: '下载失败：' + (d.message || '未知错误'),
      tip: d.code === 'NO_DOWNLOADER' ? '可到发布页手动下载安装' : (d.message || '')
    });
    return false;
  }
  logWarn('[cg] upd downloaded via ' + label + ' -> ' + d.filePath);
  setView({
    stage: 'installing',
    message: '下载完成，正在安装…',
    tip: d.filePath || '',
    filePath: d.filePath || ''
  });
  const ins = await installAmr(d.filePath);
  if (ins.code === 'NO_INSTALLER') {
    logWarn('[cg] upd install unsupported, amr kept at ' + (d.filePath || SAVE_DIR));
    setView({
      stage: 'downloaded',
      message: '安装包已下载到设备',
      tip: (d.filePath || SAVE_DIR) + ' · 当前版本不支持自动安装'
    });
    return false;
  }
  if (!ins.ok) {
    logWarn('[cg] upd install fail: ' + (ins.message || '') + ' rc=' + (ins.rc == null ? '?' : ins.rc));
    setView({
      stage: 'error',
      message: '安装失败：' + (ins.message || '未知错误'),
      tip: '安装包已下载到 ' + (d.filePath || SAVE_DIR)
    });
    return false;
  }
  logWarn('[cg] upd install done ' + rel.tag);
  setView({ stage: 'done', message: '已安装 ' + rel.tag, tip: '重新打开软件即可使用新版本' });
  return true;
}
