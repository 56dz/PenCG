<template>
  <div class="page">
    <div class="bar">
      <div class="brand"><image class="brand-logo" :src="logo" :width="26" :height="26"></image></div>
      <div class="tab" @click="goBack"><text class="tab-text">返回</text></div>
      <div class="tab" @click="goMine"><text class="tab-text">我的</text></div>
      <div class="tab tab-on"><text class="tab-text tab-text-on">登录</text></div>
    </div>
    <div v-if="kbVisible" class="content">
      <div class="kb-head">
        <text class="kb-title">{{ kbTitle }}</text>
        <text class="kb-hint">系统输入法不可用，使用屏幕键盘</text>
        <text class="kb-val">{{ kbMasked }}</text>
        <div class="kb-close" @click="kbCancel"><text class="kb-close-text">取消</text></div>
      </div>
      <div class="kb-body">
        <div class="kb-col">
          <div class="kb-row">
            <div v-for="k in kbTop" :key="'t' + k" class="kb-key" @click="kbPress(k)"><text class="kb-key-text">{{ k }}</text></div>
          </div>
          <div class="kb-row">
            <div v-for="k in kbMid" :key="'m' + k" class="kb-key" @click="kbPress(k)"><text class="kb-key-text">{{ k }}</text></div>
          </div>
          <div class="kb-row">
            <div class="kb-key kb-key-fn" @click="kbDel"><text class="kb-key-text">删除</text></div>
            <div class="kb-key kb-key-ok" @click="kbOk"><text class="kb-key-text">确认</text></div>
          </div>
        </div>
      </div>
    </div>
    <div v-else class="content">
      <div class="main">
      <div class="brand-col">
        <image class="logo-img" :src="logo" :width="62" :height="62"></image>
        <text class="brand-name">网易云游戏</text>
        <text class="brand-sub">PenCG 客户端</text>
        <text class="brand-tip">登录后可同步免费时长</text>
        <text class="brand-tip">和游玩记录</text>
      </div>
      <div class="form-col">
        <!-- 表单列包进 scroller：X6 Pro 上纵向合计约 62vh 放得下，但系统提示、
             错误行、短信剩余秒数文案都可能换行，一旦超出 100vh 底部就会被裁掉
             且没有滚动入口。内容不溢出时滚动条不出现，布局与原来一致。 -->
        <scroller class="form-scroll">
        <div class="tabs">
          <div :class="mode === 'pwd' ? 'mtab mtab-on' : 'mtab'" @click="setMode('pwd')">
            <text :class="mode === 'pwd' ? 'mtab-text mtab-text-on' : 'mtab-text'">密码登录</text>
          </div>
          <div :class="mode === 'sms' ? 'mtab mtab-on' : 'mtab'" @click="setMode('sms')">
            <text :class="mode === 'sms' ? 'mtab-text mtab-text-on' : 'mtab-text'">短信登录</text>
          </div>
          <text class="status">{{ statusText }}</text>
        </div>
        <div class="frow">
          <text class="flabel">手机号</text>
          <div class="cc" @click="editCc"><text class="cc-text">+{{ cc }}</text></div>
          <div class="field" @click="editPhone">
            <text v-if="phone" class="field-val">{{ phone }}</text>
            <text v-else class="field-ph">点击输入手机号</text>
          </div>
        </div>
        <div v-if="mode === 'pwd'" class="frow">
          <text class="flabel">密码</text>
          <div class="field field-wide" @click="editPwd">
            <text v-if="pwdMask" class="field-val">{{ pwdMask }}</text>
            <text v-else class="field-ph">点击输入密码</text>
          </div>
          <div class="swap" @click="clearPwd"><text class="swap-text">重输</text></div>
        </div>
        <div v-else class="frow">
          <text class="flabel">验证码</text>
          <div class="field field-code" @click="editCode">
            <text v-if="smsCode" class="field-val">{{ smsCode }}</text>
            <text v-else class="field-ph">点击输入验证码</text>
          </div>
          <div :class="smsLeft > 0 ? 'sms-btn sms-btn-off' : 'sms-btn'" @click="sendCode">
            <text class="sms-text">{{ smsText }}</text>
          </div>
        </div>
        <div class="frow">
          <div :class="busy ? 'go-btn go-btn-busy' : 'go-btn'" @click="submit">
            <text class="go-text">{{ busy ? '登录中…' : '登 录' }}</text>
          </div>
          <text class="err">{{ errText }}</text>
        </div>
        <div v-if="toast" class="toast"><text class="toast-text">{{ toast }}</text></div>
        <text v-else class="tip">{{ tipText }}</text>
        </scroller>
      </div>
      </div>
    </div>
  </div>
