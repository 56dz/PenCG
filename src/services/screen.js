// 屏幕保活（播放态必须，否则 270s 自动灭屏 / 300s 背光断电）
//
// ★ 为什么这个文件必须是"完整版"（详见 docs/BUGFIX-bug1-lifecycle.md）
//
// 本固件 /etc/input-event-daemon.conf 的 [Idle] 段（真机实测原文）：
//     270s = hal-screen down; hal-gsensor wakeup 1     ← 背光掉到 bl_br=5
//     300s = len_onoff.sh off; hal-screen off; freq_adj low
//     900s = system_sleep_wakeup                        ← 这个会杀进程
// 270s = 4 分 30 秒，正是用户报的「游戏打开约四分钟后停了、像自动退出」。
//
// 旧版本只有 117 行：只有 keepScreenOn() 一种手段，而且后端探测只覆盖
// `$falcon.jsapi.brightness` 的扁平形态一种。真机旧日志里命中的恰恰是
// `new Brightness()` 形态（见 PenBili/src/services/screen.js:478-523 的探测链），
// 所以这里一旦 miss，keepOnce() 永远返回 false，定时器空转，
// [Idle] 270s 照常触发 → 屏幕变暗/断电 → Falcon 把 App 判成后台：
//   · app.js onHide / play.vue bgTeardown 拆管线 + 退机位 → 用户观感"自己退出了"；
//   · 若 onHide 没到，QuickJS 定时器被系统挂起 → media.js 的 120ms poll 停摆
//     → cm_media.c 的 60s 心跳看门狗 code=-6 拆管线（第二条链）。
// 所以"保活到底有没有真的生效"必须能从日志一眼看出来，本文件每轮都打一行。
//
// 真机教训（PenBili 2026-10/11 两次实测，别再翻案）：
//   1) keepScreenOn() **只续「30s 不动作窗口」**，不恢复已经掉下去的背光。
//      被 idle 调到 bl_br=5 之后，只有 setBrightness / screenOnOff 能拉回来。
//      所以保活周期必须 < 30s（这里 20s）。
//   2) getBrightness() / hal-screen get 返回的 80 是**设置档位**，不是实际 bl_br：
//      屏幕被调暗到 5 时它仍然返回 80。**主判据必须是 getScreenState()**。
//   3) 深熄态（背光断电 bp=4）下，第 1 次 keep 只把 state 置 1、bp 仍是 4，
//      **第 2 次 keep 才真正点亮** → keep 完必须复检 state，仍是 OFF 就同 tick 补一次。
//   4) getScreenState() 语义：真机被证实的**只有 0 = 亮屏**。1/3/4 这些编号来自
//      hal-screen（HAL 命令行层）的另一套数，跟 JSAPI getScreenState() 不是一个层，
//      返回值不能互证；JS 侧日志里 1/3/4 一条都没出现过（详见
//      docs/SCREEN-STATE-FINDINGS.md）。所以判据一律走 isScreenNotOn()：
//      「0 = 亮屏，其它已知数字 = 非亮屏，读不到 = 退回亮度判据」，
//      对"3 到底存不存在"完全不敏感。日志里也只承认 0，其余打印成 '?<原值>'。
//
// JS 层不能跑 shell，所以不调 system()/native.exec；后端不可用时只降级 + 打一行日志，
// 绝不影响播放本身。
import { logWarn } from './log.js';

/* 保活周期。必须 < 30s —— keepScreenOn 自身只续 30 秒窗口。
 * 真机实测 20s 足够稳（PenBili 连续对局 1h+ 无掉屏）。 */
export const KEEP_INTERVAL_MS = 20000;

/* 亮度兜底阈值。
 * 正常 bl_br=113；被 idle 调暗后 bl_br=5。阈值 20 离 5 有安全余量，
 * 又远低于正常档位；恢复档位取 sys_br 默认值 80。
 * 只在 getScreenState() 读不到时当兜底判据用（见 needsBrightRestore）。 */
export const KEEP_BRIGHT_MIN = 20;
export const KEEP_BRIGHT_RESTORE = 80;

