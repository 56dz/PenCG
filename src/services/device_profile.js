// 设备显示 profile —— 把「960 是 X6 Pro 的视口宽度」这个写死假设，变成
// 「运行时可探测、探测失败就退回 960」。
//
// 为什么需要这个文件（应用市场新规第 4 条：多机型适配）
//   PenCG 的逻辑画布宽度来自 src/app.js 的 setViewPort(W)。W 必须等于
//   Falcon UI 合成面的宽度，否则整页被等比缩放、且纵向裁掉底部（见
//   src/styles/base.less:3-9 与 docs/PLATFORM.md）。
//   X6 Pro 的合成面是 960x266，写在 app.js 里没问题；X7 Pro 的合成面据
//   参考项目 PenBili 实测是 936x280（部分固件报 960x280），继续用 960 就会
//   整页 0.975 缩放 + 底部约 21px 黑边。
//
// 探测手段（**唯一**可用的通道，且已证实它给出的是框架默认值）
//   `$falcon.env`，由 libfalcon.so 里的 JQuick::JQuickFalconEnv 注入，字段固定为
//   platform / apiVersion / deviceModel / deviceWidth / deviceHeight / custom
//   （SDK 头文件在 native/iot-miniapp-sdk/include/JQuickEnv.h:14-26，注释里给了样例）。
//   `$falcon.env.custom` 支持 $set() 持久化，是最顺手的覆盖通道。
//
// ★ 真机实测结论（2026-11-14 00:23，X6 Pro，1.2.0 包内本文件）★
//   日志原文：
//     [pencg][I] [device] width=960 height=266 source=fallback model=- raw=800x480
//       envModel=HaaS UI platform=Linux api=1
//       note=env 800x480 model="HaaS UI" platform="Linux" api=1 → 退回 960
//   由此确定三件事（这是实测，不再是有依据的推测）：
//     ① **env 确实可读** —— 不是 raw=null。最坏情况（通道整个不通）没有发生。
//     ② 但它报的是 **800x480 / deviceModel="HaaS UI" / platform=Linux**：既不是
//        X6 Pro 的视口 960x266，也不是物理面板 480x960。800x480 是 HaaS 的通用
//        参考分辨率，**这是框架写死的默认值，与真实硬件无关**。
//     ③ 因此 envViewportUsable() 的闸门把它正确拦下，落回 960，X6 Pro 上视觉
//        零回归（逐像素比对：diff>30 的像素数 0、最大差 0，与改动前完全一致）。
//
//   结论：这条通道对「视口自适应」**没有实用价值** —— 它读得到，但不含真值。
//   保留它的两个理由（都不是为了自适应）：
//     (a) 万一某个固件的 env 真填了真值，现有代码无需改动就能自动生效；
//     (b) 它给出的一行日志比 raw=null 更有信息量，能区分「通道不通」和
//         「通道通了但只会喊 800x480」这两种完全不同的现场。
//   → X7 Pro 的视口只能靠 FORCE_WIDTH（或 env.custom.pencgDesignWidth）手工指定。
//     详见 docs/DEVICE-MATRIX.md §7。
//
// 仍然未验证的
//   ① X7 Pro 一次都没跑过（手头没有该机型），env 在它上面报什么未知；
//   ② 时序风险已排除：onLaunch 里确实能拿到值（真机日志有 raw=800x480 为证）。
//
// 已知未完成项（不要读成本文件已解决）
//   src/styles/base.less 里还有硬编码 px 布局（.page 宽 960、.bar 宽 76、
//   .content 宽 884），本文件只解决画布宽度，**不解决布局弹性**。如果
//   画布被改成 936，.page 的 960px 会溢出 24px。
//   所以真要在 X7 Pro 上发布，还需要把 base.less 的这几个宽度改成
//   vw（见 docs/DEVICE-MATRIX.md §7 的待办清单）。

/* 兜底画布宽度：X6 Pro 的合成面宽度，也是全仓 CSS 的设计基准（vw = px / 9.6）。
 * 任何探测失败都必须落回这个值 —— 宁可维持现状，也不要因为探测出错导致整页缩放。 */
export const FALLBACK_WIDTH = 960;

/* 兜底画布高度。仅用于日志/文档对照，不参与 setViewPort。 */
export const FALLBACK_HEIGHT = 266;

/* 强制宽度覆盖。0 = 自动探测。
 * 真机上如果发现自适应反而更糟（例如 base.less 还没参数化导致溢出），
 * 把它改成 960 重新构建即可回到旧行为，不用回滚整个文件。
 *
 * ★ 在 X7 Pro 上这是**唯一可达**的设定宽度手段：env 通道已被 X6 Pro 实测
 *   证伪（只会报 800x480 默认值，见文件头），所以换机型时要么改这里再构建，
 *   要么在真机上用 $falcon.env.custom.$set('pencgDesignWidth', 936) 免构建覆盖。 */
