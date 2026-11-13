<template>
  <div class="page">
    <!-- 左侧竖导航：76px -->
    <div class="bar" @touchstart="onBarStart" @touchmove="onBarMove" @touchend="onBarEnd" @touchcancel="onBarEnd">
      <div class="brand">
        <image class="brand-logo" :src="logo" :width="26" :height="26"></image>
      </div>
      <div v-for="t in tabs" :key="t.id" :class="tabClass(t)" @click="setTab(t.id)">
        <text :class="tab === t.id ? 'ih-tab-text ih-tab-text-on' : 'ih-tab-text'">{{ t.label }}</text>
      </div>
    </div>

    <!-- 内容区：884x266 -->
    <div class="content">
      <!-- 顶部条：标题 + 搜索（右上角用户名已按需求移除） -->
      <div class="topbar">
        <text class="topbar-title">{{ title }}</text>
        <div class="search-entry" @click="goSearch">
          <text class="search-hint">搜索游戏 / 关键词</text>
        </div>
      </div>

      <!-- 「我的」：原地栏目（不再跳独立页面，和手游/端游一样的界面类型） -->
      <div v-if="tab === 'mine'" class="ih-mine-wrap ih-fade">
        <mine-body ref="body"></mine-body>
      </div>

      <!-- 加载中 -->
      <div v-else-if="loading && !hasAny" class="state-row">
        <text class="state-text">{{ loadingText }}</text>
      </div>

      <!-- 出错 -->
      <div v-else-if="errText && !hasAny" class="state-row" @click="reload">
        <text class="state-text">{{ errText }}</text>
        <div class="retry-btn"><text class="retry-text">重试</text></div>
      </div>

      <!-- 赞助码：整块替换内容区（不做浮层，理由见公告条上方的注释） -->
      <div v-else-if="sponsorOn" class="ih-spon-full ih-fade">
        <image class="ih-spon-full-img" :src="sponsorImg" :width="200" :height="200"></image>
        <div class="ih-spon-full-side">
          <text class="ih-spon-full-title">如果 PenCG 对你有用</text>
          <text class="ih-spon-full-desc">欢迎扫码赞助 —— 你的赞助是我更新的动力</text>
          <text class="ih-spon-full-hint">用微信扫左侧赞赏码</text>
          <text class="ih-spon-full-leave">微信扫码后可以在赞赏页留言 —— 想要什么功能、遇到什么问题，都可以直接写上</text>
          <div class="ih-spon-full-btn" @click="closeSponsor">
            <text class="ih-spon-full-btn-text">返回</text>
          </div>
        </div>
      </div>

      <!-- 首页：横幅 + 多行模块 -->
      <scroller v-else :key="fadeKey" class="body ih-fade">
        <div v-if="errText" class="ih-err" @click="reload">
          <text class="state-text">{{ errText }}</text>
        </div>
        <!-- 横幅 -->
        <div v-if="banners.length" class="banner-row">
          <div v-for="b in banners" :key="b.id" :class="'ih-banner ' + bannerCls" @click="openBanner(b)">
            <image :class="'ih-banner-img ' + imgCls" :src="b.image" :width="bannerW" :height="76"></image>
            <div class="banner-mask">
              <text class="banner-name">{{ b.gameName || '活动' }}</text>
            </div>
          </div>
        </div>

        <!-- 公告第一位：赞助（置顶，点击展开赞赏码）
             ★ 布局约定：本运行时禁用 position:absolute / z-index，
             所以"弹层"一律做成"整块替换内容区"（见下方 sponsorOn 分支）。 -->
        <div class="ih-spon-row" @click="openSponsor">
          <text class="ih-spon-tag">赞助</text>
          <div class="ih-spon-thumb-wrap">
            <image class="ih-spon-thumb" :src="sponsorImg" :width="32" :height="32"></image>
          </div>
          <div class="ih-spon-box">
            <text class="ih-spon-text">{{ sponsorNotice }}</text>
          </div>
          <div class="ih-spon-more"><text class="ih-spon-more-text">查看</text></div>
        </div>

        <!-- 系统公告（原第一位，现退到赞助下方，弱化显示） -->
        <div v-if="notice" class="ih-notice-row">
          <text class="ih-notice-tag">公告</text>
          <div class="ih-notice-box"><text class="ih-notice-text">{{ noticeText }}</text></div>
        </div>

        <!-- 推荐模块：横向滑动 -->
        <div v-for="m in modules" :key="m.id" class="mod">
          <div class="mod-head">
            <text class="mod-title">{{ m.title }}</text>
            <text class="mod-count">{{ m.n }} 款</text>
          </div>
          <scroller class="mod-scroll">
            <div class="mod-row">
              <div v-for="g in m.games" :key="m.id + '_' + g.code" class="gcard" @click="openGame(g)">
                <div class="gcard-icon-wrap"><image class="gcard-icon" :src="g.icon" :width="56" :height="56"></image></div>
                <div class="gcard-line"><text class="gcard-name">{{ g.sName }}</text></div>
                <div class="gcard-line"><text class="gcard-btn">秒玩</text></div>
              </div>
            </div>
          </scroller>
        </div>

        <!-- 分类列表：网格 -->
        <div v-if="list.length" class="mod">
          <div class="mod-head">
            <text class="mod-title">{{ listTitle }}</text>
            <text class="mod-count">{{ countText }}</text>
          </div>
          <div class="grid">
            <div v-for="g in list" :key="'l' + g.code" class="lcard" @click="openGame(g)">
              <div class="lcard-icon-wrap"><image class="lcard-icon" :src="g.icon" :width="56" :height="56"></image></div>
              <div class="lcard-info">
                <text class="lcard-name">{{ g.dName }}</text>
                <text class="lcard-sum">{{ g.dSum }}</text>
              </div>
            </div>
          </div>
          <div class="more-row">
            <div class="more-btn" @click="loadMore"><text class="more-text">{{ moreText }}</text></div>
          </div>
        </div>

        <div v-if="!modules.length && !list.length" class="state-row">
          <text class="state-text">暂无内容，点顶部搜索试试</text>
        </div>
      </scroller>
    </div>
  </div>
