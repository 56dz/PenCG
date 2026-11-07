<template>
  <div class="page">
    <!-- 左侧 76px 竖导航 -->
    <div class="bar">
      <div class="brand">
        <image class="brand-logo" :src="logo" :width="26" :height="26"></image>
      </div>
      <div v-for="t in tabs" :key="t.id" :class="tab === t.id ? 'tab tab-on' : 'tab'" @click="setTab(t.id)">
        <text :class="tab === t.id ? 'tab-text tab-text-on' : 'tab-text'">{{ t.label }}</text>
      </div>
    </div>

    <div class="content">
      <!-- 顶部条 38px：返回 + 输入框 + 搜索 -->
      <div class="topbar">
        <div class="back-btn" @click="goBack"><text class="back-t">返回</text></div>
        <div class="search-box" @click="openKb">
          <div class="search-txt-wrap">
            <text :class="kw ? 'search-txt' : 'search-txt search-ph'">{{ kw ? short(kw, 28) : '输入游戏名搜索' }}</text>
          </div>
        </div>
        <div class="go-btn" @click="submit"><text class="go-t">搜索</text></div>
      </div>

      <div v-if="kbHint" class="hint-row"><text class="hint-t">{{ kbHint }}</text></div>

      <!-- 未搜索：热门 + 历史 -->
      <scroller v-if="!searched" class="body">
        <div class="sec">
          <div class="sec-head">
            <text class="sec-title">热门搜索</text>
            <text class="sec-sub">{{ hot.length }} 款</text>
          </div>
          <div class="chip-wrap">
            <div v-for="(h, i) in hot" :key="'h' + i" class="chip" @click="searchHot(h)">
              <text class="chip-t">{{ short(h.name, 10) }}</text>
            </div>
            <text v-if="!hot.length" class="empty-text">热门词加载中…</text>
          </div>
        </div>

        <div class="sec">
          <div class="sec-head">
            <text class="sec-title">搜索历史</text>
            <div v-if="hist.length" class="clear-btn" @click="clearHist"><text class="clear-t">清空</text></div>
          </div>
          <div class="chip-wrap">
            <div v-for="(k, i) in hist" :key="'s' + i" class="chip chip-hist" @click="searchWord(k)">
              <text class="chip-t">{{ short(k, 10) }}</text>
            </div>
            <text v-if="!hist.length" class="empty-text">还没有搜索记录</text>
          </div>
        </div>

        <div class="sec">
          <div class="sec-head">
            <text class="sec-title">大家都在玩</text>
            <text class="sec-sub">{{ guess.length }} 款</text>
          </div>
          <div class="grid">
            <div v-for="g in guess" :key="'g' + g.code" class="lcard" @click="openGame(g)">
              <div class="lcard-icon-wrap"><image class="lcard-icon" :src="g.icon" :width="56" :height="56"></image></div>
              <div class="lcard-info">
                <div class="lcard-line"><text class="lcard-name">{{ short(g.name, 13) }}</text></div>
                <div class="lcard-line"><text class="lcard-sum">{{ short(g.sum || '热门手游', 17) }}</text></div>
              </div>
            </div>
          </div>
        </div>
      </scroller>

      <!-- 已搜索 -->
      <div v-else class="content-body">
        <div v-if="loading" class="state-row"><text class="state-text">{{ loadingText }}</text></div>
        <div v-else-if="errText" class="state-row" @click="retry">
          <text class="state-text">{{ errText }}</text>
          <div class="retry-btn"><text class="retry-text">重试</text></div>
        </div>
        <template v-else>
          <div class="sec-head sec-head-flat">
            <div class="sec-title-wrap"><text class="sec-title">「{{ short(kw, 20) }}」的结果</text></div>
            <text class="sec-sub">共 {{ total }} 款</text>
          </div>
          <scroller v-if="results.length" class="body">
            <div class="grid">
              <div v-for="g in results" :key="'r' + g.code" class="lcard" @click="openGame(g)">
                <div class="lcard-icon-wrap"><image class="lcard-icon" :src="g.icon" :width="56" :height="56"></image></div>
                <div class="lcard-info">
                  <div class="lcard-line"><text class="lcard-name">{{ short(g.name, 13) }}</text></div>
                  <div class="lcard-line"><text class="lcard-sum">{{ short(g.sum || '点击查看详情', 17) }}</text></div>
                </div>
              </div>
            </div>
            <div class="more-row">
              <div class="more-btn" @click="loadMore"><text class="more-text">{{ moreText }}</text></div>
            </div>
          </scroller>
          <div v-else class="state-row">
            <text class="state-text">没有找到「{{ short(kw, 16) }}」，换个词试试</text>
            <div v-if="hotCode" class="retry-btn" @click="openHotGame"><text class="retry-text">直接打开</text></div>
            <div class="retry-btn retry-btn-gray" @click="resetSearch"><text class="retry-text">返回热门</text></div>
          </div>
        </template>
      </div>
    </div>
  </div>
