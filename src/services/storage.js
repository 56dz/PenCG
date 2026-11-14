// 存储 adapter（X6 Pro 真机实测）：
//   getStorage({key}) → {data:string}；setStorage({key, data}) 才落盘
//   传 value/content 会落盘空串；字符串形态 setStorage(k,v) 的 promise 永不 settle
// 统一对象形态 + 超时保护；失败时保留内存态。

const MEMORY = {};

export const KEYS = {
  session: 'pencg_session',
  settings: 'pencg_settings',
  history: 'pencg_history',
  autotest: 'pencg_autotest',
  lastTab: 'pencg_last_tab',
  diag: 'pencg_diag',
  /* 检查更新的提示状态：{skipVersion, quietDay}，见 services/updater.js。
   * 单独一个键而不是塞进 settings：它是「别烦我」的临时意愿，不是用户配置，
   * 混在一起会让 settings 的读写路径带上无关字段。 */
  update: 'pencg_update'
};

function store() {
  const f = typeof $falcon !== 'undefined' ? $falcon : null;
  const s = f && f.jsapi && f.jsapi.storage;
  if (s && typeof s.getStorage === 'function' && typeof s.setStorage === 'function') return s;
  return null;
}

function withTimeout(p, ms) {
  let timer = 0;
  const timeout = new Promise((resolve) => {
    timer = setTimeout(() => resolve({ __timeout: true }), ms);
  });
  return Promise.race([p, timeout]).then(
    (v) => {
      clearTimeout(timer);
      return v;
    },
    (e) => {
      clearTimeout(timer);
      throw e;
    }
  );
}

export async function getString(key, fallback) {
  const s = store();
  if (!s) return MEMORY[key] !== undefined ? MEMORY[key] : fallback;
  try {
    const r = await withTimeout(Promise.resolve(s.getStorage({ key: key })), 3000);
    if (!r || r.__timeout) return MEMORY[key] !== undefined ? MEMORY[key] : fallback;
    let v = '';
    if (typeof r === 'object') v = typeof r.data === 'string' ? r.data : '';
    else if (typeof r === 'string') v = r;
    if (!v) return fallback === undefined ? '' : fallback;
    return v;
  } catch (e) {
    return MEMORY[key] !== undefined ? MEMORY[key] : fallback;
  }
}

export async function setString(key, value) {
  const v = value == null ? '' : String(value);
  MEMORY[key] = v;
  const s = store();
  if (!s) return true;
  try {
    await withTimeout(Promise.resolve(s.setStorage({ key: key, data: v })), 3000);
    return true;
  } catch (e) {
    return false;
  }
}

export async function getJSON(key, fallback) {
  const raw = await getString(key, '');
  if (!raw) return fallback;
  try {
    return JSON.parse(raw);
  } catch (e) {
    return fallback;
  }
}

export async function setJSON(key, value) {
  let text = '';
  try {
    text = JSON.stringify(value);
  } catch (e) {
    text = '';
  }
  return setString(key, text);
}

export async function remove(key) {
  delete MEMORY[key];
  return setString(key, '');
}

// 测试注入（诊断页 / 自动化用）
export function __setMemory(key, value) {
  MEMORY[key] = value;
}