</template>

<script>
import {
  loadHomeTabs,
  loadBanners,
  loadNotices,
  loadRecommendations,
  loadGameList
} from '../../services/catalog.js';
import { restoreSession, isLoggedIn, sessionVersionAsync, sessionAge, logout } from '../../services/auth.js';
import { navTo, takeTabParam } from '../../services/nav.js';
import { logInfo, logWarn } from '../../services/log.js';
import logoImg from '../../assets/logo.png';
import sponsorImg from '../../assets/sponsor.png';
import MineBody from '../../components/mine-body.vue';
import { DEV_AUTOSTART, DEV_LOGOUT } from '../../dev-autostart.js';

// 所有栏目都是「原地换内容」，不再有跳页的 tab。
// 「搜索」入口已移除：每个栏目顶部都有搜索框，不需要重复一个搜索 tab。
const TABS = [
  { id: 'recommend', label: '推荐' },
  { id: 'mobile', label: '手游' },
  { id: 'pc', label: '端游' },
  { id: 'free', label: '限免' },
  { id: 'mine', label: '我的' }
];
// 上下滑动手势只在内容用同一套渲染的 4 个列表栏目间循环
const SWIPE_ORDER = ['recommend', 'mobile', 'pc', 'free'];

export default {
  components: { MineBody },
  data() {
    return {
      tabs: TABS,
      tab: 'recommend',
      title: '推荐',
      loading: true,
      logo: logoImg,      // 左侧栏顶部软件图标（构建时由 image 插件拷进 images/）
      loadingText: '正在加载推荐…',
      errText: '',
      banners: [],
      notice: '',
      modules: [],
      list: [],
      listTitle: '全部游戏',
      page: 0,
      total: 0,
      /* 计数文案走 data 而不是 computed：这个运行时对 computed 的模板求值不可靠，
       * 写成 method 会被当成函数对象直接渲染成源码（真机实证）。 */
      countText: '',
      moreText: '加载更多',
      tabIds: null,
      // ---- 以下为 UI 修复新增 ----
      hasAny: false,      // 是否已有可显示内容（决定 loading 是整页替换还是保持旧帧）
      fadeKey: 0,         // 每次整体替换 +1，靠 :key 重挂载重放统一入场动画
      bannerW: 282,       // banner 宽度，按张数平分整行
      bannerCls: 'ih-bn3',
      imgCls: 'ih-img3',
      noticeText: '',     // JS 硬截断后的公告
      sponsorImg: sponsorImg,
      sponsorOn: false,   // 赞助码是否占满内容区
      sponsorNotice: 'PenCG 是个人项目，喜欢可以请作者喝杯奶茶 · 赞赏页能留言提需求',
      barY: -1,           // 手势起点 pageY
      barMoved: 0,
      swipeLock: false,
      /* bug2：跨页会话版本号。登录页登录成功会把 $falcon 上的版本 +1，
       * 本页在 onShow 里比一次就知道「会话变了」，不依赖 nav 的 pending 标记。 */
      sessionSeq: 0,
      seqTimer: null,
      seqStopped: false,
      seqReady: false,
      seqChecking: false
    };
  },
  mounted() {
    this.boot();
    this.startSeqWatch();
  },
  beforeDestroy() {
    this.stopSeqWatch();
  },
  /* 用户反馈 2 的另一半：登录成功后是 navBack() 回到本页的（不是重新
   * navTo 一个 mine 页），而 navBack 没法带参数，所以登录页先用
   * setPendingTab('mine') 留一个标记，这里取一次即销。
   *
   * 另外「我的」栏目的 mine-body 是 v-if 挂载的，从登录页回来时它已经
   * 销毁又重建，所以这里必须让它重新 refresh 一次，否则会看到登录前的旧状态。 */
  onShow() {
    let t = '';
    try {
      t = takeTabParam();
    } catch (e) {
      t = '';
    }
    /* ★ bug2 第二道防线：会话版本号。
     * takeTabParam() 依赖登录页写下的标记；标记若因跨页不共享/过期/被别处
     * 取走而拿不到，下面的分支就什么也不做。这里再异步比一次版本号 ——
     * 登录页登录成功会把版本 +1，本页发现版本变了就强制落到「我的」并刷新。
     * 用版本号而不是会话对象引用：cg.js 的 session() 每次返回新对象。
     * 异步读（sessionVersionAsync）是为了在 $falcon 不可写时还能从盘上看见。
     *
     * 放在最前面（bug2 复核改）：原来这一句在 if 分支的 return 之后，
     * 于是「登录成功 → 带 pending 标记回来」这条最需要它的路径反而永远
     * 走不到它。它是纯异步的，先发出去不影响下面 setTab 的同步顺序。 */
    this.checkSeq();
    if (t && t !== this.tab) {
      this.setTab(t);
      // ★ 不能 return。之前这里直接 return，导致「登录成功 → navBack 回首页 →
      // setTab('mine')」这条路径下 mine-body 的 refresh() 永远不执行。
      // 组件是 v-if 挂载的，登录前就已经挂上（那时 logged=false），登录后
      // 不会重新挂载，于是界面一直停在「还没有登录网易云游戏」——用户 m13280
      // 报的第 2 条「登录后跳过去还是未登录」就是这个。
      // 这里要多等一帧让 v-if 渲染完再拿引用，所以用 setTimeout(0)+重试。
      this.$nextTick
        ? this.$nextTick(this.refreshMineBody)
        : this.refreshMineBody();
      return;
    }
    if (this.tab === 'mine') this.refreshMineBody();
  },
  methods: {
    /* 比对会话版本号，变了就刷新 mine-body；只有「本页的登录态从来登录
     * 变成已登录」时才额外强行落到「我的」。
     *
     * 为什么不在版本变化时就无条件跳栏：版本号是持久化的，冷启动时若首次
     * 读盘失败会读到 0，随后读到历史版本号 N 就会被误判成「刚登录」，
     * 于是用户一进软件就被强制拽到「我的」——这是不能接受的。
     * 用 isLoggedIn() 的前后翻转做二次确认：冷启动时本页 boot() 已经
     * restoreSession() 过，本来就在登录态，翻转不成立，不会误跳；
     * 真登录时本页之前是未登录，翻转成立。 */
    checkSeq() {
      const self = this;
      if (self.seqChecking) return;
      /* ★ 必须等 boot() 把基线读回来才能比对（bug2 复核改）。
       * boot() 里 `sessionSeq = await sessionVersionAsync()` 是异步的，而 onShow
       * 可能在它完成之前就触发。那时 sessionSeq 还是初值 0，任何非 0 的历史
       * 版本号都会被当成「刚变化」，再叠加 isLoggedIn() 在 restoreSession()
       * 之前恒 false，就会误判成「刚登录」→ 冷启动直接被拽到「我的」。
       * 轮询那条通道（:startSeqWatch）本来就门控了 seqReady，这里补上。 */
      if (!self.seqReady) return;
      self.seqChecking = true;
      sessionVersionAsync()
        .then(function (v) {
          if (!v || v === self.sessionSeq) {
            self.seqChecking = false;
            return;
          }
          const from = self.sessionSeq;
          self.sessionSeq = v;
          const was = isLoggedIn();
          return restoreSession()
            .catch(function () {
              return false;
            })
            .then(function () {
              self.seqChecking = false;
              const now = isLoggedIn();
              /* ★ bug2 复核改：跳栏条件加「会话刚写下」这一道。
               * 只看 `!was && now` 有一个冷启动误判窗口：本页 boot() 里的
               * restoreSession() 和 sessionVersionAsync() 都要读盘，storage.js:41
               * 的 getString() 有 3000ms 超时且超时后回落 MEMORY（本页为空的
               * MEMORY），所以两次都可能失败 → was=false；随后 2.5s 轮询里读盘
               * 恢复成功 → now=true，翻转成立 → 老用户一开软件就被拽到「我的」。
               * 用 sessionAge()（loginAt 距今毫秒）挡：只有 10 分钟内写下的会话
               * 才可能是用户刚刚在登录页登录的那一次。读不到 loginAt 时它返回 -1，
               * 一律不跳栏（宁可不动，也不要误跳）。 */
              return sessionAge()
                .catch(function () {
                  return -1;
                })
                .then(function (age) {
                  const fresh = age >= 0 && age < 10 * 60 * 1000;
                  const loggedIn = !was && !!now && fresh;
                  logInfo(
                    '[index] 会话版本 ' +
                      from +
                      ' -> ' +
                      v +
                      ' 本地登录态 ' +
                      was +
                      ' -> ' +
                      now +
                      ' 会话年龄 ' +
                      age +
                      'ms'
                  );
                  if (loggedIn && self.tab !== 'mine') {
                    try {
                      self.setTab('mine');
                    } catch (e) {
                      self.tab = 'mine';
                      self.title = '我的';
                    }
                  }
                  if (self.tab === 'mine' || loggedIn) self.refreshMineBody();
                });
            });
        })
        .catch(function () {
          self.seqChecking = false;
        });
    },
    /* 拿 mine-body 组件实例并让它重新读一次会话。
     * 三条回退路径都不能省：$refs 在本运行时实测常常拿不到（Weex 渲染树
     * 和 Vue 实例树不是一一对应），组件自己在 mounted 里挂了 this.$root.__mineBody。 */
    refreshMineBody() {
      const self = this;
      const pick = function () {
        return (self.$refs && self.$refs.body) || (self.$root && self.$root.__mineBody) || self.__mineBody;
      };
      // 尝试序列：立即、120ms、260ms。刚被 v-if 挂上去时 Vue 还没渲染，
      // 立刻取实例必然拿不到，所以要重试；但**只在全部失败后才打日志**，
      // 否则每次登录都会在日志里留一行假的「实例未就绪」，掩盖真问题。
      const delays = [0, 120, 260];
      const attempt = function (i) {
        const b = pick();
        if (b && b.refresh) {
          try {
            b.refresh();
          } catch (e) {
            /* ignore */
          }
          return;
        }
        if (i + 1 < delays.length) {
          setTimeout(function () {
            attempt(i + 1);
          }, delays[i + 1]);
          return;
        }
        logWarn('[index] mine-body 实例未就绪（已重试 ' + (delays.length - 1) + ' 次）');
      };
      attempt(0);
    },
    // Weex 的 lines:1 在本机型上不可靠（实测中文长名会换行并溢出卡片），
    // 所以按字符数硬截断。74px 宽的卡片用 10px 字只能放 6 个汉字。
    clipName(name, max) {
      const s = String(name || '');
      const n = max || 6;
      return s.length > n ? s.slice(0, n) + '…' : s;
    },
    /* ── 会话版本号兜底轮询（bug2 修复的第三道防线）───────────────────
     * 前两道（nav 的 pending 标记、onShow 里的版本比对）都依赖一件事：
     * closePageByName('login') 之后本页的 onShow 真的会被调用。
     * 真机上这一条恰恰不保证 —— 登录页是 navTo push 上来的，本页一直没被
     * 销毁，宿主可能不重跑 onShow。所以再加一条与 onShow 无关的通道：
     * 自己定时比一次版本号。2.5s 一次、只读一个数字 + 一次本地读盘，可忽略。
     * 用 setTimeout 递归而不是 setInterval：PLATFORM.md 只保证有 setTimeout。
     * 判定逻辑全在 checkSeq() 里，和 onShow 那条路径共用同一套（含误跳保护）。 */
    startSeqWatch() {
      const self = this;
      this.stopSeqWatch();
      this.seqStopped = false;
      const tick = function () {
        if (self.seqStopped) return;
        // boot() 还没把基线版本号读回来时不要比对，否则会把「历史版本号」
        // 误判成「刚登录」而强行跳到「我的」。
        if (self.seqReady) self.checkSeq();
        self.seqTimer = setTimeout(tick, 2500);
      };
      this.seqTimer = setTimeout(tick, 2500);
    },
    stopSeqWatch() {
      this.seqStopped = true;
      if (this.seqTimer) {
        try {
          clearTimeout(this.seqTimer);
        } catch (e) {
          /* ignore */
        }
        this.seqTimer = null;
      }
    },
    async boot() {
      try {
        // 右上角用户名已移除，这里仍恢复会话，避免影响后续详情/队列页的登录态
        await restoreSession();
      } catch (e) {
        /* ignore */
      }
      // 记下当前版本号作为基线：本页启动时就已经存在的会话不算「刚变化」，
      // 否则每次 onShow 都会误判成「刚登录」并强制跳到「我的」。
      // 用异步通道取值，和 onShow/轮询里的比对口径保持一致。
      try {
        this.sessionSeq = await sessionVersionAsync();
      } catch (e) {
        /* ignore */
      }
      // 基线拿到了，轮询才可以开始比对（否则 boot 未完成时轮询会把
      // 「历史版本号」误当成「刚登录」而强行跳到「我的」）。
      this.seqReady = true;
      await this.loadHome();
      this.devLogout();
      this.devAutostart();
    },
    /* ★ 仅取证用（见 src/dev-autostart.js 的 DEV_LOGOUT 说明）：
     * 让 app 自己在启动后走一次生产 logout()（清 cg.js 的 G + 删盘上
     * pencg_session + bump 版本号），用来把设备退回「未登录」初态。
     * 这样就不必 adb 去改 preferences.json（那份文件是「JSON 字符串里再套
     * 一层 JSON」，外面改完 app 内存里还留着旧 session，很容易自欺欺人）。
     * DEV_LOGOUT 为 null 时本方法是空操作。 */
    devLogout() {
      const d = DEV_LOGOUT;
      if (!d) return;
      const delay = Number(d.delay) || 3000;
      logWarn('[dev] autologout → in ' + delay + 'ms');
      const self = this;
      setTimeout(function () {
        logout()
          .then(function () {
            logWarn('[dev] autologout 完成（盘上 pencg_session 已删）');
            return self.refreshMineBody();
          })
          .catch(function (e) {
            logWarn('[dev] autologout 失败 ' + ((e && e.message) || e));
          });
      }, delay);
    },
    /* ★ 仅取证用：见 src/dev-autostart.js 顶部说明。DEV_AUTOSTART 为 null 时
     * 本方法是空操作，生产构建不产生任何跳转。
     * 额外支持一个 tab 字段：先切到该栏目再跳页，用来复现用户真实路径
     * 「在『我的』栏目里点『去登录』」—— 从别的栏目进登录页，回去时要靠
     * setPendingTab 才能落回「我的」，是两条不同的代码路径，都要能测。 */
    devAutostart() {
      const d = DEV_AUTOSTART;
      if (!d || !d.page) return;
      const delay = Number(d.delay) || 2000;
      logWarn('[dev] autostart → ' + d.page + ' in ' + delay + 'ms' + (d.tab ? ' tab=' + d.tab : ''));
      const self = this;
      setTimeout(function () {
        if (d.tab) {
          try {
            self.setTab(d.tab);
            logWarn('[dev] autostart 先切栏目 tab=' + d.tab);
          } catch (e) {
            logWarn('[dev] autostart 切栏目失败 ' + ((e && e.message) || e));
          }
        }
        setTimeout(function () {
          logWarn('[dev] autostart fire → ' + d.page);
          navTo(d.page, d.params || {});
        }, d.tab ? 600 : 0);
      }, delay);
    },
    async prepareTabs() {
      if (this.tabIds) return this.tabIds;
      try {
        const t = await loadHomeTabs();
        const map = {};
        for (const x of t) map[x.type || x.id] = x.id;
        this.tabIds = map;
      } catch (e) {
        this.tabIds = {};
      }
      return this.tabIds;
    },
    async loadHome() {
      this.errText = '';
      // 注意：这里**不置 loading**。首屏时（data 里 loading: true）骨架已经在显示，
      // 非首屏时旧帧会一直显示到新数据到齐，不再经历「骨架→内容」的整页重建。
      this.loadingText = '正在加载推荐…';
      try {
        const t = await this.prepareTabs();
        const tabId = t.recommend || t[''] || '';
        const [bs, ns, mods] = await Promise.all([
          loadBanners().catch(() => []),
          loadNotices().catch(() => []),
          loadRecommendations(tabId).catch((e) => {
            logWarn('[index] 推荐失败: ' + ((e && e.message) || e));
            return [];
          })
        ]);
        const nb = (bs || []).slice(0, 4);
        const nm = this.decorateModules(mods);
        logInfo('[index] 推荐模块 ' + nm.length + ' 个, banner ' + nb.length);
        let fb = null;
        if (!nm.length) {
          // 兜底：直接拉列表（先拿到结果，再和其它块一起提交）
          fb = await this.fetchList(0);
        }
        // ==== 一次性整体替换：所有内容在同一帧生效，不会出现“半边先到” ====
        this.banners = nb;
        this.setNotice((ns && ns[0] && ns[0].text) || '');
        this.modules = nm;
        if (fb) {
          this.applyList(fb.items, fb.total, 0);
        } else {
          this.list = [];
          this.total = 0;
          this.page = 0;
          this.setCount();
        }
        this.applyBannerWidth(nb.length);
        this.hasAny = !!(nb.length || nm.length || this.list.length);
        this.fadeKey = this.fadeKey + 1;
      } catch (e) {
        this.errText = '加载失败：' + ((e && e.message) || e);
      } finally {
        this.loading = false;
      }
    },
    // 渲染期不再调用方法/做三元运算：文本在提交换帧时一次性算好存进数据里。
    // Weex 的 v-for 每帧都会重跑模板表达式，24 个卡 * 2 个表达式在低配设备上会很卡。
    decorateModules(mods) {
      const out = [];
      const src = (mods || []).slice(0, 6);
      for (let i = 0; i < src.length; i++) {
        const m = src[i];
        const games = [];
        const gs = m.games || [];
        for (let j = 0; j < gs.length; j++) {
          const g = gs[j];
          g.sName = this.clipName(g.name);
          games.push(g);
        }
        out.push({ id: m.id, title: m.title, games: games, n: games.length });
      }
      return out;
    },
    decorateList(items) {
      const out = [];
      const src = items || [];
      for (let i = 0; i < src.length; i++) {
        const g = src[i];
        g.dName = this.clipName(g.name, 16);
        g.dSum = g.summary || (g.tags || []).join(' · ');
        out.push(g);
      }
      return out;
    },
    // 只取数据，不碰视图（供原子提交用）
    async fetchList(page) {
      const gt = this.tab === 'pc' ? 'pc' : 'mobile';
      /* 「限免」走官方 tag：`GET api/v1/games?tag=限免` 返回 total=16，
       * 正好是官网「限免专区」那 16 款（详见 services/catalog.js 上方注释）。
       * 注意那接口的 page 是**偏移量**不是页码，且带 tag 时 per_page 被忽略、
       * 一次就返回全部 16 条，所以这个栏目没有「加载更多」。 */
      const onlyFree = this.tab === 'free';
      const r = await loadGameList({ page: page || 0, perPage: 24, gameType: gt, onlyFree: onlyFree });
      return { items: r.items || [], total: r.total || 0, gt: gt, onlyFree: onlyFree,
        rawCount: (r && typeof r.rawCount === 'number') ? r.rawCount : (r.items || []).length };
    },
    setCount: function () {
      /* 限免的 total 现在就是服务端的真实 16，不再需要「已筛」这种本地口径。 */
      this.countText = '共 ' + (this.tab === 'free' ? this.list.length : this.total) + ' 款';
    },
    applyList(items, total, page) {
      this.list = this.decorateList(items);
      /* 限免一页就拿到全部（服务端 total=16），所以 total 直接用列表长度，翻页也不会变。 */
      this.total = this.tab === 'free' ? this.list.length : (total || 0);
      this.page = page || 0;
      const gt = this.tab === 'pc' ? 'pc' : 'mobile';
      this.listTitle = this.tab === 'free' ? '限免游戏' : (gt === 'pc' ? '端游列表' : '手游列表');
      this.moreText = this.tab === 'free' ? '已经到底了'
        : (this.list.length < this.total ? '加载更多' : '已经到底了');
      this.setCount();
    },
    // 一个 tab 的内容（banner/modules 清空的动作放在提交时刻，而不是点击时刻）
    async loadTabList() {
      try {
        const r = await this.fetchList(0);
        this.banners = [];
        this.setNotice('');
        this.modules = [];
        this.applyList(r.items, r.total, 0);
        this.hasAny = this.list.length > 0;
        this.fadeKey = this.fadeKey + 1;
      } catch (e) {
        this.errText = '列表加载失败：' + ((e && e.message) || e);
      } finally {
        this.loading = false;
      }
    },
    async loadList(page, reset) {
      this.loadingText = '正在加载游戏列表…';
      const keep = this.list.slice(0);
      try {
        const r = await this.fetchList(page);
        // 分页：追加，绝不丢旧页
        if (reset) this.list = this.decorateList(r.items);
        else this.list = keep.concat(this.decorateList(r.items));
        this.page = page || 0;
        this.listTitle = this.tab === 'free' ? '限免游戏' : (r.gt === 'pc' ? '端游列表' : '手游列表');
        if (r.onlyFree) {
          /* 带 tag 时限免一次返回全部 16 条（per_page 被忽略），永远没有下一页。 */
          this.total = this.list.length;
          this.moreText = '已经到底了';
        } else {
          this.total = r.total;
          this.moreText = this.list.length < this.total ? '加载更多' : '已经到底了';
        }
        this.setCount();
      } catch (e) {
        this.errText = '列表加载失败：' + ((e && e.message) || e);
      } finally {
        this.loading = false;
      }
    },
    async setTab(id) {
      if (this.swipeLock) return; // 刚滑动过，吞掉随之而来的 click
      // 「我的」和其他栏目一样原地渲染，不再 navTo 跳独立页
      // （原来跳过去后 index 被销毁，左侧导航只剩首页/搜索/我的 —— 用户报的 #9）。
      if (id === 'mine') {
        if (this.tab !== 'mine') {
          this.tab = 'mine';
          this.title = '我的';
          this.errText = '';
          return; // 首次进入：v-if 挂载 mine-body，它自己的 mounted 会 boot()
        }
        // 已经在本栏目：只刷新数据，不重建节点（省掉一次整页重排）
        // $refs 在本运行时不一定可用，组件在 mounted 里额外挂了 $root.__mineBody，做两级回退。
        const b = (this.$refs && this.$refs.body) || (this.$root && this.$root.__mineBody) || this.__mineBody;
        if (b && b.refresh) {
          try { await b.refresh(); } catch (e) { logWarn('[index] mine refresh 失败 ' + ((e && e.message) || e)); }
        }
        return;
      }
      if (id === this.tab && this.hasAny) return; // 重复点当前 tab 不重刷
      this.tab = id;
      const names = { recommend: '推荐', mobile: '手游', pc: '端游', free: '限免', mine: '我的' };
      this.title = names[id] || '推荐';
      // 关键：这里**不清空** banners/modules/list/notice。
      // 旧帧一直显示，新数据到齐后在 loadHome/loadTabList 里一次性替换 +
      // 一次性淡入，只有这样两条加载路径的入场才完全一致（原来的“部分滑入部分直接出现”
      // 就是先清空再异步补数据造成的）。
      this.errText = '';
      // 关键（#10 卡顿）：绝不在点击瞬间置 loading=true。
      // 一旦置了，模板里 v-if="loading && !hasAny" 虽不会命中（有旧内容时 !hasAny 为 false），
      // 但它会让整棵子树参与一次完整的 diff/重建；旧帧保持不动、数据到齐才一次性替换最省。
      this.loadingText = id === 'recommend' ? '正在加载推荐…' : '正在加载游戏列表…';
      if (id === 'recommend') {
        await this.loadHome();
        return;
      }
      await this.loadTabList();
    },
    // ---- 上下滑动切 tab ----
    stepTab(dir) {
      // 「我的」不在 SWIPE_ORDER 里，indexOf 会返回 -1（被当成第 0 个），
      // 不早退的话在「我的」上下滑会莫名跳到「手游」。
      if (this.tab === 'mine') return;
      let i = SWIPE_ORDER.indexOf(this.tab);
      if (i < 0) i = 0;
      let j = i + dir;
      if (j < 0) j = 0;
      if (j > SWIPE_ORDER.length - 1) j = SWIPE_ORDER.length - 1;
      if (j === i) return;
      this.setTab(SWIPE_ORDER[j]);
    },
    // 本机型只认 changedTouches；快速滑动时 touchmove 会丢，落点必须在 touchend 重取
    onBarStart(e) {
      this.barMoved = 0;
      const ts = (e && e.changedTouches) || [];
      if (!ts.length) { this.barY = -1; return; }
      const y = Number(ts[0].pageY);
      this.barY = isFinite(y) ? y : -1;
    },
    onBarMove(e) {
      if (this.barY < 0) return;
      const ts = (e && e.changedTouches) || [];
      if (!ts.length) return;
      const y = Number(ts[0].pageY);
      if (isFinite(y)) this.barMoved = y - this.barY;
    },
    onBarEnd(e) {
      const start = this.barY;
      this.barY = -1;
      if (start < 0) { this.barMoved = 0; return; }
      const ts = (e && e.changedTouches) || [];
      let end = null;
      if (ts.length) {
        const y = Number(ts[0].pageY);
        if (isFinite(y)) end = y;
      }
      if (end === null) end = start + this.barMoved;
      const dy = end - start;
      this.barMoved = 0;
      if (!isFinite(dy) || Math.abs(dy) < 40) return; // 小于阈值当点击
      const self = this;
      this.stepTab(dy < 0 ? 1 : -1); // 上滑 = 下一个，下滑 = 上一个
      // 先切，再上锁：锁只用来吞掉滑动结束后系统补发的 click，不能挡住自己这次的 setTab
      this.swipeLock = true;
      setTimeout(function () { self.swipeLock = false; }, 400);
    },
    tabClass(t) {
      return this.tab === t.id ? 'ih-tab ih-tab-on' : 'ih-tab';
    },
    setNotice(text) {
      this.notice = text || '';
      this.noticeText = this.clipText(text, 58);
    },
    clipText(text, max) {
      const s = String(text || '').replace(/\s+/g, ' ');
      const n = max || 58;
      return s.length > n ? s.slice(0, n) + '…' : s;
    },
    // 按张数平分整行：864 = 884 - 左12 - 右8，每张留 6px 间距
    applyBannerWidth(n) {
      const c = (n > 0 && n < 5) ? n : 3;
      this.bannerW = Math.floor((864 - 6 * c) / c);
      this.bannerCls = 'ih-bn' + c;
      this.imgCls = 'ih-img' + c;
    },
    async loadMore() {
      if (!this.list.length) return;
      this.moreText = '加载中…';
      await this.loadList(this.page + 1, false);
    },
    reload() {
      // 重试时如果还没有任何内容，仍要让首屏骨架回来（loading 平时只由首屏/重试占用）
      if (!this.hasAny) this.loading = true;
      this.loadHome();
    },
    goSearch() {
      navTo('search', {});
    },
    openGame(g) {
      navTo('detail', { code: g.code, name: g.name });
    },
    openSponsor() {
      this.sponsorOn = true;
      logInfo('[index] 打开赞助码');
    },
    closeSponsor() {
      this.sponsorOn = false;
    },
    openBanner(b) {
      if (b.gameCode) {
        navTo('detail', { code: b.gameCode, name: b.gameName });
        return;
      }
      navTo('search', { keyword: b.gameName });
    }
  }
};
</script>

