<template>
  <div class="page">
    <div class="bar">
      <div class="brand"><image class="brand-logo" :src="logo" :width="26" :height="26"></image></div>
      <div class="tab tab-on"><text class="tab-text-on">排队</text></div>
      <div class="tab" @click="goIndex"><text class="tab-text">首页</text></div>
    </div>

    <div class="content">
      <div class="qhead">
        <text class="qtitle">正在排队中</text>
        <div :class="badgeCls"><text :class="badgeTcls">{{ badgeText }}</text></div>
      </div>
      <div class="qname-wrap"><text class="qname">{{ nameClip }}</text></div>

      <div class="cards">
        <div class="card ccard cwait">
          <text class="cwait-label">预计等待</text>
          <text class="cwait-value">{{ waitText }}</text>
          <text class="cwait-sub">{{ waitSub }}</text>
        </div>
        <div class="card ccard crank">
          <text class="crank-label">当前排名</text>
          <text class="crank-value">{{ rankText }}</text>
          <text class="crank-sub">排队人数由服务端下发</text>
        </div>
        <div class="card ccard ctime">
          <text class="ctime-label">已等待</text>
          <text class="ctime-value">{{ elapsedText }}</text>
          <text class="ctime-sub">每 {{ pollSec }} 秒自动刷新</text>
        </div>
      </div>

      <div class="tiprow">
        <text :class="tipCls">{{ tipText }}</text>
      </div>

      <div class="qacts">
        <div class="btn btn-main a1" @click="retry"><text class="btn-text-on">{{ retryLabel }}</text></div>
        <div class="btn qbtn-danger a2" @click="doCancel"><text class="a2-text">取消排队</text></div>
        <div class="btn a3" @click="goIndex"><text class="btn-text">返回首页</text></div>
      </div>

      <div class="qfoot"><text class="qfoot-text">排队不消耗免费时长 · 入座后免费时长 25 分钟</text></div>
    </div>
  </div>
</template>
<script>
import { navTo, navBack, peekParams } from '../../services/nav.js';
import { status, stopGame, playingList } from '../../services/session.js';
import { restoreSession, isLoggedIn } from '../../services/auth.js';
import { logInfo, logWarn } from '../../services/log.js';
import logoImg from '../../assets/logo.png';

var POLL_MS = 3000;

function clip(s, n) {
  var v = String(s === undefined || s === null ? '' : s);
  if (v.length <= n) return v;
  return v.slice(0, n) + '…';
}

function pad2(n) {
  var v = Math.max(0, Math.floor(n));
  return v < 10 ? '0' + v : String(v);
}

// expires 可能是「绝对毫秒 / 绝对秒 / 剩余秒数」三种形态之一，统一成绝对毫秒。
function normExpire(v, now) {
  var n = Number(v) || 0;
  if (n <= 0) return 0;
  if (n > 1e12) return n;                 // 绝对毫秒
  if (n > 1e9) return n * 1000;           // 绝对秒
  return now + n * 1000;                  // 剩余秒数
}

function pickGateway(entry) {
  if (!entry) return '';
  return entry.gateway_url || entry.gatewayUrl || (entry.ticket && entry.ticket.gateway_url) || '';
}

