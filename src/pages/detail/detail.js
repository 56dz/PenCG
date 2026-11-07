import DetailComponent from './detail.vue';
import { BasePage } from '../../base-page.js';

class PageDetail extends BasePage {
  constructor() {
    super();
  }

  onLoad(options) {
    super.onLoad(options);
    this.setRootComponent(DetailComponent);
  }

  onNewOptions(options) {
    super.onNewOptions(options);
  }
}

export default PageDetail;