<style lang="less" scoped>
@import "base.less";

.topbar {
  height: 38px;
  flex-direction: row;
  align-items: center;
  padding-left: 12px;
  padding-right: 10px;
}

.topbar-title {
  font-size: 1.7708vw;
  color: @text-color;
  width: 78px;
}

.search-entry {
  height: 26px;
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
  border-radius: 13px;
  background-color: @card-background-color;
  /* 右上角用户名移除后搜索框独占整行。Weex 默认 flex-direction 是 column，
     不显式写 row 的话 justify-content 管的是纵向，文字仍会被横向居中。 */
  flex-direction: row;
  justify-content: flex-start;
  align-items: center;
  padding-left: 12px;
}

.search-hint {
  font-size: 1.3542vw;
  color: @text-color-dim;
}

.body {
  flex-direction: column;
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
  padding-left: 12px;
  padding-right: 8px;
  padding-bottom: 6px;
}

.banner-row {
  flex-direction: row;
  height: 78px;
}

.banner {
  width: 200px;
  height: 76px;
  margin-right: 6px;
  border-radius: 8px;
  overflow: hidden;
}

.banner-img {
  width: 200px;
  height: 76px;
}

.banner-mask {
  height: 20px;
  flex-direction: row;
  align-items: center;
  padding-left: 6px;
  background-color: rgba(0, 0, 0, 0.45);
}

