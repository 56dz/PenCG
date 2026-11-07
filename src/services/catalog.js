// 内容层：首页/分类/搜索/详情（全部走匿名 v1，登录不是必需的）
import { v1 } from './cg.js';
import { imgUrl } from './img.js';
import { logWarn } from './log.js';
import { loadNative } from './native.js';

/* 网易图床会把一部分游戏图标返回成 image/webp（实测 38 个图标里 2 个：
   xttc_cpa「星途天城」和 jql「绝区零」），而 X6 Pro 上 libfalcon / gdk-pixbuf /
   gst 全都没有 webp 解码器（strings 全 0、loader 里也没有），所以这些图标在
   <image> 里就是全透明。图床本身拒绝转格式（25 种 URL 变体实测原样返回 webp）。

   出路：cgnative 的 imgScan 用 Range: bytes=0-63 只看前 12 字节 magic 判定格式，
   imgConv 把 webp 拉下来用设备自带 /usr/bin/ffmpeg 转成 png 落到 cache/lru，
   返回 file:// 路径（falcon 的 <image> 支持 file://，真机已验证能画出来）。
   png/jpg 直接跳过，所以正常情况下只对 2 个图标付出 ~230ms。 */
async function fixWebpIcons(items, keys) {
  if (!items || !items.length) return;
  let mod = null;
  try {
    mod = await loadNative();
  } catch (e) {
    mod = null;
  }
  if (!mod || typeof mod.imgScan !== 'function') return;
  const urls = [];
  for (let i = 0; i < items.length; i++) {
    for (let k = 0; k < keys.length; k++) {
      const u = items[i] && items[i][keys[k]];
      if (u && String(u).indexOf('http') === 0 && urls.indexOf(u) < 0) urls.push(u);
    }
  }
  if (!urls.length) return;
  let map = {};
  try {
    map = JSON.parse(mod.imgScan(JSON.stringify(urls)) || '{}');
  } catch (e) {
    logWarn('[catalog] imgScan 失败');
    return;
  }
  let conv = 0;
  for (let i = 0; i < items.length; i++) {
    for (let k = 0; k < keys.length; k++) {
      const key = keys[k];
      const u = items[i] && items[i][key];
      if (!u || map[u] !== 'webp') continue;
      if (typeof mod.imgConv !== 'function') continue;
      let f = '';
      try {
        f = mod.imgConv(u);
      } catch (e) {
        f = '';
      }
      if (f) {
        items[i][key] = f;
        conv++;
      }
    }
  }
  if (conv) logWarn('[catalog] webp 图标转 png ' + conv + ' 个');
}

const ICON_KEYS = ['icon', 'bigIcon'];

// 把各种卡片形态归一成 UI 能直接吃的游戏项
function pickGame(g) {
  if (!g) return null;
  const d = g.display_game || g;
  const code = d.game_code || g.game_code || '';
  if (!code) return null;
  return {
    code,
    name: d.name || d.game_name || g.game_name || code,
    icon: imgUrl(d.icon || d.recommend_small_icon || g.icon || ''),
    bigIcon: imgUrl(d.recommend_large_icon || d.recommend_banner_icon || ''),
    summary: d.summary || g.activity_sub_title || '',
    tags: (d.tags || d.recommend_tags || []).slice(0, 3),
    btn: g.btn_text || '秒玩',
    gameType: d.game_type || g.game_type || 'mobile',
    carousels: d.carousels || [],
    /* 每日免费秒数。0 = 不免。
     *
     * 这个字段原先只在 loadGameDetail 里取，列表项不带，导致「限免」栏目
     * 没法按它过滤 —— 而服务端根本没有限免列表接口（实测 tab_type/tab_id/
     * sort 传 free 都被忽略，返回的还是同一份 989 条手游列表）。
     * 面板上的「限免」只能靠它自己筛。 */
    freeDaily: Number(d.daily_free_time || g.daily_free_time || 0) || 0
  };
}

