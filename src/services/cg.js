// 网易云游戏网关客户端
//  - v1 = 明文匿名接口
//  - v2 = 登录态接口，body 走「字节偏移 + Base64」加密
// 偏移量 off 由登录响应的 encrypt 字段推导，HTTP 与 WS 共用同一个 off。
import { API_BASE } from './const.js';
import { httpText, httpJSON, cgHeaders } from './net.js';
import { loadNative } from './native.js';
import { logInfo, logWarn } from './log.js';

// get_offset2：encrypt = "method:1,number:579572448,timestamp:1793976143,salt:58"
// 每段取冒号后的值 → 用 'd' 连接 → 按 'd' 切 → 取第 1/2/3 项 → (a|b)%256 + c，最后再 %256
export function offsetFromEncrypt(encrypt) {
  if (!encrypt) return 0;
  const parts = String(encrypt)
    .split(',')
    .map((p) => {
      const i = p.indexOf(':');
      return i < 0 ? '' : p.slice(i + 1);
    });
  const nums = parts.join('d').split('d').map((s) => parseInt(s, 10) || 0);
  const a = nums[1] || 0;
  const b = nums[2] || 0;
  const c = nums[3] || 0;
  return (((a | b) % 256) + c) % 256;
}

let NATIVE = null;

async function nt() {
  if (NATIVE) return NATIVE;
  NATIVE = await loadNative();
  return NATIVE;
}

function utf8Bytes(str) {
  // 纯 JS UTF-8 编码（QuickJS TextEncoder 不一定有）
  const out = [];
  for (let i = 0; i < str.length; i++) {
    let c = str.charCodeAt(i);
    if (c < 0x80) out.push(c);
    else if (c < 0x800) {
      out.push(0xc0 | (c >> 6), 0x80 | (c & 0x3f));
    } else if (c >= 0xd800 && c <= 0xdbff && i + 1 < str.length) {
      const c2 = str.charCodeAt(i + 1);
      if (c2 >= 0xdc00 && c2 <= 0xdfff) {
        const cp = 0x10000 + ((c - 0xd800) << 10) + (c2 - 0xdc00);
        out.push(0xf0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3f), 0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f));
        i++;
        continue;
      }
      out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 0x3f), 0x80 | (c & 0x3f));
    } else {
      out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 0x3f), 0x80 | (c & 0x3f));
    }
  }
  return out;
}

function bytesUtf8(bytes) {
  let s = '';
  let i = 0;
  while (i < bytes.length) {
    const b = bytes[i];
    if (b < 0x80) {
      s += String.fromCharCode(b);
      i++;
    } else if (b < 0xe0) {
      s += String.fromCharCode(((b & 0x1f) << 6) | (bytes[i + 1] & 0x3f));
      i += 2;
    } else if (b < 0xf0) {
      s += String.fromCharCode(((b & 0x0f) << 12) | ((bytes[i + 1] & 0x3f) << 6) | (bytes[i + 2] & 0x3f));
      i += 3;
    } else {
      const cp = ((b & 0x07) << 18) | ((bytes[i + 1] & 0x3f) << 12) | ((bytes[i + 2] & 0x3f) << 6) | (bytes[i + 3] & 0x3f);
      const v = cp - 0x10000;
      s += String.fromCharCode(0xd800 + (v >> 10), 0xdc00 + (v & 0x3ff));
      i += 4;
    }
  }
  return s;
}

const B64CHARS = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

function b64FromBytes(bytes) {
  let out = '';
  let i = 0;
  while (i + 2 < bytes.length) {
    const n = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
    out += B64CHARS[(n >> 18) & 63] + B64CHARS[(n >> 12) & 63] + B64CHARS[(n >> 6) & 63] + B64CHARS[n & 63];
    i += 3;
  }
  const rest = bytes.length - i;
  if (rest === 1) {
    const n = bytes[i] << 16;
    out += B64CHARS[(n >> 18) & 63] + B64CHARS[(n >> 12) & 63] + '==';
  } else if (rest === 2) {
    const n = (bytes[i] << 16) | (bytes[i + 1] << 8);
    out += B64CHARS[(n >> 18) & 63] + B64CHARS[(n >> 12) & 63] + B64CHARS[(n >> 6) & 63] + '=';
  }
  return out;
}

function bytesFromB64(str) {
  const map = {};
  for (let i = 0; i < B64CHARS.length; i++) map[B64CHARS[i]] = i;
  let s = String(str || '').replace(/[^A-Za-z0-9+/]/g, '');
  const out = [];
  let buf = 0;
  let bits = 0;
  for (let i = 0; i < s.length; i++) {
    buf = (buf << 6) | (map[s[i]] || 0);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push((buf >> bits) & 0xff);
    }
  }
  return out;
}

// 加密：JSON 字符串 → 逐字节 +off → Base64
export function encodeBody(off, obj) {
  const bytes = utf8Bytes(typeof obj === 'string' ? obj : JSON.stringify(obj));
  for (let i = 0; i < bytes.length; i++) bytes[i] = (bytes[i] + off) & 0xff;
  return b64FromBytes(bytes);
}

// 解密：Base64 → 逐字节 -off → UTF-8
export function decodeBody(off, b64) {
  const bytes = bytesFromB64(b64);
  for (let i = 0; i < bytes.length; i++) bytes[i] = (bytes[i] - off + 256) & 0xff;
  return bytesUtf8(bytes);
}