.banner-name {
  font-size: 1.1458vw;
  color: @text-color-strong;
}

.notice-row {
  height: 24px;
  flex-direction: row;
  align-items: center;
  margin-top: 2px;
}

.notice-tag {
  font-size: 1.1458vw;
  color: #ffffff;
  background-color: @accent;
  border-radius: 3px;
  padding-left: 4px;
  padding-right: 4px;
  margin-right: 6px;
}

.notice-text {
  font-size: 1.25vw;
  color: @text-color-dim;
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
  lines: 1;
}

.mod {
  flex-direction: column;
  margin-top: 4px;
}

.mod-head {
  height: 20px;
  flex-direction: row;
  align-items: center;
}

.mod-title {
  font-size: 1.4583vw;
  color: @text-color;
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
}

.mod-count {
  font-size: 1.1458vw;
  color: @text-color-dim;
}

.gcard:active {
  background-color: @ctrl-background;
}

.mod-scroll {
  height: 102px;
}

.mod-row {
  flex-direction: row;
}

/* 卡片 74x98 = padding 4 + 图标 60 + 名字行 15 + 秒玩行 15 + 余 4
   关键：<text> 在 Weex 里 height 常被内容行高顶掉，所以文字一律套一个
   定高 + overflow:hidden 的 div 容器（.gcard-line），由容器裁掉溢出。 */
