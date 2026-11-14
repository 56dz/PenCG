// 检查更新 / 下载安装（双源：GitCode 主 / GitHub 备）
//
// 本文件移植自 PenBili/src/services/updater.js（同一作者的另一个小程序，
// 该功能在 PenBili 上已经过真机验证）。移植时只替换了传输层与 native 调用，
// **纯逻辑部分与全部真机结论原样保留**，改之前请先读 docs/UPDATER.md。
//
// 为什么是双源：GitHub 在部分国内网络下完全不可达，所以检测顺序是
// **GitCode -> GitHub**，谁先用谁的；下载镜像只对 GitHub 资产有意义
// （GitCode 本身国内可直连，套一层代理反而多一跳）。
//
// 设计约束（都是真机实测结论，别按常识猜）：
//   1) 检测走**直连**，且**不用镜像**。镜像 https://v4.gh-proxy.org/ 访问 api.github.com
//      会被共享出口 IP 限流（实测 403 rate limit exceeded），所以镜像只用于**下载资产**。
//   2) 只用 `/releases/latest`（两个平台都有这个接口）：天然排除 prerelease 与 draft，
//      不需要自己过滤。GitCode 另外还有 `release_status` 字段（'pre' / 'latest'），
//      一并过滤。
//   3) 传输层拿不到响应头，非 2xx 时 body 也不可得，错误文本形如
//      `curl ... resCode:403` —— 状态码只能从返回体的 status 字段取。
//      PenCG 的 net.js 返回 { status, body, error, curlCode }，
//      注意字段名与 PenBili 的 { statusCode, body, errorMessage } **不同**。
//   4) 本模块的**纯逻辑部分**（版本比较、资产挑选、镜像 URL、提示决策、正文切行）
//      全部导出为无副作用函数；带运行时依赖的只有
//      fetchLatestRelease / downloadFileToDisk / installAmr 三个薄封装。
//
// 版本号来源：PenCG 有唯一来源 services/version.js 的 APP_VERSION，
// 本模块直接 import 并 re-export（PenBili 是自己写常量 + 单测钉住，
// PenCG 没有 test 目录，所以改用单源，避免两处漂移）。

import { httpText } from './net.js';
import { logWarn } from './log.js';
import { getJSON, setJSON, KEYS } from './storage.js';
import { APP_VERSION } from './version.js';

export { APP_VERSION };

/** 主源：GitCode（国内可直连，GitHub 在部分网络下完全打不开） */
export const GITCODE_REPO = 'dz56dz/PenCG';
/** GitCode 最新正式版接口（同样天然排除 prerelease / draft） */
export const API_GITCODE = 'https://api.gitcode.com/api/v5/repos/' + GITCODE_REPO + '/releases/latest';
/** GitCode 仓库主页 */
export const REPO_GITCODE_URL = 'https://gitcode.com/' + GITCODE_REPO;
/** 备源：GitHub 仓库 */
export const REPO = '56dz/PenCG';
/** GitHub 最新正式版接口（天然排除 prerelease / draft） */
export const API_LATEST = 'https://api.github.com/repos/' + REPO + '/releases/latest';
/** 同 API_LATEST，语义化别名（SOURCES 里用） */
export const API_GITHUB = API_LATEST;
/** GitHub 仓库主页（人工兜底：更新失败时告诉用户去哪儿手动下） */
export const REPO_URL = 'https://github.com/' + REPO;

/**
 * 检测源列表，**顺序即优先级**：GitCode 在前。
 * 为什么 GitCode 优先：主通道必须在国内可用；GitHub 留着给海外/能直连的网络，
 * 且它是原始发布地。headers 按平台分：GitHub API 强制要 User-Agent，
 * GitCode 不介意多带。
 */
export const SOURCES = [
  {
    id: 'gitcode',
    label: 'GitCode',
    api: API_GITCODE,
    repoUrl: REPO_GITCODE_URL,
    headers: { 'User-Agent': 'PenCG-Updater', Accept: 'application/json' }
  },
  {
    id: 'github',
    label: 'GitHub',
    api: API_LATEST,
    repoUrl: REPO_URL,
    headers: { 'User-Agent': 'PenCG-Updater', Accept: 'application/vnd.github+json' }
  }
];

