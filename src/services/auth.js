// 登录与会话持久化
//  密码登录：user-pwd-info 取盐 → SHA256(salt+pwd) → 3DES(secret+random_text) → user-pwd-tokens
//  短信登录：phone-captchas 发码 → tokens{auth_method:'phone-captcha'}
import { v1, v1Post, applyLogin, session, clearSession, setUser, setPhone, currentOffset } from './cg.js';
import { loadNative } from './native.js';
import { KEYS, getString, setString, getJSON, setJSON, remove } from './storage.js';
import { logInfo, logWarn } from './log.js';

const SESSION_KEY = KEYS.session;

/* 跨页会话版本号（bug2 修复）
 *
 * 背景：登录页和 index 页是两份独立的 JS 上下文，模块级变量跨页**看不见**
 * （HANDOFF.md「跨页面不共享模块状态」那条踩过的坑）。所以登录页里
 * setPendingTab('mine') 留下的标记，index 页的 takeTabParam() 大概率读不到，
 * 整条「登录成功 → 我的页刷新」的通道就断了。
 *
 * 修法：一个递增的、跨页可见的版本号，只在「会话真的变了」的时候 +1。
 * 用递增整数而不是会话对象引用比较：cg.js 的 session() 每次都
 * Object.assign 出新对象，引用比较在 QuickJS 下恒不相等（要么永远触发、
 * 要么永远不触发），不可靠。
 *
 * 存三处，按可靠性递增：
 *   1) 本模块变量 LOCAL_SEQ —— 只在本页可见，仅作同页内一致性；
 *   2) $falcon[key]      —— 宿主全局，所有页面共用同一对象（首选，同步可读）；
 *   3) storage 键        —— 万一 $falcon 被冻结/只读，这是唯一确定跨页可见的通道
 *                          （sharedpreferences 本来就是跨页共享的），缺点是要异步读。
 * 读的时候取三者最大值：谁写成功了都能被看见。
 */
const SEQ_KEY = '__pencgSessionSeq';
const SEQ_STORE_KEY = '__pencg_session_seq';
let LOCAL_SEQ = 0;
/* bumpSessionVersion() 的本页串行化链（见该函数注释） */
let BUMP_CHAIN = Promise.resolve();

function falconGlobal() {
  try {
    if (typeof $falcon !== 'undefined' && $falcon) return $falcon;
  } catch (e) {
    /* ignore */
  }
  return null;
}

function readHost() {
  const g = falconGlobal();
  if (!g) return 0;
  try {
    const n = Number(g[SEQ_KEY]);
    return isFinite(n) && n > 0 ? n : 0;
  } catch (e) {
    return 0;
  }
}

/* 同步版本：只读 $falcon / 本模块。用于 onShow 这种不能 await 的地方。 */
export function sessionVersion() {
  return Math.max(readHost(), LOCAL_SEQ);
}

/* 异步版本：额外读一次盘。轮询通道用这个 —— 即使 $falcon 不可写也能工作。
 *
 * ★ 这里**不能**因为 `$falcon` 读到了非 0 值就跳过读盘（bug2 复核时本想这么优化，
 * 想清楚后否掉了）：`$falcon[SEQ_KEY]` 的可见性取决于宿主是否真的把同一个对象
 * 给所有页面。若每个页面拿到的是各自的副本，就会出现这种反例 ——
 *   ① 用户在本页（index 上下文）退出登录 → 本页 bump 到 5，本页 host=5；
 *   ② 用户去登录页登录 → 登录页 bump 到 6（写它自己的 host + 共享的盘）；
 *   ③ 本页 host 仍是 5（非 0）→ 若跳过读盘就永远看不到 6，栏目切回来还是未登录。
 * 所以两处都读、取 max。代价是每 2.5s 一次 getStorage（storage.js:41 有 3000ms
 * 超时并回落 MEMORY 缓存，且 build 期没有日志输出，不会刷屏）。 */
export async function sessionVersionAsync() {
  let st = 0;
  try {
    const s = await getString(SEQ_STORE_KEY, '');
    const n = Number(s);
    if (isFinite(n) && n > 0) st = n;
  } catch (e) {
    /* 盘上没有/JSON 坏/超时一律当 0，绝不让版本号回退 */
  }
  return Math.max(readHost(), LOCAL_SEQ, st);
}

export async function bumpSessionVersion() {
  /* ★ bug2 复核加：本页内串行化。
   * `const n = (await sessionVersionAsync()) + 1` 中间有两次 await（读 $falcon 是
   * 同步的，但读盘要 await）。若两次 bump 并发（例如用户手抖连点登录按钮、或
   * 退出登录后立刻又登录而前者还没跑完），两边会读到同一个旧值、都算出 n+1，
   * 版本号实际上只前进 1 —— 在一次「登出+登录」的组合里这会让「会话变了」
   * 这件事被合并掉。串成 promise 链后，后一次一定读到前一次写完的值。
   * 注意这只解决同一上下文内的并发；跨页面（登录页 vs index）本来就是靠
   * storage 的最大值收敛，天然不会回退。 */
  const prev = BUMP_CHAIN;
  let release = null;
  BUMP_CHAIN = new Promise(function (r) {
    release = r;
  });
  try {
    await prev;
  } catch (e) {
    /* 前一次失败不影响本次 */
  }
  try {
    return await bumpOnce();
  } finally {
    if (release) release();
  }
}

