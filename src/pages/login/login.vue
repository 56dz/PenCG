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
      </div>
      </div>
    </div>
  </div>
</template>
<script>
import { loginByPassword, loginBySms, sendSmsCode, restoreSession, currentSession } from '../../services/auth.js';
import { getString, setString } from '../../services/storage.js';
import { navTo, navBack } from '../../services/nav.js';
import logoImg from '../../assets/logo.png';
import { ImSession } from '../../services/im.js';
import { logInfo, logWarn } from '../../services/log.js';

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
      navBack();
    },
    goMine() {
      navTo('mine', {});
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
        this.errText = '发送失败：' + this.friendlyErr(e, 'sms');
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
        this.showToast('登录成功');
        logInfo('[login] success uid=' + (r && r.userId));
        this.refreshStatus();
        const self = this;
        setTimeout(function () {
          self.busy = false;
          navTo('mine', {});
        }, 800);
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
  height: 40px;
  padding-left: 12px;
  padding-right: 12px;
}

.kb-title {
  font-size: 15px;
  color: @text-color-strong;
}

.kb-hint {
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
  font-size: 11px;
  color: @text-color-dim;
  margin-left: 10px;
  lines: 1;
}

.kb-val {
  font-size: 15px;
  color: @accent;
  margin-right: 10px;
  max-width: 260px;
}

.kb-close {
  width: 56px;
  height: 26px;
  border-radius: 8px;
  background-color: @ctrl-background;
  justify-content: center;
  align-items: center;
}

.kb-close-text {
  font-size: 12px;
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
  margin-top: 5px;
}

.kb-key {
  width: 74px;
  height: 40px;
  margin-left: 4px;
  margin-right: 4px;
  border-radius: 8px;
  background-color: @card-background-color;
  justify-content: center;
  align-items: center;
}

.kb-key-fn {
  background-color: @ctrl-background;
}

.kb-key-ok {
  width: 240px;
  background-color: @accent;
}

.kb-key-text {
  font-size: 16px;
  color: @text-color;
}


.logo-img {
  width: 62px;
  height: 62px;
  border-radius: 14px;
  background-color: transparent;
}


.main {
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
  width: 884px;
  height: 266px;
  flex-direction: row;
}

.brand-col {
  width: 286px;
  height: 266px;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  padding-left: 10px;
  padding-right: 6px;
}

.brand-name {
  font-size: 19px;
  color: @text-color;
  margin-top: 10px;
}

.brand-sub {
  font-size: 12px;
  color: @accent-blue;
  margin-top: 3px;
}

.brand-tip {
  font-size: 11px;
  color: @text-color-dim;
  margin-top: 8px;
}

.form-col {
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
  height: 266px;
  flex-direction: column;
  padding-top: 8px;
  padding-right: 12px;
}

.tabs {
  flex-direction: row;
  align-items: center;
  height: 28px;
}

.mtab {
  width: 88px;
  height: 28px;
  border-radius: 8px;
  background-color: @card-background-color;
  justify-content: center;
  align-items: center;
  margin-right: 8px;
}

.mtab-on {
  background-color: @accent;
}

.mtab-text {
  font-size: 13px;
  color: @text-color-dim;
}

.mtab-text-on {
  color: #FFFFFF;
}

.status {
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
  font-size: 11px;
  color: @text-color-dim;
  text-align: right;
  lines: 1;
}

.frow {
  flex-direction: row;
  align-items: center;
  height: 36px;
  margin-top: 7px;
}

.flabel {
  width: 52px;
  font-size: 12px;
  color: @text-color-dim;
}

.cc {
  width: 44px;
  height: 32px;
  border-radius: 8px;
  background-color: @card-background-color;
  justify-content: center;
  align-items: center;
  margin-right: 6px;
}

.cc-text {
  font-size: 13px;
  color: @accent-blue;
}

.field {
  width: 190px;
  height: 32px;
  border-radius: 8px;
  background-color: @card-background-color;
  flex-direction: column;
  justify-content: center;
  padding-left: 10px;
}

.field-wide {
  width: 240px;
}

.field-code {
  width: 140px;
}

.field-val {
  font-size: 14px;
  color: @text-color;
}

.field-ph {
  font-size: 12px;
  color: @text-color-dim;
}

.swap {
  width: 46px;
  height: 30px;
  margin-left: 8px;
  border-radius: 8px;
  background-color: @ctrl-background;
  justify-content: center;
  align-items: center;
}

.swap-text {
  font-size: 12px;
  color: @text-color-dim;
}

.sms-btn {
  width: 88px;
  height: 30px;
  margin-left: 8px;
  border-radius: 8px;
  background-color: @ctrl-background;
  justify-content: center;
  align-items: center;
}

.sms-btn-off {
  background-color: @background-color;
}

.sms-text {
  font-size: 12px;
  color: @accent-blue;
}

.go-btn {
  width: 132px;
  height: 34px;
  border-radius: 8px;
  background-color: @accent;
  justify-content: center;
  align-items: center;
}

.go-btn-busy {
  background-color: @ctrl-background;
}

.go-text {
  font-size: 15px;
  color: #FFFFFF;
}

.err {
  flex-grow: 1;
  flex-shrink: 1;
  flex-basis: 0%;
  font-size: 12px;
  color: @danger;
  margin-left: 10px;
  lines: 2;
}

.tip {
  font-size: 11px;
  color: @text-color-dim;
  margin-top: 6px;
  lines: 1;
}

.toast {
  height: 24px;
  margin-top: 8px;
  border-radius: 8px;
  background-color: @card-background-color;
  justify-content: center;
  align-items: center;
  width: 200px;
}

.toast-text {
  font-size: 12px;
  color: @accent-blue;
}
</style>