/* 「息屏超时」兜底（第二种打法，比 20s 轮询干净，但压不过 [Idle]）。
 * 1080s = 18 分钟：覆盖系统 10 分钟息屏设置 + 余量，又低于 1200s
 * （/usr/bin/check_stop_input_deamon_times.sh 超过 1200s 会 killall -USR2 重置）。 */
export const KEEP_CLOSE_SCREEN_SEC = 1080;

/* ---- 失败退避 + 失败日志限流（REVIEW L3）----
 *
 * 修的是什么：旧 armTick 只要 keepOnce() 返回 false 就固定 1s 重试，**无退避、无上限**。
 * 若 brightness JSAPI 永久坏掉（构建 warning 里"未找到模块 brightness"就是真实可能的
 * 状态），就会变成「每 1s 一次探测 + 每 1s 一条日志」的长期风暴，把
 * /userdata/applog/loli_s6_dev.txt 灌爆（该文件已 5.9MB，/userdata 只剩 55MB，
 * 且 JS 侧没有 fs JSAPI，删不了文件）。
 *
 * 两条独立的修法，缺一不可：
 *   ① 退避：1s → 2s → 4s → 8s → 16s → 20s 封顶。**不降级、不停止重试** ——
 *      保活是 bug1 的命脉，后端坏掉时更不能放弃（哪怕只有一条 API 活着也要续）。
 *   ② 日志限流：失败行每 SCREEN_FAIL_LOG_MS(30s) 最多一条，其余只计数不落盘；
 *      成功一次立刻清零并把静默条数补报出来。
 *      成功行不限流：正常态 20s 一条（3 条/分钟），远低于任何阈值。 */
export const SCREEN_BACKOFF_BASE_MS = 1000;
export const SCREEN_BACKOFF_MAX_MS = 20000;
export const SCREEN_FAIL_LOG_MS = 30000;
export const SCREEN_BACKOFF_LOG_AFTER = 5;

// 模块级单例状态：同一时刻只允许一条保活定时器（旧版 state.timer 单例语义保留）
const state = {
  mod: null,          // 命中后的后端（见 makeBackend）
  probed: false,      // 异步探测是否已结束
  loading: false,     // 异步探测是否在飞
  warned: false,      // 降级日志只打一次
  ticks: 0,           // keepScreenOnce 调用计数（日志里的 #N）
  alwaysArmed: false, // 本轮会话是否已调过 startAlwaysScreenOn
  timer: 0,           // 当前 setTimeout 句柄
  active: false,
  intervalMs: KEEP_INTERVAL_MS,
  lastState: -1,
  failStreak: 0,      // 连续失败次数（成功即清零），驱动退避
  backoffMs: 0,       // 当前退避间隔（0=没在退避）
  failLoggedAt: 0,    // 上一条失败日志的时刻（0=当前没有失败段）
  failSuppressed: 0   // 本轮失败段里被限流静默掉的条数
};

function logLine(msg) {
  // 真机只有 console.error 落盘（/userdata/applog/loli_s6_dev.txt），
  // console.warn / console.log 都不落盘。统一走 log.js 的 logWarn（内部 console.error）。
  try { logWarn('[screen] ' + msg); } catch (e) { /* 忽略 */ }
}

function warnOnce(msg) {
  if (state.warned) return;
  state.warned = true;
  logLine(msg);
}

/* 失败日志限流器（REVIEW L3 的第二半）。
 * 只给"失败/异常"行用：每 SCREEN_FAIL_LOG_MS 最多落一条，其余只累加计数；
 * 下一次真正落盘时把静默掉的条数一并报出来，避免"限流把诊断信息吃干净"。
 * 成功行不要走这里 —— 正常态 20s 一条本来就很稀。 */
function failLog(text) {
  const now = Date.now();
  if (state.failLoggedAt && now - state.failLoggedAt < SCREEN_FAIL_LOG_MS) {
    state.failSuppressed++;
    return;
  }
  const sup = state.failSuppressed;
  state.failSuppressed = 0;
  state.failLoggedAt = now;
  logLine(text + (sup ? '（上次之后另有 ' + sup + ' 条被限流）' : ''));
}

