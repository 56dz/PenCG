<template>
  <!-- 播放态三栏：左 243 + 中 473（KMS 视频挖孔）+ 右 244 = 960，高度统一 266。
       KMS 平面按绝对坐标挖孔，所以 .stage 必须紧接在 .p2-left 后面且宽度分毫不差；
       左右两栏都是不透明底色，.stage 自身保持透明把画面透出来。 -->
  <div class="page page-live">
    <!-- 左栏 243x266：品牌 + 游戏名（两行）· 状态调试条 · 地区/画质 · 免费时长 · 底部出画统计 -->
    <div class="p2-left">
      <div class="p2-lhead">
        <image class="p2-logo" :src="logo" :width="26" :height="26"></image>
        <div class="p2-namebox">
          <text class="p2-name">{{ nameL1 }}</text>
          <text v-if="nameL2" class="p2-name">{{ nameL2 }}</text>
        </div>
      </div>
      <div class="p2-lstrip"><text class="p2-lstrip-text">{{ stripClip }}</text></div>
      <div class="p2-lrow">
        <div class="chip"><text class="chip-text">{{ regionText }}</text></div>
        <div class="chip p2-chipgap"><text class="chip-text">{{ qualityLabel }}</text></div>
      </div>
      <!-- ★ 这里原来显示"本局剩余 N 分钟"是错的：tickets 的 time_left 是 ticket
           有效期（约 25 秒），不是单局时长限制，服务端根本不下发单局限制。
           改成显示"已玩时长"，既真实又对玩家有用。 -->
      <div class="p2-ltime">
        <text class="p2-ltime-label">已玩时长</text>
        <text class="p2-ltime-value">{{ playedText }}</text>
      </div>
      <div class="p2-lmsg">
        <text :class="errText ? 'p2-lmsg-main-err' : 'p2-lmsg-main'">{{ statusMainClip }}</text>
        <text v-if="statusSub1" class="p2-lmsg-sub">{{ statusSub1 }}</text>
        <text v-if="statusSub2" class="p2-lmsg-sub">{{ statusSub2 }}</text>
      </div>
      <div class="p2-lstat"><text class="p2-lstat-text">{{ dbgText }}</text></div>
    </div>

    <!-- 中栏 473x266：KMS 平面挖孔位。自身必须透明；未出画时 .mask 不透明盖满整块。 -->
    <div class="stage" @touchstart="onTS" @touchmove="onTM" @touchend="onTE" @touchcancel="onTC">
      <div v-if="!vizOn" class="mask">
        <div class="p2-mask-box">
          <text class="p2-mask-title">{{ maskTitle }}</text>
          <text v-if="dots" class="p2-mask-dots">{{ dots }}</text>
          <text class="p2-mask-sub">{{ maskSub }}</text>
          <text v-if="errText" class="p2-mask-err">{{ errText }}</text>
        </div>
      </div>
    </div>

    <!-- 右栏 244x266：画质四档（两行）· 码率统计 · 重连/重新申请/退出 · 底部说明 -->
    <div class="p2-right">
      <!-- ★ 数字键盘（云游戏内输入手机号/验证码）。
           放在右栏而不是整屏，是因为 KMS 视频平面的挖孔矩形固定为中间 473 宽
           （VIDEO.x=243, VIDEO.w=473），右栏 244 完全在平面之外 —— 键盘自己
           就是一块普通 UI，不需要把视频平面藏起来，也就绕开了 1x1 棘轮、
           READY 重置、等 IDR 这一整串麻烦，玩家还能边看画面边按。
           远端仍是官方那套：每位 sendText(d) 走 cmd 5，退格 sendKey(1) 走
           cmd 6，确认 sendKey(2) 走 cmd 6（官方 run.js onKeyup 的 8→1、13→2）。 -->
      <div v-if="kbShow" class="p2-kb">
        <div class="p2-kbhead">
          <text class="p2-kbtitle">{{ kbTitle }}</text>
          <div class="p2-kbsys" @click="kbSystem"><text class="p2-kbsys-text">系统键盘</text></div>
        </div>
        <div class="p2-kbdisp">
          <text class="p2-kbval">{{ kbNums || kbTip }}</text>
        </div>
        <div class="p2-kbrow">
          <div class="p2-kbkey" @click="kbDigit('1')"><text class="p2-kbnum">1</text></div>
          <div class="p2-kbkey" @click="kbDigit('2')"><text class="p2-kbnum">2</text></div>
          <div class="p2-kbkey" @click="kbDigit('3')"><text class="p2-kbnum">3</text></div>
        </div>
        <div class="p2-kbrow">
          <div class="p2-kbkey" @click="kbDigit('4')"><text class="p2-kbnum">4</text></div>
          <div class="p2-kbkey" @click="kbDigit('5')"><text class="p2-kbnum">5</text></div>
          <div class="p2-kbkey" @click="kbDigit('6')"><text class="p2-kbnum">6</text></div>
        </div>
        <div class="p2-kbrow">
          <div class="p2-kbkey" @click="kbDigit('7')"><text class="p2-kbnum">7</text></div>
          <div class="p2-kbkey" @click="kbDigit('8')"><text class="p2-kbnum">8</text></div>
          <div class="p2-kbkey" @click="kbDigit('9')"><text class="p2-kbnum">9</text></div>
        </div>
        <div class="p2-kbrow">
          <div class="p2-kbkey p2-kbdel" @click="kbDel"><text class="p2-kbnum">⌫</text></div>
          <div class="p2-kbkey" @click="kbDigit('0')"><text class="p2-kbnum">0</text></div>
          <div class="p2-kbkey p2-kbok" @click="kbOk"><text class="p2-kbok-text">确定</text></div>
        </div>
      </div>

      <!-- 退出确认：KMS 盖住中间视频区，弹层只能落在右栏 244 宽里 -->
      <div v-else-if="confirmExit" class="p2-confirm">
        <text class="p2-ctitle">确认退出游戏？</text>
        <text class="p2-csub">退出后立即释放云端机位</text>
        <text class="p2-csub">剩余免费时长不受影响</text>
        <text class="p2-cplayed">本局已玩 {{ playedText }}</text>
        <div class="p2-cbtn p2-act-main" @click="cancelExit"><text class="p2-act-text-on">继续游戏</text></div>
        <div class="p2-cbtn p2-act-danger" @click="doExit"><text class="p2-act-text-on">确认退出</text></div>
      </div>

      <div v-else class="p2-rbody">
        <!-- 画质只剩「流畅」一档（用户 m12491 授权移除其余三档：
             标清/高清/蓝光在本机 mppvideodec 上全是马赛克或纯绿）。
             qualityBottom 为空时不再渲染第二行，避免留出一条 44px 的空白。 -->
        <div class="p2-qrow">
          <div v-for="q in qualityTop" :key="q.id" :class="q.cls" @click="pickQuality(q.id)">
            <text :class="q.tcls">{{ q.label }}</text>
          </div>
        </div>
        <div v-if="qualityBottom.length" class="p2-qrow p2-qrow2">
          <div v-for="q in qualityBottom" :key="q.id" :class="q.cls" @click="pickQuality(q.id)">
            <text :class="q.tcls">{{ q.label }}</text>
          </div>
        </div>
        <!-- 画质提示恒定占位（无提示时为空串），避免按钮上下跳动 -->
        <div class="p2-rhint"><text class="p2-rhint-text">{{ qualityHint }}</text></div>

        <!-- 220px 内容宽放不下一整行 stats（12.3 Mbps · 60fps · 丢包 0 · 1280x720），拆两行 -->
        <div class="p2-rstat">
          <text class="p2-rstat-main">{{ statsMain }}</text>
          <text class="p2-rstat-dim">{{ statsSub }}</text>
        </div>
        <div class="p2-rfill"></div>

        <div v-if="stage === 'nologin'" class="p2-racts">
          <div class="p2-act p2-gap p2-act-main" @click="goLogin"><text class="p2-act-text-on">去登录</text></div>
          <div class="p2-act p2-gap" @click="goIndex"><text class="p2-act-text">返回首页</text></div>
          <div class="p2-act p2-act-lock" @click="askExit"><text class="p2-act-text">离开</text></div>
        </div>
        <div v-else class="p2-racts">
          <div :class="reconnectCls" @click="onReconnect"><text :class="reconnectTcls">重连</text></div>
          <div :class="reapplyCls" @click="onReapply"><text class="p2-act-text">重新申请</text></div>
          <div class="p2-act p2-act-danger" @click="askExit"><text class="p2-act-text-on">退出</text></div>
        </div>

        <div class="p2-rfoot"><text class="p2-rfoot-text">{{ footText }}</text></div>
      </div>
    </div>
  </div>
</template>
<script>
import { navTo, navBack, peekParams, requestHome } from '../../services/nav.js';
import { restoreSession, isLoggedIn, currentSession } from '../../services/auth.js';
import { applyTicket, stopGame } from '../../services/session.js';
import { startPlay, stopPlay, setQuality, setRect, mediaState, setEventHandler, setVideoVisible, setInputBusy, requestKeyFrame } from '../../services/media.js';
import { down, move, up, tap, cancel, setInputMode, declarePlatform, keepAlive, mouseMove, mouseDown, mouseUp, sendText, sendKey } from '../../services/input.js';
import { ImSession } from '../../services/im.js';
import { initScreen, keepAliveStart, keepAliveStop } from '../../services/screen.js';
import { loadSettings, saveSettings } from '../../services/settings.js';
import { recordPlay, touchPlay } from '../../services/history.js';
import { QUALITIES, QUALITY_LABEL, normQuality } from '../../services/const.js';
import { logInfo, logWarn, logError } from '../../services/log.js';
import logoImg from '../../assets/logo.png';

