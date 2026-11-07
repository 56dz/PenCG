<template>
  <div class="page">
    <div class="bar">
      <div class="brand"><div class="brand-mark"><text class="brand-mark-text">P</text></div></div>
      <div class="tab" @click="goIndex"><text class="tab-text">首页</text></div>
      <div class="tab tab-on"><text class="tab-text tab-text-on">我的</text></div>
    </div>
    <div class="content">
      <mine-body ref="body" />
    </div>
  </div>
</template>
<script>
import MineBody from '../../components/mine-body.vue';
import { navTo } from '../../services/nav.js';

// 薄壳：内容全部在 components/mine-body.vue（index.vue 内嵌复用同一份）。
// 保留本页是为了直接 navTo('mine') 的老入口（首页 tab、外链）仍能用。
export default {
  components: { MineBody },
  onShow() {
    // 优先 ref；$refs 在本运行时不一定可用，回退到组件自己挂在根实例上的引用
    const b = (this.$refs && this.$refs.body) || this.__mineBody;
    if (b && b.refresh) b.refresh();
  },
  methods: {
    goIndex() {
      navTo('index', {});
    }
  }
};
</script>
<style lang="less" scoped>
@import "base.less";
</style>