/* keepOnce 返回 ok 之后调用：集中算出「下一次多久后再试」，并维护退避状态。
 * 成功 → 立刻清零退避、回到 intervalMs。
 * 失败 → 1s/2s/4s/8s/16s/20s 阶梯，**永不停止重试**（保活是 bug1 的命脉）。 */
function nextDelay(ok) {
  if (ok) {
    state.failStreak = 0;
    state.backoffMs = 0;
    // 成功即认为"失败段"结束：下一次再失败要能立刻落一条日志，
    // 否则会被上一段的 30s 窗口白白吃掉（后端时好时坏时最难查）。
    state.failLoggedAt = 0;
    state.failSuppressed = 0;
    return state.intervalMs;
  }
  state.failStreak++;
  const shift = Math.min(state.failStreak - 1, 5);
  const ms = Math.min(SCREEN_BACKOFF_MAX_MS, SCREEN_BACKOFF_BASE_MS << shift);
  state.backoffMs = ms;
  if (state.failStreak === SCREEN_BACKOFF_LOG_AFTER) {
    failLog('保活连续失败 ' + state.failStreak + ' 次，退避到 ' + ms + 'ms 上限 ' +
      SCREEN_BACKOFF_MAX_MS + 'ms（不停止重试）');
  }
  return ms;
}

function errText(e) {
  if (!e) return '未知错误';
  return String((e && e.message) || e);
}

function numText(v) {
  return v == null ? 'null' : String(v);
}

/* getScreenState() 返回值的日志名。
 * 真机上唯一被证实的事实只有「0 = 亮屏」（.tmp/bug1-soak.log 里几十条 keep #N 全是 state=0(ON)）。
 * 1 / 3 / 4 的语义来自 hal-screen（HAL 命令行层）那套独立编号，而 JSAPI getScreenState()
 * 是另一个层，两层的返回值不能互证（docs/SCREEN-STATE-FINDINGS.md §2–§5）：
 * 在 JS 侧日志里 state=1 / 3 / 4 一条都没出现过。把未证实的编号当事实印进日志，
 * 会误导后来人以为 "OFF/DIM" 是验证过的，所以这里只承认 0，其余数字原样回显成 '?<原值>'，
 * 既能按原值 grep 统计，又不会伪造语义。 */
export function screenStateName(v) {
  if (v == null) return '未知';
  const n = Number(v);
  if (!isFinite(n)) return '未知';
  if (n === 0) return 'ON';
  return '?' + n;
}

/* 是否「非亮屏」（纯函数）。这是整个项目里唯一该用来判断屏幕状态的入口。
 * 判据只依赖一个已证实事实：0 = 亮屏（docs/SCREEN-STATE-FINDINGS.md §7.1）。
 *   1) 读到有效数字且不是 0 → 按非亮屏处理。这样对「3 到底存不存在」完全不敏感，
 *      不需要再猜 1/3 的语义。
 *   2) 读不到（null / NaN / 后端不可用）→ **不能**直接当非亮屏：Number(null) === 0
 *      会把"读失败"误判成"亮屏"，反过来 `n !== 0` 又可能把读失败误判成"熄屏"
 *      （docs/BUGFIX-bug1-lifecycle.md:352-353 记的就是这个坑）。
 *      所以先判 st == null 再 Number()，并退回亮度弱判据：
 *      只有 bright 也是有效值且 <= KEEP_BRIGHT_MIN 时才认为非亮屏。
 * 注意亮度也是有名的坑：正常 bl_br=113，被 idle 调暗后 bl_br=5，阈值 20 在中间。 */
export function isScreenNotOn(st, bright) {
  const n = Number(st);
  if (st == null || !isFinite(n)) {
    const b = Number(bright);
    return bright != null && isFinite(b) && b <= KEEP_BRIGHT_MIN;
  }
  return n !== 0;
}

