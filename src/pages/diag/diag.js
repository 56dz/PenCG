import DiagComponent from './diag.vue';
import { BasePage } from '../../base-page.js';

class PageDiag extends BasePage {
  constructor() {
    super();
  }

  onLoad(options) {
    super.onLoad(options);
    this.setRootComponent(DiagComponent);
  }

  onNewOptions(options) {
    super.onNewOptions(options);
  }
}

export default PageDiag;