// KMS 硬件平面是盖在 UI 之上的 overlay：视频矩形与 UI 矩形必须完全分离。
// 播放页三栏：左 243 + 中 473（视频）+ 右 244 = 960。473x266 正好 16:9，
// 所以视频矩形必须从 x=243 开始 —— 和模板里 .p2-left 宽度分毫不差，否则挖孔错位。
var VIDEO = { x: 243, y: 0, w: 473, h: 266 };
var VW = 1280;
var VH = 720;
var MAX_TOUCH = 5;

// applyTicket 内部已重试 10 次，页面拿不到真实次数，按「约 3 秒一轮」估算展示。
var ERR_TEXT = {
  1209: '暂无空闲机位，正在为你重试',
  1250: '当前网络质量太差，无法流畅云游戏',
  1251: '该游戏需要会员权益，请先开通会员',
  1269: '请先选择可用的云机房区域',
  1279: '云端电脑异常，请重新申请机位',
  1288: '网络测速未通过，正在重新测速',
  1506: '恢复上局失败，请重新申请机位',
  33803: '机位被其他设备占用，正在重试'
};

function clip(s, n) {
  var v = String(s === undefined || s === null ? '' : s);
  if (v.length <= n) return v;
  return v.slice(0, n) + '…';
}

function blocked(v) {
  if (v === false || v === 0) return true;
  if (typeof v === 'string' && (v === '0' || v === '')) return true;
  return false;
}

function pad2(n) {
  var v = Math.max(0, Math.floor(n));
  return v < 10 ? '0' + v : String(v);
}

