// 游玩会话编排：WS 信令（native ws*）+ 媒体（native cgmedia）
// 信令协议：客户端只发 auth / answer / input / game_set_quality
// 首包 auth 必须明文；之后全部走 offset 加密。
import { loadNative, loadMedia, callJSON } from './native.js';
import { encodeBody, decodeBody, currentOffset, currentToken, currentUserId } from './cg.js';
import { logInfo, logWarn, logError } from './log.js';

const S = {
  wsId: 0,
  wsState: 'idle',
  mediaId: 0,
  offset: 0,
  phase: 'idle', // idle|connecting|authing|waiting_offer|answering|playing|error|closed
  lastError: '',
  onEvent: null,
  pollTimer: 0,
  offerSdp: '',
  answerSdp: '',
  remote: null,
  /* 系统输入法是否正在使用中。系统键盘是**另一个全屏 miniapp**，弹出时
   * Falcon 会给我们发 App 级 onHide —— 而 app.js 的 onHide 会 stopPlay()。
   * 不加这个闸门，用户在云游戏里刚点开输入框，整局就被拆掉了。
   * PenBili 用同样的 imBusy 守卫（src/pages/index/index.vue:3298-3310）。 */
  imBusy: false
};

function parseSdp(sdp) {
  const out = { ufrag: '', pwd: '', fingerprint: '', candidates: [], mlines: [], audioPt: 0, videoPt: 0, videoSsrc: 0, audioSsrc: 0 };
  const lines = String(sdp || '').split(/\r?\n/);
  let cur = null;
  for (const raw of lines) {
    const l = raw.trim();
    if (!l) continue;
    if (l.startsWith('m=')) {
      const tk = l.slice(2).split(' ');
      const kind = tk[0];
      // 注意：m=application 的第 4 段是 sctp 格式名（webrtc-datachannel），不是 payload type，
      // 直接 Number() 会得到 NaN 并写回 answer 造成 m=application ... NaN 的非法行。
      cur = {
        kind,
        fmt: tk[3] || '',
        pts: kind === 'application' ? [] : tk.slice(3).map(Number)
      };
      out.mlines.push(cur);
      continue;
    }
    if (l.startsWith('a=ice-ufrag:')) {
      const v = l.slice('a=ice-ufrag:'.length);
      if (!out.ufrag) out.ufrag = v;
      if (cur) cur.ufrag = v;
    } else if (l.startsWith('a=ice-pwd:')) {
      const v = l.slice('a=ice-pwd:'.length);
      if (!out.pwd) out.pwd = v;
      if (cur) cur.pwd = v;
    } else if (l.startsWith('a=fingerprint:')) {
      out.fingerprint = l.slice('a=fingerprint:'.length);
    } else if (l.startsWith('a=candidate:')) {
      const p = l.slice('a=candidate:'.length).split(' ');
      const c = { foundation: p[0], comp: +p[1], proto: p[2], prio: +p[3], ip: p[4], port: +p[5], type: p[7] };
      out.candidates.push(c);
      if (cur) (cur.candidates = cur.candidates || []).push(c);
    } else if (l.startsWith('a=rtpmap:')) {
      const m = /^a=rtpmap:(\d+) ([^/]+)\/(\d+)(?:\/(\d+))?/.exec(l);
      if (m) {
        const pt = +m[1];
        const name = m[2].toLowerCase();
        if (name === 'opus') out.audioPt = pt;
        else if (name === 'h264') out.videoPt = pt;
        if (cur) {
          cur.rtpmap = cur.rtpmap || {};
          cur.rtpmap[pt] = name;
          // 原始串（含 /clock[/channels]），answer 直接回显，避免二次拼接出错
          cur.rtpmapRaw = cur.rtpmapRaw || {};
          cur.rtpmapRaw[pt] = m[2] + '/' + m[3] + (m[4] ? '/' + m[4] : '');
        }
      }
    } else if (l.startsWith('a=fmtp:')) {
      // H.264 的 profile-level-id / packetization-mode 必须在 answer 里回显，
      // 否则网关无法判定我方支持的 profile，会把这条 m 段判为不匹配。
      const sp = l.slice('a=fmtp:'.length);
      const k = sp.indexOf(' ');
      if (cur && k > 0) {
        cur.fmtp = cur.fmtp || {};
        cur.fmtp[sp.slice(0, k)] = sp.slice(k + 1);
      }
    } else if (l.startsWith('a=ssrc:')) {
      const m = /^a=ssrc:(\d+)/.exec(l);
      if (m) {
        const ssrc = +m[1];
        if (cur && cur.kind === 'audio' && !out.audioSsrc) out.audioSsrc = ssrc;
        if (cur && cur.kind === 'video' && !out.videoSsrc) out.videoSsrc = ssrc;
      }
    } else if (l.startsWith('a=mid:')) {
      if (cur) cur.mid = l.slice('a=mid:'.length);
    } else if (l.startsWith('a=setup:')) {
      if (cur) cur.setup = l.slice('a=setup:'.length);
    } else if (l.startsWith('a=extmap:')) {
      /* RTP 头扩展声明，例如
       *   a=extmap:9 urn:ietf:params:rtp-hdrext:transport-wide-cc-extensions-01
       *   a=extmap:11/sendonly urn:...
       * 数字是 id（1..14 单字节，256+ 走两字节），用来在 RTP 头里定位扩展块。
       * 官方网关的视频段会声明 transport-cc / playout-delay / generic-frame-descriptor。 */
      if (cur) {
        const m = /^a=extmap:(\d+)(?:\/(\w+))?\s+(\S+)/.exec(l);
        if (m) {
          (cur.extmap = cur.extmap || []).push({ id: +m[1], dir: m[2] || '', uri: m[3] });
        }
      }
    } else if (l.startsWith('a=rtcp-fb:')) {
      /* RTCP 反馈能力，例如 `a=rtcp-fb:98 nack pli`、`a=rtcp-fb:* transport-cc`。
       * answer 必须逐条回显自己支持的，否则网关不会发对应的反馈包 ——
       * 对我们而言最要紧的是 transport-cc（网关据此判链路质量）与 nack/pli。 */
      if (cur) {
        const sp = l.slice('a=rtcp-fb:'.length);
        const k = sp.indexOf(' ');
        if (k > 0) {
          (cur.rtcpFb = cur.rtcpFb || []).push({
            pt: sp.slice(0, k),
            name: sp.slice(k + 1),
          });
        }
      }
    }
  }
  return out;
}

