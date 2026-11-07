<template>
  <div class="page">
    <div class="proof">
      <text class="pt">webp 原图(应空白)</text>
      <image class="pimg" :src="proofWebp" :width="52" :height="52"></image>
      <text class="pt">imgConv → file://</text>
      <image class="pimg" :src="proofConv" :width="52" :height="52"></image>
      <text class="pt">png 对照(应出图)</text>
      <image class="pimg" :src="proofPng" :width="52" :height="52"></image>
    </div>
    <div class="content">
      <div class="hdr">
        <text class="hdr-t">cgnative diag</text>
        <text class="hdr-s">{{ summary }}</text>
      </div>
      <div class="cols">
        <div class="col scroll-y">
          <text class="ln" v-for="(l, i) in left" :key="'l' + i">{{ l }}</text>
        </div>
        <div class="col scroll-y">
          <text class="ln" v-for="(l, i) in right" :key="'r' + i">{{ l }}</text>
        </div>
      </div>
    </div>
  </div>
</template>
<script>
import { currentUserId, currentToken, currentOffset } from '../../services/cg.js';
import { restoreSession } from '../../services/auth.js';
import { loadSettings } from '../../services/settings.js';

const MD5_ABC = '900150983cd24fb0d6963f7d28e17f72';
const SHA_ABC = 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad';
const API = 'https://n.cg.163.com';
const HDR = 'X-Platform: 1\r\nX-Ver: 6.2.2\r\n';
const WEBP_URL = 'http://g.fp.ps.netease.com/cg-image/file/6ad88e5100c3e3cf0c4731d7c3pE81gm07';
const PNG_URL = 'http://g.fp.ps.netease.com/cg-image/file/691c1d2e3e7587ca8a605ecc5CxH8MW607';

