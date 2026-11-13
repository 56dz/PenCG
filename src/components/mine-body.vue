<template>
  <div class="mb-root">
    <div class="mb-head">
      <div class="mb-state"><text class="mb-state-text">{{ headSub }}</text></div>
      <div class="mb-refresh" @click="refreshProfile">
        <text class="mb-refresh-text">{{ busy ? '刷新中…' : '刷新资料' }}</text>
      </div>
    </div>

    <div v-if="confirmMode" class="mb-modal">
      <text class="mb-modal-title">{{ confirmTitle }}</text>
      <text class="mb-modal-desc">{{ confirmDesc }}</text>
      <div class="mb-modal-row">
        <div class="mb-modal-cancel" @click="cancelConfirm"><text class="mb-modal-cancel-text">取消</text></div>
        <div class="mb-modal-ok" @click="doConfirm"><text class="mb-modal-ok-text">确定</text></div>
      </div>
    </div>

    <div v-else-if="!booted" class="mb-guest">
      <div class="mb-guest-avatar"><text class="mb-guest-avatar-text">·</text></div>
      <div class="mb-guest-info">
        <text class="mb-guest-title">正在读取账号…</text>
        <text class="mb-guest-desc">请稍候</text>
      </div>
    </div>

    <div v-else-if="!logged" class="mb-guest">
      <div class="mb-guest-avatar"><text class="mb-guest-avatar-text">?</text></div>
      <div class="mb-guest-info">
        <text class="mb-guest-title">还没有登录网易云游戏</text>
        <text class="mb-guest-desc">登录后同步免费时长、会员权益与游玩记录</text>
        <text v-if="errText" class="mb-guest-err">{{ errText }}</text>
      </div>
      <div class="mb-guest-btn" @click="goLogin"><text class="mb-guest-btn-text">去登录</text></div>
    </div>

    <!-- 赞助页：和 confirmMode 一样整块替换内容区。
         本运行时禁用绝对定位（见文件末尾 .mb-modal 上方注释），所以不能做浮层。
         注意分支顺序：v-if(confirm) → v-else-if(!booted) → v-else-if(!logged)
         → v-else-if(sponsor) → v-else(scroller)。赞助放在 scroller 之前，
         这样点「赞助作者」时整块内容被替换成赞赏码而不是叠在上面。 -->
    <div v-else-if="sponsorOn" class="mb-spon">
      <image class="mb-spon-img" :src="sponsorImg" :width="180" :height="180"></image>
      <div class="mb-spon-side">
        <text class="mb-spon-title">如果 PenCG 对你有用</text>
        <text class="mb-spon-desc">欢迎扫码赞助 —— 你的赞助是我更新的动力</text>
        <text class="mb-spon-hint">用微信扫左侧赞赏码</text>
        <text class="mb-spon-leave">微信扫码后可以在赞赏页留言 —— 想要什么功能、遇到什么问题，都可以直接写上</text>
        <div class="mb-spon-btn" @click="closeSponsor">
          <text class="mb-spon-btn-text">返回</text>
        </div>
      </div>
    </div>

    <scroller v-else class="mb-body">
     <div class="mb-inner">
      <div class="mb-row">
        <div class="mb-ucard">
          <div class="mb-avatar"><text class="mb-avatar-text">{{ initial }}</text></div>
          <div class="mb-uinfo">
            <div class="mb-uname-row">
              <div class="mb-unick-wrap"><text class="mb-unick">{{ nickname }}</text></div>
              <text v-if="isVip" class="mb-vip-badge">VIP</text>
            </div>
            <text class="mb-uuid">ID {{ maskedId }}</text>
            <text class="mb-uphone">{{ phoneText }}</text>
          </div>
        </div>
        <div class="mb-tcard">
          <text class="mb-tcard-label">免费时长</text>
          <text class="mb-tcard-value">{{ timeText }}</text>
          <text class="mb-tcard-sub">{{ timeSub }}</text>
        </div>
        <div class="mb-vcard">
          <text class="mb-vcard-label">会员权益</text>
          <text class="mb-vcard-value">{{ vipName }}</text>
          <text class="mb-vcard-sub">{{ blurayText }}</text>
        </div>
      </div>

      <div class="mb-sec-head">
        <text class="mb-sec-title">游玩历史</text>
        <text class="mb-sec-count">{{ plays.length ? plays.length + ' 条' : '暂无' }}</text>
        <div v-if="plays.length" class="mb-clear-btn" @click="askClearHistory">
          <text class="mb-clear-text">清空</text>
        </div>
      </div>
      <scroller v-if="plays.length" class="mb-his-scroll">
        <div class="mb-his-row">
          <div v-for="p in plays" :key="p.code" class="mb-hcard" @click="openGame(p)">
            <div class="mb-hicon-wrap">
              <image v-if="p.icon" class="mb-hicon" :src="p.icon" :width="44" :height="44"></image>
              <text v-else class="mb-hicon-alt">{{ firstChar(p.name) }}</text>
            </div>
            <div class="mb-hname-wrap"><text class="mb-hname">{{ p.name }}</text></div>
            <text class="mb-hmeta">{{ playMeta(p) }}</text>
          </div>
        </div>
      </scroller>
      <div v-else class="mb-empty-his">
        <text class="mb-empty-text">玩过的游戏会出现在这里，去首页挑一款试试</text>
      </div>

      <div class="mb-sec-head">
        <text class="mb-sec-title">设置与工具</text>
        <text class="mb-sec-count">{{ qualityText }}</text>
      </div>
      <div class="mb-fx-row">
        <div v-for="f in funcs" :key="f.id" class="mb-fcard" @click="runFunc(f.id)">
          <text class="mb-fname">{{ f.label }}</text>
          <text class="mb-fdesc">{{ f.desc }}</text>
        </div>
      </div>

      <div class="mb-foot">
        <text class="mb-foot-text">PenCG {{ ver }} · appid 8002026110200003</text>
        <text class="mb-foot-text">非官方客户端，仅供个人学习使用</text>
      </div>
     </div>
    </scroller>

    <div v-if="toast" class="mb-toast"><text class="mb-toast-text">{{ toast }}</text></div>
  </div>