/* 是否需要走「屏幕兜底」把屏幕拉回来（纯函数）
 * 判据统一收敛到 isScreenNotOn()：播放中只要不是 0(ON) 就一律救
 * （用户真按电源键会走 onHide 去释放保活，那时这里根本不会再被调用，
 * 不存在跟用户抢屏幕）。
 * 「读到 1 才额外调 screenOnOff(1) 上电」那一步留在 keepOnce() 的硬兜底里做，
 * 因为 1 有 hal-screen 层的强证据，而未知编号只敢改亮度档位。 */
export function needsBrightRestore(bright, state2) {
  return isScreenNotOn(state2, bright);
}

/* keepScreenOn() 之后是否需要再补一次（深熄态确定性，见文件头教训 3） */
export function keepRetryNeeded(s) {
  if (s == null) return false;
  return Number(s) === 1;
}

/* 把一个模块/实例包成后端。三种形态都能适配：
 *   扁平命名空间（$falcon.jsapi.brightness / $falcon.brightness）、ESM default、类实例。
 * 缺 keepScreenOn 一律视为不是亮度后端；其余能力有则用。
 * 方法一律 call(holder)：`new Brightness()` 实例的方法 this 必须指向实例。 */
function makeBackend(name, holder) {
  if (!holder || (typeof holder !== 'object' && typeof holder !== 'function')) return null;
  const pick = (k) => (typeof holder[k] === 'function' ? holder[k] : null);
  const keep = pick('keepScreenOn');
  if (!keep) return null;
  const always = pick('startAlwaysScreenOn');
  const stopAlways = pick('stopAlwaysScreenOn');
  const getBright = pick('getBrightness');
  const setBright = pick('setBrightness');
  const getState = pick('getScreenState');
  const setClose = pick('setCloseScreenTime');
  const getClose = pick('getCloseScreenTime');
  const onOff = pick('screenOnOff');
  return {
    name: name,
    keep: function () { return keep.call(holder); },
    always: always ? function () { return always.call(holder); } : null,
    stopAlways: stopAlways ? function () { return stopAlways.call(holder); } : null,
    getBright: getBright ? function () { return getBright.call(holder); } : null,
    setBright: setBright ? function (v) { return setBright.call(holder, v); } : null,
    getState: getState ? function () { return getState.call(holder); } : null,
    setClose: setClose ? function (v) { return setClose.call(holder, v); } : null,
    getClose: getClose ? function () { return getClose.call(holder); } : null,
    onOff: onOff ? function (v) { return onOff.call(holder, v); } : null
  };
}

function makeBackendFromClass(name, Cls) {
  if (typeof Cls !== 'function') return null;
  let inst = null;
  try {
    inst = new Cls();
  } catch (e) {
    return null;
  }
  return makeBackend(name, inst);
}

let probeSummaryLogged = false;
function noteProbe(shape, hit, detail) {
  logLine('probe 形态=' + shape + ' → ' + (hit ? '命中' : 'miss(' + detail + ')'));
}

function logProbeSummary() {
  if (probeSummaryLogged) return;
  probeSummaryLogged = true;
  const b = state.mod;
  if (!b) {
    logLine('后端探测结论：全部未命中 → 保活降级（需 native 侧兜底）');
    return;
  }
  logLine('后端探测结论：采用 ' + b.name +
    ' 能力[keep=1 always=' + (b.always ? 1 : 0) + ' stopAlways=' + (b.stopAlways ? 1 : 0) +
    ' getBright=' + (b.getBright ? 1 : 0) + ' setBright=' + (b.setBright ? 1 : 0) +
    ' getState=' + (b.getState ? 1 : 0) +
    ' setClose=' + (b.setClose ? 1 : 0) + ' getClose=' + (b.getClose ? 1 : 0) +
    ' onOff=' + (b.onOff ? 1 : 0) + ']');
}

/* 同步形态 ①：$falcon.jsapi.brightness（扁平命名空间，运行时若已注入最省事）
 * ②：$falcon.brightness（platform-pitfalls.md 的示例写法，部分固件挂这一层） */