// 构造 answer：按 offer 的 m-line 数量与顺序原样回，全部 recvonly（我们只收流）
export function buildAnswer(offerSdp, our) {
  const p = parseSdp(offerSdp);
  const lines = ['v=0', 'o=- ' + Date.now() + ' 1 IN IP4 0.0.0.0', 's=PenCG', 't=0 0'];
  const mids = p.mlines.map((m, i) => m.mid || String(i));
  lines.push('a=group:BUNDLE ' + mids.join(' '));
  lines.push('a=ice-ufrag:' + our.ufrag);
  lines.push('a=ice-pwd:' + our.pwd);
  lines.push('a=fingerprint:' + our.fingerprint);
  p.mlines.forEach((m, i) => {
    /* ★ 关键：拒绝 data channel 段（端口写 0）。
     *
     * 官方客户端优先把 input 走 SCTP DataChannel（run.js:
     *   `if(this.isDataChannel) try{return this.onDataChannelSend(c.cmd)}catch(u){}`），
     * 只有 DC 建不起来或连续失败 `switch_ws_after_x_failure` 次才回落到 WS。
     * 我方没有 SCTP 实现，一旦在 answer 里接受 m=application，网关就会认为
     * 「DC 该走」，把 input 定向到一条永远建不起来的通道 —— 真机表现就是
     * 触摸帧能被网关回显（`2`/`8` 出现在 op=input 里）但游戏毫无反应。
     * 按 RFC 8122/3264，answer 里把该 m 段端口写 0 即表示拒绝，网关随即回落 WS。
     *
     * 视频/音频两段照常协商，互不影响。 */
    if (m.kind === 'application') {
      lines.push('m=application 0 UDP/DTLS/SCTP ' + (m.fmt || 'webrtc-datachannel'));
      lines.push('c=IN IP4 0.0.0.0');
      lines.push('a=mid:' + mids[i]);
      lines.push('a=inactive');
      lines.push('a=ice-ufrag:' + our.ufrag);
      lines.push('a=ice-pwd:' + our.pwd);
      lines.push('a=fingerprint:' + our.fingerprint);
      return;
    }
    const proto = 'UDP/TLS/RTP/SAVPF';
    // application 段回 sctp 格式名；其余段回 PT 列表
    const fmtStr = m.kind === 'application'
      ? (m.fmt || 'webrtc-datachannel')
      : ((m.pts && m.pts.length ? m.pts : [0]).join(' '));
    lines.push('m=' + m.kind + ' 9 ' + proto + ' ' + fmtStr);
    lines.push('c=IN IP4 0.0.0.0');
    lines.push('a=mid:' + mids[i]);
    /* ★ sendrecv 而不是 recvonly。
     *
     * 百度/网易这套网关是 ice-lite 的 SFU，官方浏览器客户端回的 answer 是
     * `a=sendrecv`（2026-11-13 真机抓包 client-answer.sdp 逐段确认）。
     * 只回 recvonly 时网关会把这条流当成「单向订阅」，会话期的 consent /
     * TWCC 记账都按「对端不上行」处理；虽不必然导致断流，但与官方行为不一致
     * 是 bug1 排查里少有的、可无损对齐的差异之一。
     * 我们并不真的发 RTP（没实现编码器），sendrecv 只是声明能力。 */
    lines.push('a=sendrecv');
    if (m.kind !== 'application') {
      lines.push('a=rtcp-mux');
      if (m.rtpmap) {
        for (const pt of Object.keys(m.rtpmap)) {
          const name = m.rtpmap[pt];
          const raw = (m.rtpmapRaw && m.rtpmapRaw[pt])
            || ((name === 'h264' ? 'H264' : name === 'opus' ? 'opus' : name.toUpperCase())
                + '/' + (name === 'opus' ? 48000 : 90000) + (name === 'opus' ? '/2' : ''));
          lines.push('a=rtpmap:' + pt + ' ' + raw);
        }
      }
      // 回显 fmtp（H.264 profile-level-id=42e01f / packetization-mode=1 等）
      if (m.fmtp) {
        for (const pt of Object.keys(m.fmtp)) lines.push('a=fmtp:' + pt + ' ' + m.fmtp[pt]);
      }
      /* ★ 回显 RTP 头扩展（a=extmap）。官方网关的视频段声明了
       *   extmap:11 transport-wide-cc-extensions-01
       *   extmap:12 playout-delay
       *   extmap:13 generic-frame-descriptor-00
       * Chrome 的 answer 是逐条原样回显的（同一次抓包确认）。
       *
       * 但 transport-cc 显式跳过：声明它 = 承诺「我会按 100ms 节奏发
       * RTCP TWCC（PT=205 FMT=15）反馈」。native 侧目前还没有 TWCC 实现，
       * 声明了却不发，网关的发送侧拥塞控制会把它当成「这个接收端一个包
       * 都没收到」，反而更早降档/停推 —— 比不声明更糟。
       * 等 cm_cmd.c 补上 TWCC 后再把 'transport-wide-cc' 从这个过滤表里去掉。 */
      if (m.extmap) {
        for (const x of m.extmap) {
          if (x.uri.indexOf('transport-wide-cc') >= 0) continue;
          lines.push('a=extmap:' + x.id + (x.dir ? '/' + x.dir : '') + ' ' + x.uri);
        }
      }
      /* ★ 回显 RTCP 反馈（a=rtcp-fb）。网关 offer 里每个视频 PT 都带全套
       *   ccm fir / nack / nack pli / goog-remb / rrtr / transport-cc
       * 不回显 = 声明不支持，网关就不发对应的反馈，也不接受我们发。
       * 会话期我们真正在用的是 nack/pli（cm_nack_tick / cm_kf_tick）和
       * RR（cm_rtcp_tick），这几个必须回显。
       * transport-cc 同上理由跳过，理由见上面对 extmap 的注释。 */
      if (m.rtcpFb) {
        for (const f of m.rtcpFb) {
          if (f.name.indexOf('transport-cc') >= 0) continue;
          lines.push('a=rtcp-fb:' + f.pt + ' ' + f.name);
        }
      }
    }
    if (m.kind === 'application') lines.push('a=sctp-port:5000');
    lines.push('a=setup:active');
    // 每段重复 ICE 凭据与指纹：网关逐 m 段解析，只在会话级写会被判凭据缺失。
    // 这是之前 DTLS 完全无响应的另一条可疑路径，与指纹错误并列。
    lines.push('a=ice-ufrag:' + our.ufrag);
    lines.push('a=ice-pwd:' + our.pwd);
    lines.push('a=fingerprint:' + our.fingerprint);
    lines.push('a=ice-options:trickle');
    lines.push('a=end-of-candidates');
  });
  return lines.join('\r\n') + '\r\n';
}