.gcard {
  width: 74px;
  height: 100px;
  margin-right: 6px;
  border-radius: 8px;
  background-color: @card-background-color;
  flex-direction: column;
  align-items: center;
  padding-top: 4px;
  overflow: hidden;
}

.gcard-icon-wrap {
  width: 56px;
  height: 56px;
  border-radius: 8px;
  overflow: hidden;
}

.gcard-icon {
  width: 56px;
  height: 56px;
  border-radius: 8px;
}

.gcard-line {
  width: 70px;
  height: 13px;
  margin-top: 3px;
  overflow: hidden;
  justify-content: center;
  align-items: center;
}

/* 说明：Weex 的 <text> 默认行高按字号 * 1.3~1.4 算，只给 height 会溢出到卡片外，
   必须显式写 line-height 才能压住。窄卡（74px）中文名只能放 6 个字，靠 lines:1 截断。 */
.gcard-name {
  font-size: 1.0417vw;
  line-height: 1.25vw;
  color: @text-color;
  text-align: center;
  lines: 1;
}

.gcard-btn {
  font-size: 1.0417vw;
  line-height: 1.25vw;
  color: @accent;
  text-align: center;
  lines: 1;
}

.grid {
  flex-direction: row;
  flex-wrap: wrap;
}

.lcard {
  width: 278px;
  height: 64px;
  margin-right: 6px;
  margin-bottom: 6px;
  border-radius: 8px;
  background-color: @card-background-color;
  flex-direction: row;
  align-items: center;
  padding-left: 6px;
}

