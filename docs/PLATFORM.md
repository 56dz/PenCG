# X6 Pro / Falcon 平台硬约束（全部真机实测，违反即黑屏或崩溃）

## 设备
- 有道 X6 Pro，aarch64（RK3562），Buildroot 2021.11，kernel 5.10。
- **逻辑画布 960x266**（合成面横向条带），物理面板 480x960 竖屏，DRM rotation=1。
- `src/app.js` 里 `DESIGN_WIDTH = 960` + `onLaunch` 调 `this.setViewPort(DESIGN_WIDTH)`。
  写错宽度 → 整页等比缩放且**纵向裁掉底部**。
- adb 序列号 `9D91…5171`，`adb.exe` 在仓库上一级 `env/adb/adb.exe`。
- **adb 命令行坑（MSYS）**：`adb push/pull /tmp/x` 会被 MSYS 改写成
  `C:\Users\...\Temp\x` 然后报 Read-only file system / does not exist。
  → 用 `powershell -NoProfile -Command "& '.\env\adb\adb.exe' pull /tmp/x local"` 规避；
  或 `adb shell 'cd /tmp && ...'`。

## Vue 方言铁律（falcon 运行时）
1. 每个 `.vue` **必须**有 `<style lang="less" scoped>` 且首行 `@import "base.less";`
   —— 没有 style 块时样式规则不进 bundle（真机黑屏）。
2. `flex: 1` **简写无效**，必须写 `flex-grow: 1; flex-shrink: 1; flex-basis: 0%;`。
3. `<text>` 的内容**不能换行**（换行即整块不渲染）；长文本先切行数组再逐行 `<text>`。
4. `position: absolute` 不可靠，全仓**没有可用的 `z-index`** → 弹层用 `v-if` 整页替换。
5. 页面组件是普通 Vue 2 options 对象（`data()` / `methods` / `computed` / `onShow` 等），
   生命周期钩子放在组件根选项里（`onShow/onHide/onUnload`）。
6. 图片用 `<img :src="url" class="...">`（框架内置 ImageLoader，走 curl）。
   **https 未必有 CA → 一律 `imgUrl()` 降级 http**（`services/img.js`）。
7. 滚动用 `<scroller class="scroll-y">` / `<scroller class="scroll-x">`；
   `scroller` 必须给固定宽高或用 flex 三连撑开，否则不滚。

## 布局
- 浏览态：左导航栏 **76** + 内容区 **884** = 960。
- 播放态：左列 213 + 中列 469（视频 469x240 + 进度条 469x26）+ 右列 278 = 960。
- **视频走 KMS overlay 平面，盖在 UI 之上，UI 画不上去** → 播放态 UI 只能画在视频矩形外。

## JS 能力边界
- 只有 `$falcon.jsapi.http.request`（**无 WebSocket、无 crypto、无 fetch、无 XMLHttpRequest**）。
- `jsapi.http.request` 会对 `data` 字符串**二次 JSON 序列化** → 发不了裸 body。
- 响应头**永远不可见**；非 2xx 时 **body 不可得**，只有 `errorMessage` 里的 `resCode:N`。
- QuickJS `bigNum: false` → **没有 BigInt**（不能直接做 53 位以上整数运算）。
- 有 `setTimeout/setInterval/Promise/async await/Map/Set/JSON/Math/Date`。
- `jsapi.storage.getStorage({key})` → `{data:string}`；**`setStorage({key,data})` 才落盘**。
- 屏幕：播放态需每 20s 调一次保活，否则 270s 自动灭屏。

## native 模块（.so）
- 打包进 AMR 的 `libs/` 目录，运行时 `import` 模块名（不带 `libjsapi_` 前缀和 `.so`）。
- 交叉编译：`..\env\pyzig\ziglang\zig.exe cc -target aarch64-linux-gnu.2.17`
  （主机无 gcc/clang；zig 找不到 `-lssl`，必须传 stub `.so` 全路径）。
- 导出：`JQUICK_EXPORT void custom_init_jsapis(void)`，里面
  `registerCModuleLoader("模块名", &loader)`。
- 设备 `.so` 的 section header 被裁剪 → **禁止静态 ELF 解析**，用真机 dlopen 探针。
- 设备 `libcurl 7.79.1` **无 WebSocket**；`libssl/libcrypto 1.1.1l` 有完整 DTLS-SRTP 能力。
