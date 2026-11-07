# native-net deploy script (does NOT modify tools/deploy.ps1)
# Difference from deploy.ps1: the AMR is pushed to an ABSOLUTE device path.
#   deploy.ps1 pushes to relative 'tmp/pencg.amr', but adb shell cwd is
#   /oem/YoudaoDictPen/output (read-only), so relative tmp/ no longer resolves.
#   PowerShell does not rewrite /tmp/... like MSYS bash does, so absolute is safe.
param(
  [string]$Page = 'diag',
  [string]$ShotName = '',
  [int]$WaitSec = 15,
  [switch]$NoBuild,
  [switch]$NoStart
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$adb = $env:PENCG_ADB
if (-not $adb) { $c = Get-Command adb -ErrorAction SilentlyContinue; if ($c) { $adb = $c.Source } }
if (-not $adb) { $cand = Join-Path $root '..\env\adb\adb.exe'; if (Test-Path $cand) { $adb = (Resolve-Path $cand).Path } }
if (-not $adb) { throw 'adb not found' }
$appid = '8002026110200003'
$desktop = '8080222437664451'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
if ($ShotName) { $stamp = $ShotName }
$ev = Join-Path $root 'profiles\evidence'
New-Item -ItemType Directory -Force -Path $ev | Out-Null
function Step($m) { Write-Host "`n=== $m ===" -ForegroundColor Cyan }

if (-not $NoBuild) {
  Step 'build'
  Push-Location $root
  node ../env/node_modules/aiot-vue-cli/src/cli.js -q -p
  Pop-Location
}

Step 'install'
$amr = Get-ChildItem -Path $root -Filter "$appid.*.amr" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $amr) { throw 'AMR not found' }
Write-Host "AMR: $($amr.Name) ($([math]::Round($amr.Length/1KB,1)) KB)"
$remote = '/userdisk/miniapp/data/pencg.amr'
& $adb push $amr.FullName $remote | Out-Null
$ins = & $adb shell "miniapp_cli install $remote"
$ins
if ("$ins" -notmatch '"ret":\s*0') { throw "install did not return ret:0" }
& $adb shell "find /userdisk/miniapp/data/mini_app/pkg/$appid -maxdepth 3 -name '*.so'"

if (-not $NoStart) {
  Step "start -> $Page"
  & $adb shell "miniapp_cli start $desktop" | Out-Null
  Start-Sleep -Seconds 2
  $st = & $adb shell "miniapp_cli start $appid $Page"
  $st
  Start-Sleep -Seconds $WaitSec
}

Step 'capture'
& $adb shell 'hal-screen on' | Out-Null
Start-Sleep -Milliseconds 700
$cap = "/userdisk/miniapp/data/pencg_$stamp.png"
& $adb shell "miniapp_cli capture $cap"
$localShot = Join-Path $ev "shot_$stamp.png"
& $adb pull $cap $localShot | Out-Null
if (Test-Path $localShot) {
  $sz = (Get-Item $localShot).Length
  Write-Host "screenshot: $localShot ($sz bytes)"
  if ($sz -lt 5000) { Write-Host "WARN: screenshot only $sz bytes, may be black or stolen by another agent" }
} else { Write-Host "WARN: pull failed" }
$log = & $adb shell "grep -h '\[pencg\]' /userdata/applog/DictPen_*.log | tail -n 60"
if ($log) { $log } else { Write-Host '(no pencg log yet)' }
