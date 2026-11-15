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

// ★ 网络波动策略（与画质档位正交，不是码率档）。
//
// 为什么需要它：网关侧三个事实（2026-11-15 真机定案，见
// docs/BUGFIX-video-sink-freeze.md）——
//   ① 一个会话的 IDR 周期可以长达 604 秒；
//   ② 它对 PLI 零响应（整局 506 条 PLI，recov=0）；
//   ③ 它对 NACK 不重传（9 条 NACK，recov=0）。
// 所以「丢一个包」之后就只剩两条路可选，二者互斥、无法同时成立：
//   A. 等 —— 继续用残缺参考帧往前走，画面在动，但错块会一直残留到下一个 IDR
//      （最坏十分钟）。这就是「流畅优先」。
//   B. 不等 —— 立刻换会话（新会话必定给新 IDR，5 秒内出画，进度不丢，因为
//      服务端对局还在跑）。代价是换会话瞬间要拆/建管线，画面会闪黑 1~2 秒。
//      这就是「画质优先」。
// 用户（m17715/m17780）要求把这条取舍显式交给玩家，并在 UI 上标注代价。
export const POLICIES = ['smooth', 'quality'];
export const POLICY_LABEL = { smooth: '流畅优先', quality: '画质优先' };
export const POLICY_DESC = {
  smooth: '波动时继续玩，允许马赛克残留',
  quality: '波动时闪黑重连，画面始终干净'
};
export const POLICY_DEFAULT = 'smooth';

// 手游机位恒 1280x720（lock_detail 决定），端游 1920x1080
export const GAME_SIZE = { mobile: { w: 1280, h: 720 }, pc: { w: 1920, h: 1080 } };

export const API_VERSION = '6.2.2';

// 把任意画质字符串归一到当前允许的档位。
// 旧版本存过 'high'/'bluray' 的设备（settings 里残留），升级后必须自动落到 'low'，
// 否则 auth 里还会报高画质，网关继续下发解不了的码流。
export function normQuality(q) {
  return QUALITIES.indexOf(q) >= 0 ? q : QUALITIES[0];
}

// 把任意策略字符串归一到当前允许的取值（旧版本没有这个字段 → 落默认值）。
export function normPolicy(p) {
  return POLICIES.indexOf(p) >= 0 ? p : POLICY_DEFAULT;
}