/** 按 id 取源（找不到给主源，永不返回 null） */
export function sourceById(id) {
  for (let i = 0; i < SOURCES.length; i++) if (SOURCES[i].id === id) return SOURCES[i];
  return SOURCES[0];
}

/** 源的展示名（弹窗按钮/提示里用，未知源回退成主源名） */
export function sourceLabel(id) {
  return sourceById(id).label;
}

/**
 * 从 downloadFile 的回调/返回值里取落盘路径。
 *
 * 真机实测（PenBili 首次装机时确认，PenCG 用同一个宿主，结论一致）：回调参数形如
 *   `{"result": "/userdisk/miniapp/downloaded/<md5>"}`  —— result 是**字符串**不是对象！
 * 所以不能只 `inner.filePath || inner.path`，那两条在字符串上永远取不到，
 * 于是「下载明明成功、文件也在盘上」却被判失败。三种形态都要认：
 *   1) {result: "/path"}     <- 实测形态
 *   2) {result: {filePath: "/path"}}
 *   3) {filePath: "/path"}
 * 顺便说明：saveName 不生效，落盘名是 url 内容的 md5（每次一样，天然去重缓存）。
 *
 * @param {*} res
 * @returns {string} 取不到返回空串
 */
export function extractFilePath(res) {
  if (typeof res === 'string') return res;
  if (!res || typeof res !== 'object') return '';
  const r = res.result;
  if (typeof r === 'string') return r;
  if (r && typeof r === 'object') {
    if (typeof r.filePath === 'string' && r.filePath) return r.filePath;
    if (typeof r.path === 'string' && r.path) return r.path;
  }
  if (typeof res.filePath === 'string' && res.filePath) return res.filePath;
  if (typeof res.path === 'string' && res.path) return res.path;
  return '';
}

/** 下载加速前缀：镜像 URL = MIRROR_PREFIX + 原始 URL */
export const MIRROR_PREFIX = 'https://v4.gh-proxy.org/';
/** 发布资产名（打包器产出的唯一发布件） */
export const ASSET_NAME = 'PenCG.amr';
/** 下载落盘目录（miniapp 以 root 运行，该目录实测可写） */
export const SAVE_DIR = '/userdisk/miniapp/downloaded';
/** 下载落盘文件名（实测不生效，落盘名是 url 的 md5，留着只是意图说明） */
export const SAVE_NAME = 'PenCG-update.amr';

/* ============================ 纯逻辑 ============================ */

/**
 * 解析版本号：`v1.2.3` / `1.2.3` / `v1.3.0-beta.1` -> {nums:[1,2,3], pre:'beta.1'}
 * 解析不出来返回 null（调用方一律当「无法比较」，绝不猜）。
 * @param {string} s
 * @returns {{nums:number[], pre:string}|null}
 */
export function parseVersion(s) {
  if (typeof s !== 'string') return null;
  const t = s.trim().replace(/^v/i, '');
  if (!t) return null;
  // 主版本必须是纯数字段，允许后面跟 -pre / +build
  const m = t.match(/^(\d+(?:\.\d+)*)(?:[-+](.*))?$/);
  if (!m) return null;
  const nums = m[1].split('.').map((x) => parseInt(x, 10));
  if (nums.some((n) => !isFinite(n) || n < 0)) return null;
  return { nums: nums, pre: m[2] || '' };
}

/**
 * 比较两个版本号：a<b 返回 -1，相等 0，a>b 返回 1。
 * 任一方解析失败返回 0（等价于「不比它新」，宁可不提示也不要弹假更新）。
 * 缺位补 0（1.3 == 1.3.0）；带预发布标记的一方视为更小（1.0.0-beta < 1.0.0）。
 * @param {string} a
 * @param {string} b
 * @returns {number}
 */
export function compareVersion(a, b) {
  const pa = parseVersion(a);
  const pb = parseVersion(b);
  if (!pa || !pb) return 0;
  const n = Math.max(pa.nums.length, pb.nums.length);
  for (let i = 0; i < n; i++) {
    const x = pa.nums[i] || 0;
    const y = pb.nums[i] || 0;
    if (x !== y) return x < y ? -1 : 1;
  }
  // 数字段相等 -> 有预发布标记的更小
  if (pa.pre && !pb.pre) return -1;
  if (!pa.pre && pb.pre) return 1;
  if (pa.pre === pb.pre) return 0;
  return pa.pre < pb.pre ? -1 : 1;
}

