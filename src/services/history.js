// 本地游玩历史（设备存储 + 内存兜底）
import { KEYS, getJSON, setJSON } from './storage.js';

const MAX = 30;

export async function listPlays() {
  const list = await getJSON(KEYS.history, []);
  return Array.isArray(list) ? list : [];
}

export async function recordPlay(item) {
  if (!item || !item.code) return;
  const list = await listPlays();
  const rest = list.filter((x) => x.code !== item.code);
  rest.unshift({
    code: item.code,
    name: item.name || item.code,
    icon: item.icon || '',
    at: item.at || Date.now(),
    seconds: item.seconds || 0
  });
  await setJSON(KEYS.history, rest.slice(0, MAX));
}

export async function clearPlays() {
  await setJSON(KEYS.history, []);
}

export async function touchPlay(code, seconds) {
  const list = await listPlays();
  const it = list.find((x) => x.code === code);
  if (!it) return;
  it.seconds = (it.seconds || 0) + (seconds || 0);
  it.at = Date.now();
  await setJSON(KEYS.history, list);
}