function randToken(n) {
  const cs = 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+/';
  let out = '';
  for (let i = 0; i < n; i++) out += cs.charAt(Math.floor(Math.random() * cs.length));
  return out;
}

// 我方证书指纹必须与 native cgmedia 内嵌证书严格一致。
// native/native/csrc/cgmedia/cm_cert.h 的 CM_CERT_FINGERPRINT 就是这个值，
// cm_dtls_fingerprint() 返回它。之前这里用随机指纹是错的：网关会做
// DTLS 证书校验（RFC 8122 规定必须比对 SDP a=fingerprint），对不上即握手失败。
// 注意：cgmedia 目前未把指纹导出给 JS，故此处与 cm_cert.h 手工同步；
// 若将来改了证书，两边必须一起改。
export const OUR_CERT_FP =
  'sha-256 AF:90:5F:35:76:9A:ED:6F:D1:28:7F:0D:72:94:0B:AC:E0:38:57:99:58:11:EC:93:D6:61:DF:20:49:A5:39:3B';

function randFingerprint() {
  return OUR_CERT_FP;
}

async function onOfferAsync() {
  let mediaErr = '';
  try {
    await startMedia();
  } catch (e) {
    mediaErr = (e && e.message) || String(e);
    logError('[media] startMedia 失败: ' + mediaErr);
    notify({ t: 'mediafail', msg: mediaErr });
  }
  // 无论媒体起没起来都要回 answer —— 不回 answer 网关永远不给流，
  // 而且播放页要能显示「媒体失败」而不是一直转圈。
  try {
    const ans = buildAnswer(S.offerSdp, { ufrag: S.ourUfrag, pwd: S.ourPwd, fingerprint: S.ourFp });
    S.answerSdp = ans;
    const ok = send('answer', { sdp: ans });
    logInfo('[media] 回 answer ok=' + ok + ' len=' + ans.length);
    notify({ t: 'answer', ok, mediaErr });
  } catch (e) {
    logError('[media] 回 answer 异常: ' + ((e && e.message) || e));
  }
}