export default {
  data: function () {
    return {
      logo: logoImg,
      code: '',
      name: '',
      icon: '',
      expires: 0,
      startAt: 0,
      clock: 0,
      elapsed: 0,
      polls: 0,
      rank: 0,
      waiting: 0,
      state: 'waiting',
      tip: '',
      finished: false,
      busy: false,
      pollSec: POLL_MS / 1000
    };
  },
  computed: {
    nameClip: function () {
      return clip(this.name || this.code || '云游戏', 16);
    },
    waitText: function () {
      if (this.state === 'ready') return '即将开始';
      if (this.state === 'expired' || this.state === 'fail') return '--';
      if (this.rank > 0) {
        var m = Math.max(1, Math.ceil(this.rank * 0.6));
        return '约 ' + m + ' 分钟';
      }
      if (this.expires > 0 && this.startAt) {
        var left = Math.max(0, Math.floor((this.expires - this.clock) / 1000));
        if (left > 0) return '最长 ' + Math.ceil(left / 60) + ' 分钟';
      }
      return '预计 1~3 分钟';
    },
    waitSub: function () {
      if (this.state === 'ready') return '正在进入游戏';
      if (this.state === 'expired') return '排队凭证已过期';
      if (this.rank > 0) return '前面还有 ' + this.rank + ' 人';
      return '高峰期可能需要稍久';
    },
    rankText: function () {
      if (this.state === 'ready') return '已就绪';
      if (this.state === 'expired' || this.state === 'fail') return '--';
      if (this.rank > 0) return '第 ' + this.rank + ' 位';
      return '获取中';
    },
    elapsedText: function () {
      var s = this.elapsed;
      var m = Math.floor(s / 60);
      return m + ' 分 ' + pad2(s % 60) + ' 秒';
    },
    badgeText: function () {
      if (this.state === 'ready') return '入座成功';
      if (this.state === 'expired') return '已超时';
      if (this.state === 'fail') return '排队失败';
      if (this.state === 'canceled') return '已取消';
      return '排队中';
    },
    badgeCls: function () {
      if (this.state === 'ready') return 'chip-green bdg';
      if (this.state === 'expired' || this.state === 'fail') return 'chip-red bdg';
      return 'chip bdg';
    },
    badgeTcls: function () {
      if (this.state === 'ready') return 'chip-green-text';
      if (this.state === 'expired' || this.state === 'fail') return 'chip-red-text';
      return 'chip-text';
    },
    tipText: function () {
      if (this.tip) return clip(this.tip, 70);
      if (this.state === 'expired') return '排队凭证已过期，你可以重新排队或返回首页';
      if (this.state === 'fail') return '排队失败：' + (this.tip || '请重新排队');
      if (this.state === 'ready') return '机位已就绪，正在跳转到游戏画面…';
      return '排队中请保持网络畅通，离开本页会失去排队位置';
    },
    tipCls: function () {
      if (this.state === 'expired' || this.state === 'fail') return 'tipline-err';
      if (this.state === 'ready') return 'tipline-ok';
      return 'tipline';
    },
    retryLabel: function () {
      if (this.state === 'expired' || this.state === 'fail') return '重新排队';
      if (this.state === 'ready') return '立即进入';
      return '刷新状态';
    }
  },
  mounted: function () {
    this.dead = false;
    this.boot();
  },
  beforeDestroy: function () {
    this.cleanup();
  },
  onUnload: function () {
    this.cleanup();
  },
  onHide: function () {
    this.clearTimers();
  },
  onShow: function () {
    if (this.dead) return;
    if (this.booted && !this.finished) this.startTimers();
  },
  methods: {
    boot: function () {
      var self = this;
      var p = peekParams('queue') || {};
      self.code = p.gameCode || p.code || '';
      self.name = p.name || self.code || '云游戏';
      self.icon = p.icon || '';
      self.startAt = Date.now();
      self.clock = self.startAt;
      self.expires = normExpire(p.expires, self.startAt);
      self.booted = true;
      logInfo('[queue] boot code=' + self.code + ' expires=' + self.expires);
      self.startTimers();
      if (!self.code) {
        self.state = 'fail';
        self.tip = '缺少游戏参数，无法继续排队';
        return;
      }
      restoreSession().catch(function () {
        return null;
      }).then(function () {
        if (!isLoggedIn()) {
          self.state = 'fail';
          self.tip = '登录已失效，请重新登录后再排队';
          self.clearTimers();
          return null;
        }
        return self.poll();
      }).catch(function (e) {
        logWarn('[queue] boot ' + clip((e && e.message) || e, 60));
        return null;
      });
    },
    poll: function () {
      var self = this;
      if (self.dead || self.finished) return Promise.resolve(null);
      if (self.state === 'expired' || self.state === 'fail' || self.state === 'canceled' || self.state === 'ready') {
        return Promise.resolve(null);
      }
      if (self.busy) return Promise.resolve(null);
      self.busy = true;
      self.polls += 1;
      return status().then(function (st) {
        if (self.dead || self.finished) return null;
        self.applyStatus(st);
        return null;
      }).catch(function (e) {
        logWarn('[queue] status 失败 ' + clip((e && e.message) || e, 50));
        return null;
      }).then(function () {
        return playingList();
      }).catch(function () {
        return [];
      }).then(function (list) {
        self.busy = false;
        if (self.dead || self.finished) return null;
        self.applyPlaying(list);
        return null;
      });
    },
    applyStatus: function (st) {
      if (!st || typeof st !== 'object') return;
      // 服务端真实字段是 ranking（不是 rank/position/queue_position）。
      // 实测 queue_status.data 里只有 ranking / notice_ranking / ranking_time /
      // vip_queue_len / non_vip_queue_len / vip_queue_time / non_vip_queue_time。
      // 有的版本 status 会把在玩列表直接塞进 games_playing，复用同一套匹配逻辑
      if (st.games_playing && st.games_playing.length) this.applyPlaying(st.games_playing);
      var rk = st.ranking !== undefined ? st.ranking
        : st.notice_ranking !== undefined ? st.notice_ranking
        : st.rank !== undefined ? st.rank
        : st.position !== undefined ? st.position
        : st.queue_position;
      if (rk !== undefined && rk !== null && rk !== '') {
        var n = Number(rk);
        if (isFinite(n) && n >= 0) this.rank = n;
      }
      var w = st.waiting !== undefined ? st.waiting : st.queue_size !== undefined ? st.queue_size : st.wait_count;
      if (w !== undefined && w !== null && w !== '') {
        var m = Number(w);
        if (isFinite(m) && m >= 0) this.waiting = m;
      }
      var e = st.expires || st.expire_at || 0;
      if (e) {
        var v = normExpire(e, Date.now());
        if (v > this.expires) this.expires = v;
      }
      if (st.gateway_url && st.game_code && this.code && String(st.game_code) === String(this.code)) {
        this.enterPlay(st.gateway_url);
      }
    },
    applyPlaying: function (list) {
      if (!list || !list.length) return;
      for (var i = 0; i < list.length; i++) {
        var it = list[i] || {};
        var c = it.game_code || it.code || it.gameCode || '';
        if (!c || String(c) !== String(this.code)) continue;
        var gw = pickGateway(it);
        if (gw) {
          this.enterPlay(gw);
          return;
        }
      }
    },
    enterPlay: function (gatewayUrl) {
      if (this.finished || this.dead) return;
      if (!gatewayUrl) return;
      this.finished = true;
      this.state = 'ready';
      this.clearTimers();
      logInfo('[queue] 入座成功 → play');
      navTo('play', { code: this.code, name: this.name, icon: this.icon, gatewayUrl: gatewayUrl });
    },
    retry: function () {
      if (this.state === 'ready') {
        this.poll();
        return;
      }
      if (this.state === 'expired' || this.state === 'fail' || this.state === 'canceled') {
        logInfo('[queue] 重新排队 → play');
        navTo('play', { code: this.code, name: this.name, icon: this.icon });
        return;
      }
      this.tip = '正在刷新排队状态…';
      this.poll();
      var self = this;
      setTimeout(function () {
        if (self.tip === '正在刷新排队状态…') self.tip = '';
      }, 1600);
    },
    doCancel: function () {
      var self = this;
      if (self.finished || self.busy) return Promise.resolve(null);
      self.finished = true;
      self.state = 'canceled';
      self.clearTimers();
      logInfo('[queue] 用户取消排队 code=' + self.code);
      return stopGame(self.code).catch(function () {
        logWarn('[queue] stopGame fail（不影响本地返回）');
        return null;
      }).then(function () {
        navBack('queue');
        return null;
      });
    },
    goIndex: function () {
      this.finished = true;
      this.clearTimers();
      // 排队成功回首页：栈里可能还夹着 detail，先摘中间页、最后关自己
      // （反过来会停在中面页上，见 HANDOFF「回首页也从 push 改成 pop」）。
      try {
        if (typeof $falcon !== 'undefined' && typeof $falcon.closePageByName === 'function') {
          $falcon.closePageByName('detail');
        }
      } catch (e) { /* ignore */ }
      if (!navBack('queue')) navTo('index', {});
    },
    startTimers: function () {
      var self = this;
      this.clearTimers();
      this.timer = setInterval(function () {
        self.tick();
      }, 1000);
      this.poller = setInterval(function () {
        self.poll();
      }, POLL_MS);
    },
    clearTimers: function () {
      if (this.timer) {
        clearInterval(this.timer);
        this.timer = 0;
      }
      if (this.poller) {
        clearInterval(this.poller);
        this.poller = 0;
      }
    },
    tick: function () {
      if (this.dead) return;
      var now = Date.now();
      this.clock = now;
      this.elapsed = Math.max(0, Math.floor((now - this.startAt) / 1000));
      if (this.expires > 0 && now >= this.expires) {
        if (this.state !== 'expired' && this.state !== 'ready' && this.state !== 'canceled') {
          this.state = 'expired';
          this.tip = '';
          this.clearTimers();
          logWarn('[queue] 排队凭证过期');
        }
      }
    },
    cleanup: function () {
      if (this.dead) return;
      this.dead = true;
      this.clearTimers();
      logInfo('[queue] cleanup state=' + this.state);
      // 已入座（ready）时绝对不能 stopGame —— 那会把刚拿到的机位释放掉；
      // 仍在 waiting 就离开本页，主动释放排队位，避免云端留一条幽灵排队。
      if (this.state === 'waiting' && this.code) {
        try {
          var q = stopGame(this.code);
          if (q && q.catch) {
            q.catch(function () {
              return null;
            });
          }
          logInfo('[queue] 离开排队页，释放排队位');
        } catch (e) {}
      }
    }
  }
};
</script>
<style lang="less" scoped>
@import "base.less";

