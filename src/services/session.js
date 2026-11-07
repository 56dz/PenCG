// 机位申请 / 排队 / 游玩编排
import { v2, v2Post, v2Get, v2Delete, currentToken, currentUserId } from './cg.js';
import { GAME_SIZE, PLATFORM, QUALITY_LABEL } from './const.js';
import { logInfo, logWarn } from './log.js';

function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

// 测速上报：bandwidth 单位是 Mbps（传 bit/s 会被判"请重新测速"）
export async function reportNetworkTest(opts) {
  const o = opts || {};
  const body = {
    latency: o.latency || 30,
    bandwidth: o.bandwidth || 60,
    quality: o.quality || 'low',
    ua: o.ua || '',
    details: [{ region: o.region || 'ghnfs', latency: o.latency || 30 }]
  };
  try {
    return await v2Post('api/v2/network-tests', body);
  } catch (e) {
    logWarn('[session] network-tests 失败:', (e && e.message) || e);
    return null;
  }
}

export async function playingList() {
  try {
    const j = await v2Get('api/v2/users/@me/games-playing');
    return Array.isArray(j) ? j : (j && j.games_playing) || [];
  } catch (e) {
    return [];
  }
}

export async function status() {
  try {
    return await v2Get('api/v2/users/@me/status');
  } catch (e) {
    return null;
  }
}

export async function stopGame(gameCode) {
  try {
    return await v2Delete('api/v2/users/@me/games-playing/' + encodeURIComponent(gameCode));
  } catch (e) {
    return null;
  }
}

export async function fetchTurnCredentials(region) {
  try {
    return await v2Post('api/v2/turn-credentials', { region: region || 'ghnfs' });
  } catch (e) {
    return null;
  }
}

export async function listMediaServers(gameCode) {
  try {
    const j = await v2Get('api/v2/media-servers?game_code=' + encodeURIComponent(gameCode));
    return Array.isArray(j) ? j : [];
  } catch (e) {
    return [];
  }
}

// 申请机位：先释放旧机位 → 测速 → POST tickets；空对象/1209/33803/1288 都重试
// 返回 { gatewayUrl, lockDetail, ticket, region, ticketTtl, raw } 或抛错（带 code）
//
// ★ ticketTtl 的语义（真机实测，别再搞错）：tickets 响应里的 `time_left` 是
// **这张 ticket 的有效期秒数**，不是任何"可游玩时长"。
// 证据：连取三次票，expires 恒为同一时间戳，而 time_left 依次 24/22/20，
// 精确等于 ceil(expires - now)。它只表示"还有多少秒可以用这张票去连网关"。
// 服务端**不下发单局时长限制**，玩家可以一直玩到主动退出。
// （我上一版把它当成"本局上限 25 分钟"显示在 UI 上，是被字段名带偏的错误。）
export async function applyTicket(opts) {
  const o = opts || {};
  const gameCode = o.gameCode;
  if (!gameCode) throw new Error('缺少 game_code');
  if (!currentToken()) throw new Error('未登录');

  const size = o.size || GAME_SIZE[o.gameType === 'pc' ? 'pc' : 'mobile'];
  const quality = o.quality || 'low';

  // ⚠️ region 必须按游戏查，不能写死。
  // 实测：手游区是 gh*/zmd*（ghnfs/ghzcs/gxbxa/ghbtj…），端游区是 pc*/at*
  // （pchnry_d/pchdxzty_d…）。给端游传手游 region 时服务端回
  // 404 {"errcode":1288,"errmsg":"no region avail, please do speed test again"}，
  // 语义是「该游戏在这些 region 没有可用容器」，与测速无关（测速是幌子）。
  // 例：wow_cn_high 的 region 只有 pchnry_d/pchdycry_d/pchbtjry_d/pchdxzty_d… 共 9 个，
  // 用 ghnfs 一轮 10 次全是 1288；换成它自己的 region 立刻拿到 gateway_url。
  let regions = o.regions && o.regions.length ? o.regions.slice(0) : [];
  if (!regions.length) {
    try {
      const servers = await listMediaServers(gameCode);
      const seen = {};
      for (let i = 0; i < servers.length; i++) {
        const rg = servers[i] && servers[i].region;
        if (rg && !seen[rg]) {
          seen[rg] = 1;
          regions.push(rg);
        }
      }
    } catch (e) {
      regions = [];
    }
    if (!regions.length) regions = [o.gameType === 'pc' ? 'pchnry_d' : 'ghnfs'];
    if (regions.length > 6) regions = regions.slice(0, 6);
    logInfo('[session] ' + gameCode + ' 可用 region ' + regions.join(','));
  }

  const body = {
    regions,
    game_code: gameCode,
    codecs: ['h264'],
    width: size.w,
    height: size.h,
    extra: {
      dpi: 3,
      resolution: size.w + 'x' + size.h,
      source_type: 'web',
      network_test_info_list: [{ region: regions[0], latency: o.latency || 30, bandwidth: o.bandwidth || 60 }]
    },
    apk_decoder: { model: '', decoder: [] }
  };

  let lastErr = null;
  for (let attempt = 0; attempt < 10; attempt++) {
    try {
      await stopGame(gameCode);
    } catch (e) {
      /* ignore */
    }
    await sleep(attempt === 0 ? 400 : 3000);

    if (attempt % 3 === 0) {
      await reportNetworkTest({ region: regions[0], latency: o.latency || 30, bandwidth: o.bandwidth || 60, quality });
    }

    try {
      const t = await v2Post('api/v2/tickets', body);
      if (t && t.gateway_url) {
        logInfo('[session] 拿到机位 ' + t.region_name + ' ' + t.gateway_url);
        return {
          gatewayUrl: t.gateway_url,
          lockDetail: t.lock_detail || {},
          ticket: t.ticket || '',
          region: t.lock_detail && t.lock_detail.region,
          regionName: t.region_name || '',
          ticketTtl: Number(t.time_left) || 0,
          screen: t.screen || 'horizontal',
          raw: t
        };
      }
      if (t && t.expires) {
        // 有 expires 无 gateway_url：排队中
        logWarn('[session] 进入排队 expires=' + t.expires);
        return { queued: true, expires: t.expires, raw: t };
      }
      lastErr = new Error('机位响应为空');
    } catch (e) {
      lastErr = e;
      const code = e && e.code;
      logWarn('[session] tickets 第' + (attempt + 1) + '次失败 code=' + code + ' msg=' + (e && e.message));
      if (code === 33803 || code === 1209 || code === 1288 || code === 1811 || code === 1269 || code === 1209) continue;
      if (code === 1251) throw e;
      continue;
    }
  }
  throw lastErr || new Error('申请机位失败');
}

// 排队推送：wss://n.cg.163.com/push/ws
export function pushAuthPayload() {
  return {
    op: 'auth',
    data: {
      user_id: currentUserId(),
      token: currentToken(),
      platform: PLATFORM.MOBILE
    }
  };
}

export async function hangUp() {
  try {
    return await v2Post('api/v2/users/@me/game-hang-up', {});
  } catch (e) {
    return null;
  }
}

export async function saveBallPosition(gameCode, x, y) {
  try {
    return await v2Post('api/v2/users/@me/games/' + encodeURIComponent(gameCode) + '/common_setting', {
      ball_x: x,
      ball_y: y
    });
  } catch (e) {
    return null;
  }
}