async function bumpOnce() {
  const n = (await sessionVersionAsync()) + 1;
  LOCAL_SEQ = n;
  const g = falconGlobal();
  if (g) {
    try {
      g[SEQ_KEY] = n;
    } catch (e) {
      /* $falcon 可能被冻结，忽略；下面还有 storage 兜底 */
    }
  }
  try {
    await setString(SEQ_STORE_KEY, String(n));
  } catch (e) {
    logWarn('[auth] 会话版本号落盘失败 ' + ((e && e.message) || e));
  }
  return n;
}

function saveSession(extra) {
  const s = session();
  const payload = Object.assign({}, s, extra || {}, { loginAt: Date.now() });
  return setJSON(SESSION_KEY, payload);
}

export function currentSession() {
  return session();
}

export function isLoggedIn() {
  const s = session();
  return !!(s && s.token);
}

// login_iv（毫秒级整数）→ 16 个 hex 字符（8 字节 IV）
function ivFromLoginIv(loginIv) {
  let n = Number(loginIv) || 0;
  let h = Math.floor(n).toString(16);
  while (h.length < 16) h = '0' + h;
  return h.slice(-16);
}

export async function loginByPassword(phone, cc, pwd) {
  const info = await v1('api/v1/user-pwd-info', { phone, ctcode: cc || '86' });
  if (!info || !info.user_pwd_login_salt) throw new Error('取登录参数失败');

  const mod = await loadNative();
  if (!mod || typeof mod.sha256Hex !== 'function') throw new Error('native 加密模块不可用（需要 cgnative）');

  const secret = mod.sha256Hex(info.user_pwd_login_salt + pwd);
  const plain = secret + info.login_random_text;
  const key = info.pwd_encrypt_key;
  const ivHex = ivFromLoginIv(info.login_iv);
  const pwdEnc = mod.tripleDesB64(plain, key, ivHex);

  const j = await v1Post('api/v1/user-pwd-tokens', {
    phone,
    ctcode: cc || '86',
    pwd: pwdEnc,
    login_mode: 'password'
  });
  if (!j || !j.token) throw new Error((j && (j.errmsgcn || j.errmsg)) || '登录失败');

  applyLogin(j);
  setPhone(phone, cc || '86');
  await saveSession({ phone, cc: cc || '86' });
  await bumpSessionVersion();
  logInfo('[auth] 密码登录成功 uid=' + j.user_id);
  return { token: j.token, userId: String(j.user_id), encrypt: j.encrypt, nickname: j.nickname, freeTimeLeft: j.free_time_left, vip: j.vip, is_vip: j.is_vip === true };
}

export async function sendSmsCode(phone, cc) {
  const j = await v1Post('api/v1/phone-captchas/' + (cc || '86') + '-' + phone, {});
  logInfo('[auth] 验证码已发送');
  return { ok: true, waitSeconds: (j && j.next_send_time) || 60 };
}

export async function loginBySms(phone, cc, code) {
  const j = await v1Post('api/v1/tokens', {
    auth_method: 'phone-captcha',
    ctcode: cc || '86',
    phone,
    captcha: code
  });
  if (!j || !j.token) throw new Error((j && (j.errmsgcn || j.errmsg)) || '验证码错误');
  applyLogin(j);
  setPhone(phone, cc || '86');
  await saveSession({ phone, cc: cc || '86' });
  await bumpSessionVersion();
  logInfo('[auth] 短信登录成功 uid=' + j.user_id);
  return { token: j.token, userId: String(j.user_id), encrypt: j.encrypt, nickname: j.nickname, freeTimeLeft: j.free_time_left, vip: j.vip, is_vip: j.is_vip === true };
}

// 启动时恢复；token 失效返回 false
export async function restoreSession() {
  const s = await getJSON(SESSION_KEY, null);
  if (!s || !s.token) return false;
  applyLogin(s);
  logInfo('[auth] 恢复会话 uid=' + s.userId + ' off=' + currentOffset());
  return true;
}

export async function logout() {
  clearSession();
  await remove(SESSION_KEY);
  await bumpSessionVersion();
}

export async function updateProfile(user) {
  setUser(user);
  return user;
}

/* ★ bug2 复核加：盘上会话的「年龄」（毫秒）。
 * 用途：index.checkSeq() 只有在「刚登录」时才把用户强行拽到「我的」。
 * 只靠 isLoggedIn() 的前后翻转不够 —— 冷启动时若本页 boot() 的两次读盘
 * （restoreSession + sessionVersionAsync）都因 storage.js 的 3000ms 超时失败，
 * 后面的轮询一旦成功，就会看到「false → true」的翻转 + 一个非 0 版本号，
 * 于是把本来就是老用户的机器误判成「刚登录」并拽到「我的」。
 * loginByPassword/loginBySms 写盘时都带 loginAt（saveSession:100），
 * 用它的年龄做门控就能把这条误判挡掉。读不到返回 -1（视为未知 → 不放行跳栏）。 */
export async function sessionAge() {
  try {
    const s = await getJSON(SESSION_KEY, null);
    if (!s || !s.token) return -1;
    const at = Number(s.loginAt) || 0;
    if (!at) return -1;
    return Date.now() - at;
  } catch (e) {
    return -1;
  }
}