</template>

<script>
import sponsorImg from '../assets/sponsor.png';
import { restoreSession, currentSession, logout, updateProfile, sessionVersionAsync } from '../services/auth.js';
import { v2Get } from '../services/cg.js';
import { listPlays, clearPlays } from '../services/history.js';
import { loadSettings, saveSettings } from '../services/settings.js';
import { loadGameDetail } from '../services/catalog.js';
import { navTo } from '../services/nav.js';
import { APP_VERSION } from '../services/version.js';
import { imgUrl } from '../services/img.js';
import { QUALITY_LABEL } from '../services/const.js';
import { logInfo, logWarn } from '../services/log.js';

const QUALITY_ORDER = ['low'];

// Weex 的 lines:1 不可靠，长文本必须 JS 侧硬截断（lead 实测结论）
function clip(s, n) {
  const v = String(s === undefined || s === null ? '' : s);
  if (v.length <= n) return v;
  return v.slice(0, n) + '\u2026';
}

// 不要用 toLocaleDateString：这个运行时不一定有，自己拼
function monthDay(ts) {
  const d = new Date(ts);
  if (!d || isNaN(d.getTime())) return '';
  return d.getMonth() + 1 + '\u6708' + d.getDate() + '\u65e5';
}

export default {
  data() {
    return {
      /* 版本号来自 services/version.js，与 package.json、git tag 保持同一号 */
      ver: APP_VERSION,
      logged: false,
      /* booted：restoreSession() 是否已经跑完。
       * 真机现象（用户反馈 1）：进入软件后点「我的」，会先闪一下
       * 「还没有登录网易云游戏」，一两秒后才换成真实资料 —— 因为
       * logged 初值是 false，而 restoreSession() 是异步的。
       * 没跑完之前不该断言「未登录」，只能显示读取中。 */
      booted: false,
      busy: false,
      toast: '',
      errText: '',
      session: null,
      user: null,
      plays: [],
      settings: null,
      headSub: '未登录',
      confirmMode: '',
      confirmTitle: '',
      confirmDesc: '',
      iconFixLock: false,
      /* bug2：跨页会话版本号（见 methods.startVersionWatch 的说明） */
      seq: 0,
      verTimer: null,
      verStopped: false,
      /* bug2 复核加：「版本号已消费但会话仍为空」的补试预算。
       * 见 methods.retryUnlogged 的注释 —— 没有这两个字段的话，一次 storage
       * 瞬时超时就会让界面永久停在「未登录」（版本号已经前进，再也不会重试）。 */
      seqPending: 0,
      seqTry: 0,
      /* bug2：refresh() 防重入。登录成功后同一瞬间会有三条路径同时叫 refresh()
       * （index.onShow 的 refreshMineBody、index.checkSeq、本组件的版本轮询），
       * 不加锁会并发跑多次 reloadLocal()/refreshProfile()，后完成的那次用的是
       * 更早的 currentSession() 快照，可能把界面覆盖回旧状态。 */
      refreshing: false,
      refreshAgain: false,
      /* 赞助弹层开关。默认关；点「赞助作者」才打开。
       * 图片走 assets 的 import（构建时会拷进 images/ 并改写路径），不能写死
       * 相对路径 —— 本运行时没有静态目录概念。 */
      sponsorOn: false,
      sponsorImg: sponsorImg
    };
  },
  computed: {
    nickname() {
      const s = this.session || {};
      const u = this.user || {};
      return clip(u.nickname || s.nickname || '云游戏玩家', 12);
    },
    initial() {
      const n = this.nickname || 'P';
      return n.charAt(0).toUpperCase();
    },
    userId() {
      const s = this.session || {};
      const u = this.user || {};
      return String(u.user_id || u.userId || s.userId || s.user_id || '');
    },
    maskedId() {
      const id = this.userId;
      if (!id) return '--------';
      if (id.length <= 6) return id;
      return id.slice(0, 3) + '****' + id.slice(id.length - 3);
    },
    phoneText() {
      // session.phone 由 auth.js 登录成功后写入（cg.js setPhone）；
      // users/@me 的 phone 字段服务端已脱敏成 138****0000（CGAPI.md:1048）。
      const s = this.session || {};
      const u = this.user || {};
      let p = String(s.phone || '');
      if (!p) p = String(u.phone || u.mobile || '');
      if (!p) return '未绑定手机号';
      // 已经是脱敏串就别再脱一次（否则 138****0000 → 138*****0000）
      if (p.indexOf('*') >= 0) return p;
      if (p.length < 7) return '未绑定手机号';
      return p.slice(0, 3) + '****' + p.slice(p.length - 4);
    },
    vipObj() {
      const u = this.user || {};
      const s = this.session || {};
      return u.vip || s.vip || null;
    },
    isVip() {
      // 权威字段是账号顶层布尔 is_vip（users/@me 实测 true，cg.js 已透传）。
      // vip 对象里只有 end_time，没有 level/expire_time/is_vip，所以按顺序兜底。
      const u = this.user || {};
      const s = this.session || {};
      if (u.is_vip === true || s.is_vip === true || s.isVip === true) return true;
      const v = this.vipObj;
      if (!v) return false;
      if (typeof v === 'boolean') return v;
      if (typeof v === 'number') return v > 0;
      const end = Number(v.end_time || 0);
      if (end > 0) return end * 1000 > Date.now();
      if (v.level || v.vip_level) return Number(v.level || v.vip_level) > 0;
      if (v.expire_time || v.expires) return true;
      return v.is_vip === true;
    },
    vipName() {
      if (!this.isVip) return '普通用户';
      const v = this.vipObj || {};
      const end = Number(v.end_time || 0);
      if (end > 0 && end * 1000 > Date.now()) {
        const md = monthDay(end * 1000);
        return md ? 'VIP 会员 · ' + md + '到期' : 'VIP 会员';
      }
      return 'VIP 会员';
    },
    blurayText() {
      // CGAPI.md:1096 —— bluray 是「是否有蓝光权限」的布尔值，不是时长
      const u = this.user || {};
      const s = this.session || {};
      const b = u.bluray !== undefined && u.bluray !== null ? u.bluray : s.bluray;
      // 三态区分：有权限 / 明确无权限 / 还不知道（users/@me 未返回时不能瞎断言）
      if (b === true || b === 1 || b === 'true') return '蓝光画质可用';
      if (b === false || b === 0 || b === 'false') return '无蓝光权限';
      if (!this.user) return '待同步';
      return this.isVip ? '蓝光画质可用' : '无蓝光权限';
    },
    freeSeconds() {
      const u = this.user || {};
      const s = this.session || {};
      let v = u.free_time_left;
      if (v === undefined || v === null) v = s.freeTimeLeft;
      if (v === undefined || v === null) v = 0;
      const n = Number(v);
      return isNaN(n) ? 0 : n;
    },
    freeMinutes() {
      // 服务端 free_time_left 单位是「秒」（CGAPI.md:975 → 975 秒），必须先换算
      return Math.floor(this.freeSeconds / 60);
    },
    hasFreeData() {
      // 判据必须是「真的拿到过数值」，不能只判 != null：
      // cg.js 的 applyLogin() 里写的是 `G.freeTimeLeft = payload.free_time_left || 0`，
      // 而 restoreSession() 把 storage 里的快照喂给 applyLogin 时，快照字段名是
      // freeTimeLeft（驼峰）、不是 free_time_left（下划线），于是 G.freeTimeLeft 被重置成 0。
      // 也就是说「刚恢复会话、还没拉 @me」和「时长真的用完了」在数据上都是 0，
      // 只能靠 this.user 是否已从 users/@me 返回过来区分。
      const u = this.user || {};
      if (u.free_time_left !== undefined && u.free_time_left !== null) return true;
      const sc = this.session || {};
      return Number(sc.freeTimeLeft) > 0;
    },
    timeText() {
      if (!this.logged) return '登录后查看';
      if (this.freeMinutes <= 0 && this.isVip) return '0 分钟';
      if (!this.hasFreeData) return '同步中…';
      const m = this.freeMinutes;
      if (m <= 0) return '已用尽';
      if (m >= 60) {
        const h = Math.floor(m / 60);
        const r = m % 60;
        return r ? h + ' 小时 ' + r + ' 分' : h + ' 小时';
      }
      return m + ' 分钟';
    },
    timeSub() {
      if (!this.logged) return '登录后自动同步';
      // 会员不走免费额度，0 分钟是正常态，不能说「已用尽」
      if (this.freeMinutes <= 0 && this.isVip) return '会员畅玩中';
      if (!this.hasFreeData) return '正在同步账户资料';
      if (this.freeMinutes <= 0) {
        // 不能只显示「0 分钟」了事，必须给出下一步动作
        return '今日额度已用完，明日 0 点重置';
      }
      return this.freeMinutes < 5 ? '不足 5 分钟，即将用完' : '免费时长可继续畅玩';
    },
    qualityText() {
      const q = (this.settings && this.settings.quality) || 'low';
      return '画质 ' + (QUALITY_LABEL[q] || q);
    },
    funcs() {
      const st = this.settings || {};
      const qLabel = QUALITY_LABEL[st.quality || 'low'] || '流畅';
      return [
        { id: 'quality', label: '画质设置', desc: '当前 ' + qLabel },
        { id: 'history', label: '清除历史', desc: '清空游玩记录' },
        { id: 'about', label: '关于 PenCG', desc: '版本 ' + APP_VERSION },
        { id: 'sponsor', label: '赞助作者', desc: '请作者喝杯奶茶' },
        { id: 'logout', label: '退出登录', desc: '清除本地会话' }
      ];
    }
  },
  mounted() {
    // $refs 在本运行时不一定可用，所以组件自己把自己挂到根实例上，
    // 让外层页面（mine.vue 薄壳）的 onShow 也能触发一次刷新。
    try {
      if (this.$root) this.$root.__mineBody = this;
    } catch (e) {
      /* ignore */
    }
    this.boot();
    this.startVersionWatch();
  },
  beforeDestroy() {
    this.stopVersionWatch();
    try {
      if (this.$root && this.$root.__mineBody === this) this.$root.__mineBody = null;
    } catch (e) {
      /* ignore */
    }
  },
  methods: {
    firstChar(n) {
      const s = String(n || '游');
      return s.charAt(0);
    },
    playMeta(p) {
      const at = Number(p.at) || 0;
      if (!at) return '玩过';
      const now = Date.now();
      const d = now - at;
      if (d < 3600000) return Math.max(1, Math.floor(d / 60000)) + ' 分钟前';
      if (d < 86400000) return Math.floor(d / 3600000) + ' 小时前';
      if (d < 2592000000) return Math.floor(d / 86400000) + ' 天前';
      try {
        return monthDay(at) || '玩过';
      } catch (e) {
        return '玩过';
      }
    },
    closeSponsor() {
      this.sponsorOn = false;
    },
    noop() {
      /* 弹层内部点击不该穿透到遮罩（遮罩绑了 closeSponsor） */
    },
    showToast(t) {
      this.toast = t;
      const self = this;
      setTimeout(function () {
        self.toast = '';
      }, 2000);
    },
    /* ── 会话版本号监看（bug2 修复的兜底通道）──────────────────────────
     * 本运行时没有可用的跨页 Vue 事件总线（全仓没有 $emit 跨页、没有 $falcon.on
     * 的自定义事件），而 watch 只能监听本组件自己的响应式字段 —— 登录页改的
     * 是 $falcon 上的一个普通属性，watch 看不见。所以这里用轮询。
     *
     * 为什么用递增整数：cg.js 的 session() 每次 Object.assign 出新对象，
     * 引用比较在 QuickJS 下恒不相等，靠引用判断「变没变」要么永远触发要么
     * 永远不触发，都不对。
     *
     * 2.5s 一次、只读一个数字，开销可忽略；页面销毁时停掉。
     */
    startVersionWatch() {
      const self = this;
      this.stopVersionWatch();
      this.verStopped = false;
      // 基线也走异步通道，和轮询比对口径一致。
      sessionVersionAsync()
        .then(function (v) {
          if (!self.verStopped) self.seq = v;
        })
        .catch(function () {
          /* ignore */
        });
      const tick = function () {
        if (self.verStopped) return;
        const again = function () {
          if (!self.verStopped) self.verTimer = setTimeout(tick, 2500);
        };
        sessionVersionAsync()
          .then(function (v) {
            if (self.verStopped) return;
            if (v && v !== self.seq) {
              const from = self.seq;
              self.seq = v;
              self.seqTry = 0; // 新版本 → 重置补试预算
              logInfo('[mine] session 版本变化 ' + from + ' -> ' + v + '，重载');
              self.fireRefresh();
              return;
            }
            /* ★ bug2 复核加：版本号没再变，但会话还是空的。
             * 上面这条分支只在「版本号变化」时触发一次，版本号一旦被消费掉，
             * 如果那次 refresh() 里的 restoreSession() 恰好读盘失败（storage.js:46
             * 3s 超时回落 MEMORY、或登录页 setStorage 还没落盘），界面就会**永久**
             * 停在「还没有登录网易云游戏」——没有任何东西会再试第二次。
             * 所以补一条有界的补偿：登录态为空且版本号 > 0 时，再做最多 2 次重试。 */
            self.retryUnlogged(v);
          })
          .catch(function () {
            /* ignore */
          })
          .then(again);
      };
      this.verTimer = setTimeout(tick, 2500);
    },
    stopVersionWatch() {
      this.verStopped = true;
      if (this.verTimer) {
        try {
          clearTimeout(this.verTimer);
        } catch (e) {
          /* ignore */
        }
        this.verTimer = null;
      }
    },
    /* ★ bug2 复核加：内部自发调用 refresh() 一律走这里。
     * refresh() 是 async，任何未捕获的 rejection 在 QuickJS 里可能打断调用链，
     * 让版本轮询（startVersionWatch 的 setTimeout 递归）就此停摆 —— 而它正是
     * 「onShow 不回调」时的唯一兜底通道。这里统一吞掉异常并留一行日志。 */
    fireRefresh() {
      const p = this.refresh();
      if (p && typeof p.catch === 'function') {
        p.catch(function (e) {
          logWarn('[mine] refresh 未捕获异常 ' + ((e && e.message) || e));
        });
      }
      return p;
    },
    /* ★ bug2 复核加：版本号没变但会话仍是空时的有界补偿（见 startVersionWatch）。
     * 为什么需要：`if (v && v !== self.seq)` 这条只在版本号**变化**的那一次触发。
     * 那一轮 refresh() 里若 restoreSession() 读盘失败（storage.js:46 —— getStorage
     * 3s 超时或抛错时回落 MEMORY，而本页 MEMORY 里可能没有这个键），界面就会永久
     * 停在「还没有登录网易云游戏」：版本号已经消费掉，再也不会触发。
     * 上限 2 次（seqTry <= 2），成功登录后自动归零，不会无限刷。 */
    retryUnlogged(v) {
      if (this.logged || this.refreshing) return;
      if (!v || v <= 0) return; // 版本号 0 = 从来没人登录过，别瞎试
      if (this.seqPending === v && this.seqTry >= 2) return;
      if (this.seqPending !== v) {
        this.seqPending = v;
        this.seqTry = 0;
      }
      this.seqTry++;
      logWarn('[mine] 版本号已到 ' + v + ' 但会话为空，补试第 ' + this.seqTry + ' 次');
      this.fireRefresh();
    },
    async boot() {
      // 与 refresh() 共用同一把重入锁（bug2 复核加）：boot() 里 `await restoreSession()`
      // 期间，index.onShow / checkSeq / 版本轮询都可能叫到 refresh()。两边并发跑
      // reloadLocal() 时，先发起后完成的那一轮会用更旧的 currentSession() 快照
      // 把界面写回去，所以 boot 期间到达的 refresh 一律合并到 boot 结束后补跑一次。
      if (this.refreshing) {
        this.refreshAgain = true;
        return;
      }
      this.refreshing = true;
      try {
        try {
          await restoreSession();
        } catch (e) {
          logWarn('[mine] restore fail ' + ((e && e.message) || e));
        }
        try {
          this.settings = await loadSettings();
        } catch (e2) {
          this.settings = null;
        }
        await this.reloadLocal();
        // 会话已恢复完，这时才算「确定」了登录态，可以安全地显示真实内容
        this.booted = true;
        logInfo('[mine] boot logged=' + this.logged);
        if (this.logged) await this.refreshProfile();
      } finally {
        this.refreshing = false;
        if (this.refreshAgain) {
          this.refreshAgain = false;
          this.fireRefresh();
        }
      }
    },
    // 外层（index.vue 内嵌、mine.vue 薄壳的 onShow）统一调它
    async refresh() {
      // ★ 防重入（bug2 复核加）：登录成功那一刻有三条路径同时叫 refresh() ——
      // index.onShow 的 refreshMineBody()、index.checkSeq()、本组件的版本轮询。
      // 不锁的话会并发跑两三遍 reloadLocal()/refreshProfile()，每次都在 await
      // 之后拿一份新的 currentSession() 快照写回 this.logged，虽然值相同但
      // refreshProfile() 内部的 busy 互斥会让后到的那次提前 return，
      // 表现为「刷新按钮偶发没反应」。并发时只让第一轮跑，其余合并成一轮重跑。
      if (this.refreshing) {
        this.refreshAgain = true;
        return;
      }
      this.refreshing = true;
      try {
        for (let round = 0; round < 2; round++) {
          this.refreshAgain = false;
          // ★ bug2 根因修复：reloadLocal() 只读本页 cg.js 里的 G 快照，**不重新读盘**。
          // 登录页在另一份 JS 上下文里写盘 + 写它自己的 G，index 这份上下文的 G
          // 从头到尾没被 applyLogin 碰过，所以 refresh() 再怎么调都还是 logged=false。
          // 必须像 boot() 一样先 restoreSession() 把盘上的会话灌进本上下文的 G。
          // 顺序是硬要求：restoreSession() 必须在 reloadLocal() 之前 await 完成，
          // 否则 reloadLocal() 里的 currentSession() 仍然读到旧的空 G。
          try {
            await restoreSession();
          } catch (e) {
            logWarn('[mine] refresh restore fail ' + ((e && e.message) || e));
          }
          await this.reloadLocal();
          // 会话真的读到了就清掉补偿预算，避免登录后还留着「补试」状态
          if (this.logged) {
            this.seqTry = 0;
            this.seqPending = 0;
          }
          // 诊断用：用户 m13280 第 2 条「登录后跳过来还显示未登录」，需要区分
          // 是 refresh 根本没被调用，还是调用了但读到的会话仍是空。
          logInfo('[mine] refresh logged=' + this.logged + ' booted=' + this.booted);
          if (this.logged) await this.refreshProfile();
          if (!this.refreshAgain) break;
        }
      } finally {
        this.refreshing = false;
        this.refreshAgain = false;
      }
    },
    async reloadLocal() {
      const s = currentSession();
      this.session = s;
      this.logged = !!(s && s.token);
      this.headSub = this.logged ? '已登录 · ' + clip(s.nickname || '云游戏玩家', 16) : '未登录';
      try {
        const list = await listPlays();
        this.plays = (list || []).slice(0, 8).map((p) => {
          return { code: p.code, name: clip(p.name || p.code, 8), icon: imgUrl(p.icon || ''), at: p.at, seconds: p.seconds };
        });
      } catch (e) {
        this.plays = [];
      }
      if (this.logged) this.fixHistoryIcons();
    },
    async fixHistoryIcons() {
      // 历史图标空白的原因：catalog.js 的 fixWebpIcons() 只对实时列表生效，
      // 历史条目走本地存储，从未做过 webp→png；而这台设备没有 webp 解码器。
      // 另外 file:// 指向 cache/lru，可能已被 LRU 淘汰。
      // 这里对「空」和「file://」两类异步补一次最新图标（loadGameDetail 内部会做 webp 转换）。
      if (this.iconFixLock || !this.plays.length) return;
      const need = [];
      for (let i = 0; i < this.plays.length; i++) {
        const ic = String(this.plays[i].icon || '');
        if (!ic || ic.indexOf('file://') === 0) need.push(i);
      }
      if (!need.length) return;
      this.iconFixLock = true;
      try {
        for (let k = 0; k < need.length; k++) {
          const idx = need[k];
          const code = this.plays[idx] && this.plays[idx].code;
          if (!code) continue;
          try {
            const d = await loadGameDetail(code);
            const ic = imgUrl((d && d.icon) || '');
            if (ic && this.plays[idx]) this.plays.splice(idx, 1, Object.assign({}, this.plays[idx], { icon: ic }));
          } catch (e) {
            // 单个失败不影响其它条目，保留原值
            logWarn('[mine] 补图标失败 ' + code);
          }
        }
        logInfo('[mine] 补历史图标 ' + need.length + ' 个');
      } finally {
        this.iconFixLock = false;
      }
    },
    async refreshProfile() {
      // 未登录时点「刷新资料」不应该把人拽去登录页（这只是一个刷新按钮），
      // 只提示一下；真正的登录入口是下面那个大按钮。
      if (!this.logged) {
        this.showToast('登录后才能同步资料');
        return;
      }
      if (this.busy) return;
      this.busy = true;
      this.errText = '';
      try {
        const u = await v2Get('api/v2/users/@me');
        if (u && typeof u === 'object') {
          this.user = u;
          await updateProfile(u);
          this.showToast('资料已更新');
          logInfo('[mine] @me ok free=' + u.free_time_left + ' vip=' + u.is_vip);
        }
      } catch (e) {
        const st = (e && e.status) || 0;
        const code = (e && e.code) || 0;
        // token 过期：v2 接口回 HTTP 401 + errcode 1006（CGAPI.md:64）。
        // 此时不能只写个 errText 就完事——旧 token 还留在 storage 里，
        // 下次进组件还是会走「已登录」分支显示一堆空昵称，必须主动清会话踢回未登录态。
        if (st === 401 || code === 1006) {
          logWarn('[mine] token 失效，清除会话踢回未登录 status=' + st + ' code=' + code);
          try {
            await logout();
          } catch (e2) {
            /* ignore */
          }
          this.user = null;
          this.session = null;
          this.logged = false;
          this.errText = '登录已过期，请重新登录';
          this.headSub = '未登录';
          this.showToast('登录已过期');
          return;
        }
        this.errText = '刷新失败：' + ((e && e.message) || e).toString().slice(0, 40);
        logWarn('[mine] @me fail ' + ((e && e.message) || e));
        this.showToast('刷新失败');
      } finally {
        this.busy = false;
        await this.reloadLocal();
      }
    },
    goLogin() {
      navTo('login', {});
    },
    openGame(p) {
      navTo('detail', { code: p.code, name: p.name });
    },
    askClearHistory() {
      this.confirmMode = 'history';
      this.confirmTitle = '清空游玩历史？';
      this.confirmDesc = '将删除本机记录的全部游玩历史，不可恢复';
    },
    cancelConfirm() {
      this.confirmMode = '';
    },
    async doConfirm() {
      const mode = this.confirmMode;
      this.confirmMode = '';
      if (mode === 'history') await this.clearHistory();
      else if (mode === 'logout') await this.doLogout();
    },
    async clearHistory() {
      try {
        await clearPlays();
        this.plays = [];
        this.showToast('历史已清空');
      } catch (e) {
        this.showToast('清空失败');
      }
    },
    async doLogout() {
      try {
        await logout();
      } catch (e) {
        /* ignore */
      }
      this.user = null;
      await this.reloadLocal();
      this.showToast('已退出登录');
    },
    async runFunc(id) {      if (id === 'quality') {
        const cur = (this.settings && this.settings.quality) || 'low';
        let i = QUALITY_ORDER.indexOf(cur);
        if (i < 0) i = 0;
        const next = QUALITY_ORDER[(i + 1) % QUALITY_ORDER.length];
        this.settings = await saveSettings({ quality: next });
        this.showToast('画质已切换为 ' + (QUALITY_LABEL[next] || next));
        return;
      }
      if (id === 'history') {
        this.askClearHistory();
        return;
      }
      if (id === 'about') {
        this.showToast('PenCG ' + APP_VERSION + ' · 非官方客户端');
        return;
      }
      if (id === 'sponsor') {
        this.sponsorOn = true;
        logInfo('[mine] 打开赞助码');
        return;
      }
      if (id === 'logout') {
        this.confirmMode = 'logout';
        this.confirmTitle = '退出登录？';
        this.confirmDesc = '退出后需要重新输入手机号与密码';
        return;
      }
    }
  }
};
</script>

