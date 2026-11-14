<template>
  <!-- 检查更新：整页替换（960x266），不是浮层。
       本运行时 position:absolute 不可靠、全仓没有一个 z-index 能用（base.less 文件头铁律），
       所以由外层页面 v-if 把正常视图整体不渲染 —— 零定位依赖，也就不会出现
       「弹窗和列表叠在一起」。 -->
  <div class="ud">
    <div class="ud-card">
      <div class="ud-left">
        <div class="ud-head">
          <text class="ud-title">{{ head }}</text>
          <!-- 版本徽标与包大小只在「真的有一个待装的包」时才有意义：
               已是最新 / 检查失败这两种状态下标题里已经有版本号，再挂一枚
               同样的徽标就是自己跟自己重复，右上也挤。 -->
          <text v-if="showMeta && ver" class="ud-ver">{{ ver }}</text>
          <text v-if="showMeta && sizeText" class="ud-size">{{ sizeText }}</text>
        </div>
        <text class="ud-sub">{{ sub }}</text>
        <!-- 更新说明可上下滑动：发布说明经常上千字，266px 屏只放得下十来行。
             scroller 内每行必须是独立 <text> —— falcon 的 <text> 内容里出现换行就
             整块不渲染，所以 bodyLines() 先把 Markdown 切成行数组。 -->
        <scroller class="ud-scroll">
          <template v-for="(ln, i) in lines">
            <text v-if="ln" :key="'ul' + i" class="ud-line">{{ ln }}</text>
            <div v-else :key="'ug' + i" class="ud-gap"></div>
          </template>
          <text v-if="!lines.length" class="ud-line">（本版本没有填写更新说明）</text>
        </scroller>
      </div>
      <div class="ud-right">
        <!-- 进入软件时自动检测：4 个按钮，两个下载按钮**都是直连**。
             第一个按钮的括号里必须是**当前命中的源名**（GitCode / GitHub）——
             写死「直连 GitHub」在 GitCode 主通道下就是错的，用户会以为点了也白点。
             第二个按钮是换源：两个平台的网络可达性互补，GitCode 挂了还能直连 GitHub。 -->
        <template v-if="mode === 'auto' && stage === 'ready'">
          <div class="ud-btn ud-btn-main" @click="onInstall()">
            <text class="ud-btn-t-main">{{ mainLabel }}（直连 {{ srcLabel }}）</text>
          </div>
          <div class="ud-btn ud-btn-main" @click="onInstallFrom(altSource)">
            <text class="ud-btn-t-main">{{ mainLabel }}（直连 {{ altLabel }}）</text>
          </div>
          <div class="ud-btn" @click="onSkip">
            <text class="ud-btn-t">跳过此版本</text>
          </div>
          <div class="ud-btn" @click="onQuiet">
            <text class="ud-btn-t">今天内不提示</text>
          </div>
        </template>
        <!-- 「我的」页手动检测：2 个按钮（下载源由 onInstall 自动换源，不给用户选） -->
        <template v-else-if="mode === 'manual' && stage === 'ready'">
          <div class="ud-btn ud-btn-main" @click="onInstall(altSource)">
            <text class="ud-btn-t-main">{{ mainLabel }}</text>
          </div>
          <div class="ud-btn" @click="onClose">
            <text class="ud-btn-t">暂不更新</text>
          </div>
        </template>
        <!-- 其它阶段（检查中 / 已最新 / 下载中 / 安装中 / 已下载 / 失败）只有一个关闭 -->
        <template v-else>
          <div class="ud-btn" @click="onClose">
            <text class="ud-btn-t">{{ busy ? '请稍候…' : '关闭' }}</text>
          </div>
          <div v-if="hasUrl" class="ud-btn" @click="onShowUrl">
            <text class="ud-btn-t">查看发布页</text>
          </div>
        </template>
        <text class="ud-tip">{{ tip }}</text>
      </div>
    </div>
  </div>
</template>

<script>
import {
  bindView,
  unbindView,
  doInstall,
  installFrom,
  onSkip,
  onQuiet,
  onShowUrl,
  closeView
} from '../services/upd-view.js';
import { sourceLabel, updateCapability } from '../services/updater.js';
import { APP_VERSION } from '../services/version.js';
import { logWarn } from '../services/log.js';

