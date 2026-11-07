// 传输层：两级实现
//  1) native（cgnative 模块）：curl 直连，能发裸 body、能读非 2xx 的 body ← v2 加密接口必须走这条
//  2) 平台 jsapi.http.request：兜底（能发普通 JSON，但拿不到非 2xx 的 body，也不能发裸 body）
//
// 统一返回 { status, body, error, curlCode }
import { MOBILE_UA, API_VERSION, PLATFORM } from './const.js';
import { logInfo, logWarn } from './log.js';
import { loadNative } from './native.js';

function platformHttp() {
  const f = typeof $falcon !== 'undefined' ? $falcon : null;
  const h = f && f.jsapi && f.jsapi.http;
  if (h && typeof h.request === 'function') return h;
  return null;
}

export function joinedHeaders(headers) {
  const out = [];
  const all = Object.assign({ 'User-Agent': MOBILE_UA }, headers || {});
  for (const k of Object.keys(all)) {
    const v = all[k];
    if (v === undefined || v === null) continue;
    out.push(k + ': ' + String(v));
  }
  return out.join('\r\n');
}

function parseResCode(msg) {
  const m = /resCode:(\d+)/.exec(msg || '');
  return m ? parseInt(m[1], 10) : 0;
}

function jsapiRequest(url, opts) {
  const http = platformHttp();
  if (!http) return Promise.resolve({ status: 0, body: '', error: 'jsapi.http 不可用', curlCode: -1 });

  const headers = Object.assign({}, opts.headers || {});
  const q = {};
  for (const k of Object.keys(headers)) q[k] = String(headers[k]);

  let base = url;
  let urlParams;
  const qi = url.indexOf('?');
  if (qi >= 0) {
    base = url.slice(0, qi);
    urlParams = {};
    const qs = url.slice(qi + 1).split('&');
    for (const kv of qs) {
      const eq = kv.indexOf('=');
      const k = eq < 0 ? kv : kv.slice(0, eq);
      const v = eq < 0 ? '' : decodeURIComponent(kv.slice(eq + 1));
      urlParams[k] = v;
    }
  }

  const options = {
    url: base,
    method: (opts.method || 'GET').toUpperCase(),
    timeout: opts.timeout || 15000,
    header: q
  };
  if (urlParams) options.urlParams = urlParams;
  if (opts.body !== undefined && opts.body !== null && opts.body !== '') options.data = opts.body;

  return new Promise((resolve) => {
    let done = false;
    const finish = (v) => {
      if (done) return;
      done = true;
      resolve(v);
    };
    setTimeout(() => finish({ status: 0, body: '', error: 'timeout', curlCode: -1 }), (opts.timeout || 15000) + 3000);
    try {
      http.request(options, (res) => {
        if (res && res.error) {
          const msg = (res.result && res.result.errorMessage) || '';
          finish({ status: parseResCode(msg) || 0, body: '', error: msg || 'jsapi error', curlCode: -1 });
          return;
        }
        let body = res && res.result;
        if (body === undefined || body === null) body = '';
        if (typeof body !== 'string') {
          try {
            body = JSON.stringify(body);
          } catch (e) {
            body = String(body);
          }
        }
        finish({ status: 200, body, error: null, curlCode: 0 });
      });
    } catch (e) {
      finish({ status: 0, body: '', error: (e && e.message) || String(e), curlCode: -1 });
    }
  });
}

export async function httpText(url, opts) {
  const o = opts || {};
  const mod = await loadNative();
  const timeout = o.timeout || 15000;
  const method = (o.method || 'GET').toUpperCase();
  const body = o.body === undefined || o.body === null ? '' : String(o.body);

  if (mod && typeof mod.requestSync === 'function') {
    const raw = mod.requestSync(method, url, joinedHeaders(o.headers), body, timeout);
    let r = null;
    try {
      r = typeof raw === 'string' ? JSON.parse(raw) : raw;
    } catch (e) {
      r = null;
    }
    if (r && typeof r.status === 'number') {
      const ok = r.status >= 200 && r.status < 300;
      return { status: r.status, body: r.body || '', error: ok ? null : r.error || 'http ' + r.status, curlCode: r.curlCode || 0 };
    }
    logWarn('[net] native requestSync 返回异常，回退 jsapi');
  } else if (mod && typeof mod.postSync === 'function') {
    const raw = mod.postSync(url, joinedHeaders(o.headers), body, timeout);
    let r = null;
    try {
      r = typeof raw === 'string' ? JSON.parse(raw) : raw;
    } catch (e) {
      r = null;
    }
    if (r && typeof r.status === 'number') {
      const ok = r.status >= 200 && r.status < 300;
      return { status: r.status, body: r.body || '', error: ok ? null : r.error || 'http ' + r.status, curlCode: 0 };
    }
  }
  return jsapiRequest(url, o);
}

export async function httpJSON(url, opts) {
  const r = await httpText(url, opts);
  if (r.status < 200 || r.status >= 300) {
    let detail = r.error;
    let bizCode = 0;
    try {
      const j = JSON.parse(r.body);
      if (j && (j.errmsg || j.errmsgcn)) detail = j.errcode + ' ' + (j.errmsgcn || j.errmsg);
      if (j && j.errcode !== undefined) bizCode = Number(j.errcode) || 0;
    } catch (e) {
      /* ignore */
    }
    const err = new Error(detail || 'http ' + r.status);
    err.status = r.status;
    err.body = r.body;
    // 业务错误码必须挂到 err.code 上：v1 接口的 errcode 是业务码（1102 验证码错 /
    // 1100 发送太频繁 / 1103 ...），之前只拼进 message，调用方无法分类处理。
    err.code = bizCode;
    err.errcode = bizCode;
    throw err;
  }
  if (!r.body) return {};
  try {
    return JSON.parse(r.body);
  } catch (e) {
    const err = new Error('响应非 JSON: ' + r.body.slice(0, 120));
    err.status = r.status;
    throw err;
  }
}

export function cgHeaders(extra) {
  return Object.assign(
    {
      'User-Agent': MOBILE_UA,
      'X-Platform': String(PLATFORM.MOBILE),
      'X-Channel': 'web',
      'X-Source-Type': 'web',
      'X-Ver': '1.0.0',
      Referer: 'https://cg.163.com/',
      Origin: 'https://cg.163.com',
      Accept: 'application/json, text/plain, */*'
    },
    extra || {}
  );
}