<style lang="less" scoped>
@import "base.less";

/* 组件化说明：本文件被 pages/mine/mine.vue（薄壳）和 pages/index/index.vue（内嵌栏目）共用。
   - 宽度必须自适应：index 内嵌时父宽约 717px（960-243 左侧导航），薄壳时约 960px。
     所以一律 width:100% / flex 三连，绝不写死 960。
   - <style scoped> 在本运行时实际是全局合并，所有类名统一 mb- 前缀避免撞车。 */

.mb-root {
  width: 100%;
  height: 100%;
  /* 父容器是 flex column（.content）时靠三连撑满；不是 flex 时靠 100% 撑满。
     flex-basis 必须是 0%，写 0 会被忽略（base.less 全项目都是 0%）。 */
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
  flex-direction: column;
}

.mb-head {
  flex-direction: row;
  align-items: center;
  height: 32px;
  padding-left: 12px;
  padding-right: 12px;
}

.mb-state {
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
  height: 16px;
  overflow: hidden;
}

.mb-state-text {
  font-size: 1.1458vw;
  color: @text-color-dim;
}

.mb-refresh {
  width: 82px;
  height: 24px;
  border-radius: 8px;
  background-color: @ctrl-background;
  justify-content: center;
  align-items: center;
}

.mb-refresh-text {
  font-size: 1.25vw;
  color: @accent-blue;
}

