import { logInfo, logWarn } from './log.js';

// 页面导航 + 参数传递（$falcon.navTo 的薄封装）
// 用法：navTo('detail', { code: 'mrfzzmd' })
const PAGES = {
  index: 'index',
  detail: 'detail',
  search: 'search',
  mine: 'mine',
  login: 'login',
  play: 'play',
  queue: 'queue',
  diag: 'diag'
};

let pending = null;

/* 「回退到 index 的某个栏目」专用通道。
 *
 * 为什么不能复用 navTo('index', {tab:'mine'})：登录页是 navTo 进来的，
 * 用户点返回必须走 navBack()，否则会再压一层 index、把首页已加载好的
 * 数据全丢掉、返回键也要按两次。而 navBack() 没有参数位。
 *
 * 所以这里单独存一个「下一次 index 显示时要落在哪个栏目」的标记，
 * index.vue 的 onShow 用 takeTabParam() 取一次即销。
 * 加 30s 过期：防止标记残留到很久以后一次莫名其妙的跳转。
 *
 * ★ bug2：标记必须存在 $falcon 上，不能只存在模块级变量里。
 * 登录页和 index 页是两份独立的 JS 上下文，模块级 pendingTab 写在一份里、
 * 另一份读不到（HANDOFF.md「跨页面不共享模块状态」踩过同一个坑）。
 * 之前只有模块变量，于是 setPendingTab('mine') 大概率等于没写，
 * index.onShow 的 takeTabParam() 拿到空串，整条刷新通道就断了。
 * 模块变量保留作为兜底（万一 $falcon 不可写）。 */
const TAB_KEY = '__pencgPendingTab';
let pendingTab = null;

function hostGlobal() {
  try {
    if (typeof $falcon !== 'undefined' && $falcon) return $falcon;
  } catch (e) {
    /* ignore */
  }
  return null;
}

export function setPendingTab(tab) {
  const v = { tab: String(tab || ''), at: Date.now() };
  pendingTab = v;
  const g = hostGlobal();
  if (g) {
    try {
      g[TAB_KEY] = v;
    } catch (e) {
      /* ignore */
    }
  }
}

export function takeTabParam() {
  /* bug2 复核改：两条通道各取一份，取「更新的那条」再用，而不是「$falcon 有就无条件
   * 用 $falcon」。原来 $falcon 一旦读到值就盖过模块变量，若模块变量里是更晚写下的
   * 标记（例如本页自己 goTab 刚写），会被旧标记顶替。两条同时置空即可 —— 取用本身
   * 是同步的，单页面内不会和别的调用交错。 */
  let a = null;
  const g = hostGlobal();
  if (g) {
    try {
      a = g[TAB_KEY] || null;
    } catch (e) {
      a = null;
    }
  }
  let v = a;
  if (pendingTab && (!a || (Number(pendingTab.at) || 0) > (Number(a.at) || 0))) v = pendingTab;
  if (a) {
    try {
      g[TAB_KEY] = null;
    } catch (e) {
      /* ignore */
    }
  }
  pendingTab = null;
  if (!v || !v.tab) return '';
  const stale = Date.now() - (Number(v.at) || 0) > 30000;
  return stale ? '' : v.tab;
}

export function navTo(name, params) {
  const p = PAGES[name] || name;
  const q = params || {};
  // 同时塞进模块级 pending：$falcon.navTo 的 options 在部分固件上会丢
  pending = { name: p, params: q, at: Date.now() };
  try {
    if (typeof $falcon !== 'undefined' && typeof $falcon.navTo === 'function') {
      $falcon.navTo(p, q);
      return true;
    }
  } catch (e) {
    /* ignore */
  }
  return false;
}

export function takePending(name) {
  if (!pending) return null;
  if (name && pending.name !== name) return null;
  if (Date.now() - pending.at > 30000) {
    pending = null;
    return null;
  }
  return pending.params;
}

export function peekParams(name, options) {
  const o = options || {};
  if (o.code || o.gameCode || o.keyword || o.from) return o;
  const p = takePending(name);
  return p || {};
}

/* 返回上一页。
 *
 * ★ 这台固件**没有 `$falcon.navBack`**（实测：`/etc/miniapp/resources/framework/*.bin`
 *   里只有 `navTo` / `closeApp` / `closePageByName` / `closePageById`，
 *   `grep -c navBack` 两个 bundle 都是 0）。原来这里 `typeof $falcon.navBack === 'function'`
 *   恒假，于是每次都掉进兜底 `navTo('index')` —— 那是**新压一层 index**，
 *   动画是 push（新页从右往左滑入），返回时看起来和「进入子页」一模一样，
 *   用户报的「退出也是向左划入」就是这个 bug；顺带页面栈还会一层层变深。
 *
 * 正确做法是 `closePageByName(<当前页名>)`：关掉自己，露出下面那页，
 * 动画是真正的 pop（当前页向右滑出）。
 *
 * @param {string} pageName 当前页在 app.json / navTo 里用的名字
 * @returns {boolean} 是否已经发起关闭（false 时调用方可自行兜底） */
export function navBack(pageName) {
  const p = String(pageName || '');
  if (p) {
    try {
      if (typeof $falcon !== 'undefined' && typeof $falcon.closePageByName === 'function') {
        $falcon.closePageByName(p);
        logInfo('[nav] closePageByName ' + p);
        return true;
      }
      logWarn('[nav] $falcon.closePageByName 不存在');
    } catch (e) {
      logWarn('[nav] closePageByName 失败: ' + ((e && e.message) || e));
    }
  }
  // 兼容别的固件：万一它们确实有 navBack
  try {
    if (typeof $falcon !== 'undefined' && typeof $falcon.navBack === 'function') {
      $falcon.navBack();
      return true;
    }
  } catch (e) {
    /* ignore */
  }
  logWarn('[nav] 无法关闭页面 name=' + p);
  return false;
}

/* ── 「一路退回首页」的接力旗 ──────────────────────────────────────────
 *
 * 场景：播放页 / 排队页 / 我的薄壳页里点「回首页」。栈可能是
 *   index → detail → play        或 index → queue → play
 * 只 navBack('play') 会落到 detail（或 queue），不是用户要的首页。
 *
 * 不能这么写：先 closePageByName('play')、再 setTimeout 关 detail ——
 * 关掉自己的时候所在页面的 JS 上下文连同定时器一起没了，后面的关不掉。
 *
 * 所以改成接力：调用方记一个「还要再退 N 层」的旗子，然后只管关自己；
 * 被露出来的那一页在自己的 onShow 里取一次旗子，如果还有剩余就接着关自己。
 * 每一页关门用的都是它**自己**的上下文，定时器不受影响。
 * index.vue 的 onShow 负责把旗子清零，避免残旗误伤（比如用户自己按返回）。 */
let homeHops = 0;

export function requestHome(hops) {
  const n = Number(hops);
  homeHops = n > 0 ? Math.floor(n) : 1;
}

export function takeHomeHop() {
  if (homeHops <= 0) return false;
  homeHops--;
  return true;
}

export function clearHomeHop() {
  homeHops = 0;
}