/* 为什么不把标题/按钮文案写成 computed：
 * index.vue 里有实测注释（countText 那一段）—— 本运行时对 computed 的模板求值
 * 在某些形态下不可靠，会把函数对象当源码渲染出来。这里所有展示串都在 apply()
 * 里一次性算好存进 data，模板只做插值，零推断风险。 */
function headText(v) {
  if (v.stage === 'checking') return '正在检查更新…';
  if (v.stage === 'latest') return '已是最新版本 · v' + (v.localVersion || '');
  if (v.stage === 'downloading') return '正在下载新版本…';
  if (v.stage === 'installing') return '正在安装…';
  if (v.stage === 'downloaded') return '安装包已下载';
  if (v.stage === 'done') return '安装完成';
  if (v.stage === 'error') return '检查更新失败';
  if (v.release) return '发现新版本 ' + v.release.tag;
  return '检查更新';
}

function sizeTextOf(rel) {
  const n = Number(rel && rel.assetSize) || 0;
  if (n <= 0) return '';
  if (n < 1024 * 1024) return Math.round(n / 1024) + ' KB';
  return (n / 1024 / 1024).toFixed(1) + ' MB';
}

function busyOf(stage) {
  return stage === 'checking' || stage === 'downloading' || stage === 'installing';
}

export default {
  data() {
    return {
      mode: 'manual',
      stage: 'idle',
      head: '检查更新',
      sub: '',
      ver: '',
      sizeText: '',
      lines: [],
      tip: '',
      srcLabel: '',
      altLabel: '',
      altSource: '',
      mainLabel: '立即更新',
      busy: false,
      showMeta: false,
      hasUrl: false
    };
  },
  mounted() {
    /* 用闭包而不是直接把 this.apply 交出去：bindView 里是 `fn(view)` 裸调用，
     * 靠 Vue2 把 methods 绑定到实例才能拿到 this。虽然 Vue2 的 initMethods 确实
     * 做了 bind(vm)，但本运行时是改造过的 fork，没必要押这个假设。
     * 绑定后 bindView 会立刻用当前状态回调一次，所以不需要再单独读 getView()。 */
    const self = this;
    this._onView = function (v) {
      self.apply(v);
    };
    bindView(this._onView);
  },
  beforeDestroy() {
    if (this._onView) unbindView(this._onView);
    this._onView = null;
  },
  methods: {
    apply(v) {
      if (!v) return;
      const rel = v.release;
      const cap = updateCapability();
      const cur = v.source || 'gitcode';
      const alt = cur === 'github' ? 'gitcode' : 'github';
      const stage = v.stage || 'idle';
      /* canInstall 为假时按钮不能写「立即更新」—— 点了只会下载完然后告诉你装不了，
       * 那是骗人。改成「下载安装包」，并在 tip 里说清要手动装。 */
      const main = cap.canInstall ? '立即更新' : '下载安装包';
      this.mode = v.mode || 'manual';
      this.stage = stage;
      this.head = headText(Object.assign({}, v, { localVersion: APP_VERSION }));
      this.sub = v.message || '';
      this.ver = rel && rel.tag ? rel.tag : '';
      this.sizeText = sizeTextOf(rel);
      this.lines = Array.isArray(v.lines) ? v.lines : [];
      this.tip = v.tip || '';
      this.srcLabel = sourceLabel(cur);
      this.altLabel = sourceLabel(alt);
      this.altSource = alt;
      this.mainLabel = main;
      this.busy = busyOf(stage);
      // 只有 ready 状态才把「版本徽标 + 包大小」挂出来，其余状态标题已含版本号
      this.showMeta = stage === 'ready' && !!(rel && rel.tag);
      this.hasUrl = !!(rel && rel.htmlUrl);
    },
    onClose() {
      if (this.busy) return;
      logWarn('[cg] upd close (mode=' + this.mode + ' stage=' + this.stage + ')');
      closeView();
    },
    onSkip() {
      onSkip();
    },
    onQuiet() {
      onQuiet();
    },
    onShowUrl() {
      onShowUrl();
    },
    onInstall(altId) {
      if (this.busy) return;
      doInstall(altId || this.altSource);
    },
    onInstallFrom(onlyId) {
      if (this.busy) return;
      installFrom(onlyId || this.altSource);
    }
  }
};
</script>