.mb-guest {
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
  flex-direction: row;
  align-items: center;
  padding-left: 24px;
  padding-right: 24px;
}

.mb-guest-avatar {
  width: 56px;
  height: 56px;
  border-radius: 28px;
  background-color: @card-background-color;
  justify-content: center;
  align-items: center;
}

.mb-guest-avatar-text {
  font-size: 2.5vw;
  color: @text-color-dim;
}

.mb-guest-info {
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
  flex-direction: column;
  margin-left: 14px;
}

.mb-guest-title {
  font-size: 1.6667vw;
  color: @text-color;
}

.mb-guest-desc {
  font-size: 1.25vw;
  color: @text-color-dim;
  margin-top: 5px;
}

.mb-guest-err {
  font-size: 1.1458vw;
  color: @danger;
  margin-top: 5px;
}

.mb-guest-btn {
  width: 132px;
  height: 40px;
  border-radius: 10px;
  background-color: @accent;
  justify-content: center;
  align-items: center;
}

.mb-guest-btn-text {
  font-size: 1.6667vw;
  color: #FFFFFF;
}

.mb-body {
  width: 100%;
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
  flex-direction: column;
  overflow: hidden;
}

/* scroller 自身不吃 padding，左右留白交给内层 */
.mb-inner {
  width: 100%;
  flex-direction: column;
  padding-left: 12px;
  padding-right: 12px;
}