</template>
<script>
import { loginByPassword, loginBySms, sendSmsCode, restoreSession, currentSession } from '../../services/auth.js';
import { getString, setString } from '../../services/storage.js';
import { navTo, navBack, setPendingTab } from '../../services/nav.js';
import logoImg from '../../assets/logo.png';
import { ImSession } from '../../services/im.js';
import { logInfo, logWarn } from '../../services/log.js';
import { DEV_AUTOLOGIN } from '../../dev-autostart.js';

const LAST_PHONE_KEY = 'pencg_login_phone';

function digits(s, max) {
  const v = String(s || '').replace(/[^0-9]/g, '');
  return max ? v.slice(0, max) : v;
}

function dots(n) {
  let out = '';
  for (let i = 0; i < n; i++) out = out + '●';
  return out;
}

export default {
  data() {
    return {
    // 软件图标 logo（构建时由 image 插件拷进 images/）
    logo: logoImg,
      mode: 'pwd',
      cc: '86',
      phone: '',
      pwd: '',
      smsCode: '',
      smsLeft: 0,
      smsWait: 60,
      busy: false,
      errText: '',
      toast: '',
      statusText: '未登录',
      kbVisible: false,
      kbTarget: 'phone',
      kbBuffer: '',
      tipText: '首次登录会消耗一次会话校验，之后自动续用'
    };
  },
  computed: {
    pwdMask() {
      return this.pwd ? dots(this.pwd.length) : '';
    },
    kbTitle() {
      if (this.kbTarget === 'pwd') return '输入密码';
      if (this.kbTarget === 'code') return '输入验证码';
      if (this.kbTarget === 'cc') return '输入区号';
      return '输入手机号';
    },
    kbMasked() {
      if (this.kbTarget === 'pwd') return this.kbBuffer ? dots(this.kbBuffer.length) : '(空)';
      return this.kbBuffer || '(空)';
    },
    kbTop() {
      return ['1', '2', '3', '4', '5'];
    },
    kbMid() {
      return ['6', '7', '8', '9', '0'];
    },
    smsText() {
      if (this.smsLeft > 0) return this.smsLeft + 's';
      return this.smsWait > 0 && this.smsWait < 60 ? '重新获取' : '获取验证码';
    }
  },
  created() {
    this.im = new ImSession();
    this.imBusy = false;
    const self = this;
    this.im.onIdle = function () {
      self.imBusy = false;
    };
  },
  mounted() {
    this.boot();
  },
  onShow() {
    this.refreshStatus();
  },
  onUnload() {
    this.stopTick();
    try {
      this.im.dispose();
    } catch (e) {
      /* ignore */
    }
  },
  methods: {
    async boot() {
      const last = await getString(LAST_PHONE_KEY, '');
      if (last) this.phone = digits(last, 11);
      await this.refreshStatus();
      logInfo('[login] ready phone=' + (this.phone ? this.phone.slice(0, 3) + '****' : '(空)'));
      this.devAutoLogin();
    },
    /* ★ 仅取证用（见 src/dev-autostart.js 的 DEV_AUTOLOGIN 说明）：
     * 自动填表 + 提交，走的仍是 submit() → loginByPassword() → goBackToMine()
     * 那条生产链路，不绕过任何被测逻辑。DEV_AUTOLOGIN 为 null 时本方法直接返回，
     * 生产构建里等同一个空函数。 */
    devAutoLogin() {
      const d = DEV_AUTOLOGIN;
      if (!d || !d.phone) return;
      if (this.busy) {
        logWarn('[dev] autologin 跳过：busy');
        return;
      }
      // 已经登录就不要再提交一次（会白白消耗一次会话校验）
      if (this.statusText && this.statusText.indexOf('未登录') < 0) {
        logWarn('[dev] autologin 跳过：已有会话 status=' + this.statusText);
        return;
      }
      const self = this;
      const wait = Number(d.delay) > 0 ? Number(d.delay) : 1200;
      logWarn('[dev] autologin 将在 ' + wait + 'ms 后提交 phone=' + String(d.phone).slice(0, 3) + '****');
      setTimeout(function () {
        if (self.busy) {
          logWarn('[dev] autologin 触发时 busy，放弃');
          return;
        }
        self.mode = 'pwd';
        self.cc = d.cc ? String(d.cc) : '86';
        self.phone = digits(d.phone, 11);
        self.pwd = String(d.pwd || '');
        self.errText = '';
        logWarn('[dev] autologin fire → submit()');
        const p = self.submit();
        if (p && typeof p.catch === 'function') {
          p.catch(function (e) {
            logWarn('[dev] autologin submit 未捕获异常 ' + ((e && e.message) || e));
          });
        }
      }, wait);
    },
    async refreshStatus() {
      try {
        const ok = await restoreSession();
        const s = currentSession();
        if (ok && s && s.token) {
          this.statusText = (s.nickname || '已登录') + ' · 可直接使用';
        } else {
          this.statusText = '未登录';
        }
      } catch (e) {
        this.statusText = '未登录';
      }
    },
    setMode(m) {
      if (this.mode === m) return;
      this.mode = m;
      this.errText = '';
    },
    goBack() {
      navBack('login');
    },
    goMine() {
      // 同 goBackToMine：顶栏「我的」也不能 navTo('mine')，
      // 否则会压一个只有两项导航的薄壳页（用户反馈 2）。
      this.goBackToMine();
    },
    // 登录成功后回到「我的」栏目：先尝试 navBack 退回 index（保住它已经
    // 加载好的首页数据），退不掉才 navTo('index')。两条路都通过 nav.js 的
    // pending 把 tab:'mine' 传给 index，让它在 onShow 里切到「我的」。
    goBackToMine() {
      try {
        setPendingTab('mine');
      } catch (e) {
        /* ignore */
      }
      const ok = navBack('login');
      if (!ok) navTo('index', { tab: 'mine' });
    },
    showToast(t) {
      this.toast = t;
      const self = this;
      setTimeout(function () {
        self.toast = '';
      }, 2000);
    },
    stopTick() {
      if (this.timer) {
        clearInterval(this.timer);
        this.timer = 0;
      }
    },
    startTick() {
      this.stopTick();
      const self = this;
      this.smsLeft = this.smsWait;
      this.timer = setInterval(function () {
        self.smsLeft = self.smsLeft - 1;
        if (self.smsLeft <= 0) {
          self.smsLeft = 0;
          self.stopTick();
        }
      }, 1000);
    },
    // 统一文字输入：走系统输入法（services/im.js）
    // 注意：ImSession.open() 内部会先 _closeCurrent() 关掉上一个仍在挂着的面板，
    // 这个关闭动作会异步抛一个空文本的 textEditFinished 回来，
    // 如果它恰好在本次 pending 建好之后到达，就会把新的等待立刻解析成
    // {confirmed:false, text:''}（真机日志：pwd edit ok=false 出现在
    // "startTextEdit 返回 uuid=..." 之前）。用户不可能在 800ms 内打完并确认，
    // 所以「极快返回的空结果」判定为这种竞态，重开一次。
    async askText(opts, field) {
      if (this.imBusy) return { confirmed: false, text: '' };
      this.imBusy = true;
      this.kbTarget = field || this.kbTarget;
      try {
        let r = null;
        for (let attempt = 0; attempt < 3; attempt++) {
          const t0 = Date.now();
          r = await this.im.open(opts);
          const cost = Date.now() - t0;
          const empty = !r || (!r.confirmed && !r.text);
          if (!empty || (r && r.error) || cost > 800 || r.canceled) break;
          logWarn('[login] im 空结果竞态 cost=' + cost + 'ms attempt=' + attempt + '，重开输入法');
          await new Promise(function (res) { setTimeout(res, 250); });
        }
        if (r && r.error) {
          // 仅系统输入法真的起不来时才走自绘键盘；正常路径 kbVisible 恒为 false，
          // 自绘键盘不会占用任何垂直空间（lead m00850 第 4 点）。
          logWarn('[login] im error ' + r.error + ' → 退化为屏幕键盘 ' + this.kbTarget);
          this.openKeypad(this.kbTarget);
        }
        return r || { confirmed: false, text: '' };
      } catch (e) {
        logWarn('[login] im throw ' + ((e && e.message) || e));
        return { confirmed: false, text: '', error: 'IM_THROW' };
      } finally {
        const orphan = this.im.consumeOrphan();
        this.imBusy = false;
        if (orphan) this.lastOrphan = orphan;
      }
    },
    openKeypad(target) {
      this.kbTarget = target || 'phone';
      this.kbBuffer = '';
      this.kbVisible = true;
      this.errText = '';
    },
    kbPress(k) {
      const max = this.kbTarget === 'pwd' ? 32 : this.kbTarget === 'cc' ? 4 : this.kbTarget === 'code' ? 8 : 11;
      if (this.kbBuffer.length >= max) return;
      this.kbBuffer = this.kbBuffer + k;
    },
    kbDel() {
      this.kbBuffer = this.kbBuffer.slice(0, Math.max(0, this.kbBuffer.length - 1));
    },
    kbCancel() {
      this.kbVisible = false;
      this.kbBuffer = '';
    },
    kbOk() {
      const v = this.kbBuffer;
      const t = this.kbTarget;
      this.kbVisible = false;
      this.kbBuffer = '';
      if (!v) return;
      if (t === 'phone') this.phone = digits(v, 11);
      else if (t === 'pwd') this.pwd = v;
      else if (t === 'code') this.smsCode = digits(v, 8);
      else if (t === 'cc') this.cc = digits(v, 4) || this.cc;
      this.errText = '';
    },
    async editPhone() {
      const r = await this.askText({ value: this.phone, placeholder: '请输入手机号', maxlength: 11 }, 'phone');
      logInfo('[login] phone edit ok=' + r.confirmed + ' err=' + (r.error || '-'));
      if (r.confirmed) {
        this.phone = digits(r.text, 11);
        this.errText = '';
      }
    },
    async editCc() {
      const r = await this.askText({ value: this.cc, placeholder: '国际区号，如 86', maxlength: 4 }, 'cc');
      if (r.confirmed) {
        const c = digits(r.text, 4);
        if (c) this.cc = c;
      }
    },
    async editPwd() {
      const r = await this.askText({ value: this.pwd, placeholder: '请输入密码', maxlength: 32 }, 'pwd');
      logInfo('[login] pwd edit ok=' + r.confirmed + ' len=' + (r.text || '').length);
      if (r.confirmed) {
        this.pwd = String(r.text || '');
        this.errText = '';
      }
    },
    clearPwd() {
      this.pwd = '';
      this.errText = '';
    },
    async editCode() {
      const r = await this.askText({ value: this.smsCode, placeholder: '短信验证码', maxlength: 8 }, 'code');
      if (r.confirmed) {
        this.smsCode = digits(r.text, 8);
        this.errText = '';
      }
    },
    async sendCode() {
      // busy 必须先判：只判 smsLeft 的话，连点两下会在倒计时启动前
      // 并发发出两次请求，服务端直接回 errcode 1100「发送太频繁」。
      if (this.smsLeft > 0 || this.busy) return;
      if (this.phone.length < 6) {
        this.errText = '请先输入手机号';
        return;
      }
      this.errText = '';
      this.busy = true;
      try {
        const r = await sendSmsCode(this.phone, this.cc);
        this.smsWait = (r && r.waitSeconds) || 60;
        this.startTick();
        this.showToast('验证码已发送');
        logInfo('[login] sms sent wait=' + this.smsWait);
      } catch (e) {
        // 易盾（1122/1125）给的是完整可执行文案，前面再叠「发送失败：」会变成
        // 「发送失败：易盾校验失败，请切换网络或稍后重试」——用户 m13280 第 3 条
        // 明确要的是后者这一句本身，所以这类文案不加前缀。
        const msg = this.friendlyErr(e, 'sms');
        this.errText = msg.indexOf('易盾') === 0 ? msg : '发送失败：' + msg;
        logWarn('[login] sms fail ' + ((e && e.message) || e) + ' | detail=' + this.errText);
      } finally {
        this.busy = false;
      }
    },
    // 把服务端/传输层的原始错误翻译成用户能看懂、能据此行动的中文。
    // 背景：net.js 抛出的 message 形如「1102 短信验证码错误」或「http 500」，
    // 直接显示到笔的 266px 屏上既占地方又不告诉用户下一步该干什么。
    friendlyErr(e, mode) {
      const msg = String((e && e.message) || e || '');
      const st = (e && e.status) || 0;
      // 关键：v1 匿名接口走 net.js 的 httpJSON，它**不设 err.code**，
      // 只把 errcode 拼进 message（net.js: `detail = j.errcode + ' ' + errmsgcn`）。
      // 只有 cg.js 的 v2() 才会设 err.code。所以这里两种都要认。
      let code = (e && e.code) || 0;
      if (!code) {
        const m = /^(\d{3,5})\s+/.exec(msg);
        if (m) code = parseInt(m[1], 10);
      }
      // 常见 errcode（CGAPI.md:1165-1184）
      if (code === 1102) return '验证码错误，请重新输入';
      if (code === 1109) return '错误次数过多，请重新获取验证码';
      if (code === 1100) return '发送太频繁，请稍后再试';
      if (code === 1103) return '区号无效，请检查 +86';
      if (code === 1104) return '该手机号未注册';
      if (code === 1006) return '登录已过期，请重新登录';
      /* 1125：password 接口被风控拦下，服务端原文是
       *   errmsg  need_yidun_validate_fail_enough
       *   errmsgcn「易盾校验失败次数过大,要前端cool」
       * 原文直接甩给用户完全看不懂（"前端cool"是给开发者看的）。
       * 实测触发条件是同一 IP 短时间内多次登录失败，换网络/等一会儿就能恢复，
       * 所以文案必须给出可执行的动作。 */
      /* 1122 / 1125 都是易盾（网易风控）拦下的：
       *   1122 → 发短信验证码时（phone-captchas）
       *   1125 → 密码登录时（user-pwd-tokens，need_yidun_validate_fail_enough）
       * 真机日志实证：`[login] sms fail 1122 易盾校验失败`。
       * 两者对用户的处置动作完全一样，合并成一个文案。 */
      if (code === 1122 || code === 1125) return '易盾校验失败，请切换网络或稍后重试';
      // auth.js 在 user-pwd-info 缺 salt 时抛的是这句话（未注册号码只返回 is_registered:false）
      if (msg.indexOf('取登录参数失败') >= 0) return '该手机号未注册，或未设置密码';
      if (msg.indexOf('native 加密模块不可用') >= 0) return '本机加密模块缺失，无法密码登录（可改用短信登录）';
      if (msg.indexOf('timeout') >= 0) return '网络超时，请检查网络后重试';
      if (msg.indexOf('curl') >= 0 || msg.indexOf('jsapi') >= 0 || st === 0) return '网络不可用，请检查网络后重试';
      if (st === 401) return mode === 'sms' ? '验证码或手机号有误' : '手机号或密码有误';
      if (st >= 500) return '服务端异常（' + st + '），请稍后重试';
      // 兜底：去掉可能存在的 errcode 数字前缀，保留服务端原文
      const clean = msg.replace(/^\d+\s+/, '');
      return clean.slice(0, 40) || '登录失败，请重试';
    },
    async submit() {
      if (this.busy) return;
      this.errText = '';
      if (!this.phone || this.phone.length < 6) {
        this.errText = '请输入手机号';
        return;
      }
      if (this.mode === 'pwd' && !this.pwd) {
        this.errText = '请输入密码';
        return;
      }
      if (this.mode === 'sms' && !this.smsCode) {
        this.errText = '请输入验证码';
        return;
      }
      this.busy = true;
      try {
        let r;
        if (this.mode === 'pwd') {
          r = await loginByPassword(this.phone, this.cc, this.pwd);
        } else {
          r = await loginBySms(this.phone, this.cc, this.smsCode);
        }
        await setString(LAST_PHONE_KEY, this.phone);
        logInfo('[login] success uid=' + (r && r.userId));
        /* 用户反馈 4：原来只 toast「登录成功」两秒就没了，而真正跳回「我的」
         * 还要再等一会儿拉 @me 资料，中间这段没有任何提示，像是卡住了。
         * 现在把 loading 态文案直接写在页面上（statusText），并明确说在跳转。 */
        this.statusText = '登录成功，正在跳转…';
        this.showToast('登录成功，正在跳转…');
        this.refreshStatus();
        const self = this;
        setTimeout(function () {
          self.busy = false;
          /* 关键（用户反馈 2）：登录页是 navTo 进来的独立页，登录完必须
           * navBack('login') 回到 index 原地栏目，而不是 navTo('mine') ——
           * navTo('mine') 会再压一个新的 mine 薄壳页，它的顶栏只有
           * 「首页 / 我的」两项，用户就以为左侧导航塌了。
           * index 现在支持 tab 参数（见 index.vue 的 takeTabParam），
           * 回退时带上 tab:'mine' 就能直接落在「我的」栏目。 */
          self.goBackToMine();
        }, 600);
      } catch (e) {
        this.busy = false;
        this.errText = this.friendlyErr(e, this.mode);
        logWarn('[login] fail ' + ((e && e.message) || e) + ' | detail=' + this.errText);
      }
    }
  }
};
</script>
<style lang="less" scoped>
@import "base.less";