let syncProbeLogged = false;
function probeFlat() {
  const f = typeof $falcon !== 'undefined' ? $falcon : null;
  if (!f) {
    if (!syncProbeLogged) {
      syncProbeLogged = true;
      noteProbe('$falcon', false, '$falcon 未定义');
    }
    return null;
  }
  let hit = null;
  const a = f.jsapi && f.jsapi.brightness;
  hit = makeBackend('$falcon.jsapi.brightness', a);
  if (hit) {
    state.mod = hit;
    state.probed = true;
    logProbeSummary();
    return hit;
  }
  const b = f.brightness;
  hit = makeBackend('$falcon.brightness', b);
  if (hit) {
    state.mod = hit;
    state.probed = true;
    logProbeSummary();
    return hit;
  }
  if (!syncProbeLogged) {
    syncProbeLogged = true;
    noteProbe('$falcon.jsapi.brightness', false, a ? '缺 keepScreenOn' : '未注入');
    noteProbe('$falcon.brightness', false, b ? '缺 keepScreenOn' : '未注入');
  }
  return null;
}

/* 异步形态 ③④⑤：裸模块 'brightness'（运行时 require）→ 全局代理 'global'。
 * 命中顺序与 PenBili 一致：default 扁平 → new Brightness() → new Global() → global 扁平。
 * 只探一次，不阻塞调用方；失败只降级。 */
function probeAsync() {
  if (state.loading || state.mod) return;
  state.loading = true;
  Promise.resolve()
    .then(function () { return import('brightness'); })
    .then(function (m) {
      const d = m && m.default ? m.default : m;
      let hit = makeBackend('brightness.default(扁平)', d);
      noteProbe('brightness.default(扁平)', !!hit, '缺 keepScreenOn');
      if (hit) return hit;
      const Cls = d && (d.Brightness || (d.default && d.default.Brightness));
      hit = makeBackendFromClass('brightness.Brightness(new)', Cls);
      noteProbe('brightness.Brightness(new)', !!hit, Cls ? '实例缺 keepScreenOn' : '类不存在');
      return hit || null;
    })
    .catch(function (e) {
      noteProbe('brightness 模块', false, '加载失败 ' + errText(e));
      return null;
    })
    .then(function (hit) {
      if (hit) return hit;
      return import('global')
        .then(function (gm) {
          const g = gm && gm.default ? gm.default : gm;
          let h = makeBackendFromClass('global.Global(new)', g && g.Global);
          noteProbe('global.Global(new)', !!h, '类不存在或实例缺 keepScreenOn');
          if (h) return h;
          h = makeBackend('global.default(扁平)', g);
          noteProbe('global.default(扁平)', !!h, '缺 keepScreenOn');
          return h || null;
        })
        .catch(function (e) {
          noteProbe('global 模块', false, '加载失败 ' + errText(e));
          return null;
        });
    })
    .then(function (hit) {
      state.loading = false;
      state.probed = true;
      if (hit) state.mod = hit;
      else warnOnce('无 brightness JSAPI（五条回退全部 miss）→ 屏幕保活降级，需 native 侧兜底');
      logProbeSummary();
      return state.mod;
    });
}

function readBright() {
  const b = state.mod;
  if (!b || !b.getBright) return null;
  try {
    const v = Number(b.getBright());
    return isFinite(v) ? v : null;
  } catch (e) {
    return null;
  }
}

function writeBright(val) {
  const b = state.mod;
  if (!b || !b.setBright) return { ok: false, err: 'setBrightness 能力缺失' };
  try {
    const r = b.setBright(val);
    if (r === false) return { ok: false, err: 'setBrightness 返回 false' };
    return { ok: true, err: '' };
  } catch (e) {
    return { ok: false, err: errText(e) };
  }
}

function readState() {
  const b = state.mod;
  if (!b || !b.getState) return null;
  try {
    const v = Number(b.getState());
    return isFinite(v) ? v : null;
  } catch (e) {
    return null;
  }
}