.mb-row {
  flex-direction: row;
  margin-top: 2px;
}

.mb-ucard {
  width: 236px;
  height: 78px;
  border-radius: 10px;
  background-color: @card-background-color;
  flex-direction: row;
  align-items: center;
  padding-left: 10px;
  margin-right: 6px;
}

.mb-avatar {
  width: 50px;
  height: 50px;
  border-radius: 25px;
  background-color: @accent;
  justify-content: center;
  align-items: center;
}

.mb-avatar-text {
  font-size: 2.2917vw;
  color: #FFFFFF;
}

.mb-uinfo {
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
  flex-direction: column;
  margin-left: 10px;
}

.mb-uname-row {
  flex-direction: row;
  align-items: center;
}

.mb-unick-wrap {
  height: 20px;
  overflow: hidden;
}

.mb-unick {
  font-size: 1.5625vw;
  color: @text-color;
  max-width: 130px;
}

.mb-vip-badge {
  font-size: 1.0417vw;
  color: #FFD35C;
  background-color: #3A3320;
  border-radius: 4px;
  padding-left: 4px;
  padding-right: 4px;
  margin-left: 5px;
}

.mb-uuid {
  font-size: 1.1458vw;
  color: @text-color-dim;
  margin-top: 4px;
}

.mb-uphone {
  font-size: 1.1458vw;
  color: @text-color-dim;
  margin-top: 2px;
}

