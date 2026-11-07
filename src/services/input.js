// 输入协议：{"op":"input","data":{"cmd":"<code> <args>"}}
// 坐标按 Math.ceil(65535 * x / videoWidth) 归一化
import { sendInput } from './media.js';

export const NX = (x, w) => Math.ceil((65535 * x) / (w || 1280));
export const NY = (y, h) => Math.ceil((65535 * y) / (h || 720));

/* ★ 移动端触摸协议（down/move/up/tap = 1/2/3/8）发的是**视频像素坐标**，不是 65535 百分比。
   证据（官方 chunk-common.144278fd.js，逐条可复核）：
     send_touchstart_message(e){ var t={x:e.posX,y:e.posY}, i=this.transform(t),
       n={x:i.x,y:i.y,id:e.slotId}; this.send_message("down",n) }
     transform(e){ t.x=Math.floor(e.x*this.scaleX), t.y=Math.floor(e.y*this.scaleY) }
     update_display_size: scaleX = videoWidth/displayWidth   （非旋转分支）
   → posX 是 DOM 像素，乘 scaleX(=videoWidth/displayWidth) 得到的就是视频像素（0..videoWidth）。
   getPercentPos(65535 归一化) **只**出现在 send_message_pc 分支（mousemove/mousedown/
   mouseup/mousewheel 共 29 处调用点逐一核对，喂给 send_message 的为 0 处）。
   另一处铁证：percent2Absolute/getAbsoluteValue 把配置里的百分比
   `Math.round(e*this.videoWidth/100)` 转成绝对像素后，才交给 send_message("down"/"move")。
   所以我方历史上按 65535 归一化发 1/2/3/8，远端收到的触摸点全被钳到视频右下角，
   表现就是「画面在动但点什么都没反应、截图 diff 恒 0」。 */
export const PX = (x, w) => {
  var v = Math.round(Number(x) || 0);
  var m = Number(w) || 1280;
  if (v < 0) v = 0;
  if (v > m) v = m;
  return v;
};
export const PY = (y, h) => {
  var v = Math.round(Number(y) || 0);
  var m = Number(h) || 720;
  if (v < 0) v = 0;
  if (v > m) v = m;
  return v;
};

export function tap(x, y, id, vw, vh) {
  return sendInput('8 ' + PX(x, vw) + ' ' + PY(y, vh) + ' ' + (id || 0));
}

export function down(x, y, id, vw, vh) {
  var r = sendInput('1 ' + PX(x, vw) + ' ' + PY(y, vh) + ' ' + (id || 0));

  return r;
}

export function move(x, y, id, vw, vh) {
  return sendInput('2 ' + PX(x, vw) + ' ' + PY(y, vh) + ' ' + (id || 0));
}

export function up(x, y, id, vw, vh) {
  var r = sendInput('3 ' + PX(x, vw) + ' ' + PY(y, vh) + ' ' + (id || 0));

  return r;
}
export function upOrig(x, y, id, vw, vh) {
  return sendInput('3 ' + PX(x, vw) + ' ' + PY(y, vh) + ' ' + (id || 0));
}

export function cancel() {
  return sendInput('4');
}

export function sendKey(keycode) {
  return sendInput('6 ' + keycode);
}

export function sendText(text) {
  return sendInput('5 ' + text);
}

export function setInputMode(mode) {
  /* 官方实现是 "25 "+(s?"keymouse":"tochscreen") —— 注意官方拼写就是
     tochscreen（少一个 u），这里原样对齐，避免网关按字面量比对时失配。 */
  return sendInput('25 ' + (mode === 'keymouse' ? 'keymouse' : 'tochscreen'));
}

export function heartbeat(ts) {
  return sendInput('0 ' + (ts || Date.now()));
}

export function shake() {
  return sendInput('28');
}

/* 起播后声明客户端平台。官方 startGame 里必发：
   var a = "10 " + ("android"!==ti||this.inApp ? "" : "h5-") + ti;  ti = "android"|"ios"|"pc"
   我们跑在安卓设备上，且是原生小程序（非 inApp 内嵌网页），所以取 "10 android"。
   部分端游靠这条切换鼠标/键盘映射，不发可能只有触摸生效。 */
export function declarePlatform(plat) {
  var p = plat || 'android';
  return sendInput('10 ' + p);
}

/* WS 心跳。官方在 datachannel 保活里每 1s 发一次 "0 <ms>"；
   我们不走 datachannel，但网关侧同样会按这个 cmd 刷新会话活性。 */
export function keepAlive(ts) {
  return sendInput('0 ' + (ts || Date.now()));
}

/* ---- 端游鼠标协议（官方 send_message_pc 的原文格式）----
   mousemove = "102 posType x y state"
   mousedown = "100 btnType posType x y state"
   mouseup   = "101 btnType posType x y state"
   posType: 0=绝对坐标、1=相对位移；btnType: 0=左键 1=中键 2=右键；
   state 是修饰键位图（Alt=1 Control=2 Shift=4 NumLock=8 CapsLock=16 ScrollLock=32）。
   官方点击是「mousemove → 50ms → mousedown → 50ms → mouseup」，只发 down/up 不一定生效。 */
export function mouseMove(x, y, vw, vh, state) {
  return sendInput('102 0 ' + NX(x, vw) + ' ' + NY(y, vh) + ' ' + (state || 0));
}

export function mouseDown(x, y, vw, vh, btn, state) {
  return sendInput('100 ' + (btn || 0) + ' 0 ' + NX(x, vw) + ' ' + NY(y, vh) + ' ' + (state || 0));
}

export function mouseUp(x, y, vw, vh, btn, state) {
  return sendInput('101 ' + (btn || 0) + ' 0 ' + NX(x, vw) + ' ' + NY(y, vh) + ' ' + (state || 0));
}

/* 校准用：直接按 0..1 的比例发绝对鼠标移动，绕过 play.vue 的坐标换算，
   这样缓冲里鼠标指针落在哪，就能反推出「我们发的比例 → 远端画面位置」的真实映射。 */
export function mouseMoveRatio(rx, ry) {
  return sendInput('102 0 ' + Math.round(rx * 65535) + ' ' + Math.round(ry * 65535) + ' 0');
}

/* 外设鼠标协议（官方 send_message_peripheral）：
   mousedown  = "21 btnType x y state"
   mouseup    = "22 btnType x y state"
   mousemove  = "23 x y state"
   与 100/101/102 的区别是多了一个「外设」标记，官方在检测到真实鼠标时才走这条。
   部分容器壳（云游戏自己的启动器 UI）只认这一路，所以留作备选。 */
export function pMouseMove(x, y, vw, vh, state) {
  return sendInput('23 ' + PX(x, vw) + ' ' + PY(y, vh) + ' ' + (state || 0));
}

export function pMouseDown(x, y, vw, vh, btn, state) {
  return sendInput('21 ' + (btn || 0) + ' ' + PX(x, vw) + ' ' + PY(y, vh) + ' ' + (state || 0));
}

export function pMouseUp(x, y, vw, vh, btn, state) {
  return sendInput('22 ' + (btn || 0) + ' ' + PX(x, vw) + ' ' + PY(y, vh) + ' ' + (state || 0));
}

/* 触摸 tap（官方 EVENTS.tap = 8）。语义上是「点一下」，容器侧等价于 down+up，
   但有些容器只认 tap，所以保留独立的导出。 */
export function tapOnly(x, y, id, vw, vh) {
  return sendInput('8 ' + PX(x, vw) + ' ' + PY(y, vh) + ' ' + (id || 0));
}
