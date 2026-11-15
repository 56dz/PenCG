// 用户设置（画质 / 波动策略 / 交互）
import { KEYS, getJSON, setJSON } from './storage.js';
import { normQuality, normPolicy, POLICY_DEFAULT } from './const.js';

const DEFAULTS = {
  quality: 'low',
  /* 网络波动策略：'smooth'（流畅优先，默认）| 'quality'（画质优先）。
     与 quality 正交——quality 决定码率档，policy 决定「丢包后等 IDR 还是换会话」。 */
  policy: POLICY_DEFAULT,
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
  // 旧版本没有 policy 字段（读到 undefined）→ normPolicy 落 'smooth'
  cache.policy = normPolicy(cache.policy);
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