function notify(ev) {
  if (typeof S.onEvent === 'function') {
    try {
      S.onEvent(ev);
    } catch (e) {
      logWarn('[media] onEvent 异常');
    }
  }
}

export function mediaState() {
  return { phase: S.phase, wsState: S.wsState, mediaId: S.mediaId, error: S.lastError };
}

export function setEventHandler(fn) {
  S.onEvent = fn;
}

// wsState 轮询 + 事件泵
function pump() {
  const tick = () => {
    if (S.wsId) {
      const mod = globalThis.__cgNet;
      if (mod && typeof mod.wsState === 'function') {
        const st = callJSON(mod, 'wsState', [S.wsId]);
        if (st && st.state && st.state !== S.wsState) {
          S.wsState = st.state;
          notify({ t: 'ws', state: st.state, msg: st.msg || '' });
          if (st.state === 'closed' || st.state === 'error') {
            if (S.phase !== 'closed') {
              S.lastError = st.msg || st.state;
              S.phase = st.state === 'error' ? 'error' : 'closed';
              notify({ t: 'wsclose', msg: S.lastError });
            }
          }
        }
        // 鉴权触发不看「状态变化」而看「当前状态」：上一次会话把 S.wsState 留在
        // 'open' 时，新连接同样 open 就不会产生变化，sendAuth 永远不触发。
        // 这里按 phase 判断，phase 只会在 authing/waiting_offer/... 之间前进，
        // 所以不会重复发 auth。
        if (st && st.state === 'open' && S.phase === 'connecting') sendAuth();
        const msgs = callJSON(mod, 'wsPoll', [S.wsId]);
        if (Array.isArray(msgs)) {
          for (const m of msgs) onWsMessage(m);
        }
      }
    }
    if (S.mediaId) {
      const mm = globalThis.__cgMedia;
      if (mm && typeof mm.poll === 'function') {
        const evs = callJSON(mm, 'poll', [S.mediaId]);
        if (Array.isArray(evs)) for (const e of evs) notify(e);
      }
    }
  };
  if (S.pollTimer) clearInterval(S.pollTimer);
  S.pollTimer = setInterval(tick, 120);
}