</template>

<script>
import { v1 } from '../../services/cg.js';
import { imgUrl } from '../../services/img.js';
import { loadHotSearches, loadGameList } from '../../services/catalog.js';
import { navTo, navBack, peekParams, setPendingTab } from '../../services/nav.js';
import { getJSON, setJSON } from '../../services/storage.js';
import { logInfo, logWarn } from '../../services/log.js';
import { ImSession } from '../../services/im.js';
import logoImg from '../../assets/logo.png';

const TABS = [
  { id: 'recommend', label: '推荐' },
  { id: 'mobile', label: '手游' },
  { id: 'pc', label: '端游' },
  { id: 'free', label: '限免' },
  /* 「搜索」不再是栏目（首页每栏顶部都有搜索框），入口只留顶栏，见用户反馈 #3 */
  { id: 'mine', label: '我的' }
];

const HIST_KEY = 'pencg_search_hist';
const HIST_MAX = 10;

// 服务端搜索真实生效的参数是 name（keyword 被忽略），这里自己归一化卡片
function pickGame(g) {
  if (!g) return null;
  const d = g.display_game || g;
  const code = d.game_code || g.game_code || '';
  if (!code) return null;
  const tags = d.tags || d.recommend_tags || g.recommend_tags || [];
  return {
    code,
    name: d.name || d.game_name || g.game_name || code,
    icon: imgUrl(d.icon || d.recommend_small_icon || g.icon || ''),
    sum: d.summary || (tags.length ? tags.slice(0, 2).join(' · ') : '')
  };
}

async function queryGames(name, page) {
  // 不传 game_type：服务端会把「手游/端游」分开过滤，很多游戏（如影之刃零）
  // 只有 PC 版，一旦写死 mobile 就永远 0 结果。
  const j = await v1('api/v1/games', {
    page: page || 0,
    per_page: 21,
    src: 4,
    name: name
  });
  const list = Array.isArray(j) ? j : (j && j.games) || [];
  const items = [];
  const seen = {};
  for (const g of list) {
    const it = pickGame(g);
    if (it && !seen[it.code]) {
      seen[it.code] = 1;
      items.push(it);
    }
  }
  return { total: (j && j.total) || items.length, items };
}

// 热门词常是运营长标题（「影之刃零（steam高配）-每日抽送游戏cdk&云币」），
// 整串丢给 name 参数一定 0 结果 → 退化成取第一个分隔符前的游戏名再搜一次。
function simplifyWord(w) {
  const src = String(w || '');
  // 先把中文书名号/方头括号统一成圆括号，避免 '【新】XX' 切出「】XX」
  const norm = src.replace(/[【「『]/g, '（').replace(/[】」』]/g, '）');
  const parts = norm.split(/[（()）·\-—–、&,，:：/\[\]]/);
  for (const p of parts) {
    const t = p.trim();
    if (t.length >= 2 && t !== src) return t;
  }
  return '';
}

async function searchByName(name, page) {
  const r = await queryGames(name, page);
  if (r.items.length || page) return r;
  const simple = simplifyWord(name);
  if (!simple) return r;
  try {
    const r2 = await queryGames(simple, page);
    if (r2.items.length) {
      logInfo('[search] 「' + name + '」无结果，回落为「' + simple + '」→ ' + r2.items.length);
      return r2;
    }
  } catch (e) {
    /* 回落失败就用原结果 */
  }
  return r;
}