.page {
  background-color: @background-color;
}

.kb-head {
  flex-direction: row;
  align-items: center;
  height: 15.0376vh;
  padding-left: 1.25vw;
  padding-right: 1.25vw;
}

.kb-title {
  font-size: 1.5625vw;
  color: @text-color-strong;
}

.kb-hint {
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
  font-size: 1.1458vw;
  color: @text-color-dim;
  margin-left: 1.0417vw;
  lines: 1;
}

.kb-val {
  font-size: 1.5625vw;
  color: @accent;
  margin-right: 1.0417vw;
  max-width: 27.0834vw;
}

.kb-close {
  width: 5.8334vw;
  height: 9.7745vh;
  border-radius: 0.8334vw;
  background-color: @ctrl-background;
  justify-content: center;
  align-items: center;
}

.kb-close-text {
  font-size: 1.25vw;
  color: @text-color-dim;
}

.kb-body {
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
  flex-direction: column;
  align-items: center;
}

.kb-col {
  flex-direction: column;
  align-items: center;
}

.kb-row {
  flex-direction: row;
  margin-top: 1.8797vh;
}

.kb-key {
  width: 7.7084vw;
  height: 15.0376vh;
  margin-left: 0.4167vw;
  margin-right: 0.4167vw;
  border-radius: 0.8334vw;
  background-color: @card-background-color;
  justify-content: center;
  align-items: center;
}

