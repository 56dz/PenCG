# 开发 / 构建 / 真机验证手册

## 目录
```
PenCG/
  package.json          appid 8002026110200003
  app_icon.png          256x256 桌面图标（必填）
  src/
    app.js             入口（DESIGN_WIDTH=960）
    app.json           页面注册表
    base-page.js       页面基类（ResourcePage 资源回收 + BasePage 生命周期代理）
    pages/<name>/      每页 xx.js（入口）+ xx.vue（组件）
    services/          纯 JS 业务层
    styles/            var.less（配色）+ base.less（骨架，每个 .vue 必须 @import）
    components/        公共组件
  native/
    csrc/cgnative/     libjsapi_cgnative.so 源码
    csrc/cgmedia/      libjsapi_cgmedia.so 源码
    vendor/openssl-include/   OpenSSL 1.1.1w 头（含手写 opensslconf.h，勿删）
    stubs-aarch64/     设备导入库（libc/libssl/libcrypto/libquickjs/libcurl/...）
    stubs-aarch64-gst/ 设备 GStreamer 导入库
    iot-miniapp-sdk/include/  SDK 头
    build-net.ps1 / build-media.ps1 / build.ps1
  libs/                构建产物 .so，打包时自动进 AMR
  tools/               图标生成、部署、ELF 检查
  docs/                架构、平台约束、接口笔记、本文件
```

## 构建（前端）
```powershell
cd PenCG
node ../env/node_modules/aiot-vue-cli/src/cli.js -q -p
```
产物：`8002026110200003.1_0_0.amr` + `.falcon_/`。
> `node_modules/falcon-ui` 是到 `../env/node_modules/falcon-ui` 的目录联接（junction），
> 已创建。若缺失：`powershell -c "New-Item -ItemType Junction -Path 'node_modules\falcon-ui' -Target '<仓库>\env\node_modules\falcon-ui'"`。

## 构建（native）
```powershell
cd PenCG
powershell -NoProfile -ExecutionPolicy Bypass -File native/build.ps1
# 单模块：-NetOnly / -MediaOnly
```
zig 默认 `../env/pyzig/ziglang/zig.exe`（可用 `ZIG_EXE` 覆盖）。
交叉目标 `aarch64-linux-gnu.2.17`。**zig 找不到 `-lssl`/`-lcrypto`，必须传 stub `.so` 全路径。**

## 真机部署
```powershell
cd PenCG
powershell -NoProfile -ExecutionPolicy Bypass -File tools/deploy.ps1 -Page index -Shot
# -NoBuild 跳过构建；-Page diag 启动到自检页
```
脚本做：build → `adb push` → `miniapp_cli install`（校验 `"ret": 0`）→
`miniapp_cli start <desktop>` 退回桌面 → `miniapp_cli start <appid> <page>` →
`miniapp_cli capture` 截图 → 打印 `[pencg]` 日志。

### ⚠ adb 路径坑（MSYS Bash）
`adb pull /tmp/x.png local` 在 MSYS Bash 里会把 `/tmp/x.png` 改写成
`C:\Users\...\Temp\x.png` 导致 "does not exist"。两种规避：
```bash
powershell -NoProfile -Command "& '.\env\adb\adb.exe' pull /tmp/x.png 'PenCG\out.png'"
```
或直接在 PowerShell 里调 `powershell -File tools/deploy.ps1`（脚本内部无此问题）。

## 看设备日志
```bash
./env/adb/adb.exe shell "grep -h '\[pencg\]' /userdata/applog/loli_s6_dev.txt | tail -n 60"
```
**落盘文件是 `/userdata/applog/loli_s6_dev.txt`**（`DictPen_*.log` 那个路径在本机不存在）。

**只有 `console.error` 会落盘**：X6 Pro 的 Falcon 运行时里 `console.warn` / `console.log`
完全不写盘（全盘 `grep -a -c 'W][JS]'` 计数为 0）。因此：

- `services/log.js` 的 `push()` 一律走 `console.error`（`log.js:44`），行内带 `[pencg][I|W|E]` 前缀 —— **真正的级别只看这个前缀**。
- 落盘行的固定形态是 `[时间戳][E][JS] [pencg][W] ...`，中间那个 `[E]` 是通道级别（恒为 E），不是我们想表达的严重程度，别被它误导。

## 截图
```bash
./env/adb/adb.exe shell "miniapp_cli capture /tmp/a.png"
powershell -NoProfile -Command "& '.\env\adb\adb.exe' pull /tmp/a.png 'PenCG\profiles\evidence\a.png'"
```
`capture` 截的是 Falcon UI 合成面（960x266，**不含 KMS 视频平面**）；
`captureFB` 截 fb 原始内容（偶尔会 `ret:-1`，重试即可）。

## 屏幕
- `hal-screen on` / `off` / `state` / `keep`（保活 30s）。
- 播放态必须每 20s 调一次保活，否则 270s 自动灭屏。

## 调试技巧
- 页面白屏且无样式 → 忘了 `<style lang="less" scoped>@import "base.less";`。
- 布局挤在一起 → 用了 `flex: 1` 简写（无效），改三连。
- 文字不显示 → `<text>` 内容里换行了。
- 图片不显示 → 没走 `imgUrl()` 降级 http。
- native 模块 import 失败但 `.so` 在 → 检查 `pkg/<appid>/<slot>/libs/` 下文件名，
  运行时会把 `libjsapi_xxx.so` 重命名成 `libjsapi_xxx_<随机数>.so`，这是正常的。