export default {
  data: function () {
    return {
      code: '',
      name: '',
      icon: '',
      logo: logoImg,     // 左栏顶部软件图标
      gameType: 'mobile',
      quality: 'low',
      gatewayUrl: '',
      stage: 'boot',
      tip: '',
      errText: '',
      errCode: 0,
      mediaErr: '',
      vizOn: false,
      dots: '',
      clock: 0,
      ticketAt: 0,
      connectAt: 0,
      startAt: 0,
      elapsed: 0,
      slowHint: false,
      ticketTtl: 0,   /* ticket 有效期秒数（≈25），只用于连接预算，不是游玩时长 */
      regionName: '',
      region: '',
      blurayLocked: false,
      qualityHint: '',
      phase: 'idle',
      wsState: 'idle',
      iceState: '',
      dtlsState: '',
      rx: 0,
      lost: 0,
      br: 0,
      fps: 0,
      vw: VW,
      vh: VH,
      kf: 0,
      pli: 0,
      statAt: 0,
      hbAt: 0,   /* 上次发 WS 心跳 "0 <ms>" 的时间 */
      kfTimers: [],   /* 换画质后补发 PLI 的定时器（必须能清，见 clearQualityKf） */
      confirmExit: false,
      pcDownAt: 0,   /* 端游按下时刻，用于区分点击与长按拖动 */
      kbOpen: false, /* 远端弹出了输入框、正在等用户用数字键盘输入 */
      videoHidden: false, /* KMS 视频平面是否已被缩成 1x1（弹系统键盘期间） */
      /* ---- 自绘数字键盘（右栏 244x266，在 KMS 平面之外，不用藏视频） ---- */
      kbShow: false,   /* 键盘是否可见 */
      kbNums: '',      /* 已输入的数字串（用于回显与拼接） */
      kbTip: '',       /* 底部提示（例如「已发送 11 位」） */
      kbTitle: '输入数字', /* 顶部标题：手机号 / 验证码 */
      busy: false
    };
  },
  computed: {
    // 左栏名字区 181px / 13px 字 → 每行最多 12 个全角字；两行放不下就省略号。
    // Falcon 的 <text> 不会自动换行，中文长文本也不自己裁切，所以必须 JS 拆行 + 外层定高。
    nameL1: function () {
      var v = String(this.name || this.code || '云游戏');
      return v.length <= 12 ? v : v.slice(0, 12);
    },
    nameL2: function () {
      var v = String(this.name || this.code || '云游戏');
      if (v.length <= 12) return '';
      return clip(v.slice(12), 12);
    },
    attemptNo: function () {
      return Math.max(1, Math.min(10, Math.floor(this.elapsed / 3) + 1));
    },
    footText: function () {
      var ss = currentSession();
      if (ss && ss.isVip) return '退出立即释放机位 · 会员畅玩中';
      var f = Number(ss && ss.freeTimeLeft) || 0;
      if (f > 0) return '退出立即释放机位 · 账号剩余 ' + Math.floor(f / 60) + ' 分钟';
      return '退出立即释放机位 · 账号免费时长已用尽';
    },
    regionText: function () {
      var r = this.regionName || this.region || '';
      return r ? clip(r, 6) : '自动机房';
    },
    qualityLabel: function () {
      return QUALITY_LABEL[this.quality] || this.quality;
    },
    tagsThird: function () {
      if (this.vizOn && this.statAt) return this.vtagText;
      return this.qualityLabel;
    },
    playedText: function () {
      if (!this.startAt) return '0 秒';
      var sec = Math.max(0, Math.floor((this.clock - this.startAt) / 1000));
      var m = Math.floor(sec / 60);
      var s = sec % 60;
      if (m <= 0) return s + ' 秒';
      return m + ' 分 ' + pad2(s) + ' 秒';
    },
    stageChip: function () {
      var map = {
        boot: '准备中',
        ticket: '申请机位',
        queued: '排队中',
        connecting: '连接中',
        waiting: '等待画面',
        live: '游玩中',
        lost: '已断开',
        fail: '已失败',
        end: '已结束',
        nologin: '未登录'
      };
      return map[this.stage] || '准备中';
    },
    maskTitle: function () {
      var map = {
        boot: '正在准备云游戏…',
        ticket: '正在申请云端机位…',
        queued: '机位繁忙，正在进入排队…',
        connecting: '正在连接游戏网关…',
        waiting: '正在连接云游戏…',
        live: '',
        lost: '连接已断开',
        fail: '未能进入游戏',
        end: '本局已结束',
        nologin: '请先登录网易云游戏'
      };
      var v = map[this.stage];
      return v === undefined ? '正在准备…' : v;
    },
    maskSub: function () {
      var s = this.stage;
      if (s === 'boot') return '校验登录状态与本地设置';
      if (s === 'ticket') return '第 ' + this.attemptNo + ' 次尝试 · 已等待 ' + this.elapsed + ' 秒';
      if (s === 'queued') return '即将进入排队页，请稍候';
      if (s === 'connecting') return '网关已下发，正在建立安全连接';
      if (s === 'waiting') return this.tip || '等待服务端下发画面参数';
      if (s === 'lost') return this.errText || '与游戏服务器断开';
      if (s === 'fail') return this.errText || '请点「重连」或「重新申请」';
      if (s === 'end') return this.errText || '云机位已释放，可重新申请';
      if (s === 'nologin') return '登录后才能申请云端机位';
      return '';
    },
    stripText: function () {
      var viz = this.vizOn ? ' · 已出画' : '';
      return 'phase ' + this.phase + ' · ws ' + this.wsState + ' · ice ' + (this.iceState || '-') + viz;
    },
    statusMain: function () {
      var s = this.stage;
      if (s === 'ticket') return '申请机位中 · 第 ' + this.attemptNo + ' 次尝试';
      if (s === 'queued') return '机位繁忙，正在进入排队…';
      if (s === 'connecting') return '连接游戏网关…';
      if (s === 'waiting') return this.vizOn ? '游玩中' : '等待画面…';
      if (s === 'live') return '游玩中 · ' + this.vw + 'x' + this.vh;
      if (s === 'lost') return '连接已断开';
      if (s === 'fail') return '启动失败';
      if (s === 'end') return '本局已结束';
      if (s === 'nologin') return '未登录';
      return '正在准备…';
    },
    statusSub: function () {
      if (this.errText) return clip(this.errText, 80);
      if (this.tip) return clip(this.tip, 80);
      if (this.stage === 'live') return '触摸中间画面即可操作游戏';
      if (this.stage === 'ticket') return '云端正在分配机位，通常 5~20 秒';
      if (this.stage === 'nologin') return '登录后可申请云端机位并继续本局';
      return '';
    },
    // 左栏只有 219px 内容宽，状态主行必须硬截断（<text> 不裁切会溢出到视频区）
    statusMainClip: function () {
      return clip(this.statusMain, 16);
    },
    statusSub1: function () {
      var v = this.statusSub || '';
      if (!v) return '';
      return v.length <= 15 ? v : v.slice(0, 15);
    },
    statusSub2: function () {
      var v = this.statusSub || '';
      if (v.length <= 15) return '';
      return clip(v.slice(15), 15);
    },
    // 右栏内容宽 220px 放不下一整行「12.3 Mbps · 60fps · 丢包 0 · 1280x720」，拆两行
    /* 统计以 stat 事件为准（native cgmedia 的 poll 里带 kf/pli/rx/bitrateKbps/fps），
       与 WS 心跳频率无关：心跳降到 10s 一次不影响这里刷新。 */
    statsMain: function () {
      if (!this.statAt && !this.vizOn) return '暂无画面数据';
      var mb = this.br > 0 ? (this.br / 1000).toFixed(1) + ' Mbps' : '--';
      return mb + ' · ' + (this.fps || 0) + 'fps';
    },
    statsSub: function () {
      if (!this.statAt && !this.vizOn) return '尚未出画';
      return '丢包 ' + (this.lost || 0) + ' · ' + this.vw + 'x' + this.vh;
    },
    stripClip: function () {
      return clip(this.stripText, 36);
    },
    qualityTop: function () {
      return this.qualityList.slice(0, 2);
    },
    qualityBottom: function () {
      return this.qualityList.slice(2);
    },
    statsText: function () {
      if (!this.statAt) return '暂无画面数据（尚未出画）';
      var mb = this.br > 0 ? (this.br / 1000).toFixed(1) + ' Mbps' : '--';
      return mb + ' · ' + (this.fps || 0) + 'fps · 丢包 ' + (this.lost || 0) + ' · ' + this.vw + 'x' + this.vh;
    },
    dbgText: function () {
      if (!this.statAt) return 'kf - · pli - · rx -';
      return 'kf ' + this.kf + ' · pli ' + this.pli + ' · rx ' + this.rx + ' · ' + this.phase;
    },
    vtagText: function () {
      var mb = this.br > 0 ? (this.br / 1000).toFixed(1) + 'M' : '--';
      return (QUALITY_LABEL[this.quality] || this.quality) + ' ' + mb + ' ' + (this.fps || 0) + 'fps';
    },
    qualityList: function () {
      var out = [];
      for (var i = 0; i < QUALITIES.length; i++) {
        var q = QUALITIES[i];
        var on = this.quality === q;
        var locked = q === 'bluray' && this.blurayLocked;
        // 右栏 2x2 排布：每行两个，只有行首那个需要右间距。
        // 只剩一档时改成铺满整行（220 = 244 - 左右 padding 12*2），且不需要右间距。
        var solo = QUALITIES.length === 1;
        var cls = solo ? 'p2-qbtn p2-qbtn-solo' : 'p2-qbtn';
        if (i % 2 === 0 && !solo) cls += ' p2-gap';
        if (on) cls += locked ? ' p2-qbtn-onlock' : ' sel-on';
        else cls += locked ? ' p2-qbtn-lock' : ' sel-off';
        var label = QUALITY_LABEL[q] || q;
        var tcls = 'p2-qbtn-text';
        if (locked) {
          label = '蓝光·会员';
          tcls = 'p2-qbtn-text-lock';
        } else if (on) {
          tcls = 'p2-qbtn-text-on';
        }
        out.push({ id: q, label: label, cls: cls, tcls: tcls });
      }
      return out;
    },
    canReconnect: function () {
      if (this.busy) return false;
      var s = this.stage;
      if (s === 'boot' || s === 'ticket' || s === 'queued' || s === 'nologin') return false;
      if (s === 'live' && this.wsState === 'open') return false;
      return true;
    },
    canReapply: function () {
      if (this.busy) return false;
      var s = this.stage;
      if (s === 'boot' || s === 'ticket' || s === 'queued' || s === 'nologin') return false;
      return true;
    },
    reconnectCls: function () {
      return this.canReconnect ? 'p2-act p2-gap p2-act-main' : 'p2-act p2-gap p2-act-lock';
    },
    reconnectTcls: function () {
      return this.canReconnect ? 'p2-act-text-on' : 'p2-act-text-lock';
    },
    reapplyCls: function () {
      return this.canReapply ? 'p2-act p2-gap' : 'p2-act p2-gap p2-act-lock';
    }
  },
  // ImSession 必须在 created 建：mounted 时 boot() 已经发出 startPlay，
  // 而网关的 cmd 1 可能在首帧后几百毫秒内就到达，晚建会漏掉第一次弹键盘。
  created: function () {
    this.im = new ImSession();
    this.imBusy = false;
    this.kbOpen = false;
    const self = this;
    // 系统输入法长时间收不到事件时解除忙碌标志，但保留会话（用户之后确认仍能送回文字）
    this.im.onIdle = function () { self.imBusy = false; };
  },
  mounted: function () {
    this.dead = false;
    this.booted = false;
    this.recorded = false;
    this.touchIds = {};
    this.touchCount = 0;
    this.boot();
  },
  beforeDestroy: function () {
    this.cleanup();
  },
  // base-page 的 onUnload 会回调 $root.onUnload（真机实测 beforeDestroy 不一定触发），
  // cleanup 用 this.dead 做幂等，两条路径重复调用都安全。
  onUnload: function () {
    this.cleanup();
  },
  onHide: function () {
    this.hidden = true;
    this.clearTimers();
    try {
      keepAliveStop();
    } catch (e) {}
    // ★ KMS 视频平面是硬件 overlay，zpos 比小程序合成面和系统桌面都高。
    // 只停定时器、不拆管线的话，退到后台 / 直接退出软件后，最后一帧会一直盖在
    // 系统界面上（用户实测报障：游戏都结束了、软件都退出了，画面还在）。
    // 所以一进后台就把管线 set_state(NULL) 掉，让 kmssink 把 plane 还回去。
    // 顺带释放机位：后台既看不到也不能操作，白烧免费时长没意义。
    this.bgTeardown();
  },
  onStart: function () {
    logWarn('[play] page onStart');
  },
  onStop: function () {
    logWarn('[play] page onStop');
    this.bgTeardown();
  },
  onShow: function () {
    logWarn('[play] page onShow stage=' + this.stage);
    this.hidden = false;
    if (this.dead) return;
    if (this.booted) this.startTimers();
    var s = this.stage;
    if (s === 'live' || s === 'waiting' || s === 'connecting') {
      try {
        keepAliveStart(20000);
      } catch (e) {}
      try {
        setRect(VIDEO);
      } catch (e) {}
    }
  },
  methods: {
    boot: function () {
      var self = this;
      var p = peekParams('play') || {};
      self.code = p.code || p.gameCode || '';
      self.name = p.name || self.code || '云游戏';
      self.icon = p.icon || '';
      self.gameType = p.gameType === 'pc' ? 'pc' : 'mobile';
      // 画质只允许当前档位（现在只有 low），历史参数里的 high/bluray 一律纠正
      if (p.quality) self.quality = normQuality(p.quality);
      if (p.gatewayUrl) self.gatewayUrl = p.gatewayUrl;
      if (p.ticketTtl) self.ticketTtl = Number(p.ticketTtl) || 0;
      if (p.regionName) self.regionName = p.regionName;
      logInfo('[play] boot code=' + self.code + ' q=' + self.quality + ' gw=' + (self.gatewayUrl ? 1 : 0));
      self.startTimers();
      self.booted = true;
      if (!self.code) {
        self.fail('缺少游戏参数，无法开始游戏', 0);
        return;
      }
      loadSettings().then(function (st) {
        if (p.quality) return null;
        if (st && st.quality) self.quality = normQuality(st.quality);
        return null;
      }).catch(function () {
        return null;
      }).then(function () {
        return restoreSession();
      }).catch(function (e) {
        logWarn('[play] restore fail ' + clip((e && e.message) || e, 60));
        return null;
      }).then(function () {
        if (!isLoggedIn()) {
          self.stage = 'nologin';
          logWarn('[play] 未登录，引导到登录页');
          return null;
        }
        return self.beginSession();
      }).then(function () {
        if (self.stage === 'nologin' || self.dead) return null;
        if (self.gatewayUrl) {
          if (!self.startAt) self.startAt = Date.now();
          return self.notePlay().then(function () {
            return self.connect();
          });
        }
        return self.applySeat();
      }).catch(function (e) {
        logError('[play] boot 异常 ' + clip((e && e.message) || e, 80));
        self.fail('启动失败：' + clip((e && e.message) || e, 40), 0);
        return null;
      });
    },
    beginSession: function () {
      try {
        setRect(VIDEO);
      } catch (e) {}
      var self = this;
      try {
        setEventHandler(function (ev) {
          self.onMedia(ev);
        });
      } catch (e) {
        logWarn('[play] setEventHandler fail');
      }
      return initScreen().catch(function () {
        return null;
      }).then(function () {
        try {
          keepAliveStart(20000);
        } catch (e) {}
        return null;
      });
    },
    applySeat: function () {
      var self = this;
      if (self.busy) return Promise.resolve(null);
      self.busy = true;
      self.stage = 'ticket';
      self.errText = '';
      self.errCode = 0;
      self.tip = '';
      self.slowHint = false;
      self.ticketAt = Date.now();
      self.elapsed = 0;
      // gameType 由详情页透传（'pc' | 'mobile'）；size 必须与之一致，
      // 端游容器是 1920x1080、手游是 1280x720，传错会让 lock_detail 与实际分辨率不符。
      // regions 故意不传：session.applyTicket 会用 media-servers 查出该游戏真实可用的
      // region 列表（写死 ghnfs 会让所有端游拿到 1288）。
      var gt = self.gameType === 'pc' ? 'pc' : 'mobile';
      var sw = gt === 'pc' ? 1920 : VW;
      var sh = gt === 'pc' ? 1080 : VH;
      return applyTicket({
        gameCode: self.code,
        gameType: gt,
        quality: self.quality,
        size: { w: sw, h: sh }
      }).then(function (r) {
        if (self.dead) {
          // 页面已被销毁但票据刚回来：立刻释放，避免机位/排队位泄漏
          if (r && r.gatewayUrl) {
            logWarn('[play] 页面已销毁但拿到机位，立即释放');
            try {
              var sp = stopGame(self.code);
              if (sp && sp.catch) sp.catch(function () {});
            } catch (e) {}
          }
          return null;
        }
        if (r && r.queued) {
          self.stage = 'queued';
          logInfo('[play] 进入排队 expires=' + r.expires);
          self.clearTimers();
          try {
            keepAliveStop();
          } catch (e) {}
          navTo('queue', { gameCode: self.code, name: self.name, icon: self.icon, expires: r.expires || 0 });
          return null;
        }
        self.gatewayUrl = (r && r.gatewayUrl) || '';
        self.ticketTtl = Number(r && r.ticketTtl) || self.ticketTtl;
        self.regionName = (r && r.regionName) || self.regionName;
        self.region = (r && r.region) || self.region;
        self.detectBluray(r);
        if (!self.startAt) self.startAt = Date.now();
        return self.notePlay().then(function () {
          self.stage = 'connecting';
          return self.connect();
        });
      }).catch(function (e) {
        self.fail(self.reasonOf(e), Number(e && e.code) || 0);
        return null;
      }).then(function () {
        self.busy = false;
        return null;
      });
    },
    connect: function () {
      var self = this;
      if (!self.gatewayUrl) {
        self.fail('没有拿到网关地址，请重新申请机位', 0);
        return Promise.resolve(null);
      }
      self.stage = 'connecting';
      self.connectAt = Date.now();
      self.slowHint = false;
      self.vizOn = false;
      self.tip = '';
      return startPlay({
        gameCode: self.code,
        quality: self.quality,
        gatewayUrl: self.gatewayUrl,
        headers: ''
      }).then(function () {
        if (self.dead) {
          logWarn('[play] 页面已销毁但 ws 已建立，立即停止');
          try {
            var zp = stopPlay();
            if (zp && zp.catch) zp.catch(function () {});
          } catch (e) {}
          return null;
        }
        self.stage = 'waiting';
        self.tip = '已建立 WebSocket，正在鉴权…';
        logInfo('[play] startPlay ok');
        return null;
      }).catch(function (e) {
        self.fail('连接游戏网关失败：' + clip((e && e.message) || e, 30), 0);
        return null;
      });
    },
    notePlay: function () {
      var self = this;
      if (self.recorded) return Promise.resolve(null);
      self.recorded = true;
      return recordPlay({
        code: self.code,
        name: self.name,
        icon: self.icon,
        at: Date.now(),
        seconds: 0
      }).catch(function () {
        return null;
      }).then(function () {
        return null;
      });
    },
    reasonOf: function (e) {
      var code = Number(e && e.code) || 0;
      if (ERR_TEXT[code]) return ERR_TEXT[code];
      var msg = (e && (e.message || e.errmsgcn)) || '';
      if (!msg) return '申请机位失败，请稍后重试';
      return clip(msg, 60);
    },
    detectBluray: function (r) {
      var vals = [];
      var raw = (r && r.raw) || {};
      if (raw.bluray !== undefined && raw.bluray !== null) vals.push(raw.bluray);
      var s = currentSession();
      if (s && s.bluray !== undefined && s.bluray !== null) vals.push(s.bluray);
      if (s && s.user && s.user.bluray !== undefined && s.user.bluray !== null) vals.push(s.user.bluray);
      var locked = false;
      for (var i = 0; i < vals.length; i++) {
        if (blocked(vals[i])) locked = true;
      }
      this.blurayLocked = locked;
      logInfo('[play] blurayLocked=' + locked);
    },
    onMedia: function (ev) {
      if (this.dead || !ev) return;
      var t = ev.t || '';
      var s = this.stage;
      if (t === 'ws') {
        if (ev.state) this.wsState = ev.state;
        if (ev.state === 'open') {
          if (s === 'connecting' && !this.vizOn) this.stage = 'waiting';
          if (!this.vizOn) this.tip = '已连接网关，正在鉴权…';
          /* 官方原文 "25 "+(s?"keymouse":"tochscreen")（tochscreen 少一个 u 是官方拼写）。
             真机实测：切到 keymouse 之后 100/101/102 鼠标事件全部被远端丢弃（点击零响应），
             留在 touchscreen 模式下用鼠标协议反而是通的 —— 所以这里固定 touchscreen，
             端游靠下面的 pcClick() 走鼠标协议即可。 */
          try {
            setInputMode('touchscreen');
          } catch (e) {}
        }
        return;
      }
      if (t === 'offer') {
        if (!this.vizOn) {
          this.stage = 'waiting';
          this.tip = '已收到画面协商，正在拉起播放器…';
        }
        return;
      }
      if (t === 'answer') {
        if (ev.mediaErr) {
          this.mediaErr = String(ev.mediaErr);
          if (!this.vizOn) this.tip = '播放器启动失败：' + clip(this.mediaErr, 32);
        } else if (!this.vizOn) {
          this.tip = '媒体通道已建立，等待首帧…';
        }
        return;
      }
      if (t === 'mediafail') {
        this.mediaErr = ev.msg || '媒体模块不可用';
        this.fail('播放器（cgmedia）启动失败：' + clip(this.mediaErr, 32), 0);
        return;
      }
      if (t === 'ice') {
        this.iceState = ev.state || '';
        if (ev.state === 'probing') { if (!this.vizOn) this.tip = '正在打通媒体网络（ICE）…'; }
        else if (ev.state === 'connected') { if (!this.vizOn) this.tip = '媒体网络已连通，正在安全握手…'; }
        else if (ev.state === 'failed') this.fail('媒体网络打不通（ICE 失败），请点「重连」', 0);
        return;
      }
      if (t === 'dtls') {
        this.dtlsState = ev.state || '';
        if (ev.state === 'connected') { if (!this.vizOn) this.tip = '安全握手完成，正在等待首个视频帧…'; }
        else if (ev.state === 'failed') this.fail('安全握手失败（DTLS），请点「重连」', 0);
        return;
      }
      if (t === 'media') {
        this.vizOn = true;
        this.stage = 'live';
        this.tip = '';
        this.errText = '';   /* 出画即证明协商成功，清掉残留的 auth 期错误文案 */
        this.errCode = 0;
        this.slowHint = false;   /* 复位「等待超过 20 秒」，否则出画后文案仍可能被写回 */
        this.connectAt = 0;
        if (ev.videoW) this.vw = ev.videoW;
        if (ev.videoH) this.vh = ev.videoH;
        try {
          setInputMode('touchscreen');
        } catch (e) {}
        /* 官方 startGame 起播后必发 "10 <platform>" 声明客户端平台，
           部分端游靠它切换鼠标/键盘映射。不发的话只有触摸能用。 */
        try {
          declarePlatform('android');
        } catch (e) {}
        logInfo('[play] 出画 ' + this.vw + 'x' + this.vh + ' audio=' + (ev.audio ? 1 : 0));
        return;
      }
      if (t === 'input') {
        /* ★ 网关下发的输入指令（官方 run.js 的 `op=input` 下行处理，逐条对齐）：
         *   cmd "0 <ts>"  —— 我们自己 keepAlive 心跳的回显，用来测延迟，忽略
         *   cmd "1"       —— 远端弹出了输入框，该唤起键盘了
         *   cmd "2"       —— 远端收起输入框，该收键盘
         *   cmd "3"       —— 微信扫码登录指引
         * 官方是在收到 cmd 1 后延迟 200ms 再聚焦（避免与远端 UI 动画抢焦点）。
         * 我们这边「聚焦」= 弹 Falcon 系统键盘（ImSession → global.startTextEdit），
         * 顺带必须先把 KMS 视频平面缩成 1x1，否则键盘被视频平面整个盖住。 */
        var ic = String(ev.cmd || '');
        var head = ic.split(' ')[0];
        if (head === '1') {
          this.openRemoteKb();
        } else if (head === '2') {
          this.closeRemoteKb();
        } else {
          logWarn('[play] input cmd 忽略: ' + clip(ic, 40));
        }
        return;
      }
      if (t === 'videovis') {
        /* native 回执：1=画面已恢复 0=画面已隐藏。仅用于日志与状态恢复判断。 */
        logInfo('[play] 视频平面 ' + (ev.on ? '恢复' : '隐藏'));
        return;
      }
      if (t === 'stat') {
        var now = Date.now();
        if (now - this.statAt < 900) return;
        this.statAt = now;
        /* 只靠 cgmedia 的 media 事件判断出画并不可靠：真机上有整轮 media 事件
           丢失、但 kf/pli/rx 照常在报的情况，于是遮罩一直盖着（KMS 平面在 UI 之上，
           玩家看到的是画面，中栏文案却永远是「正在连接云游戏…」）。
           stat 里只要出现任意解码/收包计数，就说明画面已经在跑了，直接转 live。 */
        if (!this.vizOn && ((ev.rx | 0) > 0 || (ev.kf | 0) > 0 || (ev.fps | 0) > 0)) {
          this.vizOn = true;
          this.stage = 'live';
          this.tip = '';
          this.errText = '';
          this.errCode = 0;
          this.slowHint = false;
          this.connectAt = 0;
          logInfo('[play] 由 stat 推断出画 ' + (ev.w || 0) + 'x' + (ev.h || 0));
        }
        this.rx = ev.rx || 0;
        this.lost = ev.lost || 0;
        this.br = ev.bitrateKbps || 0;
        this.fps = ev.fps || 0;
        this.kf = ev.kf || 0;
        this.pli = ev.pli || 0;
        if (ev.w) this.vw = ev.w;
        if (ev.h) this.vh = ev.h;
        return;
      }
      if (t === 'error') {
        var d = ev.data || {};
        /* 网关在 ticket 校验阶段会对首个 auth 回一条 400 format invalid，
         * 但其实 offer 紧接着就来了、协商照常进行（真机实测三轮一致）。
         * 这种「协商已成功」后的残留错误不再上屏，免得玩家看到假的失败文案。 */
        var code0 = Number(d.errcode) || 0;
        /* 这条 400 在上一条连接的 auth 之后、新 offer 之前到达，本身是网关对
           「无 media 上下文的首个 auth」的固定回显，与会话是否合法无关。
           判据改成「本局还在进行中」而不是「已经出画」：出画那一瞬间 vizOn 还没
           置位、stage 也可能还停在 connecting，用旧判据会漏掉，于是玩家在正常
           游玩时看到一行假的「服务端错误：format invalid」。 */
        var inSession = this.vizOn
          || this.stage === 'connecting'
          || this.stage === 'waiting'
          || this.stage === 'live';
        if (code0 === 400 && inSession) {
          logWarn('[play] 忽略会话进行中残留的 400 format invalid');
          return;
        }
        this.errText = '服务端错误：' + clip(ev.msg || d.errmsgcn || d.errmsg || '未知错误', 56);
        if (d.errcode) this.errCode = Number(d.errcode) || 0;
        logError('[play] ws error ' + clip(JSON.stringify(d), 120));
        return;
      }
      if (t === 'gamestop') {
        var c = Number(ev.code) || 0;
        if (c === 0) this.finish('游戏已正常结束，机位已释放');
        else this.fail('游戏已被服务端停止（code ' + c + '）', c);
        return;
      }
      if (t === 'wsclose') {
        if (s === 'live' || s === 'waiting' || s === 'connecting') {
          this.stage = 'lost';
          this.vizOn = false;
          this.tip = '';
          this.errText = '与游戏服务器的连接已断开：' + clip(ev.msg || 'closed', 28);
          logWarn('[play] wsclose ' + (ev.msg || ''));
        }
        return;
      }
      if (t === 'rect') {
        logInfo('[play] rect ' + ev.x + ',' + ev.y + ',' + ev.w + ',' + ev.h);
      }
    },
    pickQuality: function (id) {
      if (id === 'bluray' && this.blurayLocked) {
        this.setHint('蓝光需要会员权益，当前账号不可用');
        return;
      }
      if (this.quality === id) return;
      this.quality = id;
      this.setHint('已切换到 ' + (QUALITY_LABEL[id] || id) + ' 画质');
      if (QUALITIES.length <= 1) return; // 只剩一档，不需要真的去换（q 恒定 low）
      try {
        setQuality(id);
      } catch (e) {
        logWarn('[play] setQuality fail');
      }
      /* 换档后主动要一个 IDR。
       *
       * 真机实测（2026-11-07）：换画质时 SPS 报的分辨率**恒为 1280x720**，
       * 所以「档位越高分辨率越高」这个假设是错的；除流畅档外的马赛克/纯绿
       * 只能来自「换档瞬间码流断点，解码器还在等新 IDR」。native 侧只在
       * SPS profile/level 变化时才自动补 PLI，SPS 完全没变的情况它察觉不到，
       * 所以这里补两发（600ms / 2s），跨越网关换档的实际生效时刻。 */
      requestKeyFrame();
      this.clearQualityKf();
      var self = this;
      this.kfTimers = [
        setTimeout(function () { requestKeyFrame(); }, 600),
        setTimeout(function () { requestKeyFrame(); }, 2000)
      ];
      try {
        saveSettings({ quality: id });
      } catch (e) {}
      logInfo('[play] quality → ' + id);
    },
    /* 换档时排的两个补 PLI 定时器必须能被下一次换档/页面销毁清掉，
     * 否则连点四个档位会留下 8 个定时器，而且旧的会打在新会话上。 */
    clearQualityKf: function () {
      var a = this.kfTimers;
      if (!a || !a.length) return;
      for (var i = 0; i < a.length; i++) {
        try { clearTimeout(a[i]); } catch (e) {}
      }
      this.kfTimers = [];
    },
    setHint: function (text) {
      this.qualityHint = text;
      var self = this;
      setTimeout(function () {
        if (self.qualityHint === text) self.qualityHint = '';
      }, 2600);
    },
    askExit: function () {
      if (this.stage === 'nologin') {
        this.doExit();
        return;
      }
      this.confirmExit = true;
    },
    cancelExit: function () {
      this.confirmExit = false;
    },
    onReconnect: function () {
      var self = this;
      if (!self.canReconnect) return Promise.resolve(null);
      self.busy = true;
      self.errText = '';
      self.errCode = 0;
      self.slowHint = false;
      self.tip = '正在重连…';
      return stopPlay().catch(function () {
        return null;
      }).then(function () {
        if (!self.gatewayUrl) return null;
        self.stage = 'connecting';
        self.connectAt = Date.now();
        return startPlay({
          gameCode: self.code,
          quality: self.quality,
          gatewayUrl: self.gatewayUrl,
          headers: ''
        }).then(function () {
          self.stage = 'waiting';
          self.tip = '已重新连接网关，等待画面…';
          logInfo('[play] reconnect ok（复用旧网关）');
          return true;
        }).catch(function () {
          logWarn('[play] 复用旧网关失败，改走重新申请机位');
          self.gatewayUrl = '';
          return false;
        });
      }).then(function (ok) {
        self.busy = false;
        if (ok) return null;
        return self.applySeat();
      });
    },
    onReapply: function () {
      var self = this;
      if (!self.canReapply) return Promise.resolve(null);
      return stopPlay().catch(function () {
        return null;
      }).then(function () {
        self.gatewayUrl = '';
        return self.applySeat();
      });
    },
    finish: function (text) {
      var self = this;
      self.stage = 'end';
      self.errText = text || '';
      self.vizOn = false;
      self.tip = '';
      return self.accumulate().then(function () {
        return stopPlay();
      }).catch(function () {
        return null;
      }).then(function () {
        return self.code ? stopGame(self.code) : null;
      }).catch(function () {
        return null;
      }).then(function () {
        try {
          keepAliveStop();
        } catch (e) {}
        return null;
      });
    },
    fail: function (msg, code) {
      var self = this;
      self.errText = msg || '发生未知错误';
      self.errCode = Number(code) || 0;
      self.stage = 'fail';
      self.vizOn = false;
      self.tip = '';
      logError('[play] fail code=' + self.errCode + ' ' + self.errText);
      self.accumulate().then(function () {
        return stopPlay();
      }).catch(function () {
        return null;
      }).then(function () {
        return self.code ? stopGame(self.code) : null;
      }).catch(function () {
        return null;
      }).then(function () {
        try {
          keepAliveStop();
        } catch (e) {}
        return null;
      });
    },
    accumulate: function () {
      if (!this.code || !this.startAt) return Promise.resolve(null);
      var sec = Math.max(0, Math.round((Date.now() - this.startAt) / 1000));
      this.startAt = 0;
      if (sec < 3) return Promise.resolve(null);
      return touchPlay(this.code, sec).then(function () {
        logInfo('[play] 记账 ' + sec + 's');
        return null;
      }).catch(function () {
        return null;
      });
    },
    doExit: function () {
      var self = this;
      if (self.busy) return Promise.resolve(null);
      // 未登录时从来没申请过机位，直接回首页，别白发一轮 DELETE
      if (self.stage === 'nologin') {
        self.clearTimers();
        self.goHome();
        return Promise.resolve(null);
      }
      self.busy = true;
      self.confirmExit = false;
      self.stage = 'end';
      self.tip = '正在退出并释放机位…';
      logInfo('[play] 用户退出，开始释放机位');
      // 顺序：先停媒体+收 WS（立刻断流量）→ 释放机位 → 停保活 → 记账
      return stopPlay().then(function () {
        return stopGame(self.code);
      }).catch(function () {
        logWarn('[play] stopGame fail（机位可能由服务端超时释放）');
        return null;
      }).then(function () {
        try {
          keepAliveStop();
        } catch (e) {}
        return self.accumulate();
      }).catch(function () {
        return null;
      }).then(function () {
        self.clearTimers();
        self.busy = false;
        logInfo('[play] 退出完成 → index');
        self.goHome();
        return null;
      });
    },
    goLogin: function () {
      navTo('login', {});
    },
    /* 回首页：只关掉自己，剩下「再退一层」交给被露出来的 detail / queue
     * 在自己 onShow 里接力（见 services/nav.js 的 requestHome 注释）。
     * 直接 navTo('index') 会新压一层 index —— 动画是 push、页面栈还会变深。 */
    goHome: function () {
      /* 一次退到底：先把夹在中间的那些页按名字从栈里摘掉，最后再关自己。
       * 不能「先关自己再 setTimeout 关别人」——自己的 JS 上下文一没，定时器
       * 就跟着没了，后面的关不掉（真机实测会停在中间页 detail 上）。
       * 反过来（先摘下面的、最后关自己）在本固件上可行，实测见 HANDOFF。 */
      try {
        if (typeof $falcon !== 'undefined' && typeof $falcon.closePageByName === 'function') {
          $falcon.closePageByName('detail');
          $falcon.closePageByName('queue');
        }
      } catch (e) { /* ignore */ }
      if (!navBack('play')) navTo('index', {});
    },
    goIndex: function () {
      this.goHome();
    },
    startTimers: function () {
      if (this.dead) return;
      this.clearTimers();
      var self = this;
      this.timer = setInterval(function () {
        self.tick();
      }, 500);
    },
    clearTimers: function () {
      this.clearQualityKf();
      if (this.timer) {
        clearInterval(this.timer);
        this.timer = 0;
      }
    },
    tick: function () {
      if (this.dead) return;
      var now = Date.now();
      this.clock = now;
      var s = this.stage;
      if (s === 'live' || s === 'waiting' || s === 'connecting' || s === 'lost') this.dots = '';
      else this.dots = '...'.slice(0, Math.floor(now / 400) % 4);
      try {
        var ms = mediaState();
        if (ms) {
          if (ms.phase) this.phase = ms.phase;
          if (ms.wsState) this.wsState = ms.wsState;
        }
      } catch (e) {}
      if (s === 'ticket' && this.ticketAt) {
        var el = Math.floor((now - this.ticketAt) / 1000);
        if (el !== this.elapsed) this.elapsed = el;
        if (el > 75 && !this.slowHint) {
          this.slowHint = true;
          this.tip = '申请机位较慢，可点「重新申请」重试';
        }
      }
      /* 一旦出画就绝不能再写回「等待画面」类文案：vizOn 是唯一判据。 */
      if (!this.vizOn && s === 'waiting' && this.connectAt && now - this.connectAt > 20000 && !this.slowHint) {
        this.slowHint = true;
        this.tip = '等待画面超过 20 秒，可点「重连」重试';
      }
      /* 兜底：vizOn 为真但 stage 还停在 connecting/waiting 时，把 stage 推到 live，
         中栏遮罩（v-if="!vizOn"）与 stageChip 才会同时正确。 */
      if (this.vizOn && (s === 'connecting' || s === 'waiting')) {
        this.stage = 'live';
        this.tip = '';
        this.slowHint = false;
      }
      /* WS 心跳：官方是走 DataChannel 每 1s 发一次 "0 <ms>"。
         我们没有 SCTP 通道，只能借 WS 的 input 发；但网关会把每一条 input
         都回显成 `op=input`，1s 一次会把日志和轮询队列都灌满（真机实测）。
         改成 10s 一次：既提醒网关会话还活着，又不至于淹没真正的业务帧。 */
      if (s === 'live' || s === 'waiting') {
        if (!this.hbAt || now - this.hbAt >= 10000) {
          this.hbAt = now;
          try { keepAlive(now); } catch (e) {}
        }
      }
    },
    // 后台/退出软件时的兜底拆解：必须同步走到 __cgMedia.stop()，
    // 因为 stopPlay() 的首段全是同步调用（clearInterval + media.stop + ws.close），
    // 后续 Promise 链即使被挂起也不影响平面已经释放。
    bgTeardown: function () {
      var self = this;
      if (self.dead) return;
      /* 系统键盘弹出会触发 onHide/onStop：这不是真退后台，不能拆管线。 */
      if (self.imBusy || self.kbOpen) {
        logWarn('[play] bgTeardown 跳过：系统输入法使用中');
        return;
      }
      var s = self.stage;
      var playing = s === 'live' || s === 'waiting' || s === 'connecting';
      logWarn('[play] onHide 拆管线 stage=' + s + ' viz=' + (self.vizOn ? 1 : 0));
      try {
        var p = stopPlay();
        if (p && p.catch) p.catch(function () {});
      } catch (e) {}
      if (!playing) return;
      self.vizOn = false;
      self.stage = 'end';
      self.tip = '';
      self.errText = '已切到后台，本局已结束并释放机位';
      try {
        var q = self.code ? stopGame(self.code) : null;
        if (q && q.catch) q.catch(function () {});
      } catch (e) {}
      try {
        var r = self.accumulate();
        if (r && r.catch) r.catch(function () {});
      } catch (e) {}
    },
    cleanup: function () {
      if (this.dead) return;
      this.dead = true;
      logInfo('[play] cleanup（beforeDestroy）');
      /* 页面销毁时系统键盘可能还挂着：必须解除忙碌标志，否则 app.js 的
         onHide 守卫会把之后每一次真实的退后台都当成"在打字"而放过去。 */
      try {
        setInputBusy(false);
      } catch (e) {}
      try {
        if (this.im) this.im.dispose();
      } catch (e) {}
      this.videoHidden = false;
      this.clearTimers();
      try {
        setEventHandler(null);
      } catch (e) {}
      try {
        keepAliveStop();
      } catch (e) {}
      try {
        var p = stopPlay();
        if (p && p.catch) p.catch(function () {});
      } catch (e) {}
      // 关键：'queued' 时不能 stopGame —— 本页是排队的发起方，销毁本页后由 queue 页
      // 继续轮询同一个排队位；这里再发一次 DELETE 会把刚申请的排队位取消掉。
      // 'nologin' / 'boot' 时也从未申请过机位，无需释放。
      var st = this.stage;
      if (this.code && st !== 'queued' && st !== 'nologin' && st !== 'boot') {
        try {
          var q = stopGame(this.code);
          if (q && q.catch) q.catch(function () {});
        } catch (e) {}
      }
      try {
        var r = this.accumulate();
        if (r && r.catch) r.catch(function () {});
      } catch (e) {}
    },
    /* ================= 云游戏内文本输入 =================
     * 链路（全部对照官方 run.js 的 op=input 下行分支实现）：
     *   网关 cmd "1" → 弹系统键盘（ImSession→global.startTextEdit）
     *                → 用户输入的文字用 cmd "5 <text>" 逐次送回远端
     *   网关 cmd "2" → 收键盘
     *
     * 两个非显然的坑：
     * 1) KMS 视频平面 zpos=2，**盖在系统键盘所用的合成面之上**。不把画面藏起来，
     *    用户看到的就是"游戏画面还在、键盘没出来"。所以弹键盘前先
     *    setVideoVisible(false)（native 把 render-rectangle 缩到 1x1），收键盘后
     *    setVideoVisible(true)（native 销毁重建 vbox + 补 PLI 求 IDR）。
     * 2) 系统键盘是**另一个全屏 miniapp**，弹出时 Falcon 会给本 App 发 onHide，
     *    而 app.js 的 onHide 会 stopPlay() —— 刚点开输入框整局就被拆了。
     *    所以全程用 setInputBusy(true/false) 打标，app.js 据此跳过。
     */
    openRemoteKb: function () {
      if (this.dead) return;
      /* 远端连发两次 cmd 1 是可能的（玩家来回点输入框），已在弹就忽略，
         否则会把用户正在打的字冲掉。 */
      if (this.kbShow || this.imBusy || this.kbOpen) {
        logWarn('[play] 忽略重复的 cmd 1（键盘已显示）');
        return;
      }
      /* ★ 默认走右栏自绘数字键盘，而不是系统键盘。
         系统键盘是另一个全屏 miniapp，zpos 比 KMS 视频平面低，必须先把视频
         缩成 1x1 才能看见；而 kmssink 的 render-rectangle 只能缩不能放
         （PenBili/session.c:4166 六轮对照：放大只有「管线过一次 READY」有效），
         恢复代价高且实测网关不肯补 IDR，画面常常回不来。
         数字键盘放右栏 244 宽里 —— KMS 挖孔只有中间 473 宽，右栏完全在画面
         之外，天然不遮挡，视频照常播，玩家还能边看边输。
         手机号/验证码都是纯数字，数字键盘够用；真要打汉字再点「系统键盘」。 */
      this.kbNums = '';
      this.kbTip = '';
      this.kbTitle = '输入数字';
      this.kbShow = true;
      this.kbOpen = true;
      logInfo('[play] 远端请求输入 → 显示数字键盘（不遮挡视频）');
    },
    /* ---- 数字键盘：官方 run.js 的发送方式 ----
       文字走 cmd "5 <text>"（onInput 逐次发 e.data），
       按键走 cmd "6 <code>"，其中 8→1 退格、13→2 回车/确认（onKeyup 映射）。 */
    kbDigit: function (d) {
      if (this.dead) return;
      this.kbNums = String(this.kbNums || '') + String(d);
      this.kbTip = '';
      try {
        sendText(String(d));
      } catch (e) {
        logWarn('[play] 发送数字失败 ' + ((e && e.message) || e));
      }
    },
    kbDel: function () {
      if (this.dead) return;
      var s = String(this.kbNums || '');
      if (s.length > 0) this.kbNums = s.slice(0, s.length - 1);
      this.kbTip = '';
      try {
        sendKey(1);
      } catch (e) {
        logWarn('[play] 发送退格失败 ' + ((e && e.message) || e));
      }
    },
    kbOk: function () {
      if (this.dead) return;
      var n = String(this.kbNums || '').length;
      try {
        sendKey(2);
      } catch (e) {
        logWarn('[play] 发送确认失败 ' + ((e && e.message) || e));
      }
      logInfo('[play] 数字键盘确认 len=' + n);
      this.kbShow = false;
      this.kbOpen = false;
      this.kbNums = '';
      this.kbTip = '';
    },
    /* 兜底：改回系统键盘（要打汉字、或远端要求非数字输入时）。
       这条路径才需要藏视频平面，所以代价还是老的。 */
    kbSystem: function () {
      var self = this;
      if (this.dead) return;
      this.kbShow = false;
      this.imBusy = true;
      try {
        setInputBusy(true);
      } catch (e) {}
      this.hideVideoPlane();
      logInfo('[play] 切换到系统键盘 → 隐藏视频平面');
      setTimeout(function () {
        self.askRemoteText();
      }, 250);
    },
    askRemoteText: function () {
      var self = this;
      var done = function () {
        self.imBusy = false;
        self.kbOpen = false;
        try {
          setInputBusy(false);
        } catch (e) {}
        self.showVideoPlane();
        /* 恢复后必须重新声明一次平台：vbox 是新建的，
           kmssink 侧的字幕/force-aspect 等由 native 自己重置，这里只需重发平台声明。 */
        try {
          declarePlatform('android');
        } catch (e) {}
      };
      this.askText({ value: '', placeholder: '输入要发到游戏里的文字', maxlength: 64 }, 'remote')
        .then(function (r) {
          var txt = (r && r.text) || '';
          if (r && r.confirmed && txt) {
            /* 官方是逐次把 e.data（本次新字符）用 "5 <text>" 发出去；
               我们一次拿到整串，按整串发（远端输入框会整体替换成这个值的样子，
               实测原神/第五人格都接受整串）。 */
            try {
              sendText(txt);
              logInfo('[play] 已回传文字 len=' + txt.length);
            } catch (e) {
              logWarn('[play] 回传文字失败 ' + ((e && e.message) || e));
            }
          } else {
            logWarn('[play] 用户取消输入 confirmed=' + (r && r.confirmed));
          }
          done();
        })
        .catch(function (e) {
          logWarn('[play] 输入异常 ' + ((e && e.message) || e));
          done();
        });
    },
    closeRemoteKb: function () {
      /* 远端收起输入框：把还在等的系统键盘关掉，然后恢复画面。 */
      logInfo('[play] 远端收起输入框 → 关闭键盘并恢复画面');
      /* 远端主动收键盘：两种键盘都要收掉。数字键盘只是 UI，不涉及视频平面；
         系统键盘才需要把画面恢复回来。 */
      this.kbShow = false;
      this.kbNums = '';
      this.kbTip = '';
      try {
        if (this.im && this.im.isPending && this.im.isPending()) {
          this.im.cancelPending('remote-close', true);
        }
      } catch (e) {}
      this.kbOpen = false;
      try {
        setInputBusy(false);
      } catch (e) {}
      this.showVideoPlane();
    },
    /* 隐藏/恢复都做成幂等的：cmd 1 与 cmd 2 可能交错到达（玩家快速点开又关掉），
       而 showVideoPlane 的代价是销毁重建整条管线 + 等一个 IDR，重复调用会闪黑屏。 */
    hideVideoPlane: function () {
      if (this.videoHidden) return;
      this.videoHidden = true;
      try {
        setVideoVisible(false);
      } catch (e) {}
    },
    showVideoPlane: function () {
      if (!this.videoHidden) return;
      this.videoHidden = false;
      try {
        setVideoVisible(true);
      } catch (e) {}
    },
    /* 与 login.vue 的 askText 同一范式（含 IME 空结果竞态重试）：
       ImSession.open() 开头会 _closeCurrent()，那个关闭动作会异步抛一个空
       textEditFinished；若它落在本次 pending 建好之后，新 promise 会被直接
       解析成 {confirmed:false,text:''}。判据是「极快返回空结果」= 人不可能
       800ms 内打完字并确认。 */
    askText: async function (opts, field) {
      if (!this.im) return { confirmed: false, text: '' };
      var r = null;
      for (var attempt = 0; attempt < 3; attempt++) {
        var t0 = Date.now();
        try {
          r = await this.im.open(opts);
        } catch (e) {
          logWarn('[play] im throw ' + ((e && e.message) || e));
          return { confirmed: false, text: '', error: 'IM_THROW' };
        }
        var cost = Date.now() - t0;
        var empty = !r || (!r.confirmed && !r.text);
        if (!empty || (r && r.error) || cost > 800 || (r && r.canceled)) break;
        logWarn('[play] im 空结果竞态 cost=' + cost + 'ms attempt=' + attempt + '，重开(' + field + ')');
        await new Promise(function (res) { setTimeout(res, 250); });
      }
      return r || { confirmed: false, text: '' };
    },

    /* 端游（gameType==='pc'）必须走鼠标协议，走触摸协议（cmd 1/2/3）远端不认：
       真机实测发 5 组不同坐标，远端画面变化量恒为 97px 且 bbox 完全一致，
       说明触摸事件被丢在同一个落点。官方 send_message_pc 的点击是
       「mousemove -> 50ms -> mousedown -> 50ms -> mouseup」，只发 down/up 不生效。 */
    pcClick: function (t, holdMs) {
      var p = this.toVideo(t);
      var hold = holdMs || 0;
      if (hold > 300) return;
      var vx = p.vx;
      var vy = p.vy;
      var self = this;
      /* 官方点击节奏是「mousemove -> 50ms -> mousedown -> 50ms -> mouseup」。
         三条 input 连发时，网关/容器侧可能在指针还没落位就收到 down，
         远端看到的是一次无位移点击，点击就丢了。这里按官方节奏串起来。 */
      try {
        mouseMove(vx, vy, self.vw || VW, self.vh || VH, 0);
      } catch (e) {}
      self.setTimeout(function () {
        try {
          mouseDown(vx, vy, self.vw || VW, self.vh || VH, 0, 0);
        } catch (e) {}
        self.setTimeout(function () {
          try {
            mouseUp(vx, vy, self.vw || VW, self.vh || VH, 0, 0);
          } catch (e) {}
        }, 60);
      }, 60);
    },
    pcDrag: function (t) {
      var p = this.toVideo(t);
      try {
        mouseMove(p.vx, p.vy, this.vw || VW, this.vh || VH, 0);
      } catch (e) {}
    },
    /* 触摸是否需要「已出画」才放行？
     * 原来写的是 `if (!this.vizOn) return;`。真机踩过坑：vizOn 由 cgmedia 的
     * media 事件置位，而那个事件以前要求音频也 ready 才发 —— 遇到只发视频不发
     * 音频的网关时 vizOn 永远 false，玩家点击被**全部静默丢弃**（画面在播但点
     * 不动）。native 侧已放宽为「视频首帧即发」，这里再补一层：只要已经进入
     * live/waiting 阶段就放行，vizOn 只用来决定遮罩显示，不再当点击闸门。 */
    touchReady: function () {
      if (this.vizOn) return true;
      var s = this.stage;
      return s === 'live' || s === 'waiting' || s === 'connecting';
    },
    onTS: function (e) {
      /* ★ 这一行曾经在清理临时诊断日志时被误删，导致 onTS 引用未定义的 `ts`
         直接抛 ReferenceError —— 触摸按下事件从头到尾没发出去，连带 touchend
         里 touchIds 为空也被跳过，表现就是「画面在播但屏幕怎么点都没反应」。
         任何时候改动 onTS 都要确认这一行还在。 */
      var ts = (e && e.changedTouches) || [];
      if (!this.touchReady()) return;
      for (var i = 0; i < ts.length; i++) {
        this.touchDown(ts[i]);
      }
    },
    onTM: function (e) {
      if (!this.touchReady()) return;
      var ts = (e && e.changedTouches) || [];
      if (this.gameType === 'pc') {
        for (var j = 0; j < ts.length; j++) this.pcDrag(ts[j]);
        return;
      }
      for (var i = 0; i < ts.length; i++) {
        var t = ts[i];
        var key = String((t && t.identifier) === undefined ? '0' : t.identifier);
        var id = this.touchIds[key];
        if (id === undefined) continue;
        var p = this.toVideo(t);
        try {
          move(p.vx, p.vy, id, this.vw || VW, this.vh || VH);
        } catch (err) {}
      }
    },
    onTE: function (e) {
      var ts = (e && e.changedTouches) || [];
      if (this.gameType === 'pc') {
        for (var j = 0; j < ts.length; j++) this.pcClick(ts[j], Date.now() - (this.pcDownAt || 0));
        return;
      }
      for (var i = 0; i < ts.length; i++) {
        var t = ts[i];
        var key = String((t && t.identifier) === undefined ? '0' : t.identifier);
        var id = this.touchIds[key];
        if (id === undefined) continue;
        var p = this.toVideo(t);
        try {
          up(p.vx, p.vy, id, this.vw || VW, this.vh || VH);
        } catch (err) {}
        this.dropTouch(key);
      }
    },
    onTC: function (e) {
      try {
        cancel();
      } catch (e) {}
      this.touchIds = {};
      this.touchCount = 0;
    },
    touchDown: function (t) {
      if (!t) return;
      var key = String(t.identifier === undefined ? '0' : t.identifier);
      if (!this.touchIds) this.touchIds = {};
      if (this.touchCount === undefined) this.touchCount = 0;
      var id = this.touchIds[key];
      if (id === undefined) {
        if (this.touchCount >= MAX_TOUCH) return;
        id = this.freeId();
        this.touchIds[key] = id;
        this.touchCount += 1;
      }
      var p = this.toVideo(t);
      try {
        down(p.vx, p.vy, id, this.vw || VW, this.vh || VH);
      } catch (e) {}
    },
    dropTouch: function (key) {
      if (this.touchIds && this.touchIds[key] !== undefined) {
        delete this.touchIds[key];
        this.touchCount = Math.max(0, (this.touchCount || 1) - 1);
      }
    },
    freeId: function () {
      for (var i = 0; i < MAX_TOUCH; i++) {
        var used = false;
        for (var k in this.touchIds) {
          if (this.touchIds[k] === i) {
            used = true;
            break;
          }
        }
        if (!used) return i;
      }
      return 0;
    },
    toVideo: function (t) {
      /* ★ pageX/pageY 已经是「stage 内」坐标，不要再减 VIDEO.x/VIDEO.y。
         真机实测铁证（三栏布局 VIDEO.x=243）：面板 (111,480) → 逻辑 (480,4)
         → 回调 page=(237,3)，237 = 480-243 正好是 stage 内横坐标，3≈4 是纵向。
         说明 Falcon 的 pageX 横向/纵向都已相对接收事件的元素归一化：
         pageX ∈ [0,473] 对应 stage 水平，pageY ∈ [0,266] 对应 stage 垂直。
         视频缓冲已被 mppvideodec rotation=270 摆正，所以 stage 水平就是视频水平，
         直接按 VIDEO.w/h 归一化即可（历史上唯一一次「减偏移」的改动是错的，已撤回）。 */
      var x = Number(t && t.pageX);
      var y = Number(t && t.pageY);
      if (!isFinite(x)) x = 0;
      if (!isFinite(y)) y = 0;
      if (x < 0) x = 0;
      if (x > VIDEO.w) x = VIDEO.w;
      if (y < 0) y = 0;
      if (y > VIDEO.h) y = VIDEO.h;
      // 必须用出画上报的真实分辨率（data.vw/vh 由 media 事件写入），不能用常量 VW/VH
      // 兜底：不同游戏会出 720x1280 / 1600x900 / 1920x1080，写死 1280x720 会让点击整体偏移。
      var vw = Number(this.vw) || VW;
      var vh = Number(this.vh) || VH;
      /* 坐标系换算（数学推导 + 真机实测双重确认）
       *
       *   原始帧 1280x720 --mppvideodec rotation=270--> 缓冲 720x1280
       *   缓冲 --KMS 缩放到面板 rect[266x473]--> 面板（面板被系统转了 90°）--> 用户所见 473x266
       *
       * 逐级代数展开（详见 HANDOFF 的推导）后，两级 90° 旋转正好抵消，得到：
       *     raw_x = vw * (u / 473)
       *     raw_y = vh * (v / 266)
       * 也就是**线性同轴映射**，与官方 getPercentPos 的语义一致（按显示画面的百分比取值）。
       * 交叉轴、双轴取反都在真机上验证无效，不要再改回去。
       *
       * pageX/pageY 已实测是 stage 内坐标（点面板 x=207 → 逻辑 x=300 → page.x=57=300-243），
       * 所以直接用 u=x、v=y，不要减 VIDEO.x/VIDEO.y。 */
      return {
        vx: Math.round((x / VIDEO.w) * vw),
        vy: Math.round((y / VIDEO.h) * vh)
      };
    }
  }
};
</script>
<style lang="less" scoped>
@import "base.less";

