import QueueComponent from './queue.vue';
import { BasePage } from '../../base-page.js';

class PageQueue extends BasePage {
  constructor() {
    super();
  }

  onLoad(options) {
    super.onLoad(options);
    this.setRootComponent(QueueComponent);
  }

  onNewOptions(options) {
    super.onNewOptions(options);
  }
}

export default PageQueue;