.mb-tcard {
  width: 206px;
  height: 78px;
  border-radius: 10px;
  background-color: @card-background-color;
  flex-direction: column;
  padding-left: 12px;
  justify-content: center;
  margin-right: 6px;
}

.mb-tcard-label {
  font-size: 1.1458vw;
  color: @text-color-dim;
}

.mb-tcard-value {
  font-size: 2.0833vw;
  color: @accent-blue;
  margin-top: 3px;
}

.mb-tcard-sub {
  font-size: 1.0417vw;
  color: @text-color-dim;
  margin-top: 3px;
}

.mb-vcard {
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
  height: 78px;
  border-radius: 10px;
  background-color: @card-background-color;
  flex-direction: column;
  padding-left: 12px;
  justify-content: center;
}

.mb-vcard-label {
  font-size: 1.1458vw;
  color: @text-color-dim;
}

.mb-vcard-value {
  font-size: 1.5625vw;
  color: #FFD35C;
  margin-top: 3px;
}

.mb-vcard-sub {
  font-size: 1.0417vw;
  color: @text-color-dim;
  margin-top: 3px;
}

.mb-sec-head {
  flex-direction: row;
  align-items: center;
  height: 22px;
  margin-top: 3px;
}

.mb-sec-title {
  font-size: 1.3542vw;
  color: @text-color;
}