.kb-key-fn {
  background-color: @ctrl-background;
}

.kb-key-ok {
  width: 25vw;
  background-color: @accent;
}

.kb-key-text {
  font-size: 1.6667vw;
  color: @text-color;
}


.logo-img {
  width: 6.4584vw;
  height: 23.3083vh;
  border-radius: 1.4584vw;
  background-color: transparent;
}


.main {
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
  width: 92.0834vw;
  height: 100vh;
  flex-direction: row;
}

.brand-col {
  width: 29.7917vw;
  height: 100vh;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  padding-left: 1.0417vw;
  padding-right: 0.625vw;
}

.brand-name {
  font-size: 1.9792vw;
  color: @text-color;
  margin-top: 3.7594vh;
}

.brand-sub {
  font-size: 1.25vw;
  color: @accent-blue;
  margin-top: 1.1279vh;
}

.brand-tip {
  font-size: 1.1458vw;
  color: @text-color-dim;
  margin-top: 3.0076vh;
}

.form-scroll {
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
  flex-direction: column;
  overflow: hidden;
}

.form-col {
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
  height: 100vh;
  flex-direction: column;
  padding-top: 3.0076vh;
  padding-right: 1.25vw;
}

.tabs {
  flex-direction: row;
  align-items: center;
  height: 10.5264vh;
}