export const FORCE_WIDTH = 0;

/* 已知机型的参考档（来源：reference 项目 PenBili/src/services/device_profile.js
 * 的 X6P/X7P 默认值）。这里只用于日志里标注「认出了哪台」，**不参与**宽度决策 ——
 * 宽度决策只认 env 实测值，认不出机型也无所谓。 */
export const KNOWN_MODELS = {
  /* 用 indexOf 而不是正则字面量：全仓 src/ 此前没有一个正则字面量，
   * 这个运行时（QuickJS）到底怎么编译 /x6/i 没有先例可参照，
   * 而机型识别只是日志标注用途，不值得为它冒构建或运行期的险。 */
  x6p: { tag: 'x6', width: 960, height: 266, panelXOffset: 107, planeId: 76, swScale: 0 },
  x7p: { tag: 'x7', width: 936, height: 280, panelXOffset: 0, planeId: -1, swScale: 1 }
};

function num(v, fallback) {
  const n = typeof v === 'number' ? v : parseFloat(v);
  return isFinite(n) ? n : fallback;
}

/* 安全读取运行时环境。Node 单测 / 构建期没有 $falcon，绝不能抛。 */
export function readFalconEnv() {
  try {
    const f = typeof $falcon !== 'undefined' ? $falcon : null;
    const e = f && f.env ? f.env : null;
    if (!e) return null;
    return {
      width: Math.round(num(e.deviceWidth, 0)),
      height: Math.round(num(e.deviceHeight, 0)),
      model: String(e.deviceModel || ''),
      platform: String(e.platform || ''),
      apiVersion: num(e.apiVersion, 0),
      hasCustom: !!(e.custom && typeof e.custom === 'object')
    };
  } catch (err) {
    return null;
  }
}

/* 同步读 $falcon.env.custom 里的某个键。custom 的读取方式在不同固件上可能是
 * 「普通属性」或「$get(k)」两种形态（SDK 头文件只写了写入用 $set），所以两种
 * 都试一遍，任何异常都当读不到。 */
export function readCustomEnv(key) {
  try {
    const f = typeof $falcon !== 'undefined' ? $falcon : null;
    const c = f && f.env ? f.env.custom : null;
    if (!c) return null;
    if (typeof c.$get === 'function') {
      const v = c.$get(key);
      if (v !== undefined && v !== null && v !== '') return v;
    }
    const v2 = c[key];
    if (v2 !== undefined && v2 !== null && v2 !== '') return v2;
  } catch (err) {
    /* ignore */
  }
  return null;
}

/* env 可信度闸门。
 *
 * SDK 只说 deviceWidth/deviceHeight 是「设备分辨率」，没说旋转前还是旋转后。
 * 已知事实：X6 Pro 视口 960x266（宽高比 3.61）、X7 Pro 936x280（3.34），
 * 而两台的物理面板都是 480x960（宽高比 ≤ 2）。所以判据取
 * 「宽 ≥ 600 且 宽高比 ≥ 2.2」才认为这是可用的**视口**尺寸；
 * 否则整组丢弃（不能只丢宽：480x960 会把高度当成 960），退回 960。
 *
 * 这条闸门是「不确定就宁可不探测」的具体落实：读错方向的面板尺寸
 * 会导致 setViewPort(480)，那等于把整页放大 2 倍、底部裁掉 3/4。
 *
 * 真机实证（X6 Pro 2026-11-14 00:23）：env 报的是 800x480 —— 宽够 600，
 * 但宽高比 1.67 < 2.2，被本闸门拦下，落回 960。也就是说这条闸门不是
 * 纸面保险，它已经在真机上实际起过作用（详见文件头的实测日志）。 */
export function envViewportUsable(env) {
  if (!env) return false;
  if (!(env.width >= 600) || !(env.height > 0)) return false;
  return env.width / env.height >= 2.2;
}

/* 纯函数：决定画布宽度。返回 { width, source, note, device }。
 * source 取值：'force' | 'custom' | 'env' | 'fallback'，用于日志与文档对照。
 *
 * 按 X6 Pro 实测（见文件头），env 报 800x480 恒被闸门拦下，所以真实设备上
 * 大概率只会看到 'fallback'。要换宽度，用 FORCE_WIDTH 或下面的 custom 覆盖通道。 */
