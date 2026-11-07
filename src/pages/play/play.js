import PlayComponent from './play.vue';
import { BasePage } from '../../base-page.js';

class PagePlay extends BasePage {
  constructor() {
    super();
  }

  onLoad(options) {
    super.onLoad(options);
    this.setRootComponent(PlayComponent);
  }

  onNewOptions(options) {
    super.onNewOptions(options);
  }
}

export default PagePlay;
