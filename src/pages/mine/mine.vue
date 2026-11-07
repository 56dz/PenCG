<template>
  <div class="page">
    <!-- 顶栏补齐成和首页一致的 5 栏。
         用户反馈 2：原来这里只有「首页 / 我的」两项，只要走了 navTo('mine')
         这条路（登录成功、详情页点「我的」），左侧导航就"塌"成两项。
         现在所有入口都改成退回 index 切栏目了，但本页作为外链/老入口的
         兜底仍然存在，所以这里也得把 5 栏补齐，不能只留两项。 -->
    <div class="bar">
      <div class="brand"><image class="brand-logo" :src="logo" :width="26" :height="26"></image></div>
      <div v-for="t in tabs" :key="t.id" :class="tab === t.id ? 'tab tab-on' : 'tab'" @click="goTab(t.id)">
        <text :class="tab === t.id ? 'tab-text tab-text-on' : 'tab-text'">{{ t.label }}</text>
      </div>
    </div>
    <div class="content">
      <mine-body ref="body" />
    </div>
  </div>
</template>
<script>
import MineBody from '../../components/mine-body.vue';
import { navTo, navBack, setPendingTab } from '../../services/nav.js';
import logoImg from '../../assets/logo.png';

// 薄壳：内容全部在 components/mine-body.vue（index.vue 内嵌复用同一份）。
// 保留本页是为了直接 navTo('mine') 的老入口（外链、极旧版本残留的跳转）仍能用。
// 注意：日常路径已经不再 navTo 到这里，一律退回 index 原地切栏目。
const TABS = [
  { id: 'recommend', label: '推荐' },
  { id: 'mobile', label: '手游' },
  { id: 'pc', label: '端游' },
  { id: 'free', label: '限免' },
  { id: 'mine', label: '我的' }
];

export default {
  components: { MineBody },
  data() {
    return {
      tabs: TABS,
      tab: 'mine',
      logo: logoImg
    };
  },
  onShow() {
    // 优先 ref；$refs 在本运行时不一定可用，回退到组件自己挂在根实例上的引用。
    // 注意第三条 `this.$root.__mineBody` 之前漏了，只写了 `this.__mineBody`，
    // 而 mine-body 挂的是 $root 上的那个，导致本页 onShow 永远刷不到数据。
    const self = this;
    const run = function () {
      const b = (self.$refs && self.$refs.body) || (self.$root && self.$root.__mineBody) || self.__mineBody;
      if (b && b.refresh) {
        try {
          b.refresh();
          return true;
        } catch (e) {
          /* ignore */
        }
      }
      return false;
    };
    if (run()) return;
    setTimeout(run, 120);
    setTimeout(run, 260);
  },
  methods: {
    goIndex() {
      navTo('index', {});
    },
    goTab(id) {
      if (id === 'mine') return;
      // 回到 index 原地切栏目（和首页那 5 栏的行为完全一致）
      try {
        setPendingTab(id);
      } catch (e) {
        /* ignore */
      }
      if (!navBack()) navTo('index', { tab: id });
    }
  }
};
</script>
<style lang="less" scoped>
@import "base.less";
</style>
