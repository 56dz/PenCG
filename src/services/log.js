// 环形日志 + 设备日志落盘。
// 真机事实：只有 console.warn / console.error 会写进 /userdata/applog/DictPen_*.log，
// console.log 不落盘 —— 所以每条日志都走 console.warn（带 [pencg] 前缀便于 grep）。
const MAX = 300;
const buf = [];
let seq = 0;

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
  try {
    const line = '[pencg][' + level + '] ' + text;
    if (level === 'E') console.error(line);
    else console.warn(line);
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