function screenCfg() {
  return { w: 960, h: 266 };
}

/* 网关对 auth 做严格 JSON-Schema 校验（实测矩阵）：
   - 任一字段缺失 / 类型不对（数字当字符串、字符串当数字、codecs 非数组）→
     {"errcode":400,"errmsg":"format invalid"}
   - 字段齐全且类型正确但没申请机位 → {"errcode":1213,"errmsg":"ticket required"}
   - user_id 空串 → 1301；token/game_code 空串 → 1302；token 非法 → 1006
   所以要「显式补齐 + 强制类型」，绝不能让 undefined 出现在这里。 */
function buildAuthData() {
  const userId = currentUserId();
  const token = currentToken();
  const gameCode = S.gameCode;
  const q = S.quality;
  const d = {
    // 必须是字符串：数字会被判 400 format invalid
    user_id: userId === undefined || userId === null ? '' : String(userId),
    token: token === undefined || token === null ? '' : String(token),
    game_code: gameCode === undefined || gameCode === null ? '' : String(gameCode),
    // 必须是数字：字符串会被判 400 format invalid
    w: 1280,
    h: 720,
    // 必须是 non-empty 字符串
    quality: q === undefined || q === null || q === '' ? 'high' : String(q),
    // 必须是数组：字符串会被判 400 format invalid
    codecs: ['h264'],
    // 必须是数字
    platform: 1
  };
  return d;
}

function sendAuth() {
  S.phase = 'authing';
  const d = buildAuthData();
  const data = { op: 'auth', id: String(Date.now()), data: d };
  const mod = globalThis.__cgNet;

  // 逐字段自检：任何一项误型都会让网关回 400 format invalid，且服务端不告诉你是哪个字段
  const selfCheck =
    'uid=' + (d.user_id ? 'ok#' + d.user_id.length : 'EMPTY') +
    ' tk=' + (d.token ? 'ok#' + d.token.length : 'EMPTY') +
    ' gc=' + (d.game_code || 'EMPTY') +
    ' q=' + d.quality +
    ' wh=' + typeof d.w + 'x' + typeof d.h +
    ' codecs=' + (Array.isArray(d.codecs) ? 'arr' + d.codecs.length : 'BAD') +
    ' plat=' + typeof d.platform;
  logInfo('[media] auth 字段自检 ' + selfCheck);

  const payload = JSON.stringify(data);
  const ok = mod && mod.wsSend(S.wsId, payload);
  const tk = d.token;
  const shown = JSON.stringify(
    Object.assign({}, data, {
      data: Object.assign({}, d, {
        token: tk ? tk.slice(0, 12) + '...' + tk.slice(-8) + '#' + tk.length : '(empty)'
      })
    })
  );
  logInfo('[media] 发送 auth (明文) ok=' + ok + ' bytes=' + payload.length);
  logInfo('[media] auth payload=' + shown);
  // 设备日志会截断长行，所以把真实 payload 切成 200 字符一段打出来，
  // 便于在主机上逐字节比对（网关卡在 400 format invalid 时不告诉你哪个字段错）。
  for (let ci = 0; ci * 200 < payload.length; ci++) {
    logWarn('[media] AUTHCHUNK ' + ci + '/' + Math.ceil(payload.length / 200) + ' [' + payload.slice(ci * 200, (ci + 1) * 200) + ']');
  }
  if (mod && typeof mod.wsLastFrame === 'function') {
    const lf = callJSON(mod, 'wsLastFrame', [S.wsId]);
    if (lf && lf.hex) logWarn('[media] AUTHFRAME len=' + lf.len + ' op=' + lf.op + ' hex=' + lf.hex);
  }
}

function send(op, data) {
  const mod = globalThis.__cgNet;
  if (!mod || !S.wsId) return false;
  const payload = encodeBody(S.offset, JSON.stringify({ op, id: String(Date.now()), data }));
  return !!mod.wsSend(S.wsId, payload);
}

