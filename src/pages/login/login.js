import LoginComponent from './login.vue';
import { BasePage } from '../../base-page.js';

class PageLogin extends BasePage {
  constructor() {
    super();
  }

  onLoad(options) {
    super.onLoad(options);
    this.setRootComponent(LoginComponent);
  }

  onNewOptions(options) {
    super.onNewOptions(options);
  }
}

export default PageLogin;
