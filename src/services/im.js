import globalModule from 'global';

// 输入法回调结果归一化。
// 固件差异：文本字段可能是 value/text/editText/contents/content。
export function normalizeTextEditValue(v) {
  if (v && typeof v === 'object') {
    if (typeof v.value === 'string') return v.value;
    if (typeof v.text === 'string') return v.text;
    if (typeof v.editText === 'string') return v.editText;
    if (typeof v.contents === 'string') return v.contents;
    if (typeof v.content === 'string') return v.content;
  }
  return typeof v === 'string' ? v : '';
}

function imLog(msg) {
  try {
    console.warn('[im] ' + msg);
  } catch (e) {
    // 忽略日志失败
  }
}

function isIdLike(s) {
  return typeof s === 'string' && (/^[0-9a-f]{32}$/i.test(s) || /^[0-9a-f-]{36}$/i.test(s));
}

// 真正的「无响应」兜底时长。设备上打字再慢也不会 5 分钟一个事件都没有，
// 所以这个值只需大到不会误伤正常输入即可；且到点也会先尝试取回已输入内容。
const IDLE_TIMEOUT_MS = 300000;

function safeJson(v, n) {
  let s = '';
  try {
    s = typeof v === 'string' ? v : JSON.stringify(v);
  } catch (e) {
    s = '(unserializable)';
  }
  if (s == null) s = '';
  return s.length > (n || 200) ? s.slice(0, n || 200) + '…' : s;
}

export class ImSession {
  constructor() {
    this.manager = null;
    this.uuid = '';
    this.moduleHandler = null;
    this.busHandler = null;
    this.busToken = 0;
    this.closeHandler = null;
    this.closeToken = 0;
    this.pending = null;
    // 会话序号 + 看门狗句柄：晚到的旧定时器绝不能取消新会话（否则用户确认的文字会丢）
    this.seq = 0;
    this.watchdog = 0;
    // 等待被中断后仍收到的文本（见 _onFinished），由 consumeOrphan 取走
    this.orphanText = '';
    this.orphanConfirmed = false;
  }

  // 取走「没有等待会话时」收到的文本（页面 finally 里调用）
  consumeOrphan() {
    const t = this.orphanText || '';
    this.orphanText = '';
    this.orphanConfirmed = false;
    return t;
  }

  _clearWatchdog() {
    if (this.watchdog) {
      try {
        clearTimeout(this.watchdog);
      } catch (e) {
        // 忽略
      }
      this.watchdog = 0;
    }
  }

  _ensureManager() {
    if (!this.manager) {
      this.manager = new globalModule.Global();
      imLog('Global 就绪');
    }
    return this.manager;
  }

  _resolvePending(result) {
    const p = this.pending;
    this.pending = null;
    this._clearWatchdog();
    if (p) p.resolve(result);
  }

  _closeCurrent() {
    const uuid = this.uuid;
    this.uuid = '';
    let txt = '';
    try {
      txt = this._probeText(uuid) || '';
    } catch (e) {
      txt = '';
    }
    if (uuid && this.manager) {
      try {
        this.manager.closeTextEdit(uuid);
      } catch (e) {
        // 忽略旧会话关闭失败
      }
    }
    // 主动收起（如页面 onShow 打断）也尽量带回已输入文本
    this._resolvePending({ confirmed: false, text: txt, canceled: true });
  }