/* 播放态三栏：左 243 + 中 473（KMS 视频挖孔）+ 右 244 = 960，高度统一 266。
   铁律：.stage 必须紧接在 .p2-left 之后，宽度分毫不差 —— KMS 硬件平面按 native
   传过去的绝对 rect 挖孔（px=107+rect_y，py=960-rect_x-rect_w），DOM 顺序一错就错位。
   铁律：.stage 自身保持完全透明（画面从这里透出来），只有未出画时的 .mask 不透明。 */

/* ---------- 左栏 243x266 ---------- */
.p2-left {
  width: 243px;
  height: 266px;
  flex-direction: column;
  padding-left: 12px;
  padding-right: 12px;
  padding-top: 8px;
  padding-bottom: 2px;
  background-color: @nav-background;
}

/* 品牌图标 + 游戏名两行（名字由 JS 截断成 nameL1/nameL2，text 不许自动换行） */
.p2-lhead {
  height: 36px;
  flex-direction: row;
  align-items: center;
}

.p2-logo {
  width: 26px;
  height: 26px;
  border-radius: 6px;
  background-color: transparent;
}

.p2-namebox {
  width: 181px;
  height: 34px;
  flex-direction: column;
  justify-content: center;
  margin-left: 8px;
  overflow: hidden;
}