.bdg {
  margin-left: 10px;
}

.qhead {
  height: 34px;
  flex-direction: row;
  align-items: center;
  padding-left: 16px;
}

.qtitle {
  font-size: 1.875vw;
  color: @text-color-strong;
  font-weight: 700;
}

.qname-wrap {
  height: 22px;
  overflow: hidden;
  padding-left: 16px;
}

.qname {
  font-size: 1.4583vw;
  color: @text-color-dim;
}

.cards {
  height: 96px;
  flex-direction: row;
  padding-left: 16px;
  padding-right: 16px;
  margin-top: 10px;
}

.ccard {
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0;
  height: 96px;
  flex-direction: column;
  justify-content: center;
  padding-left: 14px;
  padding-right: 14px;
}

.cwait {
  margin-right: 10px;
}

.crank {
  margin-right: 10px;
}

.cwait-label {
  font-size: 1.1458vw;
  color: @text-color-dim;
}

.cwait-value {
  font-size: 2.2917vw;
  color: @accent;
  font-weight: 700;
  margin-top: 2px;
}

.cwait-sub {
  font-size: 1.0417vw;
  color: @text-color-dim;
  margin-top: 2px;
}

.crank-label {
  font-size: 1.1458vw;
  color: @text-color-dim;
}

.crank-value {
  font-size: 2.2917vw;
  color: @accent-blue;
  font-weight: 700;
  margin-top: 2px;
}