.lcard-icon-wrap {
  width: 56px;
  height: 56px;
}

.lcard-icon {
  width: 56px;
  height: 56px;
  border-radius: 8px;
}

.lcard-info {
  flex-direction: column;
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
  padding-left: 8px;
  padding-right: 6px;
}

.lcard-name {
  font-size: 1.3542vw;
  color: @text-color;
  lines: 1;
}

.lcard-sum {
  font-size: 1.1458vw;
  color: @text-color-dim;
  lines: 1;
}

.more-row {
  height: 30px;
  flex-direction: row;
  justify-content: center;
  align-items: center;
}

.more-btn {
  height: 26px;
  padding-left: 16px;
  padding-right: 16px;
  border-radius: 13px;
  background-color: @ctrl-background;
  justify-content: center;
  align-items: center;
}

.more-text {
  font-size: 1.25vw;
  color: @text-color;
}

.state-row {
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
  flex-direction: row;
  justify-content: center;
  align-items: center;
}

.state-text {
  font-size: 1.4583vw;
  color: @text-color-dim;
}

.retry-btn {
  height: 26px;
  margin-left: 10px;
  padding-left: 14px;
  padding-right: 14px;
  border-radius: 13px;
  background-color: @accent;
  justify-content: center;
  align-items: center;
}