/**
 * 远端是否比本地新。
 * @param {string} remoteTag 形如 'v1.2.3'
 * @param {string} localVersion 形如 '1.2.2'
 * @returns {boolean}
 */
export function isNewer(remoteTag, localVersion) {
  return compareVersion(remoteTag, localVersion) > 0;
}

/**
 * 把 release JSON 归一化成页面要用的形状；不是合法 release（无 tag / draft / prerelease）返回 null。
 *
 * GitHub 与 GitCode 的返回体高度相似（`tag_name` / `body` / `html_url` /
 * `assets[].name|size|browser_download_url` 同名），所以一个归一化函数通吃。
 * 唯一差别：GitCode 多一个 `release_status`（'pre' 预发布 / 'latest' 最新），
 * 预发布必须排除，所以两种标记都要判。
 *
 * @param {*} rel 最新 release 的 body（对象或 JSON 字符串）
 * @param {string} [repoUrl] 归一化兜底用的仓库主页（缺 html_url 时用）
 * @returns {{tag:string, version:string, name:string, body:string, htmlUrl:string,
 *            assetName:string, assetSize:number, assetUrl:string}|null}
 */
export function normalizeRelease(rel, repoUrl) {
  let d = rel;
  if (typeof d === 'string') {
    try {
      d = JSON.parse(d);
    } catch (e) {
      return null;
    }
  }
  if (!d || typeof d !== 'object') return null;
  if (d.draft === true) return null;
  if (d.prerelease === true || d.prerelease === 'true') return null;
  // GitCode 专有：release_status='pre' 表示预发布
  if (typeof d.release_status === 'string' && d.release_status.toLowerCase() === 'pre') return null;
  const tag = typeof d.tag_name === 'string' ? d.tag_name.trim() : '';
  if (!tag) return null;
  const parsed = parseVersion(tag);
  if (!parsed) return null;
  const assets = Array.isArray(d.assets) ? d.assets : [];
  const hit = assets.find((a) => a && a.name === ASSET_NAME) || assets[0] || null;
  return {
    tag: tag,
    version: parsed.nums.join('.'),
    name: typeof d.name === 'string' ? d.name : tag,
    body: typeof d.body === 'string' ? d.body : '',
    htmlUrl: typeof d.html_url === 'string' ? d.html_url : (repoUrl || REPO_URL),
    assetName: hit && typeof hit.name === 'string' ? hit.name : '',
    assetSize: hit ? Number(hit.size) || 0 : 0,
    assetUrl: hit && typeof hit.browser_download_url === 'string' ? hit.browser_download_url : ''
  };
}

/**
 * 这个资产 URL 能不能走 GitHub 加速镜像。
 * 只有 github.com / *.githubusercontent.com 的地址有意义 —— 镜像站是
 * 「前缀 + 完整原始 URL」的通用反代，套在 GitCode 地址上只是多一跳，
 * 而且 GitCode 在国内本来就直连可用，套了反而变慢/可能 404。
 * @param {string} assetUrl
 * @returns {boolean}
 */
