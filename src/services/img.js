// 图片地址归一化：设备网络栈基于 curl，内建 CA 未必覆盖网易图床，
// 真机实测 https 封面图常加载失败 —— 统一降级到 http。
export function imgUrl(u) {
  if (!u || typeof u !== 'string') return '';
  let s = u.trim();
  if (s.indexOf('https://') === 0) return 'http://' + s.slice(8);
  if (s.indexOf('//') === 0) return 'http:' + s;
  return s;
}
