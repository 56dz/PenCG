<template>
  <div class="page">
    <!-- 左侧 76px 竖导航（与首页同一套 tabs） -->
    <div class="bar">
      <div class="brand">
        <image class="brand-logo" :src="logo" :width="26" :height="26"></image>
      </div>
      <div v-for="t in tabs" :key="t.id" :class="tab === t.id ? 'tab tab-on' : 'tab'" @click="setTab(t.id)">
        <text :class="tab === t.id ? 'tab-text tab-text-on' : 'tab-text'">{{ t.label }}</text>
      </div>
    </div>

    <!-- 右侧 884px -->
    <div class="content">
      <!-- 顶部条 38px：返回 + 游戏名 -->
      <div class="topbar">
        <div class="back-btn" @click="goBack"><text class="back-t">返回</text></div>
        <div class="topbar-title-wrap">
          <text class="topbar-title">{{ clip(title, 16) }}</text>
        </div>
        <div class="topbar-sub-wrap">
          <text class="topbar-sub">{{ subTitle }}</text>
        </div>
      </div>

      <div v-if="loading" class="state-row">
        <text class="state-text">{{ loadingText }}</text>
      </div>

      <div v-else-if="errText" class="state-row" @click="reload">
        <text class="state-text">{{ errText }}</text>
        <div class="retry-btn"><text class="retry-text">重试</text></div>
      </div>

      <div v-else class="body">
        <!-- 左列：图标 + 名称 + 标签 + 三个数据卡 + 两个按钮 -->
        <div class="col-left">
          <div class="hero">
            <image class="hero-icon" :src="game.icon" :width="96" :height="96"></image>
            <div class="hero-info">
              <div class="line-17"><text class="hero-name">{{ clip(game.name, 20) }}</text></div>
              <div class="line-17"><text class="hero-sum">{{ clip(summaryText, 26) }}</text></div>
              <div class="chip-row">
                <div v-for="b in badgeList" :key="'bd' + b" class="chip chip-on"><text class="chip-t chip-t-on">{{ b }}</text></div>
                <div v-for="t in tagList" :key="'tg' + t" class="chip"><text class="chip-t">{{ clip(t, 8) }}</text></div>
              </div>
            </div>
          </div>

          <div class="stats">
            <div class="stat">
              <text class="stat-v">{{ coinText }}</text>
              <text class="stat-k">云币 / 分钟</text>
            </div>
            <div class="stat">
              <text class="stat-v">{{ freeText }}</text>
              <text class="stat-k">每日免费</text>
            </div>
            <div class="stat">
              <text class="stat-v">{{ regionText }}</text>
              <text class="stat-k">可用区</text>
            </div>
          </div>

          <div class="actions">
            <div class="btn btn-main" @click="startPlay"><text class="btn-text btn-text-on">开始游戏</text></div>
            <div class="btn" @click="addHistory"><text class="btn-text">加入历史</text></div>
            <text v-if="actHint" class="act-hint">{{ actHint }}</text>
          </div>

          <div v-if="histText" class="hist-line">
            <text class="hist-t">{{ histText }}</text>
          </div>
        </div>

        <!-- 右列：画质 + 简介 -->
        <div class="col-right">
          <div class="q-row">
            <text class="q-label">画质</text>
            <div v-for="q in qualityList" :key="'q' + q.id" :class="quality === q.id ? 'q-chip q-chip-on' : 'q-chip'" @click="pickQuality(q.id)">
              <text :class="quality === q.id ? 'q-t q-t-on' : 'q-t'">{{ q.label }}</text>
            </div>
          </div>
          <div class="sec-head">
            <text class="sec-title">游戏简介</text>
            <text class="sec-sub">{{ descCount }}</text>
          </div>
          <scroller class="desc-scroll">
            <text v-for="(ln, i) in descLines" :key="'dl' + i" class="desc-line">{{ ln }}</text>
            <text v-if="!descLines.length" class="desc-empty">暂无简介</text>
          </scroller>
        </div>
      </div>
    </div>
  </div>
</template>

<script>
import { loadGameDetail, loadGameList } from '../../services/catalog.js';
import { v1 } from '../../services/cg.js';
import { recordPlay, listPlays } from '../../services/history.js';
import { loadSettings, saveSettings } from '../../services/settings.js';
import { navTo, navBack, peekParams, setPendingTab } from '../../services/nav.js';
import { QUALITIES, QUALITY_LABEL, normQuality } from '../../services/const.js';
import { logInfo, logWarn } from '../../services/log.js';
import logoImg from '../../assets/logo.png';