.mtab {
  width: 9.1667vw;
  height: 10.5264vh;
  border-radius: 0.8334vw;
  background-color: @card-background-color;
  justify-content: center;
  align-items: center;
  margin-right: 0.8334vw;
}

.mtab-on {
  background-color: @accent;
}

.mtab-text {
  font-size: 1.3542vw;
  color: @text-color-dim;
}

.mtab-text-on {
  color: #FFFFFF;
}

.status {
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
  font-size: 1.1458vw;
  color: @text-color-dim;
  text-align: right;
  lines: 1;
}

.frow {
  flex-direction: row;
  align-items: center;
  height: 13.5339vh;
  margin-top: 2.6316vh;
}

.flabel {
  width: 5.4167vw;
  font-size: 1.25vw;
  color: @text-color-dim;
}

.cc {
  width: 4.5834vw;
  height: 12.0301vh;
  border-radius: 0.8334vw;
  background-color: @card-background-color;
  justify-content: center;
  align-items: center;
  margin-right: 0.625vw;
}

.cc-text {
  font-size: 1.3542vw;
  color: @accent-blue;
}

.field {
  width: 19.7917vw;
  height: 12.0301vh;
  border-radius: 0.8334vw;
  background-color: @card-background-color;
  flex-direction: column;
  justify-content: center;
  padding-left: 1.0417vw;
}