  // 统一处理各通道送来的完成事件（多参数、多字段兼容）
  _onFinished(args, source) {
    const items = Array.isArray(args) ? args : [args];
    const acc = { uuid: '', confirmed: false, canceled: false, text: '' };
    for (let i = 0; i < items.length; i++) this._absorb(items[i], acc, 0);
    const uuid = acc.uuid;
    let confirmed = acc.confirmed;
    const canceled = acc.canceled;
    let text = acc.text;

    imLog(
      'textEditFinished(' + source + '): args=' + safeJson(items.length === 1 ? items[0] : items, 300) +
        ' | uuid=' + String(uuid).slice(0, 20) +
        ' pending=' + (this.pending ? 1 : 0) +
        ' confirmed=' + confirmed + ' canceled=' + canceled + ' textLen=' + text.length
    );

    // 已处理/无会话（等待被中断）：不能直接丢弃文本，先兜住，
    // 由页面在 finally 里调 consumeOrphan() 取回，避免「点了确认却什么都没进来」
    if (!this.pending) {
      if (text && (!this.uuid || !uuid || String(uuid) === String(this.uuid))) {
        this.orphanText = text;
        this.orphanConfirmed = !!confirmed;
        imLog('无等待会话，暂存文本 len=' + text.length);
      }
      return;
    }
    // 只处理自己发起的会话
    if (this.uuid && uuid && String(uuid) !== String(this.uuid)) {
      imLog('忽略非当前会话回调');
      return;
    }
    // 回调没带文本时，尝试从模块取值接口探测
    if (!text) {
      const probed = this._probeText(uuid || this.uuid);
      if (probed) {
        text = probed;
        imLog('取值探测得到文本 len=' + probed.length);
      }
    }
    if (!confirmed && !canceled && text) confirmed = true;

    const cur = this.uuid;
    this.uuid = '';
    // 先返回结果，再延迟关闭面板：在信号回调里直接调 closeTextEdit 有重入/卡死风险
    // 取消（未确认）时也把已输入文本带回：页面上「收起键盘」不应作废用户输入
    this._resolvePending({
      confirmed: confirmed && !canceled,
      text: text || '',
      canceled: !!canceled
    });
    this._deferClose(uuid || cur);
  }

  // 延迟关闭输入法会话（脱离信号回调栈，避免重入）
  _deferClose(uuid) {
    if (!uuid) return;
    const self = this;
    setTimeout(function () {
      if (!self.manager) return;
      try {
        self.manager.closeTextEdit(uuid);
      } catch (e) {
        // 忽略
      }
    }, 60);
  }

  // 结束等待（避免 imBusy 永远为真）。默认**不**主动关闭面板：
  // 只有确认面板已经关闭时才应传 closePanel=true，否则会在打字中途把键盘弹掉。
  cancelPending(reason, closePanel) {
    if (!this.pending) return false;
    const uuid = this.uuid;
    this.uuid = '';
    imLog('结束等待(' + (reason || 'closed') + ') closePanel=' + (closePanel ? 1 : 0));
    // 超时/中断时先尝试把已经敲进去的内容取回；取到了就当作已确认，
    // 让内容落进输入框（绝不能让用户白打一遍）
    let txt = '';
    try {
      txt = this._probeText(uuid) || '';
    } catch (e) {
      txt = '';
    }
    this._resolvePending(
      txt ? { confirmed: true, text: txt, canceled: false } : { confirmed: false, text: '', canceled: true }
    );
    if (closePanel) this._deferClose(uuid);
    return true;
  }

  // 放弃等待：不解析 promise、也不关闭面板（页面销毁/疑似过期时使用）
  abandon(reason) {
    const had = !!this.pending;
    const uuid = this.uuid;
    this.uuid = '';
    this.pending = null;
    this._clearWatchdog();
    this.seq++; // 作废所有在途看门狗
    imLog('放弃会话(' + (reason || '') + ') hadPending=' + (had ? 1 : 0) + ' 保持面板不动');
    return had;
  }

  isPending() {
    return !!this.pending;
  }