export default {
  data() {
    return {
      logo: logoImg,
      tabs: TABS,
      tab: 'search',
      kw: '',
      kbHint: '',
      searched: false,
      loading: false,
      loadingText: '正在搜索…',
      errText: '',
      results: [],
      total: 0,
      page: 0,
      moreText: '加载更多',
      hot: [],
      hist: [],
      guess: [],
      hotCode: '',
      busy: false,
      imBusy: false
    };
  },
  mounted() {
    this.im = new ImSession();
    this.boot();
  },
  beforeDestroy() {
    try {
      if (this.im) this.im.dispose();
    } catch (e) {
      /* ignore */
    }
  },
  methods: {
    async boot() {
      const p = peekParams('search') || {};
      await this.loadHist();
      this.loadHot();
      this.loadGuess();
      if (p.keyword) {
        this.kw = String(p.keyword);
        await this.submit();
      }
    },
    async loadHot() {
      try {
        const list = await loadHotSearches();
        // 热门词是运营长标题（带括号/破折号/促销语），chip 上显示简化后的游戏名，
        // 原始标题留作 raw 备查。
        this.hot = (list || []).slice(0, 12).map((h) => ({
          name: simplifyWord(h.name) || h.name,
          raw: h.name,
          code: h.code || ''
        }));
      } catch (e) {
        this.hot = [];
      }
    },
    async loadGuess() {
      try {
        const r = await loadGameList({ page: 0, perPage: 12, gameType: 'mobile' });
        this.guess = (r.items || []).slice(0, 6).map((g) => ({
          code: g.code,
          name: g.name,
          icon: g.icon,
          sum: g.summary || (g.tags || []).join(' · ')
        }));
      } catch (e) {
        this.guess = [];
      }
    },
    async loadHist() {
      try {
        const list = await getJSON(HIST_KEY, []);
        this.hist = Array.isArray(list) ? list.slice(0, HIST_MAX) : [];
      } catch (e) {
        this.hist = [];
      }
    },
    async pushHist(k) {
      const w = String(k || '').trim();
      if (!w) return;
      const rest = this.hist.filter((x) => x !== w);
      rest.unshift(w);
      this.hist = rest.slice(0, HIST_MAX);
      try {
        await setJSON(HIST_KEY, this.hist);
      } catch (e) {
        /* ignore */
      }
    },
    async clearHist() {
      this.hist = [];
      try {
        await setJSON(HIST_KEY, []);
      } catch (e) {
        /* ignore */
      }
    },
    async openKb() {
      if (this.imBusy) return;
      this.kbHint = '';
      this.imBusy = true;
      try {
        const r = await this.im.open({ value: this.kw, placeholder: '输入游戏名', maxlength: 20 });
        if (r && r.error) {
          this.kbHint = '系统输入法不可用，请直接点下方热门词';
          return;
        }
        if (r && typeof r.text === 'string' && r.text) {
          this.kw = r.text;
          this.hotCode = '';   // 手输的词和热搜词无关
          if (r.confirmed) await this.submit();
        }
      } catch (e) {
        this.kbHint = '输入法调用失败，请点热门词';
      } finally {
        this.imBusy = false;
      }
    },
    short(s, n) {
      const t = String(s === undefined || s === null ? '' : s).replace(/[\r\n\t]/g, ' ').trim();
      return t.length > n ? t.slice(0, n) + '…' : t;
    },
    searchHot(h) {
      if (!h || !h.name) return;
      // 个别热搜词在游戏库里查不到（如「和平精英」total=0），但它的 game_code 一定有效
      // → 记住 code，结果为空时给一个「直接打开」的出口。
      this.kw = String(h.name);
      this.hotCode = h.code || '';
      this.submit();
    },
    searchWord(k) {
      this.kw = String(k || '');
      // 手动输入 / 点历史词时，之前热搜的 code 不再适用
      this.hotCode = '';
      this.submit();
    },
    openHotGame() {
      if (!this.hotCode) return;
      navTo('detail', { code: this.hotCode, name: this.kw });
    },
    submit() {
      const w = String(this.kw || '').trim();
      if (!w) {
        this.kbHint = '请输入游戏名或关键词';
        return;
      }
      this.kbHint = '';
      this.searched = true;
      this.pushHist(w);
      this.runSearch(0, true);
    },
    retry() {
      this.runSearch(0, true);
    },
    resetSearch() {
      this.searched = false;
      this.errText = '';
      this.results = [];
      this.kw = '';
      this.hotCode = '';
    },
    async runSearch(page, reset) {
      if (this.busy) return;
      this.busy = true;
      this.loading = reset;
      this.errText = '';
      this.loadingText = '正在搜索「' + this.short(this.kw, 14) + '」…';
      try {
        const r = await searchByName(this.kw, page || 0);
        if (reset) this.results = r.items;
        else this.results = this.results.concat(r.items);
        this.total = r.total;
        this.page = page || 0;
        this.moreText = this.results.length < this.total ? '加载更多' : '已经到底了';
        logInfo('[search] "' + this.kw + '" → ' + r.items.length + '/' + r.total);
      } catch (e) {
        this.errText = this.short('搜索失败：' + ((e && e.message) || e), 34);
        logWarn('[search] 失败 ' + this.kw + ' ' + ((e && e.message) || e));
      } finally {
        this.loading = false;
        this.busy = false;
      }
    },
    loadMore() {
      if (!this.results.length || this.results.length >= this.total) return;
      this.moreText = '加载中…';
      this.runSearch(this.page + 1, false);
    },
    goBack() {
      navBack();
    },
    setTab(id) {
      if (id === 'search') return;
      /* 用户反馈 2：不能 navTo('mine') —— 那会压一个新的 mine 薄壳页，
       * 它的顶栏只有「首页 / 我的」两项，左侧导航看起来就塌了。
       * 正确做法是退回 index 再让它切到「我的」栏目。 */
      if (id === 'mine') {
        try { setPendingTab('mine'); } catch (e) {}
        if (!navBack()) navTo('index', { tab: 'mine' });
        return;
      }
      navBack();
    },
    openGame(g) {
      if (!g || !g.code) return;
      navTo('detail', { code: g.code, name: g.name });
    }
  }
};
</script>

