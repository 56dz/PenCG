// 全局常量（网易云游戏 cg.163.com · X6 Pro 960x266 画布）
export const API_BASE = 'https://n.cg.163.com/';

// 服务端按 UA 决定返回 mobile 还是 pc 布局数据；必须带手机 UA 才拿到手游数据。
export const MOBILE_UA =
  'Mozilla/5.0 (Linux; Android 11; Melon) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Mobile Safari/537.36';

export const PLATFORM = { NONE: 0, MOBILE: 1, PC: 2 };

// ★ 画质只保留「流畅」。
// 真机实测（用户 m12491）：除流畅外，标清/高清/蓝光三档画面全是黑白马赛克或纯绿。
// 已排除「换档改变分辨率」（各档 SPS 恒 1280x720）与「我自己的 queue 回归」（已移除），
// 剩下的怀疑是网关侧在高档位下发的码流参数超出本机 mppvideodec 的能力范围。
// 用户已明确授权「干脆全部移除，只保留流畅」，所以这里只留一档，
// detail / play / mine 三处 UI 都由 QUALITIES 驱动，改这里即可全站生效。
export const QUALITIES = ['low'];
export const QUALITY_LABEL = { low: '流畅', middle: '标清', high: '高清', bluray: '蓝光' };
export const QUALITY_DESC = { low: '低码率 省流', middle: '中码率', high: '高码率 推荐', bluray: '蓝光 会员' };

// 手游机位恒 1280x720（lock_detail 决定），端游 1920x1080
export const GAME_SIZE = { mobile: { w: 1280, h: 720 }, pc: { w: 1920, h: 1080 } };

export const API_VERSION = '6.2.2';

// 把任意画质字符串归一到当前允许的档位。
// 旧版本存过 'high'/'bluray' 的设备（settings 里残留），升级后必须自动落到 'low'，
// 否则 auth 里还会报高画质，网关继续下发解不了的码流。
export function normQuality(q) {
  return QUALITIES.indexOf(q) >= 0 ? q : QUALITIES[0];
}