const G = {
  token: '',
  userId: '',
  encrypt: '',
  offset: 0,
  nickname: '',
  phone: '',
  freeTimeLeft: 0,
  vip: null,
  // 账号顶层布尔 is_vip（CGAPI users/@me 实测 true）。
  // 注意 vip 对象里只有 {create_time,current_time,end_time,extra,id,start_time,update_time,user_id}，
  // 没有 level / vip_level / expire_time / is_vip —— 判会员只能靠这个顶层字段。
  isVip: false,
  user: null
};

export function session() {
  return G.token ? Object.assign({}, G) : null;
}

export function applyLogin(payload) {
  G.token = (payload && payload.token) || '';
  G.userId = (payload && (payload.user_id || payload.userId)) || '';
  G.encrypt = (payload && payload.encrypt) || '';
  G.offset = offsetFromEncrypt(G.encrypt);
  G.nickname = (payload && payload.nickname) || '';
  G.phone = (payload && (payload.phone || payload.mobile)) || G.phone || '';
  G.freeTimeLeft = (payload && payload.free_time_left) || 0;
  G.vip = (payload && payload.vip) || null;
  // 登录响应带 is_vip（下划线）；restoreSession 喂回来的是 session() 快照（驼峰 isVip）。
  // 两种都认，且快照没带就保留原值，避免每次恢复会话把会员态清成普通用户。
  if (payload && typeof payload.is_vip === 'boolean') G.isVip = payload.is_vip;
  else if (payload && typeof payload.isVip === 'boolean') G.isVip = payload.isVip;
  logInfo('[cg] login off=' + G.offset + ' uid=' + G.userId);
  return session();
}

export function setPhone(phone, cc) {
  if (phone) G.phone = String(phone);
  if (cc) G.cc = String(cc);
  return G.phone;
}

export function setUser(user) {
  G.user = user;
  if (user) {
    if (user.nickname) G.nickname = user.nickname;
    if (user.phone || user.mobile) G.phone = user.phone || user.mobile;
    if (typeof user.free_time_left === 'number') G.freeTimeLeft = user.free_time_left;
    if (typeof user.is_vip === 'boolean') G.isVip = user.is_vip;
    if (user.vip) G.vip = user.vip;
  }
  return G.user;
}

export function clearSession() {
  G.token = '';
  G.userId = '';
  G.encrypt = '';
  G.offset = 0;
  G.user = null;
  G.isVip = false;
}

function abs(path) {
  if (/^https?:\/\//.test(path)) return path;
  return API_BASE + String(path).replace(/^\/+/, '');
}

function authHeaders() {
  const h = cgHeaders();
  if (G.token) h.Authorization = 'Bearer ' + G.token;
  return h;
}

function qs(params) {
  if (!params) return '';
  const out = [];
  for (const k of Object.keys(params)) {
    const v = params[k];
    if (v === undefined || v === null) continue;
    out.push(encodeURIComponent(k) + '=' + encodeURIComponent(String(v)));
  }
  return out.length ? '?' + out.join('&') : '';
}

// ---- v1 匿名 ----
export async function v1(path, params) {
  return httpJSON(abs(path) + qs(params), { headers: cgHeaders() });
}

export async function v1Post(path, body) {
  return httpJSON(abs(path), {
    method: 'POST',
    headers: cgHeaders({ 'Content-Type': 'application/json' }),
    body: JSON.stringify(body || {})
  });
}

// ---- v2 登录态（加密）----
export async function v2(path, body, method) {
  const m = (method || (body === undefined ? 'GET' : 'POST')).toUpperCase();
  const url = abs(path);
  const headers = authHeaders();
  headers['Content-Type'] = 'application/octet-stream';

  const r = await httpText(url, {
    method: m,
    headers,
    body: body === undefined ? '' : encodeBody(G.offset, body),
    timeout: 20000
  });

  let text = r.body || '';
  // 响应可能是加密的（裸 Base64），也可能是明文 JSON
  const trimmed = text.trim();
  if (trimmed && trimmed[0] !== '{' && trimmed[0] !== '[') {
    try {
      text = decodeBody(G.offset, trimmed);
    } catch (e) {
      logWarn('[cg] v2 解密失败:', (e && e.message) || e);
    }
  }

  if (r.status < 200 || r.status >= 300) {
    let detail = r.error;
    try {
      const j = JSON.parse(text);
      if (j && (j.errmsg || j.errmsgcn)) detail = j.errcode + ' ' + (j.errmsgcn || j.errmsg);
    } catch (e) {
      /* ignore */
    }
    const err = new Error(detail || 'http ' + r.status);
    err.status = r.status;
    err.body = text;
    err.code = 0;
    try {
      err.code = JSON.parse(text).errcode || 0;
    } catch (e) {
      /* ignore */
    }
    throw err;
  }
  if (!text) return {};
  const j = JSON.parse(text);
  return j && j.data !== undefined ? j.data : j;
}

export async function v2Post(path, body) {
  return v2(path, body, 'POST');
}

export async function v2Get(path) {
  return v2(path, undefined, 'GET');
}

export async function v2Delete(path) {
  return v2(path, undefined, 'DELETE');
}

export function currentOffset() {
  return G.offset;
}

export function currentToken() {
  return G.token;
}

export function currentUserId() {
  return G.userId;
}

// 供 native 媒体层构造 answer 时用
export { utf8Bytes, bytesUtf8, b64FromBytes, bytesFromB64 };