function normModule(m, idx) {
  const games = [];
  const seen = {};
  for (const g of m.games || []) {
    const it = pickGame(g);
    if (it && !seen[it.code]) {
      seen[it.code] = 1;
      games.push(it);
    }
  }
  if (!games.length) return null;
  return { id: m.main_recommendation_id || 'mod' + idx, title: m.title || '推荐', games };
}

export async function loadHomeTabs() {
  const j = await v1('api/v1/home_page_tabs');
  const tabs = (j && (j.tabs || j)) || [];
  return tabs
    .filter((t) => t && t.valid !== false)
    .map((t) => ({ id: t.tab_id, name: t.title || t.tab_type || '', type: t.tab_type || '' }))
    .filter((t) => t.id);
}

export async function loadBanners() {
  const j = await v1('api/v1/banners', { banner_type: 'mobile_home' });
  const list = (j && (j.banners || j)) || [];
  return list.map((b) => ({
    id: b.banner_id,
    image: imgUrl(b.image || ''),
    gameCode: b.game_code || '',
    gameName: b.game_name || '',
    link: b.link || ''
  })).filter((b) => b.image);
}

export async function loadNotices() {
  try {
    const j = await v1('api/v1/notices/list', { page: 0, per_page: 6 });
    const list = (j && (j.notices || j)) || [];
    return list.map((n) => ({ id: n.notice_id, text: n.content || '' })).filter((n) => n.text);
  } catch (e) {
    logWarn('[catalog] notices 失败');
    return [];
  }
}

export async function loadRecommendations(tabId) {
  const params = { main_recommendation_type: 'mobile_home' };
  if (tabId) params.tab_id = tabId;
  const j = await v1('api/v1/main_recommendations', params);
  const arr = Array.isArray(j) ? j : (j && (j.modules || j.main_recommendations)) || [];
  const out = [];
  if (Array.isArray(arr)) {
    for (let i = 0; i < arr.length; i++) {
      const m = normModule(arr[i] || {}, i);
      if (m) out.push(m);
    }
  } else {
    for (const k of Object.keys(arr || {})) {
      const m = normModule(arr[k] || {}, Number(k));
      if (m) out.push(m);
    }
  }
  for (let i = 0; i < out.length; i++) await fixWebpIcons(out[i].games, ICON_KEYS);
  return out;
}

// tab_type: recommend / mobile / pc / free / cloud / notime
/* onlyFree: 只要官方「限免专区」的游戏。
 *
 * ★ 正确做法（2026-11-07 用户实拍纠正后定案）：直接调
 *     GET api/v1/games?tag=限免&page=<偏移>&per_page=16
 *   服务端返回 total=16，就是官网「限免专区」那 16 款
 *   （绿梦/篮球少女/龙渊王座/桃园将帅录/星途天城/燃烧纪元/大武道/地城狩猎/
 *     俱乐大玩家/王国大作战/拳皇·命运/机战钢铁巨舰/球球英雄/少女前线2/
 *     英雄没有闪/武侠大明星）。
 *
 * ★ 两个必须记住的坑：
 *   1. `tag` 参数名只能是 `tag`。`tags` / `tag_key` / `tab_type` 全被忽略
 *      （`tag_key=限免` 反而返回 1824 条全量，是个静默失效的陷阱）。
 *   2. 这个接口的 `page` 是**偏移量**不是页码：`page=1` 返回 0 条、
 *      `page=0` 返回 16 条。翻页要写成 `page = 已取数量`。
 *
 * ★ 曾经用过的错误判据（已废弃，别再改回去）：`daily_free_time > 0`。
 *   全量 935 条手游里有 421 条 `free===true`、179 条 `daily_free_time>0`，
 *   会把异环、明日方舟：终末地这些**付费游戏**一起筛进来 —— 用户 m12859 报的就是这个。
 *   「限免专区」的语义是官方活动名单，不是「有免费时长」。 */