.retry-text {
  font-size: 1.25vw;
  color: #ffffff;
}

/* ================= 首页 UI 修复新增（ih- 前缀，避免与其它页全局合并时撞名） =================
   Weex 只支持单类名选择器，所以下面全部是独立类名，靠模板里拼字符串叠加。
   6 个 tab 原来的 h33+mt4 总高 276 会溢出 266 的栏（第 6 个被裁），改成 31+3 = 258。 */

.ih-tab {
  width: 64px;
  height: 31px;
  margin-top: 3px;
  border-radius: 8px;
  justify-content: center;
  align-items: center;
  background-color: @background-color;
}

.ih-tab-on {
  background-color: @accent;
}

.ih-tab-text {
  font-size: 1.4583vw;
  color: @text-color-dim;
  text-align: center;
}

.ih-tab-text-on {
  color: #FFFFFF;
}

/* 统一入场：每次内容整体替换时 :key 变化 → 重新挂载 → 动画重放一次。
   两条加载路径（loadHome / loadTabList）都在提交后 fadeKey+1，所以入场完全一致。 */
@keyframes ihFadeIn {
  from {
    opacity: 0;
    transform: translateX(8px);
  }
  to {
    opacity: 1;
    transform: translateX(0);
  }
}

.ih-fade {
  animation-name: ihFadeIn;
  animation-duration: 220ms;
  animation-timing-function: ease-out;
  animation-fill-mode: forwards;
}

