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