function writeScreenOn(val) {
  const b = state.mod;
  if (!b || !b.onOff) return { ok: false, err: 'screenOnOff 能力缺失' };
  try {
    const r = b.onOff(val);
    if (r === false) return { ok: false, err: 'screenOnOff 返回 false' };
    return { ok: true, err: '' };
  } catch (e) {
    return { ok: false, err: errText(e) };
  }
}

/* 设置息屏超时（一次性，管很久）。压不过 [Idle] 的 270s/300s，只是额外一层保险。 */
function applyCloseScreenTime() {
  const b = state.mod;
  if (!b || !b.setClose) return ' closeScreen=缺接口';
  try {
    const r = b.setClose(KEEP_CLOSE_SCREEN_SEC);
    if (r === false) return ' closeScreen=设' + KEEP_CLOSE_SCREEN_SEC + 's返回false';
    let back = '';
    if (b.getClose) {
      try {
        back = ' 回读=' + numText(b.getClose());
      } catch (e) {
        back = ' 回读异常';
      }
    }
    return ' closeScreen=' + KEEP_CLOSE_SCREEN_SEC + 's ok' + back;
  } catch (e) {
    return ' closeScreen=抛错 ' + errText(e);
  }
}

/* 续期一次。返回值：true=本轮至少一条动作发出并成功（不代表系统一定接受），
 * false=后端未就绪 / 全部动作失败（keeper 据此用短间隔快速重试）。
 * 每调用一次打一行，日志形如：
 *   [screen] keep #3 backend=brightness.Brightness(new) ok bright=113 state=0(ON) always=on closeScreen=1080s ok
 *   [screen] keep #4 ... err=keepScreenOn 抛错 xxx bright=null state=null
 */