<style lang="less" scoped>
@import "base.less";



/* ---------- 顶部条 ---------- */
.topbar {
  height: 38px;
  flex-direction: row;
  align-items: center;
  padding-left: 10px;
  padding-right: 10px;
}

.back-btn {
  height: 24px;
  padding-left: 10px;
  padding-right: 10px;
  border-radius: 12px;
  background-color: @ctrl-background;
  justify-content: center;
  align-items: center;
}

.back-t {
  font-size: 12px;
  color: @text-color;
}

.search-box {
  height: 26px;
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
  margin-left: 8px;
  margin-right: 8px;
  border-radius: 13px;
  background-color: @card-background-color;
  justify-content: center;
  padding-left: 12px;
  padding-right: 8px;
}

.search-txt {
  font-size: 13px;
  color: @text-color-strong;
  lines: 1;
}

.search-ph {
  color: @text-color-dim;
}

.go-btn {
  height: 26px;
  padding-left: 16px;
  padding-right: 16px;
  border-radius: 13px;
  background-color: @accent;
  justify-content: center;
  align-items: center;
}

.go-t {
  font-size: 13px;
  color: @btn-text-on-accent;
}

.hint-row {
  height: 18px;
  overflow: hidden;
  flex-direction: row;
  align-items: center;
  padding-left: 12px;
  padding-right: 12px;
}

.hint-t {
  font-size: 11px;
  color: @accent-blue;
  lines: 1;
}

/* ---------- 主体 ---------- */
.body {
  flex-direction: column;
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
  padding-left: 12px;
  padding-right: 8px;
  padding-bottom: 6px;
}

.content-body {
  flex-direction: column;
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
  padding-left: 12px;
  padding-right: 8px;
}

.sec {
  flex-direction: column;
  margin-bottom: 2px;
}

.sec-head {
  height: 20px;
  flex-direction: row;
  align-items: center;
  margin-top: 2px;
}

/* 结果页标题：父级 .content-body 已有 12 左边距，这里不能再加，
   否则标题比下方卡片网格多缩进 12px */
.sec-head-flat {
  margin-top: 0;
  height: 22px;
}

.sec-title-wrap {
  height: 20px;
  overflow: hidden;
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
}

.sec-title {
  font-size: 13px;
  color: @text-color-strong;
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
  lines: 1;
}

.sec-sub {
  font-size: 11px;
  color: @text-color-dim;
}

.chip-wrap {
  flex-direction: row;
  flex-wrap: wrap;
  padding-bottom: 2px;
}

.chip {
  height: 22px;
  max-width: 132px;
  overflow: hidden;
  padding-left: 10px;
  padding-right: 10px;
  margin-right: 6px;
  margin-bottom: 5px;
  border-radius: 11px;
  background-color: @card-background-color;
  justify-content: center;
  align-items: center;
}

.chip-hist {
  background-color: @ctrl-background;
}

.chip-t {
  font-size: 11px;
  color: @text-color;
  lines: 1;
}

.empty-text {
  font-size: 11px;
  color: @text-color-dim;
}

.clear-btn {
  height: 18px;
  padding-left: 8px;
  padding-right: 8px;
  border-radius: 9px;
  background-color: @ctrl-background;
  justify-content: center;
  align-items: center;
}

.clear-t {
  font-size: 10px;
  color: @text-color-dim;
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

.search-txt-wrap {
  height: 20px;
  overflow: hidden;
  justify-content: center;
}

.lcard-line {
  height: 17px;
  overflow: hidden;
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
  font-size: 13px;
  color: @text-color;
  lines: 1;
}

.lcard-sum {
  font-size: 11px;
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
  font-size: 12px;
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
  font-size: 13px;
  color: @text-color-dim;
  lines: 1;
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

.retry-btn-gray {
  background-color: @ctrl-background;
}

.retry-text {
  font-size: 12px;
  color: @btn-text-on-accent;
}
</style>