export function pickDesignWidth(input) {
  const env = input && input.env !== undefined ? input.env : readFalconEnv();

  let model = '';
  try {
    const m = String((env && env.model) || '').toLowerCase();
    if (m.indexOf(KNOWN_MODELS.x7p.tag) >= 0) model = 'x7p';
    else if (m.indexOf(KNOWN_MODELS.x6p.tag) >= 0) model = 'x6p';
  } catch (e) {
    model = '';
  }

  if (FORCE_WIDTH > 0) {
    return {
      width: Math.round(FORCE_WIDTH),
      source: 'force',
      model: model,
      device: env,
      note: 'FORCE_WIDTH 强制 ' + Math.round(FORCE_WIDTH)
    };
  }

  // 覆盖通道：$falcon.env.custom.pencgDesignWidth（同步可读、自动持久化）。
  // 优先级排在 FORCE_WIDTH 之后、env 之前：目的是让真机取证时能**伪造**
  // 另一台机型的视口（例如在 X6 Pro 上填 936，观察布局在 X7 Pro 宽度下会不会
  // 溢出），不用重新打包，也不用另一台机器。改动会持久化，验完记得清掉
  // （$set('pencgDesignWidth', 0)）。
  // 在 env 通道被证伪之后，这条通道成了**唯一**不用改代码就能换画布宽度的手段，
  // 也是 X7 Pro 到手之前能做的最接近真机的验证（真机日志应出现 source=custom）。
  const ov = Math.round(num(readCustomEnv('pencgDesignWidth'), 0));
  if (ov >= 320 && ov <= 1920) {
    return {
      width: ov,
      source: 'custom',
      model: model,
      device: env,
      note: 'custom.pencgDesignWidth=' + ov + '（覆盖 env ' + describeEnv(env) + '）'
    };
  }

  if (!envViewportUsable(env)) {
    return {
      width: FALLBACK_WIDTH,
      source: 'fallback',
      model: model,
      device: env,
      note: 'env ' + describeEnv(env) + ' → 退回 ' + FALLBACK_WIDTH
    };
  }

  // 走到这里要求 env 同时满足「宽≥600」和「宽高比≥2.2」。X6 Pro 实测报 800x480
  // （宽高比 1.67）够不着，因此该分支在当前固件上**不会被触发**；留着是因为
  // 换固件后 env 若真填了真值，这里就能直接生效，不必再改代码。
  return {
    width: env.width,
    source: 'env',
    model: model,
    device: env,
    note: 'env ' + describeEnv(env) + ' → ' + env.width
  };
}

function describeEnv(env) {
  if (!env) return '缺失';
  return env.width + 'x' + env.height +
    ' model="' + (env.model || '-') + '"' +
    ' platform="' + (env.platform || '-') + '"' +
    ' api=' + (env.apiVersion || '-');
}

/* ---------------- 画布几何（供布局与视频挖孔共用） ----------------
 *
 * 为什么需要它：播放页的 KMS 挖孔矩形过去写成 var VIDEO = {x:243,y:0,w:473,h:266}，
 * 那是**按 960x266 画布算好的绝对值**。换一台画布宽度不同的笔，这个矩形就会
 * 与 CSS 三栏（25.3125vw + 49.2709vw + 25.4167vw）错位，而 KMS 平面是硬件
 * overlay，错位表现为「视频盖住左右栏」或「挖孔里露出页面底色」。
 *
 * 所以把「三栏各占画布宽度的几分之几」固定下来，由画布宽度反算像素矩形。
 * 比例与 base.less / play.vue 的 vw 值严格对应，改一边必须改另一边：
 *     左栏 243 / 960 = 25.3125vw
 *     中栏 473 / 960 = 49.2708vw   <- 视频挖孔
 *     右栏 244 / 960 = 25.4167vw
 */
export const STAGE_LEFT_RATIO = 243 / 960;
export const STAGE_VIDEO_RATIO = 473 / 960;

/* 面板物理宽度（横屏方向的短边）。两台已知机型都是 480x960 的面板，
 * 所以合成面在横向上通常居中：左右死区各 (480 - 合成面宽) / 2。
 * 这个值唯一的作用是「认不出机型时的推导」，认得出就用 KNOWN_MODELS 的实测值。 */
const DEFAULT_PANEL_PHYS_W = 480;

/* bootProfile() 的结果缓存。play.vue / media.js 在页面里 import 时
 * onLaunch 已经跑过，因此这里一定已有值；没有时（例如单元测试直接 import）
 * 就退回 X6 基线，绝不抛。 */
let BOOT_CACHE = null;