.mb-sec-count {
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
  font-size: 1.1458vw;
  color: @text-color-dim;
  margin-left: 6px;
}

.mb-clear-btn {
  width: 46px;
  height: 20px;
  border-radius: 6px;
  background-color: @ctrl-background;
  justify-content: center;
  align-items: center;
}

.mb-clear-text {
  font-size: 1.1458vw;
  color: @text-color-dim;
}

.mb-his-scroll {
  width: 100%;
  height: 80px;
  overflow: hidden;
}

.mb-his-row {
  flex-direction: row;
}

.mb-hcard {
  width: 84px;
  height: 76px;
  border-radius: 10px;
  background-color: @card-background-color;
  flex-direction: column;
  align-items: center;
  margin-right: 6px;
  padding-top: 6px;
}

.mb-hicon-wrap {
  width: 44px;
  height: 44px;
  border-radius: 8px;
  background-color: @ctrl-background;
  justify-content: center;
  align-items: center;
}

.mb-hicon {
  width: 44px;
  height: 44px;
  border-radius: 8px;
}

.mb-hicon-alt {
  font-size: 1.875vw;
  color: @text-color-dim;
}

.mb-hname-wrap {
  height: 14px;
  overflow: hidden;
}

.mb-hname {
  font-size: 1.0417vw;
  color: @text-color;
  margin-top: 4px;
  text-align: center;
}

