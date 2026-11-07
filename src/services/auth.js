// 登录与会话持久化
//  密码登录：user-pwd-info 取盐 → SHA256(salt+pwd) → 3DES(secret+random_text) → user-pwd-tokens
//  短信登录：phone-captchas 发码 → tokens{auth_method:'phone-captcha'}
import { v1, v1Post, applyLogin, session, clearSession, setUser, setPhone, currentOffset } from './cg.js';
import { loadNative } from './native.js';
import { KEYS, getString, setString, getJSON, setJSON, remove } from './storage.js';
import { logInfo, logWarn } from './log.js';

const SESSION_KEY = KEYS.session;

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
}

export async function updateProfile(user) {
  setUser(user);
  return user;
}