export async function loadGameList(opts) {
  const o = opts || {};
  const gameType = o.gameType || 'mobile';
  const params = { page: o.page || 0, per_page: o.perPage || 20, game_type: gameType, src: 4 };
  if (o.tabId) params.tab_id = o.tabId;
  if (o.sort) params.sort = o.sort;
  /* 限免专区走官方 tag 过滤，服务端只返 16 条，不需要本地再筛。
   * page 是偏移量（见函数上方注释），调用方按「已取数量」传即可。 */
  if (o.onlyFree) params.tag = '限免';
  const j = await v1('api/v1/games', params);
  let list = [];
  if (Array.isArray(j)) list = j;
  else if (j && Array.isArray(j.games)) list = j.games;
  while (list.length && typeof list[0] === 'object' && list[0].game_code === 'mobile' && list[0].main) list = list.slice(1);
  let items = list.map(pickGame).filter(Boolean);
  await fixWebpIcons(items, ICON_KEYS);
  return {
    total: (j && j.total) || list.length,
    items: items,
    /* 告诉调用方「这一页筛完还剩不剩」，因为它决定了要不要继续拉下一页。 */
    rawCount: list.length
  };
}

// 把接口的 description（可能是 [{title,con}] 数组、纯字符串数组或字符串）归一成纯文本
function normDesc(d) {
  if (!d) return '';
  if (typeof d === 'string') return d.trim();
  if (!Array.isArray(d)) return '';
  const out = [];
  for (const it of d) {
    if (!it) continue;
    if (typeof it === 'string') {
      if (it.trim()) out.push(it.trim());
    } else {
      const t = (it.title || '').trim();
      const c = (it.con || it.content || it.text || '').trim();
      if (t && c) out.push(t + '\n' + c);
      else if (c) out.push(c);
      else if (t) out.push(t);
    }
  }
  return out.join('\n');
}

export async function loadGameDetail(code) {
  const g = await v1('api/v1/games/' + encodeURIComponent(code));
  if (!g || !g.game_code) throw new Error('游戏不存在');
  const item = pickGame(g);
  item.summary = g.summary || '';
  // description 是 [{title, con}] 对象数组，直接 join 会得到 "[object Object]"。
  // 实测 988 款游戏里大多数没有简介，这里统一拼成纯文本，空值留给页面兜底。
  item.desc = normDesc(g.description);
  item.tags = (g.tags || g.recommend_tags || []).slice(0, 6);
  item.regions = g.regions_online || [];
  item.coinsPerMinute = g.coins_consume_per_minute || 0;
  await fixWebpIcons([item], ICON_KEYS);
  item.freeDaily = g.daily_free_time || 0;
  item.screen = g.screen || 'horizontal';
  item.carousels = g.carousels || [];
  item.supportGamepad = !!g.support_physical_gamepad;
  item.canHangUp = !!g.can_hang_up;
  return item;
}

export async function searchGames(keyword, page) {
  // 真机实测（ui-detail 2026-11-07）：
  //   1) 服务端忽略 keyword，真正生效的参数是 name
  //   2) 不能传 game_type，否则只有 PC 版的游戏（如 影之刃零）会被全部滤掉
  const j = await v1('api/v1/games', {
    page: page || 0,
    per_page: 20,
    src: 4,
    name: keyword
  });
  const list = Array.isArray(j) ? j : (j && j.games) || [];
  return { total: (j && j.total) || list.length, items: list.map(pickGame).filter(Boolean) };
}

export async function loadHotSearches() {
  try {
    const j = await v1('api/v1/hot-searches');
    const list = (j && (j.hot_searches || j)) || [];
    return list.map((h) => ({ code: h.game_code, name: h.game_name }));
  } catch (e) {
    return [];
  }
}

export async function loadHints(keyword) {
  try {
    const j = await v1('api/v1/search_keyword_hints', { keyword });
    const list = (j && (j.search_keyword_hints || j)) || [];
    return list.map((h) => ({ keyword: h.keyword, display: h.display_keyword }));
  } catch (e) {
    return [];
  }
}

export async function loadSpeedUrl(gameType) {
  try {
    return await v1('api/v1/speed_url', { game_type: gameType || 'mobile' });
  } catch (e) {
    return null;
  }
}

export async function loadClientSettings() {
  try {
    return await v1('api/v1/client-settings/@current');
  } catch (e) {
    return null;
  }
}