.field-wide {
  width: 25vw;
}

.field-code {
  width: 14.5834vw;
}

.field-val {
  font-size: 1.4583vw;
  color: @text-color;
}

.field-ph {
  font-size: 1.25vw;
  color: @text-color-dim;
}

.swap {
  width: 4.7917vw;
  height: 11.2782vh;
  margin-left: 0.8334vw;
  border-radius: 0.8334vw;
  background-color: @ctrl-background;
  justify-content: center;
  align-items: center;
}

.swap-text {
  font-size: 1.25vw;
  color: @text-color-dim;
}

.sms-btn {
  width: 9.1667vw;
  height: 11.2782vh;
  margin-left: 0.8334vw;
  border-radius: 0.8334vw;
  background-color: @ctrl-background;
  justify-content: center;
  align-items: center;
}

.sms-btn-off {
  background-color: @background-color;
}

.sms-text {
  font-size: 1.25vw;
  color: @accent-blue;
}

.go-btn {
  width: 13.75vw;
  height: 12.782vh;
  border-radius: 0.8334vw;
  background-color: @accent;
  justify-content: center;
  align-items: center;
}

.go-btn-busy {
  background-color: @ctrl-background;
}

.go-text {
  font-size: 1.5625vw;
  color: #FFFFFF;
}

.err {
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
  font-size: 1.25vw;
  color: @danger;
  margin-left: 1.0417vw;
  lines: 2;
}

.tip {
  font-size: 1.1458vw;
  color: @text-color-dim;
  margin-top: 2.2557vh;
  lines: 1;
}

.toast {
  height: 9.0226vh;
  margin-top: 3.0076vh;
  border-radius: 0.8334vw;
  background-color: @card-background-color;
  justify-content: center;
  align-items: center;
  width: 20.8334vw;
}

.toast-text {
  font-size: 1.25vw;
  color: @accent-blue;
}
</style>