.crank-sub {
  font-size: 1.0417vw;
  color: @text-color-dim;
  margin-top: 2px;
}

.ctime-label {
  font-size: 1.1458vw;
  color: @text-color-dim;
}

.ctime-value {
  font-size: 2.2917vw;
  color: @text-color-strong;
  font-weight: 700;
  margin-top: 2px;
}

.ctime-sub {
  font-size: 1.0417vw;
  color: @text-color-dim;
  margin-top: 2px;
}

.tiprow {
  height: 20px;
  padding-left: 16px;
  padding-right: 16px;
  margin-top: 6px;
  justify-content: center;
}

.tipline {
  font-size: 1.1458vw;
  color: @text-color-dim;
}

.tipline-err {
  font-size: 1.1458vw;
  color: @danger;
}

.tipline-ok {
  font-size: 1.1458vw;
  color: @accent;
}

.qacts {
  height: 34px;
  flex-direction: row;
  padding-left: 16px;
  padding-right: 16px;
  margin-top: 8px;
}

.a1 {
  width: 170px;
  margin-right: 10px;
}

.a2 {
  width: 170px;
  margin-right: 10px;
}

.a3 {
  width: 150px;
}

.qbtn-danger {
  background-color: @danger;
}

.a2-text {
  font-size: 1.4583vw;
  color: #FFFFFF;
  font-weight: 700;
}

.qfoot {
  height: 16px;
  justify-content: center;
  margin-top: 4px;
}

.qfoot-text {
  font-size: 0.9375vw;
  color: @text-color-dim;
}
</style>