.p2-name {
  height: 17px;
  font-size: 13px;
  color: @text-color-strong;
  font-weight: 700;
}

/* phase · ws · ice 调试条 */
.p2-lstrip {
  height: 14px;
  margin-top: 6px;
  padding-left: 6px;
  padding-right: 6px;
  border-radius: 4px;
  justify-content: center;
  overflow: hidden;
  background-color: @background-color;
}

.p2-lstrip-text {
  font-size: 9px;
  color: @text-color-dim;
}

/* 地区 + 当前画质 */
.p2-lrow {
  height: 18px;
  flex-direction: row;
  align-items: center;
  margin-top: 6px;
}

.p2-chipgap {
  margin-left: 6px;
}

.p2-ltime {
  height: 24px;
  flex-direction: row;
  align-items: center;
  justify-content: space-between;
  margin-top: 8px;
}

.p2-ltime-label {
  font-size: 10px;
  color: @text-color-dim;
}

.p2-ltime-value {
  font-size: 15px;
  color: @gold;
  font-weight: 700;
}

.p2-lmsg {
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0px;
  flex-direction: column;
  margin-top: 8px;
  overflow: hidden;
}

.p2-lmsg-main {
  height: 16px;
  font-size: 12px;
  color: @text-color;
}

.p2-lmsg-main-err {
  height: 16px;
  font-size: 12px;
  color: @danger;
}

