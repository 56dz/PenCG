import MineComponent from './mine.vue';
import { BasePage } from '../../base-page.js';

class PageMine extends BasePage {
  constructor() {
    super();
  }

  onLoad(options) {
    super.onLoad(options);
    this.setRootComponent(MineComponent);
  }

  onNewOptions(options) {
    super.onNewOptions(options);
  }
}

export default PageMine;