  // 递归吸收回调参数：字符串可能是 JSON（真机实测第二个参数是
  // "{\"text\":\"啊\",\"cursorIndex\":1,\"editConfirmed\":true}"）
  _absorb(item, acc, depth) {
    if (depth > 3 || item === undefined || item === null) return;
    if (typeof item === 'string') {
      const s = item.trim();
      if (!s) return;
      if (isIdLike(s)) {
        if (!acc.uuid) acc.uuid = s;
        return;
      }
      const head = s.charAt(0);
      if (head === '{' || head === '[') {
        try {
          this._absorb(JSON.parse(s), acc, depth + 1);
          return;
        } catch (e) {
          // 不是合法 JSON，按普通文本处理
        }
      }
      if (s.length > acc.text.length) acc.text = s;
      return;
    }
    if (typeof item === 'boolean') {
      if (item) acc.confirmed = true;
      return;
    }
    if (typeof item === 'number') return;
    if (typeof item === 'object') {
      if (item.editConfirmed === true || item.confirmed === true) acc.confirmed = true;
      if (item.editCanceled === true || item.canceled === true) acc.canceled = true;
      const u = item.uuid || item.calledTaskUuid || item.taskUuid;
      if (typeof u === 'string' && u) acc.uuid = u;
      const t = normalizeTextEditValue(
        item.editText !== undefined
          ? item.editText
          : item.text !== undefined
            ? item.text
            : item.value !== undefined
              ? item.value
              : item.contents !== undefined
                ? item.contents
                : item.content
      );
      if (t && !isIdLike(t) && t.length >= acc.text.length) acc.text = t;
      // 有些固件把内容再包一层（对象里的字符串字段仍是 JSON）
      if (!t && typeof item.data === 'string') this._absorb(item.data, acc, depth + 1);
    }
  }
  // 探测模块上可能存在的文本取值接口/属性，返回第一个像文本的值（并记录探测过程）
  _probeText(uuid) {
    const m = this.manager;
    if (!m) return '';
    const names = [
      'getTextEditValue',
      'getTextEditContent',
      'getTextEditText',
      'getCurrentTextEditValue',
      'getEditText',
      'getContent',
      'getText',
      'getValue'
    ];
    const log = [];
    let hit = '';
    for (let i = 0; i < names.length; i++) {
      const n = names[i];
      let fn = null;
      try {
        fn = m[n];
      } catch (e) {
        fn = null;
      }
      if (typeof fn !== 'function') continue;
      let v = null;
      try {
        v = uuid ? fn.call(m, uuid) : fn.call(m);
      } catch (e1) {
        try {
          v = fn.call(m);
        } catch (e2) {
          v = '(调用失败)';
        }
      }
      log.push(n + '=' + safeJson(v, 60));
      if (!hit && typeof v === 'string' && v && !isIdLike(v)) hit = v;
    }
    try {
      const keys = Object.keys(m);
      const strs = [];
      for (let i = 0; i < keys.length; i++) {
        let v = null;
        try {
          v = m[keys[i]];
        } catch (e) {
          continue;
        }
        if (typeof v === 'string' && v && !isIdLike(v)) strs.push(keys[i] + '=' + v.slice(0, 40));
      }
      if (strs.length) log.push('props:' + strs.join(','));
    } catch (e4) {
      // 忽略属性枚举失败
    }
    if (log.length) imLog('取值探测: ' + log.join(' | '));
    return hit;
  }

  // 仅供自检：把一段回调参数直接喂给解析逻辑，返回解析结果
  simulate(args) {
    return new Promise((resolve) => {
      try {
        this._ensureManager();
      } catch (e) {
        resolve({ confirmed: false, text: '', error: 'IM_UNAVAILABLE' });
        return;
      }
      this.uuid = '';
      this.pending = { resolve: resolve };
      this._onFinished(args, 'selftest');
    });
  }
  _subscribe(m) {
    if (!this.moduleHandler) {
      const self = this;
      this.moduleHandler = function () {
        self._onFinished(Array.prototype.slice.call(arguments), 'module');
      };
      try {
        m.textEditFinished.on(this.moduleHandler);
        imLog('模块信号订阅成功');
      } catch (e) {
        this.moduleHandler = null;
        imLog('模块信号订阅失败: ' + (e && e.message));
      }
    }
    if (!this.busHandler) {
      const self = this;
      this.busHandler = function () {
        self._onFinished(Array.prototype.slice.call(arguments), 'bus');
      };
      try {
        this.busToken = $falcon.on('textEditFinished', this.busHandler);
        imLog('总线订阅成功');
      } catch (e) {
        try {
          $falcon.on('textEditFinished', this.busHandler);
          imLog('总线订阅成功(无 token)');
        } catch (e2) {
          this.busHandler = null;
          imLog('总线订阅失败: ' + (e2 && e2.message));
        }
      }
    }
    // IM 关闭事件只用于记录：它并不携带编辑结果，
    // 若当成完成回调会误触发 closeTextEdit，把正在输入的键盘关掉。
    if (!this.closeHandler) {
      const self = this;
      this.closeHandler = function () {
        const items = Array.prototype.slice.call(arguments);
        imLog('requestIMAppClose: args=' + safeJson(items, 200));
      };
      try {
        this.closeToken = $falcon.on('requestIMAppClose', this.closeHandler);
        imLog('IM 关闭事件订阅成功');
      } catch (e3) {
        this.closeHandler = null;
      }
    }
  }