export function keepOnce() {
  state.ticks++;
  const n = state.ticks;
  if (!state.mod) probeFlat();
  if (!state.mod) {
    if (!state.probed) probeAsync();
    warnOnce('brightness JSAPI 尚未就绪或缺失 → 本轮保活跳过（需 native 侧兜底）');
    return false;
  }
  const b = state.mod;
  const errs = [];
  let ok = false;
  let alwaysNote = '';
  let closeNote = '';
  let retryNote = '';
  let fix = '';

  // ① 每个保活会话只做一次：startAlwaysScreenOn + setCloseScreenTime(1080)。
  //    两者都是"设一次长期有效"的 API，比每 20s 续期干净；但实测压不过
  //    input-event-daemon 的 [Idle]，所以 ② 的轮询必须保留。
  //    用 alwaysArmed 而不是 n===1：onHide 会 stopAlwaysScreenOn()，
  //    下次 onShow 必须重新武装，否则第二个会话没有持续亮屏保护。
  if (!state.alwaysArmed) {
    if (b.always) {
      try {
        const r = b.always();
        if (r === false) errs.push('startAlwaysScreenOn 返回 false');
        else {
          alwaysNote = ' always=on';
          ok = true;
        }
      } catch (e) {
        errs.push('startAlwaysScreenOn 抛错 ' + errText(e));
      }
    }
    closeNote = applyCloseScreenTime();
    state.alwaysArmed = true;
  }

  // ② 每次续 30s 窗口 —— 这才是保活的真正手段（等价 hal-screen keep，但不恢复亮度）
  try {
    const r = b.keep();
    if (r === false) errs.push('keepScreenOn 返回 false');
    else ok = true;
  } catch (e) {
    errs.push('keepScreenOn 抛错 ' + errText(e));
  }

  // ②b 深熄态：单次 keep 不保证上电（bp=4 → 第 1 次只置 state=1，第 2 次才 bp=0）。
  //     keep 完立刻复检，仍 OFF 就同 tick 再补一次。
  let midState = readState();
  if (b.keep && keepRetryNeeded(midState)) {
    try {
      const r2 = b.keep();
      if (r2 === false) retryNote = ' retry=1(返回false)';
      else {
        retryNote = ' retry=1';
        midState = readState();
      }
    } catch (e) {
      retryNote = ' retry=1(抛错 ' + errText(e) + ')';
    }
  }

  // ③ 硬兜底：keepScreenOn 只续窗口、不保证把已调暗/断电的屏拉回来，所以每轮采样：
  //    非亮屏 → setBrightness(80) 把背光档位拉回（实测能恢复 br=113）
  //    而且只有 state 明确是 1 时才额外 screenOnOff(1) 给背光上电
  //    （实测 setBrightness 不能上电，只能改档位；screenOnOff 才是"开屏"那个动作）
  //    为什么只在 1 时上电：1 有 hal-screen 层的强证据（反汇编 cmp w0,#1 → SCREEN_OFF），
  //    其余编号在 JSAPI 层没有任何证据（未验证），不敢对它们做屏幕开关这种重动作，
  //    只敢改亮度档位——万一编号含义猜错，最坏是亮度被设一次，不会误关/误开屏幕。
  const bright = readBright();
  const st = midState != null ? midState : readState();
  if (needsBrightRestore(bright, st)) {
    const sNum = st == null ? null : Number(st);
    if (sNum === 1) {
      const on = writeScreenOn(1);
      if (on.ok) {
        ok = true;
        fix = ' 兜底=screenOnOff(1)ok';
      } else {
        fix = ' 兜底=screenOnOff(1)err=' + on.err;
        errs.push('开屏兜底失败 ' + on.err);
      }
      const w = writeBright(KEEP_BRIGHT_RESTORE);
      if (w.ok) fix += '+setBrightness(' + KEEP_BRIGHT_RESTORE + ')ok';
      else fix += '+setBrightness err=' + w.err;
    } else {
      const w = writeBright(KEEP_BRIGHT_RESTORE);
      if (w.ok) {
        ok = true;
        fix = ' 兜底=setBrightness(' + KEEP_BRIGHT_RESTORE + ')ok';
      } else {
        fix = ' 兜底=setBrightness(' + KEEP_BRIGHT_RESTORE + ')err=' + w.err;
        errs.push('亮度兜底失败 ' + w.err);
      }
    }
    // 走 failLog 而不是 logLine：这是「屏幕已掉」的异常分支，后端长期坏掉时
    // 它和下面的 keep #N 一样会每轮触发，必须和失败行共用同一个限流窗口。
    failLog('!!! 屏幕已非亮屏 state=' + numText(st) + '(' + screenStateName(st) + ')' +
      ' bright=' + numText(bright) + ' → 走兜底' + fix + ' !!!');
  }

  if (st != null && st !== state.lastState) {
    state.lastState = st;
    logLine('state 变化 → ' + st + '(' + screenStateName(st) + ')');
  }

  // 成功行正常打（20s 一条）；失败行走 failLog 限流（30s 最多一条），
  // 否则后端永久坏掉时这里会随 armTick 每 1~2s 刷一行（REVIEW L3）。
  const summary = 'keep #' + n + ' backend=' + b.name + ' ' +
    (ok ? 'ok' : 'err=' + (errs.length ? errs.join('；') : '未知')) +
    ' bright=' + numText(bright) +
    ' state=' + numText(st) + (st == null ? '' : '(' + screenStateName(st) + ')') +
    alwaysNote + closeNote + retryNote + fix;
  if (ok) logLine(summary);
  else failLog(summary);
  return ok;
}

/* 一次性「请勿息屏」（保留旧导出，页面/探针可能直接调） */
export function keepScreenOnOnce() {
  return keepOnce();
}

/* 读屏幕电源状态 → 0=亮屏 3=调暗 1=熄屏断电 -1=拿不到（旧接口，返回码形态） */
export function screenStateCode() {
  if (!state.mod) probeFlat();
  const v = readState();
  return v == null ? -1 : v;
}

/* 读屏幕电源状态 → 数字或 null（新接口，给 play.vue 的每 20s 存活日志用） */
export function getScreenState() {
  if (!state.mod) probeFlat();
  return readState();
}

/* 一次性开屏（保留旧导出） */
export function screenOn() {
  if (!state.mod) probeFlat();
  const r = writeScreenOn(1);
  return r.ok;
}