.p2-lmsg-sub {
  height: 14px;
  font-size: 10px;
  color: @text-color-dim;
  margin-top: 2px;
}

.p2-lstat {
  height: 14px;
  justify-content: center;
  overflow: hidden;
}

.p2-lstat-text {
  font-size: 9px;
  color: @text-color-dim;
}

/* ---------- 中栏：视频挖孔 473x266 ---------- */
.stage {
  width: 473px;
  height: 266px;
  flex-direction: column;
  justify-content: center;
  align-items: center;
  background-color: rgba(0, 0, 0, 0);
}

/* 未出画占位：必须不透明盖满 473x266，否则会透出上一页内容 */
.mask {
  width: 473px;
  height: 266px;
  background-color: #191F29;
  justify-content: center;
  align-items: center;
}

.p2-mask-box {
  width: 420px;
  flex-direction: column;
  align-items: center;
}

.p2-mask-title {
  font-size: 17px;
  color: @text-color-strong;
  font-weight: 700;
  text-align: center;
}

.p2-mask-dots {
  height: 22px;
  font-size: 17px;
  color: @accent;
  font-weight: 700;
  text-align: center;
  margin-top: 2px;
}

.p2-mask-sub {
  font-size: 12px;
  color: @text-color-dim;
  text-align: center;
  margin-top: 4px;
}