export function isMirrorable(assetUrl) {
  if (typeof assetUrl !== 'string' || !/^https?:\/\//i.test(assetUrl)) return false;
  const m = assetUrl.match(/^https?:\/\/([^/?#]+)/i);
  if (!m) return false;
  const host = m[1].toLowerCase();
  // 后缀判断必须带点，不能只 endswith('github.com')：
  // 'evilgithub.com' / 'github.com.evil.io' 都不是 GitHub，却能骗过裸后缀。
  return host === 'github.com' || host.slice(-11) === '.github.com' ||
         host === 'githubusercontent.com' || host.slice(-22) === '.githubusercontent.com';
}

/**
 * 生成下载 URL。useMirror 为真时**只**给 GitHub 资产加镜像前缀（见 isMirrorable）；
 * 没有原始资产 URL 时返回空串（调用方据此禁用「立即更新」按钮，
 * 而不是拼一个必然 404 的地址）。
 *
 * PenCG 目前一律传 false（直连）：镜像对 api.github.com 会被限流，
 * 对资产虽然能用，但真机实测直连成功率已经够高，多一跳只是多一个失败点。
 * 函数保留是为了将来某个网络环境下需要兜底时不必重写。
 * @param {string} assetUrl
 * @param {boolean} useMirror
 * @returns {string}
 */
export function downloadUrl(assetUrl, useMirror) {
  if (typeof assetUrl !== 'string' || !/^https?:\/\//i.test(assetUrl)) return '';
  if (!useMirror) return assetUrl;
  return isMirrorable(assetUrl) ? MIRROR_PREFIX + assetUrl : assetUrl;
}

/**
 * 把 release 正文（Markdown）切成纯文本行，供 `<text>` 逐行渲染 + scroller 滚动。
 * 为什么必须切行：falcon 的 `<text>` 内容里出现换行就不渲染（见 base.less 铁律），
 * 一整段长文本塞一个 `<text>` 只会显示成一行截断。
 * @param {string} body
 * @param {number} [maxLines] 上限（防止极端长的发布说明把 scroller 撑爆）
 * @returns {string[]}
 */
export function bodyLines(body, maxLines) {
  const cap = Number(maxLines) > 0 ? Number(maxLines) : 400;
  const raw = typeof body === 'string' ? body : '';
  const out = [];
  const rows = raw.replace(/\r\n/g, '\n').replace(/\r/g, '\n').split('\n');
  for (let i = 0; i < rows.length && out.length < cap; i++) {
    let s = rows[i];
    // 去 Markdown 标记：标题井号、粗体/斜体星号、行内代码反引号、引用符、列表符
    s = s.replace(/^\s*#{1,6}\s*/, '');
    s = s.replace(/^\s*>\s?/, '');
    s = s.replace(/^\s*[-*+]\s+/, '· ');
    s = s.replace(/^\s*\d+[.)]\s+/, (m) => m.trim() + ' ');
    s = s.replace(/\*\*([^*]*)\*\*/g, '$1');
    s = s.replace(/`([^`]*)`/g, '$1');
    s = s.replace(/\[([^\]]*)\]\([^)]*\)/g, '$1');
    s = s.replace(/[ \t]+$/g, '');
    if (!s.trim()) {
      // 连续空行压成一个空行，避免滚动条上大片空白
      if (out.length && out[out.length - 1] !== '') out.push('');
      continue;
    }
    out.push(s);
  }
  while (out.length && out[out.length - 1] === '') out.pop();

  /* 发布说明的第一行经常是 `## v1.2.3` 这种版本标题 —— 弹窗标题栏里已经有
   * 「发现新版本 v1.2.3」了，再在正文顶部挂一行裸版本号纯属重复，
   * 而且 266px 高屏上白吃一整行。只吃掉**开头**那一行孤零零的版本号，
   * 后面的 `## 检查更新` 之类小节标题照常保留。 */
  if (out.length && /^v?\d+(\.\d+){0,3}$/i.test(out[0].trim())) {
    out.shift();
    while (out.length && out[0] === '') out.shift();
  }
  return out;
}

/**
 * 本地日期字符串（YYYY-MM-DD），用于「今天内不提示」。
 * 用本地时区而不是 UTC：用户理解的「今天」是设备时区的今天。
 * @param {number} ts 毫秒时间戳
 * @returns {string}
 */
export function dayKey(ts) {
  const d = new Date(Number(ts) || Date.now());
  const p = (n) => (n < 10 ? '0' + n : '' + n);
  return d.getFullYear() + '-' + p(d.getMonth() + 1) + '-' + p(d.getDate());
}

/**
 * 读提示状态（存 storage 的 `pencg_update`）。
 * @param {*} raw
 * @returns {{skipVersion:string, quietDay:string}}
 */
export function normalizeUpdateState(raw) {
  let d = raw;
  if (typeof d === 'string') {
    try {
      d = JSON.parse(d);
    } catch (e) {
      d = null;
    }
  }
  const o = d && typeof d === 'object' ? d : {};
  return {
    skipVersion: typeof o.skipVersion === 'string' ? o.skipVersion : '',
    quietDay: typeof o.quietDay === 'string' ? o.quietDay : ''
  };
}

/**
 * 自动检测时该不该弹窗（手动检测永远弹，不走这里）。
 * 三条规则：①该版本被「跳过此版本」过 -> 不弹；②今天点过「今天内不提示」-> 不弹；
 * ③不是更新版本 -> 不弹。任何参数异常都返回 false（宁可不打扰）。
 * @param {{skipVersion?:string, quietDay?:string}} state
 * @param {string} remoteTag
 * @param {string} localVersion
 * @param {number} now
 * @returns {boolean}
 */
export function shouldAutoPrompt(state, remoteTag, localVersion, now) {
  const st = normalizeUpdateState(state);
  if (!isNewer(remoteTag, localVersion)) return false;
  const parsed = parseVersion(remoteTag);
  const tagKey = parsed ? 'v' + parsed.nums.join('.') : '';
  // 跳过记录同时认 'v1.2.3' 与 '1.2.3' 两种写法，避免下一版换了写法就失效
  if (st.skipVersion && (st.skipVersion === remoteTag || st.skipVersion === tagKey ||
      st.skipVersion === (parsed ? parsed.nums.join('.') : ''))) return false;
  if (st.quietDay && st.quietDay === dayKey(now)) return false;
  return true;
}

/**
 * 下载进度文案（按钮上显示，266px 屏放不下进度条，只能给百分比文字）。
 * @param {number} got 已下载字节
 * @param {number} total 总字节（0 = 未知）
 * @returns {string}
 */
export function progressText(got, total) {
  const g = Number(got) || 0;
  const t = Number(total) || 0;
  if (t > 0) {
    const pct = Math.max(0, Math.min(100, Math.round((g / t) * 100)));
    return pct + '%';
  }
  if (g > 0) return (g / 1024 / 1024).toFixed(1) + 'MB';
  return '下载中…';
}

/* ============================ 提示状态的读写 ============================ */

/**
 * 读「跳过此版本 / 今天内不提示」状态。
 * PenCG 的 storage 是 getJSON/setJSON（**大写 JSON**，PenBili 是小写 getJson），
 * 写错就静默拿到 undefined，表现为「跳过了但下次还弹」。
 * @returns {Promise<{skipVersion:string, quietDay:string}>}
 */
export async function loadUpdateState() {
  try {
    return normalizeUpdateState(await getJSON(KEYS.update, null));
  } catch (e) {
    logWarn('[cg] upd state read fail: ' + ((e && e.message) || e));
    return normalizeUpdateState(null);
  }
}

/**
 * 合并写回状态（只覆盖传入的字段）。
 * @param {{skipVersion?:string, quietDay?:string}} patch
 */
export async function saveUpdateState(patch) {
  const st = await loadUpdateState();
  const next = Object.assign({}, st, patch || {});
  try {
    await setJSON(KEYS.update, next);
  } catch (e) {
    logWarn('[cg] upd state write fail: ' + ((e && e.message) || e));
  }
  return next;
}

/** 「跳过此版本」：记下 tag，只有出现**更高**版本时才会再提醒 */
export function rememberSkip(tag) {
  return saveUpdateState({ skipVersion: String(tag || '') });
}

/** 「今天内不提示」：按本地日期记一天 */
export function rememberQuiet(now) {
  return saveUpdateState({ quietDay: dayKey(now === undefined ? Date.now() : now) });
}

/* ============================ 运行时封装 ============================ */

/** 取 $falcon.jsapi.http（本次只用到 downloadFile） */
function httpJsapi() {
  const f = typeof $falcon !== 'undefined' ? $falcon : null;
  return (f && f.jsapi && f.jsapi.http) || null;
}

/**
 * 逐个源试，返回第一个拿到的正式 release。
 *
 * 顺序即优先级（SOURCES 里 GitCode 在前）。为什么要依次而不是并发：
 * 两个平台的限流都很紧（GitCode 429 / GitHub 403），并发只会更快撞限流；
 * 而正常情况下第一个源 1~2 秒就返回了，兜底源大多数时候根本不会被访问。
 *
 * 失败一律 resolve 成 {ok:false, stage, message, tried}，**绝不 reject** ——
 * 检查更新失败不能打断用户的任何正常操作。
 *
 * PenCG 适配点：net.js 的 httpText 返回 { status, body, error, curlCode }。
 * status 为 0 表示传输层就没拿到响应（连不上 / curl 失败），这时 body 是空串，
 * 直接判 transport 而不是往下走解析 —— 否则用户会看到「发布信息解析失败」
 * 这种把网络问题说成数据问题的错误提示。
 *
 * @param {string} [onlyId] 只试某个源（换源重试用）
 * @returns {Promise<{ok:boolean, stage?:string, message?:string, release?:object,
 *                    source?:string, sourceLabel?:string, tried?:string[]}>}
 */
export async function fetchLatestRelease(onlyId) {
  const list = onlyId ? [sourceById(onlyId)] : SOURCES;
  const tried = [];
  let last = { stage: 'transport', message: '没有可用的检测源' };
  for (let i = 0; i < list.length; i++) {
    const src = list[i];
    tried.push(src.id);
    try {
      const r = await httpText(src.api, {
        timeout: 12000,
        headers: Object.assign({}, src.headers, { 'User-Agent': 'PenCG-Updater/' + APP_VERSION })
      });
      const code = r && typeof r.status === 'number' ? r.status : 0;
      if (!code) {
        last = { stage: 'transport', message: src.label + ' 连接失败' + (r && r.error ? ' · ' + r.error : '') };
        logWarn('[cg] upd source ' + src.id + ' transport fail: ' + ((r && r.error) || '-'));
        continue;
      }
      if (code !== 200) {
        // GitCode 在「仓库还没有任何 Release」时返回 400 Release not found，
        // 这不是网络故障，但对用户来说同样是「没拿到版本」，继续试下一个源。
        last = {
          stage: 'http',
          message: src.label + ' HTTP ' + code + (r && r.error ? ' · ' + r.error : '')
        };
        logWarn('[cg] upd source ' + src.id + ' http ' + code + ', next');
        continue;
      }
      const rel = normalizeRelease(r && r.body, src.repoUrl);
      if (!rel) {
        last = { stage: 'parse', message: src.label + ' 发布信息解析失败' };
        logWarn('[cg] upd source ' + src.id + ' parse fail, next');
        continue;
      }
      return { ok: true, release: rel, source: src.id, sourceLabel: src.label, tried: tried };
    } catch (e) {
      last = { stage: 'transport', message: src.label + ' ' + ((e && e.message) || String(e)) };
      logWarn('[cg] upd source ' + src.id + ' transport fail, next');
    }
  }
  return { ok: false, stage: last.stage, message: last.message, tried: tried };
}

/**
 * 下载资产到 SAVE_DIR，返回落盘路径。
 *
 * 为什么用 jsapi 的 downloadFile 而不是 http.request：http.request 只能拿到内存字符串，
 * 200KB 级的二进制 amr 经过 QuickJS 字符串会损坏；downloadFile 是原生直写磁盘。
 * 参数名（url/savePath/saveName）来自 libfalcon.so 的 strings 实测
 * （JQuick::Downloader::downloadFile(url, path, name, size, callback)）。
 *
 * PenCG 适配点：先探测 $falcon.jsapi.http.downloadFile 是否存在，不存在就优雅失败，
 * 不能让它抛出去 —— 调用方（弹窗）要能把「宿主没这个能力」显示成人话。
 *
 * @param {string} url
 * @returns {Promise<{ok:boolean, filePath?:string, message?:string}>}
 */
export function downloadFileToDisk(url) {
  return new Promise((resolve) => {
    const api = httpJsapi();
    if (!api || typeof api.downloadFile !== 'function') {
      resolve({ ok: false, code: 'NO_DOWNLOADER', message: '当前版本不支持下载安装包' });
      return;
    }
    if (typeof url !== 'string' || !/^https?:\/\//i.test(url)) {
      resolve({ ok: false, message: '下载地址无效' });
      return;
    }
    let settled = false;
    const done = (v) => {
      if (settled) return;
      settled = true;
      resolve(v);
    };
    // 硬超时：downloadFile 在本机是异步回调，回调丢失时不能让 UI 永远卡在「下载中」
    const guard = setTimeout(() => done({ ok: false, message: '下载超时' }), 120000);
    try {
      const ret = api.downloadFile(
        { url: url, savePath: SAVE_DIR, saveName: SAVE_NAME },
        (res, err) => {
          clearTimeout(guard);
          if (err) {
            done({ ok: false, message: (err && (err.message || err.errorMessage)) || '下载失败' });
            return;
          }
          const p = extractFilePath(res);
          if (p) done({ ok: true, filePath: p });
          else {
            let dbg = '';
            try { dbg = JSON.stringify(res); } catch (e2) { dbg = String(res); }
            logWarn('[cg] upd downloadFile no path, raw=' + dbg);
            done({ ok: false, message: '未拿到文件路径' });
          }
        }
      );
      if (ret && typeof ret.then === 'function') {
        ret.then(
          (res) => {
            clearTimeout(guard);
            const p = extractFilePath(res);
            if (p) done({ ok: true, filePath: p });
            else done({ ok: false, message: '未拿到文件路径' });
          },
          (e) => {
            clearTimeout(guard);
            done({ ok: false, message: (e && e.message) || '下载失败' });
          }
        );
      }
    } catch (e) {
      clearTimeout(guard);
      done({ ok: false, message: (e && e.message) || String(e) });
    }
  });
}

/**
 * 安装 .amr —— **PenCG 目前没有可用的安装通道**，本函数恒返回失败。
 *
 * 事实（已 grep 全仓核实，见 docs/UPDATER.md）：
 *   - PenBili 的安装走 native `player2.installAmr(path)`，内部调
 *     `/bin/miniapp_cli install <path>`（源码见 PenBili/native/csrc/jsapi_player2.c
 *     的 js_install_amr，约 495-555 行）。
 *   - PenCG 的 native 模块是 cgnative / cgmedia，`CG_EXPORTS` 宏里
 *     （native/csrc/cgnative/cgnative.c 约 3004-3029 行）只有
 *     postSync/requestSync/wsOpen/sha256Hex 等，**没有 installAmr**。
 *   - `$falcon.jsapi` 里也没有 pm 模块（PenBili 真机实测，模块名池只有
 *     http/storage/modal/nvue/updater），JS 侧调 pm.installPackage 永远是 undefined。
 * 所以这里不能假装能装：下载完成后如实告诉用户「手动安装」，
 * 而不是转圈转到超时。等 native 侧补上 installAmr 后，只改这一个函数即可
 * （把下面的注释块打开，其余逻辑与 PenBili 完全一致）。
 *
 * @param {string} filePath 已落盘的本地绝对路径
 * @returns {Promise<{ok:boolean, code?:string, message?:string}>}
 */
export async function installAmr(filePath) {
  if (!filePath || typeof filePath !== 'string') {
    return { ok: false, code: 'NO_PATH', message: '安装包路径为空' };
  }
  logWarn('[cg] upd install unsupported, amr at ' + filePath);
  return {
    ok: false,
    code: 'NO_INSTALLER',
    message: '当前版本不支持自动安装，请手动下载'
  };
  /* ---- native 侧补上 installAmr 之后，把上面的 return 换成下面这段 ----
  let mod = null;
  try {
    mod = await loadNative();
  } catch (e) {
    return { ok: false, message: 'native 模块不可用: ' + ((e && e.message) || e) };
  }
  if (!mod || typeof mod.installAmr !== 'function') {
    return { ok: false, code: 'NO_INSTALLER', message: '当前版本不支持自动安装，请手动下载' };
  }
  let r = null;
  try {
    r = await mod.installAmr(filePath);
  } catch (e) {
    return { ok: false, message: '安装调用抛错: ' + ((e && e.message) || e) };
  }
  const o = r && typeof r === 'object' ? r : {};
  if (o.ok) {
    logWarn('[cg] upd install ok via miniapp_cli rc=' + (o.rc == null ? '?' : o.rc));
    return { ok: true, rc: o.rc };
  }
  logWarn('[cg] upd install fail code=' + (o.code || '-') + ' msg=' + (o.error || '-'));
  return { ok: false, code: o.code || '', message: o.error || '安装失败', rc: o.rc };
  ---- */
}

/**
 * 本机是否具备「下载 + 安装」全链路能力（UI 据此决定是否显示「立即更新」）。
 * 只有下载能力没有安装能力时，按钮应改成「下载安装包」并说明要手动装。
 * @returns {{canDownload:boolean, canInstall:boolean}}
 */
export function updateCapability() {
  const api = httpJsapi();
  return {
    canDownload: !!(api && typeof api.downloadFile === 'function'),
    // 与 installAmr 保持一致：PenCG 当前恒 false
    canInstall: false
  };
}