/* 异步探测 + 返回后端（旧 initScreen 语义保留：页面在 beginSession 里 await 它） */
export async function initScreen() {
  if (state.mod) return state.mod;
  probeFlat();
  if (state.mod) return state.mod;
  probeAsync();
  return state.mod;
}

function armTick() {
  if (!state.active) return;
  const sent = keepOnce() !== false;
  // 不退避就重试：成功 → intervalMs；失败 → 1s/2s/4s/…/20s（见 nextDelay）。
  state.timer = setTimeout(armTick, nextDelay(sent));
}

/* 播放态保活：每 intervalMs 续一次。幂等性说明：
 * 旧版实现是「先 stop 再 setInterval」，本版保留这个语义（页面 onShow / beginSession
 * 会重复调用），但用 setTimeout 链避免单次续期被系统拖慢后回调堆积。
 * 失败时不再固定 1s 死循环（REVIEW L3）：
 *   - 第 1 次失败仍 1s 后快速重试，避免"后端刚探测完"这种短暂窗口跨过 270s [Idle]；
 *   - 之后按 2s → 4s → 8s → 16s → 20s 退避，**但永不停止重试**；
 *   - 失败日志按 SCREEN_FAIL_LOG_MS(30s) 限流，避免刷爆 loli_s6_dev.txt。
 * keepAliveStart 是"新一轮保活"的起手：退避状态与失败限流窗口一并重置，
 * 否则上一局的失败计数会让新一局一开始就退到 20s。 */
export function keepAliveStart(intervalMs) {
  const ms = Math.max(5000, Math.round(Number(intervalMs) || KEEP_INTERVAL_MS));
  const wasActive = state.active;
  if (state.timer) {
    clearTimeout(state.timer);
    state.timer = 0;
  }
  state.active = true;
  state.intervalMs = ms;
  state.ticks = 0;           // 每轮保活从 #1 开始：两条交错序列 = 定时器泄漏
  state.lastState = -1;
  state.failStreak = 0;
  state.backoffMs = 0;
  state.failLoggedAt = 0;
  state.failSuppressed = 0;
  if (!state.mod) probeFlat();
  const sent = keepOnce() !== false;
  state.timer = setTimeout(armTick, nextDelay(sent));
  logLine('保活启动 ' + ms + 'ms backend=' + (state.mod ? state.mod.name : '探测中') +
    ' ticks=0' + (wasActive ? '(重启，旧定时器已清)' : ''));
  return true;
}

export function keepAliveStop() {
  const wasActive = state.active;
  state.active = false;
  if (state.timer) {
    clearTimeout(state.timer);
    state.timer = 0;
  }
  if (!wasActive) return false;
  // 成对 API：不停掉「持续亮屏」会让系统一直亮着（OLED 还有烧屏风险）
  if (state.mod && state.mod.stopAlways) {
    try {
      state.mod.stopAlways();
    } catch (e) {
      logLine('stopAlwaysScreenOn 抛错 ' + errText(e));
    }
  }
  state.alwaysArmed = false;  // 下次 onShow 重新武装 startAlwaysScreenOn
  logLine('保活停止 ticks=' + state.ticks + '（定时器已清，后台不再保活）');
  return true;
}

export function isKeepingScreen() {
  return state.active;
}

export function keepAliveTicks() {
  return state.ticks;
}

/* 能力快照（排查用：真机日志里 grep '[pencg]' 就能看到命中了哪条回退） */
export function screenDiag() {
  if (!state.mod) probeFlat();
  const m = state.mod;
  return {
    backend: m ? m.name : '',
    probed: state.probed,
    active: state.active,
    ticks: state.ticks,
    available: !!m,
    hasKeep: !!(m && m.keep),
    hasAlways: !!(m && m.always),
    hasState: !!(m && m.getState),
    hasOnOff: !!(m && m.onOff),
    hasSetBright: !!(m && m.setBright),
    hasSetClose: !!(m && m.setClose),
    state: m ? readState() : -1
  };
}