/* 有旧帧时不再整页替换，错误以小条形式浮在内容顶部 */
.ih-err {
  height: 24px;
  flex-direction: row;
  align-items: center;
  justify-content: center;
  background-color: @card-background-color;
  border-radius: 6px;
  margin-bottom: 4px;
}

/* ---------------- banner 平分整行 ---------------- */
.ih-banner {
  height: 76px;
  margin-right: 6px;
  border-radius: 8px;
  overflow: hidden;
}

.ih-banner-img {
  height: 76px;
  border-radius: 8px;
}

.ih-bn1 { width: 858px; }
.ih-bn2 { width: 426px; }
.ih-bn3 { width: 282px; }
.ih-bn4 { width: 210px; }

.ih-img1 { width: 858px; }
.ih-img2 { width: 426px; }
.ih-img3 { width: 282px; }
.ih-img4 { width: 210px; }

/* ---------------- 公告条：定高 + overflow 裁切，文本已在 JS 里硬截断 ---------------- */
.ih-notice-row {
  height: 24px;
  flex-direction: row;
  align-items: center;
  margin-top: 2px;
}

.ih-notice-tag {
  font-size: 1.1458vw;
  color: #FFFFFF;
  background-color: @accent;
  border-radius: 3px;
  padding-left: 4px;
  padding-right: 4px;
  margin-right: 6px;
}

.ih-notice-box {
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0;
  height: 18px;
  overflow: hidden;
}

.ih-notice-text {
  font-size: 1.25vw;
  line-height: 1.875vw;
  color: @text-color-dim;
}


/* 「我的」原地栏目容器：接在 topbar 下方，吃掉剩余高度。
   用 flex 三连而不是写死 266 —— topbar 占掉 38，写死会把内容顶出屏幕。 */
.ih-mine-wrap {
  width: 100%;
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
  flex-direction: column;
  overflow: hidden;
}
/* ---------------- 赞助公告（置顶第一位，点击展开赞赏码） ----------------
   布局说明：整条高度 30px（比系统公告的 24px 高一档，视觉上排在第一位），
   从左到右 = 金色「赞助」标签 + 34px 赞赏码缩略图 + 一行文案 + 右侧「查看」。
   缩略图直接复用 mine-body 里那张 320x320 的 assets，运行时缩到 34px，
   不额外增加包体。整块可点，点击后由 sponsorOn 分支整块替换内容区。
   ★ 不要给这里加 position:absolute / z-index：本运行时禁用绝对定位。 */
.ih-spon-row {
  height: 40px;
  flex-direction: row;
  align-items: center;
  margin-top: 3px;
  padding-left: 6px;
  padding-right: 6px;
  border-radius: 6px;
  background-color: @card-background-color;
  border-width: 1px;
  border-color: @gold;
}

.ih-spon-tag {
  font-size: 1.1458vw;
  color: #1E2831;
  background-color: @gold;
  border-radius: 3px;
  padding-left: 4px;
  padding-right: 4px;
  margin-right: 6px;
}

.ih-spon-thumb-wrap {
  width: 32px;
  height: 32px;
  align-items: center;
  justify-content: center;
  margin-right: 8px;
}

.ih-spon-thumb {
  width: 32px;
  height: 32px;
  border-radius: 4px;
  background-color: #FFFFFF;
}

.ih-spon-box {
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0;
  height: 18px;
  overflow: hidden;
}

.ih-spon-text {
  font-size: 1.25vw;
  line-height: 1.875vw;
  color: @text-color;
}

.ih-spon-more {
  width: 46px;
  height: 20px;
  border-radius: 10px;
  background-color: @accent;
  align-items: center;
  justify-content: center;
  margin-left: 6px;
}

.ih-spon-more-text {
  font-size: 1.1458vw;
  color: #FFFFFF;
}

/* 赞助码整块页：左图右字，垂直居中，吃掉 topbar 以下的全部高度 */
.ih-spon-full {
  width: 100%;
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
  flex-direction: row;
  justify-content: center;
  align-items: center;
}

.ih-spon-full-img {
  width: 200px;
  height: 200px;
  border-radius: 8px;
  background-color: #FFFFFF;
}

.ih-spon-full-side {
  flex-direction: column;
  justify-content: center;
  margin-left: 22px;
  width: 300px;
}

.ih-spon-full-title {
  font-size: 1.5625vw;
  color: @text-color-strong;
}

.ih-spon-full-desc {
  font-size: 1.25vw;
  color: @text-color;
  margin-top: 10px;
}

.ih-spon-full-hint {
  font-size: 1.1458vw;
  color: @accent-blue;
  margin-top: 10px;
}

/* 留言提示：金色，和「赞助」标签同色系；比 hint 低一档字号 */
.ih-spon-full-leave {
  font-size: 1.1458vw;
  line-height: 1.6667vw;
  color: @gold;
  margin-top: 8px;
}

.ih-spon-full-btn {
  width: 96px;
  height: 32px;
  border-radius: 8px;
  background-color: @ctrl-background;
  justify-content: center;
  align-items: center;
  margin-top: 16px;
}

.ih-spon-full-btn-text {
  font-size: 1.3542vw;
  color: @text-color;
}
</style>