function onWsMessage(raw) {
  let text = String(raw || '');
  let j = null;
  if (text && text[0] === '{') {
    try {
      j = JSON.parse(text);
    } catch (e) {
      j = null;
    }
  }
  if (!j) {
    try {
      j = JSON.parse(decodeBody(S.offset, text));
    } catch (e) {
      logWarn('[media] 无法解析 WS 消息: ' + text.slice(0, 80));
      return;
    }
  }
  const op = j.op || '';
  // 全量 op 追踪：网关对 auth 报 400 时不会说明原因，只能靠收到的 op 序列判断
  // 是哪一步被拒（offer 到达 = 协商已开始；只有 error 400 = auth 就该包被判非法）。
  logInfo('[media] <- op=' + (op || '?') + ' ' + text.slice(0, 160));
  if (op === 'offer') {
    S.phase = 'answering';
    const sdp = (j.data && (j.data.sdp || j.data)) || '';
    S.offerSdp = sdp;
    const p = parseSdp(sdp);
    const cand = p.candidates.find((c) => c.type === 'host') || p.candidates[0];
    S.ourUfrag = randToken(6);
    S.ourPwd = randToken(22);
    S.ourFp = randFingerprint();
    // 取证：把音频/视频 m 段的 rtpmap 打出来。
  // native 的 bus 报 `wrong format`，必须先确认网关实际协商的是不是 opus。
  try {
    p.mlines.forEach(function (m) {
      if (m.kind === 'audio' || m.kind === 'video') {
        console.error('[media] SDP ' + m.kind + ' pts=' + JSON.stringify(m.pts || []) +
          ' rtpmap=' + JSON.stringify(m.rtpmap || {}) +
          ' fmtp=' + JSON.stringify(m.fmtp || {}));
      }
    });
  } catch (e) {}

  S.remote = { ufrag: p.ufrag, pwd: p.pwd, cand, fingerprint: p.fingerprint || '', audioPt: p.audioPt, videoPt: p.videoPt, videoSsrc: p.videoSsrc, audioSsrc: p.audioSsrc, setup: (p.mlines[0] && p.mlines[0].setup) || 'actpass' };
    notify({ t: 'offer', ufrag: p.ufrag, cand });
    // 时序严格对齐官网 run.js：先起媒体（DTLS client 由我们主动发），
    // 再回 answer。answer 里必须带我们的 fingerprint 与 ufrag/pwd，
    // 否则网关不认这条 m-line。
    onOfferAsync();
  } else if (op === 'result') {
    notify({ t: 'result', data: j.data });
  } else if (op === 'input') {
    notify({ t: 'input', cmd: (j.data && j.data.cmd) || '' });
  } else if (op === 'info') {
    notify({ t: 'info', data: j.data });
  } else if (op === 'error') {
    S.lastError = (j.data && (j.data.errmsgcn || j.data.errmsg)) || '服务端错误';
    notify({ t: 'error', data: j.data, msg: S.lastError });
  } else if (op === 'game_stop') {
    const code = (j.data && j.data.code) || 0;
    S.phase = 'closed';
    notify({ t: 'gamestop', code });
  } else if (op === 'close') {
    S.phase = 'closed';
    notify({ t: 'wsclose', msg: 'server close' });
  } else {
    notify({ t: 'ws', state: op || 'unknown', msg: text.slice(0, 120) });
  }
}

