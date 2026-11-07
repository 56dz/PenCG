import SearchComponent from './search.vue';
import { BasePage } from '../../base-page.js';

class PageSearch extends BasePage {
  constructor() {
    super();
  }

  onLoad(options) {
    super.onLoad(options);
    this.setRootComponent(SearchComponent);
  }

  onNewOptions(options) {
    super.onNewOptions(options);
  }
}

export default PageSearch;