export default {
  data() {
    return {
      left: [], right: [], summary: 'running...', passN: 0, failN: 0,
      proofWebp: WEBP_URL, proofConv: '', proofPng: PNG_URL
    };
  },
  mounted() {
    this.probeTopApp();
    this.run();
  },
  methods: {
    // 探针：$falcon 有没有「当前前台是哪个 app」的接口。
    // 用途：KMS 平面是硬件 overlay，退到后台后必须自己拆管线；
    // 但如果 JS 定时器还在跑（真机实测确实在跑），就没法靠心跳超时判断，
    // 只能主动问「现在前台是谁」。
    probeTopApp() {
      var out = [];
      try {
        out.push('typeof getTopApp=' + typeof (typeof $falcon !== 'undefined' && $falcon.getTopApp));
      } catch (e) { out.push('getTopApp throw:' + e.message); }
      try {
        if (typeof $falcon !== 'undefined' && typeof $falcon.getTopApp === 'function') {
          out.push('getTopApp()=' + JSON.stringify($falcon.getTopApp()));
        }
      } catch (e) { out.push('call throw:' + (e && e.message)); }
      try { out.push('\$appid=' + String(globalThis.$appid)); } catch (e) {}
      try { out.push('\$getTopApp=' + (typeof globalThis.$getTopApp)); } catch (e) {}
      for (var i = 0; i < out.length; i++) console.warn('[pencg][diag] TOPAPP ' + out[i]);
    },
    // 记账式自检：只有 PASS/FAIL 计入汇总
    say(ok, name, detail, side) {
      const tag = ok ? 'PASS' : 'FAIL';
      this.push(tag + ' ' + name + (detail ? ' | ' + detail : ''), side);
      if (ok) this.passN++;
      else this.failN++;
      try {
        console.warn('[pencg][diag] ' + tag + ' ' + name + ' ' + (detail || ''));
      } catch (e) {}
    },
    // 仅显示、不计分（别的模块的自检用这个，避免污染本模块汇总）
    info(ok, name, detail, side) {
      this.push((ok ? 'PASS' : 'FAIL') + ' ' + name + (detail ? ' | ' + detail : ''), side);
      try {
        console.warn('[pencg][diag] INFO ' + (ok ? 'PASS ' : 'FAIL ') + name + ' ' + (detail || ''));
      } catch (e) {}
    },
    push(line, side) {
      if (side === 'r') this.right = this.right.concat([line]);
      else this.left = this.left.concat([line]);
    },
    sleep(ms) {
      return new Promise((r) => setTimeout(r, ms));
    },
    // 轮询到终态（open/error/closed）
    async waitWs(m, id, tries) {
      let s = {};
      for (let i = 0; i < (tries || 40); i++) {
        try {
          s = JSON.parse(m.wsState(id));
        } catch (e) {
          return { state: 'badjson' };
        }
        if (s.state === 'open' || s.state === 'error' || s.state === 'closed') return s;
        await this.sleep(250);
      }
      return s || {};
    },
    async run() {
      let m;
      try {
        m = await import('cgnative');
      } catch (e) {
        this.say(false, 'import', String(e && e.message));
        this.summary = 'IMPORT FAILED';
        return;
      }
      const keys = Object.keys(m || {}).filter((k) => k !== 'default');
      this.say(keys.length >= 22, 'import', 'n=' + keys.length + ' ' + keys.join(' '));

      // ---------------- 左栏：加密 + HTTP ----------------
      try {
        const v = m.md5Hex('abc');
        this.say(v === MD5_ABC, 'md5Hex(abc)', v);
      } catch (e) {
        this.say(false, 'md5Hex', String(e && e.message));
      }
      try {
        const v = m.sha256Hex('abc');
        this.say(v === SHA_ABC, 'sha256Hex(abc)', v.slice(0, 12) + '..' + v.slice(60));
      } catch (e) {
        this.say(false, 'sha256Hex', String(e && e.message));
      }
      try {
        const e1 = m.b64enc('hi');
        const d1 = m.b64dec('aGk=');
        this.say(e1 === 'aGk=' && d1 === 'hi', 'b64', e1 + '/' + d1);
      } catch (e) {
        this.say(false, 'b64', String(e && e.message));
      }
      try {
        const v = m.randomHex(8);
        this.say(v.length === 16, 'randomHex(8)', v);
      } catch (e) {
        this.say(false, 'randomHex', String(e && e.message));
      }
      try {
        const v = m.version();
        this.say(!!v, 'version', v);
      } catch (e) {
        this.say(false, 'version', String(e && e.message));
      }
      try {
        const out = m.tripleDesB64('hello', '123456789012345678901234', '0011223344556677');
        this.say(!!out && out.length > 0, 'tripleDesB64', out);
      } catch (e) {
        this.say(false, 'tripleDesB64', String(e && e.message));
      }
      // 裸 body：平台 jsapi.http.request 会把 data 当 JSON 二次序列化，这里必须原样发
      try {
        const raw = m.requestSync('GET', API + '/api/v1/versions/@current?package_name=H5', HDR, '', 6000);
        const r = JSON.parse(raw);
        let jsonOk = false;
        try {
          JSON.parse(r.body);
          jsonOk = true;
        } catch (e2) {}
        this.say(
          r.status === 200 && jsonOk,
          'requestSync GET',
          'st=' + r.status + ' len=' + (r.body || '').length + ' ms=' + r.ms + ' json=' + jsonOk
        );
      } catch (e) {
        this.say(false, 'requestSync GET', String(e && e.message));
      }
      try {
        const raw = m.postSync(
          API + '/api/v1/network-tests',
          'X-Platform: 1\r\nContent-Type: text/plain\r\n',
          'not-a-valid-json-body',
          5000
        );
        const r = JSON.parse(raw);
        this.say(
          typeof r.status === 'number' && !!r.error,
          'postSync err',
          'st=' + r.status + ' c=' + r.curlCode + ' err=' + String(r.error).slice(0, 28)
        );
      } catch (e) {
        this.say(false, 'postSync err', String(e && e.message));
      }

      // ---------------- 右栏 0：auth 载荷定案（不依赖网关字段校验） ----------------
      // 背景：真机 media.js sendAuth() 的明文 auth 被网关回 400 format invalid。
      // 主机矩阵实验（Lead 已做完）证明网关对 auth.data 做严格类型校验：
      //   quality 缺失/数字、codecs 非数组、user_id 数字、w/h 字符串或缺失、
      //   id 数字、data 是字符串 —— 一律 400 format invalid；
      //   7 字段都合规时只差 ticket，回 1213 ticket required。
      // 所以这里用 app 自己的服务层拼出与 media.js 一字不差的 auth，逐字段打印类型，
      // 再用 native 侧 utf8Hex 量真实字节数（不是 JS 的 string.length）。
      let authPayload = '';
      let authBytes = 0;
      try {
        let rs;
        try {
          rs = await restoreSession();
        } catch (e) {
          rs = 'throw:' + (e && e.message);
        }
        const st = await loadSettings();
        const uid = currentUserId();
        const tk = currentToken();
        const off = currentOffset();
        // 与 media.js sendAuth() 完全一致的构造
        const data = {
          op: 'auth',
          id: String(Date.now()),
          data: {
            user_id: uid,
            token: tk,
            game_code: 'yssy',
            w: 1280,
            h: 720,
            quality: st.quality || 'low',
            codecs: ['h264'],
            platform: 1
          }
        };
        authPayload = JSON.stringify(data);
        authBytes = m.utf8Hex(authPayload).length / 2;
        this.info(rs === true, 'auth restore', 'ret=' + String(rs) + ' off=' + off, 'r');
        this.say(
          typeof uid === 'string' && uid.length > 0,
          'auth uid',
          'type=' + typeof uid + ' len=' + String(uid).length,
          'r'
        );
        this.say(
          typeof tk === 'string' && tk.length >= 20,
          'auth token',
          'type=' + typeof tk + ' len=' + String(tk).length,
          'r'
        );
        this.say(
          typeof st.quality === 'string' && st.quality.length > 0,
          'auth quality',
          'q=' + String(st.quality) + ' type=' + typeof st.quality,
          'r'
        );
        this.say(authBytes > 200 && authBytes < 2000, 'auth payload', 'bytes=' + authBytes, 'r');
        this.info(true, 'auth head', authPayload.slice(0, 96), 'r');
      } catch (e) {
        this.say(false, 'auth payload', String(e && e.message).slice(0, 40), 'r');
      }

      // 字节对账：字段全对的话，唯一剩下的可能就是把这段 UTF-8 送出去的过程。
      // 发到一台已释放但在线的网关（零云游戏时长），只看 sent 是否等于 authBytes。
      let authWsId = 0;
      if (authBytes > 0) {
        try {
          authWsId = m.wsOpen(
            'wss://gw-dx-hnfs-13.cg.163.com:6443/ws',
            '',
            8000,
            '{"tlsVerify":0,"connectTimeoutMs":6000}'
          );
          const s = await this.waitWs(m, authWsId, 40);
          this.info(
            s.state === 'open',
            'auth ws open',
            'st=' + s.state + ' sc=' + s.stageCode + ' ' + String(s.msg || '').slice(0, 24),
            'r'
          );
          if (s.state === 'open') {
            const ok = m.wsSend(authWsId, authPayload);
            await this.sleep(400);
            // 帧级自证：真实字节 81 fe <len16> <mask4> <payload^mask>
            let fr = {};
            try {
              fr = JSON.parse(m.wsLastFrame(authWsId));
            } catch (e) {
              fr = {};
            }
            const hex = String(fr.hex || '');
            const hdrLen = hex.length >= 8 ? parseInt(hex.slice(4, 8), 16) : -1;
            this.say(
              ok === 1 && hex.slice(0, 4) === '81fe' && hdrLen === authBytes && fr.len === authBytes + 8,
              'auth frame',
              'ret=' + ok + ' ' + hex.slice(0, 16) + ' len=' + fr.len + '/' + hdrLen,
              'r'
            );
            const st2 = JSON.parse(m.wsState(authWsId));
            this.say(
              st2.sent === authBytes + 8,
              'auth sent',
              'sent=' + st2.sent + ' want=' + authBytes + '+8',
              'r'
            );
            await this.sleep(1500);
            let raw = '';
            try {
              const arr = JSON.parse(m.wsPoll(authWsId));
              raw = arr.length ? String(arr[0]) : '';
            } catch (e) {
              raw = 'poll-throw:' + (e && e.message);
            }
            const fmtBad = raw.indexOf('format invalid') >= 0;
            this.say(
              raw.length > 0 && !fmtBad,
              'auth reply',
              raw ? raw.replace(/\s+/g, ' ').slice(0, 76) : '(empty)',
              'r'
            );
            m.wsClose(authWsId);
          }
        } catch (e) {
          this.info(false, 'auth ws', String(e && e.message).slice(0, 40), 'r');
        }
      }

      // 负向对照：同一条 wss 链路发一份「故意缺 game_code」的 payload。
      // 若网关回 400 format invalid，就同时证明三件事：
      //   a) 网关确实在按 JSON-Schema 逐字段校验；
      //   b) 我们的帧能把「544 字节 vs 531 字节」两种载荷如实且区分地送达；
      //   c) 生产上那次 400 的成因就是某个字段缺失/误型，而不是帧层问题。
      if (authBytes > 0) {
        try {
          const bad = authPayload.replace('"game_code":"yssy",', '');
          const badBytes = m.utf8Hex(bad).length / 2;
          const id2 = m.wsOpen(
            'wss://gw-dx-hnfs-13.cg.163.com:6443/ws',
            '',
            8000,
            '{"tlsVerify":0,"connectTimeoutMs":6000}'
          );
          const s2 = await this.waitWs(m, id2, 40);
          if (s2.state === 'open') {
            m.wsSend(id2, bad);
            await this.sleep(1500);
            let raw2 = '';
            try {
              const arr2 = JSON.parse(m.wsPoll(id2));
              raw2 = arr2.length ? String(arr2[0]) : '';
            } catch (e) {
              raw2 = 'poll-throw:' + (e && e.message);
            }
            this.say(
              raw2.indexOf('format invalid') >= 0 && badBytes < authBytes,
              'auth neg-ctl',
              'bytes=' + badBytes + ' ' + (raw2 ? raw2.replace(/\s+/g, ' ').slice(0, 52) : '(empty)'),
              'r'
            );
            m.wsClose(id2);
          } else {
            this.info(false, 'auth neg-ctl', 'st=' + s2.state, 'r');
          }
        } catch (e) {
          this.info(false, 'auth neg-ctl', String(e && e.message).slice(0, 40), 'r');
        }
      }

      // 第二对照组：game_code 为空串。media.js 注释说「空串 -> 1302」。
      // 这条决定「S.gameCode 在真机上是空」到底会不会引发 400 —— 如果回 1302 就排除。
      if (authBytes > 0) {
        try {
          const empty = authPayload.replace('"game_code":"yssy"', '"game_code":""');
          const id3 = m.wsOpen(
            'wss://gw-dx-hnfs-13.cg.163.com:6443/ws',
            '',
            8000,
            '{"tlsVerify":0,"connectTimeoutMs":6000}'
          );
          const s3 = await this.waitWs(m, id3, 40);
          if (s3.state === 'open') {
            m.wsSend(id3, empty);
            await this.sleep(1500);
            let raw3 = '';
            try {
              const arr3 = JSON.parse(m.wsPoll(id3));
              raw3 = arr3.length ? String(arr3[0]) : '';
            } catch (e) {
              raw3 = 'poll-throw:' + (e && e.message);
            }
            this.say(
              raw3.length > 0 && raw3.indexOf('format invalid') < 0,
              'auth gc-empty',
              raw3 ? raw3.replace(/\s+/g, ' ').slice(0, 64) : '(empty)',
              'r'
            );
            m.wsClose(id3);
          } else {
            this.info(false, 'auth gc-empty', 'st=' + s3.state, 'r');
          }
        } catch (e) {
          this.info(false, 'auth gc-empty', String(e && e.message).slice(0, 40), 'r');
        }
      }

      // ---------------- 右栏：WebSocket ----------------
      // 主连接：显式传 optsJson（tlsVerify:0，设备无 CA），验证第 4 参数被接受
      let wsId = 0;
      try {
        wsId = m.wsOpen(
          'wss://n.cg.163.com/push/ws',
          'X-Platform: 1\r\nX-Ver: 6.2.2\r\n',
          8000,
          '{"tlsVerify":0,"connectTimeoutMs":6000,"frameMaxBytes":262144}'
        );
        this.say(wsId > 0, 'wsOpen', 'id=' + wsId, 'r');
      } catch (e) {
        this.say(false, 'wsOpen', String(e && e.message), 'r');
      }

      if (wsId > 0) {
        const s = await this.waitWs(m, wsId, 40);
        this.say(
          s.state === 'open' && s.phase === 'open' && s.stageCode === 6 && s.httpStatus === 101,
          'wsState open',
          'st=' + s.state + ' ph=' + s.phase + ' sc=' + s.stageCode + ' hs=' + s.httpStatus,
          'r'
        );
        if (s.state === 'open') {
          try {
            const ok = m.wsSend(wsId, '{"op":"auth","data":{}}');
            this.say(ok === 1, 'wsSend', 'ret=' + ok, 'r');
          } catch (e) {
            this.say(false, 'wsSend', String(e && e.message), 'r');
          }
          await this.sleep(1200);
          try {
            const ex = JSON.parse(m.wsPollEx(wsId));
            const arr = JSON.parse(m.wsPoll(wsId));
            const first = arr.length > 0 ? String(arr[0]).replace(/\s+/g, ' ').slice(0, 30) : '';
            this.say(
              Array.isArray(ex.msgs) && typeof ex.total === 'number' && Array.isArray(arr),
              'wsPollEx',
              'n=' + ex.msgs.length + ' total=' + ex.total + ' ' + first,
              'r'
            );
          } catch (e) {
            this.say(false, 'wsPollEx', String(e && e.message), 'r');
          }
        }
        try {
          m.wsClose(wsId);
          await this.sleep(500);
          const s2 = JSON.parse(m.wsState(wsId));
          this.say(
            s2.state === 'closed' && s2.stageCode === 8 && s2.phase === 'closed',
            'wsClose',
            'st=' + s2.state + ' ph=' + s2.phase + ' sc=' + s2.stageCode,
            'r'
          );
        } catch (e) {
          this.say(false, 'wsClose', String(e && e.message), 'r');
        }
        try {
          const ids = JSON.parse(m.wsList());
          this.say(Array.isArray(ids) && ids.indexOf(wsId) >= 0, 'wsList', JSON.stringify(ids), 'r');
        } catch (e) {
          this.say(false, 'wsList', String(e && e.message), 'r');
        }
      }

      // 握手失败分类（播放页要靠这些码显示不同文案）
      // 1) DNS 解析失败 -> phase=dns stageCode=1
      try {
        const id = m.wsOpen('wss://no-such-host-xyz.invalid/ws', '', 4000, '{"tlsVerify":0,"connectTimeoutMs":3000}');
        const s = await this.waitWs(m, id, 20);
        this.say(
          s.state === 'error' && s.stageCode === 1 && s.phase === 'dns',
          'ws dns-fail',
          'sc=' + s.stageCode + ' errno=' + s.errno + ' ' + String(s.errstr || '').slice(0, 18),
          'r'
        );
      } catch (e) {
        this.say(false, 'ws dns-fail', String(e && e.message), 'r');
      }
      // 2) TCP 被拒绝（本机回环闭端口，秒拒）-> stageCode=2，errno 应为 111
      try {
        const id = m.wsOpen('wss://127.0.0.1:1/ws', '', 4000, '{"tlsVerify":0,"connectTimeoutMs":2500}');
        const s = await this.waitWs(m, id, 24);
        this.say(
          s.state === 'error' && s.stageCode === 2 && s.phase === 'tcp' && s.errno === 111,
          'ws tcp-refused',
          'sc=' + s.stageCode + ' errno=' + s.errno + ' ' + String(s.errstr || '').slice(0, 20),
          'r'
        );
      } catch (e) {
        this.say(false, 'ws tcp-refused', String(e && e.message), 'r');
      }
      // 2b) TCP 连接超时（不可路由地址）-> stageCode=12，和"被拒绝"分开
      try {
        const id = m.wsOpen('wss://10.255.255.1:9/ws', '', 4000, '{"tlsVerify":0,"connectTimeoutMs":1500}');
        const s = await this.waitWs(m, id, 24);
        this.say(
          s.state === 'error' && s.stageCode === 12 && s.phase === 'tcp',
          'ws tcp-timeout',
          'sc=' + s.stageCode + ' errno=' + s.errno + ' ' + String(s.errstr || '').slice(0, 18),
          'r'
        );
      } catch (e) {
        this.say(false, 'ws tcp-timeout', String(e && e.message), 'r');
      }
      // 3) HTTP 非 101（普通 https 路径）-> stageCode=4 + httpStatus 非 0
      try {
        const id = m.wsOpen(
          'wss://n.cg.163.com/api/v1/versions/@current?package_name=H5',
          '',
          6000,
          '{"tlsVerify":0,"connectTimeoutMs":5000}'
        );
        const s = await this.waitWs(m, id, 24);
        this.say(
          s.state === 'error' && s.stageCode === 4 && s.httpStatus > 0,
          'ws not101',
          'sc=' + s.stageCode + ' hs=' + s.httpStatus,
          'r'
        );
      } catch (e) {
        this.say(false, 'ws not101', String(e && e.message), 'r');
      }
      // 4) tlsVerify:1 在设备无 CA 下的真实表现 —— 只报事实，不计分
      try {
        const id = m.wsOpen('wss://n.cg.163.com/push/ws', '', 6000, '{"tlsVerify":1,"connectTimeoutMs":5000}');
        const s = await this.waitWs(m, id, 24);
        this.info(
          s.state === 'open',
          'ws tlsVerify=1',
          'st=' + s.state + ' ph=' + s.phase + ' sc=' + s.stageCode + ' ' + String(s.msg || '').slice(0, 20),
          'r'
        );
        if (s.state === 'open') m.wsClose(id);
      } catch (e) {
        this.info(false, 'ws tlsVerify=1', String(e && e.message), 'r');
      }

      // ---------------- 图片：webp 侦测 + 落地转换 ----------------
      // 设备全无 webp 解码器，图床拒绝转格式（25 种变体实测全返回 webp），
      // 只能 native 侧下下来用 ffmpeg 转 png，再用 file:// 给 <image>。
      try {
        const t0 = Date.now();
        const raw = m.imgScan([WEBP_URL, PNG_URL]);
        const map = JSON.parse(raw);
        const ms = Date.now() - t0;
        this.say(
          map[WEBP_URL] === 'webp' && map[PNG_URL] === 'png',
          'imgScan',
          'webp=' + map[WEBP_URL] + ' png=' + map[PNG_URL] + ' ' + ms + 'ms'
        );
      } catch (e) {
        this.say(false, 'imgScan', String(e && e.message));
      }
      try {
        // 压测：18 个 URL（38 图标场景的一半，含重复）走 12 线程池，验证并发不崩不卡
        const many = [];
        for (let k = 0; k < 9; k++) { many.push(WEBP_URL); many.push(PNG_URL); }
        const t0 = Date.now();
        const mm = JSON.parse(m.imgScan(many));
        const ms = Date.now() - t0;
        let okN = 0;
        for (const u in mm) if (mm[u] === (u === WEBP_URL ? 'webp' : 'png')) okN++;
        this.say(okN === 2 && ms < 4000, 'imgScan x18', okN + ' kinds ' + ms + 'ms');
      } catch (e) {
        this.say(false, 'imgScan x18', String(e && e.message));
      }
      try {
        const t0 = Date.now();
        const f1 = m.imgConv(WEBP_URL);
        const ms1 = Date.now() - t0;
        const t1 = Date.now();
        const f2 = m.imgConv(WEBP_URL); // 第二次必须走缓存，明显更快
        const ms2 = Date.now() - t1;
        this.proofConv = f1;
        this.say(
          f1.indexOf('file://') === 0 && f1.indexOf('.png') > 0 && f2 === f1,
          'imgConv webp',
          ms1 + 'ms→' + ms2 + 'ms ' + f1.slice(20)
        );
      } catch (e) {
        this.say(false, 'imgConv webp', String(e && e.message));
      }
      try {
        const f = m.imgConv(PNG_URL);
        this.proofPng = f || PNG_URL;
        this.say(f.indexOf('file://') === 0, 'imgConv png', f.slice(20));
      } catch (e) {
        this.say(false, 'imgConv png', String(e && e.message));
      }

      // ---------------- cgmedia（native-media agent 的自检段，不计入本模块汇总） ----------------
      try {
        const gm = await import('cgmedia');
        const gk = Object.keys(gm || {}).filter((k) => k !== 'default');
        const ver = gm.version ? gm.version() : '';
        this.info(gk.length >= 5, 'cgmedia', 'n=' + gk.length + ' ' + String(ver).slice(0, 20), 'r');
        const GMC = '{"remoteIp":"112.90.156.147","remotePort":49364,"remoteUfrag":"ogGZ","remotePwd":"GRJvpfGvijTJvVomfUX37i","ourUfrag":"a86dff46","ourPwd":"zi3bk3htti","fingerprint":"sha-256 DA:4D:1C:EE:12:3F:44:8F:AE:28:24:B1:A4:71:DC:BC:68:34:A4:5A:E0:3B:DC:61:7D:56:00:FF:77:FA:A5:43","videoPt":98,"audioPt":111,"videoSsrc":3398264230,"audioSsrc":489236220,"rect":{"x":0,"y":0,"w":960,"h":266},"codec":"h264","logLevel":4}';
        const st = JSON.parse(gm.start(GMC));
        this.info(st.id > 0, 'cgmedia start', JSON.stringify(st).slice(0, 30), 'r');
        if (st.id > 0) {
          const seen = {};
          let gotMedia = false;
          for (let i = 0; i < 60; i++) {
            await this.sleep(500);
            let evs = [];
            try {
              evs = JSON.parse(gm.poll(st.id));
            } catch (e) {
              evs = [];
            }
            if (!Array.isArray(evs)) evs = [];
            for (let k = 0; k < evs.length; k++) {
              const e = evs[k] || {};
              const s2 = JSON.stringify(e);
              if (e.t === 'stat') continue;
              if (seen[s2]) continue;
              seen[s2] = 1;
              this.info(true, 'cgmedia ev', s2.slice(0, 92), 'r');
              if (e.t === 'media' && e.video === true) gotMedia = true;
            }
            if (gotMedia && i > 10) break;
          }
          try {
            gm.cmd(st.id, JSON.stringify({ op: 'stats' }));
            const ev2 = JSON.parse(gm.poll(st.id));
            this.info(Array.isArray(ev2) && ev2.length > 0, 'cgmedia stats', JSON.stringify(ev2).slice(0, 92), 'r');
          } catch (e) {
            this.info(false, 'cgmedia stats', String(e && e.message).slice(0, 40), 'r');
          }
          this.info(gotMedia, 'cgmedia media', 'video=' + gotMedia, 'r');
          const sp = JSON.parse(gm.stop(st.id));
          this.info(sp.ok === true, 'cgmedia stop', JSON.stringify(sp).slice(0, 24), 'r');
        }
      } catch (e) {
        this.info(false, 'cgmedia', String(e && e.message).slice(0, 40), 'r');
      }

      this.summary =
        this.failN === 0 ? 'ALL PASS ' + this.passN : 'FAIL x' + this.failN + ' / pass ' + this.passN;
      try {
        console.warn('[pencg][diag] ' + this.summary);
      } catch (e) {}
    }
  }
};
</script>
<style lang="less" scoped>
@import "base.less";

.page {
  flex-direction: row;
  background-color: #101218;
}

/* 固定举证区：左栏三张图，肉眼判断 file:// 到底能不能渲染 */
.proof {
  flex-direction: column;
  width: 150px;
  height: 266px;
  padding-left: 6px;
  padding-top: 4px;
  background-color: #171b26;
}

.pt {
  font-size: 9px;
  line-height: 11px;
  color: #8e96ab;
}

.pimg {
  width: 52px;
  height: 52px;
  margin-bottom: 2px;
}

.content {
  flex-direction: column;
}

.hdr {
  flex-direction: row;
  align-items: center;
  height: 24px;
  padding-left: 8px;
  background-color: #1c2030;
}

.hdr-t {
  font-size: 13px;
  color: #f2f4f8;
  margin-right: 12px;
}

.hdr-s {
  font-size: 13px;
  color: #23b3e8;
}

.cols {
  flex-direction: row;
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0;
  padding-left: 8px;
  padding-top: 2px;
}

.col {
  flex-direction: column;
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0;
  margin-right: 8px;
}

.ln {
  font-size: 10px;
  line-height: 12px;
  color: #c8cede;
}
</style>