export async function startPlay(cfg) {
  const o = cfg || {};
  S.gameCode = o.gameCode;
  S.quality = o.quality || 'low';
  S.offset = currentOffset();
  S.phase = 'connecting';
  S.lastError = '';
  // 必须清掉上一轮的 offer/媒体状态：否则本轮若没收到 offer，会拿旧 SDP 回 answer，
  // 并让 cgmedia 连到上一台的 candidate 上，最后表现为「DTLS 一直超时、永远不出画」。
  S.offerSdp = '';
  S.remote = null;
  S.ourUfrag = '';
  S.ourPwd = '';
  S.ourFp = '';
  /* 旧会话必须先**真正停掉**再归零，不能只是把 id 忘了：
   * 重复 start / 快速重进对局时，上一轮的 native 会话可能还活着（vbox 还挂在
   * KMS 上、还在收 RTP）。过去这里只写 S.mediaId = 0，那条会话就再也没人
   * 停它、也没人给它 js_poll 心跳 → 平面一直盖着 UI，只能等 180s 看门狗
   * 兜底拆（这期间界面是"卡住的"）。stopPlay() 的前几步全是同步的。 */
  const oldMediaId = S.mediaId;
  const oldWsId = S.wsId;
  if (oldMediaId || oldWsId) {
    logWarn('[media] startPlay 前清理上一轮会话 mediaId=' + oldMediaId + ' wsId=' + oldWsId);
    try {
      clearKfBurst();
      if (oldMediaId && globalThis.__cgMedia) globalThis.__cgMedia.stop(oldMediaId);
      if (oldWsId && globalThis.__cgNet) globalThis.__cgNet.wsClose(oldWsId);
    } catch (e) {
      logWarn('[media] 清理上一轮会话失败: ' + ((e && e.message) || e));
    }
  }
  S.mediaId = 0;
  S.wsId = 0;
  S.vizOn = false;
  S.wsState = '';      // 必须清：否则新连接同为 'open' 时检测不到状态变化
  if (S.pollTimer) { clearInterval(S.pollTimer); S.pollTimer = 0; }

  const net = await loadNative();
  globalThis.__cgNet = net;
  if (!net || typeof net.wsOpen !== 'function') throw new Error('native cgnative 缺少 wsOpen');

  const hdrs = o.headers || '';
  S.wsId = net.wsOpen(o.gatewayUrl, hdrs, 15000);
  if (!S.wsId) throw new Error('wsOpen 失败');
  logInfo('[media] wsOpen id=' + S.wsId + ' -> ' + o.gatewayUrl);
  pump();
  return { wsId: S.wsId };
}

// 用当前 offer 启动媒体（native）
export async function startMedia() {
  if (!S.offerSdp || !S.remote) throw new Error('还没有 offer');
  const mm = await loadMedia();
  globalThis.__cgMedia = mm;
  if (!mm) throw new Error('native cgmedia 不可用');
  const c = S.remote.cand;
  if (!c) throw new Error('offer 里没有 candidate');
  const cfgObj = {
    remoteIp: c.ip,
    remotePort: c.port,
    // 两个名字都写：docs/ARCH.md 用 ufrag/pwd，getcfg 产出 remoteUfrag/remotePwd，
    // native 侧两个都吃，这里双写保证任何一侧改名都不炸。
    ufrag: S.remote.ufrag,
    pwd: S.remote.pwd,
    remoteUfrag: S.remote.ufrag,
    remotePwd: S.remote.pwd,
    fingerprint: S.remote.fingerprint || '',
    ourUfrag: S.ourUfrag,
    ourPwd: S.ourPwd,
    videoPt: S.remote.videoPt,
    audioPt: S.remote.audioPt,
    videoSsrc: S.remote.videoSsrc,
    audioSsrc: S.remote.audioSsrc,
    rect: S.rect || { x: 0, y: 0, w: 960, h: 266 },
    codec: 'h264'
  };
  const r = callJSON(mm, 'start', [JSON.stringify(cfgObj)]);
  if (!r || !r.id) throw new Error('cgmedia start 失败: ' + JSON.stringify(r));
  S.mediaId = r.id;
  S.phase = 'playing';
  logInfo('[media] cgmedia id=' + S.mediaId);
  return r;
}

export function sendInput(cmd) {
  return send('input', { cmd });
}

export function setQuality(q) {
  return send('game_set_quality', { quality: q });
}

/* 主动向网关要一个关键帧（RTCP PLI，走 SRTCP 加密）。
 *
 * 为什么需要：真机实测换画质时 SPS 报的分辨率恒为 1280x720，所以
 * 「档位越高分辨率越高」不成立；除流畅档外的马赛克/纯绿只能是解码器
 * 状态与码流断点不匹配。native 侧已经在 SPS profile/level 变化时自动
 * 重发 PLI 突发，这里再补一发，覆盖「换档但 SPS 完全没变、只是码流断流」
 * 的情况（此时 native 察觉不到 changed）。 */
export function requestKeyFrame() {
  if (!S.mediaId || !globalThis.__cgMedia) return;
  try {
    callJSON(globalThis.__cgMedia, 'cmd', [S.mediaId, JSON.stringify({ op: 'requestKeyFrame' })]);
  } catch (e) {
    logWarn('[media] requestKeyFrame 失败: ' + ((e && e.message) || e));
  }
}

