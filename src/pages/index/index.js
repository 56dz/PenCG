import IndexComponent from './index.vue';
import { BasePage } from '../../base-page.js';

class PageIndex extends BasePage {
  constructor() {
    super();
  }

  onLoad(options) {
    super.onLoad(options);
    this.setRootComponent(IndexComponent);
  }

  onNewOptions(options) {
    super.onNewOptions(options);
  }
}

export default PageIndex;