<style lang="less" scoped>
@import "base.less";

/* 整页替换层：铺满 960x266，内容居中。
   注意不能写 position:fixed/absolute —— 见模板顶部注释。 */
.ud {
  width: 100vw;
  height: 100vh;
  flex-direction: row;
  justify-content: center;
  align-items: center;
  background-color: @background-color;
}

.ud-card {
  width: 79.1667vw;
  height: 87.2181vh;
  flex-direction: row;
  border-radius: 1.4584vw;
  background-color: @card-background-color;
  border-width: 1px;
  border-style: solid;
  border-color: @card-border-color;
  padding-left: 1.6667vw;
  padding-right: 1.6667vw;
  padding-top: 6.0151vh;
  padding-bottom: 6.0151vh;
}

.ud-left {
  width: 45vw;
  height: 75.188vh;
  flex-direction: column;
}

.ud-right {
  width: 29.1667vw;
  height: 75.188vh;
  flex-direction: column;
  margin-left: 1.6667vw;
}

.ud-head {
  width: 45vw;
  height: 8.2707vh;
  flex-direction: row;
  align-items: center;
}

.ud-title {
  font-size: 1.5625vw;
  font-weight: 600;
  color: @text-color-strong;
  lines: 1;
}

.ud-ver {
  font-size: 1.25vw;
  color: @accent;
  margin-left: 0.8334vw;
  lines: 1;
}

.ud-size {
  font-size: 1.1459vw;
  color: @text-color-dim;
  margin-left: 0.625vw;
  lines: 1;
}

.ud-sub {
  width: 45vw;
  height: 6.0151vh;
  font-size: 1.1459vw;
  color: @text-color-dim;
  lines: 1;
  text-overflow: ellipsis;
  margin-top: 1.5038vh;
}

.ud-scroll {
  width: 45vw;
  height: 56.391vh;
  margin-top: 2.2557vh;
  overflow: hidden;
}

/* 每行单独一个 <text>；lines 限 3 防止一行超长文本撑爆宽度（Weex 的 lines
   不总可靠，bodyLines 已经在 JS 侧压过长度） */
.ud-line {
  width: 45vw;
  font-size: 1.25vw;
  color: @text-color;
  lines: 3;
}

.ud-gap {
  width: 45vw;
  height: 3.0076vh;
}

/* 按钮：4 个一组（自动检测）/ 2 个一组（手动检测），同一套尺寸。
 * 尺寸预算对齐 PenBili：4×36 + 4×8 = 176，加提示 2 行 24 = 200，正好占满右列。
 * 必须显式写 flex-direction: row —— Weex 默认是 column，靠 justify-content
 * 居中的话会变成纵向居中、横向拉伸。 */
.ud-btn {
  width: 29.1667vw;
  height: 13.5339vh;
  margin-top: 3.0076vh;
  flex-direction: row;
  align-items: center;
  justify-content: center;
  border-radius: 1.0417vw;
  background-color: @ctrl-background;
}

.ud-btn:active {
  opacity: 0.7;
}

/* 主按钮（立即更新 / 下载安装包）：品牌绿底 + 深色文字，与全项目主操作一致 */
.ud-btn-main {
  background-color: @accent;
}

.ud-btn-t {
  font-size: 1.3542vw;
  color: @text-color;
  lines: 1;
}

.ud-btn-t-main {
  font-size: 1.3542vw;
  font-weight: 600;
  color: #1A1005;
  lines: 1;
}

/* 右下角提示：解释「为什么不能点」或「下载到哪儿了」（也用来显示发布页地址）。
 * margin-top 刻意写 0：每个 .ud-btn 自带 3.0076vh(8px) 上边距，最后一个按钮的
 * 上边距就是提示与按钮之间的间距。PenBili 这里额外又加了 8px，导致右列
 * 144+32+8+24=208 > 200 溢出卡片底边 8px（提示最后一行被裁）。
 * 去掉后正好 200：4 按钮 144 + 4 间距 32 + 提示 2 行 24。 */
.ud-tip {
  width: 29.1667vw;
  height: 9.0226vh;
  margin-top: 0;
  font-size: 1.0417vw;
  color: @text-color-dim;
  lines: 2;
  text-overflow: ellipsis;
}
</style>