.mb-hmeta {
  font-size: 0.9375vw;
  color: @text-color-dim;
  margin-top: 1px;
}

.mb-empty-his {
  height: 30px;
  border-radius: 10px;
  background-color: @card-background-color;
  justify-content: center;
  align-items: center;
}

.mb-empty-text {
  font-size: 1.1458vw;
  color: @text-color-dim;
}

.mb-fx-row {
  flex-direction: row;
  margin-top: 2px;
}

.mb-fcard {
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
  height: 44px;
  border-radius: 10px;
  background-color: @card-background-color;
  flex-direction: column;
  justify-content: center;
  padding-left: 10px;
  margin-right: 6px;
}

.mb-fname {
  font-size: 1.25vw;
  color: @text-color;
}

.mb-fdesc {
  font-size: 1.0417vw;
  color: @text-color-dim;
  margin-top: 3px;
  max-width: 120px;
}

.mb-foot {
  flex-direction: column;
  margin-top: 5px;
  padding-bottom: 2px;
}

.mb-foot-text {
  font-size: 1.0417vw;
  color: @text-color-dim;
  margin-top: 2px;
}

.mb-toast {
  height: 22px;
  margin-left: 12px;
  margin-bottom: 6px;
  width: 220px;
  border-radius: 8px;
  background-color: @card-background-color;
  justify-content: center;
  align-items: center;
}

.mb-toast-text {
  font-size: 1.1458vw;
  color: @accent-blue;
}

/* 确认框：本运行时禁用绝对定位，所以用 v-if 整块替换内容区 */
.mb-modal {
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
  flex-direction: column;
  justify-content: center;
  align-items: center;
  padding-left: 24px;
  padding-right: 24px;
}

.mb-modal-title {
  font-size: 1.5625vw;
  color: @text-color;
}

.mb-modal-desc {
  font-size: 1.1458vw;
  color: @text-color-dim;
  margin-top: 8px;
}

.mb-modal-row {
  flex-direction: row;
  margin-top: 14px;
}

.mb-modal-cancel {
  width: 96px;
  height: 34px;
  border-radius: 8px;
  background-color: @ctrl-background;
  justify-content: center;
  align-items: center;
  margin-right: 12px;
}

.mb-modal-cancel-text {
  font-size: 1.3542vw;
  color: @text-color;
}

.mb-modal-ok {
  width: 96px;
  height: 34px;
  border-radius: 8px;
  background-color: @accent;
  justify-content: center;
  align-items: center;
}

.mb-modal-ok-text {
  font-size: 1.3542vw;
  color: #FFFFFF;
}

/* ---- 赞助页 ----
   屏幕只有 960x266，原图 1125x1125 塞不下，所以 assets 里的图已缩到 320x320，
   这里再按 180x180 显示（留余量给圆角与边框）。
   横向排布：左图右字，避免竖排时高度超过 266。

   ★ 不要加 position:absolute / z-index —— 本运行时禁用绝对定位，
   浮层会退化成"插在文档流里的一块"，把下面的内容顶开。这是既有约定，
   见 .mb-modal 上方的注释与 confirmMode 的实现方式。 */
.mb-spon {
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
  flex-direction: row;
  justify-content: center;
  align-items: center;
}

.mb-spon-img {
  width: 180px;
  height: 180px;
  border-radius: 8px;
  background-color: #FFFFFF;
}

.mb-spon-side {
  flex-direction: column;
  justify-content: center;
  margin-left: 20px;
  width: 260px;
}

.mb-spon-title {
  font-size: 1.4583vw;
  color: @text-color;
}

.mb-spon-desc {
  font-size: 1.1458vw;
  color: @text-color-dim;
  margin-top: 8px;
}

.mb-spon-hint {
  font-size: 1.0417vw;
  color: @accent-blue;
  margin-top: 10px;
}

/* 留言提示：比 hint 再退一档，但用金色（和赞助主题呼应），避免和蓝色链接混淆 */
.mb-spon-leave {
  font-size: 1.0417vw;
  line-height: 1.5625vw;
  color: @gold;
  margin-top: 6px;
}

.mb-spon-btn {
  width: 96px;
  height: 34px;
  border-radius: 8px;
  background-color: @ctrl-background;
  justify-content: center;
  align-items: center;
  margin-top: 16px;
}

.mb-spon-btn-text {
  font-size: 1.3542vw;
  color: @text-color;
}
</style>