.p2-mask-err {
  font-size: 12px;
  color: @danger;
  text-align: center;
  margin-top: 8px;
}

/* ---------- 右栏 244x266 ---------- */
.p2-right {
  width: 244px;
  height: 266px;
  flex-direction: column;
  background-color: @background-color;
}

/* ---------- 数字键盘（右栏 244x266 内，完全不遮挡 KMS 视频平面） ----------
   竖向预算：padding 8+8 = 16，内容 250。
   标题 20 + 回显 26 + 四行 (46*4) + 行间 4*3 = 20+26+184+12 = 242，余 8 留白。
   横向预算：244 - padding 20 = 224；键 70*3 + 间距 6*2 = 222 ≤ 224。 */
.p2-kb {
  width: 244px;
  height: 266px;
  flex-direction: column;
  padding-left: 10px;
  padding-right: 10px;
  padding-top: 8px;
  padding-bottom: 8px;
  background-color: @background-color;
}

.p2-kbhead {
  width: 224px;
  height: 20px;
  flex-direction: row;
  justify-content: space-between;
  align-items: center;
}

.p2-kbtitle {
  font-size: 13px;
  color: @text-color-strong;
}

.p2-kbsys {
  width: 62px;
  height: 18px;
  border-radius: 9px;
  border-width: 1px;
  border-style: solid;
  border-color: @card-border-color;
  justify-content: center;
  align-items: center;
}