const TABS = [
  { id: 'recommend', label: '推荐' },
  { id: 'mobile', label: '手游' },
  { id: 'pc', label: '端游' },
  { id: 'free', label: '限免' },
  /* 「搜索」不再是栏目（首页每栏顶部都有搜索框），入口只留顶栏，见用户反馈 #3 */
  { id: 'mine', label: '我的' }
];

// 每行字数：正文 12px，右列约 420px 宽 → 46 字安全
const DESC_PER_LINE = 34;
const DESC_MAX_SEG = 16;
// 少数游戏（如 clx 一梦江湖）才有 description 正文，无参数预览时优先展示它们
const DEMO_CODES = ['bh3', 'clx'];

export default {
  data() {
    return {
      logo: logoImg,
      tabs: TABS,
      tab: '',
      code: '',
      title: '游戏详情',
      subTitle: '',
      loading: true,
      loadingText: '正在加载详情…',
      errText: '',
      game: { name: '', icon: '', summary: '', tags: [], desc: '', regions: [] },
      summaryText: '',
      tagList: [],
      badgeList: [],
      descLines: [],
      descCount: '',
      coinText: '-',
      freeText: '-',
      regionText: '-',
      quality: normQuality('low'),
      qualityList: QUALITIES.map((id) => ({ id, label: QUALITY_LABEL[id] || id })),
      actHint: '',
      histText: '',
      fallback: false
    };
  },
  mounted() {
    this.boot();
  },
  methods: {
    async boot() {
      const p = peekParams('detail') || {};
      this.code = p.code || p.gameCode || '';
      if (p.name) {
        this.title = p.name;
        this.subTitle = p.name;
      }
      try {
        const s = await loadSettings();
        if (s && s.quality) this.quality = normQuality(s.quality);
      } catch (e) {
        /* ignore */
      }
      if (!this.code) {
        // 无参数直接进入（真机调试/深链兜底）：拉一款热门游戏当示例
        this.fallback = true;
        await this.loadFallback();
        return;
      }
      await this.fetch();
    },
    reload() {
      if (!this.code) {
        this.loadFallback();
        return;
      }
      this.fetch();
    },
    async loadFallback() {
      this.loading = true;
      this.errText = '';
      this.loadingText = '正在挑选示例游戏…';
      try {
        const r = await loadGameList({ page: 0, perPage: 8, gameType: 'mobile' });
        const items = (r.items || []).slice(0, 4);
        if (!items.length) {
          this.errText = '没有可展示的游戏';
          return;
        }
        // 候选顺序：先试自带长简介的样例游戏（多数游戏 description 为空数组），
        // 再退回榜单前几款，保证简介区也能看到内容。
        const codes = [];
        for (let i = 0; i < DEMO_CODES.length; i++) codes.push(DEMO_CODES[i]);
        for (let i = 0; i < items.length; i++) codes.push(items[i].code);
        let first = null;
        let withDesc = null;
        for (let i = 0; i < codes.length; i++) {
          const code = codes[i];
          if (!code) continue;
          try {
            const one = await this.fetchOne(code);
            if (!first) first = { code: code, one: one };
            const lines = this.buildDesc(this.rawDesc(one.raw) || one.g.desc || '');
            if (lines.length >= 3) {
              withDesc = { code: code, one: one };
              break;
            }
          } catch (e) {
            /* 跳过失败项 */
          }
        }
        const picked = withDesc || first;
        if (!picked) {
          this.errText = '没有可展示的游戏';
          return;
        }
        this.code = picked.code;
        this.apply(picked.one);
      } catch (e) {
        this.errText = this.clip('加载失败：' + ((e && e.message) || e), 34);
      } finally {
        this.loading = false;
      }
    },
    async fetch() {
      this.loading = true;
      this.errText = '';
      this.loadingText = '正在加载详情…';
      try {
        const r = await this.fetchOne(this.code);
        this.apply(r);
        logInfo('[detail] 载入 ' + this.code + ' 简介行 ' + this.descLines.length);
      } catch (e) {
        this.errText = this.clip('加载失败：' + ((e && e.message) || e), 34);
        logWarn('[detail] 详情失败 ' + this.code + ' ' + ((e && e.message) || e));
      } finally {
        this.loading = false;
      }
    },
    // services 的 loadGameDetail 把 description 数组直接 join('\n')，
    // 而该字段实际是 [{title,con}] 对象数组 → 会拼出 "[object Object]"。
    // 这里并行再取一次原始 JSON，自己按 title/con 拼成可读文本（不改 services）。
    async fetchOne(code) {
      const [g, raw] = await Promise.all([
        loadGameDetail(code),
        v1('api/v1/games/' + encodeURIComponent(code)).catch(() => null)
      ]);
      return { g: g, raw: raw };
    },
    rawDesc(raw) {
      if (!raw) return '';
      const d = raw.description;
      if (Array.isArray(d)) {
        const parts = [];
        for (let i = 0; i < d.length; i++) {
          const seg = d[i];
          if (!seg || typeof seg !== 'object') {
            if (typeof seg === 'string' && seg) parts.push(seg);
            continue;
          }
          const title = seg.title || '';
          const con = String(seg.con || '').trim();
          if (!con) continue;
          parts.push(title + con);
        }
        return parts.join('\n');
      }
      return typeof d === 'string' ? d : '';
    },
    apply(r) {
      const g = r.g;
      this.game = g;
      this.title = g.name || this.code;
      this.summaryText = this.clip(g.summary || (g.tags || []).join(' · ') || '', 24);
      const sub = g.summary || '';
      this.subTitle = this.fallback ? this.clip(sub, 30) + ' · 示例' : this.clip(sub, 36);
      this.tagList = this.cleanTags(g.tags).slice(0, 4);
      // 状态徽标：数据里已经有，但原来没展示（free / canHangUp / supportGamepad）
      const badges = [];
      if (g.free) badges.push('限免');
      if (g.canHangUp) badges.push('可挂机');
      if (g.supportGamepad) badges.push('支持手柄');
      this.badgeList = badges.slice(0, 2);
      if (this.badgeList.length) this.tagList = this.tagList.slice(0, 3);
      this.coinText = g.coinsPerMinute ? String(g.coinsPerMinute) : '免费';
      this.freeText = g.freeDaily ? Math.round(g.freeDaily / 60) + ' 分钟' : '无';
      this.regionText = (g.regions || []).length ? String(g.regions.length) : '-';
      const text = this.rawDesc(r.raw) || g.desc || '';
      let lines = this.buildDesc(text);
      if (!lines.length) {
        // 实测 988 款游戏里只有极少数有 description 正文（bh3 / clx），
        // 其余全空。为空时用已有字段拼一段真实信息，不要给用户一个空框。
        lines = this.buildDesc(this.fakeDesc(g));
      }
      this.descLines = lines;
      this.descCount = this.descLines.length ? this.descLines.length + ' 行' : '';
      this.loadHist(this.code);
    },
    // 游玩历史：listPlays() 里找当前 code，自己算「X 天前 · 累计 N 分钟」
    async loadHist(code) {
      this.histText = '';
      if (!code) return;
      try {
        const list = await listPlays();
        const it = (list || []).find((x) => x.code === code);
        if (!it) return;
        const mins = Math.round((it.seconds || 0) / 60);
        this.histText = '上次玩 ' + this.agoText(it.at) + ' · 累计 ' + mins + ' 分钟';
      } catch (e) {
        this.histText = '';
      }
    },
    agoText(at) {
      const t = Number(at) || 0;
      if (!t) return '未知时间';
      const diff = Date.now() - t;
      if (diff < 0) return '刚刚';
      const min = Math.floor(diff / 60000);
      if (min < 1) return '刚刚';
      if (min < 60) return min + ' 分钟前';
      const hour = Math.floor(min / 60);
      if (hour < 24) return hour + ' 小时前';
      const day = Math.floor(hour / 24);
      if (day < 30) return day + ' 天前';
      return Math.floor(day / 30) + ' 个月前';
    },
    clip(s, n) {
      const t = String(s === undefined || s === null ? '' : s).replace(/[\r\n\t]/g, ' ').trim();
      return t.length > n ? t.slice(0, n) + '…' : t;
    },
    // 标签清洗：接口里混着 pchigh / net ease / jj 这类内部标记，只留可读短词
    cleanTags(list) {
      const seen = {};
      const out = [];
      for (const raw of (list || [])) {
        const t = String(raw || '').trim();
        if (!/^[\u4e00-\u9fa5A-Za-z0-9]{2,6}$/.test(t)) continue;
        if (/^(pchigh|pclow|mobile|pc|net|ease|jj|hot|new|hd|sdk)$/i.test(t)) continue;
        if (seen[t]) continue;
        seen[t] = 1;
        out.push(t);
      }
      return out;
    },
    // 无官方简介时的兜底文案：全部来自真实字段，不编造
    fakeDesc(g) {
      const parts = [];
      const sum = String(g.summary || '').trim();
      if (sum) parts.push(sum + '。');
      const tags = this.cleanTags(g.tags);
      // 中文标签优先（内部标记多半是纯英文短码）
      tags.sort((a, b) => (/[\u4e00-\u9fa5]/.test(a) ? 0 : 1) - (/[\u4e00-\u9fa5]/.test(b) ? 0 : 1));
      const pick = tags.filter((t) => /[\u4e00-\u9fa5]/.test(t)).slice(0, 5);
      const use = pick.length ? pick : tags.slice(0, 5);
      if (use.length) parts.push('游戏标签：' + use.join('、') + '。');
      const n = (g.regions || []).length;
      if (n) parts.push('当前有 ' + n + ' 个可用区在线，选一个区服即可开始云端畅玩。');
      const fee = g.coinsPerMinute ? '每分钟消耗 ' + g.coinsPerMinute + ' 云币' : '当前限时免费';
      const daily = g.freeDaily ? '，每天赠送 ' + Math.round(g.freeDaily / 60) + ' 分钟免费时长' : '';
      parts.push(fee + daily + '。无需下载安装，云端直接开玩。');
      return parts.join('\n');
    },
    // <text> 内容不能换行 → 先把整段切成定长行数组
    wrapLine(text) {
      const out = [];
      let rest = String(text === undefined || text === null ? '' : text).replace(/[\r\n\t]/g, ' ').trim();
      while (rest.length > DESC_PER_LINE) {
        out.push(rest.slice(0, DESC_PER_LINE));
        rest = rest.slice(DESC_PER_LINE);
      }
      if (rest) out.push(rest);
      return out;
    },
    buildDesc(desc) {
      const segs = String(desc || '').split('\n');
      const out = [];
      let n = 0;
      for (let i = 0; i < segs.length && n < DESC_MAX_SEG; i++) {
        const s = (segs[i] || '').trim();
        if (!s) continue;
        const lines = this.wrapLine(s);
        for (let k = 0; k < lines.length && n < DESC_MAX_SEG; k++) {
          out.push(lines[k]);
          n++;
        }
      }
      return out;
    },
    goBack() {
      navBack('detail');
    },
    setTab(id) {
      if (id === 'search') {
        navTo('search', {});
        return;
      }
      if (id === 'mine') {
        /* 同 search.vue：退回 index 再切栏目，避免压一层只有两项导航的薄壳页 */
        try { setPendingTab('mine'); } catch (e) {}
        if (!navBack('detail')) navTo('index', { tab: 'mine' });
        return;
      }
      // 其余分区回首页（首页自己管 tab 态）
      navBack('detail');
    },
    async pickQuality(id) {
      if (this.quality === id) return;
      this.quality = id;
      try {
        await saveSettings({ quality: id });
        this.actHint = '画质已设为 ' + (QUALITY_LABEL[id] || id);
      } catch (e) {
        this.actHint = '画质保存失败';
      }
    },
    startPlay() {
      if (!this.game || !this.game.code) return;
      // gameType 必须透传：play 页据此决定 tickets 的 width/height（端游 1920x1080）
      // 并交给 session 查该游戏自己的 region 列表。漏传会让端游走手游尺寸 + 手游 region，
      // 服务端回 1288（菜单很具误导性：「请重新测速」）。
      navTo('play', {
        code: this.game.code,
        name: this.game.name,
        quality: this.quality,
        gameType: this.game.gameType || 'mobile'
      });
    },
    async addHistory() {
      if (!this.game || !this.game.code) return;
      try {
        await recordPlay({ code: this.game.code, name: this.game.name, icon: this.game.icon });
        this.actHint = '已加入游玩历史';
        this.loadHist(this.game.code);
      } catch (e) {
        this.actHint = '加入失败';
      }
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

.topbar-title-wrap {
  height: 22px;
  overflow: hidden;
  margin-left: 10px;
}

.line-17 {
  height: 17px;
  overflow: hidden;
}

.topbar-title {
  font-size: 17px;
  color: @text-color-strong;
  lines: 1;
}

.topbar-sub-wrap {
  height: 17px;
  overflow: hidden;
  margin-left: 8px;
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
}

.topbar-sub {
  font-size: 12px;
  color: @text-color-dim;
  lines: 1;
}

/* ---------- 主体 ----------
   画布 266 - 顶部条 38 = 228，高度写满才不会在底部留一条背景色空带 */
.body {
  flex-direction: row;
  padding-left: 12px;
  padding-right: 8px;
  padding-bottom: 6px;
  height: 228px;
}

.col-left {
  width: 424px;
  flex-direction: column;
}

.hero {
  flex-direction: row;
  height: 96px;
}

.hero-icon {
  width: 96px;
  height: 96px;
  border-radius: 10px;
  background-color: @card-background-color;
}

.hero-info {
  flex-direction: column;
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
  padding-left: 10px;
  padding-right: 4px;
}

.hero-name {
  font-size: 16px;
  color: @text-color-strong;
  lines: 1;
}

.hero-sum {
  font-size: 12px;
  color: @text-color-dim;
  lines: 1;
}

/* hero 只有 96px：名称 17 + 摘要 17 + 标签区最多两行（18+4 各一次） */
.chip-row {
  flex-direction: row;
  flex-wrap: wrap;
  margin-top: 6px;
  height: 44px;
  overflow: hidden;
}

.chip {
  height: 18px;
  padding-left: 6px;
  padding-right: 6px;
  margin-right: 5px;
  margin-bottom: 4px;
  border-radius: 9px;
  background-color: @ctrl-background;
  justify-content: center;
  align-items: center;
}

.chip-t {
  font-size: 10px;
  color: @text-color-dim;
}

/* 状态徽标（限免 / 可挂机 / 支持手柄）：绿底白字 */
.chip-on {
  background-color: @accent;
}

.chip-t-on {
  color: @btn-text-on-accent;
}

.stats {
  flex-direction: row;
  margin-top: 8px;
  height: 44px;
}

.stat {
  width: 132px;
  height: 44px;
  margin-right: 8px;
  border-radius: 8px;
  background-color: @card-background-color;
  flex-direction: column;
  justify-content: center;
  align-items: center;
}

.stat-v {
  font-size: 15px;
  color: @accent-blue;
  lines: 1;
}

.stat-k {
  font-size: 10px;
  color: @text-color-dim;
  margin-top: 1px;
  lines: 1;
}

.actions {
  flex-direction: row;
  align-items: center;
  margin-top: 8px;
  height: 34px;
}

.btn {
  margin-right: 10px;
}

.hist-line {
  height: 16px;
  overflow: hidden;
  margin-top: 4px;
}

.hist-t {
  font-size: 11px;
  color: @text-color-dim;
  lines: 1;
}

.act-hint {
  font-size: 11px;
  color: @accent-blue;
  lines: 1;
}

/* ---------- 右列 ---------- */
.col-right {
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
  flex-direction: column;
  margin-left: 10px;
}

.q-row {
  flex-direction: row;
  align-items: center;
  height: 26px;
}

.q-label {
  font-size: 13px;
  color: @text-color;
  margin-right: 8px;
  width: 30px;
}

.q-chip {
  height: 22px;
  padding-left: 12px;
  padding-right: 12px;
  margin-right: 6px;
  border-radius: 11px;
  background-color: @ctrl-background;
  justify-content: center;
  align-items: center;
}

.q-chip-on {
  background-color: @accent;
}

.q-t {
  font-size: 11px;
  color: @text-color-dim;
}

.q-t-on {
  color: @btn-text-on-accent;
}

.sec-head {
  height: 22px;
  flex-direction: row;
  align-items: center;
  margin-top: 4px;
}

.sec-title {
  font-size: 13px;
  color: @text-color-strong;
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
}

.sec-sub {
  font-size: 10px;
  color: @text-color-dim;
}

.desc-scroll {
  flex-direction: column;
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
  overflow: hidden;
  padding-right: 6px;
  padding-bottom: 4px;
}

.desc-line {
  font-size: 12px;
  color: @text-color-dim;
  height: 17px;
  lines: 1;
}

.desc-empty {
  font-size: 12px;
  color: @text-color-dim;
}

/* ---------- 状态 ---------- */
.state-row {
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
  flex-direction: row;
  justify-content: center;
  align-items: center;
}

.state-text {
  font-size: 14px;
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

.retry-text {
  font-size: 12px;
  color: @btn-text-on-accent;
}
</style>
