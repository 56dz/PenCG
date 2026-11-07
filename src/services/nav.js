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
 * 加 30s 过期：防止标记残留到很久以后一次莫名其妙的跳转。 */
let pendingTab = null;

export function setPendingTab(tab) {
  pendingTab = { tab: String(tab || ''), at: Date.now() };
}

export function takeTabParam() {
  if (!pendingTab) return '';
  const t = pendingTab.tab;
  const stale = Date.now() - pendingTab.at > 30000;
  pendingTab = null;
  return stale ? '' : t;
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

export function navBack() {
  try {
    if (typeof $falcon !== 'undefined' && typeof $falcon.navBack === 'function') {
      $falcon.navBack();
      return true;
    }
  } catch (e) {
    /* ignore */
  }
  navTo('index', {});
  return false;
}