.p2-kbsys-text {
  font-size: 11px;
  color: @text-color-dim;
}

.p2-kbdisp {
  width: 224px;
  height: 26px;
  margin-top: 2px;
  border-radius: 6px;
  background-color: @ctrl-background;
  justify-content: center;
  align-items: center;
}

.p2-kbval {
  font-size: 15px;
  color: @accent;
}

.p2-kbrow {
  width: 224px;
  height: 46px;
  flex-direction: row;
  justify-content: space-between;
  margin-top: 4px;
}

.p2-kbkey {
  width: 70px;
  height: 46px;
  border-radius: 8px;
  background-color: @card-background-color;
  border-width: 1px;
  border-style: solid;
  border-color: @card-border-color;
  justify-content: center;
  align-items: center;
}

.p2-kbnum {
  font-size: 20px;
  color: @text-color-strong;
}

.p2-kbdel {
  background-color: @ctrl-background;
}

/* 绿底必须纯白字（用户明确要求过，见 README「主题」） */
.p2-kbok {
  background-color: @accent;
  border-color: @accent;
}

.p2-kbok-text {
  font-size: 16px;
  color: #FFFFFF;
}

.p2-rbody {
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0px;
  flex-direction: column;
  padding-left: 12px;
  padding-right: 12px;
  padding-top: 8px;
  padding-bottom: 2px;
}

/* 画质四档 2x2：106*2 + 6 = 218 ≤ 219（244 减左右 padding 各 12，border-box） */
.p2-qrow {
  height: 44px;
  flex-direction: row;
}

.p2-qrow2 {
  margin-top: 8px;
}

.p2-qbtn {
  width: 106px;
  height: 44px;
  border-radius: 8px;
  justify-content: center;
  align-items: center;
}

/* 只剩一档画质时铺满整行（244 - 12*2 = 220），视觉上是一个状态条而不是按钮组 */
.p2-qbtn-solo {
  width: 220px;
  background-color: @card-background-color;
  border-width: 1px;
  border-style: solid;
  border-color: @accent;
}

.p2-gap {
  margin-right: 6px;
}

.p2-qbtn-lock {
  background-color: @background-color;
  border-width: 1px;
  border-style: solid;
  border-color: @card-border-color;
}

.p2-qbtn-onlock {
  background-color: @card-background-color;
  border-width: 1px;
  border-style: solid;
  border-color: @gold;
}

.p2-qbtn-text {
  font-size: 12px;
  color: @text-color;
}

.p2-qbtn-text-on {
  font-size: 12px;
  color: #FFFFFF;
  font-weight: 700;
}

.p2-qbtn-text-lock {
  font-size: 11px;
  color: @text-color-dim;
}

/* 画质提示恒定占位，避免出现/消失时按钮跳动 */
.p2-rhint {
  height: 16px;
  margin-top: 6px;
  justify-content: center;
}

.p2-rhint-text {
  font-size: 10px;
  color: @gold;
}

/* 码率 / 帧率 / 丢包 / 分辨率：拆两行写，避免长文本在 220px 里被截 */
.p2-rstat {
  flex-direction: column;
  margin-top: 8px;
}

.p2-rstat-main {
  height: 13px;
  font-size: 10px;
  color: @accent-blue;
}

.p2-rstat-dim {
  height: 12px;
  font-size: 9px;
  color: @text-color-dim;
  margin-top: 2px;
}

.p2-rfill {
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0px;
}

/* 三个操作按钮：69*3 + 6*2 = 219 ≤ 220 */
.p2-racts {
  height: 36px;
  flex-direction: row;
  margin-top: 6px;
}

.p2-act {
  width: 69px;
  height: 36px;
  border-radius: 8px;
  justify-content: center;
  align-items: center;
  background-color: @ctrl-background;
}

/* 后定义，覆盖 .p2-act 的底色（同为单类名，靠顺序取胜） */
.p2-act-main {
  background-color: @accent;
}

.p2-act-danger {
  background-color: @danger;
}

.p2-act-lock {
  background-color: @ctrl-background;
}

.p2-act-text {
  font-size: 12px;
  color: @text-color;
}

.p2-act-text-on {
  font-size: 12px;
  color: #FFFFFF;
  font-weight: 700;
}

.p2-act-text-lock {
  font-size: 12px;
  color: @text-color-dim;
}

.p2-rfoot {
  height: 16px;
  justify-content: center;
}

.p2-rfoot-text {
  font-size: 9px;
  color: @text-color-dim;
}

/* 退出确认：KMS 盖住中间视频区，弹层只能落在右栏 244 宽里 */
.p2-confirm {
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0px;
  flex-direction: column;
  padding-left: 16px;
  padding-right: 16px;
  padding-top: 10px;
  padding-bottom: 10px;
  justify-content: center;
}

.p2-ctitle {
  height: 22px;
  font-size: 16px;
  color: @text-color-strong;
  font-weight: 700;
}

.p2-csub {
  height: 16px;
  font-size: 11px;
  color: @text-color-dim;
  margin-top: 2px;
}

.p2-cplayed {
  height: 18px;
  font-size: 12px;
  color: @gold;
  margin-top: 8px;
}

/* 底色由 p2-act-main / p2-act-danger 给，这里不写 background-color，否则会覆盖掉 */
.p2-cbtn {
  width: 212px;
  height: 36px;
  border-radius: 8px;
  justify-content: center;
  align-items: center;
  margin-top: 10px;
}
</style>