/* 横向死区宽度。X6 Pro 是 107（合成面 266 宽居中于 480 面板：
 * (480-266)/2 = 107，与 cm_gst.c 里历史 #define PANEL_X_OFFSET 107 完全一致，
 * 这也是它能被这条推导复现的原因）。
 *
 * ★ 为什么必须推导而不是退回 0：X6 Pro 的 $falcon.env.deviceModel 报的是
 *   "HaaS UI" 而不是机型名，所以 model 识别**恒失败**。若此时退回 0，视频
 *   会整体横移 107px（正好一个死区宽），症状是「左栏空白、右栏被视频盖住」。
 *   换句话说，这条推导是 X6 Pro 上唯一能拿到 107 的路径。 */
function panelXOffsetFor(model, canvasH) {
  const ref = model ? KNOWN_MODELS[model] : null;
  if (ref) return ref.panelXOffset;
  const v = Math.round((DEFAULT_PANEL_PHYS_W - canvasH) / 2);
  return v > 0 ? v : 0;
}

/* 当前画布几何。w/h 是 setViewPort 声明的设计画布尺寸，
 * panel* 是 KMS 合成面在物理面板上的占位（换算见 cm_gst.c 的 apply_rect）。 */
export function canvasGeom() {
  const b = BOOT_CACHE;
  const w = (b && b.cfg && b.cfg.width > 0) ? b.cfg.width : FALLBACK_WIDTH;
  const h = (b && b.cfg && b.cfg.height > 0) ? b.cfg.height : FALLBACK_HEIGHT;
  const dev = (b && b.model) || null;
  return {
    width: w,
    height: h,
    /* 面板换算参数：合成面旋转 90° 后占面板 panelH（= 画布宽度）、
     * panelW（= 画布高度），横向起点 panelXOffset。
     * X6 Pro 实测 107 / 960 / 266，与 cm_gst.c 的历史 #define 完全一致；
     * 认不出机型时由 panelXOffsetFor() 推导（见该函数注释）。 */
    panelXOffset: panelXOffsetFor(dev, h),
    panelH: w,
    panelW: h,
    model: dev,
    source: (b && b.source) || 'fallback'
  };
}

/* 云游戏出的是 16:9（真机 SPS 恒为 1280x720）。
 *
 * 挖孔必须保持 16:9，否则 kmssink 会把画面拉伸 —— 管线里
 * force-aspect-ratio=false（要铺满矩形、不要黑边），所以「矩形不是 16:9」
 * 就直接等于「画面变形」，而不是出现黑边。
 *
 * X6 Pro 上中栏 473x266 恰好就是 16:9（473/266 = 1.778），因此下面的
 * 适配在它身上算出来与历史值逐像素相同，等于没有变化。 */
const VIDEO_ASPECT = 16 / 9;

/* 视频挖孔矩形（画布坐标系，单位=设计像素）。
 *
 * 横向：按三栏比例取中栏，必须与 .p2-left 的 25.3125vw 严丝合缝，
 *       所以宽度优先服从布局，不为了凑 16:9 去动它。
 * 纵向：在中栏高度内按 16:9 反算高度并垂直居中 —— 画布变高时视频不变形，
 *       上下留出的空隙由 .stage 的透明背景让页面底色透出来，视觉上就是
 *       常规的上下留白，不会露出「挖孔没对齐」的错位。 */
export function stageVideoRect() {
  const g = canvasGeom();
  const x = Math.round(g.width * STAGE_LEFT_RATIO);
  const w = Math.round(g.width * STAGE_VIDEO_RATIO);
  let h = Math.round(w / VIDEO_ASPECT);
  let y = 0;
  if (h > g.height) {
    /* 画布比 16:9 更扁（例如有人把宽度调得很大）：改成按高度定，横向居中。
     * X6 Pro 不会走到这里（473/266 正好 16:9，h == height）。 */
    h = g.height;
    y = 0;
  } else {
    y = Math.round((g.height - h) / 2);
  }
  return { x: x, y: y, w: w, h: h };
}

/* 启动期一次性算好的 profile。app.js 只读 cfg.width。 */
export function bootProfile() {
  const picked = pickDesignWidth({});
  const w = picked.width > 0 ? picked.width : FALLBACK_WIDTH;
  const out = {
    cfg: {
      width: w,
      // 高度不参与 setViewPort，只作日志参考：env 可信时用它，否则用 X6 基线。
      height: envViewportUsable(picked.device) ? picked.device.height : FALLBACK_HEIGHT
    },
    source: picked.source,
    model: picked.model,
    device: picked.device,
    note: picked.note,
    /* 布局侧的已知缺口，供日志与文档引用。 */
    layoutFixed: w !== FALLBACK_WIDTH
  };
  BOOT_CACHE = out;
  return out;
}
