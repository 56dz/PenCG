// 环形日志 + 设备日志落盘。
//
// ★ 真机实测（2026-11-12 22:05，X6 Pro / firmware 15.10）★
// Falcon 的 JS console 只有 **error 级别**会写进设备日志
//   /userdata/applog/loli_s6_dev.txt，形如：
//       [2026-11-12 22:01:32.296][E][JS] <消息>
// console.warn 与 console.log **完全不落盘**（全盘 grep 不到一行 [W][JS]）。
// 所以这里统一走 console.error，靠行内的 [pencg][W] / [pencg][I] 前缀区分级别。
// 若改回 console.warn，真机上所有 PenCG 日志会静默消失（这个坑踩过一次）。
const MAX = 300;
const buf = [];
let seq = 0;

/* ---- 落盘限流（REVIEW L4 / P2-11）----
 *
 * 问题：JS 侧没有任何文件轮转能力（Falcon 不提供 fs JSAPI，删不了文件），
 * 而 console.error 会被运行时直接追加到 /userdata/applog/loli_s6_dev.txt。
 * 该文件已 5.9MB，/userdata 只剩 55MB —— 我们能控制的只有"写多少"。
 *
 * 做法：令牌桶，只作用于 **落盘**，不作用于内存环形缓冲。
 *   - 内存环形缓冲（MAX=300 条）永远全量记录，页面内「日志」面板不受影响；
 *   - 超出令牌的 I/W 行只进内存、不 console.error；
 *   - **E 级（logError）永不限流** —— 诊断关键路径不能被自己的限流吃掉；
 *   - 限流一旦发生就立刻打一行「限流开启」，窗口滚动时再补一行真实丢弃条数，
 *     保证"日志变少"这件事本身是可观测的，不会被误读成"没出问题"。
 *
 * 取值理由（常量在下面 LOG_BURST_PER_SEC / LOG_BURST_PER_MIN）：
 *   - 正常态实测速率约 0.3~0.4 行/秒（play probe 20s 一条、keep #N 20s 一条、
 *     native 侧 RR/pt histogram 各 10s 一条），低于 8/s 与 200/min 一个数量级，
 *     所以稳态**一条都不会被丢**；
 *   - 8/s 的短时尖峰留给"一次事件几百行"的场景（如重连时序、error dump）；
 *   - 200/min = 3.3/s 均值，比稳态高 8 倍以上，同时把最坏情况从
 *     "无限增长"压到 ≤ 200 行/分钟（≈ 每天 1.7MB 上限，实测量级更低）。 */
const LOG_BURST_PER_SEC = 8;
const LOG_BURST_PER_MIN = 200;

const bucket = { secStart: 0, secCount: 0, minStart: 0, minCount: 0, minDrop: 0, minDropNoted: false };

/* 限流可观测性两条，缺一不可：
 *   ① 窗口内**第一次**超限就立刻打一行「限流已开启」——否则如果应用随后不再
 *      打日志，窗口滚动时的汇总永远等不到，现场会误以为"日志正常"。
 *   ② 窗口滚动时打一行**真实丢弃条数**——①只说明"开始了"，②才给出量级。
 * 这两条都不占令牌（限流生效时它们必须还能发出去），但也不计入计数，
 * 避免汇总结论受自身影响（每分钟最多 2 行开销，可忽略）。 */
function emitNote(text) {
  try {
    console.error('[pencg][W] ' + text);
  } catch (e) {
    /* 忽略 */
  }
}

function rollMinute(now) {
  const dropped = bucket.minDrop;
  bucket.minStart = now;
  bucket.minCount = 0;
  bucket.minDrop = 0;
  bucket.minDropNoted = false;
  if (dropped > 0) emitNote('[log] 日志限流：过去 60s 丢弃 ' + dropped + ' 条（I/W 降噪，E 永不丢）');
}

function takeToken() {
  const now = Date.now();
  if (!bucket.secStart || now - bucket.secStart >= 1000) {
    bucket.secStart = now;
    bucket.secCount = 0;
  }
  if (!bucket.minStart) bucket.minStart = now;
  else if (now - bucket.minStart >= 60000) rollMinute(now);
  if (bucket.secCount >= LOG_BURST_PER_SEC || bucket.minCount >= LOG_BURST_PER_MIN) {
    bucket.minDrop++;
    if (!bucket.minDropNoted) {
      bucket.minDropNoted = true;
      emitNote('[log] 日志限流开启：本分钟 I/W 超 ' + LOG_BURST_PER_SEC + '/s 或 ' +
        LOG_BURST_PER_MIN + '/min，后续只进内存缓冲（E 级不受限）');
    }
    return false;
  }
  bucket.secCount++;
  bucket.minCount++;
  return true;
}

function fmt(args) {
  let text = '';
  for (let i = 0; i < args.length; i++) {
    const a = args[i];
    if (a instanceof Error) text += (i ? ' ' : '') + a.message;
    else if (typeof a === 'object' && a !== null) {
      try {
        text += (i ? ' ' : '') + JSON.stringify(a);
      } catch (e) {
        text += (i ? ' ' : '') + '[obj]';
      }
    } else text += (i ? ' ' : '') + String(a);
  }
  return text;
}

function push(level, args) {
  let text;
  try {
    text = fmt(args);
  } catch (e) {
    text = '[fmt-error]';
  }
  seq += 1;
  buf.push({ i: seq, t: Date.now(), l: level, m: text });
  if (buf.length > MAX) buf.shift();
  // 内存环形缓冲永远全量；只有落盘受限流约束，且 E 级豁免（见文件头）。
  if (level !== 'E' && !takeToken()) return;
  try {
    const line = '[pencg][' + level + '] ' + text;
    // 见文件头：只有 console.error 能到 logcat/设备日志，故一律用 error，
    // 级别只靠行内 [pencg][I/W/E] 前缀表达。
    console.error(line);
  } catch (e) {
    /* 忽略 */
  }
}

export function logInfo() {
  push('I', arguments);
}
export function logWarn() {
  push('W', arguments);
}
export function logError() {
  push('E', arguments);
}

export function getLogs() {
  return buf.slice();
}

export function getLogTail(n) {
  const k = n || 10;
  return buf.slice(Math.max(0, buf.length - k));
}

export function getLogText(n) {
  const rows = getLogTail(n || 30);
  const out = [];
  for (let i = 0; i < rows.length; i++) {
    const d = new Date(rows[i].t);
    const hh = ('0' + d.getHours()).slice(-2);
    const mm = ('0' + d.getMinutes()).slice(-2);
    const ss = ('0' + d.getSeconds()).slice(-2);
    out.push(hh + ':' + mm + ':' + ss + ' ' + rows[i].l + ' ' + rows[i].m);
  }
  return out;
}

export function clearLogs() {
  buf.length = 0;
}
