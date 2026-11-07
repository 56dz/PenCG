// 用户设置（画质 / 交互）
import { KEYS, getJSON, setJSON } from './storage.js';
import { normQuality } from './const.js';

const DEFAULTS = {
  quality: 'low',
  showBall: true,
  hideOnTouch: true,
  autoQuality: false,
  volume: 70,
  mute: false,
  gyro: false,
  haptic: true,
  region: ''
};

let cache = null;

export async function loadSettings() {
  if (cache) return cache;
  const s = await getJSON(KEYS.settings, null);
  cache = Object.assign({}, DEFAULTS, s || {});
  // 旧设备可能存着 'high'/'bluray'，当前只允许 low，读出来就纠正
  cache.quality = normQuality(cache.quality);
  return cache;
}

export async function saveSettings(patch) {
  const cur = await loadSettings();
  cache = Object.assign({}, cur, patch || {});
  await setJSON(KEYS.settings, cache);
  return cache;
}

export function settingsSync() {
  return cache || Object.assign({}, DEFAULTS);
}

export async function resetSettings() {
  cache = Object.assign({}, DEFAULTS);
  await setJSON(KEYS.settings, cache);
  return cache;
}