  // options: { value, placeholder, maxlength } → Promise<{confirmed, text, error?}>
  open(options) {
    const opts = options || {};
    return new Promise((resolve) => {
      let m;
      try {
        m = this._ensureManager();
      } catch (e) {
        imLog('Global 创建失败: ' + (e && e.message));
        resolve({ confirmed: false, text: '', error: 'IM_UNAVAILABLE' });
        return;
      }
      this._closeCurrent();
      this._subscribe(m);
      this.orphanText = '';
      this.orphanConfirmed = false;
      const config = JSON.stringify({
        text: typeof opts.value === 'string' ? opts.value : '',
        placeholder: opts.placeholder || '请输入',
        maxlength: opts.maxlength || 2000,
        autofocus: true
      });
      let uuid = '';
      try {
        uuid = m.startTextEdit(config);
      } catch (e) {
        imLog('startTextEdit 抛错: ' + (e && e.message));
        resolve({ confirmed: false, text: '', error: 'IM_START_FAIL' });
        return;
      }
      if (uuid && typeof uuid === 'object') uuid = uuid.uuid || uuid.value || '';
      this.uuid = typeof uuid === 'string' ? uuid : '';
      this.pending = { resolve: resolve };
      imLog('startTextEdit 返回 uuid=' + (this.uuid ? this.uuid.slice(0, 20) : '(空)'));
      // 兜底计时器：只防「面板关了但一个回调都没有」导致 imBusy 永久卡死。
      // 它**不是输入时限** —— 到点后只通知页面「可以重新打开输入框了」（onIdle），
      // **不解析、不作废**当前会话：用户之后点「确认」仍能正常把文字送回。
      // 必须按会话序号校验：上一轮的定时器晚到时不能影响这一轮。
      const self = this;
      const mySeq = ++this.seq;
      this._clearWatchdog();
      this.watchdog = setTimeout(function () {
        self.watchdog = 0;
        if (mySeq !== self.seq) return; // 旧会话的定时器，忽略
        if (!self.pending) return;
        imLog('长时间无输入法事件(' + IDLE_TIMEOUT_MS + 'ms)：解除忙碌，但保留当前会话');
        try {
          if (self.onIdle) self.onIdle();
        } catch (e) {
          // 忽略回调异常
        }
      }, IDLE_TIMEOUT_MS);
    });
  }

  dispose() {
    if (this.manager && this.moduleHandler) {
      try {
        this.manager.textEditFinished.off(this.moduleHandler);
      } catch (e) {
        // off 失败不阻塞清理
      }
    }
    if (this.busHandler) {
      try {
        if (this.busToken) $falcon.off('textEditFinished', this.busToken);
        else $falcon.off('textEditFinished', this.busHandler);
      } catch (e) {
        // 同上
      }
    }
    if (this.closeHandler) {
      try {
        if (this.closeToken) $falcon.off('requestIMAppClose', this.closeToken);
        else $falcon.off('requestIMAppClose', this.closeHandler);
      } catch (e) {
        // 同上
      }
    }
    this.moduleHandler = null;
    this.busHandler = null;
    this.busToken = 0;
    this.closeHandler = null;
    this.closeToken = 0;
    // 页面销毁：只解除订阅并放弃等待；**不调用 closeTextEdit**。
    // 运行时在切后台/回收页面时会触发 onUnload，若此时关面板会把用户正在输入的系统键盘弹掉。
    this.abandon('page-unload');
    this.manager = null;
    imLog('dispose 完成(不关闭面板)');
  }
}