/* 连发 N 个 PLI（默认由 STALLGUARD 用：2 发、间隔 500ms）。
 *
 * 为什么要「连发」而不是发一次：网关对 PLI 的处理是「下次关键帧时顺带响应」，
 * 而它在某些时刻会把单个 PLI 吞掉（真机 2026-11-13：停推后单发 PLI 完全没反应，
 * 而会话其实还活着）。两发跨 500ms 能跨过它的处理窗口。
 *
 * 定时器必须由本模块持有并在 stopPlay() 里清掉：play.vue 的页面销毁路径
 * 只保证调 stopPlay()，不会替 media 层清定时器；残留的 setTimeout 会在
 * 几秒后对着已经关掉的 mediaId 发 cmd（native 侧拒绝，日志里多一条噪音）。 */
let kfBurst = [];

function clearKfBurst() {
  for (let i = 0; i < kfBurst.length; i++) {
    try { clearTimeout(kfBurst[i]); } catch (e) { /* ignore */ }
  }
  kfBurst.length = 0;
}

export function requestKeyFrameBurst(count, gapMs) {
  const n = Math.min(5, Math.max(1, (count | 0) || 1));
  const gap = Math.max(0, gapMs | 0);
  clearKfBurst();
  requestKeyFrame();
  if (n === 1 || gap <= 0) return;
  for (let i = 1; i < n; i++) {
    const h = setTimeout(function () {
      /* 先把自己从表里摘掉，否则长会话里这个数组只增不减。 */
      kfBurst = kfBurst.filter(function (x) { return x !== h; });
      requestKeyFrame();
    }, gap * i);
    kfBurst.push(h);
  }
}

export function setRect(rect) {
  S.rect = rect;
  if (S.mediaId && globalThis.__cgMedia) {
    callJSON(globalThis.__cgMedia, 'cmd', [S.mediaId, JSON.stringify({ op: 'setRect', rect })]);
  }
}

/* 系统输入法占用标志：供 app.js 的 onHide 判断「这次切走是弹键盘，不是真退后台」 */
export function setInputBusy(on) {
  S.imBusy = !!on;
}

export function inputBusy() {
  return !!S.imBusy;
}

/* 临时隐藏 / 恢复 KMS 视频平面。
 *
 * 用途：云游戏内远端弹出输入框（网关下发 op=input cmd="1"）时，我们要弹
 * Falcon 系统键盘；但 KMS 视频平面的 zpos=2，**盖在系统键盘的合成面之上**，
 * 不藏画面用户就看不见键盘。
 *
 * native 侧语义（cm_cmd.c 的 setVideoVisible）：
 *   隐藏 = 把 render-rectangle 缩到 1x1（唯一不会被 kmssink 回退成铺满的几何）
 *   恢复 = 销毁并重建 vbox（set_rect 是单向棘轮，放大回去不生效），
 *          重建后复位 video_ok/media_ev_sent 并补发 PLI 请求 IDR。
 */
export function setVideoVisible(on) {
  if (!S.mediaId || !globalThis.__cgMedia) return;
  try {
    callJSON(globalThis.__cgMedia, 'cmd', [
      S.mediaId,
      JSON.stringify({ op: 'setVideoVisible', on: on ? 1 : 0 })
    ]);
  } catch (e) {
    logWarn('[media] setVideoVisible 失败: ' + ((e && e.message) || e));
  }
}

/* 当前正在播放的 game_code。app 级 onHide 要用它释放机位 —— 页面级回调
   在真机上根本不触发，App 里拿不到 play 页的 this.code，只能从这里问。 */
export function currentGameCode() {
  return S.gameCode || '';
}

export async function stopPlay() {
  /* 先清 PLI 连发定时器：它们会在几百毫秒后对 S.mediaId 发 cmd，
     而下面几行立刻就把 mediaId 归零、native 会话销毁，留着只会在日志里
     多几条 "cmd 失败" 噪音，还会和重连抢同一个 id。 */
  clearKfBurst();
  if (S.pollTimer) {
    clearInterval(S.pollTimer);
    S.pollTimer = 0;
  }
  if (S.mediaId && globalThis.__cgMedia) {
    try {
      globalThis.__cgMedia.stop(S.mediaId);
    } catch (e) {
      /* ignore */
    }
    S.mediaId = 0;
  }
  if (S.wsId && globalThis.__cgNet) {
    try {
      globalThis.__cgNet.wsClose(S.wsId);
    } catch (e) {
      /* ignore */
    }
    S.wsId = 0;
  }
  S.phase = 'closed';
}
