# PenCG 真机部署脚本（有道 X6 Pro）
# 用法：powershell -NoProfile -ExecutionPolicy Bypass -File tools/deploy.ps1 [-Page index] [-NoBuild] [-Shot]
param(
  [string]$Page = 'index',
  [switch]$NoBuild,
  [switch]$Shot
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$adb = $env:PENCG_ADB
if (-not $adb) { $c = Get-Command adb -ErrorAction SilentlyContinue; if ($c) { $adb = $c.Source } }
if (-not $adb) { $cand = Join-Path $root '..\env\adb\adb.exe'; if (Test-Path $cand) { $adb = (Resolve-Path $cand).Path } }
if (-not $adb) { throw '找不到 adb：设置 PENCG_ADB 或把 adb 加入 PATH' }
$appid = '8002026110200003'
$desktop = '8080222437664451'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$ev = Join-Path $root 'profiles\evidence'
New-Item -ItemType Directory -Force -Path $ev | Out-Null

function Step($m) { Write-Host "`n=== $m ===" -ForegroundColor Cyan }

Step '1/4 设备'
& $adb devices -l

if (-not $NoBuild) {
  Step '2/4 构建'
  Push-Location $root
  node ../env/node_modules/aiot-vue-cli/src/cli.js -q -p
  Pop-Location
}

Step '3/4 安装'
$amr = Get-ChildItem -Path $root -Filter "$appid.*.amr" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $amr) { throw '找不到 AMR 包，请先构建' }
Write-Host "AMR: $($amr.Name) ($([math]::Round($amr.Length/1KB,1)) KB)"
# 坑：设备 shell 的 cwd 是只读的 /oem/YoudaoDictPen/output，相对路径 tmp/pencg.amr 解析不了
# （报 remote Read-only file system）。统一用设备侧绝对路径 /userdisk/miniapp/data/pencg.amr。
$devAmr = '/userdisk/miniapp/data/pencg.amr'
& $adb push $amr.FullName $devAmr | Out-Null
$ins = & $adb shell "miniapp_cli install $devAmr"
$ins
if ("$ins" -notmatch '"ret":\s*0') { throw 'install 未返回 ret:0' }
& $adb shell "find /userdisk/miniapp/data/mini_app/pkg/$appid -maxdepth 3 -name '*.so'"

Step '4/4 冷启动 -> ' + $Page
& $adb shell "miniapp_cli start $desktop" | Out-Null
Start-Sleep -Seconds 2
$st = & $adb shell "miniapp_cli start $appid $Page"
$st
Start-Sleep -Seconds 15
if ($Shot) {
  # 关键：背光关闭时 capture 仍返回 ret:0，但截图是全白 1086 字节。必须先点屏。
  & $adb shell 'hal-screen on' | Out-Null
  Start-Sleep -Milliseconds 700
  $cap = "/userdisk/miniapp/data/pencg_${stamp}.png"
  & $adb shell "miniapp_cli capture $cap"
  # 坑二：capture 只拍「当前前台合成面」。多 agent 共用一台设备时会被抢，
  # 抓到的可能是别人的页面（甚至纯黑 1667 字节）。这里检查文件大小并告警。
  $localShot = Join-Path $ev "shot_${stamp}.png"
  & $adb pull $cap $localShot | Out-Null
  if (Test-Path $localShot) {
    $sz = (Get-Item $localShot).Length
    if ($sz -lt 5000) { Write-Host "WARN: 截图仅 $sz 字节，可能被抢屏或是黑屏，请重跑 start+capture" }
  }
  Write-Host "screenshot: $(Join-Path $ev ('shot_' + $stamp + '.png'))"
}
$log = & $adb shell "grep -h '\[pencg\]' /userdata/applog/DictPen_*.log | tail -n 40"
if ($log) { $log } else { Write-Host '(no [pencg] log yet)' }
